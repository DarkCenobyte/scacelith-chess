// "Saved games": the library page of the title menu (ui.h, LibrarySetup). The games of the pgn
// folder (game_archive.h) are listed on a worker thread, and listed again every few seconds while
// the page is open (a game saved meanwhile, a file dropped in the folder: only new or changed
// files are read again), newest first, with a filter by mode. The selected game shows its
// players, result, opening, every tag and its moves in figurine notation. Replay returns
// StartReplay, Analyse StartAnalysis; Delete asks first and deletes files of one game only; Open
// folder shows the folder in the system's file manager. Save as GIF (signed in to an online
// server) sends the game's PGN text to the server (POST /gif, game::OnlineSession::savePgnGif) and
// saves the animated GIF it draws to <app data>/gif/; the folder's line shows it being made, then
// its path and Open folder.
// The Analysis page is the same page in another mode: the same list and details (the same
// selection and filter), Analyse as its main action (StartAnalysis), Paste a PGN (the first game of
// the clipboard's text, checked with the PGN reader: StartAnalysis with its text), My online games
// when signed in (the online page's game history), and the folder's line says where to put PGN
// files; no Delete nor Save as GIF there.
// Same look as ui_coach.cpp; mirrored with im::flip / im::flipX in a right-to-left language.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_online_pages.h"
#include "ui_screens_game.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../chess/pgn.h"
#include "../coach/catalog.h"
#include "../coach/openings.h"
#include "../core/log.h"
#include "../game/game_archive.h"
#include "../game/online_session.h"
#include "../game/replay.h"
#include "../game/settings.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <future>
#include <iterator>
#include <unordered_map>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;
namespace archive = game::archive;

namespace {

TextStyle style(int face, float size, vec4 color, HAlign align = HAlign::Left, float tracking = 0.0f) {
    TextStyle st;
    st.face = face;
    st.size = size;
    st.color = color;
    st.align = align;
    st.tracking = tracking;
    return st;
}
std::string T(const char* key) { return i18n::tr(key); }
std::string L(const char* key) { return std::string(i18n::tr(key)) + "##" + key; }
std::string num(long long v) { return std::to_string(v); }

const char* const kDot = "  \xC2\xB7  ";      // " · " between the parts of a line
const char* const kEllipsis = "\xE2\x80\xA6";

// s cut at the end (whole characters, "…" added) to fit maxWidth in st.
std::string elide(const std::string& s, const TextStyle& st, float maxWidth) {
    if (gfx::textWidth(s, st) <= maxWidth) return s;
    std::u32string cps = uni::decode(s);
    size_t lo = 0, hi = cps.size();
    while (lo < hi) {  // the longest prefix that fits with the ellipsis
        size_t mid = (lo + hi + 1) / 2;
        if (gfx::textWidth(uni::encode(cps.substr(0, mid)) + kEllipsis, st) <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    std::u32string head = cps.substr(0, lo);
    while (!head.empty() && head.back() == U' ') head.pop_back();
    return uni::encode(head) + kEllipsis;
}
// Fits s: a little smaller first, then cut.
void fitOrElide(std::string& s, TextStyle& st, float maxWidth, float minScale = 0.85f) {
    st.size = gfx::fitSize(s, st, maxWidth, minScale);
    s = elide(s, st, maxWidth);
}

// ---- What the rows and the details say -----------------------------------------------------------
using archive::entryKey;
using archive::hasKey;
// The key of the entry's content: changes when its file changes.
std::string contentKey(const archive::Entry& e) {
    return entryKey(e) + "@" + num(e.fileTimeMs) + "@" + num(static_cast<long long>(e.fileSize));
}

// Filter of the list: 0 every game, then one mode each (their labels in the same order).
const archive::Mode kFilterModes[] = {archive::Mode::Play, archive::Mode::Coach, archive::Mode::HotSeat,
                                      archive::Mode::Direct, archive::Mode::Server, archive::Mode::Imported};
const char* const kFilterKeys[] = {"library.filter.all", "library.filter.play", "library.filter.coach", "library.filter.hotseat",
                                   "library.filter.direct", "library.filter.server", "library.filter.imported"};
constexpr int kFilterCount = 1 + int(std::size(kFilterModes));
static_assert(std::size(kFilterKeys) == size_t(kFilterCount), "one label per filter");
bool passes(int filter, archive::Mode m) { return filter <= 0 || filter >= kFilterCount || kFilterModes[filter - 1] == m; }
std::vector<std::string> filterLabels() {
    std::vector<std::string> labels;
    for (const char* key : kFilterKeys) labels.push_back(T(key));
    return labels;
}

std::string modeLabel(archive::Mode m) {
    switch (m) {
        case archive::Mode::Play: return T("library.mode.play");
        case archive::Mode::Coach: return T("library.mode.coach");
        case archive::Mode::HotSeat: return T("library.mode.hotseat");
        case archive::Mode::Direct: return T("library.mode.direct");
        case archive::Mode::Server: return T("library.mode.server");
        default: return T("library.mode.imported");
    }
}
// What kind of game: the mode of the game's own saves, the Event of the others ("Rated blitz
// game"), else "Imported".
std::string kindLabel(const archive::Entry& e) {
    if (e.mode != archive::Mode::Imported) return modeLabel(e.mode);
    std::string ev = e.tag("Event");
    return ev.empty() || ev == "?" ? modeLabel(e.mode) : ev;
}

std::string playerName(const archive::Entry& e, const char* tag) {
    std::string n = e.tag(tag);
    return n.empty() || n == "?" ? T("library.unknown_player") : n;
}

// "1–0", "0–1", "½–½"; "" for an unfinished game.
std::string resultText(const std::string& r) {
    if (r == "1-0") return "1\xE2\x80\x93" "0";
    if (r == "0-1") return "0\xE2\x80\x93" "1";
    if (r == "1/2-1/2") return "\xC2\xBD\xE2\x80\x93\xC2\xBD";
    return std::string();
}

bool localTime(std::time_t t, std::tm& out) {
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

// "2026-10-01  21:04" from the tags ("2026" when only the year is known), else the file's time.
std::string whenText(const archive::Entry& e) {
    const std::string d = e.date();
    std::string out;
    auto known = [&](size_t at, size_t n) {
        if (d.size() < at + n) return false;
        for (size_t i = at; i < at + n; ++i)
            if (d[i] < '0' || d[i] > '9') return false;
        return true;
    };
    if (known(0, 4)) {
        out = d.substr(0, 4);
        if (known(5, 2)) {
            out += "-" + d.substr(5, 2);
            if (known(8, 2)) out += "-" + d.substr(8, 2);
        }
        const std::string t = e.time();
        if (out.size() == 10 && t.size() >= 5 && t[0] >= '0' && t[0] <= '9' && t[2] == ':') out += "  " + t.substr(0, 5);
    } else if (e.fileTimeMs > 0) {
        std::tm tm{};
        if (localTime(std::time_t(e.fileTimeMs / 1000), tm)) {
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y-%m-%d  %H:%M", &tm);
            out = buf;
        }
    }
    return i18n::ltr(out);
}

// "10+5", "1:30+0" from a TimeControl tag ("300+3"); "" when untimed or unknown.
std::string timeControlText(const std::string& tag) {
    int64_t base = 0, inc = 0;
    if (tag.empty() || tag == "-" || tag == "?" || !chess::pgn::parseTimeControl(tag, base, inc) || base <= 0) return std::string();
    const int64_t seconds = base / 1000;
    std::string out = num(seconds / 60);
    if (seconds % 60) out += (seconds % 60 < 10 ? ":0" : ":") + num(seconds % 60);
    return i18n::ltr(out + "+" + num(inc / 1000));
}

std::string movesLabel(int plies) { return i18n::trn("library.moves", (plies + 1) / 2); }

// The opening in the interface language: the Opening (and Variation) tags of a file from
// elsewhere, else the coach's classification (the curated family and its variation; the lichess
// family of a rare opening, in English).
std::string openingFromTexts(const std::string& ref, const std::string& lang) {
    const coach::OpeningTexts& texts = coach::OpeningTexts::instance();
    std::string s = texts.arg(ref, "", lang, false);
    return s.empty() ? texts.arg(ref, "", "en", false) : s;
}
std::string classifyOpening(const chess::pgn::Record& rec) {
    chess::Game g;
    if (rec.plies.empty() || !rec.toGame(g)) return std::string();
    const coach::OpeningBook& book = coach::OpeningBook::instance();
    if (book.empty()) return std::string();
    const coach::OpeningState st = coach::classify(g, book);
    if (!st.standardStart) return std::string();
    const std::string& lang = i18n::language();
    std::string family, variation;
    if (st.family >= 0 && st.family < int(book.families().size()))
        family = openingFromTexts("family:" + book.families()[size_t(st.family)].id, lang);
    int v = -1, vPly = -1;
    for (const coach::OpeningSideLabels& side : st.side)
        if (side.variation >= 0 && side.variation < int(book.variations().size()) && side.variationPly > vPly &&
            book.variations()[size_t(side.variation)].family == st.family) {
            v = side.variation;
            vPly = side.variationPly;
        }
    if (v >= 0) variation = openingFromTexts("variation:" + book.variations()[size_t(v)].id, lang);
    if (family.empty()) family = st.rare;
    if (family.empty()) return std::string();
    return variation.empty() ? family : i18n::trf("library.opening_variation", {family, variation});
}
std::string openingOf(const archive::Entry& e, const chess::pgn::Record* loaded) {
    std::string tag = e.tag("Opening");
    if (!tag.empty() && tag != "?") {
        std::string var = e.tag("Variation");
        return var.empty() || var == "?" ? tag : i18n::trf("library.opening_variation", {tag, var});
    }
    if (!e.error.empty() || e.plies == 0) return std::string();
    if (loaded) return classifyOpening(*loaded);
    archive::LoadResult r = archive::load(e);
    return r.ok ? classifyOpening(r.record) : std::string();
}

// "1. e4 e5 2. ♘f3 ♘c6 … 1–0" (a number and its move never parted at the end of a line).
std::string movesText(const chess::pgn::Record& rec) {
    const chess::Position start = rec.startPosition();
    int number = std::max(1, start.fullmoveNumber());
    bool white = start.sideToMove() == chess::White;
    std::string out;
    for (size_t i = 0; i < rec.plies.size(); ++i) {
        if (white) {
            if (!out.empty()) out += "  ";
            out += num(number) + ".\xC2\xA0";
        } else if (i == 0) {
            out += num(number) + kEllipsis + "\xC2\xA0";
        } else {
            out += " ";
        }
        out += coach::figurineSan(rec.plies[i].san);
        if (!white) ++number;
        white = !white;
    }
    std::string res = resultText(rec.result);
    if (!res.empty()) out += (out.empty() ? "" : "  ") + res;
    return out;
}

// How the game ended, in the interface language ("" when the record does not say).
std::string reasonText(const chess::pgn::Record& rec) {
    std::string key = game::replay::endReasonKey(rec);
    if (!key.empty() && i18n::has(key.c_str())) return T(key.c_str());
    std::string term = rec.tag("Termination");
    if (rec.result == "*") return T("library.unfinished");
    if (term.empty() || term == "?" || term == "normal" || term == "Normal") return std::string();
    return term;
}

// The name of a tag in the interface language for the common ones, else as written in the file.
std::string tagLabel(const std::string& name) { return i18n::trOr("library.tag." + name, name); }

// ---- State ---------------------------------------------------------------------------------------
struct Listing {
    std::string folder;
    std::vector<archive::Entry> entries;
    archive::ListStats stats;
    uint64_t gen = 0;                  // LibraryState::gen when it was asked for
    uint64_t print = 0;                // listingPrint(): equal for the same games, files and errors
    bool unchanged = false;            // the same print as the listing shown: 'entries' left empty
};
// A 64-bit hash of all a listing holds but its counts of files read and cached (8 bytes a step).
uint64_t listingPrint(const Listing& l) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto val = [&h](uint64_t v) {
        h = (h ^ v) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
    };
    auto str = [&val](const std::string& s) {
        val(s.size());
        for (size_t i = 0; i < s.size(); i += 8) {
            uint64_t w = 0;
            std::memcpy(&w, s.data() + i, std::min<size_t>(8, s.size() - i));
            val(w);
        }
    };
    str(l.folder);
    val(uint64_t(l.stats.files));
    val(l.stats.truncated);
    str(l.stats.error);
    val(l.entries.size());
    for (const archive::Entry& e : l.entries) {
        str(e.path);
        str(e.file);
        val(uint64_t(e.index));
        val(uint64_t(e.games));
        val(e.fileSize);
        val(uint64_t(e.fileTimeMs));
        val(e.offset);
        val(e.length);
        val(uint64_t(e.line));
        val(uint64_t(e.column));
        val(e.tags.size());
        for (const chess::pgn::Tag& t : e.tags) {
            str(t.name);
            str(t.value);
        }
        val(uint64_t(e.plies));
        str(e.result);
        val(uint64_t(e.mode));
        str(e.error);
        val(e.fileError);
    }
    return h;
}
// 'shownPrint' is the print of the listing shown (0: none): when this one is the same, its entries
// are freed here rather than in a frame (up to kMaxListed games every kRelistSeconds).
Listing listFolder(std::string folder, const std::atomic<bool>* cancel, uint64_t gen, uint64_t shownPrint) {
    Listing l;
    l.folder = folder;
    l.gen = gen;
    l.entries = archive::list(folder, &l.stats, cancel);
    if (l.stats.cancelled) return l;
    l.print = listingPrint(l);
    if (shownPrint != 0 && l.print == shownPrint) {
        l.entries = std::vector<archive::Entry>();
        l.unchanged = true;
    }
    // The opening book and texts are built on first use (~40 ms): here rather than in a frame.
    coach::OpeningBook::instance();
    coach::OpeningTexts::instance();
    return l;
}

// A tag of the details pane, fitted to its column once (not measured again every frame).
struct TagLine {
    std::string label, value;
    float labelSize = 0.0f, valueSize = 0.0f;
    bool fen = false;
};

struct Details {
    std::string key;                   // contentKey of the entry shown
    bool ok = false;
    chess::pgn::Record record;
    std::string error;                 // English (the loader's)
    std::string moves;                 // figurine text
    std::string opening, reason;
    std::vector<TagLine> tags;         // fitted to tagsWidth
    float tagsWidth = -1.0f;
};

struct LibraryState {
    std::string folder;
    Listing listing;
    bool listed = false;               // a listing of 'folder' arrived
    bool truncatedShown = false;       // the "too many games" notice was shown since the page opened
    bool keyboard = false;             // im::keyboardMode() last frame
    std::atomic<bool> cancel{false};   // set by libraryShutdown()
    std::future<Listing> pending;
    double lastList = -1e9;            // im::time() of the last listing
    uint64_t gen = 0;                  // games deleted: a listing asked for before may still hold one
    int filter = 0;
    std::string selected;              // entryKey of the selected game
    float scroll = 0.0f, target = 0.0f;
    float dScroll = 0.0f, dTarget = 0.0f;   // the details' tags and moves
    bool confirmDelete = false;
    std::string deleting;              // entryKey of the game the confirmation is about
    std::string lastClick;
    double lastClickTime = -1e9;
    Details details;
    std::unordered_map<std::string, std::string> openings;  // contentKey -> opening ("" = none)
    int openingsGen = -1;              // i18n::generation() of 'openings'
    bool debugGif = false;             // debug::libraryGif(): press Save as GIF at the next chance
    bool debugClipboardSet = false;    // debug::setClipboard(): Paste a PGN reads debugClipboard
    std::string debugClipboard;
};
// Never destroyed: a static's destructor would join a listing still running at exit (after the
// objects it uses may be gone); libraryShutdown() stops it first instead.
LibraryState& lib() {
    static LibraryState* s = new LibraryState();
    return *s;
}

constexpr double kRelistSeconds = 2.0;

void requestListing(LibraryState& s) {
    if (s.pending.valid() || s.folder.empty()) return;  // no folder (the Analysis page without one)
    s.cancel = false;
    s.pending = std::async(std::launch::async, listFolder, s.folder, &s.cancel, s.gen, s.listed ? s.listing.print : 0);
}
// Takes the listing once it is ready (waiting up to waitMs for it).
void pollListing(LibraryState& s, int waitMs) {
    if (!s.pending.valid()) return;
    if (s.pending.wait_for(std::chrono::milliseconds(waitMs)) != std::future_status::ready) return;
    Listing l;
    try {
        l = s.pending.get();
    } catch (const std::exception& ex) {  // out of memory on a huge folder: shown as a folder error
        l.folder = s.folder;
        l.stats.error = ex.what();
        l.gen = s.gen;
    }
    // Asked for before a game was deleted, it may still list that game: listed again at once.
    if (l.folder == s.folder && !l.stats.cancelled && l.gen != s.gen) {
        requestListing(s);
        return;
    }
    s.lastList = im::time();
    if (l.folder != s.folder || l.stats.cancelled) return;  // the folder changed meanwhile, or stopped
    const size_t games = l.unchanged ? s.listing.entries.size() : l.entries.size();
    if (!s.listed || l.stats.read > 0 || games != s.listing.entries.size())
        LOGI("library: %d games in %d files (%d read, %d cached)%s%s", int(games), l.stats.files, l.stats.read,
             l.stats.cached, l.stats.error.empty() ? "" : ": ", l.stats.error.c_str());
    if (l.stats.truncated && !s.truncatedShown) {
        notify(i18n::trf("library.truncated", {num(archive::kMaxListed)}), 6.0f);
        s.truncatedShown = true;
    }
    if (l.unchanged) s.listing.stats = l.stats;  // the games shown are kept
    else s.listing = std::move(l);
    s.listed = true;
}

const archive::Entry* findEntry(const LibraryState& s, const std::string& key) {
    for (const archive::Entry& e : s.listing.entries)
        if (hasKey(e, key)) return &e;
    return nullptr;
}

void loadDetails(LibraryState& s, const archive::Entry& e) {
    const std::string key = contentKey(e);
    if (s.details.key == key) return;
    s.details = Details();
    s.details.key = key;
    s.dScroll = s.dTarget = 0.0f;
    if (!e.error.empty()) {
        s.details.error = e.error;
        return;
    }
    archive::LoadResult r = archive::load(e);
    if (!r.ok) {
        s.details.error = r.error;
        LOGW("library: %s, game %d: %s", e.file.c_str(), e.index + 1, r.error.c_str());
        return;
    }
    s.details.ok = true;
    s.details.record = std::move(r.record);
    s.details.moves = movesText(s.details.record);
    s.details.reason = reasonText(s.details.record);
    auto it = s.openings.find(key);
    s.details.opening = it != s.openings.end() ? it->second : openingOf(e, &s.details.record);
    s.openings[key] = s.details.opening;
}

// ---- One row of the list -------------------------------------------------------------------------
// White – Black, the result at the end; then the date, the kind of game, the moves, the opening.
void drawRow(const archive::Entry& e, const Rect& r, bool sel, float hoverT, const std::string* opening) {
    if (sel) {
        if (im::rtl()) gfx::fillH(r, withAlpha(gold, 0.04f), withAlpha(gold, 0.13f), 2.0f);
        else gfx::fillH(r, withAlpha(gold, 0.13f), withAlpha(gold, 0.04f), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.55f), 0.0f, 2.0f);
        gfx::diamond(vec2(im::flipX(r, r.x), r.cy()), 4.5f, goldBright);
    } else {
        im::rowHighlight(r, hoverT);
    }
    const bool bad = !e.error.empty();
    // The result (always left to right), or "unfinished".
    std::string res = bad ? std::string() : resultText(e.result);
    TextStyle rs = res.empty() ? style(font::FACE_ITALIC, 19.0f, muted, im::endAlign())
                               : style(font::FACE_TITLE, 22.0f, sel ? goldBright : gold, im::endAlign(), 0.06f);
    rs.dir = 0;
    if (res.empty() && !bad && e.result == "*") res = T("library.unfinished");
    float resW = res.empty() ? 0.0f : gfx::textWidth(res, rs) + 26.0f;
    const float nameBase = r.y + 28.0f, x0 = r.x + 22.0f, x1 = r.r() - 18.0f;
    if (!res.empty()) gfx::text(res, im::flipX(r, x1), nameBase, rs);
    // The players, White first in the reading direction, each name in its own direction.
    vec4 nameColor = sel ? goldBright : theme::mix(ivory, goldBright, hoverT * 0.5f);
    TextStyle ns = style(font::FACE_TEXT, 25.0f, nameColor, im::startAlign());
    if (bad) {
        std::string f = e.file;
        TextStyle fs = ns;
        fs.dir = 0;
        fitOrElide(f, fs, x1 - x0 - resW);
        gfx::text(f, im::flipX(r, x0), nameBase, fs);
    } else {
        std::string w = playerName(e, "White"), b = playerName(e, "Black");
        const std::string dash = " \xE2\x80\x93 ";
        float avail = x1 - x0 - resW;
        ns.size = gfx::fitSize(w + dash + b, ns, avail, 0.8f);
        float dashW = gfx::textWidth(dash, ns);
        float half = (avail - dashW) * 0.5f;
        // Each name gets half the room unless the other needs less.
        float ww = gfx::textWidth(w, ns), bw = gfx::textWidth(b, ns);
        float wMax = std::max(half, avail - dashW - std::min(bw, half)), bMax = std::max(half, avail - dashW - std::min(ww, half));
        w = elide(w, ns, wMax);
        b = elide(b, ns, bMax);
        float x = x0;
        x += gfx::text(w, im::flipX(r, x), nameBase, ns);
        TextStyle ds = ns;
        ds.color = withAlpha(muted, 0.9f);
        x += gfx::text(dash, im::flipX(r, x), nameBase, ds);
        gfx::text(b, im::flipX(r, x), nameBase, ns);
    }
    // The second line.
    std::string line;
    TextStyle ls = style(font::FACE_ITALIC, 19.0f, bad ? danger : (sel ? ivoryDim : muted), im::startAlign());
    if (bad) {
        line = T("library.unreadable");
    } else {
        line = whenText(e);
        auto add = [&](const std::string& part) {
            if (part.empty()) return;
            if (!line.empty()) line += kDot;
            line += part;
        };
        add(kindLabel(e));
        add(movesLabel(e.plies));
        if (opening) add(*opening);
    }
    line = elide(line, ls, x1 - x0);
    gfx::text(line, im::flipX(r, x0), r.y + 52.0f, ls);
}

// ---- The details of the selected game ------------------------------------------------------------
// Header (players, result, when, opening) from y; returns the y under it.
float detailsHeader(const archive::Entry& e, const Details& d, const Rect& col, float y) {
    const float x = col.x, w = col.w;
    auto line = [&](const std::string& s, TextStyle st, float base) {
        std::string t = s;
        fitOrElide(t, st, w - 4.0f);
        gfx::text(t, im::flipX(col, x + 2.0f), base, st);
    };
    // The players, each with the king of their colour and their rating.
    const char* kings[2] = {"\xE2\x99\x94", "\xE2\x99\x9A"};  // ♔ ♚
    const char* tags[2] = {"White", "Black"};
    const char* elos[2] = {"WhiteElo", "BlackElo"};
    for (int c = 0; c < 2; ++c) {
        TextStyle ks = style(font::FACE_TEXT, 26.0f, gold, im::startAlign());
        gfx::text(kings[c], im::flipX(col, x + 2.0f), y, ks);
        std::string name = playerName(e, tags[c]);
        std::string elo = e.tag(elos[c]);
        TextStyle es = style(font::FACE_ITALIC, 21.0f, muted, im::endAlign());
        float eloW = 0.0f;
        if (!elo.empty() && elo != "?" && elo != "-") {
            elo = i18n::ltr(elo);
            eloW = gfx::textWidth(elo, es) + 20.0f;
            gfx::text(elo, im::flipX(col, x + w - 2.0f), y, es);
        }
        TextStyle ns = style(font::FACE_TEXT, 26.0f, ivory, im::startAlign());
        fitOrElide(name, ns, w - 40.0f - eloW);
        gfx::text(name, im::flipX(col, x + 38.0f), y, ns);
        y += 36.0f;
    }
    y += 16.0f;
    // Result and how it ended.
    {
        std::string res = resultText(e.result);
        float rx = x + 2.0f;
        if (!res.empty()) {
            TextStyle rs = style(font::FACE_TITLE, 30.0f, goldBright, im::startAlign(), 0.06f);
            rs.dir = 0;
            rx += gfx::text(res, im::flipX(col, rx), y, rs) + 18.0f;
        }
        std::string how = d.ok ? d.reason : std::string();
        if (e.result == "*" && how.empty()) how = T("library.unfinished");
        std::string tail = how;
        std::string moves = movesLabel(e.plies);
        tail = tail.empty() ? moves : tail + kDot + moves;
        TextStyle hs = style(font::FACE_ITALIC, 22.0f, ivoryDim, im::startAlign());
        fitOrElide(tail, hs, x + w - rx);
        gfx::text(tail, im::flipX(col, rx), y, hs);
        y += 36.0f;
    }
    // When, the kind of game (the coach's level), where.
    {
        std::string meta = whenText(e);
        auto add = [&](const std::string& part) {
            if (part.empty()) return;
            if (!meta.empty()) meta += kDot;
            meta += part;
        };
        add(kindLabel(e));
        int level = e.coachLevel();
        if (e.mode == archive::Mode::Coach && level >= 0) {
            std::string key = "coach.level." + num(level) + ".name";
            add(i18n::has(key.c_str()) ? T(key.c_str()) : i18n::trf("library.coach_level", {num(level)}));
        }
        add(timeControlText(e.tag("TimeControl")));
        line(meta, style(font::FACE_ITALIC, 20.0f, muted, im::startAlign()), y);
        y += 32.0f;
    }
    if (!d.opening.empty()) {
        line(d.opening, style(font::FACE_ITALIC, 22.0f, ivoryDim, im::startAlign()), y);
        y += 32.0f;
    }
    return y;
}

// Tags and moves, scrolled inside 'area'. Returns the height of the content.
float detailsBody(const archive::Entry& e, Details& d, const Rect& area, const Rect& col, float scroll) {
    float y = area.y + 30.0f - scroll;
    const float x = col.x, w = col.w;
    if (!d.ok) {
        TextStyle es = style(font::FACE_ITALIC, 21.0f, danger, im::startAlign());
        std::string text = i18n::trf("library.error.game", {d.error});
        int n = gfx::textWrapped(text, im::flipX(col, x + 2.0f), y, w - 8.0f, es, 28.0f);
        return 30.0f + float(n) * 28.0f;
    }
    const float y0 = y;
    // The file, and the game's place in it when it holds several.
    {
        std::string file = i18n::ltr(e.file);
        std::string text = e.games > 1 ? i18n::trf("library.file_game", {file, num(e.index + 1), num(e.games)})
                                       : i18n::trf("library.file", {file});
        TextStyle fs = style(font::FACE_ITALIC, 19.0f, muted, im::startAlign());
        fitOrElide(text, fs, w - 4.0f, 0.8f);
        gfx::text(text, im::flipX(col, x + 2.0f), y, fs);
        y += 36.0f;
    }
    // The moves, in figurine notation, left to right whatever the language.
    im::sectionLabel(T("library.section.moves"), x, y, w);
    y += 38.0f;
    if (d.moves.empty()) {
        TextStyle es = style(font::FACE_ITALIC, 21.0f, muted, im::startAlign());
        gfx::text(T("library.no_moves"), im::flipX(col, x + 2.0f), y, es);
        y += 30.0f;
    } else {
        TextStyle ms = style(font::FACE_TEXT, 22.0f, ivory, HAlign::Left);
        ms.dir = 0;
        int n = gfx::textWrapped(d.moves, x + 2.0f, y, w - 8.0f, ms, 31.0f);
        y += float(n) * 31.0f;
    }
    y += 22.0f;
    // Every tag: its name (translated when common), its value.
    im::sectionLabel(T("library.section.tags"), x, y, w);
    y += 34.0f;
    const float nameW = std::min(210.0f, w * 0.36f);
    TextStyle ns = style(font::FACE_TITLE, 15.0f, withAlpha(gold, 0.85f), im::startAlign(), 0.12f);
    TextStyle vs = style(font::FACE_TEXT, 21.0f, ivoryDim, im::startAlign());
    // Fitted once per game and column width: a game can carry 128 tags of 2 KB each.
    if (d.tagsWidth != w) {
        d.tags.clear();
        d.tagsWidth = w;
        for (const chess::pgn::Tag& t : d.record.tags) {
            if (t.name == "ScacelithEnd") continue;  // shown as the reason
            TagLine line;
            line.label = uni::toUpper(tagLabel(t.name));
            TextStyle n = ns;
            n.size = gfx::fitSize(line.label, n, nameW - 14.0f, 0.75f);
            line.label = elide(line.label, n, nameW - 14.0f);
            line.labelSize = n.size;
            line.value = t.value;
            if (t.name == "Result") line.value = resultText(t.value).empty() ? t.value : resultText(t.value);
            else if (t.name == "ScacelithMode") line.value = modeLabel(archive::modeFromName(t.value));
            TextStyle v = vs;
            line.fen = t.name == "FEN";
            if (line.fen) {  // a set-up start position: left to right, smaller
                v.dir = 0;
                v.size = 18.0f;
            }
            fitOrElide(line.value, v, w - nameW - 4.0f, 0.8f);
            line.valueSize = v.size;
            d.tags.push_back(std::move(line));
        }
    }
    const float rowH = 29.0f;
    for (const TagLine& line : d.tags) {
        if (y + rowH > area.y && y - rowH < area.b()) {  // only the rows in view are drawn
            TextStyle n = ns;
            n.size = line.labelSize;
            gfx::text(line.label, im::flipX(col, x + 2.0f), y, n);
            TextStyle v = vs;
            v.size = line.valueSize;
            if (line.fen) v.dir = 0;
            gfx::text(line.value, im::flipX(col, x + nameW), y, v);
        }
        y += rowH;
    }
    return y - y0 + 10.0f;
}

// A centred message in an area: a heading in the title face (upper-cased) and an italic wrapped text
// under it.
void message(const Rect& area, const std::string& head, const std::string& text, vec4 headColor) {
    TextStyle hs = style(font::FACE_TITLE, 24.0f, headColor, HAlign::Center, 0.14f);
    TextStyle ts = style(font::FACE_ITALIC, 23.0f, ivoryDim, HAlign::Center);
    const float lineH = 32.0f, w = std::min(area.w - 40.0f, 860.0f);
    int lines = text.empty() ? 0 : gfx::wrapLineCount(text, w, ts);
    float h = 34.0f + float(lines) * lineH;
    float y = area.y + std::max(40.0f, (area.h - h) * 0.42f);
    std::string hd = uni::toUpper(head);
    hs.size = gfx::fitSize(hd, hs, w);
    gfx::text(hd, area.cx(), y, hs);
    gfx::diamond(vec2(area.cx(), y + 26.0f), 3.5f, withAlpha(gold, 0.7f));
    if (!text.empty()) gfx::textWrapped(text, area.cx(), y + 68.0f, w, ts, lineH);
}

// ---- Save as GIF -----------------------------------------------------------------------------------------
// The online server draws the GIF of the selected game from its PGN text (POST /gif): the record
// written again by the archive's writer with only the tags the server reads and without its
// comments, NAGs and clocks (the picture uses none of the rest, and an imported game's other tags
// could take the text past the server's 64 KiB or 128 tags), seen from the side of the player
// (the signed-in account or this computer's player named as Black: Black at the bottom), saved to
// <app data>/gif/ under the name of its date, players and server game number (never over a file).
std::string gifOwner(const archive::Entry& e) { return "library:" + contentKey(e); }

std::string gifPgn(const chess::pgn::Record& record) {
    chess::pgn::Record r = record;
    // The server's names, ratings and ending, and the reader's Chess960 start (the writer gives
    // Result, SetUp and FEN from the record itself).
    static const char* const kGifTags[] = {"White", "Black", "WhiteElo", "BlackElo", "Termination", "Variant"};
    r.tags.erase(std::remove_if(r.tags.begin(), r.tags.end(),
                                [](const chess::pgn::Tag& t) {
                                    return std::none_of(std::begin(kGifTags), std::end(kGifTags),
                                                        [&t](const char* name) { return t.name == name; });
                                }),
                 r.tags.end());
    r.comment.clear();
    for (chess::pgn::Ply& ply : r.plies) {
        ply.comment.clear();
        ply.nags.clear();
        ply.clockMs = ply.elapsedMs = -1;
    }
    return chess::pgn::write(r);
}

bool sameName(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
std::string gifOrientation(const chess::pgn::Record& r) {
    const std::string white = r.tag("White"), black = r.tag("Black");
    const game::OnlineSession& se = game::onlineSession();
    for (const std::string& me : {se.account().username, se.savedUsername(), game::settings().playerName})
        if (sameName(black, me) && !sameName(white, me)) return "black";
    return "white";
}

std::string gifFileOf(const archive::Entry& e, const chess::pgn::Record& r) {
    const std::time_t when =
        game::pgnGameStart(r.tag("Date"), r.tag("Time"), r.tag("UTCDate"), r.tag("UTCTime"), std::time_t(e.fileTimeMs / 1000));
    const uint64_t id = std::strtoull(r.tag("ScacelithGameId").c_str(), nullptr, 10);
    return game::gifFileName(when, r.tag("White"), r.tag("Black"), id);
}

// The GIF's line in place of the folder's, centred at y: being made (spinner), saved (its path and
// Open folder after it) or why not.
void gifLine(const game::GifSaver& gif, const Rect& p, float maxW, float y) {
    using Stage = game::GifSaver::Stage;
    if (gif.busy()) {
        TextStyle ms = style(font::FACE_ITALIC, 20.0f, ivoryDim, HAlign::Center);
        const std::string text = T("gif.making");
        ms.size = gfx::fitSize(text, ms, maxW - 60.0f, 0.8f);
        const float w = gfx::textWidth(text, ms);
        gfx::text(text, p.cx() + (im::rtl() ? -16.0f : 16.0f), y, ms);
        detail::onl::spinner(vec2(p.cx() + (im::rtl() ? 1.0f : -1.0f) * (w * 0.5f + 8.0f), y - 6.0f), 9.0f);
        return;
    }
    if (gif.stage() == Stage::Failed) {
        TextStyle es = style(font::FACE_ITALIC, 20.0f, danger, HAlign::Center);
        std::string text = game::gifErrorText(gif.error(), gif.retryAfterSec());
        es.size = gfx::fitSize(text, es, maxW, 0.75f);
        gfx::text(elide(text, es, maxW), p.cx(), y, es);
        return;
    }
    if (gif.stage() != Stage::Saved) return;
    TextStyle ls = style(font::FACE_ITALIC, kSmall, muted);
    const std::string open = T("library.open_folder");
    const float linkW = std::max(150.0f, gfx::textWidth(open, ls) + 36.0f), gap = 16.0f;
    TextStyle ts = style(font::FACE_ITALIC, 20.0f, ivoryDim, im::startAlign());
    const std::string lead = T("gif.saved") + " ";
    TextStyle ps = style(font::FACE_ITALIC, 20.0f, goldBright, im::startAlign());
    ps.dir = 0;
    const float leadW = gfx::textWidth(lead, ts);
    const std::string path = detail::onl::elideStart(gif.path(), ps, std::max(80.0f, maxW - linkW - gap - leadW));
    const float pathW = gfx::textWidth(path, ps), total = leadW + pathW + gap + linkW;
    // The note then its path in the reading direction, Open folder at the end.
    const float x0 = p.cx() - total * 0.5f;
    const Rect row(x0, y - 28.0f, total, 40.0f);
    gfx::text(lead, im::flipX(row, x0), y, ts);
    TextStyle pe = ps;
    pe.align = im::startAlign();
    gfx::text(path, im::flipX(row, x0 + leadW), y, pe);
    if (im::button(L("library.open_folder") + "##gif", im::flip(row, Rect(x0 + leadW + pathW + gap, y - 30.0f, linkW, 42.0f)),
                   im::ButtonKind::Quiet)) {
        const std::string& file = gif.path();
        const size_t cut = file.find_last_of("/\\");
        if (!plat::openInFileManager(cut == std::string::npos ? file : file.substr(0, cut + 1))) notify(T("library.open_failed"));
    }
}

// ---- The footer -------------------------------------------------------------------------------------------
// A button of the footer: from the start side, in the middle of the room the others leave, or from
// the end side (the end side's in their order on screen, the last one at the end).
enum class FooterSide { Start, Middle, End };
struct FooterButton {
    int id;
    std::string label;                 // with its id, as the button is drawn
    FooterSide side;
    Rect r;                            // laid out, before im::flip
};
// The narrowest width at which a button shows the label whole (shrunk as far as it goes), 'most'
// when even that one does not.
float fittingWidth(const std::string& label, float most) {
    if (!im::buttonLabelFits(label, most)) return most;
    float lo = 40.0f, hi = most;
    while (hi - lo > 1.0f) {
        const float mid = (lo + hi) * 0.5f;
        if (im::buttonLabelFits(label, mid)) hi = mid;
        else lo = mid;
    }
    return std::ceil(hi);
}
// Lays the buttons out in [x0, x1] at y, 'gap' apart: as wide as each other (236 at most, or as wide
// as they can be in a narrower window) when every label shows whole at that width; else each as
// wide as its label needs and the room left shared between them (a 5:4 window in a language of
// long labels); else (no room for that either) as wide as each other, their labels cut.
void layoutFooter(std::vector<FooterButton>& bs, float x0, float x1, float y, float h, float gap) {
    const size_t n = bs.size();
    if (n == 0) return;
    const float room = x1 - x0 - float(n - 1) * gap, most = 236.0f;
    const float uniform = std::min(most, std::floor(room / float(n)));
    std::vector<float> w(n, uniform);
    bool whole = true;
    for (const FooterButton& b : bs) whole = whole && im::buttonLabelFits(b.label, uniform);
    if (!whole) {
        float need = 0.0f;
        for (size_t i = 0; i < n; ++i) need += w[i] = fittingWidth(bs[i].label, most);
        if (need <= room) {
            const float extra = std::floor((room - need) / float(n));
            for (float& wi : w) wi = std::min(most, wi + extra);
        } else {
            std::fill(w.begin(), w.end(), uniform);
        }
    }
    float xs = x0, xe = x1;
    for (size_t i = 0; i < n; ++i)
        if (bs[i].side == FooterSide::Start) {
            bs[i].r = Rect(xs, y, w[i], h);
            xs += w[i] + gap;
        }
    for (size_t i = n; i-- > 0;)
        if (bs[i].side == FooterSide::End) {
            xe -= w[i];
            bs[i].r = Rect(xe, y, w[i], h);
            xe -= gap;
        }
    // The middle one centred in the room between the two groups, keeping its gaps.
    for (size_t i = 0; i < n; ++i)
        if (bs[i].side == FooterSide::Middle) {
            const float mw = std::min(w[i], xe - xs);
            bs[i].r = Rect((xs + xe) * 0.5f - mw * 0.5f, y, mw, h);
        }
}

// ---- Paste a PGN (the Analysis page) ---------------------------------------------------------------------
// The clipboard's text is untrusted: read with the PGN reader's caps, those of a server's PGN
// (4 MiB), and two games at most (enough to know there are several). The game analysed is the
// first one that reads without error and has a move; its own text (tags and movetext, the bytes
// the reader names) is handed over. A notice says why nothing is analysed (an empty clipboard,
// no game in it), or that the first of several games is.
bool pastedGame(const std::string& clip, std::string& pgnOut) {
    bool blank = true;
    for (char c : clip)
        if (!std::isspace(static_cast<unsigned char>(c))) blank = false;
    if (blank) {
        notify(T("analysis.menu.paste.empty"), 4.0f);
        return false;
    }
    chess::pgn::Limits limits;
    limits.maxBytes = archive::kMaxServerPgnBytes;
    limits.maxGames = 2;
    const chess::pgn::Result<chess::pgn::ParsedGame> r = chess::pgn::read(clip, limits);
    for (const chess::pgn::ParsedGame& g : r.games) {
        if (!g.ok() || g.record.plies.empty() || g.offset >= clip.size()) continue;
        pgnOut = clip.substr(g.offset, g.length);
        const bool several = r.games.size() > 1 || r.truncated;
        LOGI("analysis: a pasted game of %d plies (%d bytes)%s", int(g.record.plies.size()), int(pgnOut.size()),
             several ? ", the first of several" : "");
        if (several) notify(T("analysis.menu.paste.several"), 5.0f);
        return true;
    }
    // Sizes only: the text (and the reader's errors, which quote it) may be anything the player
    // copied, a password included.
    LOGI("analysis: no game in the pasted text (%d bytes, %d games read)", int(clip.size()), int(r.games.size()));
    notify(T("analysis.menu.paste.none"), 4.0f);
    return false;
}

}  // namespace

// ==== The page ===========================================================================================
namespace detail {
namespace {

// The Saved games page, or the Analysis page ('analysis': see the top of the file). onlineGames:
// set when My online games is pressed (Analysis page).
MenuAction gamesPage(LibrarySetup& setup, float t, bool opened, bool& back, bool analysis, bool& onlineGames) {
    LibraryState& s = lib();
    vec2 v = gfx::viewSize();
    MenuAction act = MenuAction::None;
    // The Analysis page shows without a folder of saved games too (a pasted PGN needs none).
    const bool hasFolder = !setup.folder.empty();

    // ---- The listing: at once when the page opens (a short wait spares a flash of "Reading"), then
    // again every few seconds.
    if (s.folder != setup.folder) {
        s.folder = setup.folder;
        s.listed = false;
        s.listing = Listing();
        s.selected.clear();
        s.details = Details();
    }
    if (s.openingsGen != i18n::generation()) {
        s.openings.clear();
        s.details.key.clear();
        s.openingsGen = i18n::generation();
    }
    if (opened) {
        s.confirmDelete = false;
        s.truncatedShown = false;
        s.scroll = s.target;
        // A GIF of a saved game made on an earlier visit is not shown again (one being made is).
        if (game::onlineSession().gif().owner().compare(0, 8, "library:") == 0) game::onlineSession().clearGif();
        requestListing(s);
        pollListing(s, 250);
    } else {
        pollListing(s, 0);
        if (!s.pending.valid() && im::time() - s.lastList > kRelistSeconds) requestListing(s);
    }

    // The games shown (filter), the selected one (kept by its key across listings).
    const std::vector<archive::Entry>& all = s.listing.entries;
    std::vector<int> shown;
    shown.reserve(all.size());
    for (int i = 0; i < int(all.size()); ++i)
        if (passes(s.filter, all[size_t(i)].mode)) shown.push_back(i);
    int sel = -1;
    for (int i = 0; i < int(shown.size()); ++i)
        if (hasKey(all[size_t(shown[size_t(i)])], s.selected)) sel = i;
    if (sel < 0 && !shown.empty()) {
        sel = 0;
        s.selected = entryKey(all[size_t(shown[0])]);
    }
    const archive::Entry* cur = sel >= 0 ? &all[size_t(shown[size_t(sel)])] : nullptr;
    if (cur) loadDetails(s, *cur);

    const bool folderError = hasFolder && s.listed && !s.listing.stats.error.empty();
    const bool emptyFolder = !hasFolder || (s.listed && !folderError && all.empty());
    const bool reading = hasFolder && !s.listed;

    dimBackground(t);
    float w = std::min(1480.0f, v.x - 80.0f), h = 940.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 14.0f, w, h);
    if (s.confirmDelete) im::pushBlock();
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T(analysis ? "analysis.menu.title" : "library.title"), p.cx(), p.y + 78.0f);
    im::pushId(analysis ? "analysis" : "library");

    // The list first in the reading direction (on the right in a right-to-left language).
    const float pad = 64.0f, gap = 72.0f;
    const float listW = std::floor((p.w - 2.0f * pad - gap) * 0.55f), detW = p.w - 2.0f * pad - gap - listW;
    const float lx = im::flip(p, Rect(p.x + pad, 0, listW, 0)).x;
    const float dx = im::flip(p, Rect(p.x + pad + listW + gap, 0, detW, 0)).x;
    const Rect lcol(lx, 0, listW, 0), dcol(dx, 0, detW, 0);
    const float top = p.y + 150.0f;
    // The footer's buttons (layoutFooter).
    const float bh = 58.0f, bgap = 20.0f;
    const float by = p.b() - 52.0f - bh;
    const float bottom = by - 76.0f;  // under the columns: the folder line, then the footer rule
    im::Id defaultFocus = 0;
    // 'open': the page's main action (Replay, Analyse on the Analysis page), also a double click or
    // Enter on the selected game.
    bool open = false, analyse = false, askDelete = false;

    if (reading || folderError || emptyFolder) {
        // ---- Nothing to list: reading, the folder cannot be read, or no game yet.
        Rect area(p.x + pad, top, p.w - 2.0f * pad, bottom - top);
        if (reading) message(area, T("library.reading"), std::string(), gold);
        else if (folderError) message(area, T("library.error.folder"), i18n::ltr(s.listing.stats.error), danger);
        else if (!analysis) message(area, T("library.empty.title"), T("library.empty.text"), goldBright);
        else message(area, T("analysis.menu.empty.title"), T(hasFolder ? "analysis.menu.empty.text" : "analysis.menu.empty.no_folder"), goldBright);
    } else {
        gfx::vline(p.cx() + (im::rtl() ? -1.0f : 1.0f) * (listW - detW) * 0.5f, top, bottom - 10.0f, withAlpha(gold, 0.12f));

        // ---- The list: heading with the count, the filter, the rows.
        im::sectionLabel(T("library.section.games"), lx, top + 8.0f, listW);
        {
            TextStyle cs = style(font::FACE_ITALIC, 21.0f, ivoryDim, im::endAlign());
            std::string count = i18n::trn("library.count", static_cast<long long>(shown.size()));
            TextStyle label = style(font::FACE_TITLE, kSection, gold, HAlign::Left, 0.2f);
            cs.size = gfx::fitSize(count, cs, listW - gfx::textWidth(T("library.section.games"), label) - 60.0f);
            float tw = gfx::textWidth(count, cs);
            Rect band(lx, top - 12.0f, listW, 26.0f);
            vec4 clear(0.05f, 0.043f, 0.039f, 0.0f), dark(0.05f, 0.043f, 0.039f, 0.9f);
            Rect fade = im::flip(band, Rect(band.r() - tw - 40.0f, band.y, 30.0f, band.h));
            if (im::rtl()) gfx::fillH(fade, dark, clear);
            else gfx::fillH(fade, clear, dark);
            gfx::fill(im::flip(band, Rect(band.r() - tw - 10.0f, band.y, tw + 10.0f, band.h)), dark);
            gfx::text(count, im::flipX(band, band.r()), top + 8.0f, cs);
        }
        int filter = s.filter;
        if (im::selectorRow(L("library.filter"), filter, filterLabels(), Rect(lx, top + 30.0f, listW, 50.0f)) && filter != s.filter) {
            s.filter = filter;
            s.selected.clear();
            s.target = s.scroll = 0.0f;
        }
        const Rect area(lx, top + 96.0f, listW, bottom - top - 96.0f);
        const float rowH = 68.0f;
        const int n = int(shown.size());
        if (n == 0) {
            TextStyle es = style(font::FACE_ITALIC, 22.0f, muted, HAlign::Center);
            gfx::textWrapped(T("library.empty.filter"), area.cx(), area.y + 60.0f, area.w - 40.0f, es, 30.0f);
        } else {
            // The selection: one row at a time with the arrows (the focus moves from row to row and
            // the selection follows it), a page with PageUp / PageDown, the ends with Home / End
            // (no other control of the page uses these keys). Delete asks to delete it (not on the
            // Analysis page, which deletes nothing).
            auto select = [&](int i, bool focus) {
                sel = i;
                s.selected = entryKey(all[size_t(shown[size_t(i)])]);
                if (focus) im::setFocus(im::makeId(s.selected));
                const float ry = float(i) * rowH;  // brought into view
                if (ry < s.target) s.target = ry;
                if (ry + rowH > s.target + area.h) s.target = ry + rowH - area.h;
            };
            {
                const int page = std::max(1, int(area.h / rowH) - 1);
                int moveTo = -1;
                if (im::keyPressed(plat::KEY_PAGEDOWN)) moveTo = std::min(n - 1, sel + page);
                if (im::keyPressed(plat::KEY_PAGEUP)) moveTo = std::max(0, sel - page);
                if (im::keyPressed(plat::KEY_HOME)) moveTo = 0;
                if (im::keyPressed(plat::KEY_END)) moveTo = n - 1;
                if (!analysis && im::keyPressed(plat::KEY_DELETE) && cur && !cur->fileError) askDelete = true;
                if (moveTo >= 0 && moveTo != sel) {
                    select(moveTo, true);
                    im::sound(Sound::Tick);
                }
            }
            const float contentH = float(n) * rowH;
            detail::onl::wheelScroll(s.scroll, s.target, area, contentH, rowH * 1.5f, opened);
            // Rows on screen, and one more on each side so that the arrows can reach them.
            int first = std::max(0, int(std::floor(s.scroll / rowH)) - 1);
            int last = std::min(n - 1, int(std::ceil((s.scroll + area.h) / rowH)));
            gfx::pushClip(area);
            // The keyboard focus shown again (a mouse user's first arrow press) on a selected row
            // scrolled out of view: the row comes back into view.
            const bool revealed = im::keyboardMode() && !s.keyboard;
            s.keyboard = im::keyboardMode();
            // The opening of a row is worked out when it first shows (a few a frame).
            int budget = 3;
            std::string newSel;
            bool clickedRow = false;
            auto row = [&](int i) {
                const archive::Entry& e = all[size_t(shown[size_t(i)])];
                const std::string key = entryKey(e);
                Rect r(lx, area.y + float(i) * rowH - s.scroll, listW, rowH - 6.0f);
                im::Item it = im::item(im::makeId(key), r);
                bool visible = r.b() > area.y && r.y < area.b();
                // The selection follows the keyboard focus, not the mouse passing over the rows.
                if (it.focused && im::keyboardMode() && key != s.selected) newSel = key;
                if (it.focused && revealed && key == s.selected && !visible) select(i, false);
                if (it.clicked) {
                    if (s.lastClick == key && im::time() - s.lastClickTime < 0.45) open = true;
                    s.lastClick = key;
                    s.lastClickTime = im::time();
                    if (key != s.selected) {
                        newSel = key;
                        clickedRow = true;
                    }
                } else if (it.activated && key == s.selected) {
                    open = true;  // Enter on the selected game
                }
                if (!visible) return;
                const std::string ck = contentKey(e);
                auto op = s.openings.find(ck);
                if (op == s.openings.end() && budget > 0 && e.error.empty()) {
                    --budget;
                    op = s.openings.emplace(ck, openingOf(e, nullptr)).first;
                }
                drawRow(e, r, key == s.selected, it.hoverT, op != s.openings.end() ? &op->second : nullptr);
            };
            for (int i = first; i <= last; ++i) row(i);
            // The selected row out of view keeps the keyboard focus registered, and its neighbours
            // too, so that Up / Down go to the next game rather than to the nearest row on screen.
            for (int i : {sel - 1, sel, sel + 1})
                if (sel >= 0 && i >= 0 && i < n && (i < first || i > last)) row(i);
            gfx::popClip();
            detail::onl::scrollDecor(area, s.scroll, contentH);
            if (!newSel.empty()) {
                for (int i = 0; i < n; ++i)
                    if (hasKey(all[size_t(shown[size_t(i)])], newSel)) select(i, false);
                im::sound(clickedRow ? Sound::Toggle : Sound::Tick);
            }
            if (sel >= 0) defaultFocus = im::makeId(s.selected);
            cur = sel >= 0 ? &all[size_t(shown[size_t(sel)])] : nullptr;
            if (cur) loadDetails(s, *cur);
        }

        // ---- The details of the selected game.
        im::sectionLabel(T("library.section.details"), dx, top + 8.0f, detW);
        if (cur && s.details.key == contentKey(*cur)) {
            float y = detailsHeader(*cur, s.details, dcol, top + 66.0f);
            gfx::hlineFade(dx, dx + detW, y - 10.0f, withAlpha(gold, 0.25f), 0.25f);
            Rect body(dx, y, detW, bottom - y);
            float contentH = 0.0f;
            {
                // The height is known after drawing: last frame's, kept with the details.
                static std::string measuredKey;
                static float measured = 0.0f;
                if (measuredKey != s.details.key) {
                    measuredKey = s.details.key;
                    measured = 0.0f;
                }
                float scroll = detail::onl::wheelScroll(s.dScroll, s.dTarget, body, measured, 31.0f * 3.0f, false);
                gfx::pushClip(body);
                contentH = detailsBody(*cur, s.details, body, dcol, scroll);
                gfx::popClip();
                measured = contentH;
                detail::onl::scrollDecor(body, scroll, contentH);
            }
        } else if (!cur) {
            TextStyle es = style(font::FACE_ITALIC, 22.0f, muted, im::startAlign());
            gfx::text(T("library.none_selected"), im::flipX(dcol, dx + 2.0f), top + 70.0f, es);
        }
    }

    // ---- The folder (or the selected game's GIF), then the footer.
    game::OnlineSession& se = game::onlineSession();
    const game::GifSaver& gif = se.gif();
    const bool detailsShown = cur && cur->error.empty() && s.details.key == contentKey(*cur) && s.details.ok;
    const std::string gifKey = cur ? gifOwner(*cur) : std::string();
    const bool gifMine = !analysis && cur && gif.owner() == gifKey;
    // Signed in to an online server, on this run or an earlier one (the online page resumes it).
    const bool signedIn = se.signedIn() || se.hasSavedSession();
    if (gifMine && gif.stage() != game::GifSaver::Stage::Idle) {
        gifLine(gif, p, p.w - 2.0f * pad, by - 44.0f);
        se.gifShown(gifKey);
    } else if (hasFolder) {
        // The folder; on the Analysis page, as the place where PGN files are put to be analysed.
        TextStyle fs = style(font::FACE_ITALIC, 19.0f, withAlpha(muted, 0.9f), HAlign::Center);
        std::string line = i18n::trf(analysis ? "analysis.menu.folder" : "library.folder", {i18n::ltr(s.folder)});
        fs.size = gfx::fitSize(line, fs, p.w - 2.0f * pad, 0.8f);
        if (gfx::textWidth(line, fs) > p.w - 2.0f * pad) line = elide(line, fs, p.w - 2.0f * pad);
        gfx::text(line, p.cx(), by - 44.0f, fs);
    }
    gfx::hlineFade(p.x + 40.0f, p.r() - 40.0f, by - 26.0f, withAlpha(gold, 0.25f), 0.3f);
    // The footer: Back, Open folder, Save as GIF in the middle, then Delete, Analyse (the same game
    // in the Analysis mode) and Replay on the Saved games page; Back, Open folder, Paste a PGN (in
    // the middle when My online games follows it, signed in), Analyse on the Analysis page.
    enum { kBack, kFolder, kGif, kDelete, kAnalyse, kReplay, kPaste, kOnline };
    std::vector<FooterButton> foot;
    auto add = [&](int id, const char* key, FooterSide side) { foot.push_back(FooterButton{id, L(key), side, Rect()}); };
    add(kBack, "common.back", FooterSide::Start);
    if (hasFolder) add(kFolder, "library.open_folder", FooterSide::Start);
    if (analysis) {
        add(kPaste, "analysis.menu.paste", signedIn ? FooterSide::Middle : FooterSide::End);
        if (signedIn) add(kOnline, "analysis.menu.online", FooterSide::End);
        add(kAnalyse, "analysis.menu.analyse", FooterSide::End);
    } else {
        add(kGif, "gif.save", FooterSide::Middle);
        add(kDelete, "library.delete", FooterSide::End);
        add(kAnalyse, "analysis.menu.analyse", FooterSide::End);
        add(kReplay, "library.replay", FooterSide::End);
    }
    layoutFooter(foot, p.x + pad, p.r() - pad, by, bh, bgap);
    auto at = [&](int id) {
        for (const FooterButton& b : foot)
            if (b.id == id) return im::flip(p, b.r);
        return Rect();
    };
    im::Id backId = im::makeId("##common.back");
    bool backPressed = im::button(L("common.back"), at(kBack), im::ButtonKind::Secondary);
    if (hasFolder && im::button(L("library.open_folder"), at(kFolder), im::ButtonKind::Secondary)) {
        archive::makeFolder(s.folder);
        if (!plat::openInFileManager(s.folder)) notify(T("library.open_failed"));
    }
    const bool canOpen = detailsShown;
    bool paste = false;
    bool canDelete = false;
    if (analysis) {
        paste = im::button(L("analysis.menu.paste"), at(kPaste), im::ButtonKind::Secondary);
        if (signedIn && im::button(L("analysis.menu.online"), at(kOnline), im::ButtonKind::Secondary)) onlineGames = true;
        if (im::button(L("analysis.menu.analyse"), at(kAnalyse), im::ButtonKind::Primary, canOpen)) open = true;
    } else {
        // Save as GIF, between Open folder and Delete: the server draws it, so it needs an account of
        // an online server signed in (the disabled button's tooltip says so), and one GIF at a time.
        const Rect gifR = at(kGif);
        bool pressed = false;
        if (!signedIn) {
            im::disabledButton(L("gif.save"), gifR, im::ButtonKind::Secondary, T("gif.sign_in_first"), p);
        } else if (gif.busy() && !gifMine) {
            im::disabledButton(L("gif.save"), gifR, im::ButtonKind::Secondary, T("gif.busy_other"), p);
        } else {
            pressed = im::button(L("gif.save"), gifR, im::ButtonKind::Secondary, detailsShown && !gif.busy());
        }
        const bool debugPress = s.debugGif && detailsShown && signedIn && !gif.busy();
        if (debugPress) s.debugGif = false;
        if ((pressed || debugPress) && detailsShown && signedIn && !gif.busy()) {
            net::GifOptions options;
            options.orientation = gifOrientation(s.details.record);
            const std::string file = gifFileOf(*cur, s.details.record);
            if (se.savePgnGif(gifKey, gifPgn(s.details.record), options, plat::appDataDirectory() + "gif/", file)) {
                LOGI("library: GIF of %s (game %d) as %s", cur->file.c_str(), cur->index + 1, file.c_str());
                if (debugPress) se.runMock(4000);  // the fake server's answer and the file, this frame
            }
        }
        // A game of a file of several games cannot be deleted from here: Delete says so when pressed
        // (a disabled button could not explain it).
        canDelete = cur && !cur->fileError;
        if (im::button(L("library.delete"), at(kDelete), im::ButtonKind::Secondary, canDelete)) askDelete = true;
        if (askDelete && canDelete && !cur->removable() && !s.confirmDelete) {
            notify(i18n::trn("library.delete.several", cur->games), 5.0f);
            askDelete = false;
        }
        analyse = im::button(L("analysis.menu.analyse"), at(kAnalyse), im::ButtonKind::Secondary, canOpen);
        if (im::button(L("library.replay"), at(kReplay), im::ButtonKind::Primary, canOpen)) open = true;
    }
    im::setDefaultFocus(defaultFocus ? defaultFocus : backId);
    im::popId();
    gfx::popAlpha();
    if (s.confirmDelete) im::popBlock();

    if (analysis && open) analyse = true;   // the Analysis page's main action
    if (!s.confirmDelete && canOpen && analyse) {
        setup.replay.path = cur->path;
        setup.replay.game = cur->index;
        setup.replay.pgn.clear();
        LOGI("%s: analyse %s, game %d", analysis ? "analysis" : "library", cur->file.c_str(), cur->index + 1);
        act = MenuAction::StartAnalysis;
    } else if (!s.confirmDelete && canOpen && open) {
        setup.replay.path = cur->path;
        setup.replay.game = cur->index;
        setup.replay.pgn.clear();
        LOGI("library: replay %s, game %d", cur->file.c_str(), cur->index + 1);
        act = MenuAction::StartReplay;
    }
    if (paste && act == MenuAction::None) {
        std::string pgn;
        if (pastedGame(s.debugClipboardSet ? s.debugClipboard : plat::clipboardText(), pgn)) {
            setup.replay.path.clear();
            setup.replay.game = 0;
            setup.replay.pgn = std::move(pgn);
            act = MenuAction::StartAnalysis;
        }
    }
    if (askDelete && canDelete && cur->removable() && !s.confirmDelete) {
        s.confirmDelete = true;
        s.deleting = s.selected;
    }
    if (s.confirmDelete) {
        const archive::Entry* e = findEntry(s, s.deleting);
        std::string players = e ? playerName(*e, "White") + " \xE2\x80\x93 " + playerName(*e, "Black") : std::string();
        int r = im::confirmDialog("##library.delete", T("library.delete.title"), i18n::trf("library.delete.text", {players}),
                                  T("library.delete.ok"), T("common.cancel"), true);
        if (r == 1 && e) {
            archive::RemoveResult rr = archive::remove(*e);
            if (rr.ok()) {
                LOGI("library: deleted %s", e->path.c_str());
                ++s.gen;
                const std::string path = e->path;
                // Off the list at once (the next listing agrees); the next game is selected.
                int at = -1;
                for (int i = 0; i < int(shown.size()); ++i)
                    if (all[size_t(shown[size_t(i)])].path == path) at = i;
                std::string next;
                if (at >= 0 && at + 1 < int(shown.size())) next = entryKey(all[size_t(shown[size_t(at + 1)])]);
                else if (at > 0) next = entryKey(all[size_t(shown[size_t(at - 1)])]);
                auto& list = s.listing.entries;
                list.erase(std::remove_if(list.begin(), list.end(), [&](const archive::Entry& x) { return x.path == path; }), list.end());
                s.selected = next;
                s.details = Details();
                if (!next.empty()) {
                    im::pushId("library");
                    im::setFocus(im::makeId(next));
                    im::popId();
                }
                notify(T("library.deleted"));
            } else {
                LOGW("library: cannot delete %s: %s", e->path.c_str(), rr.error.c_str());
                notify(T(rr.status == archive::RemoveStatus::Changed ? "library.delete.changed" : "library.delete.failed"));
            }
            s.lastList = -1e9;  // list again now
        } else if (r == 1) {
            notify(T("library.delete.changed"));
            s.lastList = -1e9;
        }
        if (r >= 0) s.confirmDelete = false;
    } else if (backPressed || im::consumeBack()) {
        if (!backPressed) im::sound(Sound::Back);
        back = true;
    }
    return act;
}

}  // namespace

MenuAction libraryPage(LibrarySetup& setup, float t, bool opened, bool& back) {
    bool onlineGames = false;
    return gamesPage(setup, t, opened, back, false, onlineGames);
}

MenuAction analysisPage(LibrarySetup& setup, float t, bool opened, bool& back, bool& onlineGames) {
    return gamesPage(setup, t, opened, back, true, onlineGames);
}

void libraryShutdown() {
    LibraryState& s = lib();
    if (!s.pending.valid()) return;
    s.cancel = true;  // the listing stops at its next file
    s.pending.wait();
    s.pending = std::future<Listing>();
}

}  // namespace detail

namespace debug {
void libraryGif() { lib().debugGif = true; }
void setClipboard(const std::string& text) {
    lib().debugClipboardSet = true;
    lib().debugClipboard = text;
}
}  // namespace debug

}  // namespace ui
