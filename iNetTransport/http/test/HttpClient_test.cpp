// Host test for the HTTP client's pure logic: HttpLexer in Response mode
// (status lines, Content-Length, chunked and to-close bodies, 1xx, HEAD),
// HttpResponseReader, HttpUrl and the request head. Responses are fed
// whole, a byte at a time and in random pieces.
//
//   g++ -std=c++14 -Wall -Wextra -Iinc test/HttpClient_test.cpp src/*.cpp -o HttpClient_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "HttpClientProtocol.h"
#include "HttpLexer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Every token, split ones joined, as text.
struct Collect : HttpTokenSink {
    std::string s;
    bool joining = false;
    HttpTokenType last = HttpTokenType::Error;
    bool put(const HttpToken& t) override {
        static const char* names[] = {"M", "T", "V", "N", "H", "E", "B", "X", "!", "S", "R"};
        if (!(joining && last == t.type)) {
            if (!s.empty()) s += ' ';
            s += names[static_cast<int>(t.type)];
            if (t.type == HttpTokenType::HeadersEnd || t.type == HttpTokenType::Error ||
                t.type == HttpTokenType::Status) {
                s += t.num == HttpToken::kChunked ? std::string("chunked")
                     : t.num == HttpToken::kToClose ? std::string("close") : std::to_string(t.num);
            }
            if (t.len || t.type == HttpTokenType::Reason) s += "(";
        } else {
            s.pop_back();   // reopen the parenthesis
        }
        if (t.len || t.type == HttpTokenType::Reason) s += std::string(t.text, t.len) + ")";
        joining = t.more;
        last = t.type;
        return true;
    }
};

static std::string lexResponse(const std::string& in, size_t chunk, bool head = false, bool close = false) {
    HttpLexer lx(HttpLexer::Mode::Response);
    if (head) lx.expectNoBody();
    Collect c;
    size_t at = 0;
    while (at < in.size()) {
        size_t n = chunk ? chunk : 1 + static_cast<size_t>(std::rand() % 17);
        if (n > in.size() - at) n = in.size() - at;
        at += lx.feed(reinterpret_cast<const uint8_t*>(in.data()) + at, n, c);
    }
    if (close) {
        lx.endOfInput();
        lx.feed(nullptr, 0, c);
    }
    return c.s;
}

// The same in every split.
static bool lexesAs(const std::string& in, const std::string& want, bool head = false, bool close = false) {
    bool ok = lexResponse(in, 100000, head, close) == want && lexResponse(in, 1, head, close) == want;
    for (int i = 0; i < 30 && ok; ++i) ok = lexResponse(in, 0, head, close) == want;
    if (!ok) std::printf("    got  %s\n    want %s\n", lexResponse(in, 1, head, close).c_str(), want.c_str());
    return ok;
}

struct Reader {
    HttpLexer lx{HttpLexer::Mode::Response};
    HttpResponseReader r;
    uint8_t buf[64];
    std::vector<std::string> headers;
    std::string streamed;
    size_t stopAfter = 0;   // onBody: stop after this many bytes (0: never)

    static void onHeader(const char* n, const char* v, void* ctx) {
        static_cast<Reader*>(ctx)->headers.push_back(std::string(n) + "=" + v);
    }
    static bool onBody(const uint8_t* d, size_t n, void* ctx) {
        Reader& me = *static_cast<Reader*>(ctx);
        me.streamed.append(reinterpret_cast<const char*>(d), n);
        return me.stopAfter == 0 || me.streamed.size() < me.stopAfter;
    }
    void run(const std::string& in, bool stream = false, bool close = false) {
        r.begin(buf, sizeof buf, stream ? onBody : nullptr, onHeader, this);
        size_t at = 0;
        while (at < in.size()) {
            size_t n = 1 + static_cast<size_t>(std::rand() % 13);
            if (n > in.size() - at) n = in.size() - at;
            at += lx.feed(reinterpret_cast<const uint8_t*>(in.data()) + at, n, r);
        }
        if (close) { lx.endOfInput(); lx.feed(nullptr, 0, r); }
    }
};

struct Out : HttpOutput {
    std::string s;
    bool write(const void* d, size_t n) override { s.append(static_cast<const char*>(d), n); return true; }
};

int main() {
    std::srand(7);
    std::printf("response lexing\n");
    {
        check(lexesAs("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nServer: x\r\n\r\nhello",
                      "V(HTTP/1.1) S200 R(OK) N(Content-Length) H(5) N(Server) H(x) E5 B(hello) X"),
              "Content-Length");
        const std::string big(26, 'z');
        check(lexesAs("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n5;name=val\r\nhello\r\n1A\r\n" + big +
                          "\r\n0\r\nX-Trailer: y\r\n\r\n",
                      "V(HTTP/1.1) S200 R(OK) N(Transfer-Encoding) H(Chunked) Echunked B(hello" + big + ") X"),
              "chunked: extensions and trailers skipped, chunks joined");
        check(lexesAs("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 99\r\n\r\n2\r\nok\r\n0\r\n\r\n",
                      "V(HTTP/1.1) S200 R(OK) N(Transfer-Encoding) H(chunked) N(Content-Length) H(99) Echunked B(ok) X"),
              "chunked wins over Content-Length");
        check(lexesAs("HTTP/1.0 200 OK\r\n\r\nuntil the end", "V(HTTP/1.0) S200 R(OK) Eclose B(until the end) X", false,
                      true),
              "no length: the body runs to the close");
        check(lexesAs("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
                      "V(HTTP/1.1) S100 R(Continue) E0 X V(HTTP/1.1) S201 R(Created) N(Content-Length) H(2) E2 B(ok) X"),
              "100 Continue first: a message of its own");
        check(lexesAs("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\nHTTP/1.1 204 No Content\r\nContent-Length: 3\r\n\r\n"
                      "HTTP/1.1 304 Not Modified\r\n\r\n",
                      "V(HTTP/1.1) S200 R(OK) N(Content-Length) H(1000) E0 X V(HTTP/1.1) S204 R(No Content) "
                      "N(Content-Length) H(3) E0 X V(HTTP/1.1) S304 R(Not Modified) E0 X",
                      true),
              "no body after HEAD, nor for 204 and 304");
        check(lexesAs("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n", "V(HTTP/1.1) S200 R() N(Content-Length) H(0) E0 X"),
              "no reason phrase");
        check(lexesAs("HTTP/1.1 200 OK\nContent-Length: 1\n\na", "V(HTTP/1.1) S200 R(OK) N(Content-Length) H(1) E1 B(a) X"),
              "bare LF line ends");

        struct Bad { std::string in; bool close; const char* what; } bad[] = {
            {"HTTP/1.1 20 OK\r\n", false, "a two-digit status"},
            {"HTTP/1.1 2000 OK\r\n", false, "a four-digit status"},
            {"HTTP/1.1 099 OK\r\n", false, "a status under 100"},
            {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n", false, "a coding other than chunked"},
            {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n", false, "gzip, then chunked"},
            {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", false, "a bad chunk size"},
            {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nokX", false, "a chunk without its CRLF"},
            {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFF\r\n", false, "a chunk size over 2^31"},
            {"HTTP/1.1 200 OK\r\nContent-Le", true, "closed mid-headers"},
            {"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc", true, "closed mid-body"},
            {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n", true, "closed before the last chunk"},
        };
        for (auto& b : bad) {
            const std::string got = lexResponse(b.in, 1, false, b.close);
            const bool ok = got.size() >= 4 && got.compare(got.size() - 4, 4, "!502") == 0;
            if (!ok) std::printf("    %s\n", got.c_str());
            check(ok, b.what);
        }
    }

    std::printf("response reader\n");
    {
        Reader rd;
        rd.run("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"ok\":true}");
        check(rd.r.done() && rd.r.status() == 200 && rd.r.bodyLength() == 11 && !rd.r.truncated() &&
                  std::strcmp(reinterpret_cast<char*>(rd.buf), "{\"ok\":true}") == 0,
              "status and body, NUL-terminated");
        check(rd.headers.size() == 2 && rd.headers[0] == "Content-Type=application/json", "headers to the callback");
        check(rd.r.keepAlive() && rd.r.contentLength() == 11, "HTTP/1.1: keep-alive");

        rd.headers.clear();
        rd.run("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        check(rd.r.done() && rd.r.status() == 201 && !rd.r.keepAlive(), "1xx passed over; Connection: close");

        const std::string big(200, 'q');
        rd.run("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nC8\r\n" + big + "\r\n0\r\n\r\n");
        check(rd.r.done() && rd.r.bodyLength() == 200 && rd.r.truncated() &&
                  std::memcmp(rd.buf, big.data(), sizeof rd.buf) == 0,
              "a body bigger than the buffer: the start kept, the rest read and dropped");
        check(rd.r.keepAlive(), "chunked keeps the connection");

        rd.run("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
        check(rd.r.done() && !rd.r.keepAlive(), "HTTP/1.0: close");
        rd.run("HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 2\r\n\r\nok");
        check(rd.r.done() && rd.r.keepAlive(), "HTTP/1.0 with keep-alive");
        rd.lx.reset();
        rd.run("HTTP/1.1 200 OK\r\n\r\nto the close", false, true);
        check(rd.r.done() && !rd.r.keepAlive() && std::strcmp(reinterpret_cast<char*>(rd.buf), "to the close") == 0,
              "a body to the close: done, connection not kept");

        rd.lx.reset();
        rd.run("HTTP/1.1 200 OK\r\nContent-Length: 300\r\n\r\n" + std::string(300, 's'), true);
        check(rd.r.done() && rd.streamed == std::string(300, 's'), "streamed to onBody, any size");
        rd.streamed.clear();
        rd.stopAfter = 50;
        rd.lx.reset();
        rd.run("HTTP/1.1 200 OK\r\nContent-Length: 300\r\n\r\n" + std::string(300, 's'), true);
        check(rd.r.aborted() && rd.streamed.size() < 300, "onBody returning false stops it");
        rd.stopAfter = 0;

        rd.lx.reset();
        rd.run("HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n");
        check(rd.r.failed(), "HTTP/2.0: malformed");
        rd.lx.reset();
        rd.run("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");
        check(rd.r.failed() && rd.r.started(), "a lexer error fails it");
        rd.lx.reset();
        rd.r.begin(rd.buf, sizeof rd.buf, nullptr, nullptr, nullptr);
        check(!rd.r.started() && !rd.r.done(), "nothing yet: not started");

        rd.lx.reset();
        rd.headers.clear();
        rd.run("HTTP/1.1 200 OK\r\nX-Long: " + std::string(300, 'v') + "\r\nContent-Length: 0\r\n\r\n");
        check(rd.r.done() && rd.headers.size() == 2 && rd.headers[0].size() == 7 + HttpResponseReader::kValue - 1,
              "a long header value is cut for the callback");
    }

    std::printf("URLs\n");
    {
        HttpUrl u;
        check(HttpUrl::parse("http://example.com", u) && std::string(u.host, u.hostLen) == "example.com" &&
                  u.port == 80 && std::string(u.path, u.pathLen) == "/" && !u.https,
              "http://host");
        check(HttpUrl::parse("HTTP://192.168.1.10:8080/api/readings?node=3#top", u) &&
                  std::string(u.host, u.hostLen) == "192.168.1.10" && u.port == 8080 &&
                  std::string(u.path, u.pathLen) == "/api/readings?node=3",
              "address, port, path and query; no fragment");
        check(HttpUrl::parse("https://x.io/a", u) && u.https && u.port == 443, "https: port 443");
        check(HttpUrl::parse("http://x.io?q=1", u) && std::string(u.path, u.pathLen) == "?q=1", "a query with no path");
        check(HttpUrl::parse("http://x.io/a@b", u), "@ in the path is fine");
        const char* bad[] = {"ftp://x", "http://", "http://:80/", "http://x:0/", "http://x:65536/", "http://x:8a/",
                             "http://x/a b", "http://user:pw@x/", "x.io/a", "http://x\t/"};
        bool all = true;
        for (const char* b : bad) {
            const bool rejected = !HttpUrl::parse(b, u);
            if (!rejected) std::printf("    accepted %s\n", b);
            all &= rejected;
        }
        check(all, "bad URLs rejected");
        check(!HttpUrl::parse(nullptr, u), "nullptr");
    }

    std::printf("request head\n");
    {
        Out o;
        httpWriteRequestHead(o, HttpMethod::Get, "x.io", "/a?b=1", 6, nullptr, -1, nullptr, "node/1", true);
        check(o.s == "GET /a?b=1 HTTP/1.1\r\nHost: x.io\r\nUser-Agent: node/1\r\n\r\n", "GET");
        o.s.clear();
        httpWriteRequestHead(o, HttpMethod::Post, "x.io:8080", "?q", 2, "application/json", 11, "X-Key: k\r\n", nullptr,
                             false);
        check(o.s == "POST /?q HTTP/1.1\r\nHost: x.io:8080\r\nConnection: close\r\nContent-Type: application/json\r\n"
                     "Content-Length: 11\r\nX-Key: k\r\n\r\n",
              "POST: close, type, length, extra headers; a bare query gets its /");
        o.s.clear();
        httpWriteRequestHead(o, HttpMethod::Post, "x", "/", 1, nullptr, -1, nullptr, nullptr, true);
        check(o.s == "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "a POST with no body says length 0");
        check(!httpWriteRequestHead(o, HttpMethod::Other, "x", "/", 1, nullptr, -1, nullptr, nullptr, true),
              "no method: refused");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
