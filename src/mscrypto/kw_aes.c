/**
 * XMLSec library
 *
 * This is free software; see Copyright file in the source
 * distribution for preciese wording.
 *
 * Copyright (C) 2003 Cordys R&D BV, All rights reserved.
 * Copyright (C) 2003 Aleksey Sanin <aleksey@aleksey.com>
 */
#include "globals.h"

#include <string.h>

#include <windows.h>
#include <wincrypt.h>

#include <xmlsec/xmlsec.h>
#include <xmlsec/keys.h>
#include <xmlsec/transforms.h>
#include <xmlsec/errors.h>

#include <xmlsec/mscrypto/crypto.h>

#include "../kw_aes_des.h"
#include "private.h"


#if defined(__MINGW32__)
#  include "xmlsec-mingw.h"
#endif


/**************************************************************************
 *
 * Internal MSCrypto Block cipher CTX
 *
 *****************************************************************************/
typedef struct _xmlSecMSCryptoBlockCipherCtx            xmlSecMSCryptoBlockCipherCtx,
                                                        *xmlSecMSCryptoBlockCipherCtxPtr;
struct _xmlSecMSCryptoBlockCipherCtx {
    ALG_ID                              algorithmIdentifier;
    const xmlSecMSCryptoProviderInfo  * providers;
    xmlSecKeyDataId                     keyId;
    xmlSecSize                          keySize;

    HCRYPTPROV                          cryptProvider;
    HCRYPTKEY                           pubPrivKey;
    HCRYPTKEY                           cryptKey;
    xmlSecBuffer                        kwKeyBuffer; /* used only for KW algorithm - need to reset cryptKey for every operation to avoid CBC mode */
    int                                 ctxInitialized;
};
/* function declarations */
static int      xmlSecMSCryptoBlockCipherCtxUpdate      (xmlSecMSCryptoBlockCipherCtxPtr ctx,
                                                         xmlSecBufferPtr in,
                                                         xmlSecBufferPtr out,
                                                         int encrypt,
                                                         const xmlChar* cipherName,
                                                         xmlSecTransformCtxPtr transformCtx);


static int
xmlSecMSCryptoBlockCipherCtxInit(xmlSecMSCryptoBlockCipherCtxPtr ctx,
                                 xmlSecBufferPtr in,
                                 xmlSecBufferPtr out,
                                 int encrypt,
                                 const xmlChar* cipherName,
                                 xmlSecTransformCtxPtr transformCtx) {
    int blockLen;
    int ret;
    DWORD dwBlockLen, dwBlockLenLen;

    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->cryptKey != 0, -1);
    xmlSecAssert2(ctx->ctxInitialized == 0, -1);
    xmlSecAssert2(in != NULL, -1);
    xmlSecAssert2(out != NULL, -1);
    xmlSecAssert2(transformCtx != NULL, -1);

    /* iv len == block len */
    dwBlockLenLen = sizeof(DWORD);
    if (!CryptGetKeyParam(ctx->cryptKey, KP_BLOCKLEN, (BYTE *)&dwBlockLen, &dwBlockLenLen, 0)) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "CryptGetKeyParam",
                    XMLSEC_ERRORS_R_CRYPTO_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    blockLen = dwBlockLen / 8;
    xmlSecAssert2(blockLen > 0, -1);
    if(encrypt) {
        unsigned char* iv;
        size_t outSize;

        /* allocate space for IV */
        outSize = xmlSecBufferGetSize(out);
        ret = xmlSecBufferSetSize(out, outSize + blockLen);
        if(ret < 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "xmlSecBufferSetSize",
                        XMLSEC_ERRORS_R_XMLSEC_FAILED,
                        "size=%d", outSize + blockLen);
            return(-1);
        }
        iv = xmlSecBufferGetData(out) + outSize;

        /* generate and use random iv */
        if(!CryptGenRandom(ctx->cryptProvider, blockLen, iv)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptGenRandom",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        "len=%d", blockLen);
            return(-1);
        }

        if(!CryptSetKeyParam(ctx->cryptKey, KP_IV, iv, 0)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptSetKeyParam",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        XMLSEC_ERRORS_NO_MESSAGE);
            return(-1);
        }
    } else {
        /* if we don't have enough data, exit and hope that
        * we'll have iv next time */
        if(xmlSecBufferGetSize(in) < (size_t)blockLen) {
            return(0);
        }
        xmlSecAssert2(xmlSecBufferGetData(in) != NULL, -1);

        /* set iv */
        if (!CryptSetKeyParam(ctx->cryptKey, KP_IV, xmlSecBufferGetData(in), 0)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptSetKeyParam",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        XMLSEC_ERRORS_NO_MESSAGE);
            return(-1);
        }

        /* and remove from input */
        ret = xmlSecBufferRemoveHead(in, blockLen);
        if(ret < 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "xmlSecBufferRemoveHead",
                        XMLSEC_ERRORS_R_XMLSEC_FAILED,
                        "size=%d", blockLen);
            return(-1);

        }
    }

    ctx->ctxInitialized = 1;
    return(0);
}

static int
xmlSecMSCryptoBlockCipherCtxUpdate(xmlSecMSCryptoBlockCipherCtxPtr ctx,
                                   xmlSecBufferPtr in, xmlSecBufferPtr out,
                                   int encrypt,
                                   const xmlChar* cipherName,
                                   xmlSecTransformCtxPtr transformCtx) {
    size_t inSize, inBlocks, outSize;
    int blockLen;
    unsigned char* outBuf;
    unsigned char* inBuf;
    int ret;
    DWORD dwBlockLen, dwBlockLenLen, dwCLen;

    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->ctxInitialized != 0, -1);
    xmlSecAssert2(in != NULL, -1);
    xmlSecAssert2(out != NULL, -1);
    xmlSecAssert2(transformCtx != NULL, -1);

    dwBlockLenLen = sizeof(DWORD);
    if (!CryptGetKeyParam(ctx->cryptKey, KP_BLOCKLEN, (BYTE *)&dwBlockLen, &dwBlockLenLen, 0)) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "CryptSetKeyParam",
                    XMLSEC_ERRORS_R_CRYPTO_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }
    blockLen = dwBlockLen / 8;
    xmlSecAssert2(blockLen > 0, -1);

    inSize = xmlSecBufferGetSize(in);
    outSize = xmlSecBufferGetSize(out);

    if(inSize < (size_t)blockLen) {
        return(0);
    }

    if(encrypt) {
        inBlocks = inSize / ((size_t)blockLen);
    } else {
        /* we want to have the last block in the input buffer
         * for padding check */
        inBlocks = (inSize - 1) / ((size_t)blockLen);
    }
    inSize = inBlocks * ((size_t)blockLen);

    /* we write out the input size plus may be one block */
    ret = xmlSecBufferSetMaxSize(out, outSize + inSize + blockLen);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "xmlSecBufferSetMaxSize",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%d", outSize + inSize + blockLen);
        return(-1);
    }
    outBuf = xmlSecBufferGetData(out) + outSize;
    inBuf = xmlSecBufferGetData(in);
    xmlSecAssert2(inBuf != NULL, -1);

    memcpy(outBuf, inBuf, inSize);
    dwCLen = inSize;
    if(encrypt) {
        if(!CryptEncrypt(ctx->cryptKey, 0, FALSE, 0, outBuf, &dwCLen, inSize + blockLen)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptEncrypt",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        XMLSEC_ERRORS_NO_MESSAGE);
            return(-1);
        }
    } else {
        if (!CryptDecrypt(ctx->cryptKey, 0, FALSE, 0, outBuf, &dwCLen)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptSetKeyDecrypt",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        XMLSEC_ERRORS_NO_MESSAGE);
            return(-1);
        }
    }
    /* Check if we really have de/encrypted the numbers of bytes that we requested */
    if (dwCLen != inSize) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "CryptEn/Decrypt",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%ld", dwCLen);
        return(-1);
    }

    /* set correct output buffer size */
    ret = xmlSecBufferSetSize(out, outSize + inSize);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "xmlSecBufferSetSize",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%d", outSize + inSize);
        return(-1);
    }

    /* remove the processed block from input */
    ret = xmlSecBufferRemoveHead(in, inSize);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "xmlSecBufferRemoveHead",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%d", inSize);
        return(-1);
    }
    return(0);
}

static int
xmlSecMSCryptoBlockCipherCtxFinal(xmlSecMSCryptoBlockCipherCtxPtr ctx,
                                  xmlSecBufferPtr in,
                                  xmlSecBufferPtr out,
                                  int encrypt,
                                  const xmlChar* cipherName,
                                  xmlSecTransformCtxPtr transformCtx) {
    size_t inSize, outSize;
    int blockLen, outLen = 0;
    unsigned char* inBuf;
    unsigned char* outBuf;
    int ret;
    DWORD dwBlockLen, dwBlockLenLen, dwCLen;

    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->ctxInitialized != 0, -1);
    xmlSecAssert2(in != NULL, -1);
    xmlSecAssert2(out != NULL, -1);
    xmlSecAssert2(transformCtx != NULL, -1);

    dwBlockLenLen = sizeof(DWORD);
    if (!CryptGetKeyParam(ctx->cryptKey, KP_BLOCKLEN, (BYTE *)&dwBlockLen, &dwBlockLenLen, 0)) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "CryptGetKeyParam",
                    XMLSEC_ERRORS_R_CRYPTO_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }
    blockLen = dwBlockLen / 8;
    xmlSecAssert2(blockLen > 0, -1);

    inSize = xmlSecBufferGetSize(in);
    outSize = xmlSecBufferGetSize(out);

    if(encrypt != 0) {
        xmlSecAssert2(inSize < (size_t)blockLen, -1);

        /* create padding */
        ret = xmlSecBufferSetMaxSize(in, blockLen);
        if(ret < 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "xmlSecBufferSetMaxSize",
                        XMLSEC_ERRORS_R_XMLSEC_FAILED,
                        "size=%d", blockLen);
            return(-1);
        }
        inBuf = xmlSecBufferGetData(in);

        /* create random padding */
        if((size_t)blockLen > (inSize + 1)) {
            if (!CryptGenRandom(ctx->cryptProvider, blockLen - inSize - 1, inBuf + inSize)) {
                xmlSecError(XMLSEC_ERRORS_HERE,
                            xmlSecErrorsSafeString(cipherName),
                            "CryptGenRandom",
                            XMLSEC_ERRORS_R_CRYPTO_FAILED,
                            XMLSEC_ERRORS_NO_MESSAGE);
                return(-1);
            }
        }
        inBuf[blockLen - 1] = blockLen - inSize;
        inSize = blockLen;
    } else {
        if(inSize != (size_t)blockLen) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        NULL,
                        XMLSEC_ERRORS_R_INVALID_DATA,
                        "data=%d;block=%d", inSize, blockLen);
            return(-1);
        }
        inBuf = xmlSecBufferGetData(in);
    }

    /* process last block */
    ret = xmlSecBufferSetMaxSize(out, outSize + 2 * blockLen);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "xmlSecBufferSetMaxSize",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%d", outSize + 2 * blockLen);
        return(-1);
    }
    outBuf = xmlSecBufferGetData(out) + outSize;
    memcpy(outBuf, inBuf, inSize);

    dwCLen = inSize;
    if(encrypt) {
        /* Set process last block to false, since we handle padding ourselves, and MSCrypto padding
         * can be skipped. I hope this will work .... */
        if(!CryptEncrypt(ctx->cryptKey, 0, FALSE, 0, outBuf, &dwCLen, inSize + blockLen)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptEncrypt",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        XMLSEC_ERRORS_NO_MESSAGE);
            return(-1);
        }
    } else {
        if (!CryptDecrypt(ctx->cryptKey, 0, FALSE, 0, outBuf, &dwCLen)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        "CryptDecrypt",
                        XMLSEC_ERRORS_R_CRYPTO_FAILED,
                        XMLSEC_ERRORS_NO_MESSAGE);
            return(-1);
        }
    }

    /* Check if we really have de/encrypted the numbers of bytes that we requested */
    if (dwCLen != inSize) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "CryptEn/Decrypt",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%ld", dwCLen);
        return(-1);
    }

    if(encrypt == 0) {
        /* check padding */
        if(inSize < outBuf[blockLen - 1]) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(cipherName),
                        NULL,
                        XMLSEC_ERRORS_R_INVALID_DATA,
                        "padding=%d;buffer=%d",
                        outBuf[blockLen - 1], inSize);
            return(-1);
        }
        outLen = inSize - outBuf[blockLen - 1];
    } else {
        outLen = inSize;
    }

    /* set correct output buffer size */
    ret = xmlSecBufferSetSize(out, outSize + outLen);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "xmlSecBufferSetSize",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%d", outSize + outLen);
        return(-1);
    }

    /* remove the processed block from input */
    ret = xmlSecBufferRemoveHead(in, inSize);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(cipherName),
                    "xmlSecBufferRemoveHead",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "size=%d", inSize);
        return(-1);
    }

    return(0);
}

/******************************************************************************
 *
 *  Block Cipher transforms
 *
 * xmlSecMSCryptoBlockCipherCtx block is located after xmlSecTransform structure
 *
 *****************************************************************************/
#define xmlSecMSCryptoBlockCipherSize   \
    (sizeof(xmlSecTransform) + sizeof(xmlSecMSCryptoBlockCipherCtx))
#define xmlSecMSCryptoBlockCipherGetCtx(transform) \
    ((xmlSecMSCryptoBlockCipherCtxPtr)(((unsigned char*)(transform)) + sizeof(xmlSecTransform)))

static int      xmlSecMSCryptoBlockCipherInitialize     (xmlSecTransformPtr transform);
static void     xmlSecMSCryptoBlockCipherFinalize       (xmlSecTransformPtr transform);
static int      xmlSecMSCryptoBlockCipherSetKeyReq      (xmlSecTransformPtr transform,
                                                         xmlSecKeyReqPtr keyReq);
static int      xmlSecMSCryptoBlockCipherSetKey         (xmlSecTransformPtr transform,
                                                         xmlSecKeyPtr key);
static int      xmlSecMSCryptoBlockCipherExecute        (xmlSecTransformPtr transform,
                                                         int last,
                                                         xmlSecTransformCtxPtr transformCtx);
static int      xmlSecMSCryptoBlockCipherCheckId        (xmlSecTransformPtr transform);



/* Ordered list of providers to search for algorithm implementation using
 * xmlSecMSCryptoFindProvider() function
 *
 * MUST END with { NULL, 0 } !!!
 */
#ifndef XMLSEC_NO_DES
static xmlSecMSCryptoProviderInfo xmlSecMSCryptoProviderInfo_Des[] = {
    { MS_STRONG_PROV,               PROV_RSA_FULL },
    { MS_ENHANCED_PROV,             PROV_RSA_FULL },
    { NULL, 0 }
};
#endif /* XMLSEC_NO_DES */

#ifndef XMLSEC_NO_AES
static xmlSecMSCryptoProviderInfo xmlSecMSCryptoProviderInfo_Aes[] = {
    { XMLSEC_CRYPTO_MS_ENH_RSA_AES_PROV,                PROV_RSA_AES},
    { XMLSEC_CRYPTO_MS_ENH_RSA_AES_PROV_PROTOTYPE,      PROV_RSA_AES },
    { NULL, 0 }
};
#endif /* XMLSEC_NO_AES */

static int
xmlSecMSCryptoBlockCipherCheckId(xmlSecTransformPtr transform) {
#ifndef XMLSEC_NO_DES
    if(xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformDes3CbcId)) {
        return(1);
    }
#endif /* XMLSEC_NO_DES */

#ifndef XMLSEC_NO_AES
    if(xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformAes128CbcId) ||
       xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformAes192CbcId) ||
       xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformAes256CbcId)) {

       return(1);
    }

    if(xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformKWAes128Id) ||
       xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformKWAes192Id) ||
       xmlSecTransformCheckId(transform, xmlSecMSCryptoTransformKWAes256Id)) {

       return(1);
    }
#endif /* XMLSEC_NO_AES */

    return(0);
}

static int
xmlSecMSCryptoBlockCipherInitialize(xmlSecTransformPtr transform) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;
    int ret;

    xmlSecAssert2(xmlSecMSCryptoBlockCipherCheckId(transform), -1);
    xmlSecAssert2(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize), -1);

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert2(ctx != NULL, -1);

    memset(ctx, 0, sizeof(xmlSecMSCryptoBlockCipherCtx));

    ret = xmlSecBufferInitialize(&ctx->kwKeyBuffer, 0);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    NULL,
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

#ifndef XMLSEC_NO_DES
    if(transform->id == xmlSecMSCryptoTransformDes3CbcId) {
        ctx->algorithmIdentifier    = CALG_3DES;
        ctx->keyId                  = xmlSecMSCryptoKeyDataDesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Des;
        ctx->keySize                = 24;
    } else
#endif /* XMLSEC_NO_DES */

#ifndef XMLSEC_NO_AES
    if(transform->id == xmlSecMSCryptoTransformAes128CbcId) {
        ctx->algorithmIdentifier    = CALG_AES_128;
        ctx->keyId                  = xmlSecMSCryptoKeyDataAesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Aes;
        ctx->keySize                = 16;
    } else if(transform->id == xmlSecMSCryptoTransformAes192CbcId) {
        ctx->algorithmIdentifier    = CALG_AES_192;
        ctx->keyId                  = xmlSecMSCryptoKeyDataAesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Aes;
        ctx->keySize                = 24;
    } else if(transform->id == xmlSecMSCryptoTransformAes256CbcId) {
        ctx->algorithmIdentifier    = CALG_AES_256;
        ctx->keyId                  = xmlSecMSCryptoKeyDataAesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Aes;
        ctx->keySize                = 32;
    } else if(transform->id == xmlSecMSCryptoTransformKWAes128Id) {
        ctx->algorithmIdentifier    = CALG_AES_128;
        ctx->keyId                  = xmlSecMSCryptoKeyDataAesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Aes;
        ctx->keySize                = XMLSEC_KW_AES128_KEY_SIZE;
    } else if(transform->id == xmlSecMSCryptoTransformKWAes192Id) {
        ctx->algorithmIdentifier    = CALG_AES_192;
        ctx->keyId                  = xmlSecMSCryptoKeyDataAesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Aes;
        ctx->keySize                = XMLSEC_KW_AES192_KEY_SIZE;
    } else if(transform->id == xmlSecMSCryptoTransformKWAes256Id) {
        ctx->algorithmIdentifier    = CALG_AES_256;
        ctx->keyId                  = xmlSecMSCryptoKeyDataAesId;
        ctx->providers              = xmlSecMSCryptoProviderInfo_Aes;
        ctx->keySize                = XMLSEC_KW_AES256_KEY_SIZE;
    } else

#endif /* XMLSEC_NO_AES */

    {
        xmlSecError(XMLSEC_ERRORS_HERE,
            xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
            NULL,
            XMLSEC_ERRORS_R_INVALID_TRANSFORM,
            XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    ctx->cryptProvider = xmlSecMSCryptoFindProvider(ctx->providers, NULL, CRYPT_VERIFYCONTEXT, TRUE);
    if(ctx->cryptProvider == 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    "xmlSecMSCryptoFindProvider",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);

        return(-1);
    }

    /* Create dummy key to be able to import plain session keys */
    if (!xmlSecMSCryptoCreatePrivateExponentOneKey(ctx->cryptProvider, &(ctx->pubPrivKey))) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    "xmlSecMSCryptoCreatePrivateExponentOneKey",
                    XMLSEC_ERRORS_R_CRYPTO_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);

        return(-1);
    }

    ctx->ctxInitialized = 0;
    return(0);
}

static void
xmlSecMSCryptoBlockCipherFinalize(xmlSecTransformPtr transform) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;

    xmlSecAssert(xmlSecMSCryptoBlockCipherCheckId(transform));
    xmlSecAssert(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize));

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert(ctx != NULL);

    if (ctx->cryptKey) {
        CryptDestroyKey(ctx->cryptKey);
    }
    if (ctx->pubPrivKey) {
        CryptDestroyKey(ctx->pubPrivKey);
    }
    if (ctx->cryptProvider) {
        CryptReleaseContext(ctx->cryptProvider, 0);
    }
    
    xmlSecBufferFinalize(&ctx->kwKeyBuffer);

    memset(ctx, 0, sizeof(xmlSecMSCryptoBlockCipherCtx));
}

static int
xmlSecMSCryptoBlockCipherSetKeyReq(xmlSecTransformPtr transform,  xmlSecKeyReqPtr keyReq) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;

    xmlSecAssert2(xmlSecMSCryptoBlockCipherCheckId(transform), -1);
    xmlSecAssert2((transform->operation == xmlSecTransformOperationEncrypt) || (transform->operation == xmlSecTransformOperationDecrypt), -1);
    xmlSecAssert2(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize), -1);
    xmlSecAssert2(keyReq != NULL, -1);

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->cryptProvider != 0, -1);

    keyReq->keyId       = ctx->keyId;
    keyReq->keyType     = xmlSecKeyDataTypeSymmetric;
    if(transform->operation == xmlSecTransformOperationEncrypt) {
        keyReq->keyUsage = xmlSecKeyUsageEncrypt;
    } else {
        keyReq->keyUsage = xmlSecKeyUsageDecrypt;
    }

    keyReq->keyBitsSize = 8 * ctx->keySize;
    return(0);
}

static int
xmlSecMSCryptoBlockCipherSetKey(xmlSecTransformPtr transform, xmlSecKeyPtr key) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;
    xmlSecBufferPtr buffer;
    BYTE* bufData;

    xmlSecAssert2(xmlSecMSCryptoBlockCipherCheckId(transform), -1);
    xmlSecAssert2((transform->operation == xmlSecTransformOperationEncrypt) || (transform->operation == xmlSecTransformOperationDecrypt), -1);
    xmlSecAssert2(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize), -1);
    xmlSecAssert2(key != NULL, -1);

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->cryptKey == 0, -1);
    xmlSecAssert2(ctx->pubPrivKey != 0, -1);
    xmlSecAssert2(ctx->keyId != NULL, -1);
    xmlSecAssert2(xmlSecKeyCheckId(key, ctx->keyId), -1);
    xmlSecAssert2(ctx->keySize > 0, -1);

    buffer = xmlSecKeyDataBinaryValueGetBuffer(xmlSecKeyGetValue(key));
    xmlSecAssert2(buffer != NULL, -1);

    if(xmlSecBufferGetSize(buffer) < ctx->keySize) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    NULL,
                    XMLSEC_ERRORS_R_INVALID_KEY_DATA_SIZE,
                    "keySize=%d;expected=%d",
                    xmlSecBufferGetSize(buffer), ctx->keySize);
        return(-1);
    }

    bufData = xmlSecBufferGetData(buffer);
    xmlSecAssert2(bufData != NULL, -1);

    /* Import this key and get an HCRYPTKEY handle */
    if (!xmlSecMSCryptoImportPlainSessionBlob(ctx->cryptProvider,
        ctx->pubPrivKey,
        ctx->algorithmIdentifier,
        bufData,
        ctx->keySize,
        TRUE,
        &(ctx->cryptKey)))  {

        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    "xmlSecMSCryptoImportPlainSessionBlob",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    return(0);
}

static int
xmlSecMSCryptoBlockCipherExecute(xmlSecTransformPtr transform, int last, xmlSecTransformCtxPtr transformCtx) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;
    xmlSecBufferPtr in, out;
    int ret;

    xmlSecAssert2(xmlSecMSCryptoBlockCipherCheckId(transform), -1);
    xmlSecAssert2((transform->operation == xmlSecTransformOperationEncrypt) || (transform->operation == xmlSecTransformOperationDecrypt), -1);
    xmlSecAssert2(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize), -1);
    xmlSecAssert2(transformCtx != NULL, -1);

    in = &(transform->inBuf);
    out = &(transform->outBuf);

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert2(ctx != NULL, -1);

    if(transform->status == xmlSecTransformStatusNone) {
        transform->status = xmlSecTransformStatusWorking;
    }

    if(transform->status == xmlSecTransformStatusWorking) {
        if(ctx->ctxInitialized == 0) {
            ret = xmlSecMSCryptoBlockCipherCtxInit(ctx,
                                                   in,
                                                   out,
                                                   (transform->operation == xmlSecTransformOperationEncrypt) ? 1 : 0,
                                                   xmlSecTransformGetName(transform),
                                                   transformCtx);

            if(ret < 0) {
                xmlSecError(XMLSEC_ERRORS_HERE,
                            xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                            "xmlSecMSCryptoBlockCipherCtxInit",
                            XMLSEC_ERRORS_R_XMLSEC_FAILED,
                            XMLSEC_ERRORS_NO_MESSAGE);
                return(-1);
            }
        }
        if((ctx->ctxInitialized == 0) && (last != 0)) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                        NULL,
                        XMLSEC_ERRORS_R_INVALID_DATA,
                        "not enough data to initialize transform");
            return(-1);
        }
        if(ctx->ctxInitialized != 0) {
            ret = xmlSecMSCryptoBlockCipherCtxUpdate(ctx, in, out,
                (transform->operation == xmlSecTransformOperationEncrypt) ? 1 : 0,
                xmlSecTransformGetName(transform), transformCtx);
            if(ret < 0) {
                xmlSecError(XMLSEC_ERRORS_HERE,
                            xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                            "xmlSecMSCryptoBlockCipherCtxUpdate",
                            XMLSEC_ERRORS_R_XMLSEC_FAILED,
                            XMLSEC_ERRORS_NO_MESSAGE);
                return(-1);
            }
        }

        if(last) {
            ret = xmlSecMSCryptoBlockCipherCtxFinal(ctx, in, out,
                (transform->operation == xmlSecTransformOperationEncrypt) ? 1 : 0,
                xmlSecTransformGetName(transform), transformCtx);

            if(ret < 0) {
                xmlSecError(XMLSEC_ERRORS_HERE,
                            xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                            "xmlSecMSCryptoBlockCipherCtxFinal",
                            XMLSEC_ERRORS_R_XMLSEC_FAILED,
                            XMLSEC_ERRORS_NO_MESSAGE);
                return(-1);
            }
            transform->status = xmlSecTransformStatusFinished;
        }
    } else if(transform->status == xmlSecTransformStatusFinished) {
        /* the only way we can get here is if there is no input */
        xmlSecAssert2(xmlSecBufferGetSize(in) == 0, -1);
    } else if(transform->status == xmlSecTransformStatusNone) {
        /* the only way we can get here is if there is no enough data in the input */
        xmlSecAssert2(last == 0, -1);
    } else {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    NULL,
                    XMLSEC_ERRORS_R_INVALID_STATUS,
                    "status=%d", transform->status);
        return(-1);
    }

    return(0);
}

#ifndef XMLSEC_NO_AES
/*********************************************************************
 *
 * AES CBC cipher transforms
 *
 ********************************************************************/
static xmlSecTransformKlass xmlSecMSCryptoAes128CbcKlass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),               /* xmlSecSize klassSize */
    xmlSecMSCryptoBlockCipherSize,              /* xmlSecSize objSize */

    xmlSecNameAes128Cbc,                        /* const xmlChar* name; */
    xmlSecHrefAes128Cbc,                        /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,       /* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize,        /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,          /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                       /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                       /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,         /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoBlockCipherSetKey,            /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                       /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,          /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,              /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,               /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                       /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                       /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoBlockCipherExecute,           /* xmlSecTransformExecuteMethod execute; */

    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformAes128CbcGetKlass:
 *
 * AES 128 CBC encryption transform klass.
 *
 * Returns: pointer to AES 128 CBC encryption transform.
 */
xmlSecTransformId
xmlSecMSCryptoTransformAes128CbcGetKlass(void) {
    return(&xmlSecMSCryptoAes128CbcKlass);
}

static xmlSecTransformKlass xmlSecMSCryptoAes192CbcKlass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),               /* xmlSecSize klassSize */
    xmlSecMSCryptoBlockCipherSize,              /* xmlSecSize objSize */

    xmlSecNameAes192Cbc,                        /* const xmlChar* name; */
    xmlSecHrefAes192Cbc,                        /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,       /* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize,        /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,          /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                       /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                       /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,         /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoBlockCipherSetKey,            /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                       /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,          /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,              /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,               /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                       /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                       /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoBlockCipherExecute,           /* xmlSecTransformExecuteMethod execute; */

    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformAes192CbcGetKlass:
 *
 * AES 192 CBC encryption transform klass.
 *
 * Returns: pointer to AES 192 CBC encryption transform.
 */
xmlSecTransformId
xmlSecMSCryptoTransformAes192CbcGetKlass(void) {
    return(&xmlSecMSCryptoAes192CbcKlass);
}

static xmlSecTransformKlass xmlSecMSCryptoAes256CbcKlass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),               /* xmlSecSize klassSize */
    xmlSecMSCryptoBlockCipherSize,              /* xmlSecSize objSize */

    xmlSecNameAes256Cbc,                        /* const xmlChar* name; */
    xmlSecHrefAes256Cbc,                        /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,       /* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize,        /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,          /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                       /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                       /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,         /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoBlockCipherSetKey,            /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                       /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,          /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,              /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,               /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                       /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                       /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoBlockCipherExecute,           /* xmlSecTransformExecuteMethod execute; */

    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformAes256CbcGetKlass:
 *
 * AES 256 CBC encryption transform klass.
 *
 * Returns: pointer to AES 256 CBC encryption transform.
 */
xmlSecTransformId
xmlSecMSCryptoTransformAes256CbcGetKlass(void) {
    return(&xmlSecMSCryptoAes256CbcKlass);
}

#endif /* XMLSEC_NO_AES */


#ifndef XMLSEC_NO_DES
static xmlSecTransformKlass xmlSecMSCryptoDes3CbcKlass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),       /* size_t klassSize */
    xmlSecMSCryptoBlockCipherSize,      /* size_t objSize */

    xmlSecNameDes3Cbc,                  /* const xmlChar* name; */
    xmlSecHrefDes3Cbc,                  /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,/* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize, /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,   /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,  /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoBlockCipherSetKey,     /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,   /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,       /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,        /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoBlockCipherExecute,    /* xmlSecTransformExecuteMethod execute; */

    NULL,                                /* void* reserved0; */
    NULL,                                /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformDes3CbcGetKlass:
 *
 * Triple DES CBC encryption transform klass.
 *
 * Returns: pointer to Triple DES encryption transform.
 */
xmlSecTransformId
xmlSecMSCryptoTransformDes3CbcGetKlass(void) {
    return(&xmlSecMSCryptoDes3CbcKlass);
}
#endif /* XMLSEC_NO_DES */

#ifndef XMLSEC_NO_AES
static int 
xmlSecMSCryptoAesBlockEncryptCallback(const xmlSecByte * in, xmlSecSize inSize,
                                      xmlSecByte * out, xmlSecSize outSize,
                                      void * key) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx = (xmlSecMSCryptoBlockCipherCtxPtr)key;
    DWORD dwCLen;

    xmlSecAssert2(in != NULL, -1);
    xmlSecAssert2(inSize >= XMLSEC_KW_AES_BLOCK_SIZE, -1);
    xmlSecAssert2(out != NULL, -1);
    xmlSecAssert2(outSize >= inSize, -1);
    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->pubPrivKey != 0, -1);
    xmlSecAssert2(ctx->cryptKey == 0, -1);
    xmlSecAssert2(xmlSecBufferGetSize(&ctx->kwKeyBuffer) == ctx->keySize, -1);

    /* Import this key and get an HCRYPTKEY handle, we do it again and again 
       to ensure we don't go into CBC mode */
    if (!xmlSecMSCryptoImportPlainSessionBlob(ctx->cryptProvider,
        ctx->pubPrivKey,
        ctx->algorithmIdentifier,
        xmlSecBufferGetData(&ctx->kwKeyBuffer),
        xmlSecBufferGetSize(&ctx->kwKeyBuffer),
        TRUE,
        &(ctx->cryptKey)))  {

        xmlSecError(XMLSEC_ERRORS_HERE,
                    NULL,
                    "xmlSecMSCryptoImportPlainSessionBlob",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    /* Set process last block to false, since we handle padding ourselves, and MSCrypto padding
     * can be skipped. I hope this will work .... */
    memcpy(out, in, inSize);
    dwCLen = inSize;
    if(!CryptEncrypt(ctx->cryptKey, 0, FALSE, 0, out, &dwCLen, outSize)) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    NULL,
                    "CryptEncrypt",
                    XMLSEC_ERRORS_R_CRYPTO_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    /* cleanup */
    if (ctx->cryptKey != 0) {
        CryptDestroyKey(ctx->cryptKey);
        ctx->cryptKey = 0;
    }

    return(0);
}

static int
xmlSecMSCryptoAesBlockDecryptCallback(const xmlSecByte * in, xmlSecSize inSize,
                                      xmlSecByte * out, xmlSecSize outSize,
                                      void * key) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx = (xmlSecMSCryptoBlockCipherCtxPtr)key;
    DWORD dwCLen;

    xmlSecAssert2(in != NULL, -1);
    xmlSecAssert2(inSize >= XMLSEC_KW_AES_BLOCK_SIZE, -1);
    xmlSecAssert2(out != NULL, -1);
    xmlSecAssert2(outSize >= inSize, -1);
    xmlSecAssert2(ctx != NULL, -1);
    xmlSecAssert2(ctx->pubPrivKey != 0, -1);
    xmlSecAssert2(ctx->cryptKey == 0, -1);
    xmlSecAssert2(xmlSecBufferGetSize(&ctx->kwKeyBuffer) == ctx->keySize, -1);

    /* Import this key and get an HCRYPTKEY handle, we do it again and again 
       to ensure we don't go into CBC mode */
    if (!xmlSecMSCryptoImportPlainSessionBlob(ctx->cryptProvider,
        ctx->pubPrivKey,
        ctx->algorithmIdentifier,
        xmlSecBufferGetData(&ctx->kwKeyBuffer),
        xmlSecBufferGetSize(&ctx->kwKeyBuffer),
        TRUE,
        &(ctx->cryptKey)))  {

        xmlSecError(XMLSEC_ERRORS_HERE,
                    NULL,
                    "xmlSecMSCryptoImportPlainSessionBlob",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    /* Set process last block to false, since we handle padding ourselves, and MSCrypto padding
     * can be skipped. I hope this will work .... */
    memcpy(out, in, inSize);
    dwCLen = inSize;
    if(!CryptDecrypt(ctx->cryptKey, 0, FALSE, 0, out, &dwCLen)) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    NULL,
                    "CryptEncrypt",
                    XMLSEC_ERRORS_R_CRYPTO_FAILED,
                    XMLSEC_ERRORS_NO_MESSAGE);
        return(-1);
    }

    /* cleanup */
    if (ctx->cryptKey != 0) {
        CryptDestroyKey(ctx->cryptKey);
        ctx->cryptKey = 0;
    }

    return(0);
}

static int
xmlSecMSCryptoKWAesSetKey(xmlSecTransformPtr transform, xmlSecKeyPtr key) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;
    xmlSecBufferPtr buffer;
    xmlSecSize keySize;
    int ret;

    xmlSecAssert2(xmlSecMSCryptoBlockCipherCheckId(transform), -1);
    xmlSecAssert2((transform->operation == xmlSecTransformOperationEncrypt) || (transform->operation == xmlSecTransformOperationDecrypt), -1);
    xmlSecAssert2(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize), -1);
    xmlSecAssert2(key != NULL, -1);
    xmlSecAssert2(xmlSecKeyDataCheckId(xmlSecKeyGetValue(key), xmlSecMSCryptoKeyDataAesId), -1);

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert2(ctx != NULL, -1);

    buffer = xmlSecKeyDataBinaryValueGetBuffer(xmlSecKeyGetValue(key));
    xmlSecAssert2(buffer != NULL, -1);

    keySize = xmlSecBufferGetSize(buffer);
    if(keySize < ctx->keySize) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    NULL,
                    XMLSEC_ERRORS_R_INVALID_KEY_DATA_SIZE,
                    "key=%d;expected=%d",
                    keySize, ctx->keySize);
        return(-1);
    }

    ret = xmlSecBufferSetData(&(ctx->kwKeyBuffer),
                            xmlSecBufferGetData(buffer),
                            ctx->keySize);
    if(ret < 0) {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    "xmlSecBufferSetData",
                    XMLSEC_ERRORS_R_XMLSEC_FAILED,
                    "expected-size=%d", 
                    ctx->keySize);
        return(-1);
    }

    return(0);
}

static int
xmlSecMSCryptoKWAesExecute(xmlSecTransformPtr transform, int last, xmlSecTransformCtxPtr transformCtx) {
    xmlSecMSCryptoBlockCipherCtxPtr ctx;
    xmlSecBufferPtr in, out;
    xmlSecSize inSize, outSize;
    int ret;

    xmlSecAssert2(xmlSecMSCryptoBlockCipherCheckId(transform), -1);
    xmlSecAssert2((transform->operation == xmlSecTransformOperationEncrypt) || (transform->operation == xmlSecTransformOperationDecrypt), -1);
    xmlSecAssert2(xmlSecTransformCheckSize(transform, xmlSecMSCryptoBlockCipherSize), -1);
    xmlSecAssert2(transformCtx != NULL, -1);

    ctx = xmlSecMSCryptoBlockCipherGetCtx(transform);
    xmlSecAssert2(ctx != NULL, -1);

    in = &(transform->inBuf);
    out = &(transform->outBuf);
    inSize = xmlSecBufferGetSize(in);
    outSize = xmlSecBufferGetSize(out);
    xmlSecAssert2(outSize == 0, -1);

    if(transform->status == xmlSecTransformStatusNone) {
        transform->status = xmlSecTransformStatusWorking;
    }

    if((transform->status == xmlSecTransformStatusWorking) && (last == 0)) {
        /* just do nothing */
    } else  if((transform->status == xmlSecTransformStatusWorking) && (last != 0)) {
        if((inSize % 8) != 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                        NULL,
                        XMLSEC_ERRORS_R_INVALID_SIZE,
                        "size=%d(not 8 bytes aligned)", inSize);
            return(-1);
        }

        if(transform->operation == xmlSecTransformOperationEncrypt) {
            /* the encoded key might be 8 bytes longer plus 8 bytes just in case */
            outSize = inSize + XMLSEC_KW_AES_MAGIC_BLOCK_SIZE +
                               XMLSEC_KW_AES_BLOCK_SIZE;
        } else {
            outSize = inSize + XMLSEC_KW_AES_BLOCK_SIZE;
        }

        ret = xmlSecBufferSetMaxSize(out, outSize);
        if(ret < 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                        "xmlSecBufferSetMaxSize",
                        XMLSEC_ERRORS_R_XMLSEC_FAILED,
                        "outSize=%d", outSize);
            return(-1);
        }

        if(transform->operation == xmlSecTransformOperationEncrypt) {
            ret = xmlSecKWAesEncode(xmlSecMSCryptoAesBlockEncryptCallback, ctx,
                                    xmlSecBufferGetData(in), inSize,
                                    xmlSecBufferGetData(out), outSize);
            if(ret < 0) {
                xmlSecError(XMLSEC_ERRORS_HERE,
                            xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                            "xmlSecKWAesEncode",
                            XMLSEC_ERRORS_R_XMLSEC_FAILED,
                            XMLSEC_ERRORS_NO_MESSAGE);
                return(-1);
            }
            outSize = ret;
        } else {
            ret = xmlSecKWAesDecode(xmlSecMSCryptoAesBlockDecryptCallback, ctx,
                                    xmlSecBufferGetData(in), inSize,
                                    xmlSecBufferGetData(out), outSize);
            if(ret < 0) {
                xmlSecError(XMLSEC_ERRORS_HERE,
                            xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                            "xmlSecKWAesEncode",
                            XMLSEC_ERRORS_R_XMLSEC_FAILED,
                            XMLSEC_ERRORS_NO_MESSAGE);
                return(-1);
            }
            outSize = ret;
        }

        ret = xmlSecBufferSetSize(out, outSize);
        if(ret < 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                        "xmlSecBufferSetSize",
                        XMLSEC_ERRORS_R_XMLSEC_FAILED,
                        "outSize=%d", outSize);
            return(-1);
        }

        ret = xmlSecBufferRemoveHead(in, inSize);
        if(ret < 0) {
            xmlSecError(XMLSEC_ERRORS_HERE,
                        xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                        "xmlSecBufferRemoveHead",
                        XMLSEC_ERRORS_R_XMLSEC_FAILED,
                        "inSize%d", inSize);
            return(-1);
        }

        transform->status = xmlSecTransformStatusFinished;
    } else if(transform->status == xmlSecTransformStatusFinished) {
        /* the only way we can get here is if there is no input */
        xmlSecAssert2(xmlSecBufferGetSize(&(transform->inBuf)) == 0, -1);
    } else {
        xmlSecError(XMLSEC_ERRORS_HERE,
                    xmlSecErrorsSafeString(xmlSecTransformGetName(transform)),
                    NULL,
                    XMLSEC_ERRORS_R_INVALID_STATUS,
                    "status=%d", transform->status);
        return(-1);
    }
    return(0);
}

/*********************************************************************
 *
 * AES KW cipher transforms
 *
 ********************************************************************/

/*
 * The AES-128 kew wrapper transform klass.
 */
static xmlSecTransformKlass xmlSecMSCryptoKWAes128Klass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),               /* xmlSecSize klassSize */
    xmlSecMSCryptoBlockCipherSize,              /* xmlSecSize objSize */

    xmlSecNameKWAes128,                         /* const xmlChar* name; */
    xmlSecHrefKWAes128,                         /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,       /* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize,        /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,          /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                       /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                       /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,         /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoKWAesSetKey,                  /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                       /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,          /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,              /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,               /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                       /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                       /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoKWAesExecute,                 /* xmlSecTransformExecuteMethod execute; */

    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformKWAes128GetKlass:
 *
 * The AES-128 kew wrapper transform klass.
 *
 * Returns: AES-128 kew wrapper transform klass.
 */
xmlSecTransformId
xmlSecMSCryptoTransformKWAes128GetKlass(void) {
    return(&xmlSecMSCryptoKWAes128Klass);
}


/*
 * The AES-192 kew wrapper transform klass.
 */
static xmlSecTransformKlass xmlSecMSCryptoKWAes192Klass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),               /* xmlSecSize klassSize */
    xmlSecMSCryptoBlockCipherSize,              /* xmlSecSize objSize */

    xmlSecNameKWAes192,                         /* const xmlChar* name; */
    xmlSecHrefKWAes192,                         /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,       /* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize,        /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,          /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                       /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                       /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,         /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoKWAesSetKey,                  /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                       /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,          /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,              /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,               /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                       /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                       /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoKWAesExecute,                 /* xmlSecTransformExecuteMethod execute; */

    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformKWAes192GetKlass:
 *
 * The AES-192 kew wrapper transform klass.
 *
 * Returns: AES-192 kew wrapper transform klass.
 */
xmlSecTransformId
xmlSecMSCryptoTransformKWAes192GetKlass(void) {
    return(&xmlSecMSCryptoKWAes192Klass);
}

/*
 * The AES-256 kew wrapper transform klass.
 */
static xmlSecTransformKlass xmlSecMSCryptoKWAes256Klass = {
    /* klass/object sizes */
    sizeof(xmlSecTransformKlass),               /* xmlSecSize klassSize */
    xmlSecMSCryptoBlockCipherSize,              /* xmlSecSize objSize */

    xmlSecNameKWAes256,                         /* const xmlChar* name; */
    xmlSecHrefKWAes256,                         /* const xmlChar* href; */
    xmlSecTransformUsageEncryptionMethod,       /* xmlSecAlgorithmUsage usage; */

    xmlSecMSCryptoBlockCipherInitialize,        /* xmlSecTransformInitializeMethod initialize; */
    xmlSecMSCryptoBlockCipherFinalize,          /* xmlSecTransformFinalizeMethod finalize; */
    NULL,                                       /* xmlSecTransformNodeReadMethod readNode; */
    NULL,                                       /* xmlSecTransformNodeWriteMethod writeNode; */
    xmlSecMSCryptoBlockCipherSetKeyReq,         /* xmlSecTransformSetKeyMethod setKeyReq; */
    xmlSecMSCryptoKWAesSetKey,                  /* xmlSecTransformSetKeyMethod setKey; */
    NULL,                                       /* xmlSecTransformValidateMethod validate; */
    xmlSecTransformDefaultGetDataType,          /* xmlSecTransformGetDataTypeMethod getDataType; */
    xmlSecTransformDefaultPushBin,              /* xmlSecTransformPushBinMethod pushBin; */
    xmlSecTransformDefaultPopBin,               /* xmlSecTransformPopBinMethod popBin; */
    NULL,                                           /* xmlSecTransformPushXmlMethod pushXml; */
    NULL,                                       /* xmlSecTransformPopXmlMethod popXml; */
    xmlSecMSCryptoKWAesExecute,                 /* xmlSecTransformExecuteMethod execute; */

    NULL,                                       /* void* reserved0; */
    NULL,                                       /* void* reserved1; */
};

/**
 * xmlSecMSCryptoTransformKWAes256GetKlass:
 *
 * The AES-256 kew wrapper transform klass.
 *
 * Returns: AES-256 kew wrapper transform klass.
 */
xmlSecTransformId
xmlSecMSCryptoTransformKWAes256GetKlass(void) {
    return(&xmlSecMSCryptoKWAes256Klass);
}

#endif /* XMLSEC_NO_AES */