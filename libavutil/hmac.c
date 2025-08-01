/*
 * Copyright (C) 2012 Martin Storsjo
 *
 * This file is part of Librempeg
 *
 * Librempeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Librempeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Librempeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "attributes.h"
#include "error.h"
#include "hmac.h"
#include "md5.h"
#include "sha.h"
#include "sha512.h"
#include "mem.h"

#define MAX_HASHLEN 64
#define MAX_BLOCKLEN 128

typedef void (*hmac_final)(void *ctx, uint8_t *dst);
typedef void (*hmac_update)(void *ctx, const uint8_t *src, size_t len);
typedef void (*hmac_init)(void *ctx);

struct AVHMAC {
    void *hash;
    int blocklen, hashlen;
    hmac_final  final;
    hmac_update update;
    hmac_init   init;
    uint8_t key[MAX_BLOCKLEN];
    int keylen;
};

#define DEFINE_ALGO_BITS_INIT(prefix, bits)        \
static av_cold void prefix##bits##_init(void *ctx) \
{                                                  \
    av_##prefix##_init(ctx, bits);                 \
}

#define DEFINE_ALGO_INIT(prefix)             \
static av_cold void prefix##_init(void *ctx) \
{                                            \
    av_##prefix##_init(ctx);                 \
}

#define DEFINE_ALGO(prefix)                                            \
static void prefix##_update(void *ctx, const uint8_t *src, size_t len) \
{                                                                      \
    av_##prefix##_update(ctx, src, len);                               \
}                                                                      \
static void prefix##_final(void *ctx, uint8_t *dst)                    \
{                                                                      \
    av_##prefix##_final(ctx, dst);                                     \
}

DEFINE_ALGO_INIT(md5)
DEFINE_ALGO_BITS_INIT(sha, 160)
DEFINE_ALGO_BITS_INIT(sha, 224)
DEFINE_ALGO_BITS_INIT(sha, 256)
DEFINE_ALGO_BITS_INIT(sha512, 384)
DEFINE_ALGO_BITS_INIT(sha512, 512)
DEFINE_ALGO(md5)
DEFINE_ALGO(sha)
DEFINE_ALGO(sha512)

AVHMAC *av_hmac_alloc(enum AVHMACType type)
{
    AVHMAC *c = av_mallocz(sizeof(*c));
    if (!c)
        return NULL;
    switch (type) {
    case AV_HMAC_MD5:
        c->blocklen = 64;
        c->hashlen  = 16;
        c->init     = md5_init;
        c->update   = md5_update;
        c->final    = md5_final;
        c->hash     = av_md5_alloc();
        break;
    case AV_HMAC_SHA1:
        c->blocklen = 64;
        c->hashlen  = 20;
        c->init     = sha160_init;
        c->update   = sha_update;
        c->final    = sha_final;
        c->hash     = av_sha_alloc();
        break;
    case AV_HMAC_SHA224:
        c->blocklen = 64;
        c->hashlen  = 28;
        c->init     = sha224_init;
        c->update   = sha_update;
        c->final    = sha_final;
        c->hash     = av_sha_alloc();
        break;
    case AV_HMAC_SHA256:
        c->blocklen = 64;
        c->hashlen  = 32;
        c->init     = sha256_init;
        c->update   = sha_update;
        c->final    = sha_final;
        c->hash     = av_sha_alloc();
        break;
    case AV_HMAC_SHA384:
        c->blocklen = 128;
        c->hashlen  = 48;
        c->init     = sha512384_init;
        c->update   = sha512_update;
        c->final    = sha512_final;
        c->hash     = av_sha512_alloc();
        break;
    case AV_HMAC_SHA512:
        c->blocklen = 128;
        c->hashlen  = 64;
        c->init     = sha512512_init;
        c->update   = sha512_update;
        c->final    = sha512_final;
        c->hash     = av_sha512_alloc();
        break;
    default:
        av_free(c);
        return NULL;
    }
    if (!c->hash) {
        av_free(c);
        return NULL;
    }
    return c;
}

void av_hmac_free(AVHMAC *c)
{
    if (!c)
        return;
    av_freep(&c->hash);
    av_free(c);
}

void av_hmac_init(AVHMAC *c, const uint8_t *key, unsigned int keylen)
{
    int i;
    uint8_t block[MAX_BLOCKLEN];
    if (keylen > c->blocklen) {
        c->init(c->hash);
        c->update(c->hash, key, keylen);
        c->final(c->hash, c->key);
        c->keylen = c->hashlen;
    } else {
        memcpy(c->key, key, keylen);
        c->keylen = keylen;
    }
    c->init(c->hash);
    for (i = 0; i < c->keylen; i++)
        block[i] = c->key[i] ^ 0x36;
    for (i = c->keylen; i < c->blocklen; i++)
        block[i] = 0x36;
    c->update(c->hash, block, c->blocklen);
}

void av_hmac_update(AVHMAC *c, const uint8_t *data, unsigned int len)
{
    c->update(c->hash, data, len);
}

int av_hmac_final(AVHMAC *c, uint8_t *out, unsigned int outlen)
{
    uint8_t block[MAX_BLOCKLEN];
    int i;
    if (outlen < c->hashlen)
        return AVERROR(EINVAL);
    c->final(c->hash, out);
    c->init(c->hash);
    for (i = 0; i < c->keylen; i++)
        block[i] = c->key[i] ^ 0x5C;
    for (i = c->keylen; i < c->blocklen; i++)
        block[i] = 0x5C;
    c->update(c->hash, block, c->blocklen);
    c->update(c->hash, out, c->hashlen);
    c->final(c->hash, out);
    return c->hashlen;
}

int av_hmac_calc(AVHMAC *c, const uint8_t *data, unsigned int len,
                 const uint8_t *key, unsigned int keylen,
                 uint8_t *out, unsigned int outlen)
{
    av_hmac_init(c, key, keylen);
    av_hmac_update(c, data, len);
    return av_hmac_final(c, out, outlen);
}
