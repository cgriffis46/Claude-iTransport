#pragma once
#include <cstddef>
#include <cstdint>

// One token of an HTTP/1.x request or response, as HttpLexer cuts it
// from the byte stream. Tokens are a fixed size, so a FreeRTOS queue can carry them:
// text longer than kText (a long URL, header value or body) comes as
// several tokens of the same type, each but the last with `more` set.
//
//   request:  Method Target Version     (HeaderName HeaderValue)*  HeadersEnd  Body*  MessageEnd
//   response: Version Status Reason     (HeaderName HeaderValue)*  HeadersEnd  Body*  MessageEnd
//
// or, at any point, Error (and nothing more from that connection).
enum class HttpTokenType : uint8_t {
    Method,       // "GET"
    Target,       // "/path?query", as sent
    Version,      // "HTTP/1.1"
    HeaderName,   // "Content-Type", as sent
    HeaderValue,  // without the whitespace around it
    HeadersEnd,   // num: the body's length (Content-Length, else 0), or kChunked or kToClose
    Body,         // raw body bytes (dechunked)
    MessageEnd,   // the message is complete
    Error,        // num: the status code to answer with (400, 501, ...)
    Status,       // num: a response's status code
    Reason,       // a response's reason phrase ("Not Found"); may be empty
};

struct HttpToken {
    static constexpr size_t   kText = 40;
    static constexpr uint32_t kChunked = 0xFFFFFFFEu;   // HeadersEnd: Transfer-Encoding: chunked
    static constexpr uint32_t kToClose = 0xFFFFFFFFu;   // HeadersEnd: the body runs until the connection closes

    HttpTokenType type = HttpTokenType::Error;
    bool          more = false;   // continues in the next token
    uint16_t      len = 0;        // bytes in text (not NUL-terminated)
    uint32_t      num = 0;
    char          text[kText];
};

// Where the lexer puts tokens: a FreeRTOS queue on the target
// (xHttpServer), a plain container in tests. put() must not wait.
class HttpTokenSink {
public:
    virtual ~HttpTokenSink() = default;
    virtual bool put(const HttpToken& t) = 0;   // false: full, try again later
};
