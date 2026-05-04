# ffplay-gui

- [ffplay-gui](#ffplay-gui)
  - [FFPlayer API](#ffplayer-api)
  - [Application](#application)
  - [TODO](#todo)

## FFPlayer API

目前将 ffplay 抽出了以下 API，主要是对 `VideoState` 和 `stream_` 函数的提取。

## Application

- 将原版的 SDL2 后端剔除，换成了 D3D11 后端。
- 支持硬解相关纹理转换。
- 应用层直接使用 ImGUI。

![Image of ffplay-gui.exe](../images/v0.1.5.jpg)

## TODO

- 倍速
- 音频
    - 指定设备名
    - 音轨
    - 音量曲线
- 多级 Seek 分流的策略
- 字幕显示修复（当前只支持 PGS/DVDSub 等位图字幕”，文本字幕不支持）
    - 引入 libass（或 DirectWrite + ASS 解析）渲染文本字幕（SRT/ASS）。
- 异步准备（PrepareAsync）
  