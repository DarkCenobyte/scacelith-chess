# Licence texts of the AppImage runtime

The Linux AppImage (`tools/linux/make-appimage.sh`) begins with the
[AppImage type 2 runtime](https://github.com/AppImage/type2-runtime), release 20251108
(`runtime-x86_64`, SHA-256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`),
the program that mounts or extracts the rest of the file. It is statically linked with the
libraries below, as its `LICENSE` and the list that `<file>.AppImage --appimage-help` prints say.
The script copies these files into the AppImage's `usr/share/doc/scacelith/licences/AppImage-runtime/`.
The runtime's repository holds its source code and how to build it.

| File | Part | Licence | Taken from |
|---|---|---|---|
| `type2-runtime-LICENSE.txt` | the runtime | MIT, © probonopd (with the list of what it links) | AppImage/type2-runtime, `LICENSE` |
| `musl-COPYRIGHT.txt` | musl libc | MIT, © Rich Felker et al. | musl v1.2.5, `COPYRIGHT` |
| `libfuse-LGPL-2.1.txt` | libfuse | LGPL-2.1 | libfuse fuse-3.15.0, `LGPL2.txt` |
| `squashfuse-LICENSE.txt` | squashfuse | BSD-2-Clause, © Dave Vasilevsky, Phillip Lougher | squashfuse 0.5.2, `LICENSE` |
| `zstd-LICENSE.txt` | zstd | BSD-3-Clause, © Meta Platforms, Inc. and affiliates | zstd v1.5.6, `LICENSE` |
| `zlib-LICENSE.txt` | zlib | zlib licence, © Jean-loup Gailly and Mark Adler | zlib v1.3.1, `LICENSE` |

The versions are those the runtime binary names (squashfuse 0.5.2, zlib 1.3.1, libfuse 3.15.0,
zstd 1.5.6); musl's text is the same in every recent release. When the pinned runtime changes,
check its list again and update these files.
