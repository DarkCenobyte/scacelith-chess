// Context bootstrap helpers usable without including the GL headers (the X11 platform layer
// cannot include glcorearb.h next to GL/glx.h).
#pragma once

namespace gl46 {
typedef void* (*GetProcFn)(const char* name);
int load(GetProcFn getProc, const char** firstMissing);
// Logs driver strings, installs the KHR_debug callback when 'debug' is set, and applies global
// state every module relies on (reverse-Z clip control, seamless cubemaps).
void afterContextCreated(bool debug);
}  // namespace gl46
