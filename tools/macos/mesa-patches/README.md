# Patches applied to Mesa 26.2.4 for the macOS build

`tools/macos/build-deps.sh` applies these files, in the order of their names, to Mesa 26.2.4 (tag
`mesa-26.2.4`, commit `96cb43121031992b85767f9c1be8f3f48e22b1d2`) before building Zink and
KosmicKrisp. Each one fixes a fault of the Zink-on-KosmicKrisp path found while running a shipping
OpenGL game on Apple Silicon; they are candidates for upstream Mesa, not yet merged there.

Origin: the [RecoilEngine-AppleSilicon](https://github.com/benbreen/RecoilEngine-AppleSilicon)
project (macOS port of the Recoil engine for Beyond All Reason), `patches/mesa/`, fetched on
2026-10-10, by benbreen. The files are copied unchanged but for `0003`, adapted to 26.2.4 (see
its message; the series was written against a later development commit), and keep their numbers
in that series of 13 patches, of which only these five are taken. Mesa's code is under the MIT licence; the patches
modify it under the same terms.

| Patch | What it fixes |
|---|---|
| `0001-poly-barrier-after-reading-shared-scratch-out_ptr-in.patch` | A missing local memory barrier in the primitive-restart unrolling kernel (`src/poly/cl/restart.h`): a fast lane could reuse the shared scratch while a slow one still read a pointer from it (stray geometry). |
| `0003-kosmickrisp-reset-the-geometry-heap-exactly-once-per.patch` | KosmicKrisp reset its geometry heap again when a render pass restarted within a command buffer, overwriting the indices unrolled for an earlier draw. |
| `0006-kosmickrisp-zero-initialize-device-memory-allocation.patch` | Device memory from a Metal heap is recycled VRAM: it is now zeroed, as other drivers effectively do, so that sampling a never-rendered texture gives black instead of garbage. |
| `0011-zink-update-pipeline-on-vertex-elements-change-witho.patch` | Without `VK_EXT_vertex_input_dynamic_state` (KosmicKrisp has not got it), Zink reused a pipeline after a vertex layout change. |
| `0013-zink-enable-renderpass-tracking-for-kosmickrisp.patch` | Zink's render pass tracking, which every other tile-based driver has, is turned on for KosmicKrisp (fewer tile loads and stores per frame). |

All five apply cleanly to 26.2.4 (checked with `git apply --check`, with offsets), and call only
functions that 26.2.4 has: the original `0003` applied but did not compile. When Mesa is
updated, drop the patches that upstream has merged (the build stops when one does not apply) and
check the others again.
