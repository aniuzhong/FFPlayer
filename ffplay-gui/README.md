# ffplay-gui

## Architecture

- Stripped out the original SDL2 render backend, replaced with D3D11.
- D3D11VA hardware decode texture handling.
- ImGui for the UI layer.

## TODO

- Playback speed
- Audio
    - Device selection
    - Track switching
    - Volume curve
- Multi-level seek strategy
- Subtitle rendering (bitmap-only for now — PGS/DVDSub; text subs SRT/ASS need libass or DirectWrite)
- Async prepare
