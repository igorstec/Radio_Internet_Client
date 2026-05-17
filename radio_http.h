#pragma once

#include "radio_client_config.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <openssl/ssl.h>

namespace http_radio {

struct HeaderMap {
    std::map<std::string, std::string> values;

    std::string get(const std::string& key) const;
    void set(std::string key, std::string value);
    bool contains(const std::string& key) const;
};

struct HttpResponseHead {
    std::string raw_status_line;
    int status_code = 0;
    HeaderMap headers;
};

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    bool secure_only = false;
};

class Transport {
public:
    Transport() = default;
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    Transport(Transport&& other) noexcept;
    Transport& operator=(Transport&& other) noexcept;

    void connect_to(const config::RadioUrlParts& url,
                    const config::RadioClientConfig& client_cfg);

    ssize_t read_some(void* buffer, size_t size);
    void write_all(const void* buffer, size_t size);

    int native_fd() const { return fd_; }
    bool is_tls() const { return ssl_ != nullptr; }
    void close();

private:
    int fd_ = -1;
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

struct StreamSession {
    Transport transport;
    config::RadioUrlParts final_url;
    HttpResponseHead response;
    std::optional<size_t> icy_metaint;
};

[[nodiscard]] StreamSession open_stream_session(const config::RadioClientConfig& cfg,
                                                config::RadioUrlParts url);

void stream_audio(StreamSession& session,
                  const config::RadioClientConfig& cfg,
                  volatile sig_atomic_t& finish_flag);

std::string prepare_http_get_request(const RadioUrlParts& url_parts);

} // namespace http_radio