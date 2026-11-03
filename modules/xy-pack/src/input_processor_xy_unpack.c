/*
 * zip_xy_unpack: split-central input processor that turns the packed event
 * produced by zip_xy_pack on the peripheral back into REL_X and REL_Y, so
 * the rest of the chain (scalers, temp layer, scroll mapper) and ZMK's input
 * listener see the usual event stream.
 *
 * A processor can only hand one event back to the listener, so when both
 * axes moved this event becomes the REL_X (without sync) and the REL_Y is
 * re-injected with input_report() carrying the packet's sync flag. Zephyr's
 * input thread appends it to its queue, behind any packets that arrived in
 * the same connection event and are still queued; those are unpacked the
 * same way, so the sums that reach the HID report are unchanged, only the
 * X/Y pairing inside such a burst can shift by a packet. If only one axis
 * moved the event is rewritten in place and nothing is injected.
 *
 * Must be the first processor in every chain of the listener (base and
 * layer overrides), otherwise the processors before it see the packed code
 * and ignore it. Events that are not packed pass through untouched, so the
 * processor is harmless on a central whose peripheral does not pack.
 */

#define DT_DRV_COMPAT zmk_input_processor_xy_unpack

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

#include "xy_pack.h"

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS)
/* Own level so the summaries show while ZMK itself is kept at WRN/ERR. */
LOG_MODULE_REGISTER(xy_unpack, LOG_LEVEL_INF);
#else
LOG_MODULE_REGISTER(xy_unpack, CONFIG_ZMK_LOG_LEVEL);
#endif

struct xy_unpack_data {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS)
    struct xy_stats stats;
#else
    uint8_t unused;
#endif
};

static inline void xy_unpack_note(struct xy_unpack_data *data, bool inject_failed, int16_t x,
                                  int16_t y) {
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS)
    int64_t now = k_uptime_get();
    xy_stats_note(&data->stats, inject_failed, now, x, y);
    if (xy_stats_due(&data->stats, now)) {
        /* pkts: packed packets that arrived from the right half in the
         * window; inject-fail: packets whose REL_Y could not be queued (the
         * dongle's input queue was full, i.e. the input thread is slow);
         * stalls: gaps of 40-500 ms between packets; flips / one-axis / big:
         * see xy_pack.h (should match the right half's line); burst: longest
         * run of packets < 3 ms apart (a long one right after a gap = the
         * link stalled and the right half's queue drained); stack: free
         * bytes on the input thread. */
        LOG_INF("xy_unpack: %u pkts in %u ms, %u inject-fail, %u stalls, max gap %u ms, "
                "%u flips, %u one-axis, %u big, max burst %u, stack free %u",
                data->stats.pkts, xy_stats_window_ms(&data->stats, now), data->stats.extra,
                data->stats.stalls, data->stats.max_gap_ms, data->stats.flips,
                data->stats.one_axis, data->stats.big, data->stats.max_burst,
                xy_stats_stack_free());
        xy_stats_reset(&data->stats, now);
    }
#endif
}

static int xy_unpack_handle_event(const struct device *dev, struct input_event *event,
                                  uint32_t param1, uint32_t param2,
                                  struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL || event->code != XY_PACK_CODE) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct xy_unpack_data *data = dev->data;
    int16_t x = xy_pack_x(event->value);
    int16_t y = xy_pack_y(event->value);

    if (y == 0) {
        xy_unpack_note(data, false, x, y);
        event->code = INPUT_REL_X;
        event->value = x;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (x == 0) {
        xy_unpack_note(data, false, x, y);
        event->code = INPUT_REL_Y;
        event->value = y;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Both axes: X now, Y queued right behind it with the packet's sync. */
    int ret = input_report(event->dev, INPUT_EV_REL, INPUT_REL_Y, y, event->sync, K_NO_WAIT);
    xy_unpack_note(data, ret < 0, x, y);

    event->code = INPUT_REL_X;
    event->value = x;

    if (ret < 0) {
        /* Input queue full: keep the sync on the X so the frame still goes
         * out; this packet's Y movement is lost. */
        LOG_WRN("xy_unpack: could not re-inject REL_Y (%d), dropped", ret);
    } else {
        event->sync = 0;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api xy_unpack_driver_api = {
    .handle_event = xy_unpack_handle_event,
};

#define XY_UNPACK_INST(n)                                                                          \
    static struct xy_unpack_data xy_unpack_data_##n = {};                                          \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &xy_unpack_data_##n, NULL, POST_KERNEL,                   \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &xy_unpack_driver_api);

DT_INST_FOREACH_STATUS_OKAY(XY_UNPACK_INST)
