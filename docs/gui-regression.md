# GUI Regression Test

## Smoke test (every build)

```
timeout /t 3 /nobreak >nul && .\build\Release\ffplay-gui.exe
```

Expect: window appears, ImGui renders, clean exit (no crash).

## Functional checks

| # | Scenario | How | Pass criteria |
|---|----------|-----|---------------|
| 1 | **Software decode** | Open an SDR MP4 | Correct rendering, OSD shows FPS + time |
| 2 | **Hardware decode** | Open H.264/H.265 | Performance > SW, no corruption |
| 3 | **Window resize** | Drag window edge during playback | Aspect ratio preserved, ImGui adapts |
| 4 | **Fullscreen** | Double-click or shortcut | Clean transition, no black screen |
| 5 | **Seek** | Click the progress bar | Frame jumps to expected position |
| 6 | **Open/close cycles** | Open → close → re-open (×5) | No crash, textures clean up cleanly |
| 7 | **HDR → SDR** | Open PQ/HLG video | Tone-mapped, no blown-out or crushed blacks |
| 8 | **Subtitles** | Play video with embedded subs | Overlay renders correctly, no residue |
| 9 | **A/V sync** | Play any audio+video file | Lip-sync, no stutter |
| 10 | **Broken input** | Open a corrupt or empty file | Graceful error, no crash |

## Watch points (manual)

- **Console log** — `d3d11_device_create` and other init messages
- **FPS stability** — OSD `vdec=` rate shouldn't drop to zero
- **Memory** — working set shouldn't grow across open/close cycles
