# obs-optical-flow

**Real-time optical-flow frame interpolation (motion smoothing / "MEMC") as a video filter for OBS Studio on macOS.**

Turn a 30 fps source inside a 60 fps canvas into smooth 60 fps — like the motion-smoothing feature of a modern TV, but live inside OBS. As of July 2026 this appears to be the first OBS plugin of its kind: StreamFX rejected a corresponding feature request in 2021 ([#687](https://github.com/Xaymar/obs-StreamFX/issues/687)), and the OBS forums only contain unanswered requests for it.

> 🤖 This plugin was researched, designed, implemented and verified end-to-end by **[Claude Code](https://claude.com/claude-code)** powered by **Claude Fable 5** (Anthropic) — including the shader math, the test harness that runs the pipeline against libobs without launching OBS, and adversarial code reviews.

---

## Features

- **Motion-compensated frame rate upconversion** (e.g. 30 → 60 fps, 24 → 60 fps with proper 3:2 handling, variable game frame rates)
- Pyramidal block-matching optical flow computed **entirely in fragment shaders** — no compute shaders, no ML runtime, works on OBS' OpenGL backend
- **Bidirectional flow** (forward + backward) with Sundaram–Brox forward/backward-consistency occlusion detection and a graceful cross-fade fallback where the flow is unreliable
- Zero-vector bias (static UI/text stays locked), temporal seeding from the previous frame pair (3-D recursive search idea), per-level 3×3 median filtering
- **Scene-cut detection** — hard cuts are never interpolated
- Automatic **bypass** when the source already runs at canvas rate
- HDR-aware (SDR / SDR-16F / Rec.709 extended color spaces, same handling as OBS' built-in filters)
- Debug views: **motion-vector visualisation** (HSV color wheel) and **split screen** (original | interpolated)
- Lightweight: **~1.6 ms** per new source frame + **~0.1 ms** per canvas tick at 1080p on an Apple M1 Max (≈ 5 % GPU at 30→60 fps)

## Requirements

- **OBS Studio 32.x** installed at `/Applications/OBS.app` (developed and tested against 32.1.2)
- **macOS on Apple Silicon** (tested on an M1 Max; the C code and the effect file are platform-neutral libobs API — only the build script is macOS-specific, see [Porting](#porting))
- Xcode command-line tools (`clang`) to build

## Installation

No OBS source build required — the plugin links directly against the libobs framework inside your installed OBS.app. The required libobs headers (obs-studio 32.1.2) and simde are vendored in `deps/`.

```bash
git clone https://github.com/qshiqshi/obs-optical-flow.git
cd obs-optical-flow
./build.sh --install
```

This builds `build/obs-optical-flow.plugin`, ad-hoc codesigns it and copies it to
`~/Library/Application Support/obs-studio/plugins/`. Restart OBS afterwards.

## Usage guide

### 1. Set your canvas frame rate higher than the source

The whole point of the filter is filling in the duplicate frames OBS renders when a source is slower than the canvas. **If your canvas runs at 30 fps and your source is 30 fps, there is nothing to interpolate.**

- OBS → Settings → Video → **Common FPS Values → 60**

### 2. Add the filter

Right-click a source → **Filters** → **+** → **Optical Flow Frame Interpolation**.

Good first test: a Media Source playing a 24 or 30 fps video file. Set the debug view to *Split screen* and pan-heavy footage will show the difference immediately — the left half judders, the right half is smooth.

### 3. Settings

| Setting | Meaning |
|---|---|
| **Motion estimation resolution** | Flow field at ½, ¼ (default) or ⅛ of the source resolution. ½ = finer motion boundaries, ⅛ = cheapest. |
| **Interpolation strength** | 0 = plain cross-fade … 1 = full motion compensation. |
| **Artifact protection** | How aggressively occluded/inconsistent regions fall back to cross-fade. Raise it if you see halos around moving objects. |
| **New-frame detection threshold (%)** | Mean luma difference above which a rendered frame counts as a *new* source frame. Raise for noisy sources, lower for subtle content. |
| **Low latency mode** | Detects new frames in the same tick (costs one GPU sync per frame) and shortens the presentation delay by one tick. |
| **Debug view** | Off / motion vectors / split screen. |

### 4. Latency

Frame interpolation inherently needs the *next* frame before it can interpolate, so the filter delays the source by **one source-frame interval plus one canvas tick** (≈ 50 ms for a 30 fps source in a 60 fps canvas; less in low-latency mode). Irrelevant for media playback, replays or IRL cams; think twice for latency-critical self-monitoring.

## How it works

An OBS video filter renders at **canvas rate**, even when its source updates slower — OBS simply renders duplicates. This plugin exploits exactly that:

1. **Capture** — every tick the target source is rendered into a texrender ring (the pattern of OBS' own `gpu-delay` filter, including color-space handling).
2. **New-frame detection** — a 48×27 GPU difference map against the last known frame is staged and read back **one tick later** (no pipeline stall). A global-mean rule catches full-frame changes, a max-cell rule catches small moving objects. Detected frames are promoted into the frame pair (A, B) by pointer swap; the gaps feed an EMA of the source interval.
3. **Optical flow** (once per new frame pair, ~13 small draw calls):
   - Luma pyramid: both frames packed into one RG16F texture (A→R, B→G), base at ¼ source resolution by default, up to 6 levels
   - Coarse-to-fine block matching with 4×4-tap SAD blocks: candidates propagated from the coarser level (center + 4 neighbours), a zero-motion candidate with a small bonus, a temporal candidate from the previous pair; 3×3 local search (5×5 exhaustive at the coarsest level) and half-texel subpixel refinement at the finest level
   - Forward (A→B) **and** backward (B→A) flow estimated in the same pass, stored as RGBA16F in resolution-independent UV units
   - Component-wise 3×3 median (exchange network) after every level
4. **Interpolation** (every tick): presentation time `now − (source interval + 1 tick)` yields the phase *t* between A and B. The output is backward-warped from both frames — `A(x − t·v_f)` and `B(x − (1−t)·v_b)` — weighted by temporal distance and forward/backward-consistency confidence, blended over a plain cross-fade fallback. Pairs spanning a scene cut or a >250 ms gap are never interpolated.

The parameter choices (flow at ⅛–¼ resolution, 4×4 SAD taps, small per-level search radii, per-level median, zero-vector bias, temporal seeding) follow what AMD FidelityFX Optical Flow (FSR3), SVP/MVTools and the classic FRUC/3-D-recursive-search literature converged on.

## Performance (measured, M1 Max, 1080p)

| Stage | Cost | Runs |
|---|---|---|
| Flow computation (13 passes) | **1.6 ms** | once per new source frame (e.g. 30×/s) |
| Warp/interpolation pass | **0.1 ms** | once per canvas tick (e.g. 60×/s) |

Total at 30→60 fps: ~55 ms GPU time per second ≈ **5–6 % GPU load**, measured with `test/perf-test.c` on random noise (worst case for block matching).

## Testing without launching OBS

`test/` contains standalone harnesses that create a real libobs graphics context from the installed OBS.app — the actual effect parser, GLSL transpiler and GPU run the code, no OBS UI involved:

```bash
CFLAGS="-std=c17 -O1 -arch arm64 -Ideps/libobs-headers \
  -F /Applications/OBS.app/Contents/Frameworks -framework libobs \
  -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks"

# 1. Compile all 9 shader techniques through OBS' effect parser:
clang $CFLAGS -o build/effect-test test/effect-test.c
./build/effect-test data/effects/optical-flow.effect

# 2. End-to-end algorithm check (synthetic 16 px motion; verifies the flow
#    vectors AND the interpolated object position at t=0.5):
clang $CFLAGS -o build/flow-test test/flow-test.c
./build/flow-test data/effects/optical-flow.effect

# 3. Performance measurement at 1080p:
clang $CFLAGS -o build/perf-test test/perf-test.c
./build/perf-test
```

Gotcha for your own harnesses: libobs-opengl initialises with back-face culling enabled. Inside OBS' render loop OBS sets `GS_NEITHER` for you; standalone you must call `gs_set_cull_mode(GS_NEITHER)` yourself or every sprite draw is silently culled.

## Known limitations (v1)

- Classic MEMC artifacts around fast occlusions (halos) — the artifact-protection slider fades those regions to cross-fade
- Repetitive patterns (fences, grids) can lock onto the wrong period
- Sources with heavy sensor noise that deliver a frame every tick are (correctly) bypassed — raise the detection threshold if you want interpolation on such sources
- Variable-frame-rate sources (games) are smoothed via the interval EMA; micro-judder is possible
- The soap-opera effect is not a bug, it is the product 😉

## Porting

The filter (`src/optical-flow-filter.c`) and the effect file use only portable libobs APIs — nothing macOS-specific. Windows/Linux would need a build setup against the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) (or a plain CMake link against libobs); the shader dialect is transpiled to HLSL/GLSL by OBS itself. Contributions welcome.

## License & credits

**GPL-2.0-or-later** (same as OBS Studio). Vendored third-party code: libobs headers (GPL-2.0-or-later, obs-studio 32.1.2) and [simde](https://github.com/simd-everywhere/simde) (MIT).

Architecture references: OBS' `gpu-delay.c` (frame caching pattern), AMD FidelityFX Optical Flow (parameter choices), SVPflow/MVTools (zero bias, artifact masks), de Haan's 3-D Recursive Search (temporal seeding), Sundaram–Brox consistency test (occlusion detection).

Built with [Claude Code](https://claude.com/claude-code) (Claude Fable 5).
