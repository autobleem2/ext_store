//
// StoreService: the AutoBleem Store's model - what the sources offer (our catalog and the user's TSV sources),
// what is installed, and the queue a worker thread downloads and installs from. No screen in here: GuiStore
// draws it, the extension's poll() hands its progress to the launcher's bubble (docs/store-plan.md in the
// launcher repository).
//
// Everything it keeps is in its state directory (System/Extensions/store/):
//   sources/*.tsv     the user's own TSV sources, dropped on the stick
//   sources.txt       the remote TSV sources, one per line: the URL, then - after a tab - the name the user gave
//                     it, when they did (# comments)
//   cache/            the last good copy of our catalog and of each remote source (source-<md5 of its URL>.tsv)
//   downloads/        the files being downloaded (<name>.part while unfinished - they resume)
//   staging/          the installers' unpacking room
//   installed.tsv     what the Store installed: key, kind, version, path - one per line
//   queue.txt         what is still to be done, one key per line - a power-off only pauses it
//
#pragma once

#include <ableem/engine/store_catalog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

//******************
// StoreState
//******************
// Unsupported: a kind this Store does not know (hidden). NotInstallable: listed, never downloaded - a PSN
// package (.pkg) from a list in the NoPayStation layout: the Store installs disc images, not packages
enum class StoreState {
    Available,
    Queued,
    Downloading,
    Installing,
    Installed,
    UpdateAvailable,
    Failed,
    Unsupported,
    NotInstallable
};

//******************
// StoreEntry
//******************
struct StoreEntry {
    std::string key; // "<source name>|<item id>" - unique across sources
    ableem::StoreItem item;
    StoreState state = StoreState::Available;
    std::string installedVersion;
    std::string installedPath;
    std::string error; // why the last attempt failed
};

//******************
// StoreSourceInfo
//******************
struct StoreSourceInfo {
    std::string name;  // what the items say they came from
    std::string where; // the file, or the URL
    bool remote = false;
    bool ours = false; // our catalog on the download site
    int items = 0;
    std::vector<std::string> problems; // the TSV's skipped lines
    std::string error;                 // why it could not be read now (a cached copy stands in when there is one)
    bool loading = false;              // being read right now (a source just added: not read yet)
    std::string displayName;           // what the screen calls it: the name the user gave it, else `name`
    std::string customName;            // the name the user gave it, "" = none
};

//******************
// StoreService
//******************
class StoreService {
public:
    // runs a command line, stopping it once `cancelled` says so - System::runShellCommand(line, cancelled)
    using Runner = std::function<int(const std::string &, const std::function<bool()> &)>;

    struct Config {
        std::string stateDir;                  // System/Extensions/store
        std::string appsDir;                   // Apps/
        std::string gamesDir;                  // Games/
        std::string catalogUrl;                // <repo>/store/<platform key>/catalog.json; "" = no catalog of ours
        std::string fetchCommand;              // %u %o with a short timeout: the catalog and remote sources
        std::string downloadCommand;           // %u %o, continuing a partial %o: the items' files
        std::vector<std::string> platformKeys; // Env::appPlatformKeys(), for an App's check
        Runner runner;
        std::function<bool()> networkUp; // always up when not given
    };

    struct Progress {
        bool busy = false; // downloading or installing
        std::string title; // the item's
        StoreState state = StoreState::Available;
        uint64_t done = 0, total = 0; // bytes, of the item's files; total 0 = unknown
        int waiting = 0;              // in the queue after this one
        bool offline = false;
    };

    // what happened since the last poll(): for the screen and the launcher
    struct Update {
        bool listChanged = false;
        bool appsChanged = false;           // an App was installed or removed: the launcher's Apps set
        bool gamesChanged = false;          // a game was: the launcher's scan
        std::vector<std::string> installed; // titles
        std::vector<std::string> failed;    // "title: why"
    };

    explicit StoreService(Config config);
    ~StoreService();
    StoreService(const StoreService &) = delete;
    StoreService &operator=(const StoreService &) = delete;

    // the worker: reads the sources, then works the queue whenever there is a network and it is not paused
    void start();
    // the worker stops (an unfinished download is cancelled and kept for later) and is joined
    void stop();
    // the sources read again - the remote ones fetched - on the worker
    void refresh();
    // refresh(), unless the last one finished less than `seconds` ago - what opening the screen does
    void refreshIfOlderThan(int seconds);
    Update poll();

    std::vector<StoreEntry> entries() const;
    std::vector<StoreSourceInfo> sources() const;
    Progress progress() const;
    bool sourcesLoaded() const { return loaded_; }
    // a source is being read (a refresh, or one just added) - the screen's spinner
    bool readingSources() const;

    bool enqueue(const std::string &key);
    bool cancel(const std::string &key);                     // out of the queue; the one being downloaded is stopped
    bool remove(const std::string &key, std::string &error); // an installed item, uninstalled
    // a game is starting: the download in flight is stopped (its bytes kept) until resume()
    void pause();
    void resume();
    bool paused() const { return paused_; }

    // sources.txt: an added URL is read on its own (the others are not fetched again), a removed one's items go
    // at once, nothing fetched
    std::vector<std::string> sourceUrls() const;
    bool addSourceUrl(const std::string &url, std::string &error);
    bool removeSourceUrl(const std::string &url);
    // the name the screen shows for a source of sources.txt ("" = the list's own again). Only what is shown
    // changes: the items keep the list's own name inside, so what was installed or queued from it stays so
    bool renameSource(const std::string &url, const std::string &name);
    // another address for a source of sources.txt, its name kept; it is read again from there
    bool changeSourceUrl(const std::string &url, const std::string &newUrl, std::string &error);
    // an http:// or https:// address with a server and no blanks; error says why not (English - the screen
    // translates it)
    static bool validSourceUrl(const std::string &url, std::string &error);

    std::string sourcesDir() const;
    std::string sourcesFile() const;
    std::string cacheDir() const;
    std::string downloadsDir() const;
    std::string stagingDir() const;
    std::string installedFile() const;
    std::string queueFile() const;

    // the name a file is saved under: the item's, else the URL's last segment without its query
    static std::string fileNameFor(const ableem::StoreFile &file);
    static bool versionDiffers(const std::string &installed, const std::string &offered);
    // an item the Store lists but will not fetch: a game made of PSN packages (.pkg)
    static bool installable(const ableem::StoreItem &item);

private:
    struct Installed {
        std::string kind, version, path;
    };
    struct LoadedSource {
        StoreSourceInfo info;
        std::vector<ableem::StoreItem> items;
    };

    void workerMain();  // the downloads
    void sourcesMain(); // the sources
    void loadSources(); // every source, on the sources thread
    // fetch = false: the cached copy only (the list the Store opens on)
    LoadedSource readCatalog(bool fetch = true);
    LoadedSource readLocal(const std::string &path);
    LoadedSource readRemote(const std::string &url, bool fetch = true);
    std::string cachedSourceFile(const std::string &url) const;
    void assembleSources(); // items_ and sources_ from loadedSources_ (mutex_ held)
    void rebuildEntries();
    void work(const std::string &key); // one queued item, on the worker
    bool fetchTo(const std::string &url, const std::string &target, std::string &error);
    void readInstalled();
    void writeInstalled() const;
    void writeQueue() const;
    StoreEntry *find(const std::string &key);

    Config config_;
    mutable std::mutex mutex_;
    std::vector<ableem::StoreItem> items_;
    std::map<std::string, LoadedSource> loadedSources_; // by where: the catalog's URL, a file, a source URL
    std::deque<std::string> sourcesToRead_;             // URLs added since the last full read
    std::string readingNow_;                            // the source being read
    std::chrono::steady_clock::time_point lastRefresh_; // when every source was last read
    std::vector<StoreSourceInfo> sources_;
    std::vector<StoreEntry> entries_;
    std::map<std::string, Installed> installed_;
    std::deque<std::string> queue_;
    std::string current_;      // the key being worked on
    std::string currentPart_;  // its file being downloaded, for the progress
    uint64_t currentDone_ = 0; // its files already finished
    uint64_t currentTotal_ = 0;
    StoreState currentState_ = StoreState::Available;
    std::map<std::string, std::string> failed_; // key -> why, until it is queued again
    Update events_;

    std::thread worker_;
    std::thread sourcesThread_;
    std::atomic<bool> readingSources_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> refresh_{true};
    std::atomic<bool> loaded_{false};
    std::atomic<bool> refreshedOnce_{false}; // every source read afresh once: the downloads may start
    std::string cancelKey_;                  // under mutex_
};
