# Phazerville ORN8 — PKarotte Custom Firmware Branch

This branch (**`PKarotte-branch`**) adds high-performance audio applets, restores full 5-voice Mutable Instruments Braids polyphony, adds a zero-overshoot brickwall limiter for feedback/clipping protection, and fixes MIDI streaming input issues.

---

## 📦 Compiled Firmware Binaries (.hex)

If you are compiling locally with PlatformIO, the compiled `.hex` files are generated at:

| Target | Description | Location on Disk |
| :--- | :--- | :--- |
| **`T41_audio`** | Teensy 4.1 with full audio rack enabled | `software/.pio/build/T41_audio/firmware.hex` |
| **`T41`** | Teensy 4.1 standard build | `software/.pio/build/T41/firmware.hex` |

### How to Flash:
1. Open the **Teensy Loader** (or Teensy Flasher) application.
2. Select **File → Open HEX File** and choose `software/.pio/build/T41_audio/firmware.hex`.
3. Press the program button on the Teensy 4.1 (or trigger via software reboot) to flash.

---

## 🚀 Key Features & Changes in this Branch

### 1. Zero-Overshoot Brickwall Limiter (`Limiter`)
*Files: `software/src/Audio/AudioEffectBrickwallLimiter.h`, `software/src/audio_applets/LimiterApplet.h`*

A lookahead audio limiter designed specifically to tame loud modular signals, eliminate digital clipping, and suppress runaway feedback loops.
- **Lookahead Peak Detection (64 samples / ~1.33 ms):** Eliminates transient overshoot without audible clicks or harsh clipping.
- **Strict Brickwall Ceiling:** Output levels mathematically cannot exceed the user-defined ceiling (`-18.0 dB` to `0.0 dBFS`).
- **Input Drive Pre-Gain:** `-18 dB` to `+24 dB` with CV modulation support.
- **Anti-Feedback Auto-Release:** Fast recovery (35 ms) on drum/transient spikes that automatically slows down (250 ms) during sustained oscillation, eliminating harmonic buzzing and pumping.
- **Soft-Knee Polynomial Saturation:** `Hard` (clean brickwall), `Soft` (smooth transition), or `Warm` (tape-style saturation near ceiling).
- **Stereo Sidechain Linking:** Links peak detection across left and right channels to preserve stereo imaging.
- **Real-Time OLED Metering:** Right-to-left horizontal gain reduction bar with numerical dB readout and peak-hold tick.
- **Panic Killswitch:** Instant bypass toggle via the **Aux button**.
- **Mono Sidechain CV Output:** In Mono mode, **Out 2** outputs positive Gain Reduction CV (0 to 5V) for ducking external VCAs or modulating filters.

---

### 2. Full 5-Voice Braids Polyphonic Synthesizer (`Braids`)
*Files: `software/src/audio_applets/BraidsVoiceApplet.h`*

Full, faithful port of all 39 Mutable Instruments Braids macro oscillator models with independent dual-mono processing.
- **All 39 Macro Models:** CSAW, MORPH, SAW_SQ, SYNC, SIN_TR, BUZZ, 3xSAW, 3xSQR, 3xRING, SWARM, COMB, TOY, FLT_LP, FLT_PK, FLT_BP, FLT_HP, VOSIM, VOWEL, VWL_FO, FM, FDBKFM, CHAOS, BELL, DRUM, PLUCK, BOWED, BLOWN, FLUTE, WAVTBL, WAV_MP, WAV_LN, WAV_PR, NOIS_F, NOIS_T, NOIS_C, NOIS_G, NOIS_P, PARAPH, QM.
- **5-Voice Polyphony (`NUM_BRAIDS_POLY = 5`):**
  - `MON`: Strict 1-voice mono with percussive strike support
  - `RR5`: 5-voice round-robin dynamic voice allocation
  - `UNI`: 5-voice fat detuned unison spread
  - `CHD`: 5-note extended 9th chords across timbre knob (Maj9, Min9, Dom9, Sus4/9, Half-Dim9, Power/Octaves, 5th Stack, Min Octave)
- **Dual Envelopes:** Attack (snap to slow ramp) + Decay/Release (snap to tail to INF drone).
- **Dual Input Source:** Accepts CV + Gate, MIDI, or Gate toggle switching.

---

### 3. MIDI Stability & Sequencer Input Bugfixes
*Files: `software/src/audio_applets/BraidsVoiceApplet.h`, `software/src/HSIOFrame.h`*

- **Fixed MIDI Channel Setting Lockout (`E0` bug):**  
  Previously, incoming MIDI messages (clock, pitch bend, CC 16, or notes) caused the UI parameter `M:` to overwrite the channel display with a note name (e.g. `E0`). When connected to sequencers (such as Behringer Steps), `M:` was permanently stuck displaying `E0` and could not be changed. The MIDI channel field now **always** displays the active channel (`OFF`, `01`..`16`, `OMN`) and can be edited at any time while sequencer playback is running.
- **Removed Debug Serial Flood:**  
  Removed `Serial.print` calls inside the audio/MIDI interrupt callback, eliminating USB serial buffer stalling under dense clock/note traffic.
- **Strict Message Filtering:**  
  Pitch bend (`0xE0`), control change, and messages for other channels no longer corrupt internal note-tracking.
- **Memory Overhead Optimization:**  
  Internal MIDI frame queues were optimized to static inline structures, saving over 30 KB of `.bss.dma` memory. This allows full 5-voice Braids, Cloudverb, and the Brickwall Limiter to coexist with **240 KB of free RAM2 headroom**.

---

### 4. Clouds Algorithmic Reverb (`Cloudverb`)
*Files: `software/src/audio_applets/CloudverbApplet.h`, `software/src/Audio/AudioEffectCloudsReverb.h`*

Stereo algorithmic reverberation based on Mutable Instruments Clouds, featuring pitch decay, variable diffusion, damping, and freeze.
