//
// StorePictures: an App's icon and a game's cover, from the installed folder, our covers databases and the
// PlayStation rdb, or the item's own URL - never from libretro's servers.
//
#include "doctest/doctest.h"

#include "support/rdb_builder.h"
#include "support/temp_dir.h"
#include "core/main.h"
#include "../src/store_pictures.h"

#include <ableem/engine/crc32.h>
#include <ableem/engine/game_database.h>
#include <ableem/engine/md5.h>

#include <chrono>
#include <fstream>
#include <map>
#include <thread>
#include <vector>

using namespace std;
using namespace test_support;

namespace {

// a bare signature - fine for the tests that never reach validPicture() (a covers-database blob, iconImage()
// tested directly): those paths trust their source and never touch the texture loader through StorePictures.
const string Png = "\x89PNG";

string be32(uint32_t v) {
    return string({static_cast<char>((v >> 24) & 0xff), static_cast<char>((v >> 16) & 0xff),
                   static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)});
}

// one PNG chunk with a correct CRC-32 over its type and data, as validPng() (store_pictures.cpp) checks it
string pngChunk(const string &type, const string &data) {
    const uint32_t crc = ableem::Crc32::update(0, (type + data).data(), type.size() + data.size());
    return be32(static_cast<uint32_t>(data.size())) + type + data + be32(crc);
}

// a real, valid, tiny PNG every chunk CRC checks out on - what a fetched picture must be to be cached and
// shown now. `tag` goes into IDAT: our own validPng() only checks the chunk structure and CRCs, never decodes
// pixels, so any bytes there still let two pictures compare unequal in a test, exactly as the old bare "Png +
// tag" fixtures did before pictures were validated.
string realPng(const string &tag) {
    string ihdr(13, '\0');
    ihdr[3] = 1; // width = 1
    ihdr[7] = 1; // height = 1
    ihdr[8] = 8; // bit depth 8, colour type/compression/filter/interlace 0
    return string("\x89PNG\r\n\x1a\n", 8) + pngChunk("IHDR", ihdr) + pngChunk("IDAT", tag) + pngChunk("IEND", "");
}

// what the owner's install actually had cached: a PNG with the right signature and IHDR, but every chunk's
// CRC left at zero - SDL_image's libpng refuses it, our validPng() must too
string corruptPng() {
    string ihdr(13, '\0');
    ihdr[3] = 32;
    ihdr[7] = 32;
    ihdr[8] = 8;
    ihdr[9] = 2;
    auto zeroCrc = [](const string &type, const string &data) {
        return be32(static_cast<uint32_t>(data.size())) + type + data + be32(0);
    };
    return string("\x89PNG\r\n\x1a\n", 8) + zeroCrc("IHDR", ihdr) + zeroCrc("IDAT", string(29, '\0')) +
           zeroCrc("IEND", "");
}

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

TEST_CASE("StorePictures: a PSN Title ID finds the cover and the facts by the disc serial psn_serials.tsv names") {
    Setup s;
    // the list as shipped: named columns, a multi-disc game's serials " / "-separated, a Title ID with none
    s.tmp.writeFile("psn_serials.tsv", "Title ID\tPSN Name\tRegion\tMatch Type\tRedump Title\tSerial\n"
                                       "NPUJ00001\tCrash\tUS\texact\tCrash Bandicoot\tSCUS-94900 / SCUS-94901\n"
                                       "NPUJ00002\tNobody\tEU\tnone\t\t\n");
    StorePictures::Config c = s.config();
    c.psnSerialsFile = s.tmp.at("psn_serials.tsv");
    StorePictures pictures(c);
    CHECK(StorePictures::isPsnTitleId("NPUJ00001"));
    CHECK_FALSE(StorePictures::isPsnTitleId("SCUS-94900"));
    CHECK_FALSE(StorePictures::isPsnTitleId("NPUJ0001"));
    CHECK(pictures.discSerialFor("npuj00001") == "SCUS-94900"); // the first disc's
    CHECK(pictures.discSerialFor("NPUJ00002").empty());
    CHECK(pictures.discSerialFor("NPUJ99999").empty());

    StorePictures::GameFacts facts;
    const string file = pictures.resolve(s.game("A title nobody knows", "NPUJ00001"), &facts);
    REQUIRE_FALSE(file.empty());
    CHECK(fileText(file) == Png);
    CHECK(facts.serial == "SCUS-94900");
    CHECK(facts.publisher == "Sony");
    CHECK(facts.year == 1996);
    CHECK(facts.players == 1);
    CHECK(facts.region == "US"); // the PSN release's, from the list

    // a Title ID the list has no serial for: the title is tried, as before - its region is still the list's
    StorePictures::GameFacts none;
    CHECK(pictures.resolve(s.game("A title nobody knows", "NPUJ00002"), &none).empty());
    CHECK(none.serial.empty());
    CHECK(none.region == "EU");
    CHECK(pictures.resolve(s.game("Crash Bandicoot (USA)", "NPUJ00002"), &none) == file);
}

TEST_CASE("StorePictures: a disc's region follows its serial; region names become one code") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::GameFacts facts;
    pictures.resolve(s.game("Anything", "SCUS-94900"), &facts);
    CHECK(facts.region == "US");
    StorePictures::GameFacts pal;
    pictures.resolve(s.game("Some PAL game", "SLES-01234"), &pal); // unknown to the databases: the serial alone
    CHECK(pal.region == "EU");
    CHECK(StorePictures::regionCode("Europe-Aus") == "EU");
    CHECK(StorePictures::regionCode("Japan") == "JP");
    CHECK(StorePictures::regionCode(" jp ") == "JP");
    CHECK(StorePictures::regionCode("ASIA") == "ASIA");
    CHECK(StorePictures::regionCode("Mars").empty());
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
    known.imageUrl = "https://site/cover.png";
    s.bodies["https://site/cover.png"] = realPng("theirs");
    CHECK(fileText(pictures.resolve(known)) == Png); // our database wins over the source's picture
    CHECK(s.fetches.empty());

    StorePictures::Request unknown = s.game("Homebrew Thing");
    unknown.imageUrl = "https://site/cover.png";
    CHECK(fileText(pictures.resolve(unknown)) == realPng("theirs"));
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
    s.bodies[app.imageUrl] = realPng("icon");
    const string fetched = pictures.resolve(app);
    CHECK(fileText(fetched) == realPng("icon"));
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
    const uint64_t before = pictures.generation();
    StorePictures::Request r = s.game("Anything", "SCUS-94900");
    pictures.want(r);
    CHECK(pictures.pending(r.key)); // the screen shows its spinner meanwhile
    pictures.want(r);               // asking every frame is fine
    pictures.start();
    for (int i = 0; i < 500 && pictures.path(r.key).empty(); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(r.key)) == Png);
    CHECK_FALSE(pictures.pending(r.key));
    CHECK(pictures.generation() > before);

    // a refresh brings another picture for the same item: asked for again, not kept from before
    StorePictures::Request app;
    app.key = "src|app/x";
    app.kind = "app";
    app.imageUrl = "https://site/a.png";
    s.bodies["https://site/a.png"] = realPng("first");
    s.bodies["https://site/b.png"] = realPng("second");
    pictures.want(app);
    for (int i = 0; i < 500 && pictures.path(app.key).empty(); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(app.key)) == realPng("first"));
    app.imageUrl = "https://site/b.png";
    pictures.want(app);
    for (int i = 0; i < 500 && fileText(pictures.path(app.key)) != realPng("second"); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(app.key)) == realPng("second"));
    pictures.stop();
}

TEST_CASE("StorePictures::retryFailed: a failed request is retried, a success is left alone") {
    Setup s;
    StorePictures pictures(s.config());
    pictures.start();

    // a favicon that fails to fetch (no body at all: the runner returns 22, as a refused connection would)
    StorePictures::Request favicon;
    favicon.key = "favicon|https://down/";
    favicon.kind = "favicon";
    favicon.imageUrl = "https://down/";
    pictures.want(favicon);
    for (int i = 0; i < 500 && pictures.pending(favicon.key); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(pictures.path(favicon.key).empty());
    CHECK_FALSE(pictures.pending(favicon.key)); // settled empty, not still being looked for

    // a game whose cover nothing knows: settles empty too, the same way
    StorePictures::Request nobody = s.game("Nobody Knows");
    pictures.want(nobody);
    for (int i = 0; i < 500 && pictures.pending(nobody.key); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(pictures.path(nobody.key).empty());

    // an item that did find its picture: must not be touched by a retry
    StorePictures::Request found = s.game("Anything", "SCUS-94900");
    pictures.want(found);
    for (int i = 0; i < 500 && pictures.path(found.key).empty(); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    const string foundFile = pictures.path(found.key);
    REQUIRE_FALSE(foundFile.empty());

    // want() alone never repeats an unchanged, already-settled request - still empty, still not pending
    pictures.want(favicon);
    this_thread::sleep_for(chrono::milliseconds(50));
    CHECK(pictures.path(favicon.key).empty());
    CHECK_FALSE(pictures.pending(favicon.key));

    // now the site is up: retryFailed() asks the failed ones again, the found one is left as it was
    s.bodies[favicon.imageUrl] = "<html><head><link rel=\"icon\" href=\"/icon.png\"></head></html>";
    s.bodies["https://down/icon.png"] = realPng("now-up");
    pictures.retryFailed();
    CHECK(pictures.pending(favicon.key)); // asked again at once
    for (int i = 0; i < 500 && pictures.pending(favicon.key); i++)
        this_thread::sleep_for(chrono::milliseconds(10));
    CHECK(fileText(pictures.path(favicon.key)) == realPng("now-up"));
    CHECK(pictures.path(found.key) == foundFile); // untouched - it was never asked again

    pictures.stop();
}

namespace {
// an ICO file: a directory of (width, image bytes), the images after it
string icoFile(const vector<pair<int, string>> &images) {
    string out;
    auto u16 = [&out](uint32_t v) {
        out += static_cast<char>(v & 0xff);
        out += static_cast<char>((v >> 8) & 0xff);
    };
    auto u32 = [&u16](uint32_t v) {
        u16(v & 0xffff);
        u16(v >> 16);
    };
    u16(0);
    u16(1);
    u16(static_cast<uint32_t>(images.size()));
    uint32_t offset = static_cast<uint32_t>(6 + 16 * images.size());
    for (const auto &image : images) {
        out += static_cast<char>(image.first & 0xff); // 256 is written as 0
        out += static_cast<char>(image.first & 0xff);
        out += string(2, '\0');
        u16(1);
        u16(32);
        u32(static_cast<uint32_t>(image.second.size()));
        u32(offset);
        offset += static_cast<uint32_t>(image.second.size());
    }
    for (const auto &image : images)
        out += image.second;
    return out;
}
} // namespace

TEST_CASE("StorePictures: a source's favicon address is its server's /favicon.ico; rootUrl is the same, bare") {
    CHECK(StorePictures::faviconUrl("https://example.org/lists/games.tsv") == "https://example.org/favicon.ico");
    CHECK(StorePictures::faviconUrl("http://192.168.1.5:8124/list.tsv?x=1") == "http://192.168.1.5:8124/favicon.ico");
    CHECK(StorePictures::faviconUrl("HTTPS://Example.org") == "https://Example.org/favicon.ico");
    CHECK(StorePictures::faviconUrl("https://user:secret@host.net/a") == "https://host.net/favicon.ico");
    CHECK(StorePictures::faviconUrl("ftp://example.org/list.tsv").empty());
    CHECK(StorePictures::faviconUrl("https:///list.tsv").empty());
    CHECK(StorePictures::faviconUrl("/media/list.tsv").empty());

    CHECK(StorePictures::rootUrl("https://example.org/deep/lists/games.tsv") == "https://example.org/");
    CHECK(StorePictures::rootUrl("http://192.168.1.5:8124/list.tsv?x=1") == "http://192.168.1.5:8124/");
    CHECK(StorePictures::rootUrl("ftp://example.org/list.tsv").empty());
}

TEST_CASE("StorePictures::iconUrlFromHtml: rel tokens, quoting, and absolute/protocol-relative/relative hrefs") {
    const string pageUrl = "https://example.org/deep/path/list.tsv";

    // double quotes, a relative href against the page's own directory (not its own file name)
    CHECK(StorePictures::iconUrlFromHtml("<html><head><link rel=\"icon\" href=\"icon.png\"></head><body></body></html>",
                                         pageUrl) == "https://example.org/deep/path/icon.png");

    // single quotes, rel holding two tokens ("shortcut icon"), an unquoted href
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel='shortcut icon' href=favicon.png></head>", pageUrl) ==
          "https://example.org/deep/path/favicon.png");

    // an absolute href is used as it is
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"https://cdn.example.org/x.png\"></head>",
                                         pageUrl) == "https://cdn.example.org/x.png");

    // a protocol-relative href takes the page's scheme
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"//cdn.example.org/x.png\"></head>",
                                         pageUrl) == "https://cdn.example.org/x.png");

    // a root-relative href goes against the origin, not the page's directory
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"/static/x.png\"></head>", pageUrl) ==
          "https://example.org/static/x.png");

    // attribute order and case do not matter, and a self-closing tag is read the same as an open one
    CHECK(StorePictures::iconUrlFromHtml("<head><link href=\"/a.ico\" REL=\"ICON\" type=\"image/x-icon\" /></head>",
                                         pageUrl) == "https://example.org/a.ico");
}

TEST_CASE("StorePictures::iconUrlFromHtml: base href, apple-touch fallback, sizes, svg skipped, none found") {
    const string pageUrl = "https://example.org/deep/path/list.tsv";

    // <base href> redirects a relative href to itself, not to the page's own directory
    CHECK(StorePictures::iconUrlFromHtml(
              "<head><base href=\"https://cdn.example.net/assets/\"><link rel=\"icon\" href=\"x.png\"></head>",
              pageUrl) == "https://cdn.example.net/assets/x.png");

    // no plain icon: an apple-touch-icon (usually a big one) is a fine fallback
    CHECK(StorePictures::iconUrlFromHtml(
              "<head><link rel=\"apple-touch-icon\" href=\"/apple.png\" sizes=\"180x180\"></head>", pageUrl) ==
          "https://example.org/apple.png");

    // a plain icon on the same page wins over an apple-touch-icon, even a bigger one
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"apple-touch-icon\" href=\"/apple.png\" sizes=\"180x180\">"
                                         "<link rel=\"icon\" href=\"/icon.png\" sizes=\"32x32\"></head>",
                                         pageUrl) == "https://example.org/icon.png");

    // sizes= picks the largest of several plain icons
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"/small.png\" sizes=\"16x16\">"
                                         "<link rel=\"icon\" href=\"/big.png\" sizes=\"48x48\"></head>",
                                         pageUrl) == "https://example.org/big.png");

    // an .svg icon is skipped - the texture loader cannot read it - even when it is the only one offered
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"/icon.svg\" type=\"image/svg+xml\"></head>",
                                         pageUrl)
              .empty());
    CHECK(StorePictures::iconUrlFromHtml(
              "<head><link rel=\"icon\" href=\"/icon.svg\"><link rel=\"icon\" href=\"/icon.png\"></head>", pageUrl) ==
          "https://example.org/icon.png"); // skipped, a usable one further down still wins

    // &amp; in an href is decoded
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"/icon.php?a=1&amp;b=2\"></head>", pageUrl) ==
          "https://example.org/icon.php?a=1&b=2");

    // nothing that qualifies: no <link> at all, one with an unrelated rel, or one only past </head>
    CHECK(StorePictures::iconUrlFromHtml("<head><title>Nothing here</title></head>", pageUrl).empty());
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"stylesheet\" href=\"/site.css\"></head>", pageUrl).empty());
    CHECK(StorePictures::iconUrlFromHtml("<head></head><body><link rel=\"icon\" href=\"/late.png\"></body>", pageUrl)
              .empty());
    CHECK(StorePictures::iconUrlFromHtml("", pageUrl).empty());
    CHECK(StorePictures::iconUrlFromHtml("<head><link rel=\"icon\" href=\"/icon.png\"></head>", "").empty());
}

TEST_CASE("StorePictures: what a favicon holds - a picture as it is, an ICO's largest PNG, an ICO of bitmaps") {
    string image, extension;
    REQUIRE(StorePictures::iconImage(Png + "rest", image, extension));
    CHECK(image == Png + "rest");
    CHECK(extension == "png");
    REQUIRE(StorePictures::iconImage("GIF89a...", image, extension));
    CHECK(extension == "gif");

    const string bitmap = string("\x28\0\0\0", 4) + "pixels";
    REQUIRE(StorePictures::iconImage(icoFile({{16, bitmap}, {16, Png + "16"}, {0, Png + "256"}, {32, Png + "32"}}),
                                     image, extension));
    CHECK(image == Png + "256"); // a width of 0 is 256
    CHECK(extension == "png");

    const string bitmaps = icoFile({{16, bitmap}, {32, bitmap}});
    REQUIRE(StorePictures::iconImage(bitmaps, image, extension));
    CHECK(image == bitmaps); // SDL_image reads those itself
    CHECK(extension == "ico");

    CHECK_FALSE(StorePictures::iconImage("<html><body>Not Found</body></html>", image, extension));
    CHECK_FALSE(StorePictures::iconImage(icoFile({{16, Png + "16"}}).substr(0, 12), image, extension)); // cut short
    CHECK_FALSE(StorePictures::iconImage("", image, extension));
}

TEST_CASE("StorePictures: a source's icon comes from a <link> on its root page, favicon.ico only a fallback") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request r;
    r.key = "favicon|https://site/";
    r.kind = "favicon";
    r.imageUrl = StorePictures::rootUrl("https://site/lists/deep/games.tsv");
    REQUIRE(r.imageUrl == "https://site/");
    s.bodies[r.imageUrl] = "<html><head><link rel=\"icon\" href=\"/static/icon.png\"></head></html>";
    s.bodies["https://site/static/icon.png"] = icoFile({{32, realPng("32")}});
    const string file = pictures.resolve(r);
    CHECK(DirEntry::getFileExtension(file) == "png");
    CHECK(fileText(file) == realPng("32"));
    CHECK(s.fetches.size() == 2); // the root page, then the icon it named - never favicon.ico
    CHECK(pictures.resolve(r) == file);
    CHECK(s.fetches.size() == 2); // the cache answers the second time
}

TEST_CASE("StorePictures: no root page (or nothing usable in it) falls back to favicon.ico") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request r;
    r.key = "favicon|https://noroot/";
    r.kind = "favicon";
    r.imageUrl = "https://noroot/"; // no body: the runner fails it, as a 404 or a refused connection would
    s.bodies["https://noroot/favicon.ico"] = realPng("plain");
    const string file = pictures.resolve(r);
    CHECK(fileText(file) == realPng("plain"));
    CHECK(s.fetches.size() == 2); // the root page (failed), then favicon.ico

    StorePictures::Request page;
    page.key = "favicon|https://pagenoicon/";
    page.kind = "favicon";
    page.imageUrl = "https://pagenoicon/";
    s.bodies[page.imageUrl] = "<html><head><title>No icon here</title></head></html>";
    s.bodies["https://pagenoicon/favicon.ico"] = realPng("plain2");
    CHECK(fileText(pictures.resolve(page)) == realPng("plain2"));
}

TEST_CASE("StorePictures: an icon the root page named but could not fetch also falls back to favicon.ico") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request r;
    r.key = "favicon|https://partial/";
    r.kind = "favicon";
    r.imageUrl = "https://partial/";
    s.bodies[r.imageUrl] = "<html><head><link rel=\"shortcut icon\" href=\"missing.png\"></head></html>";
    // https://partial/missing.png has no body, so it fails
    s.bodies["https://partial/favicon.ico"] = realPng("fallback");
    const string file = pictures.resolve(r);
    CHECK(fileText(file) == realPng("fallback"));
    CHECK(s.fetches.size() == 3); // the page, the icon it named (failed), then favicon.ico
}

TEST_CASE("StorePictures: neither the page nor favicon.ico give anything - nothing is cached, so it is retried") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request r;
    r.key = "favicon|https://nothing/";
    r.kind = "favicon";
    r.imageUrl = "https://nothing/";
    s.bodies[r.imageUrl] = "<html></html>";
    // no https://nothing/favicon.ico body either
    CHECK(pictures.resolve(r).empty());
    CHECK(DirEntry::listNames(s.tmp.at("cache")).empty()); // no ".page"/".download" leftovers, no cache file
}

TEST_CASE("StorePictures: a cache file from before the root-page lookup existed never blocks the new one") {
    Setup s;
    StorePictures pictures(s.config());
    // a leftover under the pre-2026-09-26 scheme (keyed by the plain favicon.ico URL): must never be reused
    s.tmp.writeFile("cache/favicon-" + ableem::Md5::ofString(string("https://old/favicon.ico")) + ".png", "stale");

    StorePictures::Request r;
    r.key = "favicon|https://old/";
    r.kind = "favicon";
    r.imageUrl = "https://old/";
    s.bodies[r.imageUrl] = "<html><head><link rel=\"icon\" href=\"https://cdn.old/icon.png\"></head></html>";
    s.bodies["https://cdn.old/icon.png"] = realPng("fresh");
    const string file = pictures.resolve(r);
    CHECK(fileText(file) == realPng("fresh")); // not "stale" - a different cache key altogether
}

TEST_CASE("StorePictures::validPicture: a corrupt PNG (zero CRC) is refused, a valid one is not") {
    CHECK(StorePictures::validPicture(realPng("x"), "png"));
    CHECK_FALSE(StorePictures::validPicture(corruptPng(), "png")); // the owner's install had exactly this
    CHECK_FALSE(StorePictures::validPicture("not a picture at all", "png"));
    CHECK_FALSE(StorePictures::validPicture("", "png"));
    CHECK_FALSE(StorePictures::validPicture(realPng("x"), "made-up-extension"));
}

TEST_CASE("StorePictures: a corrupt fetched picture is never cached, and counts as a failed fetch") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request r;
    r.key = "favicon|https://badsite/";
    r.kind = "favicon";
    r.imageUrl = "https://badsite/";
    s.bodies[r.imageUrl] = "<html><head><link rel=\"icon\" href=\"/icon.png\"></head></html>";
    s.bodies["https://badsite/icon.png"] = corruptPng();
    CHECK(pictures.resolve(r).empty());
    CHECK(DirEntry::listNames(s.tmp.at("cache")).empty()); // nothing left behind - a plain failed fetch

    // an App/game picture from fetchUrl() is checked the same way
    StorePictures::Request app;
    app.key = "src|app/bad";
    app.kind = "app";
    app.imageUrl = "https://badsite/icon.png";
    CHECK(pictures.resolve(app).empty());
}

TEST_CASE("StorePictures: a pre-existing corrupt cache file - the owner's exact bug - is healed and refetched") {
    Setup s;
    StorePictures pictures(s.config());
    StorePictures::Request r;
    r.key = "favicon|https://healme/";
    r.kind = "favicon";
    r.imageUrl = "https://healme/";
    // as if an older build had cached this before a picture was validated on the way in - the owner's
    // System/Extensions/store/cache/pictures/ had exactly this, under exactly this naming
    s.tmp.writeFile("cache/favicon2-" + ableem::Md5::ofString(r.imageUrl) + ".png", corruptPng());
    s.bodies[r.imageUrl] = "<html><head><link rel=\"icon\" href=\"/icon.png\"></head></html>";
    s.bodies["https://healme/icon.png"] = realPng("healed");
    const string file = pictures.resolve(r);
    CHECK(fileText(file) == realPng("healed")); // the corrupt file was thrown out, a good one fetched instead

    // fetchUrl()'s own cache (an App/game picture) heals the same way
    const string url = "https://healme/app.png";
    s.tmp.writeFile("cache/url-" + ableem::Md5::ofString(url) + ".png", corruptPng());
    s.bodies[url] = realPng("healed-app");
    StorePictures::Request app;
    app.key = "src|app/heal";
    app.kind = "app";
    app.imageUrl = url;
    CHECK(fileText(pictures.resolve(app)) == realPng("healed-app"));
}
