# ZMK module: one-shot register tuning of the Cirque Pinnacle after Zephyr's
# driver has initialised it (see README.md and ../../flake.nix).
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_PINNACLE_TUNE src/pinnacle_tune.c)
