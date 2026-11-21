/*
 * GYRO SNAKE — buzzer.
 *
 * The BSP has no audio API, so the port owns a LEDC channel on GPIO48:
 * 2-4 kHz square wave, 30 % duty, at most 120 ms per game event, at least
 * 80 ms between events.
 *
 * Every pattern below is original to this project: short two-to-four note
 * blips, no quotation of any existing game melody. The game core only emits
 * snake_sound_t events and never touches LEDC.
 *
 * audio_port_play() is called from the game task and only enqueues; the
 * playback task owns the delays and the LEDC calls, so a long tone can never
 * delay a frame.
 */
#include "board_port.h"

#include <stddef.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "gs_audio";

#define GS_BUZZER_GPIO 48
#define GS_LEDC_MODE LEDC_LOW_SPEED_MODE
#define GS_LEDC_TIMER LEDC_TIMER_0
#define GS_LEDC_CHANNEL LEDC_CHANNEL_0
#define GS_LEDC_RES LEDC_TIMER_10_BIT
#define GS_LEDC_DUTY_MAX (1u << 10)
#define GS_LEDC_DUTY_ON (GS_LEDC_DUTY_MAX * 30u / 100u)
#define GS_NOTE_GAP_MS 5u
#define GS_EVENT_GAP_MS 80u
#define GS_SOUND_QUEUE_DEPTH 8

typedef struct {
    uint16_t hz;
    uint16_t ms;
} note_t;

typedef struct {
    const note_t *notes;
    uint8_t count;
} tune_t;

/* Longest pattern: death = 110 ms, win = 90 ms. */
static const note_t k_eat[] = {{988, 20}, {1319, 25}};
static const note_t k_turn[] = {{740, 15}};
static const note_t k_death[] = {{330, 35}, {262, 35}, {196, 40}};
static const note_t k_start[] = {{523, 25}, {659, 25}, {784, 30}};
static const note_t k_calib_done[] = {{659, 20}, {880, 20}, {1175, 30}};
static const note_t k_calib_fail[] = {{220, 40}, {175, 45}};
static const note_t k_game_over[] = {{392, 30}, {330, 30}, {262, 35}};
static const note_t k_win[] = {{523, 20}, {659, 20}, {784, 20}, {1047, 30}};

static const tune_t k_tunes[] = {
    [SNAKE_SOUND_NONE] = {NULL, 0},
    [SNAKE_SOUND_EAT] = {k_eat, 2},
    [SNAKE_SOUND_TURN] = {k_turn, 1},
    [SNAKE_SOUND_DEATH] = {k_death, 3},
    [SNAKE_SOUND_START] = {k_start, 3},
    [SNAKE_SOUND_CALIB_DONE] = {k_calib_done, 3},
    [SNAKE_SOUND_CALIB_FAIL] = {k_calib_fail, 2},
    [SNAKE_SOUND_GAME_OVER] = {k_game_over, 3},
    [SNAKE_SOUND_WIN] = {k_win, 4},
};

static QueueHandle_t s_queue;

static const tune_t *tune_for(snake_sound_t sound)
{
    if ((unsigned)sound >= sizeof(k_tunes) / sizeof(k_tunes[0])) return NULL;
    const tune_t *tune = &k_tunes[sound];
    return tune->count ? tune : NULL;
}

static void buzzer_level(uint32_t duty)
{
    (void)ledc_set_duty(GS_LEDC_MODE, GS_LEDC_CHANNEL, duty);
    (void)ledc_update_duty(GS_LEDC_MODE, GS_LEDC_CHANNEL);
}

static void play_tune(const tune_t *tune)
{
    for (uint8_t i = 0; i < tune->count; i++) {
        if (ledc_set_freq(GS_LEDC_MODE, GS_LEDC_TIMER, tune->notes[i].hz) == ESP_OK) {
            buzzer_level(GS_LEDC_DUTY_ON);
        }
        vTaskDelay(pdMS_TO_TICKS(tune->notes[i].ms));
        buzzer_level(0);
        if (i + 1 < tune->count) {
            vTaskDelay(pdMS_TO_TICKS(GS_NOTE_GAP_MS));
        }
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        snake_sound_t sound;
        if (xQueueReceive(s_queue, &sound, portMAX_DELAY) != pdTRUE) continue;
        const tune_t *tune = tune_for(sound);
        if (!tune) continue;
        play_tune(tune);
        vTaskDelay(pdMS_TO_TICKS(GS_EVENT_GAP_MS));
    }
}

esp_err_t audio_port_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = GS_LEDC_MODE,
        .timer_num = GS_LEDC_TIMER,
        .duty_resolution = GS_LEDC_RES,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channel = {
        .gpio_num = GS_BUZZER_GPIO,
        .speed_mode = GS_LEDC_MODE,
        .channel = GS_LEDC_CHANNEL,
        .timer_sel = GS_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        return err;
    }

    s_queue = xQueueCreate(GS_SOUND_QUEUE_DEPTH, sizeof(snake_sound_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audio_task, "gs_audio", 3072, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "buzzer ready: LEDC GPIO%d, 30%% duty, <=120 ms per event",
             GS_BUZZER_GPIO);
    return ESP_OK;
}

void audio_port_play(snake_sound_t sound)
{
    if (sound == SNAKE_SOUND_NONE || !s_queue) return;
    /* Zero timeout: a full queue drops the newest event, never the frame. */
    (void)xQueueSend(s_queue, &sound, 0);
}