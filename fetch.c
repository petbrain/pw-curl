#include <signal.h>
#include <string.h>

#include <pw_parse.h>
#include "pw_curl.h"

// FileRequest type extends CurlRequest with file.

PwTypeId PwTypeId_FileRequest = 0;

typedef struct {
    _PwValue file;  // autocleaned PwValue is not suitable for manually managed data,
                    // using bare structure that starts with underscore
} FileRequestData;

// this macro gets pointer to FileRequestData from PwValue
#define file_request_data_ptr(value)  ((FileRequestData*) _pw_get_data_ptr((value), PwTypeId_FileRequest))


// global parameters from argv
__PWDECL_Null( proxy );
__PWDECL_Bool( verbose, false );


// signal handling

sig_atomic_t pending_sigint = 0;

void sigint_handler(int sig)
{
    puts("\nInterrupted");
    pending_sigint = 1;
}

void create_request(void* session, PwValuePtr url)
/*
 * Helper function to create Curl request of our custom FileRequest type
 */
{
    PwValue request = pw_create(PwTypeId_FileRequest);
    if (pw_error(&request)) {
        pw_print_status(stdout, &request);
        return;
    }

    PW_CSTRING_LOCAL(url_cstr, url);
    printf("Requesting %s\n", url_cstr);

    curl_request_set_url(&request, url);
    curl_request_set_proxy(&request, &proxy);
    if (verbose.bool_value) {
        curl_request_verbose(&request, true);
    }
    add_curl_request(session, &request);

    // request is now held by Curl handle
    // and will be destroyed in curl_perform
}

size_t write_data(void* data, size_t always_1, size_t size, PwValuePtr self)
/*
 * Overloaded method of Curl interface.
 */
{
    if (size == 0) {
        printf("%s is called with size=%zu\n", __func__, size);
        return 0;
    }

    CurlRequestData* curl_req = pw_curl_request_data_ptr(self);
    FileRequestData* file_req = file_request_data_ptr(self);

    // get status from curl request
    curl_update_status(self);

    if(curl_req->status != 200) {
        PW_CSTRING_LOCAL(url_cstr, &curl_req->url);
        printf("FAILED: %u %s\n", curl_req->status, url_cstr);
        return 0;
    }
    if (pw_is_null(&file_req->file)) {

        // the file is not created yet, do that

        // get file name from response headers
        curl_request_parse_headers(curl_req);
        PwValue filename_info = curl_request_get_filename(curl_req);
        PwValue full_name = pw_map_get(&filename_info, "filename");
        PwValue filename = pw_basename(&full_name);
        if (pw_error(&filename) || pw_strlen(&filename) == 0) {
            // get file name from URL
            PwValue parts = pw_string_split_chr(&curl_req->url, '?', 1);
            PwValue url = pw_array_item(&parts, 0);
            pw_destroy(&filename);
            filename = pw_basename(&url);
            if (pw_error(&filename)) {
                pw_print_status(stdout, &filename);
                return 0;
            }
            if (pw_strlen(&filename) == 0) {
                pw_string_append(&filename, "index.html");
            }
        }

        file_req->file = pw_file_open(&filename, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (pw_error(&file_req->file)) {
            pw_print_status(stdout, &file_req->file);
            return 0;
        }
        PW_CSTRING_LOCAL(url_cstr, &curl_req->url);
        PW_CSTRING_LOCAL(filename_cstr, &filename);
        printf("Downloading %s -> %s\n", url_cstr, filename_cstr);
    }

    // write data to file

    unsigned bytes_written;
    PwValue status = pw_write(&file_req->file, data, size, &bytes_written);
    if (pw_error(&status)) {
        pw_print_status(stdout, &status);
        return 0;
    }
    return bytes_written;
}

void request_complete(PwValuePtr self)
/*
 * Overloaded method of Curl interface.
 */
{
    CurlRequestData* curl_req = pw_curl_request_data_ptr(self);
    FileRequestData* file_req = file_request_data_ptr(self);

    if(curl_req->status != 200) {
        PW_CSTRING_LOCAL(url_cstr, &curl_req->url);
        printf("FAILED: %u %s\n", curl_req->status, url_cstr);
        return;
    }

    if (pw_is_null(&file_req->file)) {
        // nothing was written to file
        return;
    }

    pw_file_close(&file_req->file);
}

void fini_file_request(PwValuePtr self)
/*
 * Overloaded method of Struct interface.
 */
{
    FileRequestData* req = file_request_data_ptr(self);

    pw_destroy(&req->file);

    // call super method
    pw_ancestor_of(PwTypeId_FileRequest)->fini(self);
}

int main(int argc, char* argv[])
{
    // global initialization

    init_allocator(&pet_allocator);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // create FileRequest subtype

    // static structure that holds the type
    static PwType file_request_type;

    // custom Curl interface
    static PwInterface_Curl file_curl_interface = {
        .write_data = write_data,
        .complete   = request_complete
    };

    // create subtype, this initializes file_request_type and returns type id
    PwTypeId_FileRequest = pw_struct_subtype(
        &file_request_type, "FileRequest",
        PwTypeId_CurlRequest,  // base type
        FileRequestData,
        // overload CURL interface:
        PwInterfaceId_Curl, &file_curl_interface
    );
    // Default initialization of Struct subtype zeroes allocated data.
    // This makes file PwNull() and we don't need to overload init method.
    // We only need to overload fini for proper cleanup:
    file_request_type.fini = fini_file_request;

    // setup signal handling

    signal(SIGINT, sigint_handler);

    // create session

    void* session = create_curl_session();

    // parse command line arguments
    PwValue urls = PwArray();
    PwValue parallel = PwUnsigned(1);
    for (int i = 1; i < argc; i++) {{  // mind double curly brackets for nested scope
        // nested scope makes autocleaning working after each iteration

        PwValue arg = pw_create_string(argv[i]);

        if (pw_startswith(&arg, "http://") || pw_startswith(&arg, "https://")) {
            pw_array_append(&urls, &arg);

        } else if (pw_startswith(&arg, "verbose=")) {
            PwValue v = pw_substr(&arg, strlen("verbose="), pw_strlen(&arg));
            verbose.bool_value = pw_equal(&v, "1");

        } else if (pw_startswith(&arg, "proxy=")) {
            proxy = pw_substr(&arg, strlen("proxy="), pw_strlen(&arg));

        } else if (pw_startswith(&arg, "parallel=")) {
            PwValue s = pw_substr(&arg, strlen("parallel="), pw_strlen(&arg));
            PwValue n = pw_parse_number(&s);
            if (pw_is_int(&n)) {
                parallel = n;
            }
        }
    }}
    if (pw_array_length(&urls) == 0) {
        printf("Usage: fetch [verbose=1|0] [proxy=<proxy>] [parallel=<n>] url1 url2 ...\n");
        goto out;
    }

    // fetch URLs
    // prepare first request
    {
        PwValue url = pw_array_pop(&urls);
        if (pw_error(&url)) {
            pw_print_status(stdout, &url);
            goto out;
        }
        create_request(session, &url);
    }

    // perform fetching

    while(!pending_sigint) {
        int running_transfers;
        if (!curl_perform(session, &running_transfers)) {
            // failure
            break;
        }
        unsigned i = running_transfers;
        // add more requests
        for(; i < parallel.signed_value; i++) {{
            if (pw_array_length(&urls) == 0) {
                break;
            }
            PwValue url = pw_array_pop(&urls);
            if (pw_error(&url)) {
                pw_print_status(stdout, &url);
                break;
            }
            create_request(session, &url);
        }}
        if (i == 0) {
            // no running transfers and no more URLs were added
            break;
        }
    }

out:

    delete_curl_session(session);

    // global finalization

    pw_destroy(&proxy);  // can be allocated string

    curl_global_cleanup();

    return 0;
}
