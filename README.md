# Finder — Nextpad++ Linux Plugin

**Version:** 1.4.0 — see [CHANGELOG.md](CHANGELOG.md) for the version history.

A sidebar panel for Nextpad++ (Linux) that shows a folder tree + file list
of the real filesystem and offers file-manager-style actions (show in file
manager, open in terminal, rename, move to trash, copy path, new
file/folder, favorites). It is the Linux port of the
[Finder.macos](https://github.com/nextpad-plus-plus-plugins/Finder.macos)
plugin (itself the macOS counterpart to the classic Windows
npp-explorer-plugin), built directly against the Nextpad++ Linux plugin
ABI (`plugin.h` from the host repo).

## Dependencies

GTK 4, GLib/GIO and json-glib — all part of a stock desktop install; the
same libraries the Nextpad++ Linux host already uses. No Scintilla access,
no bridge: Finder is a pure host-message panel plugin.

## Structure

```
Finder.linux/
├── CMakeLists.txt            Build configuration (produces Finder.so)
├── resources/                toolbar.png / toolbar_dark.png (host toolbar icon)
├── test/
│   ├── loader.cpp            dlopen smoke test (all five exports + menu ABI)
│   └── fp_test.cpp           behaviour battery: real .so + fake host +
│                             on-disk fixture tree (run under xvfb-run)
└── src/
    ├── FinderPlugin.cpp      Mandatory exports, panel registration
    │                         (NB: Linux REGISTERPANEL order is
    │                         wParam=title, lParam=GtkWidget*)
    ├── FinderPanel.{h,cpp}   GtkTreeView folder tree + GtkColumnView file
    │                         list (Name/Size/Modified, striped), toolbar,
    │                         context menu
    ├── FinderFileOps.{h,cpp} Show in file manager, terminal, new
    │                         file/folder, rename, trash, duplicate, copy
    ├── FinderPrefs.{h,cpp}   JSON prefs (same file/keys as the macOS port)
    └── FinderLoc.{h,cpp}     German/English strings (language read once
                              at load from the host config.xml)
```

## Building

Prerequisite: the Nextpad++ Linux host repo checked out as a sibling
(`../../nextpad-plus-plus-gtk4` — only its `src/plugin.h` is read).

```sh
sudo apt install build-essential cmake pkg-config libgtk-4-dev libjson-glib-dev
cmake -S . -B build
cmake --build build -j4
```

Result: `build/Finder.so`.

Tests:

```sh
cd build && ctest                      # loader is display-free
xvfb-run -a ctest                      # fp_behaviour needs a display
```

## Installing

Nextpad++ (Linux) loads plugins from
`~/.local/share/nextpad++/plugins/<Name>/<Name>.so`:

```sh
cmake --install build
```

or unpack a release zip from `dist/` into that folder. On the first launch
after installing, the panel opens once by itself; afterwards its
visibility follows the host's panel-restore setting.

## Menu commands

- **Toggle Finder Panel** (also on the toolbar)
- **Locate Current File in Finder Panel**
- **Reveal Current File in File Manager**

## Linux notes

- "Show in File Manager" uses the freedesktop `org.freedesktop.FileManager1`
  D-Bus interface (GNOME Files, Dolphin, …) with an open-the-folder
  fallback. "Open in Terminal" tries `$TERMINAL`, `x-terminal-emulator`
  and the common emulators in turn — there is no universal Linux API.
  Under a strictly confined (Snap) host both actions may be blocked by
  confinement; the deb/rpm builds are unaffected.
- UI language (German/English, like the macOS plugin) is read from the
  host's configured language once at load; a language switch applies to
  this plugin after the next launch.
- Trash uses GIO's `g_file_trash` (freedesktop Trash spec — recoverable).

## License

MIT — see [LICENSE](LICENSE).
