#include "display_driver.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "DISPLAY_DRV";

static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static esp_lcd_panel_io_handle_t s_panel_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static SemaphoreHandle_t s_oled_mutex = nullptr;
static bool s_ready = false;

void display_driver_init(void)
{
    if (s_ready) return;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)DISPLAY_DRIVER_SDA_PIN,
        .scl_io_num = (gpio_num_t)DISPLAY_DRIVER_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal membuat I2C OLED: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY_DRIVER_I2C_ADDR,
        .scl_speed_hz = DISPLAY_DRIVER_I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
    };

    err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal membuat panel I2C: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.bits_per_pixel = 1;

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = DISPLAY_DRIVER_HEIGHT,
    };
    panel_config.vendor_config = &ssd1306_config;

    ESP_LOGI(TAG, "Install SSD1306 driver");
    err = esp_lcd_new_panel_ssd1306(s_panel_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal install SSD1306: %s", esp_err_to_name(err));
        return;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED reset gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED init gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_lcd_panel_invert_color(s_panel, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED invert setup gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_lcd_panel_mirror(s_panel, true, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED mirror setup gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED display-on gagal: %s", esp_err_to_name(err));
        return;
    }

    s_oled_mutex = xSemaphoreCreateMutex();
    if (!s_oled_mutex) {
        ESP_LOGE(TAG, "Gagal membuat mutex OLED");
        return;
    }

    s_ready = true;

    ESP_LOGI(TAG,
             "SSD1306 128x64 siap: SDA=%d SCL=%d ADDR=0x%02X SPEED=%dHz MIRROR=XY",
             DISPLAY_DRIVER_SDA_PIN,
             DISPLAY_DRIVER_SCL_PIN,
             DISPLAY_DRIVER_I2C_ADDR,
             DISPLAY_DRIVER_I2C_HZ);
}

void display_driver_present(const uint8_t *buffer, int width, int height)
{
    if (!buffer) return;

    if (!s_ready) {
        display_driver_init();
    }

    if (!s_ready || !s_panel || !s_oled_mutex) return;

    if (width != DISPLAY_DRIVER_WIDTH || height != DISPLAY_DRIVER_HEIGHT) {
        ESP_LOGW(TAG,
                 "Framebuffer ditolak: %dx%d, yang didukung %dx%d",
                 width,
                 height,
                 DISPLAY_DRIVER_WIDTH,
                 DISPLAY_DRIVER_HEIGHT);
        return;
    }

    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "OLED mutex timeout");
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        0,
        DISPLAY_DRIVER_WIDTH,
        DISPLAY_DRIVER_HEIGHT,
        buffer);

    xSemaphoreGive(s_oled_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED draw gagal: %s", esp_err_to_name(err));
    }
}

bool display_driver_is_ready(void)
{
    return s_ready;
}
