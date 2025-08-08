#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pw.h>

#include "pw_curl.h"

static char* get_response_header(CURL* easy_handle, char* name)
/*
 * Return the pointer to an internally allocated memory.
 * The header should be parsed immediately, otherwise subsequent calls to CURL API
 * will clobber it.
 */
{
    char* last_value = nullptr;
    int last_request = 0;
    int last_amount = 1;

    struct curl_header* hdr;

    // iterate over requests made to get the last header
    for (int i = 0;; i++) {
        CURLHcode res = curl_easy_header(easy_handle, name, 0, CURLH_HEADER, i, &hdr);
        if (res == CURLHE_OK) {
            last_value = hdr->value;
            last_amount = hdr->amount;
            last_request = i;
        } else {
            break;
        }
    }
    if (last_value == nullptr) {
        return nullptr;
    }
    // get last instance of header
    CURLHcode res = curl_easy_header(easy_handle, name, last_amount - 1, CURLH_HEADER, last_request, &hdr);
    if (res == CURLHE_OK) {
        return hdr->value;
    } else {
        return last_value;
    }
}

static inline bool is_ctl(unsigned char c)
/*
 * https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
 *
 * CTL = <any US-ASCII control character
 *       (octets 0 - 31) and DEL (127)>
 */
{
    return (0 <= c && c <= 31) || c == 127;
}

static inline bool is_separator(unsigned char c)
/*
 * https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
 *
 * separators = "(" | ")" | "<" | ">" | "@"
 *            | "," | ";" | ":" | "\" | <">
 *            | "/" | "[" | "]" | "?" | "="
 *            | "{" | "}" | SP  | HT
 */
{
    switch (c) {
        case '(':  case ')':  case '<':  case '>':  case '@':
        case ',':  case ';':  case ':':  case '\\': case '"':
        case '/':  case '[':  case ']':  case '?':  case '=':
        case '{':  case '}':  case ' ':  case '\t':
            return true;
        default:
            return false;
    }
}

static inline void skip_lwsp(char** current_char)
/*
 * WSP = SP / HTAB
 * LWSP = *(WSP / CRLF WSP)
 */
{
    // simplified, not strictly follows the grammar
    char* ptr = *current_char;
    for (;;) {
        char c = *ptr;
        if (c == 0) {
            break;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ptr++;
    }
    *current_char = ptr;
}

[[nodiscard]] static bool parse_token(char** current_char, PwValuePtr result)
/*
 * https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
 *
 * token = 1*<any CHAR except CTLs or separators>
 *
 * Return token.
 */
{
    char* token_start = *current_char;
    char* token_end = token_start;

    while (!(is_separator(*token_end) || is_ctl(*token_end))) {
        token_end++;
    }
    PwValue token = PW_STRING("");
    if (!pw_string_append(&token, token_start, token_end)) {
        return false;
    }
    *current_char = token_end;
    pw_move(&token, result);
    return true;
}

[[nodiscard]] static bool parse_quoted_string(char** current_char, PwValuePtr result)
/*
 * https://datatracker.ietf.org/doc/html/rfc7230#section-3.2.6
 *
 * quoted-string = DQUOTE *( qdtext / quoted-pair ) DQUOTE
 * qdtext        = HTAB / SP / %x21 / %x23-5B / %x5D-7E / obs-text
 * obs-text      = %x80-FF
 * quoted-pair   = "\" ( HTAB / SP / VCHAR / obs-text )
 *
 * Return string or null value if not a quoted string.
 */
{
    char* qstr_start = *current_char;

    pw_destroy(result);  // this makes result Null

    if (*qstr_start != '"') {
        return true;
    }
    qstr_start++;

    *result = PwString("");

    char* qstr_end = qstr_start;
    for (;;) {
        unsigned char c = *qstr_end;
        if (is_ctl(c)) {
            if (c != ' ' && c != '\t') {
                break;
            }
        }
        if (c == '"') {
            break;
        }
        if (c != '\\') {
            qstr_end++;
            continue;
        }
        // append what we've got and skip quote char
        if (!pw_string_append(result, qstr_start, qstr_end)) {
            return false;
        }
        qstr_end++;
        qstr_start = qstr_end;
    }
    if (*qstr_end != '"') {
        // strict parsing, ignore malformed string
        if (!pw_string_truncate(result, 0)) {
            return false;
        }
    } else {
        if (!pw_string_append(result, qstr_start, qstr_end)) {
            return false;
        }
        qstr_end++;  // skip closing quote
    }
    *current_char = qstr_end;
    return true;
}

static inline bool is_mime_charsetc(char c)
/*
 * mime-charsetc = ALPHA / DIGIT
 *                 / "!" / "#" / "$" / "%" / "&"
 *                 / "+" / "-" / "^" / "_" / "`"
 *                 / "{" / "}" / "~"
 *                 ; as <mime-charset> in Section 2.3 of [RFC2978]
 *                 ; except that the single quote is not included
 *                 ; SHOULD be registered in the IANA charset registry
 */
{
    if (isalnum(c)) {
        return true;
    }
    switch (c) {
        case '!':  case '#':  case '$':  case '%':  case '&':
        case '+':  case '-':  case '^':  case '_':  case '`':
        case '{':  case '}':  case '~':
            return true;
        default:
            return false;
    }
}

static inline char xdigit_to_num(char** current_char)
{
    char c = **current_char;

    if (!isxdigit(c)) {
        return 0;
    }
    (*current_char)++;
    if (isdigit(c)) {
        return c - '0';
    } else if (islower(c)) {
        return 10 + c - 'a';
    } else {
        return 10 + c - 'A';
    }
}

static inline char32_t parse_value_char(char** current_char)
/*
 * value-chars = *( pct-encoded / attr-char )
 *
 * pct-encoded = "%" HEXDIG HEXDIG
 *               ; see [RFC3986], Section 2.1
 *
 * attr-char   = ALPHA / DIGIT
 *               / "!" / "#" / "$" / "&" / "+" / "-" / "."
 *               / "^" / "_" / "`" / "|" / "~"
 *               ; token except ( "*" / "'" / "%" )
 */
{
    char c = **current_char;

    if (isalnum(c)) {
        (*current_char)++;
        return c;
    }
    switch (c) {
        case '!':  case '#':  case '$':  case '&':  case '+':  case '-':  case '.':
        case '^':  case '_':  case '`':  case '|':  case '~':
            (*current_char)++;
            return c;
        case '%':
            break;
        default:
            return 0;
    }
    // pct-encoded
    (*current_char)++;
    char high_nibble = xdigit_to_num(current_char);
    if (high_nibble == 0) {
        return 0;
    }
    c = xdigit_to_num(current_char);
    if (c == 0) {
        return 0;
    }
    return (((char32_t) high_nibble) << 4) | c;
}

[[nodiscard]] static bool parse_ext_value(char** current_char, PwValuePtr result)
/*
 * current_char must point to the first non-space character
 *
 * ext-value           = charset  "'" [ language ] "'" value-chars
 *
 * charset             = "UTF-8" / "ISO-8859-1" / mime-charset
 *
 * mime-charset        = 1*mime-charsetc
 *
 * language            = <Language-Tag, defined in [RFC5646], Section 2.1>
 *
 * Return string or null value if not a quoted string.
 */
{
    char* charset_ptr = *current_char;
    char* language_ptr = *current_char;

    for (;;) {
        if (!is_mime_charsetc(*charset_ptr)) {
            break;
        }
        charset_ptr++;
    }

    pw_destroy(result);  // this makes result Null

    *current_char = language_ptr;
    if (*language_ptr != '\'') {
        // malformed ext-value
        return true;
    };

    *language_ptr++ = 0;  // terminate charset part

    char* value_ptr = language_ptr;

    // get language tag by simply searching closing single quote
    for (;;) {
        char c = *value_ptr;
        if (c == '\'' || c == 0) {
            break;
        }
        value_ptr++;
    }
    *current_char = value_ptr;

    if (*value_ptr != '\'') {
        // malformed ext-value
        return true;
    }
    *value_ptr++ = 0;  // terminate language part
    *current_char = value_ptr;

    PwValue value = PW_NULL;
    if (!pw_create_empty_string(strlen(value_ptr) + 1, 1, &value)) {
        return false;
    }
    for (;;) {
        char32_t c = parse_value_char(current_char);
        if (c == 0) {
            break;
        }
        if (!pw_string_append(&value, c)) {
            return false;
        }
    }
    PwValue charset = PW_NULL;
    if (!pw_create_string(charset_ptr, &charset)) {
        return false;
    }
    PwValue language = PW_NULL;
    if (!pw_create_string(language_ptr, &language)) {
        return false;
    }
    return pw_map_va(
        result,
        PwString("charset"),  pw_clone(&charset),
        PwString("language"), pw_clone(&language),
        PwString("value"),    pw_clone(&value)
    );
}

[[nodiscard]] static bool parse_media_type(char** current_char, CurlRequestData* req)
/*
 * https://datatracker.ietf.org/doc/html/rfc7231#section-3.1.1.1
 *
 * media-type = type "/" subtype *( OWS ";" OWS parameter )
 * type       = token
 * subtype    = token
 *
 * parameter  = token "=" ( token / quoted-string )
 *
 * XXX: replaced OWS with LWSP
 */
{
    PwValue media_type = PW_NULL;
    if (!parse_token(current_char, &media_type)) {
        return false;
    }
    if (**current_char == 0) {
        pw_set_status(PwStatus(PW_ERROR));
        return false;
    }
    if (**current_char != '/') {
        pw_set_status(PwStatus(PW_ERROR_EOF));
        return false;
    }
    (*current_char)++;

    PwValue media_subtype = PW_NULL;
    if (!parse_token(current_char, &media_subtype)) {
        return false;
    }

    PwValue params = PW_NULL;
    if (!pw_create_map(&params)) {
        return false;
    }
    for (;;) {
        skip_lwsp(current_char);
        if (**current_char == 0) {
            break;
        }
        if (**current_char != ';') {
            // malformed header, but we've got as most as we could, haven't we?
            break;
        }
        (*current_char)++;
        skip_lwsp(current_char);
        {
            PwValue param_name = PW_NULL;
            if (!parse_token(current_char, &param_name)) {
                return false;
            }
            skip_lwsp(current_char);
            if (**current_char != '=') {
                break;
            }
            (*current_char)++;
            skip_lwsp(current_char);

            if (**current_char == 0) {
                break;
            }

            PwValue param_value = PW_NULL;
            if (**current_char == '"') {
                if (!parse_quoted_string(current_char, &param_value)) {
                    return false;
                }
            } else {
                if (!parse_token(current_char, &param_value)) {
                    return false;
                }
            }
            if (!pw_is_string(&param_value)) {
                break;
            }

            if (!pw_string_lower(&param_name)) {
                return false;
            }
            if (!pw_map_update(&params, &param_name, &param_value)) {
                return false;
            }
        }
    }
    pw_move(&media_type,    &req->media_type);
    pw_move(&media_subtype, &req->media_subtype);
    pw_move(&params,        &req->media_type_params);
    return true;
}

[[nodiscard]] static bool parse_content_disposition(char** current_char, CurlRequestData* req)
/*
 * content-disposition = "Content-Disposition" ":"
 *                             disposition-type *( ";" disposition-parm )
 *
 * disposition-type    = "inline" | "attachment" | disp-ext-type
 *                       ; case-insensitive
 *
 * disp-ext-type       = token
 *
 * disposition-parm    = filename-parm | disp-ext-parm
 *
 * filename-parm       = "filename" "=" value
 *                     | "filename*" "=" ext-value
 *
 * disp-ext-parm       = token "=" value
 *                     | ext-token "=" ext-value
 *
 * ext-token           = <the characters in token, followed by "*">
 */
{
    PwValue disposition_type = PW_NULL;
    if (!parse_token(current_char, &disposition_type)) {
        return false;
    }
    if (!pw_string_lower(&disposition_type)) {
        return false;
    }
    PwValue params = PW_NULL;
    if (!pw_create_map(&params)) {
        return false;
    }
    for (;;) {
        skip_lwsp(current_char);
        if (**current_char == 0) {
            break;
        }
        if (**current_char != ';') {
            // malformed header, but we've got as most as we could, haven't we?
            break;
        }
        (*current_char)++;
        skip_lwsp(current_char);
        {
            bool is_ext_value = false;

            PwValue param_name = PW_NULL;
            if (!parse_token(current_char, &param_name)) {
                return false;
            }
            if (**current_char == '*') {
                is_ext_value = true;
                (*current_char)++;
            }
            skip_lwsp(current_char);
            if (**current_char != '=') {
                break;
            }
            (*current_char)++;
            skip_lwsp(current_char);

            if (**current_char == 0) {
                break;
            }

            PwValue param_value = PW_NULL;
            if (is_ext_value) {
                if (!parse_ext_value(current_char, &param_value)) {
                    return false;
                }
            } else if (**current_char == '"') {
                if (!parse_quoted_string(current_char, &param_value)) {
                    return false;
                }
            } else {
                if (!parse_token(current_char, &param_value)) {
                    return false;
                }
            }
            if (!pw_is_string(&param_value)) {
                break;
            }
            if (!pw_string_lower(&param_name)) {
                return false;
            }
            if (!pw_map_update(&params, &param_name, &param_value)) {
                return false;
            }
        }
    }
    pw_move(&disposition_type, &req->disposition_type);
    pw_move(&params,           &req->disposition_params);
    return true;
}

void curl_request_parse_content_type(CurlRequestData* req)
/*
 * Parse content-type header
 */
{
    CURLcode res;
    char* content_type;
    res = curl_easy_getinfo(req->easy_handle, CURLINFO_CONTENT_TYPE, &content_type);
    if (res != CURLE_OK) {
        return;
    }
    if (!content_type) {
        return;
    }
    char* ct = content_type;
    if (!parse_media_type(&ct, req)) {
        fprintf(stderr, "WARNING: failed to parse content type %s\n", content_type);
    }
}

void curl_request_parse_content_disposition(CurlRequestData* req)
/*
 * Parse content-disposition header
 */
{
    char* content_disposition = get_response_header(req->easy_handle, "Content-Disposition");
    if (!content_disposition) {
        return;
    }

    char* p = content_disposition;
    if (!parse_content_disposition(&p, req)) {
        fprintf(stderr, "WARNING: failed to parse content dispostion %s\n", content_disposition);
    }
}

void curl_request_parse_headers(CurlRequestData* req)
{
    curl_request_parse_content_type(req);
    curl_request_parse_content_disposition(req);
}

[[nodiscard]] bool curl_request_get_filename(CurlRequestData* req, PwValuePtr result)
/*
 * Get file name from the following sources:
 *   - Content-Disposition
 *   - Location
 *   - URL
 *
 * Return map containing filename and charset.
 *
 * If no filename found and URL ends with slash, return "index.html"
 */
{
    if (pw_is_map(&req->disposition_params)) {
        if (pw_is_string(&req->disposition_type) && pw_equal(&req->disposition_type, "attachment")) {
            PwValue filename = PW_NULL;
            if (pw_map_get(&req->disposition_params, "filename", &filename)) {
                if (pw_is_map(&filename)) {
                    PwValue fname = PW_NULL;
                    if (!pw_map_get(&filename, "value", &fname)) {
                        return false;
                    }
                    PwValue charset = PW_NULL;
                    if (!pw_map_get(&filename, "charset", &charset)) {
                        return false;
                    }
                    return pw_map_va(
                        result,
                        PwString("filename"), pw_clone(&fname),
                        PwString("charset"),  pw_clone(&charset)
                    );
                } else {
                    return pw_map_va(
                        result,
                        PwString("filename"), pw_clone(&filename),
                        PwString("charset"),  PwString("")
                    );
                }
            }
        }
    }

    PwValue parts = PW_NULL;

    char* last_location = get_response_header(req->easy_handle, "Location");
    if (last_location) {
        PwValue location = PW_NULL;
        if (!pw_create_string(last_location, &location)) {
            return false;
        }
        if (!pw_string_split_chr(&location, '/', 0, &parts)) {
            return false;
        }
    } else {
        if (!pw_string_split_chr(&req->url, '/', 0, &parts)) {
            return false;
        }
    }

    PwValue filename = PW_NULL;
    if (!pw_array_item(&parts, -1, &filename)) {
        return false;
    }
    if (pw_strlen(&filename) == 0) {
        if (!pw_string_append(&filename, "index.html", nullptr)) {
            return false;
        }
    }
    return pw_map_va(
        result,
        PwString("filename"), pw_clone(&filename),
        PwString("charset"),  PwString("")
    );
}

[[nodiscard]] bool urljoin_cstr(char* base_url, char* other_url, PwValuePtr result)
{
    CURLU* handle = curl_url();
    if (!handle) {
        pw_set_status(PwStatus(PW_ERROR_OOM));
        return false;
    }

    CURLUcode rc;

    rc = curl_url_set(handle, CURLUPART_URL, base_url, 0);
    if(rc) {
        pw_set_status(PwStatus(PW_ERROR), "URL error: %s", curl_url_strerror(rc));

        curl_url_cleanup(handle);
        return false;
    }
    rc = curl_url_set(handle, CURLUPART_URL, other_url, 0);
    if(rc) {
        pw_set_status(PwStatus(PW_ERROR), "URL error: %s", curl_url_strerror(rc));

        curl_url_cleanup(handle);
        return false;
    }
    char* url;
    rc = curl_url_get(handle, CURLUPART_URL, &url, 0);
    if(rc) {
        pw_set_status(PwStatus(PW_ERROR), "URL error: %s", curl_url_strerror(rc));

        curl_url_cleanup(handle);
        return false;
    }
    bool ret = pw_create_string(url, result);
    curl_free(url);
    curl_url_cleanup(handle);
    return ret;
}

[[nodiscard]] bool urljoin(PwValuePtr base_url, PwValuePtr other_url, PwValuePtr result)
{
    PW_CSTRING_LOCAL(cstr_base_url, base_url);
    PW_CSTRING_LOCAL(cstr_other_url, other_url);
    return urljoin_cstr(cstr_base_url, cstr_other_url, result);
}
