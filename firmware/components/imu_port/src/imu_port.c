/*
 * GYRO SNAKE — LSM6DS3TR-C driver and 100 Hz IMU task.
 *
 * Registers (datasheet):
 *   WHO_AM_I  0x0F  expect 0x6A
 *   CTRL1_XL  0x10  ODR_XL = 104 Hz, FS_XL = +-2 g   -> 0x40
 *   CTRL2_G   0x11  ODR_G  = 104 Hz, FS_G  = +-500 dps -> 0x44
 *   CTRL3_C   0x12  BDU + IF_INC                      -> 0x44
 *   OUTX_L_G  0x22  6 bytes, little-endian int16
 *   OUTX_L_XL 0x28  6 bytes, little-endian int16
 *
 * Sensitivities: 0.000061 g/LSB (+-2 g), 0.0175 dps/LSB (+-500 dps).
 */
#include "imu_port.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gs_imu";

/* ---- Bus and sensor ------------------------------------------------------ */

#define GS_IMU_I2C_PORT 1 /* I2C1; the new API takes a plain port number */
#define GS_IMU_PIN_SDA 1
#define GS_IMU_PIN_SCL 0
#define GS_IMU_I2C_ADDR 0x6A
#define GS_IMU_I2C_HZ 100000
#define GS_IMU_I2C_TIMEOUT_MS 50

#define GS_IMU_REG_WHO_AM_I 0x0F
#define GS_IMU_REG_CTRL1_XL 0x10
#define GS_IMU_REG_CTRL2_G 0x11
#define GS_IMU_REG_CTRL3_C 0x12
#define GS_IMU_REG_OUTX_L_G 0x22
#define GS_IMU_REG_OUTX_L_XL 0x28

#define GS_IMU_WHO_AM_I 0x6A
#define GS_IMU_CTRL1_XL_104HZ_2G 0x40
#define GS_IMU_CTRL2_G_104HZ_500DPS 0x44
#define GS_IMU_CTRL3_C_BDU_IFINC 0x44

#define GS_IMU_ACCEL_G_PER_LSB 0.000061f
#define GS_IMU_GYRO_DPS_PER_LSB 0.0175f

/* ---- Task tuning --------------------------------------------------------- */

#define GS_IMU_TASK_PERIOD_MS 10
#define GS_IMU_TASK_PRIO 6
#define GS_IMU_TASK_STACK 6144 /* NVS writes happen in this task */
#define GS_IMU_PROBE_EVERY_TICKS 50 /* 500 ms */
#define GS_IMU_READ_FAIL_LIMIT 100
#define GS_IMU_CALIB_FAIL_READS 20

#define GS_IMU_RAD_TO_DEG (180.0f / 3.14159265358979323846f)

/* ---- State --------------------------------------------------------------- */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_initialized;

static atomic_bool s_sensor_ready;
static atomic_bool s_task_started;
static atomic_bool s_calib_request;
static atomic_bool s_calib_result_ready;
static atomic_int s_direction = IMU_DIR_NONE;
static atomic_uint s_progress;

static imu_port_calib_result_t s_calib_result;

static mahony_t s_mahony;
static tilt_t s_tilt;
static imu_calib_t s_calib;
static bool s_calibrating;

static imu_bias_t s_bias; /* valid == true once a calibration is applied */
static float s_neutral_roll;
static float s_neutral_pitch;

static int s_last_who = -1;

/* ---- I2C helpers --------------------------------------------------------- */

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), GS_IMU_I2C_TIMEOUT_MS);
}

static esp_err_t imu_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, GS_IMU_I2C_TIMEOUT_MS);
}

static esp_err_t imu_sensor_config(void)
{
    esp_err_t err = imu_write_reg(GS_IMU_REG_CTRL1_XL, GS_IMU_CTRL1_XL_104HZ_2G);
    if (err == ESP_OK) err = imu_write_reg(GS_IMU_REG_CTRL2_G, GS_IMU_CTRL2_G_104HZ_500DPS);
    if (err == ESP_OK) err = imu_write_reg(GS_IMU_REG_CTRL3_C, GS_IMU_CTRL3_C_BDU_IFINC);
    return err;
}

/* One probe + configuration attempt. Sets s_sensor_ready on success. */
static esp_err_t imu_probe_once(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    uint8_t who = 0;
    esp_err_t err = imu_read_regs(GS_IMU_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) return err;

    if (who != GS_IMU_WHO_AM_I) {
        if (who != (uint8_t)s_last_who) {
            ESP_LOGW(TAG, "WHO_AM_I=0x%02X (expected 0x%02X)", who, GS_IMU_WHO_AM_I);
            s_last_who = who;
        }
        return ESP_ERR_NOT_FOUND;
    }

    err = imu_sensor_config();
    if (err != ESP_OK) return err;

    /* Let the first 104 Hz sample land before the task reads it. */
    vTaskDelay(pdMS_TO_TICKS(10));
    atomic_store(&s_sensor_ready, true);
    return ESP_OK;
}

static int16_t le16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static esp_err_t imu_read_sample(imu_sample_t *out)
{
    uint8_t g[6];
    uint8_t a[6];

    esp_err_t err = imu_read_regs(GS_IMU_REG_OUTX_L_G, g, sizeof(g));
    if (err != ESP_OK) return err;
    err = imu_read_regs(GS_IMU_REG_OUTX_L_XL, a, sizeof(a));
    if (err != ESP_OK) return err;

    out->gx = (float)le16(&g[0]) * GS_IMU_GYRO_DPS_PER_LSB;
    out->gy = (float)le16(&g[2]) * GS_IMU_GYRO_DPS_PER_LSB;
    out->gz = (float)le16(&g[4]) * GS_IMU_GYRO_DPS_PER_LSB;
    out->ax = (float)le16(&a[0]) * GS_IMU_ACCEL_G_PER_LSB;
    out->ay = (float)le16(&a[2]) * GS_IMU_ACCEL_G_PER_LSB;
    out->az = (float)le16(&a[4]) * GS_IMU_ACCEL_G_PER_LSB;
    return ESP_OK;
}

/* ---- Calibration --------------------------------------------------------- */

static void imu_publish_result(bool ok)
{
    imu_port_calib_result_t r;
    memset(&r, 0, sizeof(r));
    r.ok = ok;
    if (ok) {
        r.bias = s_bias;
        r.neutral_roll = s_neutral_roll;
        r.neutral_pitch = s_neutral_pitch;
    }
    s_calib_result = r;
    atomic_store_explicit(&s_calib_result_ready, true, memory_order_release);
}

static void imu_fail_calibration(const char *why)
{
    ESP_LOGW(TAG, "calibration failed: %s", why);
    s_calibrating = false;
    atomic_store(&s_progress, 0u);
    atomic_store(&s_direction, IMU_DIR_NONE);
    tilt_reset(&s_tilt);
    imu_publish_result(false);
}

static void imu_calibration_begin(void)
{
    imu_calib_reset(&s_calib, IMU_PORT_CALIB_SAMPLES);
    imu_calib_start(&s_calib);
    s_calibrating = true;
    atomic_store(&s_progress, 0u);
    atomic_store(&s_direction, IMU_DIR_NONE);
    tilt_reset(&s_tilt);
    ESP_LOGI(TAG, "calibration window started: %d samples at %d Hz",
             IMU_PORT_CALIB_SAMPLES, IMU_PORT_SAMPLE_HZ);
}

static void imu_calibration_finish(void)
{
    imu_bias_t bias;
    imu_calib_result(&s_calib, &bias);
    bias.valid = true;

    /* Mahony angles are radians; the neutral pose is stored in degrees. */
    float roll = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    mahony_get_angles(&s_mahony, &roll, &pitch, &yaw);

    s_bias = bias;
    s_neutral_roll = roll * GS_IMU_RAD_TO_DEG;
    s_neutral_pitch = pitch * GS_IMU_RAD_TO_DEG;
    tilt_reset(&s_tilt);

    board_imu_calib_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = GS_IMU_CALIB_VERSION;
    blob.calibrated = 1;
    blob.gyro_bias[0] = bias.gx;
    blob.gyro_bias[1] = bias.gy;
    blob.gyro_bias[2] = bias.gz;
    blob.neutral_roll = s_neutral_roll;
    blob.neutral_pitch = s_neutral_pitch;

    const esp_err_t err = storage_port_save_imu_calib(&blob);
    if (err != ESP_OK) {
        /* The pose is still valid for this session; only persistence failed. */
        ESP_LOGW(TAG, "calibration not persisted: %s", esp_err_to_name(err));
    }

    s_calibrating = false;
    atomic_store(&s_progress, 100u);
    imu_publish_result(true);

    ESP_LOGI(TAG,
             "calibration complete: gyro bias %d/%d/%d mdps, neutral %d/%d cdeg",
             (int)(bias.gx * 1000.0f), (int)(bias.gy * 1000.0f),
             (int)(bias.gz * 1000.0f), (int)(s_neutral_roll * 100.0f),
             (int)(s_neutral_pitch * 100.0f));
}

/* ---- 100 Hz task --------------------------------------------------------- */

static void imu_task(void *arg)
{
    (void)arg;

    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(GS_IMU_TASK_PERIOD_MS);
    int fail_run = 0;
    int probe_countdown = 0;

    for (;;) {
        if (atomic_exchange_explicit(&s_calib_request, false, memory_order_acquire)) {
            if (!atomic_load(&s_sensor_ready) || !s_dev) {
                imu_fail_calibration("sensor not ready");
            } else {
                imu_calibration_begin();
            }
        }

        if (!atomic_load(&s_sensor_ready)) {
            if (--probe_countdown <= 0) {
                probe_countdown = GS_IMU_PROBE_EVERY_TICKS;
                if (imu_probe_once() == ESP_OK) {
                    ESP_LOGI(TAG, "LSM6DS3TR-C ready (WHO_AM_I=0x6A)");
                    fail_run = 0;
                }
            }
            if (xTaskDelayUntil(&last, period) == pdFALSE) last = xTaskGetTickCount();
            continue;
        }

        imu_sample_t raw;
        const esp_err_t err = imu_read_sample(&raw);
        if (err != ESP_OK) {
            if (fail_run == 0) {
                ESP_LOGW(TAG, "sample read failed: %s", esp_err_to_name(err));
            }
            fail_run++;
            if (s_calibrating && fail_run >= GS_IMU_CALIB_FAIL_READS) {
                imu_fail_calibration("sensor read failed during calibration");
            }
            if (fail_run >= GS_IMU_READ_FAIL_LIMIT) {
                fail_run = 0;
                atomic_store(&s_sensor_ready, false);
                atomic_store(&s_direction, IMU_DIR_NONE);
                ESP_LOGE(TAG, "%d consecutive read failures; re-probing the sensor",
                         GS_IMU_READ_FAIL_LIMIT);
            }
            if (xTaskDelayUntil(&last, period) == pdFALSE) last = xTaskGetTickCount();
            continue;
        }
        fail_run = 0;

        if (s_calibrating) {
            /* The reference window: the mean gyro bias comes from raw, and
             * Mahony sees gravity-intact raw data. */
            const int completed = imu_calib_feed(&s_calib, &raw, &raw);
            mahony_update(&s_mahony, &raw);
            if (completed) {
                imu_calibration_finish();
            } else {
                uint32_t pct =
                    (s_calib.n * 100u) / (uint32_t)IMU_PORT_CALIB_SAMPLES;
                if (pct > 99u) pct = 99u;
                atomic_store(&s_progress, (unsigned)pct);
            }
        } else {
            /* Normal mode: subtract the gyro bias ONLY; gravity stays in the
             * accelerometer triplet that Mahony normalizes. */
            imu_sample_t corrected = raw;
            imu_apply_gyro_bias(&corrected, &s_bias);
            mahony_update(&s_mahony, &corrected);

            float roll = 0.0f;
            float pitch = 0.0f;
            float yaw = 0.0f;
            mahony_get_angles(&s_mahony, &roll, &pitch, &yaw);

            const float roll_deg = roll * GS_IMU_RAD_TO_DEG - s_neutral_roll;
            const float pitch_deg = pitch * GS_IMU_RAD_TO_DEG - s_neutral_pitch;
            const int8_t dir = tilt_update(&s_tilt, roll_deg, pitch_deg);
            atomic_store(&s_direction, dir);
        }

        if (xTaskDelayUntil(&last, period) == pdFALSE) last = xTaskGetTickCount();
    }
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t imu_port_init(void)
{
    if (s_initialized) return atomic_load(&s_sensor_ready) ? ESP_OK : ESP_ERR_NOT_FOUND;
    s_initialized = true;

    atomic_store(&s_sensor_ready, false);
    atomic_store(&s_task_started, false);
    atomic_store(&s_calib_request, false);
    atomic_store(&s_calib_result_ready, false);
    atomic_store(&s_direction, IMU_DIR_NONE);
    atomic_store(&s_progress, 0u);

    memset(&s_bias, 0, sizeof(s_bias));
    s_bias.grav_axis = -1;
    s_bias.grav_sign = 1;
    s_neutral_roll = 0.0f;
    s_neutral_pitch = 0.0f;
    s_calibrating = false;

    mahony_init(&s_mahony, 2.0f, 0.0f, 0.01f);
    tilt_init(&s_tilt, 12.0f, 6.0f, 3);
    tilt_set_axis(&s_tilt, IMU_PORT_AXIS_SWAP_XY != 0, IMU_PORT_AXIS_INVERT_ROLL != 0,
                  IMU_PORT_AXIS_INVERT_PITCH != 0);

    i2c_master_bus_config_t bus_cfg;
    memset(&bus_cfg, 0, sizeof(bus_cfg));
    bus_cfg.i2c_port = GS_IMU_I2C_PORT;
    bus_cfg.sda_io_num = (gpio_num_t)GS_IMU_PIN_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)GS_IMU_PIN_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus(I2C%d): %s", GS_IMU_I2C_PORT,
                 esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg;
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = GS_IMU_I2C_ADDR;
    dev_cfg.scl_speed_hz = GS_IMU_I2C_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X): %s", GS_IMU_I2C_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        err = imu_probe_once();
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "LSM6DS3TR-C on I2C%d SDA=%d SCL=%d @0x%02X, 104 Hz, +-2 g, +-500 dps",
                 GS_IMU_I2C_PORT, GS_IMU_PIN_SDA, GS_IMU_PIN_SCL, GS_IMU_I2C_ADDR);
    } else {
        ESP_LOGE(TAG, "LSM6DS3TR-C init failed: %s (task will re-probe)",
                 esp_err_to_name(err));
    }
    return err;
}

void imu_port_apply_saved(const board_imu_calib_t *calib)
{
    if (!calib || calib->version != GS_IMU_CALIB_VERSION || !calib->calibrated) return;

    memset(&s_bias, 0, sizeof(s_bias));
    s_bias.gx = calib->gyro_bias[0];
    s_bias.gy = calib->gyro_bias[1];
    s_bias.gz = calib->gyro_bias[2];
    s_bias.valid = true;
    s_bias.grav_axis = -1;
    s_bias.grav_sign = 1;

    s_neutral_roll = calib->neutral_roll;
    s_neutral_pitch = calib->neutral_pitch;
    tilt_reset(&s_tilt);

    ESP_LOGI(TAG,
             "saved IMU calibration applied: bias %d/%d/%d mdps, neutral %d/%d cdeg",
             (int)(s_bias.gx * 1000.0f), (int)(s_bias.gy * 1000.0f),
             (int)(s_bias.gz * 1000.0f), (int)(s_neutral_roll * 100.0f),
             (int)(s_neutral_pitch * 100.0f));
}

esp_err_t imu_port_start(void)
{
    if (atomic_load(&s_task_started)) return ESP_OK;
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    if (xTaskCreate(imu_task, "gs_imu", GS_IMU_TASK_STACK, NULL, GS_IMU_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "IMU task creation failed");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_task_started, true);
    return ESP_OK;
}

void imu_port_request_calibration(void)
{
    if (!atomic_load(&s_task_started)) {
        ESP_LOGW(TAG, "calibration requested but the IMU task is not running");
        memset(&s_calib_result, 0, sizeof(s_calib_result));
        s_calib_result.ok = false;
        atomic_store_explicit(&s_calib_result_ready, true, memory_order_release);
        return;
    }
    atomic_store_explicit(&s_calib_request, true, memory_order_release);
}

uint8_t imu_port_calib_progress(void)
{
    const unsigned pct = atomic_load(&s_progress);
    return (uint8_t)(pct > 100u ? 100u : pct);
}

bool imu_port_take_calib_result(imu_port_calib_result_t *out)
{
    if (!out) return false;
    if (!atomic_exchange_explicit(&s_calib_result_ready, false, memory_order_acquire)) {
        return false;
    }
    *out = s_calib_result;
    return true;
}

int8_t imu_port_direction(void)
{
    return (int8_t)atomic_load(&s_direction);
}

bool imu_port_ready(void)
{
    return atomic_load(&s_sensor_ready);
}