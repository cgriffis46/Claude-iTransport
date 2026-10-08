// Host test for the HTTP server's pure logic: HttpLexer (bytes to
// tokens), HttpConnection (tokens to requests, routing, responses),
// HttpRequest and HttpResponse. The tokens pass through a bounded queue,
// as they do through the FreeRTOS queue in xHttpServer, and the input is
// fed whole, a byte at a time, and in random pieces.
//
//   g++ -std=c++14 -Wall -Wextra -Iinc test/Http_test.cpp src/*.cpp -o Http_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "HttpConnection.h"
#include "HttpLexer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// A bounded token queue, as the FreeRTOS one.
struct Queue : HttpTokenSink {
    size_t depth;
    std::deque<HttpToken> q;
    explicit Queue(size_t d = 8) : depth(d) {}
    bool put(const HttpToken& t) override {
        if (q.size() >= depth) return false;
        q.push_back(t);
        return true;
    }
};

struct Tok {
    HttpTokenType type;
    std::string text;
    uint32_t num;
};

// Lexes input in pieces of `chunk` bytes (0: random pieces) through a
// queue of `depth`, joining split tokens back together.
static std::vector<Tok> lex(const std::string& input, size_t chunk = 0, size_t depth = 8) {
    HttpLexer lx;
    Queue q(depth);
    std::vector<Tok> out;
    bool joining = false;
    auto drain = [&] {
        while (!q.q.empty()) {
            const HttpToken t = q.q.front();
            q.q.pop_front();
            if (joining && out.back().type == t.type) out.back().text.append(t.text, t.len);
            else out.push_back(Tok{t.type, std::string(t.text, t.len), t.num});
            joining = t.more;
        }
    };
    size_t at = 0;
    while (at < input.size() || lx.pending()) {
        size_t n = chunk ? chunk : 1 + static_cast<size_t>(std::rand() % 23);
        if (n > input.size() - at) n = input.size() - at;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(input.data()) + at;
        at += lx.feed(p, n, q);
        drain();
    }
    return out;
}

static std::string describe(const std::vector<Tok>& v) {
    static const char* names[] = {"M", "T", "V", "N", "H", "E", "B", "X", "!"};
    std::string s;
    for (auto& t : v) {
        s += names[static_cast<int>(t.type)];
        if (t.type == HttpTokenType::HeadersEnd || t.type == HttpTokenType::Error) s += std::to_string(t.num);
        else if (!t.text.empty()) s += "(" + t.text + ")";
        s += ' ';
    }
    return s;
}

// ---- a server connection with an output that records ----

struct Out : HttpOutput {
    std::string data;
    bool broken = false;
    bool write(const void* d, size_t n) override {
        if (broken) return false;
        data.append(static_cast<const char*>(d), n);
        return true;
    }
};

struct Server {
    HttpRoutes routes;
    uint8_t arena[512];
    Out out;
    HttpConnection conn;
    HttpLexer lx;
    Queue q;
    explicit Server(uint16_t maxRequests = 100) : conn(routes, arena, sizeof arena, out, maxRequests) {}

    // As xHttpServer's worker: lex what arrived, then hand the tokens over.
    void feed(const std::string& in, size_t chunk = 0) {
        size_t at = 0;
        while ((at < in.size() || lx.pending()) && !conn.done()) {
            size_t n = chunk ? chunk : 1 + static_cast<size_t>(std::rand() % 31);
            if (n > in.size() - at) n = in.size() - at;
            at += lx.feed(reinterpret_cast<const uint8_t*>(in.data()) + at, n, q);
            while (!q.q.empty()) {
                const HttpToken t = q.q.front();
                q.q.pop_front();
                conn.onToken(t);
            }
        }
    }
    std::string take() { std::string s; s.swap(out.data); return s; }
};

static void hello(const HttpRequest& req, HttpResponse& res, void*) {
    res.header("X-Path", req.path()).send("text/plain", "hello");
}

static void echoForm(const HttpRequest& req, HttpResponse& res, void*) {
    char name[16], temp[16];
    const bool n = req.param("name", name, sizeof name);
    const bool t = req.param("temp", temp, sizeof temp);
    res.status(201);
    res.begin("text/plain");
    res.printf("name=%s(%d) temp=%s(%d) body=%u", name, n, temp, t, static_cast<unsigned>(req.bodyLength()));
    res.end();
}

static void stream(const HttpRequest&, HttpResponse& res, void*) {
    res.begin("text/html");
    res.print("<ul>");
    for (int i = 0; i < 3; ++i) res.printf("<li>%d</li>", i);
    res.print("</ul>");
    // end() is left to the connection
}

static void nothing(const HttpRequest&, HttpResponse&, void*) {}
static void shortBody(const HttpRequest&, HttpResponse& res, void*) {
    res.begin("text/plain", 10);
    res.write("abc", 3);
}

static const char kGet[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";

int main() {
    std::srand(1);
    std::printf("lexer\n");
    {
        const std::string req =
            "POST /a/b?x=1 HTTP/1.1\r\nHost: dev\r\nX-Pad:   two  words  \r\nEmpty:\r\nContent-Length: 5\r\n\r\nhello";
        const std::string want =
            "M(POST) T(/a/b?x=1) V(HTTP/1.1) N(Host) H(dev) N(X-Pad) H(two  words) N(Empty) H N(Content-Length) H(5) E5 "
            "B(hello) X ";
        check(describe(lex(req, 1000)) == want, "a request, whole");
        check(describe(lex(req, 1)) == want, "a byte at a time");
        bool same = true;
        for (int i = 0; i < 50; ++i) same &= describe(lex(req)) == want;
        check(same, "in random pieces");
        check(describe(lex(req, 1000, 1)) == want, "through a queue of one");

        std::string longTarget = "/" + std::string(200, 'p');
        auto v = lex("GET " + longTarget + " HTTP/1.1\r\n\r\n", 1000);
        check(v.size() == 5 && v[1].text == longTarget, "a long target comes in pieces, rejoined");
        {
            HttpLexer lx;
            Queue q(100);
            const std::string s = "GET " + longTarget + " HTTP/1.1\r\n\r\n";
            lx.feed(reinterpret_cast<const uint8_t*>(s.data()), s.size(), q);
            int pieces = 0;
            bool flags = true;
            for (auto& t : q.q) if (t.type == HttpTokenType::Target) { ++pieces; flags &= (pieces < 6) == t.more; }
            check(pieces == 6 && flags, "kText bytes per token, more on all but the last");
        }
        const std::string spaced = "GET / HTTP/1.1\r\nA: x" + std::string(100, ' ') + "y \r\n\r\n";
        v = lex(spaced, 7);
        check(v.size() == 7 && v[4].text == "x" + std::string(100, ' ') + "y", "a long run of spaces inside a value");

        check(describe(lex("\r\n\r\nGET / HTTP/1.0\nA: b\n\n", 3)) == "M(GET) T(/) V(HTTP/1.0) N(A) H(b) E0 X ",
              "empty lines first, and bare LF line ends");
        check(describe(lex(std::string(kGet) + "POST /p HTTP/1.1\r\nContent-Length: 2\r\n\r\nokGET /q HTTP/1.1\r\n\r\n")) ==
                  "M(GET) T(/) V(HTTP/1.1) N(Host) H(x) E0 X M(POST) T(/p) V(HTTP/1.1) N(Content-Length) H(2) E2 B(ok) X "
                  "M(GET) T(/q) V(HTTP/1.1) E0 X ",
              "pipelined requests, framed by Content-Length");
        check(describe(lex("GET / HTTP/1.1\r\nContent-Length: 3\r\ncontent-length: 3\r\n\r\nabc")).find("E3 B(abc) X") !=
                  std::string::npos,
              "Content-Length twice, the same, in any case");

        struct Bad { const char* in; const char* want; } bad[] = {
            {"G@T / HTTP/1.1\r\n", "!400"},
            {"GET /\r\n", "!400"},                               // HTTP/0.9
            {"GET  / HTTP/1.1\r\n", "!400"},
            {"GET / HTTP/1.1\r\nA : b\r\n", "!400"},             // space before the colon
            {"GET / HTTP/1.1\r\nA: b\r\n c\r\n", "!400"},        // line folding
            {"GET / HTTP/1.1\r\nA: b\x01\r\n", "!400"},
            {"GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n", "!501"},
            {"GET / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n", "!400"},
            {"GET / HTTP/1.1\r\nContent-Length: 1x\r\n", "!400"},
            {"GET / HTTP/1.1\r\nContent-Length: 1 2\r\n", "!400"},
            {"GET / HTTP/1.1\r\nContent-Length:\r\n", "!400"},
            {"GET / HTTP/1.1\r\nContent-Length: 99999999999\r\n", "!413"},
            {"GET / HTTP/1.1x\r\n", "!400"},
            {"GET / HTTP/1.1\rX", "!400"},
        };
        bool all = true;
        for (auto& b : bad) {
            const std::string d = describe(lex(b.in, 1));
            const bool ok = d.size() >= std::strlen(b.want) + 1 && d.compare(d.size() - std::strlen(b.want) - 1, std::strlen(b.want), b.want) == 0;
            if (!ok) std::printf("    %s -> %s\n", b.in, d.c_str());
            all &= ok;
        }
        check(all, "malformed requests end in an Error token with their status");
        check(describe(lex(std::string(41, 'A') + " / HTTP/1.1\r\n")) == "!501 ", "a method longer than a token: 501");
        v = lex("G@T / HTTP/1.1\r\n" + std::string(kGet));
        check(v.size() == 1, "nothing after an error");
    }

    std::printf("requests and responses\n");
    {
        Server s;
        s.routes.add(HttpMethod::Get, "/", hello);
        s.routes.add(HttpMethod::Get, "/api/*", hello);
        s.routes.add(HttpMethod::Post, "/api/form", echoForm);
        s.routes.add(HttpMethod::Get, "/stream", stream);
        s.routes.add(HttpMethod::Get, "/nothing", nothing);
        s.routes.add(HttpMethod::Get, "/short", shortBody);

        s.feed(kGet);
        std::string r = s.take();
        check(r == "HTTP/1.1 200 OK\r\nX-Path: /\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello",
              "GET: status, headers, length and body");
        check(!s.conn.done(), "HTTP/1.1 keeps the connection");

        s.feed("GET /api/a%20b/c?x=1 HTTP/1.1\r\nHost: x\r\n\r\n");
        check(has(s.take(), "X-Path: /api/a b/c\r\n"), "a prefix route; the path percent-decoded, without the query");

        s.feed("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
        r = s.take();
        check(has(r, "Content-Length: 5\r\n") && r.size() > 4 && r.compare(r.size() - 4, 4, "\r\n\r\n") == 0,
              "HEAD goes to the GET route: its headers, no body");

        s.feed("GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
        check(has(s.take(), "HTTP/1.1 404 Not Found\r\n") && !s.conn.done(), "404, connection kept");

        s.feed("DELETE /api/form HTTP/1.1\r\nHost: x\r\n\r\n");
        r = s.take();
        check(has(r, "HTTP/1.1 405 Method Not Allowed\r\n") && has(r, "Allow: GET, HEAD, POST\r\n"),
              "405 with Allow (from both routes the path matches)");

        const std::string form = "name=caf%C3%A9+bar&temp=21.5";
        s.feed("POST /api/form?name=q HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: " + std::to_string(form.size()) + "\r\n\r\n" + form);
        r = s.take();
        check(has(r, "HTTP/1.1 201 Created\r\n") && has(r, "Transfer-Encoding: chunked\r\n"), "201, streamed chunked");
        check(has(r, "name=q(1) temp=21.5(1) body=28"), "the query wins, the form fills in the rest");
        s.feed("POST /api/form HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: " + std::to_string(form.size()) + "\r\n\r\n" + form);
        check(has(s.take(), "name=caf\xC3\xA9 bar(1)"), "form values decoded, + as a space");

        s.feed("GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");
        r = s.take();
        check(has(r, "\r\n\r\n4\r\n<ul>\r\na\r\n<li>0</li>\r\na\r\n<li>1</li>\r\na\r\n<li>2</li>\r\n5\r\n</ul>\r\n0\r\n\r\n"),
              "chunks, and the end the handler left out");

        s.feed(std::string(kGet) + kGet + kGet, 1000);
        r = s.take();
        size_t n = 0;
        for (size_t at = 0; (at = r.find("HTTP/1.1 200", at)) != std::string::npos; ++at) ++n;
        check(n == 3, "three pipelined requests, three responses");
        check(s.conn.requests() == 11 && s.conn.errors() == 2, "counts requests and errors");

        s.feed("GET /nothing HTTP/1.1\r\nHost: x\r\n\r\n");
        r = s.take();
        check(has(r, "HTTP/1.1 500 ") && has(r, "Connection: close\r\n") && s.conn.done(),
              "a handler that answers nothing: 500, and close");
    }
    {
        Server s;
        s.routes.add(HttpMethod::Get, "/short", shortBody);
        s.feed("GET /short HTTP/1.1\r\nHost: x\r\n\r\n");
        check(has(s.take(), "Content-Length: 10\r\n") && s.conn.done(), "a body shorter than its length: close");
    }

    std::printf("HTTP/1.0 and Connection\n");
    {
        Server s;
        s.routes.add(HttpMethod::Get, "/", hello);
        s.routes.add(HttpMethod::Get, "/stream", stream);
        s.feed("GET / HTTP/1.0\r\n\r\n");
        check(has(s.take(), "Connection: close\r\n") && s.conn.done(), "HTTP/1.0: close (no Host needed)");
        s.conn.reset();
        s.lx.reset();
        s.feed("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
        check(has(s.take(), "Connection: keep-alive\r\n") && !s.conn.done(), "HTTP/1.0 keep-alive");
        s.feed("GET /stream HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        {
            const std::string r = s.take();
            check(!has(r, "chunked") && has(r, "Connection: close\r\n") && has(r, "<ul><li>0</li>") && s.conn.done(),
                  "HTTP/1.0 streamed: no chunks, ended by closing");
        }
        s.conn.reset();
        s.lx.reset();
        s.feed("GET / HTTP/1.1\r\nHost: x\r\nConnection: foo, close\r\n\r\n");
        check(has(s.take(), "Connection: close\r\n") && s.conn.done(), "Connection: close");
    }
    {
        Server s(2);
        s.routes.add(HttpMethod::Get, "/", hello);
        s.feed(kGet);
        check(!has(s.take(), "close") && !s.conn.done(), "request 1 of 2");
        s.feed(kGet);
        check(has(s.take(), "Connection: close\r\n") && s.conn.done(), "request 2 of 2: close");
    }

    std::printf("rejected requests\n");
    {
        struct Case { std::string in; const char* status; const char* what; } cases[] = {
            {"BREW / HTTP/1.1\r\nHost: x\r\n\r\n", "501 Not Implemented", "an unknown method: 501"},
            {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", "505 HTTP Version", "HTTP/2.0: 505"},
            {"GET / HTTP/1.1\r\n\r\n", "400 Bad Request", "HTTP/1.1 without Host: 400"},
            {"GET /" + std::string(600, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n", "414 URI Too Long", "a long target: 414"},
            {"GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + std::string(600, 'a') + "\r\n\r\n", "431 Request Header",
             "large headers: 431"},
            {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\n", "413 Content Too Large",
             "a large body: 413, before it arrives"},
            {"GET /a%00b HTTP/1.1\r\nHost: x\r\n\r\n", "400 Bad Request", "%00 in the path: 400"},
            {"GET / HTTP/1.1\r\nHost: x\r\nExpect: tea\r\n\r\n", "417 Expectation", "an unknown Expect: 417"},
            {"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", "501 Not Implemented",
             "a chunked request: 501"},
            {"GET foo HTTP/1.1\r\nHost: x\r\n\r\n", "400 Bad Request", "a target that isn't a path: 400"},
        };
        for (auto& c : cases) {
            Server s;
            s.routes.add(HttpMethod::Get, "/", hello);
            s.routes.add(HttpMethod::Post, "/", hello);
            s.feed(c.in);
            const std::string r = s.take();
            check(r.compare(0, 9 + std::strlen(c.status), std::string("HTTP/1.1 ") + c.status) == 0 &&
                      has(r, "Connection: close\r\n") && s.conn.done(),
                  c.what);
        }
    }

    std::printf("other cases\n");
    {
        Server s;
        s.routes.add(HttpMethod::Post, "/up", echoForm);
        s.routes.add(HttpMethod::Get, "/", hello);
        s.feed("POST /up HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n");
        check(s.take() == "HTTP/1.1 100 Continue\r\n\r\n", "Expect: 100-continue: 100 before the body");
        s.feed("abc");
        check(has(s.take(), "body=3"), "then the response");

        s.feed("GET http://dev.local/x?y=1 HTTP/1.1\r\nHost: dev.local\r\n\r\n");
        check(has(s.take(), "404"), "absolute-form target: its path routed");
        s.feed("GET http://dev.local HTTP/1.1\r\nHost: dev.local\r\n\r\n");
        check(has(s.take(), "200 OK"), "absolute-form with no path is /");

        s.feed("GET / HT");
        s.conn.timeout();
        check(has(s.take(), "HTTP/1.1 408 ") && s.conn.done(), "timed out mid-request: 408");
    }
    {
        Server s;
        s.conn.timeout();
        check(s.take().empty() && s.conn.done(), "timed out between requests: just close");
    }
    {
        Server s;
        s.routes.add(HttpMethod::Get, "/", hello);
        s.out.broken = true;
        s.feed(kGet);
        check(s.conn.done(), "the output failing: close");
    }
    {
        HttpRequest r;
        char out[12];
        check(HttpRequest::urlDecode("a%2Fb%zz%4", 10, out, sizeof out, false) == 8 && std::strcmp(out, "a/b%zz%4") == 0,
              "urlDecode: bad escapes stay as they are");
        check(HttpRequest::urlDecode("abcdefghijkl", 12, out, sizeof out, false) < 0 && out[0] == 0, "urlDecode: too long");
        (void)r;
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
