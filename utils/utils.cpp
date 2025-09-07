//
// Created by Adair on 05/09/2025.
//

#include "utils.h"

void buffer_consume(std::vector<uint8_t> &vector, const size_t amount) {
    vector.erase(vector.begin(), vector.begin() + static_cast<ptrdiff_t>(amount));
}

void buffer_append(std::vector<uint8_t> &buf, const uint8_t *data, const size_t len) {
    buf.insert(buf.end(), data, data + len);
}
