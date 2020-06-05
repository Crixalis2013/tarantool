#!/usr/bin/env tarantool

local tap = require('tap')
local ffi = require('ffi')
ffi.cdef([[
    struct curl_version_info_data {
        int age;                  /* see description below */
        const char *version;      /* human readable string */
        unsigned int version_num; /* numeric representation */
        const char *host;         /* human readable string */
        int features;             /* bitmask, see below */
        char *ssl_version;        /* human readable string */
        long ssl_version_num;     /* not used, always zero */
        const char *libz_version; /* human readable string */
        const char * const *protocols; /* protocols */

        /* when 'age' is CURLVERSION_SECOND or higher, the members below exist */
        const char *ares;         /* human readable string */
        int ares_num;             /* number */

        /* when 'age' is CURLVERSION_THIRD or higher, the members below exist */
        const char *libidn;       /* human readable string */

        /* when 'age' is CURLVERSION_FOURTH or higher (>= 7.16.1), the members
           below exist */
        int iconv_ver_num;       /* '_libiconv_version' if iconv support enabled */

        const char *libssh_version; /* human readable string */

        /* when 'age' is CURLVERSION_FIFTH or higher (>= 7.57.0), the members
           below exist */
        unsigned int brotli_ver_num; /* Numeric Brotli version
                                        (MAJOR << 24) | (MINOR << 12) | PATCH */
        const char *brotli_version; /* human readable string. */

        /* when 'age' is CURLVERSION_SIXTH or higher (>= 7.66.0), the members
           below exist */
        unsigned int nghttp2_ver_num; /* Numeric nghttp2 version
                                         (MAJOR << 16) | (MINOR << 8) | PATCH */
        const char *nghttp2_version; /* human readable string. */

        const char *quic_version;    /* human readable quic (+ HTTP/3) library +
                                        version or NULL */

        /* when 'age' is CURLVERSION_SEVENTH or higher (>= 7.70.0), the members
           below exist */
        const char *cainfo;          /* the built-in default CURLOPT_CAINFO, might
                                        be NULL */
        const char *capath;          /* the built-in default CURLOPT_CAPATH, might
                                        be NULL */
    };

    struct curl_version_info_data *curl_version_info(int age);
]])

local info = ffi.C.curl_version_info(7)
local test = tap.test('curl-features')
test:plan(2)

if test:ok(info.ssl_version ~= nil, 'Curl built with SSL support') then
    test:diag('ssl_version: ' .. ffi.string(info.ssl_version))
end
if test:ok(info.libz_version ~= nil, 'Curl built with LIBZ') then
    test:diag('libz_version: ' .. ffi.string(info.libz_version))
end

os.exit(test:check() and 0 or 1)
