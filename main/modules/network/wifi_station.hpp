#ifndef WIFI_STATION_HPP
#define WIFI_STATION_HPP

#include "singleton.hpp"

namespace modules::network {
    class WifiStation : public Singleton<WifiStation> {
        friend Singleton<WifiStation>;
        WifiStation();
        ~WifiStation();

    public:
        auto connect() -> void;
    }; // class WifiStation
}

#endif
