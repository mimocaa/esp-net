#include "esp_log.h"
#include "modules/driver/stm_bridge.hpp"
#include "modules/network/http_requester.hpp"
#include "modules/network/wifi_station.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"
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
 * @brief 向后端 /chat 发起一次对话请求，把流式返回的下行音频（PCM16LE 24kHz mono）
 *        逐块经 SPI 转发给 STM32 播放。
 *
 * 注意：设备端录音采集尚未实现，audio_data 目前为占位空串，仅用于打通下行链路。
 */
extern "C" void chat_once(void) {
    using namespace modules::network;
    using modules::driver::StmBridge;
    static const char* TAG = "chat";

    auto& http = HttpRequester::instance();
    StmBridge::instance();

    std::string session_id = HttpRequester::get_session_id();
    std::string audio_b64  = "";
    std::string body = std::string("{\"session_id\":\"") + session_id +
                       "\",\"audio_data\":\"" + audio_b64 + "\"}";
    std::vector<char> body_vec(body.begin(), body.end());

    std::string url = std::string(CONFIG_SERVER_BASE_URL) + "/chat";

    auto err = http.post_stream(url.c_str(), body_vec,
        [](const char* data, int len) {
            StmBridge::instance().write(
                reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(len));
        });

    auto status = HttpRequester::get_status_code();
    if (err == ESP_OK && status == 200) {
        StmBridge::instance().flush();
        ESP_LOGI(TAG, "chat done, session=%s", HttpRequester::get_session_id().c_str());
    } else {
        ESP_LOGE(TAG, "chat request failed (err=%d status=%lu)",
                 err, static_cast<unsigned long>(status));
    }
}

extern "C" void app_main(void) {
    nvs_init();
    auto& wifi_sta = modules::network::WifiStation::instance();
    wifi_sta.connect();

    chat_once();

    return;
}
