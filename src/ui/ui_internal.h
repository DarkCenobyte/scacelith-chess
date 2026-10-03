// Shared state between the UI translation units (core, screens, viewer). Internal to src/ui.
#pragma once
#include "ui.h"
#include "scacelith_version.h"
#include "ui_draw.h"
#include "ui_theme.h"
#include "../i18n/i18n.h"
#include <string>
#include <vector>

namespace ui {
namespace detail {

struct Data {
    std::vector<DifficultyInfo> difficulties;
    std::vector<std::string> timeControls;
    std::vector<m::ivec2> resolutions;
    std::vector<CoachLevelInfo> coachLevels;
    std::string version = SCACELITH_VERSION;
};
Data& data();

// Called from ui::beginFrame.
void screensBeginFrame(float dt);
void screensReset();
// The coach voice download (ui_model_download.cpp): the modal block of its prompt, called from
// ui::beginFrame, and the title page's Coach entry (setCoachEntryHook).
void modelDownloadBeginFrame();
void coachEntryOpened();

// Small helpers of the table and menu files (ui_coach, ui_hotseat, ui_model_download,
// ui_online_hud, ui_screens_game), which take them with 'using namespace detail::helpers'. A
// namespace of their own: the other files keep local copies that these must not hide.
namespace helpers {

inline gfx::TextStyle style(int face, float size, m::vec4 color, gfx::HAlign align = gfx::HAlign::Left,
                            float tracking = 0.0f) {
    gfx::TextStyle st;
    st.face = face;
    st.size = size;
    st.color = color;
    st.align = align;
    st.tracking = tracking;
    return st;
}
inline float ease(float t) { return m::smootherstep(t); }
inline std::string T(const char* key) { return i18n::tr(key); }
inline std::string T(const std::string& key) { return i18n::tr(key); }
inline std::string L(const char* key) { return std::string(i18n::tr(key)) + "##" + key; }
inline std::string num(int v) { return std::to_string(v); }

// A dark band like the notifications: a core of opacity coreAlpha * a that fades out over 'edge'
// of the width at both ends, and a gold hairline along the top and the bottom.
inline void band(const gfx::Rect& r, float a, float coreAlpha, float edge) {
    m::vec4 d(0.02f, 0.017f, 0.015f, coreAlpha * a), z(0.02f, 0.017f, 0.015f, 0.0f);
    gfx::fillH(gfx::Rect(r.x, r.y, r.w * edge, r.h), z, d);
    gfx::fill(gfx::Rect(r.x + r.w * edge, r.y, r.w * (1.0f - 2.0f * edge), r.h), d);
    gfx::fillH(gfx::Rect(r.r() - r.w * edge, r.y, r.w * edge, r.h), d, z);
    gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, r.y, theme::withAlpha(theme::gold, 0.55f * a), 0.45f);
    gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, r.b() - 1.0f, theme::withAlpha(theme::gold, 0.55f * a), 0.45f);
}

}  // namespace helpers
}  // namespace detail

// Hooks used by the "ui" viewer scene to open a given state directly.
namespace debug {
// Same order as the menu's own page list (cast by value): new pages go at the end.
enum class MenuPage {
    Title = 0, NewGame = 1, Options = 2, Credits = 3, Watch = 4, Online = 5, Calibration = 6, Coach = 7, Licences = 8,
    Library = 9  // "Saved games" (needs a LibrarySetup::folder)
};
void openMenuPage(MenuPage page);   // next mainMenu() call starts on this page
// Next mainMenu() call starts on the online page's sub-page 'sub' (see debug::openOnlinePage).
void openOnlineMenu(const std::string& sub);
void setOptionsTab(int tab);        // 0 Display, 1 Graphics, 2 Audio, 3 Gameplay, 4 Player, 5 Online, 6 Controls
void openPauseConfirm(int which);   // 1 = resign, 2 = main menu (next pauseMenu() call)
void foldGameOver(bool folded);
void showModelLicence();            // the next modelPrompt() opens on the licence text
// The next frame of Saved games with a game shown presses its Save as GIF (signed in to the
// in-process fake server with a virtual clock: the GIF is made and written before that frame ends).
void libraryGif();
}  // namespace debug

}  // namespace ui
