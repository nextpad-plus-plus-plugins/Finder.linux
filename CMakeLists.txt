cmake_minimum_required(VERSION 3.20)
# Keep PROJECT_VERSION in sync with the macOS port's plugin version
# (FINDER_PLUGIN_VERSION in src/FinderPlugin.cpp).
project(Finder VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Host plugin interface (read-only; NO host changes). Unlike the Scintilla-
# touching ports, Finder is a pure host-message panel plugin: no scintilla
# include, no LinuxViewBridge, no --allow-shlib-undefined.
set(NPP_LINUX_DIR      "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")
set(NPP_PLUGIN_INCLUDE "${NPP_LINUX_DIR}/src")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK  REQUIRED IMPORTED_TARGET gtk4 glib-2.0 gio-2.0)
pkg_check_modules(JSON REQUIRED IMPORTED_TARGET json-glib-1.0)

add_library(Finder SHARED
  src/FinderPlugin.cpp
  src/FinderPanel.cpp
  src/FinderFileOps.cpp
  src/FinderPrefs.cpp
  src/FinderLoc.cpp)
target_include_directories(Finder PRIVATE
    ${NPP_PLUGIN_INCLUDE} "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(Finder PRIVATE PkgConfig::GTK PkgConfig::JSON)
# -Wno-deprecated-declarations: GtkTreeView/GtkComboBoxText are the
# deprecated-but-present GTK4 widgets the host and earlier ports also use.
target_compile_options(Finder PRIVATE
    -Wall -Wno-unused-parameter -Wno-deprecated-declarations
    -fvisibility=hidden -fPIC)
set_target_properties(Finder PROPERTIES PREFIX "" OUTPUT_NAME "Finder")

# ---- tests ------------------------------------------------------------------
# loader: dlopen smoke test (all five exports + the menu ABI), no host needed.
add_executable(loader test/loader.cpp)
target_include_directories(loader PRIVATE
    "${CMAKE_SOURCE_DIR}/src" ${NPP_PLUGIN_INCLUDE})
target_link_libraries(loader PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})

# fp_test: drives the REAL plugin .so with a fake host (captures the panel
# widget from NPPM_DMM_REGISTERPANEL) against an on-disk fixture tree.
# Needs a display — run ctest under xvfb-run.
add_executable(fp_test test/fp_test.cpp)
target_include_directories(fp_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src" ${NPP_PLUGIN_INCLUDE})
target_link_libraries(fp_test PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})
target_compile_options(fp_test PRIVATE -Wno-deprecated-declarations)

enable_testing()
add_test(NAME plugin_loader COMMAND loader $<TARGET_FILE:Finder>)
add_test(NAME fp_behaviour  COMMAND fp_test $<TARGET_FILE:Finder>)

# ---- install ----------------------------------------------------------------
set(PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/Finder"
    CACHE PATH "Where to install the plugin folder")
install(TARGETS Finder DESTINATION "${PLUGIN_INSTALL_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "${PLUGIN_INSTALL_DIR}")
install(FILES
    "${CMAKE_SOURCE_DIR}/resources/toolbar.png"
    "${CMAKE_SOURCE_DIR}/resources/toolbar_dark.png"
    DESTINATION "${PLUGIN_INSTALL_DIR}")

# ---- distributable zip ------------------------------------------------------
set(FD_STAGE "${CMAKE_BINARY_DIR}/pkg/Finder")
file(MAKE_DIRECTORY "${FD_STAGE}")
add_custom_target(package_zip
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Finder> "${FD_STAGE}/Finder.so"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE" "${FD_STAGE}/"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_SOURCE_DIR}/resources/toolbar.png"
          "${CMAKE_SOURCE_DIR}/resources/toolbar_dark.png" "${FD_STAGE}/"
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/pkg"
          ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/Finder.linux.zip"
          --format=zip Finder
  DEPENDS Finder
  COMMENT "Packaging Finder.linux.zip")
