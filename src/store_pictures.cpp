//
// StorePictures - see the header.
//
#include "store_pictures.h"

#include "core/main.h"
#include "core/services/downloader.h"
#include "core/services/system.h"

#include <ableem/engine/md5.h>
#include <ableem/engine/metadata_lookup.h>
#include <ableem/engine/serial_scanner.h>
#include <ableem/engine/thumbnail_lookup.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>

using namespace std;

namespace {

bool isPicture(const string &name) {
    return DirEntry::matchExtension(name, "png") || DirEntry::matchExtension(name, "jpg") ||
           DirEntry::matchExtension(name, "jpeg");
}

string upperCase(string s) {
    for (char &c : s)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// a serial as a file name: letters, digits and '-' only
string serialFileName(const string &serial) {
    string name;
    for (char c : serial)
        name += isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
    return "cover-" + name + ".png";
}

} // namespace

//*******************************
// StorePictures::StorePictures / ~StorePictures
//*******************************
StorePictures::StorePictures(Config config) : config_(std::move(config)) {
    if (!config_.runner)
        config_.runner = [](const string &line, const function<bool()> &cancelled) {
            return System::runShellCommand(line, cancelled);
        };
    DirEntry::createDirs(config_.cacheDir);
}

StorePictures::~StorePictures() {
    stop();
}

//*******************************
// StorePictures::start / stop
//*******************************
void StorePictures::start() {
    if (worker_.joinable())
        return;
    stop_ = false;
    worker_ = thread([this] { workerMain(); });
}

void StorePictures::stop() {
    stop_ = true;
    if (worker_.joinable())
        worker_.join();
}

//*******************************
// StorePictures::want / path / pending
//*******************************
void StorePictures::want(const Request &request) {
    lock_guard<mutex> lock(mutex_);
    // asked again when what it is found by changed: installed or removed, another picture after a refresh
    const string signature = request.installedPath + "|" + request.imageUrl + "|" + request.serial;
    auto asked = asked_.find(request.key);
    if (asked != asked_.end() && asked->second == signature)
        return;
    asked_[request.key] = signature;
    pending_.erase(remove_if(pending_.begin(), pending_.end(), [&](const Request &r) { return r.key == request.key; }),
                   pending_.end());
    pending_.push_back(request);
    inFlight_.insert(request.key);
}

string StorePictures::path(const string &key) const {
    lock_guard<mutex> lock(mutex_);
    auto it = found_.find(key);
    return it == found_.end() ? "" : it->second;
}

bool StorePictures::pending(const string &key) const {
    lock_guard<mutex> lock(mutex_);
    return inFlight_.count(key) > 0;
}

//*******************************
// StorePictures::workerMain
//*******************************
void StorePictures::workerMain() {
    System::lowerCurrentThreadPriority();
    while (!stop_) {
        Request next;
        {
            lock_guard<mutex> lock(mutex_);
            if (!pending_.empty()) {
                // the newest first: what the screen shows now, not what it scrolled past
                next = pending_.back();
                pending_.pop_back();
            }
        }
        if (next.key.empty()) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }
        GameFacts facts;
        const string file = resolve(next, &facts);
        {
            lock_guard<mutex> lock(mutex_);
            found_[next.key] = file;
            if (!facts.serial.empty() || !facts.region.empty() || !facts.publisher.empty() || facts.year > 0)
                facts_[next.key] = facts;
            else
                facts_.erase(next.key);
            // answered - unless it was asked for again meanwhile (then it is queued once more)
            if (none_of(pending_.begin(), pending_.end(), [&](const Request &r) { return r.key == next.key; }))
                inFlight_.erase(next.key);
        }
        if (!file.empty())
            generation_++;
    }
}

//*******************************
// StorePictures::resolve
//*******************************
string StorePictures::resolve(const Request &request, GameFacts *facts) {
    if (facts != nullptr)
        *facts = GameFacts();
    if (request.kind == "favicon")
        return request.imageUrl.empty() ? "" : fetchFavicon(request.imageUrl);
    GameFacts found;
    string file = installedPicture(request);
    // a game's facts are wanted even when its folder has a picture already
    if (request.kind == "ps1") {
        const string cover = gameCover(request, found);
        if (file.empty())
            file = cover;
    }
    if (file.empty() && !request.imageUrl.empty())
        file = fetchUrl(request.imageUrl);
    if (facts != nullptr)
        *facts = found;
    return file;
}

bool StorePictures::facts(const string &key, GameFacts &out) const {
    lock_guard<mutex> lock(mutex_);
    auto it = facts_.find(key);
    if (it == facts_.end())
        return false;
    out = it->second;
    return true;
}

//*******************************
// StorePictures::isPsnTitleId / discSerialFor
//*******************************
bool StorePictures::isPsnTitleId(const string &id) {
    if (id.size() != 9 || id[0] != 'N')
        return false;
    for (size_t i = 1; i < 4; i++)
        if (!isupper(static_cast<unsigned char>(id[i])))
            return false;
    for (size_t i = 4; i < 9; i++)
        if (!isdigit(static_cast<unsigned char>(id[i])))
            return false;
    return true;
}

// psn_serials.tsv: a header naming its columns ("Title ID", "Region", ..., "Serial"), then one line per Title ID;
// a multi-disc game's serials are "SLUS-00453 / SLUS-00561 / ...", the first disc's is the cover's
void StorePictures::readPsnList() {
    if (psnSerialsRead_)
        return;
    psnSerialsRead_ = true;
    {
        ifstream in(config_.psnSerialsFile, ios::binary);
        string line;
        int idColumn = -1, serialColumn = -1, regionColumn = -1;
        while (Strings::getlineRemoveCR(in, line)) {
            vector<string> f;
            size_t start = 0;
            for (;;) {
                const size_t tab = line.find('\t', start);
                f.push_back(Strings::trim(line.substr(start, tab == string::npos ? string::npos : tab - start)));
                if (tab == string::npos)
                    break;
                start = tab + 1;
            }
            if (idColumn < 0) {
                for (size_t i = 0; i < f.size(); i++) {
                    const string name = upperCase(f[i]);
                    if (name == "TITLE ID")
                        idColumn = static_cast<int>(i);
                    else if (name == "SERIAL")
                        serialColumn = static_cast<int>(i);
                    else if (name == "REGION")
                        regionColumn = static_cast<int>(i);
                }
                if (idColumn < 0 || serialColumn < 0)
                    break; // not the list we know
                continue;
            }
            if (static_cast<int>(f.size()) <= max(idColumn, serialColumn))
                continue;
            const string &serials = f[static_cast<size_t>(serialColumn)];
            const string serial = upperCase(Strings::trim(serials.substr(0, serials.find('/'))));
            const string &id = f[static_cast<size_t>(idColumn)];
            if (!id.empty() && !serial.empty())
                psnSerials_[upperCase(id)] = serial;
            if (!id.empty() && regionColumn >= 0 && static_cast<int>(f.size()) > regionColumn) {
                const string region = regionCode(f[static_cast<size_t>(regionColumn)]);
                if (!region.empty())
                    psnRegions_[upperCase(id)] = region;
            }
        }
    }
    if (!psnSerials_.empty()) {
        PLOG_INFO << "PSN Title IDs with a disc serial: " << psnSerials_.size();
    }
}

string StorePictures::discSerialFor(const string &titleId) {
    readPsnList();
    auto it = psnSerials_.find(upperCase(titleId));
    return it == psnSerials_.end() ? "" : it->second;
}

string StorePictures::psnRegionFor(const string &titleId) {
    readPsnList();
    auto it = psnRegions_.find(upperCase(titleId));
    return it == psnRegions_.end() ? "" : it->second;
}

string StorePictures::regionCode(const string &region) {
    const string r = upperCase(Strings::trim(region));
    if (r == "US" || r == "USA" || r == "NTSC-U")
        return "US";
    if (r == "EU" || r == "EUROPE" || r == "EUROPE-AUS" || r == "PAL")
        return "EU";
    if (r == "JP" || r == "JAPAN" || r == "NTSC-J")
        return "JP";
    if (r == "ASIA")
        return "ASIA";
    return "";
}

bool StorePictures::online() const {
    return !config_.networkUp || config_.networkUp();
}

//*******************************
// StorePictures::fetchUrl
//*******************************
// into the cache under the URL's md5, once - a picture that could not be fetched is asked for again next run
string StorePictures::fetchUrl(const string &url) {
    string extension = ".png";
    for (const char *e : {".jpg", ".jpeg", ".png"}) {
        const string plain = url.substr(0, url.find('?'));
        const size_t n = strlen(e);
        string tail = plain.size() > n ? plain.substr(plain.size() - n) : "";
        if (lcase(tail) == e)
            extension = e;
    }
    const string target = config_.cacheDir + sep + "url-" + ableem::Md5::ofString(url) + extension;
    if (DirEntry::exists(target))
        return target;
    if (config_.fetchCommand.empty() || !online())
        return "";
    Downloader downloader(config_.fetchCommand, "",
                          [this](const string &line) { return config_.runner(line, [this] { return stop_.load(); }); });
    DownloadRequest fetch;
    fetch.url = url;
    fetch.target = target;
    string error;
    const Downloader::Result r = downloader.fetch(fetch, error);
    if (r == Downloader::Result::Downloaded || r == Downloader::Result::AlreadyThere)
        return target;
    PLOG_DEBUG << "no picture from " << url << ": " << error;
    return "";
}

//*******************************
// StorePictures::faviconUrl / iconImage / fetchFavicon
//*******************************
string StorePictures::faviconUrl(const string &sourceUrl) {
    const string url = Strings::trim(sourceUrl);
    const string lower = ableem::toLowerCopy(url);
    const size_t scheme = lower.compare(0, 7, "http://") == 0 ? 7 : lower.compare(0, 8, "https://") == 0 ? 8 : 0;
    if (scheme == 0)
        return "";
    const size_t end = url.find_first_of("/?#", scheme);
    string host = url.substr(scheme, end == string::npos ? string::npos : end - scheme);
    const size_t at = host.rfind('@'); // user:password@ - not the server's name
    if (at != string::npos)
        host = host.substr(at + 1);
    if (host.empty())
        return "";
    return lower.substr(0, scheme) + host + "/favicon.ico";
}

bool StorePictures::iconImage(const string &bytes, string &image, string &extension) {
    auto startsWith = [&bytes](size_t at, const char *magic, size_t length) {
        return bytes.size() >= at + length && bytes.compare(at, length, magic, length) == 0;
    };
    auto u16 = [&bytes](size_t at) {
        return static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) |
               static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8;
    };
    auto u32 = [&u16](size_t at) { return u16(at) | u16(at + 2) << 16; };
    // a picture of its own under the name favicon.ico - common, and what the texture loader reads as it is
    const struct {
        const char *magic;
        size_t length;
        const char *extension;
    } plain[] = {{"\x89PNG", 4, "png"}, {"GIF8", 4, "gif"}, {"\xFF\xD8\xFF", 3, "jpg"}, {"BM", 2, "bmp"}};
    for (const auto &p : plain)
        if (startsWith(0, p.magic, p.length)) {
            image = bytes;
            extension = p.extension;
            return true;
        }
    // an ICO: a directory of images (16 bytes each after a 6-byte header), each a PNG or a headerless bitmap
    if (bytes.size() < 6 || u16(0) != 0 || u16(2) != 1 || u16(4) == 0)
        return false;
    const size_t count = u16(4);
    if (bytes.size() < 6 + 16 * count)
        return false;
    size_t bestArea = 0, bestOffset = 0, bestSize = 0;
    bool bitmaps = false;
    for (size_t i = 0; i < count; i++) {
        const size_t entry = 6 + 16 * i;
        const size_t width = bytes[entry] == 0 ? 256 : static_cast<unsigned char>(bytes[entry]);
        const size_t height = bytes[entry + 1] == 0 ? 256 : static_cast<unsigned char>(bytes[entry + 1]);
        const size_t size = u32(entry + 8), offset = u32(entry + 12);
        if (size == 0 || offset >= bytes.size() || size > bytes.size() - offset)
            continue;
        if (!startsWith(offset, "\x89PNG", 4)) {
            bitmaps = true;
            continue;
        }
        if (width * height >= bestArea) {
            bestArea = width * height;
            bestOffset = offset;
            bestSize = size;
        }
    }
    if (bestSize > 0) {
        image = bytes.substr(bestOffset, bestSize);
        extension = "png";
        return true;
    }
    if (!bitmaps)
        return false;
    image = bytes; // SDL_image reads an ICO's bitmaps itself
    extension = "ico";
    return true;
}

// into the cache under the URL's md5, as the picture inside it, once - a server with none is asked again next run
string StorePictures::fetchFavicon(const string &url) {
    const string base = config_.cacheDir + sep + "favicon-" + ableem::Md5::ofString(url);
    for (const char *e : {".png", ".ico", ".gif", ".jpg", ".bmp"})
        if (DirEntry::exists(base + e))
            return base + e;
    if (config_.fetchCommand.empty() || !online())
        return "";
    const string download = base + ".download";
    Downloader downloader(config_.fetchCommand, "",
                          [this](const string &line) { return config_.runner(line, [this] { return stop_.load(); }); });
    DownloadRequest fetch;
    fetch.url = url;
    fetch.target = download;
    string error;
    const Downloader::Result r = downloader.fetch(fetch, error);
    if (r != Downloader::Result::Downloaded && r != Downloader::Result::AlreadyThere) {
        PLOG_DEBUG << "no favicon from " << url << ": " << error;
        return "";
    }
    string bytes;
    {
        ifstream in(download, ios::binary);
        bytes.assign(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
    }
    DirEntry::removeFile(download);
    string image, extension;
    if (!iconImage(bytes, image, extension)) {
        PLOG_DEBUG << "no favicon from " << url << ": not a picture";
        return "";
    }
    const string target = base + "." + extension;
    ofstream out(target, ios::binary);
    out.write(image.data(), static_cast<streamsize>(image.size()));
    out.close();
    if (!out.good()) {
        DirEntry::removeFile(target);
        return "";
    }
    return target;
}

//*******************************
// StorePictures::installedPicture
//*******************************
// an App's icon as its app.ini names it; a game's cover as the scan left it
string StorePictures::installedPicture(const Request &request) {
    const string &folder = request.installedPath;
    if (folder.empty() || !DirEntry::isDirectory(folder))
        return "";
    if (request.kind == "app") {
        IniFile ini;
        ini.load(folder + sep + "app.ini");
        const string image = Strings::trim(ini.values["image"]);
        if (!image.empty() && DirEntry::exists(folder + sep + image))
            return folder + sep + image;
        return "";
    }
    for (const string &name : DirEntry::listNames(folder))
        if (isPicture(name))
            return folder + sep + name;
    IniFile ini;
    ini.load(folder + sep + GAME_INI);
    const string cached = Strings::trim(ini.values["cached_cover_path"]);
    return !cached.empty() && DirEntry::exists(cached) ? cached : "";
}

//*******************************
// StorePictures::gameCover
//*******************************
// the covers databases' PNG by serial - the serial the source gives, or the one the rdb (or a covers db) knows
// the title by - then RetroArch's box art on this machine under the rdb's record name
string StorePictures::gameCover(const Request &request, GameFacts &facts) {
    if (!metadata_)
        metadata_ = make_unique<ableem::MetadataLookup>(config_.coversDir, config_.rdbFile);

    // a PSN Title ID is looked for by the disc serial it maps to - or by the title, when it maps to none
    const string given = isPsnTitleId(request.serial) ? discSerialFor(request.serial) : request.serial;
    ableem::GameMetadata md;
    bool known = !given.empty() && metadata_->findBySerial(given, md);
    if (!known)
        for (const string &title : {request.title, ableem::MetadataLookup::cleanTitle(request.title)})
            if (!title.empty() && metadata_->findByTitle(title, md)) {
                known = true;
                break;
            }
    if (known && md.bytes.empty() && !md.serial.empty()) {
        ableem::GameMetadata bySerial;
        if (metadata_->findBySerial(md.serial, bySerial))
            md.bytes = bySerial.bytes;
    }
    const string serial = !given.empty() ? given : md.serial;
    // the region: a PSN release's own (the list's - a "diff region" serial is another release's disc), else
    // the disc serial's
    facts.region = isPsnTitleId(request.serial) ? psnRegionFor(request.serial) : "";
    if (facts.region.empty() && !serial.empty())
        facts.region = regionCode(!md.region.empty() ? md.region : ableem::SerialScanner::serialToRegion(serial));
    if (known) {
        facts.serial = serial;
        facts.title = md.title;
        facts.publisher = md.publisher;
        facts.year = md.year;
        facts.players = md.players;
    } else if (!given.empty()) {
        facts.serial = given; // at least what the disc is, from the list
    }
    if (!md.bytes.empty()) {
        const string target = config_.cacheDir + sep + serialFileName(serial.empty() ? md.title : serial);
        if (!DirEntry::exists(target)) {
            ofstream out(target, ios::binary);
            out.write(md.bytes.data(), static_cast<streamsize>(md.bytes.size()));
            if (!out.good()) {
                out.close();
                DirEntry::removeFile(target);
                return "";
            }
        }
        return target;
    }
    if (!config_.localThumbnails)
        return "";
    if (!thumbnails_)
        thumbnails_ = make_unique<ableem::ThumbnailLookup>();
    return thumbnails_->findBoxArt(ableem::ThumbnailLookup::PlayStationDbName,
                                   md.title.empty() ? request.title : md.title, md.recordName);
}
