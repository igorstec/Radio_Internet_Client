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

namespace config
{
    [[nodiscard]] RadioUrlParts parse_url(std::string_view url)
    {
        RadioUrlParts result{"", "80", "/"}; // Domyślne wartości

        if (url.starts_with("http://"))
        {
            url.remove_prefix(7);
        }

        auto path_pos = url.find('/');
        if (path_pos != std::string_view::npos)
        {
            result.path = url.substr(path_pos);
            url = url.substr(0, path_pos);
        }

        auto port_pos = url.find(':');
        if (port_pos != std::string_view::npos)
        {
            result.port = url.substr(port_pos + 1);
            result.host = url.substr(0, port_pos);
        }
        else
        {
            result.host = url;
        }

        return result;
    }

    [[nodiscard]] int validate_config(const config::RadioClientConfig &config) noexcept
    {
        if (config.url.empty())
        {
            std::cerr << "Błąd: Parametr obowiązkowy -u (url) nie został podany.\n";
            return 1;
        }
        if (config.client_timeout_ms < 100 || config.client_timeout_ms > 100000)
        {
            std::cerr << "Błąd: Timeout musi być w przedziale [100, 100000].\n";
            return 1;
        }
        if (config.verbosity > 4)
        {
            std::cerr << "Błąd: Verbosity musi być w przedziale [0, 4].\n";
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
                std::cerr << "Użycie: " << argv[0] << " -u url [-m] [-t timeout] [-4] [-6] [-v verbosity] [-q]\n";
                throw std::invalid_argument("Nieprawidłowe argumenty");
                break;
            default:
                abort(); // Nie powinno się zdarzyć
            }
        }

        if (config.url.empty())
        {
            std::cerr << "Błąd: Parametr obowiązkowy -u (url) nie został podany.\n";
            throw std::invalid_argument("Brak wymaganego parametru -u");
        }

        if (validate_config(config) != 0)
        {
            std::cerr << "Błąd: Nieprawidłowa konfiguracja.\n";
            throw std::invalid_argument("Nieprawidłowa konfiguracja");
        }

        std::cout << "--- Konfiguracja klienta ---\n";
        std::cout << "URL: " << config.url << "\n";
        std::cout << "Multiplex: " << (config.multiplexing_enabled ? "TAK" : "NIE") << "\n";
        std::cout << "Timeout: " << config.client_timeout_ms << " ms\n";
        std::cout << "Wymuś IPv4: " << (config.ipv4_forced ? "TAK" : "NIE") << "\n";
        std::cout << "Wymuś IPv6: " << (config.ipv6_forced ? "TAK" : "NIE") << "\n";
        std::cout << "Verbosity: " << config.verbosity << "\n";
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

    ::sockaddr_in get_server_address(const char *host, uint16_t port) {
        struct sockaddr_in server_address;
        memset(&server_address, 0, sizeof(server_address));
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port);

        struct hostent *he = gethostbyname(host);
        if (he == nullptr) {
            throw std::runtime_error("Failed to resolve host: " + std::string(host));
        }
        memcpy(&server_address.sin_addr, he->h_addr_list[0], he->h_length);
        return server_address;
    }

std::string display_diagnostic_message(const std::string& message, uint8_t verbosity_level, uint8_t current_verbosity) {
    if (verbosity_level <= current_verbosity) {
        std::cout << message << std::endl;
    }
    return message;
}

} // namespace config