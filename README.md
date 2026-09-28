# Guitar to MIDI

A real-time audio plugin (VST3 / AU / Standalone, built on [JUCE](https://juce.com/))
that turns a clean guitar DI signal into MIDI: single notes, chords, arpeggios,
re-picked notes, hammer-ons, and (optionally) bends.

## Note from Ian
This is a continuation of a guitar pedal I made/wrote school, the hardware I used was inadequate for accurate FFT. Claude Code wrote all of this application, wanted to test Opus 5.5 out with it. The plugin works well enough for getting basic ideas from the guitar to the DAW but would like to get it in a state that live performances with it would be possible. 

## Quick start

1. Put the plugin on the track that receives your guitar DI (clean, before any
   amp sim or effects).
2. Route that track's MIDI output to an instrument track.
3. Leave **Playing Mode** on *Chords & Notes* and **Response** on *Fast*.
4. Play a few notes and watch the input meter: the bar should turn green when
   you play and grey when you stop. If hum or string noise triggers notes, raise
   **Noise Gate**; if soft notes are missed, lower it.

For lead lines on a mono synth, switch to *Single Notes (lead)*: it is faster
(about 15–30 ms) and can send pitch bend for bends and vibrato.

## Controls

| Control | What it does |
|---|---|
| Playing Mode | *Chords & Notes* (polyphonic) or *Single Notes (lead)* (monophonic, lowest latency, bends). |
| Response | How long the plugin may keep listening after a pick attack. *Fast* suits live playing; *Accurate* makes about half as many mistakes on chords for about 5 ms more latency. |
| Noise Gate | Nothing starts below this input level. The orange mark on the meter shows it. |
| Pick Sensitivity | How easily a pick attack starts a new note. |
| Max Notes | Upper limit on simultaneous notes (6 = every string). |
| Tuning (A4) | Reference pitch, if your guitar isn't at A=440. |
| MIDI Output | *Single Channel* works with any synth. *MPE* gives every note its own channel so each string can bend independently. |
| MIDI Channel | Output channel in Single Channel mode. |
| Velocity Dynamics | 0 = fixed velocity, 1 = velocity follows how hard you pick. |
| Transpose | Shifts the output, e.g. −12 for a bass patch. |
| Pitch bend / Bend Range | Sends bends and vibrato as pitch bend and sets the receiver's range (RPN 0). Small intonation wobble (under about 12 cents) is ignored so the synth doesn't sound out of tune. |
| Pass guitar audio through | Off = MIDI only; on = the dry guitar also comes out of this track. |

Hover over any control for a tooltip.

## How it works

```
Source/
  PluginProcessor.{h,cpp}   Thin wrapper: parameters -> engine, mono downmix, bypass handling.
  PluginEditor.{h,cpp}      UI: note display, input meter with gate marker, controls.
  PluginParameters.h        Parameter layout.
  dsp/
    GuitarMidiEngine.{h,cpp} The whole audio -> MIDI pipeline (host-independent).
    OnsetDetector.h          Pick-attack detection (log-compressed SuperFlux).
    PolyPitchDetector.h      Chord detection (Klapuri-style harmonic salience).
    YinPitchDetector.h       Single-note pitch (YIN, FFT-accelerated).
    MidiNoteOutput.h         Channels, MPE setup, velocity, pitch bend, note on/off.
    MusicMath.h              Pitch/frequency helpers.
Tools/Eval/                  Offline test and conversion tool (see below).
```

**Notes are decided at pick attacks.** When an attack is detected, the engine
analyses a window that *starts at the attack* and grows every 5 ms. Because it
never looks at audio from before the attack, the previous chord can't bleed
into the new one. A pitch is committed once it has been found in several
consecutive analyses (artefacts from short windows flicker; real notes
persist):

- Single notes and high strings usually commit in about 25–30 ms.
- A chord's low strings commit as soon as they can be told apart. For example,
  G2 and B2 are only 25 Hz apart and need several periods of audio, so a low
  voicing fills in over roughly 40–60 ms, much like a fast strum.
- A note is held back while unresolved energy sits below it, because until the
  low strings are identified a "high note" may just be one of their overtones.

**Chord detection** whitens the spectrum band by band, sums
harmonic amplitudes over a 1/8-semitone grid of candidate pitches, and picks
notes **bottom-up**: in guitar voicings, most upper notes are overtones of the
lower ones (an open E chord is mostly E2's own harmonic series). Taking the
lowest note first lets its overtones be explained before they can pose as
extra notes. A candidate also needs a real fundamental in the raw spectrum,
which rejects sub-octave ghosts.

**Between attacks**, a longer sliding window keeps track of which notes are
still ringing (for note-offs and per-note bends). A slower fallback path picks
up notes that start with no detectable attack, and switches quickly for
hammer-ons and pull-offs.

**Re-picks and re-strums:** an attack that brings no new pitch must have
re-struck notes that were already sounding, so those notes retrigger. In a
let-ring arpeggio each pluck adds a new pitch, so the strings that are still
ringing are left alone.

**Hum rejection:** whenever the input is silent, the engine learns the tonal
background (mains hum and its harmonics) and removes it from later analyses.

**MIDI hygiene:** a channel's pitch bend is reset before every note-on. In MPE
mode the plugin sends the MPE zone setup and bend range. Bypassing the plugin,
or changing mode, channel or transpose, releases all notes, so none get stuck.
No fixed latency is reported to the host, so live monitoring isn't delayed.

CPU use is about 10–11% of one core at 44.1 or 48 kHz.

## Testing without a guitar: `GuitarToMidiEval`

`Tools/Eval` builds a command-line tool that uses a physically modelled guitar
(Karplus-Strong strings with pick-position and pickup comb filtering, pickup
tone, string decay, bends, hammer-ons, mains hum and noise) to synthesise test
passages with exact ground truth. It runs them through the same engine the
plugin uses and scores the MIDI output.

```bash
GuitarToMidiEval                  # full suite (add --seed N for a different random take)
GuitarToMidiEval --only chords --verbose chords   # expected vs detected notes
GuitarToMidiEval --only soft --set trace=1        # log the engine's decisions
GuitarToMidiEval --render outdir  # also write each test as a .wav
GuitarToMidiEval --bench          # CPU cost
GuitarToMidiEval my_di_take.wav [out.mid] [--mono]   # convert a real recording
```

The last form is the quickest way to check the engine on your own playing:
record a dry DI take, convert it, and compare the notes it prints with what you
played.

Current results on the synthetic suite (Fast response, averaged over seeds):

| | Before rewrite | Now |
|---|---|---|
| Note F1 (right pitch, onset within 80 ms) | 0.15 | 0.77 |
| Precision | 0.10 | 0.90 |
| Wrong-pitch notes (not octave-related) | 1476 of 2393 | about 25 of 620 |
| Notes triggered by hum/noise in silence | 132 | 0 |
| Chord pitch classes present | 0.78 | 0.99 |
| Single notes, runs, repeated notes, bends | poor | 0.97–1.00 F1 |
| Median latency | 40 ms | about 27 ms for single notes, about 45 ms for chord bass notes |

Most of the remaining recall misses are octave doublings inside chords (for
example the E4 on top of an open E), which have little musical effect.

**Caveat:** these numbers come from synthesised guitar. Real pickups, strings
and playing will differ. The thresholds most worth adjusting against a real
signal are all in one place: the tunables at the top of
`Source/dsp/GuitarMidiEngine.cpp`. They can be tried without rebuilding by
passing `--set name=value` to the eval tool.

## Build

Requires a C++17 compiler, CMake 3.22+, and JUCE (fetched automatically, or
point `JUCE_LOCAL_PATH` at a local checkout).

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_LOCAL_PATH=F:/juce-8.0.15
cmake --build build --config Release --target GuitarToMidi_VST3
cmake --build build --config Release --target GuitarToMidi_Standalone
cmake --build build --config Release --target GuitarToMidiEval
```

Outputs land in `build/GuitarToMidi_artefacts/Release/` (VST3, Standalone) and
`build/GuitarToMidiEval_artefacts/Release/`. If the VST3 link step fails with
"cannot open file", a DAW still has the plugin loaded; close it and rebuild.
Pass `-DGTM_BUILD_EVAL=OFF` to skip the eval tool.

Parameter IDs changed in this version (parameter version 2), so sessions saved
with the old build will load with default settings.
