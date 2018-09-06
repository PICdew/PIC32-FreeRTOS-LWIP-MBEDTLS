/* pic32mz-crypt.c
 *
 * Copyright (C) 2006-2017 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

#include "pic32mz-crypt.h"

#ifdef WOLFSSL_MICROCHIP_PIC32MZ

#ifdef WOLFSSL_PIC32MZ_CRYPT
#endif

#ifdef WOLFSSL_PIC32MZ_HASH
#endif


#if defined(WOLFSSL_PIC32MZ_CRYPT) || defined(WOLFSSL_PIC32MZ_HASH) 

inline __attribute__((always_inline)) word32 rotlFixed(word32 x, word32 y) {
    return (x << y) | (x >> (sizeof (y) * 8 - y));
}

inline __attribute__((always_inline)) word32 rotrFixed(word32 x, word32 y) {
    return (x >> y) | (x << (sizeof (y) * 8 - y));
}

inline __attribute__((always_inline)) word32 ByteReverseWord32(word32 value) {
    value = ((value & 0xFF00FF00) >> 8) | ((value & 0x00FF00FF) << 8);
    return rotlFixed(value, 16U);
}

void ByteReverseWords(word32* out, const word32* in, word32 byteCount) {
    word32 count = byteCount / (word32)sizeof (word32), i;
    for (i = 0; i < count; i++)
        out[i] = ByteReverseWord32(in[i]);

}

static int Pic32GetBlockSize(int algo) {
    switch (algo) {
        case PIC32_ALGO_HMAC1:
            return PIC32_BLOCKSIZE_HMAC;
        case PIC32_ALGO_SHA256:
            return PIC32_BLOCKSIZE_SHA256;
        case PIC32_ALGO_SHA1:
            return PIC32_BLOCKSIZE_SHA1;
        case PIC32_ALGO_MD5:
            return PIC32_BLOCKSIZE_MD5;
        case PIC32_ALGO_AES:
            return PIC32_BLOCKSIZE_AES;
        case PIC32_ALGO_TDES:
            return PIC32_BLOCKSIZE_TDES;
        case PIC32_ALGO_DES:
            return PIC32_BLOCKSIZE_DES;
    }
    return 0;
}

static int Pic32Crypto(const byte* in, int inLen, word32* out, int outLen,
        int dir, int algo, int cryptoalgo,

        /* For DES/AES only */
        word32* key, int keyLen, word32* iv, int ivLen) {
    int ret = 0;
    int blockSize = Pic32GetBlockSize(algo);
    volatile bufferDescriptor bd __attribute__((aligned(8)));
    securityAssociation sa __attribute__((aligned(8)));
    securityAssociation *sa_p;
    bufferDescriptor *bd_p;
    byte *in_p;
    byte *out_p;
    word32* dst;
    word32 padRemain;
    int timeout = 0xFFFFFF;

    /* check args */
    if (in == NULL || inLen <= 0 || out == NULL || blockSize == 0) {
        return BAD_FUNC_ARG;
    }

    /* check pointer alignment - must be word aligned */
    if (((size_t) in % sizeof (word32)) || ((size_t) out % sizeof (word32))) {
        return BUFFER_E; /* buffer is not aligned */
    }

    /* get uncached address */
    sa_p = KVA0_TO_KVA1(&sa);
    bd_p = KVA0_TO_KVA1(&bd);
    out_p = KVA0_TO_KVA1(out);
    in_p = KVA0_TO_KVA1(in);

    /* Sync cache if in physical memory (not flash) */
    if (PIC32MZ_IF_RAM(in_p)) {
        XMEMCPY(in_p, in, inLen);
    }

    /* Set up the Security Association */
    XMEMSET(sa_p, 0, sizeof (sa));
    sa_p->SA_CTRL.ALGO = algo;
    sa_p->SA_CTRL.ENCTYPE = dir;
    sa_p->SA_CTRL.FB = 1; /* first block */
    sa_p->SA_CTRL.LNC = 1; /* Load new set of keys */
    if (key) {
        /* cipher */
        sa_p->SA_CTRL.CRYPTOALGO = cryptoalgo;

        switch (keyLen) {
            case 32:
                sa_p->SA_CTRL.KEYSIZE = PIC32_KEYSIZE_256;
                break;
            case 24:
            case 8: /* DES */
                sa_p->SA_CTRL.KEYSIZE = PIC32_KEYSIZE_192;
                break;
            case 16:
                sa_p->SA_CTRL.KEYSIZE = PIC32_KEYSIZE_128;
                break;
        }

        dst = (word32*) KVA0_TO_KVA1(sa.SA_ENCKEY +
                (sizeof (sa.SA_ENCKEY) / sizeof (word32)) - (keyLen / sizeof (word32)));
        ByteReverseWords(dst, key, keyLen);

        if (iv && ivLen > 0) {
            sa_p->SA_CTRL.LOADIV = 1;
            dst = (word32*) KVA0_TO_KVA1(sa.SA_ENCIV +
                    (sizeof (sa.SA_ENCIV) / sizeof (word32)) - (ivLen / sizeof (word32)));
            ByteReverseWords(dst, iv, ivLen);
        }
    } else {
        /* hashing */
        sa_p->SA_CTRL.LOADIV = 1;
        sa_p->SA_CTRL.IRFLAG = 0; /* immediate result for hashing */

        dst = (word32*) KVA0_TO_KVA1(sa.SA_AUTHIV +
                (sizeof (sa.SA_AUTHIV) / sizeof (word32)) - (outLen / sizeof (word32)));
        ByteReverseWords(dst, out, outLen);
    }

    /* Set up the Buffer Descriptor */
    XMEMSET(bd_p, 0, sizeof (bd));
    bd_p->BD_CTRL.BUFLEN = inLen;
    padRemain = (inLen % 4); /* make sure buffer is 4-byte multiple */
    if (padRemain != 0) {
        bd_p->BD_CTRL.BUFLEN += (4 - padRemain);
    }
    bd_p->BD_CTRL.SA_FETCH_EN = 1; /* Fetch the security association */
    bd_p->BD_CTRL.PKT_INT_EN = 1; /* enable interrupt */
    bd_p->BD_CTRL.LAST_BD = 1; /* last buffer desc in chain */
    bd_p->BD_CTRL.LIFM = 1; /* last in frame */
    bd_p->SA_ADDR = (unsigned int) KVA_TO_PA(&sa);
    bd_p->SRCADDR = (unsigned int) KVA_TO_PA(in);
    if (key) {
        /* cipher */
        if (in != (byte*) out)
            XMEMSET(out_p, 0, outLen); /* clear output buffer */
        bd_p->DSTADDR = (unsigned int) KVA_TO_PA(out);
    } else {
        /* hashing */
        /* digest result returned in UPDPTR */
        bd_p->UPDPTR = (unsigned int) KVA_TO_PA(out);
    }
    bd_p->NXTPTR = (unsigned int) KVA_TO_PA(&bd);
    bd_p->MSGLEN = inLen; /* actual message size */
    bd_p->BD_CTRL.DESC_EN = 1; /* enable this descriptor */

    /* begin access to hardware */
    ret = wolfSSL_CryptHwMutexLock();
    if (ret == 0) {
        /* Software Reset the Crypto Engine */
        CECON = 1 << 6;
        while (CECON);

        /* Clear the interrupt flags */
        CEINTSRC = 0xF;

        /* Run the engine */
        CEBDPADDR = (unsigned int) KVA_TO_PA(&bd);
        CEINTEN = 0x07; /* enable DMA Packet Completion Interrupt */

        /* input swap, enable BD fetch and start DMA */
#if PIC32_NO_OUT_SWAP
        CECON = 0x25;
#else
        CECON = 0xa5; /* bit 7 = enable out swap */
#endif

        /* wait for operation to complete */
        while (CEINTSRCbits.PKTIF == 0 && --timeout > 0) {
        };

        /* Clear the interrupt flags */
        CEINTSRC = 0xF;

        /* check for errors */
        if (CESTATbits.ERROP || timeout <= 0) {
#if 0
            printf("PIC32 Crypto: ERROP %x, ERRPHASE %x, TIMEOUT %s\n",
                    CESTATbits.ERROP, CESTATbits.ERRPHASE, timeout <= 0 ? "yes" : "no");
#endif
            ret = ASYNC_OP_E;
        }

        wolfSSL_CryptHwMutexUnLock();

        if (iv && ivLen > 0) {
            /* set iv for the next call */
            if (dir == PIC32_ENCRYPTION) {
                XMEMCPY(iv, KVA0_TO_KVA1(out + (outLen - ivLen)), ivLen);
#if !PIC32_NO_OUT_SWAP
                /* hardware already swapped output, so we need to swap back */
                ByteReverseWords(iv, iv, ivLen);
#endif
            } else {
                ByteReverseWords(iv, KVA0_TO_KVA1(in + (inLen - ivLen)), ivLen);
            }
        }

        /* copy result to output */
#if PIC32_NO_OUT_SWAP
        /* swap bytes */
        ByteReverseWords(out, (word32*) out_p, outLen);
#elif defined(_SYS_DEVCON_LOCAL_H)
        /* sync cache */
        SYS_DEVCON_DataCacheInvalidate((word32) out, outLen);
#else
        XMEMCPY(out, out_p, outLen);
#endif
    }

    return ret;
}
#endif /* WOLFSSL_PIC32MZ_CRYPT || WOLFSSL_PIC32MZ_HASH */


#ifdef WOLFSSL_PIC32MZ_HASH

#ifdef WOLFSSL_PIC32MZ_LARGE_HASH

/* tunable large hash block size */
#ifndef PIC32_BLOCK_SIZE
#define PIC32_BLOCK_SIZE 256
#endif

#define PIC32MZ_MIN_BLOCK    64
#define PIC32MZ_MAX_BLOCK    (32*1024)

#ifndef PIC32MZ_MAX_BD
#define PIC32MZ_MAX_BD   2
#endif

#if PIC32_BLOCK_SIZE < PIC32MZ_MIN_BLOCK
#error Encryption block size must be at least 64 bytes.
#endif

/* Crypt Engine descriptor */
typedef struct {
    int currBd;
    int err;
    unsigned int msgSize;
    uint32_t processed;
    uint32_t dbPtr;
    int engine_ready;
    volatile bufferDescriptor bd[PIC32MZ_MAX_BD] __attribute__((aligned(8)));
    securityAssociation sa __attribute__((aligned(8)));
} pic32mz_desc;

static pic32mz_desc gLHDesc;
static uint8_t gLHDataBuf[PIC32MZ_MAX_BD][PIC32_BLOCK_SIZE] __attribute__((aligned(4), coherent));

void reset_engine(pic32mz_desc *desc, int algo) {
    int i;
    pic32mz_desc* uc_desc = KVA0_TO_KVA1(desc);

    wolfSSL_CryptHwMutexLock();

    /* Software reset */
    CECON = 1 << 6;
    while (CECON);

    /* Clear the interrupt flags */
    CEINTSRC = 0xF;

    /* Make sure everything is clear first before we setup */
    XMEMSET(desc, 0, sizeof (pic32mz_desc));
    XMEMSET((void *) &uc_desc->sa, 0, sizeof (uc_desc->sa));

    /* Set up the Security Association */
    uc_desc->sa.SA_CTRL.ALGO = algo;
    uc_desc->sa.SA_CTRL.LNC = 1;
    uc_desc->sa.SA_CTRL.FB = 1;
    uc_desc->sa.SA_CTRL.ENCTYPE = 1;
    uc_desc->sa.SA_CTRL.LOADIV = 1;

    /* Set up the Buffer Descriptor */
    uc_desc->err = 0;
    for (i = 0; i < PIC32MZ_MAX_BD; i++) {
        XMEMSET((void *) &uc_desc->bd[i], 0, sizeof (uc_desc->bd[i]));
        uc_desc->bd[i].BD_CTRL.LAST_BD = 1;
        uc_desc->bd[i].BD_CTRL.LIFM = 1;
        uc_desc->bd[i].BD_CTRL.PKT_INT_EN = 1;
        uc_desc->bd[i].SA_ADDR = KVA_TO_PA(&uc_desc->sa);
        uc_desc->bd[i].SRCADDR = KVA_TO_PA(&gLHDataBuf[i]);
        if (PIC32MZ_MAX_BD > i + 1)
            uc_desc->bd[i].NXTPTR = KVA_TO_PA(&uc_desc->bd[i + 1]);
        else
            uc_desc->bd[i].NXTPTR = KVA_TO_PA(&uc_desc->bd[0]);
        XMEMSET((void *) &gLHDataBuf[i], 0, PIC32_BLOCK_SIZE);
    }
    uc_desc->bd[0].BD_CTRL.SA_FETCH_EN = 1; /* Fetch the security association on the first BD */
    desc->dbPtr = 0;
    desc->currBd = 0;
    desc->msgSize = 0;
    desc->processed = 0;
    CEBDPADDR = KVA_TO_PA(&(desc->bd[0]));

    CEPOLLCON = 10;

#if PIC32_NO_OUT_SWAP
    CECON = 0x27;
#else
    CECON = 0xa7;
#endif
}

static void update_engine(pic32mz_desc *desc, const byte *input, word32 len,
        word32 *hash) {
    int total;
    pic32mz_desc *uc_desc = KVA0_TO_KVA1(desc);

    uc_desc->bd[desc->currBd].UPDPTR = KVA_TO_PA(hash);

    /* Add the data to the current buffer. If the buffer fills, start processing it
       and fill the next one. */
    while (len) {
        /* If we've been given the message size, we can process along the
           way.
           Enable the current buffer descriptor if it is full. */
        if (desc->dbPtr >= PIC32_BLOCK_SIZE) {
            /* Wrap up the buffer descriptor and enable it so the engine can process */
            uc_desc->bd[desc->currBd].MSGLEN = desc->msgSize;
            uc_desc->bd[desc->currBd].BD_CTRL.BUFLEN = desc->dbPtr;
            uc_desc->bd[desc->currBd].BD_CTRL.LAST_BD = 0;
            uc_desc->bd[desc->currBd].BD_CTRL.LIFM = 0;
            uc_desc->bd[desc->currBd].BD_CTRL.DESC_EN = 1;
            /* Move to the next buffer descriptor, or wrap around. */
            desc->currBd++;
            if (desc->currBd >= PIC32MZ_MAX_BD)
                desc->currBd = 0;
            /* Wait until the engine has processed the new BD. */
            while (uc_desc->bd[desc->currBd].BD_CTRL.DESC_EN);
            uc_desc->bd[desc->currBd].UPDPTR = KVA_TO_PA(hash);
            desc->dbPtr = 0;
        }
        if (!PIC32MZ_IF_RAM(input)) {
            /* If we're inputting from flash, let the BD have
               the address and max the buffer size */
            uc_desc->bd[desc->currBd].SRCADDR = KVA_TO_PA(input);
            total = (len > PIC32MZ_MAX_BLOCK ? PIC32MZ_MAX_BLOCK : len);
            desc->dbPtr = total;
            len -= total;
            input += total;
        } else {
            if (len > PIC32_BLOCK_SIZE - desc->dbPtr) {
                /* We have more data than can be put in the buffer. Fill what we can.*/
                total = PIC32_BLOCK_SIZE - desc->dbPtr;
                XMEMCPY(&gLHDataBuf[desc->currBd][desc->dbPtr], input, total);
                len -= total;
                desc->dbPtr = PIC32_BLOCK_SIZE;
                input += total;
            } else {
                /* Fill up what we have, but don't turn on the engine.*/
                XMEMCPY(&gLHDataBuf[desc->currBd][desc->dbPtr], input, len);
                desc->dbPtr += len;
                len = 0;
            }
        }
    }
}

static void start_engine(pic32mz_desc *desc) {
    /* Wrap up the last buffer descriptor and enable it */
    int bufferLen;
    pic32mz_desc *uc_desc = KVA0_TO_KVA1(desc);

    bufferLen = desc->dbPtr;
    if (bufferLen % 4)
        bufferLen = (bufferLen + 4) - (bufferLen % 4);
    uc_desc->bd[desc->currBd].BD_CTRL.BUFLEN = bufferLen;
    uc_desc->bd[desc->currBd].BD_CTRL.LAST_BD = 1;
    uc_desc->bd[desc->currBd].BD_CTRL.LIFM = 1;
    uc_desc->bd[desc->currBd].BD_CTRL.DESC_EN = 1;
}

void wait_engine(pic32mz_desc *desc, char *hash, int hash_sz) {
    int i;
    pic32mz_desc *uc_desc = KVA0_TO_KVA1(desc);
    unsigned int engineRunning;

    do {
        engineRunning = 0;
        for (i = 0; i < PIC32MZ_MAX_BD; i++) {
            engineRunning = engineRunning || uc_desc->bd[i].BD_CTRL.DESC_EN;
        }
    } while (engineRunning);

#if PIC32_NO_OUT_SWAP
    /* swap bytes */
    ByteReverseWords(hash, KVA0_TO_KVA1(hash), hash_sz);
#else
    /* copy output - hardware already swapped */
    XMEMCPY(hash, KVA0_TO_KVA1(hash), hash_sz);
#endif

    wolfSSL_CryptHwMutexUnLock();
}

#endif /* WOLFSSL_PIC32MZ_LARGE_HASH */

int wc_Pic32Hash(const byte* in, int inLen, word32* out, int outLen, int algo) {
    return Pic32Crypto(in, inLen, out, outLen, PIC32_ENCRYPTION, algo, 0, NULL, 0, NULL, 0);
}

int wc_Pic32HashCopy(hashUpdCache* src, hashUpdCache* dst) {
    /* mark destination as copy, so cache->buf is not free'd */
    if (dst) {
        dst->isCopy = 1;
    }
    return 0;
}

static int wc_Pic32HashUpdate(hashUpdCache* cache, byte* stdBuf, int stdBufLen, word32* digest, int digestSz, const byte* data, int len, int algo, void* heap) {
    int ret = 0;
    word32 newLenUpd, newLenPad, padRemain;
    byte* newBuf;
    int isNewBuf = 0;

#ifdef WOLFSSL_PIC32MZ_LARGE_HASH
    /* if final length is set then pass straight to hardware */
    if (cache->finalLen) {
        if (cache->bufLen == 0) {
            reset_engine(&gLHDesc, algo);
            gLHDesc.msgSize = cache->finalLen;
        }
        update_engine(&gLHDesc, data, len, digest);
        cache->bufLen += len; /* track progress for blockType */
        return 0;
    }
#endif

    /* cache updates */
    /* calculate new len */
    newLenUpd = cache->updLen + len;
    LOG("C:%d L:%d\n", cache->updLen, len);

    /* calculate padded len - pad buffer at 64-bytes for hardware */
    newLenPad = newLenUpd;
    padRemain = (newLenUpd % PIC32_BLOCKSIZE_HASH);
    if (padRemain != 0) {
        newLenPad += (PIC32_BLOCKSIZE_HASH - padRemain);
    }

    /* determine buffer source */
    if (newLenPad <= stdBufLen) {
        /* use standard buffer */
        newBuf = stdBuf;
    } else if (newLenPad > cache->bufLen) {
        /* alloc buffer */
        newBuf = (byte*) XMALLOC(newLenPad, heap, DYNAMIC_TYPE_HASH_TMP);
        if (newBuf == NULL) {
            if (cache->buf != stdBuf && !cache->isCopy) {
                XFREE(cache->buf, heap, DYNAMIC_TYPE_HASH_TMP);
                cache->buf = NULL;
                cache->updLen = cache->bufLen = 0;
            }
            LOG("ERROR: ", newLenPad);
            return MEMORY_E;
        } else {
            LOG("M: %d\n", newLenPad);
        }
        isNewBuf = 1;
        cache->isCopy = 0; /* no longer using copy buffer */
    } else {
        /* use existing buffer */
        newBuf = cache->buf;
    }
    if (cache->buf && cache->updLen > 0) {
        XMEMCPY(newBuf, cache->buf, cache->updLen);
        if (isNewBuf && cache->buf != stdBuf) {
            XFREE(cache->buf, heap, DYNAMIC_TYPE_HASH_TMP);
        }
    }
    XMEMCPY(newBuf + cache->updLen, data, len);
    cache->buf = newBuf;
    cache->updLen = newLenUpd;
    cache->bufLen = newLenPad;
    return ret;
}

static int wc_Pic32HashFinal(hashUpdCache* cache, byte* stdBuf,
        word32* digest, byte* hash, int digestSz, int algo, void* heap) {
    int ret = 0;

    /* if room add the pad */
    if (cache->buf && cache->updLen < cache->bufLen) {
        cache->buf[cache->updLen] = 0x80;
    }

#ifdef WOLFSSL_PIC32MZ_LARGE_HASH
    if (cache->finalLen) {
        start_engine(&gLHDesc);
        wait_engine(&gLHDesc, (char*) digest, digestSz);
        XMEMCPY(hash, digest, digestSz);
        cache->finalLen = 0;
    } else
#endif
    {
        if (cache->updLen == 0) {
            /* handle empty input */
            switch (algo) {
                case PIC32_ALGO_SHA256:
                {
                    const char* sha256EmptyHash =
                            "\xe3\xb0\xc4\x42\x98\xfc\x1c\x14\x9a\xfb\xf4\xc8\x99\x6f\xb9"
                            "\x24\x27\xae\x41\xe4\x64\x9b\x93\x4c\xa4\x95\x99\x1b\x78\x52"
                            "\xb8\x55";
                    XMEMCPY(hash, sha256EmptyHash, digestSz);
                    break;
                }
                case PIC32_ALGO_SHA1:
                {
                    const char* shaEmptyHash =
                            "\xda\x39\xa3\xee\x5e\x6b\x4b\x0d\x32\x55\xbf\xef\x95\x60\x18"
                            "\x90\xaf\xd8\x07\x09";
                    XMEMCPY(hash, shaEmptyHash, digestSz);
                    break;
                }
                case PIC32_ALGO_MD5:
                {
                    const char* md5EmptyHash =
                            "\xd4\x1d\x8c\xd9\x8f\x00\xb2\x04\xe9\x80\x09\x98\xec\xf8\x42"
                            "\x7e";
                    XMEMCPY(hash, md5EmptyHash, digestSz);
                    break;
                }
            } /* switch */
        } else {
            ret = wc_Pic32Hash(cache->buf, cache->updLen, digest, digestSz, algo);
            if (ret == 0) {
                XMEMCPY(hash, digest, digestSz);
            }
        }


    }

    //[WizIO]
    if (cache->buf && cache->buf != stdBuf && !cache->isCopy) {
        XFREE(cache->buf, heap, DYNAMIC_TYPE_HASH_TMP);
    }

    cache->buf = NULL;
    cache->bufLen = cache->updLen = 0;
    return ret;
}

/* API's for compatability with Harmony wrappers - not used */
#ifndef NO_MD5

int wc_InitMd5_ex(Md5* md5, void* heap, int devId) {
    (void) heap;
    (void) devId;
    if (md5 == NULL)
        return BAD_FUNC_ARG;
    XMEMSET(md5, 0, sizeof (Md5));
    reset_engine(&gLHDesc, PIC32_ALGO_MD5);
    return 0;
}

int wc_Md5Update(Md5* md5, const byte* data, word32 len) {
    if (md5 == NULL || (data == NULL && len > 0))
        return BAD_FUNC_ARG;
    return wc_Pic32HashUpdate(&md5->cache, (byte*) md5->buffer, sizeof (md5->buffer), md5->digest, MD5_DIGEST_SIZE, data, len, PIC32_ALGO_MD5, md5->heap);
}

int wc_Md5Final(Md5* md5, byte* hash) {
    int ret;
    if (md5 == NULL || hash == NULL)
        return BAD_FUNC_ARG;
    ret = wc_Pic32HashFinal(&md5->cache, (byte*) md5->buffer, md5->digest, hash, MD5_DIGEST_SIZE, PIC32_ALGO_MD5, md5->heap);
    wc_InitMd5_ex(md5, md5->heap, INVALID_DEVID); /* reset state */
    return ret;
}

#endif /* !NO_MD5 */



#ifndef NO_SHA

int wc_InitSha_ex(Sha* sha, void* heap, int devId) {
    (void) heap;
    (void) devId;
    if (sha == NULL)
        return BAD_FUNC_ARG;
    XMEMSET(sha, 0, sizeof (Sha));
    reset_engine(&gLHDesc, PIC32_ALGO_SHA1);
    return 0;
}

int wc_ShaUpdate(Sha* sha, const byte* data, word32 len) {
    if (sha == NULL || (data == NULL && len > 0))
        return BAD_FUNC_ARG;
    return wc_Pic32HashUpdate(&(sha->cache), (byte*) sha->buffer, sizeof (sha->buffer), sha->digest, SHA_DIGEST_SIZE, data, len, PIC32_ALGO_SHA1, NULL);
}

int wc_ShaFinal(Sha* sha, byte* hash) {
    int ret;
    if (sha == NULL || hash == NULL)
        return BAD_FUNC_ARG;
    ret = wc_Pic32HashFinal(&(sha->cache), (byte*) sha->buffer, sha->digest, hash, SHA_DIGEST_SIZE, PIC32_ALGO_SHA1, NULL);
    wc_InitSha_ex(sha, sha->heap, INVALID_DEVID); /* reset state */
    return ret;
}

#endif /* !NO_SHA */
#ifndef NO_SHA256

int wc_InitSha256_ex(Sha256* sha256, void* heap, int devId) {
    (void) heap;
    (void) devId;
    if (sha256 == NULL)
        return BAD_FUNC_ARG;
    XMEMSET(sha256, 0, sizeof (Sha256));
    reset_engine(&gLHDesc, PIC32_ALGO_SHA256);
    return 0;
}

int wc_Sha256Update(Sha256* sha256, const byte* data, word32 len) {
    if (sha256 == NULL || (data == NULL && len > 0))
        return BAD_FUNC_ARG;
    return wc_Pic32HashUpdate(&sha256->cache, (byte*) sha256->buffer, sizeof (sha256->buffer), sha256->digest, SHA256_DIGEST_SIZE, data, len, PIC32_ALGO_SHA256, NULL);
}

int wc_Sha256Final(Sha256* sha256, byte* hash) {
    int ret;
    if (sha256 == NULL || hash == NULL)
        return BAD_FUNC_ARG;
    ret = wc_Pic32HashFinal(&sha256->cache, (byte*) sha256->buffer, sha256->digest, hash, SHA256_DIGEST_SIZE, PIC32_ALGO_SHA256, sha256->heap);
    wc_InitSha256_ex(sha256, sha256->heap, INVALID_DEVID); /* reset state */
    return ret;
}

////////////////////////////////////////////////////////////////////////////////

int wc_InitSha224(Sha224* sha224) {
    int ret = 0;
    if (sha224 == NULL)
        return BAD_FUNC_ARG;
    sha224->digest[0] = 0xc1059ed8;
    sha224->digest[1] = 0x367cd507;
    sha224->digest[2] = 0x3070dd17;
    sha224->digest[3] = 0xf70e5939;
    sha224->digest[4] = 0xffc00b31;
    sha224->digest[5] = 0x68581511;
    sha224->digest[6] = 0x64f98fa7;
    sha224->digest[7] = 0xbefa4fa4;
    sha224->buffLen = 0;
    sha224->loLen = 0;
    sha224->hiLen = 0;
    reset_engine(&gLHDesc, PIC32_ALGO_SHA256);
    return ret;
}

int wc_InitSha224_ex(Sha224* sha224, void* heap, int devId) {
    (void) heap;
    (void) devId;
    if (sha224 == NULL)
        return BAD_FUNC_ARG;
    return wc_InitSha224(sha224);
}

int wc_Sha224Update(Sha224* sha224, const byte* data, word32 len) {
    if (sha224 == NULL || (data == NULL && len > 0))
        return BAD_FUNC_ARG;
    return wc_Sha256Update((Sha256*) sha224, data, len);
}

int wc_Sha224Final(Sha224* sha224, byte* hash) {
    int ret;
    if (sha224 == NULL || hash == NULL)
        return BAD_FUNC_ARG;
    ret = wc_Pic32HashFinal(&sha224->cache, (byte*) sha224->buffer, sha224->digest, hash, SHA224_DIGEST_SIZE, PIC32_ALGO_SHA256, NULL);
    if (ret != 0)
        return ret;
    XMEMCPY(hash, sha224->digest, WC_SHA224_DIGEST_SIZE);
    return wc_InitSha224(sha224); /* reset state */
}
////////////////////////////////////////////////////////////////////////////////
#endif /* !NO_SHA256 */

#endif /* WOLFSSL_PIC32MZ_HASH */












#ifdef WOLFSSL_PIC32MZ_CRYPT
#if !defined(NO_AES)

int wc_Pic32AesCrypt(word32 *key, int keyLen, word32 *iv, int ivLen,
        byte* out, const byte* in, word32 sz,
        int dir, int algo, int cryptoalgo) {
    return Pic32Crypto(in, sz, (word32*) out, sz, dir, algo, cryptoalgo,
            key, keyLen, iv, ivLen);
}
#endif /* !NO_AES */

#ifndef NO_DES3

int wc_Pic32DesCrypt(word32 *key, int keyLen, word32 *iv, int ivLen,
        byte* out, const byte* in, word32 sz,
        int dir, int algo, int cryptoalgo) {
    return Pic32Crypto(in, sz, (word32*) out, sz, dir, algo, cryptoalgo,
            key, keyLen, iv, ivLen);
}
#endif /* !NO_DES3 */
#endif /* WOLFSSL_PIC32MZ_CRYPT */


#if !defined(NO_AES)

/* Free Aes from use with async hardware */
void wc_AesFree(Aes * aes) {
    if (aes == NULL)
        return;
}

static int wc_AesEncrypt(Aes* aes, const byte* inBlock, byte* outBlock) {
    return wc_Pic32AesCrypt(aes->key, aes->keylen, NULL, 0,
            outBlock, inBlock, AES_BLOCK_SIZE,
            PIC32_ENCRYPTION, PIC32_ALGO_AES, PIC32_CRYPTOALGO_RECB);
}

static int wc_AesDecrypt(Aes* aes, const byte* inBlock, byte* outBlock) {
    return wc_Pic32AesCrypt(aes->key, aes->keylen, NULL, 0,
            outBlock, inBlock, AES_BLOCK_SIZE,
            PIC32_DECRYPTION, PIC32_ALGO_AES, PIC32_CRYPTOALGO_RECB);
}

int wc_AesCbcEncrypt(Aes* aes, byte* out, const byte* in, word32 sz) {
    int ret;
    /* hardware fails on input that is not a multiple of AES block size */
    if (sz % AES_BLOCK_SIZE != 0) {
        return BAD_FUNC_ARG;
    }
    ret = wc_Pic32AesCrypt(
            aes->key, aes->keylen, aes->reg, AES_BLOCK_SIZE,
            out, in, sz, PIC32_ENCRYPTION,
            PIC32_ALGO_AES, PIC32_CRYPTOALGO_RCBC);

    /* store iv for next call */
    if (ret == 0) {
        XMEMCPY(aes->reg, out + sz - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
    }
    return ret;
}

int wc_AesCbcDecrypt(Aes* aes, byte* out, const byte* in, word32 sz) {
    int ret;
    byte scratch[AES_BLOCK_SIZE];
    /* hardware fails on input that is not a multiple of AES block size */
    if (sz % AES_BLOCK_SIZE != 0) {
        return BAD_FUNC_ARG;
    }
    XMEMCPY(scratch, in + sz - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
    ret = wc_Pic32AesCrypt(
            aes->key, aes->keylen, aes->reg, AES_BLOCK_SIZE,
            out, in, sz, PIC32_DECRYPTION,
            PIC32_ALGO_AES, PIC32_CRYPTOALGO_RCBC);
    /* store iv for next call */
    if (ret == 0) {
        XMEMCPY((byte*) aes->reg, scratch, AES_BLOCK_SIZE);
    }
    return ret;
}

int wc_AesCtrEncryptBlock(Aes* aes, byte* out, const byte* in) {
    word32 tmpIv[AES_BLOCK_SIZE / sizeof (word32)];
    XMEMCPY(tmpIv, aes->reg, AES_BLOCK_SIZE);
    return wc_Pic32AesCrypt(
            aes->key, aes->keylen, tmpIv, AES_BLOCK_SIZE,
            out, in, AES_BLOCK_SIZE,
            PIC32_ENCRYPTION, PIC32_ALGO_AES, PIC32_CRYPTOALGO_RCTR);
}
#endif


#endif /* WOLFSSL_MICROCHIP_PIC32MZ */