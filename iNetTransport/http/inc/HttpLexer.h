#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpToken.h"

// Cuts an HTTP/1.x request stream (a server's) or response stream (a
// client's) into HttpTokens, a byte at a time, so a message can arrive
// in any number of pieces, split anywhere. It is a state machine over
// the grammar (RFC 9112): it checks the syntax, trims header values, and
// frames the body, then starts on the next message (keep-alive and
// pipelining). Sizes are for whoever takes the tokens to limit; the lexer
// keeps none of the text.
//
// Requests: bodies are framed by Content-Length only. Rejected with an
// Error token: bad syntax (400), Transfer-Encoding (501), a
// Content-Length that is malformed or given twice differently (400) or
// over 2^31 (413), and a method over kText characters (501).
//
// Responses: bodies are framed by Transfer-Encoding: chunked (decoded;
// extensions and trailers skipped), Content-Length, or the connection
// closing (endOfInput()). 1xx, 204 and 304 responses, and the response to
// a HEAD request (expectNoBody()), have none. A 1xx response is lexed as
// a message of its own and the real one follows. Errors (all 502): bad
// syntax, a transfer coding other than chunked, a bad chunk size, a
// Content-Length as above, and a close in the middle of a message.
class HttpLexer {
public:
    enum class Mode : uint8_t { Request, Response };

    explicit HttpLexer(Mode mode = Mode::Request) : mode_(mode) { reset(); }

    void reset();                                      // keeps the mode
    void setMode(Mode m) { mode_ = m; reset(); }
    // Responses: the next one answers a HEAD request, so has no body
    // whatever its headers say.
    void expectNoBody() { noBody_ = true; }
    // Responses: the connection closed. Ends a body that runs to the
    // close; anything else cut short becomes an Error. Then feed() (with
    // nothing) hands the tokens over.
    void endOfInput();

    // Lexes data into sink. Returns the bytes used: fewer than len when
    // the sink filled up. Empty the sink and call again with the rest
    // (or with nothing, while pending()).
    size_t feed(const uint8_t* data, size_t len, HttpTokenSink& sink);

    bool pending() const { return outN_ != 0; }        // tokens made but not yet taken
    bool midMessage() const;                           // part of a message has been seen
    bool failed() const { return st_ == St::Dead; }

private:
    enum class St : uint8_t {
        LineStart, Method, TargetStart, Target, Version, LineLf,
        HdrStart, HdrName, HdrLead, HdrValue, HdrLf, EndLf, Body, Dead,
        // responses
        RVersion, RStatus, RReason,
        ChunkSize, ChunkExt, ChunkSizeLf, ChunkData, ChunkDataCr, ChunkDataLf,
        TrailerStart, TrailerLine, TrailerEndLf, BodyToClose,
    };

    void step(uint8_t c);
    void begin(HttpTokenType t);
    void add(char c);                   // to the token being built, cutting it when full
    void finish();                      // the token being built is complete
    void push(const HttpToken& t);
    void simple(HttpTokenType t, uint32_t num = 0);
    void fail(uint16_t code);
    void headerDone();
    void headersEnd();
    void message();                     // the message is complete: ready for the next
    void chunkSizeDone();
    void bodyDone();                    // a chunked or to-close body ended
    uint16_t bad() const { return mode_ == Mode::Request ? 400 : 502; }

    Mode      mode_;
    St        st_;
    HttpToken cur_;                     // the token being built
    HttpToken out_[5];                  // made, waiting: up to 3 from a byte, then 2 from endOfInput()
    uint8_t   outN_ = 0;

    // The header being read: is it one the lexer needs?
    uint8_t   nameLen_ = 0;
    bool      isCl_ = false, isTe_ = false;
    bool      inCl_ = false;            // reading a Content-Length value
    bool      clDigits_ = false;
    uint32_t  clValue_ = 0;             // this header's
    bool      haveCl_ = false;
    uint32_t  cl_ = 0;                  // the request's
    uint16_t  spaces_ = 0;              // whitespace inside a value, held until more follows
    uint32_t  bodyLeft_ = 0;

    // Responses.
    uint16_t  status_ = 0;
    uint8_t   statusDigits_ = 0;
    bool      noBody_ = false;          // expectNoBody(), until a final response
    bool      inTe_ = false;            // reading a Transfer-Encoding value
    uint8_t   teAt_ = 0;                // its characters matched against "chunked"
    bool      teBad_ = false;
    bool      chunked_ = false;
    bool      chunkDigits_ = false;
};
