//
// abstored, the Store's LAN server: the read-only scan of a games folder, the TSV it serves (read back with the
// Store's own parser), the checksum cache, and an HTTP round trip with a Range.
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"
#include "../server/src/http_server.h"
#include "../server/src/index_page.h"
#include "../server/src/lan_library.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/sha256.h>
#include <ableem/engine/store_catalog.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define TEST_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define TEST_CLOSE close
#endif

using namespace std;

namespace {

// every file under dir with its size - to prove the scan wrote nothing
string listing(const string &dir) {
    string out;
    for (const ableem::DirEntry &e : ableem::DirEntry::diru(dir)) {
        const string path = dir + "/" + e.name;
        out += path + (e.isDir ? "/\n" : "|" + to_string(ableem::DirEntry::fileSize(path)) + "\n");
        if (e.isDir)
            out += listing(path);
    }
    return out;
}

struct Games {
    Games() : tmp("lan") {
        tmp.makeSubDir("Games/Two Discs");
        tmp.writeFile("Games/Two Discs/Game (Disc 2).chd", "disc two");
        tmp.writeFile("Games/Two Discs/Game (Disc 1).chd", "disc one");
        tmp.writeFile("Games/Two Discs/cover.png", "png");
        tmp.makeSubDir("Games/Cue Game");
        tmp.writeFile("Games/Cue Game/game.cue", "FILE \"GAME.BIN\" BINARY\n  TRACK 01 MODE2/2352\n");
        tmp.writeFile("Games/Cue Game/game.bin", "binary data");
        tmp.makeSubDir("Games/Broken");
        tmp.writeFile("Games/Broken/b.cue", "FILE \"missing.bin\" BINARY\n");
        tmp.makeSubDir("Games/RPG/Nested");
        tmp.writeFile("Games/RPG/Nested/n.pbp", "pbp data");
        tmp.makeSubDir("Games/!SaveStates/Two Discs");
        tmp.writeFile("Games/!SaveStates/Two Discs/x.chd", "not a game");
        tmp.makeSubDir("Games/Empty");
        tmp.writeFile("Games/Empty/e.chd", "");
        tmp.writeFile("Games/loose.chd", "loose");
        tmp.makeSubDir("state");
    }
    LanLibrary::Config config() {
        LanLibrary::Config c;
        c.gamesDir = tmp.at("Games");
        c.stateDir = tmp.at("state");
        return c;
    }
    const LanGame *game(const LanSnapshot &s, const string &id) {
        for (const LanGame &g : s.games)
            if (g.id == id)
                return &g;
        return nullptr;
    }
    bool problem(const LanSnapshot &s, const string &path, const string &part) {
        for (const LanProblem &p : s.problems)
            if (p.path == path && p.what.find(part) != string::npos)
                return true;
        return false;
    }
    TempDir tmp;
};

} // namespace

TEST_CASE("abstored scans a games folder read-only: discs in order, cues with their files, problems said") {
    Games g;
    const string before = listing(g.tmp.at("Games"));
    LanLibrary library(g.config());
    library.scan();
    CHECK(listing(g.tmp.at("Games")) == before); // nothing written, renamed or repaired

    const auto snap = library.snapshot();
    REQUIRE(snap->games.size() == 3); // Two Discs, Cue Game, RPG/Nested
    const LanGame *two = g.game(*snap, "Two Discs");
    REQUIRE(two != nullptr);
    REQUIRE(two->files.size() == 2);
    CHECK(two->files[0].name == "Game (Disc 1).chd");
    CHECK(two->files[0].disc == 1);
    CHECK(two->files[1].disc == 2);
    CHECK(two->coverFile.find("cover.png") != string::npos);
    CHECK(two->title == "Two Discs"); // no Game.ini, no database: the folder's name

    const LanGame *cue = g.game(*snap, "Cue Game");
    REQUIRE(cue != nullptr);
    REQUIRE(cue->files.size() == 2);
    CHECK(cue->files[0].name == "game.cue");
    CHECK(cue->files[0].disc == 1);
    CHECK(cue->files[1].name == "game.bin"); // found whatever the case the cue names it in
    CHECK(cue->files[1].disc == 0);

    CHECK(g.game(*snap, "RPG/Nested") != nullptr);
    CHECK(g.game(*snap, "Broken") == nullptr);
    CHECK(g.game(*snap, "Empty") == nullptr);
    CHECK(g.problem(*snap, "Broken/b.cue", "missing.bin"));
    CHECK(g.problem(*snap, "Empty/e.chd", "empty"));
    CHECK(g.problem(*snap, "", "loose"));
    CHECK(g.problem(*snap, "Two Discs", "no serial")); // a warning: still served

    CHECK(library.servablePath("Two Discs/Game (Disc 1).chd") == g.tmp.at("Games") + "/Two Discs/Game (Disc 1).chd");
    CHECK(library.servablePath("!SaveStates/Two Discs/x.chd").empty());
    CHECK(library.servablePath("../outside.chd").empty());
    CHECK(library.servablePath("loose.chd").empty());
}

TEST_CASE("abstored's TSV is what the Store reads: one item per game, discs numbered, URLs encoded") {
    Games g;
    LanLibrary library(g.config());
    library.scan();
    const string tsv = LanLibrary::tsv(*library.snapshot(), {}, "http://10.0.0.5:8124", "Test Games");
    CHECK(tsv.find("http://10.0.0.5:8124/files/Two%20Discs/Game%20%28Disc%201%29.chd") != string::npos);

    ableem::StoreSourceTsv source = ableem::StoreSourceTsv::parse(tsv, "x");
    CHECK(source.name == "Test Games");
    CHECK(source.problems.empty());
    REQUIRE(source.items.size() == 3);
    const ableem::StoreItem *two = nullptr;
    for (const ableem::StoreItem &item : source.items)
        if (item.title == "Two Discs")
            two = &item;
    REQUIRE(two != nullptr);
    REQUIRE(two->files.size() == 2);
    CHECK(two->files[0].name == "Game (Disc 1).chd");
    CHECK(two->files[0].size == 8);
    CHECK(two->image == "http://10.0.0.5:8124/cover/Two%20Discs"); // it has a picture in its folder
}

TEST_CASE("abstored: two games of one title are told apart by their folders") {
    Games g;
    g.tmp.makeSubDir("Games/A");
    g.tmp.writeFile("Games/A/a.chd", "a");
    g.tmp.writeFile("Games/A/Game.ini", "[Game]\nTitle=Same Game\n");
    g.tmp.makeSubDir("Games/B");
    g.tmp.writeFile("Games/B/b.chd", "b");
    g.tmp.writeFile("Games/B/Game.ini", "[Game]\nTitle=Same Game\n");
    LanLibrary library(g.config());
    library.scan();
    const auto snap = library.snapshot();
    REQUIRE(g.game(*snap, "A") != nullptr);
    CHECK(g.game(*snap, "A")->title == "Same Game (A)");
    CHECK(g.game(*snap, "B")->title == "Same Game (B)");
}

TEST_CASE("abstored: a community disc without a serial is served under its own name, not reported") {
    Games g;
    g.tmp.makeSubDir("Games/RE1.5 (MZD)");
    g.tmp.writeFile("Games/RE1.5 (MZD)/BH2.cue", "FILE \"BH2.bin\" BINARY\n");
    g.tmp.writeFile("Games/RE1.5 (MZD)/BH2.bin", "no serial in here");
    g.tmp.writeFile("Games/RE1.5 (MZD)/Game.ini", "[Game]\nTitle=RE1.5 (MZD)\nSerial=\n");
    LanLibrary library(g.config());
    library.scan();
    const auto snap = library.snapshot();
    const LanGame *re = g.game(*snap, "RE1.5 (MZD)");
    REQUIRE(re != nullptr);
    CHECK(re->title == "Resident Evil 1.5");
    CHECK_FALSE(g.problem(*snap, "RE1.5 (MZD)", "no serial"));
}

TEST_CASE("abstored works the checksums out once and keeps them outside the games folder") {
    Games g;
    {
        LanLibrary library(g.config());
        library.scan();
        library.hashPending([] { return false; });
        const auto sums = library.checksums();
        CHECK(sums.size() == 5);
        CHECK(sums.at("Two Discs/Game (Disc 1).chd|8") == ableem::Sha256::ofString("disc one"));
        const string tsv = LanLibrary::tsv(*library.snapshot(), sums, "http://h", "n");
        CHECK(tsv.find(ableem::Sha256::ofString("disc one")) != string::npos);
    }
    CHECK(ableem::DirEntry::exists(g.tmp.at("state/checksums.tsv")));
    LanLibrary again(g.config());
    CHECK(again.checksums().size() == 5); // read back, nothing hashed again

    // a stop is honoured between files
    LanLibrary stopped(LanLibrary::Config{g.tmp.at("Games"), "", "", ""});
    stopped.scan();
    stopped.hashPending([] { return true; });
    CHECK(stopped.checksums().empty());
}

TEST_CASE("HttpServer::parseRange and decodePercent") {
    uint64_t first = 0, last = 0;
    bool unsatisfiable = false;
    REQUIRE(HttpServer::parseRange("bytes=10-", 100, first, last, unsatisfiable));
    CHECK((first == 10 && last == 99));
    REQUIRE(HttpServer::parseRange("bytes=10-19", 100, first, last, unsatisfiable));
    CHECK((first == 10 && last == 19));
    REQUIRE(HttpServer::parseRange("bytes=-30", 100, first, last, unsatisfiable));
    CHECK((first == 70 && last == 99));
    REQUIRE(HttpServer::parseRange("bytes=90-500", 100, first, last, unsatisfiable));
    CHECK(last == 99);
    CHECK_FALSE(HttpServer::parseRange("bytes=100-", 100, first, last, unsatisfiable));
    CHECK(unsatisfiable);
    CHECK_FALSE(HttpServer::parseRange("bytes=0-1,5-6", 100, first, last, unsatisfiable));
    CHECK_FALSE(unsatisfiable);
    CHECK_FALSE(HttpServer::parseRange("items=1-2", 100, first, last, unsatisfiable));
    CHECK(HttpServer::decodePercent("/files/Two%20Discs/A%28B%29.chd") == "/files/Two Discs/A(B).chd");
    CHECK(htmlEscape("<a & \"b\">") == "&lt;a &amp; &quot;b&quot;&gt;");
}

TEST_CASE("abstored over a socket: the list, a file resumed with a Range, a file it does not serve") {
    Games g;
    LanLibrary library(g.config());
    library.scan();
    HttpServer server([&](const HttpServer::Request &r) {
        if (r.path == "/store.tsv") {
            HttpServer::Response res;
            res.body = LanLibrary::tsv(*library.snapshot(), {}, "http://" + r.headers.at("host"), "n");
            return res;
        }
        if (r.path.compare(0, 7, "/files/") == 0 && !library.servablePath(r.path.substr(7)).empty()) {
            HttpServer::Response res;
            res.file = library.servablePath(r.path.substr(7));
            return res;
        }
        return HttpServer::Response::text(404, "no\n");
    });
    const int port = 20000 + static_cast<int>(chrono::steady_clock::now().time_since_epoch().count() % 20000);
    string error;
    REQUIRE(server.listen(port, error));
    atomic<bool> stop{false};
    thread serving([&] { server.serve(stop); });

    auto ask = [&](const string &request) {
        const int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        string reply;
        if (connect(s, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0) {
            send(s, request.data(), static_cast<int>(request.size()), 0);
            char buffer[4096];
            int got;
            while ((got = static_cast<int>(recv(s, buffer, sizeof(buffer), 0))) > 0)
                reply.append(buffer, static_cast<size_t>(got));
        }
        TEST_CLOSE(s);
        return reply;
    };
    const string list = ask("GET /store.tsv HTTP/1.1\r\nHost: 10.1.2.3:" + to_string(port) + "\r\n\r\n");
    CHECK(list.compare(0, 15, "HTTP/1.1 200 OK") == 0);
    CHECK(list.find("http://10.1.2.3:" + to_string(port) + "/files/Cue%20Game/game.bin") != string::npos);

    const string part =
        ask("GET /files/Cue%20Game/game.bin HTTP/1.1\r\nHost: h\r\nRange: bytes=7-\r\n\r\n"); // "binary data"
    CHECK(part.compare(0, 12, "HTTP/1.1 206") == 0);
    CHECK(part.find("Content-Range: bytes 7-10/11") != string::npos);
    CHECK(part.substr(part.size() - 4) == "data");

    CHECK(ask("GET /files/loose.chd HTTP/1.1\r\nHost: h\r\n\r\n").compare(0, 12, "HTTP/1.1 404") == 0);
    CHECK(ask("POST /store.tsv HTTP/1.1\r\nHost: h\r\n\r\n").compare(0, 12, "HTTP/1.1 405") == 0);
    stop = true;
    serving.join();
}
