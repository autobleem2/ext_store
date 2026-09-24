//
// StorePictures - see the header.
//
#include "store_pictures.h"

#include "core/main.h"
#include "core/services/downloader.h"
#include "core/services/system.h"

#include <ableem/engine/md5.h>
#include <ableem/engine/metadata_lookup.h>
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
// StorePictures::want / path
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
}

string StorePictures::path(const string &key) const {
    lock_guard<mutex> lock(mutex_);
    auto it = found_.find(key);
    return it == found_.end() ? "" : it->second;
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
        const string file = resolve(next);
        {
            lock_guard<mutex> lock(mutex_);
            found_[next.key] = file;
        }
        if (!file.empty())
            generation_++;
    }
}

//*******************************
// StorePictures::resolve
//*******************************
string StorePictures::resolve(const Request &request) {
    string file = installedPicture(request);
    if (file.empty() && request.kind == "ps1")
        file = gameCover(request);
    if (file.empty() && !request.imageUrl.empty())
        file = fetchUrl(request.imageUrl);
    return file;
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
string StorePictures::gameCover(const Request &request) {
    if (!metadata_)
        metadata_ = make_unique<ableem::MetadataLookup>(config_.coversDir, config_.rdbFile);

    ableem::GameMetadata md;
    bool known = !request.serial.empty() && metadata_->findBySerial(request.serial, md);
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
    const string serial = !request.serial.empty() ? request.serial : md.serial;
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
