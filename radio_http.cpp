#include "radio_http.h"

#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <poll.h>

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

std::string strip_ipv6_uri_brackets(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        return std::string(host.substr(1, host.size() - 2));
    }
    return std::string(host);
}

bool is_default_port(const config::RadioUrlParts& url) {
    return (url.scheme == config::UrlScheme::HTTP && url.port == "80") ||
           (url.scheme == config::UrlScheme::HTTPS && url.port == "443");
}

std::string format_host_header_authority(const config::RadioUrlParts& url) {
    std::string host = url.host;

    const bool bracketed_ipv6 =
        host.size() >= 2 && host.front() == '[' && host.back() == ']';

    const bool bare_ipv6 =
        !bracketed_ipv6 && host.find(':') != std::string::npos;

    if (bare_ipv6) {
        host = "[" + host + "]";
    }

    if (!is_default_port(url)) {
        host += ":" + url.port;
    }

    return host;
}

struct HeaderReadResult { 
    std::string headers_only; 
    std::string body_prefix; 
};

std::pair<std::size_t, std::size_t> find_header_terminator(const std::string& raw) {
    const std::size_t crlf = raw.find("\r\n\r\n");
    if (crlf != std::string::npos) {
        return {crlf, 4};
    }

    const std::size_t lf = raw.find("\n\n");
    if (lf != std::string::npos) {
        return {lf, 2};
    }

    return {std::string::npos, 0};
}

HeaderReadResult read_response_head(radio_http::Transport& transport, size_t max_size) {
    std::string raw;
    char buf[4096];

    while (true) {
        // 1. Użycie Twojej funkcji sprawdzającej koniec nagłówków! (C++17 structured binding)
        auto [term_pos, term_len] = find_header_terminator(raw);
        if (term_pos != std::string::npos) {
            break; // Mamy całe nagłówki, przerywamy pętlę!
        }

        struct pollfd pfd[2];
        pfd[0].fd = STDIN_FILENO;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = transport.native_fd();
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;

        int pret = ::poll(pfd, 2, 200);
        if (pret < 0 && errno == EINTR) continue;

        if (pret > 0 && (pfd[0].revents & POLLIN)) {
            char sbuf[256];
            ssize_t sn = ::read(STDIN_FILENO, sbuf, sizeof(sbuf));
            if (sn > 0) {
                std::string s(sbuf, sn);
                if (s.find("quit") != std::string::npos) {
                    exit(0); 
                }
            }
        }

        if (pret > 0 && (pfd[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            const ssize_t n = transport.read_some(buf, sizeof(buf));
            if (n <= 0) {
                throw std::runtime_error("Serwer zamknął połączenie przed końcem nagłówków.");
            }
            
            raw.append(buf, static_cast<size_t>(n));
            
            if (raw.size() > max_size) {
                throw std::runtime_error("Przekroczono maksymalny rozmiar nagłówków HTTP.");
            }
        }
    }

    // 2. Ponowne, czyste użycie funkcji do wyciągnięcia metadanych
    auto [header_end, header_len] = find_header_terminator(raw);

    HeaderReadResult res;
    res.headers_only = raw.substr(0, header_end);
    res.body_prefix = raw.substr(header_end + header_len);
    
    return res;
}

radio_http::HttpResponseHead parse_response_headers_relaxed(const std::string& raw_headers) {
    radio_http::HttpResponseHead result{};

    const std::size_t first_eol = raw_headers.find('\n');
    const std::size_t status_len =
        (first_eol == std::string::npos) ? raw_headers.size() : first_eol;

    result.raw_status_line = raw_headers.substr(0, status_len);
    if (!result.raw_status_line.empty() && result.raw_status_line.back() == '\r') {
        result.raw_status_line.pop_back();
    }

    if (result.raw_status_line.rfind("ICY ", 0) == 0) {
        if (result.raw_status_line.size() < 7) {
            throw std::runtime_error("Niepoprawna linia statusu.");
        }
        result.status_code = std::stoi(result.raw_status_line.substr(4, 3));
    } else if (result.raw_status_line.rfind("HTTP/", 0) == 0) {
        const std::size_t sp = result.raw_status_line.find(' ');
        if (sp == std::string::npos || sp + 4 > result.raw_status_line.size()) {
            throw std::runtime_error("Niepoprawna linia statusu.");
        }
        result.status_code = std::stoi(result.raw_status_line.substr(sp + 1, 3));
    } else {
        throw std::runtime_error("Nieznana linia statusu: " + result.raw_status_line);
    }

    if (first_eol == std::string::npos) {
        return result;
    }

    std::size_t line_begin = first_eol + 1;
    while (line_begin < raw_headers.size()) {
        std::size_t line_end = raw_headers.find('\n', line_begin);
        if (line_end == std::string::npos) {
            line_end = raw_headers.size();
        }

        std::string line = raw_headers.substr(line_begin, line_end - line_begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = to_lower(trim(line.substr(0, colon)));
                std::string value = trim(line.substr(colon + 1));
                result.headers.set(std::move(key), std::move(value));
            }
        }

        line_begin = line_end + 1;
    }

    return result;
}

std::string build_get_request(const config::RadioUrlParts& url,
                              bool want_metadata,
                              const std::vector<radio_http::Cookie>& cookies) {
    std::string request;
    request += "GET " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + format_host_header_authority(url) + "\r\n";
    request += "Connection: Keep-Alive\r\n";
    
    if (!cookies.empty()) {
        request += "Cookie: ";
        for (size_t i = 0; i < cookies.size(); ++i) {
            request += cookies[i].name + "=" + cookies[i].value;
            if (i + 1 < cookies.size()) request += "; ";
        }
        request += "\r\n";
    }
    
    if (want_metadata) {
        request += "Icy-MetaData: 1\r\n";
    }
    request += "\r\n";
    return request;
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

} // namespace

namespace radio_http {

std::string HeaderMap::get(const std::string& key) const {
    auto it = values.find(to_lower(key));
    if (it == values.end()) {
        return "";
    }
    return it->second;
}

std::vector<std::string> HeaderMap::get_all(const std::string& key) const {
    std::vector<std::string> result;
    auto range = values.equal_range(to_lower(key));
    for (auto it = range.first; it != range.second; ++it) {
        result.push_back(it->second);
    }
    return result;
}

void HeaderMap::set(std::string key, std::string value) {
    values.insert({to_lower(std::move(key)), std::move(value)});
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

    std::string bare_host = strip_ipv6_uri_brackets(url.host);

    const auto endpoint = config::get_server_endpoint(bare_host.c_str(), url.port.c_str(), client_cfg);

    if (client_cfg.verbosity >= config::VERBOSITY_COMMUNICATION) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now{};
        localtime_r(&time_t_now, &tm_now);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y.%m.%d %H.%M.%S", &tm_now);
        config::log_comm(time_buf, client_cfg.verbosity);
    }
    
    config::log_comm("resolving name " + bare_host, client_cfg.verbosity);
    config::log_comm("connecting to server " + endpoint_to_string(endpoint) + ":" + url.port, client_cfg.verbosity);

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

        SSL_set_tlsext_host_name(ssl_, bare_host.c_str());
        SSL_set_fd(ssl_, fd_);

        config::log_debug("starting TLS handshake with " + bare_host, client_cfg.verbosity);
        if (SSL_connect(ssl_) != 1) {
            throw std::runtime_error("SSL_connect failed");
        }
        config::log_debug("TLS handshake completed", client_cfg.verbosity);
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
    std::vector<Cookie> cookies;

    for (int redirects = 0; redirects < MAX_REDIRECTS; ++redirects) {
        StreamSession session;
        session.final_url = url;

        session.transport.connect_to(url, cfg);

        std::vector<Cookie> cookies_for_request;
        for (const auto& c : cookies) {
            if (cookie_matches(c, url.host)) {
                cookies_for_request.push_back(c);
            }
        }

        const std::string request = build_get_request(url, cfg.multiplexing_enabled, cookies_for_request);

        {
            std::string printable = request;
            // Usuwamy \r. Zostawiamy jeden końcowy \n, żeby razem z nową linią
            // od log_comm dało nam to dwie puste linie (zgodnie z przykładami).
            printable.erase(std::remove(printable.begin(), printable.end(), '\r'), printable.end());
            if (!printable.empty() && printable.back() == '\n') {
                printable.pop_back();
            }
            config::log_comm(printable, cfg.verbosity);
        }

        session.transport.write_all(request.data(), request.size());

        std::string raw;

        const HeaderReadResult header_result =
        read_response_head(session.transport, 16384);

        const std::string& headers_only = header_result.headers_only;
        const std::string& body_prefix = header_result.body_prefix;

        session.response = parse_response_headers_relaxed(headers_only);

        {
            std::string printable_headers = headers_only;
            printable_headers.erase(std::remove(printable_headers.begin(), printable_headers.end(), '\r'), printable_headers.end());
            config::log_comm(printable_headers, cfg.verbosity);
        }

        const auto set_cookies = session.response.headers.get_all("set-cookie");
        for (const auto& sc : set_cookies) {
            auto parsed = parse_cookie(sc, url);
            if (parsed.has_value()) {
                config::log_debug("stored cookie " + parsed->name +
                                  " for domain " + parsed->domain, cfg.verbosity);
                
                bool found = false;
                for (auto& existing : cookies) {
                    if (existing.name == parsed->name && existing.domain == parsed->domain) {
                        existing = *parsed;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    cookies.push_back(*parsed);
                }
            } else {
                config::log_noncritical("ignoring malformed Set-Cookie header", cfg.verbosity);
            }
        }

        if (session.response.status_code >= 300 && session.response.status_code < 400) {
            const std::string location = session.response.headers.get("location");
            if (location.empty()) {
                throw std::runtime_error("Redirect bez nagłówka Location.");
            }

            config::log_debug("redirecting to " + location, cfg.verbosity);
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
            config::log_debug("parsed icy-metaint=" + std::to_string(*session.icy_metaint), cfg.verbosity);
        } else if (cfg.multiplexing_enabled) {
            config::log_noncritical("server ignored Icy-MetaData request", cfg.verbosity);
        }

        session.input_buffer.assign(body_prefix.begin(), body_prefix.end());

        if (cfg.multiplexing_enabled && session.icy_metaint.has_value()) {
            session.audio_bytes_until_metadata = *session.icy_metaint;
        }

        return session;
    }

    throw std::runtime_error("Zbyt wiele przekierowań.");
}

ConsumeResult consume_available_data(StreamSession& session,
                                     const config::RadioClientConfig& cfg,
                                     bool attempt_read) {
    ConsumeResult res;
    
    // 1. Odczyt z gniazda tylko jeśli zostaliśmy o to poproszeni
    if (attempt_read) {
        char buf[16384]; // Zwiększamy bufor dla lepszej wydajności
        const ssize_t n = session.transport.read_some(buf, sizeof(buf));
        
        if (n == 0) {
            res.server_closed = true;
            return res;
        }
        if (n < 0) {
            throw std::runtime_error("Błąd odczytu danych z połączenia.");
        }
        session.input_buffer.insert(session.input_buffer.end(), buf, buf + n);
        res.received_new_bytes = true;
    }

    // 2. Jeśli nie ma multipleksowania, po prostu zrzucamy wszystko na STDOUT
    if (!cfg.multiplexing_enabled || !session.icy_metaint.has_value()) {
        if (!session.input_buffer.empty()) {
            write_all_fd(STDOUT_FILENO, session.input_buffer.data(), session.input_buffer.size());
            session.input_buffer.clear();
        }
        return res;
    }

    // 3. Demultipleksowanie przy użyciu indeksu (zamiast powolnego erase)
    size_t idx = 0;
    while (idx < session.input_buffer.size()) {
        
        if (session.metadata_bytes_remaining > 0) {
            // Jesteśmy w trakcie czytania bloku metadanych
            const size_t chunk = std::min(session.metadata_bytes_remaining,
                                          session.input_buffer.size() - idx);

            session.metadata_buffer.insert(session.metadata_buffer.end(),
                                           session.input_buffer.begin() + idx,
                                           session.input_buffer.begin() + idx + chunk);

            idx += chunk;
            session.metadata_bytes_remaining -= chunk;

            if (session.metadata_bytes_remaining == 0) {
                flush_remaining_metadata(session); // Usuwa zera i wysyła na STDERR
                session.audio_bytes_until_metadata = *session.icy_metaint;
            }
        } 
        else if (session.audio_bytes_until_metadata == 0) {
            // Czytamy bajt długości metadanych
            const unsigned char len = static_cast<unsigned char>(session.input_buffer[idx]);
            idx += 1; // przesuwamy indeks
            session.metadata_bytes_remaining = static_cast<size_t>(len) * 16;

            if (session.metadata_bytes_remaining == 0) {
                // Pusty blok metadanych (częsta sytuacja)
                session.audio_bytes_until_metadata = *session.icy_metaint;
            }
        } 
        else {
            // Jesteśmy w trakcie czytania bloku audio
            const size_t chunk = std::min(session.audio_bytes_until_metadata,
                                          session.input_buffer.size() - idx);

            write_all_fd(STDOUT_FILENO, session.input_buffer.data() + idx, chunk);

            idx += chunk;
            session.audio_bytes_until_metadata -= chunk;
        }
    }
    
    // Usuwamy przetworzone dane z bufora tylko raz na koniec
    session.input_buffer.erase(session.input_buffer.begin(), session.input_buffer.begin() + idx);
    
    return res;
}

void flush_remaining_metadata(StreamSession& session) {
    while (!session.metadata_buffer.empty() && session.metadata_buffer.back() == '\0') {
        session.metadata_buffer.pop_back();
    }
    
    if (!session.metadata_buffer.empty()) {
        write_all_fd(STDERR_FILENO, 
                     session.metadata_buffer.data(), 
                     session.metadata_buffer.size());
        write_all_fd(STDERR_FILENO, "\n", 1);
        session.metadata_buffer.clear();
    }
}

} // namespace radio_http
