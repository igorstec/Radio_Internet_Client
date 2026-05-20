#include "radio_client_config.h"
#include <string>
#include <unistd.h>
#include <iostream>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <arpa/inet.h> 
#include <netinet/in.h>
#include <stdexcept>
#include <algorithm>
#include <signal.h>
#include <string_view>

namespace {

void display_diagnostic_message(std::ostream& out,
                                const std::string_view message,
                                uint8_t level,
                                uint8_t current_verbosity) {
    if (current_verbosity >= level) {
        out << message << '\n';
    }
}

} // namespace
namespace config
{
    [[nodiscard]] RadioUrlParts parse_url(std::string_view url)
    {
        RadioUrlParts result{}; // Domyślne wartości

        if (url.starts_with("https://")) {
            result.scheme = UrlScheme::HTTPS;
            result.port = "443";
            url.remove_prefix(8);
        } else if (url.starts_with("http://")) {
            result.scheme = UrlScheme::HTTP;
            result.port = "80";
            url.remove_prefix(7);
        } else {
            result.scheme = UrlScheme::HTTP;
            result.port = "80";
        }

        auto path_pos = url.find('/');
        if (path_pos != std::string_view::npos) {
            result.path = std::string(url.substr(path_pos));
            url = url.substr(0, path_pos);
        } else {
            result.path = "/";
        }

        auto port_pos = url.rfind(':');
        if (port_pos != std::string_view::npos) {
            result.host = std::string(url.substr(0, port_pos));
            result.port = std::string(url.substr(port_pos + 1));
        } else {
            result.host = std::string(url);
        }

        if (result.host.empty()) {
            throw std::invalid_argument("Błąd: Host w URL nie może być pusty.\n");
        }


        return result;
    }

    [[nodiscard]] int validate_config(const config::RadioClientConfig &config) noexcept
    {
        if (config.url.empty())
        {
            // Niezalenie do verbosity bo informacje o błędnym wywołaniu (błędnych parametrach) powinny być zawsze wyświetlane
            throw std::invalid_argument("Błąd: Parametr obowiązkowy -u (url) nie został podany.\n");
            return 1;
        }
        if (config.client_timeout_ms < 100 || config.client_timeout_ms > 100000)
        {
            // Niezalenie do verbosity bo informacje o błędnym wywołaniu (błędnych parametrach) powinny być zawsze wyświetlane
            throw std::invalid_argument("Błąd: Timeout musi być w przedziale [100, 100000].\n");
            return 1;
        }
        if (config.verbosity > 4)
        {
            // Niezalenie do verbosity bo informacje o błędnym wywołaniu (błędnych parametrach) powinny być zawsze wyświetlane
            throw std::invalid_argument("Błąd: Verbosity musi być w przedziale [0, 4].\n");
            return 1;
        }
        return 0; // Wszystko OK
    }

    [[nodiscard]] RadioClientConfig parse_arguments(int argc, char *argv[])
    {
        RadioClientConfig config;
        const char *optstring = "u:mt:46v:q";
        int opt;
        while ((opt = getopt(argc, argv, optstring)) != -1)
        {
            switch (opt)
            {
            case 'u':
                config.url = optarg;
                break;
            case 'm':
                config.multiplexing_enabled = true;
                break;
            case 't':
                config.client_timeout_ms = std::atoi(optarg);
                break;
            case '4':
                config.ipv4_forced = true;
                break;
            case '6':
                config.ipv6_forced = true;
                break;
            case 'v':
                config.verbosity = std::atoi(optarg);
                break;
            case 'q':
                config.verbosity = 0; // Skrót dla parametru -v0
                break;
            case '?':
                throw std::invalid_argument("Użycie: " + std::string(argv[0]) + " -u url [-m] [-t timeout] [-4] [-6] [-v verbosity] [-q]\n");
                break;
            default:
                abort(); // Nie powinno się zdarzyć
            }
        }

        if (config.url.empty())
        {
            throw std::invalid_argument("Błąd: Parametr obowiązkowy -u (url) nie został podany.\n");
        }

        if (validate_config(config) != 0)
        {
            throw std::invalid_argument("Błąd: Nieprawidłowa konfiguracja.\n");
        }
        if (config.verbosity >= VERBOSITY_DEBUG) {
            std::string message;
            message += "=== Konfiguracja klienta ===\n";
            message += "URL: " + config.url + "\n";
            message += "Multiplex: " + std::string(config.multiplexing_enabled ? "TAK" : "NIE") + "\n";
            message += "Timeout: " + std::to_string(config.client_timeout_ms) + " ms\n";
            message += "Wymuś IPv4: " + std::string(config.ipv4_forced ? "TAK" : "NIE") + "\n";
            message += "Wymuś IPv6: " + std::string(config.ipv6_forced ? "TAK" : "NIE") + "\n";
            message += "Verbosity: " + std::to_string(config.verbosity);

            log_debug(message, config.verbosity);
        }
        return config;
    };

    uint16_t read_port(char const *string) {
    char *endptr;
    errno = 0;
    unsigned long port = strtoul(string, &endptr, 10);
    if (errno != 0 || *endptr != 0 || port > UINT16_MAX) {
        throw std::invalid_argument("Invalid port number: " + std::string(string));
    }
    return (uint16_t) port;
    }

    std::string prepare_http_get_request(const RadioUrlParts& url_parts) {
        return "GET " + url_parts.path + " HTTP/1.1\r\nHost: " + url_parts.host + "\r\n\r\n";
    }

    ResolvedEndpoint get_server_endpoint(const char *host,
                                     const char *port,
                                     const RadioClientConfig& config) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (config.ipv4_forced && !config.ipv6_forced) {
        hints.ai_family = AF_INET;
    } else if (config.ipv6_forced && !config.ipv4_forced) {
        hints.ai_family = AF_INET6;
    } else {
        // oba podane albo żaden niepodany -> wybierz pierwszy wynik getaddrinfo
        hints.ai_family = AF_UNSPEC;
    }

    addrinfo *result = nullptr;
    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(
            "Błąd podczas rozwiązywania adresu '" + std::string(host) +
            ":" + std::string(port) + "': " + gai_strerror(rc)
        );
    }

    ResolvedEndpoint endpoint{};

    for (addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
        if (rp->ai_family != AF_INET && rp->ai_family != AF_INET6) {
            continue;
        }

        endpoint.family = rp->ai_family;
        endpoint.socktype = rp->ai_socktype;
        endpoint.protocol = rp->ai_protocol;
        endpoint.addr_len = static_cast<socklen_t>(rp->ai_addrlen);
        std::memcpy(&endpoint.addr, rp->ai_addr, rp->ai_addrlen);
        freeaddrinfo(result);
        return endpoint;
    }

    freeaddrinfo(result);
    throw std::runtime_error("Nie znaleziono poprawnego adresu IPv4/IPv6 dla hosta: " +
                             std::string(host));
}

void log_comm(std::string_view message, const uint8_t verbosity) {
    display_diagnostic_message(std::cerr, message, VERBOSITY_COMMUNICATION, verbosity);
}

void log_critical(std::string_view message, const uint8_t verbosity) {
    display_diagnostic_message(std::cerr, message, VERBOSITY_CRITICAL, verbosity);
}

void log_noncritical(std::string_view message, const uint8_t verbosity) {
    display_diagnostic_message(std::cerr, message, VERBOSITY_NONCRITICAL, verbosity);
}

void log_debug(std::string_view message, const uint8_t verbosity) {
    display_diagnostic_message(std::cerr, message, VERBOSITY_DEBUG, verbosity);
}

void install_signal_handler(int signal, void (*handler)(int), int flags) {
    struct sigaction action;
    sigset_t block_mask;

    sigemptyset(&block_mask);
    action.sa_handler = handler;
    action.sa_mask = block_mask;
    action.sa_flags = flags;

    if (sigaction(signal, &action, NULL) < 0) {
        throw std::runtime_error("Nie można zainstalować handlera sygnału: " + std::string(strerror(errno)));
    }
}


} // namespace config