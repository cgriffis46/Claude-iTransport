#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "HttpConnection.h"
#include "HttpLexer.h"
#include "xClient.h"
#include "xNetInterface.h"

// An HTTP/1.1 server on any xNetInterface (xEthernet, xWifi), for
// FreeRTOS. The daemon thread (run()) listens and accepts. Each client
// that connects gets a thread of its own, created for it and deleted
// when the connection ends:
//
//     static void temp(const HttpRequest& req, HttpResponse& res, void*) {
//         res.printf ... or res.send("application/json", "{\"t\":21.5}");
//     }
//     static xHttpServer web(eth, xHttpServer::Config());
//     web.get("/temp", temp);
//     web.post("/led", led);
//     web.begin();
//     osThreadNew([](void* w) { static_cast<xHttpServer*>(w)->run(); }, &web, &attrs);
//
// In a client's thread, bytes from the socket go through HttpLexer,
// which cuts them into tokens on a FreeRTOS queue (the token stream).
// HttpConnection's state machine takes them off the queue, assembles
// each request, and calls its handler on that thread. Handlers may
// block: other clients have their own threads.
//
// Up to Config::maxClients at once, each holding one of the
// interface's sockets. Their buffers are allocated once, in begin(); only
// the threads' stacks come and go. A W5500 socket listens for one
// connection at a time, so the daemon listens again as soon as it has
// handed a client over. A client that connects in between is refused,
// as is one while every client slot is busy.
class xHttpServer {
public:
    struct Config {
        uint16_t    port = 80;
        uint8_t     maxClients = 2;
        size_t      requestBytes = 2048;     // per client: target, headers and body together
        uint8_t     tokenQueueDepth = 8;     // tokens of HttpToken::kText bytes
        uint16_t    stackWords = 768;        // a client thread's stack (3 KB on a Cortex-M)
        UBaseType_t priority = tskIDLE_PRIORITY + 2;
        uint32_t    idleTimeoutMs = 5000;    // a kept connection with no new request
        uint32_t    requestTimeoutMs = 10000;   // from a request's first byte to its last
        uint16_t    maxRequestsPerConnection = 100;
    };

    struct Stats {
        uint32_t accepted;       // connections
        uint32_t requests;       // answered
        uint32_t errors;         // of those, answered with an error by the server itself
        uint32_t timeouts;       // connections closed for idling or a slow request
        uint32_t spawnFailures;  // no thread could be created (answered 503)
        uint8_t  active;         // client threads now
    };

    xHttpServer(xNetInterface& net, const Config& cfg);
    ~xHttpServer();

    xHttpServer(const xHttpServer&) = delete;
    xHttpServer& operator=(const xHttpServer&) = delete;

    // Routes, before begin(). false: HttpRoutes::kMaxRoutes reached.
    bool on(HttpMethod method, const char* path, HttpHandler fn, void* ctx = nullptr) {
        return routes_.add(method, path, fn, ctx);
    }
    bool get(const char* path, HttpHandler fn, void* ctx = nullptr) { return on(HttpMethod::Get, path, fn, ctx); }
    bool post(const char* path, HttpHandler fn, void* ctx = nullptr) { return on(HttpMethod::Post, path, fn, ctx); }

    // Allocates every client's buffers. false: out of FreeRTOS heap.
    bool begin();

    // The daemon thread's body. Returns after stop(), once every client
    // thread has ended.
    void run();

    // Closes the listening socket and every connection; run() returns
    // within about a second.
    void stop();

    Stats stats() const;

private:
    // One client: its socket, its token queue, its state machines. The
    // daemon fills a free one in and hands it to a new thread.
    struct Client : HttpOutput, HttpTokenSink {
        xHttpServer*   server = nullptr;
        xClient        sock;
        QueueHandle_t  tokens = nullptr;
        uint8_t*       arena = nullptr;
        HttpLexer      lexer;
        HttpConnection conn;
        uint8_t        readBuf[256];
        std::atomic<bool> busy{false};

        Client(xHttpServer& s, xNetInterface& net, uint8_t* a, size_t size, uint16_t maxRequests)
            : server(&s), sock(net), arena(a), conn(s.routes_, a, size, *this, maxRequests) {}

        bool write(const void* data, size_t len) override;   // HttpOutput: to the socket
        bool put(const HttpToken& t) override;               // HttpTokenSink: onto the queue
        void serve();
        void drain();
    };

    static void clientThread(void* arg);
    Client* freeClient();
    void refuse(Client& c, uint16_t code);

    xNetInterface& net_;
    Config         cfg_;
    HttpRoutes     routes_;
    Client*        clients_ = nullptr;   // cfg_.maxClients of them
    SemaphoreHandle_t slots_ = nullptr;  // counts free clients
    std::atomic<bool> stopping_{false};

    std::atomic<uint32_t> stAccepted_{0}, stRequests_{0}, stErrors_{0}, stTimeouts_{0}, stSpawnFailures_{0};
    std::atomic<uint8_t>  active_{0};
};
