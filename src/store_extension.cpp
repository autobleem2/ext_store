//
// The AutoBleem Store as an extension (docs/extensions-plan.md, docs/store-plan.md in the launcher): the
// service starts with the launcher (Background=true) so its queue goes on in the carousel, its progress is
// the launcher's bubble, and Cross in the Extensions list shows its screen.
//
#include "gui_store.h"
#include "store_pictures.h"
#include "store_service.h"

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
        if (p.busy && now >= holdUntil) {
            const string title = GuiStore::stateText(p.state);
            host.notify(title, p.title + (p.waiting > 0 ? "  (+" + to_string(p.waiting) + ")" : ""), p.done, p.total);
            shown = true;
        } else if (!p.busy && shown && now >= holdUntil) {
            host.clearNotification();
            shown = false;
        }
    }

    void suspend() override { store.pause(); } // a game gets the machine: the download in flight stops
    void resume() override { store.resume(); }
    void shutdown() override {
        store.stop(); // first: its worker asks the pictures for an installed game's cover
        pictures.stop();
    }

private:
    ExtensionHost &host;
    StorePictures pictures; // before the store: built first, gone last - the store's worker reads it
    StoreService store;
    bool shown = false;
    chrono::steady_clock::time_point holdUntil;
};

AB_EXTENSION(StoreExtension)
