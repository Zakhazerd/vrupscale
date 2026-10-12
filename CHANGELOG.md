# Change log

What changed in each version of vrupscale, newest first.

## 0.6.4

- The app starts with SteamVR, minimized, and closes when SteamVR does. It adds itself to SteamVR's startup apps the first time SteamVR runs while it is open. **Start with SteamVR** switches this off.
- SteamVR's resolution is no longer lowered while vrupscale isn't installed in SteamVR.
- **Sharpness area**: sharpening can reach past the Temporal area, up to the whole picture. It starts at the Temporal area's defaults, 60% (40% while following your gaze).

## 0.6.3

- The app always sets SteamVR's resolution while VRChat runs; the option is gone. The resolution you set in SteamVR, VRChat's own included, is what you see, and VRChat renders the preset's share of it.
- Fixed: raising VRChat's own resolution made the app lower SteamVR's further, so you still saw the old resolution.
- Per-pass GPU times are always in the log, alongside the app's GPU figure; the `passTiming` setting is gone.
- Fixed: the app said its GPU time "needs passTiming=0" whenever nothing had been timed for a few seconds.

## 0.6.2

- A **Change log** button shows this list. After an update, what's new since your last version is marked.
- FSR 1 and Bilinear are much faster when the game renders smaller than its textures, as with **Set VRChat's resolution**.
- Fixed: `passTiming` per-pass times read several times too low.
- Fixed: an eye could stay on FSR 1 for 30 seconds after an earlier, brief **Temporal paused**.

## 0.6.1

- FSR 1 mode uses the **Temporal area** too: FSR 1 inside it, a fast spatial upscale outside. **Bilinear edges**, **Debug tint**, **Follow gaze** and calibration now work in FSR 1 mode.
- The area has its own value while **Follow gaze** is on: 40% by default (60% otherwise).

## 0.6.0

- The app updates itself from GitHub: an **Update** button appears when a new version is out.

## 0.5.1

- Resting the pointer on a control shows a longer description of it.

## 0.5.0

- Less smearing on moving objects such as avatars.
- Switching scaling presets no longer causes **Temporal paused**, and a new render size is picked up faster.
- Less GPU work outside the temporal area.

## 0.4.9

- **Set VRChat's resolution** takes effect immediately while VRChat runs.

## 0.4.8

- Eye tracking (Bigscreen Beyond 2e): the temporal area can follow your gaze. Includes **Calibrate** (Ctrl+F8 in VR), **Clear calibration** and **Show gaze**.
- The Settings card's buttons moved into the SteamVR card.

## 0.4.7

- **Native (1×)** replaces Ultra Performance: no upscaling, just temporal anti-aliasing. A saved Ultra Performance becomes Performance.
- **Set VRChat's resolution** (off by default): VRChat renders at the resolution the preset needs.

## 0.4.6

- **Bilinear edges** (off by default): a cheaper, softer upscale outside the temporal area. Ctrl+F7 in VR.

## 0.4.5

- Scaling presets replace the factor slider.
- **Sharp history** is off by default. If you used 0.4.4, switch it off once (Ctrl+F6).
- New shortcuts: Ctrl+F5 next scaling preset, Ctrl+F6 Sharp history.

## 0.4.4

- New defaults: Sharpness 1.00, History strength 1.00, Moving objects 1.00, Motion tolerance 1.50. **Restore defaults** applies them.
- **Sharp history**: keeps finer detail, for some GPU time.
- History strength goes up to 2.00, Motion tolerance to 3.00.
- Removed the fixed output height and the Reset history button.

## 0.4.3

- A new look for the app; it also works from the keyboard and scrolls on small screens.
- Fixed: unticking a texture didn't stick.

## 0.4.2

- Fixed: changing SteamVR's resolution while the game ran made temporal mode flicker.
- When an eye can't use temporal mode, it stays on FSR 1 and the app says **Temporal paused**, instead of alternating.
- The app shows the game's render resolution, and marks eye textures, which can't be unticked.
- Fixed: the app showed the wrong installed version.

## 0.4.1

- Fixed: every mode was slower than native. Each new frame is now upscaled once, not on every draw.
- The app shows new frames per second and GPU time per frame.

## 0.4.0

- Much cheaper temporal mode: only the **Temporal area** (default 60%) gets temporal detail.
- The app shows the GPU time vrupscale takes inside SteamVR.
- Uses less video memory.

## 0.3.0

- An app replaces the ini file and install script: install, uninstall, live status and live settings.
- Settings are kept in one place: `%LOCALAPPDATA%\vrupscale\vrupscale.ini`.
- The log and the app say why each texture is left alone.

## 0.2.4

- Works with SteamVR's command-list rendering.

## 0.2.3

- Covers every variant of the D3D11 calls the compositor uses.

## 0.2.2

- A cheaper temporal path, and self-test fixes for NVIDIA.
