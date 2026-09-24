//
// abstored - the AutoBleem Store's LAN server: serves a folder of PS1 games to the Store on the same network.
//
//   abstored <games folder> [--port 8124] [--name "My games"] [--covers <dir with covers*.db>]
//            [--rdb "<Sony - PlayStation.rdb>"] [--state <dir>] [--no-checksums]
//
//   /           what is served, and the problems the scan found
//   /store.tsv  the source to add in the Store (Sources -> Add a source URL)
//   /files/...  the games' files (Range supported: a stopped download resumes)
//   /cover/...  a game's cover (a picture in its folder, else the covers database's by serial)
//   /rescan     scan the folder now (it is also scanned whenever it changes)
//
// It only reads the games folder. Checksums are cached in --state (default ~/.cache/abstored).
//
#include <ableem/lanserver/http_server.h>
#include <ableem/lanserver/index_page.h>
#include <ableem/lanserver/lan_library.h>

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace std;
using ableem::HttpServer;
using ableem::IndexPageFacts;
using ableem::LanFile;
using ableem::LanGame;
using ableem::LanLibrary;

namespace {

const char *const Version = "1.0";
atomic<bool> stopping{false};

void onSignal(int) {
    stopping = true;
}

string defaultStateDir() {
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    return local != nullptr ? string(local) + "/abstored" : "";
#else
    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache != nullptr && *cache != 0)
        return string(cache) + "/abstored";
    const char *home = getenv("HOME");
    return home != nullptr ? string(home) + "/.cache/abstored" : "";
#endif
}

int usage() {
    cerr << "usage: abstored <games folder> [--port 8124] [--name NAME] [--covers DIR] [--rdb FILE] [--state DIR]"
            " [--no-checksums]\n";
    return 2;
}

// sleeps in short steps so a stop is noticed at once
void pause(int seconds) {
    for (int i = 0; i < seconds * 10 && !stopping; i++)
        this_thread::sleep_for(chrono::milliseconds(100));
}

} // namespace

int main(int argc, char *argv[]) {
    ableem::Log::initConsoleOnly(plog::info);
    LanLibrary::Config config;
    int port = 8124;
    string name = "My games";
    bool checksums = true;
    config.stateDir = defaultStateDir();
    for (int i = 1; i < argc; i++) {
        const string a = argv[i];
        auto value = [&]() -> string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port")
            port = atoi(value().c_str());
        else if (a == "--name")
            name = value();
        else if (a == "--covers")
            config.coversDir = value();
        else if (a == "--rdb")
            config.rdbFile = value();
        else if (a == "--state")
            config.stateDir = value();
        else if (a == "--no-checksums")
            checksums = false;
        else if (a == "--version") {
            cout << "abstored " << Version << "\n";
            return 0;
        } else if (a == "--help" || a == "-h" || (!a.empty() && a[0] == '-') || !config.gamesDir.empty())
            return usage();
        else
            config.gamesDir = a;
    }
    if (config.gamesDir.empty() || port <= 0 || port > 65535)
        return usage();
    if (!ableem::DirEntry::isDirectory(config.gamesDir)) {
        cerr << "abstored: " << config.gamesDir << " is not a folder\n";
        return 1;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // a client that goes mid-download is not a reason to stop
#endif

    LanLibrary library(config);
    library.scan();
    atomic<bool> rescan{false};

    HttpServer server([&](const HttpServer::Request &request) {
        // every URL we hand out is built from the address the client used to reach us
        auto host = request.headers.find("host");
        const string baseUrl =
            "http://" + (host != request.headers.end() ? host->second : "localhost:" + to_string(port));
        const string &path = request.path;
        if (path == "/" || path == "/index.html") {
            IndexPageFacts facts;
            facts.name = name;
            facts.baseUrl = baseUrl;
            facts.gamesDir = config.gamesDir;
            facts.version = Version;
            const auto snap = library.snapshot();
            const auto sums = library.checksums();
            for (const LanGame &g : snap->games)
                for (const LanFile &f : g.files)
                    facts.hashing = facts.hashing || (checksums && !sums.count(LanLibrary::checksumKey(f)));
            HttpServer::Response r;
            r.contentType = "text/html; charset=utf-8";
            r.body = indexPage(*snap, sums, facts);
            return r;
        }
        if (path == "/store.tsv") {
            HttpServer::Response r;
            r.contentType = "text/tab-separated-values; charset=utf-8";
            r.body = LanLibrary::tsv(*library.snapshot(), library.checksums(), baseUrl, name);
            PLOG_INFO << request.peer << " read the list";
            return r;
        }
        if (path == "/rescan") {
            rescan = true;
            return HttpServer::Response::redirect("/");
        }
        if (path.compare(0, 7, "/files/") == 0) {
            const string file = library.servablePath(path.substr(7));
            if (file.empty())
                return HttpServer::Response::text(404, "not served here\n");
            PLOG_INFO << request.peer << " " << (request.headers.count("range") ? "resumes " : "fetches ")
                      << path.substr(7);
            HttpServer::Response r;
            r.contentType = "application/octet-stream";
            r.file = file;
            return r;
        }
        if (path.compare(0, 7, "/cover/") == 0) {
            HttpServer::Response r;
            if (!library.cover(path.substr(7), r.body, r.contentType))
                return HttpServer::Response::text(404, "no cover\n");
            return r;
        }
        return HttpServer::Response::text(404, "not found\n");
    });
    string error;
    if (!server.listen(port, error)) {
        cerr << "abstored: " << error << "\n";
        return 1;
    }
    PLOG_INFO << "abstored " << Version << " serving " << config.gamesDir << " on port " << port
              << " - the Store's source is http://<this machine's address>:" << port << "/store.tsv";

    // the folder watched: scanned again when anything in it changed (or /rescan asked)
    thread watcher([&] {
        string last = library.fingerprint();
        while (!stopping) {
            for (int i = 0; i < 100 && !stopping && !rescan; i++) // ten seconds, or at once on /rescan
                this_thread::sleep_for(chrono::milliseconds(100));
            const string now = library.fingerprint();
            if (now != last || rescan.exchange(false)) {
                last = now;
                library.scan();
            }
        }
    });
    // the checksums, a file at a time, behind everything else
    thread hasher([&] {
        while (!stopping && checksums) {
            library.hashPending([] { return stopping.load(); });
            pause(5);
        }
    });

    server.serve(stopping);
    watcher.join();
    hasher.join();
    PLOG_INFO << "abstored stopped";
    return 0;
}
