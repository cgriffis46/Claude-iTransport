#include "HttpResponse.h"
#include <cstdio>
#include <cstring>

const char* HttpResponse::reason(uint16_t code) {
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "";
    }
}

void HttpResponse::reset(bool http11, bool keepAlive, bool head) {
    code_ = 200;
    http11_ = http11;
    keepAlive_ = keepAlive;
    head_ = head;
    started_ = ended_ = failed_ = chunked_ = false;
    left_ = 0;
    headersLen_ = 0;
}

HttpResponse& HttpResponse::status(uint16_t code) {
    if (!started_ && code >= 100 && code <= 999) code_ = code;
    return *this;
}

// A header that doesn't fit in kHeaderBytes (with the others) is dropped.
HttpResponse& HttpResponse::header(const char* name, const char* value) {
    if (started_ || name == nullptr || value == nullptr) return *this;
    const size_t n = std::strlen(name), v = std::strlen(value);
    if (headersLen_ + n + v + 4 > kHeaderBytes) return *this;
    char* p = headers_ + headersLen_;
    std::memcpy(p, name, n);
    p[n] = ':';
    p[n + 1] = ' ';
    std::memcpy(p + n + 2, value, v);
    p[n + 2 + v] = '\r';
    p[n + 3 + v] = '\n';
    headersLen_ += n + v + 4;
    return *this;
}

bool HttpResponse::raw(const void* data, size_t len) {
    if (failed_) return false;
    if (len && !out_.write(data, len)) {
        failed_ = true;
        keepAlive_ = false;
        return false;
    }
    return true;
}

bool HttpResponse::rawText(const char* s) { return raw(s, std::strlen(s)); }

bool HttpResponse::begin(const char* contentType, int32_t contentLength) {
    if (started_) return false;
    started_ = true;
    // No body at all for these (RFC 9110 6.4.1).
    const bool noBody = code_ < 200 || code_ == 204 || code_ == 304;
    char line[64];
    std::snprintf(line, sizeof line, "HTTP/1.1 %u %s\r\n", code_, reason(code_));
    rawText(line);
    raw(headers_, headersLen_);
    if (contentType && *contentType && !noBody) {
        rawText("Content-Type: ");
        rawText(contentType);
        rawText("\r\n");
    }
    if (noBody) {
        left_ = 0;
    } else if (contentLength >= 0) {
        left_ = contentLength;
        std::snprintf(line, sizeof line, "Content-Length: %ld\r\n", static_cast<long>(contentLength));
        rawText(line);
    } else if (http11_) {
        chunked_ = true;
        rawText("Transfer-Encoding: chunked\r\n");
    } else {
        keepAlive_ = false;   // an HTTP/1.0 client reads to the close
        left_ = kUnknownLength;
    }
    if (!keepAlive_) rawText("Connection: close\r\n");
    else if (!http11_) rawText("Connection: keep-alive\r\n");
    return raw("\r\n", 2);
}

bool HttpResponse::write(const void* data, size_t len) {
    if (!started_ || ended_ || failed_) return false;
    if (len == 0) return true;
    if (!chunked_ && left_ != kUnknownLength) {
        if (static_cast<size_t>(left_) < len) {   // more than Content-Length said
            keepAlive_ = false;
            return false;
        }
        left_ -= static_cast<int32_t>(len);
    }
    if (head_) return true;   // HEAD: the headers only
    if (chunked_) {
        char size[20];
        std::snprintf(size, sizeof size, "%lx\r\n", static_cast<unsigned long>(len));
        return rawText(size) && raw(data, len) && raw("\r\n", 2);
    }
    return raw(data, len);
}

bool HttpResponse::print(const char* text) {
    return text ? write(text, std::strlen(text)) : true;
}

// Formatted into 192 bytes on the stack; longer output is cut there and
// printf() returns false.
bool HttpResponse::printf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    const size_t len = static_cast<size_t>(n) < sizeof buf ? static_cast<size_t>(n) : sizeof buf - 1;
    return write(buf, len) && static_cast<size_t>(n) < sizeof buf;
}

bool HttpResponse::end() {
    if (!started_) return false;
    if (ended_) return !failed_;
    ended_ = true;
    if (chunked_ && !head_) raw("0\r\n\r\n", 5);
    if (left_ > 0) {
        // Less than Content-Length said: the client is still waiting for
        // the rest, so only closing the connection ends the response.
        keepAlive_ = false;
        return false;
    }
    return !failed_;
}

bool HttpResponse::send(const char* contentType, const void* body, size_t len) {
    return begin(contentType, static_cast<int32_t>(len)) && write(body, len) && end();
}

bool HttpResponse::send(const char* contentType, const char* text) {
    return send(contentType, text, text ? std::strlen(text) : 0);
}

bool HttpResponse::sendStatus(uint16_t code) {
    status(code);
    return send("text/plain", reason(code_));
}
