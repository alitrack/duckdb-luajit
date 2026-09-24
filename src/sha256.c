/*
 * sha256.c — self-contained SHA-256 (FIPS 180-4).
 *
 * Used by the install-mode integrity check: the libs INDEX.v2 carries a
 * per-lib sha256 and the extension verifies the downloaded source BEFORE
 * compiling/registering it (mismatch = hard fail, never cached).
 *
 * Verified against the FIPS test vectors plus DuckDB's built-in sha256()
 * on lib sources (see test/sql/luajit_install.test).
 *
 * SPDX-License-Identifier: MIT
 */

#include "sha256.h"

#include <string.h>

static const unsigned int K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(const unsigned char *p, unsigned int h[8]) {
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = ROTR(w[i-15], 7) ^ ROTR(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned int s1 = ROTR(w[i-2], 17) ^ ROTR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a = h[0], b = h[1], c = h[2], d = h[3];
    unsigned int e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int t1 = hh + S1 + ch + K[i] + w[i];
        unsigned int S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const void *data, size_t len, unsigned char out[32]) {
    unsigned int h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const size_t total = len;             /* bit length needs the ORIGINAL len */
    const unsigned char *p = (const unsigned char *)data;

    while (len >= 64) {
        sha256_block(p, h);
        p += 64;
        len -= 64;
    }
    /* padding: 0x80, zeros up to 56 (mod 64), 8-byte big-endian bit length */
    unsigned char tail[128];
    memcpy(tail, p, len);
    tail[len] = 0x80;
    size_t tail_len = (len + 1 <= 56) ? 64 : 128;
    memset(tail + len + 1, 0, tail_len - len - 1);
    unsigned long long bits = (unsigned long long)total * 8;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha256_block(tail, h);
    if (tail_len == 128)
        sha256_block(tail + 64, h);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)(h[i]);
    }
}

void sha256_hex(const void *data, size_t len, char out[65]) {
    unsigned char raw[32];
    sha256(data, len, raw);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2]   = hexd[raw[i] >> 4];
        out[i*2+1] = hexd[raw[i] & 0xf];
    }
    out[64] = 0;
}
