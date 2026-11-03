# ZMK module: zip_xy_pack / zip_xy_unpack input processors (see README.md and
# ../../flake.nix for how it is included).
zephyr_library()
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK src/input_processor_xy_pack.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_XY_UNPACK src/input_processor_xy_unpack.c)
