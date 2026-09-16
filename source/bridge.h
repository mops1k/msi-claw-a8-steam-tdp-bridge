#pragma once

#include <glib.h>
#include <gio/gio.h>

#define BRIDGE_DBUS_NAME "com.steampowered.TdpBridge"
#define BRIDGE_OBJECT_PATH "/com/steampowered/TdpBridge"
#define BRIDGE_INTERFACE "com.steampowered.SteamOSManager1.TdpLimit1"

typedef struct {
    GDBusConnection *conn;
    GMainLoop *loop;

    /* config */
    gchar *fwattr_device;
    gchar *attr_spl;
    gchar *attr_sppt;
    gchar *attr_fppt;
    gchar *platform_profile_name;
    gchar *profile_policy; /* auto | always | never */
    gchar *profile_name;   /* profile enforced while a TDP is active */
    gboolean enforce_profile; /* re-assert profile_name if changed externally */
    gboolean honor_steam_toggle; /* only enforce while Steam's TDP toggle is on */
    gchar *steam_config;   /* path to Steam's config.vdf ("" = auto-detect) */
    gboolean steam_toggle_known;
    gboolean steam_toggle_on;
    gboolean toggle_cache_valid;
    gboolean toggle_cache_on;
    guint toggle_cache_limit;
    gint64 toggle_cache_time;
    gboolean restore_last;
    guint default_limit;
    gchar *state_path;

    /* resolved paths */
    gchar *attr_dir;
    gchar *profile_dir;
    gchar *saved_profile;
    GFileMonitor *profile_monitor;
    GFileMonitor *config_monitor;

    /* runtime */
    guint value;
    guint spl_min, spl_max;
    guint sppt_min, sppt_max;
    guint fppt_min, fppt_max;
    gboolean ranges_ok;
} Bridge;

/* sysfs.c */
gboolean sysfs_read_u32(const gchar *path, guint *out);
gboolean sysfs_write_u32(const gchar *path, guint value, GError **err);
gboolean sysfs_write_str(const gchar *path, const gchar *value, GError **err);
gboolean sysfs_find_dir_by_name(const gchar *base, const gchar *want, gchar **out);
gboolean bridge_resolve_paths(Bridge *bridge);
gboolean bridge_ensure_ranges(Bridge *bridge);

/* config.c */
gboolean config_load(Bridge *bridge);

/* steam.c */
gchar *steam_find_config(void);
gboolean steam_toggle_enabled(const gchar *path, gboolean *known, gboolean *enabled);
void steam_set_ui_profile(const gchar *profile);
gboolean steam_get_toggle(Bridge *bridge, gboolean *enabled, guint *limit);

/* tdp.c */
void bridge_state_save(Bridge *bridge);
void bridge_state_restore(Bridge *bridge);
void bridge_init_value(Bridge *bridge);
void bridge_watch_profile(Bridge *bridge);
void bridge_watch_config(Bridge *bridge);
gboolean bridge_apply(Bridge *bridge, guint limit, GError **err);
guint bridge_read_current(Bridge *bridge);

/* main.c */
void bridge_emit_changed(Bridge *bridge);
