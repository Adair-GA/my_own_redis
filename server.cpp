#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <poll.h>
#include <fcntl.h>
#include <map>
#include <ranges>
#include <unistd.h>
#include <arpa/inet.h>

#include "utils/utils.h"

constexpr size_t k_max_msg = 32 << 20;

enum Want {
    READ,
    WRITE,
    CLOSE,
    NONE
};


struct Connection {
    int fd = -1;
    Want want = NONE;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

void fd_set_nonblock(const int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

Connection *handle_accept(int fd) {
    sockaddr_in addr = {};
    socklen_t addr_len = sizeof(addr);
    const int connection_fd = accept(fd, reinterpret_cast<struct sockaddr *>(&addr), &addr_len);

    if (connection_fd == -1) {
        return nullptr;
    }
    fd_set_nonblock(fd);

    const auto connection = new Connection();
    connection->fd = connection_fd;
    connection->want = READ;

    return connection;
}

int init_listening_socket() {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    constexpr int sock_reuse_addr_val = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &sock_reuse_addr_val, sizeof(sock_reuse_addr_val));

    sockaddr_in addr = {};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(1234);

    const int bind_ret = bind(socket_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (bind_ret < 0) {
        exit(-1);
    }

    const int listen_ret = listen(socket_fd, SOMAXCONN);
    if (listen_ret < 0) {
        exit(-1);
    }

    return socket_fd;
}

bool try_parse_request(Connection *connection) {
    if (connection->incoming.size() < 4) {
        return false; //Not enough data
    }

    uint32_t length = 0;
    memcpy(&length, connection->incoming.data(), 4);

    if (length > k_max_msg) {
        connection->want = CLOSE; //Error, close
        return false;
    }

    if (4 + length > connection->incoming.size()) {
        return false; //need more data
    }

    const uint8_t *request_data = connection->incoming.data() + 4;

    //TODO: Crear la respuesta

    //de momento va a ser un echo
    buffer_append(connection->outgoing, reinterpret_cast<const uint8_t *>(&length), 4);
    buffer_append(connection->outgoing, request_data, length);

    // remove the request from the buffer
    buffer_consume(connection->incoming, length + 4);

    return true;
}

void handle_read(Connection *connection) {
    uint8_t buffer[1024 * 64];
    ssize_t bytes_read = read(connection->fd, buffer, sizeof(buffer));

    if (bytes_read <= 0) {
        connection->want = CLOSE;
        return;
    }

    connection->incoming.insert(connection->incoming.end(), buffer, buffer + bytes_read);

    int requests_parsed = 0;

    while (try_parse_request(connection)) {
        requests_parsed++;
    }

    if (requests_parsed == 0) {
        connection->want = READ;
    } else {
        connection->want = WRITE;
    }


    // if (try_parse_request(connection)) {
    //     connection->want = WRITE;
    // } else if (connection->want != CLOSE) {
    //     connection->want = READ;
    // }
}

void handle_write(Connection *connection) {
    if (connection->outgoing.empty()) {
        return;
    }

    const ssize_t bytes_written = write(connection->fd, connection->outgoing.data(), connection->outgoing.size());
    if (bytes_written < 0) {
        if (errno != EAGAIN) {
            connection->want = CLOSE;
        }
        return;
    }
    if (bytes_written == connection->outgoing.size()) {
        connection->want = READ;
    } else {
        connection->want = WRITE;
    }
    buffer_consume(connection->outgoing, bytes_written);
}


int main() {
    std::map<int, Connection *> fd2conn;
    std::vector<pollfd> poll_args;

    const int listen_fd = init_listening_socket();


    while (true) {
        poll_args.clear();

        pollfd pfd = {listen_fd, POLLIN, 0};
        poll_args.push_back(pfd);

        for (const auto &connection: fd2conn | std::views::values) {
            pollfd pollfd = {connection->fd, POLLERR, 0};
            if (connection->want == READ) {
                pollfd.events |= POLLIN;
            }
            if (connection->want == WRITE) {
                pollfd.events |= POLLOUT;
            }
            poll_args.push_back(pollfd);
        }

        //poll the sockets, result is stored in place
        if (const int poll_result = poll(poll_args.data(), poll_args.size(), 500); poll_result < 0) {
            if (errno != EINTR) {
                exit(1);
            }
        }

        //accept any new connections if needed
        if (poll_args[0].revents) {
            if (Connection *connection = handle_accept(listen_fd)) {
                fd2conn[connection->fd] = connection;
            }
        }


        for (int i = 1; i < poll_args.size(); ++i) {
            const pollfd poll_result = poll_args[i];
            short int ready = poll_result.revents;
            const auto connection = fd2conn[poll_result.fd];
            if (ready & POLLIN) {
                handle_read(connection);
            }
            if (ready & POLLOUT) {
                handle_write(connection);
            }
            if (ready & POLLERR || connection->want == CLOSE) {
                printf("Connection closed. Wanted close: %s\n", connection->want == CLOSE ? "yes" : "no");
                (void) close(connection->fd);
                fd2conn.erase(connection->fd);
                delete connection;
            }
        }
    }
}
