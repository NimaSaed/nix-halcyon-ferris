# ZMK module: status screen for the Halcyon e-paper on a central, the
# keymap's layers as a list with a dot at the active one (see README.md and
# ../../flake.nix). Replaces splitkb's widget (HALCYON_EPAPER_WIDGET_STATUS).
if(CONFIG_ZMK_EPAPER_LAYERS)
  zephyr_library()
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(src/status_screen.c)
endif()
