#include "FinderLoc.h"

#include <string.h>

static gboolean s_is_english = TRUE;

void finder_loc_init_from_config_dir(const char *plugins_config_dir)
{
    if (!plugins_config_dir || !*plugins_config_dir) return;

    /* <data>/nextpad++/plugins/Config → <data>/nextpad++/config.xml */
    gchar *plugins_dir = g_path_get_dirname(plugins_config_dir);
    gchar *app_dir     = g_path_get_dirname(plugins_dir);
    gchar *config_xml  = g_build_filename(app_dir, "config.xml", NULL);
    g_free(plugins_dir);
    g_free(app_dir);

    gchar *data = NULL;
    if (g_file_get_contents(config_xml, &data, NULL, NULL) && data) {
        /* A targeted scan beats pulling in an XML parser for one
         * attribute: find the Localization GUIConfig, then its
         * language="…" value. The host writes this file itself, so the
         * attribute order/quoting is stable (prefs.c writer). */
        const char *loc = strstr(data, "name=\"Localization\"");
        if (loc) {
            const char *lang = strstr(loc, "language=\"");
            if (lang) {
                lang += strlen("language=\"");
                const char *end = strchr(lang, '"');
                if (end && end > lang) {
                    gchar *val = g_strndup(lang, (gsize)(end - lang));
                    s_is_english = (g_ascii_strcasecmp(val, "german") != 0);
                    g_free(val);
                }
            }
        }
    }
    g_free(data);
    g_free(config_xml);
}

gboolean finder_loc_is_english(void) { return s_is_english; }

const char *finder_loc_pick(const char *de, const char *en)
{
    return s_is_english ? en : de;
}
