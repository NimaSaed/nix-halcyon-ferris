/*
 * zip_xy_pack: split-peripheral input processor that folds a relative
 * pointing packet's REL_X and REL_Y events into a single event, so the split
 * transport sends one BLE notification per packet instead of two.
 *
 * ZMK's input split forwards every Zephyr input event as its own GATT
 * notification. Zephyr's cirque,pinnacle driver emits two per packet in
 * relative mode with taps disabled (REL_X, then REL_Y carrying the sync
 * flag), at up to 100 packets/s, i.e. 200 notifications/s on a link whose
 * connection event may only fit one or two of them.
 *
 * Rules: REL_X without sync is held; REL_Y (or a REL_X that carries sync)
 * closes the packet and is rewritten into a XY_PACK_CODE event (see
 * include/xy_pack.h) with the packet's sync flag. Packets with no movement
 * are dropped. Any other event passes through untouched, so a driver that
 * also reports a button (sync on the button event) still works: the packed
 * event simply goes out without sync and the button event closes the frame.
 *
 * Landing filter (see the binding): the Pinnacle reports a hump of upward
 * motion in the first ~100 ms after the finger lands, 40-100 counts on this
 * pad, before the stroke starts; with the dongle's 2x scaler that is a
 * 100-200 px jump on every re-centring touch. The first `landing-packets`
 * packets of a new stroke have their deltas limited per axis and direction,
 * so a 1-count nudge still passes while the hump is cut to a few counts.
 *
 * The central needs zip_xy_unpack first in every chain of its listener.
 */

#define DT_DRV_COMPAT zmk_input_processor_xy_pack

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

#include "xy_pack.h"

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS) ||                                       \
    IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_TRACE)
/* Own level so the summaries show while ZMK itself is kept at WRN/ERR. */
LOG_MODULE_REGISTER(xy_pack, LOG_LEVEL_INF);
#else
LOG_MODULE_REGISTER(xy_pack, CONFIG_ZMK_LOG_LEVEL);
#endif

struct xy_pack_config {
    uint8_t landing_packets;
    uint16_t landing_gap_ms;
    int16_t landing_max_up;
    int16_t landing_max_down;
    int16_t landing_max_x;
    uint8_t landing_refund_threshold;
    uint8_t landing_refund_packets;
};

struct xy_pack_data {
    int32_t x;
    int32_t y;
    int64_t last_pkt;       /* uptime of the previous packet, 0 before the first */
    uint8_t landing_left;   /* packets still inside the landing window */
    int32_t withheld_x;     /* counts clamped away in the current window */
    int32_t withheld_y;
    uint32_t head_motion;   /* raw |dx|+|dy| of window packets 3 and 4 */
    uint32_t tail_motion;   /* raw |dx|+|dy| of the last two window packets */
    int32_t refund_x;       /* withheld motion being paid back */
    int32_t refund_y;
    uint8_t refund_left;    /* packets over which to pay it back */
    uint32_t landing_pkts;  /* stats: packets altered by the filter */
    uint32_t landing_counts; /* stats: counts removed by the filter */
    uint32_t refund_counts;  /* stats: counts paid back (a real stroke, not a hump) */
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS)
    struct xy_stats stats;
#endif
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_TRACE)
    int64_t trace_last;
#endif
};

static inline void xy_pack_trace(struct xy_pack_data *data, int16_t x, int16_t y, int16_t fx,
                                 int16_t fy) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_TRACE)
    int64_t now = k_uptime_get();
    int64_t gap = data->trace_last ? now - data->trace_last : 0;
    data->trace_last = now;
    if (fx != x || fy != y) {
        LOG_INF("xy_pack: dx=%d dy=%d gap=%dms -> %d,%d", x, y, (int)gap, fx, fy);
    } else {
        LOG_INF("xy_pack: dx=%d dy=%d gap=%dms", x, y, (int)gap);
    }
#endif
}

/*
 * Landing filter: limits the deltas of the first packets of a new stroke,
 * then decides what it was. A landing hump peaks early and has decayed by
 * the end of the window; a real stroke that happens to start in the limited
 * direction is still rising or sustained. In the second case the withheld
 * motion is paid back over the next packets, so the stroke loses nothing
 * (its first part just arrives a few packets late); a hump is simply gone.
 * A quick flick that both rises and falls within the window looks like a
 * hump and loses its first part; measured on 231 landings, this rule got
 * 133 of 163 strokes and 52 of 70 humps right, the best of those tried.
 */
static void xy_pack_landing(const struct xy_pack_config *cfg, struct xy_pack_data *data,
                            int16_t *x, int16_t *y) {
    int64_t now = k_uptime_get();

    if (cfg->landing_packets > 0 &&
        (data->last_pkt == 0 || now - data->last_pkt >= cfg->landing_gap_ms)) {
        data->landing_left = cfg->landing_packets;
        data->withheld_x = 0;
        data->withheld_y = 0;
        data->head_motion = 0;
        data->tail_motion = 0;
        data->refund_left = 0; /* a new touch: forget any pending refund */
    }
    data->last_pkt = now;

    if (data->landing_left > 0) {
        data->landing_left--;

        int16_t ox = *x, oy = *y;
        if (*y < -cfg->landing_max_up) {
            *y = -cfg->landing_max_up;
        } else if (*y > cfg->landing_max_down) {
            *y = cfg->landing_max_down;
        }
        if (*x > cfg->landing_max_x) {
            *x = cfg->landing_max_x;
        } else if (*x < -cfg->landing_max_x) {
            *x = -cfg->landing_max_x;
        }
        if (ox != *x || oy != *y) {
            data->landing_pkts++;
            data->landing_counts += xy_abs(ox - *x) + xy_abs(oy - *y);
            data->withheld_x += ox - *x;
            data->withheld_y += oy - *y;
        }

        /* Hump or stroke? A hump peaks at packets 2-4 and has decayed by
         * the end of the window; a stroke is still rising or sustained. So
         * compare the raw motion of the last two window packets with that of
         * packets 3-4, with a small floor so that noise does not count. */
        uint8_t k = cfg->landing_packets - data->landing_left; /* packet number, 1-based */
        if (k == 3 || k == 4) {
            data->head_motion += xy_abs(ox) + xy_abs(oy);
        }
        if (data->landing_left < 2) {
            data->tail_motion += xy_abs(ox) + xy_abs(oy);
        }
        if (data->landing_left == 0) {
            if (cfg->landing_refund_packets > 0 &&
                data->tail_motion >= cfg->landing_refund_threshold &&
                data->tail_motion >= data->head_motion &&
                (data->withheld_x != 0 || data->withheld_y != 0)) {
                data->refund_x = data->withheld_x;
                data->refund_y = data->withheld_y;
                data->refund_left = cfg->landing_refund_packets;
            }
            data->withheld_x = 0;
            data->withheld_y = 0;
        }
        return;
    }

    if (data->refund_left > 0) {
        int32_t rx = data->refund_x / data->refund_left;
        int32_t ry = data->refund_y / data->refund_left;
        data->refund_x -= rx;
        data->refund_y -= ry;
        data->refund_left--;
        *x = xy_pack_clamp(*x + rx);
        *y = xy_pack_clamp(*y + ry);
        data->refund_counts += xy_abs(rx) + xy_abs(ry);
    }
}

static inline void xy_pack_note(struct xy_pack_data *data, bool empty, int16_t x, int16_t y) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS)
    int64_t now = k_uptime_get();
    xy_stats_note(&data->stats, empty, now, x, y);
    if (xy_stats_due(&data->stats, now)) {
        /* pkts: packets the chip delivered in the window (empty ones are
         * counted but not sent); stalls: gaps of 40-500 ms between them;
         * flips / one-axis / big / burst: see xy_pack.h; stack: free bytes
         * on the input thread. */
        LOG_INF("xy_pack: %u pkts in %u ms, %u empty, %u stalls, max gap %u ms, %u flips, "
                "%u one-axis, %u big, landing %u pkts %u counts %u refunded, max burst %u, "
                "stack free %u",
                data->stats.pkts, xy_stats_window_ms(&data->stats, now), data->stats.extra,
                data->stats.stalls, data->stats.max_gap_ms, data->stats.flips,
                data->stats.one_axis, data->stats.big, data->landing_pkts, data->landing_counts,
                data->refund_counts, data->stats.max_burst, xy_stats_stack_free());
        xy_stats_reset(&data->stats, now);
        data->landing_pkts = 0;
        data->landing_counts = 0;
        data->refund_counts = 0;
    }
#endif
}

static int xy_pack_handle_event(const struct device *dev, struct input_event *event,
                                uint32_t param1, uint32_t param2,
                                struct zmk_input_processor_state *state) {
    const struct xy_pack_config *cfg = dev->config;
    struct xy_pack_data *data = dev->data;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    switch (event->code) {
    case INPUT_REL_X:
        data->x += event->value;
        if (!event->sync) {
            /* The packet's REL_Y follows and closes it. */
            return ZMK_INPUT_PROC_STOP;
        }
        break;
    case INPUT_REL_Y:
        data->y += event->value;
        break;
    default:
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int16_t x = xy_pack_clamp(data->x);
    int16_t y = xy_pack_clamp(data->y);
    data->x = 0;
    data->y = 0;

    bool empty = (x == 0 && y == 0);
    xy_pack_note(data, empty, x, y); /* stats describe the chip's raw output */

    int16_t fx = x, fy = y;
    if (!empty) {
        xy_pack_landing(cfg, data, &fx, &fy);
    }
    xy_pack_trace(data, x, y, fx, fy);

    if (fx == 0 && fy == 0) {
        /* Nothing moved: not worth a radio packet. */
        return ZMK_INPUT_PROC_STOP;
    }

    event->code = XY_PACK_CODE;
    event->value = xy_pack_value(fx, fy);

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api xy_pack_driver_api = {
    .handle_event = xy_pack_handle_event,
};

#define XY_PACK_INST(n)                                                                            \
    static const struct xy_pack_config xy_pack_config_##n = {                                      \
        .landing_packets = DT_INST_PROP(n, landing_packets),                                       \
        .landing_gap_ms = DT_INST_PROP(n, landing_gap_ms),                                         \
        .landing_max_up = DT_INST_PROP(n, landing_max_up),                                         \
        .landing_max_down = DT_INST_PROP(n, landing_max_down),                                     \
        .landing_max_x = DT_INST_PROP(n, landing_max_x),                                           \
        .landing_refund_threshold = DT_INST_PROP(n, landing_refund_threshold),                     \
        .landing_refund_packets = DT_INST_PROP(n, landing_refund_packets),                         \
    };                                                                                             \
    static struct xy_pack_data xy_pack_data_##n = {};                                              \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &xy_pack_data_##n, &xy_pack_config_##n, POST_KERNEL,      \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &xy_pack_driver_api);

DT_INST_FOREACH_STATUS_OKAY(XY_PACK_INST)
