/*
 * GYRO SNAKE — board port public interface.
 *
 * Everything between the reTerminal Sticky hardware and the frozen game core
 * (snake.h):
 *
 *   input_port_*    BSP key callback -> queue -> SNAKE_BTN_* activity mask
 *   storage_port_*  NVS: high score and the IMU calibration blob
 *   audio_port_*    non-blocking LEDC buzzer on GPIO48
 *   display_port_*  the only owner of bsp_epd_* (dedicated task)
 *
 * Delivery contract: the display task never touches snake_t; it receives a
 * frame descriptor and reports success plus the measured refresh time back to
 * the game task, which is the only caller of snake_mark_refreshed().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "snake.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Observation window after a refresh returns. The panel refresh itself is
 * much longer and already provides most of the player's reaction time. */
#define GS_THINK_MS 120u

/* Port-side residual-image guard on top of the core's own 20-fast counter. */
#define GS_FAST_PER_FULL 20u

/* Version of the board_imu_calib_t NVS blob. */
#define GS_IMU_CALIB_VERSION 1u

/* Persisted IMU calibration: gyro bias, neutral pose, flag and version.
 * Plain data only, so storage_port.c does not need imu_core.h. */
typedef struct {
    uint32_t version;        /* GS_IMU_CALIB_VERSION                     */
    uint8_t calibrated;      /* 1 = a completed calibration is stored    */
    uint8_t reserved[3];
    float gyro_bias[3];      /* dps                                      */
    float neutral_roll;      /* degrees, Mahony roll at the neutral pose */
    float neutral_pitch;     /* degrees                                  */
} board_imu_calib_t;

/* ---- Keys ---------------------------------------------------------------- */

/* Create the event queue and start the BSP polling task with our callback.
 * The callback only enqueues, never blocks. */
esp_err_t input_port_init(void);

/* Drain the queued press edges since the previous call and return the
 * SNAKE_BTN_* activity mask. snake.h defines the mask as press edges only
 * for OK/UP (DOWN would be level-sensitive, but this game does not use it). */
uint8_t input_port_poll(void);

/* ---- Persistence (NVS) --------------------------------------------------- */

/* nvs_flash_init() with one erase-and-retry on a stale/complete partition. */
esp_err_t storage_port_init(void);

/* 0 when the key is absent or NVS is unavailable. */
uint16_t storage_port_load_hiscore(void);
esp_err_t storage_port_save_hiscore(uint16_t high_score);

/* True only for a complete, same-version blob with plausible values. */
bool storage_port_load_imu_calib(board_imu_calib_t *out);
esp_err_t storage_port_save_imu_calib(const board_imu_calib_t *calib);

/* ---- Buzzer -------------------------------------------------------------- */

/* Configure LEDC on GPIO48 and start the playback task. */
esp_err_t audio_port_init(void);

/* Queue one short original tone pattern. Never blocks the game task; events
 * beyond the queue depth are dropped and never affect game state. */
void audio_port_play(snake_sound_t sound);

/* ---- Display ------------------------------------------------------------- */

/* One frame handed from the game task to the display task. */
typedef struct {
    snake_frame_t kind;     /* MONO (48,000 B) or GRAY4 (96,000 B page) */
    const uint8_t *data;    /* read-only until display_port_refresh returns */
    bool full;              /* MONO: request a full refresh */
} board_frame_t;

/* Create the command/result queues and the display task; the task itself
 * performs bsp_epd_init() so bsp_epd_* has exactly one caller. */
esp_err_t display_port_init(void);

/* Hand the caller-owned 48,000 B mono shadow to the display task. The shadow
 * mirrors what the panel shows and is passed as `prev`. Call after
 * display_port_init() and before the first refresh. */
esp_err_t display_port_attach(uint8_t *shadow);

/* Submit a frame and block until the display task finished the BSP call.
 * *out_ms receives the measured wall-clock time, *out_full the refresh mode
 * actually used (true for every gray4 push and whenever the shadow was
 * invalid so prev had to be NULL). Call snake_mark_refreshed(g, *out_full)
 * only after this returns ESP_OK. */
esp_err_t display_port_refresh(const board_frame_t *frame, uint32_t *out_ms,
                               bool *out_full);

/* Tell the display task that the panel content is unknown (used after a failed
 * refresh so the retry passes prev = NULL, as bsp_epd.h requires). */
void display_port_forget_panel(void);

/* Draw the live tilt ball in the bottom-right corner of a MONO frame.
 * roll_cdeg/pitch_cdeg are relative to the current zero; dir is IMU_DIR_*.
 * The buffer stays caller-owned; this only writes pixels. */
void board_draw_tilt_ball(uint8_t *mono, int roll_cdeg, int pitch_cdeg, int dir);

#ifdef __cplusplus
}
#endif