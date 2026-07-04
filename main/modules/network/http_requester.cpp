#include "modules/network/http_requester.hpp"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "http_requester.hpp"
#include "sdkconfig.h"
#include <cstdio>
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
    std::string                                     HttpRequester::session_id_buffer;
    std::string                                     HttpRequester::emotion_buffer;

    HttpRequester::HttpRequester() {
        auto cfg = esp_http_client_config_t{};
        cfg.url = "http://localhost";
        cfg.event_handler  = HttpRequester::http_event_handler;
        cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
        cfg.buffer_size    = CONFIG_DATA_BUFFER_SIZE;
        // 有限超时：连接/读写超时后 open/read 返回错误，而不是无限轮询重试。
        cfg.timeout_ms     = CONFIG_HTTP_TIMEOUT_MS;
        
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

    auto HttpRequester::get_emotion() noexcept -> std::string {
        return emotion_buffer;
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

    // 把 buffer 全部写出（重试短写）；出错返回 -1。
    static auto write_all(esp_http_client_handle_t client, const char* p, int n) -> int {
        int off = 0;
        while (off < n) {
            int w = esp_http_client_write(client, p + off, n - off);
            if (w < 0) {
                return -1;
            }
            off += w;
        }
        return 0;
    }

    auto HttpRequester::post_stream(const char* url,
                                    const char* sid,
                                    int content_length,
                                    UploadCallback produce,
                                    UploadDoneCallback on_upload_done,
                                    ChunkCallback on_chunk) noexcept -> esp_err_t {
        using method_t = esp_http_client_method_t;
        static const char* const TAG = "HTTP POST STREAM";

        if (this->client == nullptr) {
            ESP_LOGE(TAG, "Http Client is not init");
            return ESP_FAIL;
        }

        const bool chunked = (content_length < 0);

        esp_http_client_set_method(this->client, method_t::HTTP_METHOD_POST);
        esp_http_client_set_url(this->client, url);
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        if (sid != nullptr) {
            esp_http_client_set_header(client, "X-Session-Id", sid);
        }

        // content_length>=0 设置 Content-Length；<0 用 open(-1) 启用 chunked。
        auto err = esp_http_client_open(client, content_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
            return ESP_FAIL;
        }

        // 上行：循环调用生产者取数据并写出，直到返回 0。
        // 静态缓冲避免占用调用任务栈（post_stream 非重入，单例客户端）。
        static char up_buf[CONFIG_SPI_FRAME_SIZE];
        int  produced = 0;
        size_t total_up = 0;
        while (produce && (produced = produce(up_buf, sizeof(up_buf))) > 0) {
            if (chunked) {
                // RFC7230 分块帧：<hex len>\r\n <data> \r\n
                char hdr[16];
                int hn = snprintf(hdr, sizeof(hdr), "%X\r\n", produced);
                if (write_all(client, hdr, hn) < 0 ||
                    write_all(client, up_buf, produced) < 0 ||
                    write_all(client, "\r\n", 2) < 0) {
                    ESP_LOGE(TAG, "chunk write failed after %u bytes", (unsigned)total_up);
                    esp_http_client_close(client);
                    return ESP_FAIL;
                }
            } else {
                if (write_all(client, up_buf, produced) < 0) {
                    ESP_LOGE(TAG, "write failed after %u bytes", (unsigned)total_up);
                    esp_http_client_close(client);
                    return ESP_FAIL;
                }
            }
            total_up += produced;
        }
        if (chunked) {
            // 结束分块传输。
            if (write_all(client, "0\r\n\r\n", 5) < 0) {
                ESP_LOGE(TAG, "chunk terminator write failed");
                esp_http_client_close(client);
                return ESP_FAIL;
            }
        }
        ESP_LOGI(TAG, "uplink sent %u bytes", (unsigned)total_up);

        // 发送结束、读取响应头（会触发 header 事件以捕获 X-Session-Id / X-Emotion）。
        auto content_len = esp_http_client_fetch_headers(client);
        (void)content_len;
        auto status = esp_http_client_get_status_code(client);
        response_buffer.code = status;

        if (status != 200) {
            ESP_LOGE(TAG, "streaming request got HTTP %d (body not forwarded)", status);
            esp_http_client_close(client);
            return ESP_FAIL;
        }

        if (on_upload_done) {
            on_upload_done();
        }

        // 下行：逐块读取响应体裸 PCM，交给 on_chunk（供 SPI 转发）。
        size_t total_down = 0;
        while (!esp_http_client_is_complete_data_received(client)) {
            int r = esp_http_client_read(client, recv_buffer.data(),
                                         CONFIG_DATA_BUFFER_SIZE);
            if (r < 0) {
                ESP_LOGE(TAG, "read failed after %u bytes", (unsigned)total_down);
                esp_http_client_close(client);
                return ESP_FAIL;
            }
            if (r == 0) {
                break;
            }
            if (on_chunk) {
                on_chunk(recv_buffer.data(), r);
            }
            total_down += r;
        }
        ESP_LOGI(TAG, "downlink recv %u bytes", (unsigned)total_down);

        esp_http_client_close(client);
        return ESP_OK;
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
                } else if (strcasecmp(event->header_key, "X-Emotion") == 0) {
                    emotion_buffer = event->header_value;
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
                break;
            }

            case eve_t::HTTP_EVENT_ON_DATA: {
                // 仅用于 perform 驱动的 get/post；post_stream 走手动 read。
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
