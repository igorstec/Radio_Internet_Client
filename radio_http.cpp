#include "radio_http.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <poll.h>
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

bool host_matches_cookie_domain(const std::string& host, const std::string& domain) {
    if (domain.empty()) {
        return false;
    }
    if (host == domain) {
        return true;
    }
    if (host.size() > domain.size() &&
        host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
        host[host.size() - domain.size() - 1] == '.') {
        return true;
    }
    return false;
}

std::string socket_address_to_string(const config::ResolvedEndpoint& ep) {
    char host[INET6_ADDRSTRLEN] = {};
    if (ep.family == AF_INET) {
        auto* a = reinterpret_cast<const sockaddr_in*>(&ep.addr);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        return std::string(host);
    }
    if (ep.family == AF_INET6) {
        auto* a = reinterpret_cast<const sockaddr_in6*>(&ep.addr);
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        return "[" + std::string(host) + "]";
    }
    return "<unknown>";
}

std::string build_get_request(const config::RadioUrlParts& url,
                              bool want_metadata,
                              const std::optional<http_radio::Cookie>& cookie) {
    std::string req;
    req += "GET " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host + "\r\n";
    req += "Connection: Keep-Alive\r\n";
    if (cookie.has_value()) {
        req += "Cookie: " + cookie->name + "=" + cookie->value + "\r\n";
    }
    if (want_metadata) {
        req += "Icy-MetaData: 1\r\n";
    }
    req += "\r\n";
    return req;
}

std::string read_until_double_crlf(http_radio::Transport& t, std::string& extra) {
    std::string data = std::move(extra);
    extra.clear();

    char buf[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = t.read_some(buf, sizeof(buf));
        if (n == 0) {
            throw std::runtime_error("Serwer zamknął połączenie przed końcem nagłówków.");
        }
        if (n < 0) {
            throw std::runtime_error("Błąd odczytu podczas czytania nagłówków.");
        }
        data.append(buf, static_cast<size_t>(n));
    }

    return data;
}

http_radio::HttpResponseHead parse_headers_block(const std::string& raw_block) {
    http_radio::HttpResponseHead out;

    size_t pos = raw_block.find("\r\n");
    out.raw_status_line = raw_block.substr(0, pos);

    if (out.raw_status_line.rfind("ICY ", 0) == 0) {
        auto code_part = out.raw_status_line.substr(4, 3);
        out.status_code = std::stoi(code_part);
    } else if (out.raw_status_line.rfind("HTTP/", 0) == 0) {
        auto first_space = out.raw_status_line.find(' ');
        if (first_space == std::string::npos || first_space + 4 > out.raw_status_line.size()) {
            throw std::runtime_error("Niepoprawna linia statusu HTTP.");
        }
        out.status_code = std::stoi(out.raw_status_line.substr(first_space + 1, 3));
    } else {
        throw std::runtime_error("Nieznana linia statusu: " + out.raw_status_line);
    }

    size_t line_start = pos + 2;
    while (line_start < raw_block.size()) {
        size_t line_end = raw_block.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end == line_start) {
            break;
        }

        std::string line = raw_block.substr(line_start, line_end - line_start);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = to_lower(trim(line.substr(0, colon)));
            std::string value = trim(line.substr(colon + 1));
            out.headers.set(std::move(key), std::move(value));
        }

        line_start = line_end + 2;
    }

    return out;
}

config::RadioUrlParts make_absolute_redirect(const config::RadioUrlParts& base,
                                             const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return config::parse_url(location);
    }

    config::RadioUrlParts next = base;
    if (!location.empty() && location[0] == '/') {
        next.path = location;
    } else {
        next.path = "/" + location;
    }
    return next;
}

std::optional<http_radio::Cookie> parse_set_cookie(const std::string& line,
                                                   const config::RadioUrlParts& current_url) {
    if (line.empty()) {
        return std::nullopt;
    }

    http_radio::Cookie cookie;
    cookie.domain = current_url.host;

    size_t start = 0;
    bool first = true;
    while (start < line.size()) {
        size_t end = line.find(';', start);
        std::string part = trim(line.substr(start, end == std::string::npos ? std::string::npos : end - start));

        if (first) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) {
                return std::nullopt;
            }
            cookie.name = trim(part.substr(0, eq));
            cookie.value = trim(part.substr(eq + 1));
            first = false;
        } else {
            std::string lower = to_lower(part);
            if (lower.rfind("domain=", 0) == 0) {
                cookie.domain = to_lower(trim(part.substr(7)));
                if (!cookie.domain.empty() && cookie.domain[0] == '.') {
                    cookie.domain.erase(cookie.domain.begin());
                }
            } else if (lower == "secure") {
                cookie.secure_only = true;
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

bool should_send_cookie(const std::optional<http_radio::Cookie>& cookie,
                        const config::RadioUrlParts& url) {
    if (!cookie.has_value()) {
        return false;
    }
    if (cookie->secure_only && url.scheme != config::UrlScheme::HTTPS) {
        return false;
    }
    return host_matches_cookie_domain(to_lower(url.host), to_lower(cookie->domain));
}

void wait_for_input(int fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        throw std::runtime_error("data receiving timeout");
    }
    if (rc < 0) {
        throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
    }
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

} // namespace

namespace http_radio {

std::string HeaderMap::get(const std::string& key) const {
    auto it = values.find(to_lower(key));
    return it == values.end() ? "" : it->second;
}

void HeaderMap::set(std::string key, std::string value) {
    values[to_lower(std::move(key))] = std::move(value);
}

bool HeaderMap::contains(const std::string& key) const {
    return values.find(to_lower(key)) != values.end();
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
    auto endpoint = config::get_server_endpoint(url.host.c_str(), url.port.c_str(), client_cfg);

    std::cerr << config::display_diagnostic_message(
        "resolving name " + url.host, 1, client_cfg.verbosity);
    std::cerr << config::display_diagnostic_message(
        "connecting to server " + socket_address_to_string(endpoint) + ":" + url.port,
        1, client_cfg.verbosity);

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
        int rc = SSL_read(ssl_, buffer, static_cast<int>(size));
        if (rc > 0) {
            return rc;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
            return -1;
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

    for (int redirect_count = 0; redirect_count < MAX_REDIRECTS; ++redirect_count) {
        StreamSession session;
        session.final_url = url;
        session.transport.connect_to(url, cfg);

        std::string request = build_get_request(url, cfg.multiplexing_enabled,
                                                should_send_cookie(cookie, url) ? cookie : std::nullopt);

        std::cerr << config::display_diagnostic_message(
            request.substr(0, request.size() - 2), 1, cfg.verbosity);

        session.transport.write_all(request.data(), request.size());

        std::string extra;
        std::string raw = read_until_double_crlf(session.transport, extra);
        size_t header_end = raw.find("\r\n\r\n");
        std::string header_block = raw.substr(0, header_end);
        std::string body_prefix = raw.substr(header_end + 4);

        session.response = parse_headers_block(header_block);

        std::cerr << config::display_diagnostic_message(
            session.response.raw_status_line, 1, cfg.verbosity);

        for (const auto& [k, v] : session.response.headers.values) {
            std::cerr << config::display_diagnostic_message(k + ": " + v, 1, cfg.verbosity);
        }

        std::string set_cookie = session.response.headers.get("set-cookie");
        if (!set_cookie.empty()) {
            auto parsed = parse_set_cookie(set_cookie, url);
            if (parsed.has_value()) {
                cookie = parsed;
            }
        }

        if (session.response.status_code >= 300 && session.response.status_code < 400) {
            std::string location = session.response.headers.get("location");
            if (location.empty()) {
                throw std::runtime_error("Redirect bez nagłówka Location.");
            }
            url = make_absolute_redirect(url, location);
            continue;
        }

        if (session.response.status_code != 200) {
            throw std::runtime_error("Serwer zwrócił kod HTTP/ICY " +
                                     std::to_string(session.response.status_code));
        }

        std::string metaint_header = session.response.headers.get("icy-metaint");
        if (!metaint_header.empty()) {
            session.icy_metaint = static_cast<size_t>(std::stoul(metaint_header));
        }

        if (!body_prefix.empty()) {
            if (session.icy_metaint.has_value() && cfg.multiplexing_enabled) {
                static thread_local std::string unread_prefix;
                unread_prefix = body_prefix;
            } else {
                write_all_fd(STDOUT_FILENO, body_prefix.data(), body_prefix.size());
            }
        }

        return session;
    }

    throw std::runtime_error("Zbyt wiele przekierowań.");
}

void stream_audio(StreamSession& session,
                  const config::RadioClientConfig& cfg,
                  volatile sig_atomic_t& finish_flag) {
    char buffer[4096];

    if (!session.icy_metaint.has_value() || !cfg.multiplexing_enabled) {
        while (!finish_flag) {
            wait_for_input(session.transport.native_fd(), cfg.client_timeout_ms);
            ssize_t n = session.transport.read_some(buffer, sizeof(buffer));
            if (n == 0) {
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                throw std::runtime_error("Błąd odczytu strumienia.");
            }
            write_all_fd(STDOUT_FILENO, buffer, static_cast<size_t>(n));
        }
        return;
    }

    const size_t metaint = *session.icy_metaint;
    std::vector<char> meta_buf(16 * 255);

    while (!finish_flag) {
        size_t audio_left = metaint;
        while (audio_left > 0 && !finish_flag) {
            wait_for_input(session.transport.native_fd(), cfg.client_timeout_ms);
            size_t chunk = std::min(audio_left, sizeof(buffer));
            ssize_t n = session.transport.read_some(buffer, chunk);
            if (n == 0) {
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                throw std::runtime_error("Błąd odczytu danych audio.");
            }
            write_all_fd(STDOUT_FILENO, buffer, static_cast<size_t>(n));
            audio_left -= static_cast<size_t>(n);
        }

        unsigned char meta_len_byte = 0;
        while (!finish_flag) {
            wait_for_input(session.transport.native_fd(), cfg.client_timeout_ms);
            ssize_t n = session.transport.read_some(&meta_len_byte, 1);
            if (n == 0) {
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                throw std::runtime_error("Błąd odczytu długości metadanych.");
            }
            break;
        }

        size_t meta_size = static_cast<size_t>(meta_len_byte) * 16;
        size_t got = 0;
        while (got < meta_size && !finish_flag) {
            wait_for_input(session.transport.native_fd(), cfg.client_timeout_ms);
            ssize_t n = session.transport.read_some(meta_buf.data() + got, meta_size - got);
            if (n == 0) {
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                throw std::runtime_error("Błąd odczytu metadanych.");
            }
            got += static_cast<size_t>(n);
        }

        if (meta_size > 0) {
            while (meta_size > 0 && meta_buf[meta_size - 1] == '\0') {
                --meta_size;
            }
            if (meta_size > 0) {
                write_all_fd(STDERR_FILENO, meta_buf.data(), meta_size);
                write_all_fd(STDERR_FILENO, "\n", 1);
            }
        }
    }
}

} // namespace http_radio