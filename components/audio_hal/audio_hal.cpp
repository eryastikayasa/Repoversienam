#include "audio_hal.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include <stddef.h>

static const char *TAG = "AUDIO_HAL";
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static bool rx_enabled = false;

void audio_hal_init(void)
{
    ESP_LOGI(TAG, "Menginisialisasi Audio I2S - proven v6.1.5 / Xiaozhi-compatible...");
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 6; tx_chan_cfg.dma_frame_num = 240; tx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, nullptr));
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6; rx_chan_cfg.dma_frame_num = 240; rx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &rx_handle));

    i2s_std_config_t rx_cfg = {};
    rx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.bclk = MIC_I2S_SCK; rx_cfg.gpio_cfg.ws = MIC_I2S_WS;
    rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.din = MIC_I2S_SD;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_cfg));

    // Keep the proven v6.1.5 MAX98357A format: 32-bit I2S slots, LEFT, Philips.
    // Gemini audio remains PCM16/24k; conversion happens only at the I2S boundary.
    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
    tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    tx_cfg.slot_cfg.ws_pol = false;
    tx_cfg.slot_cfg.bit_shift = true;
    tx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; tx_cfg.gpio_cfg.bclk = SPK_I2S_BCLK; tx_cfg.gpio_cfg.ws = SPK_I2S_LRCK;
    tx_cfg.gpio_cfg.dout = SPK_I2S_DOUT; tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    rx_enabled = true;
    ESP_LOGI(TAG, "Audio siap. MIC=%d Hz 32-bit LEFT -> PCM16, SPEAKER=%d Hz 32-bit LEFT", MIC_SAMPLE_RATE, SPK_SAMPLE_RATE);
}

esp_err_t audio_hal_start_capture(void)
{
    if (!rx_handle) return ESP_ERR_INVALID_STATE;
    if (rx_enabled) return ESP_OK;
    const esp_err_t err = i2s_channel_enable(rx_handle);
    if (err == ESP_OK) {
        rx_enabled = true;
        ESP_LOGI(TAG, "MIC capture START");
    }
    return err;
}

esp_err_t audio_hal_stop_capture(void)
{
    if (!rx_handle) return ESP_ERR_INVALID_STATE;
    if (!rx_enabled) return ESP_OK;
    const esp_err_t err = i2s_channel_disable(rx_handle);
    if (err == ESP_OK) {
        rx_enabled = false;
        ESP_LOGI(TAG, "MIC capture STOP");
    }
    return err;
}

esp_err_t audio_hal_read_pcm(int16_t *buffer, size_t samples, size_t *samples_read)
{
    if (samples_read) *samples_read = 0;
    if (!buffer || samples == 0 || !samples_read) return ESP_ERR_INVALID_ARG;
    if (!rx_handle) return ESP_ERR_INVALID_STATE;
    if (samples > 512) return ESP_ERR_INVALID_SIZE;

    if (!rx_enabled) {
        const esp_err_t start_err = audio_hal_start_capture();
        if (start_err != ESP_OK) return start_err;
    }

    static int32_t raw[512];
    size_t bytes_read = 0;
    const esp_err_t err = i2s_channel_read(
        rx_handle,
        raw,
        samples * sizeof(int32_t),
        &bytes_read,
        portMAX_DELAY);
    if (err != ESP_OK) return err;

    const size_t count = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
        buffer[i] = static_cast<int16_t>(raw[i] >> 16);
    }
    *samples_read = count;
    return ESP_OK;
}

size_t audio_read_mic(uint8_t *dest, size_t max_len)
{
    if (!rx_handle || !dest || max_len < sizeof(int16_t)) return 0;
    if (!rx_enabled && audio_hal_start_capture() != ESP_OK) return 0;
    static int32_t raw[512];
    size_t max_samples = max_len / sizeof(int16_t); if (max_samples > 512) max_samples = 512;
    size_t bytes_read = 0;
    if (i2s_channel_read(rx_handle, raw, max_samples * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) return 0;
    size_t samples = bytes_read / sizeof(int32_t); int16_t *pcm = reinterpret_cast<int16_t *>(dest);
    for (size_t i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>(raw[i] >> 16);
    return samples * sizeof(int16_t);
}

void audio_write_speaker(const uint8_t *src, size_t len)
{
    if (!tx_handle || !src || len < 2) return;
    len &= ~((size_t)1);
    static int32_t tx_buffer[1024];
    const int16_t *pcm = reinterpret_cast<const int16_t *>(src);
    size_t total = len / sizeof(int16_t), offset = 0;

    /*
     * v7.0.34: keep the bounded I2S write from v7.0.33, but also yield after
     * every successful DMA chunk. v7.0.33 only yielded when I2S stalled; a
     * stream of successful 512-sample writes could still keep audio_playback
     * runnable on CPU1 long enough to starve IDLE1 and trigger Task WDT.
     *
     * ESP-IDF 6.x uses a millisecond timeout for i2s_channel_write().
     * Do not pass portMAX_DELAY here: that value is an RTOS tick sentinel,
     * not an I2S timeout in milliseconds. Bound each DMA wait so a stalled
     * speaker path cannot monopolize CPU1 and starve IDLE1/WDT.
     *
     * 512 PCM samples = 21.3 ms of 24 kHz audio in the 32-bit I2S path.
     * The explicit scheduler yield below gives other CPU1 tasks a chance
     * between chunks without changing the I2S format or DMA configuration.
     */
    constexpr size_t I2S_WRITE_SAMPLES = 512;
    constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;

    while (offset < total) {
        size_t n = total - offset; if (n > I2S_WRITE_SAMPLES) n = I2S_WRITE_SAMPLES;
        for (size_t i = 0; i < n; ++i) tx_buffer[i] = static_cast<int32_t>(pcm[offset + i]) << 16;

        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_handle, tx_buffer, n * sizeof(int32_t), &written, I2S_WRITE_TIMEOUT_MS);
        size_t samples_written = written / sizeof(int32_t);
        if (samples_written > n) samples_written = n;
        offset += samples_written;

        if (err != ESP_OK || samples_written == 0) {
            ESP_LOGW(TAG, "I2S speaker write timeout/fail: err=%s written=%u/%u timeout=%ums",
                     esp_err_to_name(err), (unsigned)written,
                     (unsigned)(n * sizeof(int32_t)), (unsigned)I2S_WRITE_TIMEOUT_MS);
            vTaskDelay(1);
            return;
        }

        vTaskDelay(1);
    }
}

void audio_i2s_test_tone(void)
{
    static const int16_t sine_table[24] = {0,2071,4000,5657,6928,7727,8000,7727,6928,5657,4000,2071,0,-2071,-4000,-5657,-6928,-7727,-8000,-7727,-6928,-5657,-4000,-2071};
    static int16_t tone[2400];
    if (!tx_handle) return;
    for (size_t i = 0; i < 2400; ++i) tone[i] = sine_table[i % 24];
    ESP_LOGI(TAG, "I2S TEST TONE: 1kHz PCM16 -> PCM32 I2S, 24kHz, 100ms");
    audio_write_speaker(reinterpret_cast<const uint8_t *>(tone), sizeof(tone));
}
