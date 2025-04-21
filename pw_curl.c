#include <stdlib.h>

#include <pw.h>

#include "pw_curl.h"

static char* default_http_headers[] = {
    "User-Agent: pw-curl (https://tilde.club/~petbrain/)",
    "Accept-Encoding: gzip, deflate, br, zstd"
};

/****************************************************************
 * CURL request
 */

static void fini_curl_request(PwValuePtr self)
/*
 * Basic PW interface method
 */
{
    CurlRequestData* req = pw_curl_request_data_ptr(self);

    pw_destroy(&req->url);
    pw_destroy(&req->proxy);
    pw_destroy(&req->real_url);
    pw_destroy(&req->media_type);
    pw_destroy(&req->media_subtype);
    pw_destroy(&req->media_type_params);
    pw_destroy(&req->disposition_type);
    pw_destroy(&req->disposition_params);
    pw_destroy(&req->content);

    if (req->headers) {
        curl_slist_free_all(req->headers);
        req->headers = nullptr;
    }

    if (req->easy_handle) {
        curl_easy_cleanup(req->easy_handle);
        req->easy_handle = nullptr;
    }

    // call super method

    pw_ancestor_of(PwTypeId_CurlRequest)->fini(self);
}

static PwResult init_curl_request(PwValuePtr self, void* ctor_args)
/*
 * Basic PW interface method
 * Initialize request structure and create CURL easy handle
 */
{
    // call super method

    PwValue status = pw_ancestor_of(PwTypeId_CurlRequest)->init(self, ctor_args);
    pw_return_if_error(&status);

    // init request

    CurlRequestData* req = pw_curl_request_data_ptr(self);

    req->url     = PwString();
    req->proxy   = PwString();
    req->media_type    = PwString();
    req->media_subtype = PwString();
    req->media_type_params = PwMap();
    //req->content_encoding_is_utf8 = false;
    req->status  = 0;
    req->real_url = pw_clone(&req->url);

    req->easy_handle = curl_easy_init();
    if (!req->easy_handle) {
        fprintf(stderr, "Cannot make CURL handle\n");
        fini_curl_request(self);
        return PwOOM();  // XXX use Curl error
    }

    if (!curl_request_set_headers(self, default_http_headers, PW_LENGTH(default_http_headers))) {
        fini_curl_request(self);
        return PwOOM();
    }

    // other essentials
    // XXX make configurable
    curl_easy_setopt(req->easy_handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br, zstd");
    curl_easy_setopt(req->easy_handle, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");

    curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, 1200L);
    curl_easy_setopt(req->easy_handle, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(req->easy_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req->easy_handle, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(req->easy_handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(req->easy_handle, CURLOPT_AUTOREFERER, 1L);

    // set self as private data for easy_handle
    PwValuePtr self_ptr = default_allocator.allocate(sizeof(_PwValue), false);

    // request data will be held by Curl handle and destroyed in check_transfers
    *self_ptr = pw_clone(self);
    curl_easy_setopt(req->easy_handle, CURLOPT_PRIVATE, self_ptr);

    // set write function
    PwInterface_Curl* iface = pw_interface(self->type_id, Curl);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, iface->write_data);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, self_ptr);

    // python leftovers to do someday:
    //
    // if method == 'POST':
    //     if form_data is not None:
    //         post_data = urlencode(form_data)
    //     c.setopt(c.POSTFIELDS, post_data)

    return PwOK();
}

static size_t request_write_data(void* data, size_t always_1, size_t size, PwValuePtr self)
{
    CurlRequestData* req = pw_curl_request_data_ptr(self);

    if (pw_is_null(&req->content)) {
        curl_request_parse_headers(req);

        curl_off_t content_length;
        CURLcode res = curl_easy_getinfo(req->easy_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
        if (res != CURLE_OK || content_length < 0) {
            content_length = 0;
        }
        req->content = pw_create_empty_string(content_length, 1);
        if (pw_error(&req->content)) {
            return 0;
        }
    }
    if (!size) {
        return 0;
    }
    if (!pw_string_append_buffer(&req->content, (uint8_t*) data, size)) {
        return 0;
    }
    return size;
}

static void request_complete(PwValuePtr self)
{
    CurlRequestData* req = pw_curl_request_data_ptr(self);

    if (pw_is_null(&req->content)) {
        curl_request_parse_headers(req);
    }
}

void curl_request_set_url(PwValuePtr request, PwValuePtr url)
{
    CurlRequestData* req = pw_curl_request_data_ptr(request);

    PW_CSTRING_LOCAL(url_cstr, url);
    curl_easy_setopt(req->easy_handle, CURLOPT_URL, url_cstr);
    pw_destroy(&req->url);
    req->url = pw_clone(url);
}

void curl_request_set_proxy(PwValuePtr request, PwValuePtr proxy)
{
    if (!pw_is_string(proxy)) {
        return;
    }

    CurlRequestData* req = pw_curl_request_data_ptr(request);

    PW_CSTRING_LOCAL(proxy_cstr, proxy);
    curl_easy_setopt(req->easy_handle, CURLOPT_PROXY, proxy_cstr);
    pw_destroy(&req->proxy);
    req->proxy = pw_clone(proxy);
}

void curl_request_set_cookie(PwValuePtr request, PwValuePtr cookie)
{
    if (!pw_is_string(cookie)) {
        return;
    }

    CurlRequestData* req = pw_curl_request_data_ptr(request);

    PW_CSTRING_LOCAL(cookie_cstr, cookie);
    curl_easy_setopt(req->easy_handle, CURLOPT_COOKIE, cookie_cstr);
}

void curl_request_set_resume(PwValuePtr request, size_t pos)
{
    if (pos == 0) {
        return;
    }

    CurlRequestData* req = pw_curl_request_data_ptr(request);

    curl_easy_setopt(req->easy_handle, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) pos);
}

bool curl_request_set_headers(PwValuePtr request, char* http_headers[], unsigned num_headers)
{
    CurlRequestData* req = pw_curl_request_data_ptr(request);

    for (size_t i = 0; i < num_headers; i++) {
        struct curl_slist* temp = curl_slist_append(req->headers, http_headers[i]);
        if (!temp) {
            fprintf(stderr, "Cannot make headers\n");
            return false;
        }
        req->headers = temp;
    }
    curl_easy_setopt(req->easy_handle, CURLOPT_HTTPHEADER, req->headers);
    return true;
}

void curl_request_verbose(PwValuePtr request, bool verbose)
{
    CurlRequestData* req = pw_curl_request_data_ptr(request);
    curl_easy_setopt(req->easy_handle, CURLOPT_VERBOSE, (long) verbose);
}

void curl_update_status(PwValuePtr request)
{
    CurlRequestData* req = pw_curl_request_data_ptr(request);

    long status;
    CURLcode err = curl_easy_getinfo(req->easy_handle, CURLINFO_RESPONSE_CODE, &status);
    if (err) {
        fprintf(stderr, "Error: %s\n", curl_easy_strerror(err));
    } else {
        req->status = (unsigned int) status;
    }
}

unsigned PwInterfaceId_Curl = 0;

static PwInterface_Curl curl_interface = {
    .write_data = request_write_data,
    .complete   = request_complete
};

PwTypeId PwTypeId_CurlRequest = 0;

static PwType curl_request_type;

[[ gnu::constructor ]]
static void init()
{
    // create Curl interface
    PwInterfaceId_Curl = pw_register_interface("Curl", PwInterface_Curl);

    // create CURL request subtype
    PwTypeId_CurlRequest = pw_struct_subtype(
        &curl_request_type, "CurlRequest",
        PwTypeId_Struct,
        CurlRequestData,
        // overload CURL interface:
        PwInterfaceId_Curl, &curl_interface
    );
    curl_request_type.init = init_curl_request;
    curl_request_type.fini = fini_curl_request;
}

/****************************************************************
 * CURL sessions and runner
 */

void* create_curl_session()
{
    CURLM* multi_handle = curl_multi_init();

#   ifdef CURLPIPE_MULTIPLEX
        // enables http/2
        curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#   endif

    return (void*) multi_handle;
}

void delete_curl_session(void* session)
{
    CURLM* multi_handle = (CURLM*) session;

    CURLMcode err = curl_multi_cleanup(multi_handle);
    if (err) {
        fprintf(stderr, "ERROR %s: %s\n", __func__, curl_multi_strerror(err));
    }
}

bool add_curl_request(void* session, PwValuePtr request)
{
    CURLM* multi_handle = (CURLM*) session;
    CurlRequestData* req = pw_curl_request_data_ptr(request);

    CURLMcode err = curl_multi_add_handle(multi_handle, req->easy_handle);
    if (err) {
        fprintf(stderr, "ERROR: %s\n", curl_multi_strerror(err));
        return false;
    } else {
        return true;
    }
}

static void check_transfers(CURLM* multi_handle)
{
    for(;;) {
        // check transfers
        int msgs_left;
        CURLMsg *m = curl_multi_info_read(multi_handle, &msgs_left);
        if (!m) {
            break;
        }
        if(m->msg != CURLMSG_DONE) {
            continue;
        }

        PwValuePtr request = nullptr;
        CURLcode err = curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char**) &request);
        if (err) {
            fprintf(stderr, "FATAL: %s\n", curl_easy_strerror(err));
            exit(0);
        }
        curl_easy_setopt(m->easy_handle, CURLOPT_PRIVATE, nullptr);

        CurlRequestData* req = pw_curl_request_data_ptr(request);

        if(m->data.result == CURLE_OK) {
            // get real URL
            char* url = nullptr;
            curl_easy_getinfo(req->easy_handle, CURLINFO_EFFECTIVE_URL, &url);
            if (url) {
                pw_destroy(&req->real_url);
                req->real_url = pw_create_string(url);
            }
            // get response status
            curl_update_status(request);

            // complete request
            pw_interface(request->type_id, Curl)->complete(request);
        }
        curl_multi_remove_handle(multi_handle, req->easy_handle);
        pw_destroy(request);
        default_allocator.release((void**) &request, sizeof(_PwValue));
    }
}

bool curl_perform(void* session, int* running_transfers)
{
    CURLM* multi_handle = (CURLM*) session;
    CURLMcode err;

    err = curl_multi_perform(multi_handle, running_transfers);
    if (err) {
        fprintf(stderr, "FATAL %s:%s:%d: %s\n", __FILE__, __func__, __LINE__, curl_multi_strerror(err));
        return false;
    }
    if (!*running_transfers) {
        // handles for completed requests do not appear here,
        // check them before exiting:
        check_transfers(multi_handle);
        return true;
    }

    // wait for something to happen
    err = curl_multi_wait(multi_handle, NULL, 0, 1000, NULL);
    if (err) {
        fprintf(stderr, "FATAL %s:%s:%d: %s\n", __FILE__, __func__, __LINE__, curl_multi_strerror(err));
        return false;
    }

    check_transfers(multi_handle);
    return true;
}
