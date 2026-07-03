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
    /// SPI2 **slave** link to the STM32 (STM is master). Downlink audio
    /// (raw PCM16LE 24kHz mono) received over HTTP is buffered and clocked out
    /// to the STM32 in fixed-size frames; a handshake GPIO signals "frame ready".
    class StmBridge : public Singleton<StmBridge> {
        friend Singleton<StmBridge>;
        StmBridge();
        ~StmBridge();

        StreamBufferHandle_t stream_buf         {nullptr};
        TaskHandle_t         feeder_task        {nullptr};
        uint8_t*             frame              {nullptr};
        std::atomic<bool>    flush_requested    {false};
        bool                 initialized        {false};

        // Configuration
        static constexpr auto   SLAVE_SPI  = SPI2_HOST;
        static constexpr size_t FRAME_SIZE = CONFIG_SPI_FRAME_SIZE;
        static constexpr size_t RING_BYTES = 16 * 1024;

        static auto feeder_entry(void* arg) -> void;
        auto feeder_loop() -> void;

    public:
        /// Enqueue `len` bytes of downlink PCM for transmission to the STM32.
        /// Called from the HTTP streaming callback; applies backpressure when the
        /// internal buffer is full. Returns the number of bytes accepted.
        auto write(const uint8_t* data, size_t len) noexcept -> size_t;

        /// Flush any buffered remainder as a final (zero-padded) frame.
        auto flush() noexcept -> void;
    }; // class StmBridge
} // namespace modules::driver

#endif
