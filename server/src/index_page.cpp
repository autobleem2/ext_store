//
// The page at / - see the header. One self-contained page (no scripts, no outside files), refreshed every 30 s.
//
#include "index_page.h"

#include <cstdio>
#include <ctime>
#include <sstream>

using namespace std;

namespace {

string sizeText(uint64_t bytes) {
    char text[32];
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        snprintf(text, sizeof(text), "%.1f GB", mb / 1024.0);
    else
        snprintf(text, sizeof(text), "%.0f MB", mb < 1.0 ? 1.0 : mb);
    return text;
}

string timeText(time_t t) {
    char text[32];
    const tm *local = localtime(&t);
    if (local == nullptr)
        return "";
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", local);
    return text;
}

const char *const Style = R"(
:root { --bg: #f6f7f9; --panel: #fff; --text: #1d2330; --muted: #5d6679; --line: #dde1e8; --accent: #1f6feb;
        --bad: #c62828; --warn: #a15c00; --good: #2e7d32; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #11151c; --panel: #1a2029; --text: #e6e9ef; --muted: #9aa3b2; --line: #2b3340; --accent: #58a6ff;
          --bad: #ff7b72; --warn: #e3b341; --good: #56d364; } }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.45 system-ui, sans-serif; }
main { max-width: 1100px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { font-size: 22px; margin: 0 0 4px; } h2 { font-size: 17px; margin: 28px 0 8px; }
.muted { color: var(--muted); }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 12px 16px; }
code { font: 14px ui-monospace, monospace; background: var(--bg); padding: 2px 6px; border-radius: 4px;
       word-break: break-all; }
.table { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; background: var(--panel); border: 1px solid var(--line);
        border-radius: 8px; }
th, td { text-align: left; padding: 7px 10px; border-bottom: 1px solid var(--line); vertical-align: top; }
th { font-size: 13px; color: var(--muted); font-weight: 600; }
td.n { text-align: right; white-space: nowrap; }
.bad { color: var(--bad); } .warn { color: var(--warn); } .good { color: var(--good); }
a { color: var(--accent); text-decoration: none; } a:hover { text-decoration: underline; }
)";

} // namespace

string htmlEscape(const string &text) {
    string out;
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

string indexPage(const LanSnapshot &snapshot, const map<string, string> &checksums, const IndexPageFacts &facts) {
    int errors = 0, warnings = 0;
    for (const LanProblem &p : snapshot.problems)
        (p.error ? errors : warnings)++;
    uint64_t total = 0;
    for (const LanGame &g : snapshot.games)
        total += g.size();

    ostringstream h;
    h << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      << "<meta http-equiv=\"refresh\" content=\"30\"><title>" << htmlEscape(facts.name) << "</title><style>" << Style
      << "</style></head><body><main>";
    h << "<h1>" << htmlEscape(facts.name) << "</h1><p class=\"muted\">AutoBleem Store LAN server "
      << htmlEscape(facts.version) << " &middot; " << htmlEscape(facts.gamesDir) << "</p>";

    h << "<div class=\"card\"><p>In the AutoBleem Store: <b>Sources</b> &rarr; <b>Add a source URL</b>, and "
      << "enter</p><p><code>" << htmlEscape(facts.baseUrl) << "/store.tsv</code></p><p class=\"muted\">"
      << snapshot.games.size() << " games, " << sizeText(total) << " &middot; scanned "
      << htmlEscape(timeText(snapshot.scannedAt)) << " in " << static_cast<int>(snapshot.seconds * 1000 + 0.5)
      << " ms &middot; " << (errors ? "<span class=\"bad\">" : "<span class=\"good\">") << errors << " errors</span>, "
      << warnings << " warnings" << (facts.hashing ? " &middot; checksums still being worked out" : "")
      << " &middot; <a href=\"/rescan\">scan again</a></p></div>";

    h << "<h2>Problems</h2>";
    if (snapshot.problems.empty()) {
        h << "<p class=\"good\">None.</p>";
    } else {
        h << "<div class=\"table\"><table><tr><th></th><th>Where</th><th>What</th></tr>";
        for (const LanProblem &p : snapshot.problems)
            h << "<tr><td class=\"" << (p.error ? "bad\">error" : "warn\">warning") << "</td><td>"
              << htmlEscape(p.path.empty() ? "(the games folder)" : p.path) << "</td><td>" << htmlEscape(p.what)
              << "</td></tr>";
        h << "</table></div>";
    }

    h << "<h2>Served</h2>";
    if (snapshot.games.empty()) {
        h << "<p class=\"muted\">No games yet.</p>";
    } else {
        h << "<div class=\"table\"><table><tr><th>Title</th><th>Serial</th><th>Files</th><th class=\"n\">Size</th>"
          << "<th>Checksums</th></tr>";
        for (const LanGame &g : snapshot.games) {
            int summed = 0;
            for (const LanFile &f : g.files)
                summed += checksums.count(LanLibrary::checksumKey(f)) ? 1 : 0;
            h << "<tr><td>" << htmlEscape(g.title) << "<br><span class=\"muted\">" << htmlEscape(g.id)
              << "</span></td><td>" << (g.serial.empty() ? "<span class=\"warn\">none</span>" : htmlEscape(g.serial))
              << "</td><td>";
            for (const LanFile &f : g.files)
                h << (f.disc > 0 ? "Disc " + to_string(f.disc) + ": " : "") << "<a href=\"/files/"
                  << htmlEscape(LanLibrary::urlPath(f.relPath)) << "\">" << htmlEscape(f.name) << "</a><br>";
            h << "</td><td class=\"n\">" << sizeText(g.size()) << "</td><td>"
              << (summed == static_cast<int>(g.files.size()) ? "<span class=\"good\">ready</span>"
                                                             : to_string(summed) + "/" + to_string(g.files.size()))
              << "</td></tr>";
        }
        h << "</table></div>";
    }
    h << "</main></body></html>";
    return h.str();
}
