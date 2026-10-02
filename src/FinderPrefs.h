/*
 * FinderPrefs.h — persisted plugin settings. Linux port of
 * FinderPreferences.{h,mm}: same JSON file name and keys
 * (finder-plugin-prefs.json — a prefs file written by the macOS plugin
 * parses unchanged), json-glib in place of NSJSONSerialization (the
 * ElasticTabstops precedent).
 */
#pragma once

#include <glib.h>

/* Points the store at the host's plugins Config dir (created if absent)
 * and loads the file. Must run before any getter is trusted. */
void finder_prefs_configure(const char *config_dir);

void finder_prefs_save(void);

const char *finder_prefs_last_root_path(void);        /* never NULL */
void        finder_prefs_set_last_root_path(const char *path);

gboolean    finder_prefs_show_hidden(void);
void        finder_prefs_set_show_hidden(gboolean v);

gboolean    finder_prefs_did_show_panel_on_first_run(void);
void        finder_prefs_set_did_show_panel_on_first_run(gboolean v);

/* Favorites: a NULL-terminated vector owned by the store — do not free. */
const char *const *finder_prefs_favorites(void);
void finder_prefs_add_favorite(const char *path);      /* saves */
void finder_prefs_remove_favorite(const char *path);   /* saves */
