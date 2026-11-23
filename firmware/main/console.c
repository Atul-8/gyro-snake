/*
 * GYRO SNAKE — serial console for IMU debugging (see console.h).
 *
 * The log output keeps using the ROM/uart path; only RX goes through the
 * driver, so log traffic and command traffic never share a buffer.
 */
#include "console.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu_port.h"
#include "driver/uart.h"

static const char *TAG = "gs_cmd";

#define CONSOLE_UART UART_NUM_0
#define CONSOLE_BAUD 115200
#define CONSOLE_RX_BUF 2048
#define CONSOLE_STACK 6144
#define CONSOLE_PRIO 3

static bool s_installed;

static void cmd_answer(const char *what, const char *outcome)
{
    ESP_LOGI(TAG, "CMD %s %s", what, outcome);
}

static void apply_map(int swap, int inv_roll, int inv_pitch)
{
    imu_port_set_axis(swap != 0, inv_roll != 0, inv_pitch != 0);
    cmd_answer("map", "ok");
}

static void handle_line(char *line)
{
    if (line[0] != '$') return;
    char *save = NULL;
    const char *cmd = strtok_r(line + 1, " \t", &save);
    if (!cmd) return;

    if (strcmp(cmd, "?") == 0) {
        imu_port_config_t cfg;
        imu_port_get_config(&cfg);
        ESP_LOGI(TAG,
                 "CFG map=%d%d%d deadzone=%d rearm=%d stable=%d telem=%d "
                 "neutral=%d/%d ready=%d",
                 (int)cfg.swap_xy, (int)cfg.invert_roll, (int)cfg.invert_pitch,
                 (int)(cfg.deadzone_deg * 10.0f), (int)(cfg.rearm_deg * 10.0f),
                 cfg.stable_samples, cfg.telemetry_hz,
                 (int)(cfg.neutral_roll * 100.0f),
                 (int)(cfg.neutral_pitch * 100.0f), (int)cfg.ready);
        return;
    }
    if (strcmp(cmd, "zero") == 0) {
        imu_port_request_rezero();
        cmd_answer("zero", "ok");
        return;
    }
    if (strcmp(cmd, "cal") == 0) {
        imu_port_request_calibration();
        cmd_answer("cal", "ok");
        return;
    }
    if (strcmp(cmd, "map") == 0) {
        const char *a = strtok_r(NULL, " \t", &save);
        const char *b = strtok_r(NULL, " \t", &save);
        const char *c = strtok_r(NULL, " \t", &save);
        if (!a || !b || !c) {
            cmd_answer("map", "err: need 3 values");
            return;
        }
        apply_map(atoi(a), atoi(b), atoi(c));
        return;
    }
    if (strcmp(cmd, "dead") == 0 || strcmp(cmd, "rearm") == 0 ||
        strcmp(cmd, "stable") == 0 || strcmp(cmd, "hz") == 0) {
        const char *v = strtok_r(NULL, " \t", &save);
        if (!v) {
            cmd_answer(cmd, "err: need a value");
            return;
        }
        const int n = atoi(v);
        if (strcmp(cmd, "hz") == 0) {
            imu_port_set_telemetry_hz(n);
            cmd_answer("hz", "ok");
        } else if (strcmp(cmd, "stable") == 0) {
            imu_port_set_tilt_params(0.0f, 0.0f, n);
            cmd_answer("stable", "ok");
        } else {
            /* Degrees arrive as deci-degrees from the page. */
            const float deg = (float)n / 10.0f;
            if (strcmp(cmd, "dead") == 0) {
                imu_port_set_tilt_params(deg, 0.0f, 0);
            } else {
                imu_port_set_tilt_params(0.0f, deg, 0);
            }
            cmd_answer(cmd, "ok");
        }
        return;
    }
    cmd_answer(cmd, "err: unknown");
}

static void console_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t len = 0;

    for (;;) {
        uint8_t ch;
        const int n = uart_read_bytes(CONSOLE_UART, &ch, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\b') {
            if (ch == '\n' && len > 0) {
                line[len] = '\0';
                handle_line(line);
            }
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        } else {
            len = 0; /* overrun: drop the line */
        }
    }
}

esp_err_t console_port_init(void)
{
    if (s_installed) return ESP_OK;

    const uart_config_t cfg = {
        .baud_rate = CONSOLE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(CONSOLE_UART, &cfg);
    if (err != ESP_OK) return err;
    err = uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install(UART0): %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(console_task, "gs_cmd", CONSOLE_STACK, NULL, CONSOLE_PRIO,
                    NULL) != pdPASS) {
        uart_driver_delete(CONSOLE_UART);
        return ESP_ERR_NO_MEM;
    }
    s_installed = true;
    ESP_LOGI(TAG, "console ready on UART0: $map S R P, $zero, $cal, $dead N, $hz N");
    return ESP_OK;
}
