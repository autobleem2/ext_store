//
// LanLibrary: what abstored serves - the PS1 games in a folder, found with AutoBleem's own engine (the disc
// images, the serial in the image, the title and cover from the covers databases and the PlayStation rdb) but
// strictly read-only: nothing is written into the games folder, ever. The launcher's scanner repairs cue
// files, unpacks ECM and writes Game.ini; this one only looks, and reports what it cannot serve as problems.
//
// A game is a folder holding game files (any depth; "!..." folders - !SaveStates, !MemCards - and dot folders
// are skipped). Its discs, in the Store's terms (one TSV line per file, the discs numbered):
//   .chd / .pbp present  those are the discs, in disc order ("Game (Disc 2)" after "Game (Disc 1)"), .sbi along
//   else .cue files      the discs; every file each one names rides along (a missing one is a problem)
//   else .img / .bin     the discs (the Store's installer writes a .cue for a lone .bin)
//
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ableem {
class MetadataLookup;
}

//******************
// LanFile / LanGame / LanProblem / LanSnapshot
//******************
struct LanFile {
    std::string relPath; // under the games folder, '/'-separated - what /files/ serves it as
    std::string name;    // the file's own name - what the Store saves it as
    uint64_t size = 0;
    int disc = 0; // 1, 2, ... for a disc image; 0 for a file that rides along (a .bin of a .cue, an .sbi)
};

struct LanGame {
    std::string id;     // its folder under the games folder ("RPG/Chrono Trigger") - unique
    std::string title;  // Game.ini's, else the covers databases' by serial, else the folder's name
    std::string serial; // "" when the image does not say
    std::vector<LanFile> files;
    std::string coverFile; // a picture in its folder; "" = the covers database's by serial, if it has one
    uint64_t size() const;
};

struct LanProblem {
    std::string path; // under the games folder
    std::string what;
    bool error = true; // false: a warning - the game is still served
};

struct LanSnapshot {
    std::vector<LanGame> games;
    std::vector<LanProblem> problems;
    std::time_t scannedAt = 0;
    double seconds = 0; // how long the scan took
};

//******************
// LanLibrary
//******************
class LanLibrary {
public:
    struct Config {
        std::string gamesDir;
        std::string coversDir; // covers{U,P,J}.db; "" = none
        std::string rdbFile;   // "Sony - PlayStation.rdb"; "" = none
        std::string stateDir;  // the checksum cache (never inside the games folder); "" = kept in memory only
    };

    explicit LanLibrary(Config config);
    ~LanLibrary();

    // the folder read again; the snapshot is replaced when it is done
    void scan();
    std::shared_ptr<const LanSnapshot> snapshot() const;
    // names and sizes of everything under the games folder - a scan is due when it changes
    std::string fingerprint() const;

    // the SHA-256 of the files still without one, a file at a time (stop() is asked between files and every
    // few MB); the cache (<stateDir>/checksums.tsv) keeps them across runs, keyed by path and size
    void hashPending(const std::function<bool()> &stop);
    std::map<std::string, std::string> checksums() const; // "<relPath>|<size>" -> sha256
    static std::string checksumKey(const LanFile &file);

    // a file /files/ may serve: one the last scan listed, else ""
    std::string servablePath(const std::string &relPath) const;
    // a game's cover: its folder's picture, else the covers database's PNG by serial
    bool cover(const std::string &id, std::string &bytes, std::string &contentType);

    // the Store's TSV (docs/store-plan.md), every URL under baseUrl ("http://192.168.1.20:8124")
    static std::string tsv(const LanSnapshot &snapshot, const std::map<std::string, std::string> &checksums,
                           const std::string &baseUrl, const std::string &sourceName);
    // '/'-separated path, each segment percent-encoded
    static std::string urlPath(const std::string &path);

    const Config &config() const { return config_; }

private:
    void walk(const std::string &dir, const std::string &rel, LanSnapshot &out, bool root);
    void readGame(const std::string &dir, const std::string &rel, LanSnapshot &out);
    void loadChecksums();
    void saveChecksums() const; // mutex_ held

    Config config_;
    mutable std::mutex mutex_;
    std::shared_ptr<const LanSnapshot> snapshot_;
    std::map<std::string, std::string> checksums_;
    std::mutex metadataMutex_; // the sqlite handles are not shared between threads
    std::unique_ptr<ableem::MetadataLookup> metadata_;
};
