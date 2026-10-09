// Modified 2026-04-02 by Udi Tirosh for the VA5 project.
// Change: platform dispatch reduced to ESP32 only -- the ESP8266 and
//   ARDUINO_TEENSY41 branches were removed, matching the removal of the
//   non-ESP32 platform directories from this vendored copy.
// Upstream: gilmaimon/ArduinoWebsockets v0.5.4 (GPL-3.0)
//   https://github.com/gilmaimon/ArduinoWebsockets
// See VENDORED.md alongside this library for the full change inventory.

#pragma once

#include "../ws_config_defs.hpp"
#include <string>
#include <Arduino.h>

namespace websockets {
    typedef std::string WSString;
    typedef String WSInterfaceString;

    namespace internals {
        WSString fromInterfaceString(const WSInterfaceString& str);
        WSString fromInterfaceString(const WSInterfaceString&& str);
        WSInterfaceString fromInternalString(const WSString& str);
        WSInterfaceString fromInternalString(const WSString&& str);
    }
}

#ifdef ESP32

    #define PLATFORM_DOES_NOT_SUPPORT_BLOCKING_READ

    #include "../network/esp32/esp32_tcp.hpp"
    #define WSDefaultTcpClient websockets::network::Esp32TcpClient
    #define WSDefaultTcpServer websockets::network::Esp32TcpServer

    #ifndef _WS_CONFIG_NO_SSL
        // OpenSSL Dependent
        #define WSDefaultSecuredTcpClient websockets::network::SecuredEsp32TcpClient
    #endif //_WS_CONFIG_NO_SSL

#endif
