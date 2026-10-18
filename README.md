# vrupscale

A temporal upscaler inside the SteamVR compositor, for apps you can't mod, including VRChat.

VRChat's Easy Anti-Cheat rules out anything that loads into `VRChat.exe` (vrperfkit, openvr_fsr, ReShade, OpenXR API layers). vrupscale runs in `vrcompositor.exe` instead: the game renders at a lower resolution, and the compositor gets an upscaled version of each eye texture just before it applies lens distortion.

The default mode is temporal: it builds up detail across frames, using motion it estimates itself. To keep the cost down, only an area in the middle of each eye's picture (where the lens is sharpest and you look most) gets this treatment, and the rest gets a fast spatial upscale. FSR 1 mode works the same way, with FSR 1 in that area. With Bigscreen Beyond 2e eye tracking, the area can follow your gaze.

## Quick start

1. Download `vrupscale-<version>.zip` from the [releases page](https://github.com/Zakhazerd/vrupscale/releases/latest), extract it somewhere permanent that you can write to (for example `Documents\vrupscale`, not Program Files) and run **`vrupscale.exe`**. It isn't code-signed, so Windows SmartScreen may warn the first time: **More info → Run anyway**. After that, updates come through the app.
2. Close SteamVR completely and click **Install** in the app. It finds SteamVR on its own; if it can't, use **Choose folder…**. If Windows asks for administrator rights, your SteamVR folder needs them.
3. Start SteamVR with the app open. The headline changes to **Connected to SteamVR.** The app also adds itself to SteamVR's startup apps: from now on SteamVR starts it (minimized), and it closes when SteamVR does.
4. Set the resolution you want to see in SteamVR as usual and pick a scaling preset in the app (Ultra Quality by default). While VRChat runs, the app lowers SteamVR's resolution so that VRChat renders the preset's share of it, and vrupscale upscales it back.
5. Start VRChat. The line under the headline shows what is being upscaled, for example `Upscaling 2756×2756 → 3555×3555, temporal`, and the GPU time it takes, for example `GPU 1.10 ms per displayed frame (9.9%)`.
6. To check what is being upscaled, switch on **Debug tint** for a moment: the area turns magenta and the rest blue. Only the world should be tinted, not SteamVR's menus or overlays.

vrupscale itself runs inside SteamVR, so it upscales even with the app closed, but then VRChat renders at the full resolution you set: the app has to run while you play, which is why it starts with SteamVR. To start it yourself instead, switch off **Start with SteamVR** in the SteamVR panel.

## The app

vrupscale has two parts: the upscaler, which **Install** puts into SteamVR (as `dxgi.dll` in `SteamVR\bin\win64`) and which runs inside SteamVR's compositor, and this app, which installs it, shows what it is doing and changes its settings.

The headline says what vrupscale is doing. The line below it shows what is being upscaled (the game's render resolution and the output), the mode, how many new frames arrive per second, and the GPU time per frame for both eyes. The little illustrated characters on the right show the state too: asleep while SteamVR isn't running, sparkling while upscaling, with an orange **!** when something needs you. **Temporal paused** means vrupscale has had to use FSR 1 for one eye for more than a second (see If something goes wrong).

**Updates:** when a new version is out, an **Update to …** button appears at the top; point at it to see what's new. One click downloads it, checks it against GitHub's checksum, replaces the app's files and restarts the app. Your settings stay. The upscaler inside SteamVR is updated straight away if SteamVR is closed; otherwise the app updates it as soon as SteamVR closes (or the next time you open the app). The app checks GitHub when it starts and every 6 hours, sending nothing but the request.

**Change log**, next to the version number, shows what changed in each version (the same as [CHANGELOG.md](CHANGELOG.md)). After an update, the versions you didn't have yet are marked **new**; while an update is available, its notes come first.

Under the headline, the window has six panels, each described below: Upscaling, Temporal, Eye tracking, SteamVR, Textures and Recent events. Hold the pointer still over any control for a moment and a box explains it: what it does, what changing it costs, its default, and its shortcut in VR. The app also works from the keyboard: Tab moves between controls, the arrow keys move sliders and pick options, and Space flips switches and presses buttons.

**Upscaling** – changes apply at once, in the headset:
- The switch next to the title turns upscaling on and off.
- **Mode** – Temporal (best quality), FSR 1 (spatial only, cheaper) or Bilinear (a plain upscale, for comparison). Temporal and FSR 1 do their work in the **Temporal area** (below) and give the rest a fast spatial upscale.
- **Scaling** – DLSS's presets: how much larger the output is than what the game renders, per axis. Changing it rebuilds vrupscale's textures (a brief hitch).

  | Preset | Factor | Game renders, per axis | Game renders, pixels |
  |---|---|---|---|
  | Native | 1× | 100% | 100% |
  | Ultra Quality (default) | 1.30× | 77% | 59% |
  | Quality | 1.50× | 66.7% | 44% |
  | Balanced | 1.72× | 58% | 34% |
  | Performance | 2.00× | 50% | 25% |

  **Native** doesn't upscale. In Temporal mode it is anti-aliasing from your head's micro-motion, like DLAA (on the test scene about 3.5 dB better than FSR 1 at 1×, and steadier); FSR 1 at 1× only sharpens.

  **The resolution you set in SteamVR is the one you see.** SteamVR gives VRChat its global resolution × VRChat's own per-application resolution. While VRChat runs, the app lowers the global resolution to the table's pixel percentage (59% at Ultra Quality), so VRChat renders that share of what you set and vrupscale upscales it back. For example, with the global resolution at 100%, VRChat's own at 150% and Ultra Quality, the app sets the global to 59%: VRChat renders 89% (59% of 150%) and you see 150%.
  - Preset changes (in the app, or Ctrl+F5 in VR) apply at once. At Native, or while vrupscale isn't installed in SteamVR, the resolution is left alone. Your global resolution is put back when VRChat or the app closes, or, if the app closed unexpectedly, the next time it starts.
  - To change what you see while VRChat runs, change VRChat's per-application resolution; SteamVR applies it when VRChat starts, or straight away while SteamVR's settings window is open. Leave the global resolution alone meanwhile: SteamVR shows the lowered value then. "Auto" counts as 100% and is set back to Auto afterwards.
  - Each time the app changes the resolution, it tells the upscaler, which then finds VRChat's new render size in a fraction of a second instead of about five.
  - To manage the resolution yourself, set `[app] manageResolution=0` in the settings file and set VRChat to the table's percentage.
- **Sharpness** (default 1.00, the maximum) – how much the upscaled picture is sharpened (with FSR's RCAS), in Temporal and FSR 1 mode; 0 turns it off.
- **Sharpness area** (default 60%; 40% while **Follow gaze** is on) – the width and height of the part that is sharpened, centred on the **Temporal area** and moving with it. It can't be smaller than the Temporal area: at that size only the area is sharpened. Larger also sharpens the fast spatial upscale around the area, fading out across a band at its edge, for some GPU time; **all** sharpens the whole picture. Like the Temporal area it has two values, and raising the Temporal area past it raises it too.
- **FXAA** (default Off) – FXAA anti-aliasing **Before** the upscale (on the game's picture, which the upscale then works from) or **After** it (on the upscaled picture), in every mode. It smooths stair-stepped edges and softens fine detail. On the test scene it made Temporal less accurate either way (by 1.2–2.6 dB; Temporal already anti-aliases) and less steady unless the game had no anti-aliasing at all. It made FSR 1 steadier at a cost in sharpness, and came out ahead overall only for FSR 1 on a game without anti-aliasing, after the upscale. For trying it on your games.
- **Debug tint** – magenta on everything vrupscale upscales; in Temporal and FSR 1 mode, blue outside the area, where the fast spatial upscale is used.
- **Shortcuts in VR** – hold Ctrl and press a function key: F1 upscaling on/off, F2 mode (Temporal → FSR 1 → Bilinear), F3 debug tint, F4 steps through sharpness values, F5 next scaling preset (Native → … → Performance → Native, changing VRChat's resolution too), F6 Sharp history, F7 Bilinear edges, F8 calibrate the eye tracking, F9 FXAA (Off → Before → After). The app follows along and saves the change.

**Temporal** – Temporal mode keeps a *history* for each eye: the picture it has built up from earlier frames.
- **Temporal area** (default 60%; 40% while **Follow gaze** is on) – the width and height, as a share of each eye's picture, of the area that gets temporal detail (FSR 1 in FSR 1 mode). 60% covers about a third of the pixels. The rest gets a fast spatial upscale, and a soft band blends the two. This is the main cost control: smaller is cheaper, and **all** treats the whole picture.
  - It has two values: one for the centred area, and one for when it follows your gaze. That one can be smaller, since the area then sits where you look instead of covering everywhere you might look; 40% covers 16% of the pixels. The slider shows whichever value is in use, so switching **Follow gaze** on or off moves it between the two.
  - Changing it rebuilds the histories (a brief hitch).
- **History strength** (default 1.00, up to 2.00) – how much the earlier frames count. Higher is steadier but softer. On the test scene steadiness peaks around 1 and higher values only soften the image, so they are for experimenting.
- **Moving objects** (0 to 1, default 1.00) – how far history is trusted on things that move differently from your head, such as avatars. At 1 they get temporal smoothing too, with a little smearing on fast movement; at 0 they get the spatial upscale and never smear.
- **Motion tolerance** (default 1.50, up to 3.00) – how far the image may disagree with the estimated motion before history is dropped there. At 1.5 almost nothing is dropped; higher values only add ghosting risk.
- **Sharp history** (default off) – resamples the history with a Lanczos-3 filter instead of Catmull-Rom. History is resampled every frame and the softening adds up; on the test scene this keeps more of the finest detail (see the measurements below), but in the headset it is hard to see. Costs some GPU time.
- **Bilinear edges** (default off) – outside the area, a plain bilinear upscale instead of Catmull-Rom: softer, cheaper, and where the lenses blur the most anyway. The area is untouched and the blend band still hides the border. Greyed out when the area is **all**.

**Eye tracking** (Bigscreen Beyond 2e) – the area follows your gaze, so the sharp, steady part of the picture is wherever you look.
- Setup: install Bigscreen's eye tracking (the **Eyetracking Add-On** in the Bigscreen Beyond Utility), start its Eyetracking client with your trained model, and switch on **VRCFT** there. vrupscale reads that output, the same data Bigscreen's VRCFaceTracking module reads; VRCFaceTracking itself doesn't need to run, and if it does, both read side by side.
- **Follow gaze** (the switch next to the title, default off) – the area moves to where you look, once your gaze leaves a small zone around its centre (2% of the width), so tiny eye movements leave it alone. In Temporal mode, the strip the area newly covers when it moves starts without history and catches up within a few frames. If tracking drops out (a blink, the headset off), the area stays where it was.
- **Show gaze** – a green ring in the headset where vrupscale thinks you look. The ring should sit on whatever you look at; if it doesn't, calibrate.
- **Calibrate** (or Ctrl+F8 in VR) – a cross appears straight ahead in both eyes; look at its centre until it disappears (about 3 seconds). The tracker's reading during that time becomes an offset taken off every later reading, so that straight ahead lines up. Afterwards the ring shows for a few seconds so you can check. If your gaze wandered, the calibration fails and the old one stays. Needs Temporal or FSR 1 mode and live eye tracking.
- **Clear calibration** removes the offset.
- The app reads each eye's field of view from SteamVR once per session (to place the gaze in each eye's picture), so have it open the first time you use eye tracking.

**SteamVR** – installing, and the settings:
- Where SteamVR is, and which version of the upscaler is installed in it.
- **Install / Update** puts the upscaler into SteamVR and **Uninstall** removes it; both need SteamVR closed. If another tool's `dxgi.dll` was there, Install sets it aside (after asking) and Uninstall puts it back. Uninstall also switches off Start with SteamVR.
- **Start with SteamVR** (default on) – SteamVR starts the app when it starts, minimized, and the app closes when SteamVR does, putting SteamVR's resolution back first. The app adds itself to SteamVR's startup overlay apps (SteamVR settings, **Startup / Shutdown**) the first time SteamVR runs while it is open; switching vrupscale off in that list switches this off too. If the app was moved to another folder, it moves its entry there the next time SteamVR runs with it open.
- Settings are saved automatically to `%LOCALAPPDATA%\vrupscale\vrupscale.ini` and apply at once. **Restore defaults** resets them all except your texture choices, the eye tracking calibration and SteamVR's field of view; **Settings file** shows the file in Explorer.

**Textures** – every large texture the compositor sampled in the last minute, what vrupscale did with it, and why. Point at a row for details.
- The game's eye textures are marked **eye**. When the game renders into only part of a larger texture (after SteamVR's resolution was lowered), the row shows that part: `uses 2516×2516 → 3573×3573`.
- Untick a size to stop upscaling it, for example an overlay panel that got tinted. Eye textures can't be unticked (leaving one of an eye's textures alone makes it flicker); switch upscaling off instead.
- Unticked sizes stay listed even when not in use, so you can tick them again; **Tick all** ticks them all.
- Sizes skipped by rule (too small, too wide, SteamVR's own) can't be ticked.

**Recent events** – the last lines the upscaler logged inside SteamVR (scroll with the mouse wheel). **Open log** opens the whole log.

## How the temporal mode works

For each eye, on every new frame from the game:

1. **Motion estimation (no help from the game).**
   - A luma pyramid feeds a coarse block search, then pyramidal Lucas–Kanade gives local motion on an 8×8-pixel grid.
   - A **robust affine fit** of that field (iteratively re-weighted least squares) gives a precise model of the head motion. Rotation moves the image almost affinely, and thousands of local estimates pin it down to about 0.01–0.1 px.
   - Each 4×4-pixel cell keeps the global model unless it fails there: moving avatars, near objects while you move, or a scene cut.
2. **Accumulation.**
   - Head micro-motion moves the scene by fractions of a pixel every frame. Registered precisely, those offsets act like the jitter a game would add on purpose.
   - Each output pixel blends the current frame's reconstruction with the reprojected history (Catmull-Rom, or Lanczos-3 with **Sharp history**), weighted by how close a real input sample lands to it.
   - History is colour-clipped against the current frame's neighbourhood (YCoCg variance box).
   - History is dropped where the motion doesn't explain the image (contrast-normalised residual).
3. **Moving things.** Where the motion differs from the head's, the upscaler searches for that thing's own motion and uses history there as far as **Moving objects** allows. The motion it finds must pass checks first: the image there must pin it down (a plain edge or a stripe pattern fits many motions), neighbouring cells must agree, and it must continue last frame's motion. Where it fails, that cell gets no history, or the head's motion if it followed the head last frame. At **Moving objects** 0, moving things get the spatial reconstruction instead: no smearing, and no temporal gain there either.
4. **RCAS sharpening**, then the result goes to the compositor.
5. **The area.** Steps 1–4 run only for the **Temporal area**, plus a margin for the motion search. Outside it, the output is a Catmull-Rom upscale of the current frame (bilinear with **Bilinear edges**), which costs a fraction as much; through VR lenses the edges are blurrier anyway. A band inside the area's edge blends the two, so there is no visible border. Sharpening (RCAS) covers the area, and the spatial upscale around it as far as the **Sharpness area** reaches. With **Follow gaze**, the area is placed around the gaze for each new frame, and the history stays registered to the picture as it moves.

FSR 1 mode uses the area the same way: FSR 1 (EASU, then RCAS over its result) runs only there, with the same spatial upscale outside, the same blend band and the same gaze following.

Decisions are made on the GPU, so the CPU never waits for it:
- **New-frame detection:** a fingerprint of each frame tells whether the game delivered a new one. When the compositor shows an old frame again (reprojection, motion smoothing), the expensive passes are skipped through indirect dispatch.
- **Little work on SteamVR's thread:** each texture's passes are recorded once into a command list and replayed every frame after, two calls instead of about a hundred.
- **Swapchain textures:** SteamVR draws each of the game's swapchain textures every frame. An eye's textures share one history, and when the game's render size changes they move to the new size together.

## Status and testing

- **In SteamVR:** used with VRChat on an RTX 3090. The app's status line shows the GPU time vrupscale takes on your system. It depends mostly on the resolution you see (set in SteamVR), not on the preset: a higher preset makes the game cheaper, while vrupscale's output stays the same size.
- **Test harness:** vrupscale is cross-compiled and run against a real D3D11 implementation (DXVK on a software Vulkan GPU, under Wine), with a harness that behaves like SteamVR's compositor. It runs once drawing directly (193 checks) and once recording into command lists like SteamVR (195 checks); all pass. The checks cover quality against ground truth, the area in both modes (the same result inside it as processing the whole picture, and no visible seam), the Sharpness area (RCAS outside the area matches a CPU reference), FXAA before and after the upscale in every encoding, SteamVR drawing every swapchain texture each frame, the passes replayed from command lists matching them issued one by one pixel for pixel, a repaint matching what the new frame's update wrote, games rendering into part of larger textures, resolution changes while running, eye tracking, and the app's live connection.
- **SteamVR's resolution** is tested against a stand-in for `openvr_api.dll` that records the calls: VRChat starting, preset changes, Native, a change of VRChat's own resolution, VRChat closing and the app closing each set or put back SteamVR's global `supersampleScale` as they should.
- **Eye tracking** works with a real Bigscreen Beyond 2e. The tests also run it against a stand-in for Bigscreen's eye tracking client (the same shared memory and data layout that Bigscreen's own VRCFaceTracking module reads): the area follows the gaze in both eyes and both modes, the history moves with it (no smearing or shifted picture), the dead zone, calibration and its failure on a restless gaze, the markers, and running without a tracker.
- **Risk:** at worst, SteamVR fails to start or the image looks wrong. Click **Uninstall** in the app (with SteamVR closed), or delete `dxgi.dll` from `SteamVR\bin\win64`.
- **Anti-cheat:** nothing is loaded into VRChat's process. EAC protects VRChat's process; how it treats a modified SteamVR install is a reasoned guess, not a guarantee.

### Measured quality (test scene)

The test scene is a procedural VR-like scene with thin lines, tiny text, a zone plate, a grating above the input's Nyquist limit, sub-pixel head-like motion, and an independently moving object. It is upscaled 400×440 → 520×572 (1.3×) for 24 frames in both eyes and compared with the same scene rendered at the output resolution (4×4 supersampled). Values are PSNR against that ground truth; higher is better. **Steadiness** compares each frame's error with the previous frame's at the same point of the scene: less shimmer scores higher.

| Region | FSR 1 | Temporal (defaults, area 100%) | Temporal, Moving objects 0 | Temporal, Sharp history on |
|---|---|---|---|---|
| Steady background | 23.12 dB | **24.09 dB** | 24.00 dB | 24.79 dB |
| Pattern above the input's Nyquist limit | 19.35 dB | **19.97 dB** | | 21.40 dB |
| Steadiness (background) | 24.13 dB | **27.40 dB** | | 27.93 dB |
| Area the moving object just uncovered (ghosting) | 27.06 dB | **27.67 dB** | 28.00 dB | |
| Moving object | 20.16 dB | 19.60 dB (smears a little) | **20.84 dB** | |
| Still scene (no motion, nothing to gain) | 23.17 dB | 23.42 dB | | |

The defaults gain most in steadiness (less shimmer). Sharp history shows up in the fine-detail numbers, but the difference is hard to see in the headset. With **Moving objects** at 1, the moving object comes within 0.6 dB of FSR 1 and the background it uncovers beats FSR 1; at 0 the object gains 1.2 dB.

At each scaling preset (defaults, the same sequence rendered at the matching output size):

| Preset | Steady background, FSR 1 → temporal | Pattern above Nyquist | Steadiness |
|---|---|---|---|
| Native 1× | 30.47 → 34.26 dB | 26.24 → 29.91 dB | 31.55 → 34.72 dB |
| Ultra Quality 1.30× | 23.12 → 24.09 dB | 19.35 → 19.97 dB | 24.13 → 27.40 dB |
| Quality 1.50× | 22.33 → 23.46 dB | 18.36 → 19.37 dB | 23.92 → 27.96 dB |
| Balanced 1.72× | 21.58 → 22.60 dB | 17.62 → 18.73 dB | 24.03 → 28.35 dB |
| Performance 2.00× | 21.06 → 21.64 dB | 17.14 → 17.81 dB | 24.50 → 29.25 dB |

Absolute values fall as the preset rises because there is less input per output pixel; at Native, "FSR 1" means FSR 1 at 1× (sharpening only). Temporal stays ahead of FSR 1 at every preset, and its gain in steadiness grows with the factor.

History strength, on the same sequence over 40 frames (Lanczos-3 history; measured with 0.4.4):

| History strength | 0.5 | 1 | 2 | 4 |
|---|---|---|---|---|
| Steady background | 24.67 dB | 24.64 dB | 24.19 dB | 23.47 dB |
| Pattern above the input's Nyquist limit | 21.21 dB | 21.23 dB | 20.56 dB | 19.45 dB |
| Steadiness | 26.92 dB | **27.28 dB** | 27.22 dB | 26.91 dB |

Above 1 the picture gets softer faster than it gets steadier. That holds with perfectly known motion and with calmer head motion too, so the limit is the repeated resampling of the history, not motion estimation. Motion tolerance above 1.5 and a wider colour clamp (`clipGamma`) changed nothing measurable.

By part of the picture, with the area at 60% (the default):

| Part of the image | FSR 1, whole image | FSR 1, area 60% | Bilinear | Temporal, area 100% | Temporal, area 60% | Area 60%, Bilinear edges |
|---|---|---|---|---|---|---|
| Area | 21.93 dB | 21.93 dB | | 22.58 dB | 22.56 dB | 22.56 dB |
| Blend band | 22.00 dB | 21.91 dB | 20.76 dB | 22.67 dB | 22.21 dB | 21.75 dB |
| Outside (spatial) | 24.02 dB | 24.20 dB | 23.51 dB | 25.09 dB | 24.20 dB | 23.51 dB |

The moving object crosses the area, which still gains over FSR 1.

This test content is deliberately hard: heavily aliased, with sub-pixel detail everywhere. Real VRChat frames are mip-mapped and anti-aliased, which usually makes motion estimation easier.

## Self-test (optional, no SteamVR involved)

`selftest\selftest.cmd` runs the full test harness against your real GPU and D3D11, including real cross-process shared textures. It runs twice, the second time recording into command lists like SteamVR, and takes a few minutes. Expect `0 failed` for both parts (193 and 195 checks). The results (GPU name, quality tables, timings, pass/fail) are saved to `selftest\selftest_results.txt`; that is the file to send back.

The performance section measures the GPU cost at two sizes (Index-like, 1561×1735 ×2 → 2014×2238, and a high-resolution headset, 2756×2756 ×2 → 3555×3555) while a stand-in for the game keeps the GPU busy. It reports what bilinear, FSR 1 (whole image, at the 60% area, and that with the Sharpness area at all) and temporal (whole image, at the 60% area, with Bilinear edges, with the Sharpness area at all, and with vrupscale's own GPU timing on, as in use) add per frame for both eyes, with a per-pass breakdown of temporal at the 60% area. Compare the added cost with your frame budget (11.1 ms at 90 Hz).

## If something goes wrong

**SteamVR won't start or crashes:** close it, click **Uninstall** in the app, and send the log (**Open log**).

**The status says "SteamVR is running without vrupscale":** SteamVR was started before you installed. Restart SteamVR.

**Nothing is upscaled:** the texture list says why for each texture. If VRChat's eye textures are unticked, tick them (or **Tick all**). If they aren't listed at all, send the log.

**"Temporal paused":** for more than a second, the game's textures for one eye have held pictures of different sizes, which can't share a history. That eye gets FSR 1 meanwhile, for every frame (switching back and forth would flicker). A change of SteamVR's resolution shouldn't cause it. If it stays, restart the game, which then makes new textures of one size, and send the log.

**An overlay or menu gets upscaled (tinted magenta):** untick its size in the texture list.

**The app doesn't start with SteamVR:** open it once while SteamVR runs, with **Start with SteamVR** on; the SteamVR panel says if SteamVR refused. In SteamVR's settings, **Startup / Shutdown → Choose startup overlay apps** should list vrupscale, switched on.

**The update failed:** the app says why, and nothing was changed (a half-finished update is rolled back). Try again later, or download the zip from the [releases page](https://github.com/Zakhazerd/vrupscale/releases/latest) and extract it over the old folder. If the app is in a folder Windows protects (such as Program Files), it can't replace its own files: move it to a folder of your own.

**Ghosting or smearing:** on moving avatars, lower **Moving objects** (0 turns history off on them). Elsewhere, lower **History strength** (say 0.50) or **Motion tolerance** (say 0.90). Note when it happens (turning, walking, avatars moving) and send the log.

**Frame drops that weren't there with FSR 1:** check the GPU time in the app's status line. Make sure **Sharp history** is off, switch **Bilinear edges** on, bring the **Sharpness area** down to the Temporal area's size, lower **Temporal area** (at 40% it covers 16% of the pixels instead of 36%), use a lower resolution in SteamVR (vrupscale's cost follows it), or switch to FSR 1. The compositor's work has to fit into the short time before each display refresh; if it doesn't, SteamVR drops frames even when the GPU has time to spare elsewhere. The log breaks the GPU time down per pass every 10 seconds: send it.

**The image is cropped to one corner or zoomed in:** the compositor reads that texture with integer texel fetches instead of UVs. Untick its size and send the log.

**Streaming headsets (Virtual Desktop, ALVR, Steam Link):** untested. Their drivers may composite layers themselves, in which case vrupscale never sees the eye textures. Virtual Desktop has its own upscaler on the headset (SGSR) anyway.

## Settings file reference

`%LOCALAPPDATA%\vrupscale\vrupscale.ini`. The app writes the keys it shows; the upscaler re-reads the file within a second of a change, so editing it by hand works too. Keys the app doesn't show:
- `[general] active=0` – the upscaler does nothing but pass calls on to Windows' own `dxgi.dll` (read when SteamVR starts).
- `[general] logLevel` (0 errors, 1 info, 2 verbose); `statsIntervalSec` (default 10: a stats line in the log every N seconds, 0 off). The stats line includes the CPU time vrupscale takes on SteamVR's compositor threads (per second, and the longest single call), how many temporal runs were replayed from command lists, and temporal mode's GPU time per pass, per new frame and eye (timed on a sample of frames), with the quick checks of frames already processed listed apart. `gpuTiming=0` turns off all of vrupscale's GPU timing (the app's GPU figure and the per-pass times). The log is written, and the settings file read, by a thread of its own, never from inside SteamVR's frame.
- `[match]` – rules for what counts as an eye texture: `minWidth`, `minHeight`, `minAspect`, `maxAspect`, `requireShared`, `onlySizes` / `excludeSizes` (comma-separated `WxH`), `maxArraySize`.
- `[upscale] upscaleFactor` – the **Scaling** preset's factor. Any value from 1.00 to 3.00 works; one that isn't a preset shows as Custom in the app, and one within 0.02 of a preset is moved onto it when the app starts.
- `[eyes]` – `followGaze`, `showGaze` (the app's switches); `offsetX` / `offsetY` (the calibration: tangents of the angle taken off the tracker's reading, x right, y up); `projection` (each eye's field of view as SteamVR reports it: tangents left, right, top, bottom, left eye then right; written by the app); `deadzone` (default 0.02: how far the gaze must move from the area's centre, as a fraction of the width, before the area follows).
- `[app] checkUpdates=0` stops the app from asking GitHub for updates. `updateDll=1` is set by an update while the upscaler inside SteamVR still waits to be updated.
- `[app] withSteamVR=0` – **Start with SteamVR** off. `withSteamVRApplied` is what SteamVR was last told (1 on, 0 off), so that the app can tell when you switched it off in SteamVR's own list. The app's entry there comes from `vrupscale.vrmanifest`, which the app writes next to itself (or, if that folder can't be written, next to the settings file).
- `[app] manageResolution=0` – the app leaves SteamVR's resolution alone (see Scaling). `globalResBefore` holds your global resolution while the app has it lowered (the scale, and 1 for Custom or 0 for Auto); it is put back from there.
- `[upscale] sharpnessArea` and `gazeSharpnessArea` – the **Sharpness area** as fractions (0.3–1.0): centred (default 0.6) and while following the gaze (default 0.4). Less than the Temporal area counts as the Temporal area.
- `[temporal]` – `temporalArea` and `gazeArea` are the **Temporal area** as fractions (0.3–1.0): centred (default 0.6) and while following the gaze (default 0.4), in both Temporal and FSR 1 mode. `sharpHistory` and `bilinearEdges` are those switches; `maxWeight`, `localTrust` and `residualTol` are **History strength**, **Moving objects** and **Motion tolerance** (by hand, `maxWeight` goes up to 64).
- `[temporal] maxStreams` (default 4) – the most histories at once. Each takes about 38 MB for Index-sized eye textures at the default area (82 MB with the area at all).
- `[temporal] detectContent` (default 1) – find the part of each eye texture the game renders into. `probeWindowMs` (default 500) is how often that is checked (every 100 ms for a few seconds after the app changes SteamVR's resolution).
- `[temporal] localCheck` (default 47) – the checks that motion different from the head's must pass before history is used on it, as a sum of bits: 1 the image pins the motion down, 2 neighbouring cells agree, 4 it continues last frame's motion. Where they fail: 16 no history; 32 the head's motion, only where that cell followed the head last frame (otherwise no history); neither: the head's motion. 8: no history where neither the head's motion nor one of its own fits. 0: no checks. `localAgree` (default 1.0 input px): how close two motions must be to agree. `localAperture` (default 0.3): how clearly the image must pin the motion down (the ratio of the weakest to the strongest direction of its gradients).
- `[temporal] workMargin` (default 128, at least 64) – input pixels around the temporal area over which the luma pyramid for motion estimation is built.
- `[temporal] commandLists` (default 1) – record each texture's passes once into a command list and replay it. `0` issues them call by call, as before 0.6.6: more CPU time on SteamVR's thread, the same picture. If replayed lists turn out not to run on the GPU, vrupscale switches to `0` by itself and says so in the log.

## Limits

- **Recovering detail depends on head motion.** The upscaler can only recover detail when the scene moves by fractions of a pixel between frames. Your head is never perfectly still, but with the headset lying on a desk, temporal looks like a good spatial upscaler and no better.
- **Moving objects can smear.** With **Moving objects** at 1 (the default), avatars and nearby objects keep history wherever their own motion is found, and smear a little where it is found wrongly. At 0 they get the spatial upscale instead.
- **It runs after the game's post-processing and UI.** Overlays drawn into the eye texture (menus, nameplates) are upscaled like everything else.
- **Finding a new render size takes a moment** (vrupscale watches which pixels change): a fraction of a second when the app changed SteamVR's resolution, and otherwise about 5 seconds for a smaller size (a larger one is taken at once). Until then, the area is placed for the old size. A completely still scene says nothing about the size, so the last one found is kept.
- **Without eye tracking, the area is centred in each eye's picture.** On most headsets the lens centre is close to that. On headsets with strongly canted or asymmetric displays (some Pimax models), the sharpest part of the lens may sit off-centre; a larger area covers it.
- **Eye tracking: one-point calibration.** It corrects a constant offset (where straight ahead is), not a tracker that reads angles too large or too small. If the ring drifts when you look far to the side, the area is large enough to cover small errors there. Only Bigscreen's own eye tracking is read; other trackers (EyeTrackVR, Babble) don't publish this data.
- **Extra video memory:** about 38 MB per history (Index-sized eye textures, default area), plus one upscaled copy of each eye texture.
- **Compositor updates:** SteamVR updates can change the compositor. vrupscale logs hook failures and otherwise stays out of the way.

## Building from source

Requirements: MinGW-w64 (Linux, or MSYS2 on Windows) and Python 3.

```
./build.sh          # -> build/dxgi.dll (imports only KERNEL32 and msvcrt) and build/vrupscale.exe
tools/package.sh    # -> build/vrupscale-<version>.zip
```

The compute shaders come precompiled in `src/gen/shaders.bin` (one binary file, embedded in the DLL by `src/shader_pack.S`), so building needs no shader compiler. After editing `shaders/*.hlsl`, regenerate it with `tools/shaders.sh`, which needs Microsoft's `d3dcompiler_47.dll` (it runs under Wine on Linux). The app's fonts (`app/fonts/`) and icon are generated too: `tools/fonts.py` builds the fonts from Rubik in `third_party/rubik` (needs `pip install fonttools ttfautohint-py`), and `tools/icon.py` draws the icon (needs Pillow).

Tests: `test/fake_compositor.cpp` (with `test/temporal_tests.inc`) is the functional and quality harness and the self-test. Run it with `VRUPSCALE_FORCE=1` and our `dxgi.dll` next to the exe.
- `VRUPSCALE_TEST_DEFERRED=1` records the "compositor's" draws into command lists, like SteamVR.
- `VRUPSCALE_TEST_PROBE=1` exercises the probe-device fallback.
- `VRUPSCALE_TEST_QUALITY_ONLY=1` / `VRUPSCALE_TEST_PERF_ONLY=1` run one section (`VRUPSCALE_PERF_FRAMES=N` timed frames per mode); `VRUPSCALE_TEST_ONLY=switch`, `=gaze` or `=corner` runs one test.
- `VRUPSCALE_TEST_SERVE=<seconds>` keeps drawing like a running compositor, for trying the app against it (set `VRUPSCALE_CONTROL=Local\vrupscale.control.v1` so the app finds it).
- `test/stub/openvr_stub.c` stands in for `openvr_api.dll` and records what the app asks SteamVR (settings, its startup entry, and a quit request on demand); `test/stub/fakegaze.c` stands in for Bigscreen's eye tracking client.

The app also has command-line modes for scripting: `--install [--force]`, `--uninstall`, `--steamvr <folder>`, `--set key=value`, `--status-file <file>`, `--settings <ini>`, `--channel <name>`, `--changelog` (opens the change log at start) and `--from-steamvr` (what SteamVR passes: starts minimized, and quietly does nothing if the app is open already); and self-tests, each with `--report <file>`: `--test-update` (the updater's zip reader, hash and version comparison), `--test-changelog` (reading CHANGELOG.md and laying it out), `--check-update` (asks GitHub, or `VRUPSCALE_UPDATE_URL`, and reports), `--test-game-resolution` and `--test-steamvr-link` (the startup manifest).

Releases: set `VU_VERSION_STR` in `src/common.h`, add a `## <version>` section to `CHANGELOG.md` (it becomes the release's notes, which the app's Update button shows), and push to `main`. When `main` has a version without a release, GitHub Actions (`.github/workflows/release.yml`) builds everything with MinGW-w64 on Ubuntu and publishes release `v<version>` at that commit with `vrupscale-<version>.zip`, the name the app's updater looks for. The updater checks the download against the SHA-256 that GitHub publishes for it. Pushing a tag `v<version>` works too.

Layout:
- `src/exports.S`, `src/proxy.cpp`: dxgi forwarding
- `src/hooks.cpp`: MinHook detours (every method-body variant)
- `src/upscaler.cpp`: classification, proxies, the spatial path, housekeeping, command-list handling
- `src/temporal.cpp`: per-eye histories and the temporal pass sequence
- `src/control.cpp`, `src/control_shared.h`: the live channel to the app
- `shaders/upscale.hlsl`: FSR 1 and bilinear; `shaders/temporal.hlsl`: the temporal passes; `src/permutations.h`: every compiled variant
- `app/`: the Windows app (`ui.cpp` draws it, shapes with GDI+ and text with GDI; `update.cpp` updates it from GitHub; `gameres.cpp` sets SteamVR's resolution for VRChat; `vrlink.cpp` holds the connection to SteamVR and the app's startup entry; `changelog.cpp` shows the change log)

## Licenses

vrupscale: MIT. Bundled: MinHook (BSD-2-Clause, Tsuda Kageyu), AMD FidelityFX Super Resolution 1 (MIT, Advanced Micro Devices; the temporal path uses FSR 1's RCAS), the Rubik typeface in the app (SIL Open Font License 1.1, The Rubik Project Authors; subset, hinted, with an added arrow), and Valve's `openvr_api.dll` (OpenVR SDK, BSD-3-Clause), which the app uses to change SteamVR's settings and to start and close with it. See `LICENSES/`. The app's look is inspired by Valve's Steam hardware pages; its illustrations are original, and vrupscale is not affiliated with Valve.
