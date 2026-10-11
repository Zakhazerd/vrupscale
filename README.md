# vrupscale 0.6.0

A temporal upscaler inside the SteamVR compositor, for apps you can't mod, including VRChat.

VRChat's Easy Anti-Cheat rules out anything that loads into `VRChat.exe` (vrperfkit, openvr_fsr, ReShade, OpenXR API layers). vrupscale runs in `vrcompositor.exe` instead. The app renders at a lower resolution, and the compositor gets an upscaled version of each eye texture just before it applies lens distortion.

The default mode is temporal: it accumulates detail across frames using motion it estimates itself. To keep it cheap, only the centre of each eye image (where the lens is sharp and you look most) gets the temporal treatment; the edges get a fast spatial upscale. FSR 1 is still available as a spatial-only mode.

## Quick start

1. Download `vrupscale-<version>.zip` from the [releases page](https://github.com/Zakhazerd/vrupscale/releases/latest), extract the whole zip somewhere permanent that you can write to (for example `Documents\vrupscale`, not Program Files) and run **`vrupscale.exe`**. It isn't code-signed, so Windows SmartScreen may warn the first time: **More info → Run anyway**. Later versions come through the app itself (see Updates below).
2. Close SteamVR completely, then click **Install** in the app. It finds SteamVR on its own; if it can't, use **Choose folder…**. If Windows asks for administrator rights, that is because your SteamVR folder needs them.
3. Start SteamVR. The app's headline changes to **Connected to SteamVR.** (the little headset wakes up).
4. Switch on **Set VRChat's resolution** under Scaling, and keep the app open: while VRChat runs, the app sets SteamVR's resolution so VRChat renders what the preset needs (59% for the default, Ultra Quality). Or set VRChat's resolution yourself in **SteamVR → Settings → Video → Per-application video settings → VRChat**: the app shows the percentage.
5. Start VRChat. The line under the headline shows what is being upscaled, for example `Upscaling 2756×2756 → 3555×3555, temporal`.
6. Switch on **Debug tint** for a moment: everything vrupscale processes is tinted. In temporal mode the centre turns magenta and the edges blue. Only the world should be tinted, not SteamVR's menus or overlays.
7. Check the cost: the line below that shows the GPU time vrupscale takes per frame, for example `GPU 1.10 ms per displayed frame (9.9%)`.

You can keep the app open while you play or close it; vrupscale itself runs inside SteamVR either way.

## The app

At the top, a headline says what is going on, with what is being upscaled (the resolution the game renders at, and what it becomes), the mode, how many new frames per second arrive, and the GPU time vrupscale uses per frame (both eyes together). The characters on the right show it too: asleep while SteamVR isn't running, sparkling while frames are being upscaled, with an orange **!** when something needs you. **Temporal paused** means an eye's textures can't share one history (see 0.4.2 below); that eye gets FSR 1 meanwhile. Since 0.5.0 it shows only when that lasts more than a second, which a resolution change no longer causes.

**Updates:** when a newer version is out on GitHub, an **Update to …** button appears next to the version number at the top; point at it for what the new version brings. One click downloads it, checks it against the checksum published with it, replaces the app's files and restarts the app in the new version. Your settings stay. SteamVR's copy (the DLL inside SteamVR) is updated at once if SteamVR is closed, otherwise as soon as it closes, by the restarted app (keep it open, or the next time you open it). The app asks GitHub when it starts and every 6 hours while it runs; nothing is sent besides the request itself.

Below are six cards. Rest the pointer on a control for a moment and a longer description of it appears: what it does, which way to move it and what that costs, its default, and its shortcut in VR (for the mode and the scaling presets, of the one under the pointer). Everything is reachable with the keyboard as well: Tab moves between controls, arrow keys change sliders and the mode, Space switches. On a small screen the page scrolls.

**Upscaling** – changes apply immediately, in the headset:
- The switch next to the title turns upscaling on and off.
- **Mode** – Temporal (best quality), FSR 1 (spatial only, cheaper), Bilinear (plain upscale; for comparisons).
- **Scaling** – DLSS's presets: how much larger the output is than what the game renders, per axis. Changing it rebuilds the upscaler's textures (a brief hitch).

  | Preset | Factor | Game renders, per axis | VRChat's SteamVR resolution (pixels) |
  |---|---|---|---|
  | Native | 1× | 100% | 100% |
  | Ultra Quality (default) | 1.30× | 77% | 59% |
  | Quality | 1.50× | 66.7% | 44% |
  | Balanced | 1.72× | 58% | 34% |
  | Performance | 2.00× | 50% | 25% |

  **Native** doesn't upscale: the eye textures are processed at their own size. In temporal mode that is anti-aliasing from head micro-motion (like DLAA): on the test scene +3.6 dB on the background and +3.3 dB on fine detail over FSR 1 at 1×, and steadier. FSR 1 at 1× only sharpens.

  A preset only says how much to upscale what VRChat sends; the render resolution itself is SteamVR's to set. A preset works like DLSS's once VRChat renders at the table's percentage: switch on **Set VRChat's resolution** below the presets, or set it yourself.
- **Set VRChat's resolution** (default off) – while VRChat runs in SteamVR and this app is open, the app sets SteamVR's resolution so that VRChat renders at the preset's percentage, and follows every preset change at once (in the app, or by Ctrl+F5 in VR). When VRChat closes, you switch the option off, or you close the app, it puts back what you had.
  - It changes SteamVR's **global** resolution (Settings → Video → Render Resolution), not VRChat's own per-application one. SteamVR applies a global change to the running game straight away; a per-application change it only applies when the game starts, or while SteamVR's settings window is open (which is what 0.4.7 and 0.4.8 relied on without knowing).
  - VRChat renders at global × its per-application value, so the app divides by VRChat's own value: with VRChat's slider at 70% and Ultra Quality (59%), the global becomes 84%. VRChat's own setting is left alone.
  - While it is applied, SteamVR's settings window shows the changed global resolution. "Auto" resolution counts as 100% and is switched back to Auto afterwards. If the app closes unexpectedly while VRChat runs, it puts the resolution back the next time it starts.
  - Each time it changes the resolution, the app tells vrupscale, which then looks for VRChat's new render size every 100 ms for a few seconds instead of every half second, and finds a smaller one in a fraction of a second instead of about five.
  - Without the option, Ctrl+F5 changes only the upscale, not VRChat's render resolution.
- **Sharpness** (default 1.00, the most RCAS can do) – sharpening after upscaling.
- **Debug tint** – magenta on everything that is upscaled; in temporal mode, blue on the edges that get the spatial upscale.
- **Shortcuts in VR** – Ctrl+F1 on/off, Ctrl+F2 cycle temporal → FSR 1 → bilinear, Ctrl+F3 tint, Ctrl+F4 cycle sharpness, Ctrl+F5 next scaling preset (Native → Ultra Quality → … → Performance → Native), Ctrl+F6 Sharp history on/off, Ctrl+F7 Bilinear edges on/off, Ctrl+F8 calibrate the eye tracking. These are the **function keys**. The app's controls follow along and save the change. Ctrl+F5 also changes VRChat's render resolution when **Set VRChat's resolution** is on and the app is open; otherwise only the upscale.
**Temporal**
- **Temporal area** (default 60%): the width and height of the centred part that gets temporal detail, so 60% covers about a third of the pixels. The rest gets a fast spatial upscale, and a soft band blends the two. This is the main cost control: smaller is faster, **all** turns it off. Changing it rebuilds the histories (a brief hitch).
- **History strength** (default 1.00, up to 2.00): how much of the previous frames to keep, counted in frames' worth of a direct sample. Higher is steadier but softer. On the test scene steadiness peaks around 1, and above it the image only gets softer, so values over 1 are for trying, not a recommendation.
- **Moving objects** (default 1.00, the most there is: it is a share of trust, 0 to 1): history on content that moves differently from your head (avatars). On, moving things get temporal smoothing too, at the cost of some smearing on fast movement; 0 gives them the spatial upscale, with no smearing at all.
- **Motion tolerance** (default 1.50, up to 3.00): how wrong the motion may look before history is dropped there. At 1.5 almost nothing is dropped already; above it only ghosting risk grows.
- **Sharp history** (default off): resamples the history with a Lanczos-3 filter instead of Catmull-Rom. History is resampled every frame and the softening adds up; on the test scene this keeps more of the finest detail (see the measurements below), but in the headset the difference is hard to see. Costs some GPU time.
- **Bilinear edges** (default off): outside the temporal area, a plain bilinear upscale instead of Catmull-Rom. Softer at the edges, where the lenses blur the most anyway, and cheaper. The centre is untouched, and the blend band between the two still hides the border. Greyed out when the area is **all** (there are no edges).

**Eye tracking** (Bigscreen Beyond 2e) – the temporal area follows your gaze, so the sharp, steady part of the picture is wherever you look, not only in the middle.
- Setup: Bigscreen's own eye tracking (the **Eyetracking Add-On** of the Bigscreen Beyond Utility, then its Eyetracking client with your trained model active). In that client, switch on **VRCFT**. That output is what vrupscale reads (the same data Bigscreen's VRCFaceTracking module reads). VRCFaceTracking itself doesn't have to run; if you already use it with Bigscreen's module, it and vrupscale read the same data side by side.
- **Follow gaze** (the switch next to the title, default off): the area moves to where you look. It moves once your gaze leaves a small zone around its centre (2% of the width), so tiny eye movements leave it alone. When it moves, the strip it uncovers starts without history and catches up within a few frames, like anything that comes into view. If tracking drops out (a blink, the headset off), it stays where it was.
- **Show gaze**: a green ring in the headset where vrupscale thinks you look. Use it to check the calibration: the ring should sit on whatever you look at.
- **Calibrate** (or **Ctrl+F8** in VR): a cross appears straight ahead in both eyes. Look at its centre until it goes (about 3 seconds). The tracker's reading during that time becomes the offset taken off every later reading, so "straight ahead" lines up. Afterwards the ring shows for a few seconds so you can check. If your gaze wandered, it fails and keeps the old calibration. Needs Temporal mode and live eye tracking.
- **Clear calibration** removes the offset.
- To place the gaze in each eye's picture, the app asks SteamVR for each eye's field of view once per SteamVR session and saves it, so the app should be open when you first use eye tracking.

**SteamVR** – where SteamVR is, which vrupscale version is installed in it, **Install / Update** and **Uninstall**. SteamVR must be closed for both. Uninstall puts back any other tool's `dxgi.dll` that the installer had to replace (after asking you). Below that, the settings: saved automatically and applied live. **Restore defaults** resets all settings (not your texture choices, the eye tracking calibration, or SteamVR's field of view); **Settings file** shows it in Explorer.

**Textures** – every large texture the compositor sampled in the last minute, what vrupscale did with it, and why. Point at a row for its details.
- The game's eye textures are marked **eye**. When the game renders into only part of a larger texture (after SteamVR's resolution was lowered), the row shows that part: `uses 2516×2516 → 3573×3573`.
- Untick a size to stop upscaling it, for example an overlay panel that got tinted. Eye textures can't be unticked (leaving one of an eye's textures alone makes it flicker); switch upscaling off instead.
- Unticked sizes stay listed even when not in use, so you can tick them again; **Tick all** clears them all.
- Sizes skipped by rule (too small, too wide, SteamVR's own) can't be ticked.

**Recent events** – the last lines vrupscale logged inside SteamVR (scroll with the mouse wheel). **Open log** opens the whole log.

Settings are saved automatically to `%LOCALAPPDATA%\vrupscale\vrupscale.ini` and apply when SteamVR starts. Earlier versions kept them next to the DLL in `SteamVR\bin\win64`; the app takes those over once and removes the old file when it installs.

## Why not DLSS / FSR 2–4 / XeSS?

Those upscalers need data that is generated inside the game:
- a depth buffer
- per-pixel motion vectors
- deliberate sub-pixel camera jitter

VRChat sends SteamVR only colour. The compositor is also D3D11, while FSR 3.1+ requires D3D12 or Vulkan.

Running DLSS or FSR 3 on motion that vrupscale estimates itself would mean feeding networks and heuristics data they weren't designed for, on hardware this project can't test. vrupscale therefore contains its own temporal upscaler, designed around exactly what the compositor can see.

## How the temporal mode works

For each eye, on every new app frame:

1. **Motion estimation (no help from the game).**
   - A luma pyramid feeds a coarse block search, then pyramidal Lucas–Kanade produces local motion on an 8×8-pixel grid.
   - A **robust affine fit** of that field (iteratively re-weighted least squares) gives a precise model of the head motion. Rotation moves the image almost affinely, and thousands of local estimates pin it down to about 0.01–0.1 px.
   - Each 4×4-pixel cell keeps the global model unless the model fails there: moving avatars, near objects during head translation, or a scene cut.
2. **Accumulation.**
   - Head micro-motion moves the scene by fractions of a pixel every frame. Registered precisely, those offsets act like the jitter a game would add on purpose.
   - Each output pixel blends the current frame's reconstruction with the reprojected history (Lanczos-3, or Catmull-Rom with **Sharp history** off), weighted by how close a real input sample lands to it.
   - History is colour-clipped against the current frame's neighbourhood (YCoCg variance box).
   - History is dropped where the motion doesn't explain the image (contrast-normalised residual).
3. **Moving things.** Where motion differs from the head-motion model, the upscaler searches for that thing's own motion and uses history there as far as **Moving objects** allows. Before it does, the motion it found is checked: the image there must pin it down (a plain edge or a stripe pattern fits many motions), neighbouring cells must agree on it, and it must continue last frame's motion. Motion that fails gets no history there, or the head's motion where that cell followed the head last frame. At 0 (`localTrust=0`), moving things get the spatial reconstruction instead: no ghosting or smearing on avatars, and no temporal gain there either.
4. **RCAS sharpening**, then the result goes to the compositor.
5. **Foveation.** Steps 1–4 run only for the centre region (**Temporal area**), plus a margin for the motion search; the luma pyramid that motion estimation reads is built only there too. Outside it, the output is a Catmull-Rom upscale of the current frame (bilinear with **Bilinear edges**), which costs a fraction as much. Through VR lenses the edges are blurrier and lens distortion compresses them anyway, so detail there matters least. In a band inside the region's edge the two blend smoothly, so there is no visible border. With **Follow gaze**, the region is placed around the gaze each new frame instead of in the centre. The history keeps its place in the picture when the region moves: the GPU tracks where the history it holds was made, and a repeated frame that is only repainted keeps the region there until the next new frame.

Everything runs on the GPU, with no CPU readbacks:
- **New-frame detection:** a frame fingerprint decides on the GPU whether the app delivered a new frame. When the compositor re-shows an old frame (reprojection, motion smoothing), the expensive passes are skipped via indirect dispatch.
- **Rotating texture pools:** a pool of eye textures shares one history per eye. When the game's render size changes, all of an eye's textures move to the new size and history together.


## Status: read this first

- **Tested:** cross-compiled and run against a real D3D11 implementation (DXVK on a software Vulkan GPU, under Wine) with a test harness that behaves like a compositor. The harness runs once drawing on the immediate context (153 checks) and once recording into command lists the way SteamVR's compositor does (155 checks); all pass. That includes the app's live channel, a foveation test (centre identical to whole-image temporal, no seam), SteamVR's pattern of drawing all 3 swapchain textures per eye every frame (one history update per new app frame, not per draw), and the situation in the 0.4.1 log: three textures per eye of different sizes with the game rendering into their top-left corner. The app itself was exercised against the running harness.
- **In real SteamVR:** 0.3.0 and 0.4.0 ran in SteamVR with VRChat on an RTX 3090 and made every mode slower than native, because every swapchain texture was upscaled on every draw. 0.4.1 fixed that and worked well, as long as SteamVR's resolution was not changed while VRChat ran. 0.4.2 fixes that case; its fix is tested here against a simulation of what the log showed, not yet in real SteamVR.
- **GPU cost:** measured on the RTX 3090 with 0.3.0, whole-image temporal cost about 0.11 ms per million output pixels (counting both eyes), roughly 2.8 ms per frame at 2756×2756 → 3555×3555, and ran inside the compositor's tight per-frame slot. In 0.4.0 at the default 60% area, the expensive per-pixel passes cover about a third of the image, motion estimation covers only that region, and several passes are cheaper. The estimate for that size is about 1.1–1.4 ms per frame, close to FSR 1, but it has not been measured on real hardware yet (the test GPU here is software). The status line in the app now shows the real number. **Sharp history** (off by default) makes the accumulation pass more expensive: on the software test GPU, temporal at the 60% area costs about a third more with it on. On a real GPU it is 36 cached texture reads per pixel instead of 5, roughly +0.2–0.3 ms at the sizes above (an estimate, not measured). The cost grows with the output's pixel count, so a higher preset at the same SteamVR resolution costs more: 2.00× has 2.4 times the output pixels of 1.30×.
- **Set VRChat's resolution** (0.4.9) is tested against a stand-in for `openvr_api.dll` that records the calls: VRChat starting, a preset change, VRChat closing and the app closing each set or put back SteamVR's global `supersampleScale` as they should, compensating for VRChat's own value. That SteamVR applies a global change to a running VRChat live is what OpenVR Dynamic Resolution relies on (it lists VRChat as working); not yet seen with vrupscale.
- **Eye tracking** is tested against a stand-in for Bigscreen's eye tracking client: the same shared memory block and data layout as Bigscreen's own VRCFaceTracking module reads (github.com/BigscreenVR/VRCFT-Beyond, October 2025). The tests check that the area follows the gaze in both eyes, that its history moves with it (no smearing or shifted picture), the dead zone, calibration and its failure on a restless gaze, the markers, and what happens with no tracker. Not yet seen with a real Beyond 2e: the gaze direction convention (x right, y up, taken from Bigscreen's module) and how well a one-point calibration lines up across the whole view.
- **Risk:** at worst, SteamVR fails to start or the image looks wrong. Click **Uninstall** in the app (SteamVR closed), or delete `dxgi.dll` from `SteamVR\bin\win64`.
- **Anti-cheat:** nothing is loaded into VRChat's process. EAC protects VRChat's process; how it treats a modified SteamVR install is a reasoned guess, not a guarantee.

### What changed in 0.6.0

- **Updates from GitHub.** vrupscale now lives at [github.com/Zakhazerd/vrupscale](https://github.com/Zakhazerd/vrupscale), and the app shows an **Update** button when a newer release is out: one click downloads, checks and installs it and restarts the app, and SteamVR's copy follows as soon as SteamVR is closed (see Updates under The app). This is the last zip to download by hand.
- The release zip is stored uncompressed (about 4.6 MB instead of 1.5 MB), so the app can unpack it without a decompressor; a `.sha256` checksum is published next to it.

### What changed in 0.5.1

- **Descriptions on hover.** Resting the pointer on a slider, switch, mode, preset or button in the app for a moment shows a longer description of it (greyed-out ones too). Only the app changed.

### What changed in 0.5.0

- **Moving things smear less.** Where something moves differently from your head (an avatar's arm), the motion found for it is now checked before its history is used (see step 3 under How the temporal mode works). On the test scene at the defaults: the moving object 16.40 → 19.60 dB, the area it just uncovered 25.87 → 27.67 dB (less ghosting), the background 23.61 → 24.09 dB, steadiness 27.00 → 27.40 dB. The centre of the image, which the moving object crosses, now beats FSR 1 instead of trailing it. New `[temporal]` keys for experimenting: `localCheck`, `localAgree`, `localAperture` (see the settings reference).
- **Preset switches without "Temporal paused".**
  - When the game's render size changes, every texture of an eye now takes the new size in the same moment, so the eye moves to a new history at once instead of falling back to FSR 1 until each texture had found the size on its own.
  - With **Set VRChat's resolution** on, the app tells vrupscale when it changes VRChat's resolution, and vrupscale looks for the new size every 100 ms for the next few seconds.
  - In the test (three textures per eye of different sizes, the default 500 ms observation window): a smaller picture was found after 5.5–6.2 s and held each eye on FSR 1 for 7–8 frames; now 1.4 s with the notice (5.0 s without), and no frame on FSR 1. A larger picture is taken as soon as each texture has drawn it: here a frame or two on FSR 1 (the test's frames come about ten times slower than a headset's); in a headset that should be a few milliseconds.
  - The app reports **Temporal paused** only if it lasts more than a second.
  - When the picture grows, its new part shows straight away (from the current frame alone), not a stale strip along the right and bottom edges until the larger size is confirmed.
- **Less GPU work outside the temporal area.** The luma pyramid (and its copy for the next frame) is built only around the temporal area instead of over the whole image, and the pass for the edges no longer launches work for the centre. On the software test GPU at Index size and the default 60% area, building the pyramid takes 30–40% less time, about 4% of temporal mode's GPU time. The picture is the same (checked against building it over the whole image, with the area fixed and following the gaze). `[temporal] workMargin` (default 128 input pixels) sets the margin.
- The app and the DLL keep the same layout (an older DLL ignores the new notice), but update both.

### What changed in 0.4.9

- **Set VRChat's resolution now applies live.** It changed VRChat's per-application value, which SteamVR only applies when VRChat starts (or while SteamVR's settings window is open). It now sets SteamVR's global resolution while VRChat runs, which SteamVR applies at once, and puts it back when VRChat or the app closes. The per-application value 0.4.7 and 0.4.8 changed is put back to what it was.

### What changed in 0.4.8

- **Eye tracking (Bigscreen Beyond 2e):** the temporal area can follow your gaze, with a look-straight-ahead calibration (**Calibrate**, or Ctrl+F8 in VR), **Clear calibration**, and **Show gaze** to check it lines up. See the Eye tracking card above.
- The Settings card's buttons moved into the SteamVR card.
- The app and the DLL talk with a new layout: update both (the app's **Install / Update**, with SteamVR closed).

### What changed in 0.4.7

- **Native (1×)** replaces Ultra Performance: full resolution, no upscaling; temporal anti-aliasing (or FSR 1's sharpening). Ultra Performance needed VRChat at 11%, below SteamVR's per-application slider. A saved 3.00× becomes Performance (2.00×).
- **Set VRChat's resolution:** the app can now set VRChat's render resolution in SteamVR to match the preset, through OpenVR while SteamVR runs and in SteamVR's settings file otherwise. Off by default; switching it off puts the old value back. Ctrl+F5 then switches both, like DLSS's presets.
- The status line and texture list say "at 1×" / "processed at its own size" instead of an upscale at Native.

### What changed in 0.4.6

- **Bilinear edges** (Temporal card, off by default): the part outside the temporal area gets a bilinear upscale instead of Catmull-Rom. On the test scene the centre is unchanged (to 0.000 dB) and the edges match a plain bilinear upscale exactly (23.51 dB, against 24.20 dB with Catmull-Rom). On the software test GPU, temporal at the 60% area cost 13–26% less with it on. On a real GPU the saving is probably smaller, because texture reads are cheap there; the app's GPU line shows the real figure.
- **Ctrl+F7** switches Bilinear edges on and off.

### What changed in 0.4.5

- **Scaling presets instead of the factor slider:** Ultra Quality 1.30×, Quality 1.50×, Balanced 1.72×, Performance 2.00×, Ultra Performance 3.00× (DLSS's). A factor the old slider left within 0.02 of a preset (like its default, 1.29) moves onto that preset; others show as Custom until you pick one. The DLL still takes any factor from 1 to 3 in the settings file.
- **Sharp history is off by default.** If you used 0.4.4, your settings file has it on: switch it off once (or press Ctrl+F6).
- **New shortcuts:** Ctrl+F5 next scaling preset, Ctrl+F6 Sharp history on/off.
- Temporal checked at every preset on the test scene (table below).

### What changed in 0.4.4

- **New defaults:** Sharpness 1.00, History strength 1.00, Moving objects 1.00, Motion tolerance 1.50. Settings you already saved are kept; **Restore defaults** applies these.
- **Sharp history** (on by default): the history is resampled with Lanczos-3 instead of Catmull-Rom. On the test scene: +0.6 dB overall, +1.4 dB on detail finer than the input resolution, and a steadier image, for some more GPU time.
- **History strength** now goes up to 2.00 and **Motion tolerance** to 3.00, for trying. The measurements say what to expect: past 1 history mostly softens the image.
- **Removed:** the fixed output height and the Reset history button. A saved fixed height is dropped and the upscale factor applies again.
- The self-test also measures steadiness: how much the error changes from one frame to the next (shimmer).

### What changed in 0.4.3

- **A new look for the app**, after Valve's Steam hardware pages: a grey sky over a kraft-cardboard ground, heavy rounded headlines (Rubik, built in), Steam blue for whatever is on, and a few characters standing on the ground's edge that show what vrupscale is doing. The window is wider and shorter, so it fits more screens, and it scrolls where it doesn't.
- **Unticking a texture in the app sticks.** In 0.4.2, unticking a texture SteamVR draws into an eye's half of the screen was undone a moment later: the rule that keeps an eye's swapchain textures upscaled together counted that texture's own earlier frames as "another upscaled texture". Now only other textures count, upscaled after the change. Unticking every size an eye uses leaves them all alone.
- The app's controls work from the keyboard (Tab, arrows, Space), and on Windows 11 the title bar takes the page's grey.

### What changed in 0.4.2

- **Changing SteamVR's resolution while the game runs no longer breaks temporal mode.**
  - When the resolution goes down, the game keeps its larger eye textures and renders into their top-left corner.
  - A slider drag can also leave each eye with three textures of different sizes. The 0.4.1 log had 5632, 6064 and 6084 for one eye.
  - 0.4.1 gave each size its own history. Past four histories, the rest fell back to FSR 1, so the eye alternated between temporal and FSR frames: that was the flicker.
  - Now vrupscale finds the part of each texture the game actually renders into, by watching which pixels change. An eye's textures share one history whenever they hold the same picture, whatever their size. It takes a few seconds after a resolution change.
- **Never alternating quality.** Sometimes an eye's textures can't share one history: while the new size is being found, or if a game really renders a different size into each. Then every frame of that eye gets FSR 1 until they can, and the app says **Temporal paused**. If that stays (more than 10 s), restart the game.
- **The app shows the resolution the game renders at,** not just the texture size, so it follows SteamVR's resolution in both directions.
- **Eye textures are marked and can't be unticked**, because unticking one of an eye's textures made that eye flicker between upscaled and not.
  - Sizes you unticked in 0.4.1 are listed even when not in use, and **Tick all** clears them.
  - An unticked size that turns out to be one of the game's eye textures is upscaled anyway and ticked again.
- Textures SteamVR stopped drawing drop off the list after a minute.
- The upscale factor and temporal area sliders rebuild once you stop moving them, not at every step of the arrow keys or mouse wheel.
- The app shows the right installed version (it said 0.3.0 since 0.3.0).

### What changed in 0.4.1

- **Each texture is upscaled only when its content changes.** SteamVR's compositor draws every texture of the app's swapchain (3 per eye) on every display frame. A new app frame lands in only one of them, and is shown for several display frames when the app runs below the refresh rate. 0.4.0 and earlier upscaled every texture on every draw. Now a cheap GPU fingerprint (one pixel per 8×8 block) decides first, in every mode, so the full upscale runs once per new app frame and eye.
- **An eye's swapchain textures share one temporal history.** Before, each got its own (and the fourth and later fell back to FSR 1), so each history only saw every third frame.
- The app's status shows new app frames per second and the GPU time per displayed frame.

### What changed in 0.4.0

- **Much cheaper temporal mode.** Foveation with a new **Temporal area** setting (default 60%). Motion estimation only covers that region and samples it sparsely for the global fit. The full-resolution luma copy is gone (one pass now goes straight to half resolution). History textures shrink to the region, and the Lanczos kernel no longer uses trigonometry. At area 100%, quality is the same as 0.3.0 (within 0.05 dB on the test scene).
- **GPU time is measured inside SteamVR.** Upscale work recorded into SteamVR's command lists now runs on the immediate context just before each command list executes, where it can be timed. The app shows GPU ms per frame, and `passTiming=1` gives a per-pass breakdown in the log. Work on a texture that the command list itself writes earlier, such as a copy, still runs in order inside the list.
- Uses about 10% less video memory per history at area 100%, and nearly 60% less at the default.
- The self-test's performance section also runs at 2756×2756 → 3555×3555 and compares whole-image and foveated temporal.

### What changed in 0.3.0

- **An app instead of an ini file and an install script.** `vrupscale.exe` installs and uninstalls, shows live what the compositor is doing, and changes settings while SteamVR runs. Changes that don't affect texture sizes (mode, sharpness, tint, temporal tuning) apply without rebuilding anything.
- **One settings file in a fixed place** (`%LOCALAPPDATA%\vrupscale\vrupscale.ini`). The old installer kept an existing `vrupscale.ini` in the SteamVR folder, so edits to the copy in the download folder never reached SteamVR.
- **Clearer reasons** in the log and the app for every texture that is left alone.

### Earlier changes

- **0.2.4:** SteamVR's compositor records its rendering into command lists (a D3D11 deferred context, executed later on the immediate context). vrupscale now substitutes eye textures there and records the upscale into the same command list. The self-test runs every check in that mode too.
- **0.2.3:** hooks every variant of the D3D11 methods the compositor can switch to, and logs what the compositor does.
- **0.2.2:** a cheaper temporal path (8×8 motion grid, lighter global fit), a realistic performance test with a per-pass breakdown, and self-test fixes for NVIDIA.

### Measured quality (test scene)

The test scene is a procedural VR-like scene with thin lines, tiny text, a zone plate, a grating above the input Nyquist limit, sub-pixel head-like motion, and an independently moving object. It is upscaled 400×440 → 520×572 (factor 1.3) for 24 frames and both eyes, and compared with the same scene rendered at the output resolution (4×4 supersampled). Values are PSNR against that ground truth; higher is better. **Steadiness** compares each frame's error with the previous frame's at the same scene point: lower shimmer scores higher.

| Region | FSR 1 | Temporal (defaults, area 100%) | Temporal, Moving objects 0 | Temporal, Sharp history on |
|---|---|---|---|---|
| Steady background | 23.12 dB | **24.09 dB** | 24.00 dB | 24.79 dB |
| Pattern above the input Nyquist limit | 19.35 dB | **19.97 dB** | | 21.40 dB |
| Steadiness (background) | 24.13 dB | **27.40 dB** | | 27.93 dB |
| Area the moving object just uncovered (ghosting test) | 27.06 dB | **27.67 dB** | 28.00 dB | |
| Moving object | 20.16 dB | 19.60 dB (smears a little) | **20.84 dB** | |
| Still scene (no motion: nothing to gain) | 23.17 dB | 23.42 dB | | |

The defaults gain most in steadiness (less shimmer); the fine-detail numbers are where Sharp history shows up, but the difference is small enough that it is hard to see in the headset. With **Moving objects** on, the moving object now comes within 0.6 dB of FSR 1 and the area behind it beats FSR 1 (0.4.9: −3.8 dB and −1.2 dB), so turning it off buys less than before (+1.2 dB on the object).

At each scaling preset (defaults, the same sequence rendered at the matching output size):

| Preset | Steady background, FSR 1 → temporal | Pattern above Nyquist | Steadiness |
|---|---|---|---|
| Native 1× | 30.47 → 34.26 dB | 26.24 → 29.91 dB | 31.55 → 34.72 dB |
| Ultra Quality 1.30× | 23.12 → 24.09 dB | 19.35 → 19.97 dB | 24.13 → 27.40 dB |
| Quality 1.50× | 22.33 → 23.46 dB | 18.36 → 19.37 dB | 23.92 → 27.96 dB |
| Balanced 1.72× | 21.58 → 22.60 dB | 17.62 → 18.73 dB | 24.03 → 28.35 dB |
| Performance 2.00× | 21.06 → 21.64 dB | 17.14 → 17.81 dB | 24.50 → 29.25 dB |

Absolute values fall as the preset rises because there is less input per output pixel; at Native, "FSR 1" is FSR 1 at 1× (sharpening only). Temporal stays ahead of FSR 1 at every preset, and its steadiness gain grows with the factor.

How far history can be pushed (Lanczos-3 history, the sequence above with 40 frames; measured with 0.4.4):

| History strength | 0.5 | 1 | 2 | 4 |
|---|---|---|---|---|
| Steady background | 24.67 dB | 24.64 dB | 24.19 dB | 23.47 dB |
| Pattern above the input Nyquist limit | 21.21 dB | 21.23 dB | 20.56 dB | 19.45 dB |
| Steadiness | 26.92 dB | **27.28 dB** | 27.22 dB | 26.91 dB |

Above 1 the history gets softer faster than it gets steadier, and the same holds with exact motion (so it is the history's resampling, not motion estimation) and with calmer head motion. Motion tolerance above 1.5 and a wider colour clamp (`clipGamma`) changed nothing measurable.

With the default area (60%), on the same sequence:

| Part of the image | FSR 1 | Bilinear | Temporal, area 100% | Temporal, area 60% | Area 60%, Bilinear edges |
|---|---|---|---|---|---|
| Centre (history) | 21.93 dB | | 22.58 dB | 22.56 dB | 22.56 dB |
| Blend band | 22.00 dB | 20.76 dB | 22.67 dB | 22.21 dB | 21.75 dB |
| Edges (spatial) | 24.02 dB | 23.51 dB | 25.09 dB | 24.20 dB | 23.51 dB |

The moving object crosses the centre on this scene. Until 0.4.9 its smearing pulled the centre's score down to about FSR 1's; with 0.5.0's checks on its motion the centre gains like the rest.

This test content is deliberately hard: it is heavily aliased and has sub-pixel detail everywhere. Real VRChat frames are mip-mapped and MSAA'd, which usually makes motion estimation easier.

## Self-test (optional, no SteamVR involved)

`selftest\selftest.cmd` runs the full test harness against your real GPU and D3D11, including real cross-process shared textures. It runs twice, the second time recording into command lists like SteamVR, and takes a few minutes. Expect `0 failed` for each part (153 and 155 checks). The results (GPU name, quality tables, timings, pass/fail) are saved to `selftest\selftest_results.txt`; that is the file to send back.

The performance section measures the GPU cost at two realistic sizes (Index at 60%, 1561×1735 ×2 → 2014×2238, and a high-resolution headset at 50%, 2756×2756 ×2 → 3555×3555) while a stand-in for the game keeps the GPU busy. It reports what bilinear, FSR 1 and temporal (whole image, at the default 60% area, and at 60% with Bilinear edges) add per frame for both eyes, and breaks the 60% case down per pass. Compare the added cost against your frame budget (11.1 ms at 90 Hz).

## If something goes wrong

**SteamVR won't start or crashes:** close it, click **Uninstall** in the app, and send the log (**Open log**).

**The status says "SteamVR is running without vrupscale":** SteamVR was started before you installed, or an older vrupscale (before 0.3) is loaded. Restart SteamVR.

**Nothing is upscaled:** the texture list says why for each texture. If VRChat's eye textures are listed as unticked, tick them (or **Tick all**). If they aren't listed at all, send the log.

**"Temporal paused":** an eye's textures have been on different histories for more than a second, so that eye gets FSR 1, consistently, never alternating. Since 0.5.0 a change of SteamVR's resolution shouldn't cause it (an eye's textures take a new size together). If it shows and stays, restart the game (it then allocates new textures of one size) and send the log.

**An overlay or menu gets upscaled (tinted magenta):** untick its size in the texture list.

**The update failed:** the app says why, and nothing was changed (a half-finished update is rolled back). Try again later, or download the zip from the [releases page](https://github.com/Zakhazerd/vrupscale/releases/latest) and extract it over the old folder. If the app sits in a folder Windows protects (Program Files), it can't replace its own files: move it to a folder of your own.

**Ghosting or smearing:** on moving avatars, lower **Moving objects** (0 turns history off on them). Elsewhere lower **History strength** (e.g. 0.50) or **Motion tolerance** (e.g. 0.90). Note when it happens (turning, walking, avatars moving) and send the log.

**Frame drops that weren't there with FSR 1:** check the GPU time in the app's status. Make sure **Sharp history** is off, switch **Bilinear edges** on, lower **Temporal area** (at 40% the temporal part covers 16% of the pixels instead of 36%), check that VRChat's SteamVR resolution matches the scaling preset (the output should be about 100%, not more), or switch to FSR 1. The compositor's work has to fit in the short time before each display refresh. If it doesn't, SteamVR drops frames even when the GPU has time to spare elsewhere. For a breakdown, set `passTiming=1` under `[general]` in the settings file and send the log.

**The image is cropped to one corner or zoomed in:** the compositor reads that texture with integer texel fetches instead of UVs. Untick its size and send the log.

**Streaming headsets (Virtual Desktop, ALVR, Steam Link):** untested. Their drivers may composite layers themselves, in which case vrupscale never sees the eye textures. Virtual Desktop has its own headset-side upscaler (SGSR) anyway.

## Settings file reference

`%LOCALAPPDATA%\vrupscale\vrupscale.ini`. The app writes the keys it shows; SteamVR re-reads the file within a second of a change, so hand edits work too. Keys the app doesn't show:
- `[general] active=0` – the DLL does nothing except forward `dxgi.dll` (read when SteamVR starts).
- `[general] logLevel` (0 errors, 1 info, 2 verbose), `statsIntervalSec` (stats line every N seconds, 0 off), `passTiming=1` (per-pass GPU times of the temporal mode in the stats line).
- `[match]` rules for what counts as an eye texture: `minWidth`, `minHeight`, `minAspect`, `maxAspect`, `requireShared`, `onlySizes` / `excludeSizes` (comma-separated `WxH`), `maxArraySize`.
- `[upscale] upscaleFactor` is the **Scaling** preset's factor. Any value from 1.00 to 3.00 works; one that isn't a preset shows as Custom in the app, and one within 0.02 of a preset moves onto it when the app starts (3.00, the old Ultra Performance, becomes 2.00).
- `[eyes]`: `followGaze`, `showGaze` (the app's switches), `offsetX` / `offsetY` (the calibration: tangents of the angle taken off the tracker's reading, x right, y up), `projection` (each eye's field of view as SteamVR reports it: tangents left, right, top, bottom, left eye then right; written by the app), `deadzone` (default 0.02: how far, as a fraction of the width, the gaze must move from the area's centre before it follows).
- `[app] checkUpdates=0` stops the app from asking GitHub for updates. `updateDll=1` is set by an update while SteamVR's copy of the DLL still waits to be updated.
- `[app] gameResolution` is **Set VRChat's resolution**; `globalResBefore` is SteamVR's global resolution before vrupscale changed it (scale, and 1 for Custom or 0 for Auto), present only while it is changed and put back from there.
- `[temporal] temporalArea` is the app's **Temporal area** as a fraction (0.3–1.0); `sharpHistory` is **Sharp history**; `bilinearEdges` is **Bilinear edges**; `maxWeight`, `localTrust` and `residualTol` are **History strength**, **Moving objects** and **Motion tolerance** (by hand, `maxWeight` goes up to 64).
- `[temporal] maxStreams` (default 4): maximum number of histories. Each costs about 38 MB at Index 60% with the default area (82 MB at area 1.0).
- `[temporal] detectContent` (default 1): find the part of each eye texture the game renders into. `probeWindowMs` (default 500) is how often that is checked (every 100 ms for a few seconds after the app changed VRChat's resolution).
- `[temporal] localCheck` (default 47): which checks motion that differs from the head's must pass before history is used on it, as a sum of bits: 1 the image pins the motion down, 2 neighbouring cells agree, 4 it continues last frame's motion. Where the checks fail: 16 no history; 32 the head's motion, but only where that cell followed the head last frame (otherwise no history); neither: the head's motion. 8: no history where neither the head's motion nor one of its own fits. 0 is 0.4.9's behaviour. `localAgree` (default 1.0 input px): how close two motions must be to agree. `localAperture` (default 0.3): how clearly the image must pin the motion down (ratio of the weakest to the strongest direction of its gradients).
- `[temporal] workMargin` (default 128, at least 64): input pixels around the temporal area where the luma pyramid for motion estimation is built.

## Limits

- **Recovering detail depends on head motion.** The upscaler can only recover detail when the scene moves by fractions of a pixel between frames. Your head is never perfectly still, but with the headset lying on a desk, temporal looks like a good spatial upscaler and no better.
- **Moving objects can smear.** With **Moving objects** on (the default), avatars and near objects during head translation keep history wherever their own motion is found; where it is found wrongly they smear a little. At 0 they get the spatial upscale instead.
- **It runs after the app's post-processing and UI.** Overlays drawn into the eye texture (menus, nameplates) are reconstructed like everything else.
- **Finding the render size takes a moment** after SteamVR's resolution changes (vrupscale watches which pixels change): a fraction of a second when the app made the change, about 5 seconds for a smaller size when it didn't (a larger one is taken at once). Until then, the temporal area is placed for the old size. A completely still scene says nothing about it, so the last size found is kept.
- **Without eye tracking, the temporal region is centred in the eye image.** On most headsets the lens centre is close to it. On headsets with strongly canted or asymmetric displays (some Pimax models), the sharpest part of the lens may sit off-centre; a larger area covers that.
- **Eye tracking: one-point calibration.** It corrects a constant offset (where "straight ahead" is), not a tracker that reads angles too large or too small. If the ring drifts off when you look far to the side, the area is large enough to cover small errors there. Only Bigscreen's own eye tracking is read; other trackers (EyeTrackVR, Babble) don't publish this data.
- **Extra VRAM:** about 38 MB per eye history at Index 60% (default area), plus one upscaled copy per eye texture.
- **Compositor updates:** SteamVR updates can change the compositor. vrupscale logs hook failures and otherwise stays out of the way.

## Building from source

Requirements: MinGW-w64 (Linux, or MSYS2 on Windows) and Python 3.

```
./build.sh          # -> build/dxgi.dll (imports only KERNEL32 and msvcrt) and build/vrupscale.exe
tools/package.sh    # -> build/vrupscale-<version>.zip and the source zip
```

The compute shaders are precompiled into `src/gen/shader_blobs.h`. The app's fonts (`app/fonts/`) and icon are generated too: `tools/fonts.py` builds the fonts from Rubik in `third_party/rubik` (needs `pip install fonttools ttfautohint-py`), `tools/icon.py` draws the icon (needs Pillow). After editing `shaders/*.hlsl`, regenerate it with `tools/shaders.sh`, which needs Microsoft's `d3dcompiler_47.dll` (runs under Wine on Linux).

Tests: `test/fake_compositor.cpp` (+ `test/temporal_tests.inc`) is the functional and quality harness and the self-test. Run it with `VRUPSCALE_FORCE=1` and our `dxgi.dll` next to the exe.
- `VRUPSCALE_TEST_DEFERRED=1` records the "compositor" draws into command lists, like SteamVR.
- `VRUPSCALE_TEST_PROBE=1` exercises the probe-device fallback.
- `VRUPSCALE_TEST_QUALITY_ONLY=1` / `VRUPSCALE_TEST_PERF_ONLY=1` run one section (`VRUPSCALE_PERF_FRAMES=N` timed frames per mode); `VRUPSCALE_TEST_ONLY=switch` or `=gaze` runs one test.
- `VRUPSCALE_TEST_SERVE=<seconds>` keeps drawing like a running compositor, for trying the app against it (set `VRUPSCALE_CONTROL=Local\vrupscale.control.v1` so the app finds it).

The app also has command-line modes for scripting: `--install [--force]`, `--uninstall`, `--steamvr <folder>`, `--set key=value`, `--status-file <file>`, `--settings <ini>`, `--channel <name>`; and self-tests: `--test-update` (the updater's zip reader, hash and version comparison), `--check-update` (asks GitHub, or `VRUPSCALE_UPDATE_URL`, and reports), `--test-game-resolution`, each with `--report <file>`.

Releases: set `VU_VERSION_STR` in `src/common.h`, add a `### What changed in <version>` section to this README (it becomes the release's notes, which the app's Update button shows), commit, and push a tag `v<version>`. GitHub Actions (`.github/workflows/release.yml`) then builds everything with MinGW-w64 on Ubuntu and publishes the release: `vrupscale-<version>.zip`, its `.sha256`, and the source zip. The app's updater looks for exactly those names.

Layout:
- `src/exports.S`, `src/proxy.cpp`: dxgi forwarding
- `src/hooks.cpp`: MinHook detours (every method-body variant)
- `src/upscaler.cpp`: classification, proxies, spatial path, housekeeping, command-list handling
- `src/temporal.cpp`: per-eye histories and the temporal pass sequence
- `src/control.cpp`, `src/control_shared.h`: the live channel to the app
- `shaders/upscale.hlsl`: FSR 1 / bilinear; `shaders/temporal.hlsl`: the temporal passes
- `app/`: the Windows app (`ui.cpp` draws it: shapes with GDI+, text with GDI; `update.cpp` updates it from GitHub; `gameres.cpp` sets VRChat's resolution)

## Licenses

vrupscale: MIT. Bundled: MinHook (BSD-2-Clause, Tsuda Kageyu), AMD FidelityFX Super Resolution 1 (MIT, Advanced Micro Devices; the temporal path uses FSR 1's RCAS), the Rubik typeface in the app (SIL Open Font License 1.1, The Rubik Project Authors; subset, hinted, with an added arrow), and Valve's `openvr_api.dll` (OpenVR SDK, BSD-3-Clause), which the app uses to change SteamVR's settings. See `LICENSES/`. The app's look is inspired by Valve's Steam hardware pages; its illustrations are original, and vrupscale is not affiliated with Valve.
