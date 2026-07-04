#include "modules/network/http_requester.hpp"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "http_requester.hpp"
#include "sdkconfig.h"
#include <optional>
#include <string>
#include <strings.h>
#include <vector>


namespace modules::network {
    bool                                            HttpRequester::has_data_mask = false;
    std::map<std::string, std::string>              HttpRequester::header_buffer;
    std::vector<char>                               HttpRequester::data_buffer;
    std::array<char, CONFIG_DATA_BUFFER_SIZE + 1>   HttpRequester::recv_buffer({' '});
    HttpResponse                                    HttpRequester::response_buffer;
    bool                                            HttpRequester::streaming = false;
    ChunkCallback                                   HttpRequester::on_chunk;
    std::string                                     HttpRequester::session_id_buffer;

    HttpRequester::HttpRequester() {
        auto cfg = esp_http_client_config_t{};
        cfg.url = "http://localhost";
        cfg.event_handler  = HttpRequester::http_event_handler;
        cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
        cfg.buffer_size    = CONFIG_DATA_BUFFER_SIZE;
        
        auto client = esp_http_client_init(&cfg);
        this->client = std::move(client);
    }

    HttpRequester::~HttpRequester() {
        esp_http_client_cleanup(client);
    }

    auto HttpRequester::get_data() noexcept -> std::optional<HttpResponse> {
        if (data_buffer.empty() || !has_data_mask) {
            return std::nullopt;
        } else {
            auto res = response_buffer;
            res.body.insert(res.body.end(),
                std::make_move_iterator(data_buffer.begin()),
                std::make_move_iterator(data_buffer.end())
            );
            data_buffer.clear();
            has_data_mask = false;
            return res;
        }
    }

    auto HttpRequester::get_session_id() noexcept -> std::string {
        return session_id_buffer;
    }

    auto HttpRequester::get_status_code() noexcept -> uint32_t {
        return response_buffer.code;
    }

    auto HttpRequester::get(const char* url) noexcept -> esp_err_t {
        using method_t = esp_http_client_method_t;
        static const char* const TAG = "HTTP GET";
        auto res = ESP_OK;

        if (this->client == nullptr) {
            ESP_LOGE(TAG, "Http Client is not init");
            return ESP_FAIL;
        }

        esp_http_client_set_method(this->client, method_t::HTTP_METHOD_GET);
        esp_http_client_set_url(this->client, url);
        
        auto err = esp_http_client_perform(client);
        ESP_LOGI(TAG, "preform with url %s", url);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP GET require failed: %s", esp_err_to_name(err));
            res = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "HTTP GET require success");
        }

        return res;
    }

    auto HttpRequester::post(const char* url, const std::vector<char>& data) noexcept -> esp_err_t {
        using method_t = esp_http_client_method_t;
        static const char* const TAG = "HTTP POST";
        auto res = ESP_OK;

        if (this->client == nullptr) {
            ESP_LOGE(TAG, "Http Client is not init");
            return ESP_FAIL;
        }

        esp_http_client_set_method(this->client, method_t::HTTP_METHOD_POST);
        esp_http_client_set_url(this->client, url);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, data.data(), data.size());

        auto err = esp_http_client_perform(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP POST require failed: %s", esp_err_to_name(err));
            res = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "HTTP POST require success");
        }

        return res;
    }

    auto HttpRequester::post_stream(const char* url,
                                    const std::vector<char>& body,
                                    ChunkCallback on_chunk_cb) noexcept -> esp_err_t {
        using method_t = esp_http_client_method_t;
        static const char* const TAG = "HTTP POST STREAM";
        auto res = ESP_OK;

        if (this->client == nullptr) {
            ESP_LOGE(TAG, "Http Client is not init");
            return ESP_FAIL;
        }

        streaming = true;
        on_chunk  = std::move(on_chunk_cb);

        esp_http_client_set_method(this->client, method_t::HTTP_METHOD_POST);
        esp_http_client_set_url(this->client, url);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body.data(), body.size());

        // esp_http_client_perform 驱动事件处理器；响应体数据块会随到达
        // 通过 HTTP_EVENT_ON_DATA 交给 on_chunk。
        auto err = esp_http_client_perform(client);

        streaming = false;
        on_chunk  = nullptr;

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP POST STREAM failed: %s", esp_err_to_name(err));
            res = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "HTTP POST STREAM success");
        }

        return res;
    }

    auto HttpRequester::http_event_handler(esp_http_client_event_t* event) noexcept -> esp_err_t {
        using eve_t = esp_http_client_event_id_t;
        static const char* TAG = "http client";
        esp_err_t res_code = ESP_OK;

        switch(event->event_id) {
            case eve_t::HTTP_EVENT_ERROR: {
                ESP_LOGD(TAG, "HTTP EVENT ERROR");
                res_code = ESP_FAIL;
                break;
            }

            case eve_t::HTTP_EVENT_ON_CONNECTED: {
                ESP_LOGD(TAG, "HTTP EVENT CONNECTED");
                has_data_mask = false;
                if (!header_buffer.empty()) header_buffer.clear();
                if (!recv_buffer.empty())   recv_buffer.fill(' ');
                if (!data_buffer.empty())   data_buffer.clear();
                break;
            }

            case eve_t::HTTP_EVENT_HEADER_SENT: {
                ESP_LOGD(TAG, "HTTP EVENT HEADER_SENT");
                break;
            }

            case eve_t::HTTP_EVENT_ON_HEADER: {
                ESP_LOGD(TAG, "Receive header %s: %s", event->header_key, event->header_value);
                header_buffer[event->header_key] = event->header_value;
                if (strcasecmp(event->header_key, "X-Session-Id") == 0) {
                    session_id_buffer = event->header_value;
                }
                break;
            }

            case eve_t::HTTP_EVENT_ON_HEADERS_COMPLETE: {
                ESP_LOGD(TAG, "HTTP EVENT ON HEADER COMPLETE");
                break;
            }

            case eve_t::HTTP_EVENT_ON_STATUS_CODE: {
                auto code = esp_http_client_get_status_code(event->client);
                response_buffer.code = code;
                ESP_LOGD(TAG, "HTTP EVENT ON STATUS CODE %d", code);
                if (streaming && code != 200) {
                    ESP_LOGE(TAG, "streaming request got HTTP %d (body not forwarded)", code);
                }
                break;
            }

            case eve_t::HTTP_EVENT_ON_DATA: {
                ESP_LOGD(TAG, "HTTP EVENT ON DATA");
                auto length = event->data_len;
                auto* data = static_cast<const char*>(event->data);
                if (streaming && response_buffer.code == 200) {
                    // 下行流式：立即转发数据块，不做缓冲。
                    if (on_chunk) {
                        on_chunk(data, length);
                    }
                } else {
                    // 非流式，或错误响应体：缓冲起来。
                    data_buffer.insert(data_buffer.end(), data, data + length);
                }
                break;
            }

            case eve_t::HTTP_EVENT_ON_FINISH: {
                ESP_LOGD(TAG, "HTTP EVENT ON FINISH");
                has_data_mask = true;
                break;
            }

            case eve_t::HTTP_EVENT_DISCONNECTED: {
                ESP_LOGD(TAG, "HTTP EVENT DISCONNECTED");
                break;
            }

            case eve_t::HTTP_EVENT_REDIRECT: {
                ESP_LOGD(TAG, "HTTP EVENT ON REDIRECT");
                break;
            }

            default:
                break;
        }

        return res_code;
    }
}
