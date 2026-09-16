#include "bridge.h"

#define CONFIG_PATH "/etc/msi-claw-a8-steam-tdp-bridge/config.ini"
#define CONFIG_GROUP "tdp"

static gchar *
config_string(GKeyFile *keyfile, const gchar *key, const gchar *fallback)
{
    if (!keyfile)
        return g_strdup(fallback);

    GError *error = NULL;
    gchar *value = g_key_file_get_string(keyfile, CONFIG_GROUP, key, &error);
    g_clear_error(&error);
    if (value && *value)
        return value;

    g_free(value);
    return g_strdup(fallback);
}

static void
config_set_string(gchar **target, GKeyFile *keyfile, const gchar *key, const gchar *fallback)
{
    g_free(*target);
    *target = config_string(keyfile, key, fallback);
}

gboolean
config_load(Bridge *bridge)
{
    bridge->fwattr_device = NULL;
    bridge->attr_spl = NULL;
    bridge->attr_sppt = NULL;
    bridge->attr_fppt = NULL;
    bridge->platform_profile_name = NULL;
    bridge->profile_policy = NULL;
    bridge->profile_name = NULL;
    bridge->state_path = NULL;
    bridge->restore_last = TRUE;
    bridge->enforce_profile = FALSE;
    bridge->honor_steam_toggle = TRUE;
    bridge->steam_config = NULL;
    bridge->steam_toggle_known = FALSE;
    bridge->steam_toggle_on = TRUE;
    bridge->default_limit = 15;

    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(keyfile, CONFIG_PATH, G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_key_file_unref(keyfile);
        keyfile = NULL;
    }

    config_set_string(&bridge->fwattr_device, keyfile, "device", "msi-wmi-platform");
    config_set_string(&bridge->attr_spl, keyfile, "attribute_spl", "ppt_pl1_spl");
    config_set_string(&bridge->attr_sppt, keyfile, "attribute_sppt", "ppt_pl2_sppt");
    config_set_string(&bridge->attr_fppt, keyfile, "attribute_fppt", "ppt_pl3_fppt");
    config_set_string(&bridge->platform_profile_name, keyfile, "platform_profile", "msi-wmi-platform");
    config_set_string(&bridge->profile_policy, keyfile, "profile_policy", "always");
    config_set_string(&bridge->profile_name, keyfile, "profile_name", "performance");
    config_set_string(&bridge->state_path, keyfile, "state_path", "/var/lib/msi-claw-a8-steam-tdp-bridge/tdp");

    if (keyfile) {
        gchar *restore = g_key_file_get_string(keyfile, CONFIG_GROUP, "restore_last", NULL);
        if (restore) {
            bridge->restore_last = g_ascii_strcasecmp(restore, "true") == 0
                                || g_strcmp0(restore, "1") == 0;
            g_free(restore);
        }

        GError *error2 = NULL;
        gint default_limit = g_key_file_get_integer(keyfile, CONFIG_GROUP, "default_limit", &error2);
        if (!error2 && default_limit > 0)
            bridge->default_limit = (guint)default_limit;
        g_clear_error(&error2);

        gchar *enforce = g_key_file_get_string(keyfile, CONFIG_GROUP, "enforce_profile", NULL);
        if (enforce) {
            bridge->enforce_profile = g_ascii_strcasecmp(enforce, "true") == 0
                                   || g_strcmp0(enforce, "1") == 0;
            g_free(enforce);
        }

        gchar *honor = g_key_file_get_string(keyfile, CONFIG_GROUP, "honor_steam_toggle", NULL);
        if (honor) {
            bridge->honor_steam_toggle = g_ascii_strcasecmp(honor, "true") == 0
                                      || g_strcmp0(honor, "1") == 0;
            g_free(honor);
        }

        gchar *steam_config = g_key_file_get_string(keyfile, CONFIG_GROUP, "steam_config", NULL);
        if (steam_config && *steam_config)
            bridge->steam_config = steam_config;
        else
            g_free(steam_config);

        g_key_file_unref(keyfile);
    }

    return TRUE;
}
