// SPDX-License-Identifier: GPL-2.0-or-later
// HexedVoiceMod DSP core (header-only, no Android / AML dependency, so it can also be unit-tested on a PC).
// Included once by main.cpp, which contains the AML glue (hook, monitor stream, entry points).
#pragma once
// Voice changer + filter/effect rack: hooks libBASS BASS_RecordStart and runs a BASS DSP on the mic channel.
// Pitch engines: RubberBand LiveShifter (optional) or the built-in Lite shifter. Auto-Tune / chords use the YIN Tuner,
// noise suppression is a 512-point Wiener STFT. All parameters are plain floats behind vfx_get / vfx_set.
// Library API version 9 (VFX_API_VERSION) is separate from the product version (vfx_stat(17) = version x 100).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <chrono>
#include <algorithm>
#ifndef VFX_NO_RNNOISE
#include "rnnoise.h"          // RNNoise (BSD-3-Clause): fetched by the workflow into jni/rnnoise-src; if it is missing the build defines VFX_NO_RNNOISE
#endif
// RubberBand is optional: define VFX_NO_RUBBERBAND (Android.mk does it when jni/rubberband-src is missing) to build with the
// built-in Lite pitch shifter only (pitch works, formant control and the high-quality engine do not).
#ifndef VFX_NO_RUBBERBAND
#include "rubberband/RubberBandLiveShifter.h"
#endif


#define VFX_EXPORT extern "C" __attribute__((visibility("default")))
#define VFX_API_VERSION 9
#define HVM_PRODUCT_VERSION_X100 100      // HexedVoiceMod 1.0.0
// (paths can be overridden before including this header, the host tests do it)
#ifndef LOG_PATH
#define LOG_PATH "/storage/emulated/0/HexedVoiceMod_log.txt"
#endif
#ifndef CFG_PATH
#define CFG_PATH "/storage/emulated/0/HexedVoiceMod.cfg"
#endif
#ifndef OLD_CFG_PATH
#define OLD_CFG_PATH "/storage/emulated/0/voicefx.cfg"   // read once if the new cfg does not exist yet (never deleted)
#endif

// ------------------------------------------------------------------ logging
#ifndef VFX_HOST_TEST
void vfx_platform_log(const char* msg);
#endif
static std::atomic<int> g_logBudget{400};
static void vlog(const char* fmt, ...)
{
    if (g_logBudget.fetch_sub(1) <= 0) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
#ifndef VFX_HOST_TEST
    vfx_platform_log(buf);                       // defined in main.cpp: AML logger + /storage/emulated/0/HexedVoiceMod_log.txt
#else
    fprintf(stderr, "[VFX] %s\n", buf);
#endif
}

// ------------------------------------------------------------------ parameters (written by menu, read by DSP thread)
// ids 0..10 never change (old menus keep working); new ones are only appended before P_COUNT.
enum {
    P_ENABLED, P_PITCH, P_FORMANT, P_PRESERVE, P_GAIN_DB, P_GATE_DB, P_MIX, P_ROBOT, P_ROBOT_HZ, P_MON_VOL, P_MONITOR,
    P_HP_HZ, P_LP_HZ, P_BASS_DB, P_TREBLE_DB, P_DRIVE, P_BITS, P_DOWN, P_TREM_HZ, P_TREM_DEPTH,
    P_MOD_MODE, P_MOD_RATE, P_MOD_DEPTH, P_ECHO_MS, P_ECHO_FB, P_ECHO_MIX, P_REV_SIZE, P_REV_MIX,
    P_ROBOT_AMT, P_VOC_MODE, P_VOC_HZ, P_QUALITY, P_NS_ON, P_NS_AMT, P_NS_CLICK,
    P_TUNE_ON, P_TUNE_KEY, P_TUNE_SCALE, P_TUNE_TIME, P_TUNE_AMT, P_CHORD, P_CHORD_MIX, P_CHORD_SPREAD,
    // ---- Phase 1 (signal conditioning + dynamics); legacy_mode is never saved
    P_LEGACY, P_GATE_HOLD, P_GATE_RANGE, P_GATE_HYST, P_COMP_ON, P_COMP_THRESH, P_COMP_RATIO, P_COMP_ATTACK, P_COMP_RELEASE, P_COMP_MAKEUP,
    P_COMP_AUTO, P_DEESS_ON, P_DEESS_AMT, P_DEESS_FREQ, P_LIM_CEIL,
    // ---- Phase 2 (Auto-Tune timing)
    P_TUNE_LOOK, P_TUNE_VIB,
    // ---- Phase 4 (quality upgrades)
    P_NS_ENGINE, P_CHORD_FORMANT, P_REV_PREDELAY, P_REV_DAMP, P_ECHO_DAMP, P_ECHO_HP, P_VOC_CARRIER, P_COUNT
};
struct ParDef { const char* name; float def, lo, hi; int kind; };   // kind 0 float, 1 switch (0/1), 2 integer
static const ParDef kPar[P_COUNT] = {
    {"enabled",   0.f,   0.f,    1.f,   1}, {"pitch",    1.f,   0.25f,  4.f,   0}, {"formant",   1.f,  0.25f, 4.f,  0},
    {"preserve",  0.f,   0.f,    1.f,   1}, {"gain_db",  0.f,  -24.f,  24.f,   0}, {"gate_db", -80.f, -80.f, -20.f, 0},
    {"mix",       1.f,   0.f,    1.f,   0}, {"robot",    0.f,   0.f,    1.f,   1}, {"robot_hz",  80.f, 20.f, 500.f, 0},
    {"mon_vol",   1.f,   0.f,    2.f,   0}, {"monitor",  0.f,   0.f,    1.f,   1},   // monitor is never saved (feedback safety)
    {"hp_hz",     20.f,  20.f,   2000.f,0}, {"lp_hz",    20000.f, 1000.f, 20000.f, 0}, {"bass_db", 0.f, -12.f, 12.f, 0},
    {"treble_db", 0.f,  -12.f,   12.f,  0}, {"drive",    0.f,   0.f,    1.f,   0}, {"bits",     16.f,  4.f,   16.f, 0},
    {"down",      1.f,   1.f,    32.f,  0}, {"trem_hz",  5.f,   0.5f,   20.f,  0}, {"trem_depth",0.f,  0.f,   1.f,  0},
    {"mod_mode",  0.f,   0.f,    3.f,   2}, {"mod_rate", 1.f,   0.1f,   10.f,  0}, {"mod_depth", 0.5f, 0.f,   1.f,  0},
    {"echo_ms",   300.f, 40.f,   1000.f,0}, {"echo_fb",  0.35f, 0.f,    0.85f, 0}, {"echo_mix",  0.f,  0.f,   1.f,  0},
    {"rev_size",  0.5f,  0.f,    1.f,   0}, {"rev_mix",  0.f,   0.f,    1.f,   0}, {"robot_amt", 1.f,  0.f,   1.f,  0},
    {"voc_mode",  0.f,   0.f,    2.f,   2}, {"voc_hz",   110.f, 50.f,   400.f, 0}, {"quality",   0.f,  0.f,   2.f,  2},
    {"ns_on",     0.f,   0.f,    1.f,   1}, {"ns_amt",   0.6f,  0.f,    1.f,   0}, {"ns_click", 0.5f,  0.f,   1.f,  0},
    {"tune_on",   0.f,   0.f,    1.f,   1}, {"tune_key", 12.f,  0.f,    12.f,  2}, {"tune_scale", 1.f, 0.f,   7.f,  2},
    {"tune_time", 40.f,  0.f,    400.f, 0}, {"tune_amt", 0.9f,  0.f,    1.f,   0}, {"chord",    0.f,   0.f,   8.f,  2},
    {"chord_mix", 0.65f, 0.f,    1.f,   0}, {"chord_spread", 8.f, 0.f,  30.f,  0},
    {"legacy_mode", 0.f, 0.f,    1.f,   1},                                                // 1 = previous (pre-1.0) sound for A/B listening; never saved
    {"gate_hold_ms", 80.f, 0.f,  500.f, 0}, {"gate_range_db", -60.f, -80.f, -6.f, 0}, {"gate_hyst_db", 4.f, 0.f, 20.f, 0},
    {"comp_on",  0.f,   0.f,    1.f,   1}, {"comp_thresh_db", -18.f, -60.f, 0.f, 0}, {"comp_ratio", 3.f, 1.f,  20.f, 0},
    {"comp_attack_ms", 10.f, 1.f, 100.f, 0}, {"comp_release_ms", 150.f, 20.f, 1000.f, 0}, {"comp_makeup_db", 0.f, 0.f, 24.f, 0},
    {"comp_auto", 1.f,  0.f,    1.f,   1},
    {"deess_on", 0.f,   0.f,    1.f,   1}, {"deess_amt", 0.5f, 0.f,  1.f,   0}, {"deess_freq", 6500.f, 4000.f, 9000.f, 0},
    {"limit_ceil_db", -0.9f, -12.f, 0.f, 0},
    {"tune_lookahead_ms", 0.f, 0.f, 60.f, 0}, {"tune_keep_vibrato", 0.f, 0.f, 1.f, 0},   // 0 / 0 = previous Auto-Tune timing
    {"ns_engine", 0.f, 0.f, 1.f, 2}, {"chord_formant", 0.f, 0.f, 1.f, 0},                 // 0 = Classic noise suppression / old chord voices
    {"rev_predelay_ms", 0.f, 0.f, 100.f, 0}, {"rev_damp", 0.5f, 0.f, 1.f, 0},
    {"echo_damp", 0.f, 0.f, 1.f, 0}, {"echo_hp_hz", 0.f, 0.f, 800.f, 0}, {"voc_carrier", 0.f, 0.f, 2.f, 2},
};
static std::atomic<float> g_par[P_COUNT];
static std::atomic<int>   g_qualityEpoch{0};
static void params_default() { for (int i = 0; i < P_COUNT; i++) g_par[i] = kPar[i].def; }
static struct ParamInit { ParamInit() { params_default(); } } g_paramInit;   // sane defaults even before OnModLoad
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float sanitize(int id, float v)
{
    if (id < 0 || id >= P_COUNT) return 0.f;
    const ParDef& d = kPar[id];
    if (!(v == v) || v > 1e12f || v < -1e12f) v = d.def;              // NaN / inf
    if (d.kind == 1) return v >= 0.5f ? 1.f : 0.f;
    if (d.kind == 2) v = floorf(v + 0.5f);
    return clampf(v, d.lo, d.hi);
}

// ------------------------------------------------------------------ stats
static std::atomic<int>   g_monState{0};       // 0 off, 1 playing, -1 stream creation failed
static std::atomic<int>   g_hookState{0};      // 0 none, 1 installed, -1 failed
static std::atomic<int>   g_oldLib{0};         // 1 = the old libvoicefx.so is loaded in this process (double-load guard, set in main.cpp)
static std::atomic<int>   g_cfgImported{0};    // 1 = settings were imported from the old voicefx.cfg
static std::atomic<int>   g_recStarts{0}, g_dspCalls{0}, g_lastBytes{0}, g_rate{0}, g_fmtOK{0}, g_block{0};
static std::atomic<int>   g_underruns{0}, g_gapResets{0}, g_overruns{0}, g_engineMode{0}, g_autoLite{0};
static std::atomic<float> g_peakIn{0.f}, g_peakOut{0.f}, g_latMs{0.f}, g_load{0.f}, g_loadPeak{0.f}, g_nsFloor{-90.f}, g_nsRed{0.f}, g_tuneDet{-1.f}, g_tuneTgt{-1.f};
static std::atomic<int>   g_tuneRoot{0}, g_tuneVoiced{0};
static std::atomic<float> g_gateG{0.f}, g_compGR{0.f}, g_deessGR{0.f}, g_limGR{0.f}, g_stagePct[5] = { {0.f}, {0.f}, {0.f}, {0.f}, {0.f} }, g_tuneWinMs{0.f}, g_monQueueMs{0.f}, g_monRatioPct{0.f}, g_nsLatMs{0.f};
static std::atomic<int>   g_monResets{0}, g_monUnder{0}, g_probes{0}, g_recoveries{0};     // hear-myself servo: hard resets / underruns (queue ran empty)   // dynamics meters (dB, 0 = no action)

// ------------------------------------------------------------------ DSP building blocks
namespace {
const float kPi = 3.14159265358979f, kTwoPi = 6.28318530717959f;
static inline float fz(float v) { return (v > -1e-20f && v < 1e-20f) ? 0.f : v; }   // flush denormals
static inline float wrap01(float v) { return v >= 1.f ? v - 1.f : v; }

struct Ring {                         // float FIFO (overwrites oldest when full)
    std::vector<float> b; size_t r = 0, w = 0, n = 0;
    void init(size_t cap) { b.assign(cap, 0.f); r = w = n = 0; }
    void clear() { r = w = n = 0; }
    void push(float v) { if (n == b.size()) { r = (r + 1) % b.size(); n--; } b[w] = v; w = (w + 1) % b.size(); n++; }
    float pop() { if (!n) return 0.f; float v = b[r]; r = (r + 1) % b.size(); n--; return v; }
};

struct Biquad {                       // RBJ cookbook, transposed direct form II
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float proc(float x) { float y = b0 * x + z1; z1 = fz(b1 * x - a1 * y + z2); z2 = fz(b2 * x - a2 * y); return y; }
    void clear() { z1 = z2 = 0.f; }
    void set(double B0, double B1, double B2, double A0, double A1, double A2)
    { b0 = (float)(B0 / A0); b1 = (float)(B1 / A0); b2 = (float)(B2 / A0); a1 = (float)(A1 / A0); a2 = (float)(A2 / A0); }
    static double fclamp(double fs, double f) { return f < 10.0 ? 10.0 : (f > fs * 0.45 ? fs * 0.45 : f); }
    void lowpass(double fs, double f, double q)  { double w = 2 * kPi * fclamp(fs, f) / fs, c = cos(w), al = sin(w) / (2 * q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al); }
    void highpass(double fs, double f, double q) { double w = 2 * kPi * fclamp(fs, f) / fs, c = cos(w), al = sin(w) / (2 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al); }
    void bandpass(double fs, double f, double q) { double w = 2 * kPi * fclamp(fs, f) / fs, c = cos(w), al = sin(w) / (2 * q);
        set(al, 0, -al, 1 + al, -2 * c, 1 - al); }
    void lowshelf(double fs, double f, double dB) { double A = pow(10.0, dB / 40), w = 2 * kPi * fclamp(fs, f) / fs, c = cos(w);
        double be = 2 * sqrt(A) * (sin(w) / 2 * sqrt(2.0));
        set(A * ((A + 1) - (A - 1) * c + be), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - be),
            (A + 1) + (A - 1) * c + be, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - be); }
    void highshelf(double fs, double f, double dB) { double A = pow(10.0, dB / 40), w = 2 * kPi * fclamp(fs, f) / fs, c = cos(w);
        double be = 2 * sqrt(A) * (sin(w) / 2 * sqrt(2.0));
        set(A * ((A + 1) + (A - 1) * c + be), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - be),
            (A + 1) - (A - 1) * c + be, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - be); }
};

// Light built-in pitch shifter: two crossfaded read heads on a delay line. Cheap (no FFT), ~15 ms latency, no formant control.
// Pitch-adaptive: when the caller knows the voice period T0 (setPeriod) the grain length is 2*k*T0, so the two heads read the same
// waveform phase and add up coherently - without it a voice splits into two side lines ~30 Hz apart (the usual "warble").

// Sine table (4096 steps + a quarter period for cosine, linear interpolation: error < 4e-7, 20 KB). Replaces the per-sample sinf / cosf of the
// LFOs (robot, tremolo, chorus / flanger / vibrato) and of the Lite grain window. Argument = phase in turns, must be in [0, 1).
struct SinLut {
    static const int N = 4096, Q = N / 4;
    float t[N + Q + 2];
    SinLut() { for (int i = 0; i < N + Q + 2; i++) t[i] = (float)sin(6.283185307179586 * (double)i / (double)N); }
    inline float sin01(float u) const { const float x = u * (float)N; const int i = (int)x; const float f = x - (float)i; return t[i] + (t[i + 1] - t[i]) * f; }
    inline float cos01(float u) const { const float x = u * (float)N; const int i = (int)x; const float f = x - (float)i; return t[i + Q] + (t[i + Q + 1] - t[i + Q]) * f; }
};
static const SinLut g_lut;

struct LitePitch {
    std::vector<float> buf; size_t mask = 0, w = 0; float W = 1536.f, Wdef = 1536.f, Wtgt = 1536.f, d1 = 0.f, ratio = 1.f, fs = 48000.f;
    void init(float f) { fs = f; size_t n = 1; while (n < (size_t)(fs * 0.12f)) n <<= 1; buf.assign(n, 0.f); mask = n - 1; w = 0;
                         Wdef = floorf(fs * 0.032f); W = Wtgt = Wdef; d1 = 0.f; }
    void reset() { std::fill(buf.begin(), buf.end(), 0.f); w = 0; d1 = 0.f; W = Wtgt = Wdef; }
    void setPeriod(float t0) {                                  // t0 = voice period in samples; <= 0 = unknown
        if (t0 <= 8.f) { Wtgt = Wdef; return; }
        int k = std::max(1, (int)floorf(0.016f * fs / t0 + 0.5f)); Wtgt = clampf(2.f * (float)k * t0, 0.010f * fs, 0.055f * fs);
    }
    size_t latency() const { return (size_t)(W * 0.5f) + 2; }
    inline float rd(float d) const { float rp = (float)w - d; long i0 = (long)floorf(rp); float fr = rp - (float)i0;
        return buf[(size_t)i0 & mask] * (1.f - fr) + buf[(size_t)(i0 + 1) & mask] * fr; }
    float proc(float x) {
        buf[w & mask] = x; w++;
        W += (Wtgt - W) * 0.002f;
        d1 += 1.f - ratio;
        if (d1 >= W) d1 -= W; else if (d1 < 0.f) d1 += W;
        float d2 = d1 + 0.5f * W; if (d2 >= W) d2 -= W;
        float g1 = 0.5f - 0.5f * g_lut.cos01(d1 / W), g2 = 0.5f - 0.5f * g_lut.cos01(d2 / W);          // Hann grain windows (table, no cosf)
        return g1 * rd(2.f + d1) + g2 * rd(2.f + d2);
    }
};

struct Comb { std::vector<float> b; size_t i = 0; float store = 0.f;
    void init(size_t n) { b.assign(n < 1 ? 1 : n, 0.f); i = 0; store = 0.f; }
    inline float proc(float in, float fb, float d1, float d2) { float o = b[i]; store = fz(o * d2 + store * d1); b[i] = fz(in + store * fb); if (++i >= b.size()) i = 0; return o; } };
struct Allpass { std::vector<float> b; size_t i = 0;
    void init(size_t n) { b.assign(n < 1 ? 1 : n, 0.f); i = 0; }
    inline float proc(float in) { float bo = b[i]; float o = bo - in; b[i] = fz(in + bo * 0.5f); if (++i >= b.size()) i = 0; return o; } };


// ================================================================== Phase 1: conditioning + dynamics blocks
// All of them avoid per-sample transcendental functions (log / exp run at control rate, per call or every 16 samples).

// DC blocker (one pole, ~10 Hz) followed by a 75 Hz Butterworth high-pass. The first sample primes the blocker so a DC offset
// (or a mic that starts with a bias) does not produce a step.
struct InputCond {
    Biquad hp; float xp = 0.f, yp = 0.f, R = 0.9987f; bool primed = false;
    void init(float fs) { R = 1.f - kTwoPi * 10.f / fs; hp.highpass(fs, 75.0, 0.70710678); hp.clear(); primed = false; }
    void unprime() { primed = false; }
    inline float proc(float x) {
        if (!primed) { primed = true; xp = x; yp = 0.f; hp.clear(); }
        float y = x - xp + R * yp; xp = x; yp = fz(y);
        return hp.proc(y);
    }
};

// Gate with hysteresis, hold and a soft 3:1 expander below the open threshold. The detector looks at the un-delayed signal while the
// audio is delayed by the lookahead line, so word onsets are already open when they arrive.
struct GateV1 {
    float env = 0.f, g = 1.f; bool open = false; int hold = 0;
    float openThr = 0.f, closeThr = 0.f, rangeLin = 0.001f, atk = 0.01f, rel = 0.001f; int holdN = 0;
    void reset() { env = 0.f; g = 1.f; open = false; hold = 0; }
    void set(float thrLin, float hystDb, float rangeDb, float holdMs, float fs) {
        openThr = thrLin; closeThr = thrLin * powf(10.f, -hystDb / 20.f); rangeLin = powf(10.f, rangeDb / 20.f);
        holdN = (int)(holdMs * 0.001f * fs); atk = 1.f - expf(-1.f / (0.002f * fs)); rel = 1.f - expf(-1.f / (0.08f * fs));
    }
    inline float proc(float x) {
        float ax = fabsf(x); env = ax > env ? ax : env * 0.9995f;
        if (env > openThr) { open = true; hold = holdN; }
        else if (open) { if (env < closeThr) { if (hold > 0) hold--; else open = false; } else hold = holdN; }
        float tg = 1.f;
        if (!open) { float r = env / openThr; tg = r * r; if (tg < rangeLin) tg = rangeLin; if (tg > 1.f) tg = 1.f; }
        g += (tg - g) * (tg > g ? atk : rel);
        return g;
    }
};

// Feed-forward compressor: mean-square detector (~10 ms), soft knee (6 dB), gain computed every 16 samples (log2f / exp2f only there),
// attack / release smoothing on the gain in dB, linear interpolation of the gain in between. Returns a linear gain (makeup included).
struct Compressor {
    float ms = 0.f, gCur = 1.f, gNext = 1.f, grdB = 0.f, msK = 0.002f, aA = 0.1f, aR = 0.01f, thr = -18.f, ratio = 3.f, makeLin = 1.f; int cnt = 0;
    static const int CTL = 16;
    void reset() { ms = 0.f; gCur = gNext = 1.f; grdB = 0.f; cnt = 0; }
    void set(float thrDb, float r, float attMs, float relMs, float makeDb, bool autoMk, float fs) {
        thr = thrDb; ratio = r < 1.f ? 1.f : r; msK = 1.f - expf(-1.f / (0.010f * fs));
        aA = 1.f - expf(-(float)CTL / (attMs * 0.001f * fs)); aR = 1.f - expf(-(float)CTL / (relMs * 0.001f * fs));
        float mk = makeDb + (autoMk ? -thr * (1.f - 1.f / ratio) * 0.5f : 0.f);          // auto = half of the reduction a full-scale signal would get
        makeLin = exp2f(mk * 0.16609640474f);
    }
    static float curveDb(float L, float T, float R, float W) {            // gain in dB (<= 0) for an input level L dB, soft knee width W
        float over = L - T;
        if (2.f * over <= -W) return 0.f;
        float out;
        if (2.f * over < W) { float t = over + 0.5f * W; out = L + (1.f / R - 1.f) * t * t / (2.f * W); }
        else out = T + over / R;
        return out - L;
    }
    inline float proc(float x) {
        ms += (x * x - ms) * msK;
        if (++cnt >= CTL) {
            cnt = 0; gCur = gNext;
            float L = 3.01029996f * log2f(ms + 1e-12f);                    // 10*log10(ms)
            float gdB = curveDb(L, thr, ratio, 6.f);
            grdB += (gdB - grdB) * (gdB < grdB ? aA : aR);
            gNext = exp2f(grdB * 0.16609640474f) * makeLin;
        }
        return gCur + (gNext - gCur) * ((float)cnt * (1.f / (float)CTL));
    }
};

// Split-band de-esser: the sibilance band (band-pass around deess_freq) is attenuated on its own when it dominates the signal
// (band level / broadband level above a fixed ratio), the rest of the spectrum is untouched.
struct DeEsser {
    Biquad bp; float envB = 0.f, envF = 0.f, g = 1.f, ka = 0.1f, kr = 0.001f, kgA = 0.05f, kgR = 0.004f, cF = -1.f, fs = 48000.f;
    void init(float f) { fs = f; ka = 1.f - expf(-1.f / (0.0006f * fs)); kr = 1.f - expf(-1.f / (0.025f * fs));
                         kgA = 1.f - expf(-1.f / (0.002f * fs)); kgR = 1.f - expf(-1.f / (0.030f * fs)); cF = -1.f; reset(); }
    void reset() { bp.clear(); envB = envF = 0.f; g = 1.f; }
    void setFreq(float f) { if (f != cF) { cF = f; bp.bandpass(fs, f, 1.2); } }
    inline float proc(float x, float amt) {
        float band = bp.proc(x);
        float aB = fabsf(band), aF = fabsf(x);
        envB += (aB - envB) * (aB > envB ? ka : kr); envF += (aF - envF) * (aF > envF ? ka : kr);
        const float r0 = 0.35f; float r = envB / (envF + 0.002f), tg = 1.f;
        if (r > r0) { tg = 1.f - amt * (1.f - r0 / r); if (tg < 0.2f) tg = 0.2f; }
        g += (tg - g) * (tg < g ? kgA : kgR);
        return x - (1.f - g) * band;
    }
};

// Lookahead limiter (~3 ms): sliding minimum of the required gain over the lookahead window, then a box average of the same length, so the
// gain is already down when the peak leaves the delay line (the guarantee is exact: average of values that are all <= the needed gain).
// Release is a smooth one-pole that can only keep the gain lower. A final hard clamp makes the ceiling absolute.
struct Limiter {
    static const int CAP = 512; static const int MASK = CAP - 1;
    float dl[CAP], mnb[CAP], qv[CAP]; int64_t qi[CAP]; int qh = 0, qt = 0; int D = 144; int64_t t = 0; double sum = 0.0; float gs = 1.f, rel = 0.001f, lastGs = 1.f;
    void init(float fs) { D = std::min(CAP / 2 - 2, std::max(8, (int)floorf(0.003f * fs + 0.5f))); rel = 1.f - expf(-1.f / (0.060f * fs)); reset(); }
    void reset() { for (int i = 0; i < CAP; i++) { dl[i] = 0.f; mnb[i] = 1.f; qv[i] = 1.f; qi[i] = 0; } qh = qt = 0; t = 0; sum = (double)(D + 1); gs = 1.f; lastGs = 1.f; }
    inline float proc(float x, float ceil) {
        dl[t & MASK] = x;
        float ax = fabsf(x), gn = ax > ceil ? ceil / ax : 1.f;
        while (qt != qh && qv[(qt - 1) & MASK] >= gn) qt = (qt - 1) & MASK;           // monotone deque, window = the last D+1 inputs
        qv[qt] = gn; qi[qt] = t; qt = (qt + 1) & MASK;
        while (qi[qh] <= t - (D + 1)) qh = (qh + 1) & MASK;
        float mn = qv[qh];
        float old = mnb[(t - (D + 1)) & MASK]; mnb[t & MASK] = mn; sum += (double)mn - (double)old;
        float gb = (float)(sum / (double)(D + 1)); if (gb > 1.f) gb = 1.f;
        gs = gb < gs ? gb : gs + (gb - gs) * rel;
        lastGs = gs;
        float y = dl[(t - D) & MASK] * gs; t++;
        if (y > ceil) y = ceil; else if (y < -ceil) y = -ceil;
        return y;
    }
};


// ================================================================== Phase 4: quality blocks
// RNNoise wrapper: 48 kHz, 480-sample frames, input ring. Measured total delay of this engine: 960 samples (20 ms).
struct AiNs {
#ifndef VFX_NO_RNNOISE
    DenoiseState* st = nullptr;
#endif
    float inb[480], ready[480]; int fill = 0, rd = 0; bool has = false;
    static constexpr bool built() {
#ifndef VFX_NO_RNNOISE
        return true;
#else
        return false;
#endif
    }
    bool usable() const {
#ifndef VFX_NO_RNNOISE
        return st != nullptr;
#else
        return false;
#endif
    }
    ~AiNs() { destroy(); }
    void destroy() {
#ifndef VFX_NO_RNNOISE
        if (st) { rnnoise_destroy(st); st = nullptr; }
#endif
    }
    bool create() {                                              // allocation: only from rebuild (never from the audio path)
        destroy(); flush();
#ifndef VFX_NO_RNNOISE
        st = rnnoise_create(nullptr); return st != nullptr;
#else
        return false;
#endif
    }
    void flush() { fill = 0; rd = 0; has = false; for (int i = 0; i < 480; i++) { inb[i] = 0.f; ready[i] = 0.f; } }
    inline float proc(float x) {                                 // x in +-1; returns the denoised sample one frame later
        float y = has ? ready[rd] : 0.f; if (has && ++rd >= 480) rd = 0;
        inb[fill++] = x * 32768.f;
        if (fill >= 480) {
            fill = 0;
#ifndef VFX_NO_RNNOISE
            if (st) { rnnoise_process_frame(st, ready, inb); for (int i = 0; i < 480; i++) ready[i] *= (1.f / 32768.f); has = true; rd = 0; }
#endif
        }
        return y;
    }
};

// 12- or 20-band channel vocoder bank (envelope follower on the voice, band-passed carrier); the 12-band / Q 3 instance is the previous vocoder
struct VocBank {
    static const int MAXB = 20; int n = 12; Biquad an[MAXB], sy[MAXB]; float env[MAXB]; float inP = 0.f, outP = 0.f, agc = 1.f;
    void init(float fs, int nb, double q) { n = nb; for (int i = 0; i < n; i++) { double c = 140.0 * pow(7500.0 / 140.0, i / (double)(n - 1)); an[i].bandpass(fs, c, q); sy[i].bandpass(fs, c, q); } reset(); }
    void reset() { for (auto& b : an) b.clear(); for (auto& b : sy) b.clear(); for (int i = 0; i < MAXB; i++) env[i] = 0.f; inP = outP = 0.f; agc = 1.f; }
    inline float proc(float x, float car) {
        float acc = 0.f;
        for (int i = 0; i < n; i++) {
            float a = fabsf(an[i].proc(x));
            env[i] = fz(env[i] + (a - env[i]) * (a > env[i] ? 0.02f : 0.0035f));
            acc += env[i] * sy[i].proc(car);
        }
        inP = fz(inP + (x * x - inP) * 0.0008f); outP = fz(outP + (acc * acc - outP) * 0.0008f);
        float tg = sqrtf((inP + 1e-9f) / (outP + 1e-9f)); tg = clampf(tg, 0.05f, 60.f);
        agc += (tg - agc) * 0.002f;
        return acc * agc;
    }
};

// Anti-aliased drive: 2x oversampling (4th-order low-pass up and down) around a rational tanh approximation (no per-sample tanhf)
struct Drive2x {
    Biquad u[2], d[2];
    void init(float fs) { const double f2 = 2.0 * fs, fc = 0.45 * fs; u[0].lowpass(f2, fc, 0.5412); u[1].lowpass(f2, fc, 1.3066); d[0].lowpass(f2, fc, 0.5412); d[1].lowpass(f2, fc, 1.3066); reset(); }
    void reset() { u[0].clear(); u[1].clear(); d[0].clear(); d[1].clear(); }
    static inline float shp(float z) { if (z > 3.f) return 1.f; if (z < -3.f) return -1.f; float z2 = z * z; return z * (27.f + z2) / (27.f + 9.f * z2); }
    inline float proc(float x, float g, float k) {
        const float a = u[1].proc(u[0].proc(2.f * x)), b = u[1].proc(u[0].proc(0.f));
        d[1].proc(d[0].proc(shp(g * a) * k));
        return d[1].proc(d[0].proc(shp(g * b) * k));
    }
};

// 8-line feedback delay network reverb: Hadamard feedback matrix, per-line damping, slowly modulated delay lengths, pre-delay.
struct Fdn {
    static const int N = 8;
    std::vector<float> ln[N]; size_t mk[N], w[N]; float len[N], lpS[N], oc[N], os[N], oe[N], g[N], dA = 0.5f, preD = 0.f;
    std::vector<float> pre; size_t pmk = 0, pw = 0;
    void init(float fs) {
        const int base[N] = { 1087, 1283, 1481, 1669, 1913, 2131, 2381, 2677 };
        for (int i = 0; i < N; i++) { len[i] = (float)base[i] * fs / 48000.f; size_t n = 1; while (n < (size_t)len[i] + 64) n <<= 1; ln[i].assign(n, 0.f); mk[i] = n - 1; w[i] = 0;
            oe[i] = kTwoPi * (0.13f + 0.047f * (float)i) / fs; oc[i] = cosf(0.9f * (float)i); os[i] = sinf(0.9f * (float)i); lpS[i] = 0.f; g[i] = 0.8f; }
        pre.assign(8192, 0.f); pmk = 8191; pw = 0;
    }
    void reset() { for (int i = 0; i < N; i++) { std::fill(ln[i].begin(), ln[i].end(), 0.f); w[i] = 0; lpS[i] = 0.f; } std::fill(pre.begin(), pre.end(), 0.f); pw = 0; }
    void set(float fs, float size, float damp, float preMs) {
        const float rt60 = 0.4f + 3.6f * size;
        for (int i = 0; i < N; i++) g[i] = powf(10.f, -3.f * len[i] / (fs * rt60));
        dA = 0.98f - 0.95f * damp; preD = floorf(preMs * 0.001f * fs + 0.5f);
    }
    inline float proc(float in) {
        if (preD >= 1.f) { pre[pw & pmk] = in; in = pre[(pw - (size_t)preD) & pmk]; pw++; }
        float r[N], v[N];
        for (int i = 0; i < N; i++) {
            os[i] += oe[i] * oc[i]; oc[i] -= oe[i] * os[i];                               // magic-circle oscillator: slow length modulation (+-6 samples)
            float rp = (float)w[i] - (len[i] + 6.f * os[i]); if (rp < 0.f) rp += (float)ln[i].size();
            size_t i0 = (size_t)rp; float fr = rp - (float)i0;
            r[i] = ln[i][i0 & mk[i]] * (1.f - fr) + ln[i][(i0 + 1) & mk[i]] * fr;
            lpS[i] += (r[i] - lpS[i]) * dA; lpS[i] = fz(lpS[i]); v[i] = lpS[i] * g[i];
        }
        for (int h = 1; h < N; h <<= 1) for (int i = 0; i < N; i += h << 1) for (int j = i; j < i + h; j++) { float a = v[j], b = v[j + h]; v[j] = a + b; v[j + h] = a - b; }   // Hadamard
        static const float sg[N] = { 1.f, -1.f, 1.f, 1.f, -1.f, 1.f, -1.f, -1.f };
        const float k = 0.35355339f, ig = 0.35f * in;
        for (int i = 0; i < N; i++) { ln[i][w[i]] = fz(v[i] * k + sg[i] * ig); w[i] = (w[i] + 1) & mk[i]; }
        return (r[0] - r[1] + r[2] - r[3] + r[4] - r[5] + r[6] - r[7]) * 0.5f;
    }
};

// Formant-preserving voice for the chords: pitch-synchronous overlap-add. Grains of two periods are cut from the input around pitch marks
// (found by a local peak search on a period grid) and re-spaced by T / ratio, so the spectral envelope (formants) stays where it was.
// Fixed latency D (= the Lite voice latency), so it lines up with the other voices; periods up to D / 1.5 are supported.
struct Psola {
    static const int CAP = 8192, MASK = CAP - 1; float buf[CAP], acc[CAP]; int64_t n = 0, lastMark = 0; double ts = 0.0; bool synced = false; int D = 770; float curT = 0.f;
    static float* hann() { static float t[1025]; static bool ok = false; if (!ok) { for (int i = 0; i <= 1024; i++) t[i] = 0.5f - 0.5f * cosf(kTwoPi * (float)i / 1024.f); ok = true; } return t; }
    void init(int latency) { D = latency; reset(); }
    void reset() { memset(buf, 0, sizeof(buf)); memset(acc, 0, sizeof(acc)); n = 0; lastMark = 0; ts = 0.0; synced = false; curT = 0.f; }
    float maxT() const { return (float)D / 1.5f; }
    inline float proc(float x, float t0, float ratio) {
        buf[n & MASK] = x; n++;
        if (t0 > 0.f) curT = curT > 0.f ? curT + (t0 - curT) * 0.01f : t0;
        const float T = curT; const float* hw = hann();
        if (T >= 20.f && T <= maxT()) {
            if (!synced) { synced = true; ts = (double)(n - D) + (double)T; lastMark = (int64_t)(ts - D); }
            const int Tn = (int)(T + 0.5f); const float inv = 1.f / ratio, wk = 1024.f / (float)(2 * Tn);
            while (ts - D + 2.0 * T <= (double)n) {
                const double tc = ts - D;
                int64_t kk = (int64_t)llround((tc - (double)lastMark) / (double)T); if (kk < 0) kk = 0;
                const int64_t a0 = lastMark + (int64_t)llround((double)kk * (double)T), sr = (int64_t)(T * 0.25f);
                int64_t best = a0; float bv = -1.f; for (int64_t ia = a0 - sr; ia <= a0 + sr; ia++) { float v = fabsf(buf[ia & MASK]); if (v > bv) { bv = v; best = ia; } }
                lastMark = best;
                const int64_t ot = (int64_t)llround(ts);
                for (int j = -Tn; j < Tn; j++) acc[(ot + j) & MASK] += hw[(int)((float)(j + Tn) * wk)] * buf[(best + j) & MASK] * inv;
                ts += (double)T / (double)ratio;
            }
        }
        const int64_t t = n - D; float y = acc[t & MASK]; acc[t & MASK] = 0.f; return y;
    }
};

static const float kFdnGain = 1.1f;     // wet level of the FDN, calibrated against the previous reverb (see test [P4-4])

struct Snap {        // one consistent read of the parameters per DSP call
    float hp, lp, bass, treble, drive, bits, down, tremHz, tremDepth, modRate, modDepth, echoMs, echoFb, echoMix, revSize, revMix,
          robotHz, robotAmt, vocHz, echoDamp = 0.f, echoHp = 0.f, revPre = 0.f, revDamp = 0.5f, carHz = 0.f; int modMode, vocMode; bool robot;
};

// effect rack, runs sample by sample after the pitch stage
struct FxChain {
    float fs = 48000.f; Snap p{};
    Biquad hp[2], lp[2], bass, treb; bool hpOn = false, lpOn = false, bassOn = false, trebOn = false;
    float cHp = -1, cLp = -1, cBass = -99, cTreb = -99;
    float phR = 0, phT = 0, hold = 0, hcnt = 0;
    std::vector<float> mbuf; size_t mmask = 0, mw = 0; float lfo1 = 0, lfo2 = 0, fbState = 0;
    std::vector<float> ebuf; size_t ew = 0; float eCur = 0, eTgt = 0; bool eInit = false;
    Comb comb[8]; Allpass ap[4]; float revFb = 0.84f;
    VocBank vocL, vocN; float vph = 0, carCur = 0.f; uint32_t rng = 22222u;          // vocL = previous 12-band vocoder, vocN = 20-band
    Drive2x drv2; Fdn fdn; float crushLv = 1.f, drvG = 1.f, drvK = 0.85f, eLp = 0.f, eHp = 0.f, eLpA = 1.f, eHpA = 0.f;

    void init(float f) {
        fs = f; reset();
        size_t n = 1; while (n < (size_t)(fs * 0.1f)) n <<= 1; mbuf.assign(n, 0.f); mmask = n - 1;
        ebuf.assign((size_t)(fs * 1.05f) + 16, 0.f);
        const int ct[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 }, at[4] = { 556, 441, 341, 225 };
        for (int i = 0; i < 8; i++) comb[i].init((size_t)(ct[i] * fs / 44100.f));
        for (int i = 0; i < 4; i++) ap[i].init((size_t)(at[i] * fs / 44100.f));
        vocL.init(fs, 12, 3.0); vocN.init(fs, 20, 4.5); drv2.init(fs); fdn.init(fs);
        cHp = cLp = -1; cBass = cTreb = -99;
    }
    void reset() {
        for (auto& b : hp) b.clear();
        for (auto& b : lp) b.clear();
        bass.clear(); treb.clear();
        std::fill(mbuf.begin(), mbuf.end(), 0.f); std::fill(ebuf.begin(), ebuf.end(), 0.f);
        for (auto& c : comb) init_clear(c);
        for (auto& a : ap) std::fill(a.b.begin(), a.b.end(), 0.f);
        vocL.reset(); vocN.reset(); drv2.reset(); fdn.reset(); eLp = eHp = 0.f; carCur = 0.f;
        phR = phT = hold = hcnt = 0.f; lfo1 = lfo2 = fbState = 0.f; mw = ew = 0; eInit = false; vph = 0.f;
    }
    static void init_clear(Comb& c) { std::fill(c.b.begin(), c.b.end(), 0.f); c.store = 0.f; c.i = 0; }

    void update(const Snap& s) {
        p = s;
        bool on = s.hp > 25.f; if (on != hpOn) { hp[0].clear(); hp[1].clear(); cHp = -1; } hpOn = on;
        if (on && s.hp != cHp) { hp[0].highpass(fs, s.hp, 0.5412); hp[1].highpass(fs, s.hp, 1.3066); cHp = s.hp; }
        on = s.lp < 19000.f; if (on != lpOn) { lp[0].clear(); lp[1].clear(); cLp = -1; } lpOn = on;
        if (on && s.lp != cLp) { lp[0].lowpass(fs, s.lp, 0.5412); lp[1].lowpass(fs, s.lp, 1.3066); cLp = s.lp; }
        on = fabsf(s.bass) > 0.05f; if (on != bassOn) { bass.clear(); cBass = -99; } bassOn = on;
        if (on && s.bass != cBass) { bass.lowshelf(fs, 220.0, s.bass); cBass = s.bass; }
        on = fabsf(s.treble) > 0.05f; if (on != trebOn) { treb.clear(); cTreb = -99; } trebOn = on;
        if (on && s.treble != cTreb) { treb.highshelf(fs, 3500.0, s.treble); cTreb = s.treble; }
        eTgt = clampf(s.echoMs * 0.001f * fs, 8.f, (float)ebuf.size() - 8.f);
        if (!eInit) { eCur = eTgt; eInit = true; }
        revFb = 0.70f + 0.28f * s.revSize;
        drvG = 1.f + 20.f * s.drive; drvK = 0.85f / sqrtf(drvG); crushLv = exp2f(s.bits - 1.f);
        eLpA = 1.f - 0.94f * s.echoDamp; eHpA = clampf(kTwoPi * s.echoHp / fs, 0.f, 1.f); if (s.echoDamp <= 0.001f) eLp = 0.f; if (s.echoHp <= 25.f) eHp = 0.f;
        fdn.set(fs, s.revSize, s.revDamp, s.revPre);
    }

    inline float noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)(int32_t)rng * (1.f / 2147483648.f); }
    inline float mrd(float d) const { float rp = (float)mw - d; long i0 = (long)floorf(rp); float fr = rp - (float)i0;
        return mbuf[(size_t)i0 & mmask] * (1.f - fr) + mbuf[(size_t)(i0 + 1) & mmask] * fr; }

    float process(float x, float w1 = 0.f) {              // w1: 0 = previous algorithms (legacy), 1 = V1 algorithms; in between both run and are cross-faded
        // 1) vocoder (whisper = noise carrier, cyber = saw carrier): keeps the spoken envelope, replaces the voice source
        if (p.vocMode) {
            float car;
            if (p.vocMode == 1) car = noise();
            else { float hz = p.vocHz;
                   if (w1 > 0.f) { carCur += ((p.carHz > 0.f ? p.carHz : p.vocHz) - carCur) * 0.003f; if (w1 > 0.5f) hz = carCur; } else carCur = p.vocHz;     // follow-my-pitch carrier (V1 only)
                   float dt = hz / fs; vph += dt; vph -= floorf(vph); float t = vph, bl = 0.f;
                   if (t < dt) { float u = t / dt; bl = u + u - u * u - 1.f; } else if (t > 1.f - dt) { float u = (t - 1.f) / dt; bl = u * u + u + u + 1.f; }
                   car = 2.f * t - 1.f - bl; }
            if (w1 <= 0.f) x = vocL.proc(x, car);
            else if (w1 >= 1.f) x = vocN.proc(x, car);
            else { float a = vocL.proc(x, car), b = vocN.proc(x, car); x = a + w1 * (b - a); }
        }
        // 2) drive (V1: 2x oversampled, rational tanh; legacy: plain tanh)
        if (p.drive > 0.001f) {
            if (w1 >= 1.f) x = drv2.proc(x, drvG, drvK);
            else { float yl = tanhf(drvG * x) * drvK; x = (w1 <= 0.f) ? yl : yl + w1 * (drv2.proc(x, drvG, drvK) - yl); }
        }
        // 3) bit crusher / sample-rate reducer
        if (p.bits < 15.5f || p.down > 1.01f) {
            if (p.down > 1.01f) { hcnt += 1.f; if (hcnt >= p.down) { hcnt -= p.down; hold = x; } x = hold; }
            if (p.bits < 15.5f) { x = floorf(x * crushLv + 0.5f) / crushLv; }
        }
        // 4) tone: high-pass / low-pass (24 dB/oct) and shelves
        if (hpOn) x = hp[1].proc(hp[0].proc(x));
        if (lpOn) x = lp[1].proc(lp[0].proc(x));
        if (bassOn) x = bass.proc(x);
        if (trebOn) x = treb.proc(x);
        // 5) robot ring modulator
        if (p.robot) { phR += p.robotHz / fs; phR -= floorf(phR); x *= (1.f - p.robotAmt) + p.robotAmt * g_lut.sin01(phR); }
        // 6) tremolo
        if (p.tremDepth > 0.001f) { phT += p.tremHz / fs; phT -= floorf(phT); x *= 1.f - p.tremDepth * (0.5f - 0.5f * g_lut.sin01(phT)); }
        // 7) modulated delay: chorus / flanger / vibrato
        if (p.modMode) {
            float rate = p.modRate / fs; lfo1 = wrap01(lfo1 + rate); lfo2 = wrap01(lfo2 + rate * 1.13f);
            float s1 = g_lut.sin01(lfo1), s2 = g_lut.sin01(wrap01(lfo2 + 0.27056342f)), out;     // 0.2705634 turns = 1.7 rad
            if (p.modMode == 2) {                       // flanger
                mbuf[mw & mmask] = fz(x + fbState * 0.55f);
                float t = mrd(fs * (0.0025f + 0.0020f * p.modDepth * s1)); fbState = t; out = (x + t) * 0.7f;
            } else {
                mbuf[mw & mmask] = x;
                if (p.modMode == 1) { float t1 = mrd(fs * (0.018f + 0.006f * p.modDepth * s1)), t2 = mrd(fs * (0.026f + 0.008f * p.modDepth * s2)); out = x * 0.7f + (t1 + t2) * 0.35f; }
                else out = mrd(fs * (0.004f + 0.003f * p.modDepth * s1));   // vibrato
            }
            mw++; x = out;
        }
        // 8) echo
        if (p.echoMix > 0.001f) {
            eCur += (eTgt - eCur) * 0.0007f;
            float rp = (float)ew - eCur; if (rp < 0.f) rp += (float)ebuf.size();
            if (rp >= (float)ebuf.size()) rp -= (float)ebuf.size();       // rounding could land exactly on the buffer length (out-of-bounds read)
            size_t i0 = (size_t)rp, i1 = i0 + 1 >= ebuf.size() ? 0 : i0 + 1; float fr = rp - (float)i0;
            float d = ebuf[i0] * (1.f - fr) + ebuf[i1] * fr;
            float df = d;
            if (p.echoDamp > 0.001f) { eLp = fz(eLp + (df - eLp) * eLpA); df = eLp; }               // feedback loop: low-pass (damping) ...
            if (p.echoHp > 25.f) { eHp = fz(eHp + (df - eHp) * eHpA); df = df - eHp; }              // ... and optional high-pass
            ebuf[ew] = fz(x + df * p.echoFb); if (++ew >= ebuf.size()) ew = 0;
            x += d * p.echoMix;
        }
        // 9) reverb (Freeverb topology, mono)
        if (p.revMix > 0.001f) {                    // V1: 8-line FDN; legacy: the previous Freeverb
            float xl = 0.f, xn = 0.f;
            if (w1 < 1.f) { float in = x * 0.015f, sum = 0.f;
                for (int i = 0; i < 8; i++) sum += comb[i].proc(in, revFb, 0.3f, 0.7f);
                for (int i = 0; i < 4; i++) sum = ap[i].proc(sum);
                xl = x * (1.f - 0.25f * p.revMix) + sum * 2.2f * p.revMix; }
            if (w1 > 0.f) xn = x * (1.f - 0.25f * p.revMix) + fdn.proc(x) * kFdnGain * p.revMix;
            x = (w1 <= 0.f) ? xl : (w1 >= 1.f ? xn : xl + w1 * (xn - xl));
        }
        return x;
    }
};


// Noise suppressor: 512-point STFT (hop 256, sqrt-Hann), noise floor from minimum statistics over ~1 s of smoothed bin powers,
// decision-directed Wiener gain with a floor set by the strength, plus a short-burst guard for keyboard clicks.
struct NoiseSuppressor {
    static const int N = 512, HOP = 256, NB = 257, NSUB = 6, SUBLEN = 32;
    float fs = 48000.f, win[N], cosT[N / 2], sinT[N / 2]; int rev[N];
    float hist[N], hop[HOP], acc[N], re[N], im[N];
    float S[NB], minCur[NB], minHist[NSUB][NB], Nest[NB], Gprev[NB], gamPrev[NB], Gs[NB], P[NB], Graw[NB];
    int hopN = 0, frames = 0, subCount = 0, histPos = 0, histFill = 0, hfRun = 0, clickHold = 0;
    float avgHF = 0.f, avgLF = 0.f, floorDb = -90.f, redDb = 0.f;
    Ring outq;

    void init(float f) {
        fs = f;
        for (int n = 0; n < N; n++) win[n] = sqrtf(0.5f - 0.5f * cosf(kTwoPi * n / N));
        for (int k = 0; k < N / 2; k++) { cosT[k] = cosf(kTwoPi * k / N); sinT[k] = sinf(kTwoPi * k / N); }
        for (int a = 0; a < N; a++) { int r = 0, v = a; for (int b = 0; b < 9; b++) { r = (r << 1) | (v & 1); v >>= 1; } rev[a] = r; }
        outq.init(2048); reset();
    }
    void reset() { flush(); frames = 0; subCount = 0; histPos = 0; histFill = 0; avgHF = avgLF = 0.f; floorDb = -90.f;
                   for (int k = 0; k < NB; k++) { S[k] = minCur[k] = Nest[k] = 0.f; for (int j = 0; j < NSUB; j++) minHist[j][k] = 0.f; } }
    void flush() {                       // drop audio state, keep what was learned about the noise (a mic burst after a pause starts clean)
        std::fill(hist, hist + N, 0.f); std::fill(hop, hop + HOP, 0.f); std::fill(acc, acc + N, 0.f);
        for (int k = 0; k < NB; k++) { Gprev[k] = 1.f; gamPrev[k] = 0.f; Gs[k] = 1.f; }
        hopN = 0; hfRun = 0; clickHold = 0; redDb = 0.f; outq.clear(); for (int i = 0; i < HOP; i++) outq.push(0.f);
    }
    void fft(float* r, float* i) const {
        for (int a = 0; a < N; a++) { int b = rev[a]; if (b > a) { std::swap(r[a], r[b]); std::swap(i[a], i[b]); } }
        for (int len = 2; len <= N; len <<= 1) {
            int half = len >> 1, step = N / len;
            for (int s0 = 0; s0 < N; s0 += len) for (int j = 0; j < half; j++) {
                float wr = cosT[j * step], wi = -sinT[j * step]; int a = s0 + j, b = a + half;
                float tr = r[b] * wr - i[b] * wi, ti = r[b] * wi + i[b] * wr;
                r[b] = r[a] - tr; i[b] = i[a] - ti; r[a] += tr; i[a] += ti;
            }
        }
    }
    void frame(float amt, float click) {
        memmove(hist, hist + HOP, HOP * sizeof(float)); memcpy(hist + HOP, hop, HOP * sizeof(float));
        for (int n = 0; n < N; n++) { re[n] = hist[n] * win[n]; im[n] = 0.f; }
        fft(re, im);
        for (int k = 0; k < NB; k++) P[k] = re[k] * re[k] + im[k] * im[k];
        // ---- noise floor tracking
        if (frames == 0) for (int k = 0; k < NB; k++) { S[k] = P[k]; minCur[k] = P[k]; }
        for (int k = 0; k < NB; k++) { S[k] = 0.85f * S[k] + 0.15f * P[k]; if (S[k] < minCur[k]) minCur[k] = S[k]; }
        if (++subCount >= SUBLEN) {
            subCount = 0; for (int k = 0; k < NB; k++) { minHist[histPos][k] = minCur[k]; minCur[k] = S[k]; }
            histPos = (histPos + 1) % NSUB; if (histFill < NSUB) histFill++;
        }
        double mn = 0.0;
        for (int k = 0; k < NB; k++) {
            float m = minCur[k]; for (int j = 0; j < histFill; j++) if (minHist[j][k] < m) m = minHist[j][k];
            if (frames < 8) m = S[k];
            Nest[k] = m * 1.8f + 1e-12f; if (k > 0 && k < NB - 1) mn += m;
        }
        floorDb = 10.f * log10f((float)(mn / (NB - 2)) / (N / 2) + 1e-12f);
        // ---- click guard: short, broadband, high-frequency-only burst that is far above the learned noise
        const float binHz = fs / N; int kLF = std::max(3, (int)(1000.f / binHz)), kHF = std::max(kLF + 1, (int)(2000.f / binHz)), kC = std::max(2, (int)(600.f / binHz));
        float EH = 0.f, EL = 0.f, NH = 0.f, NL = 0.f;
        for (int k = kHF; k < NB; k++) { EH += P[k]; NH += Nest[k]; }
        for (int k = 3; k <= kLF; k++) { EL += P[k]; NL += Nest[k]; }
        if (frames == 0) { avgHF = EH; avgLF = EL; }
        bool burst = frames > 20 && EH > 8.f * avgHF + 6.f * NH && EL < 4.f * avgLF + 4.f * NL;
        if (!burst) { avgHF += (EH - avgHF) * 0.05f; avgLF += (EL - avgLF) * 0.05f; hfRun = 0; }
        else { hfRun++; if (hfRun <= 2) clickHold = 3; else clickHold = 0; }        // lasts longer than ~10 ms -> not a click (s, sh, fan puff)
        // ---- gains
        const float gMin = powf(10.f, -(8.f + 22.f * amt) / 20.f), overSub = 1.f + 1.5f * amt;
        for (int k = 0; k < NB; k++) {
            float gam = P[k] / (Nest[k] * overSub);
            float xi = 0.96f * Gprev[k] * Gprev[k] * gamPrev[k] + 0.04f * std::max(gam - 1.f, 0.f);
            if (xi < 0.0032f) xi = 0.0032f;
            float g = xi / (1.f + xi); if (g < gMin) g = gMin; if (g > 1.f) g = 1.f;
            Graw[k] = g; Gprev[k] = g; gamPrev[k] = gam;
        }
        const float clickLin = powf(10.f, -(12.f + 20.f * click) / 20.f); const bool clk = clickHold > 0 && click > 0.01f;
        float sumG = 0.f;
        for (int k = 0; k < NB; k++) {
            float gl = Graw[k > 0 ? k - 1 : 1], gr = Graw[k < NB - 1 ? k + 1 : NB - 2];
            float gf = 0.25f * gl + 0.5f * Graw[k] + 0.25f * gr;
            Gs[k] += (gf - Gs[k]) * (gf > Gs[k] ? 0.8f : 0.5f);
            float ga = Gs[k]; if (clk && k >= kC) ga *= clickLin;
            re[k] *= ga; im[k] *= ga; if (k > 0 && k < NB - 1) { re[N - k] *= ga; im[N - k] *= ga; }
            sumG += ga;
        }
        if (clickHold > 0) clickHold--;
        redDb += (-20.f * log10f(sumG / NB + 1e-6f) - redDb) * 0.1f;
        fft(im, re);                                       // inverse via swapped real/imag
        const float inv = 1.f / N;
        for (int n = 0; n < N; n++) acc[n] += re[n] * inv * win[n];
        for (int n = 0; n < HOP; n++) outq.push(acc[n]);
        memmove(acc, acc + HOP, HOP * sizeof(float)); std::fill(acc + HOP, acc + N, 0.f);
        frames++;
    }
    inline float proc(float x, float amt, float click) {
        hop[hopN++] = x; if (hopN >= HOP) { hopN = 0; frame(amt, click); }
        return outq.pop();
    }
};

// ================================================================== Auto-Tune / harmony helpers
// Scales as semitone sets relative to the root. 0 chromatic, 1 major, 2 minor, 3 pentatonic major, 4 pentatonic minor, 5 blues, 6 dorian, 7 harmonic minor
static const signed char kScaleDeg[8][12] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11, -1, -1, -1, -1, -1 }, { 0, 2, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1 },
    { 0, 2, 4, 7, 9, -1, -1, -1, -1, -1, -1, -1 }, { 0, 3, 5, 7, 10, -1, -1, -1, -1, -1, -1, -1 }, { 0, 3, 5, 6, 7, 10, -1, -1, -1, -1, -1, -1 },
    { 0, 2, 3, 5, 7, 9, 10, -1, -1, -1, -1, -1 }, { 0, 2, 3, 5, 7, 8, 11, -1, -1, -1, -1, -1 } };
static const int kHarmScale[8] = { 1, 1, 2, 1, 2, 2, 6, 7 };       // scale used to build chords (chromatic / pentatonic / blues borrow the full scale)
static const int kRootOffset[8] = { 0, 0, 9, 0, 9, 9, 2, 9 };      // auto key finds the major tonic; minor-type scales sit 9 semitones above it
static uint16_t scaleMask(int sc) {
    static uint16_t m[8]; static bool init = false;
    if (!init) { for (int i = 0; i < 8; i++) { m[i] = 0; for (int j = 0; j < 12 && kScaleDeg[i][j] >= 0; j++) m[i] |= (uint16_t)(1u << kScaleDeg[i][j]); } init = true; }
    return m[sc < 0 ? 0 : (sc > 7 ? 7 : sc)];
}
static inline bool inScale(int n, int root, uint16_t mask) { int pc = ((n - root) % 12 + 12) % 12; return (mask >> pc) & 1; }
// nearest scale note to a (fractional) midi pitch; keeps the previous note while it is almost as close (no flicker at the boundary)
static float nearestNote(float midi, int root, uint16_t mask, float prev) {
    int c = (int)floorf(midi + 0.5f); float best = 1e9f; int bn = c;
    for (int d = -3; d <= 3; d++) { int n = c + d; if (!inScale(n, root, mask)) continue; float dist = fabsf(midi - (float)n); if (dist < best) { best = dist; bn = n; } }
    if (prev > 0.f && inScale((int)prev, root, mask)) { float dp = fabsf(midi - prev); if (dp < best + 0.15f && dp < 1.6f) return prev; }
    return (float)bn;
}
static int stepUp(int n, int steps, int root, uint16_t hm) {         // n moved up `steps` scale degrees (n is snapped into the scale first)
    if (!inScale(n, root, hm)) { int a = n, b = n; while (!inScale(a, root, hm)) a--; while (!inScale(b, root, hm)) b++; n = (n - a <= b - n) ? a : b; }
    while (steps > 0) { n++; while (!inScale(n, root, hm)) n++; steps--; }
    return n;
}
struct ChordVoice { signed char steps, oct; float gain; };
struct ChordDef { int n; ChordVoice v[3]; };
// 1 Magic (3rd + 5th + bass root), 2 Triad, 3 Power (5th + octave), 4 Seventh, 5 Sus (4th + 5th), 6 Angel (airy, an octave up), 7 Octaves, 8 Duet (3rd)
static const ChordDef kChord[9] = {
    { 0, { { 0, 0, 0.f }, { 0, 0, 0.f }, { 0, 0, 0.f } } },
    { 3, { { 2, 0, 0.85f }, { 4, 0, 0.75f }, { 0, -1, 0.70f } } },
    { 2, { { 2, 0, 0.90f }, { 4, 0, 0.85f }, { 0, 0, 0.f } } },
    { 2, { { 4, 0, 0.90f }, { 0, 1, 0.65f }, { 0, 0, 0.f } } },
    { 3, { { 2, 0, 0.80f }, { 4, 0, 0.75f }, { 6, 0, 0.70f } } },
    { 2, { { 3, 0, 0.85f }, { 4, 0, 0.85f }, { 0, 0, 0.f } } },
    { 3, { { 0, 1, 0.75f }, { 4, 1, 0.65f }, { 2, 1, 0.55f } } },
    { 2, { { 0, -1, 0.90f }, { 0, 1, 0.60f }, { 0, 0, 0.f } } },
    { 1, { { 2, 0, 1.00f }, { 0, 0, 0.f }, { 0, 0, 0.f } } } };

struct Dly {                                  // fixed delay line
    std::vector<float> b; size_t mask = 0, w = 0, d = 0;
    void init(size_t n) { size_t k = 1; while (k < n) k <<= 1; b.assign(k, 0.f); mask = k - 1; w = 0; d = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.f); w = 0; }
    void set(size_t dd) { d = dd > mask ? mask : dd; }
    inline float proc(float x) { b[w & mask] = x; float y = b[(w - d) & mask]; w++; return y; }
};

// Pitch tracker (YIN on a 12 kHz copy, 10 ms hop) + scale/key logic + Auto-Tune correction + chord-voice pitch shifts.
struct Tuner {
    static const int W = 480, NB = 2048;
    float fs = 48000.f, fsd = 12000.f; int dec = 4, hopS = 480, hopLeft = 480, decCnt = 0, tauMin = 12, tauMax = 171, rw = 0;
    float ring[NB], win[1024], d[260], cm[260]; Biquad lp;
    int64_t anaNs = 0;                                     // time spent in the pitch analysis (CPU meter per stage)
    bool adaptive = false; float keepVib = 0.f, lastT = 0.f, slow1 = 0.f, slow2 = 0.f; int curW = 480; bool wasVoiced = false;   // Phase 2
    // parameters (written by Engine before each call)
    bool tuneOn = false; int key = 12, scale = 1, chord = 0; float timeMs = 40.f, amt = 0.9f, presetSemi = 0.f;
    // state / results
    bool voiced = false; int voicedRun = 0, unvoicedRun = 0, jumpRun = 0, root = 0, autoTonic = 0, hops = 0;
    float midi = 0.f, conf = 0.f, lastMidi = 0.f, med[3], target = -1.f, desired = 0.f, corr = 0.f, hist[12], shiftV[3];

    void init(float f) {
        fs = f; dec = std::max(1, (int)floorf(fs / 12000.f + 0.5f)); fsd = fs / (float)dec; hopS = std::max(64, (int)floorf(fs * 0.010f + 0.5f));
        tauMin = std::max(2, (int)(fsd / 1000.f)); tauMax = std::min(255, (int)(fsd / 70.f)); lp.lowpass(fs, std::min(3500.0, fsd * 0.4), 0.7071);
        reset();
    }
    void reset() { flush(); for (int k = 0; k < 12; k++) hist[k] = 0.f; autoTonic = 0; hops = 0; }
    void flush() {
        std::fill(ring, ring + NB, 0.f); lp.clear(); rw = 0; decCnt = 0; hopLeft = hopS; voiced = false; voicedRun = unvoicedRun = jumpRun = 0;
        midi = lastMidi = 0.f; conf = 0.f; med[0] = med[1] = med[2] = 0.f; target = -1.f; desired = corr = 0.f; shiftV[0] = shiftV[1] = shiftV[2] = 0.f;
        lastT = 0.f; slow1 = slow2 = 0.f; curW = W; wasVoiced = false;
    }
    inline void push(float x) {
        float y = lp.proc(x);
        if (++decCnt >= dec) { decCnt = 0; ring[rw & (NB - 1)] = y; rw++; }
        if (--hopLeft <= 0) { hopLeft = hopS; const auto a0 = std::chrono::steady_clock::now(); analyse(); anaNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a0).count(); }
    }
    int windowLen() const {                                 // YIN window (samples at the analysis rate): fixed 480, or ~2 periods of the last pitch (min 20 ms) when adaptive
        if (!adaptive || lastT <= 0.f) return W;
        int w = std::max((int)(0.020f * fsd + 0.5f), (int)(2.f * lastT + 0.5f)); return w > W ? W : w;
    }
    bool detect(float& m, float& cf) {                      // YIN: returns true and the midi pitch when the frame is voiced
        const int Wn = windowLen(); curW = Wn;
        const int L = Wn + tauMax + 1;
        for (int i = 0; i < L; i++) win[i] = ring[(rw - L + i) & (NB - 1)];
        double e = 0; for (int i = L - Wn; i < L; i++) e += (double)win[i] * win[i];
        if (sqrt(e / Wn) < 0.004) return false;
        float run = 0.f; cm[0] = 1.f;
        for (int tau = 1; tau <= tauMax; tau++) {
            float sum = 0.f; for (int j = 0; j < Wn; j++) { float df = win[j] - win[j + tau]; sum += df * df; }
            d[tau] = sum; run += sum; cm[tau] = run > 1e-12f ? sum * tau / run : 1.f;
        }
        int best = -1;
        for (int tau = tauMin; tau < tauMax; tau++) if (cm[tau] < 0.15f) { while (tau + 1 < tauMax && cm[tau + 1] < cm[tau]) tau++; best = tau; break; }
        if (best < 0) { float mn = 1e9f; for (int tau = tauMin; tau < tauMax; tau++) if (cm[tau] < mn) { mn = cm[tau]; best = tau; } if (mn > 0.30f) return false; }
        float tp = (float)best;
        if (best > 1 && best < tauMax) { float s0 = cm[best - 1], s1 = cm[best], s2 = cm[best + 1], den = 2.f * (2.f * s1 - s2 - s0); if (fabsf(den) > 1e-9f) tp += clampf((s2 - s0) / den, -1.f, 1.f); }
        float f0 = fsd / tp; if (f0 < 60.f || f0 > 1100.f) return false;
        lastT = tp;
        m = 69.f + 12.f * log2f(f0 / 440.f); cf = clampf(1.f - cm[best], 0.f, 1.f); return true;
    }
    void pickKey() {                                         // Krumhansl-Schmuckler: correlate the note histogram with 12 major + 12 minor key profiles
        static const float pmaj[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
        static const float pmin[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
        float tot = 0.f; for (int k = 0; k < 12; k++) tot += hist[k];
        if (tot < 1.2f) return;
        const float hm = tot / 12.f; float hv = 0.f; for (int k = 0; k < 12; k++) hv += (hist[k] - hm) * (hist[k] - hm);
        if (hv < 1e-6f) return;
        float best = -2.f, cur = -2.f; int bt = autoTonic;
        for (int mode = 0; mode < 2; mode++) {
            const float* pr = mode ? pmin : pmaj; float pm = 0.f; for (int k = 0; k < 12; k++) pm += pr[k]; pm /= 12.f; float pv = 0.f; for (int k = 0; k < 12; k++) pv += (pr[k] - pm) * (pr[k] - pm);
            for (int t = 0; t < 12; t++) {
                float c = 0.f; for (int pc = 0; pc < 12; pc++) c += (hist[(t + pc) % 12] - hm) * (pr[pc] - pm);
                c /= sqrtf(hv * pv);
                const int maj = mode ? (t + 3) % 12 : t;              // minor keys are stored as their relative major (same notes)
                if (maj == autoTonic && c > cur) cur = c;
                if (c > best) { best = c; bt = maj; }
            }
        }
        if (bt != autoTonic && best > cur + 0.03f) autoTonic = bt;
    }
    void analyse() {
        float m = 0.f, cf = 0.f; bool ok = detect(m, cf);
        if (ok) {                                           // reject one-frame octave jumps, then median-of-3
            if (voiced && fabsf(m - lastMidi) > 6.5f) { if (++jumpRun < 3) ok = false; else jumpRun = 0; } else jumpRun = 0;
        }
        if (ok) {
            if (!voiced) { med[0] = med[1] = med[2] = m; } else { med[0] = med[1]; med[1] = med[2]; med[2] = m; }
            float a = med[0], b = med[1], c = med[2]; midi = std::max(std::min(a, b), std::min(std::max(a, b), c)); conf = cf;
            lastMidi = midi; unvoicedRun = 0; if (++voicedRun >= 2) voiced = true;
        } else { voicedRun = 0; if (++unvoicedRun >= 4) { voiced = false; if (unvoicedRun > 25) target = -1.f; } }
        for (int k = 0; k < 12; k++) hist[k] *= 0.9985f;
        if (ok && voiced) { int pc = ((int)floorf(midi + 0.5f) % 12 + 12) % 12; hist[pc] += conf; }
        if ((++hops % 50) == 0) pickKey();
        const int sc = scale < 0 ? 0 : (scale > 7 ? 7 : scale);
        root = key < 12 ? key : (autoTonic + kRootOffset[sc]) % 12;
        const uint16_t mask = scaleMask(sc);
        if (voiced && ok) {
            const float outMidi = midi + presetSemi;
            float noteSrc = outMidi, fast = 0.f;
            if (keepVib > 0.f) {                            // split the pitch into a slow part (the note) and a fast part (vibrato, 4-8 Hz)
                if (!wasVoiced || fabsf(outMidi - slow2) > 1.2f) slow1 = slow2 = outMidi;             // a real note change re-anchors the slow part
                else { const float aS = 1.f - expf(-0.010f / 0.080f); slow1 += (outMidi - slow1) * aS; slow2 += (slow1 - slow2) * aS; }
                noteSrc = slow2; fast = outMidi - slow2;
            }
            const float cand = nearestNote(noteSrc, root, mask, target); target = cand;
            desired = tuneOn ? clampf((cand - outMidi) * amt + amt * keepVib * fast, -3.f, 3.f) : 0.f;   // keepVib 0 = previous behaviour
            if (chord > 0) {
                const ChordDef& cd = kChord[chord]; const uint16_t hm = scaleMask(kHarmScale[sc]);
                for (int v = 0; v < cd.n; v++) { int note = stepUp((int)cand, cd.v[v].steps, root, hm) + 12 * cd.v[v].oct; shiftV[v] = (float)note - midi; }
            }
        } else desired = tuneOn ? desired * 0.9f : 0.f;
        if (timeMs <= 1.f) corr = desired; else corr += (desired - corr) * (1.f - expf(-10.f / timeMs));
        wasVoiced = voiced && ok; g_tuneWinMs = 1000.f * (float)curW / fsd;
        g_tuneDet = voiced ? midi : -1.f; g_tuneTgt = (voiced && target > 0.f) ? target + 0.f : -1.f; g_tuneRoot = root; g_tuneVoiced = voiced ? 1 : 0;
    }
};


// ================================================================== Phase 3: hear-myself monitor servo (pure logic, no BASS calls: main.cpp executes the decisions)
// Called once per mic chunk with the audio already queued in the BASS push stream. V1: never drops chunks. A small playback-rate change
// (BASS_ATTRIB_FREQ, at most +-1 %, smoothed) keeps the queue at the target (~50 ms, a little more for very long mic chunks); only if the
// queue exceeds ~200 ms the stream is reset (a stall: network/app hitch). legacy: the previous behaviour (drop above 0.25 s, start at 80 ms).
struct MonitorServo {
    bool primed = false; float ratio = 1.f, sentRatio = 1.f, chunkMs = 0.f, qMs = 0.f;
    struct Out { bool drop = false, hardReset = false, startPlay = false, freqChanged = false; float freqRatio = 1.f; };
    void reset() { primed = false; ratio = sentRatio = 1.f; chunkMs = 0.f; qMs = 0.f; }
    Out step(bool legacy, uint32_t queued, uint32_t len, uint32_t rate)
    {
        Out o; if (rate < 8000) rate = 8000;
        const float bpms = (float)rate * 2.f / 1000.f;                       // bytes per ms (16-bit mono)
        const float afterMs = ((float)queued + (float)len) / bpms, beforeMs = (float)queued / bpms, cm = (float)len / bpms;
        qMs = chunkMs == 0.f ? afterMs - 0.5f * cm : qMs + ((beforeMs + 0.5f * cm) - qMs) * 0.1f;
        chunkMs = chunkMs == 0.f ? cm : chunkMs + (cm - chunkMs) * 0.2f;
        g_monQueueMs = qMs;
        if (primed && queued == 0) g_monUnder++;
        if (legacy) {
            if (queued > rate / 2) { o.drop = true; return o; }                // already >0.25 s queued: drop
            if (!primed && queued + len >= rate * 2 * 8 / 100) { primed = true; o.startPlay = true; }   // 80 ms of audio
            ratio = 1.f;
        } else {
            const float target = clampf(std::max(50.f, 0.5f * chunkMs + 25.f), 40.f, 120.f);     // mean queue we aim for
            const float aim = target + 0.5f * chunkMs;                                            // level right after a push
            if (!primed) { if (afterMs >= aim) { primed = true; o.startPlay = true; } ratio = 1.f; }
            else if (afterMs > 200.f + 0.5f * chunkMs) { o.hardReset = true; primed = false; ratio = 1.f; g_monResets++; }
            else { const float want = clampf((afterMs - aim) * 0.00025f, -0.01f, 0.01f); ratio += ((1.f + want) - ratio) * 0.1f; }
        }
        g_monRatioPct = (ratio - 1.f) * 100.f;
        if (fabsf(ratio - sentRatio) > 2e-5f) { sentRatio = ratio; o.freqChanged = true; }
        o.freqRatio = sentRatio;
        return o;
    }
};

const size_t kSub = 2048;

struct Engine {
    std::mutex mtx;
#ifndef VFX_NO_RUBBERBAND
    std::unique_ptr<RubberBand::RubberBandLiveShifter> rb;
#endif
    bool ready = false;
    size_t B = 0, rate = 48000, rbDelay = 0;
    Ring inq, outq;
    std::vector<float> inBlk, outBlk, xin, wet, dryv, dline; size_t dmask = 0, dw = 0, dryDelay = 0;
    LitePitch lite; FxChain fx; NoiseSuppressor ns;
    // Phase 1
    InputCond cond; GateV1 gateN; Compressor comp; DeEsser dees; Limiter lim; Dly la; size_t laN = 240;
    float v1 = 1.f, cmW = 0.f, dsW = 0.f;          // V1 <-> legacy cross-fade weight (1 = V1), compressor / de-esser fade weights
    std::vector<float> v1buf;
    AiNs ai; float aiW = 0.f; bool aiPrev = false; int swWarm = 0; Psola ps[3]; float psW[3] = { 0.f, 0.f, 0.f };   // Phase 4
    // Phase 5: per-stage CPU timing, automatic return from Lite to RubberBand
    int64_t stg[4] = { 0, 0, 0, 0 }, anaMark = 0, liteSamples = 0, probeSamples = 0, probeN = 0; double probeSum = 0.0; bool probing = false; float simLoadRb = 0.f;   // simLoadRb: test hook, 0 in production
    float stgAvg[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
    int gapMs = 120;                                   // a pause between DSP calls longer than this resets the engine state (tests raise it: a loaded CI machine can stall)
    std::vector<float> lkb, xinD; size_t lkmask = 0, lkw = 0; float lkD = 0.f;       // Phase 2: tune lookahead line (delayed copy for the pitch engines)
    bool nsActive = false; float nsM = 0.f; int nsWarm = 0;
    Tuner tuner; LitePitch harm[3]; Dly cdelay; std::vector<float> chordv;
    int chordCur = 0; bool chordFresh = false; float chG = 0.f, vg = 0.f, hr[3] = { 1.f, 1.f, 1.f };
    int engine = 0;                 // running pitch engine: 0 none (zero latency), 1 RubberBand, 2 lite
    float xf = 0.f;                 // 0 = direct path, 1 = pitch path (cross-faded)
    bool instant = true, autoLite = false;
    float lastPitch = -1.f, lastForm = -1.f, lastW = 0.f, env = 0.f, gateG = 1.f, load = 0.f;
    int overStreak = 0, epoch = 0;
    std::chrono::steady_clock::time_point last; bool haveLast = false;

    bool rebuild(size_t sr)         // called from the RecordStart hook thread, never from the DSP path
    {
        std::lock_guard<std::mutex> lk(mtx);
#ifndef VFX_NO_RUBBERBAND
        rb.reset();
        rb.reset(new RubberBand::RubberBandLiveShifter(sr, 1,
                 RubberBand::RubberBandLiveShifter::OptionWindowShort |
                 RubberBand::RubberBandLiveShifter::OptionFormantShifted |
                 RubberBand::RubberBandLiveShifter::OptionChannelsApart));
        rate = sr; B = rb->getBlockSize();
#else
        rate = sr; B = 512;
#endif
        inBlk.assign(B, 0.f); outBlk.assign(B, 0.f);
        xin.assign(kSub, 0.f); wet.assign(kSub, 0.f); dryv.assign(kSub, 0.f);
        inq.init(65536); outq.init(65536);
#ifndef VFX_NO_RUBBERBAND
        size_t sd = rb->getStartDelay(); rbDelay = B + sd; if (rbDelay > 16384) rbDelay = 16384;
#endif
        size_t n = 1; while (n < 32768) n <<= 1; dline.assign(n, 0.f); dmask = n - 1; dw = 0;
        lite.init((float)sr); fx.init((float)sr); ns.init((float)sr); tuner.init((float)sr);
        for (auto& h : harm) h.init((float)sr);
        for (auto& q : ps) q.init((int)(0.5f * harm[0].Wdef) + 2);
        ai.create();
        cdelay.init(16384); chordv.assign(kSub, 0.f); v1buf.assign(kSub, 1.f); xinD.assign(kSub, 0.f); lkb.assign(8192, 0.f); lkmask = 8191; lkw = 0; lkD = 0.f;
        cond.init((float)sr); dees.init((float)sr); lim.init((float)sr); la.init(1024); laN = (size_t)floorf(0.005f * (float)sr + 0.5f); la.set(laN);
        ready = true;
        softReset();
        g_block = (int)B; g_rate = (int)sr;
        return true;
    }
    void softReset()                // caller holds mtx (or is single-threaded): flush everything that could replay old audio
    {
#ifndef VFX_NO_RUBBERBAND
        if (rb) rb->reset();
#endif
        inq.clear(); outq.clear(); lite.reset(); fx.reset(); ns.flush(); nsActive = false; nsM = 0.f; nsWarm = 0;
        ai.flush(); aiW = 0.f; aiPrev = false; swWarm = 0; for (int v = 0; v < 3; v++) { ps[v].reset(); psW[v] = 0.f; }
        tuner.flush(); for (auto& h : harm) h.reset(); cdelay.clear(); chordCur = 0; chG = 0.f; vg = 0.f; for (int v = 0; v < 3; v++) hr[v] = 1.f;
        std::fill(dline.begin(), dline.end(), 0.f); dw = 0;
        probing = false; liteSamples = 0; engine = 0; xf = 0.f; instant = true; lastPitch = lastForm = -1.f; lastW = 0.f; env = 0.f; gateG = 1.f; load = 0.f; overStreak = 0;
        std::fill(lkb.begin(), lkb.end(), 0.f); lkw = 0;
        lkD = (g_par[P_LEGACY] >= 0.5f) ? 0.f : floorf(g_par[P_TUNE_LOOK] * 0.001f * (float)(rate ? rate : 48000) + 0.5f);
        cond.unprime(); gateN.reset(); comp.reset(); dees.reset(); lim.reset(); la.clear();
        v1 = (g_par[P_LEGACY] >= 0.5f) ? 0.f : 1.f; cmW = (v1 > 0.f && g_par[P_COMP_ON] >= 0.5f) ? 1.f : 0.f; dsW = (v1 > 0.f && g_par[P_DEESS_ON] >= 0.5f) ? 1.f : 0.f;
        g_engineMode = 0; g_latMs = 1000.f * (float)(v1 > 0.5f ? (laN + (size_t)lim.D) : 0) / (float)(rate ? rate : 48000);
    }
    void reset() { std::lock_guard<std::mutex> lk(mtx); softReset(); }

    void startEngine(int e)
    {
        engine = e;
#ifndef VFX_NO_RUBBERBAND
        if (e == 1) { rb->reset(); inq.clear(); outq.clear(); for (size_t i = 0; i < B; i++) outq.push(0.f); dryDelay = rbDelay; }
        else
#endif
        { lite.reset(); dryDelay = lite.latency(); }
        lastPitch = lastForm = -1.f;
        if (instant) { xf = 1.f; instant = false; }          // first start after a gap: never let the raw voice through
        g_engineMode = e; g_latMs = 1000.f * ((float)dryDelay + (v1 > 0.5f ? (float)(laN + (size_t)lim.D) : 0.f)) / (float)rate;
    }

    void process(int16_t* s, int n)   // in-place 16-bit mono, any n
    {
        std::unique_lock<std::mutex> lk(mtx, std::try_to_lock);
        if (!lk.owns_lock() || !ready || n <= 0) return;            // being rebuilt: pass audio through untouched
        auto t0 = std::chrono::steady_clock::now();
        stg[0] = stg[1] = stg[2] = stg[3] = 0; anaMark = tuner.anaNs;
        if (haveLast && std::chrono::duration_cast<std::chrono::milliseconds>(t0 - last).count() > gapMs) { softReset(); g_gapResets++; }
        int ep = g_qualityEpoch.load(); if (ep != epoch) { epoch = ep; autoLite = false; g_autoLite = 0; probing = false; liteSamples = 0; }

        const float pitch = g_par[P_PITCH], form = g_par[P_FORMANT];
        const bool preserve = g_par[P_PRESERVE] >= 0.5f;
        const float effForm = clampf(form * (preserve ? 1.f / pitch : 1.f), 0.1f, 8.f);
        const float gainLin = powf(10.f, g_par[P_GAIN_DB] / 20.f);
        const bool gateOn = g_par[P_GATE_DB] > -79.5f;
        const float gateThr = powf(10.f, g_par[P_GATE_DB] / 20.f);
        const float mix = g_par[P_MIX];
        // Phase 1 parameters (one read per call)
        const bool legacyT = g_par[P_LEGACY] >= 0.5f; const float v1T = legacyT ? 0.f : 1.f;
        const float v1Step = 1.f / (0.015f * (float)rate);
        gateN.set(gateThr, g_par[P_GATE_HYST], g_par[P_GATE_RANGE], g_par[P_GATE_HOLD], (float)rate);
        const bool compT = !legacyT && g_par[P_COMP_ON] >= 0.5f, deessT = !legacyT && g_par[P_DEESS_ON] >= 0.5f;
        comp.set(g_par[P_COMP_THRESH], g_par[P_COMP_RATIO], g_par[P_COMP_ATTACK], g_par[P_COMP_RELEASE], g_par[P_COMP_MAKEUP], g_par[P_COMP_AUTO] >= 0.5f, (float)rate);
        const float deessAmt = g_par[P_DEESS_AMT]; dees.setFreq(g_par[P_DEESS_FREQ]);
        const float ceilLin = powf(10.f, g_par[P_LIM_CEIL] / 20.f);
        // Phase 2: tune lookahead (delay of the signal that feeds the pitch engines; the analysis sees the un-delayed signal)
        const float lkT = legacyT ? 0.f : floorf(g_par[P_TUNE_LOOK] * 0.001f * (float)rate + 0.5f);
        tuner.adaptive = !legacyT; tuner.keepVib = legacyT ? 0.f : g_par[P_TUNE_VIB].load();
        const bool nsOn = g_par[P_NS_ON] >= 0.5f; const float nsAmt = g_par[P_NS_AMT], nsClick = g_par[P_NS_CLICK];
        if (nsOn && !nsActive) { ns.flush(); nsActive = true; nsWarm = (int)ns.N; nsM = 0.f; }
        const int quality = (int)g_par[P_QUALITY];
        const bool tuneOn = g_par[P_TUNE_ON] >= 0.5f; const int chordWant = (int)g_par[P_CHORD];
        const float chordMix = g_par[P_CHORD_MIX], spread = g_par[P_CHORD_SPREAD];
        tuner.tuneOn = tuneOn; tuner.key = (int)g_par[P_TUNE_KEY]; tuner.scale = (int)g_par[P_TUNE_SCALE];
        tuner.timeMs = g_par[P_TUNE_TIME]; tuner.amt = g_par[P_TUNE_AMT]; tuner.presetSemi = 12.f * log2f(pitch);
        const bool vocFollow = !legacyT && (int)g_par[P_VOC_MODE] == 2 && (int)g_par[P_VOC_CARRIER] >= 1;      // vocoder carrier follows the voice: the tuner has to run
        const bool tunerRun = tuneOn || chordWant > 0 || chordCur > 0 || fabsf(tuner.corr) > 0.01f || vocFollow;
        const bool wantsTune = tuneOn || chordWant > 0 || chordCur > 0 || fabsf(tuner.corr) > 0.01f;   // keep the pitch engine alive for tune / chords
#ifndef VFX_NO_RUBBERBAND
        const bool needShift = fabsf(pitch - 1.f) > 1e-3f || fabsf(effForm - 1.f) > 1e-3f || wantsTune;
#else
        (void)effForm; const bool needShift = fabsf(pitch - 1.f) > 1e-3f || wantsTune;       // Lite has no formant control
#endif
#ifndef VFX_NO_RUBBERBAND
        const bool useLite = quality == 2 || (quality == 0 && autoLite);
#else
        const bool useLite = true;                          // built without RubberBand: Lite is the only pitch engine
#endif
        const int desired = needShift ? (useLite ? 2 : 1) : 0;

        Snap sn;
        sn.hp = g_par[P_HP_HZ]; sn.lp = g_par[P_LP_HZ]; sn.bass = g_par[P_BASS_DB]; sn.treble = g_par[P_TREBLE_DB];
        sn.drive = g_par[P_DRIVE]; sn.bits = g_par[P_BITS]; sn.down = g_par[P_DOWN]; sn.tremHz = g_par[P_TREM_HZ]; sn.tremDepth = g_par[P_TREM_DEPTH];
        sn.modMode = (int)g_par[P_MOD_MODE]; sn.modRate = g_par[P_MOD_RATE]; sn.modDepth = g_par[P_MOD_DEPTH];
        sn.echoMs = g_par[P_ECHO_MS]; sn.echoFb = g_par[P_ECHO_FB]; sn.echoMix = g_par[P_ECHO_MIX];
        sn.revSize = g_par[P_REV_SIZE]; sn.revMix = g_par[P_REV_MIX];
        sn.robot = g_par[P_ROBOT] >= 0.5f; sn.robotHz = g_par[P_ROBOT_HZ]; sn.robotAmt = g_par[P_ROBOT_AMT];
        sn.vocMode = (int)g_par[P_VOC_MODE]; sn.vocHz = g_par[P_VOC_HZ];
        sn.echoDamp = legacyT ? 0.f : g_par[P_ECHO_DAMP].load(); sn.echoHp = legacyT ? 0.f : g_par[P_ECHO_HP].load(); sn.revPre = g_par[P_REV_PREDELAY]; sn.revDamp = g_par[P_REV_DAMP];
        const bool aiAvail = AiNs::built() && rate == 48000 && ai.usable();
        const bool aiWant = aiAvail && !legacyT && g_par[P_NS_ENGINE] >= 0.5f;
        if (aiWant != aiPrev) { aiPrev = aiWant; swWarm = 640; if (aiWant) ai.flush(); else ns.flush(); }       // both engines run during the hand-over, then a 10 ms cross-fade
        const float formantT = legacyT ? 0.f : g_par[P_CHORD_FORMANT].load();
        fx.update(sn);

        const float xfStep = 1.f / (0.010f * (float)rate), nsStep = xfStep;
        float pin = 0.f, pout = 0.f;
        const float hA = 1.f - expf(-1.f / (0.012f * (float)rate)), vgA = 1.f - expf(-1.f / (0.015f * (float)rate));
        for (int base = 0; base < n; ) {
            int m = std::min<int>((int)kSub, n - base); if (tunerRun && tuner.hopLeft < m) m = tuner.hopLeft;   // segments end on pitch-analysis hops: result does not depend on chunk size
            int16_t* sp = s + base;
            if (chordCur != chordWant && chG <= 0.001f) {          // chord mode change: fade out, switch, fade in
                chordCur = chordWant; chordFresh = true; for (auto& h : harm) h.reset(); cdelay.clear(); for (int v = 0; v < 3; v++) hr[v] = 1.f;
            }
            tuner.chord = chordCur;
            const float chT = (chordCur == chordWant && chordCur > 0) ? 1.f : 0.f;
            // results of the pitch analysis as they were when this segment started (a segment never contains an analysis hop boundary)
            const float sCorr = tuner.corr, sMidi = tuner.midi; const bool sVoiced = tuner.voiced; const int sHops = tuner.hops;
            const float sShift[3] = { tuner.shiftV[0], tuner.shiftV[1], tuner.shiftV[2] };
            float carHz = 0.f;
            if (vocFollow && sVoiced) { const float mo = ((int)g_par[P_VOC_CARRIER] == 2 && tuner.target > 0.f) ? tuner.target : (sMidi + tuner.presetSemi); carHz = clampf(440.f * exp2f((mo - 69.f) * (1.f / 12.f)), 50.f, 1200.f); }
            fx.p.carHz = carHz;
            const float t0s = (sVoiced && tunerRun) ? (float)rate / (440.f * exp2f((sMidi - 69.f) * (1.f / 12.f))) : 0.f;   // voice period in samples
            // engine state machine (once per sub-block): fade the old engine out, then start the wanted one
            if (engine != desired && (engine == 0 || xf <= 0.0005f)) { if (desired == 0) { engine = 0; g_engineMode = 0; g_latMs = 1000.f * (v1 > 0.5f ? (float)(laN + (size_t)lim.D) : 0.f) / (float)rate; } else startEngine(desired); }
            const float xfTarget = (engine != 0 && engine == desired) ? 1.f : 0.f;

            const auto ts0 = std::chrono::steady_clock::now();
            // 1) input stage: int16 -> float, gate (smoothed), gain; the dry line always runs so a delayed dry copy is always aligned
            for (int i = 0; i < m; i++) {
                float x = sp[i] * (1.f / 32768.f);
                float ax = fabsf(x); if (ax > pin) pin = ax;
                v1 += clampf(v1T - v1, -v1Step, v1Step); v1buf[i] = v1;                 // legacy <-> V1 cross-fade (15 ms)
                if (v1 > 0.f) {                                                          // DC blocker + 75 Hz high-pass, before everything else
                    float xc = cond.proc(x); if (!(xc == xc)) { cond.unprime(); cond.hp.clear(); xc = 0.f; }
                    x = (v1 >= 1.f) ? xc : x + v1 * (xc - x); ax = fabsf(x);
                } else cond.unprime();
                if (nsActive) {                      // noise suppression first: everything after it works on the cleaned mic
                    if (swWarm > 0) swWarm--; else aiW += clampf((aiWant ? 1.f : 0.f) - aiW, -nsStep, nsStep);
                    const bool runC = !(aiW >= 1.f && aiWant), runA = aiAvail && (aiW > 0.f || aiWant);
                    const float yc = runC ? ns.proc(x, nsAmt, nsClick) : 0.f;
                    float ya = 0.f; if (runA) { const float yr = ai.proc(x); const float bl = 0.4f + 0.6f * nsAmt; ya = x + bl * (yr - x); }     // AI strength = wet/dry
                    float yn = aiW <= 0.f ? yc : (aiW >= 1.f ? ya : yc + aiW * (ya - yc));
                    float tg = (nsOn && nsWarm == 0) ? 1.f : 0.f; if (nsWarm > 0) nsWarm--;
                    nsM += clampf(tg - nsM, -nsStep, nsStep);
                    x = x * (1.f - nsM) + yn * nsM; ax = fabsf(x);
                    if (!nsOn && nsM <= 0.f) nsActive = false;
                }
                const float xpre = x;                                                    // after conditioning + noise suppression
                if (gateOn) {                                                            // legacy gate (used while Compare = legacy)
                    env = ax > env ? ax : env * 0.9995f;
                    float tg = env > gateThr ? 1.f : 0.f;
                    gateG += (tg - gateG) * (tg > gateG ? 0.05f : 0.002f);
                    x *= gateG;
                }
                x *= gainLin;
                {   // V1 branch: lookahead line (always runs, so it is warm), new gate, gain, compressor
                    float xd = la.proc(xpre), xv = xd;
                    if (gateOn) xv = xd * gateN.proc(xpre);
                    xv *= gainLin;
                    if (v1 > 0.f) {
                        cmW += clampf((compT ? 1.f : 0.f) - cmW, -v1Step, v1Step);
                        if (cmW > 0.f) { float gc = comp.proc(xv); xv *= 1.f + cmW * (gc - 1.f); }
                        x = (v1 >= 1.f) ? xv : x + v1 * (xv - x);
                    } else cmW = 0.f;
                }
                xin[i] = x;
                if (tunerRun) tuner.push(x);
                dline[dw & dmask] = x;
                {   // lookahead line (slews 0.25 samples per sample when the setting changes: no click); integer delay = exact copy
                    lkb[lkw & lkmask] = x; lkw++;
                    if (lkD != lkT) lkD += clampf(lkT - lkD, -0.25f, 0.25f);
                    const size_t di = (size_t)lkD; const float fr = lkD - (float)di;
                    xinD[i] = fr == 0.f ? lkb[(lkw - 1 - di) & lkmask] : lkb[(lkw - 1 - di) & lkmask] * (1.f - fr) + lkb[(lkw - 2 - di) & lkmask] * fr;
                }
                dryv[i] = dline[(dw - dryDelay - (size_t)(lkD + 0.5f)) & dmask]; dw++;
            }
            const auto ts1 = std::chrono::steady_clock::now(); stg[0] += std::chrono::duration_cast<std::chrono::nanoseconds>(ts1 - ts0).count();
            // 2) pitch engine: preset pitch x Auto-Tune correction (updated every 10 ms)
            const float leadRatio = clampf(pitch * exp2f(sCorr * (1.f / 12.f)), 0.25f, 4.f);
#ifndef VFX_NO_RUBBERBAND
            if (engine == 1) {
                if (needShift) {
                    if (lastPitch < 0.f || fabsf(leadRatio - lastPitch) > 1e-5f * lastPitch) { rb->setPitchScale(leadRatio); lastPitch = leadRatio; }
                    if (effForm != lastForm) { rb->setFormantScale(effForm); lastForm = effForm; }
                }
                for (int i = 0; i < m; i++) inq.push(xinD[i]);
                while (inq.n >= B) {
                    for (size_t i = 0; i < B; i++) inBlk[i] = inq.pop();
                    const float* ip[1] = { inBlk.data() }; float* op[1] = { outBlk.data() };
                    rb->shift(ip, op);
                    for (size_t i = 0; i < B; i++) outq.push(outBlk[i]);
                }
                for (int i = 0; i < m; i++) { if (outq.n) lastW = outq.pop(); else g_underruns++; wet[i] = lastW; }
            } else
#endif
            if (engine == 2) {
                lite.ratio = leadRatio; lite.setPeriod(t0s);
                for (int i = 0; i < m; i++) wet[i] = lite.proc(xinD[i]);
            }
            const auto ts2 = std::chrono::steady_clock::now(); stg[1] += std::chrono::duration_cast<std::chrono::nanoseconds>(ts2 - ts1).count();
            // 2b) chord voices: three light pitch shifters following the detected, scale-snapped note
            if (chordCur > 0) {
                const ChordDef& cd = kChord[chordCur]; float tg[3] = { 1.f, 1.f, 1.f };
                const size_t own = (size_t)(0.5f * harm[0].Wdef) + 2, lead = (engine == 1) ? dryDelay : (engine == 2 ? own : 0);   // Lite lead and Lite voices share the same adaptive grain, so they stay aligned by themselves
                cdelay.set(lead > own ? lead - own : 0);           // line the voices up with the lead voice (fixed amount: no zipper noise)
                const float tsec = (float)sHops * 0.010f;
                for (int v = 0; v < cd.n; v++) {
                    float det = spread * 0.01f * sinf(kTwoPi * (0.23f + 0.17f * (float)v) * tsec + 2.1f * (float)v);   // slow detune = choir-like width
                    tg[v] = exp2f((sShift[v] + det) * (1.f / 12.f));
                    harm[v].setPeriod(t0s);
                    if (chordFresh) hr[v] = tg[v];
                }
                chordFresh = false;
                const float vgT = sVoiced ? 1.f : 0.f;
                const float psT = (formantT > 0.f && t0s >= 20.f && t0s <= ps[0].maxT()) ? formantT : 0.f;      // formant-preserving voice only where the period is reliable
                for (int i = 0; i < m; i++) {
                    chG += clampf(chT - chG, -nsStep, nsStep); vg += (vgT - vg) * vgA;
                    float cs = 0.f;
                    for (int v = 0; v < cd.n; v++) {
                        hr[v] += (tg[v] - hr[v]) * hA; harm[v].ratio = clampf(hr[v], 0.25f, 4.f);
                        float vo = harm[v].proc(xinD[i]);
                        psW[v] += (psT - psW[v]) * 0.003f;
                        if (psW[v] > 1e-4f) vo += psW[v] * (ps[v].proc(xinD[i], t0s, harm[v].ratio) - vo); else if (ps[v].synced) ps[v].reset();
                        cs += cd.v[v].gain * vo;
                    }
                    chordv[i] = cdelay.proc(cs) * (chG * vg * chordMix);
                }
            }
            const auto ts3 = std::chrono::steady_clock::now(); stg[2] += std::chrono::duration_cast<std::chrono::nanoseconds>(ts3 - ts2).count();
            // 3) cross-fade direct <-> pitch path, then the effect rack, limiter, back to int16
            for (int i = 0; i < m; i++) {
                float d = xfTarget - xf; xf += d > xfStep ? xfStep : (d < -xfStep ? -xfStep : d);
                float y = xin[i];
                if (engine != 0) { float pp = dryv[i] * (1.f - mix) + wet[i] * mix; y = y * (1.f - xf) + pp * xf; }
                if (chordCur > 0) y = y * (1.f - 0.12f * chordMix * chG) + chordv[i] * xf;
                // de-esser (after pitch + chords, before the effect rack)
                dsW += clampf((deessT ? 1.f : 0.f) - dsW, -v1Step, v1Step);
                if (dsW > 0.f) { float ye = dees.proc(y, deessAmt); if (!(ye == ye)) { dees.reset(); ye = y; } y += dsW * (ye - y); } else if (dees.g != 1.f) dees.reset();
                y = fx.process(y, v1buf[i]);
                if (!(y == y)) y = 0.f;
                // output limiter: V1 = lookahead limiter (always runs so its delay line is warm), legacy = the old tanh soft knee
                const float yv = lim.proc(y, ceilLin), w1 = v1buf[i];
                if (w1 >= 1.f) y = yv;
                else {
                    float yl = y, ay = fabsf(y);
                    if (ay > 0.9f) yl = copysignf(0.9f + 0.1f * tanhf((ay - 0.9f) * 10.f), y);   // old soft limiter
                    y = (w1 <= 0.f) ? yl : yl + w1 * (yv - yl);
                }
                if (fabsf(y) > pout) pout = fabsf(y);
                int v = (int)lrintf(y * 32767.f); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                sp[i] = (int16_t)v;
            }
            stg[3] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ts3).count();
            base += m;
        }
        if (nsActive) { g_nsFloor = ns.floorDb; g_nsRed = ns.redDb * nsM; } else g_nsRed = 0.f;
        g_gateG = gateOn ? 20.f * log10f(gateN.g + 1e-6f) : 0.f; g_compGR = cmW > 0.f ? comp.grdB * cmW : 0.f;
        g_deessGR = dsW > 0.f ? 20.f * log10f(1.f - dsW * (1.f - dees.g) + 1e-6f) : 0.f; g_limGR = 20.f * log10f(lim.lastGs + 1e-6f);
        const float nsLat = (v1 > 0.5f && nsActive) ? (aiW > 0.5f ? 960.f : (float)ns.N) : 0.f;       // noise suppression delay (classic 512, RNNoise 960 = measured: one frame of buffering + one inside the model), V1 only
        g_nsLatMs = 1000.f * nsLat / (float)rate;
        g_latMs = 1000.f * ((engine != 0 ? (float)dryDelay + lkD : 0.f) + (v1 > 0.5f ? (float)(laN + (size_t)lim.D) : 0.f) + nsLat) / (float)rate;
        float pi = g_peakIn, po = g_peakOut;
        g_peakIn = pin > pi * 0.85f ? pin : pi * 0.85f;
        g_peakOut = pout > po * 0.85f ? pout : po * 0.85f;

        // CPU meter, per-stage meter + automatic fallback / recovery
        auto t1 = std::chrono::steady_clock::now(); last = t1; haveLast = true;
        float dt = std::chrono::duration<float>(t1 - t0).count(), dur = (float)n / (float)rate;
        {   // stage meters: percent of real time, smoothed. 0 input (conditioning, noise suppression, gate, compressor), 1 pitch analysis, 2 pitch engine, 3 chords, 4 effects + limiter
            const int64_t ana = tuner.anaNs - anaMark; const double ns = (double)dur * 1e9;
            const float v[5] = { (float)(std::max<int64_t>(0, stg[0] - ana) / ns * 100.0), (float)(ana / ns * 100.0), (float)(stg[1] / ns * 100.0), (float)(stg[2] / ns * 100.0), (float)(stg[3] / ns * 100.0) };
            for (int k = 0; k < 5; k++) { stgAvg[k] += (v[k] - stgAvg[k]) * 0.1f; g_stagePct[k] = stgAvg[k]; }
        }
        float ratio = dt / dur; if (engine == 1) ratio += simLoadRb;
        load = load * 0.9f + ratio * 0.1f; g_load = load * 100.f;
        float lp = g_loadPeak; g_loadPeak = ratio * 100.f > lp * 0.995f ? ratio * 100.f : lp * 0.995f;
        if (ratio > 0.95f) { g_overruns++; overStreak++; } else overStreak = 0;
        if (quality == 0 && engine == 1 && !autoLite && !probing && (load > 0.75f || overStreak >= 3)) {
            autoLite = true; g_autoLite = 1; liteSamples = 0; vlog("CPU too high (load %.0f%%) -> switching to the lite pitch shifter", load * 100.f);
        }
#ifndef VFX_NO_RUBBERBAND
        // Recovery: while on Lite after an overload, try RubberBand again every ~20 s for a 4 s probe and stay on it only if the measured load
        // (after a 0.5 s warm-up) averages under 55 %. A probe that overloads is cut short, so it costs at most a short stretch of higher load.
        if (quality != 0) { probing = false; }
        else if (autoLite && !probing) {
            if (engine == 2) liteSamples += (int64_t)n;
            if (liteSamples >= 20 * (int64_t)rate) { probing = true; autoLite = false; liteSamples = 0; probeSamples = 0; probeN = 0; probeSum = 0.0; g_probes++; vlog("CPU recovery: trying the high-quality pitch engine again (probe)"); }
        } else if (probing) {
            probeSamples += (int64_t)n;
            const bool onRb = engine == 1;
            if (onRb && probeSamples > (int64_t)rate / 2) { probeSum += (double)ratio * (double)n; probeN += (int64_t)n; }
            const bool bad = onRb && (overStreak >= 2 || ratio > 0.9f);
            const bool done = probeSamples >= 4 * (int64_t)rate, never = !onRb && probeSamples > 2 * (int64_t)rate;
            if (bad || done || never) {
                const double mean = probeN > 0 ? probeSum / (double)probeN : 1.0; probing = false;
                if (!bad && !never && mean <= 0.55) { autoLite = false; g_autoLite = 0; g_recoveries++; load = (float)mean; vlog("CPU recovery: load %.0f%% on the high-quality engine, staying on it", mean * 100.0); }
                else { autoLite = true; g_autoLite = 1; liteSamples = 0; vlog("CPU recovery: probe failed (%s), staying on the lite pitch shifter", never ? "no pitch engine active" : (bad ? "overloaded" : "load too high")); }
            }
        }
#endif
    }
};
Engine g_eng;
}  // namespace

// Test hook / used by the BASS DSP callback.
extern "C" __attribute__((visibility("default"))) void vfx_process_i16(int16_t* buf, int n) { g_eng.process(buf, n); }
extern "C" __attribute__((visibility("default"))) void vfx_prepare(int rate) { g_eng.rebuild((size_t)rate); }

// ------------------------------------------------------------------ config file
static void cfg_save()
{
    FILE* f = fopen(CFG_PATH, "w"); if (!f) return;
    for (int i = 0; i < P_COUNT; i++) if (i != P_MONITOR && i != P_LEGACY) fprintf(f, "%s=%.4f\n", kPar[i].name, (double)g_par[i].load());
    fclose(f);
}
static void cfg_load()
{
    FILE* f = fopen(CFG_PATH, "r");
    bool imported = false;
    if (!f) { f = fopen(OLD_CFG_PATH, "r"); imported = (f != nullptr); }   // first run after the rename: import the old settings once
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        for (int i = 0; i < P_COUNT; i++) if (i != P_MONITOR && i != P_LEGACY && !strcmp(line, kPar[i].name)) g_par[i] = sanitize(i, (float)atof(eq + 1));
    }
    fclose(f);
    if (imported) { g_cfgImported = 1; cfg_save(); vlog("settings imported from %s (old file kept)", OLD_CFG_PATH); }
}

// ------------------------------------------------------------------ public C API for the menu (Lua FFI)
VFX_EXPORT float vfx_get(int id)  { return (id >= 0 && id < P_COUNT) ? g_par[id].load() : 0.f; }
static void (*g_onParam)(int id) = nullptr;   // set on Android: (re)creates / frees the monitor stream
VFX_EXPORT void  vfx_set(int id, float v)
{
    if (id < 0 || id >= P_COUNT) return;
    g_par[id] = sanitize(id, v);
    if (id == P_QUALITY) g_qualityEpoch++;
    if (g_onParam) g_onParam(id);
}
VFX_EXPORT void  vfx_save(void) { cfg_save(); }
VFX_EXPORT void  vfx_load(void) { cfg_load(); }
// stat ids: 0 hook(1/0/-1) 1 recordStarts 2 dspCalls 3 lastChunkBytes 4 rate 5 formatOK 6 blockSize 7 monitor(1/0/-1)
//           8 api version (9)  9 underruns 10 gap resets 11 overruns (call slower than real time) 12 engine (0 none/1 RubberBand/2 lite) 13 auto-lite active 14 RubberBand compiled in (1/0) 15 key root in use (0..11) 16 voice detected (1/0) 17 product version x100 (100 = 1.0) 18 old libvoicefx.so found (1/0) 19 settings imported from voicefx.cfg (1/0) 20 legacy_mode active (1/0) 21 hear-myself hard resets 22 hear-myself underruns (queue ran empty) 23 RNNoise built in (1/0) 24 CPU-recovery probes started 25 CPU recoveries (back on RubberBand)
VFX_EXPORT int   vfx_stat(int id)
{
    switch (id) { case 0: return g_hookState; case 1: return g_recStarts; case 2: return g_dspCalls;
                  case 3: return g_lastBytes; case 4: return g_rate; case 5: return g_fmtOK; case 6: return g_block; case 7: return g_monState;
                  case 8: return VFX_API_VERSION; case 9: return g_underruns; case 10: return g_gapResets; case 11: return g_overruns;
                  case 12: return g_engineMode; case 13: return g_autoLite; case 15: return g_tuneRoot; case 16: return g_tuneVoiced;
                  case 17: return HVM_PRODUCT_VERSION_X100; case 18: return g_oldLib; case 19: return g_cfgImported; case 20: return g_par[P_LEGACY] >= 0.5f ? 1 : 0; case 21: return g_monResets; case 22: return g_monUnder; case 23: return AiNs::built() ? 1 : 0; case 24: return g_probes; case 25: return g_recoveries;
#ifndef VFX_NO_RUBBERBAND
                  case 14: return 1;
#endif
                }
    return 0;
}
// float stat ids: 0 peakIn 1 peakOut 2 latencyMs 3 cpu load % (average) 4 cpu load % (peak) 5 noise floor dBFS 6 noise reduction dB 7 detected note (midi, -1 = none) 8 tuned note (midi) 9 gate gain dB 10 compressor reduction dB 11 de-esser reduction dB 12 limiter gain dB 13 pitch-analysis window ms (40 fixed, adaptive: ~20-40) 14 hear-myself: mean queued audio ms (measured monitor latency) 15 hear-myself playback-rate offset % 16 noise-suppression delay ms (V1) 17..21 CPU % per stage: input chain, pitch analysis, pitch engine, chords, effects+limiter
VFX_EXPORT float vfx_statf(int id)
{
    switch (id) { case 0: return g_peakIn.load(); case 1: return g_peakOut.load(); case 2: return g_latMs.load();
                  case 3: return g_load.load(); case 4: return g_loadPeak.load();
                  case 5: return g_nsFloor.load(); case 6: return g_nsRed.load();
                  case 7: return g_tuneDet.load(); case 8: return g_tuneTgt.load();
                  case 9: return g_gateG.load(); case 10: return g_compGR.load(); case 11: return g_deessGR.load(); case 12: return g_limGR.load(); case 13: return g_tuneWinMs.load(); case 14: return g_monQueueMs.load(); case 15: return g_monRatioPct.load(); case 16: return g_nsLatMs.load(); case 17: return g_stagePct[0].load(); case 18: return g_stagePct[1].load(); case 19: return g_stagePct[2].load(); case 20: return g_stagePct[3].load(); case 21: return g_stagePct[4].load(); }
    return 0.f;
}

// Backwards-compatible table used by the original menus.
struct vc_api_t {
    void (*set_pitch)(float); void (*enable)(void); void (*disable)(void); int (*is_enabled)(void);
    float (*get_pitch)(void); void (*set_formant)(float); float (*get_formant)(void);
};
static void  _set_pitch(float v)   { vfx_set(P_PITCH, v); }
static void  _enable(void)         { vfx_set(P_ENABLED, 1.f); }
static void  _disable(void)        { vfx_set(P_ENABLED, 0.f); }
static int   _is_enabled(void)     { return g_par[P_ENABLED] >= 0.5f; }
static float _get_pitch(void)      { return g_par[P_PITCH]; }
static void  _set_formant(float v) { vfx_set(P_FORMANT, v); }
static float _get_formant(void)    { return g_par[P_FORMANT]; }
VFX_EXPORT vc_api_t vc_api = { _set_pitch, _enable, _disable, _is_enabled, _get_pitch, _set_formant, _get_formant };

