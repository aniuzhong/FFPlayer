# Progress bar issues

Three bugs in the ffplay-gui progress bar, all sharing the same root cause but requiring fixes at different levels.

---

## 1. Seek flashback

### Symptom

Drag the seek bar to 80% → the slider jumps to 0% first, then back to 80%.

### Root cause

Ffmpeg's clock serialisation mechanism:

```
serial != audclk->serial → get_clock() returns NAN
```

**Timeline:**
1. User seeks → `stream_seek()` sets `seek_req = 1`
2. Read thread calls `handle_seek_request()` → `avformat_seek_file()` → `packet_queue_flush()` (serial incremented) → `seek_req = 0`
3. `get_clock()` sees mismatched serials → returns `NAN`
4. `stream_get_position()` receives `NAN` → returns `0.0`

This is deterministic, not noise. The serial mismatch resolves asynchronously when the next audio frame reaches `set_clock()`.

### Previous (wrong) fix

A UI-layer state machine in `main.cpp` with magic thresholds (`-0.003`, `-0.08`):

| Problem | Detail |
|---------|--------|
| Magic numbers | Tuned for one video length, breaks on others |
| Dead zone | Deltas in `(-0.08, -0.003)` freeze the bar entirely |
| Scale-dependent | 4h video needs 19min regression to trigger; 30s video needs 2.4s |
| Never reaches 100% | Required a separate `if (eof && >0.90)` hack |
| Wrong layer | State machine mixed into the ImGui draw function |

### Fix (`stream.c:stream_get_position()`)

Replace the NAN → 0.0 fallback with the last seek target:

```c
if (isnan(pos))
    return (double)is->seek_pos / AV_TIME_BASE;
```

- `is->seek_pos` is set by `stream_seek()` and zero-initialised by `av_mallocz`.
- `stream_get_position()` is read-only — no effect on A/V sync decisions (separate path).
- UI layer: removed the ~30-line state machine hack entirely.

---

## 2. UI progress-bar freeze

### Symptom

During normal playback, the progress bar occasionally freezes for several minutes.

### Root cause

The same state machine from issue #1. A 4% clock regression in a 2h video (`delta = -0.02`) hits the dead zone `(-0.08, -0.003)` and the bar locks.

### Fix

Deleted the state machine. The UI now trusts the clock entirely.

---

## 3. End-time oscillation

### Symptom

In the final second, the displayed time flickers between `00:51` and `00:52` (for a 52s clip).

### Root cause

`get_clock()` extrapolates between audio callbacks (`clock.c:60-62`):

```c
double time = av_gettime_relative() / 1000000.0;
return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
```

At speed 1.0: `clock = pts + (now - last_updated)`

| Event | Clock value | `(int)` truncation | Display |
|-------|------------|-------------------|---------|
| Audio callback sets PTS=51.999 | 51.999 | 51 | 00:51 |
| +10ms extrapolation | 52.008 | 52 | 00:52 |
| +23ms next callback resets | 51.999 | 51 | 00:51 |

The callback interval (~23ms) makes this oscillation visible.

### Fix

Changed `(int)` truncation to rounding (`+ 0.5`) in `format_duration_for_ui()`:

| Clock value | Before `(int)` | After `int+0.5` |
|-------------|----------------|-----------------|
| 51.999 | 51 ❌ | **52** ✅ |
| 52.008 | 52 | **52** ✅ |

---

## Changes

| File | Change |
|------|--------|
| `ffplay-gui/ffplayer/stream.c` | NAN fallback → `seek_pos` |
| `ffplay-gui/app/main.cpp` | Deleted ~30-line state-machine hack |
| `ffplay-gui/app/main.cpp` | `format_duration_for_ui()` rounding fix |

## Status

- [x] No seek flashback to 0%
- [x] No progress-bar freeze or jitter
- [x] Stable end-time display
- [x] Consistent across video lengths (3s – 30s+ tested)
- [x] Smooth seek at any position
