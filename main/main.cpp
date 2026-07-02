#include "esp_log.h"
#include "modules/network/http_requester.hpp"
#include "modules/network/wifi_station.hpp"
#include "nvs_flash.h"

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

extern "C" void test_http(void) {
    using namespace modules::network;
    static const char* TAG = "http test";
    auto& http_requester = HttpRequester::instance();
    auto url = "http://101.33.205.242:8080/api/v1/blogs";
    http_requester.get(url);
    auto data_opt = http_requester.get_data();
    if (data_opt.has_value()) {
        ESP_LOGI(TAG, "Receive http data successfully");
        auto data = data_opt.value().body;
        ESP_LOGI(TAG, "recv data:\n%s", data.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to receive http data");
    }
}

extern "C" void app_main(void) {
    nvs_init();
    auto& wifi_sta = modules::network::WifiStation::instance();
    wifi_sta.connect();

    /**************
     * Test func  *
     **************/
    test_http();

    // while (1) {
    
    // }

    return;
}