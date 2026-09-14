#include "sd_card.h"
#include "board.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SD_CARD";
static bool mounted = false;
static sdmmc_card_t *s_card = NULL;

static esp_err_t try_mount_once(sdmmc_card_t **out_card)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    /* Card init always runs at 400 kHz probing speed internally; cap the
     * working clock low for marginal jumper wiring. */
    host.max_freq_khz = 2000;  /* 2MHz: dupont wiring corrupts sustained reads at higher clocks (int-wdt resets) */

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_GPIO;
    slot_config.host_id = host.slot;

    spi_bus_config_t buscfg = {
        .sclk_io_num   = SD_SCK_GPIO,
        .mosi_io_num   = SD_MOSI_GPIO,
        .miso_io_num   = SD_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(host.slot, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        spi_bus_free(host.slot);
        return err;
    }

    *out_card = card;
    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    if (mounted) {
        return ESP_OK;
    }

    esp_err_t err = ESP_FAIL;
    sdmmc_card_t *card = NULL;
    for (int attempt = 1; attempt <= 2; attempt++) {
        ESP_LOGI(TAG, "mount attempt %d/2 (SCK=%d MOSI=%d MISO=%d CS=%d)",
                 attempt, SD_SCK_GPIO, SD_MOSI_GPIO, SD_MISO_GPIO, SD_CS_GPIO);
        err = try_mount_once(&card);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "attempt %d failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed after retries: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, ">> check wiring: SCK->%d SDA(MOSI)->%d MISO->%d CS->%d, module VCC=5V, GND common",
                 SD_SCK_GPIO, SD_MOSI_GPIO, SD_MISO_GPIO, SD_CS_GPIO);
        return err;
    }

    s_card = card;
    sdmmc_card_print_info(stdout, card);
    mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

void sd_card_unmount(void)
{
    if (!mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    spi_bus_free(SD_SPI_HOST);
    mounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
}
  