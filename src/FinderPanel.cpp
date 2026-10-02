#include "FinderPanel.h"
#include "FinderFileOps.h"
#include "FinderPrefs.h"
#include "FinderLoc.h"

#include <gio/gio.h>
#include <string.h>

/* GtkTreeView/GtkComboBoxText are deprecated-but-present in GTK 4.10+;
 * the host and earlier ports use them the same way. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* ─────────────────────────────────────────────────────────────────────────
 * FinderEntry — one file-list row (GObject so GListStore/GtkColumnView can
 * hold it; the host's Document List uses the same shape).
 * ───────────────────────────────────────────────────────────────────────── */

#define FINDER_TYPE_ENTRY (finder_entry_get_type())
G_DECLARE_FINAL_TYPE(FinderEntry, finder_entry, FINDER, ENTRY, GObject)

struct _FinderEntry {
    GObject   parent;
    char     *path;
    char     *name;
    gboolean  is_dir;
    guint64   size;
    gint64    mtime;      /* unix seconds */
    GIcon    *icon;       /* may be NULL */
};

G_DEFINE_TYPE(FinderEntry, finder_entry, G_TYPE_OBJECT)

static void finder_entry_finalize(GObject *obj)
{
    FinderEntry *e = FINDER_ENTRY(obj);
    g_free(e->path);
    g_free(e->name);
    g_clear_object(&e->icon);
    G_OBJECT_CLASS(finder_entry_parent_class)->finalize(obj);
}

static void finder_entry_class_init(FinderEntryClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = finder_entry_finalize;
}

static void finder_entry_init(FinderEntry *e) { (void)e; }

static FinderEntry *finder_entry_new(const char *dir, GFileInfo *info)
{
    FinderEntry *e = (FinderEntry *)g_object_new(FINDER_TYPE_ENTRY, NULL);
    e->name   = g_strdup(g_file_info_get_name(info));
    e->path   = g_build_filename(dir, e->name, NULL);
    e->is_dir = (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY);
    e->size   = (guint64)g_file_info_get_size(info);
    GDateTime *dt = g_file_info_get_modification_date_time(info);
    e->mtime  = dt ? g_date_time_to_unix(dt) : 0;
    if (dt) g_date_time_unref(dt);
    GIcon *ic = g_file_info_get_icon(info);
    e->icon   = ic ? (GIcon *)g_object_ref(ic) : NULL;
    return e;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Panel state
 * ───────────────────────────────────────────────────────────────────────── */

enum { TREE_COL_ICON, TREE_COL_NAME, TREE_COL_PATH, TREE_COL_PLACEHOLDER,
       TREE_N_COLS };

struct FinderPanel {
    FinderPanelCallbacks cb;

    GtkWidget *root;             /* the registered panel widget (vbox) */

    GtkWidget *combo;            /* root picker */
    GtkWidget *btn_up, *btn_home, *btn_locate, *btn_hidden;
    GtkWidget *btn_newfolder, *btn_newfile;
    GtkWidget *search;

    GtkWidget    *paned;
    GtkWidget    *tree_view;     /* folders */
    GtkTreeStore *tree_store;
    GtkWidget    *list_view;     /* files+folders, GtkColumnView */
    GListStore   *list_store;    /* filtered entries shown */
    GtkSingleSelection *list_sel;

    GPtrArray *entries;          /* FinderEntry*, unfiltered (owned refs) */

    char *root_path;             /* 🏠/💻/★/💾/custom context */
    char *current_list_path;     /* directory in the file list */

    GtkWidget *ctx_popover;      /* shared context menu */
    char      *ctx_target;       /* path the menu acts on */

    gboolean paned_placed;       /* one-shot 40% divider, like macOS */
    gulong   combo_changed_id;
};

/* forward decls */
static void reload_list_for_path(FinderPanel *p, const char *path);
static void reload_tree(FinderPanel *p);
static void set_root_path(FinderPanel *p, const char *path);
static void expand_tree_to_directory(FinderPanel *p, const char *dir);
static void rebuild_root_combo(FinderPanel *p);
static void sync_root_combo(FinderPanel *p);
static void update_hidden_button(FinderPanel *p);
static void refresh_tree_dir(FinderPanel *p, const char *dir);
static void select_and_scroll_named(FinderPanel *p, const char *name);
static void show_error_alert(FinderPanel *p, char *msg /* takes ownership */);
static void prompt_rename(FinderPanel *p, const char *path);

/* ─────────────────────────────────────────────────────────────────────────
 * Directory enumeration helpers (synchronous, one directory at a time —
 * the macOS FinderSubdirectories / FinderListEntries pair, via GIO so we
 * get the themed per-type icon in the same pass).
 * ───────────────────────────────────────────────────────────────────────── */

static gboolean is_hidden_name(const char *name)
{
    return name && name[0] == '.';
}

static const char *kEnumAttrs =
    G_FILE_ATTRIBUTE_STANDARD_NAME ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_STANDARD_ICON ","
    G_FILE_ATTRIBUTE_TIME_MODIFIED;

static gint entry_name_cmp(gconstpointer a, gconstpointer b)
{
    FinderEntry *ea = *(FinderEntry *const *)a;
    FinderEntry *eb = *(FinderEntry *const *)b;
    gchar *ka = g_utf8_casefold(ea->name, -1);
    gchar *kb = g_utf8_casefold(eb->name, -1);
    gint r = g_strcmp0(ka, kb);
    g_free(ka);
    g_free(kb);
    return r;
}

/* Files + folders of `dir`, folders first then files, both alphabetical
 * case-insensitively; hidden (dotfile) entries excluded unless shown. */
static GPtrArray *list_entries(const char *dir, gboolean show_hidden)
{
    GPtrArray *folders = g_ptr_array_new();
    GPtrArray *files   = g_ptr_array_new();

    GFile *gdir = g_file_new_for_path(dir);
    GFileEnumerator *en = g_file_enumerate_children(gdir, kEnumAttrs,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (en) {
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
            const char *name = g_file_info_get_name(info);
            if (show_hidden || !is_hidden_name(name)) {
                FinderEntry *e = finder_entry_new(dir, info);
                g_ptr_array_add(e->is_dir ? folders : files, e);
            }
            g_object_unref(info);
        }
        g_object_unref(en);
    }
    g_object_unref(gdir);

    g_ptr_array_sort(folders, entry_name_cmp);
    g_ptr_array_sort(files, entry_name_cmp);

    GPtrArray *all = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < folders->len; i++) g_ptr_array_add(all, folders->pdata[i]);
    for (guint i = 0; i < files->len; i++)   g_ptr_array_add(all, files->pdata[i]);
    g_ptr_array_free(folders, TRUE);
    g_ptr_array_free(files, TRUE);
    return all;
}

/* TRUE when `dir` has at least one visible subdirectory (the macOS
 * expandability probe — early exit on the first hit). */
static gboolean dir_has_subdir(const char *dir, gboolean show_hidden)
{
    gboolean found = FALSE;
    GFile *gdir = g_file_new_for_path(dir);
    GFileEnumerator *en = g_file_enumerate_children(gdir,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (en) {
        GFileInfo *info;
        while (!found &&
               (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY &&
                (show_hidden || !is_hidden_name(g_file_info_get_name(info))))
                found = TRUE;
            g_object_unref(info);
        }
        g_object_unref(en);
    }
    g_object_unref(gdir);
    return found;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Folder tree (lazy GtkTreeStore — the JSON-Viewer placeholder recipe)
 * ───────────────────────────────────────────────────────────────────────── */

static void tree_fill_children(FinderPanel *p, GtkTreeIter *parent,
                               const char *dir)
{
    gboolean show_hidden = finder_prefs_show_hidden();
    GPtrArray *subs = list_entries(dir, show_hidden);
    for (guint i = 0; i < subs->len; i++) {
        FinderEntry *e = (FinderEntry *)subs->pdata[i];
        if (!e->is_dir) continue;
        GtkTreeIter it;
        gtk_tree_store_append(p->tree_store, &it, parent);
        gtk_tree_store_set(p->tree_store, &it,
                           TREE_COL_ICON, e->icon,
                           TREE_COL_NAME, e->name,
                           TREE_COL_PATH, e->path,
                           TREE_COL_PLACEHOLDER, FALSE, -1);
        if (dir_has_subdir(e->path, show_hidden)) {
            GtkTreeIter ph;
            gtk_tree_store_append(p->tree_store, &ph, &it);
            gtk_tree_store_set(p->tree_store, &ph,
                               TREE_COL_NAME, "", TREE_COL_PATH, "",
                               TREE_COL_PLACEHOLDER, TRUE, -1);
        }
    }
    g_ptr_array_free(subs, TRUE);
}

/* If `iter`'s children are still the lazy placeholder, replace them with
 * the real subdirectories. Safe to call repeatedly.
 *
 * NextZip/JSON-Viewer's two hard-won lazy-tree rules apply verbatim
 * (PORTING_NOTES): append the REAL rows before removing the placeholder
 * — dropping the row to zero children mid-expand makes the view abandon
 * the expansion (the "clicking > does nothing" bug) — and populate from
 * "test-expand-row", which fires BEFORE the view expands. */
static void tree_ensure_children(FinderPanel *p, GtkTreeIter *iter)
{
    GtkTreeModel *m = GTK_TREE_MODEL(p->tree_store);
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(m, &child, iter)) return;
    gboolean placeholder = FALSE;
    gtk_tree_model_get(m, &child, TREE_COL_PLACEHOLDER, &placeholder, -1);
    if (!placeholder) return;

    gchar *dir = NULL;
    gtk_tree_model_get(m, iter, TREE_COL_PATH, &dir, -1);
    if (dir) tree_fill_children(p, iter, dir);   /* appended AFTER the placeholder */
    g_free(dir);

    /* Only now drop the placeholder row(s) — never zero children. */
    if (gtk_tree_model_iter_children(m, &child, iter)) {
        while (TRUE) {
            gboolean ph = FALSE;
            gtk_tree_model_get(m, &child, TREE_COL_PLACEHOLDER, &ph, -1);
            if (ph) {
                if (!gtk_tree_store_remove(p->tree_store, &child)) break;
            } else if (!gtk_tree_model_iter_next(m, &child)) {
                break;
            }
        }
    }
}

/* Env-gated diagnostics (NPP_FD_DEBUG=1) — the standard gated-trace
 * pattern; costs one g_getenv per event when off. */
static gboolean fd_debug(void)
{
    static int on = -1;
    if (on < 0) on = g_getenv("NPP_FD_DEBUG") ? 1 : 0;
    return on == 1;
}

static gboolean on_tree_test_expand_row(GtkTreeView *tv, GtkTreeIter *iter,
                                        GtkTreePath *path, gpointer user)
{
    (void)tv; (void)path;
    if (fd_debug()) {
        gchar *dir = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(((FinderPanel *)user)->tree_store),
                           iter, TREE_COL_PATH, &dir, -1);
        g_message("[Finder] test-expand-row: %s", dir ? dir : "?");
        g_free(dir);
    }
    tree_ensure_children((FinderPanel *)user, iter);
    return FALSE;   /* allow the expansion */
}

static void reload_tree(FinderPanel *p)
{
    gtk_tree_store_clear(p->tree_store);
    tree_fill_children(p, NULL, p->root_path);
}

/* Finds the iter whose PATH column equals `path` among `parent`'s
 * children (children get materialised first). */
static gboolean tree_find_child(FinderPanel *p, GtkTreeIter *parent,
                                const char *path, GtkTreeIter *out)
{
    if (parent) tree_ensure_children(p, parent);
    GtkTreeModel *m = GTK_TREE_MODEL(p->tree_store);
    GtkTreeIter it;
    if (!gtk_tree_model_iter_children(m, &it, parent)) return FALSE;
    do {
        gchar *ip = NULL;
        gtk_tree_model_get(m, &it, TREE_COL_PATH, &ip, -1);
        gboolean hit = (ip && strcmp(ip, path) == 0);
        g_free(ip);
        if (hit) { *out = it; return TRUE; }
    } while (gtk_tree_model_iter_next(m, &it));
    return FALSE;
}

/* Walks root → dir, filling + expanding each level; selects the final
 * row. Port of -expandTreeToDirectory: (silently stops when a component
 * is not visible, e.g. hidden while Show Hidden is off). */
static void expand_tree_to_directory(FinderPanel *p, const char *dir)
{
    if (!dir || strcmp(dir, p->root_path) == 0) return;
    if (!g_str_has_prefix(dir, p->root_path)) return;

    const char *rel = dir + strlen(p->root_path);
    gchar **comps = g_strsplit(rel, "/", -1);
    GString *running = g_string_new(p->root_path);
    GtkTreeIter parent;
    gboolean have_parent = FALSE;

    for (int i = 0; comps[i]; i++) {
        if (!*comps[i]) continue;
        if (running->len == 0 || running->str[running->len - 1] != '/')
            g_string_append_c(running, '/');
        g_string_append(running, comps[i]);

        GtkTreeIter found;
        if (!tree_find_child(p, have_parent ? &parent : NULL,
                             running->str, &found))
            break;

        if (have_parent) {
            GtkTreePath *tp = gtk_tree_model_get_path(
                GTK_TREE_MODEL(p->tree_store), &parent);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(p->tree_view), tp, FALSE);
            gtk_tree_path_free(tp);
        }
        parent = found;
        have_parent = TRUE;
        GtkTreePath *tp = gtk_tree_model_get_path(
            GTK_TREE_MODEL(p->tree_store), &parent);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(p->tree_view), tp, FALSE);
        gtk_tree_path_free(tp);
    }
    g_strfreev(comps);
    g_string_free(running, TRUE);

    if (have_parent) {
        GtkTreePath *tp = gtk_tree_model_get_path(
            GTK_TREE_MODEL(p->tree_store), &parent);
        gtk_tree_selection_select_iter(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree_view)), &parent);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(p->tree_view), tp, NULL,
                                     FALSE, 0, 0);
        gtk_tree_path_free(tp);
    }
}

/* Refreshes the children of ONE directory node after a mutation (new
 * folder, rename, trash) — keeps every other expanded branch intact,
 * like the macOS reloadOutline/-reloadData equality trick. Root-level
 * mutations re-fill the top level (expansion within other branches is
 * then rebuilt lazily). */
static void refresh_tree_dir(FinderPanel *p, const char *dir)
{
    if (strcmp(dir, p->root_path) == 0) {
        gchar *remember = g_strdup(p->current_list_path);
        reload_tree(p);
        expand_tree_to_directory(p, remember);
        g_free(remember);
        return;
    }
    GtkTreeIter it;
    /* Walk down from root to the node. */
    expand_tree_to_directory(p, dir);   /* materialises the chain */
    if (!g_str_has_prefix(dir, p->root_path)) return;
    /* find the node by path, starting at top level */
    const char *rel = dir + strlen(p->root_path);
    gchar **comps = g_strsplit(rel, "/", -1);
    GString *running = g_string_new(p->root_path);
    GtkTreeIter parent;
    gboolean have = FALSE;
    gboolean ok = TRUE;
    for (int i = 0; comps[i] && ok; i++) {
        if (!*comps[i]) continue;
        if (running->len == 0 || running->str[running->len - 1] != '/')
            g_string_append_c(running, '/');
        g_string_append(running, comps[i]);
        GtkTreeIter found;
        ok = tree_find_child(p, have ? &parent : NULL, running->str, &found);
        if (ok) { parent = found; have = TRUE; }
    }
    g_strfreev(comps);
    g_string_free(running, TRUE);
    if (!ok || !have) return;
    it = parent;

    GtkTreeModel *m = GTK_TREE_MODEL(p->tree_store);
    GtkTreeIter child;
    while (gtk_tree_model_iter_children(m, &child, &it))
        gtk_tree_store_remove(p->tree_store, &child);
    tree_fill_children(p, &it, dir);
}

static void on_tree_selection_changed(GtkTreeSelection *sel, gpointer user)
{
    FinderPanel *p = (FinderPanel *)user;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, &m, &it)) {
        gchar *path = NULL;
        gtk_tree_model_get(m, &it, TREE_COL_PATH, &path, -1);
        if (path && *path) reload_list_for_path(p, path);
        g_free(path);
    } else {
        reload_list_for_path(p, p->root_path);
    }
}

static void on_tree_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                  GtkTreeViewColumn *col, gpointer user)
{
    (void)col; (void)user;
    /* Double-click expands, like -outlineDoubleClicked:. */
    gtk_tree_view_expand_row(tv, path, FALSE);
}

/* ─────────────────────────────────────────────────────────────────────────
 * File list (GtkColumnView — the host Document List architecture; CSS
 * striping comes from even/odd row classes set at bind time, because
 * recycled list rows have no stable sibling order for :nth-child)
 * ───────────────────────────────────────────────────────────────────────── */

static void row_widget_set_stripe(GtkWidget *cell_child, guint position)
{
    GtkWidget *w = cell_child;
    for (int depth = 0; w && depth < 5; depth++) {
        if (g_strcmp0(gtk_widget_get_css_name(w), "row") == 0) break;
        w = gtk_widget_get_parent(w);
    }
    if (!w) return;
    gtk_widget_remove_css_class(w, "finder-even");
    gtk_widget_remove_css_class(w, "finder-odd");
    gtk_widget_add_css_class(w, (position % 2) ? "finder-odd" : "finder-even");
}

static void open_or_navigate(FinderPanel *p, const char *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        finder_panel_navigate_to_path(p, path);
    } else if (p->cb.open_file) {
        p->cb.open_file(path, p->cb.user);
    }
}

static void show_context_menu_for(FinderPanel *p, const char *target,
                                  GtkWidget *anchor, double x, double y);

/* Per-cell right-click: resolve the row via the bound GtkListItem. */
static void on_cell_right_click(GtkGestureClick *g, int n_press,
                                double x, double y, gpointer user)
{
    (void)n_press;
    FinderPanel *p = (FinderPanel *)user;
    GtkWidget *cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    gpointer li_p = g_object_get_data(G_OBJECT(cell), "finder-li");
    if (!li_p) return;
    GtkListItem *li = GTK_LIST_ITEM(li_p);
    guint pos = gtk_list_item_get_position(li);
    FinderEntry *e = (FinderEntry *)g_list_model_get_item(
        G_LIST_MODEL(p->list_sel), pos);
    if (!e) return;
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(p->list_sel), pos, TRUE);
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    show_context_menu_for(p, e->path, cell, x, y);
    g_object_unref(e);
}

static void cell_add_context_gesture(FinderPanel *p, GtkWidget *cell_child,
                                     GtkListItem *li)
{
    g_object_set_data(G_OBJECT(cell_child), "finder-li", li);
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect(g, "pressed", G_CALLBACK(on_cell_right_click), p);
    gtk_widget_add_controller(cell_child, GTK_EVENT_CONTROLLER(g));
}

/* Name column: [icon][label] */
static void name_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void)f;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_list_item_set_child(li, box);
    cell_add_context_gesture((FinderPanel *)user, box, li);
}

static void name_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void)f; (void)user;
    GtkWidget *box = gtk_list_item_get_child(li);
    FinderEntry *e = (FinderEntry *)gtk_list_item_get_item(li);
    GtkWidget *img = gtk_widget_get_first_child(box);
    GtkWidget *lbl = gtk_widget_get_next_sibling(img);
    if (e->icon) gtk_image_set_from_gicon(GTK_IMAGE(img), e->icon);
    else gtk_image_set_from_icon_name(GTK_IMAGE(img),
             e->is_dir ? "folder-symbolic" : "text-x-generic-symbolic");
    gtk_label_set_text(GTK_LABEL(lbl), e->name);
    row_widget_set_stripe(box, gtk_list_item_get_position(li));
}

/* Size / Modified columns: one right-aligned label. */
static void plain_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void)f;
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0f);
    gtk_list_item_set_child(li, lbl);
    cell_add_context_gesture((FinderPanel *)user, lbl, li);
}

static void size_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void)f; (void)user;
    FinderEntry *e = (FinderEntry *)gtk_list_item_get_item(li);
    GtkWidget *lbl = gtk_list_item_get_child(li);
    if (e->is_dir) {
        gtk_label_set_text(GTK_LABEL(lbl), "--");
    } else {
        gchar *s = g_format_size(e->size);
        gtk_label_set_text(GTK_LABEL(lbl), s);
        g_free(s);
    }
}

static void date_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer user)
{
    (void)f; (void)user;
    FinderEntry *e = (FinderEntry *)gtk_list_item_get_item(li);
    GtkWidget *lbl = gtk_list_item_get_child(li);
    if (e->mtime > 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(e->mtime);
        gchar *s = g_date_time_format(dt, "%x %R");
        gtk_label_set_text(GTK_LABEL(lbl), s);
        g_free(s);
        g_date_time_unref(dt);
    } else {
        gtk_label_set_text(GTK_LABEL(lbl), "");
    }
}

static void on_list_activate(GtkColumnView *cv, guint position, gpointer user)
{
    (void)cv;
    FinderPanel *p = (FinderPanel *)user;
    FinderEntry *e = (FinderEntry *)g_list_model_get_item(
        G_LIST_MODEL(p->list_sel), position);
    if (!e) return;
    open_or_navigate(p, e->path);
    g_object_unref(e);
}

static void apply_search_filter(FinderPanel *p)
{
    const char *query = gtk_editable_get_text(GTK_EDITABLE(p->search));
    g_list_store_remove_all(p->list_store);
    gchar *qfold = (query && *query) ? g_utf8_casefold(query, -1) : NULL;
    for (guint i = 0; i < p->entries->len; i++) {
        FinderEntry *e = (FinderEntry *)p->entries->pdata[i];
        gboolean keep = TRUE;
        if (qfold) {
            gchar *nfold = g_utf8_casefold(e->name, -1);
            keep = (strstr(nfold, qfold) != NULL);
            g_free(nfold);
        }
        if (keep) g_list_store_append(p->list_store, e);
    }
    g_free(qfold);
}

static void on_search_changed(GtkSearchEntry *e, gpointer user)
{
    (void)e;
    apply_search_filter((FinderPanel *)user);
}

static void reload_list_for_path(FinderPanel *p, const char *path)
{
    if (p->current_list_path != path) {
        g_free(p->current_list_path);
        p->current_list_path = g_strdup(path);
    }
    g_ptr_array_set_size(p->entries, 0);
    GPtrArray *fresh = list_entries(path, finder_prefs_show_hidden());
    for (guint i = 0; i < fresh->len; i++)
        g_ptr_array_add(p->entries, g_object_ref(fresh->pdata[i]));
    g_ptr_array_free(fresh, TRUE);
    apply_search_filter(p);
}

static void select_and_scroll_named(FinderPanel *p, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->list_store));
    for (guint i = 0; i < n; i++) {
        FinderEntry *e = (FinderEntry *)g_list_model_get_item(
            G_LIST_MODEL(p->list_store), i);
        gboolean hit = (strcmp(e->name, name) == 0);
        g_object_unref(e);
        if (hit) {
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->list_view), i, NULL,
                                      GTK_LIST_SCROLL_SELECT, NULL);
            return;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Root dropdown (🏠 Home / 💻 Computer / ★ favorites / 💾 mounts / Choose…)
 * ───────────────────────────────────────────────────────────────────────── */

static const char kChooseId[] = "::choose";

static void on_choose_folder_done(GObject *src, GAsyncResult *res, gpointer user)
{
    FinderPanel *p = (FinderPanel *)user;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        gchar *path = g_file_get_path(f);
        if (path) set_root_path(p, path);
        g_free(path);
        g_object_unref(f);
    } else {
        sync_root_combo(p);   /* cancelled — restore the previous choice */
    }
}

static void on_root_combo_changed(GtkComboBox *combo, gpointer user)
{
    FinderPanel *p = (FinderPanel *)user;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    if (strcmp(id, kChooseId) == 0) {
        GtkFileDialog *dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, FDLoc("Ordner wählen…", "Choose Folder…"));
        GtkRoot *root = gtk_widget_get_root(p->root);
        gtk_file_dialog_select_folder(dlg,
            GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
            on_choose_folder_done, p);
        g_object_unref(dlg);
        return;
    }
    set_root_path(p, id);
}

static void rebuild_root_combo(FinderPanel *p)
{
    GtkComboBoxText *c = GTK_COMBO_BOX_TEXT(p->combo);
    g_signal_handler_block(p->combo, p->combo_changed_id);
    gtk_combo_box_text_remove_all(c);

    gtk_combo_box_text_append(c, g_get_home_dir(), "🏠 Home");
    gtk_combo_box_text_append(c, "/", "💻 Computer");

    const char *const *favs = finder_prefs_favorites();
    for (int i = 0; favs[i]; i++) {
        gchar *base = g_path_get_basename(favs[i]);
        gchar *label = g_strdup_printf("★ %s", base);
        gtk_combo_box_text_append(c, favs[i], label);
        g_free(label);
        g_free(base);
    }

    GVolumeMonitor *vm = g_volume_monitor_get();
    GList *mounts = g_volume_monitor_get_mounts(vm);
    for (GList *l = mounts; l; l = l->next) {
        GMount *m = G_MOUNT(l->data);
        GFile *root = g_mount_get_root(m);
        gchar *path = root ? g_file_get_path(root) : NULL;
        if (path && strcmp(path, "/") != 0) {   /* "/" = Computer already */
            gchar *base = g_path_get_basename(path);
            gchar *label = g_strdup_printf("💾 %s", base);
            gtk_combo_box_text_append(c, path, label);
            g_free(label);
            g_free(base);
        }
        g_free(path);
        g_clear_object(&root);
        g_object_unref(m);
    }
    g_list_free(mounts);
    g_object_unref(vm);

    gtk_combo_box_text_append(c, kChooseId,
                              FDLoc("Ordner wählen…", "Choose Folder…"));

    g_signal_handler_unblock(p->combo, p->combo_changed_id);
    sync_root_combo(p);
}

static void sync_root_combo(FinderPanel *p)
{
    g_signal_handler_block(p->combo, p->combo_changed_id);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->combo), p->root_path)) {
        /* custom folder not present in the list — park on the sentinel,
         * like the macOS popup staying on "Choose Folder…". */
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->combo), kChooseId);
    }
    g_signal_handler_unblock(p->combo, p->combo_changed_id);
}

static void set_root_path(FinderPanel *p, const char *path)
{
    g_free(p->root_path);
    p->root_path = g_strdup(path);
    finder_prefs_set_last_root_path(path);
    finder_prefs_save();
    reload_tree(p);
    reload_list_for_path(p, path);
    sync_root_combo(p);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Toolbar actions
 * ───────────────────────────────────────────────────────────────────────── */

static void update_hidden_button(FinderPanel *p)
{
    gboolean show = finder_prefs_show_hidden();
    const char *tooltip = show
        ? FDLoc("Versteckte Dateien ausblenden", "Hide Hidden Files")
        : FDLoc("Versteckte Dateien anzeigen", "Show Hidden Files");
    /* Icon shows the CURRENT state (eye = hidden files visible), the
     * tooltip names the action — mirrors -updateHiddenButton. */
    gtk_button_set_icon_name(GTK_BUTTON(p->btn_hidden),
        show ? "view-reveal-symbolic" : "view-conceal-symbolic");
    gtk_widget_set_tooltip_text(p->btn_hidden, tooltip);
}

static void on_toggle_hidden(GtkButton *b, gpointer user)
{
    (void)b;
    FinderPanel *p = (FinderPanel *)user;
    finder_prefs_set_show_hidden(!finder_prefs_show_hidden());
    finder_prefs_save();
    update_hidden_button(p);
    gchar *remember = g_strdup(p->current_list_path);
    reload_tree(p);
    reload_list_for_path(p, remember);
    expand_tree_to_directory(p, remember);
    g_free(remember);
}

static void on_go_up(GtkButton *b, gpointer user)
{
    (void)b;
    FinderPanel *p = (FinderPanel *)user;
    if (strcmp(p->current_list_path, "/") == 0) return;
    gchar *parent = g_path_get_dirname(p->current_list_path);
    finder_panel_navigate_to_path(p, parent);
    g_free(parent);
}

static void on_go_home(GtkButton *b, gpointer user)
{
    (void)b;
    set_root_path((FinderPanel *)user, g_get_home_dir());
}

static void on_locate(GtkButton *b, gpointer user)
{
    (void)b;
    FinderPanel *p = (FinderPanel *)user;
    if (p->cb.locate_current_file) p->cb.locate_current_file(p->cb.user);
}

static void after_mutation_refresh(FinderPanel *p, const char *created_name)
{
    gchar *dir = g_strdup(p->current_list_path);
    reload_list_for_path(p, dir);
    refresh_tree_dir(p, dir);
    if (created_name) select_and_scroll_named(p, created_name);
    g_free(dir);
}

static void on_new_folder(GtkButton *b, gpointer user)
{
    (void)b;
    FinderPanel *p = (FinderPanel *)user;
    char *err = NULL;
    char *path = finder_ops_create_new_folder(p->current_list_path, &err);
    if (!path) { show_error_alert(p, err); return; }
    gchar *name = g_path_get_basename(path);
    after_mutation_refresh(p, name);
    prompt_rename(p, path);
    g_free(name);
    g_free(path);
}

static void on_new_file(GtkButton *b, gpointer user)
{
    (void)b;
    FinderPanel *p = (FinderPanel *)user;
    char *err = NULL;
    char *path = finder_ops_create_new_file(p->current_list_path, &err);
    if (!path) { show_error_alert(p, err); return; }
    gchar *name = g_path_get_basename(path);
    after_mutation_refresh(p, name);
    prompt_rename(p, path);
    g_free(name);
    g_free(path);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Dialogs
 * ───────────────────────────────────────────────────────────────────────── */

static GtkWindow *panel_window(FinderPanel *p)
{
    GtkRoot *root = gtk_widget_get_root(p->root);
    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

static void show_error_alert(FinderPanel *p, char *msg)
{
    if (!msg) return;
    GtkAlertDialog *dlg = gtk_alert_dialog_new(
        "%s", FDLoc("Aktion fehlgeschlagen", "Action Failed"));
    gtk_alert_dialog_set_detail(dlg, msg);
    gtk_alert_dialog_show(dlg, panel_window(p));
    g_object_unref(dlg);
    g_free(msg);
}

/* Rename prompt — a small modal window (entry + Cancel/OK). Per the
 * systemic plugin rule, the window hides on close and is destroyed only
 * after the last widget read. */
typedef struct {
    FinderPanel *panel;
    GtkWidget   *win;
    GtkWidget   *entry;
    char        *path;
} RenameCtx;

static void rename_ctx_finish(RenameCtx *ctx)
{
    gtk_window_destroy(GTK_WINDOW(ctx->win));
    g_free(ctx->path);
    g_free(ctx);
}

static void on_rename_ok(GtkButton *b, gpointer user)
{
    (void)b;
    RenameCtx *ctx = (RenameCtx *)user;
    FinderPanel *p = ctx->panel;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
    gtk_widget_set_visible(ctx->win, FALSE);

    char *err = NULL;
    char *new_path = finder_ops_rename(ctx->path, new_name, &err);
    if (!new_path) {
        show_error_alert(p, err);
    } else {
        gchar *name = g_path_get_basename(new_path);
        after_mutation_refresh(p, name);
        g_free(name);
        g_free(new_path);
    }
    rename_ctx_finish(ctx);
}

static void on_rename_cancel(GtkButton *b, gpointer user)
{
    (void)b;
    RenameCtx *ctx = (RenameCtx *)user;
    gtk_widget_set_visible(ctx->win, FALSE);
    rename_ctx_finish(ctx);
}

static gboolean on_rename_close(GtkWindow *w, gpointer user)
{
    (void)w;
    RenameCtx *ctx = (RenameCtx *)user;
    /* hide-on-close is set; defer the teardown out of the close path. */
    g_idle_add_once((GSourceOnceFunc)rename_ctx_finish, ctx);
    return FALSE;
}

static void prompt_rename(FinderPanel *p, const char *path)
{
    RenameCtx *ctx = g_new0(RenameCtx, 1);
    ctx->panel = p;
    ctx->path  = g_strdup(path);

    GtkWidget *win = gtk_window_new();
    ctx->win = win;
    gtk_window_set_title(GTK_WINDOW(win), FDLoc("Umbenennen", "Rename"));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), panel_window(p));
    gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 300, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    GtkWidget *entry = gtk_entry_new();
    ctx->entry = entry;
    gchar *base = g_path_get_basename(path);
    gtk_editable_set_text(GTK_EDITABLE(entry), base);
    g_free(base);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label(FDLoc("Abbrechen", "Cancel"));
    GtkWidget *ok     = gtk_button_new_with_label(FDLoc("OK", "OK"));
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(btns), cancel);
    gtk_box_append(GTK_BOX(btns), ok);
    gtk_box_append(GTK_BOX(box), btns);
    gtk_window_set_child(GTK_WINDOW(win), box);

    g_signal_connect(ok, "clicked", G_CALLBACK(on_rename_ok), ctx);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_rename_cancel), ctx);
    g_signal_connect(entry, "activate", G_CALLBACK(on_rename_ok), ctx);
    g_signal_connect(win, "close-request", G_CALLBACK(on_rename_close), ctx);

    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
}

/* Trash confirm — GtkAlertDialog, first button = Move to Trash (the
 * macOS NSAlert button order). */
typedef struct { FinderPanel *panel; char *path; } TrashCtx;

static void on_trash_choice(GObject *src, GAsyncResult *res, gpointer user)
{
    TrashCtx *ctx = (TrashCtx *)user;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (choice == 0) {
        char *err = NULL;
        if (!finder_ops_trash(ctx->path, &err)) {
            show_error_alert(ctx->panel, err);
        } else {
            after_mutation_refresh(ctx->panel, NULL);
        }
    }
    g_free(ctx->path);
    g_free(ctx);
}

static void confirm_trash(FinderPanel *p, const char *path)
{
    gchar *base = g_path_get_basename(path);
    gchar *msg = g_strdup_printf(
        FDLoc("\"%s\" in den Papierkorb legen?", "Move \"%s\" to Trash?"), base);
    g_free(base);

    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
    g_free(msg);
    const char *buttons[] = {
        FDLoc("In den Papierkorb legen", "Move to Trash"),
        FDLoc("Abbrechen", "Cancel"), NULL
    };
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 1);

    TrashCtx *ctx = g_new0(TrashCtx, 1);
    ctx->panel = p;
    ctx->path = g_strdup(path);
    gtk_alert_dialog_choose(dlg, panel_window(p), NULL, on_trash_choice, ctx);
    g_object_unref(dlg);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Context menu (GMenu + GtkPopoverMenu — never a raw GtkPopover)
 * ───────────────────────────────────────────────────────────────────────── */

static void ctx_open(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (p->ctx_target) open_or_navigate(p, p->ctx_target);
}

static void ctx_reveal(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    finder_ops_show_in_file_manager(p->ctx_target ? p->ctx_target
                                                  : p->current_list_path);
}

static void ctx_terminal(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    finder_ops_open_terminal(p->ctx_target ? p->ctx_target
                                           : p->current_list_path);
}

static void ctx_new_folder(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    on_new_folder(NULL, user);
}

static void ctx_new_file(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    on_new_file(NULL, user);
}

static void ctx_rename(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (p->ctx_target) prompt_rename(p, p->ctx_target);
}

static void ctx_duplicate(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (!p->ctx_target) return;
    char *err = NULL;
    char *new_path = finder_ops_duplicate(p->ctx_target, &err);
    if (!new_path) { show_error_alert(p, err); return; }
    after_mutation_refresh(p, NULL);
    g_free(new_path);
}

static void ctx_trash(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (p->ctx_target) confirm_trash(p, p->ctx_target);
}

static void ctx_copy_path(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (p->ctx_target)
        finder_ops_copy_path_to_clipboard(p->root, p->ctx_target);
}

static void ctx_copy_name(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (p->ctx_target)
        finder_ops_copy_name_to_clipboard(p->root, p->ctx_target);
}

static void ctx_add_favorite(GSimpleAction *a, GVariant *v, gpointer user)
{
    (void)a; (void)v;
    FinderPanel *p = (FinderPanel *)user;
    if (!p->ctx_target) return;
    gchar *folder = g_file_test(p->ctx_target, G_FILE_TEST_IS_DIR)
                        ? g_strdup(p->ctx_target)
                        : g_path_get_dirname(p->ctx_target);
    finder_prefs_add_favorite(folder);
    g_free(folder);
    rebuild_root_combo(p);
}

static GMenu *build_context_model(void)
{
    GMenu *menu = g_menu_new();

    GMenu *s1 = g_menu_new();
    g_menu_append(s1, FDLoc("Öffnen", "Open"), "finder.open");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));

    GMenu *s2 = g_menu_new();
    g_menu_append(s2, FDLoc("Im Dateimanager anzeigen", "Show in File Manager"),
                  "finder.reveal");
    g_menu_append(s2, FDLoc("Im Terminal öffnen", "Open in Terminal"),
                  "finder.terminal");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));

    GMenu *s3 = g_menu_new();
    g_menu_append(s3, FDLoc("Neuer Ordner", "New Folder"), "finder.new-folder");
    g_menu_append(s3, FDLoc("Neue Datei", "New File"), "finder.new-file");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));

    GMenu *s4 = g_menu_new();
    g_menu_append(s4, FDLoc("Umbenennen…", "Rename…"), "finder.rename");
    g_menu_append(s4, FDLoc("Duplizieren", "Duplicate"), "finder.duplicate");
    g_menu_append(s4, FDLoc("In den Papierkorb legen", "Move to Trash"),
                  "finder.trash");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s4));

    GMenu *s5 = g_menu_new();
    g_menu_append(s5, FDLoc("Pfad kopieren", "Copy Path"), "finder.copy-path");
    g_menu_append(s5, FDLoc("Name kopieren", "Copy Name"), "finder.copy-name");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s5));

    GMenu *s6 = g_menu_new();
    g_menu_append(s6, FDLoc("Als Favorit hinzufügen", "Add to Favorites"),
                  "finder.add-favorite");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s6));

    g_object_unref(s1); g_object_unref(s2); g_object_unref(s3);
    g_object_unref(s4); g_object_unref(s5); g_object_unref(s6);
    return menu;
}

static void install_context_actions(FinderPanel *p)
{
    static const struct {
        const char *name;
        void (*fn)(GSimpleAction *, GVariant *, gpointer);
    } acts[] = {
        { "open",         ctx_open },
        { "reveal",       ctx_reveal },
        { "terminal",     ctx_terminal },
        { "new-folder",   ctx_new_folder },
        { "new-file",     ctx_new_file },
        { "rename",       ctx_rename },
        { "duplicate",    ctx_duplicate },
        { "trash",        ctx_trash },
        { "copy-path",    ctx_copy_path },
        { "copy-name",    ctx_copy_name },
        { "add-favorite", ctx_add_favorite },
    };
    GSimpleActionGroup *grp = g_simple_action_group_new();
    for (gsize i = 0; i < G_N_ELEMENTS(acts); i++) {
        GSimpleAction *a = g_simple_action_new(acts[i].name, NULL);
        g_signal_connect(a, "activate", G_CALLBACK(acts[i].fn), p);
        g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(p->root, "finder", G_ACTION_GROUP(grp));
    g_object_unref(grp);

    GMenu *model = build_context_model();
    p->ctx_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
    g_object_unref(model);
    gtk_popover_set_has_arrow(GTK_POPOVER(p->ctx_popover), FALSE);
    gtk_widget_set_parent(p->ctx_popover, p->root);
}

static void show_context_menu_for(FinderPanel *p, const char *target,
                                  GtkWidget *anchor, double x, double y)
{
    g_free(p->ctx_target);
    p->ctx_target = target ? g_strdup(target) : NULL;

    graphene_point_t pt = GRAPHENE_POINT_INIT((float)x, (float)y);
    graphene_point_t out;
    if (!gtk_widget_compute_point(anchor, p->root, &pt, &out)) {
        out.x = (float)x; out.y = (float)y;
    }
    GdkRectangle rect = { (int)out.x, (int)out.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(p->ctx_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(p->ctx_popover));
}

/* Tree right-click: pick the row under the pointer. */
static void on_tree_right_click(GtkGestureClick *g, int n_press,
                                double x, double y, gpointer user)
{
    (void)n_press;
    FinderPanel *p = (FinderPanel *)user;
    GtkTreeView *tv = GTK_TREE_VIEW(p->tree_view);
    GtkTreePath *tp = NULL;
    gchar *target = NULL;
    gint bx, by;
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, (gint)x, (gint)y,
                                                      &bx, &by);
    if (gtk_tree_view_get_path_at_pos(tv, bx, by, &tp, NULL, NULL, NULL)) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(p->tree_store), &it, tp)) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(tv), &it);
            gtk_tree_model_get(GTK_TREE_MODEL(p->tree_store), &it,
                               TREE_COL_PATH, &target, -1);
        }
        gtk_tree_path_free(tp);
    }
    show_context_menu_for(p, target ? target : p->current_list_path,
                          GTK_WIDGET(tv), x, y);
    g_free(target);
}

/* Right-click on empty file-list space (cells claim their own clicks). */
static void on_list_bg_right_click(GtkGestureClick *g, int n_press,
                                   double x, double y, gpointer user)
{
    (void)g; (void)n_press;
    FinderPanel *p = (FinderPanel *)user;
    show_context_menu_for(p, p->current_list_path, p->list_view, x, y);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────── */

void finder_panel_navigate_to_path(FinderPanel *p, const char *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return;
    gboolean is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
    gchar *dir_to_show = is_dir ? g_strdup(path) : g_path_get_dirname(path);

    /* Target under a hidden folder while hidden entries are filtered:
     * switch the option on (and persist, like a manual toggle) rather
     * than silently stopping at the last visible ancestor. */
    if (!finder_prefs_show_hidden()) {
        const char *rel = g_str_has_prefix(dir_to_show, p->root_path)
                              ? dir_to_show + strlen(p->root_path)
                              : dir_to_show;
        gchar **comps = g_strsplit(rel, "/", -1);
        for (int i = 0; comps[i]; i++) {
            if (is_hidden_name(comps[i])) {
                finder_prefs_set_show_hidden(TRUE);
                finder_prefs_save();
                update_hidden_button(p);
                reload_tree(p);
                break;
            }
        }
        g_strfreev(comps);
    }

    if (!g_str_has_prefix(dir_to_show, p->root_path)) {
        /* Not reachable from the current root — fall back to Computer. */
        set_root_path(p, "/");
    }

    expand_tree_to_directory(p, dir_to_show);
    reload_list_for_path(p, dir_to_show);

    if (!is_dir) {
        gchar *name = g_path_get_basename(path);
        select_and_scroll_named(p, name);
        g_free(name);
    }
    g_free(dir_to_show);
}

void finder_panel_reveal_and_select(FinderPanel *p, const char *path)
{
    finder_panel_navigate_to_path(p, path);
    gtk_widget_grab_focus(p->list_view);
}

GtkWidget *finder_panel_widget(FinderPanel *p) { return p->root; }

/* ─────────────────────────────────────────────────────────────────────────
 * Construction
 * ───────────────────────────────────────────────────────────────────────── */

static GtkWidget *toolbar_button(const char *icon, const char *tooltip,
                                 GCallback cb, gpointer user)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    if (tooltip && *tooltip) gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", cb, user);
    return b;
}

static void install_panel_css_once(void)
{
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    /* Striping via bind-time even/odd row classes (recycled rows have no
     * stable sibling order, so :nth-child would mis-stripe on scroll).
     * alpha(currentColor) adapts to light/dark automatically; :selected
     * is excluded so the host's panel selection CSS stays in charge. */
    const char *css =
        ".finder-plugin row.finder-even:not(:selected) {"
        "  background-color: alpha(currentColor, 0.07);"
        "}";
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

/* One-shot 40 % divider placement once the panel has a real width — the
 * macOS dispatch_async(placeDivider) analogue; min sizes below are the
 * actual visibility guarantee. */
static void on_paned_map(GtkWidget *paned, gpointer user)
{
    FinderPanel *p = (FinderPanel *)user;
    if (p->paned_placed) return;
    int w = gtk_widget_get_width(paned);
    if (w < 1) return;
    p->paned_placed = TRUE;
    gtk_paned_set_position(GTK_PANED(paned), (int)(w * 0.4));
}

FinderPanel *finder_panel_new(const FinderPanelCallbacks *cb)
{
    FinderPanel *p = g_new0(FinderPanel, 1);
    if (cb) p->cb = *cb;

    install_panel_css_once();

    p->root_path = g_strdup(finder_prefs_last_root_path());
    p->current_list_path = g_strdup(p->root_path);
    p->entries = g_ptr_array_new_with_free_func(g_object_unref);

    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(p->root, "finder-plugin");
    gtk_widget_set_size_request(p->root, 260, 200);

    /* ── Toolbar ─────────────────────────────────────────────────────── */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_box_append(GTK_BOX(p->root), bar);

    p->combo = gtk_combo_box_text_new();
    p->combo_changed_id = g_signal_connect(p->combo, "changed",
        G_CALLBACK(on_root_combo_changed), p);
    gtk_box_append(GTK_BOX(bar), p->combo);

    p->btn_up = toolbar_button("go-up-symbolic",
        FDLoc("Übergeordneter Ordner", "Parent Folder"),
        G_CALLBACK(on_go_up), p);
    p->btn_home = toolbar_button("user-home-symbolic",
        FDLoc("Home-Verzeichnis", "Home Directory"),
        G_CALLBACK(on_go_home), p);
    p->btn_locate = toolbar_button("find-location-symbolic",
        FDLoc("Aktuelle Datei anzeigen", "Locate Current File"),
        G_CALLBACK(on_locate), p);
    p->btn_hidden = toolbar_button("view-conceal-symbolic", "",
        G_CALLBACK(on_toggle_hidden), p);
    update_hidden_button(p);
    p->btn_newfolder = toolbar_button("folder-new-symbolic",
        FDLoc("Neuer Ordner", "New Folder"),
        G_CALLBACK(on_new_folder), p);
    p->btn_newfile = toolbar_button("document-new-symbolic",
        FDLoc("Neue Datei", "New File"),
        G_CALLBACK(on_new_file), p);
    gtk_box_append(GTK_BOX(bar), p->btn_up);
    gtk_box_append(GTK_BOX(bar), p->btn_home);
    gtk_box_append(GTK_BOX(bar), p->btn_locate);
    gtk_box_append(GTK_BOX(bar), p->btn_hidden);
    gtk_box_append(GTK_BOX(bar), p->btn_newfolder);
    gtk_box_append(GTK_BOX(bar), p->btn_newfile);

    p->search = gtk_search_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(p->search), 8);
    g_object_set(p->search, "placeholder-text", FDLoc("Filter", "Filter"), NULL);
    gtk_widget_set_hexpand(p->search, TRUE);
    g_signal_connect(p->search, "search-changed",
                     G_CALLBACK(on_search_changed), p);
    gtk_box_append(GTK_BOX(bar), p->search);

    /* ── Split: folder tree | file list ─────────────────────────────── */
    p->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(p->paned, TRUE);
    gtk_box_append(GTK_BOX(p->root), p->paned);
    g_signal_connect(p->paned, "map", G_CALLBACK(on_paned_map), p);

    /* tree */
    p->tree_store = gtk_tree_store_new(TREE_N_COLS,
        G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    p->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->tree_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->tree_view), FALSE);
    {
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        gtk_tree_view_column_pack_start(col, pix, FALSE);
        gtk_tree_view_column_pack_start(col, txt, TRUE);
        gtk_tree_view_column_add_attribute(col, pix, "gicon", TREE_COL_ICON);
        gtk_tree_view_column_add_attribute(col, txt, "text", TREE_COL_NAME);
        gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree_view), col);
    }
    g_signal_connect(p->tree_view, "test-expand-row",
                     G_CALLBACK(on_tree_test_expand_row), p);
    g_signal_connect(p->tree_view, "row-activated",
                     G_CALLBACK(on_tree_row_activated), p);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree_view)),
                     "changed", G_CALLBACK(on_tree_selection_changed), p);
    {
        GtkGesture *g = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(g, "pressed", G_CALLBACK(on_tree_right_click), p);
        gtk_widget_add_controller(p->tree_view, GTK_EVENT_CONTROLLER(g));
    }
    GtkWidget *tree_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree_scroll),
                                  p->tree_view);
    /* Inset the tree from the panel's left edge: the HOST docks this
     * panel beside a GtkPaned whose divider has an enlarged invisible
     * grab zone that extends a few px into the panel — without the
     * margin, level-0 expander arrows sit inside that zone and single
     * clicks on them are swallowed by the divider (verified with the
     * NPP_FD_DEBUG trace: no test-expand-row for edge clicks). */
    gtk_widget_set_margin_start(tree_scroll, 8);
    gtk_widget_set_size_request(tree_scroll, 70, -1);
    gtk_paned_set_start_child(GTK_PANED(p->paned), tree_scroll);
    gtk_paned_set_shrink_start_child(GTK_PANED(p->paned), FALSE);

    /* list */
    p->list_store = g_list_store_new(FINDER_TYPE_ENTRY);
    p->list_sel = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(p->list_store)));
    gtk_single_selection_set_autoselect(p->list_sel, FALSE);
    p->list_view = gtk_column_view_new(GTK_SELECTION_MODEL(p->list_sel));
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(p->list_view), FALSE);

    {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(name_setup), p);
        g_signal_connect(f, "bind", G_CALLBACK(name_bind), p);
        GtkColumnViewColumn *col =
            gtk_column_view_column_new(FDLoc("Name", "Name"), f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_column_set_expand(col, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(p->list_view), col);
        g_object_unref(col);
    }
    {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(plain_setup), p);
        g_signal_connect(f, "bind", G_CALLBACK(size_bind), p);
        GtkColumnViewColumn *col =
            gtk_column_view_column_new(FDLoc("Größe", "Size"), f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_column_set_fixed_width(col, 76);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(p->list_view), col);
        g_object_unref(col);
    }
    {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(plain_setup), p);
        g_signal_connect(f, "bind", G_CALLBACK(date_bind), p);
        GtkColumnViewColumn *col =
            gtk_column_view_column_new(FDLoc("Geändert", "Modified"), f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_column_set_fixed_width(col, 128);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(p->list_view), col);
        g_object_unref(col);
    }
    g_signal_connect(p->list_view, "activate", G_CALLBACK(on_list_activate), p);
    {
        GtkGesture *g = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(g, "pressed", G_CALLBACK(on_list_bg_right_click), p);
        gtk_widget_add_controller(p->list_view, GTK_EVENT_CONTROLLER(g));
    }
    GtkWidget *list_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll),
                                  p->list_view);
    gtk_widget_set_size_request(list_scroll, 120, -1);
    gtk_paned_set_end_child(GTK_PANED(p->paned), list_scroll);
    gtk_paned_set_shrink_end_child(GTK_PANED(p->paned), FALSE);

    install_context_actions(p);
    rebuild_root_combo(p);
    reload_tree(p);
    reload_list_for_path(p, p->current_list_path);

    return p;
}

G_GNUC_END_IGNORE_DEPRECATIONS
