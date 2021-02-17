/***********************************************************************************************************************************
GCS Storage
***********************************************************************************************************************************/
#include "build.auto.h"

#include <string.h>

#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

#include "common/crypto/common.h"
#include "common/crypto/hash.h"
#include "common/encode.h"
#include "common/debug.h"
#include "common/io/http/client.h"
#include "common/io/http/common.h"
#include "common/io/socket/client.h"
#include "common/io/tls/client.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/regExp.h"
#include "common/type/json.h"
#include "common/type/object.h"
#include "storage/gcs/read.h"
#include "storage/gcs/storage.intern.h"
#include "storage/gcs/write.h"
#include "storage/posix/storage.h"

/***********************************************************************************************************************************
Storage type
***********************************************************************************************************************************/
STRING_EXTERN(STORAGE_GCS_TYPE_STR,                                 STORAGE_GCS_TYPE);

/***********************************************************************************************************************************
GCS http headers
***********************************************************************************************************************************/
// STRING_STATIC(GCS_HEADER_VERSION_STR,                               "x-ms-version");
// STRING_STATIC(GCS_HEADER_VERSION_VALUE_STR,                         "2019-02-02");

/***********************************************************************************************************************************
GCS query tokens
***********************************************************************************************************************************/
// STRING_STATIC(GCS_QUERY_MARKER_STR,                                 "marker");
// STRING_EXTERN(GCS_QUERY_COMP_STR,                                   GCS_QUERY_COMP);
STRING_STATIC(GCS_QUERY_DELIMITER_STR,                              "delimiter");
STRING_STATIC(GCS_QUERY_PREFIX_STR,                                 "prefix");
// STRING_EXTERN(GCS_QUERY_RESTYPE_STR,                                GCS_QUERY_RESTYPE);
// STRING_STATIC(GCS_QUERY_SIG_STR,                                    "sig");
//
// STRING_STATIC(GCS_QUERY_VALUE_LIST_STR,                             "list");
// STRING_EXTERN(GCS_QUERY_VALUE_CONTAINER_STR,                        GCS_QUERY_VALUE_CONTAINER);

/***********************************************************************************************************************************
XML tags
***********************************************************************************************************************************/
// STRING_STATIC(GCS_XML_TAG_BLOB_PREFIX_STR,                          "BlobPrefix");
// STRING_STATIC(GCS_XML_TAG_BLOB_STR,                                 "Blob");
// STRING_STATIC(GCS_XML_TAG_BLOBS_STR,                                "Blobs");
// STRING_STATIC(GCS_XML_TAG_CONTENT_LENGTH_STR,                       "Content-Length");
// STRING_STATIC(GCS_XML_TAG_LAST_MODIFIED_STR,                        "Last-Modified");
// STRING_STATIC(GCS_XML_TAG_NEXT_MARKER_STR,                          "NextMarker");
// STRING_STATIC(GCS_XML_TAG_NAME_STR,                                 "Name");
// STRING_STATIC(GCS_XML_TAG_PROPERTIES_STR,                           "Properties");

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct StorageGcs
{
    STORAGE_COMMON_MEMBER;
    MemContext *memContext;
    HttpClient *httpClient;                                         // Http client to service requests
    StringList *headerRedactList;                                   // List of headers to redact from logging
    // StringList *queryRedactList;                                    // List of query keys to redact from logging

    bool write;                                                     // Storage is writable
    const String *bucket;                                           // Bucket to store data in
    const String *project;                                          // Project
    StorageGcsKeyType keyType;                                      // Auth key type
    const String *credential;                                       // Credential !!!
    const String *privateKey;                                       // Private key in PEM format
    // const String *sharedKey;                                        // Shared key
    // const HttpQuery *sasKey;                                        // SAS key
    const String *endpoint;                                         // Endpoint
    size_t blockSize;                                               // Block size for multi-block upload
    // const String *uriPrefix;                                        // Account/container prefix

    // uint64_t fileId;                                                // Id to used to make file block identifiers unique
};

/***********************************************************************************************************************************
Get authentication header for service keys

Based on the documentation at https://developers.google.com/identity/protocols/oauth2/service-account#httprest
***********************************************************************************************************************************/
// Helper to convert base64 encoding to base64url
static String *
storageGcsEncodeBase64Url(const Buffer *source)
{
    Buffer *base64 = bufNew(encodeToStrSize(encodeBase64, bufSize(source)) + 1);
    encodeToStr(encodeBase64, bufPtrConst(source), bufSize(source), (char *)bufPtr(base64));

    for (unsigned int charIdx = 0; charIdx <= bufSize(base64); charIdx++)
    {
        if (bufPtr(base64)[charIdx] == 0)
            break;

        switch (bufPtr(base64)[charIdx])
        {
            case '+':
                bufPtr(base64)[charIdx] = '-';
                break;

            case '/':
                bufPtr(base64)[charIdx] = '_';
                break;

            case '=':
                bufPtr(base64)[charIdx] = '\0';
                break;
        }
    }

    return strNew((char *)bufPtr(base64));
}

// Helper to construct a JSON Web Token
static String *
storageGcsAuthJwt(StorageGcs *this, time_t timeBegin)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_GCS, this);
        FUNCTION_TEST_PARAM(TIME, timeBegin);
    FUNCTION_TEST_END();

    // Static header
    String *result = strNew("eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.");

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Add claim
        String *claim = strNewFmt(
            "{\"iss\":\"%s\",\"scope\":\"https://www.googleapis.com/auth/devstorage.read%s\","
            "\"aud\":\"https://oauth2.googleapis.com/token\",\"exp\":%" PRIu64 ",\"iat\":%" PRIu64 "}",
            strZ(this->credential), this->write ? "_write" : "_only", (uint64_t)timeBegin + 3600, (uint64_t)timeBegin);

        strCat(result, storageGcsEncodeBase64Url(BUFSTR(claim)));

        // Sign with RSA key !!! NEED TO MAKE SURE OPENSSL STUFF GETS FREED ON ERROR
        cryptoInit();

        BIO *bo = BIO_new(BIO_s_mem());
        BIO_write(bo, strZ(this->privateKey), (int)strSize(this->privateKey));

        EVP_PKEY *privateKey = PEM_read_bio_PrivateKey(bo, NULL, NULL, NULL);
        cryptoError(privateKey == NULL, "unable to read PEM");
        BIO_free(bo);

        EVP_MD_CTX *sign = EVP_MD_CTX_create();
        cryptoError(EVP_DigestSignInit(sign, NULL, EVP_sha256(), NULL, privateKey) <= 0, "unable to init");
        cryptoError(
            EVP_DigestSignUpdate(sign, (unsigned char *)strZ(result), (unsigned int)strSize(result)) <= 0, "unable to update");

        size_t signatureLen = 0;
        cryptoError(EVP_DigestSignFinal(sign, NULL, &signatureLen) <= 0, "unable to get size");

        Buffer *signature = bufNew(signatureLen);

        cryptoError(EVP_DigestSignFinal(sign, bufPtr(signature), &signatureLen) <= 0, "unable to finalize");

#if OPENSSL_VERSION_NUMBER < 0x10100000L
        EVP_MD_CTX_cleanup(sign);
#else
        EVP_MD_CTX_free(sign);
#endif
        EVP_PKEY_free(privateKey);

        // Add signature
        strCatChr(result, '.');
        strCat(result, storageGcsEncodeBase64Url(signature));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_TEST_RETURN(result);
}

static String *
storageGcsAuthToken(StorageGcs *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_GCS, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    String *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        HttpClient *authClient = httpClientNew(
            tlsClientNew(sckClientNew(STRDEF("oauth2.googleapis.com"), 443, 10000), STRDEF("oauth2.googleapis.com"),
            10000, true, false, false), 10000);

        String *content = strNewFmt(
            "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Ajwt-bearer&assertion=%s",
            strZ(storageGcsAuthJwt(this, time(NULL))));

        HttpHeader *header = httpHeaderNew(NULL);
        httpHeaderAdd(header, HTTP_HEADER_HOST_STR, STRDEF("oauth2.googleapis.com"));
        httpHeaderAdd(header, STRDEF("Content-Type"), STRDEF("application/x-www-form-urlencoded"));
        httpHeaderAdd(header, HTTP_HEADER_CONTENT_LENGTH_STR, strNewFmt("%zu", strSize(content)));

        HttpRequest *request = httpRequestNewP(
            authClient, HTTP_VERB_POST_STR, STRDEF("/token"), NULL, .header = header, .content = BUFSTR(content));
        HttpResponse *response = httpRequestResponse(request, true);

        KeyValue *kvResponse = jsonToKv(strNewBuf(httpResponseContent(response)));

        // Check for an error
        const String *error = varStr(kvGet(kvResponse, VARSTRDEF("error")));

        if (error != NULL)
        {
            const String *description = varStr(kvGet(kvResponse, VARSTRDEF("error_description")));

            THROW_FMT(FormatError, "unable to get authentication token: [%s] %s", strZ(error), strZNull(description));
        }

        // Check for token
        const String *tokenType = varStr(kvGet(kvResponse, VARSTRDEF("token_type")));
        const String *token = varStr(kvGet(kvResponse, VARSTRDEF("access_token")));

        if (tokenType == NULL || token == NULL)
            THROW(FormatError, "unable to find authentication token in response");

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = strNewFmt("%s %s", strZ(tokenType), strZ(token));
        }
        MEM_CONTEXT_PRIOR_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_TEST_RETURN(result);
}

/***********************************************************************************************************************************
Generate authorization header and add it to the supplied header list
***********************************************************************************************************************************/
static void
storageGcsAuth(
    StorageGcs *this, const String *verb, const String *uri, const HttpQuery *query, const String *dateTime, HttpHeader *httpHeader)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_GCS, this);
        FUNCTION_TEST_PARAM(STRING, verb);
        FUNCTION_TEST_PARAM(STRING, uri);
        FUNCTION_TEST_PARAM(HTTP_QUERY, query);
        FUNCTION_TEST_PARAM(STRING, dateTime);
        FUNCTION_TEST_PARAM(KEY_VALUE, httpHeader);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);
    ASSERT(verb != NULL);
    ASSERT(uri != NULL);
    // ASSERT(query != NULL);
    ASSERT(dateTime != NULL);
    ASSERT(httpHeader != NULL);
    ASSERT(httpHeaderGet(httpHeader, HTTP_HEADER_CONTENT_LENGTH_STR) != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Host header is required for authentication
        httpHeaderPut(httpHeader, HTTP_HEADER_HOST_STR, this->endpoint);

        // Service key authentication
        if (this->keyType == storageGcsKeyTypeService)
        {
            (void)verb; // !!! REMOVE WHEN USED
            (void)uri; // !!! REMOVE WHEN USED
            (void)query; // !!! REMOVE WHEN USED
            (void)dateTime; // !!! REMOVE WHEN USED
            (void)storageGcsAuthToken; // !!! REMOVE WHEN USED
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_TEST_RETURN_VOID();
}

/***********************************************************************************************************************************
Process Gcs request
***********************************************************************************************************************************/
HttpRequest *
storageGcsRequestAsync(StorageGcs *this, const String *verb, StorageGcsRequestAsyncParam param)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, verb);
        FUNCTION_LOG_PARAM(BOOL, param.noBucket);
        FUNCTION_LOG_PARAM(BOOL, param.upload);
        FUNCTION_LOG_PARAM(STRING, param.object);
        FUNCTION_LOG_PARAM(HTTP_HEADER, param.header);
        FUNCTION_LOG_PARAM(HTTP_QUERY, param.query);
        FUNCTION_LOG_PARAM(BUFFER, param.content);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(verb != NULL);
    ASSERT(!param.noBucket || param.object == NULL);

    HttpRequest *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Generate URI
        String *uri = strNewFmt("%s/storage/v1/b", param.upload ? "/upload" : "");

        if (!param.noBucket)
            strCatFmt(uri, "/%s/o", strZ(this->bucket));

        if (param.object != NULL)
            strCat(uri, param.object);

        // Create header list and add content length
        HttpHeader *requestHeader = param.header == NULL ?
            httpHeaderNew(this->headerRedactList) : httpHeaderDup(param.header, this->headerRedactList);

        // Set content length
        httpHeaderAdd(
            requestHeader, HTTP_HEADER_CONTENT_LENGTH_STR,
            param.content == NULL || bufEmpty(param.content) ? ZERO_STR : strNewFmt("%zu", bufUsed(param.content)));

        // Calculate content-md5 header if there is content
        // if (param.content != NULL)
        // {
        //     char md5Hash[HASH_TYPE_MD5_SIZE_HEX];
        //     encodeToStr(encodeBase64, bufPtr(cryptoHashOne(HASH_TYPE_MD5_STR, param.content)), HASH_TYPE_M5_SIZE, md5Hash);
        //     httpHeaderAdd(requestHeader, HTTP_HEADER_CONTENT_MD5_STR, STR(md5Hash));
        // }

        // Make a copy of the query so it can be modified
        // HttpQuery *query =
        //     this->sasKey != NULL && param.query == NULL ?
        //         httpQueryNewP(.redactList = this->queryRedactList) :
        //         httpQueryDupP(param.query, .redactList = this->queryRedactList);

        // Generate authorization header
        storageGcsAuth(this, verb, httpUriEncode(uri, true), param.query, httpDateFromTime(time(NULL)), requestHeader);

        // Send request
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = httpRequestNewP(
                this->httpClient, verb, uri, .query = param.query, .header = requestHeader, .content = param.content);
        }
        MEM_CONTEXT_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(HTTP_REQUEST, result);
}

HttpResponse *
storageGcsResponse(HttpRequest *request, StorageGcsResponseParam param)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(HTTP_REQUEST, request);
        FUNCTION_LOG_PARAM(BOOL, param.allowMissing);
        FUNCTION_LOG_PARAM(BOOL, param.contentIo);
    FUNCTION_LOG_END();

    ASSERT(request != NULL);

    HttpResponse *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Get response
        result = httpRequestResponse(request, !param.contentIo);

        // Error if the request was not successful
        if (!httpResponseCodeOk(result) && (!param.allowMissing || httpResponseCode(result) != HTTP_RESPONSE_CODE_NOT_FOUND))
            httpRequestError(request, result);

        // Move response to the prior context
        httpResponseMove(result, memContextPrior());
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(HTTP_RESPONSE, result);
}

HttpResponse *
storageGcsRequest(StorageGcs *this, const String *verb, StorageGcsRequestParam param)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, verb);
        FUNCTION_LOG_PARAM(BOOL, param.noBucket);
        FUNCTION_LOG_PARAM(BOOL, param.upload);
        FUNCTION_LOG_PARAM(STRING, param.object);
        FUNCTION_LOG_PARAM(HTTP_HEADER, param.header);
        FUNCTION_LOG_PARAM(HTTP_QUERY, param.query);
        FUNCTION_LOG_PARAM(BUFFER, param.content);
        FUNCTION_LOG_PARAM(BOOL, param.allowMissing);
        FUNCTION_LOG_PARAM(BOOL, param.contentIo);
    FUNCTION_LOG_END();

    FUNCTION_LOG_RETURN(
        HTTP_RESPONSE,
        storageGcsResponseP(
            storageGcsRequestAsyncP(
                this, verb, .noBucket = param.noBucket, .upload = param.upload, .object = param.object, .header = param.header,
                .query = param.query, .content = param.content),
            .allowMissing = param.allowMissing, .contentIo = param.contentIo));
}

/***********************************************************************************************************************************
General function for listing files to be used by other list routines
***********************************************************************************************************************************/
// Helper to convert YYYY-MM-DDTHH:MM:SS.MSECZ format to time_t. This format is very nearly ISO-8601 except for the inclusion of
// milliseconds, which are discarded here.
static time_t
storageGcsCvtTime(const String *time)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, time);
    FUNCTION_TEST_END();

    FUNCTION_TEST_RETURN(
        epochFromParts(
            cvtZToInt(strZ(strSubN(time, 0, 4))), cvtZToInt(strZ(strSubN(time, 5, 2))),
            cvtZToInt(strZ(strSubN(time, 8, 2))), cvtZToInt(strZ(strSubN(time, 11, 2))),
            cvtZToInt(strZ(strSubN(time, 14, 2))), cvtZToInt(strZ(strSubN(time, 17, 2))), 0));
}

static void
storageGcsInfoFile(StorageInfo *info, const KeyValue *file)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_INFO, info);
        FUNCTION_TEST_PARAM(KEY_VALUE, file);
    FUNCTION_TEST_END();

    info->size = cvtZToUInt64(strZ(varStr(kvGet(file, VARSTRDEF("size")))));
    info->timeModified = storageGcsCvtTime(varStr(kvGet(file, VARSTRDEF("updated"))));

    FUNCTION_TEST_RETURN_VOID();
}

static void
storageGcsListInternal(
    StorageGcs *this, const String *path, StorageInfoLevel level, const String *expression, bool recurse,
    StorageInfoListCallback callback, void *callbackData)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(ENUM, level);
        FUNCTION_LOG_PARAM(STRING, expression);
        FUNCTION_LOG_PARAM(BOOL, recurse);
        FUNCTION_LOG_PARAM(FUNCTIONP, callback);
        FUNCTION_LOG_PARAM_P(VOID, callbackData);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(path != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Build the base prefix by stripping off the initial /
        const String *basePrefix;

        if (strSize(path) == 1)
            basePrefix = EMPTY_STR;
        else
            basePrefix = strNewFmt("%s/", strZ(strSub(path, 1)));

        // Get the expression prefix when possible to limit initial results
        const String *expressionPrefix = regExpPrefix(expression);

        // If there is an expression prefix then use it to build the query prefix, otherwise query prefix is base prefix
        const String *queryPrefix;

        if (expressionPrefix == NULL)
            queryPrefix = basePrefix;
        else
        {
            if (strEmpty(basePrefix))
                queryPrefix = expressionPrefix;
            else
                queryPrefix = strNewFmt("%s%s", strZ(basePrefix), strZ(expressionPrefix));
        }

        // Create query
        HttpQuery *query = httpQueryNewP();

        // Add the delimiter to not recurse
        if (!recurse)
            httpQueryAdd(query, GCS_QUERY_DELIMITER_STR, FSLASH_STR);

        // Don't specify empty prefix because it is the default
        if (!strEmpty(queryPrefix))
            httpQueryAdd(query, GCS_QUERY_PREFIX_STR, queryPrefix);

        // Loop as long as a continuation marker returned
        HttpRequest *request = NULL;

        do
        {
            // Use an inner mem context here because we could potentially be retrieving millions of files so it is a good idea to
            // free memory at regular intervals
            MEM_CONTEXT_TEMP_BEGIN()
            {
                HttpResponse *response = NULL;

                // If there is an outstanding async request then wait for the response
                if (request != NULL)
                {
                    response = storageGcsResponseP(request);

                    httpRequestFree(request);
                    request = NULL;
                }
                // Else get the response immediately from a sync request
                else
                    response = storageGcsRequestP(this, HTTP_VERB_GET_STR, .query = query);

                KeyValue *content = jsonToKv(strNewBuf(httpResponseContent(response)));

                // If next page token exists then send an async request to get more data
                const String *nextPageToken = varStr(kvGet(content, VARSTRDEF("nextPageToken")));

                if (nextPageToken != NULL)
                {
                    httpQueryPut(query, STRDEF("pageToken"), nextPageToken);

                    // Store request in the outer temp context
                    MEM_CONTEXT_PRIOR_BEGIN()
                    {
                        request = storageGcsRequestAsyncP(this, HTTP_VERB_GET_STR, .query = query);
                    }
                    MEM_CONTEXT_PRIOR_END();
                }

                // Get prefix list
                const VariantList *prefixList = varVarLst(kvGet(content, VARSTRDEF("prefixes")));

                if (prefixList != NULL)
                {
                    for (unsigned int prefixIdx = 0; prefixIdx < varLstSize(prefixList); prefixIdx++)
                    {
                        // Get path name
                        StorageInfo info =
                        {
                            .level = level,
                            .name = varStr(varLstGet(prefixList, prefixIdx)),
                            .exists = true,
                        };

                        // Strip off base prefix and final /
                        info.name = strSubN(info.name, strSize(basePrefix), strSize(info.name) - strSize(basePrefix) - 1);

                        // Add basic level info if requested
                        if (level >= storageInfoLevelBasic)
                            info.type = storageTypePath;

                        // Callback with info
                        callback(callbackData, &info);
                    }
                }

                // Get file list
                const VariantList *fileList = varVarLst(kvGet(content, VARSTRDEF("items")));
                CHECK(fileList != NULL);

                for (unsigned int fileIdx = 0; fileIdx < varLstSize(fileList); fileIdx++)
                {
                    // THROW_FMT(AssertError, "!!!NOT YET IMPLEMENTED:\n%s", strZ(strNewBuf(httpResponseContent(response))));

                    const KeyValue *file = varKv(varLstGet(fileList, fileIdx));
                    CHECK(file != NULL);

                    // Get file name
                    StorageInfo info =
                    {
                        .level = level,
                        .name = varStr(kvGet(file, VARSTRDEF("name"))),
                        .exists = true,
                    };

                    CHECK(info.name != NULL);

                    // Strip off the base prefix when present
                    if (!strEmpty(basePrefix))
                        info.name = strSub(info.name, strSize(basePrefix));

                    // Add basic level info if requested
                    if (level >= storageInfoLevelBasic)
                    {
                        info.type = storageTypeFile;
                        storageGcsInfoFile(&info, file);
                    }

                    // Callback with info
                    callback(callbackData, &info);
                }
            }
            MEM_CONTEXT_TEMP_END();
        }
        while (request != NULL);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static StorageInfo
storageGcsInfo(THIS_VOID, const String *file, StorageInfoLevel level, StorageInterfaceInfoParam param)
{
    THIS(StorageGcs);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, file);
        FUNCTION_LOG_PARAM(ENUM, level);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);

    // // Attempt to get file info
    HttpResponse *httpResponse = storageGcsRequestP(this, HTTP_VERB_GET_STR, .object = file, .allowMissing = true);

    // Does the file exist?
    StorageInfo result = {.level = level, .exists = httpResponseCodeOk(httpResponse)};

    // Add basic level info if requested and the file exists
    if (result.level >= storageInfoLevelBasic && result.exists)
    {
        // THROW_FMT(AssertError, "!!!NOT YET IMPLEMENTED!!!: %s", strZ(strNewBuf(httpResponseContent(httpResponse))));
        result.type = storageTypeFile;
        storageGcsInfoFile(&result, jsonToKv(strNewBuf(httpResponseContent(httpResponse))));
    }

    FUNCTION_LOG_RETURN(STORAGE_INFO, result);
}

/**********************************************************************************************************************************/
static bool
storageGcsInfoList(
    THIS_VOID, const String *path, StorageInfoLevel level, StorageInfoListCallback callback, void *callbackData,
    StorageInterfaceInfoListParam param)
{
    THIS(StorageGcs);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(ENUM, level);
        FUNCTION_LOG_PARAM(FUNCTIONP, callback);
        FUNCTION_LOG_PARAM_P(VOID, callbackData);
        FUNCTION_LOG_PARAM(STRING, param.expression);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(path != NULL);
    ASSERT(callback != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        storageGcsListInternal(this, path, level, param.expression, false, callback, callbackData);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(BOOL, true);
}

/**********************************************************************************************************************************/
static StorageRead *
storageGcsNewRead(THIS_VOID, const String *file, bool ignoreMissing, StorageInterfaceNewReadParam param)
{
    THIS(StorageGcs);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, file);
        FUNCTION_LOG_PARAM(BOOL, ignoreMissing);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);

    FUNCTION_LOG_RETURN(STORAGE_READ, storageReadGcsNew(this, file, ignoreMissing));
}

/**********************************************************************************************************************************/
static StorageWrite *
storageGcsNewWrite(THIS_VOID, const String *file, StorageInterfaceNewWriteParam param)
{
    THIS(StorageGcs);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, file);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);
    ASSERT(param.createPath);
    ASSERT(param.user == NULL);
    ASSERT(param.group == NULL);
    ASSERT(param.timeModified == 0);

    FUNCTION_LOG_RETURN(STORAGE_WRITE, storageWriteGcsNew(this, file, this->blockSize));
}

/**********************************************************************************************************************************/
typedef struct StorageGcsPathRemoveData
{
    StorageGcs *this;                                               // Storage Object
    MemContext *memContext;                                         // Mem context to create requests in
    HttpRequest *request;                                           // Async remove request
    const String *path;                                             // Root path of remove
} StorageGcsPathRemoveData;

static void
storageGcsPathRemoveCallback(void *callbackData, const StorageInfo *info)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(VOID, callbackData);
        FUNCTION_TEST_PARAM(STORAGE_INFO, info);
    FUNCTION_TEST_END();

    ASSERT(callbackData != NULL);
    ASSERT(info != NULL);

    StorageGcsPathRemoveData *data = callbackData;

    // Get response from prior async request
    if (data->request != NULL)
    {
        storageGcsResponseP(data->request, .allowMissing = true);

        httpRequestFree(data->request);
        data->request = NULL;
    }

    // Only delete files since paths don't really exist
    if (info->type == storageTypeFile)
    {
        MEM_CONTEXT_BEGIN(data->memContext)
        {
            data->request = storageGcsRequestAsyncP(
                data->this, HTTP_VERB_DELETE_STR,
                .object = strNewFmt("%s/%s", strEq(data->path, FSLASH_STR) ? "" : strZ(data->path), strZ(info->name)));
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN_VOID();
}

static bool
storageGcsPathRemove(THIS_VOID, const String *path, bool recurse, StorageInterfacePathRemoveParam param)
{
    THIS(StorageGcs);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(BOOL, recurse);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(path != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        StorageGcsPathRemoveData data = {.this = this, .memContext = memContextCurrent(), .path = path};
        storageGcsListInternal(this, path, storageInfoLevelBasic, NULL, true, storageGcsPathRemoveCallback, &data);

        // Check response on last async request
        if (data.request != NULL)
            storageGcsResponseP(data.request, .allowMissing = true);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(BOOL, true);
}

/**********************************************************************************************************************************/
static void
storageGcsRemove(THIS_VOID, const String *file, StorageInterfaceRemoveParam param)
{
    THIS(StorageGcs);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_GCS, this);
        FUNCTION_LOG_PARAM(STRING, file);
        FUNCTION_LOG_PARAM(BOOL, param.errorOnMissing);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);
    ASSERT(!param.errorOnMissing);

    storageGcsRequestP(this, HTTP_VERB_DELETE_STR, .object = file, .allowMissing = true);

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static const StorageInterface storageInterfaceGcs =
{
    .info = storageGcsInfo,
    .infoList = storageGcsInfoList,
    .newRead = storageGcsNewRead,
    .newWrite = storageGcsNewWrite,
    .pathRemove = storageGcsPathRemove,
    .remove = storageGcsRemove,
};

Storage *
storageGcsNew(
    const String *path, bool write, StoragePathExpressionCallback pathExpressionFunction, const String *bucket,
    StorageGcsKeyType keyType, const String *key, size_t blockSize,  const String *endpoint, unsigned int port, TimeMSec timeout,
    bool verifyPeer, const String *caFile, const String *caPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(BOOL, write);
        FUNCTION_LOG_PARAM(FUNCTIONP, pathExpressionFunction);
        FUNCTION_LOG_PARAM(STRING, bucket);
        FUNCTION_LOG_PARAM(ENUM, keyType);
        FUNCTION_TEST_PARAM(STRING, key);
        FUNCTION_LOG_PARAM(SIZE, blockSize);
        FUNCTION_LOG_PARAM(STRING, endpoint);
        FUNCTION_LOG_PARAM(UINT, port);
        FUNCTION_LOG_PARAM(TIME_MSEC, timeout);
        FUNCTION_LOG_PARAM(BOOL, verifyPeer);
        FUNCTION_LOG_PARAM(STRING, caFile);
        FUNCTION_LOG_PARAM(STRING, caPath);
    FUNCTION_LOG_END();

    ASSERT(path != NULL);
    ASSERT(bucket != NULL);
    ASSERT(keyType == storageGcsKeyTypeNone || key != NULL);
    ASSERT(blockSize != 0);

    Storage *this = NULL;

    MEM_CONTEXT_NEW_BEGIN("StorageGcs")
    {
        StorageGcs *driver = memNew(sizeof(StorageGcs));

        *driver = (StorageGcs)
        {
            .memContext = MEM_CONTEXT_NEW(),
            .interface = storageInterfaceGcs,
            .write = write,
            .bucket = strDup(bucket),
            .keyType = keyType,
            .credential = strNew("service@pgbackrest-dev.iam.gserviceaccount.com"),
            .blockSize = blockSize,
            .endpoint = strDup(endpoint),
            // .uriPrefix = host == NULL ? strNewFmt("/%s", strZ(container)) : strNewFmt("/%s/%s", strZ(account), strZ(container)),
        };

        if (key != NULL)
        {
            KeyValue *kvKey = jsonToKv(strNewBuf(storageGetP(storageNewReadP(storagePosixNewP(FSLASH_STR), key))));
            driver->credential = varStr(kvGet(kvKey, VARSTRDEF("client_email")));
            driver->privateKey = varStr(kvGet(kvKey, VARSTRDEF("private_key")));
        }

        // Store shared key or parse sas query
        // if (keyType == storageGcsKeyTypeShared)
        //     driver->sharedKey = key;
        // else
        //     driver->sasKey = httpQueryNewStr(key);

        // Create the http client used to service requests
        driver->httpClient = httpClientNew(
            tlsClientNew(
                sckClientNew(driver->endpoint, port, timeout), driver->endpoint, timeout, verifyPeer, caFile, caPath), timeout);

        // Create list of redacted headers
        driver->headerRedactList = strLstNew();
        strLstAdd(driver->headerRedactList, HTTP_HEADER_AUTHORIZATION_STR);
        // strLstAdd(driver->headerRedactList, HTTP_HEADER_DATE_STR);

        // Create list of redacted query keys
        // driver->queryRedactList = strLstNew();
        // strLstAdd(driver->queryRedactList, GCS_QUERY_SIG_STR);

        // Generate starting file id
        // cryptoRandomBytes((unsigned char *)&driver->fileId, sizeof(driver->fileId));

        this = storageNew(STORAGE_GCS_TYPE_STR, path, 0, 0, write, pathExpressionFunction, driver, driver->interface);
    }
    MEM_CONTEXT_NEW_END();

    FUNCTION_LOG_RETURN(STORAGE, this);
}