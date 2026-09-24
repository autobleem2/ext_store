//
// StorePictures: an App's icon and a game's cover, from the installed folder, our covers databases and the
// PlayStation rdb, or the item's own URL - never from libretro's servers.
//
#include "doctest/doctest.h"

#include "support/rdb_builder.h"
#include "support/temp_dir.h"
#include "core/main.h"
#include "../src/store_pictures.h"

#include <ableem/engine/game_database.h>

#include <chrono>
#include <fstream>
#include <map>
#include <thread>
#include <vector>

using namespace std;
using namespace test_support;

namespace {

const string Png = "\x89PNG";

string fileText(const string &path) {
    ifstream in(path, ios::binary);
    return string((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
}

// covers<region>.db with the real schema and one game whose cover is four bytes of "PNG"
void makeCoversDb(const TempDir &tmp, const string &serial, const string &title) {
    ableem::GameDatabase db;
    REQUIRE(db.open(tmp.at("db/coversU.db")));
    REQUIRE(db.execute("CREATE TABLE GAME (ID INTEGER NOT NULL UNIQUE, TITLE TEXT NOT NULL, PUBLISHER TEXT NOT NULL, "
                       "RELEASE INTEGER NOT NULL, PLAYERS INTEGER NOT NULL, COVER BLOB, PRIMARY KEY(ID))",
                       "create GAME"));
    REQUIRE(db.execute("CREATE TABLE SERIALS (SERIAL TEXT NOT NULL, GAME INTEGER NOT NULL, PRIMARY KEY(SERIAL))",
                       "create SERIALS"));
    string insert = "INSERT INTO GAME VALUES (1, '" + title + "', 'Publisher', 1996, 1, X'89504E47')";
    REQUIRE(db.execute(insert.c_str(), "insert GAME"));
    insert = "INSERT INTO SERIALS VALUES ('" + serial + "', 1)";
    REQUIRE(db.execute(insert.c_str(), "insert SERIALS"));
}

struct Setup {
    Setup() : tmp("pictures") {
        tmp.makeSubDir("db");
        tmp.makeSubDir("cache");
        makeCoversDb(tmp, "SCUS-94900", "Crash Bandicoot");
        Bytes records;
        appendGameRecord(records, "Crash Bandicoot (USA)", "SCUS-94900", "USA", "Sony", 1996, 1);
        records.push_back(0xc0);
        rdb = writeRdb(tmp, "Sony - PlayStation.rdb", makeRdb(0, records));
    }
    StorePictures::Config config() {
        StorePictures::Config c;
        c.cacheDir = tmp.at("cache");
        c.fetchCommand = "get %u %o";
        c.coversDir = tmp.at("db");
        c.rdbFile = rdb;
        c.localThumbnails = false;
        c.runner = [this](const string &line, const function<bool()> &) {
            fetches.push_back(line);
            size_t a = line.find(' '), b = line.find(' ', a + 1);
            const string url = line.substr(a + 1, b - a - 1), out = line.substr(b + 1);
            auto it = bodies.find(url);
            if (it == bodies.end())
                return 22;
            ofstream(out, ios::binary) << it->second;
            return 0;
        };
        return c;
    }
    StorePictures::Request game(const string &title, const string &serial = "") {
        StorePictures::Request r;
        r.key = "src|ps1/" + title;
        r.kind = "ps1";
        r.title = title;
        r.serial = serial;
        return r;
    }
    TempDir tmp;
    string rdb;
    map<string, string> bodies;
    vector<string> fetches;
};

} // namespace

TEST_CASE("StorePictures: a game's cover from the covers database, by its serial") {
    Setup s;
    StorePictures pictures(s.config());
    const string file = pictures.resolve(s.game("Anything", "SCUS-94900"));
    REQUIRE_FALSE(file.empty());
    CHECK(file.find(s.tmp.at("cache")) == 0);
    CHECK(fileText(file) == Png);
    CHECK(s.fetches.empty());
}

TEST_CASE("StorePictures: a game without a serial, known to the rdb by name - the serial it gives finds the cover") {
    Setup s;
    StorePictures pictures(s.config());
    const string file = pictures.resolve(s.game("Crash Bandicoot (USA)"));
    REQUIRE_FALSE(file.empty());
    CHECK(fileText(file) == Png);
}

TEST_CASE("StorePictures: the installed game's own cover first; the source's picture only for an unknown game") {
    Setup s;
    StorePictures pictures(s.config());
    s.tmp.makeSubDir("Games/Crash");
    s.tmp.writeFile("Games/Crash/Crash.png", "mine");
    StorePictures::Request installed = s.game("Crash Bandicoot (USA)");
    installed.installedPath = s.tmp.at("Games/Crash");
    CHECK(fileText(pictures.resolve(installed)) == "mine");

    StorePictures::Request known = s.game("Anything", "SCUS-94900");
    known.imageUrl = "https://site/cover.jpg";
    s.bodies["https://site/cover.jpg"] = "theirs";
    CHECK(fileText(pictures.resolve(known)) == Png); // our database wins over the source's picture
    CHECK(s.fetches.empty());

    StorePictures::Request unknown = s.game("Homebrew Thing");
    unknown.imageUrl = "https://site/cover.jpg";
    CHECK(fileText(pictures.resolve(unknown)) == "theirs");
    CHECK(pictures.resolve(s.game("Nobody Knows")).empty());
}

TEST_CASE("StorePictures: an App's icon - its app.ini's Image= once installed, else the catalog's, fetched once") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request app;
    app.key = "AutoBleem|app/tyrian";
    app.kind = "app";
    app.title = "OpenTyrian";
    app.imageUrl = "https://site/store/psc/tyrian.png";
    s.bodies[app.imageUrl] = "icon";
    const string fetched = pictures.resolve(app);
    CHECK(fileText(fetched) == "icon");
    CHECK(pictures.resolve(app) == fetched);
    CHECK(s.fetches.size() == 1); // the cache answers the second time

    s.tmp.makeSubDir("Apps/tyrian");
    s.tmp.writeFile("Apps/tyrian/app.ini", "[app]\nTitle=OpenTyrian\nImage=tyrian.png\n");
    s.tmp.writeFile("Apps/tyrian/tyrian.png", "local");
    app.installedPath = s.tmp.at("Apps/tyrian");
    CHECK(fileText(pictures.resolve(app)) == "local");
}

TEST_CASE("StorePictures: asked for on the screen's thread, found on its own") {
    Setup s;
    StorePictures pictures(s.config());
    pictures.start();
    const uint64_t before = pictures.generation();
    StorePictures::Request r = s.game("Anything", "SCUS-94900");
    pictures.want(r);
    pictures.want(r); // asking every frame is fine
    for (int i = 0; i < 500 && pictures.path(r.key).empty(); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(r.key)) == Png);
    CHECK(pictures.generation() > before);

    // a refresh brings another picture for the same item: asked for again, not kept from before
    StorePictures::Request app;
    app.key = "src|app/x";
    app.kind = "app";
    app.imageUrl = "https://site/a.png";
    s.bodies["https://site/a.png"] = "first";
    s.bodies["https://site/b.png"] = "second";
    pictures.want(app);
    for (int i = 0; i < 500 && pictures.path(app.key).empty(); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(app.key)) == "first");
    app.imageUrl = "https://site/b.png";
    pictures.want(app);
    for (int i = 0; i < 500 && fileText(pictures.path(app.key)) != "second"; i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(app.key)) == "second");
    pictures.stop();
}
