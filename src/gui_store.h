//
// GuiStore: the AutoBleem Store's screen - four tabs (Apps, Games, Downloads, Sources), a list and the selected
// item's details, drawn with the launcher's own classic panel in the user's theme. Each item has its picture
// (an App's icon, a game's cover - StorePictures finds them) beside it in the list and above its details. It
// only shows and asks: StoreService does the work, on its own thread, and goes on after the screen is closed.
//
#pragma once

#include "store_pictures.h"
#include "store_service.h"

#include "gui/gui_screen.h"
#include "gui/panel_style.h"

#include <map>
#include <string>
#include <vector>

//******************
// GuiStore
//******************
class GuiStore : public GuiScreen {
public:
    GuiStore(ableem::GuiBase &gui, StoreService &store, StorePictures &pictures)
        : GuiScreen(gui), store(store), pictures(pictures) {}

    void init() override;
    void render() override;
    void loop() override;

    enum class Tab { Apps, Games, Downloads, Sources };

    // a state as the list says it
    static std::string stateText(StoreState state);
    // "412 MB", "3.1 GB", "" for 0
    static std::string sizeText(uint64_t bytes);

private:
    // one line of the list: an entry, a source, or an action ("Add a source URL")
    struct Row {
        std::string key; // the entry's key, or the source's URL/file
        std::string title;
        std::string detail;
        bool action = false; // the Sources tab's "Add a source URL"
        bool remoteSource = false;
    };

    void reload(); // the rows of the tab, from the service
    void moveSelection(int step);
    int visibleRows() const;
    void cross();
    void triangle();
    const StoreEntry *selectedEntry() const;
    const StoreEntry *entryFor(const std::string &key) const;
    void drawDetails(const ableem::Rect &pane);
    // the entry's picture, asked for when not known yet; an invalid texture while there is none
    ableem::Texture pictureFor(const StoreEntry &entry);
    // the texture fitted into `box`, centred, its own aspect kept
    void drawFitted(const ableem::Texture &texture, const ableem::Rect &box);

    StoreService &store;
    StorePictures &pictures;
    std::map<std::string, ableem::Texture> textures; // by file; the screen's own, gone with it
    Tab tab = Tab::Apps;
    std::vector<StoreEntry> entries;
    std::vector<StoreSourceInfo> sources;
    std::vector<Row> rows;
    int selected = 0;
    int firstVisible = 0;
    uint32_t lastReload = 0;
    PanelStyle style;
};
