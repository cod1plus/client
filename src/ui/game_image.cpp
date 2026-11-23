// game_image.cpp - see game_image.h
#include "ui/game_image.h"

#include <cstdint>
#include <cstring>

namespace patches {

namespace {

uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool ends_with(const char* s, const char* ext) {
    const size_t n = strlen(s), m = strlen(ext);
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i) {
        char a = s[n - m + i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[i]) return false;
    }
    return true;
}

void rgb565(uint16_t c, int out[3]) {
    out[0] = ((c >> 11) & 31) * 255 / 31;
    out[1] = ((c >> 5) & 63) * 255 / 63;
    out[2] = (c & 31) * 255 / 31;
}

// one 4x4 colour block (8 bytes); `dxt1` enables the 3-colour + transparent mode
void color_block(const unsigned char* b, bool dxt1, unsigned char px[16][4]) {
    const uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    int c[4][4];
    rgb565(c0, c[0]); rgb565(c1, c[1]);
    c[0][3] = c[1][3] = 255;
    if (!dxt1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
            c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
        }
        c[2][3] = c[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) { c[2][k] = (c[0][k] + c[1][k]) / 2; c[3][k] = 0; }
        c[2][3] = 255; c[3][3] = 0;
    }
    const uint32_t idx = rd32(b + 4);
    for (int i = 0; i < 16; ++i) {
        const int s = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 4; ++k) px[i][k] = (unsigned char)c[s][k];
    }
}

bool decode_dds(const unsigned char* d, int len, std::vector<unsigned char>& rgba, int& w, int& h) {
    if (len < 128 || memcmp(d, "DDS ", 4) != 0) return false;
    h = (int)rd32(d + 12);
    w = (int)rd32(d + 16);
    const uint32_t pf_flags = rd32(d + 80);
    const uint32_t fourcc = rd32(d + 84);
    const uint32_t bits = rd32(d + 88);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    rgba.assign((size_t)w * h * 4, 0);
    const unsigned char* p = d + 128;
    const unsigned char* end = d + len;

    if (pf_flags & 0x4) {                                   // DDPF_FOURCC: block compressed
        int mode = 0;
        if (fourcc == 0x31545844) mode = 1;                 // "DXT1"
        else if (fourcc == 0x33545844) mode = 3;            // "DXT3"
        else if (fourcc == 0x35545844) mode = 5;            // "DXT5"
        else return false;
        const int bsize = mode == 1 ? 8 : 16;
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (p + (size_t)bw * bh * bsize > end) return false;
        unsigned char px[16][4];
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx, p += bsize) {
                color_block(p + (mode == 1 ? 0 : 8), mode == 1, px);
                if (mode == 3) {
                    for (int i = 0; i < 16; ++i) {
                        const int a = (p[i / 2] >> ((i & 1) * 4)) & 15;
                        px[i][3] = (unsigned char)(a * 17);
                    }
                } else if (mode == 5) {
                    int a[8];
                    a[0] = p[0]; a[1] = p[1];
                    if (a[0] > a[1]) for (int k = 1; k < 7; ++k) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
                    else {
                        for (int k = 1; k < 5; ++k) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
                        a[6] = 0; a[7] = 255;
                    }
                    uint64_t bitsA = 0;
                    for (int k = 0; k < 6; ++k) bitsA |= (uint64_t)p[2 + k] << (8 * k);
                    for (int i = 0; i < 16; ++i) px[i][3] = (unsigned char)a[(bitsA >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; ++i) {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) memcpy(&rgba[((size_t)y * w + x) * 4], px[i], 4);
                }
            }
        return true;
    }
    if ((pf_flags & 0x40) && (bits == 32 || bits == 24)) {  // DDPF_RGB
        const uint32_t mr = rd32(d + 92), mg = rd32(d + 96), mb = rd32(d + 100);
        const uint32_t ma = (pf_flags & 0x1) ? rd32(d + 104) : 0;
        const int bpp = (int)bits / 8;
        if (p + (size_t)w * h * bpp > end) return false;
        auto chan = [](uint32_t v, uint32_t m) -> unsigned char {
            if (!m) return 255;
            int s = 0;
            while (!((m >> s) & 1)) ++s;
            const uint32_t max = m >> s;
            return (unsigned char)(((v & m) >> s) * 255 / (max ? max : 1));
        };
        for (int i = 0; i < w * h; ++i, p += bpp) {
            const uint32_t v = bpp == 4 ? rd32(p) : (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            unsigned char* o = &rgba[(size_t)i * 4];
            o[0] = chan(v, mr); o[1] = chan(v, mg); o[2] = chan(v, mb);
            o[3] = ma ? chan(v, ma) : 255;
        }
        return true;
    }
    return false;
}

bool decode_tga(const unsigned char* d, int len, std::vector<unsigned char>& rgba, int& w, int& h) {
    if (len < 18) return false;
    const int idlen = d[0], cmaptype = d[1], type = d[2];
    const int cmaplen = rd16(d + 5), cmapbits = d[7];
    w = rd16(d + 12); h = rd16(d + 14);
    const int bpp = d[16], desc = d[17];
    if (cmaptype != 0 || w <= 0 || h <= 0) return false;
    if (type != 2 && type != 3 && type != 10 && type != 11) return false;
    const bool gray = type == 3 || type == 11, rle = type >= 9;
    const int bytes = bpp / 8;
    if (gray ? bytes != 1 : (bytes != 3 && bytes != 4)) return false;
    const unsigned char* p = d + 18 + idlen + cmaplen * ((cmapbits + 7) / 8);
    const unsigned char* end = d + len;
    rgba.assign((size_t)w * h * 4, 0);
    const bool top_down = (desc & 0x20) != 0;
    auto put = [&](int i, const unsigned char* s) {
        const int x = i % w, row = i / w;
        const int y = top_down ? row : h - 1 - row;
        unsigned char* o = &rgba[((size_t)y * w + x) * 4];
        if (gray) { o[0] = o[1] = o[2] = s[0]; o[3] = 255; }
        else { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = bytes == 4 ? s[3] : 255; }
    };
    const int total = w * h;
    int i = 0;
    while (i < total) {
        if (!rle) {
            if (p + bytes > end) return false;
            put(i++, p); p += bytes;
            continue;
        }
        if (p >= end) return false;
        const int hdr = *p++;
        const int n = (hdr & 0x7f) + 1;
        if (hdr & 0x80) {
            if (p + bytes > end) return false;
            for (int k = 0; k < n && i < total; ++k) put(i++, p);
            p += bytes;
        } else {
            for (int k = 0; k < n && i < total; ++k) {
                if (p + bytes > end) return false;
                put(i++, p); p += bytes;
            }
        }
    }
    return true;
}

}  // namespace

bool game_image_decode(const char* path, const unsigned char* data, int len,
                       std::vector<unsigned char>& rgba, int& w, int& h) {
    w = h = 0;
    if (!path || !data || len <= 0) return false;
    if (ends_with(path, ".dds")) return decode_dds(data, len, rgba, w, h);
    if (ends_with(path, ".tga")) return decode_tga(data, len, rgba, w, h);
    return false;
}

}  // namespace patches
