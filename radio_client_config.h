#pragma once
#include <string>
#include <unistd.h>
#include <iostream>
#include <arpa/inet.h> 
#include <netinet/in.h>

namespace config {

    enum class UrlScheme {
        HTTP,
        HTTPS
    };
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
        UrlScheme scheme = UrlScheme::HTTP;
        std::string host;
        std::string port;
        std::string path;
    };

    struct ResolvedEndpoint {
        sockaddr_storage addr{};
        socklen_t addr_len = 0;
        int family = AF_UNSPEC;
        int socktype = 0;
        int protocol = 0;
    };


    [[nodiscard]] RadioUrlParts parse_url(std::string_view url);
    [[nodiscard]] int validate_config(const RadioClientConfig& config) noexcept;
    [[nodiscard]] RadioClientConfig parse_arguments(int argc, char* argv[]);

    uint16_t read_port(char const *string);

    [[nodiscard]] ResolvedEndpoint get_server_endpoint(const char *host,
                                                    const char *port,
                                                    const RadioClientConfig& config);

    constexpr uint8_t VERBOSITY_COMMUNICATION = 1;
    constexpr uint8_t VERBOSITY_CRITICAL = 2;
    constexpr uint8_t VERBOSITY_NONCRITICAL = 3;
    constexpr uint8_t VERBOSITY_DEBUG = 4;

    void log_comm(std::string_view message, const uint8_t verbosity);
    void log_critical(std::string_view message, const uint8_t verbosity);
    void log_noncritical(std::string_view message, const uint8_t verbosity);
    void log_debug(std::string_view message, const uint8_t verbosity);

    void install_signal_handler(int signal, void (*handler)(int), int flags);
} // namespace config

