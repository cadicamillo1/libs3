/** **************************************************************************
 * request.c
 * 
 * Copyright 2008 Bryan Ischo <bryan@ischo.com>
 * 
 * This file is part of libs3.
 * 
 * libs3 is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, version 3 of the License.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of this library and its programs with the
 * OpenSSL library, and distribute linked combinations including the two.
 *
 * libs3 is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * version 3 along with libs3, in a file named COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 ************************************************************************** **/

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include "request.h"
#include "request_context.h"
#include "response_headers_handler.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonHMAC.h>
#define S3_SHA256_DIGEST_LENGTH CC_SHA256_DIGEST_LENGTH
#else
#include <openssl/hmac.h>
#include <openssl/sha.h>
#define S3_SHA256_DIGEST_LENGTH SHA256_DIGEST_LENGTH
#endif

#define USER_AGENT_SIZE 256
#define REQUEST_STACK_SIZE 32
#define SIGNATURE_SCOPE_SIZE 64

//#define SIGNATURE_DEBUG

static int verifyPeer;

static char userAgentG[USER_AGENT_SIZE];

static pthread_mutex_t requestStackMutexG;

static Request *requestStackG[REQUEST_STACK_SIZE];

static int requestStackCountG;

char defaultHostNameG[S3_MAX_HOSTNAME_SIZE];


typedef struct RequestComputedValues
{
    // All x-amz- headers, in normalized form (i.e. NAME: VALUE, no other ws)
    char *amzHeaders[S3_MAX_METADATA_COUNT + 2]; // + 2 for acl and date

    // The number of x-amz- headers
    int amzHeadersCount;

    // Storage for amzHeaders (the +256 is for x-amz-acl and x-amz-date)
    char amzHeadersRaw[COMPACTED_METADATA_BUFFER_SIZE + 256 + 1];

    // Length of populated data in raw buffer
    int amzHeadersRawLength;

    // Canonicalized headers for signature
    string_multibuffer(canonicalizedSignatureHeaders,
                       COMPACTED_METADATA_BUFFER_SIZE + 256 + 1);

    // Delimited list of header names used for signature
    char signedHeaders[COMPACTED_METADATA_BUFFER_SIZE];

    // URL-Encoded key
    char urlEncodedKey[MAX_URLENCODED_KEY_SIZE + 1];

    // Canonicalized resource
    char canonicalURI[MAX_CANONICALIZED_RESOURCE_SIZE + 1];

    // Canonical sub-resource & query string
    char canonicalQueryString[MAX_CANONICALIZED_RESOURCE_SIZE + 1];

    // Cache-Control header (or empty)
    char cacheControlHeader[128];

    // Content-Type header (or empty)
    char contentTypeHeader[128];

    // Content-MD5 header (or empty)
    char md5Header[128];

    // Content-Disposition header (or empty)
    char contentDispositionHeader[128];

    // Content-Encoding header (or empty)
    char contentEncodingHeader[128];

    // Expires header (or empty)
    char expiresHeader[128];

    // If-Modified-Since header
    char ifModifiedSinceHeader[128];

    // If-Unmodified-Since header
    char ifUnmodifiedSinceHeader[128];

    // If-Match header
    char ifMatchHeader[128];

    // If-None-Match header
    char ifNoneMatchHeader[128];

    // Range header
    char rangeHeader[128];

    // Authorization header
    char authorizationHeader[256];

    // Host header
    char hostHeader[128];

    // Hex string of hash of request payload
    char payloadHash[S3_SHA256_DIGEST_LENGTH * 2 + 1];
} RequestComputedValues;


// Called whenever we detect that the request headers have been completely
// processed; which happens either when we get our first read/write callback,
// or the request is finished being processed.  Returns nonzero on success,
// zero on failure.
static void request_headers_done(Request *request)
{
    if (request->propertiesCallbackMade) {
        return;
    }

    request->propertiesCallbackMade = 1;

    // Get the http response code
    long httpResponseCode;
    request->httpResponseCode = 0;
    if (curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, 
                          &httpResponseCode) != CURLE_OK) {
        // Not able to get the HTTP response code - error
        request->status = S3StatusInternalError;
        return;
    }
    else {
        request->httpResponseCode = httpResponseCode;
    }

    response_headers_handler_done(&(request->responseHeadersHandler), 
                                  request->curl);

    // Only make the callback if it was a successful request; otherwise we're
    // returning information about the error response itself
    if (request->propertiesCallback &&
        (request->httpResponseCode >= 200) &&
        (request->httpResponseCode <= 299)) {
        request->status = (*(request->propertiesCallback))
            (&(request->responseHeadersHandler.responseProperties), 
             request->callbackData);
    }
}


static size_t curl_header_func(void *ptr, size_t size, size_t nmemb,
                               void *data)
{
    Request *request = (Request *) data;

    int len = size * nmemb;

    response_headers_handler_add
        (&(request->responseHeadersHandler), (char *) ptr, len);

    return len;
}


static size_t curl_read_func(void *ptr, size_t size, size_t nmemb, void *data)
{
    Request *request = (Request *) data;

    int len = size * nmemb;

    // CURL may call this function before response headers are available,
    // so don't assume response headers are available and attempt to parse
    // them.  Leave that to curl_write_func, which is guaranteed to be called
    // only after headers are available.

    if (request->status != S3StatusOK) {
        return CURL_READFUNC_ABORT;
    }

    // If there is no data callback, or the data callback has already returned
    // contentLength bytes, return 0;
    if (!request->toS3Callback || !request->toS3CallbackBytesRemaining) {
        return 0;
    }

    // Don't tell the callback that we are willing to accept more data than we
    // really are
    if (len > request->toS3CallbackBytesRemaining) {
        len = request->toS3CallbackBytesRemaining;
    }

    // Otherwise, make the data callback
    int ret = (*(request->toS3Callback))
        (len, (char *) ptr, request->callbackData);
    if (ret < 0) {
        request->status = S3StatusAbortedByCallback;
        return CURL_READFUNC_ABORT;
    }
    else {
        if (ret > request->toS3CallbackBytesRemaining) {
            ret = request->toS3CallbackBytesRemaining;
        }
        request->toS3CallbackBytesRemaining -= ret;
        return ret;
    }
}


static size_t curl_write_func(void *ptr, size_t size, size_t nmemb,
                              void *data)
{
    Request *request = (Request *) data;

    int len = size * nmemb;

    request_headers_done(request);

    if (request->status != S3StatusOK) {
        return 0;
    }

    // On HTTP error, we expect to parse an HTTP error response
    if ((request->httpResponseCode < 200) || 
        (request->httpResponseCode > 299)) {
        request->status = error_parser_add
            (&(request->errorParser), (char *) ptr, len);
    }
    // If there was a callback registered, make it
    else if (request->fromS3Callback) {
        request->status = (*(request->fromS3Callback))
            (len, (char *) ptr, request->callbackData);
    }
    // Else, consider this an error - S3 has sent back data when it was not
    // expected
    else {
        request->status = S3StatusInternalError;
    }

    return ((request->status == S3StatusOK) ? len : 0);
}


static S3Status append_amz_header(RequestComputedValues *values,
                                  int addPrefix,
                                  const char *headerName,
                                  const char *headerValue)
{
    int rawPos = values->amzHeadersRawLength + 1;
    values->amzHeaders[values->amzHeadersCount++] = &(values->amzHeadersRaw[rawPos]);

    const char *headerStr = headerName;
    if (addPrefix) {
        char headerNameWithPrefix[S3_MAX_METADATA_SIZE - sizeof(": v")];
        snprintf(headerNameWithPrefix, sizeof(headerNameWithPrefix),
                 S3_METADATA_HEADER_NAME_PREFIX "%s", headerName);
        headerStr = headerNameWithPrefix;
    }

    // Make sure the new header (plus ": " plus string terminator) will fit
    // in the buffer.
    if ((values->amzHeadersRawLength + strlen(headerStr) + strlen(headerValue)
        + 3) >= sizeof(values->amzHeadersRaw)) {
        return S3StatusMetaDataHeadersTooLong;
    }

    unsigned long i = 0;
    for (; i < strlen(headerStr); i++) {
        values->amzHeadersRaw[rawPos++] = tolower(headerStr[i]);
    }

    strcat(&(values->amzHeadersRaw[rawPos]), ": ");
    rawPos += 2;

    for (i = 0; i < strlen(headerValue); i++) {
        values->amzHeadersRaw[rawPos++] = headerValue[i];
    }
    rawPos--;

    while (isblank(values->amzHeadersRaw[rawPos])) {
        rawPos--;
    }
    values->amzHeadersRaw[++rawPos] = '\0';
    values->amzHeadersRawLength = rawPos;
    return S3StatusOK;
}

// This function 'normalizes' all x-amz-meta headers provided in
// params->requestHeaders, which means it removes all whitespace from
// them such that they all look exactly like this:
// x-amz-meta-${NAME}: ${VALUE}
// It also adds the x-amz-acl, x-amz-copy-source, x-amz-metadata-directive,
// and x-amz-server-side-encryption headers if necessary, and always adds the
// x-amz-date header.  It copies the raw string values into
// params->amzHeadersRaw, and creates an array of string pointers representing
// these headers in params->amzHeaders (and also sets params->amzHeadersCount
// to be the count of the total number of x-amz- headers thus created).
static S3Status compose_amz_headers(const RequestParams *params,
                                    const char *dateStr,
                                    RequestComputedValues *values)
{
    const S3PutProperties *properties = params->putProperties;

    values->amzHeadersCount = 0;
    values->amzHeadersRaw[0] = '\0';
    values->amzHeadersRawLength = 0;

    // Check and copy in the x-amz-meta headers
    if (properties) {
        int i;
        for (i = 0; i < properties->metaDataCount; i++) {
            const S3NameValue *property = &(properties->metaData[i]);
            append_amz_header(values, 1, property->name, property->value);
        }

        // Add the x-amz-acl header, if necessary
        const char *cannedAclString;
        switch (properties->cannedAcl) {
        case S3CannedAclPrivate:
            cannedAclString = NULL;
            break;
        case S3CannedAclPublicRead:
            cannedAclString = "public-read";
            break;
        case S3CannedAclPublicReadWrite:
            cannedAclString = "public-read-write";
            break;
        default: // S3CannedAclAuthenticatedRead
            cannedAclString = "authenticated-read";
            break;
        }
        if (cannedAclString) {
            append_amz_header(values, 0, "x-amz-acl", cannedAclString);
        }

        // Add the x-amz-server-side-encryption header, if necessary
        if (properties->useServerSideEncryption) {
            append_amz_header(values, 0, "x-amz-server-side-encryption",
                              "AES256");
        }
    }

    // Add the x-amz-date header
    append_amz_header(values, 0, "x-amz-date", dateStr);

    if (params->httpRequestType == HttpRequestTypeCOPY) {
        // Add the x-amz-copy-source header
        if (params->copySourceBucketName && params->copySourceBucketName[0]
            && params->copySourceKey && params->copySourceKey[0]) {
            char bucketKey[S3_MAX_METADATA_SIZE];
            snprintf(bucketKey, sizeof(bucketKey), "/%s/%s",
                     params->copySourceBucketName, params->copySourceKey);
            append_amz_header(values, 0, "x-amz-copy-source", bucketKey);
        }
        // If byteCount != 0 then we're just copying a range, add header
        if (params->byteCount > 0) {
            char byteRange[S3_MAX_METADATA_SIZE];
            snprintf(byteRange, sizeof(byteRange), "bytes=%zd-%zd",
                     params->startByte, params->startByte + params->byteCount);
            append_amz_header(values, 0, "x-amz-copy-source-range", byteRange);
        }
        // And the x-amz-metadata-directive header
        if (properties) {
            append_amz_header(values, 0, "x-amz-metadata-directive", "REPLACE");
        }
    }

    // Add the x-amz-security-token header if necessary
    if (params->bucketContext.securityToken) {
        append_amz_header(values, 0, "x-amz-security-token",
                          params->bucketContext.securityToken);
    }

    if (params->httpRequestType == HttpRequestTypeGET
        || params->httpRequestType == HttpRequestTypeCOPY
        || params->httpRequestType == HttpRequestTypeDELETE
        || params->httpRequestType == HttpRequestTypeHEAD) {
        // empty payload
        unsigned char md[S3_SHA256_DIGEST_LENGTH];
#ifdef __APPLE__
        CC_SHA256("", 0, md);
#else
        SHA256((const unsigned char*) "", 0, md);
#endif
        values->payloadHash[0] = '\0';
        int i = 0;
        for (; i < S3_SHA256_DIGEST_LENGTH; i++) {
            snprintf(&(values->payloadHash[i * 2]), 3, "%02x", md[i]);
        }
    }
    else {
        // TODO: figure out how to manage signed payloads
        strcpy(values->payloadHash, "UNSIGNED-PAYLOAD");
    }

    append_amz_header(values, 0, "x-amz-content-sha256",
                      values->payloadHash);

    return S3StatusOK;
}


// Composes the other headers
static S3Status compose_standard_headers(const RequestParams *params,
                                         RequestComputedValues *values)
{

#define do_put_header(fmt, sourceField, destField, badError, tooLongError)  \
    do {                                                                    \
        if (params->putProperties &&                                        \
            params->putProperties-> sourceField &&                          \
            params->putProperties-> sourceField[0]) {                       \
            /* Skip whitespace at beginning of val */                       \
            const char *val = params->putProperties-> sourceField;          \
            while (*val && is_blank(*val)) {                                \
                val++;                                                      \
            }                                                               \
            if (!*val) {                                                    \
                return badError;                                            \
            }                                                               \
            /* Compose header, make sure it all fit */                      \
            int len = snprintf(values-> destField,                          \
                               sizeof(values-> destField), fmt, val);       \
            if (len >= (int) sizeof(values-> destField)) {                  \
                return tooLongError;                                        \
            }                                                               \
            /* Now remove the whitespace at the end */                      \
            while (is_blank(values-> destField[len])) {                     \
                len--;                                                      \
            }                                                               \
            values-> destField[len] = 0;                                    \
        }                                                                   \
        else {                                                              \
            values-> destField[0] = 0;                                      \
        }                                                                   \
    } while (0)

#define do_get_header(fmt, sourceField, destField, badError, tooLongError)  \
    do {                                                                    \
        if (params->getConditions &&                                        \
            params->getConditions-> sourceField &&                          \
            params->getConditions-> sourceField[0]) {                       \
            /* Skip whitespace at beginning of val */                       \
            const char *val = params->getConditions-> sourceField;          \
            while (*val && is_blank(*val)) {                                \
                val++;                                                      \
            }                                                               \
            if (!*val) {                                                    \
                return badError;                                            \
            }                                                               \
            /* Compose header, make sure it all fit */                      \
            int len = snprintf(values-> destField,                          \
                               sizeof(values-> destField), fmt, val);       \
            if (len >= (int) sizeof(values-> destField)) {                  \
                return tooLongError;                                        \
            }                                                               \
            /* Now remove the whitespace at the end */                      \
            while (is_blank(values-> destField[len])) {                     \
                len--;                                                      \
            }                                                               \
            values-> destField[len] = 0;                                    \
        }                                                                   \
        else {                                                              \
            values-> destField[0] = 0;                                      \
        }                                                                   \
    } while (0)

    // Host
    if (params->bucketContext.uriStyle == S3UriStyleVirtualHost) {
        const char *requestHostName = params->bucketContext.hostName
                ? params->bucketContext.hostName : defaultHostNameG;

        size_t len = snprintf(values->hostHeader, sizeof(values->hostHeader),
                              "Host: %s.%s", params->bucketContext.bucketName,
                              requestHostName);
        if (len >= sizeof(values->hostHeader)) {
            return S3StatusUriTooLong;
        }
        while (is_blank(values->hostHeader[len])) {
            len--;
        }
        values->hostHeader[len] = 0;
    }
    else if (params->bucketContext.hostName) {
        size_t len = snprintf(values->hostHeader, sizeof(values->hostHeader),
                              "Host: %s", params->bucketContext.hostName);
        if (len >= sizeof(values->hostHeader)) {
            return S3StatusUriTooLong;
        }
        while (is_blank(values->hostHeader[len])) {
            len--;
        }
        values->hostHeader[len] = 0;
    }
    else {
        size_t len = snprintf(values->hostHeader, sizeof(values->hostHeader),
                              "Host: %s", S3_DEFAULT_HOSTNAME);
        if (len >= sizeof(values->hostHeader)) {
            return S3StatusUriTooLong;
        }
        while (is_blank(values->hostHeader[len])) {
            len--;
        }
        values->hostHeader[len] = 0;
    }

    // Cache-Control
    do_put_header("Cache-Control: %s", cacheControl, cacheControlHeader,
                  S3StatusBadCacheControl, S3StatusCacheControlTooLong);

    // ContentType
    do_put_header("Content-Type: %s", contentType, contentTypeHeader,
                  S3StatusBadContentType, S3StatusContentTypeTooLong);

    // MD5
    do_put_header("Content-MD5: %s", md5, md5Header, S3StatusBadMD5,
                  S3StatusMD5TooLong);

    // Content-Disposition
    do_put_header("Content-Disposition: attachment; filename=\"%s\"",
                  contentDispositionFilename, contentDispositionHeader,
                  S3StatusBadContentDispositionFilename,
                  S3StatusContentDispositionFilenameTooLong);

    // ContentEncoding
    do_put_header("Content-Encoding: %s", contentEncoding, 
                  contentEncodingHeader, S3StatusBadContentEncoding,
                  S3StatusContentEncodingTooLong);

    // Expires
    if (params->putProperties && (params->putProperties->expires >= 0)) {
        time_t t = (time_t) params->putProperties->expires;
        struct tm gmt;
        strftime(values->expiresHeader, sizeof(values->expiresHeader),
                 "Expires: %a, %d %b %Y %H:%M:%S UTC", gmtime_r(&t, &gmt));
    }
    else {
        values->expiresHeader[0] = 0;
    }

    // If-Modified-Since
    if (params->getConditions &&
        (params->getConditions->ifModifiedSince >= 0)) {
        time_t t = (time_t) params->getConditions->ifModifiedSince;
        struct tm gmt;
        strftime(values->ifModifiedSinceHeader,
                 sizeof(values->ifModifiedSinceHeader),
                 "If-Modified-Since: %a, %d %b %Y %H:%M:%S UTC", gmtime_r(&t, &gmt));
    }
    else {
        values->ifModifiedSinceHeader[0] = 0;
    }

    // If-Unmodified-Since header
    if (params->getConditions &&
        (params->getConditions->ifNotModifiedSince >= 0)) {
        time_t t = (time_t) params->getConditions->ifNotModifiedSince;
        struct tm gmt;
        strftime(values->ifUnmodifiedSinceHeader,
                 sizeof(values->ifUnmodifiedSinceHeader),
                 "If-Unmodified-Since: %a, %d %b %Y %H:%M:%S UTC", gmtime_r(&t, &gmt));
    }
    else {
        values->ifUnmodifiedSinceHeader[0] = 0;
    }

    // If-Match header
    do_get_header("If-Match: %s", ifMatchETag, ifMatchHeader,
                  S3StatusBadIfMatchETag, S3StatusIfMatchETagTooLong);

    // If-None-Match header
    do_get_header("If-None-Match: %s", ifNotMatchETag, ifNoneMatchHeader,
                  S3StatusBadIfNotMatchETag, 
                  S3StatusIfNotMatchETagTooLong);

    // Range header
    if (params->startByte || params->byteCount) {
        if (params->byteCount) {
            snprintf(values->rangeHeader, sizeof(values->rangeHeader),
                     "Range: bytes=%llu-%llu", 
                     (unsigned long long) params->startByte,
                     (unsigned long long) (params->startByte + 
                                           params->byteCount - 1));
        }
        else {
            snprintf(values->rangeHeader, sizeof(values->rangeHeader),
                     "Range: bytes=%llu-", 
                     (unsigned long long) params->startByte);
        }
    }
    else {
        values->rangeHeader[0] = 0;
    }

    return S3StatusOK;
}


// URL encodes the params->key value into params->urlEncodedKey
static S3Status encode_key(const RequestParams *params,
                           RequestComputedValues *values)
{
    return (urlEncode(values->urlEncodedKey, params->key, S3_MAX_KEY_SIZE) ?
            S3StatusOK : S3StatusUriTooLong);
}


// Simple comparison function for comparing two HTTP header names that are
// embedded within an HTTP header line, returning true if header1 comes
// before header2 alphabetically, false if not
static int headerle(const char *header1, const char *header2)
{
    while (1) {
        if (*header1 == ':') {
            return (*header2 != ':');
        }
        else if (*header2 == ':') {
            return 0;
        }
        else if (*header2 < *header1) {
            return 0;
        }
        else if (*header2 > *header1) {
            return 1;
        }
        header1++, header2++;
    }
}


// Replace this with merge sort eventually, it's the best stable sort.  But
// since typically the number of elements being sorted is small, it doesn't
// matter that much which sort is used, and gnome sort is the world's simplest
// stable sort.  Added a slight twist to the standard gnome_sort - don't go
// forward +1, go forward to the last highest index considered.  This saves
// all the string comparisons that would be done "going forward", and thus
// only does the necessary string comparisons to move values back into their
// sorted position.
static void header_gnome_sort(const char **headers, int size)
{
    int i = 0, last_highest = 0;

    while (i < size) {
        if ((i == 0) || headerle(headers[i - 1], headers[i])) {
            i = ++last_highest;
        }
        else {
            const char *tmp = headers[i];
            headers[i] = headers[i - 1];
            headers[--i] = tmp;
        }
    }
}


// Canonicalizes the signature headers into the canonicalizedSignatureHeaders buffer
static void canonicalize_signature_headers(RequestComputedValues *values)
{
    // Make a copy of the headers that will be sorted
    const char *sortedHeaders[S3_MAX_METADATA_COUNT + 3];

    memcpy(sortedHeaders, values->amzHeaders,
           (values->amzHeadersCount * sizeof(sortedHeaders[0])));

    // add the content-type header and host header
    int headerCount = values->amzHeadersCount;
    if (values->contentTypeHeader[0]) {
        sortedHeaders[headerCount++] = values->contentTypeHeader;
    }
    if (values->hostHeader[0]) {
        sortedHeaders[headerCount++] = values->hostHeader;
    }
    if (values->rangeHeader[0]) {
        sortedHeaders[headerCount++] = values->rangeHeader;
    }

    // Now sort these
    header_gnome_sort(sortedHeaders, headerCount);

    // Now copy this sorted list into the buffer, all the while:
    // - folding repeated headers into single lines, and
    // - folding multiple lines
    // - removing the space after the colon
    int lastHeaderLen = 0;
    char *buffer = values->canonicalizedSignatureHeaders;
    char *hbuf = values->signedHeaders;
    int i = 0;
    for (; i < headerCount; i++) {
        const char *header = sortedHeaders[i];
        const char *c = header;
        char v;
        // If the header names are the same, append the next value
        if ((i > 0) && 
            !strncmp(header, sortedHeaders[i - 1], lastHeaderLen)) {
            // Replacing the previous newline with a comma
            *(buffer - 1) = ',';
            // Skip the header name and space
            c += (lastHeaderLen + 1);
        }
        // Else this is a new header
        else {
            // Copy in everything up to the space in the ": "
            while (*c != ' ') {
                v = tolower(*c++);
                *buffer++ = v;
                *hbuf++ = v;
            }
            // replace the ":" with a ";"
            *(hbuf - 1) = ';';
            // Save the header len since it's a new header
            lastHeaderLen = c - header;
            // Skip the space
            c++;
        }
        // Now copy in the value, folding the lines
        while (*c) {
            // If c points to a \r\n[whitespace] sequence, then fold
            // this newline out
            if ((*c == '\r') && (*(c + 1) == '\n') && is_blank(*(c + 2))) {
                c += 3;
                while (is_blank(*c)) {
                    c++;
                }
                // Also, what has most recently been copied into buffer may
                // have been whitespace, and since we're folding whitespace
                // out around this newline sequence, back buffer up over
                // any whitespace it contains
                while (is_blank(*(buffer - 1))) {
                    buffer--;
                }
                continue;
            }
            *buffer++ = *c++;
        }
        // Finally, add the newline
        *buffer++ = '\n';
    }
    // Remove the extra trailing semicolon from the header name list
    // and terminate the string.
    *(hbuf - 1) = '\0';

    // Terminate the buffer
    *buffer = 0;
}


// Canonicalizes the resource into params->canonicalizedResource
static void canonicalize_resource(const S3BucketContext *context,
                                  const char *urlEncodedKey,
                                  char *buffer)
{
    int len = 0;

    *buffer = 0;

#define append(str) len += sprintf(&(buffer[len]), "%s", str)

    if (context->uriStyle == S3UriStylePath) {
        if (context->bucketName && context->bucketName[0]) {
            buffer[len++] = '/';
            append(context->bucketName);
        }
    }

    append("/");

    if (urlEncodedKey && urlEncodedKey[0]) {
        append(urlEncodedKey);
    }

#undef append
}

// Canonicalize the query string part of the request into a buffer
static void canonicalize_query_string(const RequestParams *params, char *buffer)
{
    int len = 0;

    *buffer = 0;

#define append(str) len += sprintf(&(buffer[len]), "%s", str)

    if (params->queryParams && params->queryParams[0]) {
        char appendage[4];
        int foundEquals = 0;
        unsigned long i = 0;
        for (; i < strlen(params->queryParams); i++) {
            char c = params->queryParams[i];
            if (isalnum(c) || (c == '_') || (c == '-') || (c == '~')
                || (c == '.')) {
                appendage[0] = c;
                appendage[1] = '\0';
            }
            else if ((c == '=') && !foundEquals) {
                appendage[0] = c;
                appendage[1] = '\0';
                foundEquals = 1;
            }
            else {
                snprintf(appendage, 4, "%%%02X", c);
            }
            append(appendage);
        }
    }

    if (params->subResource && params->subResource[0]) {
        if (params->queryParams && params->queryParams[0]) {
            append("?");
        }
    }

#undef append
}

// Convert an HttpRequestType to an HTTP Verb string
static const char *http_request_type_to_verb(HttpRequestType requestType)
{
    switch (requestType) {
    case HttpRequestTypePOST:
        return "POST";
    case HttpRequestTypeGET:
        return "GET";
    case HttpRequestTypeHEAD:
        return "HEAD";
    case HttpRequestTypePUT:
    case HttpRequestTypeCOPY:
        return "PUT";
    default: // HttpRequestTypeDELETE
        return "DELETE";
    }
}


// Composes the Authorization header for the request
static S3Status compose_auth_header(const RequestParams *params,
                                    const char *dateStr,
                                    RequestComputedValues *values)
{
    const char *httpMethod = http_request_type_to_verb(params->httpRequestType);
    int canonicalRequestLen = strlen(httpMethod) + 1 +
    strlen(values->canonicalURI) + 1 +
    strlen(values->canonicalQueryString) + 1 +
    strlen(values->canonicalizedSignatureHeaders) + 1 +
    strlen(values->signedHeaders) + 1 +
    2 * S3_SHA256_DIGEST_LENGTH + 1; // 2 hex digits for each byte

    int len = 0;

    char canonicalRequest[canonicalRequestLen];

#define buf_append(buf, format, ...)                    \
    len += snprintf(&(buf[len]), sizeof(buf) - len,     \
                    format, __VA_ARGS__)

    canonicalRequest[0] = '\0';
    buf_append(canonicalRequest, "%s\n", httpMethod);
    buf_append(canonicalRequest, "%s\n", values->canonicalURI);
    buf_append(canonicalRequest, "%s\n", values->canonicalQueryString);
    buf_append(canonicalRequest, "%s\n", values->canonicalizedSignatureHeaders);
    buf_append(canonicalRequest, "%s\n", values->signedHeaders);

    buf_append(canonicalRequest, "%s", values->payloadHash);

#ifdef SIGNATURE_DEBUG
    printf("--\nCanonical Request:\n%s\n", canonicalRequest);
#endif

    len = 0;
    unsigned char canonicalRequestHash[S3_SHA256_DIGEST_LENGTH];
#ifdef __APPLE__
    CC_SHA256(canonicalRequest, strlen(canonicalRequest), canonicalRequestHash);
#else
    const unsigned char *rqstData = (const unsigned char*) canonicalRequest;
    SHA256(rqstData, strlen(canonicalRequest), canonicalRequestHash);
#endif
    char canonicalRequestHashHex[2 * S3_SHA256_DIGEST_LENGTH + 1];
    canonicalRequestHashHex[0] = '\0';
    int i = 0;
    for (; i < S3_SHA256_DIGEST_LENGTH; i++) {
        buf_append(canonicalRequestHashHex, "%02x", canonicalRequestHash[i]);
    }

    // FIXME: determine correct region
    const char *awsRegion = "us-east-1";
    char scope[SIGNATURE_SCOPE_SIZE + 1];
    snprintf(scope, sizeof(scope), "%.8s/%s/s3/aws4_request", dateStr,
             awsRegion);

    char stringToSign[17 + 17 + SIGNATURE_SCOPE_SIZE + 1
        + strlen(canonicalRequestHashHex)];
    snprintf(stringToSign, sizeof(stringToSign), "AWS4-HMAC-SHA256\n%s\n%s\n%s",
             dateStr, scope, canonicalRequestHashHex);

#ifdef SIGNATURE_DEBUG
    printf("--\nString to Sign:\n%s\n", stringToSign);
#endif

    const char *secretAccessKey = params->bucketContext.secretAccessKey;
    char accessKey[strlen(secretAccessKey) + 5];
    snprintf(accessKey, sizeof(accessKey), "AWS4%s", secretAccessKey);

#ifdef __APPLE__
    unsigned char dateKey[S3_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, accessKey, strlen(accessKey), dateStr, 8, dateKey);
    unsigned char dateRegionKey[S3_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, dateKey, S3_SHA256_DIGEST_LENGTH, awsRegion,
           strlen(awsRegion), dateRegionKey);
    unsigned char dateRegionServiceKey[S3_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, dateRegionKey, S3_SHA256_DIGEST_LENGTH, "s3", 2,
           dateRegionServiceKey);
    unsigned char signingKey[S3_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, dateRegionServiceKey, S3_SHA256_DIGEST_LENGTH,
           "aws4_request", strlen("aws4_request"), signingKey);

    unsigned char finalSignature[S3_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, signingKey, S3_SHA256_DIGEST_LENGTH, stringToSign,
            strlen(stringToSign), finalSignature);
#else
    const EVP_MD *sha256evp = EVP_sha256();
    unsigned char dateKey[S3_SHA256_DIGEST_LENGTH];
    HMAC(sha256evp, accessKey, strlen(accessKey),
         (const unsigned char*) dateStr, 8, dateKey,
         NULL);
    unsigned char dateRegionKey[S3_SHA256_DIGEST_LENGTH];
    HMAC(sha256evp, dateKey, S3_SHA256_DIGEST_LENGTH,
         (const unsigned char*) awsRegion, strlen(awsRegion), dateRegionKey,
         NULL);
    unsigned char dateRegionServiceKey[S3_SHA256_DIGEST_LENGTH];
    HMAC(sha256evp, dateRegionKey, S3_SHA256_DIGEST_LENGTH,
         (const unsigned char*) "s3", 2, dateRegionServiceKey, NULL);
    unsigned char signingKey[S3_SHA256_DIGEST_LENGTH];
    HMAC(sha256evp, dateRegionServiceKey, S3_SHA256_DIGEST_LENGTH,
         (const unsigned char*) "aws4_request", strlen("aws4_request"),
         signingKey,
         NULL);

    unsigned char finalSignature[S3_SHA256_DIGEST_LENGTH];
    HMAC(sha256evp, signingKey, S3_SHA256_DIGEST_LENGTH,
         (const unsigned char*) stringToSign, strlen(stringToSign),
         finalSignature, NULL);
#endif

    len = 0;
    char finalSignatureHex[2 * S3_SHA256_DIGEST_LENGTH + 1];
    finalSignatureHex[0] = '\0';
    for (i = 0; i < S3_SHA256_DIGEST_LENGTH; i++) {
        buf_append(finalSignatureHex, "%02x", finalSignature[i]);
    }

    char credential[strlen(params->bucketContext.accessKeyId) + 1 + 8 + 1
        + strlen(awsRegion) + 1 + 2 + 1 + 12 + 1];
    snprintf(credential, sizeof(credential), "%s/%.8s/%s/s3/aws4_request",
             params->bucketContext.accessKeyId, dateStr, awsRegion);

    snprintf(
            values->authorizationHeader,
            sizeof(values->authorizationHeader),
            "Authorization: AWS4-HMAC-SHA256 Credential=%s,SignedHeaders=%s,Signature=%s",
            credential, values->signedHeaders, finalSignatureHex);

#ifdef SIGNATURE_DEBUG
    printf("--\nAuthorization Header:\n%s\n", values->authorizationHeader);
    printf("--\nAMZ Headers:\n");
    for (int i = 0; i < values->amzHeadersCount; i++) {
        printf("%s\n", values->amzHeaders[i]);
    }
#endif

    return S3StatusOK;

#undef buf_append

}


// Compose the URI to use for the request given the request parameters
static S3Status compose_uri(char *buffer, int bufferSize,
                            const S3BucketContext *bucketContext,
                            const char *urlEncodedKey,
                            const char *subResource, const char *queryParams)
{
    int len = 0;

#define uri_append(fmt, ...)                                                 \
    do {                                                                     \
        len += snprintf(&(buffer[len]), bufferSize - len, fmt, __VA_ARGS__); \
        if (len >= bufferSize) {                                             \
            return S3StatusUriTooLong;                                       \
        }                                                                    \
    } while (0)

    uri_append("http%s://",
               (bucketContext->protocol == S3ProtocolHTTP) ? "" : "s");

    const char *hostName =
        bucketContext->hostName ? bucketContext->hostName : defaultHostNameG;

    if (bucketContext->bucketName &&
        bucketContext->bucketName[0]) {
        if (bucketContext->uriStyle == S3UriStyleVirtualHost) {
            if (strchr(bucketContext->bucketName, '.') == NULL) {
                uri_append("%s.%s", bucketContext->bucketName, hostName);
            }
            else {
                // We'll use the hostName in the URL, and then explicitly set
                // the Host header to match bucket.host so that host validation
                // works.
                uri_append("%s", hostName);
            }
        }
        else {
            uri_append("%s/%s", hostName, bucketContext->bucketName);
        }
    }
    else {
        uri_append("%s", hostName);
    }

    uri_append("%s", "/");

    uri_append("%s", urlEncodedKey);

    if (subResource && subResource[0]) {
        uri_append("?%s", subResource);
    }

    if (queryParams) {
        uri_append("%s%s", (subResource && subResource[0]) ? "&" : "?",
                   queryParams);
    }

    return S3StatusOK;
}

// Sets up the curl handle given the completely computed RequestParams
static S3Status setup_curl(Request *request,
                           const RequestParams *params,
                           const RequestComputedValues *values)
{
    CURLcode status;

#define curl_easy_setopt_safe(opt, val)                                 \
    if ((status = curl_easy_setopt                                      \
         (request->curl, opt, val)) != CURLE_OK) {                      \
        return S3StatusFailedToInitializeRequest;                       \
    }

    // Debugging only
    // curl_easy_setopt_safe(CURLOPT_VERBOSE, 1);
    
    // Set private data to request for the benefit of S3RequestContext
    curl_easy_setopt_safe(CURLOPT_PRIVATE, request);
    
    // Set header callback and data
    curl_easy_setopt_safe(CURLOPT_HEADERDATA, request);
    curl_easy_setopt_safe(CURLOPT_HEADERFUNCTION, &curl_header_func);
    
    // Set read callback, data, and readSize
    curl_easy_setopt_safe(CURLOPT_READFUNCTION, &curl_read_func);
    curl_easy_setopt_safe(CURLOPT_READDATA, request);
    
    // Set write callback and data
    curl_easy_setopt_safe(CURLOPT_WRITEFUNCTION, &curl_write_func);
    curl_easy_setopt_safe(CURLOPT_WRITEDATA, request);

    // Ask curl to parse the Last-Modified header.  This is easier than
    // parsing it ourselves.
    curl_easy_setopt_safe(CURLOPT_FILETIME, 1);

    // Curl docs suggest that this is necessary for multithreaded code.
    // However, it also points out that DNS timeouts will not be honored
    // during DNS lookup, which can be worked around by using the c-ares
    // library, which we do not do yet.
    curl_easy_setopt_safe(CURLOPT_NOSIGNAL, 1);

    // Turn off Curl's built-in progress meter
    curl_easy_setopt_safe(CURLOPT_NOPROGRESS, 1);

    // xxx todo - support setting the proxy for Curl to use (can't use https
    // for proxies though)

    // xxx todo - support setting the network interface for Curl to use

    // I think this is useful - we don't need interactive performance, we need
    // to complete large operations quickly
    curl_easy_setopt_safe(CURLOPT_TCP_NODELAY, 1);
    
    // Don't use Curl's 'netrc' feature
    curl_easy_setopt_safe(CURLOPT_NETRC, CURL_NETRC_IGNORED);

    // Don't verify S3's certificate unless S3_INIT_VERIFY_PEER is set.
    // The request_context may be set to override this
    curl_easy_setopt_safe(CURLOPT_SSL_VERIFYPEER, verifyPeer);

    // Follow any redirection directives that S3 sends
    curl_easy_setopt_safe(CURLOPT_FOLLOWLOCATION, 1);

    // A safety valve in case S3 goes bananas with redirects
    curl_easy_setopt_safe(CURLOPT_MAXREDIRS, 10);

    // Set the User-Agent; maybe Amazon will track these?
    curl_easy_setopt_safe(CURLOPT_USERAGENT, userAgentG);

    // Set the low speed limit and time; we abort transfers that stay at
    // less than 1K per second for more than 15 seconds.
    // xxx todo - make these configurable
    // xxx todo - allow configurable max send and receive speed
    curl_easy_setopt_safe(CURLOPT_LOW_SPEED_LIMIT, 1024);
    curl_easy_setopt_safe(CURLOPT_LOW_SPEED_TIME, 15);

    // Append standard headers
#define append_standard_header(fieldName)                               \
    if (values-> fieldName [0]) {                                       \
        request->headers = curl_slist_append(request->headers,          \
                                             values-> fieldName);       \
    }

    // Would use CURLOPT_INFILESIZE_LARGE, but it is buggy in libcurl
    if ((params->httpRequestType == HttpRequestTypePUT) ||
        (params->httpRequestType == HttpRequestTypePOST)) {
        char header[256];
        snprintf(header, sizeof(header), "Content-Length: %llu",
                 (unsigned long long) params->toS3CallbackTotalSize);
        request->headers = curl_slist_append(request->headers, header);
        request->headers = curl_slist_append(request->headers, 
                                             "Transfer-Encoding:");
    }
    else if (params->httpRequestType == HttpRequestTypeCOPY) {
        request->headers = curl_slist_append(request->headers, 
                                             "Transfer-Encoding:");
    }
    
    append_standard_header(hostHeader);
    append_standard_header(cacheControlHeader);
    append_standard_header(contentTypeHeader);
    append_standard_header(md5Header);
    append_standard_header(contentDispositionHeader);
    append_standard_header(contentEncodingHeader);
    append_standard_header(expiresHeader);
    append_standard_header(ifModifiedSinceHeader);
    append_standard_header(ifUnmodifiedSinceHeader);
    append_standard_header(ifMatchHeader);
    append_standard_header(ifNoneMatchHeader);
    append_standard_header(rangeHeader);
    append_standard_header(authorizationHeader);

    // Append x-amz- headers
    int i;
    for (i = 0; i < values->amzHeadersCount; i++) {
        request->headers = 
            curl_slist_append(request->headers, values->amzHeaders[i]);
    }

    // Set the HTTP headers
    curl_easy_setopt_safe(CURLOPT_HTTPHEADER, request->headers);

    // Set URI
    curl_easy_setopt_safe(CURLOPT_URL, request->uri);

    // Set request type.
    switch (params->httpRequestType) {
    case HttpRequestTypeHEAD:
        curl_easy_setopt_safe(CURLOPT_NOBODY, 1);
        break;
    case HttpRequestTypePOST:
        curl_easy_setopt_safe(CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt_safe(CURLOPT_UPLOAD, 1);
        break;

    case HttpRequestTypePUT:
    case HttpRequestTypeCOPY:
        curl_easy_setopt_safe(CURLOPT_UPLOAD, 1);
        break;
    case HttpRequestTypeDELETE:
        curl_easy_setopt_safe(CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    default: // HttpRequestTypeGET
        break;
    }
    
    return S3StatusOK;
}


static void request_deinitialize(Request *request)
{
    if (request->headers) {
        curl_slist_free_all(request->headers);
    }
    
    error_parser_deinitialize(&(request->errorParser));

    // curl_easy_reset prevents connections from being re-used for some
    // reason.  This makes HTTP Keep-Alive meaningless and is very bad for
    // performance.  But it is necessary to allow curl to work properly.
    // xxx todo figure out why
    curl_easy_reset(request->curl);
}


static S3Status request_get(const RequestParams *params,
                            const RequestComputedValues *values,
                            Request **reqReturn)
{
    Request *request = 0;

    // Try to get one from the request stack.  We hold the lock for the
    // shortest time possible here.
    pthread_mutex_lock(&requestStackMutexG);

    if (requestStackCountG) {
        request = requestStackG[--requestStackCountG];
    }

    pthread_mutex_unlock(&requestStackMutexG);

    // If we got one, deinitialize it for re-use
    if (request) {
        request_deinitialize(request);
    }
    // Else there wasn't one available in the request stack, so create one
    else {
        if (!(request = (Request *) malloc(sizeof(Request)))) {
            return S3StatusOutOfMemory;
        }
        if (!(request->curl = curl_easy_init())) {
            free(request);
            return S3StatusFailedToInitializeRequest;
        }
    }

    // Initialize the request
    request->prev = 0;
    request->next = 0;

    // Request status is initialized to no error, will be updated whenever
    // an error occurs
    request->status = S3StatusOK;

    S3Status status;

    // Start out with no headers
    request->headers = 0;

    // Compute the URL
    if ((status = compose_uri
         (request->uri, sizeof(request->uri), 
          &(params->bucketContext), values->urlEncodedKey,
          params->subResource, params->queryParams)) != S3StatusOK) {
        curl_easy_cleanup(request->curl);
        free(request);
        return status;
    }

    // Set all of the curl handle options
    if ((status = setup_curl(request, params, values)) != S3StatusOK) {
        curl_easy_cleanup(request->curl);
        free(request);
        return status;
    }

    request->propertiesCallback = params->propertiesCallback;

    request->toS3Callback = params->toS3Callback;

    request->toS3CallbackBytesRemaining = params->toS3CallbackTotalSize;

    request->fromS3Callback = params->fromS3Callback;

    request->completeCallback = params->completeCallback;

    request->callbackData = params->callbackData;

    response_headers_handler_initialize(&(request->responseHeadersHandler));

    request->propertiesCallbackMade = 0;

    error_parser_initialize(&(request->errorParser));

    *reqReturn = request;

    return S3StatusOK;
}


static void request_destroy(Request *request)
{
    request_deinitialize(request);
    curl_easy_cleanup(request->curl);
    free(request);
}


static void request_release(Request *request)
{
    pthread_mutex_lock(&requestStackMutexG);

    // If the request stack is full, destroy this one
    if (requestStackCountG == REQUEST_STACK_SIZE) {
        pthread_mutex_unlock(&requestStackMutexG);
        request_destroy(request);
    }
    // Else put this one at the front of the request stack; we do this because
    // we want the most-recently-used curl handle to be re-used on the next
    // request, to maximize our chances of re-using a TCP connection before it
    // times out
    else {
        requestStackG[requestStackCountG++] = request;
        pthread_mutex_unlock(&requestStackMutexG);
    }
}


S3Status request_api_initialize(const char *userAgentInfo, int flags,
                                const char *defaultHostName)
{
    if (curl_global_init(CURL_GLOBAL_ALL & 
                         ~((flags & S3_INIT_WINSOCK) ? 0 : CURL_GLOBAL_WIN32))
        != CURLE_OK) {
        return S3StatusInternalError;
    }
    verifyPeer = (flags & S3_INIT_VERIFY_PEER) != 0;

    if (!defaultHostName) {
        defaultHostName = S3_DEFAULT_HOSTNAME;
    }

    if (snprintf(defaultHostNameG, S3_MAX_HOSTNAME_SIZE, 
                 "%s", defaultHostName) >= S3_MAX_HOSTNAME_SIZE) {
        return S3StatusUriTooLong;
    }

    pthread_mutex_init(&requestStackMutexG, 0);

    requestStackCountG = 0;

    if (!userAgentInfo || !*userAgentInfo) {
        userAgentInfo = "Unknown";
    }

    char platform[96];
    struct utsname utsn;
    if (uname(&utsn)) {
        snprintf(platform, sizeof(platform), "Unknown");
    }
    else {
        snprintf(platform, sizeof(platform), "%s%s%s", utsn.sysname, 
                 utsn.machine[0] ? " " : "", utsn.machine);
    }

    snprintf(userAgentG, sizeof(userAgentG), 
             "Mozilla/4.0 (Compatible; %s; libs3 %s.%s; %s)",
             userAgentInfo, LIBS3_VER_MAJOR, LIBS3_VER_MINOR, platform);
    
    return S3StatusOK;
}


void request_api_deinitialize()
{
    pthread_mutex_destroy(&requestStackMutexG);

    while (requestStackCountG--) {
        request_destroy(requestStackG[requestStackCountG]);
    }
}

void request_perform(const RequestParams *params, S3RequestContext *context)
{
    Request *request;
    S3Status status;
    int verifyPeerRequest = verifyPeer;
    CURLcode curlstatus;

#define return_status(status)                                           \
    (*(params->completeCallback))(status, 0, params->callbackData);     \
    return

    // These will hold the computed values
    RequestComputedValues computed;

    // Validate the bucket name
    if (params->bucketContext.bucketName && 
        ((status = S3_validate_bucket_name
          (params->bucketContext.bucketName, 
           params->bucketContext.uriStyle)) != S3StatusOK)) {
        return_status(status);
    }

    char date[64];
    if (1) {
        time_t now = time(NULL);
        struct tm gmt;
        gmtime_r(&now, &gmt);
        strftime(date, sizeof(date), "%Y%m%dT%H%M%SZ", &gmt);
    }
    else {
        snprintf(date, sizeof(date), "20130524T000000Z");
    }

    // Compose the amz headers
    if ((status = compose_amz_headers(params, date, &computed)) != S3StatusOK) {
        return_status(status);
    }

    // Compose standard headers
    if ((status = compose_standard_headers(params, &computed)) != S3StatusOK) {
        return_status(status);
    }

    // URL encode the key
    if ((status = encode_key(params, &computed)) != S3StatusOK) {
        return_status(status);
    }

    // Compute the canonicalized amz headers
    canonicalize_signature_headers(&computed);

    // Compute the canonicalized resource
    canonicalize_resource(&params->bucketContext, computed.urlEncodedKey,
                          computed.canonicalURI);
    canonicalize_query_string(params, computed.canonicalQueryString);

    // Compose Authorization header
    if ((status = compose_auth_header(params, date, &computed)) != S3StatusOK) {
        return_status(status);
    }

    // Get an initialized Request structure now
    if ((status = request_get(params, &computed, &request)) != S3StatusOK) {
        return_status(status);
    }
    if (context && context->verifyPeerSet) {
        verifyPeerRequest = context->verifyPeerSet;
    }
    // Allow per-context override of verifyPeer
    if (verifyPeerRequest != verifyPeer) {
        if ((curlstatus = curl_easy_setopt(request->curl,
                                           CURLOPT_SSL_VERIFYPEER,
                                           context->verifyPeer))
            != CURLE_OK) {
            return_status(S3StatusFailedToInitializeRequest);
        }
    }

    // If a RequestContext was provided, add the request to the curl multi
    if (context) {
        CURLMcode code = curl_multi_add_handle(context->curlm, request->curl);
        if (code == CURLM_OK) {
            if (context->requests) {
                request->prev = context->requests->prev;
                request->next = context->requests;
                context->requests->prev->next = request;
                context->requests->prev = request;
            }
            else {
                context->requests = request->next = request->prev = request;
            }
        }
        else {
            if (request->status == S3StatusOK) {
                request->status = (code == CURLM_OUT_OF_MEMORY) ?
                    S3StatusOutOfMemory : S3StatusInternalError;
            }
            request_finish(request);
        }
    }
    // Else, perform the request immediately
    else {
        CURLcode code = curl_easy_perform(request->curl);
        if ((code != CURLE_OK) && (request->status == S3StatusOK)) {
            request->status = request_curl_code_to_status(code);
        }

        // Finish the request, ensuring that all callbacks have been made, and
        // also releases the request
        request_finish(request);
    }
}


void request_finish(Request *request)
{
    // If we haven't detected this already, we now know that the headers are
    // definitely done being read in
    request_headers_done(request);
    
    // If there was no error processing the request, then possibly there was
    // an S3 error parsed, which should be converted into the request status
    if (request->status == S3StatusOK) {
        error_parser_convert_status(&(request->errorParser), 
                                    &(request->status));
        // If there still was no error recorded, then it is possible that
        // there was in fact an error but that there was no error XML
        // detailing the error
        if ((request->status == S3StatusOK) &&
            ((request->httpResponseCode < 200) ||
             (request->httpResponseCode > 299))) {
            switch (request->httpResponseCode) {
            case 0:
                // This happens if the request never got any HTTP response
                // headers at all, we call this a ConnectionFailed error
                request->status = S3StatusConnectionFailed;
                break;
            case 100: // Some versions of libcurl erroneously set HTTP
                      // status to this
                break;
            case 301:
                request->status = S3StatusErrorPermanentRedirect;
                break;
            case 307:
                request->status = S3StatusHttpErrorMovedTemporarily;
                break;
            case 400:
                request->status = S3StatusHttpErrorBadRequest;
                break;
            case 403: 
                request->status = S3StatusHttpErrorForbidden;
                break;
            case 404:
                request->status = S3StatusHttpErrorNotFound;
                break;
            case 405:
                request->status = S3StatusErrorMethodNotAllowed;
                break;
            case 409:
                request->status = S3StatusHttpErrorConflict;
                break;
            case 411:
                request->status = S3StatusErrorMissingContentLength;
                break;
            case 412:
                request->status = S3StatusErrorPreconditionFailed;
                break;
            case 416:
                request->status = S3StatusErrorInvalidRange;
                break;
            case 500:
                request->status = S3StatusErrorInternalError;
                break;
            case 501:
                request->status = S3StatusErrorNotImplemented;
                break;
            case 503:
                request->status = S3StatusErrorSlowDown;
                break;
            default:
                request->status = S3StatusHttpErrorUnknown;
                break;
            }
        }
    }

    (*(request->completeCallback))
        (request->status, &(request->errorParser.s3ErrorDetails),
         request->callbackData);

    request_release(request);
}


S3Status request_curl_code_to_status(CURLcode code)
{
    switch (code) {
    case CURLE_OUT_OF_MEMORY:
        return S3StatusOutOfMemory;
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
        return S3StatusNameLookupError;
    case CURLE_COULDNT_CONNECT:
        return S3StatusFailedToConnect;
    case CURLE_WRITE_ERROR:
    case CURLE_OPERATION_TIMEDOUT:
        return S3StatusConnectionFailed;
    case CURLE_PARTIAL_FILE:
        return S3StatusOK;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT:
        return S3StatusServerFailedVerification;
    default:
        return S3StatusInternalError;
    }
}


S3Status S3_generate_authenticated_query_string
    (char *buffer, const S3BucketContext *bucketContext,
     const char *key, int64_t expires, const char *resource)
{
#define MAX_EXPIRES (((int64_t) 1 << 31) - 1)
    // S3 seems to only accept expiration dates up to the number of seconds
    // representably by a signed 32-bit integer
    if (expires < 0) {
        expires = MAX_EXPIRES;
    }
    else if (expires > MAX_EXPIRES) {
        expires = MAX_EXPIRES;
    }

    // xxx todo: rework this so that it can be incorporated into shared code
    // with request_perform().  It's really unfortunate that this code is not
    // shared with request_perform().

    // URL encode the key
    char urlEncodedKey[S3_MAX_KEY_SIZE * 3];
    if (key) {
        urlEncode(urlEncodedKey, key, strlen(key));
    }
    else {
        urlEncodedKey[0] = 0;
    }

    // Compute canonicalized resource
    char canonicalizedResource[MAX_CANONICALIZED_RESOURCE_SIZE];
    canonicalize_resource(bucketContext, urlEncodedKey,
                          canonicalizedResource);

    // We allow for:
    // 17 bytes for HTTP-Verb + \n
    // 1 byte for empty Content-MD5 + \n
    // 1 byte for empty Content-Type + \n
    // 20 bytes for Expires + \n
    // 0 bytes for CanonicalizedAmzHeaders
    // CanonicalizedResource
    char signbuf[17 + 1 + 1 + 1 + 20 + sizeof(canonicalizedResource) + 1];
    int len = 0;

#define signbuf_append(format, ...)                             \
    len += snprintf(&(signbuf[len]), sizeof(signbuf) - len,     \
                    format, __VA_ARGS__)

    signbuf_append("%s\n", "GET"); // HTTP-Verb
    signbuf_append("%s\n", ""); // Content-MD5
    signbuf_append("%s\n", ""); // Content-Type
    signbuf_append("%llu\n", (unsigned long long) expires);
    signbuf_append("%s", canonicalizedResource);

    // Generate an HMAC-SHA-1 of the signbuf
    unsigned char hmac[20];

    HMAC_SHA1(hmac, (unsigned char *) bucketContext->secretAccessKey,
              strlen(bucketContext->secretAccessKey),
              (unsigned char *) signbuf, len);

    // Now base-64 encode the results
    char b64[((20 + 1) * 4) / 3];
    int b64Len = base64Encode(hmac, 20, b64);

    // Now urlEncode that
    char signature[sizeof(b64) * 3];
    urlEncode(signature, b64, b64Len);

    // Finally, compose the uri, with params:
    // ?AWSAccessKeyId=xxx[&Expires=]&Signature=xxx
    char queryParams[sizeof("AWSAccessKeyId=") + 20 + 
                     sizeof("&Expires=") + 20 + 
                     sizeof("&Signature=") + sizeof(signature) + 1];

    sprintf(queryParams, "AWSAccessKeyId=%s&Expires=%ld&Signature=%s",
            bucketContext->accessKeyId, (long) expires, signature);

    return compose_uri(buffer, S3_MAX_AUTHENTICATED_QUERY_STRING_SIZE,
                       bucketContext, urlEncodedKey, resource, queryParams);
}
