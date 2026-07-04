#ifndef STM_BRIDGE_HPP
#define STM_BRIDGE_HPP

#include "singleton.hpp"
#include "driver/spi_slave.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace modules::driver {
    /** @brief 桥接相位：录音（收麦克风）/ 播放（发音频）。 */
    enum class Phase { Record, Play };

    /**
     * @brief 连接 STM32 的 SPI2 从机（STM 为主机）。
     *
     * 时分半双工：RECORD 相位收麦克风(MOSI, 16k)入 rx_stream；PLAY 相位从
     * tx_stream 取播放(MISO, 24k)发给 STM。相位由 ESP 决定并用握手 GPIO 通知
     * STM（Record=拉低 / Play=拉高）。每事务固定 CONFIG_SPI_FRAME_SIZE 字节。
     */
    class StmBridge : public Singleton<StmBridge> {
        friend Singleton<StmBridge>;
        StmBridge();
        ~StmBridge();

        StreamBufferHandle_t tx_stream   {nullptr}; // 下行播放 ESP->STM
        StreamBufferHandle_t rx_stream   {nullptr}; // 上行麦克风 STM->ESP
        TaskHandle_t         feeder_task {nullptr};
        uint8_t*             tx_frame    {nullptr};
        uint8_t*             rx_frame    {nullptr};
        std::atomic<Phase>   phase       {Phase::Record};
        bool                 initialized {false};

        // 配置项
        static constexpr auto   SLAVE_SPI     = SPI2_HOST;
        static constexpr size_t FRAME_SIZE    = CONFIG_SPI_FRAME_SIZE;
        static constexpr size_t TX_RING_BYTES = CONFIG_SPI_RING_BYTES;
        static constexpr size_t RX_RING_BYTES = CONFIG_MIC_RING_BYTES;

        static auto feeder_entry(void* arg) -> void;
        auto feeder_loop() -> void;
        /** @brief 非阻塞地从 tx_stream 取一帧到 tx_frame，不足补零。 */
        auto assemble_tx() -> void;

    public:
        /** @brief 设置相位并驱动握手线（Record=拉低 / Play=拉高）。 */
        auto set_phase(Phase p) noexcept -> void;
        auto get_phase() const noexcept -> Phase { return phase.load(); }

        /**
         * @brief 下行：将播放 PCM 入队（Play 相位由 feeder 发往 STM）。
         * @return 实际接受的字节数（缓冲满时产生背压）。
         */
        auto write(const uint8_t* data, size_t len) noexcept -> size_t;

        /** @brief 阻塞直到 tx_stream 中的播放数据全部发出（播放收尾）。 */
        auto flush() noexcept -> void;

        /**
         * @brief 上行：读取已收到的麦克风 PCM（Record 相位）。
         * @return 读到的字节数；超时返回已读到的（可能为 0）。
         */
        auto read_mic(uint8_t* buf, size_t max_len, uint32_t timeout_ms) noexcept -> size_t;
    }; // class StmBridge
} // namespace modules::driver

#endif
