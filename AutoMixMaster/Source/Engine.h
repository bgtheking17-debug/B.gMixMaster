#pragma once
// B.g processing engine: pure C++17 (no JUCE). The JUCE plugin is a thin wrapper around this class.
#include "Dsp.h"
#include "Genres.h"
#include <array>
#include <atomic>
#include <vector>

namespace bg
{
struct EngineParams
{
    int    genre = 0;
    bool   manual = false;          // false = Auto, true = Manual
    bool   analysisFrozen = false;  // false = Live analysis, true = use the "Learn" snapshot
    bool   learn = false;           // while true, the engine collects the learn snapshot
    double amount = 0.7;            // 0..1
    double loudOffsetDb = 0.0;      // offset to the genre loudness target (auto)
    double ceilingDb = -1.0;
    double limReleaseMs = 100.0;
    double widthScale = 1.0;        // global width multiplier (1 = genre / manual width)
    bool   monoBass = false;
    double monoFreq = 120.0;
    int    dither = 0;              // 0 off, 1 = 16 bit, 2 = 24 bit
    bool   bypass = false;
    bool   bypassMatch = true;      // loudness-matched bypass

    // manual section
    double lowcut = 25.0;
    double eqQ = 1.0;
    double eqGain[9] = {};
    double xover1 = 150.0, xover2 = 3000.0;
    double bThr[3]   = { -18.0, -18.0, -18.0 };
    double bRatio[3] = { 2.0, 2.0, 2.0 };
    double bAtk[3]   = { 30.0, 20.0, 10.0 };
    double bRel[3]   = { 200.0, 150.0, 100.0 };
    double bMake[3]  = { 0.0, 0.0, 0.0 };
    double bWidth[3] = { 100.0, 100.0, 100.0 };  // percent
    double sat = 0.0;                            // 0..1
    double outGainDb = 0.0;
};

struct Meters
{
    std::atomic<float> momentary { -120.f }, shortTerm { -120.f }, integrated { -120.f };
    std::atomic<float> peakDb { -120.f }, grDb { 0.f }, correlation { 1.f };
};

// What "Auto" currently computes (used by "copy auto to manual")
struct AutoValues
{
    double eq[9] = {};
    double thr[3] = {}, ratio[3] = {}, atk[3] = {}, rel[3] = {}, width[3] = {};
    double sat = 0.0, gainDb = 0.0;
};

class Engine
{
public:
    static constexpr int kBands = 10;
    static constexpr int kChunk = 512;
    static constexpr int kFftOrder = 12;
    static constexpr int kFft = 1 << kFftOrder;

    explicit Engine (bool master) : isMaster (master), fft (kFftOrder) {}

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        inMeter.prepare (sr);
        outMeter.prepare (sr);
        limiter.prepare (sr);
        ringLen = limiter.latency() + 1;
        for (auto& r : dryRing) r.assign ((size_t) ringLen, 0.f);
        dryPos = 0;

        for (int c = 0; c < 2; ++c)
        {
            hp[c].setIdentity(); hp[c].reset();
            for (auto& b : eq[c]) { b.setIdentity(); b.reset(); }
            lr1[c].reset(); lr2[c].reset(); ap2[c].reset(); mb[c].reset();
        }
        for (auto& c : comp) c.prepare (sr);

        window.resize ((size_t) kFft);
        for (int i = 0; i < kFft; ++i) window[(size_t) i] = 0.5 * (1.0 - std::cos (2.0 * kPi * i / (kFft - 1)));
        re.assign ((size_t) kFft, 0.0);
        im.assign ((size_t) kFft, 0.0);
        fifo.assign ((size_t) kFft, 0.f);
        fifoPos = 0;

        bandPow.fill (0.0); learnAcc.fill (0.0); learnedPow.fill (0.0);
        learnCount = 0; learning = false; hasLearned = false; haveSpectrum = false;
        eqSmooth.fill (0.0); eqApplied.fill (0.0); autoEqSm.fill (0.0);
        lastHp = lastQ = lastX1 = lastX2 = lastMonoF = -1.0;
        bandRmsEma.fill (1e-6); bandRmsDb.fill (-60.0); rmsInit = false;
        gainDb = 0.0; lastGain = 1.0; avgGr = 0.0;
        monoMix = 0.0; bypassMix = 0.0; matchDb = 0.0;
        rngState = 0x12345678u;
        corrSm = 1.0;
    }

    int latency() const noexcept { return limiter.latency(); }

    void process (float* L, float* R, int n, const EngineParams& p)
    {
        for (int start = 0; start < n; start += kChunk)
            processChunk (L + start, R + start, std::min (kChunk, n - start), p);
    }

    const Meters& meters() const noexcept { return meterValues; }

    AutoValues autoValues() const
    {
        AutoValues v;
        for (int b = 0; b < 9; ++b) v.eq[b] = snapEq[(size_t) b].load();
        for (int b = 0; b < 3; ++b)
        {
            v.thr[b] = snapThr[(size_t) b].load();   v.ratio[b] = snapRatio[(size_t) b].load();
            v.atk[b] = snapAtk[(size_t) b].load();   v.rel[b] = snapRel[(size_t) b].load();
            v.width[b] = snapWidth[(size_t) b].load();
        }
        v.sat = snapSat.load(); v.gainDb = snapGain.load();
        return v;
    }

    // for tests / GUI
    double appliedEqDb (int band) const noexcept { return eqApplied[(size_t) band]; }
    bool   hasLearnedSnapshot() const noexcept { return hasLearned; }
    double currentGainDb() const noexcept { return gainDb; }

    static constexpr double kBandFreq[kBands] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

private:
    //--------------------------------------------------------------------------
    void analyse()
    {
        for (int i = 0; i < kFft; ++i)
        {
            re[(size_t) i] = (double) fifo[(size_t) i] * window[(size_t) i];
            im[(size_t) i] = 0.0;
        }
        fft.forward (re.data(), im.data());

        const double scale = 4.0 / (double) kFft;   // amplitude-calibrated
        double total = 0.0;
        double pw[kBands];
        for (int b = 0; b < kBands; ++b)
        {
            const double lo = kBandFreq[b] / 1.41421356;
            const double hi = std::min (kBandFreq[b] * 1.41421356, sr * 0.5);
            const int i0 = std::clamp ((int) std::floor (lo * kFft / sr), 1, kFft / 2 - 1);
            const int i1 = std::clamp ((int) std::ceil (hi * kFft / sr), i0 + 1, kFft / 2);
            double s = 0.0;
            for (int i = i0; i < i1; ++i)
            {
                const double m = std::sqrt (re[(size_t) i] * re[(size_t) i] + im[(size_t) i] * im[(size_t) i]) * scale;
                s += m * m;
            }
            pw[b] = s / (double) (i1 - i0);
            total += pw[b];
        }
        if (total < 1e-12) return; // silence: keep previous analysis

        for (int b = 0; b < kBands; ++b)
        {
            bandPow[(size_t) b] = haveSpectrum ? 0.85 * bandPow[(size_t) b] + 0.15 * pw[b] : pw[b];
            if (learning) learnAcc[(size_t) b] += pw[b];
        }
        if (learning) ++learnCount;
        haveSpectrum = true;
    }

    // spectral balance -> per-band EQ correction (dB), before smoothing
    void computeTargets (const double* pow, const GenrePreset& g, double amount, double* out) const
    {
        double diff[kBands];
        double mean = 0.0;
        for (int b = 0; b < kBands; ++b)
        {
            const double db = 10.0 * std::log10 (pow[b] + 1e-18);
            diff[b] = db - (g.tilt * b + g.offsets[b]);
            if (b >= 1 && b <= 8) mean += diff[b];
        }
        mean /= 8.0;
        const double maxC = isMaster ? 3.0 : 5.0;
        out[0] = 0.0;
        for (int b = 1; b < kBands; ++b)
            out[b] = std::clamp (-(diff[b] - mean), -maxC, maxC) * amount;
    }

    double nextRandom() noexcept
    {
        rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
        return (double) (rngState & 0xFFFFFF) / 16777216.0;
    }

    //--------------------------------------------------------------------------
    void processChunk (float* L, float* R, int n, const EngineParams& p)
    {
        const GenrePreset& g = genres[std::clamp (p.genre, 0, kNumGenres - 1)];
        const double dt = (double) n / sr;
        const double amount = std::clamp (p.amount, 0.0, 1.0);
        const bool manual = p.manual;

        // ---- learn state machine
        if (p.learn && ! learning)
        {
            learning = true; learnAcc.fill (0.0); learnCount = 0;
        }
        else if (! p.learn && learning)
        {
            learning = false;
            if (learnCount > 0)
            {
                for (int b = 0; b < kBands; ++b) learnedPow[(size_t) b] = learnAcc[(size_t) b] / (double) learnCount;
                hasLearned = true;
            }
        }

        // ---- keep the dry input (for bypass) and meter the input
        for (int i = 0; i < n; ++i) { dryL[(size_t) i] = L[i]; dryR[(size_t) i] = R[i]; }
        inMeter.process (L, R, n);
        const double inShort = inMeter.shortTerm();

        // ---- spectrum analysis of the input
        for (int i = 0; i < n; ++i)
        {
            fifo[(size_t) fifoPos++] = 0.5f * (L[i] + R[i]);
            if (fifoPos >= kFft) { analyse(); fifoPos = 0; }
        }

        // ---- auto EQ targets (always computed so "copy auto" works in either mode)
        std::array<double, kBands> autoTgt {};
        double autoTau = 1.5;
        if (p.analysisFrozen)
        {
            autoTau = 0.4;
            if (hasLearned) computeTargets (learnedPow.data(), g, amount, autoTgt.data());
        }
        else if (haveSpectrum)
        {
            computeTargets (bandPow.data(), g, amount, autoTgt.data());
        }
        const double kAuto = 1.0 - std::exp (-dt / autoTau);
        for (int b = 1; b < kBands; ++b)
        {
            autoEqSm[(size_t) b] += (autoTgt[(size_t) b] - autoEqSm[(size_t) b]) * kAuto;
            snapEq[(size_t) (b - 1)].store ((float) autoEqSm[(size_t) b]);
        }

        // ---- effective EQ (auto or manual)
        const double tau = manual ? 0.03 : autoTau;
        const double kEq = 1.0 - std::exp (-dt / tau);
        const double q = manual ? std::clamp (p.eqQ, 0.2, 5.0) : 1.0;
        const bool qChanged = std::abs (q - lastQ) > 1e-9;
        lastQ = q;
        for (int b = 1; b < kBands; ++b)
        {
            const double target = manual ? std::clamp (p.eqGain[b - 1], -12.0, 12.0) : autoTgt[(size_t) b];
            eqSmooth[(size_t) b] += (target - eqSmooth[(size_t) b]) * kEq;
            if ((qChanged || std::abs (eqSmooth[(size_t) b] - eqApplied[(size_t) b]) > 0.01)
                && kBandFreq[b] < sr * 0.45)
            {
                eqApplied[(size_t) b] = eqSmooth[(size_t) b];
                eq[0][b].setPeak (sr, kBandFreq[b], q, eqApplied[(size_t) b]);
                eq[1][b].copyCoeffs (eq[0][b]);
            }
        }
        const double hpF = manual ? std::clamp (p.lowcut, 20.0, 200.0) : 25.0;
        if (std::abs (hpF - lastHp) > 1e-9)
        {
            lastHp = hpF;
            hp[0].setHighPass (sr, hpF, 0.707); hp[1].copyCoeffs (hp[0]);
        }

        // ---- compressor / imager parameters (auto values always computed for the snapshot)
        const double baseRatio = isMaster ? 1.0 + (g.ratio - 1.0) * 0.5 : g.ratio;
        const double ratioEff = 1.0 + (baseRatio - 1.0) * amount;
        static const double ratMul[3] = { 0.8, 1.0, 0.8 }, atkMul[3] = { 1.5, 1.0, 0.6 }, relMul[3] = { 1.3, 1.0, 0.7 };
        double thr[3], rat[3], atk[3], rel[3], mk[3], wid[3];
        for (int b = 0; b < 3; ++b)
        {
            const double aThr = std::clamp (bandRmsDb[(size_t) b] + (isMaster ? 0.0 : -2.0), -60.0, 0.0);
            const double aRat = 1.0 + (ratioEff - 1.0) * ratMul[b];
            const double aAtk = (isMaster ? g.attackMs * 2.0 : g.attackMs) * atkMul[b];
            const double aRel = (isMaster ? g.releaseMs * 1.5 : g.releaseMs) * relMul[b];
            const double gw = g.width / 100.0;
            const double aWid = (b == 0) ? std::min (1.0, gw) : gw;
            snapThr[(size_t) b].store ((float) aThr);   snapRatio[(size_t) b].store ((float) aRat);
            snapAtk[(size_t) b].store ((float) aAtk);   snapRel[(size_t) b].store ((float) aRel);
            snapWidth[(size_t) b].store ((float) (aWid * 100.0));

            thr[b] = manual ? p.bThr[b] : aThr;
            rat[b] = manual ? p.bRatio[b] : aRat;
            atk[b] = manual ? p.bAtk[b] : aAtk;
            rel[b] = manual ? p.bRel[b] : aRel;
            mk[b]  = manual ? p.bMake[b] : 0.0;
            wid[b] = (manual ? p.bWidth[b] / 100.0 : aWid) * p.widthScale;
            comp[b].setParams (thr[b], rat[b], atk[b], rel[b], mk[b]);
        }
        const double autoSat = g.sat / 100.0 * amount * (isMaster ? 1.0 : 0.5);
        snapSat.store ((float) autoSat);

        const double x1 = manual ? std::clamp (p.xover1, 50.0, 400.0) : 150.0;
        const double x2 = manual ? std::clamp (p.xover2, 1000.0, 8000.0) : 3000.0;
        if (std::abs (x1 - lastX1) > 1e-9)
        {
            lastX1 = x1;
            lr1[0].setFreq (sr, x1); lr1[1].setFreq (sr, x1);
        }
        if (std::abs (x2 - lastX2) > 1e-9)
        {
            lastX2 = x2;
            lr2[0].setFreq (sr, x2); lr2[1].setFreq (sr, x2);
            ap2[0].setFreq (sr, x2); ap2[1].setFreq (sr, x2);
        }

        // ---- main loop 1: low-cut, EQ, 3-band split, per-band compression and width
        double bandSum[3] = { 0.0, 0.0, 0.0 };
        for (int i = 0; i < n; ++i)
        {
            double x[2] = { (double) L[i], (double) R[i] };
            double lo[2], rest[2], mid[2], hi[2], a[2], bb[2];
            for (int c = 0; c < 2; ++c)
            {
                double v = hp[c].process (x[c]);
                for (int b = 1; b < kBands; ++b) v = eq[c][b].process (v);
                lr1[c].process (v, lo[c], rest[c]);
                lr2[c].process (rest[c], mid[c], hi[c]);
                ap2[c].process (lo[c], a[c], bb[c]);   // all-pass compensation for the low band
                lo[c] = a[c] + bb[c];
            }
            const double bl[3] = { lo[0], mid[0], hi[0] };
            const double br[3] = { lo[1], mid[1], hi[1] };
            double oL = 0.0, oR = 0.0;
            for (int b = 0; b < 3; ++b)
            {
                bandSum[b] += 0.5 * (bl[b] * bl[b] + br[b] * br[b]);
                const double gn = comp[b].process (bl[b], br[b]);
                const double l = bl[b] * gn, r = br[b] * gn;
                const double m = 0.5 * (l + r), s = 0.5 * (l - r) * wid[b];
                oL += m + s; oR += m - s;
            }
            L[i] = (float) oL; R[i] = (float) oR;
        }
        {
            const double a = std::exp (-dt / 1.0);
            for (int b = 0; b < 3; ++b)
            {
                const double ms = bandSum[b] / (double) n;
                bandRmsEma[(size_t) b] = rmsInit ? a * bandRmsEma[(size_t) b] + (1.0 - a) * ms : ms;
                bandRmsDb[(size_t) b] = 10.0 * std::log10 (bandRmsEma[(size_t) b] + 1e-12);
            }
            rmsInit = true;
        }

        // ---- main loop 2: gain, saturation, mono bass, limiter
        const double ceilDb = std::clamp (p.ceilingDb, -6.0, 0.0);
        const double ceilLin = dbToLin (ceilDb);
        const double gainTargetDb = manual ? std::clamp (p.outGainDb, -12.0, 12.0) : gainDb;
        const double newGain = dbToLin (gainTargetDb);
        const double satAmt = manual ? std::clamp (p.sat, 0.0, 1.0) : autoSat;
        const double drive = 1.0 + 3.0 * satAmt;
        const bool useSat = satAmt > 0.001;
        const double monoF = std::clamp (p.monoFreq, 40.0, 300.0);
        if (std::abs (monoF - lastMonoF) > 1e-9)
        {
            lastMonoF = monoF;
            mb[0].setFreq (sr, monoF); mb[1].setFreq (sr, monoF);
        }
        const double monoTarget = p.monoBass ? 1.0 : 0.0;
        const double monoCoef = 1.0 - std::exp (-1.0 / (0.02 * sr));
        for (int i = 0; i < n; ++i)
        {
            const double gl = lastGain + (newGain - lastGain) * (double) (i + 1) / (double) n;
            double l = (double) L[i] * gl, r = (double) R[i] * gl;
            if (useSat)
            {
                l = ceilLin * std::tanh (drive * l / ceilLin) / drive;
                r = ceilLin * std::tanh (drive * r / ceilLin) / drive;
            }
            monoMix += (monoTarget - monoMix) * monoCoef;
            double loL, hiL, loR, hiR;
            mb[0].process (l, loL, hiL);
            mb[1].process (r, loR, hiR);
            const double mono = 0.5 * (loL + loR);
            l += ((hiL + mono) - l) * monoMix;
            r += ((hiR + mono) - r) * monoMix;
            L[i] = (float) l; R[i] = (float) r;
        }
        lastGain = newGain;

        limiter.setCeilingDb (ceilDb);
        limiter.setReleaseMs (std::clamp (p.limReleaseMs, 5.0, 1000.0));
        limiter.process (L, R, n);
        const double grChunk = std::max (0.0, -linToDb (limiter.takeMinGain()));
        const float lim = (float) ceilLin;
        for (int i = 0; i < n; ++i)
        {
            L[i] = std::clamp (L[i], -lim, lim);
            R[i] = std::clamp (R[i], -lim, lim);
        }

        outMeter.process (L, R, n);
        const double outShort = outMeter.shortTerm();

        // ---- loudness loop (auto only)
        avgGr += (grChunk - avgGr) * (1.0 - std::exp (-dt / 2.0));
        if (! manual && outShort > -55.0)
        {
            const double target = g.masterLufs - (isMaster ? 0.0 : 8.0) + p.loudOffsetDb;
            double err = target - outShort;
            if (err > 0.0 && avgGr > 9.0) err = 0.0;   // limiter already working hard: do not push further
            gainDb = std::clamp (gainDb + err * dt * 0.1, -18.0, 18.0);
        }
        snapGain.store ((float) gainDb);

        // ---- bypass (cross-faded, loudness matched, latency aligned) + dither + meters
        double matchTarget = 0.0;
        if (p.bypassMatch && inShort > -70.0 && outShort > -70.0)
            matchTarget = std::clamp (outShort - inShort, -12.0, 12.0);
        matchDb += (matchTarget - matchDb) * (1.0 - std::exp (-dt / 0.5));
        const double matchLin = dbToLin (matchDb);
        const double bTarget = p.bypass ? 1.0 : 0.0;
        const double bStep = 1.0 / (0.02 * sr);
        double peak = 0.0, sLL = 0.0, sRR = 0.0, sLR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            dryRing[0][(size_t) dryPos] = dryL[(size_t) i];
            dryRing[1][(size_t) dryPos] = dryR[(size_t) i];
            const int rp = (dryPos + 1) % ringLen;
            const double dl = (double) dryRing[0][(size_t) rp] * matchLin;
            const double dr = (double) dryRing[1][(size_t) rp] * matchLin;
            dryPos = rp;

            if (bypassMix < bTarget)      bypassMix = std::min (bTarget, bypassMix + bStep);
            else if (bypassMix > bTarget) bypassMix = std::max (bTarget, bypassMix - bStep);

            double oL = (double) L[i] * (1.0 - bypassMix) + dl * bypassMix;
            double oR = (double) R[i] * (1.0 - bypassMix) + dr * bypassMix;
            if (p.dither > 0)
            {
                const double lsb = (p.dither == 1) ? 1.0 / 32768.0 : 1.0 / 8388608.0;
                oL += (nextRandom() - nextRandom()) * lsb;
                oR += (nextRandom() - nextRandom()) * lsb;
            }
            L[i] = (float) oL; R[i] = (float) oR;
            peak = std::max (peak, std::max (std::abs (oL), std::abs (oR)));
            sLL += oL * oL; sRR += oR * oR; sLR += oL * oR;
        }

        const double corr = sLR / std::sqrt (sLL * sRR + 1e-20);
        corrSm += (corr - corrSm) * (1.0 - std::exp (-dt / 0.3));
        meterValues.momentary.store ((float) outMeter.momentary());
        meterValues.shortTerm.store ((float) outShort);
        meterValues.integrated.store ((float) outMeter.integrated());
        meterValues.peakDb.store ((float) linToDb (peak));
        meterValues.grDb.store ((float) grChunk);
        meterValues.correlation.store ((float) corrSm);
    }

    //--------------------------------------------------------------------------
    bool isMaster;
    double sr = 44100.0;

    // analysis
    Fft fft;
    std::vector<double> window, re, im;
    std::vector<float> fifo;
    int fifoPos = 0;
    std::array<double, kBands> bandPow {}, learnAcc {}, learnedPow {};
    int learnCount = 0;
    bool learning = false, hasLearned = false, haveSpectrum = false;

    // processing
    std::array<double, kBands> eqSmooth {}, eqApplied {}, autoEqSm {};
    Biquad eq[2][kBands];
    Biquad hp[2];
    LR4 lr1[2], lr2[2], ap2[2], mb[2];
    LinkedCompressor comp[3];
    LookaheadLimiter limiter;
    LoudnessMeter inMeter, outMeter;
    double lastHp = -1, lastQ = -1, lastX1 = -1, lastX2 = -1, lastMonoF = -1;
    std::array<double, 3> bandRmsEma {}, bandRmsDb {};
    bool rmsInit = false;
    double gainDb = 0.0, lastGain = 1.0, avgGr = 0.0;
    double monoMix = 0.0, bypassMix = 0.0, matchDb = 0.0, corrSm = 1.0;

    // dry path (latency aligned) and scratch
    std::vector<float> dryRing[2];
    int ringLen = 70, dryPos = 0;
    std::array<float, kChunk> dryL {}, dryR {};
    uint32_t rngState = 0x12345678u;

    // public-facing snapshots
    Meters meterValues;
    std::array<std::atomic<float>, 9> snapEq {};
    std::array<std::atomic<float>, 3> snapThr {}, snapRatio {}, snapAtk {}, snapRel {}, snapWidth {};
    std::atomic<float> snapSat { 0.f }, snapGain { 0.f };
};

} // namespace bg
