# zmk-nix — Halcyon Ferris firmware (`vanilla` branch)

This branch builds what splitkb releases for the wireless Halcyon Ferris
(dongle, coin-cell halves, e-paper on the left, Cirque touchpad on the right)
with the keymap replaced by mine and the touchpad kept as plain as possible:
no taps, no gesture handling, no logging; the pad only moves the cursor and
the changes below are about getting its stream over the radio reliably. It
exists to compare against `main`, which carries the tap/drag work.

**Touchpad stream over the radio.** The Cirque chip reports up to 100
packets/s while the finger moves. Zephyr's driver turns each one into
Zephyr input events (`REL_X`, then `REL_Y` carrying the sync flag; a third,
the button, when taps are on) and ZMK's input split sends *every event as
its own BLE notification*. The dongle runs one BLE connection per half, both
at a 7.5 ms interval, and Zephyr's controller schedules the second one about
1 ms after the first (the time of a single packet exchange); a connection
event that still has data is cut off when the other half's starts. So the
half that happened to connect first gets ~1 ms of every 7.5 ms, one or two
notifications, and when that is the touchpad half the link cannot carry
200 notifications/s: the right half's send queue fills (cursor lags the
finger), then drops. Hence the three measures below: fewer events per
packet (taps off), one notification per packet (`&zip_xy_pack`), and an
even split of the interval between the halves (`CONFIG_BT_CTLR_CENTRAL_SPACING`).

| Part   | Board                   | Shields (as in splitkb's `build.yaml`)                                 |
| ------ | ----------------------- | ----------------------------------------------------------------------- |
| dongle | `halcyon_dongle//zmk`   | `halcyon_ferris_dongle mod_cirque_central` (+ ZMK Studio over USB)      |
| left   | `halcyon_wireless//zmk` | `halcyon_ferris_left mod_battery_coincell mod_display_epaper_forest`    |
| right  | `halcyon_wireless//zmk` | `halcyon_ferris_right mod_battery_coincell mod_cirque_hw_right`         |

Differences from the stock images:

- the keymap (`config/halcyon_ferris.keymap`, see the top of the file for the
  layout and the tap/hold settings);
- `CONFIG_ZMK_POINTING` / `CONFIG_ZMK_POINTING_SMOOTH_SCROLLING` enabled on
  all parts (`config/halcyon_ferris.conf`) for the keymap's mouse keys; the
  Cirque shields already imply both on the dongle and the right half;
- `config/halcyon_ferris_left_halcyon_wireless.conf` makes the left half a
  peripheral, which splitkb does with a cmake argument;
- the dependency pins in `config/west.yml` (splitkb builds from the moving
  `main+halcyon-fixes` branch).
- `config/halcyon_ferris_right_halcyon_wireless.overlay`: the touchpad's taps are turned off
  (the chip's tap-to-click comes with a tap-and-drag gesture that misfires
  on short strokes); click and drag with the MB1/MB2/MB3 keys on the Mouse
  layer; this also makes a packet two input events instead of three;
- `modules/xy-pack/` (in-repo ZMK module, see its README): `&zip_xy_pack`
  runs on the right half (`config/halcyon_ferris_right_halcyon_wireless.overlay`, in the
  `zmk,input-split` node) and folds a packet's `REL_X`/`REL_Y` into one
  event, so the radio carries one notification per packet (100/s at most)
  instead of two, and packets without movement are not sent at all;
  `&zip_xy_unpack` is the first processor of every chain on the dongle and
  restores `REL_X`/`REL_Y`. Flash the dongle and the right half together
  (a packed event on a dongle without the unpacker is ignored);
- `modules/pinnacle-tune/` (in-repo ZMK module, see its README): one-shot
  writes to Cirque registers the Zephyr driver does not expose, after its
  init: edge sensitivity and the chip's compensation / feature bits, all as
  `CONFIG_PINNACLE_TUNE_*` in `config/halcyon_ferris_right_halcyon_wireless.conf`;
- `config/halcyon_ferris_dongle.conf`: `CONFIG_BT_CTLR_CENTRAL_SPACING=3000`
  places the two halves' connection events 3 ms apart instead of ~1 ms, so
  each half gets at least 3 ms per 7.5 ms interval whichever connected
  first (`CONFIG_BT_CTLR_ADVANCED_FEATURES=y` is only there to make the
  option settable);
- wheel scrolling on the `&msc` keys (Nav layer U/I, Mouse layer E/R and the
  right hand) at 400 units/s (top of the keymap).
- `config/halcyon_ferris_dongle.overlay`: cursor movement doubled (ZMK's
  built-in `&zip_xy_scaler 2 1`), and the Mouse layer switches on by itself
  while the pad is moved and off 600 ms after the last movement (ZMK's
  built-in `&zip_temp_layer`), so D/F/G click, E/R scroll, S holds pad
  scroll and V/B are browser forward/back while the finger is on the pad; it
  does not engage within 300 ms of a keystroke, and any other key ends it;
- holding S on the Mouse layer (a momentary Scroll layer, 5) makes pad
  motion scroll instead of moving the cursor;
- `config/halcyon_ferris_dongle.conf` and
  `config/halcyon_ferris_right_halcyon_wireless.conf`: larger BLE receive/transmit buffers and event queues, so a burst after a
  stalled connection event is absorbed rather than dropped (the stock
  dongle acknowledged one packet per connection event and queued it in a
  5-entry queue that drops silently). The right half's send queue is back
  at ZMK's default of 32: a queue entry is now a whole packet, so a drop
  cannot tear a packet's X from its Y any more, and 32 packets (320 ms) is
  as far as the cursor should ever be allowed to lag the finger.

```sh
nix build              # -> result/zmk_{dongle,left,right}.uf2
nix run .#flash        # double-tap reset on each part when prompted
nix run .#flash dongle # flash only one part (dongle | left | right)
git checkout main      # back to the tuned firmware
```

## The wired copy (`firmware-wired`)

The same keyboard built for splitkb's wired Halcyon controllers (RP2040,
board `halcyon_wired//zmk`): e-paper on the left, Cirque on the right, the
halves on a TRRS cable, no dongle and no batteries. Same `config/`
directory, same keymap: ZMK looks for `<shield>_<board>.{conf,overlay}`
next to `<shield>.*`, all matching `.conf` files are merged and the most
specific `.overlay` wins, so everything board-specific sits in
`*_halcyon_wireless.*` / `*_halcyon_wired.*` / `halcyon_wired.conf` and
only `halcyon_ferris.keymap` and `halcyon_ferris.conf` are shared. (That is
also why the wireless files carry the `_halcyon_wireless` suffix: a
shield-named `halcyon_ferris_right.conf` would be merged into the wired
build too, and its `CONFIG_BT_*` lines would abort it, Zephyr treats
assignments to unavailable symbols as errors.)

| Part  | Board                | Shields                                                                | Role                              |
| ----- | -------------------- | ---------------------------------------------------------------------- | --------------------------------- |
| left  | `halcyon_wired//zmk` | `halcyon_ferris_left mod_display_epaper_forest mod_cirque_central`     | central, USB, ZMK Studio          |
| right | `halcyon_wired//zmk` | `halcyon_ferris_right mod_cirque_hw_right`                             | peripheral over the TRRS UART     |

```sh
nix build .#firmware-wired -o result-wired   # -> result-wired/zmk_{left,right}.uf2
nix run .#flash-wired                        # RP2040 bootloader: double-tap reset (or hold BOOT while plugging in)
nix run .#flash-wired right
```

Never plug or unplug the TRRS cable while either half is on USB (ZMK's own
warning: it can kill the controllers).

What differs from the wireless build, and why:

- `config/halcyon_wired.dtsi` (included by both wired overlays) and the RGB
  block in `config/halcyon_wired.conf` are splitkb's own
  `boards/shields/halcyon_ferris/boards/halcyon_wired.{overlay,conf}`,
  copied: Zephyr never applies those files for the `halcyon_wired//zmk`
  target (it looks for `halcyon_wired_rp2040_zmk.*` or `halcyon_wired_zmk.*`;
  splitkb's build matrix has no wired entries yet, `# TODO: Handle wired
  controller` in their generator), so without the copy there is no
  `zmk,wired-split` node, the wired transport is not even compiled, and the
  halves build fine but never talk. Drop the copies once splitkb renames
  their files. Deliberate changes in the copy: the underglow starts
  **off** (`CONFIG_ZMK_RGB_UNDERGLOW_ON_START=n`, splitkb has it on), like
  the wireless halves, and is a solid light blue when turned on (hue 200,
  saturation 60; ZMK's default is red); note that a toggle or colour change
  is only saved to flash 60 s after the last change, and these halves lose
  power when unplugged;
- `config/halcyon_ferris_left_halcyon_wired.overlay` is the dongle overlay
  minus `&zip_xy_unpack` (the touchpad's processors: scaler, auto Mouse
  layer, Scroll layer); `config/halcyon_ferris_right_halcyon_wired.overlay`
  is the wireless right overlay minus `&zip_xy_pack` and the USB console:
  the UART carries every event as is, so there is nothing to pack, and
  `uart0` *is* the split link, so no logging is enabled on the wired build;
- `config/halcyon_ferris_right_halcyon_wired.conf` repeats the
  `CONFIG_PINNACLE_TUNE_*` block (same pad, same values); none of the BLE
  buffer tuning applies;
- `config/halcyon_ferris_left_halcyon_wired.conf`: the e-paper is updated
  within 100 ms of a change (`CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS`, splitkb's
  shield uses 1000 ms, so a layer change could lag by up to a second); the
  panel's own partial refresh, a few hundred ms from the LUT in splitkb's
  devicetree, is the floor. The wireless left half is a peripheral and shows
  no layer, so it keeps splitkb's value;
- `config/halcyon_ferris_left_halcyon_wired.conf`: ZMK's built-in status
  screen on the e-paper (layer name, words per minute) for now, because
  splitkb's widget (artwork, battery gauge, BLE profile circles) is written
  for the wireless board and does not compile without Bluetooth and a
  battery (`widgets/status.c` calls `zmk_ble_*` and
  `zmk_battery_state_of_charge` and uses `CONFIG_BT_MAX_PAIRED` unguarded).
  Known flaw: the panel is mounted a quarter turn off and the built-in
  screen does not know, so its text runs sideways;
- in-repo modules: only `pinnacle-tune` is passed to the wired build.

**Measured (four captures, ~25 000 packets):** every packet the right half
produced reached the dongle, no queue ever filled, no drop, no stack came
near its limit, and the gaps the dongle saw were the gaps the chip produced,
within 7 ms. Holes in the stream are not evidence of anything: in relative
mode the chip is silent while the finger rests.

**The actual "acts funny on re-centring" cause, found with a per-packet
trace:** in the first ~100 ms after the finger lands the chip reports a hump
of upward motion (45-55 counts, up to 104 on a firm landing; x2 scaler =
90-200 px) before the stroke starts, with a small wrong-way X kick on about
a third of landings. `&zip_xy_pack`'s landing filter
(`config/halcyon_ferris_right_halcyon_wireless.overlay`, documented in
`modules/xy-pack/README.md`) limits the upward component of the first 8
packets of every stroke to 1 count, then pays the withheld motion back when
the shape says it was a real upward stroke rather than a hump (still rising
or sustained at the end of the window); the per-second log line shows both
(`landing N pkts M counts R refunded`).

**"Messed up after touching the rim, better when pressing harder":** the
chip's own NERD / track-error / palm-error compensation reacting to a
contact half off the sensor by shifting its baseline, which then makes a
normal finger marginal for a few seconds. `modules/pinnacle-tune/` writes
the registers Zephyr's driver does not expose, once after the driver's init
(`config/halcyon_ferris_right_halcyon_wireless.conf`, `CONFIG_PINNACLE_TUNE_*`): those three
compensations off (CalConfig1 0x3e -> 0x12, background and tap compensation
kept), and the edge-sensitivity registers at Cirque's curved-overlay values
(0x04 / 0x03). Measured: the after-rim effect no longer reproduces and
dropouts of 40-99 ms inside a stroke fell from 19.8 to 4.2 per 1 000
packets. Caveat: if the pad boots with a finger on it, only the slow
background compensation is left to fix the baseline (replug). The driver's
`sensitivity` is at `"1x"` (shield default `"2x"`), the chip's maximum ADC
gain; it made no measurable difference to the holes and is kept for feel.

**Open:** the above was measured with the right half on USB power; the
battery (floating ground) run is the next check.

## Getting logs (diagnosing a choke)

The `CONFIG_ZMK_USB_LOGGING` blocks at the end of
`config/halcyon_ferris_dongle.conf` and
`config/halcyon_ferris_{right,left}_halcyon_wireless.conf` are currently **on** (the
investigation is still open); comment them out when done (8 KB of RAM for
the log buffer and an extra USB serial port per part). ZMK itself logs at
WRN; Zephyr's thread analyzer prints every thread's stack high-water mark
every 30 s on each part; `pinnacle_tune` logs the chip registers it wrote at
boot; and the xy-pack module logs one summary per second of touchpad
traffic (`CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_TRACE=y` adds one line per
packet, for short controlled gestures only):

- right half: `xy_pack: N pkts in M ms, E empty, S stalls, max gap G ms,
  F flips, O one-axis, J big, max burst B, stack free K` — what the chip
  delivered (`flips`: sign reversals of dx or dy between consecutive packets
  of a stroke, `one-axis`: packets with one axis exactly zero, `big`: |dx| or
  |dy| ≥ 40 — the numbers behind "wrong direction", "one axis", "jumps");
- dongle: `xy_unpack: N pkts in M ms, F inject-fail, S stalls, max gap G ms,
  F flips, O one-axis, J big, max burst B, stack free K` — what arrived
  over the link, same columns (`inject-fail`
  > 0 means the dongle's own input queue was full, i.e. its USB output is
  slow; `max burst` is the longest run of packets under 3 ms apart: 1-2 is
  normal, a long one right after a gap means the link stalled and the right
  half's queue then drained in one go; `stack free` is the input thread's
  unused stack in bytes);
- plus ZMK's `Input event queue full, dropping one and retry` / `Error
  notifying input event` on the right half when the link falls behind.

1. Plug the right half into USB as well (it keeps talking to the dongle over
   BLE; being USB-powered also takes the coin cell out of the picture, so
   note whether the choke still happens at all).
2. `scripts/zmk-log.sh` captures every ZMK serial port into
   `~/zmk-logs/<serial>-<interface>.log` with host timestamps (the dongle has
   two ports, the one that stays empty is ZMK Studio; you may need to be in
   the `dialout` group).
3. Move the cursor continuously until it chokes, note the clock time, keep
   going ~20 s, Ctrl-C.

Reading: the same gap on both sides = the chip stopped or the finger was
lifted (the chip sends nothing on lift-off in relative mode, so the two
cannot be told apart from the logs); `pkts` much lower on the dongle than
on the right half, `queue full` lines, or a gap followed by a long burst on
the dongle = the link; `inject-fail` or a gap only on the dongle = the
dongle's output. A healthy capture has matching `pkts` on both sides,
`0 inject-fail`, `max burst` 1-2, and 100-105 pkts/s while sliding. The
dongle log alone is enough for the link-vs-chip question (the burst column),
so the halves can stay on battery unless their own threads are in doubt.

Not changed, and why: the chip's own 100 Hz sample rate (Zephyr's driver has
no property for it, lowering it to 50-60 Hz would need a register write
behind the driver's back), its sleep mode (saves the coin cell, costs only
the first touch after 5 s idle), and the halves' +8 dBm radio power (set by
splitkb's board; 0 dBm would cut the right half's current draw during
touchpad use by about two thirds and the dongle is on the desk, but range
was not the complaint).
