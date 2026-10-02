/*
 * FinderPanel.h — the Finder sidebar widget. Linux/GTK4 port of
 * FinderPanelView.{h,mm}: toolbar (root dropdown / up / home / locate /
 * hidden toggle / new folder / new file / live filter), a GtkPaned of
 * folder tree (GtkTreeView, lazily loaded) and file list (GtkColumnView
 * with Name/Size/Modified, striped like the macOS table), plus the
 * Finder-style context menu. All filesystem access is synchronous GIO
 * enumeration scoped to one directory at a time, like the original.
 *
 * The view deliberately has no reference to NppData — it reports user
 * intent through the callbacks (the macOS delegate protocol) and the
 * plugin controller calls back in with navigate/reveal.
 */
#pragma once

#include <gtk/gtk.h>

typedef struct FinderPanel FinderPanel;

typedef struct {
    /* User double-clicked / activated a file — open it in Nextpad++. */
    void (*open_file)(const char *path, void *user);
    /* "Locate Current File" toolbar button: the controller knows the
     * active buffer's path and answers with reveal_and_select(). */
    void (*locate_current_file)(void *user);
    void *user;
} FinderPanelCallbacks;

FinderPanel *finder_panel_new(const FinderPanelCallbacks *cb);
GtkWidget   *finder_panel_widget(FinderPanel *p);

/* Shows `path`'s directory in tree + list; selects the file if `path`
 * is a file. Mirrors -navigateToPath:. */
void finder_panel_navigate_to_path(FinderPanel *p, const char *path);

/* Convenience for the Locate command (navigate + focus the list). */
void finder_panel_reveal_and_select(FinderPanel *p, const char *path);
