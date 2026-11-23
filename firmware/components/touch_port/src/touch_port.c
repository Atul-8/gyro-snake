#include "touch_port.h"

#include "bsp_pins.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "gs_touch";

#define GT911_ADDR1 0x5D
#define GT911_ADDR2 0x14
#define GT911_REG_ID 0x8140
#define GT911_REG_COORD_RES 0x8146
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINTS 0x8150
#define GT911_I2C_TIMEOUT_MS 20
#define GT911_POLL_MS 30

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static atomic_bool s_press;
static atomic_bool s_ready;
static atomic_bool s_touching;
static atomic_int s_press_x;
static atomic_int s_press_y;
static int s_max_x = 800;
static int s_max_y = 480;
static int s_address;
static int s_fail_run;

/* The GT911 on this panel reports its range as 480x800 (checked in the boot
 * log), i.e. already portrait like the UI, so raw X feeds screen X and raw Y
 * feeds screen Y. Flips are not applied yet: whether the film's origin is the
 * panel's top-left is only knowable from a tap in a known corner, and
 * touch_task prints raw and mapped values on every touch-down for that check.
 * The game's zero gesture accepts any corner, so a flip cannot break it. */
static void touch_map(int rx, int ry, int *sx, int *sy)
{
    const int mx = s_max_x > 1 ? s_max_x - 1 : 1;
    const int my = s_max_y > 1 ? s_max_y - 1 : 1;

    int x = (rx * 479) / mx;
    int y = (ry * 799) / my;

    if (x < 0) x = 0;
    if (x > 479) x = 479;
    if (y < 0) y = 0;
    if (y > 799) y = 799;
    *sx = x;
    *sy = y;
}

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFu)};
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, len,
                                       GT911_I2C_TIMEOUT_MS);
}

static esp_err_t gt_write(uint16_t reg, uint8_t value)
{
    const uint8_t addr[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFu), value};
    return i2c_master_transmit(s_dev, addr, sizeof(addr), GT911_I2C_TIMEOUT_MS);
}

/* GT911 address select: INT=0 during reset -> 0x5D, INT=1 -> 0x14. */
static bool gt_probe(uint8_t address)
{
    gpio_hold_dis((gpio_num_t)BSP_PIN_TOUCH_RST);
    gpio_hold_dis((gpio_num_t)BSP_PIN_TOUCH_INT);

    gpio_set_direction((gpio_num_t)BSP_PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)BSP_PIN_TOUCH_INT, GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)BSP_PIN_TOUCH_RST, 0);
    gpio_set_level((gpio_num_t)BSP_PIN_TOUCH_INT, address == GT911_ADDR1 ? 0 : 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level((gpio_num_t)BSP_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level((gpio_num_t)BSP_PIN_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(60));

    gpio_set_direction((gpio_num_t)BSP_PIN_TOUCH_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (i2c_master_probe(s_bus, address, GT911_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &dev) != ESP_OK) {
        return false;
    }

    s_dev = dev;
    uint8_t id[4] = {0};
    if (gt_read(GT911_REG_ID, id, sizeof(id)) == ESP_OK &&
        id[0] == '9' && id[1] == '1' && id[2] == '1') {
        uint8_t res[4] = {0};
        if (gt_read(GT911_REG_COORD_RES, res, sizeof(res)) == ESP_OK) {
            const int mx = (int)(res[0] | (res[1] << 8));
            const int my = (int)(res[2] | (res[3] << 8));
            if (mx > 0) s_max_x = mx;
            if (my > 0) s_max_y = my;
        }
        s_address = address;
        ESP_LOGI(TAG, "GT911 ready: address=0x%02X id=%c%c%c res=%dx%d", address,
                 id[0], id[1], id[2], s_max_x, s_max_y);
        return true;
    }

    s_dev = NULL;
    (void)i2c_master_bus_rm_device(dev);
    return false;
}

static void touch_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint8_t status = 0;
        if (gt_read(GT911_REG_STATUS, &status, 1) == ESP_OK) {
            s_fail_run = 0;
            if ((status & 0x80u) != 0u) {
                const uint8_t count = status & 0x0Fu;
                if (count > 0u && count <= 5u) {
                    uint8_t point[8];
                    if (gt_read(GT911_REG_POINTS, point, sizeof(point)) == ESP_OK) {
                        const int rx = (int)(point[0] | (point[1] << 8));
                        const int ry = (int)(point[2] | (point[3] << 8));
                        int sx = 0;
                        int sy = 0;
                        touch_map(rx, ry, &sx, &sy);
                        if (!atomic_load(&s_touching)) {
                            atomic_store(&s_touching, true);
                            atomic_store(&s_press_x, sx);
                            atomic_store(&s_press_y, sy);
                            atomic_store(&s_press, true);
                            ESP_LOGI(TAG, "tap raw=%d,%d -> screen=%d,%d", rx, ry, sx, sy);
                        } else {
                            atomic_store(&s_press_x, sx);
                            atomic_store(&s_press_y, sy);
                        }
                    }
                } else {
                    atomic_store(&s_touching, false);
                }
                /* Clear the buffer-ready bit so the next report can latch. */
                (void)gt_write(GT911_REG_STATUS, 0);
            }
        } else if (++s_fail_run == 50) {
            ESP_LOGW(TAG, "GT911 status read keeps failing");
        }
        vTaskDelay(pdMS_TO_TICKS(GT911_POLL_MS));
    }
}

esp_err_t touch_port_init(void)
{
    if (atomic_load(&s_ready)) {
        return ESP_OK;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_PIN_TOUCH_PWR) | (1ULL << BSP_PIN_TOUCH_RST) |
                        (1ULL << BSP_PIN_TOUCH_INT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level((gpio_num_t)BSP_PIN_TOUCH_PWR, 1);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_PIN_I2C0_SDA,
        .scl_io_num = BSP_PIN_I2C0_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    if (s_bus == NULL) {
        err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C0 bus init failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!gt_probe(GT911_ADDR2) && !gt_probe(GT911_ADDR1)) {
        ESP_LOGE(TAG, "GT911 not found at 0x14 or 0x5D; touch disabled");
        return ESP_FAIL;
    }

    if (xTaskCreate(touch_task, "gs_touch", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "touch task create failed");
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_ready, true);
    ESP_LOGI(TAG, "touch polling every %d ms", GT911_POLL_MS);
    return ESP_OK;
}

bool touch_port_take_press_xy(int *x, int *y)
{
    if (!atomic_exchange(&s_press, false)) {
        return false;
    }
    if (x) *x = atomic_load(&s_press_x);
    if (y) *y = atomic_load(&s_press_y);
    return true;
}

bool touch_port_take_press(void)
{
    return touch_port_take_press_xy(NULL, NULL);
}

bool touch_port_ready(void)
{
    return atomic_load(&s_ready);
}

int touch_port_address(void)
{
    return s_address;
}