#ifndef HTTP_REQUESTER_HPP
#define HTTP_REQUESTER_HPP

#include "singleton.hpp"
#include "esp_err.h"
#include "esp_http_client.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <map>
#include <vector>

namespace modules::network {
    struct HttpResponse {
        uint32_t code;
        std::string body;
    };

    class HttpRequester : public Singleton<HttpRequester> {
        friend Singleton<HttpRequester>;
        HttpRequester();
        ~HttpRequester();

        /**
         * Http configration
         */
        static const std::size_t                    DATA_BUFFER_SIZE = 2048;

        /**
         * Http buffer
         */
        esp_http_client_handle_t client {};
        /** @brief */
        static bool                                     has_data_mask;
        /** @brief 用于保存接收Http头信息的缓冲区 */
        static std::map<const char*, std::string>       header_buffer;
        /** @brief 发送Http body信息或接受总Http server数据的缓冲区 */
        static std::vector<char>                        data_buffer;
        /** @brief 接收Http单帧信息的缓冲区，考虑被弃用 */
        static std::array<char, DATA_BUFFER_SIZE + 1>   recv_buffer;
        static HttpResponse                             response_buffer;

    public:
        inline static  auto has_data() -> bool {
            return has_data_mask;
        }

        static auto get_data() noexcept -> std::optional<HttpResponse>;
        auto get (const char* url) noexcept -> esp_err_t;
        auto post(const char* url, const std::vector<char>& data) noexcept -> esp_err_t;

    private:
        static auto http_event_handler(esp_http_client_event_t* event) noexcept -> esp_err_t;
    };
}

#endif
