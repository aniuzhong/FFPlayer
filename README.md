# **FFPlayer**

## **`v0.1.0`**

在 **不编译整套 FFmpeg 库** 的前提下，编译 **ffplay.exe**。

> 在 Windows 上把 libplacebo 接进工程比较麻烦，所以不支持 Vulkan 渲染路径。

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
    config.h.in                     # 编译时没有启用 libplacebo（不支持 Vulkan 渲染路径）
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

## **`v0.1.1`**

删除 Vulkan 渲染路径相关代码。

- **`src/ffplay_renderer.c`**：整文件删除。
- **`src/ffplay_renderer.h`**：整文件删除。
- **`src/ffplay.c`**
    - 去掉 `#include "ffplay_renderer.h"`，在文件内**内联**原头文件中的 SDL 共用定义：`VIDEO_BACKGROUND_TILE_SIZE`、`enum VideoBackgroundType`、`typedef struct RenderParams`。
    - 删除 `enable_vulkan`、`vulkan_params`、`vk_renderer` 及所有相关分支（显示、`do_exit`、窗口事件 resize、主函数里创建窗口/渲染器）。
    - `video_image_display`：只保留 SDL 纹理路径。
    - `configure_video_filters`：`buffersink` 的 colorspaces 限制改为**始终**设置（原先仅在 `!vk_renderer` 时设置，与当前仅 SDL 行为一致）。
    - `create_hwaccel`：不再依赖 Vulkan；在指定 `-hwaccel` 时直接 `av_hwdevice_ctx_create(device_ctx, type, NULL, NULL, 0)`。
    - `选项表`：移除 -enable_vulkan、-vulkan_params。
    - `main`：创建窗口后**直接走** `SDL_CreateRenderer`（不再尝试 `SDL_WINDOW_VULKAN` / `vk_renderer_create`）。


1. FFmpeg DLL 链接的仍是 `8.1-full_build-www.gyan.dev` 那套带 `--enable-vulkan` 、`--enable-libplacebo` 等编译配置的库；变的是 ffplay.exe 源码，不再使用其中的 `Vulkan 窗口渲染 + libplacebo` 显示链路。

2. 相对改动前 **ffplay.c** 逻辑：`-hwaccel`(`create_hwaccel`) 不再被**必须先有 Vulkan 渲染器**的逻辑阻挡。

3. 解码器内部是否仍可用 Vulkan 由 -hwaccel 与 av_hwdevice_ctx_create 决定。

## feature/ffplay-imgui

- 去掉原版 ffplay.exe 的命令行功能（包括一些高价值的参数 `hwaccel/sync/framedrop/infbuf/genpts/af/vf`）
- 将 ffplay.c 拆分成更多的文件（文件层面模块化，但运行时边界仍然混乱）
    - 真正的**模块接口**被 `VideoState` 吞掉
- 引入 ImGUI

## 播放器化接口重构

- `MediaPlayer 风格`：命令式 API（setDataSource/prepare/start/pause/seekTo/release）
- `ExoPlayer 风格`：事件回调（IDLE/BUFFERING/READY/ENDED + listener）

```c++
class IPlayer {
public:
  virtual void setDataSource(const std::string& uri) = 0;
  virtual void prepareAsync() = 0;
  virtual void play() = 0;
  virtual void pause() = 0;
  virtual void stop() = 0;
  virtual void seekToMs(int64_t positionMs) = 0;
  virtual void setVolume(float volume01) = 0;
  virtual void setMuted(bool muted) = 0;
  virtual void setSurface(void* nativeWindowOrRenderer) = 0;
  virtual void release() = 0;

  virtual PlayerSnapshot getSnapshot() const = 0;
  virtual void addListener(IPlayerListener*) = 0;
};
```

- `setDataSource` -> stream_open（再拆成 open + prepare）
- `play/pause` -> stream_toggle_pause_and_clear_step（后续换成显式 play/pause）
- `seekToMs` -> stream_seek_relative/stream_seek
- `setVolume/setMuted` -> stream_set_volume + stream_toggle_mute（补 stream_set_muted(bool)）
- `render loop` -> video_renderer_refresh/display/present
- `release` -> stream_close