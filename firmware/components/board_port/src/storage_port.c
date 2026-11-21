/*
 * GYRO SNAKE — NVS persistence.
 *
 * Two keys in namespace "gyrosnake":
 *   hiscore   uint16   the all-time high score (0..9999)
 *   imu_calib blob     board_imu_calib_t (version, flag, gyro bias, neutral)
 *
 * Failures are logged and never block the game: a lost read means a high
 * score of 0 (or "not calibrated"), a lost write costs one record.
 */
#include "board_port.h"

#include <math.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gs_store";

#define GS_NVS_NAMESPACE "gyrosnake"
#define GS_NVS_KEY_HISCORE "hiscore"
#define GS_NVS_KEY_IMU_CALIB "imu_calib"
#define GS_SCORE_MAX 9999u

/* Reject a blob that would obviously misbehave as a bias / zero pose. */
static bool calib_values_plausible(const board_imu_calib_t *c)
{
    if (c->version != GS_IMU_CALIB_VERSION) return false;
    if (!c->calibrated) return false;
    if (!isfinite(c->neutral_roll) || !isfinite(c->neutral_pitch)) return false;
    if (fabsf(c->neutral_roll) > 180.0f) return false;
    if (fabsf(c->neutral_pitch) > 90.5f) return false;
    for (int i = 0; i < 3; i++) {
        if (!isfinite(c->gyro_bias[i])) return false;
        if (fabsf(c->gyro_bias[i]) > 100.0f) return false;
    }
    return true;
}

esp_err_t storage_port_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NVS ready (namespace \"%s\")", GS_NVS_NAMESPACE);
    }
    return err;
}

uint16_t storage_port_load_hiscore(void)
{
    nvs_handle_t handle;
    uint16_t value = 0;

    esp_err_t err = nvs_open(GS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u16(handle, GS_NVS_KEY_HISCORE, &value);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            value = 0;
            err = ESP_OK;
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "high score read failed (%s), using 0", esp_err_to_name(err));
        return 0;
    }
    if (value > GS_SCORE_MAX) value = (uint16_t)GS_SCORE_MAX;
    return value;
}

esp_err_t storage_port_save_hiscore(uint16_t high_score)
{
    if (high_score > GS_SCORE_MAX) high_score = (uint16_t)GS_SCORE_MAX;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(GS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u16(handle, GS_NVS_KEY_HISCORE, high_score);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "high score write failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool storage_port_load_imu_calib(board_imu_calib_t *out)
{
    if (!out) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(GS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Not an error: first boot has no namespace yet. */
        return false;
    }

    board_imu_calib_t tmp;
    size_t len = sizeof(tmp);
    err = nvs_get_blob(handle, GS_NVS_KEY_IMU_CALIB, &tmp, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK || len != sizeof(tmp)) {
        ESP_LOGW(TAG, "IMU calibration blob unreadable (%s, %u B)",
                 esp_err_to_name(err), (unsigned)len);
        return false;
    }
    if (!calib_values_plausible(&tmp)) {
        ESP_LOGW(TAG, "IMU calibration blob rejected (version %u, calibrated %u)",
                 (unsigned)tmp.version, (unsigned)tmp.calibrated);
        return false;
    }

    *out = tmp;
    ESP_LOGI(TAG, "IMU calibration loaded (v%u): bias %d/%d/%d mdps neutral %d/%d cdeg",
             (unsigned)tmp.version,
             (int)(tmp.gyro_bias[0] * 1000.0f),
             (int)(tmp.gyro_bias[1] * 1000.0f),
             (int)(tmp.gyro_bias[2] * 1000.0f),
             (int)(tmp.neutral_roll * 100.0f),
             (int)(tmp.neutral_pitch * 100.0f));
    return true;
}

esp_err_t storage_port_save_imu_calib(const board_imu_calib_t *calib)
{
    if (!calib) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(GS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, GS_NVS_KEY_IMU_CALIB, calib, sizeof(*calib));
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU calibration write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "IMU calibration saved (v%u): bias %d/%d/%d mdps neutral %d/%d cdeg",
                 (unsigned)calib->version,
                 (int)(calib->gyro_bias[0] * 1000.0f),
                 (int)(calib->gyro_bias[1] * 1000.0f),
                 (int)(calib->gyro_bias[2] * 1000.0f),
                 (int)(calib->neutral_roll * 100.0f),
                 (int)(calib->neutral_pitch * 100.0f));
    }
    return err;
}