//
// HttpServer: the smallest HTTP/1.1 server abstored needs - GET and HEAD, one connection per request
// (Connection: close), a thread per connection, bytes from memory or a file with Range support (the Store
// resumes a stopped download with Range). Plain HTTP: it is meant for a home LAN, not the internet.
//
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

//******************
// HttpServer
//******************
class HttpServer {
public:
    struct Request {
        std::string method;                         // "GET", "HEAD"
        std::string path;                           // percent-decoded, without the query
        std::string query;                          // after '?', as sent
        std::map<std::string, std::string> headers; // names lower-cased
        std::string peer;                           // the client's address
    };
    struct Response {
        int status = 200;
        std::string contentType = "text/plain; charset=utf-8";
        std::string body;
        std::string file; // when set, the file is sent instead of body - Range honoured
        std::map<std::string, std::string> headers;
        static Response text(int status, const std::string &text);
        static Response redirect(const std::string &location);
    };
    using Handler = std::function<Response(const Request &)>;

    explicit HttpServer(Handler handler) : handler_(std::move(handler)) {}
    ~HttpServer();

    // bind and listen on every interface; false (and why) when the port is taken
    bool listen(int port, std::string &error);
    // accepts until stop is set (checked twice a second); each connection on a thread of its own
    void serve(const std::atomic<bool> &stop);

    // "bytes=a-b" / "bytes=a-" / "bytes=-n" against a file of `size`: false when there is no usable single
    // range (the whole file is sent); `unsatisfiable` when the start lies past the end (416)
    static bool parseRange(const std::string &header, uint64_t size, uint64_t &first, uint64_t &last,
                           bool &unsatisfiable);
    static std::string decodePercent(const std::string &s);
    static std::string reason(int status);

private:
    void handle(intptr_t socket, const std::string &peer);

    Handler handler_;
    intptr_t listener_ = -1;
    std::atomic<int> connections_{0};
};
