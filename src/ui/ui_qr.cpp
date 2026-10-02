// QR codes (the otpauth:// link of the two-factor setup page), with Nayuki's QR Code generator
// (third_party/qrcodegen, MIT licence) compiled into this unit.
#include "ui_screens_online.h"
#include "ui_theme.h"
#include "qrcodegen/qrcodegen.cpp"
#include <cmath>
#include <vector>

namespace ui {
namespace detail {

namespace {
struct QrCache {
    std::string text;
    int size = 0;
    std::vector<unsigned char> modules;
    bool ok = false;
};
QrCache g_qr;
}  // namespace

bool drawQrCode(const std::string& text, const gfx::Rect& r) {
    if (text.empty()) return false;
    if (g_qr.text != text) {
        g_qr = QrCache();
        g_qr.text = text;
        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            g_qr.size = qr.getSize();
            g_qr.modules.resize(size_t(g_qr.size * g_qr.size));
            for (int y = 0; y < g_qr.size; ++y)
                for (int x = 0; x < g_qr.size; ++x) g_qr.modules[size_t(y * g_qr.size + x)] = qr.getModule(x, y) ? 1 : 0;
            g_qr.ok = true;
        } catch (...) {
            g_qr.ok = false;
        }
    }
    if (!g_qr.ok) return false;
    // Light paper with a quiet zone of 4 modules; modules snapped to whole physical pixels so
    // the code stays crisp for the phone's camera.
    const int n = g_qr.size + 8;
    float px = gfx::px();
    float side = std::min(r.w, r.h);
    float module = std::floor(side / float(n) / px) * px;
    if (module <= 0.0f) module = side / float(n);
    float total = module * float(n);
    float ox = gfx::snap(r.x + (r.w - total) * 0.5f), oy = gfx::snap(r.y + (r.h - total) * 0.5f);
    gfx::shadow(gfx::Rect(ox, oy + 6.0f, total, total), 2.0f, 18.0f, m::vec4(0, 0, 0, 0.5f));
    gfx::fill(gfx::Rect(ox, oy, total, total), m::vec4(0.97f, 0.96f, 0.93f, 1.0f), 2.0f);
    const m::vec4 ink(0.05f, 0.05f, 0.06f, 1.0f);
    for (int y = 0; y < g_qr.size; ++y) {
        // Runs of dark modules in a row: one rectangle each.
        int x = 0;
        while (x < g_qr.size) {
            if (!g_qr.modules[size_t(y * g_qr.size + x)]) {
                ++x;
                continue;
            }
            int x0 = x;
            while (x < g_qr.size && g_qr.modules[size_t(y * g_qr.size + x)]) ++x;
            gfx::fill(gfx::Rect(ox + module * float(4 + x0), oy + module * float(4 + y), module * float(x - x0), module), ink);
        }
    }
    return true;
}

void clearQrCache() { g_qr = QrCache(); }

}  // namespace detail
}  // namespace ui
