#include "bridge.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

gboolean
sysfs_read_u32(const gchar *path, guint *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return FALSE;

    unsigned long value = 0;
    int matched = fscanf(f, "%lu", &value);
    fclose(f);

    if (matched != 1)
        return FALSE;

    *out = (guint)value;
    return TRUE;
}

gboolean
sysfs_write_u32(const gchar *path, guint value, GError **err)
{
    gchar *text = g_strdup_printf("%u\n", value);
    gboolean ok = sysfs_write_str(path, text, err);
    g_free(text);
    return ok;
}

gboolean
sysfs_write_str(const gchar *path, const gchar *value, GError **err)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "open %s: %s", path, g_strerror(errno));
        return FALSE;
    }

    gboolean ok = fputs(value, f) >= 0;
    int saved_errno = errno;
    if (fclose(f) != 0 && ok) {
        ok = FALSE;
        saved_errno = errno;
    }

    if (!ok) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "write %s: %s", path, g_strerror(saved_errno));
    }
    return ok;
}

gboolean
sysfs_find_dir_by_name(const gchar *base, const gchar *want, gchar **out)
{
    GDir *dir = g_dir_open(base, 0, NULL);
    if (!dir)
        return FALSE;

    gboolean found = FALSE;
    const gchar *entry;
    while ((entry = g_dir_read_name(dir))) {
        gchar *name_file = g_build_filename(base, entry, "name", NULL);
        gchar *content = NULL;
        if (g_file_get_contents(name_file, &content, NULL, NULL)) {
            g_strstrip(content);
            if (g_strcmp0(content, want) == 0) {
                *out = g_build_filename(base, entry, NULL);
                found = TRUE;
            }
        }
        g_free(content);
        g_free(name_file);
        if (found)
            break;
    }

    g_dir_close(dir);
    return found;
}

gboolean
bridge_resolve_paths(Bridge *bridge)
{
    g_free(bridge->attr_dir);
    bridge->attr_dir = g_build_filename("/sys/class/firmware-attributes",
                                        bridge->fwattr_device, "attributes", NULL);
    gboolean attr_ok = g_file_test(bridge->attr_dir, G_FILE_TEST_IS_DIR);

    if (!bridge->profile_dir)
        sysfs_find_dir_by_name("/sys/class/platform-profile",
                               bridge->platform_profile_name, &bridge->profile_dir);

    return attr_ok;
}

static gboolean
read_attr_range(const gchar *base, const gchar *attr, guint *min, guint *max)
{
    gchar *path = g_build_filename(base, attr, "min_value", NULL);
    gboolean ok_min = sysfs_read_u32(path, min);
    g_free(path);

    path = g_build_filename(base, attr, "max_value", NULL);
    gboolean ok_max = sysfs_read_u32(path, max);
    g_free(path);

    return ok_min && ok_max;
}

gboolean
bridge_ensure_ranges(Bridge *bridge)
{
    if (bridge->ranges_ok)
        return TRUE;

    if (!bridge->attr_dir || !g_file_test(bridge->attr_dir, G_FILE_TEST_IS_DIR)) {
        bridge_resolve_paths(bridge);
        if (!bridge->attr_dir || !g_file_test(bridge->attr_dir, G_FILE_TEST_IS_DIR))
            return FALSE;
    }

    gboolean ok = read_attr_range(bridge->attr_dir, bridge->attr_spl,
                                  &bridge->spl_min, &bridge->spl_max)
               && read_attr_range(bridge->attr_dir, bridge->attr_sppt,
                                  &bridge->sppt_min, &bridge->sppt_max)
               && read_attr_range(bridge->attr_dir, bridge->attr_fppt,
                                  &bridge->fppt_min, &bridge->fppt_max);

    bridge->ranges_ok = ok;
    return ok;
}
