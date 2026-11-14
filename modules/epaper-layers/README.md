# epaper-layers — layer list for the Halcyon e-paper

ZMK module (`zephyr/module.yml`) providing `zmk_display_status_screen()` for
the e-paper of splitkb's `mod_display_epaper` shield, on a part that knows
the keymap (a central): every layer of the keymap in a column, in index
order, with a dot in front of the active one (the highest active layer, as
ZMK's own layer widget reports it). Nothing else, so between two updates
only the dot moves.

Used by the wired build's left half (`config/halcyon_ferris_left_halcyon_wired.conf`):

```
CONFIG_HALCYON_EPAPER_WIDGET_STATUS=n   # splitkb's widget also defines the screen
CONFIG_ZMK_EPAPER_LAYERS=y
```

The shield's own driver and LVGL setup (1 bpp, full-size draw buffer,
partial-refresh waveform) are kept; only the status screen is replaced.

## How it draws

The panel is 184 x 100 for the driver and mounted turned a quarter turn,
so the user looks at 88 x 184 (88, not 100: splitkb's widgets use that much
of the height, the rest is under the bezel). `src/status_screen.c` draws in
the user's orientation on a hidden LVGL canvas and turns it into the
visible, panel-oriented canvas with `lv_draw_sw_rotate` before each update,
the same way splitkb's widgets do (source `(x, y)` lands at panel
`(y, W - 1 - x)`). Both canvases are L8, the smallest format the rotation
supports; two 88 x 184 buffers, ~32 KB of RAM.

Every update redraws the whole column: the e-paper's partial refresh only
changes the pixels that differ and takes the same time regardless, so this
costs nothing over moving the dot alone.

Layer names come from the keymap at run time (`zmk_keymap_layer_name`), so
ZMK Studio renames show up at the next layer change; unnamed layers show as
`Layer N`. Rows are 28 px apart for up to six layers and tighter beyond; if
more layers exist than fit at the font's line height, the rest are left out
with a warning in the log.

`CONFIG_ZMK_EPAPER_LAYERS_INVERTED=y` draws white on black.
