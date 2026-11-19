# vrupscale: notes for Claude

vrupscale upscales VR eye images inside SteamVR's compositor. It has two parts:
- **`dxgi.dll`:** a proxy DLL that sits next to `vrcompositor.exe` in `SteamVR\bin\win64`. It hooks D3D11 and replaces each eye texture the compositor samples with an upscaled "proxy". There are three modes: Temporal (TAAU with motion estimated from colour alone; the game supplies no motion vectors), FSR 1 and bilinear.
- **`vrupscale.exe`:** the control app. It installs the DLL, changes settings live over shared memory, sets VRChat's SteamVR resolution and updates itself from GitHub releases.

The README covers user-facing behaviour, every setting and the build. This file holds what the README doesn't: the working rules, the build on Windows, the code map, and the work in progress.

Owner: Zak (GitHub `Zakhazerd`, repo `Zakhazerd/vrupscale`). His setup:
- RTX 3090, Bigscreen Beyond 2e with eye tracking, VRChat (around 22 fps in heavy worlds).
- Native 1× on 4572×4572 per-eye textures.
- Temporal area 0.6 (0.5 while following the gaze), Moving objects (`localTrust`) 0.8, `residualTol` 0.9, Bilinear edges on.
- Sharpness 1.0, Sharpness area 0.8 while following the gaze.

The `u1` variant in `tools/ab/run.sh` uses these settings.

## Rules

- **Branches.** Work on `dev`; commit and push to `dev` freely. Merging or pushing to `main` needs Zak's explicit OK every time. Pushing `main` runs `.github/workflows/release.yml`, which publishes a GitHub release whenever `VU_VERSION_STR` (in `src/common.h`) has no release yet, and every installed app then offers that release as an update.
- **Commit messages.** End them with `Co-Authored-By: Claude <model> <noreply@anthropic.com>` only. No `Claude-Session:` trailer and no session links.
- **CHANGELOG.md.** Write only what users notice, concisely. No "X is unchanged" notes, and no UI placement details unless something moved. Add the entry when a change is going into a release, not for every experiment on `dev`.
- **Don't bring back:**
  - Frame generation. It was tried and rejected.
  - Patching SteamVR's own shaders or compositor internals beyond the D3D11 hooks we have. That was ruled out.
  - Anything loaded into VRChat's process. EAC protects it; vrupscale only ever touches `vrcompositor.exe`.
- **Licences.** Ported code keeps its licence: `third_party/ffx/LICENSE*.txt`, shipped in `LICENSES/` by `tools/package.sh`. Most of AMD's FidelityFX SDK 2.x is under a binary-only licence. Its `Kits/FidelityFX/docs/license.md` lists the files that are MIT, so check that list before porting anything else from it.
- **Working with Zak.** No flattery. Don't ask him for what you can work out or look up. Report when something is done rather than sending progress updates. Give a plain account of what was measured and what wasn't.

## Building

The compiled shaders are committed (`src/gen/shaders.bin`), so a plain build needs only MinGW-w64. After editing `shaders/*.hlsl`, regenerate `shaders.bin` and commit it with the change.

### Windows (MSYS2)

CI builds with Ubuntu's MinGW-w64 (GCC 13, win32 threads, msvcrt). On Windows:

1. Install MSYS2 (`winget install --id MSYS2.MSYS2`, into `C:\msys64`).
2. In its **MINGW64** shell (not UCRT64, which links the UCRT instead of msvcrt): `pacman -Syuu` twice (the first run updates the core and closes the shell), then `pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-python git`
3. Build:
   ```
   CC=gcc CXX=g++ WINDRES=windres sh build.sh          # build/dxgi.dll, build/vrupscale.exe
   CXX=g++ sh tools/ab/build-test.sh                   # build/selftest.exe (the test harness)
   CC=gcc CXX=g++ WINDRES=windres sh tools/package.sh  # the release zip, in build/
   D3DCOMPILER=C:/Windows/System32/d3dcompiler_47.dll CXX=g++ sh tools/shaders.sh   # after shader edits
   ```
4. Check the imports: `objdump -p build/dxgi.dll | grep "DLL Name"` must list only `KERNEL32.dll` and `msvcrt.dll`. If MSYS2's GCC pulls in more, `libwinpthread` for example, fix the link flags before shipping.
5. Check the compiler: `tools/shaders.sh` should reproduce `src/gen/shaders.bin` byte for byte (`git status` stays clean) when no shader changed.

**Verified on Zak's PC (2026-11-18, `dev` 18edcca, MSYS2 GCC 16.2.0, no script changes needed):**
- All four builds succeed without warnings. `dxgi.dll` imports only `KERNEL32.dll` and `msvcrt.dll`.
- `tools/shaders.sh` with System32's `d3dcompiler_47.dll` reproduced `shaders.bin` byte for byte (FXC still prints its X4000 warnings for `upscale.hlsl` line 208).
- `selftest.sh` both ways on the RTX 3090: 0 failed, with the eye-tracking checks skipped (see Testing).
- The standard A/B check below (`build-at.sh`, `run.sh`, `cmp.py`) ran as written: all IDENTICAL. The worktree build of the same commit gave a byte-identical `dxgi.dll`.
- Not verified: whether this GCC 16 build behaves like CI's GCC 13 build (no A/B against a CI-built DLL), and nothing has run in SteamVR.

**Gotchas:**
- MSYS2's git lacks the `core.autocrlf=true` that Git for Windows sets system-wide. In a checkout made by Git for Windows it lists about 117 files as modified (line endings only). Use Git for Windows' git, or `git -c core.autocrlf=true status`.
- From PowerShell (Claude sessions): `$env:MSYSTEM='MINGW64'; $env:CHERE_INVOKING='1'; C:\msys64\usr\bin\bash.exe -lc '...'` runs in MINGW64 in the current folder. Windows PowerShell 5.1 breaks double quotes inside that argument, so use none or put the commands in a script file.

WSL2 with Ubuntu is the fallback. It builds exactly like CI (the Linux steps below), and the outputs run on Windows.

### Linux (CI, cloud sessions)

```
sudo apt-get install mingw-w64 && sh build.sh
sh tools/ab/setup-wine.sh     # once: Wine prefix with DXVK, tools/d3dcompiler_47.dll (for shaders.sh and the harness)
sh tools/shaders.sh           # after shader edits (runs Microsoft's compiler under Wine)
```

- **Shader toolchain check:** with Electron 33.2.1's `d3dcompiler_47.dll`, which `setup-wine.sh` fetches, `tools/shaders.sh` reproduced the committed `shaders.bin` bit for bit (2026-11-19).
- **Wine 9.0 crash:** Wine aborts with `free(): invalid pointer` when `/run/user/<uid>` is missing, as it is in containers. `setup-wine.sh` and `tools/ab` create that folder.
- **Cloud machine:** 2 cores and 8 GB. Run at most two harness runs at once at the default scene size. Two 1200×1320 runs at once ran out of memory, so run large scenes (1000×1100 and up) one at a time.

## Testing

`tools/ab/` holds the harness scripts. They run natively on Windows (MSYS2 or Git Bash, on the real GPU) and under Wine on Linux (DXVK on lavapipe, a software GPU). Each script prints its options when run without arguments.

| Script | What it does |
|---|---|
| `selftest.sh <dll> <dir> [deferred]` | The full self-test, the same as the release's `selftest\selftest.cmd`: 193 checks drawing directly, 195 with `deferred` (command lists, like SteamVR). Expect `0 failed`. Its performance section gives real GPU times only on Windows. |
| `build-at.sh <commit> <out.dll>` | Builds `dxgi.dll` at any commit in a temporary worktree: the "before" side of an A/B. |
| `run.sh <dll> <dir>` | The A/B variants (v1–v14 scaled, u1–u3 at 1×, w1/w3/w8/x1 with `opticalFlow=fsr3`). Writes every frame of both eyes as raw RGBA. Options: `VARIANTS="v1 v3 v8 u1"`, `NOTIME=1`, `STATS=1` (per-pass GPU ms), `TOGA=n TOGB=n` (Ctrl+F10 before frame n), `W H FACTOR FRAMES MOTION`. |
| `cmp.py <dirA>[:tag] <dirB>[:tag] [-v]` | Compares two runs per variant: IDENTICAL, or the differing values and PSNR. Exits 0 only if everything is identical. On Windows run it as `python tools/ab/cmp.py`. |
| `quality.sh <dll> <dir> ["extra settings"]` | The quality table: FSR 1 vs temporal against ground truth per region (background, grating, trail, object, whole, steadiness), plus flow error against the true motion. `MOTION=40` is fast and `100` very fast and jerky. `W=1000 H=1100 FACTOR=1.0 FRAMES=16` is closer to a headset. `FULL=1` adds all the quality checks. |

**Standard check for a change meant to leave the picture alone** (refactors, speed-ups):

```
sh tools/ab/build-at.sh dev /tmp/before.dll && sh build.sh && sh tools/ab/build-test.sh
NOTIME=1 VARIANTS="v1 v3 v8 u1" sh tools/ab/run.sh /tmp/before.dll /tmp/ab_before
NOTIME=1 VARIANTS="v1 v3 v8 u1" sh tools/ab/run.sh build/dxgi.dll /tmp/ab_after
python3 tools/ab/cmp.py /tmp/ab_before /tmp/ab_after     # every variant: IDENTICAL
```

Add variants that cover the code touched (v7/v10–v14 for FSR 1 mode, v9 for no command lists, v5/v6/v12/v14 for FXAA). Then run `selftest.sh` both ways.

**On Zak's PC (real GPU):**
- **Eye tracker:** Bigscreen's real `VRCFTMemmapData` gaze mapping exists whenever its eye tracking client runs. On Zak's PC that is always, since `BeyondHID.exe` (the Beyond driver) runs in the background. The gaze test then skips its 15 checks (178 and 180 instead of 193 and 195, with a note on the RESULT line), because its stand-in would write fake samples into the real data. Those checks only run with no Bigscreen client running, as under Wine. Before this fix, "no eye tracker" failed there and the test wrote into the real mapping.
- **GPU times:** `run.sh STATS=1` gives no usable times. The same pass varied up to 17× between identical runs, probably because its sparse frames let the GPU clock down. Use the sustained-load performance section instead: `VRUPSCALE_TEST_PERF_ONLY=1 sh tools/ab/selftest.sh build/dxgi.dll <dir>`, adding `VRUPSCALE_TEST_EXTRA=opticalFlow=fsr3` (or any other `[temporal]` lines) to time other settings. Its small passes repeat within about 0.005 ms. Accumulate and output still vary 2–4× between runs at 2756², so its "cheaper than" timing checks can fail from noise (one did, in one of six runs).

The harness runs `test/fake_compositor.cpp`, which includes `test/temporal_tests.inc`. It acts like the compositor and draws a procedural scene with ground truth (sub-pixel head motion, a moving object, a grating above Nyquist). The `VRUPSCALE_TEST_*` environment variables are listed in the README's "Building from source" section; `VRUPSCALE_TEST_ONLY=abdump` is the `run.sh` mode.

**Logs.** The DLL logs to `%LOCALAPPDATA%\vrupscale\vrupscale.log`, or to `VRUPSCALE_LOG`. Zak's logs from SteamVR are the main real-world evidence. The periodic stats line has per-pass GPU ms (luma+decide, pyramid, search, lucas-kanade, global fit, refine, accumulate, output, copy). Settings live in `%LOCALAPPDATA%\vrupscale\vrupscale.ini`, which the app manages.

## Code map

`src/` (the DLL):
- `dllmain.cpp`: attach. The process is the target if it's `vrcompositor.exe` or `VRUPSCALE_FORCE` is set. Also the log and settings paths.
- `exports.S`, `exports.def`, `proxy.cpp`: forward the dxgi exports to the real `System32\dxgi.dll`.
- `hooks.cpp`, `vtables.c/.h`: MinHook detours on D3D11 device and context methods (every method-body variant), found by name from live objects.
- `upscaler.cpp`:
  - Classifies the views the compositor binds, makes the proxies, and runs jobs before draws that sample them.
  - The spatial path (FSR 1, bilinear) and command-list handling.
  - Hotkeys: `ApplyHotkey`. Ctrl+F1–F10 are on/off, mode, debug tint, sharpness, preset, sharp history, bilinear edges, calibrate, FXAA, and the optical-flow experiment.
  - Config apply and reload (`ApplyConfig`, `ApplyFileConfig`), stats, and the debug exports the harness uses (`VrUpscaleDebug*`).
- `temporal.cpp`:
  - Per-eye streams (history, pyramid, flow) and the eye pools: one history per eye across its swapchain textures and size changes.
  - Content rectangles (the game rendering into part of larger textures) and region planning (`PlanRegion`, `PlaceRegion`).
  - `IssueStream`, the pass sequence below.
- `internal.h`: types shared by the two paths. `common.h`: `Config` and version. `config.cpp`: ini parsing.
- `control.cpp`, `control_shared.h`: the app channel. Plain C layout; bump `kVuLayout` on any change.
- `gaze.cpp`: Beyond 2e gaze from Bigscreen's `VRCFTMemmapData` shared memory. `log.cpp`: the log.
- `permutations.h`: every compiled shader variant. `shaders.cpp` and `shader_pack.S` embed `gen/shaders.bin`.

`shaders/`:
- `temporal.hlsl`: all temporal passes, selected by `TPASS`. Its header comment describes each pass.
- `upscale.hlsl`: FSR 1 EASU / EASU+RCAS / bilinear / OUTER / FXAA (`PASS` 0–4).
- All shaders are cs_5_0 compiled with FXC. That means no wave ops, no typed UAV loads beyond single 32-bit formats, and at most 8 UAVs and 8 SRVs per pass (`kCsUavSlots` / `kCsSrvSlots`).

`app/`:
- `main.cpp`: window, install/uninstall, settings, command-line modes.
- `ui.cpp`: hand-drawn UI in the style of Valve's hardware pages, using GDI+ and GDI.
- `update.cpp`: GitHub release updater. It reads a stored, uncompressed zip with no decompressor and checks the SHA-256.
- `gameres.cpp`: sets VRChat's SteamVR resolution. `vrlink.cpp`: the OpenVR connection and the startup entry. `changelog.cpp`: shows CHANGELOG.md.

`test/`:
- `fake_compositor.cpp` and `temporal_tests.inc`: the harness.
- `stub/openvr_stub.c`: stands in for `openvr_api.dll`. `stub/fakegaze.c`: stands in for Bigscreen's eye tracking client.

`tools/`:
- `shaders.sh` and `compile_shaders.cpp`: build `shaders.bin`. `package.sh`: the release zip. `release_notes.py`: the release notes. `fonts.py`, `icon.py`: app assets.
- `ab/`: the harness scripts above.

### Temporal pass sequence (one eye stream, `IssueStream` in `src/temporal.cpp`)

| Pass | Grid | Does |
|---|---|---|
| FP + DECIDE | 1/64 | Sparse fingerprint of the texture. Its last group decides whether this is a new frame and writes the `DispatchIndirect` args for every pass after it: zero for a repeated frame, so nothing runs. |
| (FXPRE) | input | FXAA before the upscale, when on. |
| LUMA | input → L1 | Perceptual luma, straight to L1 (half resolution). |
| DOWN | L2–L4 | The pyramid plus (I, dI/dx, dI/dy) packs. L5–L7 too, only with `opticalFlow=fsr3`. |
| SEARCH | L4 (1/16) | Integer block search, ±3 L4 px, seeded by last frame's flow. Writes `Flow4` (R16G16F, L4 px). With `fsr3` the OF passes replace it (below). |
| LK | L3 grid | Pyramidal Lucas-Kanade L3 → L2 → L1, starting at `Flow4`·2. Writes `FlowLoc`. |
| GLOBAL | 1 group/eye | Robust affine fit (IRLS) of `FlowLoc`: the head motion. OUTER runs beside it. |
| REFINE | L2 grid | Per cell: the global model unless a local candidate fits clearly better, plus a confidence. Writes `flow2`. COPY runs beside the accumulation. |
| ACCUM ×2 | region | Reconstruction plus reprojected history (Catmull-Rom, or Lanczos-3 with Sharp history), YCoCg clip, residual rejection. The history is ping-ponged between two textures, and the GPU tracks which is current. |
| OUT ×2 | region | RCAS (or copy) into the proxy, fading into OUTER across a band at the region's edge. |
| OUTER | output | Outside the region: Catmull-Rom or bilinear of this frame alone, with RCAS inside the Sharpness area. |
| (MARGIN, FXPOST) | | Outside the content rectangle (repaints only); FXAA after the upscale. |
| COPY | L1–L4 | This pyramid and `flow2` become next frame's "prev". L5–L7 too with `fsr3`. |

- **Region:** the Temporal area, a rectangle placed per frame (it follows the gaze if set). Motion passes cover `Work`: the region in L4 px plus `workMargin`.
- **Timing:** `Mark()` timestamps split the run into the 9 named pass groups in the stats.
- **Command-list replay:** each texture's passes are recorded once and replayed. Anything that changes which dispatches run needs a `SHAPE_*` bit, so the list gets re-recorded.

Adding a temporal pass touches:
- `temporal.hlsl`: the `TP_*` define and section.
- `permutations.h`: an `X(...)` line.
- The `A_*` args enum in both `temporal.cpp` and DECIDE in `temporal.hlsl`. A `static_assert` checks that they match, and DECIDE's `SetArgs` must be filled in too.
- `BindCS` + `Indirect` in `IssueStream`.
- A `SHAPE_*` bit if the pass is conditional.
- Regenerated `shaders.bin`, then an A/B.

## Open: the FSR 3 optical flow experiment (`dev` 696357c)

`[temporal] opticalFlow=fsr3` (Ctrl+F10 in VR switches it, and each switch restarts the history) replaces SEARCH with a port of FSR 3's optical flow. The source is the AMD FidelityFX SDK's `framegeneration/fsr3` `ffx_opticalflow_*` files: search, filter, scale. The port:
- **Levels:** our L1–L7 (L1 = FSR 3's level 0). L5–L7 are built only for it, and each level is limited to the work area.
- **Per level, from the top:**
  - OFSEARCH: 8×8 blocks, ±8 px SAD search around the upscaled vector from the level above, with FSR 3's top-left-bias fix and its zero-motion fallback at L1.
  - OFFILTER: each vector replaced by the one in its 3×3 neighbourhood closest to all the others.
  - OFSCALE: of the 4 nearest coarse vectors, doubled, the one with the lowest SAD.
- **Output:** L1's vectors ÷ 8 go to `Flow4`, where LK starts. Everything after is unchanged.
- **cs_5_0 changes:** groupshared `InterlockedMin`/`InterlockedAdd` instead of wave ops; two `R16G16_SINT` textures per level (T1: prediction, then filtered; T2: raw) because SM5 can't do typed UAV loads of that format; the plain SAD instead of `msad4`.
- FSR 3's scene-change detection is left out.
- With it off (the default), the output is bit-exact with 85d758a (`run.sh` v1 v3 v8 u1, untimed).

Harness results (`quality.sh`):

| Scene | Flow error, background (ours → FSR 3) | Picture with FSR 3 |
|---|---|---|
| 400×440, motion 1 | identical | identical |
| 1000×1100 at 1×, motion 40 (about 15 px/frame) | mean 1.09 → 0.27 px, median 0.27 → 0.11 | steadiness +1.65 dB, grating +0.5, trail +0.5, object −0.7, whole +0.15 |
| 1000×1100 at 1×, motion 100 (about 37 px drift, ±45 random) | mean 33 → 4.1 px, median 17.5 → 0.4 | background −3.8 dB, trail −3.7, whole −2.9, steadiness −1.7, object +2.3 |
| 400×440, motion 40 | median 1.24 → 0.23 but mean 4.1 → 24.7 (outliers) | whole +0.34, steadiness +0.74, trail −0.8 |

At motion 100, ours fails outright, so the history is dropped and the output is close to the current frame alone. FSR 3's flow is mostly right but a few px off in places, so slightly misaligned history is kept. The test motion is also much jerkier than a real head. The small scene's top pyramid levels are only a few pixels wide, so its outliers say little about a 4572² eye.

**Waiting on Zak's headset A/B.** He has the test build (`vrupscale-0.6.7-fsr3flow.zip`, which is `dev` 696357c), and is checking:
- Fast head turns and moving people: frizz, smear, ghosting.
- The GPU cost: the log's "search" pass time with the experiment on and off.

GPU cost in the harness (RTX 3090, self-test performance section, area 0.6, GPU ms per eye, two runs each, 2026-11-18):

| Input per eye | search, default → fsr3 | pyramid, default → fsr3 |
|---|---|---|
| 1561×1735 | 0.010 → 0.157–0.160 | 0.018 → 0.025 |
| 2756×2756 | 0.012 → 0.32–0.45 | 0.026 → 0.034–0.063 |

LK, global fit and refine don't change. The search cost grows roughly with the pixel count, which suggests about 1 ms per eye at 4572² (not measured).

What happens next depends on his answer:
- **If kept:** decide whether it becomes the default or an app option, add a CHANGELOG entry, and consider `msad4` (FSR 3's default path; on NVIDIA it's emulated anyway).
- **If rejected:** revert 696357c, which takes its README lines and licence file with it.

Not tried yet: a temporal predictor for the top level, and robustness to extreme motion.

## History worth knowing

- **Releases.** v0.6.7 is the latest. `main` = 85d758a ("Less GPU time in Temporal and FSR 1 mode, same picture"). Zak once reported a quality drop after 85d758a; it was bit-exact in every harness path, and he then saw no difference in VR.
- **`fsr2-experiments` (pushed, not merged).** FSR 2's thin-feature locks and luma instability as `[temporal] locks` / `lumaStability`. By themselves they changed quality by 0.05 dB or less. With both on and clipGamma 0.75, the moving object gained 0.4–0.6 dB, and the accumulate pass took about 50% longer. FSR 2's own accumulation (`fsrHistory`) measured worse than ours and was dropped.
- **VRSS 2 (NVIDIA foveated shading) can't be driven from the compositor.** NVAPI's gaze handler is per process. Zak's probe on driver 616.56 showed in-process yes, cross-process no. The probe tool was never pushed.
- **Neural optical flow was considered and not pursued:** too slow at these resolutions, and not sub-pixel precise.
