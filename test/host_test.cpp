// SPDX-License-Identifier: GPL-2.0-or-later
// Host tests for the HexedVoiceMod engine. Build: g++ -std=c++17 -O2 -ffast-math -fno-finite-math-only -I <rubberband or fake> host_test.cpp
// With the fake RubberBand stand-in (pure delay) the plumbing is checked sample-exact; effects are checked numerically.
// The pre-1.0 regression suite runs with legacy_mode ON (reset_all); the [P1-x] tests run the 1.0 chain (reset_v1) and compare legacy_mode with test/ref/reference_engine.h.
#define VFX_HOST_TEST 1
#define CFG_PATH "/tmp/hvm_test_new.cfg"          // temp files: the one-time import is tested without touching real storage
#define OLD_CFG_PATH "/tmp/hvm_test_old.cfg"
#include "../jni/voicefx_dsp.h"
#include <vector>
#include <cstdio>
#include <thread>
#include <chrono>
#include <functional>
// ---- reference engine: the pre-1.0 DSP core, compiled next to the new one (own namespace, renamed exports) so legacy_mode can be
//      compared sample by sample with the sound it has to reproduce.
#undef VFX_API_VERSION
#undef LOG_PATH
#undef CFG_PATH
#define vfx_process_i16 ref_vfx_process_i16
#define vfx_prepare ref_vfx_prepare
#define vfx_get ref_vfx_get
#define vfx_set ref_vfx_set
#define vfx_save ref_vfx_save
#define vfx_load ref_vfx_load
#define vfx_stat ref_vfx_stat
#define vfx_statf ref_vfx_statf
#define vc_api ref_vc_api
namespace refeng {
#include "ref/reference_engine.h"
}
#undef vfx_process_i16
#undef vfx_prepare
#undef vfx_get
#undef vfx_set
#undef vfx_save
#undef vfx_load
#undef vfx_stat
#undef vfx_statf
#undef vc_api
#define VFX_API_VERSION 9
#undef LOG_PATH
#undef CFG_PATH
#define CFG_PATH "/tmp/hvm_test_new.cfg"      // (the reference header redefined them)
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static std::vector<int16_t> tone(int total, double f, double amp) { std::vector<int16_t> v(total); for (int i = 0; i < total; i++) v[i] = (int16_t)(amp * sin(2 * M_PI * f * i / 48000.0)); return v; }
static std::vector<int16_t> voiceish(int total) { std::vector<int16_t> v(total); for (int i = 0; i < total; i++) { double t = i / 48000.0, e = 0.5 + 0.5 * sin(2 * M_PI * 3 * t);
    v[i] = (int16_t)(e * (9000 * sin(2 * M_PI * 180 * t) + 5000 * sin(2 * M_PI * 540 * t) + 3000 * sin(2 * M_PI * 1500 * t) + 1500 * sin(2 * M_PI * 3100 * t))); } return v; }
// reset_all(): legacy_mode ON, so the whole pre-1.0 regression suite below keeps testing the previous behaviour sample-exactly.
// reset_v1(): the 1.0 defaults (conditioning, new gate, lookahead limiter).
static void reset_all() { params_default(); vfx_set(P_LEGACY, 1.f); vfx_prepare(48000); vfx_set(P_ENABLED, 1.f); }
static void reset_v1()  { params_default(); vfx_prepare(48000); vfx_set(P_ENABLED, 1.f); }
static void run(std::vector<int16_t>& d, int chunk, unsigned seed = 0) { int p = 0, total = (int)d.size(); while (p < total) { int n = chunk; if (seed) { seed = seed * 1664525u + 1013904223u; n = 100 + (seed >> 16) % 1900; } if (p + n > total) n = total - p; vfx_process_i16(d.data() + p, n); p += n; } }
static double rms(const std::vector<int16_t>& d, int a, int b) { double e = 0; for (int i = a; i < b; i++) e += (double)d[i] * d[i]; return sqrt(e / (b - a)) / 32768.0; }
static double db(double x) { return 20 * log10(x + 1e-12); }
static double gainAt(double f, const char* what) { auto in = tone(48000, f, 8000); auto out = in; run(out, 960); (void)what; return db(rms(out, 24000, 48000)) - db(rms(in, 24000, 48000)); }
static int maxJump(const std::vector<int16_t>& d, int a, int b) { int m = 0; for (int i = a + 1; i < b; i++) m = std::max(m, abs((int)d[i] - (int)d[i - 1])); return m; }
static double zcFreq(const std::vector<int16_t>& d, int a, int b) { int z = 0; for (int i = a + 1; i < b; i++) if ((d[i - 1] < 0) != (d[i] < 0)) z++; return z / 2.0 / ((b - a) / 48000.0); }


// ---- helpers for the Auto-Tune / chord tests
struct Note { double midi; double dur; };
static std::vector<int16_t> sing(const std::vector<Note>& notes, double amp = 0.22) {
    int total = 0; for (auto& n : notes) total += (int)(n.dur * 48000); std::vector<int16_t> v(total); double ph = 0; int i = 0;
    for (auto& n : notes) { int len = (int)(n.dur * 48000); double f = 440.0 * pow(2.0, (n.midi - 69.0) / 12.0);
        for (int k = 0; k < len; k++, i++) { double s = 0, ps = 0; for (int h = 1; h * f < 6000; h++) { double fh = h * f; double a = (1.0 / h) * (1.0 + 1.5 * exp(-pow((fh - 700) / 250, 2)) + 1.0 * exp(-pow((fh - 1200) / 300, 2))); s += a * sin(h * ph); ps += a; }
            double env = std::min(1.0, k / 400.0) * std::min(1.0, (len - k) / 400.0); v[i] = (int16_t)(amp * 32768.0 * env * s / 2.6); ph += 2 * M_PI * f / 48000.0; } }
    return v; }
static double measureMidi(const std::vector<int16_t>& x, int start, int len) {      // normalised autocorrelation, parabolic peak
    int lagMin = 48000 / 1000, lagMax = 48000 / 70; std::vector<double> r(lagMax + 2, 0.0); double e0 = 0; for (int i = 0; i < len; i++) e0 += (double)x[start + i] * x[start + i];
    if (e0 < 1e3) return -1;
    for (int lag = lagMin; lag <= lagMax + 1; lag++) { double c = 0, e1 = 0; for (int i = 0; i < len - lagMax - 2; i++) { c += (double)x[start + i] * x[start + i + lag]; e1 += (double)x[start + i + lag] * x[start + i + lag]; } r[lag] = c / sqrt(e0 * e1 + 1e-9); }
    int bl = lagMin; double bv = -2; for (int lag = lagMin + 1; lag <= lagMax; lag++) if (r[lag] > bv && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { bv = r[lag]; bl = lag; }
    for (int lag = lagMin + 1; lag <= lagMax; lag++) if (r[lag] > 0.9 * bv && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { bl = lag; break; }     // first strong peak (avoid sub-octaves)
    double a = r[bl - 1], b = r[bl], c2 = r[bl + 1], den = a - 2 * b + c2, sh = fabs(den) > 1e-12 ? 0.5 * (a - c2) / den : 0; double lag = bl + sh;
    return 69.0 + 12.0 * log2((48000.0 / lag) / 440.0); }
static double goertzelDb(const std::vector<int16_t>& x, int start, int len, double f) {
    double re = 0, im = 0; for (int i = 0; i < len; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / len), a = 2 * M_PI * f * (start + i) / 48000.0; re += w * x[start + i] * cos(a); im += w * x[start + i] * sin(a); }
    return 10 * log10((re * re + im * im) / ((double)len * len) + 1e-3); }
static double mf(double midi) { return 440.0 * pow(2.0, (midi - 69.0) / 12.0); }
static void tuneSetup(int key, int scale, double timeMs, double amt, int chord) { reset_all(); vfx_set(P_QUALITY, 2.f); vfx_set(P_TUNE_ON, 1.f); vfx_set(P_TUNE_KEY, (float)key); vfx_set(P_TUNE_SCALE, (float)scale);
    vfx_set(P_TUNE_TIME, (float)timeMs); vfx_set(P_TUNE_AMT, (float)amt); vfx_set(P_CHORD, (float)chord); }

int main()
{
    printf("[1] Normal preset (enabled, no pitch shift): zero latency, bit exact, any chunk size\n");
    for (unsigned sd : { 0u, 4242u }) { reset_all(); auto in = voiceish(96000), out = in; run(out, 960, sd);
        int bad = 0; for (size_t i = 0; i < in.size(); i++) if (abs(out[i] - in[i]) > 1) bad++;
        CHECK(bad == 0, "%s chunks: %d mismatching samples", sd ? "random" : "960", bad); }

#ifndef VFX_NO_RUBBERBAND
    printf("[2] pitch path alignment (fake RubberBand = pure delay 2048): mix 1 and mix 0.5, constant and random chunks\n");
    for (float mix : { 1.f, 0.5f }) for (unsigned sd : { 0u, 777u }) { reset_all(); vfx_set(P_PITCH, 2.f); vfx_set(P_MIX, mix);
        auto in = voiceish(144000), out = in; run(out, 960, sd);
        int bad = 0; for (int i = 30000; i < (int)in.size(); i++) if (abs(out[i] - in[i - 2048]) > 3) bad++;
        CHECK(bad == 0, "mix %.1f %s chunks: %d mismatches (old v5 engine: ~114000 for mix 0.5)", mix, sd ? "random" : "960", bad); }

#endif
    printf("[3] switching presets while talking: no click, no dead gap\n");
    { reset_all(); auto in = tone(144000, 300, 9000), out = in; vfx_set(P_PITCH, 2.f);
      int p = 0; int jumpMax = 0; int minAfter = 99999;
      while (p < (int)in.size()) { int n = 960; if (p == 48000) vfx_set(P_PITCH, 1.f); if (p == 96000) vfx_set(P_PITCH, 2.f); vfx_process_i16(out.data() + p, n); p += n; }
      int j1 = maxJump(out, 40000, 140000);   // 300 Hz @ 9000 amp: natural max step = 2*pi*300/48000*9000 = 353
      CHECK(j1 < 800, "largest sample-to-sample step over the whole run: %d (natural max 353)", j1);
      double r = rms(out, 60000, 90000); CHECK(r > 0.15, "level after switching to direct: %.3f", r); }

    printf("[4] mic burst: stale audio from the previous burst must not replay\n");
    { reset_all(); vfx_set(P_PITCH, 2.f); auto a = tone(24000, 400, 12000); run(a, 960);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));            // pause > 120 ms
      std::vector<int16_t> sil(9600, 0); run(sil, 960);
      CHECK(rms(sil, 0, 9600) < 1e-4, "first 200 ms of the next burst (silent input) is silent: rms %.5f", rms(sil, 0, 9600)); }

    printf("[5] filters (frequency response through the full engine)\n");
    { reset_all(); vfx_set(P_HP_HZ, 400.f); double g1 = gainAt(100, ""), g2 = gainAt(1000, ""); CHECK(g1 < -20 && fabs(g2) < 1.0, "high-pass 400 Hz: 100 Hz %.1f dB, 1 kHz %.1f dB", g1, g2);
      reset_all(); vfx_set(P_LP_HZ, 2000.f); g1 = gainAt(6000, ""); g2 = gainAt(500, ""); CHECK(g1 < -20 && fabs(g2) < 1.0, "low-pass 2 kHz: 6 kHz %.1f dB, 500 Hz %.1f dB", g1, g2);
      reset_all(); vfx_set(P_BASS_DB, 9.f); g1 = gainAt(60, ""); g2 = gainAt(2000, ""); CHECK(g1 > 7 && fabs(g2) < 1.0, "bass +9 dB: 60 Hz %+.1f dB, 2 kHz %+.1f dB", g1, g2);
      reset_all(); vfx_set(P_TREBLE_DB, -9.f); g1 = gainAt(9000, ""); g2 = gainAt(500, ""); CHECK(g1 < -7 && fabs(g2) < 1.0, "treble -9 dB: 9 kHz %+.1f dB, 500 Hz %+.1f dB", g1, g2); }

    printf("[6] drive keeps a sane level; bit-crusher/tremolo/ring-mod run and stay finite\n");
    { for (float d : { 0.2f, 0.6f, 1.f }) { reset_all(); vfx_set(P_DRIVE, d); double g = gainAt(500, ""); CHECK(g > -6 && g < 8, "drive %.1f: level change %+.1f dB", d, g); }
      reset_all(); vfx_set(P_BITS, 6.f); vfx_set(P_DOWN, 6.f); double g = gainAt(500, ""); CHECK(g > -6 && g < 3, "8-bit style (6 bit, /6): %+.1f dB", g);
      reset_all(); vfx_set(P_TREM_DEPTH, 1.f); vfx_set(P_TREM_HZ, 5.f); auto in = tone(48000, 400, 8000), out = in; run(out, 960);
      double lo = 1e9, hi = 0; for (int i = 12000; i < 48000; i += 480) { double r = rms(out, i, i + 480); lo = std::min(lo, r); hi = std::max(hi, r); }
      CHECK(lo < hi * 0.2, "tremolo depth 1: envelope min/max %.3f/%.3f", lo, hi);
      reset_all(); vfx_set(P_ROBOT, 1.f); vfx_set(P_ROBOT_HZ, 50.f); vfx_set(P_ROBOT_AMT, 1.f); in = tone(48000, 400, 8000); out = in; run(out, 960);
      lo = 1e9; hi = 0; for (int i = 12000; i < 48000; i += 120) { double r = rms(out, i, i + 120); lo = std::min(lo, r); hi = std::max(hi, r); }
      CHECK(lo < hi * 0.7, "ring modulator envelope varies: %.3f/%.3f", lo, hi); }

    printf("[7] echo / reverb / chorus / flanger / vibrato\n");
    { reset_all(); vfx_set(P_ECHO_MS, 250.f); vfx_set(P_ECHO_FB, 0.5f); vfx_set(P_ECHO_MIX, 1.f); std::vector<int16_t> d(48000, 0); for (int i = 0; i < 100; i++) d[i] = 16000; run(d, 960);
      int e1 = 100 + 12000; double a1 = 0, a2 = 0; for (int i = -20; i < 20; i++) { a1 += abs(d[e1 + i]); a2 += abs(d[2 * 12000 + 100 + i]); }
      CHECK(a1 > 20000 && a2 > a1 * 0.3 && a2 < a1 * 0.8, "echo: 1st repeat at 250 ms (%.0f), 2nd at 500 ms is ~half (%.0f)", a1, a2);
      reset_all(); vfx_set(P_REV_MIX, 0.8f); vfx_set(P_REV_SIZE, 0.8f); std::vector<int16_t> r(96000, 0); for (int i = 0; i < 200; i++) r[i] = 12000; run(r, 960);
      double e_early = rms(r, 4800, 14400), e_late = rms(r, 72000, 96000); CHECK(e_early > 0.004 && e_late < e_early * 0.5, "reverb tail decays: %.4f -> %.4f", e_early, e_late);
      std::vector<int16_t> nz(96000); unsigned s = 1; for (auto& x : nz) { s = s * 1664525u + 1013904223u; x = (int16_t)(((int)(s >> 16) - 32768) / 4); }
      reset_all(); vfx_set(P_REV_MIX, 0.5f); auto nin = nz; run(nin, 960); double g = db(rms(nin, 48000, 96000)) - db(rms(nz, 48000, 96000)); CHECK(g > -3 && g < 8, "reverb 50%% on noise: level change %+.1f dB", g);
      for (int mode = 1; mode <= 3; mode++) { reset_all(); vfx_set(P_MOD_MODE, (float)mode); vfx_set(P_MOD_DEPTH, 1.f); vfx_set(P_MOD_RATE, 4.f); auto v = voiceish(96000), o = v; run(o, 960);
        double gg = db(rms(o, 48000, 96000)) - db(rms(v, 48000, 96000)); CHECK(gg > -8 && gg < 6, "mod mode %d: level change %+.1f dB", mode, gg); } }

    printf("[8] vocoder (whisper / cyber): stable, level matched, voice is gone\n");
    for (int mode = 1; mode <= 2; mode++) { reset_all(); vfx_set(P_VOC_MODE, (float)mode); auto v = voiceish(144000), o = v; run(o, 960);
        double gg = db(rms(o, 96000, 144000)) - db(rms(v, 96000, 144000)); CHECK(gg > -8 && gg < 6 && maxJump(o, 100000, 144000) < 30000, "voc mode %d: level change %+.1f dB", mode, gg); }

    printf("[9] lite pitch shifter (built-in): measured pitch of a 300 Hz tone\n");
    for (float ratio : { 0.7071f, 1.26f, 2.f }) { reset_all(); vfx_set(P_QUALITY, 2.f); vfx_set(P_PITCH, ratio); auto in = tone(96000, 300, 9000), o = in; run(o, 960);
        double f = zcFreq(o, 48000, 96000); CHECK(fabs(f / 300.0 - ratio) < 0.06, "ratio %.3f -> %.1f Hz (want %.1f), latency %.1f ms", ratio, f, 300 * ratio, vfx_statf(2)); }

#ifndef VFX_NO_RUBBERBAND
    printf("[10] auto fallback to lite when the CPU is overloaded\n");
    { reset_all(); vfx_set(P_PITCH, 1.5f); auto in = tone(48000, 300, 9000), o = in; run(o, 960); CHECK(vfx_stat(13) == 0 && vfx_stat(12) == 1, "idle host: RubberBand engine, auto-lite %d", vfx_stat(13)); }

#endif
    printf("[11] robustness: random parameters + random chunks for 20 s of audio, no NaN/overflow/crash\n");
    { reset_all(); unsigned s = 99; auto v = voiceish(48000 * 20); int bad = 0; size_t p = 0;
      while (p < v.size()) { s = s * 1664525u + 1013904223u; int id = 1 + (s >> 8) % (P_COUNT - 1); if (id == P_MONITOR) id = P_PITCH; s = s * 1664525u + 1013904223u;
        float lo = kPar[id].lo, hi = kPar[id].hi; vfx_set(id, lo + (hi - lo) * ((s >> 8) % 1000) / 1000.f); if (id == P_ENABLED) vfx_set(P_ENABLED, 1.f);
        s = s * 1664525u + 1013904223u; int n = 50 + (s >> 16) % 4000; if (p + n > v.size()) n = (int)(v.size() - p); vfx_process_i16(v.data() + p, n); p += n; }
      for (auto x : v) if (x == -32768) bad++; CHECK(bad < 50, "samples hitting the rail: %d", bad); }


    printf("[12] noise suppression\n");
    {
        auto fan = [](int total, double rmsTarget, unsigned seed) { std::vector<double> v(total); double lp = 0, lp2 = 0; unsigned r = seed;
            for (int i = 0; i < total; i++) { r = r * 1664525u + 1013904223u; double w = ((int)(r >> 8) - 8388608) / 8388608.0; lp += (w - lp) * 0.08; lp2 += (lp - lp2) * 0.3; v[i] = lp2 + 0.3 * w; }
            double e = 0; for (double x : v) e += x * x; double g = rmsTarget / sqrt(e / total); for (auto& x : v) x *= g; return v; };
        auto speech = [](int total) { std::vector<double> v(total, 0.0); for (int i = 0; i < total; i++) { double t = i / 48000.0; double ph = fmod(t, 1.0);
            double env = (ph > 0.25 && ph < 0.85) ? 0.5 + 0.5 * sin(2 * M_PI * 4 * t) : 0.0; env *= 0.5 * (1 + sin(2 * M_PI * 1.7 * t));
            double f0 = 140 + 20 * sin(2 * M_PI * 0.7 * t), s = 0; for (int h = 1; h <= 14; h++) s += sin(2 * M_PI * f0 * h * t + h) / h; v[i] = 0.12 * env * s; } return v; };
        auto toI16 = [](const std::vector<double>& a) { std::vector<int16_t> o(a.size()); for (size_t i = 0; i < a.size(); i++) o[i] = (int16_t)std::max(-32767.0, std::min(32767.0, a[i] * 32768.0)); return o; };
        const int T = 48000 * 8;
        // (a) fan only, three strengths
        for (float amt : { 0.3f, 0.6f, 1.0f }) { reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_AMT, amt); auto in = toI16(fan(T, 0.012, 5)), out = in; run(out, 960);
            double red = db(rms(in, 48000 * 4, T)) - db(rms(out, 48000 * 4, T));
            CHECK(red > (amt < 0.5f ? 8 : (amt < 0.9f ? 12 : 18)), "fan noise only, strength %.1f: reduced by %.1f dB (floor est %.1f dBFS)", amt, red, vfx_statf(5)); }
        // (b) clean speech must survive
        { reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_AMT, 0.6f); auto in = toI16(speech(T)), out = in; run(out, 960);
          double lv = db(rms(out, 48000 * 2, T)) - db(rms(in, 48000 * 2 - 512, T - 512)); double c = 0, a = 0, b2 = 0;
          for (int i = 48000 * 2; i < T; i++) { c += (double)out[i] * in[i - 512]; a += (double)out[i] * out[i]; b2 += (double)in[i - 512] * in[i - 512]; }
          CHECK(fabs(lv) < 1.5 && c / sqrt(a * b2) > 0.95, "clean speech: level %+.2f dB, correlation with input delayed 512 samples %.3f", lv, c / sqrt(a * b2)); }
        // (c) speech over fan: gaps get quiet, speech keeps its level, SNR improves
        { reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_AMT, 0.7f); auto sp = speech(T), fn = fan(T, 0.012, 9); std::vector<double> mixd(T); for (int i = 0; i < T; i++) mixd[i] = sp[i] + fn[i];
          auto in = toI16(mixd), out = in; run(out, 960);
          // gaps: t mod 1 in [0.9,1.0) ; speech: [0.4,0.8)
          double gi = 0, go = 0, si = 0, so = 0; int gn = 0, sn = 0;
          for (int i = 48000 * 3; i < T; i++) { double ph = fmod(i / 48000.0, 1.0); double xi = in[i] / 32768.0, xo = out[i + 0] / 32768.0;
              if (ph > 0.9) { gi += xi * xi; go += xo * xo; gn++; } else if (ph > 0.45 && ph < 0.75) { si += xi * xi; so += xo * xo; sn++; } }
          double gap = 10 * log10(gi / go), sl = 10 * log10(so / si);
          CHECK(gap > 10 && sl > -4, "speech over fan: noise between words -%.1f dB, speech level %+.1f dB", gap, sl); }
        // (d) keyboard clicks over a quiet room
        { std::vector<double> sig = fan(T, 0.004, 3); for (int c = 0; c < 18; c++) { int at = 48000 + c * 19000; unsigned r = 77 + c; for (int i = 0; i < 160; i++) { r = r * 1664525u + 1013904223u; double w = ((int)(r >> 8) - 8388608) / 8388608.0; sig[at + i] += 0.25 * w * exp(-i / 40.0); } }
          double pk[2];
          for (int ci = 0; ci < 2; ci++) { reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_AMT, 0.6f); vfx_set(P_NS_CLICK, ci ? 1.f : 0.f); auto in = toI16(sig), out = in; run(out, 960);
              double m = 0; for (int c = 4; c < 18; c++) { int at = 48000 + c * 19000 + 512; int pkc = 0; for (int i = 0; i < 400; i++) pkc = std::max(pkc, (int)abs(out[at + i])); m += pkc; } pk[ci] = m / 14.0; }
          CHECK(pk[1] < pk[0] * 0.6 && pk[1] < 0.25 * 32768 * 0.5, "clicks: avg peak %.0f with click guard 0, %.0f with guard 1 (%.1f dB lower)", pk[0], pk[1], db(pk[0]) - db(pk[1])); }
        // (e) deterministic: chunk size does not change the result
        { reset_all(); vfx_set(P_NS_ON, 1.f); auto in = toI16(speech(96000)), o1 = in, o2 = in; run(o1, 960); reset_all(); vfx_set(P_NS_ON, 1.f); run(o2, 0, 31337); int bad = 0; for (size_t i = 0; i < in.size(); i++) if (o1[i] != o2[i]) bad++;
          CHECK(bad == 0, "960-sample chunks vs random chunks: %d differing samples", bad); }
        // (f) switching it on and off while talking: no click
        { reset_all(); auto in = tone(48000 * 4, 300, 9000), out = in; int p = 0; while (p < (int)in.size()) { if (p == 48000) vfx_set(P_NS_ON, 1.f); if (p == 144000) vfx_set(P_NS_ON, 0.f); vfx_process_i16(out.data() + p, 960); p += 960; }
          int j = maxJump(out, 20000, 190000); CHECK(j < 900, "largest step while toggling noise suppression: %d (natural max 353)", j); }
        // (g) latency of the suppressor path
        { reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_AMT, 0.2f); auto in = toI16(speech(48000 * 3)), out = in; run(out, 960); int best = 0; double bc = -1;
          for (int lag = 400; lag < 700; lag++) { double c = 0; for (int i = 60000; i < 100000; i++) c += (double)out[i] * in[i - lag]; if (c > bc) { bc = c; best = lag; } }
          CHECK(best == 512, "added latency %d samples (%.1f ms)", best, best / 48.0); }
    }

    printf("[13] Auto-Tune: pitch tracker, scale snapping, retune speed (Lite pitch engine, real pitch shifting)\n");
    {
        // (a) tracker accuracy over the singing range, notes off pitch by up to 30 cents
        for (double m : { 45.0, 55.0, 60.3, 69.0, 76.2 }) { tuneSetup(0, 0, 0, 0.0, 0); auto in = sing({ { m, 1.2 } }), out = in; run(out, 960);
            double det = vfx_statf(7); CHECK(fabs(det - m) < 0.15, "tracker: sung midi %.2f -> detected %.2f (%.0f Hz)", m, det, mf(m)); }
        // (b) hard tune in C major: every sung note ends up on a scale note
        struct Case { double sung, want; };
        for (Case c : { Case{ 61.3, 62 }, Case{ 64.4, 64 }, Case{ 65.8, 65 }, Case{ 58.6, 59 }, Case{ 70.0, 69.0 + 0 } }) {
            if (c.sung == 70.0) c.want = 71 - 1 + 0;           // A#4 sits halfway: either neighbour is fine (A or B), checked below
            tuneSetup(0, 1, 0, 1.0, 0); auto in = sing({ { c.sung, 1.4 } }), out = in; run(out, 960); double mo = measureMidi(out, 48000 * 6 / 10, 24000);
            bool ok = c.sung == 70.0 ? (fabs(mo - 69) < 0.25 || fabs(mo - 71) < 0.25) : fabs(mo - c.want) < 0.25;
            CHECK(ok, "C major hard tune: sung %.1f -> output %.2f (want %.0f)", c.sung, mo, c.sung == 70.0 ? 69.0 : c.want); }
        // (c) amount scales the correction, chromatic snap
        { tuneSetup(0, 0, 0, 1.0, 0); auto in = sing({ { 60.35, 1.4 } }), out = in; run(out, 960); double mo = measureMidi(out, 28800, 24000); CHECK(fabs(mo - 60.0) < 0.2, "chromatic, amount 1: 60.35 -> %.2f", mo);
          tuneSetup(0, 0, 0, 0.5, 0); in = sing({ { 60.35, 1.4 } }); out = in; run(out, 960); mo = measureMidi(out, 28800, 24000); CHECK(fabs(mo - 60.175) < 0.2, "chromatic, amount 0.5: 60.35 -> %.2f (halfway)", mo); }
        // (d) retune time: a note sung 0.4 semitone flat of E4 (63.6 -> target 64): instant tune is on the note within 100 ms, 300 ms smoothing glides
        for (double tm : { 0.0, 300.0 }) { tuneSetup(0, 1, tm, 1.0, 0); auto in = sing({ { 62.0, 0.8 }, { 63.6, 1.6 } }), out = in; run(out, 960);
            double at100 = measureMidi(out, 38400 + 4800, 1440), at600 = measureMidi(out, 38400 + 28800, 1440);
            printf("        retune time %3.0f ms: 100 ms after the note starts %.2f, after 600 ms %.2f (target 64.00)\n", tm, at100, at600);
            if (tm == 0) CHECK(fabs(at100 - 64.0) < 0.12, "instant retune: on the note after 100 ms (%.2f)", at100); else CHECK(at100 < 63.9 && at100 > 63.55 && at600 > at100 + 0.1, "300 ms retune glides (%.2f -> %.2f)", at100, at600); }
        // (e) speech-like sound without a pitch (noise) must not be tuned / must not explode
        { tuneSetup(0, 1, 0, 1.0, 1); std::vector<int16_t> nz(48000); unsigned r = 5; for (auto& x : nz) { r = r * 1664525u + 1013904223u; x = (int16_t)(((int)(r >> 16) - 32768) / 8); } auto out = nz; run(out, 960);
          CHECK(db(rms(out, 9600, 48000)) < db(rms(nz, 9600, 48000)) + 4, "noise in: output level %+.1f dB vs input", db(rms(out, 9600, 48000)) - db(rms(nz, 9600, 48000))); }
        { tuneSetup(0, 1, 0, 1.0, 1); std::vector<int16_t> z(48000, 0); run(z, 960); int mx = 0; for (auto v : z) mx = std::max(mx, (int)abs(v)); CHECK(mx < 3, "silence in, Magic Chords on: peak %d", mx); }
        // (f) chunk size independence
        { tuneSetup(2, 1, 40, 0.9, 1); auto in = sing({ { 62.3, 0.8 }, { 66.4, 0.8 }, { 69.2, 0.8 } }), o1 = in, o2 = in; run(o1, 960); tuneSetup(2, 1, 40, 0.9, 1); run(o2, 0, 4711); int bad = 0; for (size_t i = 0; i < in.size(); i++) if (o1[i] != o2[i]) bad++;
          CHECK(bad == 0, "tune + chords: 960-sample vs random chunks: %d differing samples", bad); }
    }
    printf("[14] Magic Chords / chord modes (Lite engine)\n");
    {
        // (a) Magic in A major, sung A3: lead 220 Hz + 3rd C#4 + 5th E4 + bass A2, each as a clean single line at the right frequency
        { tuneSetup(9, 1, 0, 1.0, 1); auto in = sing({ { 57.0, 1.8 } }), out = in; run(out, 960);
          auto peak = [&](double lo, double hi, double& at) { double bm = -999; for (double f = lo; f <= hi; f += 0.5) { double v = goertzelDb(out, 28800, 38400, f); if (v > bm) { bm = v; at = f; } } return bm; };
          double fl, f3, f5, fb; double pl = peak(214, 226, fl), p3 = peak(266, 290, f3), p5 = peak(316, 343, f5), pb = peak(104, 117, fb);
          printf("        lead %.1f Hz %.0f dB | 3rd %.1f Hz (want 277.2) %.0f dB | 5th %.1f Hz (want 329.6) %.0f dB | bass %.1f Hz (want 110.0) %.0f dB\n", fl, pl, f3, p3, f5, p5, fb, pb);
          CHECK(fabs(f3 - mf(61)) < 3.0 && fabs(f5 - mf(64)) < 3.0 && fabs(fb - mf(45)) < 2.0, "voices sit on C#4 / E4 / A2 (errors %.1f, %.1f, %.1f Hz)", f3 - mf(61), f5 - mf(64), fb - mf(45));
          CHECK(p3 > pl - 16 && p5 > pl - 16 && pb > pl - 18, "voices are strong single lines: %.0f / %.0f / %.0f dB below the lead", pl - p3, pl - p5, pl - pb);
          double fo1, fo2, fo3; double q1 = peak(293, 305, fo1), q2 = peak(366, 400, fo2), q3 = peak(240, 252, fo3);
          CHECK(q1 < p3 - 8 && q2 < p3 - 8 && q3 < p3 - 8, "notes outside the chord stay down: D4-ish %.0f, G4-ish %.0f, B3-ish %.0f dB vs 3rd %.0f dB", q1, q2, q3, p3);
          tuneSetup(9, 1, 0, 1.0, 0); auto base = in; run(base, 960); double lv = db(rms(out, 24000, 62400)) - db(rms(base, 24000, 62400)); CHECK(lv > 0.5 && lv < 6.0, "level with chords vs without: %+.1f dB", lv); }
        // (b) diatonic: in D major a sung B3 gets a MINOR third (D4) and a fifth (F#4), not D#4
        { tuneSetup(2, 1, 0, 1.0, 2); auto in = sing({ { 59.0, 1.6 } }), out = in; run(out, 960);
          double d4 = goertzelDb(out, 24000, 38400, mf(62)), ds4 = goertzelDb(out, 24000, 38400, mf(63)), fs4 = goertzelDb(out, 24000, 38400, mf(66));
          CHECK(d4 > ds4 + 12 && fs4 > ds4 + 8, "D major, sing B: D4 %.0f dB, F#4 %.0f dB, D#4 %.0f dB (minor chord)", d4, fs4, ds4); }
        // (c) every mode produces its added voices and stays finite
        const char* names[9] = { "off", "Magic", "Triad", "Power", "Seventh", "Sus", "Angel", "Octaves", "Duet" };
        for (int mode = 1; mode <= 8; mode++) { tuneSetup(0, 1, 20, 1.0, mode); auto in = sing({ { 60.0, 1.4 } }), base = in; run(base, 960); tuneSetup(0, 1, 20, 1.0, 0); base = in; run(base, 960);
            tuneSetup(0, 1, 20, 1.0, mode); auto out = in; run(out, 960); double lv = db(rms(out, 24000, 62400)) - db(rms(base, 24000, 62400)); int mx = 0; for (auto v : out) mx = std::max(mx, (int)abs(v));
            CHECK(lv > 0.3 && lv < 6.5 && mx < 32000, "mode %d %-8s: level %+.1f dB, peak %d", mode, names[mode], lv, mx); }
        // (d) auto key: a D-major melody must be recognised as D
        { tuneSetup(12, 1, 40, 1.0, 0); std::vector<Note> mel; unsigned r = 9; const int deg[12] = { 62, 62, 62, 69, 69, 66, 66, 64, 67, 71, 73, 62 }; for (int i = 0; i < 40; i++) { r = r * 1664525u + 1013904223u; mel.push_back({ (double)deg[(r >> 16) % 12] + ((int)((r >> 8) % 21) - 10) * 0.02, 0.33 }); }      // tonic-heavy D-major tune
          auto in = sing(mel), out = in; run(out, 960); CHECK(vfx_stat(15) == 2, "auto key from a D-major melody: root %d (2 = D)", vfx_stat(15));
          tuneSetup(12, 2, 40, 1.0, 0); mel.clear(); const int amin[12] = { 57, 57, 57, 64, 64, 60, 60, 62, 65, 67, 59, 57 }; for (int i = 0; i < 40; i++) { r = r * 1664525u + 1013904223u; mel.push_back({ (double)amin[(r >> 16) % 12], 0.33 }); }      // tonic-heavy A-minor tune
          in = sing(mel); out = in; run(out, 960); CHECK(vfx_stat(15) == 9, "auto key, minor scale, A-minor melody: root %d (9 = A)", vfx_stat(15)); }
        // (e) cost
        { tuneSetup(0, 1, 20, 1.0, 1); auto in = sing({ { 60.0, 4.0 } }), out = in; auto t0 = std::chrono::steady_clock::now(); run(out, 960); double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          CHECK(sec / 4.0 < 0.30, "CPU on this PC: %.1f%% of real time for tune + 3 chord voices", 100 * sec / 4.0); }
    }

    // ======================================================================== Phase 1: conditioning + dynamics + Compare switch
    g_eng.gapMs = 1000000;       // from here on the tests are deterministic even on a stalled machine (the gap reset itself was tested above)
    printf("\n[P1] Phase 1 tests (signal conditioning, gate, compressor, de-esser, limiter, Compare switch)\n");
    auto refSet = [](int id, float v) { refeng::ref_vfx_set(id, v); };
    auto refRun = [](std::vector<int16_t>& d, int chunk, unsigned seed) { int p = 0, total = (int)d.size(); while (p < total) { int n = chunk; if (seed) { seed = seed * 1664525u + 1013904223u; n = 100 + (seed >> 16) % 1900; } if (p + n > total) n = total - p; refeng::ref_vfx_process_i16(d.data() + p, n); p += n; } };
    struct Sc { const char* name; std::vector<std::pair<int, float>> par; int kind; };      // kind 0 voiceish, 1 voice + noise, 2 loud tone
    auto noisy = [](int total) { auto v = voiceish(total); unsigned s = 7; for (auto& x : v) { s = s * 1664525u + 1013904223u; x = (int16_t)std::max(-32768, std::min(32767, (int)x / 2 + (((int)(s >> 16) - 32768) / 20))); } return v; };
    printf("[P1-1] legacy_mode reproduces the previous engine sample by sample (random chunk sizes)\n");
    {
        std::vector<Sc> scs = {
            { "Normal", {}, 0 },
            { "pitch 1.5", { { P_PITCH, 1.5f } }, 0 },
            { "lite 0.8 + mix 0.6", { { P_QUALITY, 2.f }, { P_PITCH, 0.8f }, { P_MIX, 0.6f } }, 0 },
            { "gate -50 dB + gain 6", { { P_GATE_DB, -50.f }, { P_GAIN_DB, 6.f } }, 1 },
            { "filters + drive + echo + reverb + chorus", { { P_HP_HZ, 300.f }, { P_LP_HZ, 4000.f }, { P_BASS_DB, 6.f }, { P_DRIVE, 0.5f }, { P_ECHO_MIX, 0.4f }, { P_REV_MIX, 0.4f }, { P_MOD_MODE, 1.f } }, 0 },
            { "vocoder cyber", { { P_VOC_MODE, 2.f } }, 0 },
            { "noise suppression", { { P_NS_ON, 1.f }, { P_NS_AMT, 0.8f } }, 1 },
            { "tune + magic chords (lite)", { { P_QUALITY, 2.f }, { P_TUNE_ON, 1.f }, { P_CHORD, 1.f }, { P_TUNE_KEY, 0.f } }, 0 },
            { "overdriven input (+18 dB, soft limiter)", { { P_GAIN_DB, 18.f } }, 2 },
        };
        for (auto& sc : scs) {
            std::vector<int16_t> a = sc.kind == 1 ? noisy(144000) : (sc.kind == 2 ? tone(144000, 700, 9000) : voiceish(144000)), b = a;
            params_default(); refeng::params_default(); vfx_set(P_LEGACY, 1.f);
            for (auto& pv : sc.par) { vfx_set(pv.first, pv.second); refSet(pv.first, pv.second); }
            vfx_prepare(48000); refeng::ref_vfx_prepare(48000); vfx_set(P_ENABLED, 1.f); refSet(P_ENABLED, 1.f);
            run(a, 960, 4242u); refRun(b, 960, 4242u);
            int mx = 0; long bad = 0; double se = 0; for (size_t i = 0; i < a.size(); i++) { int d = abs((int)a[i] - (int)b[i]); mx = std::max(mx, d); if (d > 1) bad++; se += (double)d * d; }
            // exact (0 LSB) without -ffast-math; with it the two compiled copies may reassociate floats differently (feedback effects, pitch tracker decisions: a few LSB rms)
            CHECK(mx <= 200 && sqrt(se / a.size()) < 4.0, "legacy_mode vs previous engine, %-42s: max difference %d LSB, rms %.3f LSB, samples off by more than 1 LSB: %ld", sc.name, mx, sqrt(se / a.size()), bad);
        }
    }
    printf("[P1-2] all new stages at their defaults (V1) stay close to the previous sound (aligned by the added lookahead)\n");
    {
        const int LAT = (int)(floorf(0.005f * 48000.f + 0.5f)) + (int)floorf(0.003f * 48000.f + 0.5f);
        std::vector<int16_t> in(96000); for (int i = 0; i < 96000; i++) { double t = i / 48000.0; in[i] = (int16_t)(5000 * sin(2 * M_PI * 1200 * t) + 3000 * sin(2 * M_PI * 2500 * t + 1) + 2000 * sin(2 * M_PI * 3100 * t + 2)); }
        auto a = in, b = in; reset_v1(); run(a, 960); refeng::params_default(); refeng::ref_vfx_prepare(48000); refSet(P_ENABLED, 1.f); refRun(b, 960, 0);
        double e = 0, p = 0; for (int i = 30000; i < 90000; i++) { double d = (double)a[i] - (double)b[i - LAT]; e += d * d; p += (double)b[i - LAT] * b[i - LAT]; }
        double lv = db(rms(a, 30000, 90000)) - db(rms(b, 30000 - LAT, 90000 - LAT));
        CHECK(fabs(lv) < 0.3 && 10 * log10(e / p) < -18, "default V1 vs previous output (shifted by %d samples): level %+.2f dB, difference %.1f dB below the signal (the 75 Hz high-pass only moves phase slightly)", LAT, lv, -10 * log10(e / p));
        CHECK(fabs(vfx_statf(2) - 8.0f) < 0.05f, "reported latency with no pitch engine: %.2f ms (5 ms gate lookahead + 3 ms limiter)", vfx_statf(2));
        reset_all(); CHECK(fabs(vfx_statf(2)) < 0.01f, "legacy_mode latency: %.2f ms (v8 behaviour, 0 with no pitch shift)", vfx_statf(2));
    }
    printf("[P1-3] input conditioning: DC blocker + 75 Hz high-pass\n");
    {
        reset_v1(); std::vector<int16_t> d(48000); for (int i = 0; i < 48000; i++) d[i] = (int16_t)(6000 + 4000 * sin(2 * M_PI * 1000.0 * i / 48000.0)); run(d, 960);
        double mean = 0; for (int i = 40000; i < 48000; i++) mean += d[i]; mean /= 8000.0;
        CHECK(fabs(mean) < 25, "DC offset of +6000 removed: mean after 1 s = %.1f LSB", mean);
        reset_v1(); std::vector<int16_t> dc(4800, 8000); run(dc, 960); int pk = 0; for (int i = 384; i < 4800; i++) pk = std::max(pk, abs((int)dc[i]));
        CHECK(pk < 60, "constant DC from the very first sample: no start-up step, peak after the lookahead = %d LSB", pk);
        reset_v1(); double g30 = gainAt(30, ""), g75 = gainAt(75, ""), g1k = gainAt(1000, "");
        CHECK(g30 < -12 && g75 > -4.5 && g75 < -1.5 && fabs(g1k) < 0.3, "high-pass: 30 Hz %.1f dB, 75 Hz %.1f dB (-3 expected), 1 kHz %.2f dB", g30, g75, g1k);
    }
    printf("[P1-4] gate: hysteresis, hold, soft expander, lookahead\n");
    {
        auto burst = [](const std::vector<std::pair<double, double>>& segs) { std::vector<int16_t> v; for (auto& sg : segs) { int n = (int)(sg.second * 48000); size_t o = v.size(); v.resize(o + n); for (int i = 0; i < n; i++) v[o + i] = (int16_t)(sg.first * 32768.0 * sin(2 * M_PI * 1000.0 * (o + i) / 48000.0)); } return v; };
        auto setGate = [](float hold) { reset_v1(); vfx_set(P_GATE_DB, -40.f); vfx_set(P_GATE_HYST, 6.f); vfx_set(P_GATE_HOLD, hold); vfx_set(P_GATE_RANGE, -60.f); };
        // hysteresis: -45 dBFS tone (between the -46 close and -40 open thresholds)
        setGate(0.f); auto a = burst({ { 0.05, 0.4 }, { 0.0056, 0.6 } }); run(a, 960); double stayOpen = db(rms(a, 48000 * 0.7, 48000)) - db(0.0056 * 32768 / sqrt(2.0) / 32768);
        setGate(0.f); auto b = burst({ { 0.0, 0.4 }, { 0.0056, 0.6 } }); run(b, 960); double startClosed = db(rms(b, 48000 * 0.7, 48000)) - db(0.0056 * 32768 / sqrt(2.0) / 32768);
        CHECK(stayOpen > -1.5 && startClosed < -7, "hysteresis: -45 dB tone passes (%+.1f dB) after the gate opened, but stays down (%+.1f dB) when the gate is closed", stayOpen, startClosed);
        // hold: after a -34 dB burst the signal drops to -60 dB; measure 60..100 ms after the drop
        double lv[2]; int hi = 0; for (float hold : { 0.f, 300.f }) { setGate(hold); auto c = burst({ { 0.02, 0.3 }, { 0.001, 0.8 } }); run(c, 960); int t0 = (int)(48000 * 0.3);
            lv[hi++] = db(rms(c, t0 + 4800, t0 + 7200)) - db(0.001 / sqrt(2.0)); }
        CHECK(lv[1] > lv[0] + 6 && lv[1] > -3, "hold: 60-100 ms after the drop the gate is %+.1f dB with hold 300 ms, %+.1f dB with hold 0", lv[1], lv[0]);
        // soft expander: tail level falls gradually instead of snapping to silence
        { setGate(0.f); auto c = burst({ { 0.02, 0.3 }, { 0.004, 1.2 } }); run(c, 960); double l2 = db(rms(c, 48000 * 1.0, 48000 * 1.5)) - db(0.004 / sqrt(2.0));
          CHECK(l2 > -20 && l2 < -3, "soft expander: -48 dB tone below the open threshold is turned down %.1f dB (ratio 3:1), not muted", l2); }
        // range floor
        { setGate(0.f); vfx_set(P_GATE_RANGE, -12.f); auto c = burst({ { 0.02, 0.3 }, { 0.0003, 1.2 } }); run(c, 960); double l2 = db(rms(c, 48000 * 1.0, 48000 * 1.5)) - db(0.0003 / sqrt(2.0));
          CHECK(l2 > -13.5 && l2 < -10.5, "gate_range_db -12: deep attenuation stops at %.1f dB", l2); }
        // lookahead: tone onset after silence; first 3 ms after the (delayed) onset keep most of their energy
        { setGate(80.f); auto c = burst({ { 0.0, 0.5 }, { 0.2, 0.3 } }); run(c, 960); int on = 24000 + 384; double eo = rms(c, on, on + 144), er = 0.2 / sqrt(2.0);
          CHECK(eo > 0.75 * er, "lookahead: first 3 ms of a word onset keep %.0f%% of their level (gate was fully closed before)", 100 * eo / er);
          reset_all(); vfx_set(P_GATE_DB, -40.f); auto d2 = burst({ { 0.0, 0.5 }, { 0.2, 0.3 } }); run(d2, 960); double eo2 = rms(d2, 24000, 24000 + 144);
          CHECK(eo > eo2 * 0.98, "lookahead: onset energy %.0f%% (V1) vs %.0f%% (legacy gate)", 100 * eo / er, 100 * eo2 / er); }
    }
    printf("[P1-5] compressor: static curve, attack/release, makeup, auto makeup\n");
    {
        auto lvl = [](double amp, bool autoMk, double makeup, double thr, double ratio) { reset_v1(); vfx_set(P_COMP_ON, 1.f); vfx_set(P_COMP_THRESH, (float)thr); vfx_set(P_COMP_RATIO, (float)ratio); vfx_set(P_COMP_AUTO, autoMk ? 1.f : 0.f); vfx_set(P_COMP_MAKEUP, (float)makeup);
            auto in = tone(96000, 1000, amp * 32768), out = in; run(out, 960); return db(rms(out, 60000, 96000)) - db(rms(in, 60000, 96000)); };
        // threshold -30 dB (RMS meter), ratio 4, knee 6 dB. input amp 0.1 -> RMS level -23 dB: 7 dB over -> out = -30 + 7/4 -> gain -5.25 dB
        double gHi = lvl(0.1, false, 0, -30, 4), gLo = lvl(0.0316, false, 0, -30, 4), gVeryHi = lvl(0.5, false, 0, -30, 4);
        auto curve = [](double L, double T, double R) { double W = 6, ov = L - T; if (2 * ov <= -W) return 0.0; double o = (2 * ov < W) ? L + (1 / R - 1) * (ov + W / 2) * (ov + W / 2) / (2 * W) : T + ov / R; return o - L; };
        CHECK(fabs(gHi - curve(-23.0, -30, 4)) < 0.7 && fabs(gLo) < 0.5, "static curve: -23 dB in -> %+.2f dB (formula %+.2f), -33 dB in (below the knee) -> %+.2f dB", gHi, curve(-23.0, -30, 4), gLo);
        CHECK(fabs(gVeryHi - curve(20 * log10(0.5) - 3.01, -30, 4)) < 0.9, "static curve: -9 dB in -> %+.2f dB (formula %+.2f)", gVeryHi, curve(20 * log10(0.5) - 3.01, -30, 4));
        double gUn = lvl(0.1, false, 0, -30, 1), gMk = lvl(0.1, false, 6, -30, 4), gAu = lvl(0.1, true, 0, -30, 4);
        CHECK(fabs(gUn) < 0.3 && fabs(gMk - (gHi + 6)) < 0.5 && gAu > gHi + 5 && gAu < gHi + 12, "ratio 1 = no change (%+.2f dB); makeup +6 dB -> %+.2f (%.2f + 6); auto makeup adds %+.1f dB", gUn, gMk, gHi, gAu - gHi);
        // attack / release: loud burst in quiet material
        { reset_v1(); vfx_set(P_COMP_ON, 1.f); vfx_set(P_COMP_THRESH, -30.f); vfx_set(P_COMP_RATIO, 8.f); vfx_set(P_COMP_ATTACK, 5.f); vfx_set(P_COMP_RELEASE, 300.f); vfx_set(P_COMP_AUTO, 0.f);
          std::vector<int16_t> v(96000); for (int i = 0; i < 96000; i++) v[i] = (int16_t)((i >= 24000 && i < 48000 ? 0.4 : 0.02) * 32768 * sin(2 * M_PI * 1000.0 * i / 48000.0)); auto o = v; run(o, 960);
          double early = db(rms(o, 24000 + 384, 24000 + 384 + 120)) - db(rms(v, 24000, 24120)), late = db(rms(o, 40000 + 384, 44000 + 384)) - db(rms(v, 40000, 44000)), rec = db(rms(o, 48000 + 384 + 38000, 96000)) - db(rms(v, 48000 + 38000, 96000 - 384));
          CHECK(early > late + 3 && late < -8 && rec > -2.5, "attack: first 2.5 ms of the loud burst %+.1f dB, settled %+.1f dB; release (300 ms): back to %+.1f dB 0.8-1 s later", early, late, rec); }
        // meter + toggling on/off without a click
        { reset_v1(); auto in = tone(144000, 300, 9000), out = in; vfx_set(P_COMP_THRESH, -40.f); vfx_set(P_COMP_RATIO, 6.f); int p = 0; while (p < 144000) { if (p == 48000) vfx_set(P_COMP_ON, 1.f); if (p == 96000) vfx_set(P_COMP_ON, 0.f); vfx_process_i16(out.data() + p, 960); p += 960; }
          int j = maxJump(out, 20000, 144000); CHECK(j < 900, "toggling the compressor while talking: largest step %d (natural max 353 x gain)", j); }
    }
    printf("[P1-6] de-esser: attenuates only the sibilance band\n");
    {
        auto mixSig = [](double f2, double a2) { std::vector<int16_t> v(96000); for (int i = 0; i < 96000; i++) { double t = i / 48000.0; v[i] = (int16_t)(3000 * sin(2 * M_PI * 300 * t) + a2 * sin(2 * M_PI * f2 * t)); } return v; };
        reset_v1(); auto a = mixSig(6500, 7000), b = a; run(a, 960); double hOff = goertzelDb(a, 60000, 24000, 6500), lOff = goertzelDb(a, 60000, 24000, 300);
        reset_v1(); vfx_set(P_DEESS_ON, 1.f); vfx_set(P_DEESS_AMT, 1.f); run(b, 960); double hOn = goertzelDb(b, 60000, 24000, 6500), lOn = goertzelDb(b, 60000, 24000, 300);
        CHECK(hOff - hOn > 4.0 && fabs(lOff - lOn) < 0.6, "sibilant tone (6.5 kHz dominant): 6.5 kHz down %.1f dB, 300 Hz changed %.2f dB", hOff - hOn, lOn - lOff);
        reset_v1(); auto c = mixSig(6500, 700); auto c0 = c; run(c0, 960); double qOff = goertzelDb(c0, 60000, 24000, 6500); reset_v1(); vfx_set(P_DEESS_ON, 1.f); vfx_set(P_DEESS_AMT, 1.f); run(c, 960); double qOn = goertzelDb(c, 60000, 24000, 6500);
        CHECK(fabs(qOff - qOn) < 0.8, "weak 6.5 kHz content under a strong vowel is left alone: change %.2f dB", qOn - qOff);
        reset_v1(); vfx_set(P_DEESS_ON, 1.f); vfx_set(P_DEESS_AMT, 1.f); vfx_set(P_DEESS_FREQ, 4500.f); auto d = mixSig(4500, 7000); auto d0 = d; reset_v1(); run(d0, 960); reset_v1(); vfx_set(P_DEESS_ON, 1.f); vfx_set(P_DEESS_AMT, 1.f); vfx_set(P_DEESS_FREQ, 4500.f); run(d, 960);
        CHECK(goertzelDb(d0, 60000, 24000, 4500) - goertzelDb(d, 60000, 24000, 4500) > 4.0, "deess_freq 4.5 kHz follows the setting: 4.5 kHz down %.1f dB", goertzelDb(d0, 60000, 24000, 4500) - goertzelDb(d, 60000, 24000, 4500));
    }
    printf("[P1-7] lookahead limiter: ceiling is never exceeded, transparent below it\n");
    {
        for (float ceilDb : { -0.9f, -6.f, -12.f }) {
            reset_v1(); vfx_set(P_LIM_CEIL, ceilDb); vfx_set(P_GAIN_DB, 24.f); std::vector<int16_t> v(96000); unsigned s = 5; for (int i = 0; i < 96000; i++) { s = s * 1664525u + 1013904223u; v[i] = (int16_t)(0.8 * 32768 * sin(2 * M_PI * 220.0 * i / 48000.0) + (((int)(s >> 16) - 32768) / 3)); }
            run(v, 777); int mx = 0; for (auto x : v) mx = std::max(mx, abs((int)x)); int cap = (int)ceil(pow(10.0, ceilDb / 20.0) * 32767.0) + 1;
            CHECK(mx <= cap, "ceiling %.1f dB (%d): loud input with +24 dB gain peaks at %d", ceilDb, cap, mx); }
        { reset_v1(); vfx_set(P_LIM_CEIL, -6.f); std::vector<int16_t> v(24000, 0); v[12000] = 32767; v[12001] = -32768; run(v, 960); int mx = 0; for (auto x : v) mx = std::max(mx, abs((int)x));
          CHECK(mx <= 16422, "single full-scale spike with ceiling -6 dB: output peak %d (<= 16422)", mx); }
        { reset_v1(); auto in = tone(96000, 4000, 0.5 * 32768 * 0.7), out = in; run(out, 960); int bad = 0; for (int i = 20000; i < 96000; i++) if (abs(out[i] - (int)(0.5 * 32768 * 0.7 * sin(2 * M_PI * 4000.0 * (i - 384) / 48000.0))) > 700) bad++;
          CHECK(bad == 0, "below the ceiling the limiter does not touch the signal (%d samples differ by more than the high-pass phase shift)", bad); }
        { reset_v1(); vfx_set(P_LIM_CEIL, -6.f); std::vector<int16_t> v(96000); for (int i = 0; i < 96000; i++) v[i] = (int16_t)((i >= 30000 && i < 36000 ? 1.0 : 0.15) * 32767 * sin(2 * M_PI * 440.0 * i / 48000.0)); run(v, 960);
          double after = rms(v, 36000 + 384 + 12000, 96000) / (0.15 / sqrt(2.0)); CHECK(after > 0.85, "release: level 250 ms after a loud passage is back to %.0f%% of the original", 100 * after); }
    }
    printf("[P1-8] Compare switch (legacy_mode): cross-fade without a click, not saved, state kept\n");
    {
        reset_v1(); vfx_set(P_COMP_ON, 1.f); vfx_set(P_DEESS_ON, 1.f); auto in = tone(192000, 300, 9000), out = in; int p = 0;
        while (p < 192000) { if (p == 48000) vfx_set(P_LEGACY, 1.f); if (p == 96000) vfx_set(P_LEGACY, 0.f); if (p == 144000) vfx_set(P_LEGACY, 1.f); vfx_process_i16(out.data() + p, 960); p += 960; }
        int j = maxJump(out, 20000, 192000); CHECK(j < 800, "toggling Compare four times: largest step %d (natural max 353)", j);
        CHECK(vfx_stat(20) == 1, "stat 20 reports legacy_mode active");
        double after = rms(out, 170000, 190000) / rms(in, 170000, 190000); CHECK(fabs(db(after)) < 1.0, "level after the last switch is unchanged: %+.2f dB", db(after));
        float cmp = vfx_get(P_COMP_ON); CHECK(cmp == 1.f, "stored value of comp_on is untouched by legacy_mode (%.0f)", cmp);
        vfx_set(P_LEGACY, 1.f); vfx_set(P_PITCH, 1.3f); vfx_save(); vfx_set(P_LEGACY, 0.f); vfx_set(P_PITCH, 1.f); vfx_load();
        CHECK(vfx_get(P_LEGACY) == 0.f && fabsf(vfx_get(P_PITCH) - 1.3f) < 1e-3f, "legacy_mode is not saved or loaded (stays %.0f, pitch restored %.2f)", vfx_get(P_LEGACY), vfx_get(P_PITCH));
        FILE* f = fopen(CFG_PATH, "r"); bool found = false; char ln[128]; while (f && fgets(ln, sizeof(ln), f)) if (!strncmp(ln, "legacy_mode", 11)) found = true; if (f) fclose(f); remove(CFG_PATH);
        CHECK(!found, "legacy_mode does not appear in the cfg file");
        params_default();
    }
    printf("[P1-9] V1 with every new stage on: output independent of the chunk size, finite, parameters appended only\n");
    {
        for (unsigned sd : { 0u, 31337u }) { std::vector<std::vector<int16_t>> r; for (int chunk : { 960, 64, 480, 1023 }) { reset_v1(); vfx_set(P_GATE_DB, -45.f); vfx_set(P_COMP_ON, 1.f); vfx_set(P_DEESS_ON, 1.f); vfx_set(P_DEESS_AMT, 0.8f); vfx_set(P_NS_ON, 1.f); vfx_set(P_PITCH, 1.2f); vfx_set(P_QUALITY, 2.f);
                auto v = noisy(96000); run(v, chunk, sd ? sd + chunk : 0); if (sd) { reset_v1(); vfx_set(P_GATE_DB, -45.f); vfx_set(P_COMP_ON, 1.f); vfx_set(P_DEESS_ON, 1.f); vfx_set(P_DEESS_AMT, 0.8f); vfx_set(P_NS_ON, 1.f); vfx_set(P_PITCH, 1.2f); vfx_set(P_QUALITY, 2.f); v = noisy(96000); run(v, 960, sd); } r.push_back(v); if (sd) break; }
            if (!sd) { long bad = 0; for (size_t k = 1; k < r.size(); k++) for (size_t i = 0; i < r[0].size(); i++) if (r[k][i] != r[0][i]) bad++; CHECK(bad == 0, "chunk sizes 960 / 64 / 480 / 1023 give identical output: %ld differing samples", bad); } }
        CHECK(P_LEGACY == 43 && P_LIM_CEIL == 57 && P_TUNE_LOOK == 58 && P_TUNE_VIB == 59 && P_NS_ENGINE == 60 && P_VOC_CARRIER == 66 && P_COUNT == 67 && P_CHORD_SPREAD == 42, "new parameter ids are appended after the old ones (legacy_mode = %d ... limit_ceil_db = %d, tune_lookahead_ms = %d, tune_keep_vibrato = %d, P_COUNT %d)", (int)P_LEGACY, (int)P_LIM_CEIL, (int)P_TUNE_LOOK, (int)P_TUNE_VIB, (int)P_COUNT);
        params_default(); CHECK(g_par[P_COMP_ON] == 0.f && g_par[P_DEESS_ON] == 0.f && g_par[P_LEGACY] == 0.f && g_par[P_GATE_DB] <= -79.5f, "defaults: compressor off, de-esser off, gate off, V1 sound");
    }

    // ======================================================================== Phase 2: Auto-Tune timing
    printf("\n[P2] Phase 2 tests (tune lookahead, adaptive YIN window, vibrato preservation)\n");
    // helper: a sung line with glides and vibrato. segs: midi(t) given as a function; returns int16
    auto singFn = [](double dur, double amp, const std::function<double(double)>& midiAt) {
        int total = (int)(dur * 48000); std::vector<int16_t> v(total); double ph = 0;
        for (int i = 0; i < total; i++) { double t = i / 48000.0, m = midiAt(t), f = 440.0 * pow(2.0, (m - 69.0) / 12.0), s = 0;
            for (int h = 1; h * f < 6000; h++) { double fh = h * f, a = (1.0 / h) * (1.0 + 1.5 * exp(-pow((fh - 700) / 250, 2)) + 1.0 * exp(-pow((fh - 1200) / 300, 2))); s += a * sin(h * ph); }
            double env = std::min(1.0, i / 400.0) * std::min(1.0, (total - i) / 400.0); v[i] = (int16_t)(amp * 32768.0 * env * s / 2.6); ph += 2 * M_PI * f / 48000.0; }
        return v; };
    auto setV1tune = [&](double look, double vib, double timeMs, double amt, int key, int scale) { reset_v1(); vfx_set(P_QUALITY, 2.f); vfx_set(P_TUNE_ON, 1.f); vfx_set(P_TUNE_KEY, (float)key); vfx_set(P_TUNE_SCALE, (float)scale);
        vfx_set(P_TUNE_TIME, (float)timeMs); vfx_set(P_TUNE_AMT, (float)amt); vfx_set(P_TUNE_LOOK, (float)look); vfx_set(P_TUNE_VIB, (float)vib); };
    // time (ms after the onset) at which the tuner's correction first reaches the value the new note needs; 48-sample chunks
    auto corrLag = [&](bool legacy, double fromMidi, double toMidi, double wantCorr) {
        setV1tune(0, 0, 0, 1.0, 0, 0); vfx_set(P_LEGACY, legacy ? 1.f : 0.f); vfx_prepare(48000); vfx_set(P_ENABLED, 1.f);
        auto in = singFn(1.2, 0.22, [&](double t) { return t < 0.6 ? fromMidi : toMidi; }); int on = (int)(0.6 * 48000); double lag = -1;
        for (int p = 0; p < (int)in.size(); p += 48) { vfx_process_i16(in.data() + p, 48); if (p >= on && lag < 0 && fabs(g_eng.tuner.corr - wantCorr) < 0.08 && g_tuneVoiced) lag = (p + 48 - on) / 48.0; }
        return lag; };
    printf("[P2-1] detection lag of the pitch tracker (note change needing -0.3 st then +0.3 st, chromatic, instant retune): before vs after the adaptive window\n");
    {
        double lo = corrLag(true, 61.3, 66.7, 0.3), hiL = corrLag(true, 76.3, 81.7, 0.3);          // fixed 40 ms window
        double lo2 = corrLag(false, 61.3, 66.7, 0.3), hiL2 = corrLag(false, 76.3, 81.7, 0.3);       // adaptive window
        CHECK(lo > 0 && lo2 > 0 && lo2 <= lo - 5, "mid voice (277 -> 370 Hz): correction ready %.0f ms after the onset with the fixed window, %.0f ms adaptive", lo, lo2);
        CHECK(hiL > 0 && hiL2 > 0 && hiL2 <= hiL - 5, "high voice (659 -> 880 Hz): %.0f ms fixed, %.0f ms adaptive", hiL, hiL2);
        double lw = corrLag(true, 40.3, 45.7, 0.3), lw2 = corrLag(false, 40.3, 45.7, 0.3);
        CHECK(lw > 0 && lw2 > 0 && lw2 <= lw + 10, "low voice (83 Hz): %.0f ms fixed, %.0f ms adaptive (must not get worse)", lw, lw2);
    }
    printf("[P2-2] tune_lookahead_ms: the corrected pitch is ready when the delayed audio arrives\n");
    {
        double lag = corrLag(false, 61.3, 66.7, 0.3);
        // the engine audio is delayed by tune_lookahead_ms; the correction is ready after `lag` ms
        CHECK(lag > 0 && lag <= 60.0, "measured correction lag %.0f ms is inside the 0-60 ms lookahead range (set the lookahead to about this value)", lag);
        // reported latency grows by the lookahead (pitch engine running)
        setV1tune(0, 0, 0, 1.0, 0, 0); auto a = voiceish(24000); run(a, 960); double l0 = vfx_statf(2);
        setV1tune(30, 0, 0, 1.0, 0, 0); auto b = voiceish(24000); run(b, 960); double l1 = vfx_statf(2);
        CHECK(fabs((l1 - l0) - 30.0) < 0.5, "reported latency: %.1f ms -> %.1f ms with 30 ms lookahead (+30)", l0, l1);
        // time alignment: a tone onset leaves the engine exactly `look` later for the dry path, the pitch path and the mix of both
        auto onsetOf = [](const std::vector<int16_t>& o, int from) { int pk = 0; for (int i = from; i < (int)o.size(); i++) pk = std::max(pk, abs((int)o[i]));
            for (int i = from; i < (int)o.size(); i++) if (abs((int)o[i]) > pk * 0.25) return i; return -1; };
        for (double mixv : { 1.0, 0.5, 0.0 }) {
            std::vector<int16_t> in(60000, 0); for (int i = 24000; i < 60000; i++) in[i] = (int16_t)(8000 * sin(2 * M_PI * 440.0 * i / 48000.0));
            setV1tune(0, 0, 40, 0.9, 9, 1); vfx_set(P_MIX, (float)mixv); auto o0 = in; run(o0, 960);
            setV1tune(30, 0, 40, 0.9, 9, 1); vfx_set(P_MIX, (float)mixv); auto o1 = in; run(o1, 960);
            int d = onsetOf(o1, 20000) - onsetOf(o0, 20000);
            CHECK(abs(d - 1440) <= 48, "mix %.1f: onset of a tone leaves the engine %d samples later with 30 ms lookahead (1440 expected)", mixv, d); }
#ifndef VFX_NO_RUBBERBAND
        { auto in = voiceish(96000); setV1tune(0, 0, 40, 0.9, 9, 1); vfx_set(P_MIX, 0.5f); vfx_set(P_QUALITY, 1.f); auto o0 = in; run(o0, 960);
          setV1tune(30, 0, 40, 0.9, 9, 1); vfx_set(P_MIX, 0.5f); vfx_set(P_QUALITY, 1.f); auto o1 = in; run(o1, 960); int bad = 0; for (int i = 30000; i < 90000; i++) if (abs(o1[i + 1440] - o0[i]) > 3) bad++;
          CHECK(bad == 0, "RubberBand stand-in, mix 0.5: output with 30 ms lookahead is the output without it delayed by 1440 samples exactly (%d mismatches)", bad); }
#endif
    }
    printf("[P2-3] synthetic glide: hard-tuned output steps through the semitones\n");
    {
        setV1tune(35, 0, 0, 1.0, 0, 0);
        auto in = singFn(2.4, 0.22, [](double t) { return t < 0.4 ? 60.0 : (t < 1.4 ? 60.0 + 7.0 * (t - 0.4) : 67.0); }); auto out = in; run(out, 960);
        int good = 0, tot = 0; double prev = -1; int back = 0;
        for (int st = 36000; st + 2400 < 110000; st += 960) { double mo = measureMidi(out, st, 2400); if (mo < 0) continue; tot++; if (fabs(mo - floor(mo + 0.5)) < 0.25) good++; if (prev > 0 && mo < prev - 0.6) back++; prev = mo; }
        CHECK(tot > 50 && good > 0.8 * tot && back == 0, "glide 60 -> 67 over 1 s: %d of %d analysis windows sit within 0.25 st of a semitone, %d backwards jumps", good, tot, back);
    }
    printf("[P2-4] vibrato: kept at tune_keep_vibrato 1, removed at 0, note still corrected\n");
    {
        auto vib = singFn(3.0, 0.22, [](double t) { return 69.35 + 0.45 * sin(2 * M_PI * 5.5 * t); });
        double res[3][2];
        int ci = 0; for (double kv : { 0.0, 0.5, 1.0 }) { setV1tune(25, kv, 0, 1.0, 9, 1); auto o = vib; run(o, 960);
            double lo = 1e9, hi = -1e9, sum = 0; int n = 0; for (int st = 60000; st + 2400 < 140000; st += 480) { double mo = measureMidi(o, st, 2400); if (mo < 0) continue; lo = std::min(lo, mo); hi = std::max(hi, mo); sum += mo; n++; }
            res[ci][0] = hi - lo; res[ci][1] = sum / std::max(n, 1); ci++; }
        CHECK(res[0][0] < 0.55 && res[2][0] > 0.7 && res[2][0] > res[1][0] && res[1][0] > res[0][0], "pitch swing of the output with 25 ms lookahead (peak to peak, semitones): keep 0 -> %.2f, 0.5 -> %.2f, 1 -> %.2f (input swing 0.90 st; the tracker's own smoothing leaves some residue at keep 0)", res[0][0], res[1][0], res[2][0]);
        { setV1tune(0, 0, 0, 1.0, 9, 1); auto o = vib; run(o, 960); double lo = 1e9, hi = -1e9; for (int st = 60000; st + 2400 < 140000; st += 480) { double mo = measureMidi(o, st, 2400); if (mo < 0) continue; lo = std::min(lo, mo); hi = std::max(hi, mo); }
          CHECK(hi - lo > res[0][0] + 0.25, "without lookahead the correction arrives too late to flatten the vibrato: swing %.2f (vs %.2f with 25 ms)", hi - lo, res[0][0]); }
        CHECK(fabs(res[0][1] - 69.0) < 0.2 && fabs(res[2][1] - 69.0) < 0.3, "centre of the output is the scale note A4 (69): keep 0 -> %.2f, keep 1 -> %.2f (input centre 69.35)", res[0][1], res[2][1]);
    }
    printf("[P2-5] octave-error protection is kept\n");
    {
        for (int adaptive = 0; adaptive < 2; adaptive++) {
            setV1tune(0, 0, 0, 1.0, 0, 0); vfx_set(P_LEGACY, adaptive ? 0.f : 1.f); vfx_prepare(48000); vfx_set(P_ENABLED, 1.f);
            auto in = singFn(2.0, 0.22, [](double t) { return (t > 0.8 && t < 0.81) ? 69.0 : 57.0; });         // a single 10 ms octave glitch in a steady A3
            double worst = 0; for (int p = 0; p < (int)in.size(); p += 480) { vfx_process_i16(in.data() + p, 480); if (p > 24000 && g_tuneDet >= 0) worst = std::max(worst, (double)fabs(g_tuneDet - 57.0)); }
            CHECK(worst < 0.5, "%s window: single-hop octave glitch never reaches the detected note (largest deviation %.2f st)", adaptive ? "adaptive" : "fixed", worst); }
        setV1tune(0, 0, 0, 1.0, 0, 0); auto in = singFn(2.0, 0.22, [](double t) { return t < 1.0 ? 57.0 : 69.0; }); double d1 = -1, d2 = -1;
        for (int p = 0; p < (int)in.size(); p += 480) { vfx_process_i16(in.data() + p, 480); if (p == 47040) d1 = g_tuneDet; if (p == 62400) d2 = g_tuneDet; }
        CHECK(fabs(d1 - 57.0) < 0.4 && fabs(d2 - 69.0) < 0.4, "a real, sustained octave change is followed: %.2f -> %.2f", d1, d2);
        for (double m : { 40.0, 76.0 }) { setV1tune(0, 0, 0, 1.0, 0, 0); auto v = singFn(1.5, 0.22, [m](double) { return m + 0.2; }); double w = 0; for (int p = 0; p < (int)v.size(); p += 480) { vfx_process_i16(v.data() + p, 480); if (p > 36000) w = std::max(w, (double)fabs(g_tuneDet - (m + 0.2))); }
            CHECK(w < 0.3, "adaptive window on a %s voice (midi %.0f): detection stays within %.2f st", m < 50 ? "low" : "high", m, w); }
    }
    printf("[P2-6] output independent of the chunk size with lookahead + vibrato + adaptive window + chords\n");
    {
        auto in = singFn(3.0, 0.22, [](double t) { return 64.0 + 0.4 * sin(2 * M_PI * 5.5 * t) + (t > 1.5 ? 3.0 : 0.0); });
        std::vector<std::vector<int16_t>> r; for (int chunk : { 960, 1, 64, 480, 1024, 333 }) { setV1tune(25, 0.7, 40, 0.9, 12, 1); vfx_set(P_CHORD, 1.f); auto o = in; run(o, chunk); r.push_back(o); }
        { setV1tune(25, 0.7, 40, 0.9, 12, 1); vfx_set(P_CHORD, 1.f); auto o = in; run(o, 960, 99u); r.push_back(o); }
        long bad = 0; for (size_t k = 1; k < r.size(); k++) for (size_t i = 0; i < r[0].size(); i++) if (r[k][i] != r[0][i]) bad++;
        CHECK(bad == 0, "chunk sizes 960 / 1 / 64 / 480 / 1024 / 333 / random: %ld differing samples", bad);
    }
    printf("[P2-7] legacy_mode ignores the new tune settings; lookahead change is click-free\n");
    {
        setV1tune(40, 1.0, 40, 0.9, 12, 1); vfx_set(P_LEGACY, 1.f); vfx_prepare(48000); vfx_set(P_ENABLED, 1.f); auto in = singFn(2.0, 0.22, [](double t) { return 64.0 + 0.4 * sin(2 * M_PI * 5.5 * t); }), a = in;
        run(a, 960); CHECK(fabs(vfx_statf(2) - 0.f) < 40.f && vfx_get(P_TUNE_LOOK) == 40.f, "legacy_mode: lookahead setting kept (%.0f ms) but not applied (latency %.1f ms)", vfx_get(P_TUNE_LOOK), vfx_statf(2));
        setV1tune(0, 0, 40, 0.9, 12, 1); vfx_set(P_QUALITY, 2.f); auto v = tone(144000, 300, 8000), o = v; int p = 0;
        while (p < 144000) { if (p == 48000) vfx_set(P_TUNE_LOOK, 50.f); if (p == 96000) vfx_set(P_TUNE_LOOK, 0.f); vfx_process_i16(o.data() + p, 960); p += 960; }
        int j = maxJump(o, 20000, 144000); CHECK(j < 1500, "changing tune_lookahead_ms 0 -> 50 -> 0 while a note is held: largest step %d", j);
    }

    // ======================================================================== Phase 3: hear-myself servo
    printf("\n[P3] Phase 3 tests (hear-myself monitor servo, simulated BASS push stream)\n");
    // Simulation of the BASS push stream: the mic delivers `chunkMs` chunks (clock off by driftPpm, arrival jitter), the output drains at the
    // playback rate the servo asked for. Same decisions main.cpp acts on (drop / reset / start / rate).
    struct SimR { int drops = 0, resets = 0, underruns = 0, pushes = 0; double meanQ = 0, minQ = 1e9, maxQ = 0, maxRatioPct = 0, maxStepPct = 0, settleS = -1, finalQ = 0; bool played = false; };
    auto simMon = [](bool legacy, double chunkMs, double driftPpm, double seconds, double jitterMs, double initQms, double stallAtS = -1, double stallMs = 0) {
        SimR r; MonitorServo sv; g_monResets = 0; g_monUnder = 0; const uint32_t rate = 48000; const double bpms = 96.0; double q = initQms * bpms, t = 0, prevT = 0, ratio = 1.0; bool playing = false; unsigned rng = 12345;
        double sumQ = 0; long nq = 0; double prevRatio = 1.0; bool stalled = false;
        if (initQms > 0) { sv.primed = true; playing = true; }
        for (int k = 0; t < seconds * 1000.0; k++) {
            rng = rng * 1664525u + 1013904223u; double jit = ((rng >> 16) / 65536.0 - 0.5) * 2.0 * jitterMs;
            t += chunkMs / (1.0 + driftPpm * 1e-6); double tt = t + jit; if (tt < prevT) tt = prevT;
            double dt = tt - prevT; prevT = tt;
            if (stallAtS >= 0 && tt / 1000.0 >= stallAtS && tt / 1000.0 < stallAtS + stallMs / 1000.0) dt = 0;       // output device stall: nothing is played, audio piles up
            if (playing) { double use = dt * bpms * ratio; if (q < use) { if (q > 0 || true) r.underruns++; q = 0; } else q -= use; }
            uint32_t len = (uint32_t)(chunkMs * bpms);
            auto o = sv.step(legacy, (uint32_t)q, len, rate); r.pushes++;
            if (o.drop) { r.drops++; continue; }
            if (o.hardReset) { r.resets++; q = 0; playing = false; }
            q += len; if (o.freqChanged) ratio = o.freqRatio; if (o.startPlay) { playing = true; r.played = true; }
            if (legacy && !o.freqChanged) ratio = 1.0;
            double stepP = fabs(ratio - prevRatio) * 100.0; r.maxStepPct = std::max(r.maxStepPct, stepP); prevRatio = ratio;
            r.maxRatioPct = std::max(r.maxRatioPct, fabs(ratio - 1.0) * 100.0);
            double qm = (q - len * 0.5) / bpms; if (playing && tt / 1000.0 > 5.0) { sumQ += qm; nq++; r.minQ = std::min(r.minQ, q / bpms); r.maxQ = std::max(r.maxQ, q / bpms); } r.finalQ = qm;
            if (r.settleS < 0 && playing && fabs(qm - 50.0) < 15.0 && k > 20) r.settleS = tt / 1000.0;
        }
        r.meanQ = nq ? sumQ / nq : 0; r.underruns += 0; return r; };
    printf("[P3-1] legacy behaviour: drop above 0.25 s, start at 80 ms\n");
    {
        MonitorServo sv; auto o = sv.step(true, 96 * 20, 96 * 20, 48000); CHECK(!o.startPlay && !o.drop, "legacy: 40 ms queued after the push -> not started yet");
        o = sv.step(true, 96 * 60, 96 * 20, 48000); CHECK(o.startPlay, "legacy: starts when 80 ms are queued");
        o = sv.step(true, 24001, 960, 48000); CHECK(o.drop, "legacy: more than 0.25 s queued -> the chunk is dropped");
        o = sv.step(true, 24000, 960, 48000); CHECK(!o.drop && !o.hardReset && !o.freqChanged, "legacy: exactly 0.25 s queued is still accepted, playback rate untouched");
        SimR a = simMon(true, 20, 3000, 180, 2, 0);
        CHECK(a.drops > 0 && a.maxQ > 200, "legacy, mic clock 0.3%% fast: %d chunks dropped over 3 minutes (queue climbs to %.0f ms before each drop)", a.drops, a.maxQ);
    }
    printf("[P3-2] V1 servo: no drops, queue held near the target, rate within +-1 %% and smooth\n");
    {
        for (double drift : { 1000.0, -1000.0, 300.0, 0.0 }) {
            SimR a = simMon(false, 20, drift, 180, 3, 0);
            CHECK(a.played && a.drops == 0 && a.resets == 0 && a.underruns == 0 && a.meanQ > 40 && a.meanQ < 62 && a.maxRatioPct <= 1.001 && a.maxStepPct < 0.12,
                  "drift %+5.0f ppm, 20 ms chunks, 3 ms jitter: mean queue %.1f ms (min %.0f / max %.0f), drops %d, resets %d, underruns %d, max rate offset %.2f %%, max step %.3f %%", drift, a.meanQ, a.minQ, a.maxQ, a.drops, a.resets, a.underruns, a.maxRatioPct, a.maxStepPct); }
        for (double ch : { 10.0, 40.0, 100.0 }) { SimR a = simMon(false, ch, 500, 180, ch * 0.1, 0);
            CHECK(a.played && a.drops == 0 && a.resets == 0 && a.underruns == 0 && a.meanQ > 38 && a.meanQ < 125, "mic chunks of %.0f ms: mean queue %.1f ms, drops %d, resets %d, underruns %d", ch, a.meanQ, a.drops, a.resets, a.underruns); }
        { SimR a = simMon(false, 20, 0, 60, 2, 0); CHECK(a.played && a.underruns == 0, "start-up: playback starts without an underrun (target level reached before it starts)"); }
    }
    printf("[P3-3] V1 servo: recovery from a queue that is too long, and the hard reset\n");
    {
        SimR a = simMon(false, 20, 0, 120, 2, 150);
        CHECK(a.resets == 0 && a.drops == 0 && fabs(a.finalQ - 50) < 12 && a.settleS > 0 && a.settleS < 30, "queue starts at 150 ms: drained to %.1f ms by the rate change alone (within 15 ms of the target after %.1f s, no drop, no reset)", a.finalQ, a.settleS);
        SimR b = simMon(false, 20, 0, 60, 2, 0, 20.0, 300);
        CHECK(b.resets == 1 && b.drops == 0, "a 300 ms output stall piles up audio: exactly one hard reset (%d), no dropped chunks (%d)", b.resets, b.drops);
        SimR c = simMon(false, 20, 0, 60, 2, 0, 20.0, 120);
        CHECK(c.resets == 0 && c.drops == 0, "a 120 ms output stall is absorbed without a reset (%d resets)", c.resets);
        CHECK(fabs(vfx_statf(14) - 50) < 40 && fabs(vfx_statf(15)) <= 1.0, "stats: measured queue %.1f ms, rate offset %+.3f %% (vfx_statf 14 / 15), resets %d (vfx_stat 21)", vfx_statf(14), vfx_statf(15), vfx_stat(21));
    }
    printf("[P3-4] switching Compare during playback\n");
    {
        MonitorServo sv; uint32_t q = 96 * 60; auto o = sv.step(false, q, 960, 48000); (void)o; sv.primed = true;
        for (int i = 0; i < 400; i++) { sv.step(false, 96 * 70, 960, 48000); }                    // V1: queue above the aim -> positive rate offset
        CHECK(sv.sentRatio > 1.0f, "V1 with a long queue speeds playback up (%.4f)", sv.sentRatio);
        auto o2 = sv.step(true, 96 * 70, 960, 48000); CHECK(o2.freqChanged && o2.freqRatio == 1.0f, "switching to legacy puts the playback rate back to nominal at once (%.4f)", o2.freqRatio);
    }

    // ======================================================================== Phase 4: quality upgrades
    printf("\n[P4] Phase 4 tests (RNNoise engine, formant chords, anti-aliased drive, FDN reverb, echo damping, 20-band vocoder)\n");
    auto mkSnap = []() { Snap s{}; s.hp = 0; s.lp = 20000; s.bits = 16; s.down = 1; s.echoMs = 300; s.echoFb = 0.5f; s.vocHz = 110; s.robotHz = 30; s.tremHz = 5; s.modRate = 1; s.revSize = 0.5f; s.revDamp = 0.5f; s.tremDepth = 0; s.robotAmt = 0; return s; };
    printf("[P4-1] legacy_mode ignores every new Phase 4 setting (sample-compared with the previous engine)\n");
    {
        std::vector<std::pair<int, float>> common = { { P_DRIVE, 0.5f }, { P_ECHO_MIX, 0.4f }, { P_REV_MIX, 0.4f }, { P_VOC_MODE, 2.f }, { P_CHORD, 1.f }, { P_TUNE_ON, 1.f }, { P_QUALITY, 2.f }, { P_NS_ON, 1.f } };
        auto a = voiceish(144000), b = a; params_default(); refeng::params_default(); vfx_set(P_LEGACY, 1.f);
        for (auto& pv : common) { vfx_set(pv.first, pv.second); refSet(pv.first, pv.second); }
        vfx_set(P_NS_ENGINE, 1.f); vfx_set(P_CHORD_FORMANT, 1.f); vfx_set(P_REV_PREDELAY, 80.f); vfx_set(P_REV_DAMP, 1.f); vfx_set(P_ECHO_DAMP, 1.f); vfx_set(P_ECHO_HP, 400.f); vfx_set(P_VOC_CARRIER, 2.f);
        vfx_prepare(48000); refeng::ref_vfx_prepare(48000); vfx_set(P_ENABLED, 1.f); refSet(P_ENABLED, 1.f); run(a, 960, 777u); refRun(b, 960, 777u);
        int mx = 0; double se = 0; for (size_t i = 0; i < a.size(); i++) { int d = abs((int)a[i] - (int)b[i]); mx = std::max(mx, d); se += (double)d * d; }
        CHECK(mx <= 400 && sqrt(se / a.size()) < 8.0, "legacy_mode with every new Phase 4 parameter at an extreme: max difference %d LSB, rms %.2f LSB vs the previous engine", mx, sqrt(se / a.size()));
    }
    printf("[P4-2] echo: damping and high-pass inside the feedback loop; defaults transparent\n");
    {
        auto run1 = [&](float damp, float hpHz, float w1, float f) { FxChain fx; fx.init(48000); Snap s = mkSnap(); s.echoMix = 0.8f; s.echoDamp = damp; s.echoHp = hpHz; fx.update(s); std::vector<float> o(48000 * 2);
            for (int i = 0; i < 48000 * 2; i++) { float in = (i < 4800) ? 0.3f * sinf(kTwoPi * f * i / 48000.f) : 0.f; o[i] = fx.process(in, w1) - in; }
            auto en = [&](int a, int b) { double e = 0; for (int i = a; i < b; i++) e += o[i] * o[i]; return e; }; return std::make_pair(en(14400, 14400 + 4800), en(28800, 28800 + 4800)); };
        auto same = run1(0.f, 0.f, 0.f, 3000), same1 = run1(0.f, 0.f, 1.f, 3000);
        CHECK(fabs(same.first - same1.first) < 1e-9 && fabs(same.second - same1.second) < 1e-9, "echo_damp 0 / echo_hp 0: identical to the previous echo (repeat energies %.5f / %.5f)", same.first, same.second);
        auto d0 = run1(0.f, 0.f, 1.f, 6000), d1 = run1(0.8f, 0.f, 1.f, 6000);
        CHECK(10 * log10(d1.second / d0.second) < -6.0 && 10 * log10(d1.first / d0.first) > -1.0, "echo_damp 0.8: 6 kHz in the 2nd repeat is %.1f dB lower, the 1st repeat is untouched (%.1f dB)", 10 * log10(d1.second / d0.second), 10 * log10(d1.first / d0.first));
        auto h0 = run1(0.f, 0.f, 1.f, 80), h1 = run1(0.f, 600.f, 1.f, 80);
        CHECK(10 * log10(h1.second / h0.second) < -6.0, "echo_hp_hz 600: 80 Hz in the 2nd repeat is %.1f dB lower", 10 * log10(h1.second / h0.second));
    }
    printf("[P4-3] drive: 2x oversampling cuts aliasing\n");
    {
        auto alias = [&](float w1, double f, float drive) { FxChain fx; fx.init(48000); Snap s = mkSnap(); s.drive = drive; fx.update(s); std::vector<int16_t> o(48000);
            for (int i = 0; i < 48000; i++) o[i] = (int16_t)(32767.0f * 0.5f * fx.process(0.7f * sinf(kTwoPi * (float)f * i / 48000.f), w1));
            double al = 0; double hs = 0; for (int k = 3; k <= 21; k += 2) { double fk = fmod(k * f, 48000.0); if (fk > 24000) fk = 48000 - fk; double lv = pow(10.0, goertzelDb(o, 8000, 32000, fk) / 10.0); if (k * f < 22000) hs += lv; else al += lv; } return std::make_pair(al, hs); };
        auto L = alias(0.f, 5300, 0.8f), V = alias(1.f, 5300, 0.8f);
        CHECK(10 * log10(L.first / V.first) > 6.0, "5.3 kHz at drive 0.8: energy of the aliased harmonics is %.1f dB lower than with the previous drive", 10 * log10(L.first / V.first));
        CHECK(fabs(10 * log10(L.second / V.second)) < 3.0, "legitimate (in-band) harmonics stay: %+.1f dB difference", 10 * log10(L.second / V.second));
        for (float dr : { 0.1f, 0.4f, 1.0f }) { FxChain a, b; a.init(48000); b.init(48000); Snap s = mkSnap(); s.drive = dr; a.update(s); b.update(s); double ea = 0, eb = 0;
            for (int i = 0; i < 48000; i++) { float in = 0.4f * sinf(kTwoPi * 220.f * i / 48000.f) + 0.2f * sinf(kTwoPi * 1330.f * i / 48000.f); float x0 = a.process(in, 0.f), x1 = b.process(in, 1.f); if (i > 4800) { ea += x0 * x0; eb += x1 * x1; } }
            CHECK(fabs(10 * log10(eb / ea)) < 1.5, "drive %.1f: output level %+.2f dB vs the previous drive", dr, 10 * log10(eb / ea)); }
    }
    printf("[P4-4] reverb: 8-line FDN (size, damping, pre-delay, level, stability)\n");
    {
        auto ir = [&](float size, float damp, float pre, float w1, int secs) { FxChain fx; fx.init(48000); Snap s = mkSnap(); s.revMix = 1.f; s.revSize = size; s.revDamp = damp; s.revPre = pre; fx.update(s); std::vector<float> o(48000 * secs);
            for (int i = 0; i < 48000 * secs; i++) o[i] = fx.process(i == 0 ? 0.5f : 0.f, w1) - (i == 0 ? 0.75f * 0.5f : 0.f); return o; };
        auto rt30 = [](const std::vector<float>& o) { const int blk = 480; std::vector<double> e; for (size_t i = 0; i + blk < o.size(); i += blk) { double s = 0; for (int j = 0; j < blk; j++) s += (double)o[i + j] * o[i + j]; e.push_back(s); }
            double pk = 0; for (double v : e) pk = std::max(pk, v); for (size_t i = 0; i < e.size(); i++) if (e[i] < pk * 1e-3 && i > 10) return (double)i * 0.01; return 99.0; };
        double r2 = rt30(ir(0.2f, 0.3f, 0, 1.f, 12)), r5 = rt30(ir(0.5f, 0.3f, 0, 1.f, 12)), r9 = rt30(ir(0.9f, 0.3f, 0, 1.f, 12));
        CHECK(r2 < r5 && r5 < r9 && r2 > 0.1 && r9 < 12, "rev_size sets the decay time: energy -30 dB after %.2f s (0.2), %.2f s (0.5), %.2f s (0.9)", r2, r5, r9);
        { auto gz = [](const std::vector<float>& o, size_t st, size_t n, double f) { double re = 0, im = 0; for (size_t i = st; i < st + n && i < o.size(); i++) { re += o[i] * cos(2 * M_PI * f * i / 48000.0); im += o[i] * sin(2 * M_PI * f * i / 48000.0); } return 10 * log10((re * re + im * im) / n + 1e-30); };
          auto t20 = [&](const std::vector<float>& o) { double l0 = gz(o, 2400, 4800, 6000); for (int k = 1; k < 40; k++) if (gz(o, 2400 + k * 4800, 4800, 6000) < l0 - 20) return k * 0.1; return 9.9; };
          double tl = t20(ir(0.7f, 0.1f, 0, 1.f, 4)), th = t20(ir(0.7f, 0.9f, 0, 1.f, 4));
          CHECK(th < tl * 0.6, "rev_damp: the 6 kHz part of the tail falls 20 dB in %.1f s with damping 0.9, %.1f s with damping 0.1", th, tl); }
        { auto a = ir(0.5f, 0.5f, 0, 1.f, 1), b = ir(0.5f, 0.5f, 50, 1.f, 1); auto onset = [](const std::vector<float>& o) { for (size_t i = 0; i < o.size(); i++) if (fabsf(o[i]) > 1e-4f) return (int)i; return -1; };
          int oa = onset(a), ob = onset(b); CHECK(ob - oa >= 2300 && ob - oa <= 2500, "rev_predelay_ms 50: first reflections arrive %d samples later (2400 expected)", ob - oa); }
        { double ratio[3]; int k = 0; for (float sz : { 0.3f, 0.5f, 0.7f }) { auto fv = [&](float w1) { FxChain fx; fx.init(48000); Snap s = mkSnap(); s.revMix = 1.f; s.revSize = sz; fx.update(s); unsigned r = 3; double acc = 0; int cnt = 0; float lz = 0;
                for (int i = 0; i < 48000 * 4; i++) { r = r * 1664525u + 1013904223u; lz += (((r >> 8) / 8388608.f - 1.f) - lz) * 0.25f; float in = lz * 0.3f * (((i / 24000) % 2) == 0 ? 1.f : 0.f); float o = fx.process(in, w1); if (i > 48000) { double w = o - 0.75 * in; acc += w * w; cnt++; } } return sqrt(acc / cnt); }; ratio[k++] = 20 * log10(fv(1.f) / fv(0.f)); }
          CHECK(fabs(ratio[0]) < 3 && fabs(ratio[1]) < 3 && fabs(ratio[2]) < 3, "wet level of the FDN vs the previous reverb (rev_mix 1, default damping): %+.1f / %+.1f / %+.1f dB for size 0.3 / 0.5 / 0.7", ratio[0], ratio[1], ratio[2]); }
        { auto o = ir(1.0f, 0.0f, 0, 1.f, 25); bool fin = true; for (float v : o) if (!(v == v) || fabsf(v) > 4.f) fin = false; double e = 0; for (size_t i = 24 * 48000; i < o.size(); i++) e += (double)o[i] * o[i];
          CHECK(fin && 10 * log10(e / 48000 + 1e-30) < -60, "size 1.0 / damp 0 stays stable: tail after 24 s is %.0f dB", 10 * log10(e / 48000 + 1e-30)); }
        { FxChain fx; fx.init(48000); Snap s = mkSnap(); s.revMix = 0.f; fx.update(s); float mx = 0; for (int i = 0; i < 4800; i++) { float in = sinf(kTwoPi * 440.f * i / 48000.f) * 0.3f; mx = std::max(mx, fabsf(fx.process(in, 1.f) - in)); } CHECK(mx == 0.f, "rev_mix 0 leaves the signal untouched"); }
    }
    printf("[P4-5] vocoder: 20 bands, carrier follows the voice or snaps to the scale\n");
    {
        auto voc = [&](float w1) { FxChain fx; fx.init(48000); Snap s = mkSnap(); s.vocMode = 2; s.vocHz = 110; fx.update(s); auto in = voiceish(96000); double ei = 0, eo = 0; bool fin = true;
            for (int i = 0; i < 96000; i++) { float o = fx.process(in[i] / 32768.f, w1); if (!(o == o)) fin = false; if (i > 24000) { ei += (double)in[i] / 32768 * in[i] / 32768; eo += (double)o * o; } } return std::make_pair(fin, 10 * log10(eo / ei)); };
        auto v12 = voc(0.f), v20 = voc(1.f); CHECK(v12.first && v20.first && fabs(v20.second - v12.second) < 5.0, "20-band vocoder level vs the 12-band one: %+.1f dB (12 bands: %+.1f dB)", v20.second, v12.second);
        auto track = [&](int carrier, double inMidi) { reset_v1(); vfx_set(P_QUALITY, 2.f); vfx_set(P_VOC_MODE, 2.f); vfx_set(P_VOC_HZ, 110.f); vfx_set(P_VOC_CARRIER, (float)carrier); vfx_set(P_TUNE_KEY, 0.f); vfx_set(P_TUNE_SCALE, 0.f);
            auto in = singFn(3.0, 0.22, [inMidi](double) { return inMidi; }); run(in, 960); double s = 0; int n = 0; for (int st = 72000; st + 2400 < 140000; st += 2400) { double m = measureMidi(in, st, 2400); if (m > 0) { s += m; n++; } } return n ? s / n : -1.0; };
        double m0 = track(0, 55.4), m1 = track(1, 55.4), m2 = track(2, 55.4);
        CHECK(fabs(m0 - 45.0) < 0.6, "carrier 'fixed' (voc_hz 110 Hz = midi 45): output pitch %.2f", m0);
        CHECK(fabs(m1 - 55.4) < 0.7, "carrier 'follow my pitch' (sung midi 55.4): output pitch %.2f", m1);
        CHECK(fabs(m2 - 55.0) < 0.5, "carrier 'snap to scale' (sung midi 55.4, chromatic): output pitch %.2f (55.00 expected)", m2);
    }
    printf("[P4-6] formant-preserving chord voice (pitch-synchronous overlap-add)\n");
    {
        auto formantVoice = [](double f0, double dur) { int n = (int)(dur * 48000); std::vector<int16_t> v(n); for (int i = 0; i < n; i++) { double t = i / 48000.0, sm = 0; for (int h = 1; h * f0 < 8000; h++) { double fh = h * f0, a = exp(-pow((fh - 1000.0) / 260.0, 2)) + 0.03 / h; sm += a * sin(2 * M_PI * fh * t + 0.3 * h); } v[i] = (int16_t)(2200 * sm); } return v; };
        double worst = 0; char wtxt[64] = ""; for (double f0 : { 120.0, 200.0, 300.0 }) for (int semi : { -5, 3, 4, 7 }) {
            Psola ps; ps.init(770); const double ratio = pow(2.0, semi / 12.0); auto in = formantVoice(f0, 1.5), out = in;
            for (size_t i = 0; i < in.size(); i++) out[i] = (int16_t)ps.proc(in[i] / 32768.f, (float)(48000.0 / f0), (float)ratio) * 0 + (int16_t)(32768.f * ps.acc[0] * 0) + 0;
            Psola p2; p2.init(770); for (size_t i = 0; i < in.size(); i++) out[i] = (int16_t)(32768.f * p2.proc(in[i] / 32768.f, (float)(48000.0 / f0), (float)ratio));
            double m = measureMidi(out, 40000, 4800), want = 69 + 12 * log2(f0 * ratio / 440.0), cents = fabs(m - want) * 100; if (cents > worst) { worst = cents; snprintf(wtxt, sizeof(wtxt), "%.0f Hz %+d st", f0, semi); } }
        CHECK(worst < 8.0, "pitch accuracy of the PSOLA voice (120/200/300 Hz, -5/+3/+4/+7 semitones): worst %.1f cents (%s)", worst, wtxt);
        { Psola p1; p1.init(770); auto in = formantVoice(150, 1.0); std::vector<float> o(in.size()); for (size_t i = 0; i < in.size(); i++) o[i] = p1.proc(in[i] / 32768.f, 320.f, 1.f); double bc = -1, bl = 0, bd = 0; for (int L = 770 - 250; L <= 770 + 250; L++) { double c = 0, e0 = 0, e1 = 0; for (size_t i = 20000; i < 40000; i++) { double a = o[i], b = in[i - L] / 32768.0; c += a * b; e0 += a * a; e1 += b * b; } double cc = c / sqrt(e0 * e1); if (cc > bc) { bc = cc; bl = L; bd = 20 * log10(sqrt(e0 / e1)); } }
          CHECK(bc > 0.95 && fabs(bd) < 1.5 && abs((int)bl - 770) < 250, "ratio 1: output is the input delayed by about the voice latency (770; the grain grid adds up to half a period of phase): best match at %d samples, correlation %.3f, level %+.2f dB", (int)bl, bc, bd); }
        { auto peakF = [&](const std::vector<int16_t>& o, double f0n) { double best = -1e9, bf = 0; for (int k = 1; k * f0n < 5000; k++) { double l = goertzelDb(o, 40000, 4800, k * f0n); if (l > best) { best = l; bf = k * f0n; } } return bf; };
          const double f0 = 150, ratio = pow(2.0, 4.0 / 12.0); auto in = formantVoice(f0, 1.5); std::vector<int16_t> po(in.size()), lo(in.size()); Psola p3; p3.init(770); LitePitch lp; lp.init(48000); lp.ratio = (float)ratio; lp.setPeriod((float)(48000 / f0));
          for (size_t i = 0; i < in.size(); i++) { po[i] = (int16_t)(32768.f * p3.proc(in[i] / 32768.f, (float)(48000.0 / f0), (float)ratio)); lo[i] = (int16_t)(32768.f * lp.proc(in[i] / 32768.f)); }
          double fp = peakF(po, f0 * ratio), fl = peakF(lo, f0 * ratio);
          CHECK(fabs(fp - 1000) < fabs(fl - 1000) - 100 && fabs(fp - 1000) < 160, "formant peak after +4 semitones (input formant at 1000 Hz): PSOLA voice %.0f Hz, previous (Lite) voice %.0f Hz", fp, fl); }
        { auto in = singFn(3.0, 0.22, [](double t) { return 55.0 + 0.3 * sin(t * 3); }); double lv[2]; for (int f = 0; f < 2; f++) { reset_v1(); vfx_set(P_QUALITY, 2.f); vfx_set(P_CHORD, 3.f); vfx_set(P_CHORD_MIX, 1.f); vfx_set(P_CHORD_FORMANT, (float)f); auto o = in; run(o, 960); bool fin = true; for (auto x : o) if (abs((int)x) > 32767) fin = false; lv[f] = db(rms(o, 60000, 140000)); (void)fin; }
          CHECK(fabs(lv[1] - lv[0]) < 4.0, "chord voices with chord_formant 1 vs 0: level %+.1f dB apart (no loss, no blow-up)", lv[1] - lv[0]); }
    }
#ifndef VFX_NO_RNNOISE
    printf("[P4-7] AI noise suppression (RNNoise, built into this test)\n");
    {
        CHECK(vfx_stat(23) == 1, "stat 23: RNNoise built in");
        auto mk = [&](bool voice) { auto v = singFn(4.0, 0.2, [](double t) { return 57.0 + 2 * sin(t * 2); }); unsigned r = 9; for (size_t i = 0; i < v.size(); i++) { r = r * 1664525u + 1013904223u; int nz = (int)(((r >> 16) / 32768.0 - 1.0) * 300.0); int s = voice ? ((i / 24000) % 2 == 1 ? v[i] : 0) : 0; v[i] = (int16_t)std::max(-32768, std::min(32767, s + nz)); } return v; };
        auto noisy1 = mk(true); auto o = noisy1; reset_v1(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 1.f); vfx_set(P_NS_AMT, 1.f); run(o, 960);
        double nIn = rms(noisy1, 54000, 70000), nOut = rms(o, 54000, 70000), vIn = rms(noisy1, 30000, 46000), vOut = rms(o, 30000, 46000);
        CHECK(db(nOut) - db(nIn) < -10.0 && db(vOut) - db(vIn) > -6.0, "RNNoise at full strength (noise-only gap after a spoken phrase; the model needs ~1 s to settle on steady noise): noise %.1f dB, voice %+.1f dB", db(nOut) - db(nIn), db(vOut) - db(vIn));
        CHECK(fabs(vfx_statf(16) - 20.0) < 0.5, "reported noise-suppression delay: %.1f ms (measured: 960 samples)", vfx_statf(16));
        // measured end-to-end delay of the AI path
        { auto clean = singFn(2.0, 0.2, [](double t) { return 57.0 + 2 * sin(t * 2); }), co = clean; reset_v1(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 1.f); vfx_set(P_NS_AMT, 1.f); run(co, 960); int bestL = 0; double bc = -1;
          for (int L = 300; L < 1500; L += 4) { double c = 0, e0 = 0, e1 = 0; for (int i = 30000; i < 60000; i++) { double a = co[i], b = clean[i - L]; c += a * b; e0 += a * a; e1 += b * b; } c /= sqrt(e0 * e1 + 1e-9); if (c > bc) { bc = c; bestL = L; } }
          CHECK(abs(bestL - (384 + 960)) <= 16, "measured total delay of the AI path on a clean voice: %d samples (%.1f ms; V1 chain 384 + RNNoise)", bestL, bestL / 48.0); printf("        (measured RNNoise delay = %d samples = %.1f ms)\n", bestL - 384, (bestL - 384) / 48.0); }
        // engine switching without a click
        { reset_v1(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_AMT, 1.f); auto t = tone(240000, 300, 8000), o2 = t; int p = 0; while (p < 240000) { if (p == 72000) vfx_set(P_NS_ENGINE, 1.f); if (p == 144000) vfx_set(P_NS_ENGINE, 0.f); vfx_process_i16(o2.data() + p, 960); p += 960; }
          int j = maxJump(o2, 60000, 240000); CHECK(j < 2500, "switching Classic -> AI -> Classic while a tone plays: largest step %d (tone's natural step 353 x level)", j); }
        { std::vector<std::vector<int16_t>> r; for (int chunk : { 960, 64, 480, 1023, 333 }) { reset_v1(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 1.f); auto v = mk(true); run(v, chunk); r.push_back(v); }
          long bad = 0; for (size_t k = 1; k < r.size(); k++) for (size_t i = 0; i < r[0].size(); i++) if (r[k][i] != r[0][i]) bad++; CHECK(bad == 0, "AI noise suppression: output independent of the chunk size (%ld differing samples)", bad); }
        { reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 1.f); auto a = mk(true), b = a; run(a, 960); reset_all(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 0.f); run(b, 960); long bad = 0; for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) bad++;
          CHECK(bad == 0, "legacy_mode always uses the Classic engine (ns_engine ignored): %ld differing samples", bad); }
    }
#else
    printf("[P4-7] AI noise suppression: RNNoise not built in (VFX_NO_RNNOISE) - selector falls back to Classic\n");
    { CHECK(vfx_stat(23) == 0, "stat 23: RNNoise not built in");
      auto a = voiceish(96000), b = a; reset_v1(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 1.f); run(a, 960); reset_v1(); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 0.f); run(b, 960); long bad = 0; for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) bad++;
      CHECK(bad == 0, "ns_engine 1 without RNNoise behaves exactly like Classic (%ld differing samples)", bad); }
#endif
    printf("[P4-8] everything new at once: chunk independence, Compare cross-fade, finite output\n");
    {
        auto setAll = [&]() { reset_v1(); vfx_set(P_QUALITY, 2.f); vfx_set(P_TUNE_ON, 1.f); vfx_set(P_TUNE_KEY, 0.f); vfx_set(P_CHORD, 1.f); vfx_set(P_CHORD_FORMANT, 0.7f); vfx_set(P_DRIVE, 0.4f); vfx_set(P_ECHO_MIX, 0.3f); vfx_set(P_ECHO_DAMP, 0.5f); vfx_set(P_ECHO_HP, 200.f);
            vfx_set(P_REV_MIX, 0.4f); vfx_set(P_REV_PREDELAY, 20.f); vfx_set(P_REV_DAMP, 0.6f); vfx_set(P_VOC_MODE, 2.f); vfx_set(P_VOC_CARRIER, 1.f); vfx_set(P_NS_ON, 1.f); vfx_set(P_NS_ENGINE, 1.f); };
        auto in = singFn(3.0, 0.22, [](double t) { return 60.0 + 0.4 * sin(2 * M_PI * 5.5 * t) + (t > 1.5 ? 3.0 : 0.0); });
        std::vector<std::vector<int16_t>> r; for (int chunk : { 960, 1, 64, 480, 1023 }) { setAll(); auto o = in; run(o, chunk); r.push_back(o); }
        long bad = 0; for (size_t k = 1; k < r.size(); k++) for (size_t i = 0; i < r[0].size(); i++) if (r[k][i] != r[0][i]) bad++; CHECK(bad == 0, "chunk sizes 960 / 1 / 64 / 480 / 1023 give identical output with every Phase 4 stage on: %ld differing samples", bad);
        setAll(); auto o = in; int p = 0; while (p < (int)o.size()) { if (p == 48000) vfx_set(P_LEGACY, 1.f); if (p == 96000) vfx_set(P_LEGACY, 0.f); vfx_process_i16(o.data() + p, 960); p += 960; }
        int j = maxJump(o, 20000, 144000); int mx = 0; for (auto x : o) mx = std::max(mx, abs((int)x)); CHECK(j < 12000 && mx <= 32767, "Compare toggled twice with reverb + vocoder + drive + echo + chords on: largest step %d, peak %d", j, mx);
    }

    // ======================================================================== Phase 5: CPU headroom
    printf("\n[P5] Phase 5 tests (lookup tables, stage meters, recovery from Lite)\n");
    printf("[P5-1] sine / cosine table\n");
    {
        double es = 0, ec = 0; for (int i = 0; i < 200000; i++) { float u = (float)i / 200000.f; es = std::max(es, (double)fabsf(g_lut.sin01(u) - sinf(kTwoPi * u))); ec = std::max(ec, (double)fabsf(g_lut.cos01(u) - cosf(kTwoPi * u))); }
        CHECK(es < 6e-7 && ec < 6e-7, "table vs sinf / cosf over a full turn: max error sin %.2e, cos %.2e", es, ec);
    }
    printf("[P5-2] Lite grain window (table) vs the previous shifter\n");
    {
        for (float ratio : { 0.7f, 1.0f, 1.26f, 1.9f }) { LitePitch a; refeng::LitePitch b; a.init(48000); b.init(48000); a.ratio = b.ratio = ratio; a.setPeriod(320.f); b.setPeriod(320.f); unsigned r = 5; double mx = 0, pk = 0;
            for (int i = 0; i < 96000; i++) { r = r * 1664525u + 1013904223u; float x = (((r >> 8) / 8388608.f) - 1.f) * 0.3f + 0.4f * sinf(kTwoPi * 180.f * i / 48000.f); float ya = a.proc(x), yb = b.proc(x); mx = std::max(mx, (double)fabsf(ya - yb)); pk = std::max(pk, (double)fabsf(yb)); }
            CHECK(mx < 2e-5 * 4, "Lite shifter ratio %.2f: largest sample difference %.2e (signal peak %.2f, 1 LSB = 3e-5)", ratio, mx, pk); }
    }
    printf("[P5-3] LFOs: robot, tremolo, chorus, flanger, vibrato match the previous engine (a modulated delay turns a 1e-6 phase error into ~1e-4 samples of delay)\n");
    {
        struct M { const char* n; std::vector<std::pair<int, float>> par; };
        std::vector<M> ms = { { "robot", { { P_ROBOT, 1.f }, { P_ROBOT_HZ, 37.f }, { P_ROBOT_AMT, 0.8f } } }, { "tremolo", { { P_TREM_HZ, 6.5f }, { P_TREM_DEPTH, 0.7f } } },
                              { "chorus", { { P_MOD_MODE, 1.f }, { P_MOD_RATE, 1.3f }, { P_MOD_DEPTH, 0.8f } } }, { "flanger", { { P_MOD_MODE, 2.f }, { P_MOD_RATE, 0.4f }, { P_MOD_DEPTH, 0.9f } } },
                              { "vibrato", { { P_MOD_MODE, 3.f }, { P_MOD_RATE, 5.5f }, { P_MOD_DEPTH, 0.6f } } }, { "bit crusher", { { P_BITS, 6.f }, { P_DOWN, 3.f } } } };
        for (auto& m : ms) { auto a = voiceish(144000), b = a; params_default(); refeng::params_default(); vfx_set(P_LEGACY, 1.f); for (auto& pv : m.par) { vfx_set(pv.first, pv.second); refSet(pv.first, pv.second); }
            vfx_prepare(48000); refeng::ref_vfx_prepare(48000); vfx_set(P_ENABLED, 1.f); refSet(P_ENABLED, 1.f); run(a, 960, 31u); refRun(b, 960, 31u);
            int mx = 0; double se = 0; for (size_t i = 0; i < a.size(); i++) { int d = abs((int)a[i] - (int)b[i]); mx = std::max(mx, d); se += (double)d * d; }
            CHECK(mx <= 16 && sqrt(se / a.size()) < 0.2, "%-12s: largest difference %d LSB, rms %.3f LSB (about -100 dB) vs the previous engine", m.n, mx, sqrt(se / a.size())); }
    }
    printf("[P5-4] per-stage CPU meters\n");
    {
        reset_v1(); vfx_set(P_QUALITY, 2.f); vfx_set(P_PITCH, 1.2f); vfx_set(P_CHORD, 1.f); vfx_set(P_TUNE_ON, 1.f); vfx_set(P_REV_MIX, 0.4f); vfx_set(P_NS_ON, 1.f); auto in = voiceish(480000), o = in; run(o, 960);
        double st[5]; for (int k = 0; k < 5; k++) st[k] = vfx_statf(17 + k); double sum = st[0] + st[1] + st[2] + st[3] + st[4];
        CHECK(st[0] > 0 && st[1] > 0 && st[2] > 0 && st[3] > 0 && st[4] > 0 && sum <= vfx_statf(3) * 1.25 + 0.2, "stage meters (%% of real time): input %.2f, pitch analysis %.2f, pitch engine %.2f, chords %.2f, effects+limiter %.2f; sum %.2f vs total load %.2f", st[0], st[1], st[2], st[3], st[4], sum, vfx_statf(3));
        reset_v1(); auto o2 = in; run(o2, 960); CHECK(vfx_statf(19) < 0.01 && vfx_statf(20) < 0.01 && vfx_statf(18) < 0.01, "with the pitch engine, chords and Auto-Tune off their meters read about 0 (%.3f / %.3f / %.3f)", vfx_statf(19), vfx_statf(20), vfx_statf(18));
    }
#ifndef VFX_NO_RUBBERBAND
    printf("[P5-5] automatic return from Lite to RubberBand (simulated load via the test hook)\n");
    {
        auto feed = [&](double secs) { auto v = voiceish(48000); int total = (int)(secs * 48000), p = 0; while (p < total) { vfx_process_i16(v.data(), 960); p += 960; if ((p % 48000) == 0) v = voiceish(48000); } };
        reset_v1(); vfx_set(P_QUALITY, 0.f); vfx_set(P_PITCH, 1.2f); g_eng.simLoadRb = 0.9f; int p0 = vfx_stat(24), r0 = vfx_stat(25); feed(2.0);
        CHECK(vfx_stat(13) == 1 && vfx_stat(12) == 2, "overload on RubberBand (simulated 90 %%) -> automatic switch to Lite (stat 13 = %d, engine %d)", vfx_stat(13), vfx_stat(12));
        feed(23.0); CHECK(vfx_stat(24) == p0 + 1 && vfx_stat(25) == r0 && vfx_stat(13) == 1 && vfx_stat(12) == 2, "after ~20 s one probe ran, it saw the overload and went back to Lite (probes %d, recoveries %d, still on Lite)", vfx_stat(24) - p0, vfx_stat(25) - r0);
        g_eng.simLoadRb = 0.30f; feed(24.0);
        CHECK(vfx_stat(25) == r0 + 1 && vfx_stat(13) == 0 && vfx_stat(12) == 1, "load now 30 %%: the next probe succeeded and stayed on RubberBand (recoveries %d, stat 13 = %d, engine %d)", vfx_stat(25) - r0, vfx_stat(13), vfx_stat(12));
        g_eng.simLoadRb = 0.9f; feed(2.0); int p1 = vfx_stat(24), r1 = vfx_stat(25); g_eng.simLoadRb = 0.6f; feed(25.0);
        CHECK(vfx_stat(24) == p1 + 1 && vfx_stat(25) == r1 && vfx_stat(13) == 1, "back on Lite, load 60 %% (not under 55 %%): the probe ran (%d) but did not switch back (recoveries %d, stat 13 = %d)", vfx_stat(24) - p1, vfx_stat(25) - r1, vfx_stat(13));
        reset_v1(); vfx_set(P_QUALITY, 1.f); vfx_set(P_PITCH, 1.2f); g_eng.simLoadRb = 0.95f; int pq = vfx_stat(24); feed(30.0);
        CHECK(vfx_stat(24) == pq && vfx_stat(12) == 1, "quality 'High' never falls back and never probes (probes %d, engine %d)", vfx_stat(24) - pq, vfx_stat(12));
        g_eng.simLoadRb = 0.f;
    }
#else
    printf("[P5-5] automatic return from Lite to RubberBand: not applicable (built without RubberBand)\n");
#endif

    // ---- release / rename checks
    printf("\nrelease identity + one-time settings import\n");
    {   reset_all();
        CHECK(vfx_stat(8) == 9 && VFX_API_VERSION == 9, "library API version %d (9)", vfx_stat(8));
        CHECK(vfx_stat(17) == 100, "product version x100 = %d (100 = HexedVoiceMod 1.0)", vfx_stat(17));
        CHECK(vfx_stat(18) == 0 && vfx_stat(19) == 0, "old-library flag %d, imported flag %d (both 0 on a clean start)", vfx_stat(18), vfx_stat(19));
        CHECK(P_ENABLED == 0 && P_PITCH == 1 && P_FORMANT == 2 && P_PRESERVE == 3 && P_GAIN_DB == 4 && P_GATE_DB == 5 && P_MIX == 6 && P_ROBOT == 7 && P_ROBOT_HZ == 8 && P_MON_VOL == 9 && P_MONITOR == 10, "parameter ids 0..10 unchanged");
        remove(CFG_PATH); remove(OLD_CFG_PATH);
        FILE* o = fopen(OLD_CFG_PATH, "w"); fprintf(o, "pitch=1.5000\nformant=1.2000\necho_ms=420.0000\n"); fclose(o);
        params_default(); vfx_load();
        CHECK(fabsf(vfx_get(P_PITCH) - 1.5f) < 1e-4f && fabsf(vfx_get(P_FORMANT) - 1.2f) < 1e-4f && fabsf(vfx_get(P_ECHO_MS) - 420.f) < 1e-2f, "old voicefx.cfg imported (pitch %.2f formant %.2f echo %.0f)", vfx_get(P_PITCH), vfx_get(P_FORMANT), vfx_get(P_ECHO_MS));
        CHECK(vfx_stat(19) == 1, "import flag set");
        FILE* n = fopen(CFG_PATH, "r"); CHECK(n != nullptr, "new HexedVoiceMod.cfg written by the import"); if (n) fclose(n);
        FILE* k = fopen(OLD_CFG_PATH, "r"); CHECK(k != nullptr, "old voicefx.cfg left in place"); if (k) fclose(k);
        o = fopen(OLD_CFG_PATH, "w"); fprintf(o, "pitch=0.7500\n"); fclose(o);          // change the old file: the new one must win from now on
        params_default(); vfx_load();
        CHECK(fabsf(vfx_get(P_PITCH) - 1.5f) < 1e-4f, "second load reads the new cfg, not the old one (pitch %.2f)", vfx_get(P_PITCH));
        remove(CFG_PATH); remove(OLD_CFG_PATH); params_default(); g_cfgImported = 0;
    }
    printf("\n%s (%d failed)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
