//
// Created by Adair on 05/09/2025.
//

#ifndef MY_OWN_REDIS_CPP_UTILS_H
#define MY_OWN_REDIS_CPP_UTILS_H
#include <cstdint>
#include <vector>
    void buffer_consume(std::vector<uint8_t> &vector, size_t amount);
    void buffer_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len);
#endif //MY_OWN_REDIS_CPP_UTILS_H