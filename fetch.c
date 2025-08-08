#include <signal.h>
#include <string.h>

#include <pw_parse.h>
#include "pw_curl.h"

// FileRequest type extends CurlRequest with file.

PwTypeId PwTypeId_FileRequest = 0;

typedef struct {
    /*
     * This structure extends CurlRequestData.
     */
    CurlRequestData curl_request;

    _PwValue file;  // autocleaned PwValue is not suitable for manually managed data,
                    // using bare structure that starts with underscore
} FileRequestData;

// this macro gets pointer to FileRequestData from PwValue
#define file_request_data_ptr(value)  ((FileRequestData*) ((value)->struct_data))


// global parameters from argv
_PwValue proxy   = PW_NULL;
_PwValue verbose = PW_BOOL(false);


// CURL session
void* curl_session = nullptr;


// signal handling

sig_atomic_t pending_sigint = 0;

void sigint_handler(int sig)
{
    puts("\nInterrupted");
    pending_sigint = 1;
}

[[nodiscard]] bool create_request(PwValuePtr url)
/*
 * Helper function to create Curl request of our custom FileRequest type
 */
{
    PwValue request = PW_NULL;
    if (!pw_create(PwTypeId_FileRequest, &request)) {
        pw_print_status(stdout, &current_task->status);
        return false;
    }

    PW_CSTRING_LOCAL(url_cstr, url);
    printf("Requesting %s\n", url_cstr);

    curl_request_set_url(&request, url);
    curl_request_set_proxy(&request, &proxy);
    if (verbose.bool_value) {
        curl_request_verbose(&request, true);
    }
    add_curl_request(curl_session, &request);

    // request is now held by Curl handle
    // and will be destroyed in curl_perform

    return true;
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

    FileRequestData* file_req = file_request_data_ptr(self);

    // get status from curl request
    curl_update_status(self);

    if(file_req->curl_request.status != 200) {
        PW_CSTRING_LOCAL(url_cstr, &file_req->curl_request.url);
        printf("FAILED: %u %s\n", file_req->curl_request.status, url_cstr);
        return 0;
    }
    if (pw_is_null(&file_req->file)) {

        // the file is not created yet, do that

        // get file name from response headers
        curl_request_parse_headers(&file_req->curl_request);
        PwValue filename_info = PW_NULL;
        if (!curl_request_get_filename(&file_req->curl_request, &filename_info)) {
            return 0;
        }
        PwValue full_name = PW_NULL;
        if (!pw_map_get(&filename_info, "filename", &full_name)) {
            pw_destroy(&full_name);
            full_name = PwString("");
        }
        PwValue filename = PW_NULL;
        if (!pw_basename(&full_name, &filename) || pw_strlen(&filename) == 0) {
            // get file name from URL
            PwValue parts = PW_NULL;
            if (!pw_string_split_chr(&file_req->curl_request.url, '?', 1, &parts)) {
                return 0;
            }
            PwValue url = PW_NULL;
            if (!pw_array_item(&parts, 0, &url)) {
                return 0;
            }
            if (!pw_basename(&url, &filename)) {
                return 0;
            }
            if (pw_strlen(&filename) == 0) {
                if (!pw_string_append(&filename, "index.html", nullptr)) {
                    return 0;
                }
            }
        }

        if (!pw_file_open(&filename, O_CREAT | O_RDWR | O_TRUNC, 0644, &file_req->file)) {
            pw_print_status(stdout, &current_task->status);
            return 0;
        }
        PW_CSTRING_LOCAL(url_cstr, &file_req->curl_request.url);
        PW_CSTRING_LOCAL(filename_cstr, &filename);
        printf("Downloading %s -> %s\n", url_cstr, filename_cstr);
    }

    // write data to file

    unsigned bytes_written;
    if (!pw_write(&file_req->file, data, size, &bytes_written)) {
        pw_print_status(stdout, &current_task->status);
        return 0;
    }
    return bytes_written;
}

void request_complete(PwValuePtr self)
/*
 * Overloaded method of Curl interface.
 */
{
    FileRequestData* file_req = file_request_data_ptr(self);

    if(file_req->curl_request.status != 200) {
        PW_CSTRING_LOCAL(url_cstr, &file_req->curl_request.url);
        printf("FAILED: %u %s\n", file_req->curl_request.status, url_cstr);
        return;
    }

    if (pw_is_null(&file_req->file)) {
        // nothing was written to file
        return;
    }

    if (!pw_file_close(&file_req->file)) {
        // ignore error
    }
}

void fini_file_request(PwValuePtr self)
/*
 * Overloaded method of Struct interface.
 */
{
    FileRequestData* req = file_request_data_ptr(self);

    pw_destroy(&req->file);
}

static bool pw_main(int argc, char* argv[])
{
    // parse command line arguments
    PwValue urls = PW_NULL;
    if (!pw_create_array(&urls)) {
        return false;
    }
    PwValue parallel = PW_UNSIGNED(1);
    for (int i = 1; i < argc; i++) {{  // mind double curly brackets for nested scope
        // nested scope makes autocleaning working after each iteration

        PwValue arg = PW_NULL;
        if (!pw_create_string(argv[i], &arg)) {
            return false;
        }
        if (pw_startswith(&arg, "http://") || pw_startswith(&arg, "https://")) {
            if (!pw_array_append(&urls, &arg)) {
                return false;
            }
        } else if (pw_startswith(&arg, "verbose=")) {
            PwValue v = PW_NULL;
            if (!pw_substr(&arg, strlen("verbose="), pw_strlen(&arg), &v)) {
                return false;
            }
            verbose.bool_value = pw_equal(&v, "1");

        } else if (pw_startswith(&arg, "proxy=")) {
            if (!pw_substr(&arg, strlen("proxy="), pw_strlen(&arg), &proxy)) {
                return false;
            }
        } else if (pw_startswith(&arg, "parallel=")) {
            PwValue s = PW_NULL;
            if (!pw_substr(&arg, strlen("parallel="), pw_strlen(&arg), &s)) {
                return false;
            }
            PwValue n = PW_NULL;
            if (pw_parse_number(&s, &n)) {
                parallel = n;
            }
        }
    }}
    if (pw_array_length(&urls) == 0) {
        printf("Usage: fetch [verbose=1|0] [proxy=<proxy>] [parallel=<n>] url1 url2 ...\n");
        return true;
    }

    // fetch URLs
    // prepare first request
    {
        PwValue url = PW_NULL;
        if (!pw_array_pop(&urls, &url)) {
            return false;
        }
        if (!create_request(&url)) {
            return false;
        }
    }

    // perform fetching

    while(!pending_sigint) {
        int running_transfers;
        if (!curl_perform(curl_session, &running_transfers)) {
            // failure
            // XXX set status
            break;
        }
        unsigned i = running_transfers;
        // add more requests
        for(; i < parallel.signed_value; i++) {{
            if (pw_array_length(&urls) == 0) {
                break;
            }
            PwValue url = PW_NULL;
            if (!pw_array_pop(&urls, &url)) {
                return false;
            }
            if (!create_request(&url)) {
                return false;
            }
        }}
        if (i == 0) {
            // no running transfers and no more URLs were added
            break;
        }
    }
    return true;
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

    // main routine

    curl_session = create_curl_session();

    if (!pw_main(argc, argv)) {
        pw_print_status(stdout, &current_task->status);
    }

    delete_curl_session(curl_session);

    // global finalization

    pw_destroy(&proxy);  // can be allocated string

    curl_global_cleanup();

    return 0;
}
