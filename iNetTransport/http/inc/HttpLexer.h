#pragma once
#include <cstddef>
#include <cstdint>
#include "HttpToken.h"

// Cuts an HTTP/1.x request stream into HttpTokens, a byte at a time, so
// a request can arrive in any number of pieces, split anywhere. It is a
// state machine over the request's grammar (RFC 9112): it checks the
// syntax, trims header values, and frames the body by Content-Length,
// then starts on the next request (keep-alive and pipelining). Sizes are
// for whoever takes the tokens to limit; the lexer keeps none of the text.
//
// Rejected with an Error token: bad syntax (400), Transfer-Encoding
// (501: request bodies must have a Content-Length), a Content-Length that
// is malformed or given twice differently (400) or over 2^31 (413), and
// a method over kText characters (501).
class HttpLexer {
public:
    HttpLexer() { reset(); }

    void reset();

    // Lexes data into sink. Returns the bytes used: fewer than len when
    // the sink filled up. Empty the sink and call again with the rest
    // (or with nothing, while pending()).
    size_t feed(const uint8_t* data, size_t len, HttpTokenSink& sink);

    bool pending() const { return outN_ != 0; }        // tokens made but not yet taken
    bool midMessage() const;                           // part of a request has been seen
    bool failed() const { return st_ == St::Dead; }

private:
    enum class St : uint8_t {
        LineStart, Method, TargetStart, Target, Version, LineLf,
        HdrStart, HdrName, HdrLead, HdrValue, HdrLf, EndLf, Body, Dead,
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
    void message();                     // the request is complete: ready for the next

    St        st_;
    HttpToken cur_;                     // the token being built
    HttpToken out_[3];                  // made, waiting for the sink
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
};
