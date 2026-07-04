#include "modules/audio/vad.hpp"
#include "sdkconfig.h"
#include <cmath>

namespace modules::audio {
    Vad::Vad() {
        start_rms = CONFIG_VAD_START_RMS;
        end_rms   = CONFIG_VAD_END_RMS;

        // 16-bit @ 16kHz：每毫秒 16 采样、32 字节。帧时长(ms) = 字节数 / 32。
        int frame_ms = CONFIG_SPI_FRAME_SIZE / 32;
        if (frame_ms < 1) {
            frame_ms = 1;
        }
        hangover_frames = CONFIG_VAD_HANGOVER_MS / frame_ms;
        if (hangover_frames < 1) {
            hangover_frames = 1;
        }
    }

    auto Vad::reset() -> void {
        speech      = false;
        silence_run = 0;
    }

    auto Vad::frame_rms(const int16_t* samples, size_t count) -> int {
        if (samples == nullptr || count == 0) {
            return 0;
        }
        uint64_t acc = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t s = samples[i];
            acc += static_cast<uint64_t>(s * s);
        }
        return static_cast<int>(std::sqrt(static_cast<double>(acc) / count));
    }

    auto Vad::process(const int16_t* samples, size_t count) -> Event {
        const int rms = frame_rms(samples, count);

        if (!speech) {
            if (rms >= start_rms) {
                speech      = true;
                silence_run = 0;
                return Event::SpeechStart;
            }
            return Event::None;
        }

        if (rms < end_rms) {
            if (++silence_run >= hangover_frames) {
                speech      = false;
                silence_run = 0;
                return Event::SpeechEnd;
            }
        } else {
            silence_run = 0;
        }
        return Event::None;
    }
}
