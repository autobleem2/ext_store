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
// It only reads the games folder. Checksums are cached in --state (default ~/.cache/abstored). The server
// itself - the routes, the watcher, the hasher - is core's ableem::LanServer, which pc-tools' LAN Share runs
// too; this is its command line.
//
#include <ableem/lanserver/lan_server.h>

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
using ableem::LanServer;

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

} // namespace

int main(int argc, char *argv[]) {
    ableem::Log::initConsoleOnly(plog::info);
    LanServer::Config config;
    config.library.stateDir = defaultStateDir();
    config.version = Version;
    for (int i = 1; i < argc; i++) {
        const string a = argv[i];
        auto value = [&]() -> string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port")
            config.port = atoi(value().c_str());
        else if (a == "--name")
            config.name = value();
        else if (a == "--covers")
            config.library.coversDir = value();
        else if (a == "--rdb")
            config.library.rdbFile = value();
        else if (a == "--state")
            config.library.stateDir = value();
        else if (a == "--no-checksums")
            config.checksums = false;
        else if (a == "--version") {
            cout << "abstored " << Version << "\n";
            return 0;
        } else if (a == "--help" || a == "-h" || (!a.empty() && a[0] == '-') || !config.library.gamesDir.empty())
            return usage();
        else
            config.library.gamesDir = a;
    }
    if (config.library.gamesDir.empty() || config.port <= 0 || config.port > 65535)
        return usage();
    if (!ableem::DirEntry::isDirectory(config.library.gamesDir)) {
        cerr << "abstored: " << config.library.gamesDir << " is not a folder\n";
        return 1;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // a client that goes mid-download is not a reason to stop
#endif

    LanServer server(config);
    string error;
    if (!server.start(error)) {
        cerr << "abstored: " << error << "\n";
        return 1;
    }
    PLOG_INFO << "abstored " << Version << " serving " << config.library.gamesDir << " on port " << config.port
              << " - the Store's source is http://<this machine's address>:" << config.port << "/store.tsv";
    while (!stopping)
        this_thread::sleep_for(chrono::milliseconds(200));
    server.stop();
    PLOG_INFO << "abstored stopped";
    return 0;
}
