#include "i2s_output.h"
#include "board.h"
#include "app_config.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static const char *TAG = "I2S_OUT";
static i2s_chan_handle_t tx_handle = NULL;

esp_err_t i2s_output_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = I2S_DMA_DESC_NUM,
        .dma_frame_num = I2S_DMA_FRAME_NUM,
        /* Zero each DMA buffer once played: when playback pauses/stops and
         * feeding stops, the DMA replays silence instead of looping the
         * last audio chunk as a stuck tone. */
        .auto_clear = true,
    };

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRC_GPIO,
            .dout = I2S_DIN_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return err;
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return err;
    }
    ESP_LOGI(TAG, "I2S output initialized (BCLK=%d, LRC=%d, DIN=%d)",
             I2S_BCLK_GPIO, I2S_LRC_GPIO, I2S_DIN_GPIO);
    return ESP_OK;
}

esp_err_t i2s_output_write(const uint8_t *data, size_t len, size_t *bytes_written, TickType_t timeout)
{
    if (!tx_handle) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(tx_handle, data, len, bytes_written, timeout);
}

void i2s_output_set_sample_rate(uint32_t rate)
{
    if (!tx_handle) return;
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(tx_handle);
    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
    i2s_channel_enable(tx_handle);

    ESP_LOGI(TAG, "sample rate changed to %lu Hz", (unsigned long)rate);
}

void i2s_output_deinit(void)
{
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        ESP_LOGI(TAG, "I2S output deinitialized");
    }
}
