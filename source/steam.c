#include "bridge.h"

#include <string.h>
#include <sys/stat.h>

/*
 * Steam stores the "TDP Limit" checkbox state in
 * ~/.local/share/Steam/config/config.vdf:
 *
 *   "SteamOS"
 *   {
 *       ...
 *       "TDPLimitEnabled"
 *       {
 *           "<id>"  "1"
 *       }
 *       ...
 *   }
 *
 * where <id> is a per-install key (not necessarily a game appid). We only need
 * to know whether the toggle is currently on.
 */

static gchar *
read_quoted(const gchar **cursor)
{
    const gchar *p = *cursor;
    while (*p && *p != '"')
        p++;
    if (!*p)
        return NULL;

    p++;
    const gchar *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++;
        p++;
    }
    if (!*p)
        return NULL;

    gchar *result = g_strndup(start, p - start);
    *cursor = p + 1;
    return result;
}

static gchar *
find_steam_config_in_home(const gchar *home)
{
    gchar *path = g_build_filename(home, ".local", "share", "Steam",
                                   "config", "config.vdf", NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return path;
    g_free(path);
    return NULL;
}

gchar *
steam_find_config(void)
{
    const gchar *env = g_getenv("STEAM_CONFIG");
    if (env && *env)
        return g_strdup(env);

    gchar *best = NULL;
    GDir *dir = g_dir_open("/home", 0, NULL);
    if (!dir)
        return NULL;

    time_t best_mtime = 0;
    const gchar *entry;
    while ((entry = g_dir_read_name(dir))) {
        gchar *home = g_build_filename("/home", entry, NULL);
        gchar *candidate = find_steam_config_in_home(home);
        g_free(home);
        if (!candidate)
            continue;

        struct stat st;
        if (stat(candidate, &st) == 0 && st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            g_free(best);
            best = candidate;
        } else {
            g_free(candidate);
        }
    }
    g_dir_close(dir);
    return best;
}

gboolean
steam_toggle_enabled(const gchar *path, gboolean *known, gboolean *enabled)
{
    *known = FALSE;
    *enabled = TRUE;

    gchar *content = NULL;
    if (!path || !g_file_get_contents(path, &content, NULL, NULL))
        return FALSE;

    const gchar *key = strstr(content, "\"TDPLimitEnabled\"");
    if (!key) {
        g_free(content);
        return FALSE;
    }

    const gchar *open = strchr(key, '{');
    if (!open) {
        g_free(content);
        return FALSE;
    }

    /* Extract the block up to the matching closing brace. */
    const gchar *p = open;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                p++;
                break;
            }
        }
    }

    gchar *block = g_strndup(open, p - open);
    const gchar *cursor = block;
    gboolean any = FALSE;
    gboolean on = FALSE;
    gchar *name;
    while ((name = read_quoted(&cursor))) {
        gchar *value = read_quoted(&cursor);
        if (!value) {
            g_free(name);
            break;
        }
        any = TRUE;
        if (g_strcmp0(value, "1") == 0)
            on = TRUE;
        g_free(name);
        g_free(value);
    }
    g_free(block);
    g_free(content);

    if (!any)
        return FALSE;

    *known = TRUE;
    *enabled = on;
    return TRUE;
}

/*
 * Read the TDP checkbox state from Steam's in-memory settings through the
 * webhelper debug port. Cached briefly so dragging the slider does not spawn
 * the helper on every tick. Falls back to reading config.vdf.
 */
gboolean
steam_get_toggle(Bridge *bridge, gboolean *enabled, guint *limit)
{
    const gint64 cache_us = 1 * 1000 * 1000;
    gint64 now = g_get_monotonic_time();
    if (bridge->toggle_cache_valid && (now - bridge->toggle_cache_time) < cache_us) {
        if (enabled)
            *enabled = bridge->toggle_cache_on;
        if (limit)
            *limit = bridge->toggle_cache_limit;
        return TRUE;
    }

    const gchar *helper = "/usr/lib/msi-claw-a8-steam-tdp-bridge/msi-claw-a8-steam-tdp-bridge-set-profile.py";
    if (!g_file_test(helper, G_FILE_TEST_IS_REGULAR))
        return FALSE;

    gchar *argv[] = { "python3", (gchar *)helper, "get", NULL };
    gchar *output = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                    NULL, NULL, &output, NULL, &status, &error);
    if (!spawned || !g_spawn_check_wait_status(status, NULL)) {
        g_clear_error(&error);
        g_free(output);
        return FALSE;
    }

    gboolean parsed = FALSE;
    gboolean on = FALSE;
    guint value = 0;
    if (output) {
        const gchar *e = strstr(output, "\"enabled\"");
        if (e && (e = strchr(e, ':'))) {
            while (*e && (*e == ':' || g_ascii_isspace(*e)))
                e++;
            on = g_str_has_prefix(e, "true");
            parsed = TRUE;
        }
        const gchar *l = strstr(output, "\"limit\"");
        if (l && (l = strchr(l, ':')))
            value = (guint)g_ascii_strtoull(l + 1, NULL, 10);
    }
    g_debug("steam_get_toggle raw: %s", output ? g_strstrip(output) : "(null)");
    g_free(output);
    if (!parsed)
        return FALSE;

    bridge->toggle_cache_valid = TRUE;
    bridge->toggle_cache_on = on;
    bridge->toggle_cache_limit = value;
    bridge->toggle_cache_time = now;

    if (enabled)
        *enabled = on;
    if (limit)
        *limit = value;
    return TRUE;
}

/*
 * Update Steam's own "performance profile" setting so the UI and config.vdf
 * match the profile we apply. Best effort: the helper drives the webhelper
 * debug port.
 */
void
steam_set_ui_profile(const gchar *profile)
{
    const gchar *helper = "/usr/lib/msi-claw-a8-steam-tdp-bridge/msi-claw-a8-steam-tdp-bridge-set-profile.py";
    if (!g_file_test(helper, G_FILE_TEST_IS_REGULAR))
        return;

    gchar *argv[] = { "python3", (gchar *)helper, "set", (gchar *)profile, NULL };
    GError *error = NULL;
    gboolean spawned = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, NULL, &error);
    if (!spawned) {
        g_message("msi-claw-a8-steam-tdp-bridge-set-profile helper failed: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}
