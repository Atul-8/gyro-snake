/*
 * GYRO SNAKE — firmware entry point.
 *
 * Boot order:
 *   1. GPIO45 (HOLD) / GPIO46 (LOCK) high - the board drops off battery
 *      power without it, so this is the very first thing.
 *   2. NVS (high score + IMU calibration blob)
 *   3. panel (display_port task performs bsp_epd_init itself)
 *   4. keys (BSP polling task -> event queue)
 *   5. LEDC buzzer on GPIO48
 *   6. PSRAM framebuffers: mono 48,000 B + gray4 192,000 B + mono shadow
 *   7. IMU port: LSM6DS3TR-C on I2C1 + 100 Hz task
 *   8. game task: the only caller of snake_* and snake_mark_refreshed()
 *
 * The game task is paced by the panel: it pushes one dirty frame, waits for
 * the display task to finish, waits GS_THINK_MS, samples the key edges and
 * the latest tilt direction, then advances the core by exactly one tick.
 * Serial output and the panel carry no text other than GYRO SNAKE.
 */
#include "board_port.h"
#include "console.h"
#include "imu_port.h"
#include "snake.h"
#include "touch_port.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "gyro_snake";

#define GS_GAME_SEED 0x47534E4Bu /* "GSNK" */
#define GS_PIN_HOLD 45
#define GS_PIN_LOCK 46
#define GS_GAME_TASK_STACK 8192
#define GS_GAME_TASK_PRIO 4

/* Static: 8 KiB, never on the stack (snake.h §3). Only the game task uses it. */
static snake_t s_game;

typedef struct {
    snake_buffers_t buffers;
    uint16_t high_score;
    bool imu_calibrated; /* a valid blob was loaded from NVS */
} game_ctx_t;

static game_ctx_t s_ctx;

static void reboot_soon(const char *why)
{
    ESP_LOGE(TAG, "%s - restarting in 3 s", why);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* GPIO45 (HOLD) and GPIO46 (LOCK) must be driven high before anything else:
 * without them the board powers off on battery. */
static esp_err_t power_hold_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GS_PIN_HOLD) | (1ULL << GS_PIN_LOCK),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_set_level(GS_PIN_HOLD, 1);
    if (err == ESP_OK) err = gpio_set_level(GS_PIN_LOCK, 1);

    const int hold = gpio_get_level(GS_PIN_HOLD);
    const int lock = gpio_get_level(GS_PIN_LOCK);
    if (err == ESP_OK && hold == 1 && lock == 1) {
        ESP_LOGI(TAG, "power hold high: GPIO%d(HOLD)=%d GPIO%d(LOCK)=%d", GS_PIN_HOLD, hold,
                 GS_PIN_LOCK, lock);
    } else {
        ESP_LOGW(TAG,
                 "power hold set high but readback GPIO%d=%d GPIO%d=%d; USB is fine, verify on battery",
                 GS_PIN_HOLD, hold, GS_PIN_LOCK, lock);
    }
    return err;
}

static uint8_t *alloc_psram(size_t bytes, const char *what)
{
    uint8_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "PSRAM alloc %s (%u B) failed, free=%u B", what, (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    return p;
}

/* The game task is the only caller of snake_take_sound (snake_* contract). */
static void drain_sounds(void)
{
    snake_sound_t sound;
    while ((sound = snake_take_sound(&s_game)) != SNAKE_SOUND_NONE) {
        audio_port_play(sound);
    }
}

/* One iteration = one displayed frame:
 *   push the dirty frame -> wait for the refresh -> observation window ->
 *   sample key edges and the latest tilt -> tick the core -> queue sounds.
 * The tick period is therefore (measured refresh + GS_THINK_MS). */
static void game_task(void *arg)
{
    const game_ctx_t *ctx = (const game_ctx_t *)arg;

    if (!snake_init(&s_game, &ctx->buffers, GS_GAME_SEED)) {
        reboot_soon("snake_init rejected the framebuffers");
    }
    snake_set_high_score(&s_game, ctx->high_score);
    ESP_LOGI(TAG, "high score from NVS: %u", (unsigned)ctx->high_score);

    if (ctx->imu_calibrated) {
        /* The saved pose is only a starting point: every start still runs a
         * fresh 4 s zeroing so the neutral pose matches how the user holds
         * the device now. */
        ESP_LOGI(TAG, "saved IMU calibration loaded; every start re-zeroes");
    }

    uint32_t fast_run = 0;
    bool hiscore_saved = false;
    bool calib_pending = false;
    uint8_t calib_shown = 0;
    bool play_grace = false;

    for (;;) {
        const snake_status_t *status = snake_status(&s_game);
        if (!status) reboot_soon("core status unavailable");

        /* Entering CALIBRATING (from TITLE, PAUSE+UP or GAME_OVER+UP) is the
         * signal to ask the IMU task for a fresh 400-sample window. */
        if (status->state == SNAKE_STATE_CALIBRATING) {
            if (!calib_pending) {
                imu_port_request_calibration();
                calib_pending = true;
                calib_shown = 0;
            }
        } else {
            calib_pending = false;
        }

        imu_port_calib_result_t cal;
        if (imu_port_take_calib_result(&cal)) {
            /* A result can only follow a request, but drop a late one if the
             * core has already left CALIBRATING (e.g. a rebootless retry). */
            if (status->state == SNAKE_STATE_CALIBRATING) {
                if (cal.ok) {
                    ESP_LOGI(TAG,
                             "IMU calibration accepted: bias %d/%d/%d mdps neutral %d/%d cdeg",
                             (int)(cal.bias.gx * 1000.0f), (int)(cal.bias.gy * 1000.0f),
                             (int)(cal.bias.gz * 1000.0f), (int)(cal.neutral_roll * 100.0f),
                             (int)(cal.neutral_pitch * 100.0f));
                } else {
                    ESP_LOGW(TAG, "IMU calibration failed; returning to the title card");
                }
                snake_calibration_done(&s_game, cal.ok);
                if (cal.ok) {
                    /* Let the player see the board and tilt before the snake
                     * takes its first step. */
                    play_grace = true;
                }
                drain_sounds();
                calib_pending = false;
            } else {
                ESP_LOGW(TAG, "dropping a stale IMU calibration result");
            }
        } else if (status->state == SNAKE_STATE_CALIBRATING) {
            const uint8_t pct = imu_port_calib_progress();
            if (pct != calib_shown) {
                snake_calibration_progress(&s_game, pct);
                calib_shown = pct;
            }
        }

        if (snake_dirty(&s_game)) {
            status = snake_status(&s_game);
            const snake_frame_t kind = status->frame;
            const uint8_t *data = (kind == SNAKE_FRAME_GRAY4) ? snake_gray4(&s_game)
                                                             : snake_mono(&s_game);
            if (!data) reboot_soon("core returned a NULL framebuffer");

            /* The tilt ball is an instrument on top of the composed play
             * frame. Every play frame starts from a cleared buffer, so the
             * ball never leaves a trail; drawing it here keeps it in step with
             * what the panel is about to show. Cards are gray4 and stay clean. */
            if (kind == SNAKE_FRAME_MONO) {
                int roll_cdeg = 0;
                int pitch_cdeg = 0;
                imu_port_get_tilt(&roll_cdeg, &pitch_cdeg);
                board_draw_tilt_ball(ctx->buffers.mono, roll_cdeg, pitch_cdeg,
                                     imu_port_direction());
            }

            /* gray4 cards are always full; mono is full on the core's request
             * and after GS_FAST_PER_FULL consecutive fast mono frames. */
            const bool full = (kind == SNAKE_FRAME_GRAY4) || snake_needs_full_refresh(&s_game) ||
                              (fast_run >= GS_FAST_PER_FULL);

            board_frame_t frame = {
                .kind = kind,
                .data = data,
                .full = full,
            };

            uint32_t ms = 0;
            bool was_full = false;
            esp_err_t err = display_port_refresh(&frame, &ms, &was_full);
            if (err != ESP_OK) {
                /* bsp_epd.h: after a failed update pass prev = NULL and full. */
                ESP_LOGW(TAG, "refresh failed (%s), retrying full", esp_err_to_name(err));
                display_port_forget_panel();
                frame.full = true;
                err = display_port_refresh(&frame, &ms, &was_full);
            }
            while (err != ESP_OK) {
                ESP_LOGE(TAG, "refresh still failing (%s); retrying in 1 s",
                         esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1000));
                display_port_forget_panel();
                frame.full = true;
                err = display_port_refresh(&frame, &ms, &was_full);
            }

            /* The frame reached the panel: only now may the core advance. */
            snake_mark_refreshed(&s_game, was_full);

            if (kind == SNAKE_FRAME_GRAY4 || was_full) {
                fast_run = 0;
            } else if (fast_run < GS_FAST_PER_FULL) {
                fast_run++;
            }

            status = snake_status(&s_game);
            ESP_LOGI(TAG, "%s %s %u ms | state=%s score=%u high=%u heap=%u psram=%u",
                     kind == SNAKE_FRAME_GRAY4 ? "gray4" : "mono",
                     was_full ? "full" : "fast", (unsigned)ms,
                     snake_state_name(status->state), (unsigned)status->score,
                     (unsigned)status->high_score,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }

        /* Observation window: the panel is stable from the moment the refresh
         * returned. Key edges seen during the refresh or the window stay in
         * the queue and land in this tick's mask. */
        if (play_grace) {
            ESP_LOGI(TAG, "ready window: 3 s to tilt before the first step");
            vTaskDelay(pdMS_TO_TICKS(3000));
            play_grace = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(GS_THINK_MS));
        }

        uint8_t buttons = input_port_poll();
        int tap_x = 0;
        int tap_y = 0;
        if (touch_port_take_press_xy(&tap_x, &tap_y)) {
            status = snake_status(&s_game);
            /* The tilt ball lives in the bottom-right corner. Accept a corner
             * tap as the zero gesture: the GT911 axis transform is provisional,
             * and a corner stays a corner under any axis order or flip. The
             * touch driver logs the raw and mapped point of every tap, so the
             * map can be pinned down and this test tightened later. */
            const bool in_corner =
                (tap_x >= 380 || tap_x <= 100) && (tap_y >= 700 || tap_y <= 100);
            const bool playing =
                status && (status->state == SNAKE_STATE_PLAY ||
                           status->state == SNAKE_STATE_PAUSE);
            if (playing && in_corner) {
                imu_port_request_rezero();
                ESP_LOGI(TAG, "ball tap (%d,%d): set current pose as tilt zero", tap_x,
                         tap_y);
            } else {
                buttons |= SNAKE_BTN_OK;
            }
        }
        status = snake_status(&s_game);
        bool started = false;
        if (status && (buttons & SNAKE_BTN_DOWN)) {
            /* Volume-down key: the current pose becomes the tilt zero at once,
             * in every state. Hold the device the way you play, press DOWN,
             * then tilt: up/down/left/right are measured from that pose. */
            imu_port_request_rezero();
            buttons &= (uint8_t)~SNAKE_BTN_DOWN;
            ESP_LOGI(TAG, "DOWN: set current pose as tilt zero");
        }
        if (status && status->state == SNAKE_STATE_TITLE && (buttons & SNAKE_BTN_OK)) {
            /* Every start runs a fresh zeroing window; a saved NVS pose is a
             * fallback for the IMU task, never a reason to skip calibration. */
            snake_begin_calibration(&s_game);
            buttons &= (uint8_t)~SNAKE_BTN_OK;
            started = true;
            ESP_LOGI(TAG, "start requested (tap/OK) -> calibration");
        }
        /* Real-time following: sample the CURRENT tilt sector, not a latch.
         * The IMU task re-evaluates the 45-degree sector at 100 Hz, so the
         * snake turns as soon as the hand moves into another sector, even
         * without passing through the center pose. NONE keeps the direction. */
        const int8_t tilt = imu_port_direction();
        if (!started) {
            /* snake_begin_calibration() already composed a dirty card; ticking
             * now would overwrite it (the core requires !dirty before tick). */
            snake_tick(&s_game, buttons, tilt);
        }
        drain_sounds();

        /* Persist once per run, when the GAME_OVER card appears. */
        status = snake_status(&s_game);
        if (status && status->state == SNAKE_STATE_GAME_OVER) {
            if (status->record_beaten && !hiscore_saved) {
                const esp_err_t err = storage_port_save_hiscore(status->high_score);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "high score saved: %u", (unsigned)status->high_score);
                }
                hiscore_saved = true; /* one attempt per run, never a write storm */
            }
        } else {
            hiscore_saved = false;
        }
    }
}

void app_main(void)
{
    /* 1. Battery rail, before logging or any driver. */
    const esp_err_t power_err = power_hold_init();
    if (power_err != ESP_OK) {
        ESP_LOGE(TAG, "power hold error: %s", esp_err_to_name(power_err));
    }

    ESP_LOGI(TAG, "GYRO SNAKE - starting (ESP-IDF %s)", esp_get_idf_version());

    /* 2. NVS: high score and the IMU calibration blob. */
    esp_err_t err = storage_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, persistence disabled (%s)", esp_err_to_name(err));
    }

    /* 3. Panel transport: the display task is the only bsp_epd_* caller. */
    err = display_port_init();
    if (err != ESP_OK) {
        reboot_soon("display port init failed");
    }

    /* 4. Keys: BSP polling task with a non-blocking callback. */
    err = input_port_init();
    if (err != ESP_OK) {
        reboot_soon("key init failed");
    }

    /* 4.5 Touch: GT911 on I2C0. A tap is the OK button; a failure is
     *      non-fatal because AI/OK still works. */
    err = touch_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable (%s); AI/OK still starts",
                 esp_err_to_name(err));
    }

    /* 5. Buzzer. Non-critical: play muted if LEDC cannot start. */
    err = audio_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "buzzer disabled: %s", esp_err_to_name(err));
    }

    /* 6. Framebuffers: 288,000 B from PSRAM, allocated once. mono is the
     *    core's working frame; gray4 holds the core's two 96,000 B pages;
     *    shadow mirrors the panel and is the mono `prev`. */
    uint8_t *mono = alloc_psram(SNAKE_MONO_BYTES, "mono");
    uint8_t *gray4 = alloc_psram(SNAKE_GRAY4_BYTES, "gray4 pages");
    uint8_t *shadow = alloc_psram(SNAKE_MONO_BYTES, "shadow");
    if (!mono || !gray4 || !shadow) {
        reboot_soon("PSRAM framebuffer allocation failed");
    }
    ESP_LOGI(TAG, "framebuffers: %u B mono + %u B gray4 + %u B shadow",
             (unsigned)SNAKE_MONO_BYTES, (unsigned)SNAKE_GRAY4_BYTES,
             (unsigned)SNAKE_MONO_BYTES);

    err = display_port_attach(shadow);
    if (err != ESP_OK) {
        reboot_soon("display shadow attach failed");
    }

    /* 7. IMU: bus, sensor, saved pose from NVS, 100 Hz task. */
    board_imu_calib_t calib = {0};
    const bool have_calib = storage_port_load_imu_calib(&calib);

    err = imu_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init incomplete (%s); the game will start without tilt",
                 esp_err_to_name(err));
    }
    if (have_calib) {
        imu_port_apply_saved(&calib);
    }
    err = imu_port_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU task not started: %s", esp_err_to_name(err));
    }

    /* 7b. Serial console: $-commands retune the IMU without reflashing. */
    err = console_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console not available: %s", esp_err_to_name(err));
    }

    /* 8. Game task: buffers, persisted high score, saved-calibration flag. */
    s_ctx.buffers.mono = mono;
    s_ctx.buffers.gray4 = gray4;
    s_ctx.buffers.gray4_page = 0; /* blank panel after boot */
    s_ctx.high_score = storage_port_load_hiscore();
    s_ctx.imu_calibrated = have_calib;

    ESP_LOGI(TAG, "keys: OK(4)=start/pause DOWN(6)=set zero UP(5)=recalibrate (pause/over)");
    ESP_LOGI(TAG, "tilt: 45-degree sectors, real-time follow; deadzone 8 deg, boundary hysteresis 4 deg, 2 stable samples; axis map %d/%d/%d",
             IMU_PORT_AXIS_SWAP_XY, IMU_PORT_AXIS_INVERT_ROLL, IMU_PORT_AXIS_INVERT_PITCH);

    if (xTaskCreate(game_task, "gs_game", GS_GAME_TASK_STACK, &s_ctx, GS_GAME_TASK_PRIO, NULL) !=
        pdPASS) {
        reboot_soon("game task creation failed");
    }
}