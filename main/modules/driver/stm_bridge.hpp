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
    /**
     * @brief 连接 STM32 的 SPI2 从机（STM 为主机）。
     *
     * 通过 HTTP 收到的下行音频（裸 PCM16LE 24kHz 单声道）会被缓冲，并以定长帧
     * 的形式时钟输出给 STM32；一根握手 GPIO 用于告知“帧已就绪”。
     */
    class StmBridge : public Singleton<StmBridge> {
        friend Singleton<StmBridge>;
        StmBridge();
        ~StmBridge();

        StreamBufferHandle_t stream_buf         {nullptr};
        TaskHandle_t         feeder_task        {nullptr};
        uint8_t*             frame              {nullptr};
        std::atomic<bool>    flush_requested    {false};
        bool                 initialized        {false};

        // 配置项
        static constexpr auto   SLAVE_SPI  = SPI2_HOST;
        static constexpr size_t FRAME_SIZE = CONFIG_SPI_FRAME_SIZE;
        static constexpr size_t RING_BYTES = CONFIG_SPI_RING_BYTES;

        static auto feeder_entry(void* arg) -> void;
        auto feeder_loop() -> void;
        /**
         * @brief 阻塞直到在 frame 中组装出一个完整的 FRAME_SIZE 帧。
         *
         * 待 flush 时对不足一帧的尾部补零。
         */
        auto assemble_frame() -> void;

    public:
        /**
         * @brief 将 len 字节的下行 PCM 入队，等待发送给 STM32。
         *
         * 由 HTTP 流式回调调用；内部缓冲区满时会产生背压。
         * @return 实际接受的字节数。
         */
        auto write(const uint8_t* data, size_t len) noexcept -> size_t;

        /**
         * @brief 将缓冲区中剩余的不足一帧数据补零后作为最后一帧发出。
         */
        auto flush() noexcept -> void;
    }; // class StmBridge
} // namespace modules::driver

#endif
