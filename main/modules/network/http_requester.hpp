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

    /**
     * @brief 上行分块上传的生产者回调类型。
     *
     * 每次调用应向 buf 填入至多 max_len 字节的待上传数据。
     * @return 实际填入的字节数；返回 0 表示上传结束。
     */
    using UploadCallback = std::function<int(char* buf, int max_len)>;

    /** @brief 上行结束、响应头就绪(200)后、开始读响应体前的回调。 */
    using UploadDoneCallback = std::function<void()>;

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

        /** @brief 从响应头 X-Session-Id 捕获的会话 ID，跨请求保留 */
        static std::string                                      session_id_buffer;
        /** @brief 从响应头 X-Emotion 捕获的情绪标记 */
        static std::string                                      emotion_buffer;

    public:
        inline static  auto has_data() -> bool {
            return has_data_mask;
        }

        static auto get_data() noexcept -> std::optional<HttpResponse>;
        /** @brief 返回最近一次响应头中捕获的会话 ID（可能为空） */
        static auto get_session_id() noexcept -> std::string;
        /** @brief 返回最近一次响应头中捕获的情绪标记（可能为空） */
        static auto get_emotion() noexcept -> std::string;
        /** @brief 返回最近一次响应的 HTTP 状态码（0 表示未知） */
        static auto get_status_code() noexcept -> uint32_t;

        auto get (const char* url) noexcept -> esp_err_t;
        auto post(const char* url, const std::vector<char>& data) noexcept -> esp_err_t;

        /**
         * @brief 上传请求体、并把响应体逐块交给 on_chunk 的双向流式请求。
         *
         * 上行：`content_length >= 0` 用 Content-Length 写裸字节；`content_length < 0`
         * 用 chunked（长度未知，内部逐块加 RFC7230 分块帧）。
         * 下行：读取响应体并逐块交给 on_chunk（供 SPI 转发），不整段缓冲。
         * 调用在整个请求-响应结束后返回。
         *
         * @param url            目标 URL
         * @param sid            请求头 X-Session-Id 的值（可为空串）
         * @param content_length 上行总字节数；<0 表示未知(用 chunked)
         * @param emotion_code   STM 宠物情绪协议码(0–6)，<0 表示不传
         * @param produce        上行数据生产者回调（返回 0 结束）
         * @param on_upload_done 上行结束、状态 200、开始读响应前回调（可为空）
         * @param on_chunk       下行数据块回调
         */
        auto post_stream(const char* url,
                         const char* sid,
                         int content_length,
                         int emotion_code,
                         UploadCallback produce,
                         UploadDoneCallback on_upload_done,
                         ChunkCallback on_chunk) noexcept -> esp_err_t;

    private:
        static auto http_event_handler(esp_http_client_event_t* event) noexcept -> esp_err_t;
    };
}

#endif
