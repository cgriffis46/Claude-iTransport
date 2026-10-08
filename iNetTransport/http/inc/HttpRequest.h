#pragma once
#include <cstddef>
#include <cstdint>

enum class HttpMethod : uint8_t { Get, Head, Post, Put, Delete, Options, Patch, Other };

// A request, as HttpConnection assembles it from the lexer's tokens into
// one fixed buffer (the arena): the target, the decoded path, the
// headers and the body. Everything here points into the arena and is
// valid while the handler runs.
class HttpRequest {
public:
    HttpMethod  method() const { return method_; }
    const char* methodName() const { return methodName_; }
    const char* target() const { return target_; }   // as sent: "/a%20b?x=1"
    const char* path() const { return path_; }       // percent-decoded, no query: "/a b"
    const char* query() const { return query_; }     // raw, after '?'; "" if none
    bool        http11() const { return minor_ >= 1; }

    // The first header of that name (any case), or nullptr.
    const char* header(const char* name) const;
    size_t      headerCount() const { return headerCount_; }
    // The i'th header, in the order sent.
    bool        headerAt(size_t i, const char*& name, const char*& value) const;

    const uint8_t* body() const { return body_; }
    size_t         bodyLength() const { return bodyLen_; }

    // A parameter from the query string or, for a body of type
    // application/x-www-form-urlencoded, the body (query first),
    // decoded into out. false: not there, or longer than cap - 1 (out
    // is "" then).
    bool param(const char* name, char* out, size_t cap) const;

    // Percent-decoding (and '+' as a space, for query strings and
    // forms). Returns the decoded length, or -1 if it doesn't fit in
    // cap - 1 or decodes to a NUL. out is NUL-terminated.
    static int32_t urlDecode(const char* in, size_t len, char* out, size_t cap, bool plusIsSpace);

private:
    friend class HttpConnection;
    static bool findParam(const char* s, size_t len, const char* name, char* out, size_t cap, bool& found);

    HttpMethod  method_ = HttpMethod::Other;
    char        methodName_[8] = {};
    uint8_t     minor_ = 1;
    const char* target_ = "";
    const char* path_ = "";
    const char* query_ = "";
    const char* headers_ = nullptr;   // name\0value\0 ...
    size_t      headerCount_ = 0;
    const uint8_t* body_ = nullptr;
    size_t      bodyLen_ = 0;
};
