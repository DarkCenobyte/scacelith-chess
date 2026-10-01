// Unit tests for src/game/game_archive: which games are saved, the record of a game, file names,
// the atomic save that never replaces a file, the cached listing, loading and removal.
#include "test.h"
#include "game/game_archive.h"
#include "net/net_sys.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace game;
namespace pgn = chess::pgn;

namespace {

#ifdef _WIN32
// The archive's paths are UTF-8: the helpers use the wide API too (the ANSI one fails on "Élodie").
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(w.size() - 1);
    return w;
}
std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 1), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(s.size() - 1);
    return s;
}
#endif

FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(wide(path).c_str(), mode[0] == 'r' ? L"rb" : L"wb");
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// A fresh folder beside the test executable, removed by the destructor.
struct TempFolder {
    std::string path;
    explicit TempFolder(const char* tag) {
#ifdef _WIN32
        const unsigned pid = unsigned(GetCurrentProcessId());
#else
        const unsigned pid = unsigned(getpid());
#endif
        path = net::sys::exeDirectory() + "archive-test-" + tag + "-" + std::to_string(pid);
        wipe(path);
        archive::clearCache();
    }
    ~TempFolder() { wipe(path); }
    std::string file(const std::string& name) const { return archive::joinPath(path, name); }
    static void wipe(const std::string& dir) {
        for (const std::string& n : names(dir)) {
            const std::string p = archive::joinPath(dir, n);
            if (!removeFile(p)) wipe(p);
        }
#ifdef _WIN32
        RemoveDirectoryW(wide(dir).c_str());
#else
        rmdir(dir.c_str());
#endif
    }
    static bool removeFile(const std::string& p) {
#ifdef _WIN32
        return DeleteFileW(wide(p).c_str()) != 0;
#else
        return std::remove(p.c_str()) == 0;
#endif
    }
    static std::vector<std::string> names(const std::string& dir) {
        std::vector<std::string> out;
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            std::string n = utf8(fd.cFileName);
            if (n != "." && n != "..") out.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
#else
        DIR* d = opendir(dir.c_str());
        if (!d) return out;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
#endif
        return out;
    }
};

bool writeText(const std::string& path, const std::string& text) {
    FILE* f = openUtf8(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

std::string readText(const std::string& path) {
    std::string out;
    FILE* f = openUtf8(path, "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

chess::Game playedGame(const std::vector<const char*>& sans) {
    chess::Game g;
    for (const char* s : sans) CHECK(g.play(g.position().parseSAN(s)));
    return g;
}

std::time_t localMoment(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

archive::GameInfo info(archive::Mode mode, const char* white, const char* black, std::time_t started) {
    archive::GameInfo i;
    i.mode = mode;
    i.white = white;
    i.black = black;
    i.started = started;
    return i;
}

}  // namespace

TEST(archive_which_games_are_saved) {
    using archive::Mode;
    CHECK(archive::shouldSave(Mode::Play, -1, 12, true, true));
    CHECK(archive::shouldSave(Mode::HotSeat, -1, 3, false, true));  // left before its end
    CHECK(archive::shouldSave(Mode::Direct, -1, 40, true, true));
    for (int level = 1; level <= 6; ++level) CHECK(archive::shouldSave(Mode::Coach, level, 20, true, true));
    CHECK(!archive::shouldSave(Mode::Coach, 0, 20, true, true));    // the rules lesson
    CHECK(!archive::shouldSave(Mode::Coach, -1, 20, true, true));
    CHECK(!archive::shouldSave(Mode::Server, -1, 40, true, true));  // the server keeps its games
    CHECK(!archive::shouldSave(Mode::Watch, -1, 40, true, true));
    CHECK(!archive::shouldSave(Mode::Imported, -1, 40, true, true));
    CHECK(!archive::shouldSave(Mode::Play, -1, 0, false, true));    // left before the first move
    CHECK(archive::shouldSave(Mode::Play, -1, 0, true, true));      // resigned at once: a result
    CHECK(!archive::shouldSave(Mode::Play, -1, 30, true, false));   // Options: off
    CHECK_EQ(std::string(archive::modeName(Mode::HotSeat)), std::string("hotseat"));
    CHECK(archive::modeFromName("coach") == Mode::Coach);
    CHECK(archive::modeFromName("OTB") == Mode::Imported);
}

TEST(archive_record_of_a_game) {
    chess::Game g = playedGame({"f3", "e5", "g4", "Qh4#"});
    archive::GameInfo in = info(archive::Mode::Coach, "Olivier", "Coach", localMoment(2026, 10, 1, 21, 4, 9));
    in.coachLevel = 3;
    in.blackElo = 1300;
    in.timeControl = "-";
    in.round = 2;
    in.elapsedMs = {1234, 2071, 950, 3049};
    pgn::Record r = archive::makeRecord(g, in);
    CHECK_EQ(r.tag("Event"), std::string("Coach game"));
    CHECK_EQ(r.tag("Site"), std::string("Scacelith"));
    CHECK_EQ(r.tag("Date"), std::string("2026.10.01"));
    CHECK_EQ(r.tag("Time"), std::string("21:04:09"));
    CHECK_EQ(r.tag("Round"), std::string("2"));
    CHECK_EQ(r.tag("Result"), std::string("0-1"));
    CHECK_EQ(r.result, std::string("0-1"));
    CHECK_EQ(r.tag("Termination"), std::string("normal"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("checkmate"));
    CHECK_EQ(r.tag("ScacelithMode"), std::string("coach"));
    CHECK_EQ(r.tag("CoachLevel"), std::string("3"));
    CHECK_EQ(r.tag("BlackElo"), std::string("1300"));
    CHECK(r.findTag("WhiteElo") == nullptr);
    CHECK_EQ(int(r.plies.size()), 4);
    CHECK_EQ(r.plies[0].elapsedMs, int64_t(1200));  // rounded to 0.1 s
    CHECK_EQ(r.plies[1].elapsedMs, int64_t(2100));
    CHECK_EQ(r.plies[3].elapsedMs, int64_t(3000));
    CHECK_EQ(r.plies[0].clockMs, int64_t(-1));
    // A game left before its end, timed, with clocks; a direct match ended by the authority.
    chess::Game open = playedGame({"e4", "e5", "Nf3"});
    archive::GameInfo hs = info(archive::Mode::HotSeat, "Alice", "Bob", localMoment(2026, 1, 2, 3, 4, 5));
    hs.timeControl = "300+3";
    hs.clockMs = {300000, 299540, 297960};
    pgn::Record left = archive::makeRecord(open, hs);
    CHECK_EQ(left.result, std::string("*"));
    CHECK_EQ(left.tag("Termination"), std::string("unterminated"));
    CHECK(left.findTag("ScacelithEnd") == nullptr);
    CHECK_EQ(left.plies[1].clockMs, int64_t(299500));
    CHECK_EQ(left.tag("TimeControl"), std::string("300+3"));
    archive::GameInfo dm = info(archive::Mode::Direct, "Me", "Friend", 0);
    dm.result = "1-0";
    dm.endKey = "reason.online.abandonment";
    dm.termination = "abandoned";
    pgn::Record direct = archive::makeRecord(open, dm);
    CHECK_EQ(direct.result, std::string("1-0"));
    CHECK_EQ(direct.tag("Event"), std::string("Direct match"));
    CHECK_EQ(direct.tag("Termination"), std::string("abandoned"));
    CHECK_EQ(direct.tag("ScacelithEnd"), std::string("online.abandonment"));
}

TEST(archive_file_names) {
    CHECK_EQ(archive::sanitizeName("Alice"), std::string("Alice"));
    CHECK_EQ(archive::sanitizeName("Jean Dupont"), std::string("Jean_Dupont"));
    CHECK_EQ(archive::sanitizeName("a<b>c:d\"e/f\\g|h?i*j"), std::string("a_b_c_d_e_f_g_h_i_j"));
    CHECK_EQ(archive::sanitizeName("  ..hidden..  "), std::string("hidden"));
    CHECK_EQ(archive::sanitizeName("tab\there\nnew\x01line"), std::string("tabherenewline"));
    CHECK_EQ(archive::sanitizeName(""), std::string("Unknown"));
    CHECK_EQ(archive::sanitizeName("???"), std::string("Unknown"));
    CHECK_EQ(archive::sanitizeName("CON"), std::string("CON_"));
    CHECK_EQ(archive::sanitizeName("nul"), std::string("nul_"));
    CHECK_EQ(archive::sanitizeName("Com1"), std::string("Com1_"));
    CHECK_EQ(archive::sanitizeName("LPT9.txt"), std::string("LPT9.txt_"));
    CHECK_EQ(archive::sanitizeName("COM\xC2\xB9"), std::string("COM\xC2\xB9_"));
    CHECK_EQ(archive::sanitizeName("Console"), std::string("Console"));
    // Unicode is kept; bidi overrides and zero-width characters are not.
    CHECK_EQ(archive::sanitizeName("\xC3\x89lodie"), std::string("\xC3\x89lodie"));
    CHECK_EQ(archive::sanitizeName("\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E"), std::string("\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E"));
    CHECK_EQ(archive::sanitizeName("evil\xE2\x80\xAE" "fdp.exe"), std::string("evilfdp.exe"));
    CHECK_EQ(archive::sanitizeName("a\xE2\x80\x8B" "b\xE3\x80\x80" "c"), std::string("ab_c"));
    // Length: whole UTF-8 characters only.
    std::string longName;
    for (int i = 0; i < 30; ++i) longName += "\xC3\xA9";  // 60 bytes
    std::string cut = archive::sanitizeName(longName, 41);
    CHECK_EQ(cut.size(), size_t(40));
    CHECK_EQ(archive::sanitizeName(std::string(100, 'x')).size(), size_t(40));
    // The full name.
    pgn::Record r;
    r.setTag("White", "Olivier");
    r.setTag("Black", "Stockfish (Expert)");
    r.setTag("ScacelithMode", "play");
    CHECK_EQ(archive::fileName(r, localMoment(2026, 10, 1, 21, 4, 9)),
             std::string("2026-10-01_210409_play_Olivier-vs-Stockfish_(Expert).pgn"));
    pgn::Record other;
    CHECK_EQ(archive::fileName(other, localMoment(2025, 3, 9, 8, 7, 6)), std::string("2025-03-09_080706_game_Unknown-vs-Unknown.pgn"));
    CHECK_EQ(archive::joinPath("/a/b", "c.pgn"), std::string("/a/b/c.pgn"));
    CHECK_EQ(archive::joinPath("/a/b/", "c.pgn"), std::string("/a/b/c.pgn"));
    CHECK_EQ(archive::joinPath("C:\\Users\\x\\pgn", "c.pgn"), std::string("C:\\Users\\x\\pgn\\c.pgn"));
}

TEST(archive_save_is_atomic_and_never_overwrites) {
    TempFolder tmp("save");
    const std::string folder = archive::joinPath(tmp.path, "deep/pgn");  // created with its parent
    chess::Game g = playedGame({"e4", "e5", "Nf3", "Nc6", "Bb5"});
    const std::time_t when = localMoment(2026, 10, 1, 21, 4, 9);
    archive::GameInfo in = info(archive::Mode::Play, "\xC3\x89lodie", "Stockfish", when);
    in.elapsedMs = {500, 700, 1500, 2500, 4000};
    pgn::Record r = archive::makeRecord(g, in);
    archive::SaveResult a = archive::save(folder, r, when);
    if (!a.ok) std::fprintf(stderr, "  save: %s\n", a.error.c_str());
    CHECK(a.ok);
    CHECK_EQ(a.path, archive::joinPath(folder, "2026-10-01_210409_play_\xC3\x89lodie-vs-Stockfish.pgn"));
    CHECK_EQ(readText(a.path), pgn::write(r));
    // The same name again: suffixes, and the first file is untouched.
    pgn::Record r2 = r;
    r2.setTag("Round", "9");
    archive::SaveResult b = archive::save(folder, r2, when);
    archive::SaveResult c = archive::save(folder, r2, when);
    CHECK(b.ok && c.ok);
    CHECK_EQ(b.path, archive::joinPath(folder, "2026-10-01_210409_play_\xC3\x89lodie-vs-Stockfish_2.pgn"));
    CHECK_EQ(c.path, archive::joinPath(folder, "2026-10-01_210409_play_\xC3\x89lodie-vs-Stockfish_3.pgn"));
    CHECK_EQ(readText(a.path), pgn::write(r));
    CHECK_EQ(readText(b.path), pgn::write(r2));
    // A file the player put there under that name is never replaced either.
    const std::string theirs = archive::joinPath(folder, "2025-01-01_000000_play_A-vs-B.pgn");
    CHECK(writeText(theirs, "their own text"));
    pgn::Record rab;
    rab.setTag("White", "A");
    rab.setTag("Black", "B");
    rab.setTag("ScacelithMode", "play");
    archive::SaveResult d = archive::save(folder, rab, localMoment(2025, 1, 1, 0, 0, 0));
    CHECK(d.ok);
    CHECK(d.path != theirs);
    CHECK_EQ(readText(theirs), std::string("their own text"));
    // Only finished files in the folder: no temporary file is left behind.
    std::vector<std::string> names = TempFolder::names(folder);
    CHECK_EQ(int(names.size()), 5);
    for (const std::string& n : names) CHECK(n.find(".tmp") == std::string::npos);
    // The saved games read back with their times.
    archive::LoadResult back = archive::loadFile(a.path);
    CHECK(back.ok);
    CHECK_EQ(int(back.record.plies.size()), 5);
    if (back.record.plies.size() == 5) CHECK_EQ(back.record.plies[4].elapsedMs, int64_t(4000));
    // A folder that cannot be created.
    CHECK(writeText(archive::joinPath(tmp.path, "plainfile"), "x"));
    archive::SaveResult bad = archive::save(archive::joinPath(tmp.path, "plainfile/sub"), r, when);
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());
}

TEST(archive_listing_load_and_cache) {
    TempFolder tmp("list");
    const std::string folder = tmp.path;
    // A missing folder lists as empty, without an error.
    archive::ListStats st;
    CHECK(archive::list(folder, &st).empty());
    CHECK(st.error.empty());
    // Three saved games on three days, a file of two imported games, a broken file, other files.
    for (int day = 1; day <= 3; ++day) {
        chess::Game g = playedGame({"d4", "d5", "c4"});
        archive::GameInfo in = info(day == 2 ? archive::Mode::HotSeat : archive::Mode::Play, "P", "Q", localMoment(2026, 9, day, 12, 0, 0));
        in.clockMs = {60000, 59000, 58000};
        CHECK(archive::save(folder, archive::makeRecord(g, in), localMoment(2026, 9, day, 12, 0, 0)).ok);
    }
    CHECK(writeText(archive::joinPath(folder, "lichess_export.PGN"),
                    "[Event \"A\"]\n[Date \"2026.09.02\"]\n[UTCTime \"20:00:00\"]\n[White \"X\"]\n[Black \"Y\"]\n\n1. e4 e5 1-0\n\n"
                    "[Event \"B\"]\n[UTCDate \"2026.09.04\"]\n[White \"Z\"]\n[Black \"W\"]\n\n1. c4 { unclosed\n"));
    CHECK(writeText(archive::joinPath(folder, "broken.pgn"), std::string("\xFF\xFE[\0E\0", 6)));
    CHECK(writeText(archive::joinPath(folder, "notes.txt"), "1. e4 *"));
    CHECK(writeText(archive::joinPath(folder, ".hidden.pgn"), "1. e4 *"));
    std::vector<archive::Entry> list = archive::list(folder, &st);
    CHECK_EQ(st.files, 5);
    CHECK_EQ(st.read, 5);
    CHECK_EQ(int(list.size()), 6);
    if (list.size() != 6) return;
    // Newest first: B (09.04), day 3, A (09.02 20:00), day 2 (12:00), day 1, the broken file (no date).
    CHECK_EQ(list[0].tag("Event"), std::string("B"));
    CHECK(!list[0].error.empty());           // its unclosed comment
    CHECK_EQ(list[0].index, 1);
    CHECK_EQ(list[0].games, 2);
    CHECK(!list[0].removable());
    CHECK_EQ(list[1].date(), std::string("2026.09.03"));
    CHECK(list[1].mode == archive::Mode::Play);
    CHECK_EQ(list[1].plies, 3);
    CHECK(list[1].removable());
    CHECK_EQ(list[2].tag("Event"), std::string("A"));
    CHECK(list[2].mode == archive::Mode::Imported);
    CHECK_EQ(list[2].result, std::string("1-0"));
    CHECK_EQ(list[3].date(), std::string("2026.09.02"));
    CHECK(list[3].mode == archive::Mode::HotSeat);
    CHECK_EQ(list[4].date(), std::string("2026.09.01"));
    CHECK_EQ(list[5].file, std::string("broken.pgn"));
    CHECK(list[5].fileError);
    CHECK(!list[5].error.empty());
    // Loading: the whole game, with its clocks; an imported game from the middle of its file.
    archive::LoadResult l1 = archive::load(list[1]);
    CHECK(l1.ok);
    CHECK_EQ(int(l1.record.plies.size()), 3);
    if (l1.record.plies.size() == 3) CHECK_EQ(l1.record.plies[2].clockMs, int64_t(58000));
    archive::LoadResult la = archive::load(list[2]);
    CHECK(la.ok);
    CHECK_EQ(la.record.tag("Event"), std::string("A"));
    archive::LoadResult lb = archive::load(list[0]);
    CHECK(!lb.ok);
    CHECK(lb.error.find("line 14, column 7") != std::string::npos);
    CHECK(!archive::load(list[5]).ok);
    CHECK(!archive::loadFile(archive::joinPath(folder, "lichess_export.PGN"), 5).ok);
    CHECK(archive::loadFile(archive::joinPath(folder, "lichess_export.PGN"), 0).ok);
    // The second listing reads nothing; a changed file is read again, a deleted one disappears.
    list = archive::list(folder, &st);
    CHECK_EQ(st.read, 0);
    CHECK_EQ(st.cached, 5);
    CHECK(writeText(archive::joinPath(folder, "lichess_export.PGN"), "[Event \"A2\"]\n[Date \"2026.09.10\"]\n\n1. e4 *\n"));
    std::remove(archive::joinPath(folder, "broken.pgn").c_str());
    list = archive::list(folder, &st);
    CHECK_EQ(st.read, 1);
    CHECK_EQ(st.cached, 3);
    CHECK_EQ(int(list.size()), 4);
    if (!list.empty()) CHECK_EQ(list[0].tag("Event"), std::string("A2"));
}

TEST(archive_remove_rules) {
    TempFolder tmp("remove");
    const std::string folder = tmp.path;
    chess::Game g = playedGame({"e4"});
    archive::SaveResult s = archive::save(folder, archive::makeRecord(g, info(archive::Mode::Play, "A", "B", 0)));
    CHECK(s.ok);
    CHECK(writeText(archive::joinPath(folder, "two.pgn"), "1. e4 *\n\n1. d4 *\n"));
    std::vector<archive::Entry> list = archive::list(folder);
    CHECK_EQ(int(list.size()), 3);
    const archive::Entry* single = nullptr;
    const archive::Entry* multi = nullptr;
    for (const archive::Entry& e : list) (e.games == 1 ? single : multi) = &e;
    if (!single || !multi) return;
    // A file of several games is never deleted from here.
    archive::RemoveResult m = archive::remove(*multi);
    CHECK(m.status == archive::RemoveStatus::SeveralGames);
    CHECK(!m.error.empty());
    CHECK_EQ(readText(multi->path), std::string("1. e4 *\n\n1. d4 *\n"));
    // A file changed since the listing is not deleted.
    archive::Entry stale = *single;
    stale.fileSize += 1;
    CHECK(archive::remove(stale).status == archive::RemoveStatus::Changed);
    // The game's own file goes; a second removal finds it gone; the listing forgets it.
    CHECK(archive::remove(*single).ok());
    CHECK(archive::remove(*single).status == archive::RemoveStatus::NotFound);
    CHECK_EQ(int(archive::list(folder).size()), 2);
}

TEST(archive_lists_thousands_quickly) {
    TempFolder tmp("many");
    const std::string folder = tmp.path;
    chess::Game g = playedGame({"e4", "c5", "Nf3", "d6", "d4", "cxd4", "Nxd4", "Nf6", "Nc3", "a6"});
    pgn::Record base = archive::makeRecord(g, info(archive::Mode::Play, "Many", "Games", localMoment(2026, 1, 1, 0, 0, 0)));
    const int n = 1500;
    // Written directly (the same text save() writes), without a log line per game.
    CHECK(archive::save(folder, base, localMoment(2026, 1, 1, 0, 0, 0)).ok);
    for (int i = 1; i < n; ++i) {
        pgn::Record r = base;
        r.setTag("Round", std::to_string(i + 1));
        CHECK(writeText(archive::joinPath(folder, archive::fileName(r, localMoment(2026, 1, 1, 0, 0, 0) + i * 60)), pgn::write(r)));
    }
    archive::clearCache();
    archive::ListStats st;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<archive::Entry> first = archive::list(folder, &st);
    auto t1 = std::chrono::steady_clock::now();
    CHECK_EQ(int(first.size()), n);
    CHECK_EQ(st.read, n);
    std::vector<archive::Entry> second = archive::list(folder, &st);
    auto t2 = std::chrono::steady_clock::now();
    CHECK_EQ(st.read, 0);
    CHECK_EQ(int(second.size()), n);
    const double coldMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double warmMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::fprintf(stderr, "  %d games: first listing %.1f ms, cached %.1f ms\n", n, coldMs, warmMs);
    CHECK(warmMs < 1000.0);
    CHECK(warmMs < coldMs);
}
