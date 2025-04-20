#pragma once

#include <curl/curl.h>
#include <pw.h>

extern PwTypeId PwTypeId_CurlRequest;
/*
 * type id for CURL request, returned by pw_subtype
 */

extern unsigned PwInterfaceId_Curl;
/*
 * CURL interface id for CurlRequest
 *
 * important: stick to naming conventions for pw_get_interface macro to work
 */

typedef struct {
    size_t (*write_data)(void* data, size_t always_1, size_t size, PwValuePtr self);
    void   (*complete)  (PwValuePtr self);

} PwInterface_Curl;


typedef struct {
    CURL* easy_handle;

    _PwValue url;
    _PwValue proxy;
    _PwValue real_url;

    // Parsed headers, call curl_request_parse_headers for that.
    // Can be nullptr!
    _PwValue media_type;
    _PwValue media_subtype;
    _PwValue media_type_params;  // map
    _PwValue disposition_type;
    _PwValue disposition_params; // values can be strings of maps containing charset, language, and value

    // The content received by default handlers.
    // Always binary, regardless of content-type charset
    _PwValue content;

    struct curl_slist* headers;

    unsigned int status;

} CurlRequestData;

#define pw_curl_request_data_ptr(value)  ((CurlRequestData*) _pw_get_data_ptr((value), PwTypeId_CurlRequest))

// sessions
void* create_curl_session();
bool add_curl_request(void* session, PwValuePtr request);
void delete_curl_session(void* session);

// request
void curl_request_set_url(PwValuePtr request, PwValuePtr url);
void curl_request_set_proxy(PwValuePtr request, PwValuePtr proxy);
void curl_request_set_cookie(PwValuePtr request, PwValuePtr cookie);
void curl_request_set_resume(PwValuePtr request, size_t pos);
bool curl_request_set_headers(PwValuePtr request, char* http_headers[], unsigned num_headers);
void curl_request_verbose(PwValuePtr request, bool verbose);

void curl_update_status(PwValuePtr request);

// runner
bool curl_perform(void* session, int* running_transfers);

// utils
PwResult urljoin_cstr(char* base_url, char* other_url);
PwResult urljoin(PwValuePtr base_url, PwValuePtr other_url);

void curl_request_parse_content_type(CurlRequestData* req);
void curl_request_parse_content_disposition(CurlRequestData* req);
void curl_request_parse_headers(CurlRequestData* req);

PwResult curl_request_get_filename(CurlRequestData* req);
