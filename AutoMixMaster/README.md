# B.g Mastering / B.g Mix (VST3)

Two VST3 plugins for FL Studio (Windows): **B.g Mastering** (master bus) and **B.g Mix** (tracks / buses).
Auto & Manual modes, genre presets, Learn, 3-band compression, true-peak limiter, LUFS metering.

## Build (GitHub Actions)
Upload everything (keep folders, including `.github`), open the Actions tab, wait for the green check,
download artifact `BG-Plugins-VST3-Windows`.

## Install
1. Delete the old `Auto Mix.vst3` / `Auto Master.vst3` from `C:\Program Files\Common Files\VST3`.
2. Copy `B.g Mastering.vst3` and `B.g Mix.vst3` there.
3. Close FL fully, reopen, Options -> Manage plugins -> Find installed plugins.

## Tests
`g++ -std=c++17 -O2 -I Source Tests/dsp_test.cpp -o dsp_test && ./dsp_test`
