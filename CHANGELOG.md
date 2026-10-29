# Changelog

## V1.0

First stable release: **HexedVoiceMod 1.0.0**, library API 9. It replaces the VoiceFX v8 beta (below); the VoiceFX v1..v8 numbering ends here. The Compare switch (`legacy_mode`, `/hvm ab`) plays the pre-1.0 sound at any time without touching your settings.

### Sound at default settings: what is different from v8
Everything new is off or transparent by default, with these exceptions that are always active in V1 (use Compare to hear the old behaviour):
* 75 Hz high-pass and DC blocker on the input (inaudible on speech, removes rumble).
* Gate with lookahead / hysteresis / soft expander (only acts when the gate threshold is above -80 dB).
* Lookahead limiter instead of the tanh knee (+3 ms).
* New reverb algorithm (FDN) and 20-band vocoder, 2x oversampled drive, new adaptive Auto-Tune analysis window. These only act when the corresponding effect is used.
* Noise-suppression delay is now counted in the reported latency.

### Release and renaming
* Product **HexedVoiceMod 1.0.0**; `VFX_API_VERSION` = 9 (separate from the product version).
* Library `libHexedVoiceMod.so` (`MYMOD(com.hexed.voicemod, HexedVoiceMod, 1.0.0, brruham)`, logger tag `HexedVoiceMod`). Workflow `build-hexedvoicemod`, artifact `HexedVoiceMod`.
* **Double-load guard**: if the old `libvoicefx.so` is loaded in the process (dlopen NOLOAD, then a /proc/self/maps scan) the BASS hook is NOT installed, the log gets a clear line and the menu shows the red warning "Old libvoicefx.so found in the mods folder - delete it".
* Paths: `/storage/emulated/0/HexedVoiceMod.cfg`, `/storage/emulated/0/HexedVoiceMod_log.txt`. One-time import of `voicefx.cfg`; menu files `config/HexedVoiceMod_settings.cfg` and `config/HexedVoiceMod_favorites.txt` are imported once from the `VoiceFX_*` files. Old files are never deleted.
* Menu `HexedVoiceMod_Menu.lua`, `script_name('HexedVoiceMod')`, version 1.0, chat prefix `[HexedVoiceMod]`. Commands `/hvm`, `/hexedvoice` added; `/vfx` and `/voicefx` still work. The loader tries `libHexedVoiceMod.so`, then scans loaded paths for `hexedvoicemod` / `voicefx`. The menu needs API >= 9 and shows "Update libHexedVoiceMod.so" otherwise.
* Icon and font folders unchanged (`HEXED/Icons/VoiceFX/`, `HEXED/Fonts/`). Internal names (`vfx_*`, `P_*`, `voicefx_dsp.h`, `vc_api`) unchanged.
* CI: the naming check fails the build if "beta" or "VoiceFX v8" appears anywhere except `CHANGELOG.md` and the upgrade section of `README.md`.
* Documentation: new README (player-oriented), LICENSE (GPL-2.0-or-later), THIRD_PARTY_NOTICES.md; BUILD.md is now a pointer to the README.

### Compare switch
* Param `legacy_mode` (id 43, 0/1, never saved). 1 = the pre-1.0 sound for every stage listed below, WITHOUT changing any stored value. Cross-faded over 15 ms (no click). Menu: "Compare: V1 / v8" button on the voice panel, `/hvm ab [v1|v8]`. `vfx_stat(20)` = legacy active.
* Restores: no conditioning, old gate, tanh limiter, no lookahead, no compressor / de-esser, Auto-Tune without lookahead / vibrato handling / adaptive window, old hear-myself drop behaviour, Classic noise suppression, LitePitch chord voices, plain tanh drive, Freeverb, 12-band vocoder with fixed carrier, echo without damping.

### Signal conditioning and dynamics
* **Input conditioning** (always on): DC blocker (~10 Hz) + 75 Hz Butterworth high-pass before noise suppression and the tuner; primed on the first sample (no start-up step).
* **Gate**: hysteresis, hold, soft 3:1 expander with a floor, 5 ms lookahead (the detector sees the un-delayed signal). New `gate_hold_ms` (80), `gate_range_db` (-60), `gate_hyst_db` (4). Threshold is still `gate_db` (-80 = off).
* **Compressor** after gate + gain, before the pitch stage: feed-forward, 6 dB soft knee, ~10 ms mean-square detector. `comp_on` (0), `comp_thresh_db`, `comp_ratio`, `comp_attack_ms`, `comp_release_ms`, `comp_makeup_db`, `comp_auto` (auto makeup = half of the reduction a full-scale signal would get).
* **De-esser** after pitch + chords, before the FX rack: split-band, only the band around `deess_freq` (4-9 kHz) is reduced when it dominates. `deess_on` (0), `deess_amt`, `deess_freq`.
* **Limiter**: 3 ms lookahead limiter replaces the tanh knee; never exceeds `limit_ceil_db` (default -0.9 dB), smooth 60 ms release; latency included in the reported latency.
* Menu: Effects > **Dynamics** tab (Gate / Compressor / De-esser / Limiter, live meters). Commands `/hvm comp on|off|auto`, `/hvm deess on|off`. Presets never touch the dynamics settings.
* **Bug fix** (also affected the old engine): the echo read position could round to exactly the buffer length and read one float past the end of the buffer (heap garbage, rare click). Fixed in both V1 and legacy_mode (the only intentional difference from the old code).
* Extra latency: **+8 ms** in V1 (5 ms gate lookahead + 3 ms limiter), always; 0 in legacy_mode.

### Auto-Tune timing
* **Lag compensation**: `tune_lookahead_ms` (id 58, 0..60, default 0). The pitch analysis keeps seeing the live signal; the lead pitch engine and chord voices are fed from a copy delayed by this amount, and the dry path is delayed equally, so Mix, chords and dry stay aligned. Changes slew without a click.
* **Adaptive YIN window**: about two periods of the last detected pitch, minimum 20 ms, maximum the old 40 ms. New stat `vfx_statf(13)` = current window in ms.
* **Vibrato preservation**: `tune_keep_vibrato` (id 59, 0..1, default 0). The slow pitch component (~2 Hz smoother) is snapped to the scale note; this fraction of the fast (4-8 Hz) part is passed through. A real note change (more than 1.2 st away) re-anchors immediately; the note choice follows the slow part, so vibrato does not flip the target note.
* Octave-error protection unchanged (single-hop octave jumps rejected, 3 hops for a real jump). Output does not depend on the BASS chunk size.
* Menu: Tune page sliders "Tune lookahead" and "Keep vibrato"; `/hvm tune look <ms>`, `/hvm tune vibrato <0..1>`. Presets do not touch these.
* Extra latency: exactly `tune_lookahead_ms` while the pitch engine runs (0 by default).
* Host numbers (synthetic voice): correction ready ~50-60 ms after a note change with the fixed window, ~40-50 ms adaptive; best vibrato flattening with the Lite engine at 20-25 ms lookahead (swing 0.95 st at 0 ms, 0.43 st at 20-25 ms). The best value for RubberBand has to be found by ear.

### Hear myself
* **Servo instead of dropping**: no chunk is dropped any more. The queue is kept at about 50 ms (aim = max(50, chunk/2 + 25) ms, 40-120 ms) by nudging `BASS_ATTRIB_FREQ` by at most +-1 % (smoothed, largest step 0.05 %). A queue above ~200 ms (output stall) resets the stream once.
* Playback starts when the target level is queued (~60 ms for 20 ms chunks, instead of the fixed 80 ms).
* Stats: `vfx_statf(14)` measured monitor latency (ms), `vfx_statf(15)` rate offset (%), `vfx_stat(21)` hard resets, `vfx_stat(22)` underruns. Status page and `/hvm status` show them.
* The decisions live in a small pure class (`MonitorServo`); `main.cpp` only executes them. Switching to Compare restores the old drop behaviour and nominal rate at once.
* Extra latency: none added; the delay you hear is the measured queue (~50 ms) + device output buffer (unknown) + engine delay.

### Quality upgrades
* **AI noise suppression**: `ns_engine` (id 60: 0 Classic, 1 AI). RNNoise v0.1.1 on 480-sample frames through an input ring; strength = wet/dry of the AI result. Engine switch runs both for 13 ms then cross-fades 10 ms. Measured delay **960 samples = 20 ms** (classic 512 samples = 10.7 ms; both now included in the reported latency, `vfx_statf(16)`). Needs 48 kHz; at other rates or without RNNoise (`vfx_stat(23)` = 0) it falls back to Classic. Compare always uses Classic. RNNoise needs about a second to settle on steady hiss. `/hvm noise ai|classic`.
* **Chord voices with formants**: `chord_formant` (id 61, 0..1, default 0). Pitch-synchronous overlap-add shifter (grains of two periods around pitch marks, re-spaced by T / ratio) driven by the tuner's period, blended with the Lite voice, only while the period is reliable (~96 Hz to 1.1 kHz; deeper voices fall back). Same latency as the Lite voice (770 samples). Host: pitch within a few cents, formant peak stays near its original place where the Lite voice moves it.
* **Drive**: 2x oversampling (4th-order filters) around a rational tanh approximation; aliased harmonics more than 6 dB lower at heavy drive, level within 1.5 dB of the old drive.
* **Reverb**: 8-line feedback delay network (Hadamard feedback, per-line damping, modulated delays, pre-delay). New `rev_predelay_ms` (0..100, default 0), `rev_damp` (0..1, default 0.5); `rev_size` = decay time (RT60 0.4-4 s), `rev_mix` = amount; level within +-1 dB of the old reverb at default damping. **This changes the default reverb sound.**
* **Echo**: `echo_damp` (0..1) low-pass and `echo_hp_hz` (0..800, 0 = off) high-pass in the feedback loop; identical to the old echo at the defaults.
* **Vocoder**: 20 bands (was 12); `voc_carrier` (id 66: 0 fixed pitch, 1 follow my pitch, 2 snap to the scale), driven by the tuner. Host: fixed 45.0, follow 55.4 (sung 55.4), snap 55.0 (MIDI numbers).
* Menu: Clean tab engine selector; Chords tab "Keep formants"; Effects > Space split into Echo and Reverb pages; Robot tab carrier selector.
* Build: `Android.mk` builds RNNoise as a separate static C library (it refuses `-ffast-math`) when `jni/rnnoise-src/src/denoise.c` exists. Sources: denoise.c rnn.c rnn_data.c pitch.c kiss_fft.c celt_lpc.c; flags `-O3 -DNDEBUG -fno-fast-math`; include paths `include` and `src`. Compiled and run with gcc on x86 only; **not** compiled with ndk-build for armeabi-v7a here. The fetch step is optional (`continue-on-error`) and the library builds with `-DVFX_NO_RNNOISE` without it.
* Extra latency: RNNoise 20 ms when the AI engine is selected; the rest adds none.

### CPU headroom
* **No per-sample sinf / cosf**: one 4096-step sine table (linear interpolation, error under 6e-7, 20 KB) serves the Lite shifter's Hann windows and the robot, tremolo, chorus, flanger and vibrato LFOs; the bit crusher's `exp2f` moved out of the sample loop. Differences to the old code are about -100 dB (robot, tremolo, Lite shifter, crusher within 1 LSB; modulated delays within 16 LSB at their worst sample, 0.1 LSB rms), so Compare still reproduces the old sound. Remaining per-sample libm: the legacy drive / limiter `tanhf` (only while Compare is on or cross-fading).
* Host micro-benchmark (x86, glibc): Lite shifter 15.7 -> 10.6 ns/sample, robot + tremolo + chorus 37.3 -> 24.4 ns/sample. The gain on the phone was not measured.
* **Automatic return from Lite**: while Auto quality is on Lite after an overload, the engine retries RubberBand every ~20 s (audio time) for a 4 s probe, keeps it only if the load after a 0.5 s warm-up stays at or below 55 %; a probe that overloads (above 90 % or two slow calls in a row) is cut short. Stats `vfx_stat(24)` probes, `vfx_stat(25)` recoveries. High and Lite never probe. Cost of a failed probe: a few seconds of RubberBand load every 20 s while the phone struggles.
* **Per-stage CPU meters** (`vfx_statf(17..21)`, Status page "CPU by stage"): input chain, pitch analysis, pitch engine, chords, effects + limiter, in % of real time.
* Test hook `Engine::simLoadRb` (0 in production); the engine's pause-reset threshold is a member (`gapMs`, 120 ms) so a stalled CI machine cannot make chunk-size tests flaky.

### New stats (vfx_stat / vfx_statf)
`vfx_stat`: 17 product version x100, 18 old libvoicefx.so found, 19 settings imported, 20 legacy active, 21 monitor hard resets, 22 monitor underruns, 23 RNNoise built in, 24 recovery probes, 25 recoveries. `vfx_statf`: 9..12 gate / compressor / de-esser / limiter meters, 13 analysis window ms, 14 monitor latency ms, 15 monitor rate offset %, 16 noise-suppression delay ms, 17..21 CPU by stage.

### Known limits
Mono 48 kHz only; AI noise needs 48 kHz; Auto-Tune needs a clean single voice; formant chord voice only for ~96 Hz-1.1 kHz voices; RNNoise settles after about a second; the default reverb sounds different from the old one; total latency in V1 is higher than before (see the numbers above, shown on the Status page).

### Verified by the host tests (x86, g++ 13)
Engine plumbing sample-exact with a RubberBand stand-in and in Lite-only mode; DC blocker, gate hysteresis, compressor curve, limiter ceiling, de-esser; Compare reproduces the pre-1.0 engine bit-close (reference engine compiled next to the new one); chunk-size independence (1, 64, 480, 1024 and odd sizes); Auto-Tune lag, glides, vibrato, octave jumps; monitor servo logic; formant chord pitch accuracy; oscillator / LUT equivalence; config import and migration. RNNoise tests run in the CI workflow (the offline final pass of this package could only run the two modes without RNNoise).

### Not verified (needs a real device or the real toolchain)
* The BASS hook on the Nezuko build, the double-load guard with a real second library, loading through AML.
* `main.cpp` was only parsed against the AML headers with stand-in JNI headers; no real ndk-build for armeabi-v7a was run (neither for the library nor for RNNoise, which was compiled with gcc on x86 only).
* RubberBand 4.0.0 integration was exercised with a stand-in (and Lite) in the host tests; the real RubberBand engine is only built by the workflow.
* CPU load of every stage and of RNNoise on phones, the Auto-lite recovery thresholds on a real overloaded phone, the per-stage meters' accuracy.
* All latency numbers (they are computed / host-measured; the real hear-myself delay depends on the phone's output buffer), the monitor servo behaviour with real BASS chunk sizes.
* Best `tune_lookahead_ms` for the RubberBand engine; how the AI noise engine and the compressor sound on real microphones and in game voice chat.
* Menu layout and touch handling on different screen sizes; icons and font loading on devices.
* Licenses of the AML headers, the fAwesome6 table and the Arial font (see THIRD_PARTY_NOTICES.md).
* No on-device test reports from the earlier phases are recorded in this package yet; send them and this list will be updated.

### Upgrading from the VoiceFX v8 beta
1. Put `libHexedVoiceMod.so` in the mods folder and **delete `libvoicefx.so`**.
2. Replace `VoiceFX_Menu.lua` with `HexedVoiceMod_Menu.lua` (delete the old menu file, or both menus would register `/vfx`).
3. Start the game: old settings are imported automatically (old files are left in place).

## v8 beta
Last VoiceFX beta (library API 8, menu 8.0): Auto-Tune styles and Magic Chords, noise suppression, effect rack, RubberBand 4.0 LiveShifter with Lite fallback.
