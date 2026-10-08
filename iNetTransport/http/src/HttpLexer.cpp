#include "HttpLexer.h"
#include <cstring>

namespace {

// RFC 9110 tchar: the characters of methods and header names.
bool tchar(uint8_t c) {
    if (c >= '0' && c <= '9') return true;
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return true;
    return c != 0 && std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

bool visible(uint8_t c) { return c > 0x20 && c < 0x7F; }

int hexDigit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
    return -1;
}

const char kChunked[] = "chunked";

const char kContentLength[] = "content-length";
const char kTransferEncoding[] = "transfer-encoding";

}  // namespace

void HttpLexer::reset() {
    st_ = St::LineStart;
    outN_ = 0;
    cur_.len = 0;
    nameLen_ = 0;
    isCl_ = isTe_ = inCl_ = clDigits_ = haveCl_ = false;
    clValue_ = cl_ = 0;
    spaces_ = 0;
    bodyLeft_ = 0;
    status_ = 0;
    statusDigits_ = 0;
    noBody_ = false;
    inTe_ = teBad_ = chunked_ = chunkDigits_ = false;
    teAt_ = 0;
}

void HttpLexer::endOfInput() {
    if (st_ == St::BodyToClose) return bodyDone();
    if (st_ == St::LineStart || st_ == St::Dead) return;
    fail(bad());   // cut off in the middle of a message
}

bool HttpLexer::midMessage() const {
    return st_ != St::LineStart && st_ != St::Dead;
}

size_t HttpLexer::feed(const uint8_t* data, size_t len, HttpTokenSink& sink) {
    size_t used = 0;
    for (;;) {
        // Hand over what's made before making more: tokens stay in order,
        // and at most a few wait here.
        while (outN_ != 0) {
            if (!sink.put(out_[0])) return used;
            for (uint8_t i = 1; i < outN_; ++i) out_[i - 1] = out_[i];
            --outN_;
        }
        if (used == len) return used;
        if (st_ == St::Dead) return len;   // nothing more from this connection
        step(data[used++]);
    }
}

void HttpLexer::push(const HttpToken& t) {
    out_[outN_++] = t;   // step() makes at most three per byte
}

void HttpLexer::begin(HttpTokenType t) {
    cur_.type = t;
    cur_.more = false;
    cur_.len = 0;
    cur_.num = 0;
}

void HttpLexer::add(char c) {
    if (cur_.len == HttpToken::kText) {
        cur_.more = true;
        push(cur_);
        cur_.len = 0;
        cur_.more = false;
    }
    cur_.text[cur_.len++] = c;
}

void HttpLexer::finish() {
    cur_.more = false;
    push(cur_);
    cur_.len = 0;
}

void HttpLexer::simple(HttpTokenType t, uint32_t num) {
    HttpToken k;
    k.type = t;
    k.num = num;
    push(k);
}

void HttpLexer::fail(uint16_t code) {
    // Whatever was being built is dropped: the message is rejected whole.
    // A client has no one to answer, so every response error is a 502.
    simple(HttpTokenType::Error, mode_ == Mode::Request ? code : 502);
    st_ = St::Dead;
}

void HttpLexer::step(uint8_t c) {
    switch (st_) {
    case St::LineStart:
        // Empty lines before a request are allowed (RFC 9112 2.2).
        if (c == '\r' || c == '\n') return;
        if (mode_ == Mode::Response) {
            if (!visible(c)) return fail(502);
            begin(HttpTokenType::Version);
            add(static_cast<char>(c));
            st_ = St::RVersion;
            return;
        }
        if (!tchar(c)) return fail(400);
        begin(HttpTokenType::Method);
        add(static_cast<char>(c));
        st_ = St::Method;
        return;

    case St::Method:
        if (c == ' ') { finish(); st_ = St::TargetStart; return; }
        if (!tchar(c)) return fail(400);
        if (cur_.len == HttpToken::kText) return fail(501);   // no such method
        add(static_cast<char>(c));
        return;

    case St::TargetStart:
        if (!visible(c)) return fail(400);
        begin(HttpTokenType::Target);
        add(static_cast<char>(c));
        st_ = St::Target;
        return;

    case St::Target:
        if (c == ' ') { finish(); begin(HttpTokenType::Version); st_ = St::Version; return; }
        if (!visible(c)) return fail(400);   // CR/LF here: an HTTP/0.9 request
        add(static_cast<char>(c));
        return;

    case St::Version:
        if (c == '\r' || c == '\n') {
            if (cur_.len == 0) return fail(400);
            finish();
            st_ = c == '\r' ? St::LineLf : St::HdrStart;
            return;
        }
        if (!visible(c) || cur_.len == 8) return fail(400);   // "HTTP/1.1"
        add(static_cast<char>(c));
        return;

    case St::RVersion:
        if (c == ' ') {
            finish();
            status_ = 0;
            statusDigits_ = 0;
            st_ = St::RStatus;
            return;
        }
        if (!visible(c) || cur_.len == 8) return fail(502);   // "HTTP/1.1"
        add(static_cast<char>(c));
        return;

    case St::RStatus:
        if (c >= '0' && c <= '9' && statusDigits_ < 3) {
            status_ = static_cast<uint16_t>(status_ * 10 + (c - '0'));
            ++statusDigits_;
            return;
        }
        if (statusDigits_ != 3 || status_ < 100 || (c != ' ' && c != '\r' && c != '\n')) return fail(502);
        simple(HttpTokenType::Status, status_);
        begin(HttpTokenType::Reason);
        st_ = St::RReason;
        if (c == ' ') return;
        /* FALLTHRU */   // no reason phrase: the line ends here
    case St::RReason:
        if (c == '\r' || c == '\n') {
            finish();
            st_ = c == '\r' ? St::LineLf : St::HdrStart;
            return;
        }
        if (c != '\t' && (c < 0x20 || c == 0x7F)) return fail(502);
        add(static_cast<char>(c));
        return;

    case St::LineLf:
        if (c != '\n') return fail(400);
        st_ = St::HdrStart;
        return;

    case St::HdrStart:
        if (c == '\r') { st_ = St::EndLf; return; }
        if (c == '\n') return headersEnd();
        if (!tchar(c)) return fail(400);   // including obsolete line folding
        begin(HttpTokenType::HeaderName);
        nameLen_ = 0;
        isCl_ = isTe_ = true;
        st_ = St::HdrName;
        // fall through: the first character of the name
        /* FALLTHRU */
    case St::HdrName: {
        if (c == ':') {
            isCl_ = isCl_ && nameLen_ == sizeof kContentLength - 1;
            isTe_ = isTe_ && nameLen_ == sizeof kTransferEncoding - 1;
            if (isTe_ && mode_ == Mode::Request) return fail(501);
            finish();
            inTe_ = isTe_;
            teAt_ = 0;
            teBad_ = false;
            begin(HttpTokenType::HeaderValue);
            inCl_ = isCl_;
            clDigits_ = false;
            clValue_ = 0;
            spaces_ = 0;
            st_ = St::HdrLead;
            return;
        }
        if (!tchar(c)) return fail(400);   // whitespace before the colon, too
        const char lc = static_cast<char>(c | 0x20);
        if (nameLen_ >= sizeof kContentLength - 1 || kContentLength[nameLen_] != lc) isCl_ = false;
        if (nameLen_ >= sizeof kTransferEncoding - 1 || kTransferEncoding[nameLen_] != lc) isTe_ = false;
        if (nameLen_ < 255) ++nameLen_;
        add(static_cast<char>(c));
        return;
    }

    case St::HdrLead:
        if (c == ' ' || c == '\t') return;
        st_ = St::HdrValue;
        /* FALLTHRU */
    case St::HdrValue:
        if (c == ' ' || c == '\t') {
            // Kept only if more follows. A run as long as a token goes
            // into the value now, so one byte never makes more tokens
            // than out_ holds.
            if (inCl_) spaces_ = 1;
            else if (++spaces_ == HttpToken::kText) for (; spaces_; --spaces_) add(' ');
            return;
        }
        if (c == '\r' || c == '\n') {
            headerDone();
            if (st_ == St::Dead) return;
            st_ = c == '\r' ? St::HdrLf : St::HdrStart;
            return;
        }
        if (c < 0x20 || c == 0x7F) return fail(400);
        if (inTe_) {
            // Only "chunked" alone: no other coding can be undone here.
            if (spaces_ || teAt_ >= sizeof kChunked - 1 || kChunked[teAt_] != static_cast<char>(c | 0x20)) teBad_ = true;
            else ++teAt_;
        }
        if (inCl_) {
            if (spaces_ || c < '0' || c > '9') return fail(400);
            if (clValue_ > 214748364u) return fail(413);   // over 2^31
            clValue_ = clValue_ * 10 + (c - '0');
            clDigits_ = true;
        }
        for (; spaces_; --spaces_) add(' ');
        add(static_cast<char>(c));
        return;

    case St::HdrLf:
        if (c != '\n') return fail(400);
        st_ = St::HdrStart;
        return;

    case St::EndLf:
        if (c != '\n') return fail(400);
        return headersEnd();

    case St::Body:
        add(static_cast<char>(c));
        if (--bodyLeft_ == 0) {
            finish();
            message();
        }
        return;

    case St::ChunkSize: {
        const int h = hexDigit(c);
        if (h >= 0) {
            if (bodyLeft_ > 0x07FFFFFFu) return fail(502);
            bodyLeft_ = bodyLeft_ * 16 + static_cast<uint32_t>(h);
            chunkDigits_ = true;
            return;
        }
        if (!chunkDigits_) return fail(502);
        if (c == ';' || c == ' ' || c == '\t') { st_ = St::ChunkExt; return; }
        if (c == '\r') { st_ = St::ChunkSizeLf; return; }
        if (c == '\n') return chunkSizeDone();
        return fail(502);
    }

    case St::ChunkExt:   // chunk extensions: skipped
        if (c == '\r') st_ = St::ChunkSizeLf;
        else if (c == '\n') chunkSizeDone();
        else if (c < 0x20 && c != '\t') fail(502);
        return;

    case St::ChunkSizeLf:
        if (c != '\n') return fail(502);
        return chunkSizeDone();

    case St::ChunkData:
        add(static_cast<char>(c));
        if (--bodyLeft_ == 0) st_ = St::ChunkDataCr;
        return;

    case St::ChunkDataCr:
        if (c == '\r') { st_ = St::ChunkDataLf; return; }
        if (c == '\n') { st_ = St::ChunkSize; return; }
        return fail(502);

    case St::ChunkDataLf:
        if (c != '\n') return fail(502);
        st_ = St::ChunkSize;
        return;

    case St::TrailerStart:   // trailer fields: skipped
        if (c == '\r') { st_ = St::TrailerEndLf; return; }
        if (c == '\n') return bodyDone();
        st_ = St::TrailerLine;
        return;

    case St::TrailerLine:
        if (c == '\n') st_ = St::TrailerStart;
        return;

    case St::TrailerEndLf:
        if (c != '\n') return fail(502);
        return bodyDone();

    case St::BodyToClose:
        add(static_cast<char>(c));
        return;

    case St::Dead:
        return;
    }
}

void HttpLexer::chunkSizeDone() {
    chunkDigits_ = false;
    st_ = bodyLeft_ == 0 ? St::TrailerStart : St::ChunkData;
}

void HttpLexer::bodyDone() {
    if (cur_.len) finish();
    message();
}

void HttpLexer::headerDone() {
    spaces_ = 0;   // trailing whitespace
    finish();
    if (inTe_) {
        inTe_ = false;
        if (teBad_ || teAt_ != sizeof kChunked - 1) return fail(502);
        chunked_ = true;
    }
    if (!inCl_) return;
    inCl_ = false;
    if (!clDigits_) return fail(400);
    if (haveCl_ && clValue_ != cl_) return fail(400);
    haveCl_ = true;
    cl_ = clValue_;
}

void HttpLexer::headersEnd() {
    if (mode_ == Mode::Response) {
        const bool interim = status_ < 200;
        if (interim || noBody_ || status_ == 204 || status_ == 304) {
            if (!interim) noBody_ = false;   // a 1xx: the real response is still to come
            simple(HttpTokenType::HeadersEnd, 0);
            return message();
        }
        if (chunked_) {   // wins over a Content-Length (RFC 9112 6.3)
            simple(HttpTokenType::HeadersEnd, HttpToken::kChunked);
            begin(HttpTokenType::Body);
            bodyLeft_ = 0;
            chunkDigits_ = false;
            st_ = St::ChunkSize;
            return;
        }
        if (!haveCl_) {
            simple(HttpTokenType::HeadersEnd, HttpToken::kToClose);
            begin(HttpTokenType::Body);
            st_ = St::BodyToClose;
            return;
        }
    }
    simple(HttpTokenType::HeadersEnd, cl_);
    if (cl_ == 0) return message();
    bodyLeft_ = cl_;
    begin(HttpTokenType::Body);
    st_ = St::Body;
}

void HttpLexer::message() {
    simple(HttpTokenType::MessageEnd);
    st_ = St::LineStart;
    haveCl_ = false;
    cl_ = 0;
    chunked_ = false;
}
