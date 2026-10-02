/*
 * FinderLoc.h — tiny German/English string helper for the Finder plugin's
 * own UI text (menu items, tooltips, context menu, dialogs). Linux port of
 * FinderLocalization.{h,mm}.
 *
 * Linux delta (vs macOS): there is no usable language hook in the Linux
 * plugin ABI either — NPPM_GETNATIVELANGFILENAME is a stub returning ""
 * and no NPPN_NATIVELANGCHANGED fires — and the macOS fallbacks
 * (a shared NSUserDefaults domain + the host's "NPPLocalizationChanged"
 * NSNotification) have no GTK equivalent reachable from a plugin. So the
 * language is detected ONCE at load by reading the host's own config.xml
 * (<GUIConfig name="Localization" language="…"/>, two directories above
 * the NPPM_GETPLUGINSCONFIGDIR answer). A language switch therefore
 * applies to this plugin after the next launch — acceptable because the
 * host builds the Plugins menu once from FuncItem names anyway, so live
 * relabeling had nowhere to go on Linux in the first place.
 */
#pragma once

#include <glib.h>

/* Call once (FinderPlugin setInfo/ensurePanel) with the host's plugins
 * config dir; parses the host config.xml language. Safe to call again. */
void finder_loc_init_from_config_dir(const char *plugins_config_dir);

/* TRUE unless the host language is exactly "german" (case-insensitive) —
 * this plugin ships German + English only, everything else falls back to
 * English, matching the macOS behaviour. */
gboolean finder_loc_is_english(void);

/* Picks the German or English variant. Returned pointers are the literals
 * passed in — no allocation. */
const char *finder_loc_pick(const char *de, const char *en);

/* Shorthand, mirroring the macOS FDLoc(de, en) macro. */
#define FDLoc(deText, enText) finder_loc_pick((deText), (enText))
