// Internal to the macOS layer (platform_cocoa.mm): the OpenGL 4.6 core context and the readback of
// each finished frame, without the GL headers (the Objective-C++ layer includes AppKit, Metal and
// QuartzCore only).
//
// macOS has no OpenGL 4.6 driver: the game draws with Mesa's Zink (OpenGL on Vulkan) over
// KosmicKrisp (Vulkan on Metal), both in the application bundle (Contents/Frameworks), through
// Mesa's EGL on its surfaceless platform. The context draws into a pbuffer: its default framebuffer
// (FB 0) is what the renderer draws into on every platform. Each finished frame is flipped into a
// texture of its own (GL's rows go up, Metal's go down) and read into one of three pixel buffers
// with a fence; the layer copies the newest frame that has arrived into its CAMetalLayer, one frame
// later, so that the CPU never waits for the GPU's current frame.
#pragma once
#include <cstddef>
#include <string>

namespace plat {
namespace macgl {

// Sets the variables Mesa and the Vulkan loader read (only those the user has not set), loads
// libEGL, then makes the display, the context (4.6 core; KHR_debug when 'debug'), a pbuffer of
// width x height pixels, and loads GL. false: why says what failed (the details are in the log).
bool create(int width, int height, bool debug, std::string& why);
// Destroys what create() made (the libraries stay loaded).
void destroy();
// The pbuffer follows the window's size in pixels (a new one, FB 0's contents are lost). False when
// the new one could not be made: the previous one stays.
bool resize(int width, int height);
int surfaceWidth();
int surfaceHeight();

// The frame the renderer has finished in FB 0 starts its way to the CPU (asynchronously). The GL
// state it touches is restored. Waits only when the GPU is three frames behind.
void endFrame();
// The newest frame whose pixels have arrived, the one endFrame() just started excepted: its pixels
// as BGRA bytes (Metal's MTLPixelFormatBGRA8Unorm), top row first, mapped until releaseFrame().
// Waits a short moment for the previous frame; false when none has arrived (nothing new to show).
struct Frame {
    const unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    size_t stride = 0;   // bytes per row
};
bool takeFrame(Frame& frame);
void releaseFrame();
// Waits until the GPU has finished every frame (nothing to show: the window is hidden).
void finish();

}  // namespace macgl
}  // namespace plat
