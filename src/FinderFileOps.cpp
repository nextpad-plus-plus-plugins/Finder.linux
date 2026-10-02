#include "FinderFileOps.h"

#include <gio/gio.h>
#include <string.h>

/* ── reveal / open ──────────────────────────────────────────────────────── */

static void open_dir_fallback(const char *path)
{
    gchar *dir = g_file_test(path, G_FILE_TEST_IS_DIR)
                     ? g_strdup(path) : g_path_get_dirname(path);
    GFile *f = g_file_new_for_path(dir);
    gchar *uri = g_file_get_uri(f);
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_object_unref(f);
    g_free(dir);
}

void finder_ops_show_in_file_manager(const char *path)
{
    if (!path || !*path) return;

    GFile *f = g_file_new_for_path(path);
    gchar *uri = g_file_get_uri(f);
    g_object_unref(f);

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    gboolean ok = FALSE;
    if (bus) {
        const char *uris[] = { uri, NULL };
        GVariant *ret = g_dbus_connection_call_sync(bus,
            "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1", "ShowItems",
            g_variant_new("(^ass)", uris, ""),
            NULL, G_DBUS_CALL_FLAGS_NONE, 2000 /* ms */, NULL, NULL);
        if (ret) { g_variant_unref(ret); ok = TRUE; }
        g_object_unref(bus);
    }
    g_free(uri);

    if (!ok) open_dir_fallback(path);
}

void finder_ops_open_terminal(const char *path)
{
    if (!path || !*path) return;
    gchar *dir = g_file_test(path, G_FILE_TEST_IS_DIR)
                     ? g_strdup(path) : g_path_get_dirname(path);

    /* $TERMINAL first (power users set it), then the Debian alternatives
     * name, then the common emulators. Each candidate is spawned bare
     * with cwd=dir — every listed terminal starts its shell in the
     * process working directory. Fire-and-forget: no pipes are involved,
     * so none of the stdin/SIGPIPE spawn traps apply here. */
    const char *env_term = g_getenv("TERMINAL");
    const char *candidates[] = {
        env_term && *env_term ? env_term : NULL,
        "x-terminal-emulator", "gnome-terminal", "konsole",
        "xfce4-terminal", "mate-terminal", "kitty", "alacritty",
        "foot", "xterm", NULL
    };
    for (int i = 0; candidates[i] || i == 0; i++) {
        const char *term = candidates[i];
        if (!term) continue;
        gchar *exe = g_find_program_in_path(term);
        if (!exe) continue;
        gchar *argv[] = { exe, NULL };
        gboolean ok = g_spawn_async(dir, argv, NULL,
                                    G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(exe);
        if (ok) break;
    }
    g_free(dir);
}

void finder_ops_open_default(const char *path)
{
    if (!path || !*path) return;
    GFile *f = g_file_new_for_path(path);
    gchar *uri = g_file_get_uri(f);
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_object_unref(f);
}

/* ── create / rename / trash / duplicate ────────────────────────────────── */

/* "Untitled.txt", "Untitled 2.txt", … — the exact macOS
 * uniqueNameInDirectory loop (suffix starts at 2, caps at 10000). */
static gchar *unique_name_in_dir(const char *dir, const char *base,
                                 const char *extension /* no dot, or NULL */)
{
    gchar *ext = (extension && *extension)
                     ? g_strdup_printf(".%s", extension) : g_strdup("");
    gchar *candidate = g_strdup_printf("%s%s", base, ext);
    gchar *full = g_build_filename(dir, candidate, NULL);
    if (!g_file_test(full, G_FILE_TEST_EXISTS)) { g_free(full); g_free(ext); return candidate; }
    g_free(candidate);
    g_free(full);

    for (int i = 2; i < 10000; i++) {
        candidate = g_strdup_printf("%s %d%s", base, i, ext);
        full = g_build_filename(dir, candidate, NULL);
        gboolean exists = g_file_test(full, G_FILE_TEST_EXISTS);
        g_free(full);
        if (!exists) { g_free(ext); return candidate; }
        g_free(candidate);
    }
    gchar *uid = g_uuid_string_random();
    candidate = g_strdup_printf("%s %s%s", base, uid, ext);
    g_free(uid);
    g_free(ext);
    return candidate;
}

char *finder_ops_create_new_file(const char *dir, char **error_msg)
{
    gchar *name = unique_name_in_dir(dir, "Untitled", "txt");
    gchar *full = g_build_filename(dir, name, NULL);
    g_free(name);
    GFile *f = g_file_new_for_path(full);
    GError *err = NULL;
    GFileOutputStream *os = g_file_create(f, G_FILE_CREATE_NONE, NULL, &err);
    g_object_unref(f);
    if (!os) {
        if (error_msg) *error_msg = g_strdup(err ? err->message : "Could not create file.");
        g_clear_error(&err);
        g_free(full);
        return NULL;
    }
    g_output_stream_close(G_OUTPUT_STREAM(os), NULL, NULL);
    g_object_unref(os);
    return full;
}

char *finder_ops_create_new_folder(const char *dir, char **error_msg)
{
    gchar *name = unique_name_in_dir(dir, "Untitled Folder", NULL);
    gchar *full = g_build_filename(dir, name, NULL);
    g_free(name);
    if (g_mkdir_with_parents(full, 0755) != 0) {
        if (error_msg) *error_msg = g_strdup("Could not create folder.");
        g_free(full);
        return NULL;
    }
    return full;
}

char *finder_ops_rename(const char *path, const char *new_name,
                        char **error_msg)
{
    if (!new_name || !*new_name || strchr(new_name, '/')) {
        if (error_msg) *error_msg = g_strdup("Invalid name.");
        return NULL;
    }
    gchar *dir = g_path_get_dirname(path);
    gchar *new_path = g_build_filename(dir, new_name, NULL);
    g_free(dir);
    if (strcmp(new_path, path) == 0) return new_path;   /* no-op rename */
    if (g_file_test(new_path, G_FILE_TEST_EXISTS)) {
        if (error_msg) *error_msg = g_strdup("An item with that name already exists.");
        g_free(new_path);
        return NULL;
    }
    GFile *src = g_file_new_for_path(path);
    GFile *dst = g_file_new_for_path(new_path);
    GError *err = NULL;
    gboolean ok = g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dst);
    if (!ok) {
        if (error_msg) *error_msg = g_strdup(err ? err->message : "Rename failed.");
        g_clear_error(&err);
        g_free(new_path);
        return NULL;
    }
    return new_path;
}

gboolean finder_ops_trash(const char *path, char **error_msg)
{
    GFile *f = g_file_new_for_path(path);
    GError *err = NULL;
    gboolean ok = g_file_trash(f, NULL, &err);
    g_object_unref(f);
    if (!ok && error_msg)
        *error_msg = g_strdup(err ? err->message : "Could not move to Trash.");
    g_clear_error(&err);
    return ok;
}

/* Recursive copy — GIO's g_file_copy refuses G_IO_ERROR_WOULD_RECURSE on
 * directories, so walk them by hand (NSFileManager copyItemAtPath did
 * this for free on macOS). */
static gboolean copy_recursive(GFile *src, GFile *dst, GError **err)
{
    GFileType type = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    if (type == G_FILE_TYPE_DIRECTORY) {
        if (!g_file_make_directory(dst, NULL, err)) return FALSE;
        GFileEnumerator *en = g_file_enumerate_children(src,
            G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, err);
        if (!en) return FALSE;
        GFileInfo *info;
        gboolean ok = TRUE;
        while (ok && (info = g_file_enumerator_next_file(en, NULL, err)) != NULL) {
            const char *name = g_file_info_get_name(info);
            GFile *cs = g_file_get_child(src, name);
            GFile *cd = g_file_get_child(dst, name);
            ok = copy_recursive(cs, cd, err);
            g_object_unref(cs);
            g_object_unref(cd);
            g_object_unref(info);
        }
        g_object_unref(en);
        return ok && (!err || !*err);
    }
    return g_file_copy(src, dst,
        (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS), NULL, NULL, NULL, err);
}

char *finder_ops_duplicate(const char *path, char **error_msg)
{
    gchar *dir  = g_path_get_dirname(path);
    gchar *name = g_path_get_basename(path);

    /* Split "Foo.ext" → base "Foo" + ext "ext"; directories and dotless
     * names keep the whole name as base (macOS pathExtension rules). A
     * leading-dot name like ".bashrc" has no extension in this sense. */
    gchar *base = g_strdup(name);
    gchar *ext  = NULL;
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        char *dot = strrchr(base, '.');
        if (dot && dot != base) { ext = g_strdup(dot + 1); *dot = '\0'; }
    }

    gchar *full = NULL;
    gchar *cand_name = ext ? g_strdup_printf("%s copy.%s", base, ext)
                           : g_strdup_printf("%s copy", base);
    full = g_build_filename(dir, cand_name, NULL);
    g_free(cand_name);
    int suffix = 2;
    while (g_file_test(full, G_FILE_TEST_EXISTS)) {
        g_free(full);
        cand_name = ext ? g_strdup_printf("%s copy %d.%s", base, suffix, ext)
                        : g_strdup_printf("%s copy %d", base, suffix);
        full = g_build_filename(dir, cand_name, NULL);
        g_free(cand_name);
        if (++suffix > 10000) break;   /* safety valve, as on macOS */
    }

    GFile *src = g_file_new_for_path(path);
    GFile *dst = g_file_new_for_path(full);
    GError *err = NULL;
    gboolean ok = copy_recursive(src, dst, &err);
    g_object_unref(src);
    g_object_unref(dst);
    g_free(dir);
    g_free(name);
    g_free(base);
    g_free(ext);
    if (!ok) {
        if (error_msg) *error_msg = g_strdup(err ? err->message : "Duplicate failed.");
        g_clear_error(&err);
        g_free(full);
        return NULL;
    }
    return full;
}

/* ── clipboard ──────────────────────────────────────────────────────────── */

static void clipboard_set(GtkWidget *for_widget, const char *text)
{
    GdkClipboard *cb = for_widget ? gtk_widget_get_clipboard(for_widget)
                                  : gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(cb, text ? text : "");
}

void finder_ops_copy_path_to_clipboard(GtkWidget *for_widget, const char *path)
{
    clipboard_set(for_widget, path);
}

void finder_ops_copy_name_to_clipboard(GtkWidget *for_widget, const char *path)
{
    gchar *name = g_path_get_basename(path);
    clipboard_set(for_widget, name);
    g_free(name);
}
