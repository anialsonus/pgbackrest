/***********************************************************************************************************************************
Test Block Cipher
***********************************************************************************************************************************/
#include "common/io/io.h"

/***********************************************************************************************************************************
Data for testing
***********************************************************************************************************************************/
#define TEST_CIPHER                                                 "aes-256-cbc"
#define TEST_PASS                                                   "areallybadpassphrase"
#define TEST_PASS_SIZE                                              strlen(TEST_PASS)
#define TEST_PLAINTEXT                                              "plaintext"
#define TEST_BUFFER_SIZE                                            256

/***********************************************************************************************************************************
Test Run
***********************************************************************************************************************************/
void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    const Buffer *testPass = BUFSTRDEF(TEST_PASS);
    const Buffer *testPlainText = BUFSTRDEF(TEST_PLAINTEXT);

    // *****************************************************************************************************************************
    if (testBegin("Common"))
    {
        TEST_RESULT_BOOL(cryptoIsInit(), false, "crypto is not initialized");
        TEST_RESULT_VOID(cryptoInit(), "initialize crypto");
        TEST_RESULT_BOOL(cryptoIsInit(), true, "crypto is initialized");
        TEST_RESULT_VOID(cryptoInit(), "initialize crypto again");

        // -------------------------------------------------------------------------------------------------------------------------
        cryptoInit();

        TEST_RESULT_VOID(cryptoError(false, "no error here"), "no error");

        EVP_MD_CTX *context = EVP_MD_CTX_create();
        TEST_ERROR(
            cryptoError(EVP_DigestInit_ex(context, NULL, NULL) != 1, "unable to initialize hash context"), CryptoError,
            "unable to initialize hash context: [101187723] no digest set");
        EVP_MD_CTX_destroy(context);

        TEST_ERROR(cryptoError(true, "no error"), CryptoError, "no error: [0] no details available");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ERROR(cipherType(strNew(BOGUS_STR)), AssertError, "invalid cipher name 'BOGUS'");
        TEST_RESULT_UINT(cipherType(strNew("none")), cipherTypeNone, "none type");
        TEST_RESULT_UINT(cipherType(strNew("aes-256-cbc")), cipherTypeAes256Cbc, "aes-256-cbc type");

        TEST_ERROR(cipherTypeName((CipherType)2), AssertError, "invalid cipher type 2");
        TEST_RESULT_STR(strPtr(cipherTypeName(cipherTypeNone)), "none", "none name");
        TEST_RESULT_STR(strPtr(cipherTypeName(cipherTypeAes256Cbc)), "aes-256-cbc", "aes-256-cbc name");

        // Test if the buffer was overrun
        // -------------------------------------------------------------------------------------------------------------------------
        size_t bufferSize = 256;
        unsigned char *buffer = memNew(bufferSize + 1);

        cryptoRandomBytes(buffer, bufferSize);
        TEST_RESULT_BOOL(buffer[bufferSize] == 0, true, "check that buffer did not overrun (though random byte could be 0)");

        // Count bytes that are not zero (there shouldn't be all zeroes)
        // -------------------------------------------------------------------------------------------------------------------------
        int nonZeroTotal = 0;

        for (unsigned int charIdx = 0; charIdx < bufferSize; charIdx++)
            if (buffer[charIdx] != 0)                               // {uncovered - ok if there are no zeros}
                nonZeroTotal++;

        TEST_RESULT_INT_NE(nonZeroTotal, 0, "check that there are non-zero values in the buffer");
    }

    // *****************************************************************************************************************************
    if (testBegin("CipherBlock"))
    {
        // Cipher and digest errors
        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ERROR(
            cipherBlockNewC(
                cipherModeEncrypt, BOGUS_STR, (unsigned char *)TEST_PASS, TEST_PASS_SIZE, NULL), AssertError,
                "unable to load cipher 'BOGUS'");
        TEST_ERROR(
            cipherBlockNew(
                cipherModeEncrypt, cipherTypeAes256Cbc, testPass, strNew(BOGUS_STR)), AssertError, "unable to load digest 'BOGUS'");

        // Initialization of object
        // -------------------------------------------------------------------------------------------------------------------------
        CipherBlock *cipherBlock = cipherBlockNewC(
            cipherModeEncrypt, TEST_CIPHER, (unsigned char *)TEST_PASS, TEST_PASS_SIZE, NULL);
        TEST_RESULT_STR(memContextName(cipherBlock->memContext), "cipherBlock", "mem context name is valid");
        TEST_RESULT_INT(cipherBlock->mode, cipherModeEncrypt, "mode is valid");
        TEST_RESULT_INT(cipherBlock->passSize, TEST_PASS_SIZE, "passphrase size is valid");
        TEST_RESULT_BOOL(memcmp(cipherBlock->pass, TEST_PASS, TEST_PASS_SIZE) == 0, true, "passphrase is valid");
        TEST_RESULT_BOOL(cipherBlock->saltDone, false, "salt done is false");
        TEST_RESULT_BOOL(cipherBlock->processDone, false, "process done is false");
        TEST_RESULT_INT(cipherBlock->headerSize, 0, "header size is 0");
        TEST_RESULT_PTR_NE(cipherBlock->cipher, NULL, "cipher is set");
        TEST_RESULT_PTR_NE(cipherBlock->digest, NULL, "digest is set");
        TEST_RESULT_PTR(cipherBlock->cipherContext, NULL, "cipher context is not set");

        TEST_RESULT_VOID(cipherBlockFree(cipherBlock), "free cipher block");
        TEST_RESULT_VOID(cipherBlockFree(NULL), "free null cipher block");

        // Encrypt
        // -------------------------------------------------------------------------------------------------------------------------
        Buffer *encryptBuffer = bufNew(TEST_BUFFER_SIZE);

        CipherBlock *blockEncrypt = cipherBlockNew(cipherModeEncrypt, cipherTypeAes256Cbc, testPass, NULL);
        IoFilter *blockEncryptFilter = cipherBlockFilter(blockEncrypt);

        TEST_RESULT_INT(
            cipherBlockProcessSizeC(blockEncrypt, strlen(TEST_PLAINTEXT)),
            strlen(TEST_PLAINTEXT) + EVP_MAX_BLOCK_LENGTH + CIPHER_BLOCK_MAGIC_SIZE + PKCS5_SALT_LEN, "check process size");

        bufLimitSet(encryptBuffer, CIPHER_BLOCK_MAGIC_SIZE);
        ioFilterProcessInOut(blockEncryptFilter, testPlainText, encryptBuffer);
        TEST_RESULT_INT(bufUsed(encryptBuffer), CIPHER_BLOCK_MAGIC_SIZE, "cipher size is magic size");
        TEST_RESULT_BOOL(ioFilterInputSame(blockEncryptFilter), true,  "filter needs same input");

        bufLimitSet(encryptBuffer, CIPHER_BLOCK_MAGIC_SIZE + PKCS5_SALT_LEN);
        ioFilterProcessInOut(blockEncryptFilter, testPlainText, encryptBuffer);
        TEST_RESULT_BOOL(ioFilterInputSame(blockEncryptFilter), false,  "filter does not need same input");

        TEST_RESULT_BOOL(blockEncrypt->saltDone, true, "salt done is true");
        TEST_RESULT_BOOL(blockEncrypt->processDone, true, "process done is true");
        TEST_RESULT_INT(blockEncrypt->headerSize, 0, "header size is 0");
        TEST_RESULT_INT(bufUsed(encryptBuffer), CIPHER_BLOCK_HEADER_SIZE, "cipher size is header len");

        TEST_RESULT_INT(
            cipherBlockProcessSizeC(blockEncrypt, strlen(TEST_PLAINTEXT)),
            strlen(TEST_PLAINTEXT) + EVP_MAX_BLOCK_LENGTH, "check process size");

        bufLimitSet(
            encryptBuffer, CIPHER_BLOCK_MAGIC_SIZE + PKCS5_SALT_LEN + (size_t)EVP_CIPHER_block_size(blockEncrypt->cipher) / 2);
        ioFilterProcessInOut(blockEncryptFilter, testPlainText, encryptBuffer);
        bufLimitSet(
            encryptBuffer, CIPHER_BLOCK_MAGIC_SIZE + PKCS5_SALT_LEN + (size_t)EVP_CIPHER_block_size(blockEncrypt->cipher));
        ioFilterProcessInOut(blockEncryptFilter, testPlainText, encryptBuffer);
        bufLimitClear(encryptBuffer);

        TEST_RESULT_INT(
            bufUsed(encryptBuffer), CIPHER_BLOCK_HEADER_SIZE + (size_t)EVP_CIPHER_block_size(blockEncrypt->cipher),
            "cipher size increases by one block");
        TEST_RESULT_BOOL(ioFilterDone(blockEncryptFilter), false,  "filter is not done");

        ioFilterProcessInOut(blockEncryptFilter, NULL, encryptBuffer);
        TEST_RESULT_INT(
            bufUsed(encryptBuffer), CIPHER_BLOCK_HEADER_SIZE + (size_t)(EVP_CIPHER_block_size(blockEncrypt->cipher) * 2),
            "cipher size increases by one block on flush");
        TEST_RESULT_BOOL(ioFilterDone(blockEncryptFilter), true,  "filter is done");

        cipherBlockFree(blockEncrypt);

        // Decrypt in one pass
        // -------------------------------------------------------------------------------------------------------------------------
        Buffer *decryptBuffer = bufNew(TEST_BUFFER_SIZE);

        CipherBlock *blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        IoFilter *blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        TEST_RESULT_INT(
            cipherBlockProcessSizeC(blockDecrypt, bufUsed(encryptBuffer)), bufUsed(encryptBuffer) + EVP_MAX_BLOCK_LENGTH,
            "check process size");

        ioFilterProcessInOut(blockDecryptFilter, encryptBuffer, decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), EVP_CIPHER_block_size(blockDecrypt->cipher), "decrypt size is one block");

        ioFilterProcessInOut(blockDecryptFilter, NULL, decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), strlen(TEST_PLAINTEXT) * 2, "check final decrypt size");

        TEST_RESULT_STR(strPtr(strNewBuf(decryptBuffer)), TEST_PLAINTEXT TEST_PLAINTEXT, "check final decrypt buffer");

        cipherBlockFree(blockDecrypt);

        // Decrypt in small chunks to test buffering
        // -------------------------------------------------------------------------------------------------------------------------
        blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        bufUsedZero(decryptBuffer);

        ioFilterProcessInOut(blockDecryptFilter, bufNewC(bufPtr(encryptBuffer), CIPHER_BLOCK_MAGIC_SIZE), decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), 0, "no decrypt since header read is not complete");
        TEST_RESULT_BOOL(blockDecrypt->saltDone, false, "salt done is false");
        TEST_RESULT_BOOL(blockDecrypt->processDone, false, "process done is false");
        TEST_RESULT_INT(blockDecrypt->headerSize, CIPHER_BLOCK_MAGIC_SIZE, "check header size");
        TEST_RESULT_BOOL(
            memcmp(blockDecrypt->header, CIPHER_BLOCK_MAGIC, CIPHER_BLOCK_MAGIC_SIZE) == 0, true, "check header magic");

        ioFilterProcessInOut(
            blockDecryptFilter, bufNewC(bufPtr(encryptBuffer) + CIPHER_BLOCK_MAGIC_SIZE, PKCS5_SALT_LEN), decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), 0, "no decrypt since no data processed yet");
        TEST_RESULT_BOOL(blockDecrypt->saltDone, true, "salt done is true");
        TEST_RESULT_BOOL(blockDecrypt->processDone, false, "process done is false");
        TEST_RESULT_INT(blockDecrypt->headerSize, CIPHER_BLOCK_MAGIC_SIZE, "check header size (not increased)");
        TEST_RESULT_BOOL(
            memcmp(
                blockDecrypt->header + CIPHER_BLOCK_MAGIC_SIZE, bufPtr(encryptBuffer) + CIPHER_BLOCK_MAGIC_SIZE,
                PKCS5_SALT_LEN) == 0,
            true, "check header salt");

        ioFilterProcessInOut(
            blockDecryptFilter,
            bufNewC(bufPtr(encryptBuffer) + CIPHER_BLOCK_HEADER_SIZE, bufUsed(encryptBuffer) - CIPHER_BLOCK_HEADER_SIZE),
            decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), EVP_CIPHER_block_size(blockDecrypt->cipher), "decrypt size is one block");

        ioFilterProcessInOut(blockDecryptFilter, NULL, decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), strlen(TEST_PLAINTEXT) * 2, "check final decrypt size");

        TEST_RESULT_STR(strPtr(strNewBuf(decryptBuffer)), TEST_PLAINTEXT TEST_PLAINTEXT, "check final decrypt buffer");

        cipherBlockFree(blockDecrypt);

        // Encrypt zero byte file and decrypt it
        // -------------------------------------------------------------------------------------------------------------------------
        blockEncrypt = cipherBlockNew(cipherModeEncrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockEncryptFilter = cipherBlockFilter(blockEncrypt);

        bufUsedZero(encryptBuffer);

        ioFilterProcessInOut(blockEncryptFilter, NULL, encryptBuffer);
        TEST_RESULT_INT(bufUsed(encryptBuffer), 32, "check remaining size");

        cipherBlockFree(blockEncrypt);

        blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        bufUsedZero(decryptBuffer);

        ioFilterProcessInOut(blockDecryptFilter, encryptBuffer, decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), 0, "0 bytes processed");
        ioFilterProcessInOut(blockDecryptFilter, NULL, decryptBuffer);
        TEST_RESULT_INT(bufUsed(decryptBuffer), 0, "0 bytes on flush");

        cipherBlockFree(blockDecrypt);

        // Invalid cipher header
        // -------------------------------------------------------------------------------------------------------------------------
        blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        TEST_ERROR(
            ioFilterProcessInOut(blockDecryptFilter, BUFSTRDEF("1234567890123456"), decryptBuffer), CryptoError,
            "cipher header invalid");

        cipherBlockFree(blockDecrypt);

        // Invalid encrypted data cannot be flushed
        // -------------------------------------------------------------------------------------------------------------------------
        blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        bufUsedZero(decryptBuffer);

        ioFilterProcessInOut(blockDecryptFilter, BUFSTRDEF(CIPHER_BLOCK_MAGIC "12345678"), decryptBuffer);
        ioFilterProcessInOut(blockDecryptFilter, BUFSTRDEF("1234567890123456"), decryptBuffer);

        TEST_ERROR(ioFilterProcessInOut(blockDecryptFilter, NULL, decryptBuffer), CryptoError, "unable to flush");

        cipherBlockFree(blockDecrypt);

        // File with no header should not flush
        // -------------------------------------------------------------------------------------------------------------------------
        blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        bufUsedZero(decryptBuffer);

        TEST_ERROR(ioFilterProcessInOut(blockDecryptFilter, NULL, decryptBuffer), CryptoError, "cipher header missing");

        cipherBlockFree(blockDecrypt);

        // File with header only should error
        // -------------------------------------------------------------------------------------------------------------------------
        blockDecrypt = cipherBlockNew(cipherModeDecrypt, cipherTypeAes256Cbc, testPass, NULL);
        blockDecryptFilter = cipherBlockFilter(blockDecrypt);

        bufUsedZero(decryptBuffer);

        ioFilterProcessInOut(blockDecryptFilter, BUFSTRDEF(CIPHER_BLOCK_MAGIC "12345678"), decryptBuffer);
        TEST_ERROR(ioFilterProcessInOut(blockDecryptFilter, NULL, decryptBuffer), CryptoError, "unable to flush");

        cipherBlockFree(blockDecrypt);
    }

    // *****************************************************************************************************************************
    if (testBegin("CryptoHash"))
    {
        CryptoHash *hash = NULL;
        IoFilter *hashFilter = NULL;

        TEST_ERROR(cryptoHashNew(strNew(BOGUS_STR)), AssertError, "unable to load hash 'BOGUS'");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ASSIGN(hash, cryptoHashNew(strNew(HASH_TYPE_SHA1)), "create sha1 hash");
        TEST_RESULT_VOID(cryptoHashFree(hash), "    free hash");
        TEST_RESULT_VOID(cryptoHashFree(NULL), "    free null hash");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ASSIGN(hash, cryptoHashNew(strNew(HASH_TYPE_SHA1)), "create sha1 hash");
        TEST_RESULT_STR(strPtr(bufHex(cryptoHash(hash))), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "    check empty hash");
        TEST_RESULT_STR(strPtr(bufHex(cryptoHash(hash))), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "    check empty hash again");
        TEST_RESULT_VOID(cryptoHashFree(hash), "    free hash");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ASSIGN(hash, cryptoHashNew(strNew(HASH_TYPE_SHA1)), "create sha1 hash");
        TEST_ASSIGN(hashFilter, cryptoHashFilter(hash), "create sha1 hash");
        TEST_RESULT_VOID(cryptoHashProcessC(hash, (const unsigned char *)"1", 1), "    add 1");
        TEST_RESULT_VOID(cryptoHashProcessStr(hash, strNew("2")), "    add 2");
        TEST_RESULT_VOID(ioFilterProcessIn(hashFilter, BUFSTRDEF("3")), "    add 3");
        TEST_RESULT_VOID(ioFilterProcessIn(hashFilter, BUFSTRDEF("4")), "    add 4");
        TEST_RESULT_VOID(ioFilterProcessIn(hashFilter, BUFSTRDEF("5")), "    add 5");

        TEST_RESULT_STR(
            strPtr(varStr(ioFilterResult(hashFilter))), "8cb2237d0679ca88db6464eac60da96345513964", "    check small hash");
        TEST_RESULT_VOID(cryptoHashFree(hash), "    free hash");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ASSIGN(hash, cryptoHashNew(strNew(HASH_TYPE_MD5)), "create md5 hash");
        TEST_RESULT_STR(strPtr(bufHex(cryptoHash(hash))), "d41d8cd98f00b204e9800998ecf8427e", "    check empty hash");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ASSIGN(hash, cryptoHashNew(strNew(HASH_TYPE_SHA256)), "create sha256 hash");
        TEST_RESULT_STR(
            strPtr(bufHex(cryptoHash(hash))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "    check empty hash");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_RESULT_STR(
            strPtr(bufHex(cryptoHashOne(strNew(HASH_TYPE_SHA1), BUFSTRDEF("12345")))), "8cb2237d0679ca88db6464eac60da96345513964",
            "    check small hash");
        TEST_RESULT_STR(
            strPtr(bufHex(cryptoHashOneStr(strNew(HASH_TYPE_SHA1), strNew("12345")))), "8cb2237d0679ca88db6464eac60da96345513964",
            "    check small hash");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_RESULT_STR(
            strPtr(
                bufHex(
                    cryptoHmacOne(
                        strNew(HASH_TYPE_SHA256),
                        BUFSTRDEF("AWS4wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                        BUFSTRDEF("20170412")))),
            "8b05c497afe9e1f42c8ada4cb88392e118649db1e5c98f0f0fb0a158bdd2dd76",
            "    check hmac");
    }

    FUNCTION_HARNESS_RESULT_VOID();
}
