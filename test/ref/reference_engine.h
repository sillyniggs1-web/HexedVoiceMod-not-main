// REFERENCE ENGINE for the host tests only (not built into the library).
// Copy of the DSP core as it was before HexedVoiceMod 1.0 (pre-release engine), kept so the tests can prove that
// legacy_mode reproduces the previous sound. The test includes it inside a namespace with renamed exports (see host_test.cpp).
#pragma once
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
// RubberBand is optional: define VFX_NO_RUBBERBAND (Android.mk does it when jni/rubberband-src is missing) to build with the
// built-in Lite pitch shifter only (pitch works, formant control and the high-quality engine do not).
#ifndef VFX_NO_RUBBERBAND
#include "rubberband/RubberBandLiveShifter.h"
#endif


#define VFX_EXPORT extern "C" __attribute__((visibility("default")))
#define VFX_API_VERSION 8
#define LOG_PATH "/storage/emulated/0/voicefx_log.txt"
#define CFG_PATH "/storage/emulated/0/voicefx.cfg"

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
    vfx_platform_log(buf);                       // defined in main.cpp: AML logger + /storage/emulated/0/voicefx_log.txt
#else
    fprintf(stderr, "[VFX] %s\n", buf);
#endif
}

// ------------------------------------------------------------------ parameters (written by menu, read by DSP thread)
// ids 0..10 are identical to v5 (old menus keep working); new ones are appended.
enum {
    P_ENABLED, P_PITCH, P_FORMANT, P_PRESERVE, P_GAIN_DB, P_GATE_DB, P_MIX, P_ROBOT, P_ROBOT_HZ, P_MON_VOL, P_MONITOR,
    P_HP_HZ, P_LP_HZ, P_BASS_DB, P_TREBLE_DB, P_DRIVE, P_BITS, P_DOWN, P_TREM_HZ, P_TREM_DEPTH,
    P_MOD_MODE, P_MOD_RATE, P_MOD_DEPTH, P_ECHO_MS, P_ECHO_FB, P_ECHO_MIX, P_REV_SIZE, P_REV_MIX,
    P_ROBOT_AMT, P_VOC_MODE, P_VOC_HZ, P_QUALITY, P_NS_ON, P_NS_AMT, P_NS_CLICK,
    P_TUNE_ON, P_TUNE_KEY, P_TUNE_SCALE, P_TUNE_TIME, P_TUNE_AMT, P_CHORD, P_CHORD_MIX, P_CHORD_SPREAD, P_COUNT
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
static std::atomic<int>   g_recStarts{0}, g_dspCalls{0}, g_lastBytes{0}, g_rate{0}, g_fmtOK{0}, g_block{0};
static std::atomic<int>   g_underruns{0}, g_gapResets{0}, g_overruns{0}, g_engineMode{0}, g_autoLite{0};
static std::atomic<float> g_peakIn{0.f}, g_peakOut{0.f}, g_latMs{0.f}, g_load{0.f}, g_loadPeak{0.f}, g_nsFloor{-90.f}, g_nsRed{0.f}, g_tuneDet{-1.f}, g_tuneTgt{-1.f};
static std::atomic<int>   g_tuneRoot{0}, g_tuneVoiced{0};

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
        float g1 = 0.5f - 0.5f * cosf(kTwoPi * d1 / W), g2 = 0.5f - 0.5f * cosf(kTwoPi * d2 / W);
        return g1 * rd(2.f + d1) + g2 * rd(2.f + d2);
    }
};

struct Comb { std::vector<float> b; size_t i = 0; float store = 0.f;
    void init(size_t n) { b.assign(n < 1 ? 1 : n, 0.f); i = 0; store = 0.f; }
    inline float proc(float in, float fb, float d1, float d2) { float o = b[i]; store = fz(o * d2 + store * d1); b[i] = fz(in + store * fb); if (++i >= b.size()) i = 0; return o; } };
struct Allpass { std::vector<float> b; size_t i = 0;
    void init(size_t n) { b.assign(n < 1 ? 1 : n, 0.f); i = 0; }
    inline float proc(float in) { float bo = b[i]; float o = bo - in; b[i] = fz(in + bo * 0.5f); if (++i >= b.size()) i = 0; return o; } };

struct Snap {        // one consistent read of the parameters per DSP call
    float hp, lp, bass, treble, drive, bits, down, tremHz, tremDepth, modRate, modDepth, echoMs, echoFb, echoMix, revSize, revMix,
          robotHz, robotAmt, vocHz; int modMode, vocMode; bool robot;
};

// effect rack, runs sample by sample after the pitch stage
struct FxChain {
    static const int NB = 12;
    float fs = 48000.f; Snap p{};
    Biquad hp[2], lp[2], bass, treb; bool hpOn = false, lpOn = false, bassOn = false, trebOn = false;
    float cHp = -1, cLp = -1, cBass = -99, cTreb = -99;
    float phR = 0, phT = 0, hold = 0, hcnt = 0;
    std::vector<float> mbuf; size_t mmask = 0, mw = 0; float lfo1 = 0, lfo2 = 0, fbState = 0;
    std::vector<float> ebuf; size_t ew = 0; float eCur = 0, eTgt = 0; bool eInit = false;
    Comb comb[8]; Allpass ap[4]; float revFb = 0.84f;
    Biquad an[NB], sy[NB]; float venv[NB]; float vph = 0, inP = 0, outP = 0, vAgc = 1.f; uint32_t rng = 22222u;

    void init(float f) {
        fs = f; reset();
        size_t n = 1; while (n < (size_t)(fs * 0.1f)) n <<= 1; mbuf.assign(n, 0.f); mmask = n - 1;
        ebuf.assign((size_t)(fs * 1.05f) + 16, 0.f);
        const int ct[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 }, at[4] = { 556, 441, 341, 225 };
        for (int i = 0; i < 8; i++) comb[i].init((size_t)(ct[i] * fs / 44100.f));
        for (int i = 0; i < 4; i++) ap[i].init((size_t)(at[i] * fs / 44100.f));
        for (int i = 0; i < NB; i++) { double c = 140.0 * pow(7500.0 / 140.0, i / (double)(NB - 1)); an[i].bandpass(fs, c, 3.0); sy[i].bandpass(fs, c, 3.0); }
        cHp = cLp = -1; cBass = cTreb = -99;
    }
    void reset() {
        for (auto& b : hp) b.clear();
        for (auto& b : lp) b.clear();
        bass.clear(); treb.clear();
        std::fill(mbuf.begin(), mbuf.end(), 0.f); std::fill(ebuf.begin(), ebuf.end(), 0.f);
        for (auto& c : comb) init_clear(c);
        for (auto& a : ap) std::fill(a.b.begin(), a.b.end(), 0.f);
        for (auto& b : an) b.clear();
        for (auto& b : sy) b.clear();
        for (int i = 0; i < NB; i++) venv[i] = 0.f;
        phR = phT = hold = hcnt = 0.f; lfo1 = lfo2 = fbState = 0.f; mw = ew = 0; eInit = false; vph = 0.f; inP = outP = 0.f; vAgc = 1.f;
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
    }

    inline float noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)(int32_t)rng * (1.f / 2147483648.f); }
    inline float mrd(float d) const { float rp = (float)mw - d; long i0 = (long)floorf(rp); float fr = rp - (float)i0;
        return mbuf[(size_t)i0 & mmask] * (1.f - fr) + mbuf[(size_t)(i0 + 1) & mmask] * fr; }

    float process(float x) {
        // 1) vocoder (whisper = noise carrier, cyber = saw carrier): keeps the spoken envelope, replaces the voice source
        if (p.vocMode) {
            float car;
            if (p.vocMode == 1) car = noise();
            else { float dt = p.vocHz / fs; vph += dt; vph -= floorf(vph); float t = vph, bl = 0.f;
                   if (t < dt) { float u = t / dt; bl = u + u - u * u - 1.f; } else if (t > 1.f - dt) { float u = (t - 1.f) / dt; bl = u * u + u + u + 1.f; }
                   car = 2.f * t - 1.f - bl; }
            float acc = 0.f;
            for (int i = 0; i < NB; i++) {
                float a = fabsf(an[i].proc(x));
                venv[i] = fz(venv[i] + (a - venv[i]) * (a > venv[i] ? 0.02f : 0.0035f));
                acc += venv[i] * sy[i].proc(car);
            }
            inP = fz(inP + (x * x - inP) * 0.0008f); outP = fz(outP + (acc * acc - outP) * 0.0008f);
            float tg = sqrtf((inP + 1e-9f) / (outP + 1e-9f)); tg = clampf(tg, 0.05f, 60.f);
            vAgc += (tg - vAgc) * 0.002f;
            x = acc * vAgc;
        }
        // 2) drive
        if (p.drive > 0.001f) { float g = 1.f + 20.f * p.drive; x = tanhf(g * x) * (0.85f / sqrtf(g)); }
        // 3) bit crusher / sample-rate reducer
        if (p.bits < 15.5f || p.down > 1.01f) {
            if (p.down > 1.01f) { hcnt += 1.f; if (hcnt >= p.down) { hcnt -= p.down; hold = x; } x = hold; }
            if (p.bits < 15.5f) { float lv = exp2f(p.bits - 1.f); x = floorf(x * lv + 0.5f) / lv; }
        }
        // 4) tone: high-pass / low-pass (24 dB/oct) and shelves
        if (hpOn) x = hp[1].proc(hp[0].proc(x));
        if (lpOn) x = lp[1].proc(lp[0].proc(x));
        if (bassOn) x = bass.proc(x);
        if (trebOn) x = treb.proc(x);
        // 5) robot ring modulator
        if (p.robot) { phR += p.robotHz / fs; phR -= floorf(phR); x *= (1.f - p.robotAmt) + p.robotAmt * sinf(kTwoPi * phR); }
        // 6) tremolo
        if (p.tremDepth > 0.001f) { phT += p.tremHz / fs; phT -= floorf(phT); x *= 1.f - p.tremDepth * (0.5f - 0.5f * sinf(kTwoPi * phT)); }
        // 7) modulated delay: chorus / flanger / vibrato
        if (p.modMode) {
            float rate = p.modRate / fs; lfo1 = wrap01(lfo1 + rate); lfo2 = wrap01(lfo2 + rate * 1.13f);
            float s1 = sinf(kTwoPi * lfo1), s2 = sinf(kTwoPi * lfo2 + 1.7f), out;
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
            ebuf[ew] = fz(x + d * p.echoFb); if (++ew >= ebuf.size()) ew = 0;
            x += d * p.echoMix;
        }
        // 9) reverb (Freeverb topology, mono)
        if (p.revMix > 0.001f) {
            float in = x * 0.015f, sum = 0.f;
            for (int i = 0; i < 8; i++) sum += comb[i].proc(in, revFb, 0.3f, 0.7f);
            for (int i = 0; i < 4; i++) sum = ap[i].proc(sum);
            x = x * (1.f - 0.25f * p.revMix) + sum * 2.2f * p.revMix;
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
    }
    inline void push(float x) {
        float y = lp.proc(x);
        if (++decCnt >= dec) { decCnt = 0; ring[rw & (NB - 1)] = y; rw++; }
        if (--hopLeft <= 0) { hopLeft = hopS; analyse(); }
    }
    bool detect(float& m, float& cf) {                      // YIN: returns true and the midi pitch when the frame is voiced
        const int L = W + tauMax + 1;
        for (int i = 0; i < L; i++) win[i] = ring[(rw - L + i) & (NB - 1)];
        double e = 0; for (int i = L - W; i < L; i++) e += (double)win[i] * win[i];
        if (sqrt(e / W) < 0.004) return false;
        float run = 0.f; cm[0] = 1.f;
        for (int tau = 1; tau <= tauMax; tau++) {
            float sum = 0.f; for (int j = 0; j < W; j++) { float df = win[j] - win[j + tau]; sum += df * df; }
            d[tau] = sum; run += sum; cm[tau] = run > 1e-12f ? sum * tau / run : 1.f;
        }
        int best = -1;
        for (int tau = tauMin; tau < tauMax; tau++) if (cm[tau] < 0.15f) { while (tau + 1 < tauMax && cm[tau + 1] < cm[tau]) tau++; best = tau; break; }
        if (best < 0) { float mn = 1e9f; for (int tau = tauMin; tau < tauMax; tau++) if (cm[tau] < mn) { mn = cm[tau]; best = tau; } if (mn > 0.30f) return false; }
        float tp = (float)best;
        if (best > 1 && best < tauMax) { float s0 = cm[best - 1], s1 = cm[best], s2 = cm[best + 1], den = 2.f * (2.f * s1 - s2 - s0); if (fabsf(den) > 1e-9f) tp += clampf((s2 - s0) / den, -1.f, 1.f); }
        float f0 = fsd / tp; if (f0 < 60.f || f0 > 1100.f) return false;
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
            const float cand = nearestNote(outMidi, root, mask, target); target = cand;
            desired = tuneOn ? clampf((cand - outMidi) * amt, -3.f, 3.f) : 0.f;
            if (chord > 0) {
                const ChordDef& cd = kChord[chord]; const uint16_t hm = scaleMask(kHarmScale[sc]);
                for (int v = 0; v < cd.n; v++) { int note = stepUp((int)cand, cd.v[v].steps, root, hm) + 12 * cd.v[v].oct; shiftV[v] = (float)note - midi; }
            }
        } else desired = tuneOn ? desired * 0.9f : 0.f;
        if (timeMs <= 1.f) corr = desired; else corr += (desired - corr) * (1.f - expf(-10.f / timeMs));
        g_tuneDet = voiced ? midi : -1.f; g_tuneTgt = (voiced && target > 0.f) ? target + 0.f : -1.f; g_tuneRoot = root; g_tuneVoiced = voiced ? 1 : 0;
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
        cdelay.init(16384); chordv.assign(kSub, 0.f);
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
        tuner.flush(); for (auto& h : harm) h.reset(); cdelay.clear(); chordCur = 0; chG = 0.f; vg = 0.f; for (int v = 0; v < 3; v++) hr[v] = 1.f;
        std::fill(dline.begin(), dline.end(), 0.f); dw = 0;
        engine = 0; xf = 0.f; instant = true; lastPitch = lastForm = -1.f; lastW = 0.f; env = 0.f; gateG = 1.f; load = 0.f; overStreak = 0;
        g_engineMode = 0; g_latMs = 0.f;
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
        g_engineMode = e; g_latMs = 1000.f * (float)dryDelay / (float)rate;
    }

    void process(int16_t* s, int n)   // in-place 16-bit mono, any n
    {
        std::unique_lock<std::mutex> lk(mtx, std::try_to_lock);
        if (!lk.owns_lock() || !ready || n <= 0) return;            // being rebuilt: pass audio through untouched
        auto t0 = std::chrono::steady_clock::now();
        if (haveLast && std::chrono::duration_cast<std::chrono::milliseconds>(t0 - last).count() > 120) { softReset(); g_gapResets++; }
        int ep = g_qualityEpoch.load(); if (ep != epoch) { epoch = ep; autoLite = false; g_autoLite = 0; }

        const float pitch = g_par[P_PITCH], form = g_par[P_FORMANT];
        const bool preserve = g_par[P_PRESERVE] >= 0.5f;
        const float effForm = clampf(form * (preserve ? 1.f / pitch : 1.f), 0.1f, 8.f);
        const float gainLin = powf(10.f, g_par[P_GAIN_DB] / 20.f);
        const bool gateOn = g_par[P_GATE_DB] > -79.5f;
        const float gateThr = powf(10.f, g_par[P_GATE_DB] / 20.f);
        const float mix = g_par[P_MIX];
        const bool nsOn = g_par[P_NS_ON] >= 0.5f; const float nsAmt = g_par[P_NS_AMT], nsClick = g_par[P_NS_CLICK];
        if (nsOn && !nsActive) { ns.flush(); nsActive = true; nsWarm = (int)ns.N; nsM = 0.f; }
        const int quality = (int)g_par[P_QUALITY];
        const bool tuneOn = g_par[P_TUNE_ON] >= 0.5f; const int chordWant = (int)g_par[P_CHORD];
        const float chordMix = g_par[P_CHORD_MIX], spread = g_par[P_CHORD_SPREAD];
        tuner.tuneOn = tuneOn; tuner.key = (int)g_par[P_TUNE_KEY]; tuner.scale = (int)g_par[P_TUNE_SCALE];
        tuner.timeMs = g_par[P_TUNE_TIME]; tuner.amt = g_par[P_TUNE_AMT]; tuner.presetSemi = 12.f * log2f(pitch);
        const bool tunerRun = tuneOn || chordWant > 0 || chordCur > 0 || fabsf(tuner.corr) > 0.01f;
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
            const float t0s = (sVoiced && tunerRun) ? (float)rate / (440.f * exp2f((sMidi - 69.f) * (1.f / 12.f))) : 0.f;   // voice period in samples
            // engine state machine (once per sub-block): fade the old engine out, then start the wanted one
            if (engine != desired && (engine == 0 || xf <= 0.0005f)) { if (desired == 0) { engine = 0; g_engineMode = 0; g_latMs = 0.f; } else startEngine(desired); }
            const float xfTarget = (engine != 0 && engine == desired) ? 1.f : 0.f;

            // 1) input stage: int16 -> float, gate (smoothed), gain; the dry line always runs so a delayed dry copy is always aligned
            for (int i = 0; i < m; i++) {
                float x = sp[i] * (1.f / 32768.f);
                float ax = fabsf(x); if (ax > pin) pin = ax;
                if (nsActive) {                      // noise suppression first: everything after it works on the cleaned mic
                    float yn = ns.proc(x, nsAmt, nsClick);
                    float tg = (nsOn && nsWarm == 0) ? 1.f : 0.f; if (nsWarm > 0) nsWarm--;
                    nsM += clampf(tg - nsM, -nsStep, nsStep);
                    x = x * (1.f - nsM) + yn * nsM; ax = fabsf(x);
                    if (!nsOn && nsM <= 0.f) nsActive = false;
                }
                if (gateOn) {
                    env = ax > env ? ax : env * 0.9995f;
                    float tg = env > gateThr ? 1.f : 0.f;
                    gateG += (tg - gateG) * (tg > gateG ? 0.05f : 0.002f);
                    x *= gateG;
                }
                x *= gainLin; xin[i] = x;
                if (tunerRun) tuner.push(x);
                dline[dw & dmask] = x;
                dryv[i] = dline[(dw - dryDelay) & dmask]; dw++;
            }
            // 2) pitch engine: preset pitch x Auto-Tune correction (updated every 10 ms)
            const float leadRatio = clampf(pitch * exp2f(sCorr * (1.f / 12.f)), 0.25f, 4.f);
#ifndef VFX_NO_RUBBERBAND
            if (engine == 1) {
                if (needShift) {
                    if (lastPitch < 0.f || fabsf(leadRatio - lastPitch) > 1e-5f * lastPitch) { rb->setPitchScale(leadRatio); lastPitch = leadRatio; }
                    if (effForm != lastForm) { rb->setFormantScale(effForm); lastForm = effForm; }
                }
                for (int i = 0; i < m; i++) inq.push(xin[i]);
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
                for (int i = 0; i < m; i++) wet[i] = lite.proc(xin[i]);
            }
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
                for (int i = 0; i < m; i++) {
                    chG += clampf(chT - chG, -nsStep, nsStep); vg += (vgT - vg) * vgA;
                    float cs = 0.f;
                    for (int v = 0; v < cd.n; v++) { hr[v] += (tg[v] - hr[v]) * hA; harm[v].ratio = clampf(hr[v], 0.25f, 4.f); cs += cd.v[v].gain * harm[v].proc(xin[i]); }
                    chordv[i] = cdelay.proc(cs) * (chG * vg * chordMix);
                }
            }
            // 3) cross-fade direct <-> pitch path, then the effect rack, limiter, back to int16
            for (int i = 0; i < m; i++) {
                float d = xfTarget - xf; xf += d > xfStep ? xfStep : (d < -xfStep ? -xfStep : d);
                float y = xin[i];
                if (engine != 0) { float pp = dryv[i] * (1.f - mix) + wet[i] * mix; y = y * (1.f - xf) + pp * xf; }
                if (chordCur > 0) y = y * (1.f - 0.12f * chordMix * chG) + chordv[i] * xf;
                y = fx.process(y);
                if (!(y == y)) y = 0.f;
                float ay = fabsf(y);
                if (ay > 0.9f) y = copysignf(0.9f + 0.1f * tanhf((ay - 0.9f) * 10.f), y);   // soft limiter
                if (fabsf(y) > pout) pout = fabsf(y);
                int v = (int)lrintf(y * 32767.f); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                sp[i] = (int16_t)v;
            }
            base += m;
        }
        if (nsActive) { g_nsFloor = ns.floorDb; g_nsRed = ns.redDb * nsM; } else g_nsRed = 0.f;
        float pi = g_peakIn, po = g_peakOut;
        g_peakIn = pin > pi * 0.85f ? pin : pi * 0.85f;
        g_peakOut = pout > po * 0.85f ? pout : po * 0.85f;

        // CPU meter + automatic fallback
        auto t1 = std::chrono::steady_clock::now(); last = t1; haveLast = true;
        float dt = std::chrono::duration<float>(t1 - t0).count(), dur = (float)n / (float)rate, ratio = dt / dur;
        load = load * 0.9f + ratio * 0.1f; g_load = load * 100.f;
        float lp = g_loadPeak; g_loadPeak = ratio * 100.f > lp * 0.995f ? ratio * 100.f : lp * 0.995f;
        if (ratio > 0.95f) { g_overruns++; overStreak++; } else overStreak = 0;
        if (quality == 0 && engine == 1 && !autoLite && (load > 0.75f || overStreak >= 3)) {
            autoLite = true; g_autoLite = 1; vlog("CPU too high (load %.0f%%) -> switching to the lite pitch shifter", load * 100.f);
        }
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
    for (int i = 0; i < P_COUNT; i++) if (i != P_MONITOR) fprintf(f, "%s=%.4f\n", kPar[i].name, (double)g_par[i].load());
    fclose(f);
}
static void cfg_load()
{
    FILE* f = fopen(CFG_PATH, "r"); if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        for (int i = 0; i < P_COUNT; i++) if (i != P_MONITOR && !strcmp(line, kPar[i].name)) g_par[i] = sanitize(i, (float)atof(eq + 1));
    }
    fclose(f);
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
//           8 api version (7)  9 underruns 10 gap resets 11 overruns (call slower than real time) 12 engine (0 none/1 RubberBand/2 lite) 13 auto-lite active 14 RubberBand compiled in (1/0) 15 key root in use (0..11) 16 voice detected (1/0)
VFX_EXPORT int   vfx_stat(int id)
{
    switch (id) { case 0: return g_hookState; case 1: return g_recStarts; case 2: return g_dspCalls;
                  case 3: return g_lastBytes; case 4: return g_rate; case 5: return g_fmtOK; case 6: return g_block; case 7: return g_monState;
                  case 8: return VFX_API_VERSION; case 9: return g_underruns; case 10: return g_gapResets; case 11: return g_overruns;
                  case 12: return g_engineMode; case 13: return g_autoLite; case 15: return g_tuneRoot; case 16: return g_tuneVoiced;
#ifndef VFX_NO_RUBBERBAND
                  case 14: return 1;
#endif
                }
    return 0;
}
// float stat ids: 0 peakIn 1 peakOut 2 latencyMs 3 cpu load % (average) 4 cpu load % (peak) 5 noise floor dBFS 6 noise reduction dB 7 detected note (midi, -1 = none) 8 tuned note (midi)
VFX_EXPORT float vfx_statf(int id)
{
    switch (id) { case 0: return g_peakIn.load(); case 1: return g_peakOut.load(); case 2: return g_latMs.load();
                  case 3: return g_load.load(); case 4: return g_loadPeak.load();
                  case 5: return g_nsFloor.load(); case 6: return g_nsRed.load();
                  case 7: return g_tuneDet.load(); case 8: return g_tuneTgt.load(); }
    return 0.f;
}

// Backwards-compatible table used by the original v4 menus.
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

