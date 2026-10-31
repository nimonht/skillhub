// SHA-1 (git object ids), SHA-256 (folder digests, the project lock's computedHash) and base64 (export files).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sh_str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    size_t used;
} sh_sha1;

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t used;
} sh_sha256;

void sh_sha1_init(sh_sha1 *c);
void sh_sha1_update(sh_sha1 *c, const void *data, size_t n);
void sh_sha1_final(sh_sha1 *c, unsigned char out[20]);

void sh_sha256_init(sh_sha256 *c);
void sh_sha256_update(sh_sha256 *c, const void *data, size_t n);
void sh_sha256_final(sh_sha256 *c, unsigned char out[32]);

// Lowercase hex of `n` bytes into `out` (2n + 1 bytes).
void sh_hex(const unsigned char *bytes, size_t n, char *out);

void sh_base64_encode(const void *data, size_t n, sh_buf *out);
// Decodes like Qt's QByteArray::fromBase64 without AbortOnBase64DecodingErrors, as export files from older versions expect: every
// character outside the alphabet (padding, line breaks, junk) is skipped and decoding goes on.
void sh_base64_decode_lenient(const char *s, size_t n, sh_buf *out);

#ifdef __cplusplus
}
#endif
