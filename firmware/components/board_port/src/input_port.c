/*
 * GYRO SNAKE — keys.
 *
 * bsp_button_init() runs its own 20 ms poll / 30 ms debounce task and invokes
 * the callback from that task. The callback must stay non-blocking, so it only
 * pushes one small event into a queue. The game task drains the queue at the
 * end of each observation window and converts the events into the
 * SNAKE_BTN_* activity mask.
 *
 * snake.h defines that mask as *press edges* since the previous tick (only
 * DOWN would be level-sensitive, and this game does not use DOWN), so held
 * levels are deliberately not added here. A press during a long e-paper
 * refresh is latched in the queue and delivered exactly once on the next tick.
 *
 * Mapping: AI (GPIO4, the key labelled OK/Power) = OK, UP (GPIO5) = UP.
 */
#include "board_port.h"

#include <stdatomic.h>

#include "bsp_button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "gs_input";

#define GS_INPUT_QUEUE_DEPTH 16

typedef struct {
    uint8_t id;
    bool pressed;
} button_event_t;

static QueueHandle_t s_events;
static atomic_uint_fast32_t s_dropped;

/* BSP button task context. No logging, no delays, no BSP calls. */
static void button_callback(bsp_button_id_t id, bool pressed, void *arg)
{
    (void)arg;
    if (!s_events) return;
    const button_event_t ev = {
        .id = (uint8_t)id,
        .pressed = pressed,
    };
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
    }
}

static uint8_t button_bit(bsp_button_id_t id)
{
    switch (id) {
    case BSP_BUTTON_AI:
        return SNAKE_BTN_OK;
    case BSP_BUTTON_UP:
        return SNAKE_BTN_UP;
    default:
        return 0; /* DOWN is reserved by the requirements and unused. */
    }
}

esp_err_t input_port_init(void)
{
    s_events = xQueueCreate(GS_INPUT_QUEUE_DEPTH, sizeof(button_event_t));
    if (!s_events) return ESP_ERR_NO_MEM;
    atomic_store(&s_dropped, 0);

    /* The callback may fire before bsp_button_init() returns, so the queue
     * must exist first. */
    const esp_err_t err = bsp_button_init(button_callback, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_button_init: %s", esp_err_to_name(err));
        vQueueDelete(s_events);
        s_events = NULL;
        return err;
    }
    ESP_LOGI(TAG, "keys ready: AI(4)=OK UP(5)=recalibrate, press edges queued");
    return ESP_OK;
}

uint8_t input_port_poll(void)
{
    uint8_t mask = 0;
    if (s_events) {
        button_event_t ev;
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            if (ev.pressed) mask |= button_bit((bsp_button_id_t)ev.id);
        }
    }

    const uint32_t dropped = (uint32_t)atomic_exchange(&s_dropped, 0);
    if (dropped) {
        ESP_LOGW(TAG, "dropped %u key events", (unsigned)dropped);
    }
    return mask;
}