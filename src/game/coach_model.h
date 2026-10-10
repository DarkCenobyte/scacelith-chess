// The coach's voice model in the game: when to offer its download, the download job, and what the
// menus and the table show of it (the prompt, the progress panel, the notices). The model itself
// (its files, folder and download job) is tts/model_store.h; the drawing is ui::modelPrompt and
// ui::modelProgressPanel. The model is never shipped with the game (OpenRAIL-M weights): it is
// downloaded on the player's request into <application data>/coach/ (or --coach-dir).
//
// When the prompt shows:
//   - the player opens the Coach page (title page) while [coach] voice is on and the model is
//     not all there: the prompt shows over the Coach page (hooked through ui::setCoachEntryHook);
//   - the player switches Options > Audio > Coach voice on (applied) and the model is missing;
//   - the voice failed to load files that looked complete (coachModelLoadFailed), once;
//   - the player opens a game in the Analysis mode, the voice is on ([coach] voice and the
//     analysis's own, M), the model missing, and the prompt was never shown before
//     (Settings::coachVoiceOffered; offerVoiceForAnalysis).
// "Not now" (or Esc) switches [coach] voice off and saves: the coach speaks through subtitles,
// and is not offered the download again until the player switches the voice back on.
//
// The old INT8 model (tts::legacyManifest(), downloaded by earlier versions) still speaks, and its
// update to the official Supertonic 3 is offered:
//   - once at start-up (Settings::coachVoiceUpdateOffered), after a background check that its
//     files are there and hash right, when the title page first shows (after the loading screen
//     and the brightness calibration); the prompt then says that the new version replaces it, its
//     size, and that the old files are deleted first;
//   - from then on by a row of Options > Audio, under Coach voice, which opens the same prompt.
// "Not now" there keeps the old voice. Any download into a folder that holds old files (the
// update, or a plain download after the old files failed to load) shows the update form of the
// prompt; the job deletes the old files before it fetches the new ones, once the scene has let
// them go (setVoiceReleaseHook: a TTS worker maps them).
//
// Integration, for a scene that shows the menus and Coach mode (GameScene):
//   1. Once, after the Settings are loaded and --coach-dir applied (tts::setModelDirectory):
//          game::coachModelInit();
//   2. Every frame, menus and table alike, after the menus and the HUD, before ui::endFrame():
//          game::drawModelDownload(on the title page);
//          int fetched = 0;
//          if (game::coachModelInstalled(&fetched)) coachModelDownloaded(fetched);   // the voice is there now
//      where coachModelDownloaded stops a TTS worker that failed when coachVoiceRetry() says so,
//      then calls refreshCoachVoice().
//   3. Where the scene decides whether the coach can be heard (GameScene::refreshCoachVoice and
//      initCoachArgs):
//          coachVoiceFiles_ = game::coachVoiceWanted();   // in place of tts::modelFilesPresent()
//      and call refreshCoachVoice() on MenuAction::OptionsChanged (the option may have changed).
//      A coach game being played then starts its TTS worker (CoachStage::ensureWorker): the
//      voice is heard from the coach's next line on.
//   4. When the TTS worker failed to load, or its warm-up failed (tts::Worker::failed()),
//      although coachVoiceWanted():
//          game::coachModelLoadFailed();
//      The download it offers checks every file; when it replaced some, the worker gets one more
//      try (step 2).
//   5. Once, where the scene owns the TTS worker: a hook that stops it (and lets its model files
//      go) before a download deletes the old model's files:
//          game::setVoiceReleaseHook([this] { stop the worker; });
//      The next coach line starts a new worker, with the new files.
//   6. On exit (GameScene::shutdown; the ui viewer's coach-flow screen likewise):
//          game::coachModelShutdown();
//      The download stops; its .part files stay and the next download continues them.
// Testing aid: the environment variable SCACELITH_COACH_SOURCE=archive skips Supertone's
// repository (Supertone's archive copy only, the fallback path), =official never falls back to the
// archive copy. The ui viewer's "coach-flow" screen runs all this over the title page
// (scacelith --scene ui --ui-screen coach-flow [--coach-dir <folder>]).
#pragma once
#include <functional>

namespace game {

void coachModelInit();
// The voice is wanted ([coach] voice) but its files are not all there (or failed to load), and
// no download is running: Coach mode should offer the download.
bool coachModelNeedsPrompt();
// Opens the prompt (drawn by drawModelDownload from this frame on).
void openModelPrompt();
// The Analysis mode starts: opens the prompt when the voice is wanted, its model is missing, and
// the player was never offered it (from the Coach page, the option, or an earlier analysis).
// Returns whether it opened.
bool offerVoiceForAnalysis();
// The prompt (modal), the progress panel (top end corner) and the notices; also notices that the
// Coach voice option was switched on. Every frame, after the menus / HUD, before ui::endFrame().
// 'startupOffer': the start-up offer of the old model's update may open now (GameScene: on the
// title page, never over the loading screen nor the brightness calibration); until then the
// result of the background check waits.
void drawModelDownload(bool startupOffer = true);
// True once after a download that ended with every file checked. 'fetched' (optional) receives
// how many files it wrote (0: they were all there and right).
bool coachModelInstalled(int* fetched = nullptr);
// After such a download: whether a TTS worker that failed (to load, or its warm-up) is stopped so
// that a new one starts, one more warm-up. Only when the download wrote at least one file: files
// that all checked out would fail the same way. A worker whose new warm-up fails waits for the
// next download that writes files (one try per such download, never a loop).
inline bool coachVoiceRetry(bool workerFailed, int fetched) { return workerFailed && fetched > 0; }
bool coachModelDownloading();
// The coach can be heard as far as the option and the files go: [coach] voice on, every file
// present (tts::modelFilesPresent) and no download running.
bool coachVoiceWanted();
// The TTS worker could not load, or not speak with, files that looked complete: the next Coach
// entry offers the download, whose first step checks every file (SHA-256) and fetches only the
// bad ones.
void coachModelLoadFailed();
// Cancels and joins a running download.
void coachModelShutdown();
// Stops whatever maps the voice model's files (the scene's TTS worker), synchronously; called before
// a download deletes the old model's files. nullptr = nothing to stop.
void setVoiceReleaseHook(std::function<void()> hook);
// The old INT8 model is installed (every file with its size) and the official one is not: the
// update can be offered (the Options row shows).
bool coachModelUpdateAvailable();
// Opens the prompt in its update form (the Options row's button).
void openModelUpdatePrompt();

}  // namespace game
