#include "sh_hash.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void put_be64(unsigned char *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

// ---- SHA-1 (FIPS 180-4) ----

static void sha1_block(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sh_sha1_init(sh_sha1 *c) {
    static const uint32_t iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->used = 0;
}

void sh_sha1_update(sh_sha1 *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += n;
    while (n > 0) {
        const size_t take = n < 64 - c->used ? n : 64 - c->used;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { sha1_block(c->h, c->buf); c->used = 0; }
    }
}

void sh_sha1_final(sh_sha1 *c, unsigned char out[20]) {
    const uint64_t bits = c->len * 8;
    const unsigned char pad = 0x80, zero = 0;
    sh_sha1_update(c, &pad, 1);
    while (c->used != 56) sh_sha1_update(c, &zero, 1);
    unsigned char len[8];
    put_be64(len, bits);
    sh_sha1_update(c, len, 8);
    for (int i = 0; i < 5; ++i) put_be32(out + 4 * i, c->h[i]);
}

// ---- SHA-256 (FIPS 180-4) ----

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void sh_sha256_init(sh_sha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->used = 0;
}

void sh_sha256_update(sh_sha256 *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += n;
    while (n > 0) {
        const size_t take = n < 64 - c->used ? n : 64 - c->used;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { sha256_block(c->h, c->buf); c->used = 0; }
    }
}

void sh_sha256_final(sh_sha256 *c, unsigned char out[32]) {
    const uint64_t bits = c->len * 8;
    const unsigned char pad = 0x80, zero = 0;
    sh_sha256_update(c, &pad, 1);
    while (c->used != 56) sh_sha256_update(c, &zero, 1);
    unsigned char len[8];
    put_be64(len, bits);
    sh_sha256_update(c, len, 8);
    for (int i = 0; i < 8; ++i) put_be32(out + 4 * i, c->h[i]);
}

void sh_hex(const unsigned char *bytes, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = '\0';
}

// ---- base64 ----

void sh_base64_encode(const void *data, size_t n, sh_buf *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = data;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        const char quad[4] = {alphabet[v >> 18], alphabet[(v >> 12) & 63], alphabet[(v >> 6) & 63], alphabet[v & 63]};
        sh_buf_append(out, quad, 4);
    }
    if (n - i == 1) {
        const uint32_t v = (uint32_t)p[i] << 16;
        const char quad[4] = {alphabet[v >> 18], alphabet[(v >> 12) & 63], '=', '='};
        sh_buf_append(out, quad, 4);
    } else if (n - i == 2) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        const char quad[4] = {alphabet[v >> 18], alphabet[(v >> 12) & 63], alphabet[(v >> 6) & 63], '='};
        sh_buf_append(out, quad, 4);
    }
    if (!out->data) sh_buf_append(out, "", 0);
}

void sh_base64_decode_lenient(const char *s, size_t n, sh_buf *out) {
    uint32_t buf = 0;
    int bits = 0;
    if (!out->data) sh_buf_append(out, "", 0);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ch = (unsigned char)s[i];
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+') d = 62;
        else if (ch == '/') d = 63;
        else continue;
        buf = (buf << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            sh_buf_putc(out, (char)(buf >> bits));
            buf &= (1u << bits) - 1;
        }
    }
}
