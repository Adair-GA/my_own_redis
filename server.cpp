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

enum Result {
    NOT_FOUND,
    SUCCESS,
    COMMAND_ERROR,
};


struct Connection {
    int fd = -1;
    Want want = NONE;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

struct Response {
    Result status;
    std::vector<uint8_t> data;
};

static std::map<std::string, std::string> data_store;

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

uint32_t read_u32(const uint8_t *data, const uint8_t *end) {
    if (end - data < 4) {
        return -1;
    }

    uint32_t result = 0;
    memcpy(&result, data, 4);
    return result;
}

int32_t parse_request(uint8_t *request_data, size_t length, std::vector<std::string> &outgoing) {
    uint32_t string_number = 0;
    uint8_t *end = request_data + length;

    string_number = read_u32(request_data, end);

    if (string_number == -1 || string_number > k_max_msg) {
        return -1;
    }
    request_data += 4;

    for (int i = 0; i < string_number; ++i) {
        uint32_t string_length = 0;
        string_length = read_u32(request_data, end);

        if (string_length == -1) {
            return -1;
        }

        std::string string;
        string.assign(reinterpret_cast<const char *>(request_data + 4), string_length);
        outgoing.push_back(string);
        request_data += string_length + 4;
    }

    if (request_data != end) {
        std::cout << "El puntero no coincide con el final. Hay más datos?";
        return -1; // trailing garbage
    }
    return 0;
}

static void do_request(std::vector<std::string> &cmd, Response &out) {
    const std::string &command = cmd[0];

    if (command == "get") {
        if (cmd.size() != 2) {
            out.status = COMMAND_ERROR;
            return;
        }
        auto value = data_store.find(cmd[1]);
        if (value == data_store.end()) {
            out.status = NOT_FOUND;
        } else {
            out.status = SUCCESS;
            out.data.assign(value->second.begin(), value->second.end());
        }
    } else if (command == "put") {
        if (cmd.size() != 3) {
            out.status = COMMAND_ERROR;
        }
        data_store.insert_or_assign(cmd[1], cmd[2]);
        out.status = SUCCESS;
    } else if (command == "del") {
        if (cmd.size() != 2) {
            out.status = COMMAND_ERROR;
        }
        data_store.erase(cmd[1]);
        out.status = SUCCESS;
    } else {
        out.status = COMMAND_ERROR;
    }
}

void serialize_response(Response *response, std::vector<uint8_t> &output_buffer) {
    const uint32_t response_len = 4 + static_cast<uint32_t>(response->data.size());
    buffer_append(output_buffer, reinterpret_cast<const uint8_t *>(&response_len), 4);
    buffer_append(output_buffer, reinterpret_cast<const uint8_t *>(&response->status), 4);
    buffer_append(output_buffer, response->data.data(), response->data.size());
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

    uint8_t *request_data = connection->incoming.data() + 4;

    std::vector<std::string> commands;
    if (!parse_request(request_data, length, commands)) {
        buffer_consume(connection->incoming, length + 4);
    }
    auto *response = new Response();
    do_request(commands, *response);
    serialize_response(response, connection->outgoing);

    delete response;
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
