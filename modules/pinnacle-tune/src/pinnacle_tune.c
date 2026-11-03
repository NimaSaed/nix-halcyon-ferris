/*
 * One-shot tuning of a Cirque Pinnacle on SPI after Zephyr's cirque,pinnacle
 * driver has initialised it (see README.md). Talks to the chip over the same
 * bus with the same Register Access Protocol the driver uses: a standard
 * register is written as [0x80 | addr, value] and read as [0xA0 | addr, FB,
 * FB, FB] with the value in the fourth byte; extended registers go through
 * the ERA window (0x1B value, 0x1C/0x1D address, 0x1E control, which the
 * chip clears when done).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pinnacle_tune, LOG_LEVEL_INF);

#define TRACKPAD_NODE DT_NODELABEL(trackpad)

#define REG_FIRMWARE_ID 0x00
#define REG_STATUS1 0x02
#define REG_FEED_CONFIG1 0x04
#define REG_FEED_CONFIG3 0x06
#define REG_CAL_CONFIG1 0x07
#define REG_ERA_VALUE 0x1B
#define REG_ERA_ADDR_HIGH 0x1C
#define REG_ERA_ADDR_LOW 0x1D
#define REG_ERA_CTRL 0x1E

#define FIRMWARE_ID 0x07
#define FEED_CONFIG1_FEED_ENABLE BIT(0)
#define CAL_CONFIG1_CALIBRATE BIT(0)
#define ERA_CTRL_READ BIT(0)
#define ERA_CTRL_WRITE BIT(1)

#define ERA_X_WIDEZMIN 0x0149
#define ERA_Y_WIDEZMIN 0x0168

#define SPI_FB 0xFB
#define READ_REG(a) (0xA0 | (a))
#define WRITE_REG(a) (0x80 | (a))

/* Same operation flags as the driver (Mode 1: CPOL=0, CPHA=1, MSB first). */
#define PINNACLE_SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_MODE_CPHA | SPI_WORD_SET(8))

static const struct spi_dt_spec bus = SPI_DT_SPEC_GET(TRACKPAD_NODE, PINNACLE_SPI_OP, 0U);

static int rap_write(uint8_t addr, uint8_t value) {
    uint8_t tx[] = {WRITE_REG(addr), value};
    const struct spi_buf b = {.buf = tx, .len = sizeof(tx)};
    const struct spi_buf_set set = {.buffers = &b, .count = 1};
    return spi_write_dt(&bus, &set);
}

static int rap_read(uint8_t addr, uint8_t *value) {
    uint8_t tx[] = {READ_REG(addr), SPI_FB, SPI_FB, SPI_FB};
    const struct spi_buf tb = {.buf = tx, .len = sizeof(tx)};
    const struct spi_buf_set tset = {.buffers = &tb, .count = 1};
    const struct spi_buf rb[] = {{.buf = NULL, .len = 3}, {.buf = value, .len = 1}};
    const struct spi_buf_set rset = {.buffers = rb, .count = ARRAY_SIZE(rb)};
    return spi_transceive_dt(&bus, &tset, &rset);
}

static int clear_flags(void) { return rap_write(REG_STATUS1, 0x00); }

/* Wait for the chip to clear the ERA control register (command done). */
static int era_wait(void) {
    for (int i = 0; i < 20; i++) {
        uint8_t ctrl;
        int rc = rap_read(REG_ERA_CTRL, &ctrl);
        if (rc) {
            return rc;
        }
        if (ctrl == 0) {
            return 0;
        }
        k_sleep(K_MSEC(5));
    }
    return -ETIMEDOUT;
}

static int era_read(uint16_t addr, uint8_t *value) {
    int rc = rap_write(REG_ERA_ADDR_HIGH, addr >> 8);
    rc = rc ? rc : rap_write(REG_ERA_ADDR_LOW, addr & 0xFF);
    rc = rc ? rc : rap_write(REG_ERA_CTRL, ERA_CTRL_READ);
    rc = rc ? rc : era_wait();
    rc = rc ? rc : rap_read(REG_ERA_VALUE, value);
    rc = rc ? rc : clear_flags();
    return rc;
}

static int era_write(uint16_t addr, uint8_t value) {
    int rc = rap_write(REG_ERA_VALUE, value);
    rc = rc ? rc : rap_write(REG_ERA_ADDR_HIGH, addr >> 8);
    rc = rc ? rc : rap_write(REG_ERA_ADDR_LOW, addr & 0xFF);
    rc = rc ? rc : rap_write(REG_ERA_CTRL, ERA_CTRL_WRITE);
    rc = rc ? rc : era_wait();
    rc = rc ? rc : clear_flags();
    return rc;
}

static int tune_era(const char *name, uint16_t addr, uint8_t value) {
    uint8_t before = 0, after = 0;
    int rc = era_read(addr, &before);
    rc = rc ? rc : era_write(addr, value);
    rc = rc ? rc : era_read(addr, &after);
    if (rc) {
        LOG_ERR("%s (ERA 0x%04x): failed (%d)", name, addr, rc);
    } else {
        LOG_INF("%s (ERA 0x%04x): 0x%02x -> 0x%02x", name, addr, before, after);
    }
    return rc;
}

static int tune_reg(const char *name, uint8_t addr, uint8_t clear, uint8_t set) {
    uint8_t before = 0, after = 0;
    int rc = rap_read(addr, &before);
    rc = rc ? rc : rap_write(addr, (before & ~clear) | set);
    rc = rc ? rc : rap_read(addr, &after);
    if (rc) {
        LOG_ERR("%s (0x%02x): failed (%d)", name, addr, rc);
    } else {
        LOG_INF("%s (0x%02x): 0x%02x -> 0x%02x", name, addr, before, after);
    }
    return rc;
}

static int calibrate(void) {
    uint8_t cal;
    int rc = rap_read(REG_CAL_CONFIG1, &cal);
    rc = rc ? rc : rap_write(REG_CAL_CONFIG1, cal | CAL_CONFIG1_CALIBRATE);
    if (rc) {
        return rc;
    }
    /* ~100 ms per Cirque; the bit clears when done. */
    for (int i = 0; i < 40; i++) {
        k_sleep(K_MSEC(10));
        rc = rap_read(REG_CAL_CONFIG1, &cal);
        if (rc) {
            return rc;
        }
        if (!(cal & CAL_CONFIG1_CALIBRATE)) {
            LOG_INF("calibrated in ~%d ms", (i + 1) * 10);
            return clear_flags();
        }
    }
    LOG_WRN("calibration did not finish within 400 ms");
    return clear_flags();
}

static int pinnacle_tune_init(void) {
    uint8_t id = 0, feed1 = 0;
    int rc;

    if (!spi_is_ready_dt(&bus)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    rc = rap_read(REG_FIRMWARE_ID, &id);
    if (rc || id != FIRMWARE_ID) {
        LOG_ERR("no Pinnacle (id 0x%02x, rc %d), nothing tuned", id, rc);
        return rc ? rc : -ENODEV;
    }

    /* Pause the feed while writing (as the Cirque samples do). */
    rc = rap_read(REG_FEED_CONFIG1, &feed1);
    rc = rc ? rc : rap_write(REG_FEED_CONFIG1, feed1 & ~FEED_CONFIG1_FEED_ENABLE);
    if (rc) {
        LOG_ERR("could not pause the feed (%d)", rc);
        return rc;
    }

#if IS_ENABLED(CONFIG_PINNACLE_TUNE_EDGE_SENSITIVITY)
    tune_era("X WideZ-min", ERA_X_WIDEZMIN, CONFIG_PINNACLE_TUNE_EDGE_X_WIDEZMIN);
    tune_era("Y WideZ-min", ERA_Y_WIDEZMIN, CONFIG_PINNACLE_TUNE_EDGE_Y_WIDEZMIN);
#endif
#if CONFIG_PINNACLE_TUNE_CALCONFIG1_CLEAR != 0
    tune_reg("CalConfig1", REG_CAL_CONFIG1, CONFIG_PINNACLE_TUNE_CALCONFIG1_CLEAR, 0);
#else
    tune_reg("CalConfig1 (unchanged)", REG_CAL_CONFIG1, 0, 0);
#endif
#if CONFIG_PINNACLE_TUNE_FEEDCONFIG3_SET != 0
    tune_reg("FeedConfig3", REG_FEED_CONFIG3, 0, CONFIG_PINNACLE_TUNE_FEEDCONFIG3_SET);
#else
    tune_reg("FeedConfig3 (unchanged)", REG_FEED_CONFIG3, 0, 0);
#endif
#if IS_ENABLED(CONFIG_PINNACLE_TUNE_CALIBRATE)
    calibrate();
#endif

    rc = rap_write(REG_FEED_CONFIG1, feed1);
    rc = rc ? rc : clear_flags();
    if (rc) {
        LOG_ERR("could not resume the feed (%d)", rc);
        return rc;
    }
    LOG_INF("done, feed restored (FeedConfig1 0x%02x)", feed1);
    return 0;
}

/* After every driver init (the Pinnacle driver runs at POST_KERNEL,
 * CONFIG_INPUT_INIT_PRIORITY); the priority must be a literal. */
SYS_INIT(pinnacle_tune_init, APPLICATION, 0);
