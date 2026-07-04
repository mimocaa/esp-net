#include "modules/driver/stm_bridge.hpp"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
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

    StmBridge::StmBridge() {
        gpio_config_t hs_cfg = {};
        hs_cfg.intr_type    = GPIO_INTR_DISABLE;
        hs_cfg.mode         = GPIO_MODE_OUTPUT;
        hs_cfg.pin_bit_mask = 1ULL << CONFIG_SPI2_HANDSHAKE;
        gpio_config(&hs_cfg);
        // 初始 Record 相位：握手无效。
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
        spi_slv_cfg.mode         = 0;
        spi_slv_cfg.spics_io_num = CONFIG_SPI2_CS;
        spi_slv_cfg.queue_size   = 3;
        spi_slv_cfg.flags        = 0;
        // 握手改为按相位显式驱动，不再用 post_setup/post_trans 回调。

        auto err = spi_slave_initialize(SLAVE_SPI, &bus_cfg, &spi_slv_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(err));
            return;
        }

        this->tx_frame = static_cast<uint8_t*>(heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_DMA));
        this->rx_frame = static_cast<uint8_t*>(heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_DMA));
        if (this->tx_frame == nullptr || this->rx_frame == nullptr) {
            ESP_LOGE(TAG, "frame buffer alloc failed");
            spi_slave_free(SLAVE_SPI);
            return;
        }

        this->tx_stream = xStreamBufferCreate(TX_RING_BYTES, 1);
        this->rx_stream = xStreamBufferCreate(RX_RING_BYTES, 1);
        if (this->tx_stream == nullptr || this->rx_stream == nullptr) {
            ESP_LOGE(TAG, "stream buffer create failed");
            spi_slave_free(SLAVE_SPI);
            return;
        }

        this->initialized = true;

        xTaskCreate(&StmBridge::feeder_entry, "stm_feeder", 4096, this, 6,
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
        if (this->tx_stream != nullptr) {
            vStreamBufferDelete(this->tx_stream);
            this->tx_stream = nullptr;
        }
        if (this->rx_stream != nullptr) {
            vStreamBufferDelete(this->rx_stream);
            this->rx_stream = nullptr;
        }
        if (this->tx_frame != nullptr) {
            heap_caps_free(this->tx_frame);
            this->tx_frame = nullptr;
        }
        if (this->rx_frame != nullptr) {
            heap_caps_free(this->rx_frame);
            this->rx_frame = nullptr;
        }
        if (this->initialized) {
            spi_slave_free(SLAVE_SPI);
        }
    }

    auto StmBridge::set_phase(Phase p) noexcept -> void {
        this->phase.store(p);
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_SPI2_HANDSHAKE),
                       p == Phase::Play ? HS_ASSERTED : HS_DEASSERTED);
    }

    auto StmBridge::assemble_tx() -> void {
        const size_t got = xStreamBufferReceive(this->tx_stream, this->tx_frame,
                                                 FRAME_SIZE, 0);
        if (got < FRAME_SIZE) {
            std::memset(this->tx_frame + got, 0, FRAME_SIZE - got);
        }
    }

    auto StmBridge::feeder_entry(void* arg) -> void {
        static_cast<StmBridge*>(arg)->feeder_loop();
    }

    auto StmBridge::feeder_loop() -> void {
        const TickType_t tx_timeout = pdMS_TO_TICKS(CONFIG_SPI_SLAVE_TX_TIMEOUT_MS);

        while (1) {
            const Phase p = this->phase.load();

            if (p == Phase::Play) {
                assemble_tx();
            } else {
                // Record：MISO 发静音。
                std::memset(this->tx_frame, 0, FRAME_SIZE);
            }

            spi_slave_transaction_t trans = {};
            trans.length    = FRAME_SIZE * 8; // 单位为比特
            trans.tx_buffer = this->tx_frame;
            trans.rx_buffer = this->rx_frame;

            auto qerr = spi_slave_queue_trans(SLAVE_SPI, &trans, portMAX_DELAY);
            if (qerr != ESP_OK) {
                ESP_LOGE(TAG, "spi_slave_queue_trans failed: %s", esp_err_to_name(qerr));
                continue;
            }

            // 等主机时钟走完该帧；超时（主机未在时钟）时事务仍挂起，继续轮询
            // 同一结果，不重新排队（避免死锁与事务泄漏）。
            spi_slave_transaction_t* done = nullptr;
            while (spi_slave_get_trans_result(SLAVE_SPI, &done, tx_timeout) != ESP_OK) {
                ESP_LOGD(TAG, "SPI wait timeout (master idle)");
            }

            // Record 相位：把收到的麦克风帧压入 rx_stream（满则丢弃）。
            if (p == Phase::Record) {
                xStreamBufferSend(this->rx_stream, this->rx_frame, FRAME_SIZE, 0);
            }
        }
    }

    auto StmBridge::write(const uint8_t* data, size_t len) noexcept -> size_t {
        if (!this->initialized || this->tx_stream == nullptr ||
            data == nullptr || len == 0) {
            return 0;
        }
        return xStreamBufferSend(this->tx_stream, data, len, portMAX_DELAY);
    }

    auto StmBridge::flush() noexcept -> void {
        if (!this->initialized || this->tx_stream == nullptr) {
            return;
        }
        // 等待播放缓冲排空。
        while (xStreamBufferBytesAvailable(this->tx_stream) > 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        // 再留一帧时间让最后一帧被主机时钟走。
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    auto StmBridge::read_mic(uint8_t* buf, size_t max_len, uint32_t timeout_ms) noexcept
        -> size_t {
        if (!this->initialized || this->rx_stream == nullptr ||
            buf == nullptr || max_len == 0) {
            return 0;
        }
        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        size_t got = 0;
        while (got < max_len) {
            const size_t r = xStreamBufferReceive(this->rx_stream, buf + got,
                                                  max_len - got, ticks);
            if (r == 0) {
                break;
            }
            got += r;
        }
        return got;
    }
} // namespace modules::driver
