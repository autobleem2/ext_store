//
// StoreService - see the header.
//
#include "store_service.h"

#include "core/main.h"
#include "core/services/content_installer.h"
#include "core/services/downloader.h"
#include "core/services/system.h"

#include <ableem/engine/md5.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>

using namespace std;
using ableem::StoreCatalog;
using ableem::StoreFile;
using ableem::StoreItem;
using ableem::StoreSourceTsv;

namespace {
const char *const OurSourceName = "AutoBleem";

vector<string> readLines(const string &path) {
    vector<string> lines;
    ifstream in(path);
    string line;
    while (Strings::getlineRemoveCR(in, line)) {
        line = Strings::trim(line);
        if (!line.empty() && line[0] != '#')
            lines.push_back(line);
    }
    return lines;
}

// sources.txt's lines: the URL, and - after a tab - the name the user gave the source
struct SourceLine {
    string url, name;
};

vector<SourceLine> readSourceLines(const string &path) {
    vector<SourceLine> out;
    for (const string &line : readLines(path)) {
        const size_t tab = line.find('\t');
        SourceLine s;
        s.url = Strings::trim(line.substr(0, tab));
        s.name = tab == string::npos ? "" : Strings::trim(line.substr(tab + 1));
        if (!s.url.empty())
            out.push_back(s);
    }
    return out;
}

void writeSourceLines(const string &path, const vector<SourceLine> &lines) {
    ofstream out(path, ios::binary | ios::trunc);
    for (const SourceLine &s : lines)
        out << s.url << (s.name.empty() ? "" : "\t" + s.name) << "\n";
}

// a name as one line of text
string oneLine(string s) {
    for (char &c : s)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return Strings::trim(s);
}

// an App's folder name from our catalog's id ("app/opentyrian" -> "opentyrian"); "" for anything else
string appNameFromId(const string &id) {
    return id.compare(0, 4, "app/") == 0 ? id.substr(4) : "";
}

bool supported(const string &kind) {
    return kind == "app" || kind == "ps1";
}
} // namespace

//*******************************
// StoreService::StoreService / ~StoreService
//*******************************
StoreService::StoreService(Config config) : config_(std::move(config)) {
    if (!config_.runner)
        config_.runner = [](const string &line, const function<bool()> &cancelled) {
            return System::runShellCommand(line, cancelled);
        };
    for (const string &dir : {config_.stateDir, sourcesDir(), cacheDir(), downloadsDir(), stagingDir()})
        DirEntry::createDirs(dir);
    readInstalled();
    for (const string &key : readLines(queueFile()))
        queue_.push_back(key);
}

StoreService::~StoreService() {
    stop();
}

string StoreService::sourcesDir() const {
    return config_.stateDir + sep + "sources";
}
string StoreService::sourcesFile() const {
    return config_.stateDir + sep + "sources.txt";
}
string StoreService::cacheDir() const {
    return config_.stateDir + sep + "cache";
}
string StoreService::downloadsDir() const {
    return config_.stateDir + sep + "downloads";
}
string StoreService::stagingDir() const {
    return config_.stateDir + sep + "staging";
}
string StoreService::installedFile() const {
    return config_.stateDir + sep + "installed.tsv";
}
string StoreService::queueFile() const {
    return config_.stateDir + sep + "queue.txt";
}

//*******************************
// StoreService::fileNameFor / versionDiffers
//*******************************
string StoreService::fileNameFor(const StoreFile &file) {
    if (!file.name.empty())
        return DirEntry::getFileNameFromPath(file.name);
    string url = file.url.substr(0, file.url.find_first_of("?#"));
    string name = url.substr(url.find_last_of('/') + 1);
    Strings::replaceAll(name, "%20", " ");
    return name.empty() ? "download" : name;
}

bool StoreService::installable(const StoreItem &item) {
    // temporary for testing - TODO: will be removed later
    /*
    for (const StoreFile &f : item.files) {
        const string name = ableem::toLowerCopy(fileNameFor(f));
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".pkg") == 0)
            return false;
    }
    */
    return true;
}

bool StoreService::versionDiffers(const string &installed, const string &offered) {
    return !offered.empty() && Strings::trim(installed) != Strings::trim(offered);
}

//*******************************
// StoreService::start / stop / refresh
//*******************************
void StoreService::start() {
    if (worker_.joinable())
        return;
    stop_ = false;
    worker_ = thread([this] { workerMain(); });
    sourcesThread_ = thread([this] { sourcesMain(); });
}

void StoreService::stop() {
    stop_ = true;
    if (worker_.joinable())
        worker_.join();
    if (sourcesThread_.joinable())
        sourcesThread_.join();
}

void StoreService::refresh() {
    refresh_ = true;
}

void StoreService::refreshIfOlderThan(int seconds) {
    lock_guard<mutex> lock(mutex_);
    if (lastRefresh_ == chrono::steady_clock::time_point() ||
        chrono::steady_clock::now() - lastRefresh_ > chrono::seconds(seconds))
        refresh_ = true;
}

void StoreService::pause() {
    paused_ = true;
}

void StoreService::resume() {
    paused_ = false;
}

bool StoreService::readingSources() const {
    if (readingSources_ || refresh_)
        return true;
    lock_guard<mutex> lock(mutex_);
    return !sourcesToRead_.empty();
}

//*******************************
// StoreService::workerMain
//*******************************
// the downloads - the sources are read on their own thread, so adding one never waits for a download
void StoreService::workerMain() {
    System::lowerCurrentThreadPriority(); // never the CPU the launcher's frames or a game need
    while (!stop_) {
        const bool online = !config_.networkUp || config_.networkUp();
        string next;
        if (!paused_ && online && loaded_ && refreshedOnce_) {
            lock_guard<mutex> lock(mutex_);
            if (!queue_.empty())
                next = queue_.front();
        }
        if (next.empty()) {
            this_thread::sleep_for(chrono::milliseconds(250));
            continue;
        }
        work(next);
    }
}

//*******************************
// StoreService::sourcesMain
//*******************************
// every source read again on refresh(); a source added since, on its own
void StoreService::sourcesMain() {
    System::lowerCurrentThreadPriority();
    while (!stop_) {
        if (refresh_.exchange(false)) {
            readingSources_ = true;
            loadSources();
            readingSources_ = false;
            continue;
        }
        string url;
        {
            lock_guard<mutex> lock(mutex_);
            if (!sourcesToRead_.empty())
                url = sourcesToRead_.front();
        }
        if (url.empty()) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }
        readingSources_ = true;
        {
            lock_guard<mutex> lock(mutex_);
            readingNow_ = url;
            assembleSources();
            events_.listChanged = true;
        }
        LoadedSource source = readRemote(url);
        {
            lock_guard<mutex> lock(mutex_);
            if (!sourcesToRead_.empty() && sourcesToRead_.front() == url)
                sourcesToRead_.pop_front();
            // removed while it was being read: forgotten, not brought back
            const vector<string> urls = sourceUrls();
            if (std::find(urls.begin(), urls.end(), url) != urls.end())
                loadedSources_[url] = source;
            readingNow_.clear();
            assembleSources();
            events_.listChanged = true;
        }
        readingSources_ = false;
    }
}

//*******************************
// StoreService::fetchTo
//*******************************
bool StoreService::fetchTo(const string &url, const string &target, string &error) {
    Downloader downloader(config_.fetchCommand, "",
                          [this](const string &line) { return config_.runner(line, [this] { return stop_.load(); }); });
    DownloadRequest request;
    request.url = url;
    request.target = target;
    Downloader::Result r = downloader.fetch(request, error);
    return r == Downloader::Result::Downloaded || r == Downloader::Result::AlreadyThere;
}

//*******************************
// StoreService::readCatalog / readLocal / readRemote
//*******************************
// ours: the catalog on the download site, its last good copy when it cannot be fetched now
StoreService::LoadedSource StoreService::readCatalog(bool fetch) {
    const bool online = fetch && (!config_.networkUp || config_.networkUp());
    LoadedSource s;
    s.info.name = OurSourceName;
    s.info.where = config_.catalogUrl;
    s.info.remote = true;
    s.info.ours = true;
    const string cached = cacheDir() + sep + "catalog.json";
    string fetchError;
    if (online && !fetchTo(config_.catalogUrl, cached, fetchError))
        s.info.error = fetchError;
    StoreCatalog catalog;
    string error;
    if (DirEntry::exists(cached) && catalog.load(cached, OurSourceName, error)) {
        s.info.items = static_cast<int>(catalog.items.size());
        s.items = catalog.items;
    } else if (s.info.error.empty()) {
        s.info.error = online ? error : "not connected";
    }
    return s;
}

// a TSV dropped in sources/
StoreService::LoadedSource StoreService::readLocal(const string &path) {
    LoadedSource s;
    s.info.where = path;
    StoreSourceTsv tsv;
    string error;
    if (StoreSourceTsv::load(path, DirEntry::getFileNameWithoutExtension(DirEntry::getFileNameFromPath(path)), tsv,
                             error)) {
        s.info.name = tsv.name;
        s.info.items = static_cast<int>(tsv.items.size());
        s.info.problems = tsv.problems;
        s.items = tsv.items;
    } else {
        s.info.name = DirEntry::getFileNameFromPath(path);
        s.info.error = error;
    }
    return s;
}

// a URL from sources.txt, its last good copy (cache/source-<md5 of the URL>.tsv) when it cannot be fetched now
StoreService::LoadedSource StoreService::readRemote(const string &url, bool fetch) {
    const bool online = fetch && (!config_.networkUp || config_.networkUp());
    LoadedSource s;
    s.info.where = url;
    s.info.remote = true;
    const string cached = cachedSourceFile(url);
    string fetchError;
    if (online && !fetchTo(url, cached, fetchError))
        s.info.error = fetchError;
    StoreSourceTsv tsv;
    string error;
    const string fallbackName = url.substr(url.find_last_of('/') + 1);
    if (DirEntry::exists(cached) && StoreSourceTsv::load(cached, fallbackName, tsv, error)) {
        s.info.name = tsv.name;
        s.info.items = static_cast<int>(tsv.items.size());
        s.info.problems = tsv.problems;
        s.items = tsv.items;
    } else {
        s.info.name = fallbackName;
        if (s.info.error.empty())
            s.info.error = online ? error : "not connected";
    }
    return s;
}

string StoreService::cachedSourceFile(const string &url) const {
    return cacheDir() + sep + "source-" + ableem::Md5::ofString(url) + ".tsv";
}

//*******************************
// StoreService::loadSources
//*******************************
// every source read again, each shown as it arrives; a source gone since (a file deleted) is forgotten
void StoreService::loadSources() {
    vector<string> where;
    auto keep = [&](const string &key, const LoadedSource &source) {
        where.push_back(key);
        PLOG_INFO << "source " << source.info.name << " (" << source.info.where << "): " << source.info.items
                  << " items" << (source.info.error.empty() ? "" : " - " + source.info.error)
                  << (source.info.problems.empty() ? ""
                                                   : ", " + to_string(source.info.problems.size()) + " lines skipped");
        lock_guard<mutex> lock(mutex_);
        loadedSources_[key] = source;
        readingNow_.clear();
        assembleSources();
        events_.listChanged = true;
    };
    auto reading = [&](const string &key) {
        lock_guard<mutex> lock(mutex_);
        readingNow_ = key;
        assembleSources();
        events_.listChanged = true;
    };

    // the first time: the last good copies from the cache, at once - the Store opens on them while every source
    // is fetched afresh below, each one replacing its copy as it arrives
    if (!loaded_) {
        {
            lock_guard<mutex> lock(mutex_);
            if (!config_.catalogUrl.empty() && DirEntry::exists(cacheDir() + sep + "catalog.json"))
                loadedSources_[config_.catalogUrl] = readCatalog(false);
            for (const string &url : sourceUrls())
                if (DirEntry::exists(cachedSourceFile(url)))
                    loadedSources_[url] = readRemote(url, false);
            for (const DirEntry &e : DirEntry::diru_FilesOnly(sourcesDir()))
                if (ableem::toLowerCopy(DirEntry::getFileExtension(e.name)) == "tsv")
                    loadedSources_[sourcesDir() + sep + e.name] = readLocal(sourcesDir() + sep + e.name);
            assembleSources();
            events_.listChanged = true;
        }
        loaded_ = true;
    }

    if (!config_.catalogUrl.empty()) {
        reading(config_.catalogUrl);
        keep(config_.catalogUrl, readCatalog());
    }
    for (const DirEntry &e : DirEntry::diru_FilesOnly(sourcesDir())) {
        if (ableem::toLowerCopy(DirEntry::getFileExtension(e.name)) != "tsv")
            continue;
        const string path = sourcesDir() + sep + e.name;
        keep(path, readLocal(path));
    }
    for (const string &url : sourceUrls()) {
        if (stop_)
            return;
        reading(url);
        keep(url, readRemote(url));
    }

    lock_guard<mutex> lock(mutex_);
    for (auto it = loadedSources_.begin(); it != loadedSources_.end();)
        it = std::find(where.begin(), where.end(), it->first) == where.end() ? loadedSources_.erase(it) : std::next(it);
    // a URL read on its own meanwhile has just been read with the rest
    sourcesToRead_.clear();
    assembleSources();
    lastRefresh_ = chrono::steady_clock::now();
    refreshedOnce_ = true;
    loaded_ = true;
    events_.listChanged = true;
}

//*******************************
// StoreService::assembleSources (mutex_ held)
//*******************************
// the list of sources and every item they offer, in the order the Sources tab shows them: ours, the local
// files, then the URLs as sources.txt has them - one not read yet (just added) as a placeholder, "loading"
void StoreService::assembleSources() {
    vector<StoreItem> items;
    vector<StoreSourceInfo> infos;
    map<string, string> customNames;
    for (const SourceLine &s : readSourceLines(sourcesFile()))
        customNames[s.url] = s.name;
    auto named = [&](StoreSourceInfo &info) {
        auto custom = customNames.find(info.where);
        info.customName = custom == customNames.end() ? "" : custom->second;
        info.displayName = info.customName.empty() ? info.name : info.customName;
    };
    auto add = [&](const string &key) {
        auto it = loadedSources_.find(key);
        if (it == loadedSources_.end())
            return false;
        StoreSourceInfo info = it->second.info;
        info.loading = key == readingNow_;
        named(info);
        infos.push_back(info);
        items.insert(items.end(), it->second.items.begin(), it->second.items.end());
        return true;
    };
    if (!config_.catalogUrl.empty() && !add(config_.catalogUrl)) {
        StoreSourceInfo info;
        info.name = OurSourceName;
        info.where = config_.catalogUrl;
        info.remote = info.ours = info.loading = true;
        named(info);
        infos.push_back(info);
    }
    for (const auto &source : loadedSources_)
        if (!source.second.info.remote)
            add(source.first);
    for (const string &url : sourceUrls())
        if (!add(url)) {
            StoreSourceInfo info;
            info.where = url;
            info.name = url.substr(url.find_last_of('/') + 1);
            info.remote = info.loading = true;
            named(info);
            infos.push_back(info);
        }
    items_ = items;
    sources_ = infos;
    rebuildEntries();
}

//*******************************
// StoreService::rebuildEntries (mutex_ held)
//*******************************
void StoreService::rebuildEntries() {
    vector<StoreEntry> entries;
    for (const StoreItem &item : items_) {
        StoreEntry e;
        e.item = item;
        e.key = item.source + "|" + item.id;
        if (!supported(item.kind)) {
            e.state = StoreState::Unsupported;
        } else if (!installable(item)) {
            e.state = StoreState::NotInstallable;
        } else {
            auto installed = installed_.find(e.key);
            if (installed != installed_.end()) {
                e.installedVersion = installed->second.version;
                e.installedPath = installed->second.path;
            } else if (item.kind == "app" && !appNameFromId(item.id).empty()) {
                // an App put there by hand (or by the PC installer's pack) counts as installed too
                const string folder = config_.appsDir + sep + appNameFromId(item.id);
                if (DirEntry::exists(folder + sep + "app.ini")) {
                    IniFile ini;
                    ini.load(folder + sep + "app.ini");
                    e.installedVersion = Strings::trim(ini.values["version"]);
                    e.installedPath = folder;
                }
            }
            if (!e.installedPath.empty())
                e.state = versionDiffers(e.installedVersion, item.version) ? StoreState::UpdateAvailable
                                                                           : StoreState::Installed;
            if (find_if(queue_.begin(), queue_.end(), [&](const string &k) { return k == e.key; }) != queue_.end())
                e.state = StoreState::Queued;
            if (e.key == current_)
                e.state = currentState_;
            auto failed = failed_.find(e.key);
            if (failed != failed_.end() && e.state != StoreState::Queued && e.key != current_) {
                e.state = StoreState::Failed;
                e.error = failed->second;
            }
        }
        entries.push_back(e);
    }
    entries_ = entries;
}

StoreEntry *StoreService::find(const string &key) {
    for (StoreEntry &e : entries_)
        if (e.key == key)
            return &e;
    return nullptr;
}

//*******************************
// StoreService::work
//*******************************
void StoreService::work(const string &key) {
    StoreItem item;
    {
        lock_guard<mutex> lock(mutex_);
        StoreEntry *entry = find(key);
        if (entry == nullptr) {
            if (!loaded_)
                return;         // the sources are not read yet: it waits
            queue_.pop_front(); // no source offers it any more
            writeQueue();
            return;
        }
        item = entry->item;
        current_ = key;
        currentState_ = StoreState::Downloading;
        currentDone_ = 0;
        currentTotal_ = item.size();
        cancelKey_.clear();
        failed_.erase(key);
        rebuildEntries();
        events_.listChanged = true;
    }
    auto finish = [this, &key](const string &error, bool dequeue) {
        lock_guard<mutex> lock(mutex_);
        if (dequeue && !queue_.empty() && queue_.front() == key)
            queue_.pop_front();
        if (!error.empty())
            failed_[key] = error;
        current_.clear();
        currentPart_.clear();
        writeQueue();
        rebuildEntries();
        events_.listChanged = true;
    };
    auto cancelled = [this, &key]() {
        lock_guard<mutex> lock(mutex_);
        return cancelKey_ == key;
    };

    // the files, each resumed from what an earlier attempt left
    Downloader downloader(config_.downloadCommand, config_.downloadCommand, [this, &cancelled](const string &line) {
        return config_.runner(line, [this, &cancelled] { return stop_.load() || paused_.load() || cancelled(); });
    });
    vector<string> local;
    for (const StoreFile &file : item.files) {
        const string target = downloadsDir() + sep + fileNameFor(file);
        {
            lock_guard<mutex> lock(mutex_);
            currentPart_ = Downloader::partPath(target);
        }
        DownloadRequest request;
        request.url = file.url;
        request.target = target;
        request.size = file.size;
        request.sha256 = file.sha256;
        request.resume = true;
        string error;
        Downloader::Result r = downloader.fetch(request, error);
        if (r != Downloader::Result::Downloaded && r != Downloader::Result::AlreadyThere) {
            if (cancelled()) {
                // the whole item goes: the file in flight and the discs already finished
                DirEntry::removeFile(Downloader::partPath(target));
                for (const string &f : local)
                    DirEntry::removeFile(f);
                PLOG_INFO << item.title << ": cancelled";
                finish("", true);
                return;
            }
            if (stop_ || paused_) {
                PLOG_INFO << item.title << ": paused - it continues later";
                finish("", false); // still first in the queue; the .part stays
                return;
            }
            PLOG_WARNING << item.title << ": " << error;
            {
                lock_guard<mutex> lock(mutex_);
                events_.failed.push_back(item.title + ": " + error);
            }
            finish(error, true);
            return;
        }
        local.push_back(target);
        lock_guard<mutex> lock(mutex_);
        currentDone_ += file.size;
    }

    {
        lock_guard<mutex> lock(mutex_);
        currentState_ = StoreState::Installing;
        currentPart_.clear();
        rebuildEntries();
        events_.listChanged = true;
    }
    InstallResult result =
        item.kind == "app" ? AppInstaller::install(local.front(), config_.appsDir, stagingDir(), config_.platformKeys)
                           : GameInstaller::install(local, item.title, config_.gamesDir, stagingDir());
    for (const string &f : local)
        DirEntry::removeFile(f); // what the installer did not move
    if (!result.ok) {
        PLOG_WARNING << item.title << ": " << result.error;
        {
            lock_guard<mutex> lock(mutex_);
            events_.failed.push_back(item.title + ": " + result.error);
        }
        finish(result.error, true);
        return;
    }
    if (item.kind == "ps1") {
        // the picture the Store showed: the carousel's cover - a game no covers database knows (a homebrew, a
        // community disc like RE1.5) would otherwise get the default one. Before gamesChanged: that starts the
        // launcher's scan, which must find it there
        const string cover = placeCover(result.path, pictureSource_ ? pictureSource_(key) : string(), item.image);
        if (!cover.empty()) {
            PLOG_INFO << item.title << ": cover " << cover;
        }
    }
    {
        lock_guard<mutex> lock(mutex_);
        installed_[key] = Installed{item.kind, item.version, result.path};
        writeInstalled();
        events_.installed.push_back(item.title);
        if (item.kind == "app")
            events_.appsChanged = true;
        else
            events_.gamesChanged = true;
    }
    PLOG_INFO << item.title << " installed to " << result.path;
    finish("", true);
}

//*******************************
// StoreService::coverNameFor / placeCover
//*******************************
string StoreService::coverNameFor(const string &folder) {
    vector<string> cues, images;
    for (const DirEntry &e : DirEntry::diru_FilesOnly(folder)) {
        const string ext = ableem::toLowerCopy(DirEntry::getFileExtension(e.name));
        if (ext == "cue")
            cues.push_back(e.name);
        else if (ext == "chd" || ext == "pbp")
            images.push_back(e.name);
    }
    sort(cues.begin(), cues.end());
    sort(images.begin(), images.end());
    // as the scanner names a disc: a cue's name without .cue, a chd's or pbp's whole name
    if (!cues.empty())
        return DirEntry::getFileNameWithoutExtension(cues.front()) + ".png";
    if (!images.empty())
        return images.front() + ".png";
    return "";
}

string StoreService::placeCover(const string &folder, const string &picture, const string &imageUrl) {
    for (const DirEntry &e : DirEntry::diru_FilesOnly(folder))
        if (ableem::toLowerCopy(DirEntry::getFileExtension(e.name)) == "png")
            return ""; // it has one: the scan's, or the game's own
    const string name = coverNameFor(folder);
    if (name.empty())
        return "";
    const string target = folder + sep + name;
    if (!picture.empty() && DirEntry::exists(picture)) {
        if (DirEntry::copy(picture, target))
            return target;
    } else if (!imageUrl.empty()) {
        string error;
        if (fetchTo(imageUrl, target, error))
            return target;
    }
    return "";
}

//*******************************
// StoreService::poll / entries / sources / progress
//*******************************
StoreService::Update StoreService::poll() {
    lock_guard<mutex> lock(mutex_);
    Update update = events_;
    events_ = Update();
    return update;
}

vector<StoreEntry> StoreService::entries() const {
    lock_guard<mutex> lock(mutex_);
    return entries_;
}

vector<StoreSourceInfo> StoreService::sources() const {
    lock_guard<mutex> lock(mutex_);
    return sources_;
}

StoreService::Progress StoreService::progress() const {
    lock_guard<mutex> lock(mutex_);
    Progress p;
    p.offline = config_.networkUp && !config_.networkUp();
    p.waiting = static_cast<int>(queue_.size()) - (current_.empty() ? 0 : 1);
    if (current_.empty())
        return p;
    p.busy = true;
    p.state = currentState_;
    for (const StoreEntry &e : entries_)
        if (e.key == current_)
            p.title = e.item.title;
    p.total = currentTotal_;
    p.done = currentDone_;
    if (!currentPart_.empty()) {
        const long long part = DirEntry::fileSize(currentPart_);
        if (part > 0)
            p.done += static_cast<uint64_t>(part);
    }
    return p;
}

//*******************************
// StoreService::enqueue / cancel / remove
//*******************************
bool StoreService::enqueue(const string &key) {
    lock_guard<mutex> lock(mutex_);
    StoreEntry *e = find(key);
    if (e == nullptr || e->state == StoreState::Unsupported || e->state == StoreState::NotInstallable ||
        e->state == StoreState::Queued || key == current_)
        return false;
    queue_.push_back(key);
    failed_.erase(key);
    writeQueue();
    rebuildEntries();
    events_.listChanged = true;
    return true;
}

bool StoreService::cancel(const string &key) {
    lock_guard<mutex> lock(mutex_);
    if (key == current_) {
        cancelKey_ = key; // the worker stops its download and drops it
        return true;
    }
    auto it = std::find(queue_.begin(), queue_.end(), key);
    if (it == queue_.end())
        return false;
    queue_.erase(it);
    writeQueue();
    rebuildEntries();
    events_.listChanged = true;
    return true;
}

bool StoreService::remove(const string &key, string &error) {
    lock_guard<mutex> lock(mutex_);
    StoreEntry *e = find(key);
    if (e == nullptr || e->installedPath.empty()) {
        error = "it is not installed";
        return false;
    }
    const bool app = e->item.kind == "app";
    const bool ok =
        app ? AppInstaller::remove(e->installedPath, error) : GameInstaller::remove(e->installedPath, error);
    if (!ok)
        return false;
    installed_.erase(key);
    writeInstalled();
    if (app)
        events_.appsChanged = true;
    else
        events_.gamesChanged = true;
    rebuildEntries();
    events_.listChanged = true;
    return true;
}

//*******************************
// StoreService: sources.txt
//*******************************
vector<string> StoreService::sourceUrls() const {
    vector<string> urls;
    for (const SourceLine &s : readSourceLines(sourcesFile()))
        urls.push_back(s.url);
    return urls;
}

bool StoreService::validSourceUrl(const string &url, string &error) {
    const size_t scheme = url.compare(0, 7, "http://") == 0 ? 7 : url.compare(0, 8, "https://") == 0 ? 8 : 0;
    if (scheme == 0) {
        error = "A source is an http:// or https:// address";
        return false;
    }
    // a server's name (a typed "http:///x" has none), and no blank inside ("8 126") - curl would only say "3"
    if (url.size() == scheme || url[scheme] == '/' ||
        find_if(url.begin(), url.end(), [](unsigned char c) { return isspace(c) != 0; }) != url.end()) {
        error = "The address is not valid";
        return false;
    }
    return true;
}

bool StoreService::addSourceUrl(const string &url, string &error) {
    const string u = Strings::trim(url);
    if (!validSourceUrl(u, error))
        return false;
    vector<string> urls = sourceUrls();
    if (std::find(urls.begin(), urls.end(), u) != urls.end()) {
        error = "That source is in the list already";
        return false;
    }
    {
        ofstream out(sourcesFile(), ios::binary | ios::app);
        out << u << "\n";
    }
    // read on the sources thread, on its own - it shows at once, as loading
    lock_guard<mutex> lock(mutex_);
    sourcesToRead_.push_back(u);
    assembleSources();
    events_.listChanged = true;
    return true;
}

bool StoreService::removeSourceUrl(const string &url) {
    vector<SourceLine> lines = readSourceLines(sourcesFile());
    auto it = find_if(lines.begin(), lines.end(), [&](const SourceLine &s) { return s.url == url; });
    if (it == lines.end())
        return false;
    lines.erase(it);
    writeSourceLines(sourcesFile(), lines);
    DirEntry::removeFile(cachedSourceFile(url));
    // nothing to fetch: its items go now
    lock_guard<mutex> lock(mutex_);
    loadedSources_.erase(url);
    sourcesToRead_.erase(std::remove(sourcesToRead_.begin(), sourcesToRead_.end(), url), sourcesToRead_.end());
    assembleSources();
    events_.listChanged = true;
    return true;
}

bool StoreService::renameSource(const string &url, const string &name) {
    vector<SourceLine> lines = readSourceLines(sourcesFile());
    auto it = find_if(lines.begin(), lines.end(), [&](const SourceLine &s) { return s.url == url; });
    if (it == lines.end())
        return false;
    it->name = oneLine(name);
    writeSourceLines(sourcesFile(), lines);
    lock_guard<mutex> lock(mutex_);
    assembleSources();
    events_.listChanged = true;
    return true;
}

bool StoreService::changeSourceUrl(const string &url, const string &newUrl, string &error) {
    const string u = Strings::trim(newUrl);
    if (!validSourceUrl(u, error))
        return false;
    vector<SourceLine> lines = readSourceLines(sourcesFile());
    auto it = find_if(lines.begin(), lines.end(), [&](const SourceLine &s) { return s.url == url; });
    if (it == lines.end()) {
        error = "The address is not valid";
        return false;
    }
    if (u == url)
        return true;
    if (find_if(lines.begin(), lines.end(), [&](const SourceLine &s) { return s.url == u; }) != lines.end()) {
        error = "That source is in the list already";
        return false;
    }
    it->url = u;
    writeSourceLines(sourcesFile(), lines);
    DirEntry::removeFile(cachedSourceFile(url));
    // the new address is read on its own; until it answers the source keeps its name and items (as loading),
    // and what it answers - items or why not - replaces them
    lock_guard<mutex> lock(mutex_);
    auto old = loadedSources_.find(url);
    if (old != loadedSources_.end()) {
        LoadedSource moved = old->second;
        moved.info.where = u;
        loadedSources_.erase(old);
        loadedSources_[u] = moved;
    }
    sourcesToRead_.erase(std::remove(sourcesToRead_.begin(), sourcesToRead_.end(), url), sourcesToRead_.end());
    sourcesToRead_.push_back(u);
    assembleSources();
    events_.listChanged = true;
    return true;
}

//*******************************
// StoreService: installed.tsv, queue.txt
//*******************************
void StoreService::readInstalled() {
    ifstream in(installedFile());
    string line;
    while (Strings::getlineRemoveCR(in, line)) {
        vector<string> f;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            f.push_back(line.substr(start, tab == string::npos ? string::npos : tab - start));
            if (tab == string::npos)
                break;
            start = tab + 1;
        }
        if (f.size() >= 4 && !f[0].empty() && DirEntry::exists(f[3]))
            installed_[f[0]] = Installed{f[1], f[2], f[3]}; // a folder deleted by hand is not installed any more
    }
}

void StoreService::writeInstalled() const {
    ofstream out(installedFile(), ios::binary | ios::trunc);
    for (const auto &i : installed_)
        out << i.first << "\t" << i.second.kind << "\t" << i.second.version << "\t" << i.second.path << "\n";
}

void StoreService::writeQueue() const {
    ofstream out(queueFile(), ios::binary | ios::trunc);
    for (const string &k : queue_)
        out << k << "\n";
}
