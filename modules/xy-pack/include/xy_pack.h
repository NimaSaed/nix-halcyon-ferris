/*
 * Shared encoding for the zip_xy_pack (split peripheral) and zip_xy_unpack
 * (split central) input processors: one INPUT_EV_REL event whose 32-bit value
 * carries a packet's X delta in the low 16 bits and its Y delta in the high
 * 16 bits. Both parts must be built from the same definition.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

/* Event code used for the packed event. INPUT_REL_MISC is otherwise unused
 * by ZMK's input listener (it ignores it), so a packed event that reaches a
 * central without zip_xy_unpack does nothing instead of moving the cursor. */
#define XY_PACK_CODE INPUT_REL_MISC

static inline int32_t xy_pack_value(int16_t x, int16_t y) {
    return (int32_t)(((uint32_t)(uint16_t)y << 16) | (uint32_t)(uint16_t)x);
}

static inline int16_t xy_pack_x(int32_t value) { return (int16_t)((uint32_t)value & 0xffffU); }

static inline int16_t xy_pack_y(int32_t value) {
    return (int16_t)(((uint32_t)value >> 16) & 0xffffU);
}

static inline int16_t xy_pack_clamp(int32_t v) {
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
}

/*
 * Traffic statistics (CONFIG_ZMK_INPUT_PROCESSOR_XY_PACK_STATS): both sides
 * count the packets they see and the gaps between consecutive packets, and
 * log one summary line per second of traffic. Comparing the two logs tells
 * where a stall happens: a gap the right half sees too means the chip (or
 * the finger) stopped, a gap only the dongle sees means the link or a queue
 * in between held the packets back.
 */
#define XY_STATS_PERIOD_MS 1000
#define XY_STATS_STALL_MS 40      /* gap >= this between packets counts as a stall... */
#define XY_STATS_STROKE_GAP_MS 500 /* ...unless it is this long: a new stroke, not counted */
#define XY_STATS_BURST_MS 3        /* packets closer than this form a burst */
#define XY_STATS_BIG_DELTA 40      /* |dx| or |dy| at least this in one packet is "big" */

/*
 * burst: longest run of packets arriving less than XY_STATS_BURST_MS apart.
 * The chip delivers one packet per 10 ms, so the right half never sees
 * bursts; the dongle normally sees 1-2 (one connection event's worth). A
 * long burst on the dongle right after a gap means the link stalled and the
 * right half's send queue then drained in one go: a link problem, whereas a
 * gap without a burst is the chip (or the finger) having produced nothing.
 */
/*
 * Quality of the deltas themselves (what "wrong direction", "choppy" and
 * "moves in one axis" look like in numbers):
 *   flips    sign reversals of dx or dy between consecutive packets of the
 *            same stroke (a straight drag has none; a contact that is lost
 *            and re-found produces them);
 *   one-axis packets where exactly one of dx, dy is zero;
 *   big      packets with |dx| or |dy| >= XY_STATS_BIG_DELTA (jumps).
 */
struct xy_stats {
    int64_t window_start; /* 0 until the first packet */
    int64_t last_pkt;
    uint32_t pkts;
    uint32_t extra; /* side-specific counter, see the LOG line */
    uint32_t stalls;
    uint32_t max_gap_ms;
    uint32_t burst;
    uint32_t max_burst;
    uint32_t flips;
    uint32_t one_axis;
    uint32_t big;
    int8_t last_sx; /* sign of the last non-zero dx / dy in this stroke */
    int8_t last_sy;
};

static inline int8_t xy_sign(int32_t v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

static inline uint32_t xy_abs(int32_t v) { return v < 0 ? (uint32_t)(-v) : (uint32_t)v; }

static inline void xy_stats_note(struct xy_stats *s, bool extra, int64_t now, int16_t x,
                                 int16_t y) {
    bool same_stroke = false;

    if (s->last_pkt != 0) {
        int64_t gap = now - s->last_pkt;
        if (gap < XY_STATS_STROKE_GAP_MS) {
            same_stroke = true;
            if (gap >= XY_STATS_STALL_MS) {
                s->stalls++;
            }
            if (gap > s->max_gap_ms) {
                s->max_gap_ms = (uint32_t)gap;
            }
        }
        s->burst = (gap < XY_STATS_BURST_MS) ? s->burst + 1 : 1;
    } else {
        s->burst = 1;
    }
    if (s->burst > s->max_burst) {
        s->max_burst = s->burst;
    }

    if (!same_stroke) {
        s->last_sx = 0;
        s->last_sy = 0;
    }
    int8_t sx = xy_sign(x);
    int8_t sy = xy_sign(y);
    if (sx != 0) {
        if (s->last_sx != 0 && sx != s->last_sx) {
            s->flips++;
        }
        s->last_sx = sx;
    }
    if (sy != 0) {
        if (s->last_sy != 0 && sy != s->last_sy) {
            s->flips++;
        }
        s->last_sy = sy;
    }
    if ((x == 0) != (y == 0)) {
        s->one_axis++;
    }
    if (xy_abs(x) >= XY_STATS_BIG_DELTA || xy_abs(y) >= XY_STATS_BIG_DELTA) {
        s->big++;
    }

    s->last_pkt = now;
    s->pkts++;
    if (extra) {
        s->extra++;
    }
    if (s->window_start == 0) {
        s->window_start = now;
    }
}

/* Free bytes on the calling thread's stack (the input thread, when called
 * from a processor), or 0 when the kernel does not track it
 * (CONFIG_INIT_STACKS + CONFIG_THREAD_STACK_INFO, both selected by
 * CONFIG_THREAD_ANALYZER). */
static inline uint32_t xy_stats_stack_free(void) {
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
    size_t unused = 0;
    if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
        return (uint32_t)unused;
    }
#endif
    return 0;
}

static inline bool xy_stats_due(const struct xy_stats *s, int64_t now) {
    return s->window_start != 0 && (now - s->window_start) >= XY_STATS_PERIOD_MS;
}

static inline uint32_t xy_stats_window_ms(const struct xy_stats *s, int64_t now) {
    return (uint32_t)(now - s->window_start);
}

static inline void xy_stats_reset(struct xy_stats *s, int64_t now) {
    s->window_start = now;
    s->pkts = 0;
    s->extra = 0;
    s->stalls = 0;
    s->max_gap_ms = 0;
    s->max_burst = 0;
    s->flips = 0;
    s->one_axis = 0;
    s->big = 0;
}
