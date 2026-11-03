# pinnacle-tune — Cirque Pinnacle registers Zephyr's driver does not expose

Zephyr's `cirque,pinnacle` driver configures data mode, taps, sleep mode,
axis inversion and the ADC attenuation (`sensitivity`), and leaves the rest
of the chip at its defaults. This module runs once after the driver's init
and writes a few more, the same way Cirque's and QMK's drivers do:

- **edge sensitivity** — the "WideZ-min" extended registers (0x0149 X,
  0x0168 Y), set to Cirque's curved-overlay values 0x04 / 0x03 (chip
  defaults 0x06 / 0x05), so a lighter finger stays detected at the rim;
- **CalConfig1** — bits to clear among the chip's compensation mechanisms
  (background, NERD, track error, tap, palm error);
- **FeedConfig3** — feature bits to set (disable cross-rate smoothing,
  palm/NERD measurement, noise avoidance, wrap lockout, dynamic EMI adjust);
- a **forced calibration** afterwards.

The feed is paused around the writes; every register is read back and the
before/after values are logged at INFO under `pinnacle_tune`. All options
are Kconfig (`CONFIG_PINNACLE_TUNE_*`), so an experiment is a `.conf`
change. Register names and values from Cirque's `Cirque_Pinnacle_1CA027`
samples as carried in QMK's `cirque_pinnacle_regdefs.h`; the WideZ
registers are undocumented ("magic numbers from Cirque sample code").
