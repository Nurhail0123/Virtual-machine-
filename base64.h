// ============================================================================
// base64.h — encoder Base64 minimal, dependency-free
// ============================================================================
// Dipakai utk menyimpan bytecode biner (.luavmc) sbg teks ASCII cetak aman-
// transfer (.luavmc.b64), krn bytecode mentah berisi byte NUL/CR/LF yg rawan
// rusak kalau melewati jalur apa pun yg memperlakukan data sbg teks alih-alih
// byte mentah (mis. copy-paste, beberapa mekanisme upload/download file chat).
// ============================================================================
#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace vmc {

inline std::string base64Encode(const std::vector<uint8_t>& data) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        out.push_back(table[n & 0x3F]);
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = data[i] << 16;
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (data[i] << 16) | (data[i+1] << 8);
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

} // namespace vmc
