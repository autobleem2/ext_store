//
// GuiStore - see the header.
//
#include "gui_store.h"

#include "gui/gui.h"
#include "gui/screens/gui_confirm.h"
#include "gui/screens/gui_keyboard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace std;

namespace {
const int Margin = 40;
const int HeaderHeight = PanelStyle::HeaderHeight;
const int FooterHeight = PanelStyle::FooterHeight;
const int RowHeight = 60;
const int RowInset = PanelStyle::RowInset;
const int PaneWidth = 400;
const int Thumb = 48;              // a row's picture, square
const int ThumbSpace = Thumb + 14; // what it takes of the row's text
const int PanePictureHeight = 160; // the details pane's picture
const uint32_t ReloadEvery = 500;  // ms: the worker's news, often enough for a progress to move
} // namespace

//*******************************
// GuiStore::stateText / sizeText
//*******************************
string GuiStore::stateText(StoreState state) {
    switch (state) {
    case StoreState::Installed:
        return _("Installed");
    case StoreState::UpdateAvailable:
        return _("Update available");
    case StoreState::Queued:
        return _("Queued");
    case StoreState::Downloading:
        return _("Downloading");
    case StoreState::Installing:
        return _("Installing");
    case StoreState::Failed:
        return _("Failed");
    case StoreState::NotInstallable:
        return _("Not installable");
    default:
        return "";
    }
}

string GuiStore::sizeText(uint64_t bytes) {
    if (bytes == 0)
        return "";
    char text[32];
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        snprintf(text, sizeof(text), "%.1f GB", mb / 1024.0);
    else if (mb >= 1.0)
        snprintf(text, sizeof(text), "%.0f MB", mb);
    else
        snprintf(text, sizeof(text), "%.0f KB", bytes / 1024.0);
    return text;
}

//*******************************
// GuiStore::init / reload
//*******************************
void GuiStore::init() {
    style = gui->panelStyle();
    reload();
}

void GuiStore::reload() {
    entries = store.entries();
    sources = store.sources();
    lastReload = gui->platform().ticks();
    const string keep = selected < static_cast<int>(rows.size()) ? rows[selected].key : "";
    rows.clear();
    auto entryRow = [](const StoreEntry &e) {
        Row r;
        r.key = e.key;
        r.title = e.item.title;
        string detail = stateText(e.state);
        const string size = sizeText(e.item.size());
        for (const string &part : {e.item.version, size, e.item.source})
            if (!part.empty())
                detail += (detail.empty() ? "" : "  -  ") + part;
        r.detail = detail;
        return r;
    };
    switch (tab) {
    case Tab::Apps:
    case Tab::Games:
        for (const StoreEntry &e : entries)
            if (e.state != StoreState::Unsupported && e.item.kind == (tab == Tab::Apps ? "app" : "ps1"))
                rows.push_back(entryRow(e));
        sort(rows.begin(), rows.end(),
             [](const Row &a, const Row &b) { return lessCaseInsensitive(a.title, b.title); });
        break;
    case Tab::Downloads:
        for (const StoreEntry &e : entries)
            if (e.state == StoreState::Downloading || e.state == StoreState::Installing)
                rows.push_back(entryRow(e));
        for (const StoreEntry &e : entries)
            if (e.state == StoreState::Queued || e.state == StoreState::Failed) {
                Row r = entryRow(e);
                if (e.state == StoreState::Failed && !e.error.empty())
                    r.detail = _("Failed") + ": " + e.error;
                rows.push_back(r);
            }
        // then what is installed - where Triangle removes it again
        for (const StoreEntry &e : entries)
            if (e.state == StoreState::Installed || e.state == StoreState::UpdateAvailable)
                rows.push_back(entryRow(e));
        break;
    case Tab::Sources:
        for (const StoreSourceInfo &s : sources) {
            Row r;
            r.key = s.where;
            r.title = s.name;
            r.remoteSource = s.remote && !s.ours;
            r.detail = to_string(s.items) + " " + _("items");
            if (!s.error.empty())
                r.detail += "  -  " + s.error;
            else if (!s.problems.empty())
                r.detail += "  -  " + s.problems.front();
            rows.push_back(r);
        }
        {
            Row add;
            add.title = _("Add a source URL");
            add.detail = _("A TSV list of downloads, on any web server");
            add.action = true;
            rows.push_back(add);
        }
        break;
    }
    // the same row again: by its key, or - a row without one (the Sources tab's "Add a source URL") - by its
    // place, which a reload every half second must not take away from under the cursor
    const int before = selected;
    selected = 0;
    bool kept = false;
    for (size_t i = 0; i < rows.size(); i++)
        if (!keep.empty() && rows[i].key == keep) {
            selected = static_cast<int>(i);
            kept = true;
        }
    if (!kept && keep.empty() && !rows.empty())
        selected = min(before, static_cast<int>(rows.size()) - 1);
    firstVisible = min(firstVisible, max(0, static_cast<int>(rows.size()) - visibleRows()));
    moveSelection(0);
}

int GuiStore::visibleRows() const {
    return max(1, (SCREEN_HEIGHT - 2 * Margin - HeaderHeight - FooterHeight - 30) / RowHeight);
}

void GuiStore::moveSelection(int step) {
    const int count = static_cast<int>(rows.size());
    if (count == 0) {
        selected = firstVisible = 0;
        return;
    }
    if (step == 1 || step == -1)
        selected = (selected + step + count) % count;
    else
        selected = max(0, min(count - 1, selected + step));
    const int visible = visibleRows();
    if (selected < firstVisible)
        firstVisible = selected;
    else if (selected >= firstVisible + visible)
        firstVisible = selected - visible + 1;
}

const StoreEntry *GuiStore::selectedEntry() const {
    if (selected >= static_cast<int>(rows.size()))
        return nullptr;
    return entryFor(rows[selected].key);
}

const StoreEntry *GuiStore::entryFor(const string &key) const {
    if (key.empty())
        return nullptr;
    for (const StoreEntry &e : entries)
        if (e.key == key)
            return &e;
    return nullptr;
}

//*******************************
// GuiStore::pictureFor / drawFitted
//*******************************
ableem::Texture GuiStore::pictureFor(const StoreEntry &entry) {
    StorePictures::Request request;
    request.key = entry.key;
    request.kind = entry.item.kind;
    request.title = entry.item.title;
    request.serial = entry.item.serial;
    request.imageUrl = entry.item.image;
    request.installedPath = entry.installedPath;
    pictures.want(request);
    const string file = pictures.path(entry.key);
    if (file.empty())
        return ableem::Texture();
    auto it = textures.find(file);
    if (it == textures.end())
        it = textures.emplace(file, ableem::Texture::loadFile(renderer, file)).first;
    return it->second;
}

void GuiStore::drawPicture(const StoreEntry &entry, const ableem::Rect &box, bool frame) {
    const ableem::Texture picture = pictureFor(entry);
    if (picture.valid()) {
        drawFitted(picture, box);
        return;
    }
    if (pictures.pending(entry.key)) {
        drawSpinner(box);
    } else if (frame) {
        renderer.setDrawColor(style.secondary);
        renderer.drawRect(box);
    }
}

void GuiStore::drawSpinner(const ableem::Rect &box) {
    // twelve dots on a ring, the brightest leading, turning a dot every 70 ms - the launcher's busy spinner,
    // at the box's size
    const int size = min(box.w, box.h);
    const int dot = min(8, max(3, size / 10)); // never bigger than the launcher's own (8 px dots, radius 30)
    const int radius = min(30, max(6, size / 2 - dot - 2));
    const int cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    const int lead = static_cast<int>(gui->platform().ticks() / 70) % 12;
    renderer.setBlendMode(ableem::BlendMode::Blend);
    for (int i = 0; i < 12; i++) {
        const int behind = (lead - i + 12) % 12;
        const int alpha = 255 - behind * 19;
        const double a = i * 3.14159265 / 6.0;
        const int x = cx + static_cast<int>(radius * cos(a)) - dot / 2;
        const int y = cy + static_cast<int>(radius * sin(a)) - dot / 2;
        renderer.setDrawColor(
            ableem::Color(style.text.r, style.text.g, style.text.b, static_cast<unsigned char>(alpha)));
        renderer.fillRect(ableem::Rect(x, y, dot, dot));
    }
}

void GuiStore::drawFitted(const ableem::Texture &texture, const ableem::Rect &box) {
    const ableem::Size size = texture.size();
    if (size.w <= 0 || size.h <= 0)
        return;
    const double scale = min(static_cast<double>(box.w) / size.w, static_cast<double>(box.h) / size.h);
    const int w = static_cast<int>(size.w * scale), h = static_cast<int>(size.h * scale);
    const ableem::Rect target(box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h);
    renderer.copy(texture, nullptr, &target);
}

//*******************************
// GuiStore::render
//*******************************
void GuiStore::render() {
    gui->renderBackground();
    style.dim(renderer);
    const ableem::Rect panel{Margin, Margin, SCREEN_WIDTH - 2 * Margin, SCREEN_HEIGHT - 2 * Margin};
    style.sheet(renderer, panel);

    const TextRenderer::Shadow classicShadow = gui->text().shadow();
    TextRenderer::Shadow shadow;
    shadow.enabled = style.textShadow;
    gui->text().setShadow(shadow);
    Fonts &fonts = gui->assets().themeFonts;

    int y = style.header(*gui, panel, _("Store"));
    // the tabs, on the header's right: the current one in the text colour
    {
        const vector<pair<Tab, string>> tabs{{Tab::Apps, _("Apps")},
                                             {Tab::Games, _("Games")},
                                             {Tab::Downloads, _("Downloads")},
                                             {Tab::Sources, _("Sources")}};
        int x = panel.x + panel.w - RowInset;
        for (auto it = tabs.rbegin(); it != tabs.rend(); ++it) {
            const int w = fonts[FONT_20_BOLD].width(it->second);
            x -= w;
            gui->text().renderText_WithColor(fonts[FONT_20_BOLD], it->second, x, panel.y + 26,
                                             it->first == tab ? style.text : style.secondary, XALIGN_LEFT);
            if (it->first == tab) {
                renderer.setDrawColor(style.text);
                renderer.fillRect(ableem::Rect(x, panel.y + 56, w, 3));
            }
            x -= 28;
        }
    }

    // the progress (or why nothing moves) on one line under the header
    const StoreService::Progress progress = store.progress();
    string line;
    if (progress.offline)
        line = _("Not connected");
    else if (!store.sourcesLoaded())
        line = _("Reading the sources...");
    else if (progress.busy) {
        line = stateText(progress.state) + ": " + progress.title;
        if (progress.total > 0)
            line += "  " + to_string(progress.done * 100 / progress.total) + "%";
        if (progress.waiting > 0)
            line += "  (+" + to_string(progress.waiting) + " " + _("waiting") + ")";
    }
    gui->text().renderText_WithColor(fonts[FONT_15_BOLD], line, panel.x + RowInset + 8, y + 4, style.secondary,
                                     XALIGN_LEFT);
    y += 30;

    // the list, left; the details, right (not on the Sources tab)
    const bool pane = tab != Tab::Sources;
    const int listWidth = panel.w - (pane ? PaneWidth : 0);
    const int visible = visibleRows();
    if (rows.empty()) {
        gui->text().renderText_WithColor(fonts[FONT_22_MED],
                                         tab == Tab::Downloads ? _("Nothing is downloading") : _("Nothing here yet"),
                                         panel.x + RowInset + 8, y + 16, style.secondary, XALIGN_LEFT);
    }
    const bool withPictures = tab != Tab::Sources;
    for (int i = firstVisible; i < firstVisible + visible && i < static_cast<int>(rows.size()); i++) {
        const ableem::Rect row(panel.x + 1, y, listWidth - 2, RowHeight);
        if (i == selected)
            style.selection(renderer, row);
        int textX = panel.x + RowInset + 8;
        const StoreEntry *entry = entryFor(rows[i].key);
        if (withPictures) {
            const ableem::Rect box(textX, y + (RowHeight - Thumb) / 2, Thumb, Thumb);
            if (entry != nullptr) {
                drawPicture(*entry, box, true);
            } else {
                renderer.setDrawColor(style.secondary);
                renderer.drawRect(box);
            }
            textX += ThumbSpace;
        }
        const int textWidth = listWidth - (textX - panel.x) - RowInset - 8;
        gui->text().renderText_WithColor(fonts[FONT_22_MED],
                                         gui->text().elide(fonts[FONT_22_MED], rows[i].title, textWidth), textX, y + 6,
                                         i == selected ? style.text : style.secondary, XALIGN_LEFT);
        gui->text().renderText_WithColor(fonts[FONT_15_BOLD],
                                         gui->text().elide(fonts[FONT_15_BOLD], rows[i].detail, textWidth), textX,
                                         y + 34, style.secondary, XALIGN_LEFT);
        // the one downloading: how far, as a bar along the row's foot
        if (entry != nullptr && entry->state == StoreState::Downloading && progress.busy && progress.total > 0) {
            const int barWidth = textWidth;
            const int done = static_cast<int>(barWidth * min<uint64_t>(progress.done, progress.total) / progress.total);
            renderer.setDrawColor(style.secondary);
            renderer.fillRect(ableem::Rect(textX, y + RowHeight - 6, barWidth, 3));
            renderer.setDrawColor(style.text);
            renderer.fillRect(ableem::Rect(textX, y + RowHeight - 6, done, 3));
        }
        y += RowHeight;
    }
    const int markerX = panel.x + listWidth - RowInset;
    if (firstVisible > 0)
        style.scrollMarker(renderer, markerX, panel.y + HeaderHeight + 26, -1);
    if (firstVisible + visible < static_cast<int>(rows.size()))
        style.scrollMarker(renderer, markerX, panel.y + HeaderHeight + 30 + visible * RowHeight + 2, 1);
    if (pane)
        drawDetails(ableem::Rect(panel.x + listWidth, panel.y + HeaderHeight, PaneWidth,
                                 panel.h - HeaderHeight - FooterHeight));
    if (tab == Tab::Sources)
        gui->text().renderText_WithColor(fonts[FONT_15_BOLD], _("You are responsible for what your sources contain"),
                                         panel.x + RowInset + 8, panel.y + panel.h - FooterHeight - 26, style.hint,
                                         XALIGN_LEFT);

    // the footer: what Cross and Triangle do for this row
    vector<PanelStyle::HintItem> hints;
    const StoreEntry *entry = selectedEntry();
    if (entry != nullptr) {
        switch (entry->state) {
        case StoreState::Available:
            hints.push_back({{"X"}, _("Install")});
            break;
        case StoreState::UpdateAvailable:
            hints.push_back({{"X"}, _("Update")});
            break;
        case StoreState::Failed:
            hints.push_back({{"X"}, _("Retry")});
            break;
        case StoreState::Queued:
        case StoreState::Downloading:
            hints.push_back({{"X"}, _("Cancel")});
            break;
        default:
            break;
        }
    } else if (!rows.empty() && rows[selected].action) {
        hints.push_back({{"X"}, _("Add")});
    }
    hints.push_back({{"O"}, _("Back")});
    if (entry != nullptr && !entry->installedPath.empty() && entry->state != StoreState::Queued &&
        entry->state != StoreState::Downloading && entry->state != StoreState::Installing)
        hints.push_back({{"T"}, _("Remove")});
    if (!rows.empty() && rows[selected].remoteSource)
        hints.push_back({{"T"}, _("Remove this source")});
    hints.push_back({{"S"}, _("Refresh")});
    hints.push_back({{"L1", "R1"}, _("Tab")});
    style.footer(*gui, ableem::Rect(panel.x, panel.y + panel.h - FooterHeight, panel.w, FooterHeight), hints,
                 rows.empty() ? "" : to_string(selected + 1) + "/" + to_string(rows.size()), true);

    gui->text().setShadow(classicShadow);
    renderer.present();
}

//*******************************
// GuiStore::drawDetails
//*******************************
void GuiStore::drawDetails(const ableem::Rect &pane) {
    renderer.setDrawColor(style.secondary);
    renderer.fillRect(ableem::Rect(pane.x, pane.y + 16, 1, pane.h - 32));
    const StoreEntry *e = selectedEntry();
    if (e == nullptr)
        return;
    Fonts &fonts = gui->assets().themeFonts;
    const int x = pane.x + 24, width = pane.w - 48;
    int y = pane.y + 20;
    // the picture's room is kept while it is being looked for, so the facts do not jump when it arrives
    if (pictureFor(*e).valid() || pictures.pending(e->key)) {
        drawPicture(*e, ableem::Rect(x, y, width, PanePictureHeight), false);
        y += PanePictureHeight + 14;
    }
    for (const string &line : gui->text().wrapLines(fonts[FONT_22_MED], e->item.title, width)) {
        gui->text().renderText_WithColor(fonts[FONT_22_MED], line, x, y, style.text, XALIGN_LEFT);
        y += 30;
    }
    y += 8;
    auto fact = [&](const string &label, const string &value) {
        if (value.empty())
            return;
        gui->text().renderText_WithColor(fonts[FONT_15_BOLD], label, x, y, style.secondary, XALIGN_LEFT);
        gui->text().renderText_WithColor(fonts[FONT_20_BOLD], gui->text().elide(fonts[FONT_20_BOLD], value, width), x,
                                         y + 18, style.text, XALIGN_LEFT);
        y += 50;
    };
    fact(_("Status"), stateText(e->state));
    fact(_("Version"), e->installedVersion.empty() || e->installedVersion == e->item.version
                           ? e->item.version
                           : e->installedVersion + " -> " + e->item.version);
    fact(_("Size"), sizeText(e->item.size()));
    fact(_("Author"), e->item.author);
    fact(_("Licence"), e->item.licence);
    fact(_("Source"), e->item.source);
    if (!e->item.description.empty())
        for (const string &line : gui->text().wrapLines(fonts[FONT_15_BOLD], e->item.description, width)) {
            if (y > pane.y + pane.h - 30)
                break;
            gui->text().renderText_WithColor(fonts[FONT_15_BOLD], line, x, y, style.secondary, XALIGN_LEFT);
            y += 22;
        }
}

//*******************************
// GuiStore::cross / triangle
//*******************************
void GuiStore::cross() {
    if (rows.empty())
        return;
    if (rows[selected].action) {
        GuiKeyboard keyboard(*gui);
        keyboard.label = _("Add a source URL");
        keyboard.result = "https://";
        keyboard.show();
        string error;
        if (!keyboard.cancelled && !store.addSourceUrl(keyboard.result, error)) {
            GuiConfirm message(*gui);
            message.label = error;
            message.show();
        }
        return;
    }
    const StoreEntry *e = selectedEntry();
    if (e == nullptr)
        return;
    switch (e->state) {
    case StoreState::Available:
    case StoreState::UpdateAvailable:
    case StoreState::Failed:
        app.audio().cursor.play();
        store.enqueue(e->key);
        break;
    case StoreState::Queued:
    case StoreState::Downloading:
        app.audio().cancel.play();
        store.cancel(e->key);
        break;
    default:
        break;
    }
}

void GuiStore::triangle() {
    if (rows.empty())
        return;
    if (rows[selected].remoteSource) {
        store.removeSourceUrl(rows[selected].key);
        return;
    }
    const StoreEntry *e = selectedEntry();
    if (e == nullptr || e->installedPath.empty() || e->state == StoreState::Queued ||
        e->state == StoreState::Downloading || e->state == StoreState::Installing)
        return;
    GuiConfirm confirm(*gui);
    confirm.title = e->item.title;
    confirm.label = _("Remove it from this system?");
    confirm.show();
    if (!confirm.result)
        return;
    string error;
    if (!store.remove(e->key, error)) {
        GuiConfirm message(*gui);
        message.label = error;
        message.show();
    }
}

//*******************************
// GuiStore::loop
//*******************************
void GuiStore::loop() {
    menuVisible = true;
    while (menuVisible) {
        if (gui->platform().ticks() - lastReload > ReloadEvery)
            reload();
        render();
        Event e;
        while (gui->input().poll(e)) {
            if (e.type == Event::Type::Quit)
                menuVisible = false;
            switch (e.type) {
            case Event::Type::DpadDown:
            case Event::Type::DpadUp:
                if (gui->input().dpadUp()) {
                    app.audio().cursor.play();
                    moveSelection(-1);
                } else if (gui->input().dpadDown()) {
                    app.audio().cursor.play();
                    moveSelection(1);
                }
                break;
            case Event::Type::ButtonDown:
                if (e.button == Button::Cross) {
                    cross();
                    reload();
                } else if (e.button == Button::Triangle) {
                    triangle();
                    reload();
                } else if (e.button == Button::Square) {
                    app.audio().cursor.play();
                    store.refresh();
                } else if (e.button == Button::L1 || e.button == Button::R1) {
                    app.audio().cursor.play();
                    const int step = e.button == Button::L1 ? 3 : 1; // four tabs, round
                    tab = static_cast<Tab>((static_cast<int>(tab) + step) % 4);
                    selected = firstVisible = 0;
                    rows.clear();
                    reload();
                } else if (e.button == Button::L2) {
                    app.audio().cursor.play();
                    moveSelection(-visibleRows());
                } else if (e.button == Button::R2) {
                    app.audio().cursor.play();
                    moveSelection(visibleRows());
                } else if (e.button == Button::Circle) {
                    app.audio().cancel.play();
                    menuVisible = false;
                }
                break;
            default:
                break;
            }
        }
    }
}
