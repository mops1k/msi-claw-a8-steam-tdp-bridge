#include "bridge.h"

#include <stdio.h>

static guint
clamp_range(guint value, guint lo, guint hi)
{
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

static gchar *
profile_get(Bridge *bridge)
{
    if (!bridge->profile_dir)
        return NULL;

    gchar *path = g_build_filename(bridge->profile_dir, "profile", NULL);
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL))
        g_strstrip(content);
    g_free(path);
    return content;
}

static gboolean
profile_set(Bridge *bridge, const gchar *profile)
{
    if (!bridge->profile_dir)
        return FALSE;

    gchar *path = g_build_filename(bridge->profile_dir, "profile", NULL);
    gboolean ok = sysfs_write_str(path, profile, NULL);
    g_free(path);
    return ok;
}

/*
 * The MSI EC only enforces ppt_* while the platform profile is the configured
 * one, so while Steam's "TDP Limit" toggle is on we keep that profile. When the
 * toggle is off we release it, letting the user pick any profile again.
 */
static gboolean
steam_toggle_on(Bridge *bridge, guint limit)
{
    if (!bridge->honor_steam_toggle)
        return bridge->enforce_profile;

    /*
     * The cached toggle can be stale right after the user flips the checkbox.
     * A fresh read is cheap enough at value boundaries: Steam sends the max
     * value when the limit is disabled and a smaller value when enabled.
     */
    if (bridge->toggle_cache_valid
        && ((bridge->ranges_ok && limit == bridge->spl_max)
            || (bridge->ranges_ok && limit < bridge->spl_max && !bridge->toggle_cache_on)))
        bridge->toggle_cache_valid = FALSE;

    (void)limit;

    /*
     * Read the toggle from config.vdf. This never talks to the Steam client,
     * so it is safe (and cheap) during boot / game-mode startup, when poking
     * the webhelper would disrupt the UI.
     */
    gchar *path = (bridge->steam_config && *bridge->steam_config)
                    ? g_strdup(bridge->steam_config)
                    : steam_find_config();
    if (!path) {
        bridge->steam_toggle_known = FALSE;
        return FALSE;
    }

    gboolean known = FALSE;
    gboolean enabled = FALSE;
    gboolean ok = steam_toggle_enabled(path, &known, &enabled);
    g_free(path);

    if (!ok || !known) {
        /* Steam config not readable (e.g. very early boot): don't force. */
        bridge->steam_toggle_known = FALSE;
        return FALSE;
    }

    if (!bridge->steam_toggle_known || enabled != bridge->steam_toggle_on)
        g_message("Steam TDP toggle: %s", enabled ? "on" : "off");
    bridge->steam_toggle_known = TRUE;
    bridge->steam_toggle_on = enabled;
    return enabled;
}

static gboolean
should_enforce_profile(Bridge *bridge, guint limit)
{
    if (g_strcmp0(bridge->profile_policy, "never") == 0)
        return FALSE;
    return steam_toggle_on(bridge, limit);
}

/*
 * Prefer steamos-manager's session-bus API so the Steam UI notices the change;
 * fall back to writing sysfs directly.
 */
static void
apply_profile(Bridge *bridge, const gchar *profile)
{
    if (!profile)
        return;

    /* Nothing to do (and, crucially, no reason to poke Steam) if already set. */
    gchar *current = profile_get(bridge);
    if (current && g_strcmp0(current, profile) == 0) {
        g_free(current);
        return;
    }
    g_free(current);

    /* Keep Steam's own setting/config in sync with what we apply. */
    steam_set_ui_profile(profile);

    /* Apply through sysfs directly (no steamos-manager emit) so Steam does not
     * overwrite its setting in response. */
    profile_set(bridge, profile);
}

/*
 * Keep the platform profile consistent with Steam's TDP toggle: force
 * profile_name while the limit is enabled, hand the profile back to the user
 * when it is disabled. apply_profile() is a no-op when nothing changes.
 */
static void
sync_profile(Bridge *bridge, guint limit)
{
    if (should_enforce_profile(bridge, limit)) {
        if (!bridge->saved_profile) {
            gchar *current = profile_get(bridge);
            if (current && g_strcmp0(current, bridge->profile_name) != 0)
                bridge->saved_profile = current;
            else
                g_free(current);
        }
        apply_profile(bridge, bridge->profile_name);
    } else if (bridge->saved_profile) {
        apply_profile(bridge, bridge->saved_profile);
        g_clear_pointer(&bridge->saved_profile, g_free);
    }
}

static gboolean
write_limits(Bridge *bridge, guint spl, guint sppt, guint fppt, GError **err)
{
    gchar *path = g_build_filename(bridge->attr_dir, bridge->attr_spl, "current_value", NULL);
    gboolean ok = sysfs_write_u32(path, spl, err);
    g_free(path);
    if (!ok)
        return FALSE;

    path = g_build_filename(bridge->attr_dir, bridge->attr_sppt, "current_value", NULL);
    ok = sysfs_write_u32(path, sppt, err);
    g_free(path);
    if (!ok)
        return FALSE;

    path = g_build_filename(bridge->attr_dir, bridge->attr_fppt, "current_value", NULL);
    ok = sysfs_write_u32(path, fppt, err);
    g_free(path);
    return ok;
}

void
bridge_state_save(Bridge *bridge)
{
    if (!bridge->state_path)
        return;

    gchar *dir = g_path_get_dirname(bridge->state_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    gchar *text = g_strdup_printf("%u\n", bridge->value);
    g_file_set_contents(bridge->state_path, text, -1, NULL);
    g_free(text);
}

static gboolean
state_read(Bridge *bridge, guint *out)
{
    if (!bridge->state_path)
        return FALSE;

    gchar *content = NULL;
    if (!g_file_get_contents(bridge->state_path, &content, NULL, NULL))
        return FALSE;

    guint value = 0;
    gboolean ok = sscanf(content, "%u", &value) == 1;
    g_free(content);

    if (ok)
        *out = value;
    return ok;
}

void
bridge_state_restore(Bridge *bridge)
{
    guint value = 0;
    if (state_read(bridge, &value) && value > 0)
        bridge_apply(bridge, value, NULL);
}

/*
 * Decide what value to expose on startup.
 *
 * The EC reports ppt_* current_value as 0 until userspace programs it, but the
 * TdpLimit1 contract requires a value inside [min, max], so 0 must never be
 * reported to Steam. Prefer the saved value, then the value currently in sysfs,
 * then a configured default.
 */
void
bridge_init_value(Bridge *bridge)
{
    bridge_ensure_ranges(bridge);

    guint saved = 0;
    if (bridge->restore_last && state_read(bridge, &saved) && saved > 0) {
        bridge->value = saved;
        if (bridge_apply(bridge, saved, NULL))
            return;
    }

    guint current = bridge_read_current(bridge);
    if (bridge->ranges_ok && current >= bridge->spl_min
        && current <= bridge->spl_max && current > 0) {
        bridge->value = current;
        return;
    }

    if (bridge->ranges_ok && bridge->default_limit >= bridge->spl_min
        && bridge->default_limit <= bridge->spl_max) {
        if (bridge_apply(bridge, bridge->default_limit, NULL))
            return;
        bridge->value = bridge->default_limit;
        return;
    }

    bridge->value = bridge->ranges_ok ? bridge->spl_min : 0;
}

gboolean
bridge_apply(Bridge *bridge, guint limit, GError **err)
{
    if (limit == 0) {
        /* Not a valid limit; keep the current one. */
        if (bridge->saved_profile) {
            apply_profile(bridge, bridge->saved_profile);
            g_clear_pointer(&bridge->saved_profile, g_free);
        }
        return TRUE;
    }

    if (!bridge_ensure_ranges(bridge)) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "firmware attributes %s not available", bridge->fwattr_device);
        return FALSE;
    }

    guint spl = clamp_range(limit, bridge->spl_min, bridge->spl_max);
    guint sppt = clamp_range(MAX(limit, bridge->sppt_min), bridge->sppt_min, bridge->sppt_max);
    guint fppt = clamp_range(MAX(limit, bridge->fppt_min), bridge->fppt_min, bridge->fppt_max);

    sync_profile(bridge, limit);

    GError *write_error = NULL;
    gboolean ok = write_limits(bridge, spl, sppt, fppt, &write_error);
    if (!ok) {
        if (write_error)
            g_propagate_error(err, write_error);
        return FALSE;
    }

    bridge->value = limit;
    bridge_state_save(bridge);
    return TRUE;
}

/*
 * TDP is only enforced by the MSI EC while the msi-wmi-platform profile is the
 * configured one (performance). If something changes it underneath us (Steam
 * QAM profile picker, resume, ...), put it back while a limit is active.
 */
static void
on_profile_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                   GFileMonitorEvent event, gpointer user_data)
{
    (void)monitor; (void)file; (void)other;
    Bridge *bridge = user_data;

    if (event != G_FILE_MONITOR_EVENT_CHANGED
        && event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT
        && event != G_FILE_MONITOR_EVENT_CREATED)
        return;

    if (g_strcmp0(bridge->profile_policy, "never") == 0)
        return;

    gchar *current = profile_get(bridge);
    if (!current || g_strcmp0(current, bridge->profile_name) == 0) {
        g_free(current);
        return;
    }

    g_message("platform profile changed to '%s'", current);

    if (should_enforce_profile(bridge, bridge->value) && bridge->value > 0) {
        if (!bridge->saved_profile)
            bridge->saved_profile = g_strdup(current);
        g_message("re-asserting '%s' for TDP %u", bridge->profile_name, bridge->value);
        /* sysfs only: a drifting platform profile is none of Steam's business,
         * so never touch the Steam client from here. */
        profile_set(bridge, bridge->profile_name);
    } else {
        /* TDP toggle is off: the user took control, don't restore our profile. */
        g_clear_pointer(&bridge->saved_profile, g_free);
    }
    g_free(current);
}

/*
 * Steam writes config.vdf when the TDP checkbox changes. Watching it covers the
 * case where Steam sends the TDP update before persisting the new toggle state.
 * We only act when the toggle actually changed, so normal config writes never
 * poke the Steam client.
 */
static gboolean
on_config_changed_idle(gpointer data)
{
    Bridge *bridge = data;
    if (bridge->value == 0)
        return G_SOURCE_REMOVE;

    gboolean was_known = bridge->steam_toggle_known;
    gboolean was_on = bridge->steam_toggle_on;

    (void)steam_toggle_on(bridge, bridge->value);

    if (bridge->steam_toggle_known
        && (!was_known || bridge->steam_toggle_on != was_on))
        sync_profile(bridge, bridge->value);

    return G_SOURCE_REMOVE;
}

static void
on_config_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                  GFileMonitorEvent event, gpointer user_data)
{
    (void)monitor; (void)file; (void)other;
    if (event != G_FILE_MONITOR_EVENT_CHANGED
        && event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT
        && event != G_FILE_MONITOR_EVENT_CREATED)
        return;
    g_idle_add(on_config_changed_idle, user_data);
}

void
bridge_watch_config(Bridge *bridge)
{
    gchar *path = (bridge->steam_config && *bridge->steam_config)
                    ? g_strdup(bridge->steam_config)
                    : steam_find_config();
    if (!path)
        return;

    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    bridge->config_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
    if (bridge->config_monitor) {
        g_signal_connect(bridge->config_monitor, "changed",
                         G_CALLBACK(on_config_changed), bridge);
    } else {
        g_warning("cannot watch Steam config: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(file);
    g_free(path);
}

void
bridge_watch_profile(Bridge *bridge)
{
    if (g_strcmp0(bridge->profile_policy, "always") != 0 || !bridge->profile_dir)
        return;

    gchar *path = g_build_filename(bridge->profile_dir, "profile", NULL);
    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    bridge->profile_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
    if (bridge->profile_monitor) {
        g_signal_connect(bridge->profile_monitor, "changed",
                         G_CALLBACK(on_profile_changed), bridge);
    } else {
        g_warning("cannot watch platform profile: %s", error->message);
        g_clear_error(&error);
    }
    g_object_unref(file);
    g_free(path);
}

guint
bridge_read_current(Bridge *bridge)
{
    if (!bridge->attr_dir)
        bridge_resolve_paths(bridge);

    gchar *path = g_build_filename(bridge->attr_dir, bridge->attr_spl, "current_value", NULL);
    guint value = 0;
    gboolean ok = sysfs_read_u32(path, &value);
    g_free(path);
    return ok ? value : 0;
}
