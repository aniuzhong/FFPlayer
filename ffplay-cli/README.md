# ffplay-cli — standalone ffplay.exe

Builds `ffplay.exe` **without compiling FFmpeg from source**.
Uses Gyan's FFmpeg 8.1 prebuilt shared libraries + SDL2 with CMake/MSVC.

> This exists to bootstrap the ffplay.c refactoring — the initial setup is
> tedious, so it's documented here.

## Dependencies

1. Gyan FFmpeg 8.1 shared bundle (include/ lib/ bin/)
2. SDL2 MSVC dev package
3. CMake + Visual Studio 2022
4. Upstream FFmpeg repo (for syncing fftools sources)

## Aligning with Gyan's prebuilt

### Identifying the Gyan commit

Gyan's `README.txt` lists the source commit:

```
Source Code: https://github.com/FFmpeg/FFmpeg/commit/<hash>
```

Checkout that commit in the upstream repo:

```powershell
git fetch origin
git checkout <hash>
git rev-parse HEAD   # verify match
```

### Syncing sources

With FFmpeg checked out at the matching commit:

1. Copy `ffmpeg/fftools/` and `ffmpeg/compat/` into `ffplay-cli/`.
2. Copy `ffmpeg/libavutil/getenv_utf8.h`, `wchar_filename.h` into `ffplay-cli/shim/include/libavutil/`.
   Keep the local include adaptations (`mem.h` → `libavutil/mem.h`; `getenv_utf8.h` uses `"wchar_filename.h"`).

### Matching `ffplay -version` output

`cmake/FFmpegGyan.cmake` → `ffplay_generate_config()` generates `build/generated/config.h`:

| Macro | Source | Purpose |
|-------|--------|---------|
| `FFMPEG_VERSION` | `include/libavutil/ffversion.h` | Match Gyan's version string (e.g. `8.1-full_build-www.gyan.dev`) |
| `FFMPEG_CONFIGURATION` | Extracted from `bin/ffplay.exe -hide_banner -version` via regex | Match Gyan's `configuration:` line so `opt_common.c` doesn't warn about mismatch |
| `CC_IDENT` | Same `-version` output (`built with ...`) | Match compiler identity line |
| License/feature macros | Hardcoded in `config.h.in` (GPLv3 + full features; `CONFIG_LIBPLACEBO=0`) | Satisfy fftools compile paths |

### `config_components.h`

Generated from `config_components.h.in`. Provides `CONFIG_RTSP_DEMUXER`, `CONFIG_MMSH_PROTOCOL` etc. consistent with Gyan's full build, so local `#if` guards match what the DLLs actually provide.

### Linking & runtime

| Aspect | Detail |
|--------|--------|
| **Link** | Import libs from `FFMPEG_PREBUILT_ROOT/lib` (avutil, avcodec, avformat, avfilter, avdevice, swscale, swresample) |
| **`-version` library lines** | Come from the loaded DLLs at runtime — identical to Gyan's `ffplay.exe` with the same DLLs |
| **POST_BUILD** | Copies `SDL2.dll` + `FFMPEG_PREBUILT_ROOT/bin/*.dll` next to the exe |

## Automated download

With `FFPLAY_FETCH_PREBUILT_DEPS=ON` (default), CMake downloads missing dependencies
at configure time:

| Component | URL (overridable via cache var) |
|-----------|---------------------------------|
| FFmpeg | `FFPLAY_FFMPEG_PREBUILT_URL` → [ffmpeg-8.1-full_build-shared.zip](https://github.com/GyanD/codexffmpeg/releases/download/8.1/ffmpeg-8.1-full_build-shared.zip) |
| SDL2 | `FFPLAY_SDL2_DEV_ZIP_URL` → [SDL2-devel-2.32.10-VC.zip](https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip) |

- Cache in `third_party/.cache/`, skipped if present.
- Both are zip files, extracted with `cmake -E tar xf` (no 7-Zip needed).

For offline builds: `-DFFPLAY_FETCH_PREBUILT_DEPS=OFF`.

## Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/ffplay.exe` + runtime DLLs.

### Key cache variables

| Variable | Default |
|----------|---------|
| `THIRD_PARTY_DIR` | `third_party` |
| `FFPLAY_FETCH_PREBUILT_DEPS` | `ON` |
| `FFPLAY_FFMPEG_PREBUILT_URL` | .../ffmpeg-8.1-full_build-shared.zip |
| `FFPLAY_SDL2_DEV_ZIP_URL` | .../SDL2-devel-2.32.10-VC.zip |
| `FFPLAY_SDL2_ROOT_DIRNAME` | `SDL2-2.32.10` |
| `FFMPEG_PREBUILT_ROOT` | `third_party/ffmpeg-8.1-full_build-shared` |
| `FFMPEG_LIB_DIR` | `$(FFMPEG_PREBUILT_ROOT)/lib` |
| `SDL2_DIR` | auto-detected |
