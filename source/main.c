#include "bridge.h"

#include <stdio.h>

static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='com.steampowered.SteamOSManager1.TdpLimit1'>"
    "    <property name='TdpLimit' type='u' access='readwrite'/>"
    "    <property name='TdpLimitMin' type='u' access='read'/>"
    "    <property name='TdpLimitMax' type='u' access='read'/>"
    "  </interface>"
    "</node>";

void
bridge_emit_changed(Bridge *bridge)
{
    if (!bridge->conn)
        return;

    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE_ARRAY);
    g_variant_builder_add(&changed, "{sv}", "TdpLimit",
                          g_variant_new_uint32(bridge->value));

    GVariantBuilder invalidated;
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

    GVariant *params = g_variant_new("(s@a{sv}@as)", BRIDGE_INTERFACE,
                                     g_variant_builder_end(&changed),
                                     g_variant_builder_end(&invalidated));

    g_dbus_connection_emit_signal(bridge->conn, NULL, BRIDGE_OBJECT_PATH,
                                  "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged", params, NULL);
}

static GVariant *
on_get_property(GDBusConnection *connection, const gchar *sender, const gchar *path,
                const gchar *interface, const gchar *property, GError **error,
                gpointer user_data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)error;
    Bridge *bridge = user_data;

    /* The MSI module may be loaded after us; refresh the range on demand. */
    if (g_strcmp0(property, "TdpLimit") != 0)
        bridge_ensure_ranges(bridge);

    if (g_strcmp0(property, "TdpLimit") == 0)
        return g_variant_new_uint32(bridge->value ? bridge->value : bridge->default_limit);
    if (g_strcmp0(property, "TdpLimitMin") == 0)
        return g_variant_new_uint32(bridge->spl_min);
    if (g_strcmp0(property, "TdpLimitMax") == 0)
        return g_variant_new_uint32(bridge->spl_max);
    return NULL;
}

static gboolean
on_set_property(GDBusConnection *connection, const gchar *sender, const gchar *path,
                const gchar *interface, const gchar *property, GVariant *value,
                GError **error, gpointer user_data)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    Bridge *bridge = user_data;

    if (g_strcmp0(property, "TdpLimit") != 0)
        return FALSE;

    guint limit = g_variant_get_uint32(value);
    g_message("Set TdpLimit: %u", limit);
    if (!bridge_apply(bridge, limit, error))
        return FALSE;

    bridge_emit_changed(bridge);
    return TRUE;
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = NULL,
    .get_property = on_get_property,
    .set_property = on_set_property,
};

static void
on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)connection; (void)user_data;
    g_message("acquired bus name %s", name);
}

static void
on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)connection; (void)user_data;
    g_warning("lost bus name %s", name);
}

/*
 * Restore the saved limit once the bus name is ours. Kept out of the startup
 * path so the daemon (Type=dbus) never delays the session, and so it does not
 * touch the platform profile before Steam is available.
 */
static gboolean
on_startup_idle(gpointer user_data)
{
    Bridge *bridge = user_data;
    bridge_init_value(bridge);
    bridge_watch_profile(bridge);
    bridge_watch_config(bridge);
    return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    gboolean show_get = FALSE;
    gboolean show_restore = FALSE;
    gint apply_value = -1;

    GOptionEntry entries[] = {
        { "get", 'g', 0, G_OPTION_ARG_NONE, &show_get, "Print current TDP and exit", NULL },
        { "apply", 'a', 0, G_OPTION_ARG_INT, &apply_value, "Apply TDP value (W) and exit", "W" },
        { "restore", 'r', 0, G_OPTION_ARG_NONE, &show_restore, "Apply saved TDP and exit", NULL },
        { NULL }
    };

    GOptionContext *context = g_option_context_new("- Steam TDP bridge for MSI Claw");
    g_option_context_add_main_entries(context, entries, NULL);

    GError *error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("option parsing failed: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        return 1;
    }

    Bridge bridge = { 0 };
    config_load(&bridge);
    bridge_resolve_paths(&bridge);
    bridge_ensure_ranges(&bridge);

    if (show_get) {
        guint value = bridge_read_current(&bridge);
        if (value == 0)
            value = bridge.default_limit;
        g_print("%u\n", value);
        g_option_context_free(context);
        return 0;
    }

    if (apply_value >= 0) {
        if (!bridge_apply(&bridge, (guint)apply_value, &error)) {
            g_printerr("apply failed: %s\n", error->message);
            g_clear_error(&error);
            g_option_context_free(context);
            return 1;
        }
        g_print("applied %u\n", bridge_read_current(&bridge));
        g_option_context_free(context);
        return 0;
    }

    if (show_restore) {
        bridge_state_restore(&bridge);
        g_print("applied %u\n", bridge_read_current(&bridge));
        g_option_context_free(context);
        return 0;
    }

    /* Daemon mode: connect to the system bus, own the name, export the object. */
    bridge.conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bridge.conn) {
        g_printerr("cannot connect to system bus: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        return 1;
    }

    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!node_info) {
        g_printerr("introspection failed: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        return 1;
    }

    guint registration_id = g_dbus_connection_register_object(
        bridge.conn, BRIDGE_OBJECT_PATH, node_info->interfaces[0],
        &interface_vtable, &bridge, NULL, &error);
    if (registration_id == 0) {
        g_printerr("cannot register object: %s\n", error->message);
        g_clear_error(&error);
        g_dbus_node_info_unref(node_info);
        g_option_context_free(context);
        return 1;
    }

    guint owner_id = g_bus_own_name_on_connection(
        bridge.conn, BRIDGE_DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
        on_name_acquired, on_name_lost, NULL, NULL);

    g_message("steam-tdp-bridge started: %s%s (range %u..%u W, policy=%s)",
              BRIDGE_DBUS_NAME, BRIDGE_OBJECT_PATH,
              bridge.spl_min, bridge.spl_max, bridge.profile_policy);

    bridge.loop = g_main_loop_new(NULL, FALSE);
    g_idle_add(on_startup_idle, &bridge);
    g_main_loop_run(bridge.loop);

    g_bus_unown_name(owner_id);
    g_dbus_connection_unregister_object(bridge.conn, registration_id);
    g_dbus_node_info_unref(node_info);
    g_option_context_free(context);
    return 0;
}
