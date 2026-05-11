#include "radio_client_config.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>

#include <string>
#include <vector>
#include <poll.h>

const size_t BUFFER_SIZE = 4096;
const size_t CONNECTIONS = 2;
static const char quit_string[] = "quit";

const size_t CLIENT_STDIN_IDX = 0;
const size_t CLIENT_SOCKET_IDX = 1;

static volatile sig_atomic_t finish = 0;

int main(int argc, char* argv[]) {
    auto config = config::parse_arguments(argc, argv);
    auto url_parts = config::parse_url(config.url);


    const char *host = url_parts.host.c_str();
    uint16_t port = std::stoi(url_parts.port);
    struct sockaddr_in server_address = config::get_server_address(host, port);
    bool reconnect_needed = true;
    while (reconnect_needed && !finish) {

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        throw std::runtime_error("Błąd podczas tworzenia gniazda: " + std::string(strerror(errno)));
    }

    if (connect(socket_fd, (struct sockaddr *) &server_address,
                (socklen_t) sizeof(server_address)) < 0) {
        throw std::runtime_error("Błąd podczas łączenia się z serwerem: " + std::string(strerror(errno)));
    }

    std::cerr<< "Połączono z serwerem " << host << " na porcie " << port << std::endl;


    //NOw we have to send a GET request to the server, but first we have to prepare the request string
    std::string request = config::prepare_http_get_request(url_parts);


    pollfd poll_descriptors[CONNECTIONS];
    // We set one descriptor for standard input and he 2 for the socket
    poll_descriptors[CLIENT_STDIN_IDX].fd = STDIN_FILENO;
    poll_descriptors[CLIENT_STDIN_IDX].events = POLLIN;
    poll_descriptors[CLIENT_SOCKET_IDX].fd = socket_fd;
    poll_descriptors[CLIENT_SOCKET_IDX].events = POLLIN;

    static char buffer[BUFFER_SIZE];

    // Send the GET request to the server
    ssize_t sent_bytes = send(socket_fd, request.c_str(), request.size(), 0);
    if (sent_bytes < 0) {
        throw std::runtime_error("Błąd podczas wysyłania danych do serwera: " + std::string(strerror(errno)));
    }

    do{
         for (int i = 0; i < CONNECTIONS; ++i) {
            poll_descriptors[i].revents = 0;
        }

        // After Ctrl-C the to-server socket is closed.
        if (finish && poll_descriptors[CLIENT_SOCKET_IDX].fd >= 0) {
            close(poll_descriptors[CLIENT_SOCKET_IDX].fd);
            poll_descriptors[CLIENT_STDIN_IDX].fd = -1;
        }

        int poll_status = poll(poll_descriptors, CONNECTIONS, config.client_timeout_ms);
        if (poll_status == -1 ) {
            if (errno == EINTR) {
                printf("interrupted system call\n");
            }
            else {
                throw std::runtime_error("Błąd podczas oczekiwania na dane: " + std::string(strerror(errno)));
            }
        }else if (poll_status > 0){

            if (poll_descriptors[CLIENT_STDIN_IDX].fd != -1 && (poll_descriptors[CLIENT_STDIN_IDX].revents & (POLLIN | POLLERR)))
            {
                ssize_t received_bytes = read(poll_descriptors[CLIENT_STDIN_IDX].fd, buffer, BUFFER_SIZE - 1);

                if (received_bytes > 0)
                {
                    buffer[received_bytes] = '\0';

                    if (strncmp(buffer, quit_string, 5) == 0)
                    {
                        finish = 1;
                        std::cerr << "Zakończenie klienta...\n";
                    }
                }
            }
            if (poll_descriptors[CLIENT_SOCKET_IDX].fd != -1 && (poll_descriptors[CLIENT_SOCKET_IDX].revents & (POLLIN | POLLERR)))
            {
                ssize_t received_bytes = read(poll_descriptors[CLIENT_SOCKET_IDX].fd, buffer, BUFFER_SIZE - 1);

                if (received_bytes > 0) {
                    write(STDOUT_FILENO, buffer, received_bytes);
                } else if (received_bytes == 0) {
                    std::cerr << "Serwer zamknął połączenie.\n";
                    finish = 1;
                    reconnect_needed = false;
                } else {
                    std::cerr << "Błąd odczytu z socketu.\n";
                    finish = 1;
                    reconnect_needed = false;
                }
            }
        }else{
            // poll_status == 0 - timeout
            std::cerr << "Brak danych do odczytu po upływie timeoutu (" << config.client_timeout_ms << " ms). Ponawiam połączenie...\n";
            reconnect_needed = true; 
            
            // Wychodzimy z pętli, żeby zamknąć stare gniazdo i otworzyć nowe.
            break;
        }
    }while(!finish);

    if (poll_descriptors[CLIENT_SOCKET_IDX].fd >= 0) {
        close(poll_descriptors[CLIENT_SOCKET_IDX].fd);
    }
}
    return 0;
}