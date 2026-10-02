#include "FinderPrefs.h"

#include <json-glib/json-glib.h>
#include <string.h>

static const char kPrefsFileName[]  = "finder-plugin-prefs.json";
static const char kKeyLastRootPath[] = "lastRootPath";
static const char kKeyFavoritePaths[] = "favoritePaths";
static const char kKeyDidShowPanelOnFirstRun[] = "didShowPanelOnFirstRun";
static const char kKeyShowHiddenFiles[] = "showHiddenFiles";

static gchar     *s_config_dir;
static gchar     *s_last_root;        /* falls back to $HOME */
static GPtrArray *s_favorites;        /* char*, NULL-terminated via pdata */
static gboolean   s_show_hidden;
static gboolean   s_did_first_run;

static void ensure_defaults(void)
{
    if (!s_last_root) s_last_root = g_strdup(g_get_home_dir());
    if (!s_favorites) {
        s_favorites = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(s_favorites, NULL);   /* keep NULL-terminated */
    }
}

static gchar *prefs_file_path(void)
{
    if (!s_config_dir) return NULL;
    return g_build_filename(s_config_dir, kPrefsFileName, NULL);
}

static void load_prefs(void)
{
    gchar *path = prefs_file_path();
    if (!path) return;

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_file(parser, path, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);

            if (json_object_has_member(obj, kKeyLastRootPath)) {
                const char *v = json_object_get_string_member_with_default(
                    obj, kKeyLastRootPath, NULL);
                /* Like macOS: only adopt a root that still exists. */
                if (v && *v && g_file_test(v, G_FILE_TEST_EXISTS)) {
                    g_free(s_last_root);
                    s_last_root = g_strdup(v);
                }
            }
            s_show_hidden = json_object_get_boolean_member_with_default(
                obj, kKeyShowHiddenFiles, s_show_hidden);
            s_did_first_run = json_object_get_boolean_member_with_default(
                obj, kKeyDidShowPanelOnFirstRun, s_did_first_run);

            if (json_object_has_member(obj, kKeyFavoritePaths)) {
                JsonArray *arr = json_object_get_array_member(obj, kKeyFavoritePaths);
                if (arr) {
                    g_ptr_array_set_size(s_favorites, 0);
                    guint n = json_array_get_length(arr);
                    for (guint i = 0; i < n; i++) {
                        const char *v = json_array_get_string_element(arr, i);
                        if (v && *v) g_ptr_array_add(s_favorites, g_strdup(v));
                    }
                    g_ptr_array_add(s_favorites, NULL);
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(path);
}

void finder_prefs_configure(const char *config_dir)
{
    ensure_defaults();
    if (!config_dir || !*config_dir) return;
    g_free(s_config_dir);
    s_config_dir = g_strdup(config_dir);
    g_mkdir_with_parents(s_config_dir, 0755);
    load_prefs();
}

void finder_prefs_save(void)
{
    ensure_defaults();
    gchar *path = prefs_file_path();
    if (!path) return;   /* not configured yet; nothing to persist to */

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, kKeyLastRootPath);
    json_builder_add_string_value(b, s_last_root);
    json_builder_set_member_name(b, kKeyFavoritePaths);
    json_builder_begin_array(b);
    for (guint i = 0; i + 1 < s_favorites->len; i++)
        json_builder_add_string_value(b, (const char *)s_favorites->pdata[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, kKeyDidShowPanelOnFirstRun);
    json_builder_add_boolean_value(b, s_did_first_run);
    json_builder_set_member_name(b, kKeyShowHiddenFiles);
    json_builder_add_boolean_value(b, s_show_hidden);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    gchar *text = json_generator_to_data(gen, NULL);
    /* g_file_set_contents writes via a temp file + rename — the atomic
     * write NSDataWritingAtomic gave us on macOS. */
    if (!g_file_set_contents(path, text, -1, NULL))
        g_warning("[Finder] Failed to write prefs to %s", path);

    g_free(text);
    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(b);
    g_free(path);
}

const char *finder_prefs_last_root_path(void)
{
    ensure_defaults();
    return s_last_root;
}

void finder_prefs_set_last_root_path(const char *path)
{
    if (!path || !*path) return;
    ensure_defaults();
    g_free(s_last_root);
    s_last_root = g_strdup(path);
}

gboolean finder_prefs_show_hidden(void) { return s_show_hidden; }
void finder_prefs_set_show_hidden(gboolean v) { s_show_hidden = v; }

gboolean finder_prefs_did_show_panel_on_first_run(void) { return s_did_first_run; }
void finder_prefs_set_did_show_panel_on_first_run(gboolean v) { s_did_first_run = v; }

const char *const *finder_prefs_favorites(void)
{
    ensure_defaults();
    return (const char *const *)s_favorites->pdata;
}

static gint fav_index(const char *path)
{
    for (guint i = 0; i + 1 < s_favorites->len; i++)
        if (strcmp((const char *)s_favorites->pdata[i], path) == 0)
            return (gint)i;
    return -1;
}

void finder_prefs_add_favorite(const char *path)
{
    if (!path || !*path) return;
    ensure_defaults();
    if (fav_index(path) >= 0) return;
    /* insert before the NULL terminator */
    g_ptr_array_insert(s_favorites, (gint)(s_favorites->len - 1), g_strdup(path));
    finder_prefs_save();
}

void finder_prefs_remove_favorite(const char *path)
{
    ensure_defaults();
    gint i = fav_index(path);
    if (i < 0) return;
    g_ptr_array_remove_index(s_favorites, (guint)i);
    finder_prefs_save();
}
