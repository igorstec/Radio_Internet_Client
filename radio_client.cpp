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
#include <string_view>

namespace {
    const size_t CONNECTIONS = 2;
    const char quit_string[] = "quit\n";

    const size_t CLIENT_STDIN_IDX = 0;
    const size_t CLIENT_SOCKET_IDX = 1;

    volatile sig_atomic_t finish = 0;

    void handle_sigint(int) {
        finish = 1;
    }
} // namespace

int main(int argc, char* argv[]) {

    config::RadioClientConfig cfg;
    bool config_ready = false;
    
    try {
        cfg = config::parse_arguments(argc, argv);
        auto url = config::parse_url(cfg.url);
        config_ready = true;

        config::install_signal_handler(SIGINT, handle_sigint, SA_RESTART);
        
        bool reconnect_needed = true;

        while (reconnect_needed && !finish) {
            reconnect_needed = false;

            config::log_debug("opening streaming session", cfg.verbosity);

            // Otwórz sesję strumieniową (łączy się z serwerem, wysyła żądanie i odbiera nagłówki)
            radio_http::StreamSession session = radio_http::open_stream_session(cfg, url);

            pollfd poll_descriptors[CONNECTIONS];
            poll_descriptors[CLIENT_STDIN_IDX].fd = STDIN_FILENO;
            poll_descriptors[CLIENT_STDIN_IDX].events = POLLIN;
            poll_descriptors[CLIENT_STDIN_IDX].revents = 0;

            poll_descriptors[CLIENT_SOCKET_IDX].fd = session.transport.native_fd();
            poll_descriptors[CLIENT_SOCKET_IDX].events = POLLIN | POLLERR | POLLHUP;
            poll_descriptors[CLIENT_SOCKET_IDX].revents = 0;

            std::string stdin_buffer;
            
            while (!finish) {
                poll_descriptors[CLIENT_STDIN_IDX].revents = 0;
                poll_descriptors[CLIENT_SOCKET_IDX].revents = 0;

                int poll_status = poll(poll_descriptors,
                                       CONNECTIONS,
                                       cfg.client_timeout_ms);

                if (poll_status < 0) {
                    if (errno == EINTR) {
                        config::log_debug("poll interrupted by signal", cfg.verbosity);
                        continue;
                    }
                    throw std::runtime_error("Błąd podczas poll(): " +
                                             std::string(std::strerror(errno)));
                }

                if (poll_status == 0) {
                    config::log_comm("data receiving timeout", cfg.verbosity);
                    config::log_debug("reconnecting after timeout", cfg.verbosity);
                    reconnect_needed = true;
                    break;
                }

                if (poll_descriptors[CLIENT_STDIN_IDX].revents & POLLIN) {
                    char input[256];
                    ssize_t n = read(STDIN_FILENO, input, sizeof(input));
                    if (n < 0) {
                        if (errno == EINTR) {
                            config::log_debug("stdin read interrupted by signal", cfg.verbosity);
                        } else {
                            throw std::runtime_error("Błąd odczytu z STDIN: " + std::string(std::strerror(errno)));
                        }
                    } else if (n > 0) {
                        // Doklejamy przeczytane bajty do bufora
                        stdin_buffer.append(input, static_cast<size_t>(n));

                        // Przetwarzamy bufor linijka po linijce
                        size_t pos;
                        while ((pos = stdin_buffer.find('\n')) != std::string::npos) {
                            // Wyciągamy jedną pełną linię (bez znaku nowej linii)
                            std::string line = stdin_buffer.substr(0, pos);
                            
                            // Usuwamy tę linię z bufora (wraz ze znakiem nowej linii)
                            stdin_buffer.erase(0, pos + 1);

                            // Sprawdzamy, czy linia to dokładnie słowo "quit"
                            if (line == "quit") {
                                config::log_debug("received quit command", cfg.verbosity);
                                finish = 1;
                                break;
                            }
                        }

                        if (finish) {
                            break; // Przerywa zewnętrzną pętlę poll, jeśli znaleziono quit
                        }

                        // Zabezpieczenie przed przepełnieniem RAM w przypadku złośliwego spamu bez znaków nowej linii
                        if (stdin_buffer.size() > 4096) {
                            stdin_buffer.clear();
                        }
                    }
                }

                if (finish) {
                    break;
                }

                if (poll_descriptors[CLIENT_SOCKET_IDX].revents & (POLLIN | POLLERR | POLLHUP)) {
                    bool current_server_closed = false;
                    radio_http::consume_available_data(session, cfg, current_server_closed);

                    if (current_server_closed) {
                        config::log_debug("server closed connection", cfg.verbosity);
                        finish = 1;
                        break;
                    }

                    if (poll_descriptors[CLIENT_SOCKET_IDX].revents & (POLLERR | POLLHUP)) {
                        config::log_debug("socket reported POLLERR or POLLHUP", cfg.verbosity);
}
                }
            }

            config::log_debug("closing current session", cfg.verbosity);
            
            // 1. Zabezpieczenie przed ucięciem body_prefix (audio/metadane pobrane przy nagłówkach)
            if (!session.input_buffer.empty()) {
                bool dummy_closed = false;
                // Wywołanie consume_available_data gdy input_buffer nie jest pusty, 
                // gwarantuje zdemultipleksowanie i wypisanie resztek na STDOUT bez czytania z gniazda.
                radio_http::consume_available_data(session, cfg, dummy_closed);
            }

            // 2. Wypisanie "dotychczas odebranych" resztek metadanych przed zamknięciem strumienia
            radio_http::flush_remaining_metadata(session);

            session.transport.close();
        }

        return 0;
    } catch (const std::invalid_argument& e) {
        // Niezalenie do verbosity bo informacje o błędnym wywołaniu (błędnych parametrach) powinny być zawsze wyświetlane
        std::cerr << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        if (config_ready) {
            config::log_critical(std::string("Błąd: ") + e.what(), cfg.verbosity);
        }
        return 1;
    }
}
