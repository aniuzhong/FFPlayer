# FFPlayer

## v0.1.0

在 **不编译整套 FFmpeg 库** 的前提下，编译 **ffplay.exe** 。

目录结构：

```
third_party/
  ffmpeg-8.1-full_build-shared/   # gyan 完整包：include、lib、bin（含 ffplay/ffmpeg 等）
  SDL2-2.32.10/                   # SDL2 MSVC 开发包（含 cmake/sdl2-config.cmake、lib/x64/SDL2.dll 等）
  .cache/                         # CMake 下载的 .zip 缓存
src/
  ffplay.c, cmdutils.c, ...       # 与官方 fftools 对齐的源码快照
  compat/                         # va_copy.h、w32dlfcn.h
  shim/include/libavutil/         # 补头 + include 适配
cmake/
  FFmpegGyan.cmake                # 生成 config、configuration 探测
  config.h.in
  config_components.h.in
```

构建命令：

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

验证输出：

```powershell
.\build\Release\ffplay.exe -version
```