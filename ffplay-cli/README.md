# **ffplay-cli** - 独立编译 ffplay.exe

- [**ffplay-cli** - 独立编译 ffplay.exe](#ffplay-cli---独立编译-ffplayexe)
  - [依赖与环境](#依赖与环境)
  - [自编译 ffplay.exe 与 gyan 预编译包输出对齐](#自编译-ffplayexe-与-gyan-预编译包输出对齐)
    - [gyan 声明的 commit 在哪里](#gyan-声明的-commit-在哪里)
    - [与 gyan 原装 `ffplay -version` 文本一致（Configure 阶段）](#与-gyan-原装-ffplay--version-文本一致configure-阶段)
    - [`config_components.h`（仅影响少数 `#if`）](#config_componentsh仅影响少数-if)
    - [与 gyan 包内「库侧输出」一致（链接与运行时）](#与-gyan-包内库侧输出一致链接与运行时)
  - [CMake 自动下载预编译包](#cmake-自动下载预编译包)
  - [配置、编译](#配置编译)

在 **不编译整套 FFmpeg 库** 的前提下，仅编译 **ffplay.exe**：使用 **gyan 提供的 FFmpeg 8.1 预编译共享库**（头文件 + 导入库 + 运行时 DLL）与 **SDL2**，在 Windows 上用 CMake/MSVC 生成可执行文件。

> 这个工程主要是为了重构 ffplay.c 打基础，配置一次相当繁琐。

## 依赖与环境

1. **gyan FFmpeg 8.1 shared 完整目录**
2. **SDL2** MSVC 开发包
3. **CMake**、**Visual Studio 2022**（或改用 Ninja + 其他工具链并调整链接方式）  
4. 官方 FFmpeg 仓库

## 自编译 ffplay.exe 与 gyan 预编译包输出对齐

### gyan 声明的 commit 在哪里

gyan 压缩包根目录的 **`README.txt`** 中有一行 **Source Code**，指向官方仓库的某次提交，例如：

```text
Source Code: https://github.com/FFmpeg/FFmpeg/commit/9047fa1b08
```

即 **gyan 宣称** 该预编译所对应的 FFmpeg 源码版本。

在用于对照的 FFmpeg 仓库根目录执行（将 `<id>` 换成当前 `README.txt` 里解析出的哈希）：

```powershell
git fetch origin
git checkout <id>
```

确认：

```bash
git rev-parse HEAD
```

应与 README 中的提交一致（短哈希时为前缀一致）。

在 **`ffmpeg` 已 checkout 到与 gyan 一致的 commit** 之后，

1. 将 **`ffmpeg/fftools`** 与 **`ffmpeg/compat`** 中相关文件 **同步拷贝到 `ffplay-cli/`**。

2. 将 `ffmpeg/libavutil/getenv_utf8.h`、`wchar_filename.h` 拷入 `ffplay-cli/shim/include/libavutil/`，并保留本仓库中的 **include 适配**（`mem.h` → `libavutil/mem.h`；`getenv_utf8.h` 内对 `wchar_filename` 使用同目录 `"wchar_filename.h"`）。

### 与 gyan 原装 `ffplay -version` 文本一致（Configure 阶段）

在 **CMake 配置** 时，`cmake/FFmpegGyan.cmake` 的 **`ffplay_generate_config()`** 会生成 `build/generated/config.h`（由 `config.h.in` 展开）。

| 宏 | 来源 | 作用 |
|----|------|------|
| **`FFMPEG_VERSION`** | 预编译包内 **`include/libavutil/ffversion.h`** | 与 gyan 包内版本字符串一致（如 `8.1-full_build-www.gyan.dev`）。 |
| **`FFMPEG_CONFIGURATION`** | **优先**执行 **`FFMPEG_PREBUILT_ROOT/bin/ffplay.exe -hide_banner -version`**，从输出中用正则截取 **`configuration:`** 后的整行，经 C 转义写入宏。 | 与 **gyan 原装 ffplay** 及 **各 `libav*.dll` 内 `*_configuration()` 返回的 configure 字符串** 一致，避免 `opt_common.c` 里与 DLL 比较时出现 **configuration mismatch** 警告。 |
| **`CC_IDENT`** | 同上条 **`ffplay.exe -version`** 输出中的 **`built with ...`** 行。 | 与 gyan 原装 `-version` 中「编译器标识」一行一致。 |
| **许可证 / 组件类 `CONFIG_*`、`HAVE_*`** | `config.h.in` 中按 **GPLv3 + full 特性** 思路写死（如 `CONFIG_GPLV3`、`CONFIG_LIBPLACEBO=0` 等）。 | 满足 fftools 编译与授权分支；`CONFIG_LIBPLACEBO=0` 用于避免再链 **libplacebo**（与「只依赖预编译包 + SDL2」一致）。 |

### `config_components.h`（仅影响少数 `#if`）

`config_components.h.in` 生成 **`config_components.h`**，为 `ffplay.c` 中 **`CONFIG_RTSP_DEMUXER` / `CONFIG_MMSH_PROTOCOL`** 等与 gyan full 一致的宏，避免与预编译里已启用的协议/解复用行为冲突。其余组件仍由 **DLL 在运行时** 提供。

### 与 gyan 包内「库侧输出」一致（链接与运行时）

| 方法 | 说明 |
|------|------|
| **链接** | 链接 **`FFMPEG_PREBUILT_ROOT/lib`** 下与 gyan 相同的导入库（`avutil`、`avcodec`、`avformat`、`avfilter`、`avdevice`、`swscale`、`swresample`）。 |
| **`-version` 里各库版本行** | 来自 **已加载 DLL** 的 `lib*_version()`，与使用 gyan 原装 `ffplay.exe` 且 **同一套 `bin` 下 DLL** 时一致。 |
| **POST_BUILD** | 将 **`SDL2.dll`** 及 **`FFMPEG_PREBUILT_ROOT/bin/*.dll`**（若存在）复制到 **`ffplay.exe` 同目录**，保证实际加载的仍是 gyan 包内那一套运行时，避免混用其它路径的旧 DLL。 |

## CMake 自动下载预编译包

默认 **`FFPLAY_FETCH_PREBUILT_DEPS=ON`**：若缺少下列目录中的关键文件，CMake **配置阶段**会从网上下载并解压到 **`THIRD_PARTY_DIR`**（与手动解压后的布局一致）：

| 组件 | 默认 URL（可用 CMake 缓存变量覆盖） |
|------|--------------------------------------|
| FFmpeg | `FFPLAY_FFMPEG_PREBUILT_URL` → [ffmpeg-8.1-full_build-shared.zip](https://github.com/GyanD/codexffmpeg/releases/download/8.1/ffmpeg-8.1-full_build-shared.zip) |
| SDL2 | `FFPLAY_SDL2_DEV_ZIP_URL` → [SDL2-devel-2.32.10-VC.zip](https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip) |

- 下载文件缓存在 **`third_party/.cache/`**，已存在则不会重复下载。  
- **FFmpeg 与 SDL2** 均为 **zip**，使用 **`cmake -E tar xf`** 解压，**不依赖 7-Zip**。  
- 若希望完全离线、只用本地已解压目录：配置时加 **`-DFFPLAY_FETCH_PREBUILT_DEPS=OFF`**。  
- 若仍使用旧版目录名 **`SDL2-2.32.4`**：`-DFFPLAY_SDL2_ROOT_DIRNAME=SDL2-2.32.4` 并自行放入对应开发包。

## 配置、编译

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

编译后生成 `build/Release/ffplay.exe`（及复制到同目录的运行时 DLL）。

可选缓存变量（见 `CMakeLists.txt`）：

| 变量 | 含义 |
|------|------|
| `THIRD_PARTY_DIR` | 默认 `third_party` |
| `FFPLAY_FETCH_PREBUILT_DEPS` | 缺依赖时是否自动下载解压（默认 `ON`） |
| `FFPLAY_FFMPEG_PREBUILT_URL` | FFmpeg `.zip` 下载地址 |
| `FFPLAY_SDL2_DEV_ZIP_URL` | SDL2 MSVC `.zip` 下载地址 |
| `FFPLAY_SDL2_ROOT_DIRNAME` | `THIRD_PARTY_DIR` 下 SDL2 文件夹名（默认 `SDL2-2.32.10`） |
| `FFMPEG_PREBUILT_ROOT` | gyan 解压根目录 |
| `FFMPEG_LIB_DIR` | 一般为 `.../lib` |
| `SDL2_DIR` | 若未放在默认路径，指向含 `SDL2Config.cmake` 的目录 |