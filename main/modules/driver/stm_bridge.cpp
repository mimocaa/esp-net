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

#ifdef CONFIG_SPI2_HANDSHAKE_ACTIVE_HIGH
    static constexpr int HS_ASSERTED   = 1;
    static constexpr int HS_DEASSERTED = 0;
#else
    static constexpr int HS_ASSERTED   = 0;
    static constexpr int HS_DEASSERTED = 1;
#endif

    /**
     * @brief 事务已装载、从机准备好被主机读取时的回调（在 ISR 上下文中调用）：拉有效握手线。
     */
    static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t*) {
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE), HS_ASSERTED);
    }

    /**
     * @brief 主机完成事务后的回调（在 ISR 上下文中调用）：拉无效握手线。
     */
    static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t*) {
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE), HS_DEASSERTED);
    }

    StmBridge::StmBridge() {
        gpio_config_t hs_cfg = {};
        hs_cfg.intr_type    = GPIO_INTR_DISABLE;
        hs_cfg.mode         = GPIO_MODE_OUTPUT;
        hs_cfg.pin_bit_mask = 1ULL << CONFIG_SPI2_HANDSHAKE;
        gpio_config(&hs_cfg);
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE), HS_DEASSERTED);

        // 上拉可避免主机空闲/断开时产生杂散事务。
        gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_SPI2_MOSI), GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_SPI2_SCLK), GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_SPI2_CS), GPIO_PULLUP_ONLY);

        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num     = CONFIG_SPI2_MOSI;
        bus_cfg.miso_io_num     = CONFIG_SPI2_MISO;
        bus_cfg.sclk_io_num     = CONFIG_SPI2_SCLK;
        bus_cfg.quadwp_io_num   = -1;
        bus_cfg.quadhd_io_num   = -1;
        bus_cfg.max_transfer_sz = static_cast<int>(FRAME_SIZE);

        spi_slave_interface_config_t spi_slv_cfg = {};
        spi_slv_cfg.mode          = 0;
        spi_slv_cfg.spics_io_num  = CONFIG_SPI2_CS;
        spi_slv_cfg.queue_size    = 3;
        spi_slv_cfg.flags         = 0;
        spi_slv_cfg.post_setup_cb = spi_post_setup_cb;
        spi_slv_cfg.post_trans_cb = spi_post_trans_cb;

        auto err = spi_slave_initialize(SLAVE_SPI, &bus_cfg, &spi_slv_cfg, SPI_DMA_CH_AUTO);
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

    auto StmBridge::assemble_frame() -> void {
        size_t filled = 0;

        while (1) {
            const auto got = xStreamBufferReceive(
                this->stream_buf, this->frame + filled, FRAME_SIZE - filled,
                pdMS_TO_TICKS(50));
            filled += got;

            if (filled == FRAME_SIZE) {
                return;
            }

            if (this->flush_requested.load() && got == 0) {
                this->flush_requested.store(false);
                if (filled > 0) {
                    std::memset(this->frame + filled, 0, FRAME_SIZE - filled);
                    return;
                }
                // 缓冲区为空：继续等待下一段音频。
            }
        }
    }

    auto StmBridge::feeder_loop() -> void {
        const TickType_t tx_timeout = pdMS_TO_TICKS(CONFIG_SPI_SLAVE_TX_TIMEOUT_MS);
        uint32_t frames    = 0;
        uint32_t underruns = 0;

        while (1) {
            assemble_frame();

            spi_slave_transaction_t trans = {};
            trans.length    = FRAME_SIZE * 8; // 单位为比特
            trans.tx_buffer = this->frame;
            trans.rx_buffer = nullptr;

            auto qerr = spi_slave_queue_trans(SLAVE_SPI, &trans, portMAX_DELAY);
            if (qerr != ESP_OK) {
                ESP_LOGE(TAG, "spi_slave_queue_trans failed: %s", esp_err_to_name(qerr));
                continue;
            }

            // 等待主机把该帧时钟读走。超时后事务仍在驱动中挂起，因此继续轮询
            // 同一个结果，而不是再排队新事务（避免死锁和事务泄漏）。
            spi_slave_transaction_t* done = nullptr;
            while (spi_slave_get_trans_result(SLAVE_SPI, &done, tx_timeout) != ESP_OK) {
                ++underruns;
                ESP_LOGW(TAG, "SPI tx timeout: master not reading (underruns=%lu)",
                         static_cast<unsigned long>(underruns));
            }

            ++frames;
            if ((frames % 100) == 0) {
                ESP_LOGI(TAG, "frames=%lu underruns=%lu",
                         static_cast<unsigned long>(frames),
                         static_cast<unsigned long>(underruns));
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
