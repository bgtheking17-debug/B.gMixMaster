#pragma once
// Genre presets (pure data). Values are a starting point and are meant to be tuned by ear.

namespace bg
{
struct GenrePreset
{
    const char* name;
    double tilt;         // target spectral slope, dB per octave
    double offsets[10];  // per-band shape on top of the slope (dB): 31,62,125,250,500,1k,2k,4k,8k,16k
    double masterLufs;   // loudness target when used as the mastering plugin
    double ratio;        // compressor ratio (mix plugin; the mastering plugin uses a gentler version)
    double attackMs;
    double releaseMs;
    double width;        // stereo width, percent
    double sat;          // saturation amount, percent
};

inline const GenrePreset genres[] =
{
    //  name                tilt   31    62    125   250   500   1k    2k    4k    8k    16k     LUFS   ratio atk   rel    width sat
    { "Pop",               -4.5, { 0.0,  1.0,  1.0,  0.0, -0.5,  0.0,  1.0,  1.5,  2.0,  1.5 }, -10.0, 3.0,  15.0, 120.0, 110.0, 12.0 },
    { "Mizrahit",          -4.5, { 0.0,  1.0,  1.5,  1.0,  0.0,  1.0,  2.5,  2.0,  1.5,  1.0 },  -9.0, 3.5,  12.0, 110.0, 105.0, 15.0 },
    { "Mizrahit Rhythmic", -4.2, { 0.0,  2.0,  1.0, -0.5, -0.5,  0.5,  2.0,  2.5,  2.0,  1.0 },  -8.5, 4.0,   8.0,  90.0, 105.0, 18.0 },
    { "Mizrahit Quiet",    -4.8, { 0.0,  0.0,  1.0,  1.5,  0.5,  0.5,  1.0,  0.5,  0.0, -0.5 }, -12.0, 2.0,  25.0, 220.0, 115.0,  8.0 },
    { "Boom Bap",          -4.8, { 0.0,  2.0,  2.5,  1.0,  0.0,  0.5,  1.0,  0.0, -1.0, -2.0 }, -11.0, 3.0,  30.0, 130.0,  95.0, 25.0 },
    { "Hip Hop",           -4.2, { 0.0,  3.0,  2.0, -1.0, -1.0,  0.0,  1.5,  2.5,  2.0,  1.0 },  -9.0, 4.0,  20.0, 100.0, 100.0, 18.0 },
    { "Trap",              -4.0, { 0.0,  4.0,  2.0, -1.5, -1.5,  0.0,  1.0,  2.5,  3.0,  2.0 },  -8.0, 4.0,  15.0,  90.0, 100.0, 20.0 },
    { "R&B",               -4.8, { 0.0,  2.0,  2.0,  1.0,  0.0,  0.0,  1.0,  1.0,  1.0,  1.0 }, -11.0, 2.5,  20.0, 160.0, 115.0, 12.0 },
    { "EDM / Dance",       -4.0, { 0.0,  3.0,  1.5, -1.0, -1.0,  0.0,  1.0,  2.0,  3.0,  3.0 },  -7.5, 4.0,  10.0,  80.0, 120.0, 22.0 },
    { "Rock",              -4.5, { 0.0,  0.0,  1.0,  1.0,  0.5,  1.5,  2.0,  1.5,  1.0,  0.5 }, -10.0, 3.0,  15.0, 130.0, 105.0, 25.0 },
    { "Acoustic",          -5.0, { 0.0, -1.0,  0.0,  1.0,  0.5,  0.5,  1.0,  1.0,  1.5,  1.5 }, -13.0, 2.0,  25.0, 200.0, 110.0,  5.0 },
    { "Neutral",           -4.5, { 0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0 }, -14.0, 3.0,  20.0, 150.0, 100.0, 10.0 },
};

inline constexpr int kNumGenres = (int) (sizeof (genres) / sizeof (genres[0]));
} // namespace bg
