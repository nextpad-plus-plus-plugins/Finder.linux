// fp_test — behaviour battery for the REAL Finder.so against a FAKE host.
//
// The fake hostMsg implements the handful of messages the plugin uses:
// GETPLUGINSCONFIGDIR (a temp dir), DMM_REGISTERPANEL (captures the
// GtkWidget*, returns 1 — asserting the LINUX param order: wParam=title,
// lParam=widget), SHOW/HIDE, SETPANELINFO, GETFULLCURRENTPATH and DOOPEN
// (recorded). The captured panel goes into a real GtkWindow; the test
// then drives widgets directly against an on-disk fixture tree and
// asserts both the models and the FILESYSTEM.
//
// Needs a display — run under xvfb-run.

#include "plugin.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, what) do { \
    bool _ok = (cond); \
    printf("%-58s %s\n", what, _ok ? "OK" : "FAIL"); \
    if (!_ok) g_fail++; \
} while (0)

// ── fake host ────────────────────────────────────────────────────────────────
static char        g_config_dir[512];
static GtkWidget  *g_captured_panel;
static std::string g_captured_title;
static std::string g_panelinfo_module;
static int         g_panelinfo_cmdindex = -1;
static int         g_show_calls, g_hide_calls, g_unregister_calls;
static std::string g_current_file;        // GETFULLCURRENTPATH answer
static std::string g_doopen_path;         // last DOOPEN

static long fake_host_msg(unsigned int msg, unsigned long wParam, long lParam)
{
    switch (msg) {
    case NPPM_GETPLUGINSCONFIGDIR: {
        char *buf = (char *)(intptr_t)lParam;
        if (buf) g_strlcpy(buf, g_config_dir, (gsize)wParam);
        return 1;
    }
    case NPPM_DMM_REGISTERPANEL:
        g_captured_title = (const char *)(intptr_t)wParam;   // LINUX order
        g_captured_panel = (GtkWidget *)(intptr_t)lParam;
        return 1;
    case NPPM_DMM_SETPANELINFO: {
        const NppPanelInfo *info = (const NppPanelInfo *)(intptr_t)lParam;
        if (info) {
            g_panelinfo_module   = info->moduleName ? info->moduleName : "";
            g_panelinfo_cmdindex = info->cmdIndex;
        }
        return 1;
    }
    case NPPM_DMM_SHOWPANEL:   g_show_calls++; return 1;
    case NPPM_DMM_HIDEPANEL:   g_hide_calls++; return 1;
    case NPPM_DMM_UNREGISTERPANEL: g_unregister_calls++; return 1;
    case NPPM_GETFULLCURRENTPATH: {
        char *buf = (char *)(intptr_t)lParam;
        if (buf) g_strlcpy(buf, g_current_file.c_str(), 2048);
        return 1;
    }
    case NPPM_DOOPEN:
        g_doopen_path = (const char *)(intptr_t)lParam;
        return 1;
    default:
        return 0;
    }
}

// ── helpers ──────────────────────────────────────────────────────────────────
static void pump(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
}

static GtkWidget *find_by_type(GtkWidget *root, GType type)
{
    if (g_type_is_a(G_OBJECT_TYPE(root), type)) return root;
    for (GtkWidget *c = gtk_widget_get_first_child(root); c;
         c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *hit = find_by_type(c, type);
        if (hit) return hit;
    }
    return NULL;
}

static GtkWidget *find_button_with_icon(GtkWidget *root, const char *icon)
{
    if (GTK_IS_BUTTON(root)) {
        const char *n = gtk_button_get_icon_name(GTK_BUTTON(root));
        if (n && strcmp(n, icon) == 0) return root;
    }
    for (GtkWidget *c = gtk_widget_get_first_child(root); c;
         c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *hit = find_button_with_icon(c, icon);
        if (hit) return hit;
    }
    return NULL;
}

static guint list_count(GtkWidget *panel)
{
    GtkWidget *cv = find_by_type(panel, GTK_TYPE_COLUMN_VIEW);
    if (!cv) return 0;
    GtkSelectionModel *m = gtk_column_view_get_model(GTK_COLUMN_VIEW(cv));
    return g_list_model_get_n_items(G_LIST_MODEL(m));
}

// Entry names, joined with '|', read generically via the "name"-ish field:
// the model items are the plugin's FinderEntry GObjects; the test reads
// them through GObject introspection-free accessors it DOESN'T have, so it
// instead re-reads the rendered labels — simpler: fp_test only checks
// counts + selection + disk state, which is behaviour, not internals.

static GtkWidget *mkfile(const char *path)
{
    g_file_set_contents(path, "x", 1, NULL);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <so>\n", argv[0]); return 2; }
    gtk_init();

    // fixture tree
    char *tmp = g_dir_make_tmp("finder-test-XXXXXX", NULL);
    if (!tmp) { printf("tmpdir FAIL\n"); return 1; }
    gchar *fix = g_build_filename(tmp, "fix", NULL);
    g_mkdir_with_parents(fix, 0755);
    gchar *alpha  = g_build_filename(fix, "alpha", NULL);
    gchar *nested = g_build_filename(fix, "alpha", "nested", NULL);
    gchar *beta   = g_build_filename(fix, "beta", NULL);
    gchar *hidden = g_build_filename(fix, ".hidden", NULL);
    g_mkdir_with_parents(nested, 0755);
    g_mkdir_with_parents(beta, 0755);
    g_mkdir_with_parents(hidden, 0755);
    gchar *deep = g_build_filename(nested, "deep.txt", NULL);
    mkfile(deep);
    gchar *zeta = g_build_filename(fix, "zeta.txt", NULL);
    mkfile(zeta);
    gchar *bmd = g_build_filename(fix, "b.md", NULL);
    mkfile(bmd);

    // config dir with a pre-seeded prefs file rooting the panel at fix/
    gchar *cfg = g_build_filename(tmp, "Config", NULL);
    g_mkdir_with_parents(cfg, 0755);
    g_strlcpy(g_config_dir, cfg, sizeof(g_config_dir));
    gchar *prefs = g_build_filename(cfg, "finder-plugin-prefs.json", NULL);
    gchar *pjson = g_strdup_printf(
        "{\"lastRootPath\":\"%s\",\"favoritePaths\":[],"
        "\"didShowPanelOnFirstRun\":false,\"showHiddenFiles\":false}", fix);
    g_file_set_contents(prefs, pjson, -1, NULL);

    // load the real plugin
    void *h = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!h) { printf("dlopen FAIL: %s\n", dlerror()); return 1; }
    auto pSetInfo  = (void (*)(NppData))dlsym(h, "setInfo");
    auto pGetFuncs = (FuncItem * (*)(int *)) dlsym(h, "getFuncsArray");
    auto pNotify   = (void (*)(void *))dlsym(h, "beNotified");
    if (!pSetInfo || !pGetFuncs || !pNotify) { printf("dlsym FAIL\n"); return 1; }

    NppData nd = {};
    nd.hostMsg = fake_host_msg;
    pSetInfo(nd);
    int nf = 0;
    FuncItem *funcs = pGetFuncs(&nf);

    struct { struct { void *hwndFrom; uintptr_t idFrom; unsigned int code; } nmhdr; } scn = {};
    scn.nmhdr.code = NPPN_READY;
    pNotify(&scn);
    pump(100);   // first-run show runs from an idle

    CHECK(g_captured_panel != NULL, "REGISTERPANEL captured a widget");
    CHECK(g_captured_title == "Finder", "REGISTERPANEL wParam carried the title");
    CHECK(g_panelinfo_module == "Finder" && g_panelinfo_cmdindex == 0,
          "SETPANELINFO declared module+cmdIndex 0");
    CHECK(g_show_calls == 1, "first-run SHOWPANEL fired exactly once");

    // host the panel in a window so GTK lays it out
    GtkWidget *win = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 420);
    gtk_window_set_child(GTK_WINDOW(win), g_captured_panel);
    gtk_window_present(GTK_WINDOW(win));
    pump(250);

    CHECK(list_count(g_captured_panel) == 4,
          "list shows 4 visible entries (hidden filtered)");

    // live filter
    GtkWidget *search = find_by_type(g_captured_panel, GTK_TYPE_SEARCH_ENTRY);
    CHECK(search != NULL, "search entry present");
    gtk_editable_set_text(GTK_EDITABLE(search), "zeta");
    pump(450);   /* GtkSearchEntry delays search-changed (~150 ms default) */
    CHECK(list_count(g_captured_panel) == 1, "filter 'zeta' leaves 1 entry");
    gtk_editable_set_text(GTK_EDITABLE(search), "");
    pump(450);
    CHECK(list_count(g_captured_panel) == 4, "clearing the filter restores 4");

    // hidden toggle (eye): .hidden appears, then disappears again
    GtkWidget *eye = find_button_with_icon(g_captured_panel,
                                           "view-conceal-symbolic");
    CHECK(eye != NULL, "hidden-files toggle present (conceal icon)");
    g_signal_emit_by_name(eye, "clicked");
    pump(150);
    CHECK(list_count(g_captured_panel) == 5, "show-hidden reveals .hidden");
    GtkWidget *eye2 = find_button_with_icon(g_captured_panel,
                                            "view-reveal-symbolic");
    CHECK(eye2 != NULL, "toggle icon flipped to reveal (eye)");
    g_signal_emit_by_name(eye2, "clicked");
    pump(150);
    CHECK(list_count(g_captured_panel) == 4, "hide-hidden filters again");

    // expander-click path: expanding a placeholder-backed row must STICK.
    // gtk_tree_view_expand_row goes through "test-expand-row" exactly like
    // the user's > click; the NextZip lazy-tree rule (fill BEFORE removing
    // the placeholder) is what keeps the view from abandoning it.
    {
        GtkWidget *tv = find_by_type(g_captured_panel, GTK_TYPE_TREE_VIEW);
        GtkTreeModel *tm = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
        GtkTreeIter it;
        gboolean found_alpha = FALSE;
        if (gtk_tree_model_get_iter_first(tm, &it)) {
            do {
                gchar *p = NULL;
                gtk_tree_model_get(tm, &it, 2, &p, -1);
                if (p && g_str_has_suffix(p, "/alpha")) found_alpha = TRUE;
                g_free(p);
            } while (!found_alpha && gtk_tree_model_iter_next(tm, &it));
        }
        CHECK(found_alpha, "tree has the alpha row");
        if (found_alpha) {
            GtkTreePath *tp = gtk_tree_model_get_path(tm, &it);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(tv), tp, FALSE);
            pump(100);
            CHECK(gtk_tree_view_row_expanded(GTK_TREE_VIEW(tv), tp),
                  "expander expansion STICKS (lazy fill order)");
            GtkTreeIter child;
            gboolean real_child = FALSE;
            if (gtk_tree_model_iter_children(tm, &child, &it)) {
                gchar *cp = NULL;
                gtk_tree_model_get(tm, &child, 2, &cp, -1);
                real_child = cp && g_str_has_suffix(cp, "/nested");
                g_free(cp);
            }
            CHECK(real_child, "expanded row shows real children");
            // and collapsing works again
            gtk_tree_view_collapse_row(GTK_TREE_VIEW(tv), tp);
            CHECK(!gtk_tree_view_row_expanded(GTK_TREE_VIEW(tv), tp),
                  "collapse works after expand");
            gtk_tree_path_free(tp);
        }
    }

    // locate current file: deep inside alpha/nested
    g_current_file = deep;
    funcs[1].pFunc();   // Locate Current File in Finder Panel
    pump(200);
    CHECK(list_count(g_captured_panel) == 1, "locate navigated to nested/");
    {
        GtkWidget *cv = find_by_type(g_captured_panel, GTK_TYPE_COLUMN_VIEW);
        GtkSelectionModel *m = gtk_column_view_get_model(GTK_COLUMN_VIEW(cv));
        guint sel = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(m));
        CHECK(sel == 0, "locate selected the file row");
    }
    // the tree now exposes alpha expanded with nested under it
    {
        GtkWidget *tv = find_by_type(g_captured_panel, GTK_TYPE_TREE_VIEW);
        GtkTreeModel *tm = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
        GtkTreeIter it;
        gboolean found_nested = FALSE;
        if (gtk_tree_model_get_iter_first(tm, &it)) {
            do {
                GtkTreeIter child;
                if (gtk_tree_model_iter_children(tm, &child, &it)) {
                    do {
                        gchar *p = NULL;
                        gtk_tree_model_get(tm, &child, 2 /*path col*/, &p, -1);
                        if (p && strcmp(p, nested) == 0) found_nested = TRUE;
                        g_free(p);
                    } while (!found_nested &&
                             gtk_tree_model_iter_next(tm, &child));
                }
            } while (!found_nested && gtk_tree_model_iter_next(tm, &it));
        }
        CHECK(found_nested, "tree materialised alpha/nested on locate");
    }

    // activate the selected row -> DOOPEN with the file's path
    {
        GtkWidget *cv = find_by_type(g_captured_panel, GTK_TYPE_COLUMN_VIEW);
        g_signal_emit_by_name(cv, "activate", (guint)0);
        pump(50);
        CHECK(g_doopen_path == deep, "activating the file row sent NPPM_DOOPEN");
    }

    // new folder: on-disk creation + the rename prompt appears
    GtkWidget *newfolder = find_button_with_icon(g_captured_panel,
                                                 "folder-new-symbolic");
    CHECK(newfolder != NULL, "new-folder button present");
    g_signal_emit_by_name(newfolder, "clicked");
    pump(250);
    gchar *untitled = g_build_filename(nested, "Untitled Folder", NULL);
    CHECK(g_file_test(untitled, G_FILE_TEST_IS_DIR),
          "new folder exists on disk");

    // find the rename window + its entry; rename to "Renamed"
    {
        GtkWindow *rename_win = NULL;
        GListModel *tops = gtk_window_get_toplevels();
        guint n = g_list_model_get_n_items(tops);
        for (guint i = 0; i < n && !rename_win; i++) {
            GtkWindow *w = GTK_WINDOW(g_list_model_get_item(tops, i));
            if (w != GTK_WINDOW(win) && gtk_window_get_modal(w) &&
                gtk_widget_get_visible(GTK_WIDGET(w)))
                rename_win = w;   // keep the ref
            else
                g_object_unref(w);
        }
        CHECK(rename_win != NULL, "rename prompt appeared");
        if (rename_win) {
            GtkWidget *entry = find_by_type(GTK_WIDGET(rename_win),
                                            GTK_TYPE_ENTRY);
            CHECK(entry != NULL, "rename prompt has an entry");
            if (entry) {
                const char *pre = gtk_editable_get_text(GTK_EDITABLE(entry));
                CHECK(g_strcmp0(pre, "Untitled Folder") == 0,
                      "entry pre-filled with the new name");
                gtk_editable_set_text(GTK_EDITABLE(entry), "Renamed");
                g_signal_emit_by_name(entry, "activate");  // Enter = OK
                pump(250);
            }
            g_object_unref(rename_win);
        }
        gchar *renamed = g_build_filename(nested, "Renamed", NULL);
        CHECK(!g_file_test(untitled, G_FILE_TEST_EXISTS) &&
                  g_file_test(renamed, G_FILE_TEST_IS_DIR),
              "rename landed on disk");
        CHECK(list_count(g_captured_panel) == 2,
              "list refreshed after rename (Renamed + deep.txt)");
        g_free(renamed);
    }
    g_free(untitled);

    // shutdown contract
    scn.nmhdr.code = NPPN_BEFORESHUTDOWN;
    pNotify(&scn);
    CHECK(g_unregister_calls == 1, "BEFORESHUTDOWN unregistered the panel");

    printf("fp_test: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
