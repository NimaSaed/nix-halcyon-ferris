# xy-pack — one BLE notification per touchpad packet

ZMK's input split sends every Zephyr input event from a peripheral as its own
GATT notification. Zephyr's `cirque,pinnacle` driver reports two per packet
in relative mode with taps off (`REL_X`, then `REL_Y` with the sync flag), at
up to 100 packets/s: 200 notifications/s, which is more than a split link can
carry when the touchpad half only gets one or two exchanges per connection
event.

- `zip_xy_pack` (peripheral, in the `zmk,input-split` node's
  `input-processors`): holds the `REL_X`, rewrites the packet's `REL_Y` into
  one `INPUT_REL_MISC` event carrying both deltas (X low 16 bits, Y high 16
  bits), drops packets without movement.
- `zip_xy_unpack` (central, **first** in every chain of the listener):
  restores `REL_X`/`REL_Y`. A processor can only return one event, so the
  `REL_Y` is re-injected with `input_report()`; the sums reaching the HID
  report are unchanged.

Both parts must be flashed together: a packed event on a central without the
unpacker is ignored (no cursor movement), the unpacker on a central whose
peripheral does not pack does nothing.

## Landing filter (`zip_xy_pack`)

Traced on a Halcyon Cirque pad in relative mode: in the first ~100 ms after
the finger lands, the chip reports a hump of motion that the finger did not
make, e.g. `(0,-4) (0,-10) (1,-11) (2,-11) (0,-7) (0,-5) (-1,-3) (-1,-2)`
(dx,dy per 10 ms packet) — 45-55 counts on a normal touch, 104 on a firm
one, always upward (negative Y), sometimes with a small wrong-way X. With a
2x scaler on the central that is a 90-200 px jump on every re-centring
touch, i.e. the cursor "drags the opposite way" exactly when the finger is
put back down. Once settled, the deltas are clean (no reversals, no jumps).

`landing-packets` / `landing-gap-ms` / `landing-max-up` / `-down` / `-x`
limit the per-packet deltas of the first packets of a new stroke (a packet
after a gap). Only the direction the hump takes needs limiting; the defaults
(8 packets, 150 ms, up limited to 1 count, the rest unlimited) match the pad
above. A real stroke that starts upward would be trimmed the same way, so at
the end of the window the shape decides: a hump peaks at packets 2-4 and
has decayed by packets 7-8, a stroke is still rising or sustained (raw
motion of packets 7-8 at least that of packets 3-4 and at least
`landing-refund-threshold`, 6), and for a stroke the withheld counts are
paid back over the next `landing-refund-packets` (4). A quick upward flick
that rises and falls within 80 ms is indistinguishable from a hump and
loses its first part; on 231 traced landings this rule classified 133/163
strokes and 52/70 humps correctly, the best of the rules tried.
The per-second stats line reports `landing N pkts M counts R refunded`, and
the TRACE option prints `-> dx,dy` on altered packets.
