/*
 * Status screen for the Halcyon e-paper on a central (the wired left half):
 * the keymap's layers as a column, a dot in front of the active one.
 *
 * The panel (solomon,ssd1680 in splitkb's mod_display_epaper.dtsi) is 184
 * wide and 100 high for the driver, and the module is mounted turned a
 * quarter turn, so the user looks at a surface 88 wide and 184 tall (88, not
 * 100: splitkb's own widgets use that much of the height, the rest is under
 * the bezel). This file draws in the user's orientation on a hidden canvas
 * and turns the result into the visible, panel-oriented canvas before every
 * update, the same way splitkb's widgets do: lv_draw_sw_rotate puts source
 * (x, y) at panel (y, W - 1 - x).
 *
 * Every update redraws the whole column; the e-paper's partial refresh
 * only changes the pixels that differ, and its duration does not depend on
 * how many do, so this costs nothing over moving the dot alone.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

/* The user's view, and the panel's. */
#define VIEW_W 88
#define VIEW_H 184
#define PANEL_W VIEW_H
#define PANEL_H VIEW_W

#define CF LV_COLOR_FORMAT_L8 /* the smallest format lv_draw_sw_rotate handles */
#define VIEW_BUF_SIZE LV_CANVAS_BUF_SIZE(VIEW_W, VIEW_H, 8, LV_DRAW_BUF_STRIDE_ALIGN)
#define PANEL_BUF_SIZE LV_CANVAS_BUF_SIZE(PANEL_W, PANEL_H, 8, LV_DRAW_BUF_STRIDE_ALIGN)

#define FG (IS_ENABLED(CONFIG_ZMK_EPAPER_LAYERS_INVERTED) ? lv_color_white() : lv_color_black())
#define BG (IS_ENABLED(CONFIG_ZMK_EPAPER_LAYERS_INVERTED) ? lv_color_black() : lv_color_white())

/* Layout, in the user's view. One row per layer, spread over the height
 * (28 px apart for the six layers of the keymap); rows that would not fit at
 * the font's line height are left out. */
#define FONT (&lv_font_montserrat_14)
#define MARGIN_Y 6
#define ROW_H_MAX 28
#define ROW_H_MIN 16
#define DOT_X 10 /* centre */
#define DOT_R 3
#define TEXT_X 20

struct layer_list_state {
    zmk_keymap_layer_index_t active;
};

static lv_obj_t *view;  /* hidden, drawn on in the user's orientation */
static lv_obj_t *panel; /* visible, the panel's orientation */
static uint8_t view_buf[VIEW_BUF_SIZE];
static uint8_t panel_buf[PANEL_BUF_SIZE];

static void draw(zmk_keymap_layer_index_t active) {
    lv_canvas_fill_bg(view, BG, LV_OPA_COVER);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = FG;
    label_dsc.font = FONT;
    label_dsc.align = LV_TEXT_ALIGN_LEFT;
    label_dsc.text_local = 1; /* the fallback name below lives on the stack */

    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.bg_color = FG;
    dot_dsc.radius = LV_RADIUS_CIRCLE;

    const int n = ZMK_KEYMAP_LAYERS_LEN;
    const int row_h = CLAMP((VIEW_H - 2 * MARGIN_Y) / n, ROW_H_MIN, ROW_H_MAX);
    const int rows = MIN(n, (VIEW_H - 2 * MARGIN_Y) / row_h);
    if (rows < n) {
        LOG_WRN("%d layers, only %d fit on the display", n, rows);
    }

    lv_layer_t layer;
    lv_canvas_init_layer(view, &layer);

    for (int i = 0; i < rows; i++) {
        const int y = MARGIN_Y + i * row_h;
        const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(i));
        char fallback[12];
        if (name == NULL || name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "Layer %d", i);
            name = fallback;
        }

        label_dsc.text = name;
        lv_area_t text = {TEXT_X, y, VIEW_W - 1, y + row_h - 1};
        lv_draw_label(&layer, &label_dsc, &text);

        if (i == active) {
            const int cy = y + FONT->line_height / 2;
            lv_area_t dot = {DOT_X - DOT_R, cy - DOT_R, DOT_X + DOT_R, cy + DOT_R};
            lv_draw_rect(&layer, &dot_dsc, &dot);
        }
    }

    lv_canvas_finish_layer(view, &layer);

    lv_draw_sw_rotate(lv_canvas_get_draw_buf(view)->data, lv_canvas_get_draw_buf(panel)->data,
                      VIEW_W, VIEW_H, lv_draw_buf_width_to_stride(VIEW_W, CF),
                      lv_draw_buf_width_to_stride(PANEL_W, CF), LV_DISPLAY_ROTATION_90, CF);
    lv_obj_invalidate(panel);
}

static void layer_list_update_cb(struct layer_list_state state) {
    if (panel != NULL) {
        draw(state.active);
    }
}

static struct layer_list_state layer_list_get_state(const zmk_event_t *eh) {
    return (struct layer_list_state){.active = zmk_keymap_highest_layer_active()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_list, struct layer_list_state, layer_list_update_cb,
                            layer_list_get_state)
ZMK_SUBSCRIPTION(widget_layer_list, zmk_layer_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    view = lv_canvas_create(screen);
    lv_canvas_set_buffer(view, view_buf, VIEW_W, VIEW_H, CF);
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);

    panel = lv_canvas_create(screen);
    lv_canvas_set_buffer(panel, panel_buf, PANEL_W, PANEL_H, CF);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);

    widget_layer_list_init();

    return screen;
}
