#include "modules/network/http_requester.hpp"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "http_requester.hpp"
#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <vector>


namespace modules::network {
    #define BufferSize HttpRequester::DATA_BUFFER_SIZE

    bool                                HttpRequester::has_data_mask = false;
    std::map<const char*, std::string>  HttpRequester::header_buffer;
    std::vector<char>                   HttpRequester::data_buffer;
    std::array<char, BufferSize + 1>    HttpRequester::recv_buffer({' '});
    HttpResponse                        HttpRequester::response_buffer;

    HttpRequester::HttpRequester() {
        auto cfg = esp_http_client_config_t{};
        cfg.url = "http://localhost";
        cfg.event_handler  = HttpRequester::http_event_handler;
        cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
        cfg.buffer_size    = DATA_BUFFER_SIZE;
        
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
                // ESP_LOGD(TAG, "HTTP EVENT ON HEADER");
                ESP_LOGD(TAG, "Receive header %s: %s", event->header_key, event->header_value);
                header_buffer[event->header_key] = event->header_value;
                break;
            }

            case eve_t::HTTP_EVENT_ON_HEADERS_COMPLETE: {
                ESP_LOGD(TAG, "HTTP EVENT ON HEADER COMPLETE");
                if (header_buffer.contains("Content-Length")) {
                    auto data_len_str = header_buffer["Content-Length"];
                    auto data_size = 0u;
                    auto [mat_ptr, err] = std::from_chars(data_len_str.data(), data_len_str.end().base(), data_size);
                    if (mat_ptr == data_len_str.end().base() && err == std::errc{}) {
                        data_buffer.resize(data_size);
                    } else {
                        break;
                    }
                }

                break;
            }

            case eve_t::HTTP_EVENT_ON_STATUS_CODE: {
                auto code = esp_http_client_get_status_code(event->client);
                response_buffer.code = code;
                ESP_LOGD(TAG, "HTTP EVENT ON STATUS CODE %d", code);
                break;
            }

            case eve_t::HTTP_EVENT_ON_DATA: {
                ESP_LOGD(TAG, "HTTP EVENT ON DATA");
                auto length = event->data_len;
                auto* data = static_cast<const char*>(event->data);
                data_buffer.insert(data_buffer.end(), data, data + length);
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
