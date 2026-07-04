#ifndef HTTP_REQUESTER_HPP
#define HTTP_REQUESTER_HPP

#include "singleton.hpp"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace modules::network {
    struct HttpResponse {
        uint32_t code;
        std::string body;
    };

    /**
     * @brief 流式请求期间，每个响应体数据块到达时都会被调用的回调类型。
     *
     * @param data 数据块指针，仅在本次调用期间有效。
     * @param len  数据块字节数。
     */
    using ChunkCallback = std::function<void(const char* data, int len)>;

    class HttpRequester : public Singleton<HttpRequester> {
        friend Singleton<HttpRequester>;
        HttpRequester();
        ~HttpRequester();

        /**
         * Http buffer
         */
        esp_http_client_handle_t client {};
        /** @brief */
        static bool                                             has_data_mask;
        /** @brief 用于保存接收Http头信息的缓冲区 */
        static std::map<std::string, std::string>               header_buffer;
        /** @brief 发送Http body信息或接受总Http server数据的缓冲区 */
        static std::vector<char>                                data_buffer;
        /** @brief 接收Http单帧信息的缓冲区，考虑被弃用 */
        static std::array<char, CONFIG_DATA_BUFFER_SIZE + 1>    recv_buffer;
        static HttpResponse                                     response_buffer;

        /** @brief 流式接收状态：为 true 时逐块回调，不整段缓冲 */
        static bool                                             streaming;
        /** @brief 流式接收时每个响应数据块的回调 */
        static ChunkCallback                                    on_chunk;
        /** @brief 从响应头 X-Session-Id 捕获的会话 ID，跨请求保留 */
        static std::string                                      session_id_buffer;

    public:
        inline static  auto has_data() -> bool {
            return has_data_mask;
        }

        static auto get_data() noexcept -> std::optional<HttpResponse>;
        /** @brief 返回最近一次响应头中捕获的会话 ID（可能为空） */
        static auto get_session_id() noexcept -> std::string;
        /** @brief 返回最近一次响应的 HTTP 状态码（0 表示未知） */
        static auto get_status_code() noexcept -> uint32_t;

        auto get (const char* url) noexcept -> esp_err_t;
        auto post(const char* url, const std::vector<char>& data) noexcept -> esp_err_t;

        /**
         * @brief POST body 并把响应体逐块交给 on_chunk（不整段缓冲）。
         *
         * 用于下行音频流式接收后经 SPI 转发。调用在响应结束后返回。
         */
        auto post_stream(const char* url,
                         const std::vector<char>& body,
                         ChunkCallback on_chunk) noexcept -> esp_err_t;

    private:
        static auto http_event_handler(esp_http_client_event_t* event) noexcept -> esp_err_t;
    };
}

#endif
