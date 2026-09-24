/*
 * sha256.h — self-contained SHA-256 (FIPS 180-4) for the lib install
 * integrity check. No external dependencies (no OpenSSL): the extension
 * must stay zero-dep per project convention.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUAJIT_SHA256_H
#define LUAJIT_SHA256_H

#include <stddef.h>

/* Raw 32-byte digest. */
void sha256(const void *data, size_t len, unsigned char out[32]);

/* Lowercase hex digest (64 chars + NUL). */
void sha256_hex(const void *data, size_t len, char out[65]);

#endif /* LUAJIT_SHA256_H */
