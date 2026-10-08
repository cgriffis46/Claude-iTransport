// Host test for HttpStaticFiles with HttpMemoryFiles and HttpStdioFiles
// (a real folder, through FILE*), through the real lexer and connection
// state machine: index.html, types, gzip, a fallback, HEAD, 404, and
// paths that try to leave the folder.
//
//   g++ -std=c++14 -Wall -Wextra -Iinc test/HttpFiles_test.cpp src/*.cpp -o HttpFiles_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "HttpConnection.h"
#include "HttpFiles.h"
#include "HttpLexer.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct Out : HttpOutput {
    std::string data;
    bool write(const void* d, size_t n) override { data.append(static_cast<const char*>(d), n); return true; }
};
struct Sink : HttpTokenSink {
    std::deque<HttpToken> q;
    bool put(const HttpToken& t) override { q.push_back(t); return true; }
};

struct Web {
    HttpRoutes routes;
    uint8_t arena[1024];
    Out out;
    HttpConnection conn{routes, arena, sizeof arena, out};
    HttpLexer lx;
    Sink sink;
    std::string request(const std::string& line, const std::string& headers = "") {
        out.data.clear();
        conn.reset();
        lx.reset();
        const std::string req = line + " HTTP/1.1\r\nHost: x\r\n" + headers + "\r\n";
        lx.feed(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sink);
        while (!sink.q.empty()) { conn.onToken(sink.q.front()); sink.q.pop_front(); }
        return out.data;
    }
    static std::string body(const std::string& r) {
        const size_t at = r.find("\r\n\r\n");
        return at == std::string::npos ? "" : r.substr(at + 4);
    }
};

static void writeFile(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

int main() {
    std::printf("memory files\n");
    {
        static const char kIndex[] = "<h1>built in</h1>";
        static const char kCss[] = "body{}";
        static const uint8_t kGz[] = {0x1f, 0x8b, 8, 0, 1, 2, 3};
        const HttpMemoryFiles::File files[] = {
            {"/index.html", reinterpret_cast<const uint8_t*>(kIndex), sizeof kIndex - 1},
            {"/app.css", reinterpret_cast<const uint8_t*>(kCss), sizeof kCss - 1},
            {"/app.js.gz", kGz, sizeof kGz},
            {"/app.js", reinterpret_cast<const uint8_t*>("x()"), 3},
        };
        HttpMemoryFiles mem(files, 4);
        HttpStaticFiles site(mem);
        Web w;
        w.routes.on(HttpMethod::Get, "/*", HttpStaticFiles::handler, &site);

        std::string r = w.request("GET /");
        check(has(r, "200 OK") && has(r, "Content-Type: text/html; charset=utf-8\r\n") &&
                  has(r, "Content-Length: 17\r\n") && has(r, "Cache-Control: no-cache\r\n") &&
                  Web::body(r) == kIndex,
              "/ is index.html, typed, sized, not cached stale");
        r = w.request("GET /app.css");
        check(has(r, "text/css") && Web::body(r) == kCss, "a stylesheet");
        r = w.request("GET /app.js", "Accept-Encoding: br, gzip;q=0.8\r\n");
        check(has(r, "Content-Encoding: gzip\r\n") && has(r, "text/javascript") &&
                  Web::body(r) == std::string(reinterpret_cast<const char*>(kGz), sizeof kGz),
              "gzip taken: the .gz, typed by the original name");
        r = w.request("GET /app.js");
        check(!has(r, "Content-Encoding") && Web::body(r) == "x()", "gzip not taken: the plain file");
        r = w.request("HEAD /app.css");
        check(has(r, "Content-Length: 6\r\n") && Web::body(r).empty(), "HEAD: length, no body");
        check(has(w.request("GET /nope.html"), "404 Not Found"), "missing: 404");
    }

    std::printf("files in a folder, through FILE*\n");
    {
        char dir[] = "/tmp/httpfilesXXXXXX";
        check(mkdtemp(dir) != nullptr, "a folder");
        const std::string root = dir;
        mkdir((root + "/www").c_str(), 0755);
        mkdir((root + "/www/sub").c_str(), 0755);
        writeFile(root + "/www/index.html", "<h1>from the card</h1>");
        writeFile(root + "/www/sub/index.html", "sub");
        std::string big(70000, 0);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + (i >> 8));
        writeFile(root + "/www/data.bin", big);
        writeFile(root + "/secret.txt", "secret");

        static const char kFallback[] = "fallback page";
        static const char kOnly[] = "only built in";
        const HttpMemoryFiles::File files[] = {
            {"/index.html", reinterpret_cast<const uint8_t*>(kFallback), sizeof kFallback - 1},
            {"/only.txt", reinterpret_cast<const uint8_t*>(kOnly), sizeof kOnly - 1},
        };
        HttpMemoryFiles mem(files, 2);
        const std::string wwwRoot = root + "/www";
        HttpStdioFiles disk(wwwRoot.c_str());
        HttpStaticFiles site(disk, &mem);
        Web w;
        w.routes.on(HttpMethod::Get, "/*", HttpStaticFiles::handler, &site);

        check(Web::body(w.request("GET /")) == "<h1>from the card</h1>", "the folder's index.html, over the built-in one");
        check(Web::body(w.request("GET /sub/")) == "sub", "a sub-folder's index.html");
        check(Web::body(w.request("GET /only.txt")) == kOnly, "not in the folder: the fallback's");
        const std::string r = w.request("GET /data.bin");
        check(has(r, "application/octet-stream") && Web::body(r) == big, "70 KB, every byte");
        writeFile(root + "/www/index.html", "<h1>changed</h1>");
        check(Web::body(w.request("GET /")) == "<h1>changed</h1>", "a changed file shows at once: no firmware change");

        const char* attacks[] = {"/../secret.txt", "/sub/../../secret.txt", "/%2e%2e/secret.txt", "/..", "/a\\b"};
        bool all = true;
        for (const char* a : attacks) {
            const std::string resp = w.request(std::string("GET ") + a);
            if (!has(resp, "404") || has(resp, "secret")) { std::printf("    %s served\n", a); all = false; }
        }
        check(all, "nothing outside the folder (.., encoded .., backslash)");
        check(HttpStaticFiles::safePath("/a..b/c..") && HttpStaticFiles::safePath("/a/b.c.d"), "names with dots are fine");
        writeFile(root + "/www/.secret", "hidden");
        writeFile(root + "/www/sub/.index.html.part", "half an upload");
        check(!HttpStaticFiles::safePath("/..a") && !HttpStaticFiles::safePath("/sub/.x") &&
                  has(w.request("GET /.secret"), "404") && has(w.request("GET /sub/.index.html.part"), "404"),
              "hidden files (a name starting with '.', such as an upload in progress): 404");
        {
            static const uint8_t only3[] = "third";
            static const HttpMemoryFiles::File third[] = {{"/third.txt", only3, 5}, {"/index.html", only3, 5}};
            HttpMemoryFiles mem3(third, 2);
            HttpStaticFiles three(disk, &mem, &mem3);
            Web w3;
            w3.routes.on(HttpMethod::Get, "/*", HttpStaticFiles::handler, &three);
            check(Web::body(w3.request("GET /third.txt")) == "third" && Web::body(w3.request("GET /only.txt")) == kOnly &&
                      Web::body(w3.request("GET /")) == "<h1>changed</h1>",
                  "three sources, tried in order");
        }
        check(has(w.request("GET /missing"), "404"), "missing everywhere: 404");
        check(has(w.request("GET /sub"), "404"), "a folder without its slash: 404");

        std::string cmd = "rm -rf " + root;
        check(std::system(cmd.c_str()) == 0, "cleaned up");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
