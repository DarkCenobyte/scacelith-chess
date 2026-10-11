# README badges

The badges of the [README](../../README.md) whose text never changes are the files of this folder
rather than images from img.shields.io. GitHub shows outside images through its image proxy, which
gives up after about four seconds; shields.io often answers later than that, and each such badge
then shows as a broken image. The CI, Release build and CodeQL badges are GitHub's own workflow
badges, and the coverage badge is `coverage.svg` on the `badges` branch, written by the Coverage
workflow ([DEVELOPMENT.md](../DEVELOPMENT.md#test-coverage)). Only the badges whose value changes
by itself (latest release, downloads, online protocol, website and official server status, dedicated
server version) still come from shields.io, with a one-hour cache where the value changes rarely.

Each file is the shields.io badge of the table below, saved by `tools/readme_badge.py`, which also
writes the logo into the file (shields.io links it as a `data:` URI). To change one, change its URL
here and save it again from the repository's root:

```sh
python3 -I tools/readme_badge.py '<URL>' docs/badges/<file>
```

| File | shields.io URL |
|---|---|
| `license.svg` | `https://img.shields.io/badge/license-GPL--3.0-blue` |
| `cplusplus-17.svg` | `https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white` |
| `opengl-4.6.svg` | `https://img.shields.io/badge/OpenGL-4.6-5586A4?logo=opengl&logoColor=white` |
| `stockfish-19.svg` | `https://img.shields.io/badge/Stockfish-19-2B2B2B` |
| `windows-x64.svg` | `https://img.shields.io/badge/Windows-x64-0078D4` |
| `linux.svg` | `https://img.shields.io/badge/Linux-x86--64%20%7C%20aarch64-FCC624?logo=linux&logoColor=black` |
| `macos.svg` | `https://img.shields.io/badge/macOS-arm64%20%28experimental%29-555555?logo=apple&logoColor=white` |
| `claude-code.svg` | `https://img.shields.io/badge/built%20with-Claude%20Code-D97757?logo=claude&logoColor=white` |
