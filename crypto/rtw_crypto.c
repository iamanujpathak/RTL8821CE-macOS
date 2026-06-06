/*
 * rtw_crypto.c — SHA1 / HMAC-SHA1 / PBKDF2-SHA1 / AES-128-ECB.
 * Standard, self-contained reference implementations (no libc beyond mem*,
 * no heap). Public-domain algorithms; intended to compile unchanged in both
 * userspace and the kernel. See rtw_crypto.h.
 */
#include "rtw_crypto.h"

#if defined(KERNEL) || defined(__KERNEL__)
#include <string.h>   /* kernel provides memcpy/memset */
#else
#include <string.h>
#endif

/* ------------------------------------------------------------------ SHA-1 */

typedef struct { uint32_t h[5]; uint64_t bits; uint8_t buf[64]; uint32_t n; } sha1_ctx;

static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    /* a,b,c,d,e named a,b,d,e,f here ('c' is the ctx) — keep mapping consistent */
    {
        uint32_t aa = a, bb = b, cc = d, dd = e, ee = f;
        for (i = 0; i < 80; i++) {
            if (i < 20)      { f = (bb & cc) | (~bb & dd);            k = 0x5A827999; }
            else if (i < 40) { f = bb ^ cc ^ dd;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (bb & cc) | (bb & dd) | (cc & dd); k = 0x8F1BBCDC; }
            else             { f = bb ^ cc ^ dd;                     k = 0xCA62C1D6; }
            t = rol32(aa, 5) + f + ee + k + w[i];
            ee = dd; dd = cc; cc = rol32(bb, 30); bb = aa; aa = t;
        }
        c->h[0] += aa; c->h[1] += bb; c->h[2] += cc; c->h[3] += dd; c->h[4] += ee;
    }
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->bits = 0; c->n = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t len)
{
    c->bits += (uint64_t)len * 8;
    while (len) {
        uint32_t take = 64 - c->n;
        if (take > len) take = (uint32_t)len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint8_t pad = 0x80; uint64_t bits = c->bits; int i;
    sha1_update(c, &pad, 1);
    pad = 0x00;
    while (c->n != 56) sha1_update(c, &pad, 1);
    for (i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (i*8)); sha1_update(c, &b, 1); }
    for (i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24); out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);  out[i*4+3] = (uint8_t)c->h[i];
    }
}

/* --------------------------------------------------------------- HMAC-SHA1 */

void rtw_hmac_sha1(const uint8_t *key, size_t keylen,
                   const uint8_t *msg, size_t msglen, uint8_t out[20])
{
    uint8_t k[64], ipad[64], opad[64], inner[20];
    sha1_ctx c;
    size_t i;
    if (keylen > 64) { sha1_init(&c); sha1_update(&c, key, keylen); sha1_final(&c, k); memset(k+20, 0, 44); }
    else             { memcpy(k, key, keylen); memset(k+keylen, 0, 64-keylen); }
    for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, msglen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

/* ------------------------------------------------------------- PBKDF2-SHA1 */

void rtw_pbkdf2_sha1(const uint8_t *pass, size_t passlen,
                     const uint8_t *salt, size_t saltlen,
                     uint32_t iterations, uint8_t *out, size_t outlen)
{
    uint8_t asalt[64 + 4], U[20], T[20];
    uint32_t block = 1;
    if (saltlen > 64) saltlen = 64;
    while (outlen) {
        size_t i, j, c;
        memcpy(asalt, salt, saltlen);
        asalt[saltlen+0] = (uint8_t)(block >> 24); asalt[saltlen+1] = (uint8_t)(block >> 16);
        asalt[saltlen+2] = (uint8_t)(block >> 8);  asalt[saltlen+3] = (uint8_t)block;
        rtw_hmac_sha1(pass, passlen, asalt, saltlen + 4, U);
        memcpy(T, U, 20);
        for (i = 1; i < iterations; i++) {
            rtw_hmac_sha1(pass, passlen, U, 20, U);
            for (j = 0; j < 20; j++) T[j] ^= U[j];
        }
        c = outlen < 20 ? outlen : 20;
        memcpy(out, T, c);
        out += c; outlen -= c; block++;
    }
}

/* ------------------------------------------------------------------ AES-128 */
/* Compact byte-oriented AES (FIPS-197), 128-bit key, single ECB block. */

static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t rsbox[256];
static int rsbox_ready = 0;
static void build_rsbox(void) { for (int i = 0; i < 256; i++) rsbox[sbox[i]] = (uint8_t)i; rsbox_ready = 1; }

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

/* AES-128 key expansion -> 11 round keys (176 bytes). */
static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
    static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    int i;
    memcpy(rk, key, 16);
    for (i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i-1)*4, 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0]; t[0] = (uint8_t)(sbox[t[1]] ^ rcon[i/4 - 1]);
            t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[tmp];
        }
        rk[i*4+0] = rk[(i-4)*4+0] ^ t[0]; rk[i*4+1] = rk[(i-4)*4+1] ^ t[1];
        rk[i*4+2] = rk[(i-4)*4+2] ^ t[2]; rk[i*4+3] = rk[(i-4)*4+3] ^ t[3];
    }
}

static void add_rk(uint8_t s[16], const uint8_t *rk) { for (int i = 0; i < 16; i++) s[i] ^= rk[i]; }

void rtw_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16]; int r, i;
    aes128_expand(key, rk);
    memcpy(s, in, 16);
    add_rk(s, rk);
    for (r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (i = 0; i < 16; i++) s[i] = sbox[s[i]];                 /* SubBytes */
        /* ShiftRows (state is column-major: s[col*4+row]) */
        t[0]=s[0]; t[4]=s[4]; t[8]=s[8]; t[12]=s[12];
        t[1]=s[5]; t[5]=s[9]; t[9]=s[13]; t[13]=s[1];
        t[2]=s[10];t[6]=s[14];t[10]=s[2]; t[14]=s[6];
        t[3]=s[15];t[7]=s[3]; t[11]=s[7]; t[15]=s[11];
        if (r != 10) {                                              /* MixColumns */
            for (i = 0; i < 4; i++) {
                uint8_t a0=t[i*4],a1=t[i*4+1],a2=t[i*4+2],a3=t[i*4+3];
                s[i*4+0]=(uint8_t)(mul(a0,2)^mul(a1,3)^a2^a3);
                s[i*4+1]=(uint8_t)(a0^mul(a1,2)^mul(a2,3)^a3);
                s[i*4+2]=(uint8_t)(a0^a1^mul(a2,2)^mul(a3,3));
                s[i*4+3]=(uint8_t)(mul(a0,3)^a1^a2^mul(a3,2));
            }
        } else memcpy(s, t, 16);
        add_rk(s, rk + r*16);
    }
    memcpy(out, s, 16);
}

void rtw_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16]; int r, i;
    if (!rsbox_ready) build_rsbox();
    aes128_expand(key, rk);
    memcpy(s, in, 16);
    add_rk(s, rk + 10*16);
    for (r = 9; r >= 0; r--) {
        uint8_t t[16];
        /* InvShiftRows */
        t[0]=s[0]; t[4]=s[4]; t[8]=s[8]; t[12]=s[12];
        t[1]=s[13];t[5]=s[1]; t[9]=s[5]; t[13]=s[9];
        t[2]=s[10];t[6]=s[14];t[10]=s[2]; t[14]=s[6];
        t[3]=s[7]; t[7]=s[11];t[11]=s[15];t[15]=s[3];
        for (i = 0; i < 16; i++) t[i] = rsbox[t[i]];               /* InvSubBytes */
        add_rk(t, rk + r*16);
        if (r != 0) {                                             /* InvMixColumns */
            for (i = 0; i < 4; i++) {
                uint8_t a0=t[i*4],a1=t[i*4+1],a2=t[i*4+2],a3=t[i*4+3];
                s[i*4+0]=(uint8_t)(mul(a0,14)^mul(a1,11)^mul(a2,13)^mul(a3,9));
                s[i*4+1]=(uint8_t)(mul(a0,9)^mul(a1,14)^mul(a2,11)^mul(a3,13));
                s[i*4+2]=(uint8_t)(mul(a0,13)^mul(a1,9)^mul(a2,14)^mul(a3,11));
                s[i*4+3]=(uint8_t)(mul(a0,11)^mul(a1,13)^mul(a2,9)^mul(a3,14));
            }
        } else memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}
