/*
 * FinderFileOps.h — stateless filesystem/desktop helpers. Linux port of
 * FinderFileOperations.{h,mm}: NSWorkspace/NSFileManager/NSTask become
 * GIO + GDK clipboard + best-effort spawns. No third-party dependency.
 *
 * Returned paths are newly allocated (g_free). On failure the *error_msg
 * out-param (when present) receives a newly allocated human-readable
 * message for the alert dialog (g_free), mirroring NSError's
 * localizedDescription role.
 */
#pragma once

#include <gtk/gtk.h>

/* "Reveal in Finder" analogue: select the item in the user's file
 * manager via the freedesktop org.freedesktop.FileManager1 D-Bus API
 * (GNOME Files, Dolphin, …); falls back to opening the containing
 * directory with the default handler when the call fails. */
void finder_ops_show_in_file_manager(const char *path);

/* Opens the user's terminal cd'd into `path` (containing dir for files).
 * No universal Linux API exists; tries, in order: $TERMINAL,
 * x-terminal-emulator, gnome-terminal, konsole, xfce4-terminal,
 * mate-terminal, kitty, alacritty, foot, xterm. */
void finder_ops_open_terminal(const char *path);

/* Double-click behaviour outside the editor: default application. */
void finder_ops_open_default(const char *path);

/* "Untitled.txt" / "Untitled 2.txt" …; returns the new path. */
char *finder_ops_create_new_file(const char *dir, char **error_msg);

/* "Untitled Folder" / "Untitled Folder 2" …; returns the new path. */
char *finder_ops_create_new_folder(const char *dir, char **error_msg);

/* Renames within the same directory; rejects empty names and '/'. */
char *finder_ops_rename(const char *path, const char *new_name,
                        char **error_msg);

/* freedesktop Trash (recoverable), the g_file_trash analogue of
 * NSWorkspace's recycle behaviour. */
gboolean finder_ops_trash(const char *path, char **error_msg);

/* Finder-style "Foo copy.ext", "Foo copy 2.ext" duplicate (recursive for
 * directories — GIO's g_file_copy does not recurse on its own). */
char *finder_ops_duplicate(const char *path, char **error_msg);

void finder_ops_copy_path_to_clipboard(GtkWidget *for_widget, const char *path);
void finder_ops_copy_name_to_clipboard(GtkWidget *for_widget, const char *path);
