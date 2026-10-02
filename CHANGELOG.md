# Changelog

All notable changes to the Finder plugin (Linux) are documented here.
The version number tracks the macOS plugin it is ported from.

Version scheme: `XX.Y.ZZ` (Major.Minor.Patch)
- **ZZ** (Patch): Bugfixes / small changes
- **Y** (Minor): medium updates / new features
- **XX** (Major): large breaking changes

## [1.4.0] — 2026-10-02

First Linux release — a full port of Finder.macos 1.4.0 to GTK4:

- Folder tree (lazy-loading GtkTreeView) + file list (GtkColumnView with
  Name/Size/Modified columns and striped rows), toolbar with root
  dropdown (🏠 Home / 💻 Computer / ★ favorites / 💾 mounted volumes /
  Choose Folder…), parent/home/locate/hidden-toggle/new-folder/new-file
  buttons and a live filter field.
- Context menu: Open, Show in File Manager, Open in Terminal, New
  Folder/File, Rename…, Duplicate, Move to Trash, Copy Path/Name, Add to
  Favorites.
- Panel visibility restore via the host contract
  (NPPM_DMM_SETPANELINFO); first-launch auto-show; host toolbar icon.
- Preferences file is byte-compatible with the macOS plugin
  (`finder-plugin-prefs.json`: lastRootPath, favoritePaths,
  showHiddenFiles, didShowPanelOnFirstRun).
- German/English UI strings, language detected from the host at load.

Platform mapping vs macOS: "Reveal in Finder" → "Show in File Manager"
(freedesktop FileManager1 D-Bus, fallback opens the folder); "Open in
Terminal" → `$TERMINAL`/`x-terminal-emulator`/common-emulator chain;
Trash → GIO `g_file_trash`.
