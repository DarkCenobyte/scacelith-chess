// The core helpers the other tests leave out: an embedded file that does not exist, icon entries
// whose bitmap the X11 icon cannot use (not 32 bits, compressed, too large, not square), and a
// settings file that cannot replace what is at its path (its temporary copy removed).
#include "test.h"
#include "headless.h"
#include "core/embedded.h"
#include "core/image.h"
#include "core/ini.h"
#include "net/net_sys.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

void le(std::vector<uint8_t>& v, uint64_t x, int bytes) {
    for (int i = 0; i < bytes; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// An icon with one bitmap entry: the BITMAPINFOHEADER fields given, w x w pixels of 32 bits.
std::vector<uint8_t> icon(int32_t w, int32_t h2, uint16_t bits, uint32_t compression) {
    std::vector<uint8_t> dib;
    le(dib, 40, 4); le(dib, uint32_t(w), 4); le(dib, uint32_t(h2), 4); le(dib, 1, 2); le(dib, bits, 2);
    le(dib, compression, 4);
    for (int i = 0; i < 5; ++i) le(dib, 0, 4);
    const size_t pixels = w > 0 && w <= 512 ? size_t(w) * size_t(w) * 4 : 16;
    dib.resize(dib.size() + pixels + size_t(w > 0 ? w : 1) * 4, 0x7F);
    std::vector<uint8_t> ico = {0, 0, 1, 0, 1, 0};
    ico.insert(ico.end(), {uint8_t(w), uint8_t(w), 0, 0, 1, 0, uint8_t(bits), 0});
    le(ico, dib.size(), 4);
    le(ico, 6 + 16, 4);
    ico.insert(ico.end(), dib.begin(), dib.end());
    return ico;
}

}  // namespace

TEST(core_gaps_embedded_file_not_found) {
    CHECK(embedded::find("assets/gaps/no-such-file.txt") == nullptr);
    CHECK(embedded::text("assets/gaps/no-such-file.txt").empty());
    CHECK(!embedded::text("assets/i18n/en.lang").empty());
}

TEST(core_gaps_ico_bitmaps_refused) {
    std::vector<uint8_t> good = icon(2, 4, 32, 0);
    CHECK_EQ(image::icoImages(good.data(), good.size()).size(), size_t(2 + 4));
    for (const std::vector<uint8_t>& bad : {icon(2, 4, 24, 0),     // 24 bits per pixel
                                             icon(2, 4, 32, 3),     // compressed (BI_BITFIELDS)
                                             icon(2, 2, 32, 0),     // the height not doubled
                                             icon(0, 0, 32, 0),     // no width
                                             icon(300, 600, 32, 0)})   // wider than an icon
        CHECK(image::icoImages(bad.data(), bad.size()).empty());
    // A header that says it is longer than its entry.
    std::vector<uint8_t> longHeader = good;
    longHeader[6 + 16] = 0xFF;
    longHeader[6 + 16 + 1] = 0x0F;
    CHECK(image::icoImages(longHeader.data(), longHeader.size()).empty());
}

TEST(core_gaps_ini_cannot_replace_a_folder) {
    // The path is a folder (with a file in it): neither the renamed copy nor the direct write can
    // take its place; the temporary copy does not stay behind.
    headless::TempDir dir("core-gaps-ini");
    const std::string path = dir.file("Scacelith.ini");
    CHECK(net::sys::makeDirectories(path + "/"));
    std::FILE* f = net::sys::openFile(path + "/inside.txt", "wb");
    CHECK(f != nullptr);
    if (f) std::fclose(f);
    IniFile ini;
    ini.set("game.language", "fr");
    CHECK(!ini.save(path));
    CHECK(!net::sys::fileExists(path + ".tmp"));
    CHECK(net::sys::fileExists(path + "/inside.txt"));
    // Next to it, the same settings save.
    CHECK(ini.save(dir.file("other.ini")));
    IniFile back;
    CHECK(back.load(dir.file("other.ini")));
    CHECK_EQ(back.getString("game.language"), std::string("fr"));
}
