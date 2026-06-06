/*
 * rtw_crypto.h — self-contained crypto for the WPA2 4-way handshake, with NO
 * libc / CommonCrypto dependency so the same code compiles in userspace AND in
 * the kernel (the restructure moves the 802.11 MAC, incl. WPA2, into the kext —
 * CommonCrypto is unavailable there). CCMP itself stays in the hardware CAM; we
 * only need these primitives for key derivation + the EAPOL MIC + GTK unwrap.
 *
 * Implementations use only memcpy/memset and fixed/stack buffers (no malloc).
 */
#ifndef RTW_CRYPTO_H
#define RTW_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* HMAC-SHA1 -> 20-byte MAC. Used for the 802.11i PRF and the EAPOL-Key MIC. */
void rtw_hmac_sha1(const uint8_t *key, size_t keylen,
                   const uint8_t *msg, size_t msglen, uint8_t out[20]);

/* PBKDF2-HMAC-SHA1. WPA2-PSK: PMK = pbkdf2(passphrase, SSID, 4096, 32). */
void rtw_pbkdf2_sha1(const uint8_t *pass, size_t passlen,
                     const uint8_t *salt, size_t saltlen,
                     uint32_t iterations, uint8_t *out, size_t outlen);

/* AES-128 single-block ECB. Decrypt is used for RFC-3394 GTK key unwrap. */
void rtw_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void rtw_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif /* RTW_CRYPTO_H */
