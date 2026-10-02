#include "coach/openings.h"

#include "core/embedded.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/json.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace coach {

namespace {

const char* const kTsvFiles[] = {"assets/coach/openings_data/a.tsv", "assets/coach/openings_data/b.tsv",
                                 "assets/coach/openings_data/c.tsv", "assets/coach/openings_data/d.tsv",
                                 "assets/coach/openings_data/e.tsv"};
const char* const kMetadata = "assets/coach/openings_data/openings.json";

// Words that keep a book line out of the teaching repertoire even inside a sound family: gambits, traps and the
// famous dubious or trick lines (checked on the name minus the family's own name).
const char* const kDubious[] = {"Gambit", "Countergambit", "Trap", "Sacrifice", "Swindle", "Fried Liver",
                                "Wayward Queen", "Napoleon", "Parham", "Halloween", "Traxler", "Blackburne Shilling",
                                "Jerome", "Kostić", "Damiano", "Hillbilly", "Stafford", "Frankenstein-Dracula",
                                "Kiddie", "Bongcloud", "Marshall Attack", "Schliemann", "Irish", "Toilet"};

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// A lichess name matches a prefix at a component boundary.
bool matchesPrefix(const std::string& name, const std::string& p) {
    if (!startsWith(name, p)) return false;
    if (name.size() == p.size()) return true;
    return name.compare(p.size(), 2, ": ") == 0 || name.compare(p.size(), 2, ", ") == 0;
}

std::vector<std::string> splitComponents(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t a = s.find(", ", i), b = s.find(": ", i);
        size_t j = std::min(a, b);
        out.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 2;
    }
    return out;
}

// Move notation inside a lichess component ("with e3", "Bb4+ Line", "O-O Line") is never spoken.
bool hasMoveNotation(const std::string& c) {
    if (c.find("O-O") != std::string::npos) return true;
    for (size_t i = 0; i + 1 < c.size(); ++i)
        if (c[i] >= 'a' && c[i] <= 'h' && c[i + 1] >= '1' && c[i + 1] <= '8') return true;
    return false;
}

OpeningSide parseSide(const std::string& s) {
    return s == "black" ? OpeningSide::Black : s == "both" ? OpeningSide::Both : OpeningSide::White;
}

bool sideOwns(OpeningSide s, chess::Color mover) {
    return s == OpeningSide::Both || (s == OpeningSide::White) == (mover == chess::White);
}

OpeningSide sideOf(chess::Color c) { return c == chess::White ? OpeningSide::White : OpeningSide::Black; }

std::vector<std::string> stringList(const net::json::Value& v) {
    std::vector<std::string> out;
    for (const net::json::Value& x : v.items())
        if (x.isString()) out.push_back(x.asString());
    return out;
}

net::json::Limits metadataLimits() {
    net::json::Limits l;
    l.maxBytes = 4u << 20;
    l.maxElements = 1000000;
    return l;
}

}  // namespace

// ---- OpeningBook ---------------------------------------------------------------------------------------------------
const OpeningBook& OpeningBook::instance() {
    static const OpeningBook book = [] {
        OpeningBook b;
        std::vector<std::string> files;
        for (const char* f : kTsvFiles) files.push_back(embedded::text(f));
        std::string error;
        if (!b.build(files, embedded::text(kMetadata), &error)) {
            LOGE("Opening book: %s", error.c_str());
            b = OpeningBook();
        } else {
            LOGI("Opening book: %u lines, %u positions, %.1f ms", unsigned(b.rows()), unsigned(b.positions()), b.buildMs());
        }
        return b;
    }();
    return book;
}

bool OpeningBook::parseMetadata(const std::string& text, std::string* error) {
    net::json::Value root;
    if (!net::json::parse(text, root, error, metadataLimits())) return false;
    families_.clear();
    variations_.clear();
    std::vector<std::string> umbrellas;
    for (const net::json::Value& f : root["families"].items()) {
        OpeningFamily F;
        F.id = f["id"].asString();
        F.lichess = stringList(f["lichess"]);
        F.side = parseSide(f["side"].asString());
        F.tier = int(f["tier"].asInt(3));
        F.generic = f["generic"].asBool();
        F.answer = f["answer"].asString();
        F.structure = f["structure"].asString();
        F.character = f["character"].asString();
        if (F.id.empty() || F.lichess.empty()) {
            if (error) *error = "family without id or lichess prefixes";
            return false;
        }
        umbrellas.push_back(f["umbrella"].asString());
        families_.push_back(std::move(F));
    }
    for (size_t i = 0; i < families_.size(); ++i) {
        if (umbrellas[i].empty()) continue;
        families_[i].umbrella = familyIndex(umbrellas[i]);
        if (families_[i].umbrella < 0) {
            if (error) *error = "unknown umbrella family " + umbrellas[i];
            return false;
        }
    }
    for (const net::json::Value& v : root["variations"].items()) {
        OpeningVariation V;
        V.id = v["id"].asString();
        V.family = familyIndex(v["family"].asString());
        V.side = parseSide(v["side"].asString());
        V.lichess = stringList(v["lichess"]);
        V.beginnerComment = v["beginner"].asBool();
        if (V.id.empty() || V.family < 0 || V.lichess.empty()) {
            if (error) *error = "bad notable variation " + V.id;
            return false;
        }
        variations_.push_back(std::move(V));
    }
    ignored_ = stringList(root["ignore_components"]);
    if (families_.empty()) {
        if (error) *error = "no opening families";
        return false;
    }
    return true;
}

int OpeningBook::longestPrefix(const std::string& name, bool variations, int* prefixLength) const {
    int best = -1, bestLen = -1;
    auto consider = [&](int idx, const std::vector<std::string>& prefixes) {
        for (const std::string& p : prefixes)
            if (int(p.size()) > bestLen && matchesPrefix(name, p)) {
                best = idx;
                bestLen = int(p.size());
            }
    };
    if (variations)
        for (size_t i = 0; i < variations_.size(); ++i) consider(int(i), variations_[i].lichess);
    else
        for (size_t i = 0; i < families_.size(); ++i) consider(int(i), families_[i].lichess);
    if (prefixLength) *prefixLength = bestLen;
    return best;
}

int OpeningBook::familyOf(const std::string& lichessName) const { return longestPrefix(lichessName, false, nullptr); }
int OpeningBook::variationOf(const std::string& lichessName) const { return longestPrefix(lichessName, true, nullptr); }

int OpeningBook::familyIndex(const std::string& id) const {
    for (size_t i = 0; i < families_.size(); ++i)
        if (families_[i].id == id) return int(i);
    return -1;
}

int OpeningBook::variationIndex(const std::string& id) const {
    for (size_t i = 0; i < variations_.size(); ++i)
        if (variations_[i].id == id) return int(i);
    return -1;
}

bool OpeningBook::build(const std::vector<std::string>& tsvFiles, const std::string& metadataJson, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    *this = OpeningBook();
    if (!parseMetadata(metadataJson, error)) return false;

    // Replay every line and note each position it passes through.
    struct Visit {
        uint64_t hash;
        int32_t row;
        bool last;
    };
    std::vector<Visit> visits;
    visits.reserve(40000);
    std::vector<int> rowName;
    std::map<std::string, int> nameIndex;
    for (const std::string& text : tsvFiles) {
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || startsWith(line, "eco\t")) continue;
            size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) {
                ++bad_;
                continue;
            }
            const std::string name = line.substr(t1 + 1, t2 - t1 - 1);
            const std::string pgn = line.substr(t2 + 1);
            const int row = int(rowName.size());
            auto it = nameIndex.find(name);
            if (it == nameIndex.end()) {
                it = nameIndex.emplace(name, int(names_.size())).first;
                names_.push_back(name);
            }
            rowName.push_back(it->second);
            chess::Position p;
            size_t first = visits.size();
            size_t i = 0;
            bool ok = true;
            while (i < pgn.size()) {
                size_t j = pgn.find(' ', i);
                if (j == std::string::npos) j = pgn.size();
                std::string tok = pgn.substr(i, j - i);
                i = j + 1;
                // Move numbers ("1.", "12.") first: parseSAN("1.") would only answer an invalid move.
                if (tok.empty() || tok.back() == '.') continue;
                const chess::Move m = p.parseSAN(tok);
                if (!m.valid()) {
                    ++bad_;
                    ok = false;
                    break;
                }
                p.makeMove(m);
                visits.push_back({p.hash(), row, false});
            }
            if (!ok || visits.size() == first) {
                visits.resize(first);
                continue;
            }
            visits.back().last = true;
        }
    }
    rows_ = rowName.size();

    // Classify every name once.
    nameFamily_.assign(names_.size(), -1);
    nameVariation_.assign(names_.size(), -1);
    nameFamilyPrefix_.assign(names_.size(), 0);
    nameVariationPrefix_.assign(names_.size(), 0);
    std::vector<uint8_t> teachTier(names_.size(), 0);   // 0 = not for teaching, else the family tier
    for (size_t n = 0; n < names_.size(); ++n) {
        const std::string& nm = names_[n];
        nameFamily_[n] = longestPrefix(nm, false, &nameFamilyPrefix_[n]);
        nameVariation_[n] = longestPrefix(nm, true, &nameVariationPrefix_[n]);
        const int f = nameFamily_[n];
        if (f < 0) continue;
        const OpeningFamily& F = families_[f];
        if (F.character == "gambit") continue;
        // The family's own name may hold "Gambit" (Queen's Gambit Declined): check the rest only. A name that
        // reached the family through another prefix (Vienna Gambit under Vienna Game) is checked whole.
        std::string rest = matchesPrefix(nm, F.lichess.front()) ? nm.substr(F.lichess.front().size()) : nm;
        bool dubious = false;
        for (const char* w : kDubious)
            if (rest.find(w) != std::string::npos) dubious = true;
        if (!dubious) teachTier[n] = uint8_t(F.tier);
    }

    // One entry per distinct position.
    std::sort(visits.begin(), visits.end(), [](const Visit& a, const Visit& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.row < b.row;
    });
    entries_.reserve(8000);
    for (size_t i = 0; i < visits.size();) {
        size_t j = i;
        Entry e;
        e.hash = visits[i].hash;
        int lastRow = -1;
        int teach1 = 0, teach2 = 0;
        for (; j < visits.size() && visits[j].hash == e.hash; ++j) {
            const Visit& v = visits[j];
            if (v.last) {
                if (e.name >= 0 && e.name != rowName[v.row]) ++dupNamed_;
                if (e.name < 0) e.name = rowName[v.row];
            } else {
                e.flags |= 1;
            }
            if (v.row != lastRow) {   // count each line once, even if it passed twice
                lastRow = v.row;
                const uint8_t t = teachTier[rowName[v.row]];
                if (t == 1) ++teach1;
                if (t == 1 || t == 2) ++teach2;
            }
        }
        // A position whose own name is not a teaching line (a gambit named inside a sound family, such as the
        // Greco Gambit of the Giuoco Piano) is not one either, whatever sound lines pass through it.
        if (e.name >= 0) {
            const uint8_t t = teachTier[e.name];
            if (t == 0 || t > 1) teach1 = 0;
            if (t == 0 || t > 2) teach2 = 0;
        }
        e.teach[0] = uint16_t(std::min(teach1, 65535));
        e.teach[1] = uint16_t(std::min(teach2, 65535));
        if (e.name >= 0) {
            ++named_;
            if (!(e.flags & 1)) ++leaves_;
        }
        entries_.push_back(e);
        i = j;
    }
    buildMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (entries_.empty()) {
        if (error) *error = "no opening lines";
        return false;
    }
    return true;
}

const OpeningBook::Entry* OpeningBook::find(uint64_t hash) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), hash,
                               [](const Entry& e, uint64_t h) { return e.hash < h; });
    return it != entries_.end() && it->hash == hash ? &*it : nullptr;
}

bool OpeningBook::lookup(uint64_t hash, Hit* out) const {
    const Entry* e = find(hash);
    if (out) *out = e ? Hit{e->name, (e->flags & 1) != 0} : Hit{};
    return e != nullptr;
}

const std::string& OpeningBook::name(int index) const {
    static const std::string none;
    return index >= 0 && index < int(names_.size()) ? names_[index] : none;
}

int OpeningBook::familyOfName(int n) const { return n >= 0 && n < int(nameFamily_.size()) ? nameFamily_[n] : -1; }
int OpeningBook::variationOfName(int n) const {
    return n >= 0 && n < int(nameVariation_.size()) ? nameVariation_[n] : -1;
}

int OpeningBook::teachingLines(uint64_t hash, int maxTier) const {
    const Entry* e = find(hash);
    if (!e) return 0;
    return maxTier <= 1 ? e->teach[0] : e->teach[1];
}

std::vector<std::string> OpeningBook::extraComponents(int n) const {
    std::vector<std::string> out;
    if (n < 0 || n >= int(names_.size()) || nameFamily_[n] < 0) return out;
    const std::string& nm = names_[n];
    // The notable variation's prefix when it belongs to the same family, else the family's.
    int v = nameVariation_[n];
    size_t prefix = (v >= 0 && variations_[v].family == nameFamily_[n]) ? size_t(nameVariationPrefix_[n])
                                                                         : size_t(nameFamilyPrefix_[n]);
    if (prefix >= nm.size()) return out;
    for (const std::string& c : splitComponents(nm.substr(prefix + 2))) {
        if (c.empty() || startsWith(c, "with ") || hasMoveNotation(c)) continue;
        if (std::find(ignored_.begin(), ignored_.end(), c) != ignored_.end()) continue;
        out.push_back(c);
    }
    return out;
}

std::string OpeningBook::lichessFamily(const std::string& nm) {
    size_t p = nm.find(": ");
    if (p == std::string::npos) p = nm.find(", ");
    return nm.substr(0, p);
}

// ---- Classification --------------------------------------------------------------------------------------------------
OpeningState classify(const chess::Game& game, const OpeningBook& book, int maxPly) {
    OpeningState st;
    const std::vector<chess::Move>& moves = game.moves();
    st.plies = int(moves.size());
    chess::Position pos = game.startPosition();
    if (pos.hash() != chess::Position().hash()) {
        st.standardStart = false;
        st.inBook = false;
        return st;
    }
    const std::vector<OpeningFamily>& fams = book.families();
    const std::vector<OpeningVariation>& vars = book.variations();
    auto push = [&st](OpeningEvent::Kind kind, int ply, OpeningSide owner) -> OpeningEvent& {
        OpeningEvent e;
        e.kind = kind;
        e.ply = ply;
        e.owner = owner;
        st.events.push_back(e);
        return st.events.back();
    };
    int announced = -1;                  // family of the latest Family / Answer / Transposed event
    std::vector<std::string> prevComps;  // extra components of the latest named position of the family
    int outRun = 0;
    for (size_t i = 0; i < moves.size(); ++i) {
        const chess::Color mover = pos.sideToMove();
        pos.makeMove(moves[i]);
        const int ply = int(i) + 1;
        OpeningBook::Hit hit;
        const bool inBook = book.lookup(pos.hash(), &hit);
        if (inBook && hit.name >= 0) {
            st.lastNamed = hit.name;
            st.lastNamedPly = ply;
            const int f = book.familyOfName(hit.name);
            if (f < 0) {
                st.rare = OpeningBook::lichessFamily(book.name(hit.name));
            } else {
                // A generic name (King's Pawn Game, Indian Defense) never replaces a specific family.
                const bool specific = st.family >= 0 && !fams[st.family].generic;
                if (f != st.family && !(fams[f].generic && specific)) {
                    // Another family: the variation and deep labels belonged to the old one.
                    for (OpeningSideLabels& s : st.side) {
                        s.variation = s.variationPly = -1;
                        s.deep.clear();
                        s.deepPly = -1;
                    }
                    prevComps.clear();
                    st.family = f;
                }
                if (f == st.family) {
                    const int v = book.variationOfName(hit.name);
                    if (v >= 0 && vars[v].family == f) {
                        const chess::Color owner = vars[v].side == OpeningSide::Black ? chess::Black : chess::White;
                        OpeningSideLabels& lab = st.side[owner];
                        if (lab.variation != v) {   // the latest notable wins, it is never cleared by a vaguer name
                            lab.variation = v;
                            lab.variationPly = ply;
                            OpeningEvent& e = push(OpeningEvent::Kind::Variation, ply, vars[v].side);
                            e.family = f;
                            e.variation = v;
                        }
                    }
                    // Deeper lichess components: credited to the side whose move produced them.
                    std::vector<std::string> comps = book.extraComponents(hit.name);
                    std::string fresh;
                    for (const std::string& c : comps)
                        if (std::find(prevComps.begin(), prevComps.end(), c) == prevComps.end()) fresh = c;
                    if (!fresh.empty()) {
                        OpeningSideLabels& lab = st.side[mover];
                        lab.deep = fresh;
                        lab.deepPly = ply;
                        OpeningEvent& e = push(OpeningEvent::Kind::Deep, ply, sideOf(mover));
                        e.family = f;
                        e.deep = fresh;
                    }
                    prevComps = std::move(comps);
                }
            }
        }
        // A family is credited to its owner only once the owner has moved inside it: 1.d4 Nf6 2.c4 g6 3.Nc3 is
        // named "King's Indian Defense", but Black may still choose the Grünfeld with 3...d5.
        if (st.family >= 0 && sideOwns(fams[st.family].side, mover)) {
            const OpeningFamily& F = fams[st.family];
            if (F.generic) {
                OpeningSideLabels& lab = st.side[mover];
                if (lab.generic != st.family) {
                    lab.generic = st.family;
                    lab.genericPly = ply;
                }
            } else if (announced != st.family) {
                OpeningEvent::Kind kind = OpeningEvent::Kind::Family;
                if (announced >= 0)
                    kind = F.umbrella >= 0 && (F.umbrella == announced || st.side[chess::White].family == F.umbrella)
                               ? OpeningEvent::Kind::Answer
                               : OpeningEvent::Kind::Transposed;
                push(kind, ply, F.side).family = st.family;
                announced = st.family;
                auto credit = [&](chess::Color c, int fam) {
                    if (st.side[c].family != fam) {
                        st.side[c].family = fam;
                        st.side[c].familyPly = ply;
                    }
                };
                if (F.side == OpeningSide::Both) {
                    credit(chess::White, st.family);
                    credit(chess::Black, st.family);
                } else {
                    credit(F.side == OpeningSide::White ? chess::White : chess::Black, st.family);
                }
                // White's opening becomes the one Black answers: the Queen's Gambit when Black declines it.
                if (F.umbrella >= 0) credit(chess::White, F.umbrella);
            }
        }
        if (inBook) {
            outRun = 0;
        } else if (++outRun == 2) {
            st.leftBookPly = ply - 1;
            push(OpeningEvent::Kind::LeftBook, ply - 1, sideOf(chess::opposite(mover)));
        }
        if (!st.determined) {
            DeterminedBy by = DeterminedBy::None;
            if (outRun >= 2)
                by = DeterminedBy::OutOfBook;
            else if (inBook && hit.name >= 0 && !hit.hasDeeper)
                by = DeterminedBy::NamedLeaf;
            else if (ply >= maxPly)
                by = DeterminedBy::PlyLimit;
            if (by != DeterminedBy::None) {
                st.determined = true;
                st.determinedPly = ply;
                st.determinedBy = by;
                push(OpeningEvent::Kind::Determined, ply, sideOf(mover)).by = by;
            }
        }
    }
    st.inBook = outRun == 0;
    return st;
}

// ---- Announcer -------------------------------------------------------------------------------------------------------
int openingPlyLimit(int level) {
    switch (level) {
    case 0: case 1: case 2: return 8;
    case 3: return 12;
    case 4: return 16;
    case 5: return 20;
    default: return 30;
    }
}

namespace {
int utteranceCap(int level) { return level <= 2 ? 2 : level <= 4 ? 3 : 4; }

OpeningArg openingArg(const std::string& ref) {
    OpeningArg a;
    a.kind = OpeningArg::Kind::Opening;
    a.text = ref;
    return a;
}
}  // namespace

struct OpeningAnnouncer::Plan {
    bool summary = false;            // the sentence naming both sides' openings
    std::string white, black, both;  // side labels (references), "" = none
    std::string relation;            // "declined" / "accepted" (Black's family answers White's umbrella)
    std::string transposed;          // later: a new family ("By transposition, we are now in ...")
    std::string varWhite, varBlack;  // notable variations mentioned besides the labels
    std::string deep;                // lichess component (level 6)
    bool leftBook = false;
    int leftBookMove = 0;
    bool rare = false;
    std::string rareName;            // lichess family of a rare opening, when the languages can say it
    std::string comment;             // comment key
    std::vector<std::string> names;  // "name:<ref>" keys this plan mentions
    int since = 0;                   // ply at which the newest item appeared
    bool empty() const {
        return !summary && transposed.empty() && varWhite.empty() && varBlack.empty() && deep.empty() && !leftBook &&
               !rare;
    }
};

OpeningAnnouncer::OpeningAnnouncer(const OpeningBook& book) : book_(&book) {}

void OpeningAnnouncer::setLevel(int level) { level_ = std::max(0, std::min(6, level)); }
void OpeningAnnouncer::setHumanColor(chess::Color c) { human_ = c; }
void OpeningAnnouncer::setLanguages(const std::string& subtitle, const std::string& speech) {
    subtitleLang_ = subtitle.empty() ? "en" : subtitle;
    speechLang_ = speech.empty() ? "en" : speech;
}

void OpeningAnnouncer::newGame() {
    said_.clear();
    summarySaid_ = false;
    takebackSinceTalk_ = false;
    utterances_ = 0;
    lastTalkPly_ = -100;
    lastPlies_ = 0;
    lastHash_ = 0;
}

void OpeningAnnouncer::reset() {
    newGame();
    commentsSaid_.clear();
}

OpeningAnnouncer::Plan OpeningAnnouncer::plan(const OpeningState& st) const {
    Plan p;
    const std::vector<OpeningFamily>& fams = book().families();
    const std::vector<OpeningVariation>& vars = book().variations();
    auto famRef = [&](int f) { return "family:" + fams[f].id; };
    auto varRef = [&](int v) { return "variation:" + vars[v].id; };
    auto said = [&](const std::string& k) { return said_.count(k) != 0; };
    auto varNameable = [&](int v) { return v >= 0 && (level_ >= 3 || (level_ == 2 && vars[v].beginnerComment)); };
    const OpeningTexts& texts = OpeningTexts::instance();
    auto speakable = [&](const std::string& ref) {
        return !texts.arg(ref, "", subtitleLang_, false).empty() && !texts.arg(ref, "", speechLang_, true).empty();
    };

    // Each side's label: its specific family, else a notable variation that stands for it (the Open Sicilian for
    // White), else its generic first move. At levels 1-2 the coach never names its own tier-3 family.
    int labelFamily[2] = {-1, -1}, labelVariation[2] = {-1, -1};
    std::string label[2];
    for (chess::Color c : {chess::White, chess::Black}) {
        const OpeningSideLabels& L = st.side[c];
        const bool coachSide = c != human_;
        if (L.family >= 0 && !(level_ <= 2 && coachSide && fams[L.family].tier >= 3)) {
            labelFamily[c] = L.family;
            label[c] = famRef(L.family);
        } else if (varNameable(L.variation)) {
            labelVariation[c] = L.variation;
            label[c] = varRef(L.variation);
        } else if (L.generic >= 0) {
            label[c] = famRef(L.generic);
        }
    }
    const int wf = labelFamily[chess::White], bf = labelFamily[chess::Black];
    if (wf >= 0 && wf == bf && fams[wf].side == OpeningSide::Both) {
        p.both = label[chess::White];
    } else {
        p.white = label[chess::White];
        p.black = label[chess::Black];
        if (bf >= 0 && wf >= 0 && fams[bf].umbrella == wf && !fams[bf].answer.empty()) p.relation = fams[bf].answer;
    }
    auto isNew = [&](const std::string& ref) { return !ref.empty() && !said("name:" + ref); };
    auto newsworthy = [&](chess::Color c) {   // a specific family or a variation label nobody named yet
        return (labelFamily[c] >= 0 && !fams[labelFamily[c]].generic && isNew(label[c])) ||
               (labelVariation[c] >= 0 && isNew(label[c]));
    };
    auto plyOfLabel = [&](chess::Color c) {
        return labelFamily[c] >= 0 ? st.side[c].familyPly
               : labelVariation[c] >= 0 ? st.side[c].variationPly
                                        : st.side[c].genericPly;
    };

    std::vector<int> subjects;   // candidates for the comment, best first: >= 0 family, < 0 variation (-1 - v)
    if (!summarySaid_) {
        p.summary = !p.white.empty() || !p.black.empty() || !p.both.empty();
        p.since = st.determinedPly;
        if (!p.summary && !st.rare.empty() && !said("rare")) {
            p.rare = true;
            if (level_ >= 3 && speakable("line:" + st.rare)) p.rareName = st.rare;
        }
    } else {
        const bool newWhite = newsworthy(chess::White), newBlack = newsworthy(chess::Black);
        const bool newBoth = !p.both.empty() && isNew(p.both);
        if (newWhite || newBlack || newBoth) {
            if (level_ >= 3 && !takebackSinceTalk_) {
                // A transposition: name the family that changed last (a new variation label is handled below).
                const bool famW = newWhite && labelFamily[chess::White] >= 0;
                const bool famB = newBlack && labelFamily[chess::Black] >= 0;
                if (newBoth) {
                    p.transposed = p.both;
                    p.since = std::max(p.since, plyOfLabel(chess::White));
                } else if (famW || famB) {
                    const chess::Color c =
                        famB && (!famW || plyOfLabel(chess::Black) >= plyOfLabel(chess::White)) ? chess::Black
                                                                                                  : chess::White;
                    p.transposed = label[c];
                    p.since = std::max(p.since, plyOfLabel(c));
                }
            } else {
                // Levels 1-2, or after a takeback: both openings again, with the new names.
                p.summary = true;
                p.since = std::max({p.since, newWhite ? plyOfLabel(chess::White) : 0,
                                    newBlack ? plyOfLabel(chess::Black) : 0, newBoth ? plyOfLabel(chess::White) : 0});
            }
        }
    }
    if (p.summary) {
        for (const std::string& r : {p.white, p.black, p.both})
            if (!r.empty()) p.names.push_back("name:" + r);
    }
    if (!p.transposed.empty()) p.names.push_back("name:" + p.transposed);

    // Notable variations besides the labels (with the first summary, or as news afterwards).
    for (chess::Color c : {chess::White, chess::Black}) {
        const int v = st.side[c].variation;
        if (!varNameable(v) || labelVariation[c] == v) continue;
        const std::string ref = varRef(v);
        if (said("name:" + ref) || (!summarySaid_ && !p.summary)) continue;
        (c == chess::White ? p.varWhite : p.varBlack) = ref;
        p.names.push_back("name:" + ref);
        if (summarySaid_) p.since = std::max(p.since, st.side[c].variationPly);
    }
    // A variation that became a side's label after the summary (level >= 3, no family for that side).
    if (summarySaid_ && !p.summary)
        for (chess::Color c : {chess::White, chess::Black})
            if (labelVariation[c] >= 0 && isNew(label[c])) {
                (c == chess::White ? p.varWhite : p.varBlack) = label[c];
                p.names.push_back("name:" + label[c]);
                p.since = std::max(p.since, st.side[c].variationPly);
            }

    // Level 6: the newest deeper lichess component, in neutral wording.
    if (level_ >= 6) {
        chess::Color c = st.side[chess::Black].deepPly > st.side[chess::White].deepPly ? chess::Black : chess::White;
        const std::string& d = st.side[c].deep;
        if (!d.empty() && !said("deep:" + d) && speakable("line:" + d)) {
            p.deep = d;
            if (summarySaid_) p.since = std::max(p.since, st.side[c].deepPly);
        }
    }
    // Levels 4+: leaving the book is information.
    if (level_ >= 4 && st.leftBookPly >= 0 && !said("left_book")) {
        p.leftBook = true;
        p.leftBookMove = (st.leftBookPly + 1) / 2;
        if (summarySaid_) p.since = std::max(p.since, st.leftBookPly + 1);
    }
    if (p.empty()) return p;

    // One comment on what the opening leads to: the newest notable variation that has a comment for the level,
    // else the family named now (the current family first, then Black's, then White's; a generic one last).
    const bool advanced = level_ >= 4;
    auto addVar = [&](const std::string& ref) {
        if (ref.empty()) return;
        int v = book().variationIndex(ref.substr(ref.find(':') + 1));
        if (v >= 0 && (advanced || vars[v].beginnerComment)) subjects.push_back(-1 - v);
    };
    auto addFam = [&](const std::string& ref) {
        if (ref.rfind("family:", 0) != 0) return;
        int f = book().familyIndex(ref.substr(7));
        if (f >= 0) subjects.push_back(f);
    };
    if (level_ >= 2) {
        addVar(p.varBlack);
        addVar(p.varWhite);
        if (p.summary) {
            addVar(p.black);
            addVar(p.white);
        }
    }
    std::vector<std::string> famRefs;
    if (!p.transposed.empty()) famRefs.push_back(p.transposed);
    if (p.summary) {
        if (!p.both.empty()) famRefs.push_back(p.both);
        std::string cur = st.family >= 0 ? famRef(st.family) : "";
        for (const std::string& r : {p.white, p.black})
            if (r == cur) famRefs.push_back(r);
        famRefs.push_back(p.black);
        famRefs.push_back(p.white);
    }
    for (const std::string& r : famRefs)
        if (!r.empty() && book().familyIndex(r.substr(r.find(':') + 1)) >= 0 &&
            !fams[book().familyIndex(r.substr(r.find(':') + 1))].generic)
            addFam(r);
    if (p.summary)   // a generic opening still has something to teach (1.e4, 1.d4)
        for (const std::string& r : {p.white, p.black}) addFam(r);
    for (int s : subjects) {
        std::string key = s >= 0 ? "opening.family." + fams[s].id + (advanced ? ".advanced" : ".beginner")
                                 : "opening.variation." + vars[-1 - s].id + (advanced ? ".advanced" : ".beginner");
        if (!commentsSaid_.count(key) && texts.find("en", key)) {
            p.comment = key;
            break;
        }
    }
    return p;
}

std::vector<OpeningLine> OpeningAnnouncer::lines(const Plan& p, bool first) const {
    std::vector<OpeningLine> out;
    auto line = [&out](const std::string& key) -> OpeningLine& {
        out.push_back(OpeningLine{key, {}});
        return out.back();
    };
    const std::string pv = human_ == chess::White ? "opening.say.pw." : "opening.say.pb.";
    if (p.summary) {
        if (!p.both.empty()) {
            line("opening.say.both").args.push_back({"both", openingArg(p.both)});
        } else if (!p.white.empty() && !p.black.empty()) {
            // White's label may be a variation of Black's defence (the Advance French): the defence comes first,
            // so the variation name ("the Advance Variation") is heard in its context.
            const bool whiteVariation = p.white.rfind("variation:", 0) == 0;
            OpeningLine& l =
                line(pv + (!p.relation.empty() ? p.relation : whiteVariation ? std::string("var_white") : "both"));
            l.args.push_back({"white", openingArg(p.white)});
            if (p.relation.empty()) l.args.push_back({"black", openingArg(p.black)});
        } else if (!p.white.empty()) {
            line(pv + "white").args.push_back({"white", openingArg(p.white)});
        } else if (!p.black.empty()) {
            line(pv + "black").args.push_back({"black", openingArg(p.black)});
        }
    }
    if (!p.transposed.empty()) line("opening.say.transposed").args.push_back({"opening", openingArg(p.transposed)});
    if (p.rare) {
        if (p.rareName.empty()) line("opening.say.rare");
        else line("opening.say.rare_named").args.push_back({"line", openingArg("line:" + p.rareName)});
    }
    if (!p.varWhite.empty() && !p.varBlack.empty()) {
        OpeningLine& l = line("opening.say.variations");
        l.args.push_back({"white", openingArg(p.varWhite)});
        l.args.push_back({"black", openingArg(p.varBlack)});
    } else if (!p.varWhite.empty() || !p.varBlack.empty()) {
        const std::string& v = p.varWhite.empty() ? p.varBlack : p.varWhite;
        line(first || p.summary ? "opening.say.variation" : "opening.say.now_variation")
            .args.push_back({"variation", openingArg(v)});
    }
    if (!p.deep.empty()) line("opening.say.deep").args.push_back({"line", openingArg("line:" + p.deep)});
    if (!p.comment.empty()) line(p.comment);
    if (p.leftBook) {
        if (level_ >= 5) {
            OpeningArg n;
            n.kind = OpeningArg::Kind::Number;
            n.number = p.leftBookMove;
            line("opening.say.left_book_move").args.push_back({"n", n});
        } else {
            line("opening.say.left_book");
        }
    }
    return out;
}

void OpeningAnnouncer::markSaid(const Plan& p) {
    for (const std::string& n : p.names) said_.insert(n);
    if (!p.deep.empty()) said_.insert("deep:" + p.deep);
    if (p.leftBook) said_.insert("left_book");
    if (p.rare) said_.insert("rare");
    if (!p.comment.empty()) commentsSaid_.insert(p.comment);
    if (p.summary || p.rare) summarySaid_ = true;
}

std::vector<OpeningLine> OpeningAnnouncer::update(const chess::Game& game, bool canSpeak) {
    if (level_ <= 0) return {};
    const OpeningState st = classify(game, book(), openingPlyLimit(level_));
    if (!st.standardStart) return {};
    // A takeback: fewer plies, or a move replaced (taken back and another played before this update).
    if (st.plies < lastPlies_ || (lastPlies_ > 0 && game.positionAt(size_t(lastPlies_)).hash() != lastHash_)) {
        takebackSinceTalk_ = true;
        if (lastTalkPly_ > st.plies) lastTalkPly_ = -100;
    }
    lastPlies_ = st.plies;
    lastHash_ = game.position().hash();
    if (!st.determined || utterances_ >= utteranceCap(level_)) return {};
    Plan p = plan(st);
    if (p.empty()) return {};
    if (summarySaid_ && st.plies - p.since > 4) {   // stale news: never said (its comment stays for later)
        Plan q = p;
        q.comment.clear();
        markSaid(q);
        return {};
    }
    if (st.plies < lastTalkPly_ + 2 || !canSpeak) return {};
    std::vector<OpeningLine> out = lines(p, !summarySaid_);
    markSaid(p);
    ++utterances_;
    lastTalkPly_ = st.plies;
    takebackSinceTalk_ = false;
    return out;
}

void OpeningAnnouncer::catchUp(const chess::Game& game) {
    if (level_ <= 0) return;
    const OpeningState st = classify(game, book(), openingPlyLimit(level_));
    lastPlies_ = st.plies;
    lastHash_ = game.position().hash();
    lastTalkPly_ = st.plies;
    takebackSinceTalk_ = false;
    if (!st.standardStart || !st.determined) return;
    // Everything the game has shown so far counts as said, including what later news would repeat.
    for (int i = 0; i < 4; ++i) {
        Plan p = plan(st);
        if (p.empty()) break;
        markSaid(p);
    }
    summarySaid_ = true;
}

std::vector<OpeningLine> OpeningAnnouncer::summary(const chess::Game& game) const {
    OpeningAnnouncer fresh(book());
    fresh.level_ = std::max(1, level_);
    fresh.human_ = human_;
    fresh.subtitleLang_ = subtitleLang_;
    fresh.speechLang_ = speechLang_;
    OpeningState st = classify(game, book(), openingPlyLimit(fresh.level_));
    if (!st.standardStart) return {};
    st.determined = true;   // at the end of the game, whatever the opening has become is the opening
    Plan p = fresh.plan(st);
    if (!p.summary) return {};
    Plan only;
    only.summary = true;
    only.white = p.white;
    only.black = p.black;
    only.both = p.both;
    only.relation = p.relation;
    return fresh.lines(only, true);
}

// ---- Texts -------------------------------------------------------------------------------------------------------------
const OpeningTexts& OpeningTexts::instance() {
    static const OpeningTexts texts = [] {
        OpeningTexts t;
        std::string error;
        if (!t.load(&error)) LOGE("Opening texts: %s", error.c_str());
        return t;
    }();
    return texts;
}

bool OpeningTexts::load(std::string* error) {
    langs_.clear();
    bool ok = true;
    for (const i18n::Language& l : i18n::languages()) {
        const std::string path = std::string("assets/coach/openings/") + l.code + ".lang";
        if (!embedded::find(path.c_str())) {
            if (error && ok) *error = "missing " + path;
            ok = false;
            continue;
        }
        std::vector<std::pair<std::string, std::string>> kv;
        std::string e;
        if (!i18n::parse(embedded::text(path.c_str()), kv, &e)) {
            if (error && ok) *error = path + ": " + e;
            ok = false;
        }
        std::map<std::string, std::string>& m = langs_[l.code];
        for (auto& p : kv) m[p.first] = p.second;
    }
    net::json::Value root;
    std::string e;
    if (!net::json::parse(embedded::text(kMetadata), root, &e, metadataLimits())) {
        if (error && ok) *error = std::string(kMetadata) + ": " + e;
        return false;
    }
    respell_.clear();
    for (const auto& m : root["tts_en_respell"].members()) respell_.push_back({m.first, m.second.asString()});
    std::sort(respell_.begin(), respell_.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& m : root["compose_types"].members()) composeTypes_[m.first] = m.second.asString();
    for (const auto& m : root["components"].members()) components_[m.first] = m.second.asString();
    for (const auto& m : root["eponyms"].members()) eponyms_[m.first] = m.second.asString();
    for (const std::string& w : stringList(root["common_words"])) commonWords_.insert(w);
    return ok;
}

const std::string* OpeningTexts::find(const std::string& lang, const std::string& key) const {
    auto l = langs_.find(lang);
    if (l == langs_.end()) return nullptr;
    auto it = l->second.find(key);
    return it == l->second.end() ? nullptr : &it->second;
}

int OpeningTexts::variants(const std::string& key) const {
    if (!find("en", key)) return 0;
    int n = 1;
    while (find("en", key + "." + std::to_string(n + 1))) ++n;
    return n;
}

namespace {
void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
}

// First code point of a UTF-8 string at byte i.
uint32_t codepointAt(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return c;
    if ((c >> 5) == 6 && i + 1 < s.size()) return ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
    if ((c >> 4) == 14 && i + 2 < s.size()) return ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
    return 0;
}

bool upperLetter(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 0x0410 && cp <= 0x042F) || cp == 0x0401 || cp == 0x0404 ||
           cp == 0x0406 || cp == 0x0407 || cp == 0x0490;
}
}  // namespace

std::string fixNameDashesForSpeech(const std::string& text) {
    static const std::string dash = " \xE2\x80\x94 ";   // " — "
    std::string s = text;
    for (size_t p = 0; (p = s.find(dash, p)) != std::string::npos;) {
        size_t wordStart = s.rfind(' ', p == 0 ? 0 : p - 1);
        wordStart = wordStart == std::string::npos ? 0 : wordStart + 1;
        const size_t next = p + dash.size();
        if (p > 0 && next < s.size() && upperLetter(codepointAt(s, wordStart)) && upperLetter(codepointAt(s, next))) {
            s.replace(p, dash.size(), "-");
            p += 1;
        } else {
            p = next;
        }
    }
    return s;
}

bool OpeningTexts::properName(const std::string& words) const {
    std::string w;
    auto ok = [&](const std::string& word) {
        if (word.empty() || commonWords_.count(word)) return false;
        const unsigned char c = static_cast<unsigned char>(word[0]);
        if (c < 0x80 && !(c >= 'A' && c <= 'Z')) return false;   // lower case or a digit: not a proper name
        if (word.size() >= 2 && (word.compare(word.size() - 2, 2, "'s") == 0 || word.back() == '\'')) return false;
        for (char ch : word)
            if (ch >= '0' && ch <= '9') return false;
        return true;
    };
    for (char c : words + " ") {
        if (c == ' ' || c == '-') {
            if (!ok(w)) return false;
            w.clear();
        } else {
            w += c;
        }
    }
    return true;
}

std::string OpeningTexts::compose(const std::string& component, const std::string& lang, bool spoken) const {
    if (component.empty()) return {};
    if (lang == "en") {
        std::string s = component;
        if (spoken)
            for (const auto& r : respell_) replaceAll(s, r.first, r.second);
        return s;
    }
    std::string out;
    auto fixed = components_.find(component);
    if (fixed != components_.end()) {
        if (const std::string* v = find(lang, "opening.component." + fixed->second)) out = *v;
    } else {
        const size_t sp = component.rfind(' ');
        auto type = sp == std::string::npos ? composeTypes_.end() : composeTypes_.find(component.substr(sp + 1));
        const std::string* pattern = type == composeTypes_.end() ? nullptr : find(lang, "opening.compose." + type->second);
        if (pattern) {
            const std::string proper = component.substr(0, sp);
            std::string x, xgen;
            auto ep = eponyms_.find(proper);
            if (ep != eponyms_.end()) {
                const std::string* n = find(lang, "opening.eponym." + ep->second);
                const std::string* g = find(lang, "opening.eponym." + ep->second + ".gen");
                if (n && g) {
                    x = *n;
                    xgen = *g;
                }
            } else if ((lang == "fr" || lang == "de" || lang == "es") && properName(proper)) {
                x = xgen = proper;
            }
            if (!x.empty()) {
                out = *pattern;
                replaceAll(out, "{X:gen}", xgen);
                replaceAll(out, "{X}", x);
            }
        }
    }
    // Chinese subtitles keep the English words rather than lose the name (the voice speaks English anyway).
    if (out.empty() && (lang == "zh-Hans" || lang == "zh-Hant")) out = component;
    if (spoken && (lang == "ru" || lang == "uk")) out = fixNameDashesForSpeech(out);
    return out;
}

std::string OpeningTexts::arg(const std::string& ref, const std::string& form, const std::string& lang,
                              bool spoken) const {
    const size_t colon = ref.find(':');
    if (colon == std::string::npos) return {};
    const std::string kind = ref.substr(0, colon), id = ref.substr(colon + 1);
    if (kind == "line") return compose(id, lang, spoken);
    if (kind != "family" && kind != "variation") return {};
    const std::string base = "opening." + kind + "." + id;
    const std::string key = form.empty() || form == "nom" ? base : base + "." + form;
    const std::string* v = nullptr;
    for (const std::string& k : {key, base}) {
        if (spoken) v = find(lang, k + ".spoken");
        if (!v) v = find(lang, k);
        if (v) break;
    }
    return v ? *v : std::string();
}

std::string OpeningTexts::render(const OpeningLine& line, const std::string& lang, bool spoken,
                                 uint32_t variantSeed) const {
    const int n = variants(line.key);
    if (n == 0) return {};
    const int idx = int(variantSeed % uint32_t(n));
    const std::string key = idx == 0 ? line.key : line.key + "." + std::to_string(idx + 1);
    const std::string* tmpl = spoken ? find(lang, key + ".spoken") : nullptr;
    if (!tmpl) tmpl = find(lang, key);
    if (!tmpl) return {};
    std::string out;
    const std::string& t = *tmpl;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '{') {
            out += t[i];
            continue;
        }
        const size_t close = t.find('}', i);
        if (close == std::string::npos) return {};
        const std::string spec = t.substr(i + 1, close - i - 1);
        const size_t c = spec.find(':');
        const std::string name = spec.substr(0, c), form = c == std::string::npos ? "" : spec.substr(c + 1);
        const OpeningArg* a = nullptr;
        for (const auto& p : line.args)
            if (p.first == name) a = &p.second;
        if (!a) return {};
        std::string v = a->kind == OpeningArg::Kind::Number ? std::to_string(a->number) : arg(a->text, form, lang, spoken);
        if (v.empty()) return {};
        out += v;
        i = close;
    }
    if (spoken && (lang == "ru" || lang == "uk")) out = fixNameDashesForSpeech(out);
    return out;
}

}  // namespace coach
