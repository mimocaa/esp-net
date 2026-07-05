#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modules/audio/vad.hpp"
#include "modules/driver/stm_bridge.hpp"
#include "modules/network/http_requester.hpp"
#include "modules/network/wifi_station.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/**
 * @brief 初始化 NVS
 */
void nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/**
 * @brief 一个对话回合：Record 相位用 VAD 等待说话 → chunked 上传（pre-roll +
 *        持续麦克风）→ 上传结束切 Play 相位 → 读下行 TTS 经 SPI 播放。
 */
static void chat_turn(modules::audio::Vad& vad) {
    using namespace modules::network;
    using modules::audio::Vad;
    using modules::driver::Phase;
    using modules::driver::StmBridge;
    static const char* const TAG = "chat";

    constexpr int FRAME = CONFIG_SPI_FRAME_SIZE;
    const int frame_ms = FRAME / 32 > 0 ? FRAME / 32 : 1;
    const int preroll_frames = CONFIG_VAD_PREROLL_MS / frame_ms;

    auto& http = HttpRequester::instance();
    auto& stm  = StmBridge::instance();

    vad.reset();
    stm.set_phase(Phase::Record);
    stm.resume(); // 通知 feeder 期待下一帧为控制帧（STM 使能时发送）

    // ---- Record：等待说话开始，维护 pre-roll 环 ----
    std::vector<uint8_t> preroll;
    std::vector<uint8_t> frame(FRAME);
    bool started = false;
    while (!started) {
        size_t n = stm.read_mic(frame.data(), FRAME, 1000);
        if (n == 0) {
            continue;
        }
        auto ev = vad.process(reinterpret_cast<const int16_t*>(frame.data()), n / 2);
        preroll.insert(preroll.end(), frame.begin(), frame.begin() + n);
        const size_t cap = static_cast<size_t>(preroll_frames) * FRAME;
        if (cap > 0 && preroll.size() > cap) {
            preroll.erase(preroll.begin(), preroll.begin() + (preroll.size() - cap));
        }
        if (ev == Vad::Event::SpeechStart) {
            started = true;
        }
    }
    ESP_LOGI(TAG, "speech start (preroll=%u bytes)", (unsigned)preroll.size());

    // ---- 上行生产者：先发 pre-roll，再持续读麦克风直到 VAD 判定结束 ----
    size_t pre_off = 0;
    bool   ended   = false;
    auto produce = [&](char* buf, int max_len) -> int {
        if (pre_off < preroll.size()) {
            size_t r = std::min(static_cast<size_t>(max_len), preroll.size() - pre_off);
            std::memcpy(buf, preroll.data() + pre_off, r);
            pre_off += r;
            return static_cast<int>(r);
        }
        if (ended) {
            return 0;
        }
        size_t want = std::min(max_len, FRAME);
        size_t n = stm.read_mic(reinterpret_cast<uint8_t*>(buf), want, 1000);
        if (n == 0) {
            ended = true; // 超时无麦克风数据：视为结束，避免挂死
            return 0;
        }
        auto ev = vad.process(reinterpret_cast<const int16_t*>(buf), n / 2);
        if (ev == Vad::Event::SpeechEnd) {
            ended = true;
        }
        return static_cast<int>(n);
    };

    auto on_upload_done = [&]() { stm.set_phase(Phase::Play); };
    auto on_chunk = [](const char* data, int len) {
        StmBridge::instance().write(reinterpret_cast<const uint8_t*>(data),
                                    static_cast<size_t>(len));
    };

    std::string url = std::string(CONFIG_SERVER_BASE_URL) + "/chat";
    std::string sid = HttpRequester::get_session_id();

    int emotion_code = stm.get_stm_emotion();

    auto err = http.post_stream(url.c_str(), sid.c_str(), -1, emotion_code,
                                produce, on_upload_done, on_chunk);
    auto status = HttpRequester::get_status_code();

    if (err == ESP_OK && status == 200) {
        stm.flush();
        ESP_LOGI(TAG, "turn done, session=%s emotion=%s",
                 HttpRequester::get_session_id().c_str(),
                 HttpRequester::get_emotion().c_str());
    } else {
        ESP_LOGE(TAG, "turn failed (err=%d status=%lu)",
                 err, static_cast<unsigned long>(status));
    }

    stm.set_phase(Phase::Record);
}

extern "C" void app_main(void) {
    nvs_init();
    modules::driver::StmBridge::instance();

    auto& wifi_sta = modules::network::WifiStation::instance();
    wifi_sta.connect();

    modules::audio::Vad vad;
    while (1) {
        chat_turn(vad);
    }
}
