// Unit tests for the B.g engine. Build & run:
//   g++ -std=c++17 -O2 -I Source Tests/dsp_test.cpp -o dsp_test && ./dsp_test
#include "Engine.h"
#include <cstdio>
#include <random>

using namespace bg;

static int failures = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) std::printf ("  PASS: ");                           \
        else { std::printf ("  FAIL: "); ++failures; }                \
        std::printf (__VA_ARGS__); std::printf ("\n");                \
    } while (0)

static std::mt19937 rng (1234);
static double randn() { static std::normal_distribution<double> d (0.0, 1.0); return d (rng); }

// crude pink-ish noise (Voss-McCartney style, 8 octaves)
struct Pink
{
    double rows[8] = {}; double sum = 0; int counter = 0;
    double next()
    {
        ++counter;
        for (int k = 0; k < 8; ++k)
            if ((counter & ((1 << (k + 1)) - 1)) == (1 << k)) { sum -= rows[k]; rows[k] = randn(); sum += rows[k]; break; }
        return (sum + randn()) / 3.0;
    }
};

static double rms (const std::vector<float>& v, size_t a, size_t b)
{
    double s = 0; for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) (b - a));
}

// band power (dB) of a mono signal around f (1-octave wide) via the library FFT
static double bandDb (const std::vector<float>& x, size_t start, double fs, double f)
{
    const int N = 8192; Fft fft (13);
    std::vector<double> re (N), im (N, 0.0);
    double acc = 0.0; int frames = 0;
    for (size_t pos = start; pos + N <= x.size(); pos += N)
    {
        for (int i = 0; i < N; ++i) re[(size_t) i] = x[pos + (size_t) i] * (0.5 - 0.5 * std::cos (2 * kPi * i / (N - 1))), im[(size_t) i] = 0;
        fft.forward (re.data(), im.data());
        double s = 0; int c = 0;
        for (int i = 1; i < N / 2; ++i)
        {
            const double fr = i * fs / N;
            if (fr >= f / 1.41421356 && fr < f * 1.41421356) { s += re[(size_t) i] * re[(size_t) i] + im[(size_t) i] * im[(size_t) i]; ++c; }
        }
        acc += s / std::max (1, c); ++frames;
    }
    return 10.0 * std::log10 (acc / std::max (1, frames) + 1e-30);
}

//------------------------------------------------------------------------------
static void testLoudness()
{
    std::printf ("[LUFS meter]\n");
    for (double fs : { 44100.0, 48000.0 })
    {
        LoudnessMeter m; m.prepare (fs);
        const int n = (int) (fs * 12);
        std::vector<float> s ((size_t) n);
        for (int i = 0; i < n; ++i) s[(size_t) i] = (float) (0.1 * std::sin (2 * kPi * 997.0 * i / fs)); // -20 dBFS peak
        m.process (s.data(), s.data(), n);
        CHECK (std::abs (m.integrated() - (-20.0)) < 0.2, "fs=%.0f 997 Hz stereo -20 dBFS -> integrated %.2f LUFS (expect -20.0)", fs, m.integrated());
        CHECK (std::abs (m.shortTerm() - m.momentary()) < 0.1, "short-term %.2f ~ momentary %.2f", m.shortTerm(), m.momentary());
    }
    // gating: loud part then long silence must keep integrated near the loud part
    {
        const double fs = 48000; LoudnessMeter m; m.prepare (fs);
        std::vector<float> s ((size_t) (fs * 10));
        for (size_t i = 0; i < s.size(); ++i) s[i] = (i < (size_t) (fs * 5)) ? (float) (0.1 * std::sin (2 * kPi * 997.0 * (double) i / fs)) : 0.f;
        m.process (s.data(), s.data(), (int) s.size());
        CHECK (std::abs (m.integrated() - (-20.0)) < 0.3, "gated integrated with silence = %.2f (expect -20.0)", m.integrated());
    }
}

static void testCrossover()
{
    std::printf ("[LR4 crossover]\n");
    const double fs = 48000.0;
    LR4 x; x.setFreq (fs, 1000.0);
    double eIn = 0, eSum = 0;
    for (int i = 0; i < 200000; ++i)
    {
        const double v = randn() * 0.1; double lo, hi;
        x.process (v, lo, hi);
        if (i > 1000) { eIn += v * v; eSum += (lo + hi) * (lo + hi); }
    }
    CHECK (std::abs (10 * std::log10 (eSum / eIn)) < 0.1, "lo+hi preserves energy (all-pass): %.3f dB", 10 * std::log10 (eSum / eIn));

    LR4 y; y.setFreq (fs, 1000.0);
    double eLo = 0, eIn2 = 0;
    for (int i = 0; i < 48000; ++i)
    {
        const double v = std::sin (2 * kPi * 1000.0 * i / fs); double lo, hi;
        y.process (v, lo, hi);
        if (i > 4800) { eLo += lo * lo; eIn2 += v * v; }
    }
    CHECK (std::abs (10 * std::log10 (eLo / eIn2) - (-6.02)) < 0.2, "-6 dB at the crossover frequency: %.2f dB", 10 * std::log10 (eLo / eIn2));
}

static void testCompressor()
{
    std::printf ("[Compressor]\n");
    LinkedCompressor c; c.prepare (48000.0);
    c.setParams (-20.0, 4.0, 5.0, 100.0, 0.0);
    double gl = 1;
    for (int i = 0; i < 48000; ++i) gl = c.process (0.5, 0.5);   // -6 dBFS constant
    const double grDb = -linToDb (gl);
    CHECK (std::abs (grDb - 10.5) < 0.2, "steady-state GR = %.2f dB (expect 10.5)", grDb);
    for (int i = 0; i < 96000; ++i) gl = c.process (0.0, 0.0);
    CHECK (-linToDb (gl) < 0.2, "releases to ~0 dB: %.3f", -linToDb (gl));
}

// 8x oversampled true-peak measurement (windowed sinc)
static double truePeak8x (const std::vector<float>& x, size_t from)
{
    double peak = 0;
    const int taps = 24;
    for (size_t i = from; i + taps < x.size(); ++i)
        for (int ph = 1; ph < 8; ++ph)
        {
            const double frac = ph / 8.0; double s = 0;
            for (int k = -taps / 2 + 1; k <= taps / 2; ++k)
            {
                const double t = k - frac;
                const double sinc = std::sin (kPi * t) / (kPi * t);
                const double w = 0.5 + 0.5 * std::cos (kPi * t / (taps / 2 + 1));
                s += x[i + (size_t) (k + taps / 2)] * sinc * w;
            }
            peak = std::max (peak, std::abs (s));
        }
    return peak;
}

static void testLimiter()
{
    std::printf ("[Limiter]\n");
    const double fs = 48000.0;
    const double ceil = dbToLin (-1.0);
    for (int mode = 0; mode < 2; ++mode)
    {
        // mode 0: realistic band-limited (16 kHz) loud material. mode 1: clipped white noise (worst case, info only)
        LookaheadLimiter lim; lim.prepare (fs); lim.setCeilingDb (-1.0); lim.setReleaseMs (80.0);
        const int n = (int) fs * 4;
        std::vector<float> L ((size_t) n), R ((size_t) n);
        Biquad lpL, lpR; lpL.setLowPass (fs, 16000.0, 0.707); lpR = lpL;
        for (int i = 0; i < n; ++i)
        {
            const double burst = (i / 4800) % 3 == 0 ? 3.0 : 0.3;
            double a = std::clamp (burst * (0.6 * std::sin (2 * kPi * 3100.0 * i / fs) + 0.4 * randn()), -8.0, 8.0);
            double b = std::clamp (burst * (0.6 * std::sin (2 * kPi * 7300.0 * i / fs) + 0.4 * randn()), -8.0, 8.0);
            if (mode == 0) { a = lpL.process (a); b = lpR.process (b); }
            L[(size_t) i] = (float) a; R[(size_t) i] = (float) b;
        }
        for (int i = 0; i < n; i += 480) lim.process (L.data() + i, R.data() + i, 480);
        float pk = 0; for (int i = 0; i < n; ++i) pk = std::max (pk, std::max (std::abs (L[(size_t) i]), std::abs (R[(size_t) i])));
        const double tp = std::max (truePeak8x (L, 2000), truePeak8x (R, 2000));
        if (mode == 0)
        {
            CHECK (pk <= ceil * 1.0005, "sample peak %.4f <= ceiling %.4f", pk, ceil);
            CHECK (linToDb (tp) <= -1.0 + 0.05, "true peak of band-limited loud material: %.2f dB (ceiling -1.0, tolerance 0.05)", linToDb (tp));
        }
        else
        {
            std::printf ("  INFO: worst case (clipped white noise up to Nyquist) true peak %.2f dB\n", linToDb (tp));
        }
    }

    // transparency + latency for a quiet signal
    LookaheadLimiter l2; l2.prepare (fs); l2.setCeilingDb (-1.0);
    std::vector<float> a ((size_t) 48000), b ((size_t) 48000);
    for (size_t i = 0; i < a.size(); ++i) a[i] = b[i] = (float) (0.2 * std::sin (2 * kPi * 440.0 * (double) i / fs));
    std::vector<float> ref = a;
    l2.process (a.data(), b.data(), (int) a.size());
    const int lat = l2.latency();
    double err = 0; for (size_t i = 2000; i + (size_t) lat < a.size(); ++i) err = std::max (err, (double) std::abs (a[i + (size_t) lat] - ref[i]));
    CHECK (err < 1e-6, "quiet signal passes untouched, delayed by %d samples (max err %.2e)", lat, err);
}

//------------------------------------------------------------------------------
static void runEngine (Engine& e, std::vector<float>& L, std::vector<float>& R, const EngineParams& p, int block = 512)
{
    const int n = (int) L.size();
    for (int i = 0; i < n; i += block)
    {
        const int len = std::min (block, n - i);
        e.process (L.data() + i, R.data() + i, len, p);
    }
}

static void makePink (std::vector<float>& L, std::vector<float>& R, int n, double rmsDb)
{
    Pink pl, pr; L.resize ((size_t) n); R.resize ((size_t) n);
    for (int i = 0; i < n; ++i) { L[(size_t) i] = (float) pl.next(); R[(size_t) i] = (float) pr.next(); }
    const double g = dbToLin (rmsDb) / rms (L, 0, (size_t) n);
    for (int i = 0; i < n; ++i) { L[(size_t) i] *= (float) g; R[(size_t) i] *= (float) g; }
}

static void testEngineLoudness()
{
    std::printf ("[Engine: loudness loop]\n");
    const double fs = 48000.0;
    for (bool master : { true, false })
    {
        Engine e (master); e.prepare (fs);
        EngineParams p; p.genre = kNumGenres - 1; // Neutral: -14 LUFS (master) / -22 (mix)
        std::vector<float> L, R; makePink (L, R, (int) (fs * 60), -28.0);
        runEngine (e, L, R, p);
        const double target = -14.0 - (master ? 0.0 : 8.0);
        const double got = e.meters().shortTerm.load();
        bool finite = true; for (float v : L) if (! std::isfinite (v)) finite = false;
        CHECK (finite, "%s: output is finite", master ? "master" : "mix");
        CHECK (std::abs (got - target) < 1.0, "%s: short-term %.2f LUFS vs target %.1f", master ? "master" : "mix", got, target);
        float pk = 0; for (size_t i = (size_t) fs * 10; i < L.size(); ++i) pk = std::max (pk, std::max (std::abs (L[i]), std::abs (R[i])));
        CHECK (pk <= dbToLin (-1.0) * 1.001, "%s: peak %.3f <= ceiling", master ? "master" : "mix", pk);
    }
    // loud genre on dynamic material: must stay under the ceiling whatever happens
    {
        Engine e (true); e.prepare (fs);
        EngineParams p; p.genre = 6; // Trap: -8 LUFS
        std::vector<float> L, R; makePink (L, R, (int) (fs * 40), -30.0);
        runEngine (e, L, R, p, 333);
        float pk = 0; for (size_t i = 0; i < L.size(); ++i) pk = std::max (pk, std::max (std::abs (L[i]), std::abs (R[i])));
        CHECK (pk <= dbToLin (-1.0) * 1.001, "trap/master odd block size: peak %.3f within ceiling, short-term %.2f LUFS", pk, e.meters().shortTerm.load());
    }
}

static void testEngineBypassAndManual()
{
    std::printf ("[Engine: bypass / manual neutrality]\n");
    const double fs = 48000.0;
    {
        Engine e (true); e.prepare (fs);
        EngineParams p; p.bypass = true; p.bypassMatch = false;
        std::vector<float> L, R; makePink (L, R, (int) fs * 3, -24.0);
        std::vector<float> refL = L;
        runEngine (e, L, R, p);
        const int lat = e.latency();
        double err = 0; for (size_t i = 4800; i + (size_t) lat < L.size(); ++i) err = std::max (err, (double) std::abs (L[i + (size_t) lat] - refL[i]));
        CHECK (err < 1e-5, "bypass = exact input delayed by %d samples (err %.2e)", lat, err);
    }
    {
        Engine e (true); e.prepare (fs);
        EngineParams p; p.manual = true; p.bThr[0] = p.bThr[1] = p.bThr[2] = 0.0;
        p.bRatio[0] = p.bRatio[1] = p.bRatio[2] = 1.0; p.ceilingDb = 0.0;
        std::vector<float> L, R; makePink (L, R, (int) fs * 6, -30.0);
        Biquad pre; pre.setHighPass (fs, 100.0, 0.707);   // keep the test signal above the 25 Hz low-cut
        Biquad pre2 = pre;
        for (size_t i = 0; i < L.size(); ++i) { L[i] = (float) pre.process (L[i]); R[i] = (float) pre2.process (R[i]); }
        std::vector<float> refL = L;
        runEngine (e, L, R, p);
        const double rin = rms (refL, 48000, 240000), rout = rms (L, 48000 + (size_t) e.latency(), 240000 + (size_t) e.latency());
        CHECK (std::abs (20 * std::log10 (rout / rin)) < 0.25, "manual with everything neutral: level change %.3f dB", 20 * std::log10 (rout / rin));
    }
}

static void testEngineEqAndLearn()
{
    std::printf ("[Engine: auto EQ / Learn]\n");
    const double fs = 48000.0;
    // pink noise with an extra +9 dB bump around 4 kHz
    auto makeBumpy = [&] (std::vector<float>& L, std::vector<float>& R, int n)
    {
        makePink (L, R, n, -24.0);
        Biquad bq; bq.setPeak (fs, 4000.0, 0.7, 9.0);
        for (int i = 0; i < n; ++i) { L[(size_t) i] = (float) bq.process (L[(size_t) i]); }
        Biquad bq2; bq2.setPeak (fs, 4000.0, 0.7, 9.0);
        for (int i = 0; i < n; ++i) { R[(size_t) i] = (float) bq2.process (R[(size_t) i]); }
    };
    {
        Engine e (false); e.prepare (fs);
        EngineParams p; p.genre = kNumGenres - 1; p.amount = 1.0; p.ceilingDb = 0.0;
        std::vector<float> L, R; makeBumpy (L, R, (int) fs * 30);
        std::vector<float> inL = L;
        runEngine (e, L, R, p);
        const double eq4k = e.appliedEqDb (7); // band 7 = 4 kHz
        CHECK (eq4k < -1.0, "auto EQ cuts the 4 kHz bump: applied %.2f dB", eq4k);
        const size_t from = (size_t) fs * 20;
        std::vector<float> a (inL.begin() + (long) from, inL.end()), b (L.begin() + (long) from + e.latency(), L.end());
        const double relIn = bandDb (a, 0, fs, 4000.0) - bandDb (a, 0, fs, 1000.0);
        const double relOut = bandDb (b, 0, fs, 4000.0) - bandDb (b, 0, fs, 1000.0);
        CHECK (relOut < relIn - 0.5, "4k-vs-1k ratio drops: in %.2f dB -> out %.2f dB", relIn, relOut);
    }
    {
        // Learn: collect on bumpy material, freeze, then feed plain noise: EQ must stay where it was learned
        Engine e (false); e.prepare (fs);
        EngineParams p; p.genre = kNumGenres - 1; p.amount = 1.0; p.ceilingDb = 0.0;
        std::vector<float> L, R; makeBumpy (L, R, (int) fs * 12);
        p.learn = true; p.analysisFrozen = true;
        runEngine (e, L, R, p);
        p.learn = false;
        std::vector<float> L2, R2; makeBumpy (L2, R2, (int) fs * 10);
        runEngine (e, L2, R2, p);
        const double learned = e.appliedEqDb (7);
        CHECK (e.hasLearnedSnapshot() && learned < -1.0, "learned snapshot applied after Learn: %.2f dB at 4 kHz", learned);
        std::vector<float> L3, R3; makePink (L3, R3, (int) fs * 10, -24.0);
        runEngine (e, L3, R3, p);
        CHECK (std::abs (e.appliedEqDb (7) - learned) < 0.05, "EQ stays frozen on different material: %.2f -> %.2f dB", learned, e.appliedEqDb (7));
    }
}

static void testMonoBass()
{
    std::printf ("[Engine: mono bass]\n");
    const double fs = 48000.0;
    Engine e (true); e.prepare (fs);
    EngineParams p; p.manual = true; p.monoBass = true; p.monoFreq = 120.0; p.ceilingDb = 0.0;
    p.bThr[0] = p.bThr[1] = p.bThr[2] = 0.0; p.bRatio[0] = p.bRatio[1] = p.bRatio[2] = 1.0;
    const int n = (int) fs * 3;
    std::vector<float> L ((size_t) n), R ((size_t) n);
    for (int i = 0; i < n; ++i) { const float v = (float) (0.2 * std::sin (2 * kPi * 50.0 * i / fs)); L[(size_t) i] = v; R[(size_t) i] = -v; }
    const double before = rms (L, 48000, 100000);
    runEngine (e, L, R, p);
    const double after = rms (L, 48000, 100000);
    CHECK (after < before * 0.1, "out-of-phase 50 Hz is collapsed to mono: %.4f -> %.4f", before, after);
}

int main()
{
    testLoudness();
    testCrossover();
    testCompressor();
    testLimiter();
    testEngineLoudness();
    testEngineBypassAndManual();
    testEngineEqAndLearn();
    testMonoBass();
    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
