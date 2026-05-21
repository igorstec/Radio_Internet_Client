#pragma once

#include "radio_client_config.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <openssl/ssl.h>

namespace radio_http {

struct HeaderMap {
    std::multimap<std::string, std::string> values;

    std::string get(const std::string& key) const;
    std::vector<std::string> get_all(const std::string& key) const;
    void set(std::string key, std::string value);
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

    std::vector<char> input_buffer;
    size_t audio_bytes_until_metadata = 0;
    size_t metadata_bytes_remaining = 0;
    std::vector<char> metadata_buffer;
};

struct ConsumeResult {
    bool server_closed = false;
    bool received_new_bytes = false;
};

[[nodiscard]] StreamSession open_stream_session(const config::RadioClientConfig& cfg,
                                                config::RadioUrlParts url);

ConsumeResult consume_available_data(StreamSession& session,
                                     const config::RadioClientConfig& cfg,
                                     bool attempt_read = true);

void flush_remaining_metadata(StreamSession& session);

} // namespace radio_http