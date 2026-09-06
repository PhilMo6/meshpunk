// The six extern "C" crypto primitives meshtastic-lite's software-fallback
// seam expects (vendor headers built WITHOUT MESH_CRYPTO_USE_MBEDTLS declare
// them and the mtlite module imports them via proto_exports[]). Implemented
// host-side with the firmware's mbedtls 2.28 (hardware AES on the S3) and the
// esp_random TRNG, so the module carries no crypto code and no mbedtls
// headers — the context/config hazards stay on this side of the ABI.

#include <Arduino.h>
#include <esp_random.h>

#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>

extern "C" {

// Single-block AES encrypt: out = AES_encrypt(key, in). key_bits 128 or 256.
// (meshtastic-lite drives its own CTR loop around this.)
void mesh_aes_block_encrypt(const uint8_t *key, int key_bits,
                            const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, (unsigned)key_bits) == 0) {
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    } else {
        memset(out, 0, 16);
    }
    mbedtls_aes_free(&ctx);
}

void mesh_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_ret(data, len, out, 0 /* SHA-256 */);
}

bool mesh_ccm_encrypt(const uint8_t key[32], const uint8_t nonce[13],
                      const uint8_t *plain, size_t plain_len,
                      uint8_t *cipher, uint8_t *tag, size_t tag_len) {
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    bool ok = false;
    if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256) == 0) {
        ok = mbedtls_ccm_encrypt_and_tag(&ctx, plain_len, nonce, 13,
                                         NULL, 0, plain, cipher,
                                         tag, tag_len) == 0;
    }
    mbedtls_ccm_free(&ctx);
    return ok;
}

bool mesh_ccm_decrypt(const uint8_t key[32], const uint8_t nonce[13],
                      const uint8_t *cipher, size_t cipher_len,
                      const uint8_t *tag, size_t tag_len,
                      uint8_t *plain) {
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    bool ok = false;
    if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256) == 0) {
        ok = mbedtls_ccm_auth_decrypt(&ctx, cipher_len, nonce, 13,
                                      NULL, 0, cipher, plain,
                                      tag, tag_len) == 0;
    }
    mbedtls_ccm_free(&ctx);
    return ok;
}

// f_rng bridge to the hardware TRNG (esp_random stays unseeded on purpose —
// see the StdRNG note in setup()).
static int esp_rng_fn(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

// x25519 shared secret over RAW Curve25519 (Montgomery) keys — Meshtastic's
// key format. NOT the vendored ed25519_key_exchange (that expects Edwards
// public keys and would derive a different secret).
bool mesh_x25519_dh(const uint8_t our_private[32],
                    const uint8_t their_public[32],
                    uint8_t shared_out[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Q);

    bool ok = false;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto cleanup;
    if (mbedtls_mpi_read_binary_le(&d, our_private, 32) != 0) goto cleanup;
    if (mbedtls_mpi_read_binary_le(&Q.X, their_public, 32) != 0) goto cleanup;
    if (mbedtls_mpi_lset(&Q.Z, 1) != 0) goto cleanup;
    if (mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, esp_rng_fn, NULL) != 0) goto cleanup;
    if (mbedtls_mpi_write_binary_le(&z, shared_out, 32) != 0) goto cleanup;
    ok = true;

cleanup:
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool mesh_generate_keypair(uint8_t public_key[32], uint8_t private_key[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    bool ok = false;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto cleanup;
    if (mbedtls_ecdh_gen_public(&grp, &d, &Q, esp_rng_fn, NULL) != 0) goto cleanup;
    if (mbedtls_mpi_write_binary_le(&d, private_key, 32) != 0) goto cleanup;
    if (mbedtls_mpi_write_binary_le(&Q.X, public_key, 32) != 0) goto cleanup;
    ok = true;

cleanup:
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

}  // extern "C"
