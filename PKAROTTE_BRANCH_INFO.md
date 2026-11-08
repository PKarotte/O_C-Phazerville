# Phazerville ORN8 — PKarotte Custom Firmware Branch

This branch (**`PKarotte-branch`**) introduces major enhancements, new audio synthesis engines, an Ableton-style MIDI capture workflow, an overhauled chord-learning quantizer, modular clipping protection, and system-wide MIDI/clock stability improvements compared to the official upstream `main-dev` branch.

---

## 📦 Compiled Firmware Binaries (.hex)

For Teensy 4.1 users who want ready-to-flash binaries without compiling locally:

| Target | Description | Local Path on Disk |
| :--- | :--- | :--- |
| **`T41_audio`** *(Recommended)* | Teensy 4.1 with full Audio rack & applets enabled | `software/.pio/build/T41_audio/firmware.hex` |
| **`T41`** | Teensy 4.1 standard CV build | `software/.pio/build/T41/firmware.hex` |

### How to Flash:
1. Open the **Teensy Loader** (or Teensy Flasher) desktop app.
2. Go to **File → Open HEX File** and choose `software/.pio/build/T41_audio/firmware.hex`.
3. Press the physical button on your Teensy 4.1 (or trigger via software reboot) to flash.

---

## 🚀 Key Features & Changes vs Official `main-dev`

### 1. Full 5-Voice Braids Polyphonic Synthesizer (`Braids`)
*Files: `software/src/audio_applets/BraidsVoiceApplet.h`, `software/src/braids/*`*

Integrates the full sound palette of Mutable Instruments Braids as a native 5-voice polyphonic audio applet:
- **All 39 Macro Oscillator Models:** CSAW, MORPH, SAW_SQ, SYNC, SIN_TR, BUZZ, 3xSAW, 3xSQR, 3xRING, SWARM, COMB, TOY, FLT_LP, FLT_PK, FLT_BP, FLT_HP, VOSIM, VOWEL, VWL_FO, FM, FDBKFM, CHAOS, BELL, DRUM, PLUCK, BOWED, BLOWN, FLUTE, WAVTBL, WAV_MP, WAV_LN, WAV_PR, NOIS_F, NOIS_T, NOIS_C, NOIS_G, NOIS_P, PARAPH, QM.
- **5-Voice Polyphony (`NUM_BRAIDS_POLY = 5`):**
  - `MON`: Strict 1-voice mono mode with percussive strike support.
  - `RR5`: 5-voice dynamic round-robin voice allocation.
  - `UNI`: 5-voice detuned unison spread (`{0.0f, -1.0f, +1.0f, -0.5f, +0.5f}`).
  - `CHD`: 5-note extended 9th chord structures morphing across the Timbre control (Maj9, Min9, Dom9, Sus4/9, Half-Dim9, 5th Stack, etc.).
- **Dual Envelopes:** Attack (snap to slow swell) + Decay/Release (snap to 15s tail to infinite drone).
- **Dual Pitch/Gate Source:** Accepts CV + Gate, MIDI, or Gate toggle switching.
- **RAM2 Optimized:** Enables full 5-voice Braids, Cloudverb, and Limiter with **240 KB of free headroom** in Teensy 4.1 RAM2.

---

### 2. MultiScale Applet Overhaul — Interactive & MIDI Chord Learning
*File: `software/src/applets/MultiScale.h`*

Completely revamps the `MultiScale` quantizer from a basic CV-switched quantizer into an interactive 2-page scale workstation with live chord capture:
- **Page 1 (Interactive Keyboard Visualizer):**
  - Full white and black piano key graphic display.
  - Simultaneous visual indicators for active Playing scale, Editing scale, and Recording scale.
  - Interactive cursor navigation to quickly toggle scale degrees on/off.
- **Page 2 (MIDI Settings & Management):**
  - MIDI Channel assignment (`Omni`, `1`..`16`).
  - Target Recording Scale selector (`1`..`4`) with mini-keyboard preview showing the active mask.
  - `[RESET ALL]` button to instantly clear custom scales back to root.
- **Intelligent MIDI Chord & Progression Capture:**
  - Play chords or melodies over MIDI (keyboard, sequencer, or DAW) directly into MultiScale.
  - Built-in musical chord-gathering window (320 ms gap / 600 ms max window) automatically binds held notes into a scale mask.
  - Automatically advances to the next scale slot (`rec_scale = (rec_scale + 1) % 4`), allowing you to record an entire 4-chord progression on the fly without touching the encoder.
  - Automatic progression timeout (8s of silence resets target back to Scale 1).
  - Activity dot in the upper-left corner confirms incoming MIDI packets.
- **Sequential Scale Clocking (TR2 / TR4):**
  - Advance through scales 1–4 sequentially on incoming clock triggers.
- **Dual-Hemisphere Lockstep Normalization:**
  - When loaded in the Right Hemisphere without TR4 physically patched, it automatically normals to TR2 (`frame.clocked[1]`), keeping left and right hemispheres perfectly synchronized without external mults.
- **Dedicated Voice Gate & Trigger Passthrough:**
  - CV2 (GateIn) passes through to Out 2 (GateOut) with calibrated ~5ms trigger pulses on rising edges and sustained gate hysteresis, completely independent of clock tempo.
- **Transport Stop Protection:**
  - Instantly silences gate outputs when the master clock stops to prevent hanging notes.

---

### 3. Ableton Push / Move-Style `MidiCapture` Applet
*File: `software/src/applets/MidiCapture.h` (also aliases `MidiLoop`)*

A retrospective MIDI recorder that constantly listens in the background:
- **Retrospective Capture:** Jam freely without pressure. Whenever you play a great phrase, hit **[CAP]**, the **Aux button**, or send a pulse to **Digital 2** to instantly capture the last 1, 2, 4, or 8 bars into a looping sequence.
- **Musical Downbeat Alignment:** Automatically phases the captured phrase to the master clock grid so it starts on beat 1.
- **Polyphonic Capture:** Captures up to 4 notes per step with note durations and velocities.
- **Zero-Latency Live Thru:** Passes Pitch CV, Gate, and MIDI Out in real-time while playing.
- **Overdub Mode:** Layer new notes and chords onto an existing loop.
- **Live OLED Step Timeline:** Visual scrolling playhead showing note events.
- **Internal Clock Fallback:** Runs at 120 BPM internally if no clock cable is patched.

---

### 4. Zero-Overshoot Brickwall Limiter (`Limiter`)
*Files: `software/src/Audio/AudioEffectBrickwallLimiter.h`, `software/src/audio_applets/LimiterApplet.h`*

A lookahead audio limiter designed specifically for Eurorack to tame hot signals, eliminate digital clipping, and suppress runaway feedback:
- **Lookahead Peak Detection (64 samples / ~1.33 ms):** Eliminates transient overshoot without audible clicks.
- **Strict Brickwall Ceiling:** Guaranteed output ceiling from `-18.0 dB` to `0.0 dBFS`.
- **Pre-Gain Input Drive:** `-18 dB` to `+24 dB` with CV modulation support.
- **Anti-Feedback Auto-Release:** Fast recovery (35 ms) on transient drum hits that dynamically slows down (250 ms) during sustained feedback oscillation, eliminating harmonic buzzing and pumping.
- **Soft-Knee Saturation Options:** `Hard` (pure transparent brickwall), `Soft` (smooth transition), or `Warm` (tape-style saturation near ceiling).
- **Stereo Sidechain Linking:** Links peak detection across left and right channels to preserve stereo imaging.
- **Panic Killswitch:** Aux button instantly bypasses the limiter.
- **Sidechain Ducking CV Out:** In Mono mode, Out 2 outputs a 0–5V gain-reduction envelope for ducking external VCAs or modulating filters.

---

### 5. Clouds Algorithmic Reverb (`Cloudverb`)
*Files: `software/src/Audio/AudioEffectCloudsReverb.h`, `software/src/audio_applets/CloudverbApplet.h`*

High-density algorithmic stereo reverberation based on Mutable Instruments Clouds, featuring pitch decay, variable diffusion, damping, and freeze.

---

### 6. Enhanced WAV Player (`WavPlayer`)
*File: `software/src/audio_applets/WAVPlayerApplet.h`*

- **Dynamic Loop Points:** Set Loop Start % and Loop Length % with CV modulation.
- **Sub-Beat Loop Rolls:** 1/16, 1/8, 1/4, and 1/2 beat repeat loop rolls synced to clock.
- **Clock Transport Integration:** Playback cleanly starts on clock run and stops when clock stops.

---

### 7. System-Wide MIDI, Clock & Core Infrastructure Fixes

- **Fixed MIDI Channel UI Freeze (`E0` Bug):**
  - Incoming MIDI streams (pitch bend `0xE0`, clock, CC 16 from sequencers like Behringer Steps) previously overwrote the `M:` parameter display with note names and locked the encoder.
  - The channel display now unconditionally shows the active channel (`OFF`, `01`..`16`, `OMN`) and remains fully editable during playback.
  - Debug `Serial.print` calls inside audio/MIDI interrupts were removed to prevent USB serial buffer stalls.
- **System-Wide Panic & Transport Stop Broadcast:**
  - When the master clock stops, `HSClockManager` now broadcasts **All Notes Off (CC 123)** and **All Sound Off (CC 120)** across all 16 MIDI channels on USB Device, USB Host 1, USB Host 2, and Hardware Serial MIDI.
- **Sequencer Hanging Gate Prevention:**
  - `Seq32` and `TB_3PO` drop gates immediately when the clock stops. `TB_3PO` also includes a slide timeout to prevent notes from droning if the clock stops mid-slide.
- **Digital Inputs as Modulation Sources (`CVInputMap`):**
  - Added `TYPE_DIGITAL_INPUT` (`T` / Gate icons) to CVInputMap so physical digital inputs (TR1..TR4) can directly modulate applet parameters.
- **Direct MIDI Routing to Audio Applets:**
  - `AudioAppletSubapp` routes incoming USB/Host/Serial MIDI and internal messages directly to audio applets via `OnMIDIMessage`.
- **Memory Overhead Optimization:**
  - Internal MIDI queues were converted to static inline structures, saving over 30 KB of `.bss.dma` memory and ensuring zero dynamic allocation during audio processing.

---

### 8. TB-3PO Pattern Length CV Modulation
*Files: `software/src/applets/TB3PO.h`, `software/src/HemisphereApplet.h`, `software/src/HemisphereApplet.cpp`, `software/src/apps/Hemisphere.h`, `software/src/apps/Quadrants.h`*

- **Assignable Pattern Length Modulation:** Added assignable `CVInputMap` modulation directly into the double-click config menu underneath `Pitch` and `Gate` as `[ - ] Ptn Lgth`.
- **Source Selection & Attenuation:** Modulate pattern length using any CV or MIDI source (`C 1`–`C 8`, `M 1`–`M 16`, etc.) with full attenuversion (`-148%` to `+148%`) using the standard 3-click encoder workflow.
- **Non-Destructive Storage:** The base pattern length (`num_steps`) remains safe and intact in preset data while CV dynamically modulates the effective loop length between 1 and 32 steps in real time.
- **Main Screen Untouched:** Preserves TB-3PO's single-page main performance interface without extra menu clutter.
