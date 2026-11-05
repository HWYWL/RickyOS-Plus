#pragma once
// Real host SHA-256, not a constant-success checksum seam.
#include <openssl/sha.h>
struct mbedtls_sha256_context {
  SHA256_CTX context;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { return SHA256_Init(&ctx->context) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* data, size_t size) {
  return SHA256_Update(&ctx->context, data, size) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char* out) {
  return SHA256_Final(out, &ctx->context) == 1 ? 0 : -1;
}
