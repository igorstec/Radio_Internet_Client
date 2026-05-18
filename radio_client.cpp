#include "radio_client_config.h"
#include "radio_http.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace {
    const size_t CONNECTIONS = 2;
    const char quit_string[] = "quit";

    const size_t CLIENT_STDIN_IDX = 0;
    const size_t CLIENT_SOCKET_IDX = 1;

    volatile sig_atomic_t finish = 0;

    void handle_sigint(int) {
        finish = 1;
    }
} // namespace

int main(int argc, char* argv[]) {
    try {
        config::install_signal_handler(SIGINT, handle_sigint, SA_RESTART);

        auto cfg = config::parse_arguments(argc, argv);
        auto url = config::parse_url(cfg.url);

        bool reconnect_needed = true;

        while (reconnect_needed && !finish) {
            reconnect_needed = false;

            // Otwórz sesję strumieniową (łączy się z serwerem, wysyła żądanie i odbiera nagłówki)
            radio_http::StreamSession session = radio_http::open_stream_session(cfg, url);

            bool server_closed = false;

            //
            radio_http::consume_available_data(session, cfg, server_closed);

            if (server_closed) {
                std::cerr << config::display_diagnostic_message(
                    "server closed connection", 1, cfg.verbosity);
                break;
            }

            pollfd poll_descriptors[CONNECTIONS];
            poll_descriptors[CLIENT_STDIN_IDX].fd = STDIN_FILENO;
            poll_descriptors[CLIENT_STDIN_IDX].events = POLLIN;
            poll_descriptors[CLIENT_STDIN_IDX].revents = 0;

            poll_descriptors[CLIENT_SOCKET_IDX].fd = session.transport.native_fd();
            poll_descriptors[CLIENT_SOCKET_IDX].events = POLLIN | POLLERR | POLLHUP;
            poll_descriptors[CLIENT_SOCKET_IDX].revents = 0;

            while (!finish) {
                poll_descriptors[CLIENT_STDIN_IDX].revents = 0;
                poll_descriptors[CLIENT_SOCKET_IDX].revents = 0;

                if (finish && poll_descriptors[CLIENT_SOCKET_IDX].fd >= 0) {
                    session.transport.close();
                    poll_descriptors[CLIENT_SOCKET_IDX].fd = -1;
                }

                int poll_status = poll(poll_descriptors,
                                       CONNECTIONS,
                                       cfg.client_timeout_ms);

                if (poll_status < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error("Błąd podczas poll(): " +
                                             std::string(std::strerror(errno)));
                }

                if (poll_status == 0) {
                    std::cerr << config::display_diagnostic_message(
                        "data receiving timeout", 1, cfg.verbosity);
                    reconnect_needed = true;
                    break;
                }

                if (poll_descriptors[CLIENT_STDIN_IDX].revents & POLLIN) {
                    char input[256];
                    ssize_t n = read(STDIN_FILENO, input, sizeof(input) - 1);
                    if (n > 0) {
                        input[n] = '\0';
                        if (std::strncmp(input, quit_string, 4) == 0) {
                            finish = 1;
                            break;
                        }
                    }
                }

                if (finish) {
                    break;
                }

                if (poll_descriptors[CLIENT_SOCKET_IDX].revents & (POLLIN | POLLERR | POLLHUP)) {
                    bool current_server_closed = false;
                    radio_http::consume_available_data(session, cfg, current_server_closed);

                    if (current_server_closed ||
                        (poll_descriptors[CLIENT_SOCKET_IDX].revents & (POLLERR | POLLHUP))) {
                        std::cerr << config::display_diagnostic_message(
                            "server closed connection", 1, cfg.verbosity);
                        finish = 1;
                        break;
                    }
                }
            }

            session.transport.close();
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}