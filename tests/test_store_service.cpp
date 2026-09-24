//
// StoreService: the sources read, the queue worked - download, install, record - and paused, cancelled,
// failed, offline. The "site" is a fake runner serving files from a map; the installers are the real ones.
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"
#include "core/main.h"
#include "../src/store_service.h"

#include <ableem/engine/sha256.h>
#include <ableem/engine/zip_writer.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

using namespace std;

namespace {

string fileText(const string &path) {
    ifstream in(path, ios::binary);
    return string((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
}

// "get <url> <out>" (fresh) and "dl <url> <out>" (continues what <out> holds) - from a map of bodies. A URL
// in `stalls` writes half its body and then waits to be cancelled, as a download over a slow line would.
struct FakeSite {
    map<string, string> bodies;
    set<string> stalls;
    mutex m;
    vector<string> lines;

    StoreService::Runner runner() {
        return [this](const string &line, const function<bool()> &cancelled) {
            {
                lock_guard<mutex> lock(m);
                lines.push_back(line);
            }
            size_t a = line.find(' '), b = line.find(' ', a + 1);
            const string verb = line.substr(0, a), url = line.substr(a + 1, b - a - 1), out = line.substr(b + 1);
            string body;
            bool stall = false;
            {
                lock_guard<mutex> lock(m);
                auto it = bodies.find(url);
                if (it == bodies.end())
                    return 22;
                body = it->second;
                stall = stalls.count(url) > 0;
            }
            size_t from = 0;
            if (verb == "dl") {
                long long have = DirEntry::fileSize(out);
                from = have > 0 ? static_cast<size_t>(have) : 0;
            }
            string rest = body.substr(min(from, body.size()));
            ofstream o(out, verb == "dl" ? ios::binary | ios::app : ios::binary | ios::trunc);
            if (stall) {
                o << rest.substr(0, rest.size() / 2);
                o.close();
                while (!cancelled())
                    this_thread::sleep_for(chrono::milliseconds(10));
                return -2;
            }
            o << rest;
            return 0;
        };
    }
};

template <class Condition> bool waitFor(Condition condition, int ms = 8000) {
    for (int i = 0; i < ms / 10; i++) {
        if (condition())
            return true;
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    return condition();
}

string zipBytes(const map<string, string> &entries, const TempDir &tmp, const string &name) {
    ableem::ZipWriter w;
    REQUIRE(w.open(tmp.at(name)));
    for (const auto &e : entries)
        REQUIRE(w.addBytes(e.first, e.second));
    REQUIRE(w.close());
    return fileText(tmp.at(name));
}

struct Setup {
    Setup() : tmp("store") {
        tmp.makeSubDir("Apps");
        tmp.makeSubDir("Games");
        tmp.makeSubDir("System/Extensions/store/sources");
        tyrian = zipBytes({{"Apps/opentyrian/app.ini", "[app]\nTitle=OpenTyrian\nVersion=2.1\nExec=bin/{key}/tyrian\n"},
                           {"Apps/opentyrian/bin/psc/tyrian", "binary"}},
                          tmp, "tyrian.zip");
        site.bodies["https://site/store/psc/catalog.json"] = catalog("2.1");
        site.bodies["https://site/tyrian.zip"] = tyrian;
    }
    string catalog(const string &version) const {
        return R"({"schema": 1, "platform": "psc", "items": [
            {"id": "app/opentyrian", "kind": "app", "title": "OpenTyrian", "version": ")" +
               version + R"(",
             "files": [{"url": "https://site/tyrian.zip", "size": )" +
               to_string(tyrian.size()) + R"(, "sha256": ")" + ableem::Sha256::ofString(tyrian) + R"("}]},
            {"id": "theme/x", "kind": "theme", "title": "A Theme", "files": [{"url": "https://site/t.zip"}]}]})";
    }
    StoreService::Config config(bool online = true) {
        StoreService::Config c;
        c.stateDir = tmp.at("System/Extensions/store");
        c.appsDir = tmp.at("Apps");
        c.gamesDir = tmp.at("Games");
        c.catalogUrl = "https://site/store/psc/catalog.json";
        c.fetchCommand = "get %u %o";
        c.downloadCommand = "dl %u %o";
        c.platformKeys = {"psc"};
        c.runner = site.runner();
        c.networkUp = [online] { return online; };
        return c;
    }
    const StoreEntry *entry(const vector<StoreEntry> &entries, const string &key) {
        for (const StoreEntry &e : entries)
            if (e.key == key)
                return &e;
        return nullptr;
    }
    TempDir tmp;
    FakeSite site;
    string tyrian;
};

} // namespace

TEST_CASE("StoreService reads our catalog and the user's sources, local and remote") {
    Setup s;
    s.tmp.writeFile("System/Extensions/store/sources/mine.tsv",
                    "# name: My Games\ntitle\turl\tsize\nSome Game\thttps://site/d1.chd\t4\nbroken line\n");
    s.tmp.writeFile("System/Extensions/store/sources.txt", "# a remote one\nhttps://acme/list.tsv\n");
    s.site.bodies["https://acme/list.tsv"] = "# name: Acme\nAcme Game\thttps://acme/a.pbp\n";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));

    vector<StoreSourceInfo> sources = store.sources();
    REQUIRE(sources.size() == 3);
    CHECK(sources[0].ours);
    CHECK(sources[0].items == 2);
    CHECK(sources[1].name == "My Games");
    CHECK(sources[1].problems.size() == 1);
    CHECK(sources[2].name == "Acme");
    CHECK(sources[2].remote);

    vector<StoreEntry> entries = store.entries();
    REQUIRE(entries.size() == 4);
    CHECK(s.entry(entries, "AutoBleem|app/opentyrian")->state == StoreState::Available);
    CHECK(s.entry(entries, "AutoBleem|theme/x")->state == StoreState::Unsupported);
    CHECK(s.entry(entries, "My Games|ps1/Some Game") != nullptr);
    CHECK(s.entry(entries, "Acme|ps1/Acme Game") != nullptr);
    CHECK(store.poll().listChanged);
}

TEST_CASE("StoreService installs an App, remembers it, offers an update, removes it") {
    Setup s;
    const string key = "AutoBleem|app/opentyrian";
    {
        StoreService store(s.config());
        store.start();
        REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
        REQUIRE(store.enqueue(key));
        CHECK_FALSE(store.enqueue(key)); // once
        REQUIRE(waitFor([&] { return s.entry(store.entries(), key)->state == StoreState::Installed; }));
        CHECK(fileText(s.tmp.at("Apps/opentyrian/bin/psc/tyrian")) == "binary");
        StoreService::Update u = store.poll();
        CHECK(u.appsChanged);
        REQUIRE(u.installed.size() == 1);
        CHECK(u.installed[0] == "OpenTyrian");
        CHECK(DirEntry::diru(store.downloadsDir()).empty());
        CHECK(s.tmp.readFile("System/Extensions/store/queue.txt").empty());
    }
    // a new start remembers it; the catalog offering a newer version makes it an update
    s.site.bodies["https://site/store/psc/catalog.json"] = s.catalog("2.2");
    StoreService again(s.config());
    again.start();
    REQUIRE(waitFor([&] { return again.sourcesLoaded() && !again.readingSources(); }));
    const StoreEntry *e = s.entry(again.entries(), key);
    CHECK(e->state == StoreState::UpdateAvailable);
    CHECK(e->installedVersion == "2.1");

    string error;
    REQUIRE(again.remove(key, error));
    CHECK_FALSE(DirEntry::exists(s.tmp.at("Apps/opentyrian")));
    CHECK(s.entry(again.entries(), key)->state == StoreState::Available);
    CHECK_FALSE(again.remove(key, error));
}

TEST_CASE("StoreService installs a two-disc game from a TSV source into one folder") {
    Setup s;
    s.tmp.writeFile("System/Extensions/store/sources/g.tsv",
                    "# name: G\nkind\ttitle\turl\tdisc\nps1\tTwo Discs\thttps://g/d2.chd\t2\nps1\tTwo "
                    "Discs\thttps://g/d1.chd\t1\n");
    s.site.bodies["https://g/d1.chd"] = "disc one";
    s.site.bodies["https://g/d2.chd"] = "disc two";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    REQUIRE(store.enqueue("G|ps1/Two Discs"));
    REQUIRE(waitFor([&] { return s.entry(store.entries(), "G|ps1/Two Discs")->state == StoreState::Installed; }));
    CHECK(fileText(s.tmp.at("Games/Two Discs/d1.chd")) == "disc one");
    CHECK(fileText(s.tmp.at("Games/Two Discs/d2.chd")) == "disc two");
    CHECK(store.poll().gamesChanged);
}

TEST_CASE("StoreService: a download that does not match fails, says why, and leaves the queue") {
    Setup s;
    s.site.bodies["https://site/tyrian.zip"] = "tampered";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    REQUIRE(store.enqueue("AutoBleem|app/opentyrian"));
    REQUIRE(waitFor([&] { return s.entry(store.entries(), "AutoBleem|app/opentyrian")->state == StoreState::Failed; }));
    CHECK_FALSE(s.entry(store.entries(), "AutoBleem|app/opentyrian")->error.empty());
    StoreService::Update u = store.poll();
    CHECK(u.failed.size() == 1);
    CHECK_FALSE(DirEntry::exists(s.tmp.at("Apps/opentyrian")));
}

TEST_CASE("StoreService: paused mid-download keeps the bytes and the place in the queue, and resumes") {
    Setup s;
    s.site.stalls.insert("https://site/tyrian.zip");
    const string key = "AutoBleem|app/opentyrian";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    REQUIRE(store.enqueue(key));
    REQUIRE(waitFor([&] { return store.progress().busy && store.progress().done > 0; }));
    store.pause(); // a game is starting
    REQUIRE(waitFor([&] { return !store.progress().busy; }));
    CHECK(DirEntry::fileSize(store.downloadsDir() + "/tyrian.zip.part") == static_cast<long long>(s.tyrian.size() / 2));
    CHECK(s.tmp.readFile("System/Extensions/store/queue.txt") == key + "\n");
    CHECK(s.entry(store.entries(), key)->state == StoreState::Queued);

    {
        lock_guard<mutex> lock(s.site.m);
        s.site.stalls.clear();
    }
    store.resume();
    REQUIRE(waitFor([&] { return s.entry(store.entries(), key)->state == StoreState::Installed; }));
    CHECK(fileText(s.tmp.at("Apps/opentyrian/bin/psc/tyrian")) == "binary"); // the halves joined right
}

TEST_CASE("StoreService: a stop (power off) mid-download leaves it queued for the next start") {
    Setup s;
    s.site.stalls.insert("https://site/tyrian.zip");
    const string key = "AutoBleem|app/opentyrian";
    {
        StoreService store(s.config());
        store.start();
        REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
        REQUIRE(store.enqueue(key));
        REQUIRE(waitFor([&] { return store.progress().done > 0; }));
        auto before = chrono::steady_clock::now();
        store.stop();
        // not the stalled download's end (never): the threads run at idle priority, so a busy machine may
        // take a few seconds to schedule them
        CHECK(chrono::steady_clock::now() - before < chrono::seconds(10));
    }
    s.site.stalls.clear();
    StoreService next(s.config());
    next.start();
    REQUIRE(waitFor([&] {
        return s.entry(next.entries(), key) != nullptr && s.entry(next.entries(), key)->state == StoreState::Installed;
    }));
}

TEST_CASE("StoreService: cancel stops the download in flight and drops it") {
    Setup s;
    s.site.stalls.insert("https://site/tyrian.zip");
    const string key = "AutoBleem|app/opentyrian";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    REQUIRE(store.enqueue(key));
    REQUIRE(waitFor([&] { return store.progress().done > 0; }));
    REQUIRE(store.cancel(key));
    REQUIRE(waitFor([&] { return !store.progress().busy; }));
    CHECK(s.entry(store.entries(), key)->state == StoreState::Available);
    CHECK_FALSE(DirEntry::exists(store.downloadsDir() + "/tyrian.zip.part"));
    CHECK(s.tmp.readFile("System/Extensions/store/queue.txt").empty());
}

TEST_CASE("StoreService: a cancel mid-way through a two-disc game drops the disc already finished too") {
    Setup s;
    s.tmp.writeFile("System/Extensions/store/sources/two.tsv", "# name: Two\ntitle\turl\tdisc\n"
                                                               "Two Discs\thttps://site/d1.chd\t1\n"
                                                               "Two Discs\thttps://site/d2.chd\t2\n");
    s.site.bodies["https://site/d1.chd"] = "disc one";
    s.site.bodies["https://site/d2.chd"] = "disc two, slowly";
    s.site.stalls.insert("https://site/d2.chd");
    const string key = "Two|ps1/Two Discs";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    REQUIRE(store.enqueue(key));
    REQUIRE(waitFor([&] { return DirEntry::exists(store.downloadsDir() + "/d2.chd.part"); }));
    REQUIRE(store.cancel(key));
    REQUIRE(waitFor([&] { return !store.progress().busy; }));
    CHECK(s.entry(store.entries(), key)->state == StoreState::Available);
    CHECK_FALSE(DirEntry::exists(store.downloadsDir() + "/d1.chd"));
    CHECK_FALSE(DirEntry::exists(store.downloadsDir() + "/d2.chd.part"));
}

TEST_CASE("StoreService offline: the cached catalog, and nothing downloaded until there is a network") {
    Setup s;
    {
        StoreService online(s.config(true));
        online.start();
        REQUIRE(waitFor([&] { return online.sourcesLoaded() && !online.readingSources(); }));
    }
    StoreService store(s.config(false));
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    CHECK(store.entries().size() == 2); // from the cache
    CHECK(store.progress().offline);
    REQUIRE(store.enqueue("AutoBleem|app/opentyrian"));
    this_thread::sleep_for(chrono::milliseconds(600));
    CHECK_FALSE(store.progress().busy);
    CHECK(s.entry(store.entries(), "AutoBleem|app/opentyrian")->state == StoreState::Queued);
}

TEST_CASE("StoreService lists a PSN package (.pkg) but never queues or fetches it") {
    Setup s;
    // the NoPayStation layout, made-up rows: nothing here points anywhere real
    s.tmp.writeFile("System/Extensions/store/sources/nps.tsv",
                    "Title ID\tRegion\tName\tPKG direct link\tFile Size\n"
                    "NPUJ00001\tJP\tPlaceholder\thttp://example.invalid/a.pkg\t1000\n");
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    const StoreEntry *e = s.entry(store.entries(), "nps|ps1/Placeholder");
    REQUIRE(e != nullptr);
    CHECK(e->state == StoreState::NotInstallable);
    CHECK_FALSE(store.enqueue(e->key));
    this_thread::sleep_for(chrono::milliseconds(400));
    lock_guard<mutex> lock(s.site.m);
    for (const string &line : s.site.lines)
        CHECK(line.find("example.invalid") == string::npos); // not one request for it
}

TEST_CASE("StoreService: a source added during a download is read at once, alone; a removed one goes at once") {
    Setup s;
    s.tmp.writeFile("System/Extensions/store/sources.txt", "https://acme/one.tsv\n");
    s.site.bodies["https://acme/one.tsv"] = "# name: One\nGame One\thttps://acme/1.chd\n";
    s.site.bodies["https://acme/two.tsv"] = "# name: Two\nGame Two\thttps://acme/2.chd\n";
    s.site.stalls.insert("https://site/tyrian.zip"); // a download that takes its time
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));
    REQUIRE(store.enqueue("AutoBleem|app/opentyrian"));
    REQUIRE(waitFor([&] { return store.progress().busy; }));
    size_t fetchesBefore;
    {
        lock_guard<mutex> lock(s.site.m);
        fetchesBefore = s.site.lines.size();
    }

    string error;
    REQUIRE(store.addSourceUrl("https://acme/two.tsv", error));
    // listed at once, as loading, then read - while the download is still going
    CHECK(store.sources().back().where == "https://acme/two.tsv");
    REQUIRE(waitFor([&] { return s.entry(store.entries(), "Two|ps1/Game Two") != nullptr; }));
    CHECK_FALSE(store.sources().back().loading);
    CHECK(store.progress().busy);
    {
        lock_guard<mutex> lock(s.site.m);
        // only the new source was fetched - not the catalog, not the other source
        REQUIRE(s.site.lines.size() == fetchesBefore + 1);
        CHECK(s.site.lines.back().find("https://acme/two.tsv") != string::npos);
    }

    REQUIRE(store.removeSourceUrl("https://acme/one.tsv"));
    CHECK(s.entry(store.entries(), "One|ps1/Game One") == nullptr); // no waiting for anything
    CHECK(s.entry(store.entries(), "Two|ps1/Game Two") != nullptr);
    store.cancel("AutoBleem|app/opentyrian");
}

TEST_CASE("StoreService opens on the cached copies at once, while a slow source is fetched afresh") {
    Setup s;
    s.tmp.makeSubDir("System/Extensions/store/cache");
    s.tmp.writeFile("System/Extensions/store/cache/catalog.json", s.catalog("2.0"));
    s.site.stalls.insert("https://site/store/psc/catalog.json"); // the site takes its time today
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded(); }, 1000));
    const StoreEntry *tyrian = s.entry(store.entries(), "AutoBleem|app/opentyrian");
    REQUIRE(tyrian != nullptr);
    CHECK(tyrian->item.version == "2.0"); // the cached copy
    CHECK(store.readingSources());        // and the fresh one on its way
    CHECK(store.sources().front().loading);

    store.refreshIfOlderThan(300); // no finished refresh yet: asks for one - still just the one flag
    CHECK(store.readingSources());
}

TEST_CASE("StoreService: a source renamed and moved - what came from it is still known") {
    Setup s;
    s.tmp.writeFile("System/Extensions/store/sources.txt", "https://acme/one.tsv\n");
    s.site.bodies["https://acme/one.tsv"] = "# name: One\nGame One\thttps://acme/1.chd\n";
    s.site.bodies["http://mirror/one.tsv"] = "# name: One\nGame One\thttps://acme/1.chd\n";
    StoreService store(s.config());
    store.start();
    REQUIRE(waitFor([&] { return store.sourcesLoaded() && !store.readingSources(); }));

    REQUIRE(store.renameSource("https://acme/one.tsv", "  Living\troom  "));
    CHECK(s.tmp.readFile("System/Extensions/store/sources.txt") == "https://acme/one.tsv\tLiving room\n");
    StoreSourceInfo one = store.sources().back();
    CHECK(one.displayName == "Living room");
    CHECK(one.name == "One"); // the list's own name, what its items are keyed by
    CHECK(s.entry(store.entries(), "One|ps1/Game One") != nullptr);

    string error;
    CHECK_FALSE(store.changeSourceUrl("https://acme/one.tsv", "http://mirror /one.tsv", error));
    REQUIRE(store.changeSourceUrl("https://acme/one.tsv", "http://mirror/one.tsv", error));
    CHECK(s.tmp.readFile("System/Extensions/store/sources.txt") == "http://mirror/one.tsv\tLiving room\n");
    REQUIRE(waitFor([&] { return !store.readingSources() && s.entry(store.entries(), "One|ps1/Game One"); }));
    CHECK(store.sources().back().where == "http://mirror/one.tsv");
    CHECK(store.sources().back().displayName == "Living room");

    REQUIRE(store.renameSource("http://mirror/one.tsv", "")); // the list's own name again
    CHECK(store.sources().back().displayName == "One");
    CHECK_FALSE(store.renameSource("https://nowhere/x.tsv", "x"));
}

TEST_CASE("StoreService: source URLs, and file names") {
    Setup s;
    StoreService store(s.config());
    string error;
    CHECK_FALSE(store.addSourceUrl("ftp://x/y.tsv", error));
    CHECK(store.addSourceUrl(" https://acme/list.tsv ", error));
    CHECK_FALSE(store.addSourceUrl("https://acme/list.tsv", error)); // once, and said so
    CHECK(error == "That source is in the list already");
    CHECK_FALSE(store.addSourceUrl("http://192.168.1.2:8 126/store.tsv", error)); // a blank: curl's "3"
    CHECK(error == "The address is not valid");
    CHECK_FALSE(store.addSourceUrl("http:///store.tsv", error)); // no server
    CHECK_FALSE(store.addSourceUrl("192.168.1.2/store.tsv", error));
    CHECK(error == "A source is an http:// or https:// address");
    CHECK(store.sourceUrls() == vector<string>{"https://acme/list.tsv"});
    CHECK(store.removeSourceUrl("https://acme/list.tsv"));
    CHECK(store.sourceUrls().empty());

    ableem::StoreFile f;
    f.url = "https://x/path/Some%20Game.chd?token=1";
    CHECK(StoreService::fileNameFor(f) == "Some Game.chd");
    f.name = "own.chd";
    CHECK(StoreService::fileNameFor(f) == "own.chd");
    CHECK(StoreService::versionDiffers("1.0", "1.1"));
    CHECK_FALSE(StoreService::versionDiffers("1.0", ""));
    CHECK_FALSE(StoreService::versionDiffers(" 1.0", "1.0 "));
}
