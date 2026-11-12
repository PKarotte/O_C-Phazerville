# Phazerville ORN8 — PKarotte Custom Firmware Suite

[![PlatformIO CI](https://github.com/djphazer/O_C-Phazerville/actions/workflows/firmware.yml/badge.svg)](https://github.com/djphazer/O_C-Phazerville/actions/workflows/firmware.yml)

A high-performance custom firmware fork of the **Phazerville Suite** for Ornament & Crime / ORN8, tailored for live Eurorack performance, multi-voice digital audio synthesis, live MIDI chord learning, and modular feedback/clipping protection.

> [!IMPORTANT]
> **Hardware Requirement: Teensy 4.1 ONLY (`T41_audio`)**  
> This custom firmware is designed and optimized exclusively for **Teensy 4.1 hardware** (such as **ORN8** or Teensy 4.1-based Ornament & Crime modules).
> - The **Braids** polyphonic synthesizer, **Brickwall Limiter**, and **Cloudverb** run inside Phazerville's digital **Audio Rack**, which strictly requires Teensy 4.1.
> - Advanced features like **MultiScale live MIDI chord capture & chord routing** and **MidiCapture** rely on the extended USB Host, high-speed RAM, and DMA architecture present on the Teensy 4.1.
> - **Teensy 4.0 (`T40`) and Teensy 3.2 are not supported by this custom suite.**

---

## 📦 Download & Flashing

Pre-compiled firmware binaries are available in the repository Releases or in your local build directory:

| Firmware Target | Target Hardware | Description | File Location |
| :--- | :--- | :--- | :--- |
| **`T41_audio`** *(Recommended)* | Teensy 4.1 / ORN8 | Full build with Audio Rack, 5-Voice Braids, Limiter, & all applets | `software/.pio/build/T41_audio/firmware.hex` |
| **`T41`** | Teensy 4.1 | Standard CV-only build without Audio Rack | `software/.pio/build/T41/firmware.hex` |

### How to Flash:
1. Download or locate `software/.pio/build/T41_audio/firmware.hex` (or grab it from [**GitHub Releases**](https://github.com/PKarotte/O_C-Phazerville/releases)).
2. Open the official **[Teensy Loader](https://www.pjrc.com/teensy/loader.html)** application.
3. In Teensy Loader, click **File → Open HEX File** and choose `firmware.hex`.
4. Press the physical button on your Teensy 4.1 (or trigger via software reboot) to flash.

---

## 🚀 Key Features & Changes in this Firmware

### 1. 🎹 Full 5-Voice Braids Polyphonic Synthesizer (`Braids`)
*Source: `software/src/audio_applets/BraidsVoiceApplet.h`, `software/src/braids/*`*

Integrates the complete sound palette of Mutable Instruments Braids as a native 5-voice polyphonic audio applet running at high resolution:
- **All 39 Macro Oscillator Models:** CSAW, MORPH, SAW_SQ, SYNC, SIN_TR, BUZZ, 3xSAW, 3xSQR, 3xRING, SWARM, COMB, TOY, FLT_LP, FLT_PK, FLT_BP, FLT_HP, VOSIM, VOWEL, VWL_FO, FM, FDBKFM, CHAOS, BELL, DRUM, PLUCK, BOWED, BLOWN, FLUTE, WAVTBL, WAV_MP, WAV_LN, WAV_PR, NOIS_F, NOIS_T, NOIS_C, NOIS_G, NOIS_P, PARAPH, QM.
- **5-Voice Polyphony (`NUM_BRAIDS_POLY = 5`):**
  - `MON`: Strict 1-voice mono mode with percussive strike support.
  - `RR5`: 5-voice dynamic round-robin voice allocation.
  - `UNI`: 5-voice detuned unison spread (`{0.0f, -1.0f, +1.0f, -0.5f, +0.5f}`).
  - `CHD`: 5-note extended 9th chord structures morphing across Timbre (Maj9, Min9, Dom9, Sus4/9, Half-Dim9, 5th Stack, etc.).
- **Dual Envelopes:** Attack (snap to slow swell) + Decay/Release (snap to 15s tail to infinite drone).
- **Dual Pitch/Gate Source:** Accepts CV + Gate, MIDI, or Gate toggle switching.
- **RAM2 Optimized:** Enables 5-voice Braids, Cloudverb, and Limiter with **240 KB of free headroom** in Teensy 4.1 RAM2.

---

### 2. 🎼 MultiScale Applet Overhaul — Interactive, Live Chords & MIDI Chord Out
*Source: `software/src/applets/MultiScale.h`*

Completely transforms the `MultiScale` quantizer from a basic CV-switched quantizer into an interactive 3-page harmonic workstation with live chord detection and polyphonic MIDI output:
- **Page 1 (Interactive Keyboard Visualizer):**
  - Full white and black piano key graphic display.
  - Simultaneous visual indicators for active Playing scale, Editing scale, and Recording scale / `LIVE` status.
  - Interactive cursor navigation to manually toggle scale degrees on/off.
- **Page 2 (MIDI In Settings & Live Harmonic Follower):**
  - MIDI Channel assignment (`Omni`, `1`..`16`).
  - Target Recording Scale selector (`1`..`4`) with mini-keyboard preview showing the active mask.
  - **Live Harmonic Follower Mode (`Live: ON/OFF`):** Detects incoming MIDI chords in real time and immediately updates the active quantizer scale. Keeps the last played chord held indefinitely until the next chord is played. While Live mode is ON, Digital 2 scale cycling is ignored so your live keyboard playing acts as the harmonic master.
  - `[CLR]` button to instantly clear custom scales back to root.
- **Page 3 (MIDI Chord Out & Strumming Engine):**
  - Polyphonic MIDI Out channel (`Off`, `1`..`16`) with internal routing directly into synthesizer applets (e.g. Braids).
  - **Gate Length:** Tempo-synced duration in bar fractions (`1/1` hold for the entire scale duration, `1/2`, `1/4`, `1/8`, `1/16`, `1/32`).
  - **Gate Offset:** Delayed start timing (`0`, `1/16`, `1/8`, `1/4`, `1/2` bar).
  - **Play Modes:** `Chord` (all voices struck simultaneously) or `Strum` (notes articulated sequentially from low to high).
  - **Voicing Controls:** Octave selection (`0`–`8`) and Inversions (`-6` to `+6`).
- **Intelligent MIDI Chord & Progression Capture:**
  - Play chords or melodies over MIDI (keyboard, sequencer, or DAW) directly into MultiScale.
  - Built-in musical chord-gathering window (320 ms gap / 600 ms max window) automatically binds held notes into a scale mask.
  - In normal progression mode, automatically advances to the next scale slot (`rec_scale = (rec_scale + 1) % 4`), allowing you to record an entire 4-chord progression on the fly without touching the encoder.
  - Automatic progression timeout (8s of silence resets target back to Scale 1).
  - Activity dot in the upper-left corner confirms incoming MIDI packets.
- **Sequential Scale Clocking (TR2 / TR4):**
  - Advance through scales 1–4 sequentially on incoming clock triggers (in normal progression mode).
- **Dual-Hemisphere Lockstep Normalization:**
  - When loaded in the Right Hemisphere without TR4 physically patched, it automatically normals to TR2 (`frame.clocked[1]`), keeping left and right hemispheres synchronized without external mults.
- **Dedicated Voice Gate & Trigger Passthrough:**
  - CV2 (GateIn) passes through to Out 2 (GateOut) with calibrated ~5ms trigger pulses on rising edges and sustained gate hysteresis, completely independent of clock tempo.
- **Transport Stop Protection:**
  - Instantly silences gate outputs and sends MIDI note-offs when the master clock stops to prevent hanging notes.

---

### 3. 🛡️ Zero-Overshoot Brickwall Limiter (`Limiter`)
*Source: `software/src/Audio/AudioEffectBrickwallLimiter.h`, `software/src/audio_applets/LimiterApplet.h`*

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

### 4. ⏺️ Ableton Push / Move-Style `MidiCapture` Applet
*Source: `software/src/applets/MidiCapture.h` (also aliases `MidiLoop`)*

A retrospective MIDI recorder that constantly listens in the background:
- **Retrospective Capture:** Jam freely without pressure. Whenever you play a great phrase, hit **`[CAP]`**, the **Aux button**, or send a pulse to **Digital 2** to instantly capture the last 1, 2, 4, or 8 bars into a looping sequence.
- **Musical Downbeat Alignment:** Automatically phases the captured phrase to the master clock grid so it starts on beat 1.
- **Polyphonic Capture:** Captures up to 4 notes per step with note durations and velocities.
- **Zero-Latency Live Thru:** Passes Pitch CV, Gate, and MIDI Out in real-time while playing.
- **Overdub Mode:** Layer new notes and chords onto an existing loop.
- **Live OLED Step Timeline:** Visual scrolling playhead showing note events.
- **Internal Clock Fallback:** Runs at 120 BPM internally if no clock cable is patched.

---

### 5. 🎛️ TB-3PO Pattern Length CV Modulation
*Source: `software/src/applets/TB3PO.h`*

- Added assignable **CV Modulation for Pattern Length** directly under Pitch and Gate in the double-click config menu.
- Full attenuversion support (`-100%` to `+100%`).
- Non-destructive: base pattern length is retained and modulated dynamically by incoming control voltage.

---

### 6. 🌌 Clouds Algorithmic Reverb (`Cloudverb`)
*Source: `software/src/Audio/AudioEffectCloudsReverb.h`, `software/src/audio_applets/CloudverbApplet.h`*

High-density algorithmic stereo reverberation based on Mutable Instruments Clouds, featuring pitch decay, variable diffusion, damping, and freeze.

---

### 7. 📁 Enhanced WAV Player (`WavPlayer`)
*Source: `software/src/audio_applets/WAVPlayerApplet.h`*

- **Dynamic Loop Points:** Set Loop Start % and Loop Length % with CV modulation.
- **Sub-Beat Loop Rolls:** 1/16, 1/8, 1/4, and 1/2 beat repeat loop rolls synced to clock.
- **Clock Transport Integration:** Playback cleanly starts on clock run and stops when clock stops.

---

### 8. 🧹 Streamlined Audio Rack & Memory Optimization (What Was Removed)

In official upstream `main-dev`, dozens of experimental, placeholder, or toy audio applets were statically allocated in memory simultaneously. This completely choked Teensy 4.1's internal RAM2 (AXI SRAM), causing linker overflow crashes and making polyphonic synthesis impossible.

To make room for **5-voice Braids**, the **Brickwall Limiter**, and **Cloudverb** while maintaining **240 KB of clean RAM2 headroom**, the audio rack was intentionally streamlined:

#### Removed / Replaced Audio Applets:
* **Redundant Oscillators:** `OscApplet`, `HandSawApplet`, `HarmOscApplet`, `FMDrumApplet`, `WTVCOApplet` (all completely superseded by Braids' 39 macro oscillator engines).
* **Experimental / Toy FX:** `Abyss`, `Animorf`, `DelayApplet`, `FilterFolder`, `DynamicsApplet`, `PhazerApplet`, `Samverb/Bungverb`, `ThreeBandz`, `TuneTracker`, `Glitch`, `Grit`, `AdvKrpsStrng`, `ModalResonator`, `WavRecorder`, `Upsampled`, `Crosspan`, `MidSide`.
* **Legacy Voice Engine:** The older 2-voice prototype `VoiceApplet` was removed in favor of `BraidsVoiceApplet`.

#### Removed CV Applets:
* `BugCrack` and `DrLoFi` were removed from the CV menu to free up applet slots.
* `MidiLoop` was replaced by the significantly more powerful retrospective `MidiCapture` (aliased for backwards compatibility).

#### Active Curated Audio Rack:
1. **`Braids`** — 5-Voice Polyphonic Synthesizer (39 macro models)
2. **`Limiter`** — Zero-Overshoot Lookahead Brickwall Limiter
3. **`Cloudverb`** — High-density stereo algorithmic reverb
4. **`WavPlayer`** — Enhanced beat-synchronized WAV player with sub-beat rolls & loop points
5. **`Ladder`** — Resonant 4-pole lowpass ladder filter
6. **`Input` & `Passthru`** — High-fidelity stereo routing utilities

**Result:** A clean, uncluttered live performance menu, rock-solid audio stability with zero buffer stuttering, and **240 KB of free headroom** in high-speed RAM2.

---

### 9. ⚙️ System-Wide MIDI, Clock & Core Infrastructure Fixes

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
- **DMA Memory Optimization:**
  - Internal MIDI queues were converted to static inline structures, saving over 30 KB of `.bss.dma` memory and ensuring zero dynamic allocation during audio processing.

---

## 🛠️ Building with PlatformIO

If you prefer building from source instead of using the pre-compiled `.hex`:

```bash
# Clone the repository
git clone -b PKarotte-branch https://github.com/PKarotte/O_C-Phazerville.git
cd O_C-Phazerville/software

# Build the recommended Teensy 4.1 audio target
pio run -e T41_audio
```

Compiled output will be placed in:
`software/.pio/build/T41_audio/firmware.hex`

---

## 🙏 Credits & Acknowledgments

- **djphazer** for the incredible [Phazerville Suite](https://github.com/djphazer/O_C-Phazerville).
- **mxmxmx** and the original Ornament & Crime team for the iconic O_C hardware platform.
- **Émilie Gillet / Mutable Instruments** for the DSP algorithms of Braids and Clouds.
- **Nic Newdigate** for the TeensyVariablePlayback library.
- **Paul Stoffregen** for the Teensy platform and Audio library.

---

## 📄 License
This project is released under the **MIT License**, consistent with the upstream Phazerville and Ornament & Crime projects. See individual source files for third-party algorithm copyrights (Mutable Instruments, etc.).
