//
// LanLibrary - see the header.
//
#include "lan_library.h"

#include <ableem/engine/disc_suffix.h>
#include <ableem/engine/filesystem.h>
#include <ableem/engine/ini_file.h>
#include <ableem/engine/log.h>
#include <ableem/engine/md5.h>
#include <ableem/engine/metadata_lookup.h>
#include <ableem/engine/serial_scanner.h>
#include <ableem/engine/sha256.h>
#include <ableem/engine/strings.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

using namespace std;
using ableem::DirEntries;
using ableem::DirEntry;
using ableem::sep;

namespace {

string lower(string s) {
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

string extensionOf(const string &name) {
    return lower(DirEntry::getFileExtension(name));
}

bool skippedFolder(const string &name) {
    return name.empty() || name[0] == '!' || name[0] == '.';
}

// a disc serial ("SLUS-00123", "SCES-00060"): four letters, a dash, digits - what the engine gives for a disc
// with none (a fingerprint of the image, 64 hex characters) is not one, and would name no cover
string plausibleSerial(const string &serial) {
    if (serial.size() < 8 || serial.size() > 12 || serial[4] != '-')
        return "";
    for (size_t i = 0; i < 4; i++)
        if (!isalpha(static_cast<unsigned char>(serial[i])))
            return "";
    return serial;
}

// The discs without a serial that AutoBleem's engine still recognises (SerialScanner::readSerialByWorkaround -
// by the image's name, then an md5 of it): community releases that no covers database or rdb lists, so their
// name comes from here. The image's file name, upper-cased, contains the marker.
string knownSerialLessTitle(const string &image) {
    static const struct {
        const char *marker;
        const char *title;
    } known[] = {
        {"BH2", "Resident Evil 1.5"}, // the unreleased Biohazard 2 prototype, rebuilt by the community
    };
    string name = DirEntry::getFileNameFromPath(image);
    transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
    for (const auto &k : known)
        if (!name.empty() && name.find(k.marker) != string::npos)
            return k.title;
    return "";
}

bool isPicture(const string &name) {
    const string e = extensionOf(name);
    return e == "png" || e == "jpg" || e == "jpeg";
}

string joinRel(const string &rel, const string &name) {
    return rel.empty() ? name : rel + "/" + name;
}

// discs in order: by the "(Disc n)" marker, then by name
void sortDiscs(vector<string> &names) {
    stable_sort(names.begin(), names.end(), [](const string &a, const string &b) {
        const int da = ableem::DiscSuffix::parse(DirEntry::getFileNameWithoutExtension(a)).disc;
        const int db = ableem::DiscSuffix::parse(DirEntry::getFileNameWithoutExtension(b)).disc;
        if (da != db)
            return da < db;
        return lower(a) < lower(b);
    });
}

string tsvField(string s) {
    for (char &c : s)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return s;
}

} // namespace

//*******************************
// LanGame::size
//*******************************
uint64_t LanGame::size() const {
    uint64_t total = 0;
    for (const LanFile &f : files)
        total += f.size;
    return total;
}

//*******************************
// LanLibrary::LanLibrary / ~LanLibrary
//*******************************
LanLibrary::LanLibrary(Config config) : config_(std::move(config)), snapshot_(make_shared<LanSnapshot>()) {
    config_.gamesDir = DirEntry::removeSeparatorFromEndOfPath(config_.gamesDir);
    metadata_ = make_unique<ableem::MetadataLookup>(config_.coversDir, config_.rdbFile);
    loadChecksums();
}

LanLibrary::~LanLibrary() = default;

shared_ptr<const LanSnapshot> LanLibrary::snapshot() const {
    lock_guard<mutex> lock(mutex_);
    return snapshot_;
}

//*******************************
// LanLibrary::scan
//*******************************
void LanLibrary::scan() {
    const auto started = chrono::steady_clock::now();
    auto next = make_shared<LanSnapshot>();
    if (!DirEntry::isDirectory(config_.gamesDir))
        next->problems.push_back({"", "the games folder " + config_.gamesDir + " is not there", true});
    else
        walk(config_.gamesDir, "", *next, true);

    // two games of one title would be one item in the Store: the folder's name tells them apart
    map<string, int> uses;
    for (const LanGame &g : next->games)
        uses[lower(g.title)]++;
    for (LanGame &g : next->games)
        if (uses[lower(g.title)] > 1)
            g.title += " (" + g.id + ")";
    sort(next->games.begin(), next->games.end(),
         [](const LanGame &a, const LanGame &b) { return lower(a.title) < lower(b.title); });

    next->scannedAt = time(nullptr);
    next->seconds = chrono::duration<double>(chrono::steady_clock::now() - started).count();
    int errors = 0;
    for (const LanProblem &p : next->problems)
        errors += p.error ? 1 : 0;
    PLOG_INFO << "scanned " << config_.gamesDir << ": " << next->games.size() << " games, " << errors << " errors, "
              << next->problems.size() - errors << " warnings";
    lock_guard<mutex> lock(mutex_);
    snapshot_ = next;
}

void LanLibrary::walk(const string &dir, const string &rel, LanSnapshot &out, bool root) {
    DirEntries entries = DirEntry::diru(dir);
    sort(entries.begin(), entries.end(), DirEntry::sortDirEntryByName);
    // any disc file makes a game folder - a .cue alone too (its missing files are a problem to report, where
    // the engine's own test would pass the folder by in silence)
    const bool gameFiles = any_of(entries.begin(), entries.end(), [](const DirEntry &e) {
        const string ext = extensionOf(e.name);
        return !e.isDir && (ext == "chd" || ext == "pbp" || ext == "cue" || ext == "bin" || ext == "img");
    });
    if (gameFiles) {
        if (root) {
            out.problems.push_back({"",
                                    "game files lie loose in the games folder - a game has to be in a folder "
                                    "of its own to be served",
                                    true});
        } else {
            readGame(dir, rel, out);
            return;
        }
    }
    for (const DirEntry &e : entries)
        if (e.isDir && !skippedFolder(e.name))
            walk(dir + sep + e.name, joinRel(rel, e.name), out, false);
}

//*******************************
// LanLibrary::readGame
//*******************************
void LanLibrary::readGame(const string &dir, const string &rel, LanSnapshot &out) {
    vector<string> images, cues, bins, sbis, pictures;
    for (const DirEntry &e : DirEntry::diru(dir)) {
        if (e.isDir)
            continue;
        const string ext = extensionOf(e.name);
        if (ext == "chd" || ext == "pbp")
            images.push_back(e.name);
        else if (ext == "cue")
            cues.push_back(e.name);
        else if (ext == "bin" || ext == "img")
            bins.push_back(e.name);
        else if (ext == "sbi")
            sbis.push_back(e.name);
        else if (isPicture(e.name))
            pictures.push_back(e.name);
    }
    sort(pictures.begin(), pictures.end());

    LanGame game;
    game.id = rel;
    auto add = [&](const string &name, int disc) {
        LanFile f;
        f.name = name;
        f.relPath = joinRel(rel, name);
        const long long size = DirEntry::fileSize(dir + sep + name);
        f.size = size > 0 ? static_cast<uint64_t>(size) : 0;
        f.disc = disc;
        game.files.push_back(f);
        return size > 0;
    };

    ableem::ImageType type = ableem::IMAGE_NO_GAME_FOUND;
    string firstImage; // what the serial is read from
    bool ok = true;
    if (!images.empty()) {
        sortDiscs(images);
        for (size_t i = 0; i < images.size(); i++)
            ok = add(images[i], static_cast<int>(i) + 1) && ok;
        for (const string &s : sbis)
            add(s, 0);
        type = extensionOf(images[0]) == "pbp" ? ableem::IMAGE_PBP : ableem::IMAGE_CHD;
        firstImage = type == ableem::IMAGE_PBP ? "" : dir + sep + images[0];
    } else if (!cues.empty()) {
        sortDiscs(cues);
        set<string> riding;
        for (size_t i = 0; i < cues.size(); i++) {
            ok = add(cues[i], static_cast<int>(i) + 1) && ok;
            for (const string &file : DirEntry::cueToBinList(dir + sep + cues[i])) {
                if (file.empty() || riding.count(lower(file)))
                    continue;
                riding.insert(lower(file));
                // a cue names its files as they were when it was made: find them whatever their case now
                string actual;
                for (const string &b : bins)
                    if (lower(b) == lower(file))
                        actual = b;
                if (actual.empty()) {
                    out.problems.push_back(
                        {joinRel(rel, cues[i]), "names " + file + ", which is not in the folder", true});
                    ok = false;
                    continue;
                }
                ok = add(actual, 0) && ok;
                if (firstImage.empty()) {
                    firstImage = dir + sep + actual;
                    type = extensionOf(actual) == "img" ? ableem::IMAGE_IMG : ableem::IMAGE_BIN;
                }
            }
        }
        for (const string &s : sbis)
            add(s, 0);
    } else if (!bins.empty()) {
        sortDiscs(bins);
        for (size_t i = 0; i < bins.size(); i++)
            ok = add(bins[i], static_cast<int>(i) + 1) && ok;
        type = extensionOf(bins[0]) == "img" ? ableem::IMAGE_IMG : ableem::IMAGE_BIN;
        firstImage = dir + sep + bins[0];
    }
    for (const LanFile &f : game.files)
        if (f.size == 0)
            out.problems.push_back({f.relPath, "is empty (or cannot be read)", true});
    if (game.files.empty() || !ok) {
        if (game.files.empty())
            out.problems.push_back({rel, "no disc image the Store can install (.chd, .pbp, .cue, .bin, .img)", true});
        return;
    }

    if (type != ableem::IMAGE_NO_GAME_FOUND)
        game.serial = plausibleSerial(ableem::SerialScanner::readSerial(type, dir, firstImage));
    // a disc that carries no serial, but that the engine knows by its image (the community releases): listed
    // under its own name, and not reported
    const string known =
        game.serial.empty() ? knownSerialLessTitle(firstImage.empty() && !images.empty() ? images[0] : firstImage) : "";

    ableem::IniFile ini;
    if (DirEntry::exists(dir + sep + "Game.ini")) {
        ini.load(dir + sep + "Game.ini");
        game.title = ableem::Strings::trim(ini.values["title"]);
        if (game.serial.empty())
            game.serial =
                plausibleSerial(ableem::SerialScanner::normalizeSerial(ableem::Strings::trim(ini.values["serial"])));
    }
    if (!known.empty())
        game.title = known;
    if (game.title.empty() && !game.serial.empty()) {
        lock_guard<mutex> lock(metadataMutex_);
        ableem::GameMetadata md;
        if (metadata_->findBySerial(game.serial, md))
            game.title = md.title;
    }
    if (game.title.empty())
        game.title = DirEntry::getFileNameFromPath(dir);
    if (!pictures.empty())
        game.coverFile = dir + sep + pictures.front();
    if (game.serial.empty() && known.empty())
        out.problems.push_back(
            {rel, "no serial found in the image - served, but the Store cannot find its cover", false});
    out.games.push_back(game);
}

//*******************************
// LanLibrary::fingerprint
//*******************************
string LanLibrary::fingerprint() const {
    ostringstream all;
    function<void(const string &, const string &)> list = [&](const string &dir, const string &rel) {
        DirEntries entries = DirEntry::diru(dir);
        sort(entries.begin(), entries.end(), DirEntry::sortDirEntryByName);
        for (const DirEntry &e : entries) {
            if (e.isDir) {
                if (!skippedFolder(e.name))
                    list(dir + sep + e.name, joinRel(rel, e.name));
            } else {
                all << joinRel(rel, e.name) << '|' << DirEntry::fileSize(dir + sep + e.name) << '\n';
            }
        }
    };
    list(config_.gamesDir, "");
    return ableem::Md5::ofString(all.str());
}

//*******************************
// LanLibrary::checksums
//*******************************
string LanLibrary::checksumKey(const LanFile &file) {
    return file.relPath + "|" + to_string(file.size);
}

map<string, string> LanLibrary::checksums() const {
    lock_guard<mutex> lock(mutex_);
    return checksums_;
}

void LanLibrary::loadChecksums() {
    if (config_.stateDir.empty())
        return;
    ifstream in(config_.stateDir + sep + "checksums.tsv", ios::binary);
    string line;
    while (getline(in, line)) {
        const size_t tab = line.rfind('\t');
        if (tab != string::npos && line.size() - tab - 1 == 64)
            checksums_[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

void LanLibrary::saveChecksums() const {
    if (config_.stateDir.empty())
        return;
    DirEntry::createDirs(config_.stateDir);
    const string path = config_.stateDir + sep + "checksums.tsv";
    {
        ofstream out(path + ".tmp", ios::binary | ios::trunc);
        for (const auto &c : checksums_)
            out << c.first << '\t' << c.second << '\n';
    }
    DirEntry::removeFile(path);
    DirEntry::renameFile(path + ".tmp", path);
}

void LanLibrary::hashPending(const function<bool()> &stop) {
    const shared_ptr<const LanSnapshot> snap = snapshot();
    for (const LanGame &game : snap->games)
        for (const LanFile &file : game.files) {
            if (stop())
                return;
            const string key = checksumKey(file);
            {
                lock_guard<mutex> lock(mutex_);
                if (checksums_.count(key))
                    continue;
            }
            ifstream in(config_.gamesDir + sep + file.relPath, ios::binary);
            if (!in)
                continue;
            ableem::Sha256 sha;
            vector<char> buffer(1 << 20);
            bool stopped = false;
            while (in) {
                in.read(buffer.data(), static_cast<streamsize>(buffer.size()));
                if (in.gcount() > 0)
                    sha.update(reinterpret_cast<const unsigned char *>(buffer.data()),
                               static_cast<size_t>(in.gcount()));
                if (stop()) {
                    stopped = true;
                    break;
                }
            }
            if (stopped)
                return;
            const string digest = sha.hexDigest();
            PLOG_DEBUG << "sha256 " << file.relPath << " " << digest;
            lock_guard<mutex> lock(mutex_);
            checksums_[key] = digest;
            saveChecksums();
        }
}

//*******************************
// LanLibrary::servablePath / cover
//*******************************
string LanLibrary::servablePath(const string &relPath) const {
    const shared_ptr<const LanSnapshot> snap = snapshot();
    for (const LanGame &game : snap->games)
        for (const LanFile &file : game.files)
            if (file.relPath == relPath)
                return config_.gamesDir + sep + relPath;
    return "";
}

bool LanLibrary::cover(const string &id, string &bytes, string &contentType) {
    const shared_ptr<const LanSnapshot> snap = snapshot();
    for (const LanGame &game : snap->games) {
        if (game.id != id)
            continue;
        if (!game.coverFile.empty()) {
            ifstream in(game.coverFile, ios::binary);
            bytes.assign(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
            contentType = extensionOf(game.coverFile) == "png" ? "image/png" : "image/jpeg";
            return !bytes.empty();
        }
        if (game.serial.empty())
            return false;
        lock_guard<mutex> lock(metadataMutex_);
        ableem::GameMetadata md;
        if (!metadata_->findBySerial(game.serial, md) || md.bytes.empty())
            return false;
        bytes.assign(md.bytes.begin(), md.bytes.end());
        contentType = "image/png";
        return true;
    }
    return false;
}

//*******************************
// LanLibrary::tsv / urlPath
//*******************************
string LanLibrary::urlPath(const string &path) {
    static const char hex[] = "0123456789ABCDEF";
    string out;
    for (unsigned char c : path) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

string LanLibrary::tsv(const LanSnapshot &snapshot, const map<string, string> &checksums, const string &baseUrl,
                       const string &sourceName) {
    ostringstream out;
    out << "# autobleem-store 1\n# name: " << tsvField(sourceName) << "\n";
    out << "kind\ttitle\turl\tsize\tsha256\tdisc\tname\tserial\timage\n";
    for (const LanGame &game : snapshot.games) {
        // a cover to offer: a picture in its folder, or a serial the covers databases may know
        const string image =
            !game.coverFile.empty() || !game.serial.empty() ? baseUrl + "/cover/" + urlPath(game.id) : "";
        bool first = true;
        for (const LanFile &file : game.files) {
            auto sum = checksums.find(checksumKey(file));
            out << "ps1\t" << tsvField(game.title) << '\t' << baseUrl << "/files/" << urlPath(file.relPath) << '\t'
                << file.size << '\t' << (sum == checksums.end() ? "" : sum->second) << '\t'
                << (file.disc > 0 ? to_string(file.disc) : "") << '\t' << tsvField(file.name) << '\t'
                << (first ? game.serial : "") << '\t' << (first ? image : "") << '\n';
            first = false;
        }
    }
    return out.str();
}
