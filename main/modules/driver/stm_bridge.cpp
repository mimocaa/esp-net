#include "modules/driver/stm_bridge.hpp"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <cstring>

namespace modules::driver {
    static const char* const TAG = "StmBridge";

    // Called (in ISR context) once a transaction is loaded and the slave is ready
    // for the master to clock data: raise the handshake line.
    static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t*) {
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE), 1);
    }

    // Called (in ISR context) after the master finished the transaction: lower it.
    static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t*) {
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE), 0);
    }

    StmBridge::StmBridge() {
        gpio_config_t hs = {};
        hs.intr_type    = GPIO_INTR_DISABLE;
        hs.mode         = GPIO_MODE_OUTPUT;
        hs.pin_bit_mask = 1ULL << CONFIG_SPI2_HANDSHAKE;
        gpio_config(&hs);
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE), 0);

        // Pull-ups avoid spurious transactions while the master is idle/disconnected.
        gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_SPI2_MOSI), GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_SPI2_SCLK), GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_SPI2_CS), GPIO_PULLUP_ONLY);

        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num     = CONFIG_SPI2_MOSI;
        buscfg.miso_io_num     = CONFIG_SPI2_MISO;
        buscfg.sclk_io_num     = CONFIG_SPI2_SCLK;
        buscfg.quadwp_io_num   = -1;
        buscfg.quadhd_io_num   = -1;
        buscfg.max_transfer_sz = static_cast<int>(FRAME_SIZE);

        spi_slave_interface_config_t slvcfg = {};
        slvcfg.mode          = 0;
        slvcfg.spics_io_num  = CONFIG_SPI2_CS;
        slvcfg.queue_size    = 3;
        slvcfg.flags         = 0;
        slvcfg.post_setup_cb = spi_post_setup_cb;
        slvcfg.post_trans_cb = spi_post_trans_cb;

        auto err = spi_slave_initialize(SLAVE_SPI, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(err));
            return;
        }

        this->frame = static_cast<uint8_t*>(
            heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_DMA));
        if (this->frame == nullptr) {
            ESP_LOGE(TAG, "frame buffer alloc failed");
            spi_slave_free(SLAVE_SPI);
            return;
        }

        this->stream_buf = xStreamBufferCreate(RING_BYTES, 1);
        if (this->stream_buf == nullptr) {
            ESP_LOGE(TAG, "stream buffer create failed");
            heap_caps_free(this->frame);
            this->frame = nullptr;
            spi_slave_free(SLAVE_SPI);
            return;
        }

        this->initialized = true;

        xTaskCreate(&StmBridge::feeder_entry, "stm_feeder", 4096, this, 5,
                    &this->feeder_task);

        ESP_LOGI(TAG, "SPI2 slave ready (MOSI=%d MISO=%d SCLK=%d CS=%d HS=%d frame=%d)",
                 CONFIG_SPI2_MOSI, CONFIG_SPI2_MISO, CONFIG_SPI2_SCLK,
                 CONFIG_SPI2_CS, CONFIG_SPI2_HANDSHAKE, static_cast<int>(FRAME_SIZE));
    }

    StmBridge::~StmBridge() {
        if (this->feeder_task != nullptr) {
            vTaskDelete(this->feeder_task);
            this->feeder_task = nullptr;
        }

        if (this->stream_buf != nullptr) {
            vStreamBufferDelete(this->stream_buf);
            this->stream_buf = nullptr;
        }

        if (this->frame != nullptr) {
            heap_caps_free(this->frame);
            this->frame = nullptr;
        }

        if (this->initialized) {
            spi_slave_free(SLAVE_SPI);
        }
    }

    auto StmBridge::feeder_entry(void* arg) -> void {
        static_cast<StmBridge*>(arg)->feeder_loop();
    }

    auto StmBridge::feeder_loop() -> void {
        size_t filled = 0;

        while (1) {
            const auto got = xStreamBufferReceive(
                this->stream_buf, this->frame + filled, FRAME_SIZE - filled,
                pdMS_TO_TICKS(50));
            filled += got;

            const bool full  = (filled == FRAME_SIZE);
            const bool flush = this->flush_requested.load() && got == 0 && filled > 0;

            if (full || flush) {
                if (flush) {
                    std::memset(this->frame + filled, 0, FRAME_SIZE - filled);
                }

                spi_slave_transaction_t trans = {};
                trans.length    = FRAME_SIZE * 8; // bits
                trans.tx_buffer = this->frame;
                trans.rx_buffer = nullptr;

                auto err = spi_slave_transmit(SLAVE_SPI, &trans, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "spi_slave_transmit failed: %s", esp_err_to_name(err));
                }
                filled = 0;
            }

            if (this->flush_requested.load() && got == 0) {
                this->flush_requested.store(false);
            }
        }
    }

    auto StmBridge::write(const uint8_t* data, size_t len) noexcept -> size_t {
        if (!this->initialized || this->stream_buf == nullptr ||
            data == nullptr || len == 0) {
            return 0;
        }
        return xStreamBufferSend(this->stream_buf, data, len, portMAX_DELAY);
    }

    auto StmBridge::flush() noexcept -> void {
        if (this->initialized) {
            this->flush_requested.store(true);
        }
    }
} // namespace modules::driver
