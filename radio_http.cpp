#include "radio_http.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

void write_all_fd(int fd, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    size_t left = size;

    while (left > 0) {
        ssize_t n = ::write(fd, ptr, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
        }
        ptr += n;
        left -= static_cast<size_t>(n);
    }
}

std::string endpoint_to_string(const config::ResolvedEndpoint& ep) {
    char buffer[INET6_ADDRSTRLEN] = {};

    if (ep.family == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(&ep.addr);
        inet_ntop(AF_INET, &a->sin_addr, buffer, sizeof(buffer));
        return std::string(buffer);
    }

    if (ep.family == AF_INET6) {
        auto* a = reinterpret_cast<const sockaddr_in6*>(&ep.addr);
        inet_ntop(AF_INET6, &a->sin6_addr, buffer, sizeof(buffer));
        return "[" + std::string(buffer) + "]";
    }

    return "<unknown>";
}

std::string build_get_request(const config::RadioUrlParts& url,
                              bool want_metadata,
                              const std::optional<radio_http::Cookie>& cookie) {
    std::string request;
    request += "GET " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + url.host + "\r\n";
    request += "Connection: Keep-Alive\r\n";
    if (cookie.has_value()) {
        request += "Cookie: " + cookie->name + "=" + cookie->value + "\r\n";
    }
    if (want_metadata) {
        request += "Icy-MetaData: 1\r\n";
    }
    request += "\r\n";
    return request;
}

radio_http::HttpResponseHead parse_response_headers(const std::string& raw_headers) {
    radio_http::HttpResponseHead result{};

    const size_t first_eol = raw_headers.find("\r\n");
    result.raw_status_line = raw_headers.substr(0, first_eol);

    if (result.raw_status_line.rfind("ICY ", 0) == 0) {
        result.status_code = std::stoi(result.raw_status_line.substr(4, 3));
    } else if (result.raw_status_line.rfind("HTTP/", 0) == 0) {
        const size_t sp = result.raw_status_line.find(' ');
        if (sp == std::string::npos || sp + 4 > result.raw_status_line.size()) {
            throw std::runtime_error("Niepoprawna linia statusu.");
        }
        result.status_code = std::stoi(result.raw_status_line.substr(sp + 1, 3));
    } else {
        throw std::runtime_error("Nieznana linia statusu: " + result.raw_status_line);
    }

    size_t line_begin = first_eol + 2;
    while (line_begin < raw_headers.size()) {
        size_t line_end = raw_headers.find("\r\n", line_begin);
        if (line_end == std::string::npos || line_end == line_begin) {
            break;
        }

        const std::string line = raw_headers.substr(line_begin, line_end - line_begin);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = to_lower(trim(line.substr(0, colon)));
            std::string value = trim(line.substr(colon + 1));
            result.headers.set(std::move(key), std::move(value));
        }

        line_begin = line_end + 2;
    }

    return result;
}

config::RadioUrlParts redirect_target(const config::RadioUrlParts& current,
                                      const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return config::parse_url(location);
    }

    config::RadioUrlParts next = current;
    if (!location.empty() && location.front() == '/') {
        next.path = location;
    } else {
        next.path = "/" + location;
    }
    return next;
}

std::optional<radio_http::Cookie> parse_cookie(const std::string& set_cookie,
                                               const config::RadioUrlParts& current_url) {
    if (set_cookie.empty()) {
        return std::nullopt;
    }

    radio_http::Cookie cookie;
    cookie.domain = current_url.host;

    size_t start = 0;
    bool first = true;

    while (start < set_cookie.size()) {
        const size_t end = set_cookie.find(';', start);
        const std::string part = trim(set_cookie.substr(
            start, end == std::string::npos ? std::string::npos : end - start));

        if (first) {
            const size_t eq = part.find('=');
            if (eq == std::string::npos) {
                return std::nullopt;
            }
            cookie.name = trim(part.substr(0, eq));
            cookie.value = trim(part.substr(eq + 1));
            first = false;
        } else {
            const std::string lower = to_lower(part);
            if (lower.rfind("domain=", 0) == 0) {
                cookie.domain = trim(part.substr(7));
                if (!cookie.domain.empty() && cookie.domain.front() == '.') {
                    cookie.domain.erase(cookie.domain.begin());
                }
            }
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (cookie.name.empty()) {
        return std::nullopt;
    }

    return cookie;
}

bool cookie_matches(const radio_http::Cookie& cookie, const std::string& host) {
    if (host == cookie.domain) {
        return true;
    }

    if (host.size() > cookie.domain.size() &&
        host.compare(host.size() - cookie.domain.size(), cookie.domain.size(), cookie.domain) == 0 &&
        host[host.size() - cookie.domain.size() - 1] == '.') {
        return true;
    }

    return false;
}

void log_text(const config::RadioClientConfig& cfg,
              const std::string& text,
              uint8_t verbosity_level = 1) {
    std::cerr << config::display_diagnostic_message(text, verbosity_level, cfg.verbosity);
}

} // namespace

namespace radio_http {

std::string HeaderMap::get(const std::string& key) const {
    auto it = values.find(to_lower(key));
    if (it == values.end()) {
        return "";
    }
    return it->second;
}

void HeaderMap::set(std::string key, std::string value) {
    values[to_lower(std::move(key))] = std::move(value);
}

Transport::~Transport() {
    close();
}

Transport::Transport(Transport&& other) noexcept {
    *this = std::move(other);
}

Transport& Transport::operator=(Transport&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        ssl_ctx_ = other.ssl_ctx_;
        ssl_ = other.ssl_;
        other.fd_ = -1;
        other.ssl_ctx_ = nullptr;
        other.ssl_ = nullptr;
    }
    return *this;
}

void Transport::connect_to(const config::RadioUrlParts& url,
                           const config::RadioClientConfig& client_cfg) {
    close();

    const auto endpoint = config::get_server_endpoint(url.host.c_str(), url.port.c_str(), client_cfg);

    log_text(client_cfg, "resolving name " + url.host);
    log_text(client_cfg, "connecting to server " + endpoint_to_string(endpoint) + ":" + url.port);

    fd_ = ::socket(endpoint.family, endpoint.socktype, endpoint.protocol);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    if (::connect(fd_,
                  reinterpret_cast<const sockaddr*>(&endpoint.addr),
                  endpoint.addr_len) < 0) {
        throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
    }

    if (url.scheme == config::UrlScheme::HTTPS) {
        SSL_library_init();
        SSL_load_error_strings();

        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            throw std::runtime_error("SSL_CTX_new failed");
        }

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            throw std::runtime_error("SSL_new failed");
        }

        SSL_set_tlsext_host_name(ssl_, url.host.c_str());
        SSL_set_fd(ssl_, fd_);

        if (SSL_connect(ssl_) != 1) {
            throw std::runtime_error("SSL_connect failed");
        }
    }
}

ssize_t Transport::read_some(void* buffer, size_t size) {
    if (ssl_) {
        const int n = SSL_read(ssl_, buffer, static_cast<int>(size));
        if (n > 0) {
            return n;
        }

        const int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }

        throw std::runtime_error("SSL_read failed");
    }

    ssize_t n;
    do {
        n = ::read(fd_, buffer, size);
    } while (n < 0 && errno == EINTR);

    return n;
}

void Transport::write_all(const void* buffer, size_t size) {
    const char* ptr = static_cast<const char*>(buffer);
    size_t left = size;

    while (left > 0) {
        int n;
        if (ssl_) {
            n = SSL_write(ssl_, ptr, static_cast<int>(left));
            if (n <= 0) {
                throw std::runtime_error("SSL_write failed");
            }
        } else {
            do {
                n = static_cast<int>(::write(fd_, ptr, left));
            } while (n < 0 && errno == EINTR);

            if (n < 0) {
                throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
            }
        }

        ptr += n;
        left -= static_cast<size_t>(n);
    }
}

void Transport::close() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }

    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

StreamSession open_stream_session(const config::RadioClientConfig& cfg,
                                  config::RadioUrlParts url) {
    constexpr int MAX_REDIRECTS = 10;
    std::optional<Cookie> cookie;

    for (int redirects = 0; redirects < MAX_REDIRECTS; ++redirects) {
        StreamSession session;
        session.final_url = url;

        session.transport.connect_to(url, cfg);

        std::optional<Cookie> cookie_for_request;
        if (cookie.has_value() && cookie_matches(*cookie, url.host)) {
            cookie_for_request = cookie;
        }

        const std::string request = build_get_request(url, cfg.multiplexing_enabled, cookie_for_request);

        {
            std::string printable = request;
            printable.erase(std::remove(printable.begin(), printable.end(), '\r'), printable.end());
            if (!printable.empty() && printable.back() == '\n') {
                printable.pop_back();
            }
            log_text(cfg, printable);
        }

        session.transport.write_all(request.data(), request.size());

        std::string raw;
        char buf[4096];

        while (raw.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = session.transport.read_some(buf, sizeof(buf));
            if (n <= 0) {
                throw std::runtime_error("Serwer zamknął połączenie przed końcem nagłówków.");
            }
            raw.append(buf, static_cast<size_t>(n));
        }

        const size_t header_end = raw.find("\r\n\r\n");
        const std::string headers_only = raw.substr(0, header_end);
        const std::string body_prefix = raw.substr(header_end + 4);

        session.response = parse_response_headers(headers_only);

        log_text(cfg, session.response.raw_status_line);
        for (const auto& [k, v] : session.response.headers.values) {
            log_text(cfg, k + ": " + v);
        }

        const std::string set_cookie = session.response.headers.get("set-cookie");
        if (!set_cookie.empty()) {
            cookie = parse_cookie(set_cookie, url);
        }

        if (session.response.status_code >= 300 && session.response.status_code < 400) {
            const std::string location = session.response.headers.get("location");
            if (location.empty()) {
                throw std::runtime_error("Redirect bez nagłówka Location.");
            }
            url = redirect_target(url, location);
            continue;
        }

        if (session.response.status_code != 200) {
            throw std::runtime_error("Nieobsługiwany kod odpowiedzi: " +
                                     std::to_string(session.response.status_code));
        }

        const std::string metaint = session.response.headers.get("icy-metaint");
        if (!metaint.empty()) {
            session.icy_metaint = static_cast<size_t>(std::stoul(metaint));
        }

        session.input_buffer.assign(body_prefix.begin(), body_prefix.end());

        if (cfg.multiplexing_enabled && session.icy_metaint.has_value()) {
            session.audio_bytes_until_metadata = *session.icy_metaint;
        }

        return session;
    }

    throw std::runtime_error("Zbyt wiele przekierowań.");
}

void consume_available_data(StreamSession& session,
                            const config::RadioClientConfig& cfg,
                            bool& server_closed) {
    server_closed = false;

    if (session.input_buffer.empty()) {
        char buf[4096];
        const ssize_t n = session.transport.read_some(buf, sizeof(buf));
        if (n == 0) {
            server_closed = true;
            return;
        }
        if (n < 0) {
            throw std::runtime_error("Błąd odczytu danych z połączenia.");
        }
        session.input_buffer.insert(session.input_buffer.end(), buf, buf + n);
    }

    if (!cfg.multiplexing_enabled || !session.icy_metaint.has_value()) {
        if (!session.input_buffer.empty()) {
            write_all_fd(STDOUT_FILENO,
                         session.input_buffer.data(),
                         session.input_buffer.size());
            session.input_buffer.clear();
        }
        return;
    }

    while (!session.input_buffer.empty()) {
        if (session.metadata_bytes_remaining > 0) {
            const size_t chunk = std::min(session.metadata_bytes_remaining,
                                          session.input_buffer.size());

            session.metadata_buffer.insert(session.metadata_buffer.end(),
                                           session.input_buffer.begin(),
                                           session.input_buffer.begin() + static_cast<std::ptrdiff_t>(chunk));

            session.input_buffer.erase(session.input_buffer.begin(),
                                       session.input_buffer.begin() + static_cast<std::ptrdiff_t>(chunk));

            session.metadata_bytes_remaining -= chunk;

            if (session.metadata_bytes_remaining == 0) {
                while (!session.metadata_buffer.empty() &&
                       session.metadata_buffer.back() == '\0') {
                    session.metadata_buffer.pop_back();
                }

                if (!session.metadata_buffer.empty()) {
                    write_all_fd(STDERR_FILENO,
                                 session.metadata_buffer.data(),
                                 session.metadata_buffer.size());
                    write_all_fd(STDERR_FILENO, "\n", 1);
                }

                session.metadata_buffer.clear();
                session.audio_bytes_until_metadata = *session.icy_metaint;
            }

            continue;
        }

        if (session.audio_bytes_until_metadata == 0) {
            const unsigned char len = static_cast<unsigned char>(session.input_buffer.front());
            session.input_buffer.erase(session.input_buffer.begin());
            session.metadata_bytes_remaining = static_cast<size_t>(len) * 16;

            if (session.metadata_bytes_remaining == 0) {
                session.audio_bytes_until_metadata = *session.icy_metaint;
            }
            continue;
        }

        const size_t chunk = std::min(session.audio_bytes_until_metadata,
                                      session.input_buffer.size());

        write_all_fd(STDOUT_FILENO,
                     session.input_buffer.data(),
                     chunk);

        session.input_buffer.erase(session.input_buffer.begin(),
                                   session.input_buffer.begin() + static_cast<std::ptrdiff_t>(chunk));

        session.audio_bytes_until_metadata -= chunk;
    }
}

} // namespace radio_http