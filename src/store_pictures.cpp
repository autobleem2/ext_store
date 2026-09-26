//
// StorePictures - see the header.
//
#include "store_pictures.h"

#include "core/main.h"
#include "core/services/downloader.h"
#include "core/services/system.h"

#include <ableem/engine/crc32.h>
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

uint32_t beU32(const string &bytes, size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) << 24 |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

// a real PNG, not a file that merely starts with the signature: every chunk's CRC-32 (the type and data, as
// the spec defines it) is checked against what the file itself claims, and an IHDR before an IEND is required.
// SDL_image's libpng is stricter than our earlier bare-signature sniff (iconImage()) and rejects a chunk whose
// CRC is wrong - a truncated download or a hand-made placeholder with the CRC left as zero, say - so this is
// what actually predicts whether the texture loader will accept the file.
bool validPng(const string &bytes) {
    if (bytes.size() < 8 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0)
        return false;
    size_t pos = 8;
    bool sawIHDR = false;
    while (pos + 12 <= bytes.size()) {
        const uint32_t length = beU32(bytes, pos);
        if (length > bytes.size() || pos + 12 + length > bytes.size())
            return false;
        const uint32_t crc = ableem::Crc32::update(0, bytes.data() + pos + 4, 4 + length);
        if (crc != beU32(bytes, pos + 8 + length))
            return false;
        const string type = bytes.substr(pos + 4, 4);
        if (type == "IHDR")
            sawIHDR = true;
        if (type == "IEND")
            return sawIHDR;
        pos += 12 + length;
    }
    return false; // ran out of bytes before IEND
}

uint32_t leU16(const string &bytes, size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8;
}
uint32_t leU32(const string &bytes, size_t at) {
    return leU16(bytes, at) | leU16(bytes, at + 2) << 16;
}

// the same directory bounds iconImage() extracts an ICO's images by (ICO fields are little-endian), checked
// on the whole file - for the "bitmaps, no embedded PNG" case, where there is nothing else to validate a
// texture loader would decode.
bool validIco(const string &bytes) {
    if (bytes.size() < 6 || leU16(bytes, 0) != 0 || leU16(bytes, 2) != 1 || leU16(bytes, 4) == 0)
        return false;
    const size_t count = leU16(bytes, 4);
    if (bytes.size() < 6 + 16 * count)
        return false;
    for (size_t i = 0; i < count; i++) {
        const size_t entry = 6 + 16 * i;
        const size_t size = leU32(bytes, entry + 8), offset = leU32(bytes, entry + 12);
        if (size == 0 || offset >= bytes.size() || size > bytes.size() - offset)
            return false;
    }
    return true;
}

// how much of a page's HTML is worth scanning for its icon links
constexpr size_t HeadScanCap = 256 * 1024;

// the first `cap` bytes of `path`, or "" when it cannot be opened - used to cap what a fetched page's HTML
// costs to parse, whatever curl actually pulled over the wire
string readFilePrefix(const string &path, size_t cap) {
    ifstream in(path, ios::binary);
    if (!in)
        return "";
    string buf(cap, '\0');
    in.read(&buf[0], static_cast<streamsize>(cap));
    buf.resize(static_cast<size_t>(in.gcount()));
    return buf;
}

// finds attribute `name`'s value inside `tag` (the text from '<' to '>' of one element), case-insensitive,
// quoted with '"', with '\'' or bare; false when the attribute is absent. A word boundary is required on
// both sides of the name, so "href" does not match inside "hreflang".
bool tagAttr(const string &tag, const string &name, string &value) {
    const string lowerTag = ableem::toLowerCopy(tag);
    const string lowerName = ableem::toLowerCopy(name);
    auto isNameChar = [](char c) { return isalnum(static_cast<unsigned char>(c)) != 0 || c == '-'; };
    size_t pos = 0;
    while ((pos = lowerTag.find(lowerName, pos)) != string::npos) {
        const bool boundaryBefore = pos == 0 || !isNameChar(tag[pos - 1]);
        size_t p = pos + lowerName.size();
        const bool boundaryAfter = p >= tag.size() || !isNameChar(tag[p]);
        if (!boundaryBefore || !boundaryAfter) {
            pos = p;
            continue;
        }
        while (p < tag.size() && isspace(static_cast<unsigned char>(tag[p])))
            p++;
        if (p >= tag.size() || tag[p] != '=') {
            pos = p;
            continue;
        }
        p++;
        while (p < tag.size() && isspace(static_cast<unsigned char>(tag[p])))
            p++;
        if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
            const char quote = tag[p];
            p++;
            const size_t end = tag.find(quote, p);
            value = tag.substr(p, end == string::npos ? tag.size() - p : end - p);
        } else {
            size_t end = p;
            while (end < tag.size() && !isspace(static_cast<unsigned char>(tag[end])) && tag[end] != '>' &&
                   tag[end] != '/')
                end++;
            value = tag.substr(p, end - p);
        }
        return true;
    }
    return false;
}

// "Foo &amp; Bar" -> "Foo & Bar" - the one entity an href realistically carries
string decodeAmp(string s) {
    size_t pos = 0;
    while ((pos = s.find("&amp;", pos)) != string::npos)
        s.replace(pos, 5, "&"), pos += 1;
    return s;
}

// a link's rel="..." holds one or more space-separated tokens; true when one of them names an icon
bool relNamesIcon(const string &rel) {
    const string lowerRel = ableem::toLowerCopy(rel);
    size_t start = 0;
    while (start < lowerRel.size()) {
        while (start < lowerRel.size() && isspace(static_cast<unsigned char>(lowerRel[start])))
            start++;
        size_t end = start;
        while (end < lowerRel.size() && !isspace(static_cast<unsigned char>(lowerRel[end])))
            end++;
        const string token = lowerRel.substr(start, end - start);
        if (token == "icon" || token == "apple-touch-icon" || token == "apple-touch-icon-precomposed")
            return true;
        start = end;
    }
    return false;
}

bool relIsAppleTouch(const string &rel) {
    return ableem::toLowerCopy(rel).find("apple-touch-icon") != string::npos;
}

bool hrefIsSvg(const string &href) {
    const string plain = href.substr(0, href.find_first_of("?#"));
    const string lower = ableem::toLowerCopy(plain);
    return lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".svg") == 0;
}

// "32x32", "16x16 32x32", "any" -> the largest side named, 0 when none parses
int sizesScore(const string &sizes) {
    int best = 0;
    size_t start = 0;
    while (start < sizes.size()) {
        while (start < sizes.size() && isspace(static_cast<unsigned char>(sizes[start])))
            start++;
        size_t end = start;
        while (end < sizes.size() && !isspace(static_cast<unsigned char>(sizes[end])))
            end++;
        const string token = sizes.substr(start, end - start);
        const size_t x = token.find_first_of("xX");
        if (x != string::npos)
            best = max(best, atoi(token.substr(0, x).c_str()));
        start = end;
    }
    return best;
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
    lastRequest_[request.key] = request; // kept up to date even when the signature below dedupes the ask
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

//*******************************
// StorePictures::retryFailed
//*******************************
void StorePictures::retryFailed() {
    lock_guard<mutex> lock(mutex_);
    for (const auto &kv : found_) {
        if (!kv.second.empty() || inFlight_.count(kv.first) > 0)
            continue; // a success stays cached; already being looked for again
        auto reqIt = lastRequest_.find(kv.first);
        if (reqIt == lastRequest_.end())
            continue;
        pending_.erase(remove_if(pending_.begin(), pending_.end(), [&](const Request &r) { return r.key == kv.first; }),
                       pending_.end());
        pending_.push_back(reqIt->second);
        inFlight_.insert(kv.first);
    }
}

//*******************************
// StorePictures::forget
//*******************************
void StorePictures::forget(const string &key) {
    lock_guard<mutex> lock(mutex_);
    auto it = found_.find(key);
    if (it == found_.end() || it->second.empty())
        return;
    DirEntry::removeFile(it->second);
    it->second.clear(); // counts as failed again - retryFailed() picks it up
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
        return request.imageUrl.empty() ? "" : fetchSiteIcon(request.imageUrl);
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
// reads `path` back and checks it is a picture the texture loader will actually accept (see validPicture());
// an invalid file is removed - a stale cache from before this check existed, or a download corrupted in
// transit - and false is returned, so the caller treats it exactly as a fetch that never landed anything
bool StorePictures::validCacheFile(const string &path, const string &extension) {
    ifstream in(path, ios::binary);
    if (!in)
        return false;
    const string bytes((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    in.close();
    if (validPicture(bytes, extension))
        return true;
    DirEntry::removeFile(path);
    return false;
}

string StorePictures::fetchUrl(const string &url) {
    string extension = "png";
    for (const char *e : {"jpg", "jpeg", "png"}) {
        const string plain = url.substr(0, url.find('?'));
        const size_t n = strlen(e);
        string tail = plain.size() > n + 1 ? plain.substr(plain.size() - n - 1) : "";
        string rest = tail.empty() ? "" : tail.substr(1);
        if (!tail.empty() && tail[0] == '.' && lcase(rest) == e)
            extension = e;
    }
    const string target = config_.cacheDir + sep + "url-" + ableem::Md5::ofString(url) + "." + extension;
    if (DirEntry::exists(target) && validCacheFile(target, extension))
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
    if (r != Downloader::Result::Downloaded && r != Downloader::Result::AlreadyThere) {
        PLOG_DEBUG << "no picture from " << url << ": " << error;
        return "";
    }
    if (!validCacheFile(target, extension)) {
        PLOG_DEBUG << "no picture from " << url << ": a picture the texture loader would refuse - not cached";
        return "";
    }
    return target;
}

//*******************************
// StorePictures::faviconUrl / rootUrl / iconUrlFromHtml / iconImage / fetchSiteIcon
//*******************************
string StorePictures::faviconUrl(const string &sourceUrl) {
    const string root = rootUrl(sourceUrl);
    return root.empty() ? "" : root + "favicon.ico";
}

string StorePictures::rootUrl(const string &sourceUrl) {
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
    return lower.substr(0, scheme) + host + "/";
}

// href resolved against `base` (an absolute http(s) URL): itself if already absolute, "//host/x" against
// base's scheme, "/x" against base's origin, else against base's directory. "" when base is not http(s) or
// href is empty.
static string resolveIconHref(const string &base, const string &href) {
    const string h = decodeAmp(Strings::trim(href));
    if (h.empty())
        return "";
    const string lowerH = ableem::toLowerCopy(h);
    if (lowerH.compare(0, 7, "http://") == 0 || lowerH.compare(0, 8, "https://") == 0)
        return h;
    const string lowerBase = ableem::toLowerCopy(base);
    const size_t schemeLen = lowerBase.compare(0, 7, "http://") == 0    ? 7
                             : lowerBase.compare(0, 8, "https://") == 0 ? 8
                                                                        : 0;
    if (schemeLen == 0)
        return "";
    if (h.compare(0, 2, "//") == 0)
        return base.substr(0, schemeLen - 2) + h; // "http:" / "https:" + "//host/x"
    const size_t hostEnd = base.find_first_of("/?#", schemeLen);
    const string origin = base.substr(0, hostEnd == string::npos ? base.size() : hostEnd);
    if (h[0] == '/')
        return origin + h;
    string path = hostEnd == string::npos ? "/" : base.substr(hostEnd);
    const size_t cut = path.find_first_of("?#");
    if (cut != string::npos)
        path = path.substr(0, cut);
    const size_t slash = path.rfind('/');
    string dir = slash == string::npos ? "/" : path.substr(0, slash + 1);
    string rel = h;
    while (rel.compare(0, 2, "./") == 0)
        rel = rel.substr(2);
    return origin + dir + rel;
}

string StorePictures::iconUrlFromHtml(const string &html, const string &pageUrl) {
    if (pageUrl.empty())
        return "";
    const string prefix = html.substr(0, min(html.size(), HeadScanCap));
    const string lowerPrefix = ableem::toLowerCopy(prefix);
    const size_t headEnd = lowerPrefix.find("</head>");
    const string region = headEnd == string::npos ? prefix : prefix.substr(0, headEnd);
    const string lowerRegion = headEnd == string::npos ? lowerPrefix : lowerPrefix.substr(0, headEnd);

    // <base href="...">, itself resolved against pageUrl when it is relative
    string baseUrl = pageUrl;
    const size_t basePos = lowerRegion.find("<base");
    if (basePos != string::npos) {
        const size_t close = region.find('>', basePos);
        const string tag =
            region.substr(basePos, close == string::npos ? region.size() - basePos : close - basePos + 1);
        string href;
        if (tagAttr(tag, "href", href) && !href.empty()) {
            const string resolved = resolveIconHref(pageUrl, href);
            if (!resolved.empty())
                baseUrl = resolved;
        }
    }

    struct Candidate {
        string href;
        int score;
    };
    vector<Candidate> candidates;
    size_t pos = 0;
    while ((pos = lowerRegion.find("<link", pos)) != string::npos) {
        const size_t close = region.find('>', pos);
        if (close == string::npos)
            break;
        const string tag = region.substr(pos, close - pos + 1);
        pos = close + 1;

        string rel, href, sizes;
        if (!tagAttr(tag, "rel", rel) || !relNamesIcon(rel))
            continue;
        if (!tagAttr(tag, "href", href) || href.empty() || hrefIsSvg(href))
            continue;
        tagAttr(tag, "sizes", sizes);
        int score = (relIsAppleTouch(rel) ? 500 : 1000) + sizesScore(sizes);
        candidates.push_back({href, score});
    }
    if (candidates.empty())
        return "";
    const Candidate &best = *max_element(candidates.begin(), candidates.end(),
                                         [](const Candidate &a, const Candidate &b) { return a.score < b.score; });
    return resolveIconHref(baseUrl, best.href);
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

//*******************************
// StorePictures::validPicture
//*******************************
bool StorePictures::validPicture(const string &bytes, const string &extension) {
    if (extension == "png")
        return validPng(bytes);
    if (extension == "ico")
        return validIco(bytes);
    // no cheap SDL-free decoder for these three is linked here - a signature and a size a real one could
    // plausibly hold is the best this can check without one
    if (extension == "gif")
        return bytes.size() >= 32 && bytes.compare(0, 4, "GIF8", 4) == 0;
    if (extension == "jpg" || extension == "jpeg")
        return bytes.size() >= 32 && bytes.compare(0, 3, "\xFF\xD8\xFF", 3) == 0;
    if (extension == "bmp")
        return bytes.size() >= 32 && bytes.compare(0, 2, "BM", 2) == 0;
    return false;
}

// into the cache under the root URL's md5 ("favicon2-": a new key, so a plain favicon.ico result cached under
// the pre-2026-09-26 scheme never blocks the icon-in-<head> lookup for a source whose TSV is not at the
// root), as the picture inside it, once - a server with neither is asked again next run (nothing is cached
// on failure)
string StorePictures::fetchSiteIcon(const string &rootUrl) {
    const string base = config_.cacheDir + sep + "favicon2-" + ableem::Md5::ofString(rootUrl);
    for (const string ext : {"png", "ico", "gif", "jpg", "bmp"}) {
        const string cached = base + "." + ext;
        if (!DirEntry::exists(cached))
            continue;
        ifstream in(cached, ios::binary);
        const string bytes((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
        in.close();
        if (validPicture(bytes, ext))
            return cached;
        DirEntry::removeFile(cached); // a cache file from before a picture was validated on the way in - gone
    }
    if (config_.fetchCommand.empty() || !online())
        return "";
    Downloader downloader(config_.fetchCommand, "",
                          [this](const string &line) { return config_.runner(line, [this] { return stop_.load(); }); });

    // 1) the root page's <head>, for a <link rel="icon"|"shortcut icon"|"apple-touch-icon">
    string iconUrl;
    {
        const string page = base + ".page";
        DownloadRequest fetch;
        fetch.url = rootUrl;
        fetch.target = page;
        string error;
        if (downloader.fetch(fetch, error) == Downloader::Result::Downloaded)
            iconUrl = iconUrlFromHtml(readFilePrefix(page, HeadScanCap), rootUrl);
        DirEntry::removeFile(page);
    }
    const string plainFavicon = rootUrl + "favicon.ico";
    if (iconUrl.empty())
        iconUrl = plainFavicon;

    // 2) that icon, falling back to the plain favicon.ico when the page named one that could not be fetched
    const string download = base + ".download";
    DownloadRequest fetch;
    fetch.url = iconUrl;
    fetch.target = download;
    string error;
    Downloader::Result r = downloader.fetch(fetch, error);
    if (r != Downloader::Result::Downloaded && r != Downloader::Result::AlreadyThere && iconUrl != plainFavicon) {
        DirEntry::removeFile(download);
        fetch.url = plainFavicon;
        r = downloader.fetch(fetch, error);
        iconUrl = plainFavicon;
    }
    if (r != Downloader::Result::Downloaded && r != Downloader::Result::AlreadyThere) {
        PLOG_DEBUG << "no icon from " << rootUrl << ": " << error;
        DirEntry::removeFile(download);
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
        PLOG_DEBUG << "no icon from " << rootUrl << " (" << iconUrl << "): not a picture";
        return "";
    }
    if (!validPicture(image, extension)) {
        PLOG_DEBUG << "no icon from " << rootUrl << " (" << iconUrl << "): a picture the texture loader "
                   << "would refuse - not cached";
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
