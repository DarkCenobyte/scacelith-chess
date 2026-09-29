// Hot-seat (two players on one PC) at the table: the players, the caption naming the player whose
// turn begins, and the draw offer card. Same look as ui_screens.cpp and the viewer's overlay.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../i18n/i18n.h"
#include <algorithm>
#include <cmath>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;

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
float ease(float t) { return m::smootherstep(t); }
std::string T(const char* key) { return i18n::tr(key); }
std::string L(const char* key) { return std::string(i18n::tr(key)) + "##" + key; }

// A dark band like the notifications (fades out at both ends).
void band(const Rect& r, float a) {
    float edge = 0.25f;
    vec4 d(0.02f, 0.017f, 0.015f, 0.74f * a), z(0.02f, 0.017f, 0.015f, 0.0f);
    gfx::fillH(Rect(r.x, r.y, r.w * edge, r.h), z, d);
    gfx::fill(Rect(r.x + r.w * edge, r.y, r.w * (1.0f - 2.0f * edge), r.h), d);
    gfx::fillH(Rect(r.r() - r.w * edge, r.y, r.w * edge, r.h), d, z);
    gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, r.y, withAlpha(gold, 0.55f * a), 0.45f);
    gfx::hlineFade(r.x + 40.0f, r.r() - 40.0f, r.b() - 1.0f, withAlpha(gold, 0.55f * a), 0.45f);
}

}  // namespace

HotSeatAction hotSeatHud(const HotSeatHud& hud) {
    HotSeatAction act = HotSeatAction::None;
    vec2 v = gfx::viewSize();
    const Rect screen(0.0f, 0.0f, v.x, v.y);
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MAIN);

    // The players, top left (mirrored in a right-to-left language); a diamond marks the one to move.
    {
        TextStyle side = style(font::FACE_TITLE, 17.0f, gold, im::startAlign(), 0.22f);
        TextStyle name = style(font::FACE_TEXT, 24.0f, ivory, im::startAlign());
        TextStyle rate = style(font::FACE_ITALIC, 20.0f, muted, im::startAlign());
        const char* sides[2] = {"viewer.white", "viewer.black"};
        float sw = std::max(gfx::textWidth(T(sides[0]), side), gfx::textWidth(T(sides[1]), side));
        float nw = 0.0f;
        for (int i = 0; i < 2; ++i) {
            float w = gfx::textWidth(hud.names[i], name);
            if (!hud.ratings[i].empty()) w += 14.0f + gfx::textWidth(hud.ratings[i], rate);
            nw = std::max(nw, w);
        }
        float x = 56.0f;
        Rect p(x - 16.0f, 40.0f, 48.0f + sw + 20.0f + nw + 34.0f, 124.0f);
        im::panel(im::flip(screen, p), 0.82f);
        float tx = p.x + 48.0f;
        for (int i = 0; i < 2; ++i) {
            float y = p.y + 52.0f + float(i) * 38.0f;
            bool toMove = hud.toMove == i;
            if (toMove) gfx::diamond(vec2(im::flipX(screen, tx - 18.0f), y - 7.0f), 4.0f, goldBright);
            side.color = toMove ? goldBright : gold;
            gfx::text(T(sides[i]), im::flipX(screen, tx), y, side);
            name.color = toMove ? ivory : ivoryDim;
            float w = gfx::text(hud.names[i], im::flipX(screen, tx + sw + 20.0f), y + 1.0f, name);
            if (!hud.ratings[i].empty())
                gfx::text(hud.ratings[i], im::flipX(screen, tx + sw + 20.0f + w + 14.0f), y + 1.0f, rate);
        }
    }

    // Whose turn it is, bottom centre: shown while the view goes over and a moment after.
    if (!hud.caption.empty()) {
        const float hold = 2.2f, fadeOut = 0.8f;
        float a = std::min(1.0f, hud.captionAge / 0.3f) * (1.0f - m::saturate((hud.captionAge - hold) / fadeOut));
        if (a > 0.01f) {
            TextStyle ts = style(font::FACE_TITLE, 24.0f, withAlpha(goldBright, a), HAlign::Center, 0.16f);
            float w = gfx::textWidth(hud.caption, ts) + 240.0f;
            Rect r(v.x * 0.5f - w * 0.5f, v.y - 150.0f + (1.0f - ease(a)) * 8.0f, w, 56.0f);
            band(r, a);
            gfx::text(hud.caption, r.cx(), r.cy() + 8.0f, ts);
        }
    }

    // A draw offered by the other player: a card on the end side, mouse buttons only (Space
    // presses the clock). Touching a piece declines it (the game does that).
    gfx::setLayer(gfx::LAYER_OVERLAY);
    im::Anim& da = im::anim(im::makeId("##hotseat.drawcard"));
    da.v[0] = im::approach(da.v[0], hud.drawOffer ? 1.0f : 0.0f, 10.0f);
    static std::string lastText;  // kept while the card fades out
    if (hud.drawOffer) lastText = hud.drawOfferText;
    if (da.v[0] > 0.01f) {
        float t = ease(da.v[0]);
        float w = 460.0f, h = 196.0f;
        Rect r = im::flip(screen, Rect(v.x - w - 40.0f + (1.0f - t) * 30.0f, v.y * 0.5f - h * 0.5f + 60.0f, w, h));
        im::captureMouseRect(r);
        gfx::pushAlpha(t);
        if (!hud.drawOffer) im::pushBlock();
        im::panel(r);
        TextStyle cap = style(font::FACE_TITLE, 19.0f, gold, HAlign::Center, 0.2f);
        gfx::text(T("hotseat.draw.card_title"), r.cx(), r.y + 46.0f, cap);
        TextStyle ts = style(font::FACE_ITALIC, kSmall, ivory, HAlign::Center);
        ts.size = gfx::fitSize(lastText, ts, w - 40.0f, 0.75f);
        gfx::text(lastText, r.cx(), r.y + 82.0f, ts);
        TextStyle hs = style(font::FACE_ITALIC, 18.0f, muted, HAlign::Center);
        hs.size = gfx::fitSize(T("hotseat.draw.card_hint"), hs, w - 40.0f, 0.75f);
        gfx::text(T("hotseat.draw.card_hint"), r.cx(), r.y + 110.0f, hs);
        float bw = 170.0f, bh = 48.0f, gap = 20.0f;
        float y = r.b() - 24.0f - bh;
        im::pushId("hotseatdraw");
        if (im::button(L("hotseat.draw.decline"), im::flip(r, Rect(r.cx() - gap * 0.5f - bw, y, bw, bh)), im::ButtonKind::Secondary,
                       true, im::ITEM_MOUSE_ONLY))
            act = HotSeatAction::DeclineDraw;
        if (im::button(L("hotseat.draw.accept"), im::flip(r, Rect(r.cx() + gap * 0.5f, y, bw, bh)), im::ButtonKind::Primary, true,
                       im::ITEM_MOUSE_ONLY))
            act = HotSeatAction::AcceptDraw;
        im::popId();
        if (!hud.drawOffer) im::popBlock();
        gfx::popAlpha();
    }
    gfx::setLayer(prev);
    return act;
}

}  // namespace ui
