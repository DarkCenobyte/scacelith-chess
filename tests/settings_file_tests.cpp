// The settings file (src/game/settings.cpp, game::Settings::load and save): the defaults of a first
// start, a file written and read back, the values a hand-edited file cannot push out of range,
// the two-player ratings read back with gaps and duplicates, the fallback of a portable install
// to the user data folder, the failures to save, and the --lang argument over the file's language.
// Every file lives in a folder of the test's (headless::TempDir); the platform's data folder,
// command line and system language, and the interface language, are put back at the end of each
// test. No App is started.
#include "test.h"
#include "fake_platform.h"
#include "headless.h"
#include "core/ini.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include "net/net_sys.h"
#include "platform/platform.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

// The platform's answers and the interface language as the test found them.
struct PlatformGuard {
    std::string folder = plat::userDataDirectory() == net::sys::userDataDirectory() ? std::string() : plat::userDataDirectory();
    std::vector<std::string> args = plat::commandLine();
    std::string systemLanguage = plat::systemLanguage();
    std::string language = i18n::language();
    ~PlatformGuard() {
        fakeplat::setDataFolder(folder);
        fakeplat::setCommandLine(args);
        fakeplat::setSystemLanguage(systemLanguage);
        i18n::setLanguage(language);
    }
};

void writeText(const std::string& path, const std::string& text) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

std::string readText(const std::string& path) {
    std::string out;
    net::sys::readFile(path, out, 1 << 20);
    return out;
}

bool nearly(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

}  // namespace

// No file yet: the defaults, the brightness calibration still to do, the language of the system.
TEST(settings_file_missing_file_starts_with_defaults) {
    PlatformGuard guard;
    headless::TempDir dir("settings");
    fakeplat::setSystemLanguage("fr-CA");
    game::Settings s;
    s.brightnessCalibrated = true;
    CHECK(!s.load(dir.file("Scacelith.ini")));
    CHECK_EQ(s.path, dir.file("Scacelith.ini"));
    CHECK(!s.brightnessCalibrated);
    CHECK_EQ(s.language, std::string("fr"));
    CHECK_EQ(i18n::language(), std::string("fr"));
    CHECK_EQ(s.playerName, game::Settings().playerName);
    CHECK(!exists(dir.file("Scacelith.ini")));   // a load writes nothing
}

// Every section written and read back as it was: display, gameplay, the New Game and Watch a Game
// pages, the engine, the player's record against Stockfish, the hot-seat names and the two-player
// ratings, the online and direct match choices, the saved games, the interface and the player.
TEST(settings_file_written_and_read_back) {
    PlatformGuard guard;
    headless::TempDir dir("settings");
    game::Settings a;
    a.path = dir.file("Scacelith.ini");
    a.displayWidth = 1600;
    a.displayHeight = 900;
    a.fullscreen = true;
    a.vsync = false;
    a.renderScale = 0.75f;
    a.brightness = 1.25f;
    a.brightnessCalibrated = true;
    a.masterVolume = 0.5f;
    a.showLegalMoves = false;
    a.showCoordinates = true;
    a.autoPressClock = true;
    a.nextColor = 1;
    a.handoverSeconds = 1.5f;
    a.opponent = 1;
    a.difficultyPreset = 4;
    a.timeControlPreset = -1;
    a.customBaseSeconds = 45;
    a.customIncrementSeconds = 2;
    a.customDelaySeconds = 3;
    a.engineArch = "x86-64";
    a.humanizeThinking = false;
    elo::Record me;
    me.rating = 1612;
    me.games = 14;
    me.wins = 8;
    me.draws = 2;
    me.losses = 4;
    me.peak = 1650;
    me.rated = true;
    me.countedGames = 14;
    a.setPlayerRecord(me);
    a.hotseatNames[0] = "Ann";
    a.hotseatNames[1] = "Ben";
    a.hotseatHands[0] = 2;
    a.hotseatHands[1] = 0;
    a.hotseatRated = true;
    a.localPlayer("Ann").record.games = 3;
    a.localPlayer("Ben").record.wins = 1;
    CHECK(&a.localPlayer("Ann") == a.findLocalPlayer("Ann"));   // found, not added twice
    a.viewerWhitePreset = 2;
    a.viewerTimeControl = -1;
    a.viewerCustomBaseSeconds = 15;
    a.viewerShowControls = false;
    a.onlineHost = "chess.example.org";
    a.onlineApiPort = 8443;
    a.onlineColor = 1;
    a.directPort = 47123;
    a.directAddress = "10.0.0.2";
    a.directColor = 2;
    a.saveGames = false;
    a.language = "de";
    a.subtitles = 2;
    a.playerName = "Zo\xC3\xA9";
    a.handStyle = ui::font::HAND_MARCK;
    REQUIRE(a.save());
    const std::string text = readText(a.path);
    CHECK(text.find("[local_player_2]") != std::string::npos);
    CHECK(text.find("name = Ben") != std::string::npos);

    game::Settings b;
    REQUIRE(b.load(a.path));
    CHECK_EQ(b.displayWidth, 1600);
    CHECK_EQ(b.displayHeight, 900);
    CHECK(b.fullscreen);
    CHECK(!b.vsync);
    CHECK(nearly(b.renderScale, 0.75f, 1e-4f));
    CHECK(nearly(b.brightness, 1.25f, 1e-4f));
    CHECK(b.brightnessCalibrated);
    CHECK(nearly(b.masterVolume, 0.5f, 1e-4f));
    CHECK(!b.showLegalMoves);
    CHECK(b.showCoordinates);
    CHECK(b.autoPressClock);
    CHECK_EQ(b.nextColor, 1);
    CHECK(nearly(b.handoverSeconds, 1.5f, 1e-4f));
    CHECK_EQ(b.opponent, 1);
    CHECK_EQ(b.difficultyPreset, 4);
    CHECK_EQ(b.timeControlPreset, -1);
    CHECK_EQ(b.customBaseSeconds, 45);
    CHECK_EQ(b.customIncrementSeconds, 2);
    CHECK_EQ(b.customDelaySeconds, 3);
    CHECK_EQ(b.engineArch, std::string("x86-64"));
    CHECK(!b.humanizeThinking);
    const elo::Record r = b.playerRecord();
    CHECK_EQ(r.rating, 1612);
    CHECK_EQ(r.games, 14);
    CHECK_EQ(r.wins, 8);
    CHECK_EQ(r.draws, 2);
    CHECK_EQ(r.losses, 4);
    CHECK_EQ(r.peak, 1650);
    CHECK(r.rated);
    CHECK_EQ(b.hotseatNames[0], std::string("Ann"));
    CHECK_EQ(b.hotseatNames[1], std::string("Ben"));
    CHECK_EQ(b.hotseatHands[0], 2);
    CHECK_EQ(b.hotseatHands[1], 0);
    CHECK(b.hotseatRated);
    REQUIRE(b.localPlayers.size() == 2u);
    CHECK_EQ(b.localPlayers[0].name, std::string("Ann"));
    CHECK_EQ(b.localPlayers[0].record.games, 3);
    CHECK_EQ(b.localPlayers[1].name, std::string("Ben"));
    CHECK_EQ(b.localPlayers[1].record.wins, 1);
    CHECK_EQ(b.viewerWhitePreset, 2);
    CHECK_EQ(b.viewerTimeControl, -1);
    CHECK_EQ(b.viewerCustomBaseSeconds, 15);
    CHECK(!b.viewerShowControls);
    CHECK_EQ(b.onlineHost, std::string("chess.example.org"));
    CHECK_EQ(b.onlineApiPort, 8443);
    CHECK_EQ(b.onlineColor, 1);
    CHECK_EQ(b.directPort, 47123);
    CHECK_EQ(b.directAddress, std::string("10.0.0.2"));
    CHECK_EQ(b.directColor, 2);
    CHECK(!b.saveGames);
    CHECK_EQ(b.language, std::string("de"));
    CHECK_EQ(i18n::language(), std::string("de"));
    CHECK_EQ(b.subtitles, 2);
    CHECK_EQ(b.playerName, std::string("Zo\xC3\xA9"));
    CHECK_EQ(int(b.handStyle), int(ui::font::HAND_MARCK));
}

// A hand-edited file: what is out of range is brought back into it, an empty name is the default
// one, a value that is not a number keeps the default, a file from before the brightness
// calibration counts as calibrated.
TEST(settings_file_values_brought_into_range) {
    PlatformGuard guard;
    headless::TempDir dir("settings");
    const std::string path = dir.file("Scacelith.ini");
    writeText(path,
              "[display]\nrender_scale = nan\n"
              "[audio]\nvoice_volume = 3\n"
              "[gameplay]\nhandover_seconds = 0.3\n"
              "[newgame]\nopponent = 5\n"
              "[online]\napi_port = 0\nws_port = 99999\ncolor = 7\n"
              "[direct]\nport = 70000\ncolor = -2\njoin_port = 0\n"
              "[interface]\nsubtitles = 9\nlanguage = xx\n"
              "[player]\nname =\nhand_style = 42\n");
    game::Settings s;
    REQUIRE(s.load(path));
    CHECK(nearly(s.renderScale, game::Settings().renderScale, 1e-6f));
    CHECK(nearly(s.voiceVolume, 1.0f, 1e-6f));
    CHECK(nearly(s.handoverSeconds, 0.8f, 1e-6f));   // a flight is never shorter than 0.8 s
    CHECK_EQ(s.opponent, 1);
    CHECK_EQ(s.onlineApiPort, 1);
    CHECK_EQ(s.onlineWsPort, 65535);
    CHECK_EQ(s.onlineColor, 2);
    CHECK_EQ(s.directPort, 65535);
    CHECK_EQ(s.directColor, 0);
    CHECK_EQ(s.directJoinPort, 1);
    CHECK_EQ(s.subtitles, 2);
    CHECK_EQ(s.language, std::string("en"));        // unknown: the system's (en-US)
    CHECK_EQ(s.playerName, std::string("Human"));
    CHECK_EQ(int(s.handStyle), int(ui::font::HAND_STYLE_COUNT) - 1);
    CHECK(s.brightnessCalibrated);

    writeText(path, "[gameplay]\nhandover_seconds = 5\n");
    game::Settings t;
    REQUIRE(t.load(path));
    CHECK(nearly(t.handoverSeconds, 2.0f, 1e-6f));
    writeText(path, "[gameplay]\nhandover_seconds = 0\n");
    game::Settings u;
    REQUIRE(u.load(path));
    CHECK_EQ(u.handoverSeconds, 0.0f);                 // the instant cut
}

// The two-player ratings as a hand-edited file may hold them: a section removed leaves a gap
// (read past within the first 256), a name twice is read once, an empty name is skipped, and
// past a gap beyond the 256th nothing more is read.
TEST(settings_file_two_player_ratings_with_gaps) {
    PlatformGuard guard;
    headless::TempDir dir("settings");
    const std::string path = dir.file("Scacelith.ini");
    writeText(path,
              "[local_player_1]\nname = Ann\ngames = 3\nwins = 2\n"
              "[local_player_3]\nname = Ben\n"
              "[local_player_4]\nname = Ann\ngames = 9\n"
              "[local_player_5]\nname =\n"
              "[local_player_256]\nname = Cid\n"
              "[local_player_300]\nname = Far\n");
    game::Settings s;
    REQUIRE(s.load(path));
    REQUIRE(s.localPlayers.size() == 3u);
    CHECK_EQ(s.localPlayers[0].name, std::string("Ann"));
    CHECK_EQ(s.localPlayers[0].record.games, 3);
    CHECK_EQ(s.localPlayers[0].record.wins, 2);
    CHECK_EQ(s.localPlayers[1].name, std::string("Ben"));
    CHECK_EQ(s.localPlayers[2].name, std::string("Cid"));
    CHECK(s.findLocalPlayer("Far") == nullptr);
    const game::Settings& cs = s;
    CHECK(cs.findLocalPlayer("Ben") != nullptr);
    CHECK(cs.findLocalPlayer("Nobody") == nullptr);
    // Written back without the gaps.
    REQUIRE(s.save());
    game::Settings t;
    REQUIRE(t.load(path));
    REQUIRE(t.localPlayers.size() == 3u);
    CHECK_EQ(t.localPlayers[2].name, std::string("Cid"));
    CHECK(readText(path).find("[local_player_3]") != std::string::npos);
    CHECK(readText(path).find("[local_player_256]") == std::string::npos);
}

// A portable install's file (next to the executable) that does not exist yet is read from the user
// data folder instead, where a file without a path is saved too. Nothing is written next to the
// executable.
TEST(settings_file_portable_install_falls_back_to_the_user_folder) {
    const std::string portable = plat::exeDirectory() + "Scacelith.ini";
    if (exists(portable)) SKIP("a Scacelith.ini lies next to the test executable");
    PlatformGuard guard;
    headless::TempDir dir("settings");
    fakeplat::setDataFolder(dir.path);
    game::Settings none;
    CHECK(!none.load(portable));                 // no file in either place: the defaults
    writeText(dir.file("Scacelith.ini"), "[player]\nname = Portable\n");
    game::Settings s;
    REQUIRE(s.load(portable));
    CHECK_EQ(s.playerName, std::string("Portable"));
    CHECK_EQ(s.path, portable);                    // still the first choice for saving
    CHECK(!exists(portable));

    game::Settings w;
    w.playerName = "Saved";
    w.path.clear();
    REQUIRE(w.save());
    game::Settings back;
    REQUIRE(back.load(dir.file("Scacelith.ini")));
    CHECK_EQ(back.playerName, std::string("Saved"));
    CHECK(!exists(portable));
}

// A file that cannot be written (its folder does not exist, and an explicit file has no
// fallback): save() says so. A read-only run (a screenshot) writes nothing and succeeds.
TEST(settings_file_save_failures_and_read_only) {
    PlatformGuard guard;
    headless::TempDir dir("settings");
    game::Settings s;
    s.path = dir.file("no/such/folder/Scacelith.ini");
    CHECK(!s.save());
    CHECK(!exists(s.path));
    s.path = dir.file("Scacelith.ini");
    s.readOnly = true;
    CHECK(s.save());
    CHECK(!exists(s.path));
    s.readOnly = false;
    CHECK(s.save());
    CHECK(exists(s.path));
}

// --lang on the command line wins over the file's language for the interface, and the file keeps
// its own choice.
TEST(settings_file_lang_argument_overrides_the_file) {
    PlatformGuard guard;
    headless::TempDir dir("settings");
    const std::string path = dir.file("Scacelith.ini");
    writeText(path, "[interface]\nlanguage = fr\n");
    fakeplat::setCommandLine({"--no-intro", "--lang", "es-MX"});
    game::Settings s;
    REQUIRE(s.load(path));
    CHECK_EQ(s.language, std::string("fr"));
    CHECK_EQ(i18n::language(), std::string("es"));
    fakeplat::setCommandLine({});
    s.applyLanguage();
    CHECK_EQ(i18n::language(), std::string("fr"));
}
