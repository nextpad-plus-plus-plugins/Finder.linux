/*
 * FinderPlugin.cpp — plugin entry point for the Nextpad++ Linux "Finder"
 * plugin (a folder-tree / file-list sidebar, the Linux port of
 * Finder.macos, itself modeled on the Windows npp-explorer plugin).
 *
 * Owns the 5 mandatory C exports (setInfo, getName, getFuncsArray,
 * beNotified, messageProc — plus isUnicode, which the LINUX loader
 * requires and silently skips the plugin without) and a small controller
 * that bridges the plugin ABI to FinderPanel.
 *
 * Linux ABI notes vs the macOS original:
 *  - NPPM_DMM_REGISTERPANEL parameters are SWAPPED on this host:
 *    wParam = panel title (const char *), lParam = GtkWidget *. The host
 *    deliberately rejects auto-detection (a GTK_IS_WIDGET sniff on the
 *    title string segfaults — see the host's GAP-88j comment).
 *  - FuncItem is the host's 80-byte shape with no shortcut field.
 *  - Localization is resolved once at load (FinderLoc.h explains why).
 *
 * ─────────────────────────────────────────────────────────────────────────
 * VERSION — single source of truth for this plugin's version number.
 * On every change, bump this AND the version line in README.md, and add
 * an entry to CHANGELOG.md. Kept in lockstep with the macOS plugin.
 * ───────────────────────────────────────────────────────────────────────── */
#define FINDER_PLUGIN_VERSION "1.4.0"

#include <gtk/gtk.h>
#include <string.h>
#include <stdint.h>

#include "plugin.h"            /* the Linux host ABI (structs + NPPM/NPPN) */
#include "FinderPanel.h"
#include "FinderPrefs.h"
#include "FinderFileOps.h"
#include "FinderLoc.h"

/* ─────────────────────────────────────────────────────────────────────────
 * SCNotification — minimal local mirror (same approach as the macOS
 * plugin): the host passes the real Scintilla SCNotification*, but this
 * plugin only ever reads nmhdr.code, so the leading-member mirror is all
 * that is declared. Do not add field access without vendoring the full
 * layout from Scintilla.h.
 * ───────────────────────────────────────────────────────────────────────── */
struct FinderSCNotification {
    struct {
        void      *hwndFrom;    /* host handle on Linux (may be NULL) */
        uintptr_t  idFrom;
        unsigned int code;
    } nmhdr;
};

static NppData g_npp;

#define FINDER_FUNC_COUNT 3
static FuncItem g_func_items[FINDER_FUNC_COUNT];

static const char *localized_func_item_name(int idx)
{
    switch (idx) {
    case 0: return FDLoc("Finder-Panel ein-/ausblenden",
                         "Toggle Finder Panel");
    case 1: return FDLoc("Aktuelle Datei im Finder-Panel anzeigen",
                         "Locate Current File in Finder Panel");
    case 2: return FDLoc("Aktuelle Datei im Dateimanager anzeigen",
                         "Reveal Current File in File Manager");
    default: return "";
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Controller
 * ───────────────────────────────────────────────────────────────────────── */

static FinderPanel *g_panel;
static long         g_panel_handle;
static gboolean     g_panel_visible;

/* Reads the active buffer's full path via NPPM_GETFULLCURRENTPATH.
 * Returns a newly-allocated string or NULL for an untitled buffer.
 * (The Linux host snprintf's up to 2048 bytes into the buffer.) */
static char *current_file_path(void)
{
    char buf[2048] = { 0 };
    g_npp.hostMsg(NPPM_GETFULLCURRENTPATH, 0, (long)(intptr_t)buf);
    if (!buf[0]) return NULL;
    return g_strdup(buf);
}

static void panel_open_file(const char *path, void *user)
{
    (void)user;
    g_npp.hostMsg(NPPM_DOOPEN, 0, (long)(intptr_t)path);
}

static void cmd_locate_current_file(void);

static void panel_locate_request(void *user)
{
    (void)user;
    cmd_locate_current_file();
}

static void ensure_panel_created(void)
{
    if (g_panel) return;

    char config_buf[1024] = { 0 };
    g_npp.hostMsg(NPPM_GETPLUGINSCONFIGDIR, sizeof(config_buf),
                  (long)(intptr_t)config_buf);
    if (config_buf[0]) {
        finder_loc_init_from_config_dir(config_buf);
        finder_prefs_configure(config_buf);
    }

    FinderPanelCallbacks cb = {
        panel_open_file,
        panel_locate_request,
        NULL,
    };
    g_panel = finder_panel_new(&cb);

    /* Linux order: wParam = title, lParam = widget (see header comment). */
    g_panel_handle = g_npp.hostMsg(NPPM_DMM_REGISTERPANEL,
        (unsigned long)(uintptr_t)"Finder",
        (long)(intptr_t)finder_panel_widget(g_panel));

    if (!g_panel_handle) {
        g_warning("[Finder] NPPM_DMM_REGISTERPANEL failed — host may "
                  "predate panel docking support. Panel unavailable.");
        return;
    }

    /* Restore metadata: the host re-opens a visible-at-quit panel by
     * invoking our menu command at cmdIndex 0 — togglePanel, a plain
     * toggle with no side effects, exactly what the restore contract
     * requires (NPPM_DMM_SETPANELINFO; older hosts return 0, ignore). */
    NppPanelInfo info;
    info.moduleName = "Finder";
    info.cmdIndex   = 0;
    g_npp.hostMsg(NPPM_DMM_SETPANELINFO, (unsigned long)g_panel_handle,
                  (long)(intptr_t)&info);
}

/* First-launch-after-install force-show, deferred one main-loop tick so
 * the host's split-view geometry has settled (the macOS plugin needed
 * the identical defer; a too-early show "succeeds" into a zero-width
 * panel). Every later launch defers to the host's own panel restore. */
static gboolean first_run_show_idle(gpointer user)
{
    (void)user;
    long result = g_npp.hostMsg(NPPM_DMM_SHOWPANEL,
                                (unsigned long)g_panel_handle, 0);
    g_panel_visible = (result != 0);
    finder_prefs_set_did_show_panel_on_first_run(TRUE);
    finder_prefs_save();
    return G_SOURCE_REMOVE;
}

static void handle_ready(void)
{
    ensure_panel_created();
    if (!g_panel_handle) return;
    if (finder_prefs_did_show_panel_on_first_run()) return;
    g_idle_add(first_run_show_idle, NULL);
}

static void handle_before_shutdown(void)
{
    if (g_panel_handle) {
        g_npp.hostMsg(NPPM_DMM_UNREGISTERPANEL,
                      (unsigned long)g_panel_handle, 0);
        g_panel_handle = 0;
    }
    finder_prefs_save();
}

/* ─────────────────────────────────────────────────────────────────────────
 * Menu commands
 * ───────────────────────────────────────────────────────────────────────── */

static void cmd_toggle_panel(void)
{
    ensure_panel_created();
    if (!g_panel_handle) return;

    if (g_panel_visible)
        g_npp.hostMsg(NPPM_DMM_HIDEPANEL, (unsigned long)g_panel_handle, 0);
    else
        g_npp.hostMsg(NPPM_DMM_SHOWPANEL, (unsigned long)g_panel_handle, 0);
    g_panel_visible = !g_panel_visible;
}

static void cmd_locate_current_file(void)
{
    ensure_panel_created();
    char *path = current_file_path();
    if (!path) return;

    if (!g_panel_visible && g_panel_handle) {
        g_npp.hostMsg(NPPM_DMM_SHOWPANEL, (unsigned long)g_panel_handle, 0);
        g_panel_visible = TRUE;
    }
    finder_panel_reveal_and_select(g_panel, path);
    g_free(path);
}

static void cmd_reveal_current_file(void)
{
    char *path = current_file_path();
    if (!path) return;
    finder_ops_show_in_file_manager(path);
    g_free(path);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Plugin exports
 * ───────────────────────────────────────────────────────────────────────── */

extern "C" {

G_MODULE_EXPORT void setInfo(NppData data)
{
    g_npp = data;

    /* Language must be known before the FuncItem names are copied, and
     * the panel (with its prefs) isn't created yet — resolve it from the
     * config dir here. */
    char config_buf[1024] = { 0 };
    g_npp.hostMsg(NPPM_GETPLUGINSCONFIGDIR, sizeof(config_buf),
                  (long)(intptr_t)config_buf);
    if (config_buf[0]) finder_loc_init_from_config_dir(config_buf);

    memset(g_func_items, 0, sizeof(g_func_items));
    g_strlcpy(g_func_items[0].itemName, localized_func_item_name(0),
              sizeof(g_func_items[0].itemName));
    g_func_items[0].pFunc = cmd_toggle_panel;
    g_strlcpy(g_func_items[1].itemName, localized_func_item_name(1),
              sizeof(g_func_items[1].itemName));
    g_func_items[1].pFunc = cmd_locate_current_file;
    g_strlcpy(g_func_items[2].itemName, localized_func_item_name(2),
              sizeof(g_func_items[2].itemName));
    g_func_items[2].pFunc = cmd_reveal_current_file;
}

G_MODULE_EXPORT const char *getName(void)
{
    return "Finder";
}

G_MODULE_EXPORT FuncItem *getFuncsArray(int *nbF)
{
    *nbF = FINDER_FUNC_COUNT;
    return g_func_items;
}

G_MODULE_EXPORT void beNotified(void *notification)
{
    const FinderSCNotification *n =
        (const FinderSCNotification *)notification;
    if (!n) return;
    switch (n->nmhdr.code) {
    case NPPN_READY:
        handle_ready();
        /* Toolbar icon for the toggle command. lParam = NULL icon hint →
         * the host falls back to its default lookup convention
         * (toolbar_dark.png / toolbar.png in the plugin dir). */
        g_npp.hostMsg(NPPM_ADDTOOLBARICON_FORDARKMODE,
                      (unsigned long)g_func_items[0].cmdID, 0);
        break;
    case NPPN_BEFORESHUTDOWN:
        handle_before_shutdown();
        break;
    default:
        break;
    }
}

G_MODULE_EXPORT long messageProc(unsigned int msg, unsigned long wParam,
                                 long lParam)
{
    (void)msg; (void)wParam; (void)lParam;
    return 0;
}

G_MODULE_EXPORT int isUnicode(void)
{
    return 1;
}

} /* extern "C" */
