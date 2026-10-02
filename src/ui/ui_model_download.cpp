// The coach's voice model download: the prompt that offers it (what it is, its size, its licence
// with the OpenRAIL-M text one click away, the acceptance line, Download / Not now) and the
// progress panel of the download (bar, megabytes, the host in use, Cancel; the reason and Retry /
// Close when it failed). The game owns the job and the decisions (game/coach_model.h); this file
// only draws and reports the player's choice. Same look as ui_screens.cpp and ui_coach.cpp;
// mirrored with im::flip / im::flipX in a right-to-left language.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../core/embedded.h"
#include "../i18n/i18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;
using namespace detail::helpers;

namespace {

// "145.3" with the language's decimal separator.
std::string megabytes(double bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", bytes / 1e6);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos) s.replace(dot, 1, i18n::tr("number.decimal"));
    return s;
}

const char* const kLicencePath = "assets/licences/Supertonic-3-OpenRAIL-M.txt";

struct PromptState {
    bool licence = false;             // the licence text is shown instead of the prompt
    bool forceLicence = false;        // debug::showModelLicence(): open on the licence
    float scroll = 0.0f, target = 0.0f;
    uint64_t lastFrame = 0;           // last frame the prompt was drawn
    bool blockPushed = false;         // modelDownloadBeginFrame() blocked the input below it
    std::function<void()> coachEntryHook;
};
PromptState g_prompt;

// The licence text laid out once for a width: its lines (no '\r', tabs expanded) and the number
// of wrapped rows each takes.
struct LicenceLines {
    float width = 0.0f;
    int generation = -1;
    std::vector<std::string> lines;
    std::vector<int> rows;
    int totalRows = 0;
};
const LicenceLines& licenceLines(float width, const TextStyle& st) {
    static LicenceLines lay;
    if (std::fabs(lay.width - width) < 0.5f && lay.generation == font::atlasGeneration()) return lay;
    lay = LicenceLines();
    lay.width = width;
    lay.generation = font::atlasGeneration();
    std::string text = embedded::text(kLicencePath), line;
    auto flush = [&]() {
        int n = line.empty() || gfx::textWidth(line, st) <= width ? 1 : std::max(1, gfx::wrapLineCount(line, width, st));
        lay.lines.push_back(line);
        lay.rows.push_back(n);
        lay.totalRows += n;
        line.clear();
    };
    for (char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') flush();
        else if (ch == '\t') line += "    ";
        else line += ch;
    }
    if (!line.empty()) flush();
    return lay;
}

// The licence inside the prompt's card: the text (left to right: it is in English), scrolled with
// the wheel, the arrows, PageUp / PageDown, Home / End; Back returns to the prompt.
void licenceView(const Rect& r, bool opened) {
    im::pageTitle(T("coach.download.licence_title"), r.cx(), r.y + 72.0f);
    TextStyle ns = style(font::FACE_ITALIC, 20.0f, muted, HAlign::Center);
    ns.size = gfx::fitSize(T("licences.note"), ns, r.w - 120.0f);
    gfx::text(T("licences.note"), r.cx(), r.y + 128.0f, ns);
    Rect area(r.x + 60.0f, r.y + 150.0f, r.w - 138.0f, r.h - 150.0f - 120.0f);
    TextStyle ts = style(font::FACE_TEXT, 20.0f, ivoryDim, HAlign::Left);
    ts.dir = 0;
    const float lineH = 27.0f;
    const LicenceLines& lay = licenceLines(area.w, ts);
    float contentH = float(lay.totalRows) * lineH + 16.0f;
    float maxScroll = std::max(0.0f, contentH - area.h);
    float& target = g_prompt.target;
    if (area.contains(im::mouse()) && im::wheel() != 0.0f) target -= im::wheel() * lineH * 3.0f;
    if (im::keyPressed(plat::KEY_PAGEDOWN)) target += area.h * 0.85f;
    if (im::keyPressed(plat::KEY_PAGEUP)) target -= area.h * 0.85f;
    if (im::keyPressed(plat::KEY_HOME)) target = 0.0f;
    if (im::keyPressed(plat::KEY_END)) target = maxScroll;
    int dy = 0;
    if (im::consumeNavigation(nullptr, &dy)) target += float(dy) * lineH * 2.0f;
    target = m::clamp(target, 0.0f, maxScroll);
    g_prompt.scroll = opened ? target : std::min(im::approach(g_prompt.scroll, target, 16.0f), maxScroll);
    float scroll = g_prompt.scroll;
    gfx::pushClip(Rect(area.x - 4.0f, area.y, area.w + 8.0f, area.h));
    float y = area.y + 20.0f - scroll;
    for (size_t i = 0; i < lay.lines.size(); ++i) {
        float h = float(lay.rows[i]) * lineH;
        if (y + h > area.y - lineH && y - lineH < area.b() && !lay.lines[i].empty()) {
            if (lay.rows[i] == 1) gfx::text(lay.lines[i], area.x, y, ts);
            else gfx::textWrapped(lay.lines[i], area.x, y, area.w, ts, lineH);
        }
        y += h;
    }
    gfx::popClip();
    if (maxScroll > 0.5f) {   // the scroll indicator and the fades at the edges
        float bh = area.h * area.h / contentH;
        gfx::fill(Rect(area.r() + 12.0f, area.y + (area.h - bh) * (scroll / maxScroll), 2.0f, bh), withAlpha(gold, 0.35f), 1.0f);
        vec4 pc(0.035f, 0.03f, 0.027f, 1.0f), pz(0.035f, 0.03f, 0.027f, 0.0f);
        if (scroll > 0.5f) gfx::fillV(Rect(area.x, area.y, area.w, 26.0f), pc, pz);
        if (scroll < maxScroll - 0.5f) gfx::fillV(Rect(area.x, area.b() - 26.0f, area.w, 26.0f), pz, pc);
    }
}

}  // namespace

// ==== Hooks ===============================================================================================
namespace detail {
// From ui::beginFrame: while the prompt is open (drawn on the previous frame), everything drawn
// before it this frame (the menu page, the pause menu, the HUD) is blocked; modelPrompt() lifts
// the block for itself.
void modelDownloadBeginFrame() {
    g_prompt.blockPushed = false;
    if (g_prompt.lastFrame != 0 && g_prompt.lastFrame + 1 == im::frame()) {
        im::pushBlock();
        g_prompt.blockPushed = true;
    }
}
void coachEntryOpened() {
    if (g_prompt.coachEntryHook) g_prompt.coachEntryHook();
}
}  // namespace detail

namespace debug {
void showModelLicence() { g_prompt.forceLicence = true; }
}  // namespace debug

void setCoachEntryHook(std::function<void()> hook) { g_prompt.coachEntryHook = std::move(hook); }

// ==== The prompt ==========================================================================================
ModelPromptAction modelPrompt(const ModelPrompt& p) {
    if (g_prompt.blockPushed) {
        im::popBlock();
        g_prompt.blockPushed = false;
    }
    im::Id id = im::makeId("##modelprompt");
    im::Anim& a = im::anim(id);
    bool first = a.firstFrame == im::frame();
    if (first) {
        g_prompt.licence = g_prompt.forceLicence;   // the ui viewer may open it on the licence
        g_prompt.forceLicence = false;
        g_prompt.scroll = g_prompt.target = 0.0f;
        a.v[0] = a.v[1] = 0.0f;
        im::sound(Sound::Open);
    }
    g_prompt.lastFrame = im::frame();
    a.v[0] = im::approach(a.v[0], 1.0f, 10.0f);
    float t = ease(a.v[0]);
    im::captureMouseAll();
    im::captureKeyboard();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MODAL);
    vec2 v = gfx::viewSize();
    gfx::fill(Rect(0, 0, v.x, v.y), vec4(0, 0, 0, 0.6f * t));
    gfx::pushAlpha(t);
    im::pushId("modelprompt");
    ModelPromptAction act = ModelPromptAction::None;

    if (g_prompt.licence) {
        float w = std::min(1240.0f, v.x - 80.0f), h = std::min(960.0f, v.y - 60.0f);
        Rect r(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
        gfx::fill(r, vec4(0.035f, 0.03f, 0.027f, 1.0f), 3.0f);
        im::panel(r);
        bool opened = a.v[1] < 0.5f;
        a.v[1] = 1.0f;
        licenceView(r, opened);
        float bw = 260.0f, bh = 56.0f, by = r.b() - 40.0f - bh;
        gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, by - 22.0f, withAlpha(gold, 0.25f), 0.3f);
        im::Id backId = im::makeId("##coach.download.back");
        im::setDefaultFocus(backId);
        bool back = im::button(L("coach.download.back"), Rect(r.cx() - bw * 0.5f, by, bw, bh), im::ButtonKind::Secondary);
        if (!back && im::consumeBack()) {
            back = true;
            im::sound(Sound::Back);
        }
        if (back) {
            g_prompt.licence = false;
            a.v[1] = 0.0f;
        }
    } else {
        // What it is, its size, where it goes; the licence and the acceptance line; the choice.
        float w = std::min(1000.0f, v.x - 80.0f);
        const float textW = w - 140.0f;
        TextStyle bs = style(font::FACE_TEXT, 25.0f, ivoryDim, im::startAlign());
        TextStyle ls = style(font::FACE_TEXT, 23.0f, ivoryDim, im::startAlign());
        TextStyle as = style(font::FACE_ITALIC, 23.0f, goldBright, im::startAlign());
        TextStyle fs = style(font::FACE_ITALIC, 19.0f, muted, im::startAlign());
        const std::string mb = megabytes(p.bytes);
        std::string body = i18n::trf("coach.download.text", {mb});
        std::string licence = T("coach.download.licence");
        std::string accept = T("coach.download.accept");
        std::string folder = i18n::trf("coach.download.folder", {i18n::ltr(p.folder)});
        const float bodyH = 34.0f, smallH = 31.0f;
        int bodyLines = gfx::wrapLineCount(body, textW, bs);
        int licLines = gfx::wrapLineCount(licence, textW, ls);
        int accLines = gfx::wrapLineCount(accept, textW, as);
        int folderLines = std::min(2, gfx::wrapLineCount(folder, textW, fs));
        float h = 150.0f + bodyH * float(bodyLines) + 26.0f + smallH * float(licLines) + 18.0f + 54.0f + 26.0f +
                  smallH * float(accLines) + 14.0f + 26.0f * float(folderLines) + 36.0f + 58.0f + 44.0f;
        h = std::min(h, v.y - 40.0f);
        Rect r(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
        gfx::fill(r, vec4(0.035f, 0.03f, 0.027f, 1.0f), 3.0f);   // opaque: hide the page below
        im::panel(r);
        im::pageTitle(T("coach.download.title"), r.cx(), r.y + 76.0f);
        const float x = im::flipX(r, r.x + 70.0f);
        float y = r.y + 150.0f;
        gfx::textWrapped(body, x, y, textW, bs, bodyH);
        y += bodyH * float(bodyLines) + 26.0f;
        // The licence: its name, the restrictions in a sentence, the full text one click away.
        gfx::diamond(vec2(im::flipX(r, r.x + 56.0f), y - 8.0f), 3.5f, withAlpha(gold, 0.9f));
        gfx::textWrapped(licence, x, y, textW, ls, smallH);
        y += smallH * float(licLines) + 18.0f;
        float lbw = std::min(textW, std::max(300.0f, gfx::textWidth(T("coach.download.read_licence"), style(font::FACE_TEXT, kButton, ivory)) + 70.0f));
        if (im::button(L("coach.download.read_licence"), im::flip(r, Rect(r.x + 70.0f, y - 6.0f, lbw, 48.0f)), im::ButtonKind::Secondary)) {
            g_prompt.licence = true;
            g_prompt.scroll = g_prompt.target = 0.0f;
            a.v[1] = 0.0f;
        }
        y += 54.0f + 26.0f;
        gfx::textWrapped(accept, x, y, textW, as, smallH);
        y += smallH * float(accLines) + 14.0f;
        fs.size = gfx::fitSize(folder, fs, textW * float(folderLines), 0.8f);
        gfx::textWrapped(folder, x, y, textW, fs, 26.0f);
        // Buttons: Not now first in the reading direction, Download (the size on it) after.
        float bw = 300.0f, bh = 58.0f, gap = 30.0f, by = r.b() - 44.0f - bh;
        gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, by - 24.0f, withAlpha(gold, 0.25f), 0.3f);
        std::string dl = i18n::trf("coach.download.download", {mb}) + "##coach.download.download";
        im::Id dlId = im::makeId("##coach.download.download");
        if (first) im::setFocus(dlId);
        im::setDefaultFocus(dlId);
        if (im::button(L("coach.download.not_now"), im::flip(r, Rect(r.cx() - gap * 0.5f - bw, by, bw, bh)), im::ButtonKind::Secondary))
            act = ModelPromptAction::NotNow;
        if (im::button(dl, im::flip(r, Rect(r.cx() + gap * 0.5f, by, bw, bh)), im::ButtonKind::Primary))
            act = ModelPromptAction::Download;
        if (act == ModelPromptAction::None && !first && im::consumeBack()) {
            act = ModelPromptAction::NotNow;
            im::sound(Sound::Back);
        }
        if (act == ModelPromptAction::Download) im::sound(Sound::Confirm);
    }
    im::popId();
    gfx::popAlpha();
    gfx::setLayer(prev);
    if (act != ModelPromptAction::None) {
        g_prompt.lastFrame = 0;   // closed: nothing is blocked on the next frame
        g_prompt.licence = false;
    }
    return act;
}

// ==== The progress panel ===================================================================================
ModelPanelAction modelProgressPanel(const ModelProgressView& pv) {
    using State = ModelProgressView::State;
    ModelPanelAction act = ModelPanelAction::None;
    im::Id id = im::makeId("##modelpanel");
    im::Anim& a = im::anim(id);
    static ModelProgressView last;    // kept while the panel fades out
    if (pv.state != State::Hidden) last = pv;
    a.v[0] = im::approach(a.v[0], pv.state != State::Hidden ? 1.0f : 0.0f, 9.0f);
    // The bar moves smoothly between progress reports.
    float frac = last.total > 0.0 ? float(std::min(1.0, last.done / last.total)) : 0.0f;
    a.v[1] = pv.state == State::Hidden ? a.v[1] : (frac < a.v[1] - 0.02f ? frac : im::approach(a.v[1], frac, 8.0f));
    if (a.v[0] < 0.01f) {
        a.v[1] = 0.0f;
        return act;
    }
    float t = ease(a.v[0]);
    vec2 v = gfx::viewSize();
    const Rect screen(0.0f, 0.0f, v.x, v.y);
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);
    const bool failed = last.state == State::Failed;
    const float w = 520.0f;
    TextStyle es = style(font::FACE_TEXT, 21.0f, ivoryDim, im::startAlign());
    int errLines = failed ? std::min(4, gfx::wrapLineCount(last.error, w - 64.0f, es)) : 0;
    float h = failed ? 150.0f + 28.0f * float(errLines) + 64.0f : 240.0f;
    Rect r = im::flip(screen, Rect(v.x - w - 36.0f + (1.0f - t) * 24.0f, 36.0f, w, h));
    im::captureMouseRect(r);
    gfx::pushAlpha(t);
    if (pv.state == State::Hidden) im::pushBlock();
    gfx::shadow(r.offset(0, 8), 3, 30, vec4(0, 0, 0, 0.5f));
    im::panel(r);
    const float x = im::flipX(r, r.x + 32.0f), innerW = w - 64.0f;
    // Caption: "THE COACH'S VOICE", set like the section labels.
    TextStyle cap = style(font::FACE_TITLE, 17.0f, gold, im::startAlign(), 0.2f);
    cap.size = gfx::fitSize(T("coach.download.caption"), cap, innerW);
    gfx::text(T("coach.download.caption"), x, r.y + 40.0f, cap);
    gfx::hlineFade(r.x + 24.0f, r.r() - 24.0f, r.y + 54.0f, withAlpha(gold, 0.3f), 0.4f);
    im::pushId("modelpanel");
    if (!failed) {
        // What is happening, and the megabytes when something is being downloaded.
        std::string what, amount;
        switch (last.state) {
            case State::Checking: what = T("coach.download.checking"); break;
            case State::Extracting: what = T("coach.download.extracting"); break;
            default:
                what = T("coach.download.downloading");
                amount = i18n::trf("coach.download.megabytes", {megabytes(last.done), megabytes(last.total)});
                break;
        }
        TextStyle ws = style(font::FACE_TEXT, 23.0f, ivory, im::startAlign());
        TextStyle ms = style(font::FACE_TEXT, 21.0f, goldBright, im::endAlign());
        float aw = amount.empty() ? 0.0f : gfx::textWidth(amount, ms) + 16.0f;
        ws.size = gfx::fitSize(what, ws, innerW - aw);
        gfx::text(what, x, r.y + 92.0f, ws);
        if (!amount.empty()) gfx::text(amount, im::flipX(r, r.r() - 32.0f), r.y + 92.0f, ms);
        // The bar (fills from the start side).
        Rect bar(r.x + 32.0f, r.y + 108.0f, innerW, 10.0f);
        gfx::fill(bar, vec4(0.0f, 0.0f, 0.0f, 0.45f), 5.0f);
        gfx::stroke(bar, withAlpha(gold, 0.35f), 0.0f, 5.0f);
        float fw = std::max(10.0f, innerW * m::clamp(a.v[1], 0.0f, 1.0f));
        Rect fill = im::flip(r, Rect(bar.x, bar.y, fw, bar.h));
        vec4 deep = withAlpha(goldDeep, 0.95f), bright = withAlpha(goldBright, 0.95f);
        if (im::rtl()) gfx::fillH(fill, bright, deep, 5.0f);
        else gfx::fillH(fill, deep, bright, 5.0f);
        // Where from: "From Hugging Face" / "From GitHub (release archive)", then the repository
        // ("huggingface.co/csukuangfj2/...", "github.com/k2-fsa/sherpa-onnx") as it is written.
        std::string from = T(last.github ? "coach.download.from_github" : "coach.download.from_hub");
        TextStyle fs = style(font::FACE_ITALIC, 20.0f, ivoryDim, im::startAlign());
        fs.size = gfx::fitSize(from, fs, innerW);
        gfx::text(from, x, r.y + 150.0f, fs);
        TextStyle us = style(font::FACE_TEXT, 17.0f, muted, im::startAlign());
        us.dir = 0;
        us.size = gfx::fitSize(last.sourceLabel, us, innerW, 0.5f);
        gfx::pushClip(Rect(r.x + 24.0f, r.y + 156.0f, w - 48.0f, 24.0f));
        gfx::text(last.sourceLabel, x, r.y + 174.0f, us);
        gfx::popClip();
        // Cancel, mouse only (the keyboard stays with the menus and the game).
        float bw = 170.0f, bh = 42.0f;
        if (im::button(L("coach.download.cancel"), im::flip(r, Rect(r.r() - 24.0f - bw, r.b() - 18.0f - bh, bw, bh)), im::ButtonKind::Quiet,
                       true, im::ITEM_MOUSE_ONLY))
            act = ModelPanelAction::Cancel;
        if (!last.file.empty() && !last.github) {   // a file of the hub (the archive is named above)
            TextStyle ns = style(font::FACE_TEXT, 17.0f, faint, im::startAlign());
            ns.dir = 0;
            ns.size = gfx::fitSize(last.file, ns, innerW - bw - 20.0f, 0.8f);
            gfx::text(last.file, x, r.b() - 32.0f, ns);
        }
    } else {
        TextStyle ts = style(font::FACE_TEXT, 23.0f, danger, im::startAlign());
        ts.size = gfx::fitSize(T("coach.download.failed"), ts, innerW);
        gfx::text(T("coach.download.failed"), x, r.y + 92.0f, ts);
        gfx::pushClip(Rect(r.x + 24.0f, r.y + 104.0f, w - 48.0f, 28.0f * float(errLines) + 16.0f));
        gfx::textWrapped(last.error, x, r.y + 130.0f, innerW, es, 28.0f);
        gfx::popClip();
        float bw = 170.0f, bh = 44.0f, gap = 14.0f, by = r.b() - 20.0f - bh;
        if (im::button(L("coach.download.close"), im::flip(r, Rect(r.r() - 24.0f - 2.0f * bw - gap, by, bw, bh)), im::ButtonKind::Secondary,
                       true, im::ITEM_MOUSE_ONLY))
            act = ModelPanelAction::Close;
        if (im::button(L("coach.download.retry"), im::flip(r, Rect(r.r() - 24.0f - bw, by, bw, bh)), im::ButtonKind::Primary, true,
                       im::ITEM_MOUSE_ONLY))
            act = ModelPanelAction::Retry;
    }
    im::popId();
    if (pv.state == State::Hidden) {
        im::popBlock();
        act = ModelPanelAction::None;
    }
    gfx::popAlpha();
    gfx::setLayer(prev);
    return act;
}

}  // namespace ui
