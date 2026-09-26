//
// StorePictures: the picture the Store shows for an item - an App's icon, a game's cover - found on a thread of
// its own, so a picture never waits for a download of several hundred MB on the service's worker.
//
// Where a picture comes from, first hit wins:
//   an App      its installed folder's icon (app.ini's Image=), else the catalog's image URL (the App's icon)
//   a PS1 game  our own cover sources, as the launcher's scan uses them: the installed folder's cover (the PNG
//               next to the game, or Game.ini's cached cover), then the covers databases' PNG - found by the
//               serial, or by the title through the PlayStation rdb (which also gives the serial and the
//               record name), then RetroArch's box art on this machine by that record name; the source's own
//               image URL only for a game none of them knows. A PSN Title ID (NoPayStation's "NPUF30001") is
//               no disc serial: data/psn_serials.tsv, shipped with the Store, names the disc's ("SLPS-00624")
//   a source    the favicon of its server (kind "favicon", imageUrl rootUrl()): the root page
//               (<scheme>://<host[:port]>/) is read for a <link rel="icon"|"shortcut icon"|"apple-touch-icon">
//               in its <head> (iconUrlFromHtml()), else <root>/favicon.ico as before; fetched once and cached
//               as the image inside it (iconImage())
// The databases' facts about a PS1 game (publisher, year, players, the disc serial) are found on the way and
// kept for the details pane (facts()).
// Nothing is asked of libretro's servers. What is fetched or taken out of a database goes into the Store's
// cache (cache/pictures/).
//
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ableem {
class MetadataLookup;
class ThumbnailLookup;
} // namespace ableem

//******************
// StorePictures
//******************
class StorePictures {
public:
    // runs a command line, stopping it once `cancelled` says so - as StoreService::Runner
    using Runner = std::function<int(const std::string &, const std::function<bool()> &)>;

    struct Config {
        std::string cacheDir;        // System/Extensions/store/cache/pictures
        std::string fetchCommand;    // %u %o, a short timeout - an App's icon, a source's picture
        std::string coversDir;       // covers{U,P,J}.db; "" = none
        std::string rdbFile;         // "Sony - PlayStation.rdb"; "" = none
        bool localThumbnails = true; // RetroArch's thumbnails tree on this machine (Environment's)
        std::string psnSerialsFile;  // Extensions/store/data/psn_serials.tsv; "" = none
        Runner runner;
        std::function<bool()> networkUp; // always up when not given
    };

    // what an item's picture is found by
    struct Request {
        std::string key;  // the Store entry's key
        std::string kind; // "app", "ps1", "favicon" (a source's: imageUrl is rootUrl())
        std::string title, serial;
        std::string imageUrl;      // the source's picture, "" = none
        std::string installedPath; // the App's or the game's folder once installed
    };

    // what the databases know of a PS1 game - found with its picture
    struct GameFacts {
        std::string serial; // the disc's: the source's own, or the one a PSN Title ID maps to
        std::string region; // "US", "EU", "JP", "ASIA": a PSN release's own (the list's), else the serial's
        std::string title;  // the databases' name for it
        std::string publisher;
        int year = 0;
        int players = 0;
    };

    explicit StorePictures(Config config);
    ~StorePictures();
    StorePictures(const StorePictures &) = delete;
    StorePictures &operator=(const StorePictures &) = delete;

    void start();
    void stop();

    // asked for - again when the item was installed or removed, or its source now names another picture; cheap
    // to call every frame
    void want(const Request &request);
    // the picture's file, "" while it is being looked for or when there is none
    std::string path(const std::string &key) const;
    // being looked for right now (asked for, not answered yet) - the screen shows a spinner meanwhile
    bool pending(const std::string &key) const;
    // changes whenever a picture was found - the screen loads new textures then
    uint64_t generation() const { return generation_; }

    // what was found about a PS1 game with its picture; false while it is being looked for or when nothing is known
    bool facts(const std::string &key, GameFacts &out) const;

    // the lookup itself, on the calling thread (the worker's, and the tests'); `facts` gets what the databases
    // know of a PS1 game
    std::string resolve(const Request &request, GameFacts *facts = nullptr);

    // "NPUF30001", "NPJJ00630": a PlayStation Store Title ID - N, three letters, five digits
    static bool isPsnTitleId(const std::string &id);
    // the disc serial psn_serials.tsv names for a Title ID (the first disc's for a multi-disc game), "" when it
    // names none; read on first use
    std::string discSerialFor(const std::string &titleId);
    // the PSN release's region the list gives ("US", "EU", "JP", "ASIA"), "" when it gives none
    std::string psnRegionFor(const std::string &titleId);
    // a region as GameMetadata spells it (SerialScanner::serialToRegion: "US", "Europe-Aus", "Japan") or as
    // the list does, as one of "US", "EU", "JP", "ASIA" - "" for anything else
    static std::string regionCode(const std::string &region);

    // a source's favicon file, as it always was: <scheme>://<host[:port]>/favicon.ico of an http(s) URL, ""
    // for anything else. Still the last resort (fetchSiteIcon falls back to it) and the tests' known-good URL.
    static std::string faviconUrl(const std::string &sourceUrl);
    // the same URL's root: <scheme>://<host[:port]>/ - what a source's picture is actually looked up from
    // (iconUrlFromHtml is tried against it first), "" for anything else
    static std::string rootUrl(const std::string &sourceUrl);
    // the best icon <link> in `html`'s <head> (rel="icon"/"shortcut icon"/"apple-touch-icon"[-precomposed],
    // case-insensitive, quoted any way, several rel tokens; a `sizes` attribute picked by its largest side; an
    // apple-touch-icon is a fallback behind a plain icon of the same size), resolved to an absolute URL against
    // `pageUrl` and any <base href> the page gives; "" when none qualifies (none present, or all are .svg - the
    // texture loader cannot read those) or `html`/`pageUrl` make no sense. Stops at </head> or 256 KB, whichever
    // is first. A pure function - no fetching, no cache - so it is tested directly on HTML snippets.
    static std::string iconUrlFromHtml(const std::string &html, const std::string &pageUrl);
    // what a favicon file holds, as a picture the texture loader reads: a PNG, GIF, JPEG or BMP as it is; an ICO's
    // largest PNG image taken out of it (SDL_image reads an ICO's bitmaps, not its PNGs), an ICO of bitmaps as
    // it is. `extension` says which ("png", "ico", ...); false for anything else (an HTML error page)
    static bool iconImage(const std::string &bytes, std::string &image, std::string &extension);

private:
    void workerMain();
    std::string fetchUrl(const std::string &url);
    std::string fetchSiteIcon(const std::string &rootUrl);
    std::string installedPicture(const Request &request);
    std::string gameCover(const Request &request, GameFacts &facts);
    void readPsnList();
    bool online() const;

    Config config_;
    mutable std::mutex mutex_;
    std::deque<Request> pending_;
    std::map<std::string, std::string> asked_; // key -> what it was asked with (folder, image URL, serial)
    std::map<std::string, std::string> found_; // key -> file ("" = none)
    std::set<std::string> inFlight_;           // asked for, not answered yet
    std::map<std::string, GameFacts> facts_;   // key -> what the databases know (PS1 games they know only)
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    // made on first use: the rdb is read whole, the thumbnail listings are cached
    std::unique_ptr<ableem::MetadataLookup> metadata_;
    std::unique_ptr<ableem::ThumbnailLookup> thumbnails_;
    bool psnSerialsRead_ = false;
    std::map<std::string, std::string> psnSerials_; // Title ID -> disc serial (the worker's)
    std::map<std::string, std::string> psnRegions_; // Title ID -> the release's region
};
