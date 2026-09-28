# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

JUCE 8 audio plugin (VST3/AU/Standalone) that converts a clean guitar DI signal into MIDI (chords + single notes). Windows / MSVC / Visual Studio 2022; JUCE is a local checkout at `F:/juce-8.0.15`.

## Build

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_LOCAL_PATH=F:/juce-8.0.15
cmake --build build --config Release --target GuitarToMidi_VST3        # also: GuitarToMidi_Standalone
cmake --build build --config Release --target GuitarToMidiEval -- -m   # offline test tool
```

- The VST3 link step fails with `LNK1104 cannot open file ...Guitar to MIDI.vst3` when a DAW (the user runs Ableton Live 11) has the plugin loaded. Ask the user to close it; don't kill it.
- `F:\VST\Guitar to MIDI.vst3` is a junction to the build output, so a successful build installs it. Nothing needs to be copied.

## Testing: `GuitarToMidiEval` (the only test harness)

There are no unit tests. `Tools/Eval` synthesises guitar audio with exact ground truth (Karplus-Strong strings, pick/pickup combs, hum, noise; `GuitarSynth.h`). It runs the audio through the same `GuitarMidiEngine` the plugin uses and scores the MIDI.

```bash
E=./build/GuitarToMidiEval_artefacts/Release/GuitarToMidiEval.exe
$E < /dev/null                                   # full suite; last line = TOTAL
$E --only poly/chords-strummed --verbose chords  # one scenario, expected vs detected notes
$E --only soft --set trace=1                     # per-hop engine decision log (onsets, gather detections, note on/off)
$E --set kGhostRatio=1.0 --set kRelAmplitude=0.3 # try engine tunables without rebuilding
$E --seed 3                                      # different random take (check for overfitting)
$E --response 2                                  # 0 fast (default) / 1 balanced / 2 accurate
$E --probe 40 47 52 56 59 64 [--spectrum --peaks]  # poly detector alone on one pluck/chord at growing window lengths
$E --bench                                       # CPU cost
$E take.wav [out.mid] [--mono]                   # convert a real recording
```

Columns: `prec`/`recall`/`F1` count a note as correct if the pitch matches and it starts within 80 ms. `oct` = extra octave-related notes (mostly harmless). `wrong` = extra non-octave wrong pitches (the ones that sound bad). `pcR` = pitch class sounding 100 ms in. `susR` = pitch class still sounding mid-note and near the end.

Recent baseline (seed 0): F1 about 0.78, prec 0.90, wrong 26, pcR 0.99, susR 0.98. Judge changes by this TOTAL line, and check several `--seed`s before adopting a tuning. The user plays live and prioritises low latency and few wrong notes over strict recall.

Run the tool with `< /dev/null`. The shell tool here mangles backslash escapes in heredocs, so write multi-line Python patch scripts with the file tool, not `cat <<EOF`. Source files may have CRLF line endings.

## Architecture

`PluginProcessor` is a thin wrapper. It reads APVTS params into `gtm::EngineSettings` each block, downmixes to mono, and calls the engine. It also sends all-notes-off on bypass and reports zero latency, which is deliberate for live monitoring. All DSP lives in `Source/dsp/`, which is host-independent so the eval tool can drive it.

`GuitarMidiEngine` (the core): every hop (about 5 ms) runs onset detection, then either `polyHop` or `monoHop`.
- **Poly, attack-driven "gather":** an onset opens a window starting at the attack that grows each hop, so the previous chord never bleeds in. `PolyPitchDetector` runs on it each hop. A pitch is committed once it is seen `confirmAnalyses` hops in a row, is past `polyCommitMin`, spans at least `kMinPeriods` periods, and has no unexplained energy below it (`unexplainedBelow`). High notes commit fast; low strings fill in later. The gather lasts `kGatherSeconds`.
- **Commits** can retrigger a sounding note. The per-partial energy jump excludes partials shared with other detected notes. An attack that brings *no new pitch* retriggers the sounding notes whose energy rose most. Notes sounding before the attack that aren't re-detected are released if their energy fell (`releaseMutedNotes`).
- **`polyTrack`** runs between gathers on a 70 ms sliding window. It handles note-offs (miss count, decay floor, lenient "fundamental still there" hold for octave notes), MPE bends, a slow fallback for notes without attacks, a fast legato switch for hammer-ons, and hum learning during silence.
- **Mono:** YIN on the post-attack segment, then median smoothing. Hammer-ons become legato note switches; bends become pitch bend.
- Tunables are mutable globals at the top of `GuitarMidiEngine.cpp`, settable via `setEngineTuning` (eval `--set`). `GTM_ENGINE_TRACE` is defined only for the eval target.

`PolyPitchDetector`: Hann window, zero-padded to one fixed FFT size (shared bin grid for any window length). Then hum subtraction, Klapuri band whitening, a peak-only spectrum, and harmonic salience on a 1/8-semitone grid with a low-partial presence gate. Selection is **bottom-up** (lowest clear salience peak first, because upper chord notes are mostly overtones of lower ones). Each pick also needs a real raw fundamental (rejects sub-octave ghosts), plus relative-salience, relative-amplitude and harmonic-ghost checks, and spectral-smoothness subtraction. Things tried and rejected: an asymmetric fast-rise window (sidelobe phantoms) and top-down greedy selection (high notes steal low strings' partials).

`MidiNoteOutput`: voices and channels (single channel or MPE lower zone via `MPEMessages::setLowerZone`), transpose, bend dead-zone, bend reset before note-on, RPN 0 bend range. A structural config change requires all notes off; the engine handles this.

Parameters are in `PluginParameters.h` (parameter version 2; IDs changed from v1). The editor derives combo items from the `AudioParameterChoice` choices.
