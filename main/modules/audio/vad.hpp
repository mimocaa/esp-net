#ifndef VAD_HPP
#define VAD_HPP

#include <cstddef>
#include <cstdint>

namespace modules::audio {
    /**
     * @brief 基于能量(RMS)的简单语音活动检测。
     *
     * 逐帧喂入 16-bit PCM 采样；RMS 超过起始阈值判定说话开始，
     * 低于结束阈值并持续 hangover 时长后判定说话结束。阈值/挂起时长
     * 取自 Kconfig（`VAD_START_RMS` / `VAD_END_RMS` / `VAD_HANGOVER_MS`）。
     */
    class Vad {
    public:
        enum class Event { None, SpeechStart, SpeechEnd };

        Vad();

        /** @brief 处理一帧采样，返回本帧引发的事件。 */
        auto process(const int16_t* samples, size_t count) -> Event;

        /** @brief 当前是否处于说话态。 */
        auto is_speech() const -> bool { return speech; }

        /** @brief 复位到初始（静默）状态，用于开始新回合。 */
        auto reset() -> void;

    private:
        int  start_rms;
        int  end_rms;
        int  hangover_frames;
        bool speech {false};
        int  silence_run {0};

        static auto frame_rms(const int16_t* samples, size_t count) -> int;
    };
}

#endif
