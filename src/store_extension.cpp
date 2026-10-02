//
// The AutoBleem Store as an extension (docs/extensions-plan.md, docs/store-plan.md in the launcher): the
// service starts with the launcher (Background=true) so its queue goes on in the carousel, its progress is
// the launcher's bubble, and Cross in the Extensions list shows its screen.
//
#include "gui_store.h"
#include "store_pictures.h"
#include "store_service.h"
#include "store_speed.h"

#include "core/main.h"
#include "core/services/environment.h"
#include "gui/extension.h"
#include "gui/gui.h"

#include <chrono>
#include <cstdlib>

using namespace std;

namespace {

// where our catalog is: <repo>/store/<platform key>/catalog.json - AB_STORE_CATALOG overrides it (a dev
// machine, a test site)
string catalogUrl() {
    const char *own = getenv("AB_STORE_CATALOG");
    if (own != nullptr && *own != 0)
        return own;
    if (Env::repoUrl().empty())
        return "";
    return Env::repoUrl() + "/store/" + Env::buildTargetKey() + "/catalog.json";
}

StoreService::Config configFor(ExtensionHost &host) {
    StoreService::Config c;
    c.stateDir = host.stateDir();
    c.appsDir = Env::getPathToAppsDir();
    c.gamesDir = Env::getPathToGamesDir();
    c.catalogUrl = catalogUrl();
    // the catalog and the sources with the platform's short-timeout command when it has one; the files with
    // the one that continues a partial download
    c.downloadCommand = Env::storeDownloadCommand();
    c.fetchCommand = Env::downloadCommand().empty() ? c.downloadCommand : Env::downloadCommand();
    c.platformKeys = Env::appPlatformKeys();
    c.networkUp = [&host] { return host.networkUp(); };
    return c;
}

// the pictures: our covers databases and the PlayStation rdb for a game, the item's own URL for an App's icon
StorePictures::Config picturesFor(ExtensionHost &host, const StoreService::Config &store) {
    StorePictures::Config c;
    c.cacheDir = host.stateDir() + sep + "cache" + sep + "pictures";
    c.fetchCommand = store.fetchCommand;
    c.coversDir = Env::getPathToCoversDBDir();
    c.rdbFile = Env::getPathToPlayStationRdbFile();
    // a PSN Title ID's disc serial (data/README.md)
    c.psnSerialsFile = host.folder() + sep + "data" + sep + "psn_serials.tsv";
    c.networkUp = store.networkUp;
    return c;
}

} // namespace

//******************
// StoreExtension
//******************
class StoreExtension : public Extension {
public:
    explicit StoreExtension(ExtensionHost &host)
        : host(host), pictures(picturesFor(host, configFor(host))), store(configFor(host)) {
        // an installed game's cover: the picture the Store showed for it
        store.setPictureSource([this](const string &key) { return pictures.path(key); });
        PLOG_INFO << "the Store, state in " << host.stateDir();
        store.start();
    }

    void run() override {
        // the sources fetched again while the screen is up - it opens on what it has (the cached copies at worst),
        // and a refresh younger than five minutes is not repeated (Square in the screen forces one)
        store.refreshIfOlderThan(300);
        pictures.start(); // looked for while the screen is up, and kept for the next time
        GuiStore screen(*Gui::getInstance(), store, pictures);
        screen.discPicture = host.folder() + sep + "data" + sep + "disc.png"; // a game with no cover
        screen.show();
    }

    // once a frame in the carousel: what finished, and the progress as the launcher's bubble
    void poll() override {
        const StoreService::Update update = store.poll();
        if (update.appsChanged)
            host.reloadApps();
        if (update.gamesChanged)
            host.requestRescan();
        const auto now = chrono::steady_clock::now();
        if (!update.installed.empty()) {
            host.notify(_("Installed"), update.installed.back(), 0, 0);
            shown = true;
            holdUntil = now + chrono::seconds(5);
        } else if (!update.failed.empty()) {
            host.notify(_("Failed"), update.failed.back(), 0, 0);
            shown = true;
            holdUntil = now + chrono::seconds(8);
        }
        const StoreService::Progress p = store.progress();
        // the speed and the time left: only while downloading, of one item, in one stretch (see SpeedMeter)
        if (p.busy && p.state == StoreState::Downloading) {
            if (meterTitle != p.title || meterState != p.state)
                speed.reset();
            meterTitle = p.title;
            meterState = p.state;
            speed.sample(p.done, now);
        } else {
            speed.reset();
            meterTitle.clear();
        }
        if (p.busy && now >= holdUntil) {
            const string title = GuiStore::stateText(p.state);
            const string queued = p.waiting > 0 ? "  (+" + to_string(p.waiting) + ")" : "";
            const string stats = formatSpeedEta(speed, p.done, p.total);
            const string tail = stats.empty() ? "" : "  " + stats;
            const string detail = fitTitle(p.title, queued, stats, p.total > 0) + queued + tail;
            host.notify(title, detail, p.done, p.total);
            shown = true;
        } else if (!p.busy && shown && now >= holdUntil) {
            host.clearNotification();
            shown = false;
        }
    }

    void suspend() override {
        store.pause(); // a game gets the machine: the download in flight stops
        speed.reset();
    }
    void resume() override {
        store.resume();
        speed.reset();
    }
    void shutdown() override {
        store.stop(); // first: its worker asks the pictures for an installed game's cover
        pictures.stop();
    }

private:
    // The item's title, shortened (with "...") so that the queue count, the speed and time left, and the launcher's
    // own "  100%" after the detail fit its bubble - the stats are never the part that is cut. The bubble's text
    // is 440 px less its 2 x 12 px padding wide (evoui_notification_bubble.cpp), in the 15 px bold font.
    static string fitTitle(const string &title, const string &queued, const string &stats, bool percent) {
        const int textWidth = 440 - 2 * 12 - 4; // a few pixels of margin
        const auto gui = Gui::getInstance();
        Fonts &fonts = gui->assets().themeFonts;
        const string tail = queued + (stats.empty() ? "" : "  " + stats) + (percent ? "  100%" : "");
        const int room = max(60, textWidth - gui->text().textWidth(fonts[FONT_15_BOLD], tail));
        return gui->text().elide(fonts[FONT_15_BOLD], title, room);
    }

    ExtensionHost &host;
    SpeedMeter speed;
    string meterTitle;
    StoreState meterState = StoreState::Available;
    StorePictures pictures; // before the store: built first, gone last - the store's worker reads it
    StoreService store;
    bool shown = false;
    chrono::steady_clock::time_point holdUntil;
};

AB_EXTENSION(StoreExtension)
