//
// GuiStore: the AutoBleem Store's screen - four tabs (Apps, Games, Downloads, Sources), a list and the selected
// item's details, drawn with the launcher's own classic panel in the user's theme. The Apps and Games lists
// page with L2/R2 (and Left/Right) - held, any of them goes on moving, faster the longer - show one source at a time
// with Select, and filter by a search with Start. Each item has its picture (an App's icon, a game's cover -
// StorePictures finds them) beside it in the list and above its details. It only shows and asks: StoreService does the
// work, on its own thread, and goes on after the screen is closed.
//
#pragma once

#include "store_pictures.h"
#include "store_service.h"

#include "gui/gui_screen.h"
#include "gui/hold_repeat.h"
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
    // why something failed, in words: curl's exit code ("the download failed (7)") as what it means
    static std::string errorText(const std::string &error);
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
        bool loading = false; // a source being read: a spinner at the row's end
    };

    void reload(); // the rows of the tab, from the service
    void moveSelection(int step);
    int visibleRows() const;
    void cross();
    void triangle();
    // Cross on a source the user added: rename it, give it another address, or remove it
    void editSource();
    // a source's name as the screen shows it (the name the user gave it) from the name its items carry
    std::string sourceTitle(const std::string &name) const;
    // Select: the next source of this tab's items, round to all of them
    void nextSourceFilter();
    // Start: the on-screen keyboard for a search, an empty one clears it
    void askSearch();
    bool filtered() const { return !sourceFilter.empty() || !search.empty(); }
    bool matchesFilter(const StoreEntry &e) const;
    const StoreEntry *selectedEntry() const;
    const StoreEntry *entryFor(const std::string &key) const;
    void drawDetails(const ableem::Rect &pane);
    // the entry's picture, asked for when not known yet; an invalid texture while there is none
    ableem::Texture pictureFor(const StoreEntry &entry);
    // the texture fitted into `box`, centred, its own aspect kept
    void drawFitted(const ableem::Texture &texture, const ableem::Rect &box);
    // the picture's box while it is being looked for: a ring of dots turning in it, as the launcher's busy
    // spinner, sized to the box
    void drawSpinner(const ableem::Rect &box);
    // the picture, the spinner while it is being looked for, or an empty frame (`frame`) when there is none
    void drawPicture(const StoreEntry &entry, const ableem::Rect &box, bool frame);

    StoreService &store;
    StorePictures &pictures;
    std::map<std::string, ableem::Texture> textures; // by file; the screen's own, gone with it
    Tab tab = Tab::Apps;
    std::vector<StoreEntry> entries;
    std::vector<StoreSourceInfo> sources;
    std::vector<Row> rows;
    int selected = 0;
    int firstVisible = 0;
    std::string sourceFilter; // the one source the Apps/Games lists show, "" = all
    std::string search;       // what a title must contain (any case), "" = anything
    uint32_t lastReload = 0;
    // a held Up/Down/Left/Right or L2/R2 moving on by itself, sooner the longer it is held; which of them it
    // is, so the release that ends it is that one's
    enum class HoldSource { None, Dpad, L2, R2 };
    HoldRepeat hold;
    HoldSource holdSource = HoldSource::None;
    void startHold(HoldSource source, int step, HoldRepeat::Timing timing);
    void endHold();
    PanelStyle style;
};
