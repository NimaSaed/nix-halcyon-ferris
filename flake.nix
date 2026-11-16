{
  description = "My splitkb Halcyon Ferris: ZMK firmware for the wireless and the wired one, built and flashed with Nix";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

    zmk-nix = {
      url = "github:lilyinstarlight/zmk-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, zmk-nix }: let
    forAllSystems = nixpkgs.lib.genAttrs (nixpkgs.lib.attrNames zmk-nix.packages);

    # UF2 flasher for the wired (RP2040) controllers, shared by flash-wired
    # and settings-reset-wired below. Same usage and flow as zmk-nix's
    # flasher (`nix run .#flash`), but our own script because that one does
    # not work here, for two reasons:
    #  - it looks for a SCSI disk whose model is "nRF UF2" and mounts that
    #    disk, since the nRF bootloader puts its FAT straight on the device.
    #    The RP2040 bootloader has a partition table instead, the FAT is on
    #    /dev/sdX1; find that by its filesystem label.
    #  - it mounts with udisksctl, which it does not ship and which is of no
    #    use without the udisks2 daemon anyway (services.udisks2 on NixOS),
    #    and hides the error: without udisks it waits for a mount forever.
    #    Use udisks when it is running, otherwise mount the partition
    #    ourselves with sudo.
    #
    # `main` is the script's body. It sets `firmware_name` and the `parts`
    # array, then has:
    #   select_parts "$@"              -> `flash` array: the parts given on the
    #                                    command line (none: all of `parts`)
    #   sudo_prime                     -> asks for the sudo password up front
    #                                    when there is no udisks
    #   flash_uf2 <part> <file> <what> -> waits for <part>'s bootloader, copies
    #                                    <file> to it, waits for the reboot
    mkWiredFlasher = system: { name, description, main }: let
      pkgs = nixpkgs.legacyPackages.${system};
    in pkgs.writeShellApplication {
      inherit name;
      runtimeInputs = [ pkgs.util-linux pkgs.coreutils ];
      text = ''
        available() {
          lsblk -lno path,label | grep -Fw 'RPI-RP2' | cut -d' ' -f1
        }

        mounted() {
          findmnt "$device" -no target
        }

        # The daemon, not just the udisksctl binary. udisks unmounts by itself
        # when the board reboots after the copy.
        use_udisks=""
        if udisksctl status >/dev/null 2>&1; then
          use_udisks=1
        fi

        # Our own mount when there is no udisks: a temporary directory, owned
        # by us so that cp works, with vfat's `flush` (as udisks mounts) so
        # that the file is on the device when cp returns. The bootloader
        # reboots as soon as it has the last block of the UF2.
        ours=""
        mount_ours() {
          ours="$(mktemp -d --tmpdir zmk-flash.XXXXXX)"
          if sudo mount -o "uid=$(id -u),gid=$(id -g),flush" "$device" "$ours"; then
            return 0
          fi
          rmdir "$ours"
          ours=""
          return 1
        }

        unmount_ours() {
          [ -n "$ours" ] || return 0
          sudo umount "$ours" 2>/dev/null || sudo umount -l "$ours" || true
          rmdir "$ours" 2>/dev/null || true
          ours=""
        }

        select_parts() {
          flash=("$@")
          if [ "''${#flash[@]}" -eq 0 ]; then
            flash=("''${parts[@]}")
          else
            for part in "''${flash[@]}"; do
              if ! printf '%s\0' "''${parts[@]}" | grep -Fxqz -- "$part"; then
                echo "The '$part' part does not exist in the firmware '$firmware_name'"
                exit 1
              fi
            done
          fi
        }

        # Without udisks we mount with sudo: ask for the password now, before
        # the keyboard is in its bootloader, not in the middle of flashing.
        sudo_prime() {
          if [ -z "$use_udisks" ]; then
            echo "udisks is not running; the device will be mounted with sudo."
            sudo -v
          fi
        }

        flash_uf2() {
          local part="$1" file="$2" what="$3"

          echo -n "Put the '$part' part of the keyboard into its bootloader (double tap reset, or hold BOOT while plugging it in via USB)"
          while ! device="$(available)"; do
            # Keep sudo's cached credentials from expiring while we wait
            # (non-interactive: never prompts here).
            [ -n "$use_udisks" ] || sudo -n -v 2>/dev/null || true
            echo -n .
            sleep 1
          done
          echo
          echo "Found device at $device"

          if [ -n "$use_udisks" ]; then
            echo "Mounting the device with udisks."
          else
            echo "Mounting the device with sudo."
            # Once, not in the loop below: no sudo prompt every second.
            mount_ours || echo "Could not mount $device; mount it manually anywhere you prefer."
          fi
          echo -n "Waiting for mount "

          while ! mountpoint="$(mounted)"; do
            if [ -n "$use_udisks" ]; then
              # Retried every second: udisks may not have probed the new
              # device yet.
              udisksctl mount --block-device "$device" >/dev/null 2>&1 && continue
            fi
            echo -n .
            sleep 1
          done
          echo
          echo "Found mountpoint at $mountpoint"

          cp "$file" "$mountpoint"
          sync -f "$mountpoint" 2>/dev/null || true
          echo "''${what^} copy complete."

          sleep 1
          unmount_ours

          if mounted >/dev/null && available >/dev/null; then
            echo -n "Please unmount the mass storage device at $device and disconnect the '$part' part of the keyboard via USB"
            while mounted >/dev/null && available >/dev/null; do
              echo -n .
              sleep 3
            done
          fi
          echo
        }

      '' + main;
      meta = {
        inherit description;
        mainProgram = name;
        license = nixpkgs.lib.licenses.mit;
        platforms = nixpkgs.lib.platforms.linux;
      };
    };
  in {
    packages = forAllSystems (system: let
      # Shared by both keyboards: config/ (keymap, west.yml, the per-board
      # .conf/.overlay files) and the in-repo modules.
      src = nixpkgs.lib.sourceFilesBySuffices self [ ".board" ".cmake" ".conf" ".defconfig" ".dts" ".dtsi" ".json" ".keymap" ".overlay" ".shield" ".yml" "_defconfig" ".c" ".h" ".yaml" "CMakeLists.txt" "Kconfig" ];

      # Hash of the west workspace (config/west.yml); `nix run .#update` after
      # changing the pins there.
      zephyrDepsHash = "sha256-AiMZ3KJQBdo/MT+xYT8B8FTYx/hEoMH2AQATrxGD3b0=";
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
      # `nix run .#settings-reset-wired [left|right]` (apps below) does the
      # whole cycle; this is the image, `nix build .#settings-reset-wired`.
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

      # Flasher for the wired controllers, `nix run .#flash-wired [left|right]`
      # (no argument: both, one after the other). See mkWiredFlasher at the top.
      flash-wired = mkWiredFlasher system {
        name = "zmk-uf2-flash-wired";
        description = "UF2 flasher for the wired Halcyon Ferris";
        main = ''
          firmware=${firmware-wired}
          firmware_name="${firmware-wired.name}"
          parts=(${toString firmware-wired.parts})

          select_parts "$@"
          sudo_prime

          for part in "''${flash[@]}"; do
            flash_uf2 "$part" "$firmware/zmk_$part.uf2" "firmware"
          done
        '';
      };

      update = zmk-nix.packages.${system}.update;
    });

    apps = forAllSystems (system: let
      inherit (self.packages.${system}) firmware-wired settings-reset-wired;
    in {
      # `nix run .#settings-reset-wired [left|right]` (no argument: both, one
      # after the other): wipes a wired half's saved settings and puts the
      # firmware back. Per half, the settings-reset-wired image is flashed,
      # the half boots it once (the erase runs at kernel init, so a couple of
      # seconds is plenty), then firmware-wired's image for that half is
      # flashed, as flash-wired does. Needed when a saved state should go:
      # an underglow that was toggled on and comes back on at every boot
      # despite CONFIG_ZMK_RGB_UNDERGLOW_ON_START=n (the saved state wins over
      # the *_START defaults, and reflashing does not touch the settings
      # partition), or ZMK Studio keymap edits.
      settings-reset-wired = {
        type = "app";
        program = nixpkgs.lib.getExe (mkWiredFlasher system {
          name = "zmk-uf2-settings-reset-wired";
          description = "Settings reset and reflash for the wired Halcyon Ferris";
          main = ''
            firmware=${firmware-wired}
            firmware_name="${firmware-wired.name}"
            parts=(${toString firmware-wired.parts})
            reset=${settings-reset-wired}

            select_parts "$@"
            sudo_prime

            for part in "''${flash[@]}"; do
              flash_uf2 "$part" "$reset/zmk.uf2" "settings reset image"
              echo "The '$part' part now boots the reset image, which wipes its saved settings"
              echo "(underglow state, ZMK Studio edits) right away. Give it a couple of seconds,"
              echo "then put it back into its bootloader for the firmware."
              echo
              flash_uf2 "$part" "$firmware/zmk_$part.uf2" "firmware"
            done
          '';
        });
        meta.description = "Settings reset and reflash for the wired Halcyon Ferris";
      };
    });

    devShells = forAllSystems (system: {
      default = zmk-nix.devShells.${system}.default;
    });
  };
}
