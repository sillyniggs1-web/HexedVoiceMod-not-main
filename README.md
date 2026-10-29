# HexedVoiceMod

[![build-hexedvoicemod](../../actions/workflows/build.yml/badge.svg)](../../actions/workflows/build.yml)

**A real-time voice changer for SA-MP Nezuko on Android: voice presets, pitch and formant control, Auto-Tune and Magic Chords, noise suppression, a studio-style dynamics section and a full effect rack, all running on your microphone before it reaches the game.**

Version 1.0.0 - library API 9 - `libHexedVoiceMod.so` (armeabi-v7a, 32-bit) plus a MoonLoader menu (`/hvm`).

> **Honest status:** the audio engine is covered by automated host tests (see [Running the host tests](#running-the-host-tests)). The hook into the Nezuko build, CPU load on real phones, latency numbers and the menu layout on different screens can only be confirmed on a device. See "Not verified" in [CHANGELOG.md](CHANGELOG.md).

## Features

* **Voice presets** - 45 ready-made voices (Normal, Deep, Child, Robot, Demon, Radio, Cave, Auto-Tune, Choir, ...) with favourites and search.
* **Pitch and formant** - +-12 semitones and a separate formant control (RubberBand 4.0 "High" engine, with a built-in "Lite" shifter as fallback). "Preserve formants" keeps the voice natural while shifting.
* **Auto-Tune and Magic Chords** - YIN pitch tracking, 8 scales, key by hand or automatic, retune time, amount, lag compensation (lookahead) and vibrato preservation. Up to 3 extra chord voices that follow the snapped note; the optional formant-preserving chord voice keeps them from sounding like chipmunks.
* **Noise suppression** - Classic (Wiener STFT) or AI (RNNoise, optional), keyboard-click guard.
* **Dynamics** - input conditioning (DC blocker + 75 Hz high-pass), gate with hysteresis / hold / soft expander and 5 ms lookahead, compressor with soft knee and auto makeup, de-esser, and a lookahead limiter that never goes over the ceiling.
* **Effect rack** - vocoder (20 bands, carrier can follow your pitch or snap to the scale), drive (oversampled), bit crusher, EQ, robot, tremolo, chorus / flanger / vibrato, echo (with damping), reverb (8-line FDN with pre-delay and damping).
* **Hear myself** - monitor your processed voice with a servo that keeps the delay small and never drops audio.
* **Compare switch** - one button (or `/hvm ab`) flips between the V1 sound and the pre-1.0 sound, with a cross-fade and without touching your settings, so you can hear exactly what 1.0 changed.

## Requirements

* SA-MP **Nezuko** client for Android (armeabi-v7a, 32-bit)
* **AML / Modloader** (AndroidModLoader) to load the `.so`
* **MoonLoader or MonetLoader** with **mimgui** for the menu
* A headset is strongly recommended when "Hear myself" is on (feedback!)

## Install

1. Build the library (see [Build on GitHub](#build-on-github)) or take `libHexedVoiceMod.so` from the workflow artifact.
2. Copy **`libHexedVoiceMod.so`** into Nezuko's AML **mods** folder.
3. **Delete the old `libvoicefx.so`** if it is there. Two copies would hook BASS twice; the new library refuses to hook and the menu shows a red warning.
4. Copy the contents of `menu/` into your MoonLoader folder: `HexedVoiceMod_Menu.lua`, `lib/`, `HEXED/Icons/VoiceFX/` and `HEXED/Fonts/` (the icon and font folders keep their old names on purpose).
5. Start the game, open the chat and type **`/hvm`**. The Status page must say **HexedVoiceMod V1.0**.
6. Run **`/hvm icons`** once if the voice icons are missing.

## Commands

`/hvm` and `/hexedvoice` open the menu. `/vfx` and `/voicefx` still work. `/hvm help` prints the list in chat.

| Command | What it does |
|---|---|
| `/hvm` , `/hvm menu` | open / close the menu |
| `/hvm on` , `/hvm off` | voice changer on / off |
| `/hvm preset <name>` | apply a voice preset (and switch on) |
| `/hvm pitch <-12..12>` , `/hvm formant <0.5..2>` | pitch in semitones, formant factor |
| `/hvm gain <dB>` , `/hvm mix <0..1>` | mic gain, dry/wet |
| `/hvm gate <-80..-20>` | noise gate threshold (-80 = off) |
| `/hvm preserve on\|off` , `/hvm robot on\|off` , `/hvm robothz <Hz>` | formant preserve, robot |
| `/hvm hear on\|off` , `/hvm hearvol <0..2>` | hear myself, monitor volume |
| `/hvm noise on\|off\|ai\|classic\|<0..1>` | noise suppression on/off, engine, strength |
| `/hvm tune on\|off\|key <C..B\|auto>\|scale <0..7>` | Auto-Tune |
| `/hvm tune look <0..60>` , `/hvm tune vibrato <0..1>` | tune lookahead (ms), keep vibrato |
| `/hvm chord <0..8>` | Magic Chords style |
| `/hvm quality auto\|high\|lite` | pitch engine |
| `/hvm comp on\|off\|auto` , `/hvm deess on\|off` | compressor (auto = auto makeup), de-esser |
| `/hvm ab [v1\|v8]` | Compare: no argument toggles, `v8` = pre-1.0 sound, `v1` = new sound |
| `/hvm set <name> <value>` | any other setting (e.g. `tunetime`, `revsize`, `echodamp`, `voccarrier`); `/hvm set` lists the names |
| `/hvm status` | engine, CPU, per-stage CPU, dynamics, latency, hear-myself state |
| `/hvm hook` , `/hvm save` , `/hvm icons` | retry the hook, save settings, reload icons |

## Menu overview

* **Voices** - preset grid with category chips (All, High, Deep, FX, Filter, Space, Tune), search, **Favorites** page.
* **Effects** - tabs: **Clean** (noise suppression, engine selector, click guard), **Voice**, **Tune** (key, scale, retune, amount, lookahead, keep vibrato), **Chords** (styles, level, width, keep formants), **Tone**, **Modulate**, **Crunch**, **Space** (Echo and Reverb pages), **Robot** (robot, vocoder, carrier), **Dynamics** (Gate, Compressor, De-esser, Limiter with live meters).
* **Status** - library version, hook state, pitch engine, CPU and dropout meters, CPU by stage, latency, hear-myself queue, old-library warning.
* **Settings** - menu size, icon folder, file paths, save.
* **Voice panel (right side)** - on/off, favourite, Noise and Tune quick switches, gain, pitch, formant, mix, gate, Preserve, Robot, monitor volume and the **Compare** button.

## Signal chain

```
mic (BASS record thread, 48 kHz mono 16-bit)
  |
  v
DC blocker + 75 Hz high-pass (always on)
  v
noise suppression  (Classic Wiener STFT  |  AI RNNoise)
  v
gate  (hysteresis, hold, soft expander, 5 ms lookahead)
  v
mic gain -> compressor  (soft knee, auto makeup)
  v
pitch / formant  (RubberBand High | Lite)  +  Auto-Tune  +  chord voices
  v
de-esser  (split band 4-9 kHz)
  v
effect rack:  vocoder -> drive -> bit crusher -> EQ -> robot -> tremolo
              -> chorus / flanger / vibrato -> echo -> reverb
  v
lookahead limiter  (3 ms, ceiling -0.9 dB)
  v
back into the BASS recording  (+ optional hear-myself monitor)
```

Compare (`legacy_mode`) bypasses everything added in 1.0 and runs the previous gate, limiter, reverb, vocoder, chord voices, drive and echo instead.

## Performance tips

* **Pitch engine**: *Auto* starts with High (RubberBand) and drops to Lite if the phone cannot keep up; it retries High every ~20 s and stays only if the load is under about 55 %. *High* never falls back, *Lite* is the cheapest. Switch with `/hvm quality auto|high|lite`.
* Watch the **CPU** and **dropout** meters on the Status page. Underruns or "slow calls" mean the phone is overloaded: try Lite, switch the AI noise engine back to Classic, turn off the vocoder or reverb.
* **CPU by stage** (Status page) shows which part costs the most: input chain, pitch analysis, pitch engine, chords, effects.
* Added delay: +8 ms from the gate lookahead and limiter, +10.7 ms (Classic) or +20 ms (AI) from noise suppression while it is on, plus the pitch engine and any tune lookahead you set. The Status page shows the total.
* Hear myself adds the monitor queue (about 50 ms) plus your phone's output buffer.

## Build on GitHub

1. Create a repository and upload **everything in this project** (keep `jni/`, `.github/`, `test/`, `menu/`, `project.properties`).
2. Open **Actions**. The workflow `build-hexedvoicemod` runs on every push (or press *Run workflow*). It first runs the naming check and the host tests, then builds.
3. **RubberBand 4.0.0** (GPL-2.0-or-later) and **RNNoise v0.1.1** (BSD-3-Clause) are **downloaded by the workflow** and are **not stored in this repository**. If RNNoise cannot be fetched, the build still succeeds without the AI noise engine.
4. Open the finished run -> **Artifacts** -> **HexedVoiceMod** -> `libHexedVoiceMod.so`.

### Local build

With the Android NDK: fetch the two libraries, then run ndk-build.

```
mkdir -p jni/rubberband-src jni/rnnoise-src
curl -fL https://codeload.github.com/breakfastquay/rubberband/tar.gz/refs/tags/v4.0.0 | tar xz --strip-components=1 -C jni/rubberband-src
curl -fL https://codeload.github.com/xiph/rnnoise/tar.gz/refs/tags/v0.1.1 | tar xz --strip-components=1 -C jni/rnnoise-src
$ANDROID_NDK_HOME/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
```

Without `jni/rubberband-src` the library builds with the Lite pitch shifter only; without `jni/rnnoise-src` it builds without AI noise suppression. The RNNoise build with ndk-build for armeabi-v7a has not been confirmed outside the CI workflow.

## Files and logs

| File | Purpose |
|---|---|
| `/storage/emulated/0/HexedVoiceMod.cfg` | library settings |
| `/storage/emulated/0/HexedVoiceMod_log.txt` | log (hook result, errors) |
| `config/HexedVoiceMod_settings.cfg` (MoonLoader) | menu settings |
| `config/HexedVoiceMod_favorites.txt` (MoonLoader) | favourite voices |

## Upgrading from the VoiceFX v8 beta

1. Put `libHexedVoiceMod.so` in the mods folder and **delete `libvoicefx.so`**.
2. Replace `VoiceFX_Menu.lua` with `HexedVoiceMod_Menu.lua` (delete the old menu file, otherwise both register `/vfx`).
3. Start the game. Your old settings are imported once from `voicefx.cfg`, `VoiceFX_settings.cfg` and `VoiceFX_favorites.txt`; the old files are left in place.
4. Use `/hvm ab` to compare the new sound with the old one.

## Troubleshooting

* **Menu says "hook failed" / audio is not processed** - the BASS hook did not install. Send `/storage/emulated/0/HexedVoiceMod_log.txt` and the game build number. `/hvm hook` retries.
* **Red warning "Old libvoicefx.so found in the mods folder - delete it"** - remove `libvoicefx.so` and restart the game. While it is present the new library deliberately does not hook BASS.
* **"Engine not found"** - `libHexedVoiceMod.so` is not in the mods folder or was not loaded. The menu also accepts older names containing `voicefx`.
* **"Update libHexedVoiceMod.so"** - the loaded library is older than API 9.
* **No icons** - check that `HEXED/Icons/VoiceFX/` is inside MoonLoader, then run `/hvm icons` and reopen the menu.
* **Menu does not open** - make sure MoonLoader and mimgui are installed and `HexedVoiceMod_Menu.lua` and `lib/fAwesome6_solid.lua` are in place; try `/vfx`. Check the MoonLoader log for `[HexedVoiceMod] menu error`.
* **Crackling / dropouts** - see the performance tips above.
* **Feedback howl with Hear myself** - use headphones.

## Known limits

* Mono, 48 kHz microphone only. The AI noise engine needs 48 kHz and falls back to Classic otherwise.
* Auto-Tune needs a clear single voice (turn Noise on). A new note is corrected after roughly 40-50 ms unless you add lookahead (which adds delay).
* The formant-preserving chord voice works only for voices from about 96 Hz to 1.1 kHz; deeper voices fall back to the normal chord voice.
* RNNoise needs about a second to settle on steady hiss.
* The reverb in 1.0 sounds different from the old one by design (use Compare).
* Everything that needs a device (hook on the Nezuko build, CPU of each stage, real latency) is listed under "Not verified" in the changelog.

## Running the host tests

The engine is tested on a PC without Android. From the project root:

```
# RubberBand stand-in + Lite-only, without RNNoise
g++ -std=c++17 -O2 -ffast-math -fno-finite-math-only -DVFX_NO_RNNOISE -I test/fake test/host_test.cpp -o host_test && ./host_test
g++ -std=c++17 -O2 -ffast-math -fno-finite-math-only -DVFX_NO_RNNOISE -DVFX_NO_RUBBERBAND test/host_test.cpp -o host_test_lite && ./host_test_lite
```

The workflow also runs both with RNNoise (it builds the RNNoise C sources with plain gcc first). The last line of each run must be `ALL TESTS PASSED (0 failed)`.

## Credits and license

HexedVoiceMod is by **brruham** / HEXED Studio.

The project is licensed under the **GNU General Public License, version 2 or (at your option) any later version** (GPL-2.0-or-later), see [LICENSE](LICENSE). The built library links RubberBand, which has the same license. Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**RubberBand and RNNoise are downloaded by the workflow and are not stored in this repository.**
