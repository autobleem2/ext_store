//
// HttpServer - see the header.
//
#include "http_server.h"

#include <ableem/engine/log.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLength = int;
#define AB_CLOSE_SOCKET closesocket
#define AB_SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLength = socklen_t;
#define AB_CLOSE_SOCKET close
#define AB_SEND_FLAGS MSG_NOSIGNAL
#endif

using namespace std;

namespace {

const int MaxConnections = 32;
const size_t MaxRequestHead = 16 * 1024;

bool sendAll(intptr_t socket, const char *data, size_t length) {
    while (length > 0) {
        const int sent = static_cast<int>(
            send(static_cast<int>(socket), data, static_cast<int>(min<size_t>(length, 1 << 20)), AB_SEND_FLAGS));
        if (sent <= 0)
            return false;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

string lower(string s) {
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

string trim(const string &s) {
    const size_t a = s.find_first_not_of(" \t");
    const size_t b = s.find_last_not_of(" \t\r");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}

} // namespace

//*******************************
// HttpServer::Response helpers
//*******************************
HttpServer::Response HttpServer::Response::text(int status, const string &text) {
    Response r;
    r.status = status;
    r.body = text;
    return r;
}

HttpServer::Response HttpServer::Response::redirect(const string &location) {
    Response r;
    r.status = 303;
    r.headers["Location"] = location;
    return r;
}

string HttpServer::reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 303:
        return "See Other";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 416:
        return "Range Not Satisfiable";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

//*******************************
// HttpServer::decodePercent / parseRange
//*******************************
string HttpServer::decodePercent(const string &s) {
    string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool HttpServer::parseRange(const string &header, uint64_t size, uint64_t &first, uint64_t &last, bool &unsatisfiable) {
    unsatisfiable = false;
    const string h = trim(header);
    if (h.compare(0, 6, "bytes=") != 0 || h.find(',') != string::npos || size == 0)
        return false;
    const string spec = h.substr(6);
    const size_t dash = spec.find('-');
    if (dash == string::npos)
        return false;
    const string a = trim(spec.substr(0, dash)), b = trim(spec.substr(dash + 1));
    auto number = [](const string &s, uint64_t &v) {
        if (s.empty() || s.find_first_not_of("0123456789") != string::npos)
            return false;
        v = stoull(s);
        return true;
    };
    uint64_t x = 0, y = 0;
    if (a.empty()) { // the last y bytes
        if (!number(b, y) || y == 0)
            return false;
        first = y >= size ? 0 : size - y;
        last = size - 1;
        return true;
    }
    if (!number(a, x))
        return false;
    if (x >= size) {
        unsatisfiable = true;
        return false;
    }
    first = x;
    last = size - 1;
    if (!b.empty()) {
        if (!number(b, y) || y < x)
            return false;
        last = min(y, size - 1);
    }
    return true;
}

//*******************************
// HttpServer::listen / serve
//*******************************
HttpServer::~HttpServer() {
    if (listener_ >= 0)
        AB_CLOSE_SOCKET(static_cast<int>(listener_));
#ifdef _WIN32
    WSACleanup();
#endif
}

bool HttpServer::listen(int port, string &error) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }
#endif
    const int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (s < 0) {
        error = "no socket";
        return false;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(s, 16) != 0) {
        error = "port " + to_string(port) + " is in use or not allowed";
        AB_CLOSE_SOCKET(s);
        return false;
    }
    listener_ = s;
    return true;
}

void HttpServer::serve(const atomic<bool> &stop) {
    while (!stop) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(static_cast<int>(listener_), &ready);
        timeval wait{0, 500000};
        if (select(static_cast<int>(listener_) + 1, &ready, nullptr, nullptr, &wait) <= 0)
            continue;
        sockaddr_in from{};
        SocketLength length = sizeof(from);
        const int client =
            static_cast<int>(accept(static_cast<int>(listener_), reinterpret_cast<sockaddr *>(&from), &length));
        if (client < 0)
            continue;
        char peer[64] = {0};
        inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
        if (connections_ >= MaxConnections) {
            const string busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, busy.data(), busy.size());
            AB_CLOSE_SOCKET(client);
            continue;
        }
        connections_++;
        thread([this, client, peerName = string(peer)] {
            handle(client, peerName);
            AB_CLOSE_SOCKET(client);
            connections_--;
        }).detach();
    }
}

//*******************************
// HttpServer::handle
//*******************************
void HttpServer::handle(intptr_t socket, const string &peer) {
    // the head: up to the blank line
    string head;
    char buffer[4096];
    while (head.find("\r\n\r\n") == string::npos && head.size() < MaxRequestHead) {
        const int got = static_cast<int>(recv(static_cast<int>(socket), buffer, sizeof(buffer), 0));
        if (got <= 0)
            return;
        head.append(buffer, static_cast<size_t>(got));
    }
    Request request;
    request.peer = peer;
    istringstream lines(head);
    string line;
    getline(lines, line);
    {
        istringstream first(trim(line));
        string target, version;
        first >> request.method >> target >> version;
        const size_t q = target.find('?');
        request.path = decodePercent(target.substr(0, q));
        request.query = q == string::npos ? "" : target.substr(q + 1);
    }
    while (getline(lines, line)) {
        const size_t colon = line.find(':');
        if (colon != string::npos)
            request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    Response response;
    if (request.method != "GET" && request.method != "HEAD")
        response = Response::text(405, "GET and HEAD only\n");
    else if (request.path.empty() || request.path[0] != '/')
        response = Response::text(400, "bad request\n");
    else
        response = handler_(request);
    const bool body = request.method == "GET";

    ostringstream out;
    if (!response.file.empty()) {
        ifstream in(response.file, ios::binary | ios::ate);
        if (!in) {
            response = Response::text(404, "not found\n");
        } else {
            const uint64_t size = static_cast<uint64_t>(in.tellg());
            uint64_t first = 0, last = size == 0 ? 0 : size - 1;
            bool unsatisfiable = false;
            auto range = request.headers.find("range");
            const bool partial =
                range != request.headers.end() && parseRange(range->second, size, first, last, unsatisfiable);
            if (unsatisfiable) {
                out << "HTTP/1.1 416 " << reason(416) << "\r\nContent-Range: bytes */" << size
                    << "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                const string text = out.str();
                sendAll(socket, text.data(), text.size());
                return;
            }
            const uint64_t length = size == 0 ? 0 : last - first + 1;
            out << "HTTP/1.1 " << (partial ? 206 : 200) << ' ' << reason(partial ? 206 : 200) << "\r\n"
                << "Content-Type: " << response.contentType << "\r\nAccept-Ranges: bytes\r\n"
                << "Content-Length: " << length << "\r\n";
            if (partial)
                out << "Content-Range: bytes " << first << '-' << last << '/' << size << "\r\n";
            for (const auto &h : response.headers)
                out << h.first << ": " << h.second << "\r\n";
            out << "Connection: close\r\n\r\n";
            const string text = out.str();
            if (!sendAll(socket, text.data(), text.size()) || !body || length == 0)
                return;
            in.seekg(static_cast<streamoff>(first));
            vector<char> chunk(1 << 20);
            uint64_t left = length;
            while (left > 0 && in) {
                in.read(chunk.data(), static_cast<streamsize>(min<uint64_t>(left, chunk.size())));
                const streamsize got = in.gcount();
                if (got <= 0 || !sendAll(socket, chunk.data(), static_cast<size_t>(got)))
                    return; // the client went (a pause or a cancel in the Store)
                left -= static_cast<uint64_t>(got);
            }
            return;
        }
    }
    out << "HTTP/1.1 " << response.status << ' ' << reason(response.status) << "\r\n"
        << "Content-Type: " << response.contentType << "\r\nContent-Length: " << response.body.size() << "\r\n";
    for (const auto &h : response.headers)
        out << h.first << ": " << h.second << "\r\n";
    out << "Connection: close\r\n\r\n";
    if (body)
        out << response.body;
    const string text = out.str();
    sendAll(socket, text.data(), text.size());
}
