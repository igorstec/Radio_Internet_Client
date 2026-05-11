#pragma once
#include <string>
#include <unistd.h>
#include <iostream>
#include <arpa/inet.h> 
#include <netinet/in.h>

namespace config {

    struct RadioClientConfig
    {
        std::string url; // (-u) MUST EXIST
        bool multiplexing_enabled = false; // (-m) OPTIONAL, default: false
        int client_timeout_ms = 5000; // (-t) OPTIONAL, from 100 to 100000
        bool ipv6_forced = false; // (-6) OPTIONAL, default: false
        bool ipv4_forced= false; // (-4) OPTIONAL, default: false
        uint8_t verbosity = 2; // (-v) OPTIONAL, from 0 to 4, default: 2, -q means -v0
    };

    struct RadioUrlParts
    {
        std::string host;
        std::string port;
        std::string path;
    };

    [[nodiscard]] RadioUrlParts parse_url(std::string_view url);
    [[nodiscard]] int validate_config(const RadioClientConfig& config) noexcept;
    [[nodiscard]] RadioClientConfig parse_arguments(int argc, char* argv[]);

    uint16_t read_port(char const *string);

    std::string prepare_http_get_request(const RadioUrlParts& url_parts);

    ::sockaddr_in get_server_address(const char *host, uint16_t port);

    std::string display_diagnostic_message(const std::string& message, uint8_t verbosity_level, uint8_t current_verbosity);

} // namespace config

