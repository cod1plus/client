// pak_checksum.cpp - see pak_checksum.h
#include "features/pak_checksum.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace patches {

namespace {

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// MD4 (RFC 1320) of `data`, the four words xored: Com_BlockChecksum
int32_t block_checksum(std::vector<uint8_t> data) {
    const uint64_t bits = (uint64_t)data.size() * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 0; i < 8; ++i) data.push_back((uint8_t)(bits >> (8 * i)));
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    static const int S1[4] = { 3, 7, 11, 19 }, S2[4] = { 3, 5, 9, 13 }, S3[4] = { 3, 9, 11, 15 };
    static const int O2[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
    static const int O3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    for (size_t off = 0; off < data.size(); off += 64) {
        uint32_t x[16];
        memcpy(x, &data[off], 64);
        const uint32_t aa = a, bb = b, cc = c, dd = d;
        for (int i = 0; i < 16; ++i) {
            const uint32_t t = rotl(a + ((b & c) | (~b & d)) + x[i], S1[i % 4]);
            a = d; d = c; c = b; b = t;
        }
        for (int i = 0; i < 16; ++i) {
            const uint32_t t = rotl(a + ((b & c) | (b & d) | (c & d)) + x[O2[i]] + 0x5a827999u, S2[i % 4]);
            a = d; d = c; c = b; b = t;
        }
        for (int i = 0; i < 16; ++i) {
            const uint32_t t = rotl(a + (b ^ c ^ d) + x[O3[i]] + 0x6ed9eba1u, S3[i % 4]);
            a = d; d = c; c = b; b = t;
        }
        a += aa; b += bb; c += cc; d += dd;
    }
    return (int32_t)(a ^ b ^ c ^ d);
}

}  // namespace

bool pak_checksum(const char* path, int32_t* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long tail = size < 65557 ? size : 65557;              // end of central directory + max comment
    std::vector<uint8_t> t((size_t)(tail > 0 ? tail : 0));
    fseek(f, size - tail, SEEK_SET);
    if (tail >= 22 && fread(t.data(), 1, t.size(), f) == t.size()) {
        for (long i = tail - 22; i >= 0; --i) {
            if (t[i] != 0x50 || t[i + 1] != 0x4b || t[i + 2] != 0x05 || t[i + 3] != 0x06) continue;
            uint32_t cd_size, cd_off;
            memcpy(&cd_size, &t[i + 12], 4);
            memcpy(&cd_off, &t[i + 16], 4);
            if ((long)cd_off + (long)cd_size > size || cd_size > 64u * 1024 * 1024) break;
            std::vector<uint8_t> cd(cd_size);
            fseek(f, (long)cd_off, SEEK_SET);
            if (fread(cd.data(), 1, cd.size(), f) != cd.size()) break;
            std::vector<uint8_t> crcs;
            size_t p = 0;
            while (p + 46 <= cd.size()) {
                if (cd[p] != 0x50 || cd[p + 1] != 0x4b || cd[p + 2] != 0x01 || cd[p + 3] != 0x02) break;
                uint32_t crc, usize;
                uint16_t nlen, xlen, clen;
                memcpy(&crc, &cd[p + 16], 4);
                memcpy(&usize, &cd[p + 24], 4);
                memcpy(&nlen, &cd[p + 28], 2);
                memcpy(&xlen, &cd[p + 30], 2);
                memcpy(&clen, &cd[p + 32], 2);
                if (usize > 0) crcs.insert(crcs.end(), (uint8_t*)&crc, (uint8_t*)&crc + 4);
                p += 46 + (size_t)nlen + xlen + clen;
            }
            *out = block_checksum(crcs);
            ok = true;
            break;
        }
    }
    fclose(f);
    return ok;
}

}  // namespace patches
