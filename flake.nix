{
  description = "ZMK firmware for my splitkb Halcyon Ferris (ported from my ZSA Voyager layout)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

    zmk-nix = {
      url = "github:lilyinstarlight/zmk-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, zmk-nix }: let
    forAllSystems = nixpkgs.lib.genAttrs (nixpkgs.lib.attrNames zmk-nix.packages);
  in {
    packages = forAllSystems (system: let
      # Shared by both keyboards: config/ (keymap, west.yml, the per-board
      # .conf/.overlay files) and the in-repo modules.
      src = nixpkgs.lib.sourceFilesBySuffices self [ ".board" ".cmake" ".conf" ".defconfig" ".dts" ".dtsi" ".json" ".keymap" ".overlay" ".shield" ".yml" "_defconfig" ".c" ".h" ".yaml" "CMakeLists.txt" "Kconfig" ];

      # Hash of the west workspace (config/west.yml); `nix run .#update` after
      # changing the pins there.
      zephyrDepsHash = "sha256-79OugfKZjMurv5Mq7s0dUypYI8z7Z4q8bWG6WNSBE84=";
    in rec {
      default = firmware;

      firmware = zmk-nix.legacyPackages.${system}.buildSplitKeyboard {
        name = "firmware";

        inherit src zephyrDepsHash;

        # In-repo ZMK modules (modules/<name>, each a Zephyr module with its own
        # zephyr/module.yml). xy-pack: the &zip_xy_pack / &zip_xy_unpack input
        # processors that send one BLE notification per touchpad packet (and
        # the landing filter); pinnacle-tune: Cirque registers Zephyr's driver
        # does not expose (edge sensitivity, compensation bits).
        extraCmakeFlags = [ "-DZMK_EXTRA_MODULES=${src}/modules/xy-pack;${src}/modules/pinnacle-tune" ];

        # Three parts: the dongle is the central (USB/BT host), both halves are
        # peripherals. The left half additionally needs CONFIG_ZMK_SPLIT_ROLE_CENTRAL=n
        # (see config/halcyon_ferris_left_halcyon_wireless.conf) since the shield
        # defaults it to central.
        parts = [ "dongle" "left" "right" ];
        centralPart = "dongle";

        # ZMK Studio over USB (only applied to the central part, i.e. the dongle).
        # Unlock with the &studio_unlock key on the Func layer.
        enableZmkStudio = true;

        board = {
          dongle = "halcyon_dongle//zmk";
          _      = "halcyon_wireless//zmk";
        };

        # Coin cell battery boards, e-paper display module on the left, Cirque
        # touchpad module on the right (its input is forwarded to the dongle,
        # hence mod_cirque_central on the dongle).
        # E-paper artwork: mod_display_epaper_{forest,mountain,cityscape}
        shield = {
          dongle = "halcyon_ferris_dongle mod_cirque_central";
          left   = "halcyon_ferris_left mod_battery_coincell mod_display_epaper_forest";
          right  = "halcyon_ferris_right mod_battery_coincell mod_cirque_hw_right";
        };

        meta = {
          description = "ZMK firmware for the Halcyon Ferris";
          license = nixpkgs.lib.licenses.mit;
          platforms = nixpkgs.lib.platforms.all;
        };
      };

      # The same keyboard with splitkb's wired (RP2040) controllers: no dongle,
      # the left half is the central (USB, ZMK Studio) and the right half the
      # peripheral over the UART on the USB-C cable between the halves;
      # e-paper on the left, Cirque on the
      # right, as on the wireless one. Same config/ directory: the keymap and
      # halcyon_ferris.conf are shared, and ZMK loads <shield>_<board>.{conf,
      # overlay} (halcyon_wired vs halcyon_wireless) for what differs.
      firmware-wired = zmk-nix.legacyPackages.${system}.buildSplitKeyboard {
        name = "firmware-wired";

        inherit src zephyrDepsHash;
        inherit (firmware) westDeps;   # same west.yml, reuse the fetched deps

        # pinnacle-tune as on the wireless right half; epaper-layers: the
        # left half's e-paper status screen (a column of the layers with a
        # dot at the active one; see the module's README). No xy-pack: it
        # exists to save BLE notifications and the wired split does not need it.
        extraCmakeFlags = [ "-DZMK_EXTRA_MODULES=${src}/modules/pinnacle-tune;${src}/modules/epaper-layers" ];

        parts = [ "left" "right" ];
        centralPart = "left";

        # ZMK Studio over USB on the central, i.e. the left half.
        enableZmkStudio = true;

        board = "halcyon_wired//zmk";

        # No battery boards on the wired controllers. mod_cirque_central on the
        # left is the receiving end of the right half's touchpad (as on the
        # dongle); see config/halcyon_ferris_left_halcyon_wired.overlay.
        # mod_display_epaper_forest brings the panel's driver and LVGL setup;
        # its own status widget is switched off in favour of epaper-layers
        # (config/halcyon_ferris_left_halcyon_wired.conf).
        shield = {
          left  = "halcyon_ferris_left mod_display_epaper_forest mod_cirque_central";
          right = "halcyon_ferris_right mod_cirque_hw_right";
        };

        meta = firmware.meta // {
          description = "ZMK firmware for the Halcyon Ferris, wired controllers";
        };
      };

      # ZMK's settings_reset shield for the wired controllers: flash it to a
      # half, let it boot once (it wipes the saved settings: RGB state, Studio
      # keymap edits, ...), then flash the real firmware again. Needed because
      # a saved RGB state always wins over the *_START defaults in
      # config/halcyon_wired.conf. One image, works for either half.
      settings-reset-wired = zmk-nix.legacyPackages.${system}.buildKeyboard {
        name = "settings-reset-wired";
        inherit src zephyrDepsHash;
        inherit (firmware) westDeps;
        board = "halcyon_wired//zmk";
        shield = "settings_reset";
        # config/halcyon_wired.conf (loaded for every build on this board)
        # enables the underglow, but the strip's devicetree is in the per-half
        # overlays, which this shield does not get.
        extraCmakeFlags = [ "-DCONFIG_ZMK_RGB_UNDERGLOW=n" ];
        meta = firmware.meta // {
          description = "Settings reset image for the wired Halcyon controllers";
        };
      };

      flash = zmk-nix.packages.${system}.flash.override { inherit firmware; };

      # zmk-nix's flasher looks for a SCSI disk whose model is "nRF UF2" and
      # mounts that disk: the nRF bootloader puts its FAT straight on the
      # device. The RP2040 bootloader (model "RP2") has a partition table
      # instead, the FAT is on /dev/sdX1, so mounting the disk fails forever.
      # Patch the detection to find the partition by its filesystem label.
      flash-wired = let
        pkgs = nixpkgs.legacyPackages.${system};
        nrf = zmk-nix.packages.${system}.flash.override { firmware = firmware-wired; };
      in pkgs.runCommand "zmk-uf2-flash" { meta.mainProgram = "zmk-uf2-flash"; } ''
        mkdir -p $out/bin
        sed "s#lsblk -Sno path,model | grep -F 'nRF UF2'#lsblk -lno path,label | grep -Fw 'RPI-RP2'#" \
          ${nrf}/bin/zmk-uf2-flash > $out/bin/zmk-uf2-flash
        grep -q "RPI-RP2" $out/bin/zmk-uf2-flash   # fail the build if zmk-nix changed the line
        chmod +x $out/bin/zmk-uf2-flash
      '';

      update = zmk-nix.packages.${system}.update;
    });

    devShells = forAllSystems (system: {
      default = zmk-nix.devShells.${system}.default;
    });
  };
}
