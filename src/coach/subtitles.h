// Whether the coach's subtitles are shown: always when its voice cannot be heard, else per the
// option 'mode' (game::SubtitleMode: 0 Automatic, 1 On, 2 Off), 'Automatic' meaning when the voice
// speaks another language than the UI. The director applies it per line; game/settings.h
// (game::coachSubtitlesShown) forwards to it, so this header stays dependency-free.
#pragma once
#include <string>

namespace coach {

inline bool subtitlesShown(int mode, const std::string& uiLanguage, const std::string& speechLanguage,
                           bool voiceAvailable) {
    if (!voiceAvailable) return true;
    if (mode == 1) return true;
    if (mode == 2) return false;
    return speechLanguage != uiLanguage;
}

}  // namespace coach
