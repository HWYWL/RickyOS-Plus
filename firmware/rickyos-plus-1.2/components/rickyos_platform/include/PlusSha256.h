#pragma once
// IDF 6.1 / Mbed TLS 4 removed the public SHA-256 context API. Keep the
// existing RickyOS incremental OTA hashing contract through PSA Crypto.
#include <psa/crypto.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { psa_hash_operation_t operation; } PlusSha256Context;
static inline void plus_sha256_init(PlusSha256Context* ctx) {
  const psa_hash_operation_t initial = PSA_HASH_OPERATION_INIT;
  ctx->operation = initial;
}
static inline void plus_sha256_free(PlusSha256Context* ctx) {
  (void)psa_hash_abort(&ctx->operation);
}
static inline int plus_sha256_starts(PlusSha256Context* ctx, int is224) {
  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) return (int)status;
  status = psa_hash_abort(&ctx->operation);
  if (status != PSA_SUCCESS) return (int)status;
  return (int)psa_hash_setup(&ctx->operation, is224 ? PSA_ALG_SHA_224 : PSA_ALG_SHA_256);
}
static inline int plus_sha256_update(PlusSha256Context* ctx, const unsigned char* data, size_t len) {
  return (int)psa_hash_update(&ctx->operation, data, len);
}
static inline int plus_sha256_finish(PlusSha256Context* ctx, unsigned char output[32]) {
  size_t length = 0;
  return (int)psa_hash_finish(&ctx->operation, output, 32, &length);
}
#ifdef __cplusplus
}
#endif
