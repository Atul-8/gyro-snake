/*
 * GYRO SNAKE — display port.
 *
 * One dedicated task is the only caller of bsp_epd_* (including the panel
 * init, which the task performs for its command). The game task hands over a
 * frame descriptor and blocks on the result; the display task performs the
 * blocking refresh, keeps the mono shadow that is passed as `prev`, and
 * reports the measured wall-clock time. Only the game task calls
 * snake_mark_refreshed().
 *
 * Mono shadow: the shadow is the panel content. It is updated only after a
 * successful mono refresh; a gray4 push (or a failed update) invalidates it,
 * so the next mono refresh passes prev = NULL and is full, matching
 * bsp_epd.h.
 *
 * Gray4: the core composes into its own staging page and snake_gray4()
 * returns exactly one 96,000 B page. The display port never sees the pair and
 * does not do any page ping-pong itself.
 */
#include "board_port.h"

#include <string.h>

#include "bsp_epd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "gs_display";

#define GS_DISPLAY_STACK 8192
#define GS_DISPLAY_PRIO 5

/* The core and the BSP describe the same glass. */
_Static_assert(BSP_EPD_WIDTH == SNAKE_SCREEN_W, "panel width mismatch");
_Static_assert(BSP_EPD_HEIGHT == SNAKE_SCREEN_H, "panel height mismatch");
_Static_assert(BSP_EPD_FRAME_BYTES == SNAKE_MONO_BYTES, "mono frame size mismatch");
_Static_assert(BSP_EPD_GRAY4_FRAME_BYTES == SNAKE_GRAY4_PAGE_BYTES,
               "gray4 page size mismatch");

typedef enum {
    GS_DISP_CMD_INIT = 0,
    GS_DISP_CMD_FRAME,
    GS_DISP_CMD_FORGET,
} disp_cmd_kind_t;

typedef struct {
    disp_cmd_kind_t kind;
    board_frame_t frame;
} disp_cmd_t;

typedef struct {
    esp_err_t err;
    uint32_t ms;
    bool was_full;
} disp_result_t;

static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_res_queue;
static uint8_t *s_shadow;
static bool s_shadow_valid;

/* ---- Display task work --------------------------------------------------- */

static disp_result_t do_init(void)
{
    disp_result_t res = {0};
    const int64_t start = esp_timer_get_time();
    res.err = bsp_epd_init();
    res.ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    s_shadow_valid = false;
    if (res.err == ESP_OK) {
        ESP_LOGI(TAG, "panel ready in %u ms", (unsigned)res.ms);
    }
    return res;
}

static disp_result_t do_frame(const board_frame_t *frame)
{
    disp_result_t res = {.err = ESP_ERR_INVALID_ARG};
    if (!frame || !frame->data) return res;

    if (frame->kind == SNAKE_FRAME_GRAY4) {
        const int64_t start = esp_timer_get_time();
        res.err = bsp_epd_update_gray4(frame->data);
        res.ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
        res.was_full = true;
        /* Gray4 changes the panel content to something the mono shadow does
         * not describe, even when the update failed (content unknown). */
        s_shadow_valid = false;
        return res;
    }

    if (frame->kind != SNAKE_FRAME_MONO) return res;
    if (!s_shadow) {
        res.err = ESP_ERR_INVALID_STATE;
        return res;
    }

    const bool full = frame->full || !s_shadow_valid;
    const uint8_t *prev = s_shadow_valid ? s_shadow : NULL;

    const int64_t start = esp_timer_get_time();
    res.err = bsp_epd_update(frame->data, prev, full);
    res.ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    res.was_full = full;

    if (res.err == ESP_OK) {
        memcpy(s_shadow, frame->data, SNAKE_MONO_BYTES);
        s_shadow_valid = true;
    } else {
        /* bsp_epd.h: after a failed attempted refresh the panel content is
         * uncertain; the next mono update must be full with prev = NULL. */
        s_shadow_valid = false;
    }
    return res;
}

static disp_result_t do_forget(void)
{
    s_shadow_valid = false;
    disp_result_t res = {.err = ESP_OK};
    return res;
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        disp_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        disp_result_t res;
        switch (cmd.kind) {
        case GS_DISP_CMD_INIT:
            res = do_init();
            break;
        case GS_DISP_CMD_FORGET:
            res = do_forget();
            break;
        case GS_DISP_CMD_FRAME:
        default:
            res = do_frame(&cmd.frame);
            break;
        }

        if (res.err != ESP_OK) {
            ESP_LOGE(TAG, "refresh failed: %s (%u ms)", esp_err_to_name(res.err),
                     (unsigned)res.ms);
        } else if (cmd.kind == GS_DISP_CMD_FRAME) {
            ESP_LOGI(TAG, "%s %s %u ms", cmd.frame.kind == SNAKE_FRAME_GRAY4 ? "gray4" : "mono",
                     res.was_full ? "full" : "fast", (unsigned)res.ms);
        }
        (void)xQueueSend(s_res_queue, &res, portMAX_DELAY);
    }
}

/* ---- Public API ---------------------------------------------------------- */

static disp_result_t submit(const disp_cmd_t *cmd)
{
    disp_result_t res = {.err = ESP_FAIL};
    if (!s_cmd_queue || !s_res_queue) return res;
    if (xQueueSend(s_cmd_queue, cmd, portMAX_DELAY) != pdTRUE) return res;
    if (xQueueReceive(s_res_queue, &res, portMAX_DELAY) != pdTRUE) {
        res.err = ESP_FAIL;
    }
    return res;
}

esp_err_t display_port_init(void)
{
    if (s_cmd_queue && s_res_queue) return ESP_OK;

    s_shadow = NULL;
    s_shadow_valid = false;

    s_cmd_queue = xQueueCreate(1, sizeof(disp_cmd_t));
    s_res_queue = xQueueCreate(1, sizeof(disp_result_t));
    if (!s_cmd_queue || !s_res_queue) {
        if (s_cmd_queue) vQueueDelete(s_cmd_queue);
        if (s_res_queue) vQueueDelete(s_res_queue);
        s_cmd_queue = NULL;
        s_res_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(display_task, "gs_display", GS_DISPLAY_STACK, NULL, GS_DISPLAY_PRIO, NULL) !=
        pdPASS) {
        vQueueDelete(s_cmd_queue);
        vQueueDelete(s_res_queue);
        s_cmd_queue = NULL;
        s_res_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    const disp_cmd_t cmd = {.kind = GS_DISP_CMD_INIT};
    const disp_result_t res = submit(&cmd);
    if (res.err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(res.err));
    }
    return res.err;
}

esp_err_t display_port_attach(uint8_t *shadow)
{
    if (!shadow) return ESP_ERR_INVALID_ARG;
    s_shadow = shadow;
    s_shadow_valid = false;
    return ESP_OK;
}

esp_err_t display_port_refresh(const board_frame_t *frame, uint32_t *out_ms, bool *out_full)
{
    if (!frame || !frame->data) return ESP_ERR_INVALID_ARG;
    if (!s_cmd_queue || !s_res_queue) return ESP_ERR_INVALID_STATE;

    const disp_cmd_t cmd = {.kind = GS_DISP_CMD_FRAME, .frame = *frame};
    const disp_result_t res = submit(&cmd);
    if (out_ms) *out_ms = res.ms;
    if (out_full) *out_full = res.was_full;
    return res.err;
}

void display_port_forget_panel(void)
{
    if (!s_cmd_queue || !s_res_queue) return;
    const disp_cmd_t cmd = {.kind = GS_DISP_CMD_FORGET};
    (void)submit(&cmd);
}