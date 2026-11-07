// Copyright (c) 2025, Nicholas J. Michalek and 2026, Pascal
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <array>
#include <algorithm>
#include "../HSUtils.h"

// ============================================================================
// MidiCapture: Ableton Move / Push-style Retrospective MIDI Capture Applet
// ============================================================================
// Features:
//  - Retrospective Capture: Constantly listens in the background. Jam freely!
//    Hit [CAP] (or Aux button, or pulse Digital 2) to capture what you just played!
//  - Bar-Synchronized: Loops of 1, 2, 4, or 8 bars (16 steps per bar).
//  - Musical Downbeat Alignment: Automatically extracts and phases the captured
//    phrase with the master clock grid without missing a beat.
//  - Polyphonic Capture: Records up to 4 notes per step with gate durations.
//  - Zero-Latency Live Thru: Direct CV Pitch, Gate, and MIDI Out while playing.
//  - Overdub Mode: Layer chords and melodies on top of captured loops.
//  - Live Visual Timeline: Real-time OLED step display with moving playhead.
//  - Auto Internal Clock Fallback (120 BPM) if no clock cable is patched.
// ============================================================================

class MidiCapture : public HemisphereApplet {
public:
    static constexpr int STEPS_PER_BAR = 16;
    static constexpr int MAX_BARS = 8;
    static constexpr int MAX_STEPS = MAX_BARS * STEPS_PER_BAR; // 128 steps
    static constexpr int MAX_POLY = 4;
    static constexpr int MAX_HELD_NOTES = 16;

    enum NotePriority : uint8_t {
        PRIORITY_LAST = 0,
        PRIORITY_HIGH = 1,
        PRIORITY_LOW  = 2
    };

    enum QuantizeMode : uint8_t {
        QUANT_16TH = 0, // 1/16 grid
        QUANT_8TH  = 1, // 1/8 grid
        QUANT_4TH  = 2, // 1/4 grid
        QUANT_OFF  = 3  // Raw step timing
    };

    enum PlaybackState : uint8_t {
        STATE_LIVE = 0,    // Listening in background, no loop playing
        STATE_PLAYING = 1, // Loop actively playing
        STATE_OVERDUB = 2, // Playing loop + merging new live notes
        STATE_MUTED = 3    // Loop paused/muted (live listening continues)
    };

    enum AppMode : uint8_t {
        MODE_CAPTURE = 0, // [CAP] captures/replaces loop
        MODE_OVERDUB = 1  // [DUB] overdubs notes into loop
    };

    enum MidiCaptureCursor {
        CURSOR_BARS = 0,
        CURSOR_QUANTIZE,
        CURSOR_MIDI_IN,
        CURSOR_MIDI_OUT,
        CURSOR_MODE,
        CURSOR_PRIORITY,
        CURSOR_ACTION_CAP,
        CURSOR_ACTION_CLR,
        CURSOR_ACTION_MUTE,
        CURSOR_COUNT
    };
    static constexpr int MAX_CURSOR = CURSOR_COUNT - 1;

    struct CapturedNote {
        uint8_t note;     // 0..127
        uint8_t vel;      // 1..127
        uint8_t gate_len; // Duration in steps (1..MAX_STEPS)
    };

    struct StepNotes {
        uint8_t count;
        CapturedNote notes[MAX_POLY];

        void clear() {
            count = 0;
        }

        bool add(uint8_t n, uint8_t v, uint8_t len = 1) {
            for (int i = 0; i < count; ++i) {
                if (notes[i].note == n) {
                    notes[i].vel = v;
                    notes[i].gate_len = len;
                    return true;
                }
            }
            if (count < MAX_POLY) {
                notes[count++] = {n, v, len};
                return true;
            }
            return false;
        }
    };

    struct LiveNote {
        uint8_t note;
        uint8_t vel;
    };

    const char* applet_name() {
        return "MidiCap";
    }
    const uint8_t* applet_icon() {
        return PhzIcons::midiIn;
    }

    void Start() {
        bars = 2; // Default: 2 bars (32 steps)
        quantize = QUANT_16TH;
        midi_in_ch = 0;  // Omni
        midi_out_ch = 1; // Ch 1
        app_mode = MODE_CAPTURE;
        note_priority = PRIORITY_LAST;
        state = STATE_LIVE;
        num_held_notes = 0;
        last_cv_pitch = 0;
        last_clock_ms = 0;
        step_period_ms = 125; // 120 BPM 16ths
        last_internal_tick_ms = 0;
        global_clock_step = 0;
        last_played_step = 0;
        note_timer = 0;
        beat_flash = 0;
        capture_flash = 0;

        for (int i = 0; i < MAX_STEPS; ++i) {
            history_buffer[i].clear();
            loop_buffer[i].clear();
        }

        frame.MIDIState.any_channel_omni = true;
        frame.MIDIState.ClearMonoBuffer();
        frame.MIDIState.ClearPolyBuffer();
    }

    void Reset() {
        SendNotesOff();
        if (num_held_notes == 0) GateOut(1, false);
        global_clock_step = 0;
        note_timer = 0;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Retrospective Capture Core: extracts the most recent phrase into the loop
    // ──────────────────────────────────────────────────────────────────────────
    void Capture() {
        int total_steps = GetLoopLength();
        int current_pos = global_clock_step % total_steps;

        // How long ago was the last note played?
        int steps_since_last_note = global_clock_step - last_played_step;

        // Align capture window with musical bar boundaries:
        // If the phrase was played in the current cycle: take current cycle.
        // If the phrase ended in the previous bar and the user hit capture slightly late:
        // take the previous complete bar cycle!
        bool use_prev_cycle = (steps_since_last_note > current_pos) &&
                              (steps_since_last_note < (total_steps + current_pos));

        for (int p = 0; p < total_steps; ++p) {
            int target_tick = 0;
            if (use_prev_cycle) {
                target_tick = (global_clock_step - current_pos - total_steps) + p;
            } else {
                if (p <= current_pos) {
                    target_tick = global_clock_step - (current_pos - p);
                } else {
                    target_tick = global_clock_step - (current_pos + total_steps - p);
                }
            }

            int hist_idx = (target_tick % MAX_STEPS + MAX_STEPS) % MAX_STEPS;
            loop_buffer[p] = history_buffer[hist_idx];
        }

        state = STATE_PLAYING;
        capture_flash = 12; // Visual flash badge
    }

    void ClearLoop() {
        SendNotesOff();
        for (int i = 0; i < MAX_STEPS; ++i) {
            loop_buffer[i].clear();
        }
        if (num_held_notes == 0) GateOut(1, false);
        state = STATE_LIVE;
    }

    void ToggleMute() {
        if (state == STATE_PLAYING || state == STATE_OVERDUB) {
            SendNotesOff();
            if (num_held_notes == 0) GateOut(1, false);
            state = STATE_MUTED;
        } else if (state == STATE_MUTED) {
            state = STATE_PLAYING;
        } else if (state == STATE_LIVE) {
            Capture();
        }
    }

    int GetLoopLength() const {
        return constrain(bars * STEPS_PER_BAR, STEPS_PER_BAR, MAX_STEPS);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Live Keyboard Tracking
    // ──────────────────────────────────────────────────────────────────────────
    int GetHeldNoteCV() {
        if (num_held_notes <= 0) return -1;
        int selected = -1;
        for (int i = 0; i < num_held_notes; ++i) {
            int n = held_notes[i].note;
            if (selected == -1) {
                selected = n;
            } else if (note_priority == PRIORITY_LOW && n < selected) {
                selected = n;
            } else if (note_priority == PRIORITY_HIGH && n > selected) {
                selected = n;
            } else if (note_priority == PRIORITY_LAST) {
                selected = n;
            }
        }
        return selected;
    }

    void AddHeldNote(uint8_t note, uint8_t vel) {
        for (int i = 0; i < num_held_notes; ++i) {
            if (held_notes[i].note == note) {
                held_notes[i].vel = vel;
                return;
            }
        }
        if (num_held_notes < MAX_HELD_NOTES) {
            held_notes[num_held_notes++] = {note, vel};
        }
    }

    void RemoveHeldNote(uint8_t note) {
        for (int i = 0; i < num_held_notes; ++i) {
            if (held_notes[i].note == note) {
                for (int j = i; j < num_held_notes - 1; ++j) {
                    held_notes[j] = held_notes[j + 1];
                }
                num_held_notes--;
                return;
            }
        }
    }

    int GetStepNote(int s) {
        int len = GetLoopLength();
        if (s < 0 || s >= len || loop_buffer[s].count == 0) return -1;
        int selected = -1;
        for (int i = 0; i < loop_buffer[s].count; ++i) {
            int n = loop_buffer[s].notes[i].note;
            if (selected == -1) {
                selected = n;
            } else if (note_priority == PRIORITY_LOW && n < selected) {
                selected = n;
            } else if (note_priority == PRIORITY_HIGH && n > selected) {
                selected = n;
            } else if (note_priority == PRIORITY_LAST) {
                selected = n;
            }
        }
        return selected;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Controller Loop: Advances Clock, Plays Loop, Manages Gate Durations
    // ──────────────────────────────────────────────────────────────────────────
    void Controller() {
        uint32_t now = millis();
        bool clock_tick = false;

        bool is_clock_running = HS::clock_m.IsRunning();
        if (was_clock_running && !is_clock_running) {
            SendNotesOff();
            GateOut(1, false);
            note_timer = 0;
            for (int i = 0; i < num_held_notes; ++i) {
                if (midi_out_ch > 0 && midi_out_ch <= 16) {
                    HS::frame.MIDIState.SendNoteOff(midi_out_ch - 1, held_notes[i].note);
                    HS::frame.MIDIState.ProcessMIDIMsg({(uint8_t)midi_out_ch, 0x80, held_notes[i].note, 0});
                    HS::frame.MIDIState.QueueInternalMsg({(uint8_t)midi_out_ch, 0x80, held_notes[i].note, 0});
                }
            }
            num_held_notes = 0;
            if (midi_out_ch > 0 && midi_out_ch <= 16) {
                HS::frame.MIDIState.SendCC(midi_out_ch - 1, 123, 0);
                HS::frame.MIDIState.SendCC(midi_out_ch - 1, 120, 0);
            }
        }
        was_clock_running = is_clock_running;

        // Digital 2: External Trigger for Instant Capture!
        if (Clock(1)) {
            Capture();
        }

        // Digital 1: Master Clock Input
        if (Clock(0)) {
            clock_tick = true;
            if (last_clock_ms > 0 && now > last_clock_ms) {
                uint32_t diff = now - last_clock_ms;
                if (diff >= 10 && diff <= 5000) {
                    step_period_ms = diff;
                }
            }
            last_clock_ms = now;
        } else if (is_clock_running && last_clock_ms == 0) {
            // Auto fallback only when internal clock is active and no external pulses
            if (now - last_internal_tick_ms >= step_period_ms) {
                clock_tick = true;
                last_internal_tick_ms = now;
            }
        }

        if (clock_tick) {
            beat_flash = 2;
            global_clock_step++;

            int total_steps = GetLoopLength();
            int current_play_step = global_clock_step % total_steps;
            int hist_step = global_clock_step % MAX_STEPS;

            // Prepare fresh slot in circular history buffer
            history_buffer[hist_step].clear();

            // Record any currently sustained live notes into the history buffer
            for (int i = 0; i < num_held_notes; ++i) {
                history_buffer[hist_step].add(held_notes[i].note, held_notes[i].vel, 1);
                if (state == STATE_OVERDUB) {
                    loop_buffer[current_play_step].add(held_notes[i].note, held_notes[i].vel, 1);
                }
            }

            // Note-off for previously playing step
            SendNotesOff();

            // Active Loop Playback (when no live keys are overriding)
            if ((state == STATE_PLAYING || state == STATE_OVERDUB) && num_held_notes == 0) {
                if (loop_buffer[current_play_step].count > 0) {
                    // Send Polyphonic MIDI Out
                    if (midi_out_ch > 0 && midi_out_ch <= 16) {
                        for (int i = 0; i < loop_buffer[current_play_step].count; ++i) {
                            uint8_t n = loop_buffer[current_play_step].notes[i].note;
                            uint8_t v = loop_buffer[current_play_step].notes[i].vel;
                            HS::frame.MIDIState.SendNoteOn(midi_out_ch - 1, n, v);
                            HS::frame.MIDIState.ProcessMIDIMsg({(uint8_t)midi_out_ch, 0x90, n, v});
                            HS::frame.MIDIState.QueueInternalMsg({(uint8_t)midi_out_ch, 0x90, n, v});
                            active_notes[active_note_count++] = n;
                        }
                    }

                    // Monophonic CV Pitch & Gate Out
                    int cv_note = GetStepNote(current_play_step);
                    if (cv_note >= 0) {
                        // Optional CV Transpose on In(0)
                        int transpose = 0;
                        if (DetentedIn(0)) {
                            transpose = Proportion(In(0), HEMISPHERE_3V_CV, 24);
                        }
                        last_cv_pitch = MIDIQuantizer::CV(constrain(cv_note + transpose, 0, 127));
                        Out(0, last_cv_pitch);
                        GateOut(1, true);
                    }

                    // Gate duration timer (approx 65% of clock step)
                    note_timer = (ClockCycleTicks(0) > 0) ? (ClockCycleTicks(0) * 2 / 3) : 200;
                } else {
                    GateOut(1, false);
                }
            } else if (num_held_notes > 0) {
                // Live monitoring priority
                int cv_note = GetHeldNoteCV();
                if (cv_note >= 0) {
                    last_cv_pitch = MIDIQuantizer::CV(cv_note);
                    Out(0, last_cv_pitch);
                    GateOut(1, true);
                }
            }
        }

        // Timed NoteOff between clock edges when playing captured loop
        if (num_held_notes == 0 && note_timer > 0) {
            if (--note_timer == 0) {
                SendNotesOff();
                GateOut(1, false);
            }
        }
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Incoming MIDI Processing: Instant Zero-Latency Thru + Smart History Log
    // ──────────────────────────────────────────────────────────────────────────
    void OnMIDIMessage(uint8_t channel, uint8_t message, uint8_t data1, uint8_t data2) override {
        if (midi_in_ch > 0 && midi_in_ch <= 16 && channel != midi_in_ch) return;

        uint8_t status = message & 0xF0;
        bool is_note_on  = (status == 0x90 && data2 > 0);
        bool is_note_off = (status == 0x80 || (status == 0x90 && data2 == 0));

        if (is_note_on) {
            uint8_t note = data1;
            uint8_t vel = data2;

            AddHeldNote(note, vel);
            last_played_step = global_clock_step;

            // Live Monitoring: CV Pitch & Gate
            int cv_note = GetHeldNoteCV();
            if (cv_note >= 0) {
                last_cv_pitch = MIDIQuantizer::CV(cv_note);
                Out(0, last_cv_pitch);
                GateOut(1, true);
            }

            // Live Monitoring: MIDI Out
            if (midi_out_ch > 0 && midi_out_ch <= 16) {
                HS::frame.MIDIState.SendNoteOn(midi_out_ch - 1, note, vel);
                HS::frame.MIDIState.ProcessMIDIMsg({(uint8_t)midi_out_ch, 0x90, note, vel});
                if (midi_out_ch != channel) {
                    HS::frame.MIDIState.QueueInternalMsg({(uint8_t)midi_out_ch, 0x90, note, vel});
                }
            }

            // Quantization & Target Step Calculation
            uint32_t now = millis();
            uint32_t elapsed = (now >= last_clock_ms) ? (now - last_clock_ms) : 0;
            int target_tick = global_clock_step;

            if (quantize == QUANT_16TH) {
                // If played in the latter 40% of the step, snap forward to the downbeat
                if (step_period_ms > 0 && elapsed > ((step_period_ms * 3) / 5)) {
                    target_tick = global_clock_step + 1;
                }
            } else if (quantize == QUANT_8TH) {
                // Snap to 8th note boundary (even steps)
                if (target_tick % 2 != 0) target_tick++;
            } else if (quantize == QUANT_4TH) {
                // Snap to quarter note boundary
                int rem = target_tick % 4;
                if (rem >= 2) target_tick += (4 - rem);
                else target_tick -= rem;
            }

            int target_hist_step = (target_tick % MAX_STEPS + MAX_STEPS) % MAX_STEPS;
            history_buffer[target_hist_step].add(note, vel, 1);

            // If in Overdub mode, inject directly into active playback loop
            if (state == STATE_OVERDUB) {
                int total_steps = GetLoopLength();
                int target_loop_step = (target_tick % total_steps + total_steps) % total_steps;
                loop_buffer[target_loop_step].add(note, vel, 1);
            }
        } else if (is_note_off) {
            uint8_t note = data1;
            RemoveHeldNote(note);

            if (midi_out_ch > 0 && midi_out_ch <= 16) {
                HS::frame.MIDIState.SendNoteOff(midi_out_ch - 1, note);
                HS::frame.MIDIState.ProcessMIDIMsg({(uint8_t)midi_out_ch, 0x80, note, 0});
                if (midi_out_ch != channel) {
                    HS::frame.MIDIState.QueueInternalMsg({(uint8_t)midi_out_ch, 0x80, note, 0});
                }
            }

            if (num_held_notes == 0) {
                if (note_timer <= 0) GateOut(1, false);
            } else {
                int cv_note = GetHeldNoteCV();
                if (cv_note >= 0) {
                    last_cv_pitch = MIDIQuantizer::CV(cv_note);
                    Out(0, last_cv_pitch);
                    GateOut(1, true);
                }
            }
        } else if (status == 0xB0) {
            if (data1 == 120 || data1 == 123) {
                SendNotesOff();
                GateOut(1, false);
                note_timer = 0;
                num_held_notes = 0;
            }
        } else if (message == 0xFC) {
            SendNotesOff();
            GateOut(1, false);
            note_timer = 0;
            num_held_notes = 0;
        }
    }

    void SendNotesOff() {
        if (midi_out_ch > 0 && midi_out_ch <= 16 && active_note_count > 0) {
            for (int i = 0; i < active_note_count; ++i) {
                HS::frame.MIDIState.SendNoteOff(midi_out_ch - 1, active_notes[i]);
                HS::frame.MIDIState.ProcessMIDIMsg({(uint8_t)midi_out_ch, 0x80, active_notes[i], 0});
                HS::frame.MIDIState.QueueInternalMsg({(uint8_t)midi_out_ch, 0x80, active_notes[i], 0});
            }
        }
        active_note_count = 0;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // OLED View: Header, Settings, Mini Piano Roll / Timeline, Buttons
    // ──────────────────────────────────────────────────────────────────────────
    void View() {
        int total_steps = GetLoopLength();
        int current_step = global_clock_step % total_steps;
        int current_bar = (current_step / STEPS_PER_BAR) + 1;
        int current_beat = ((current_step % STEPS_PER_BAR) / 4) + 1;

        // Header: Bar.Beat Progress (e.g. "1.1") & Beat Flash Dot
        gfxPos(40, 2);
        graphics.printf("%d.%d", current_bar, current_beat);
        if (beat_flash > 0) {
            beat_flash--;
            gfxPixel(60, 2);
            gfxPixel(61, 2);
            gfxPixel(60, 3);
            gfxPixel(61, 3);
        }

        // Row 1: Bars & Quantize (y = 13)
        gfxPrint(1, 13, "Bar:");
        gfxStartCursor(23, 13);
        graphics.printf("%d", bars);
        gfxEndCursor(cursor == CURSOR_BARS);

        gfxPrint(37, 13, "Q:");
        gfxStartCursor(47, 13);
        if (quantize == QUANT_16TH) graphics.print("16");
        else if (quantize == QUANT_8TH) graphics.print(" 8");
        else if (quantize == QUANT_4TH) graphics.print(" 4");
        else graphics.print("--");
        gfxEndCursor(cursor == CURSOR_QUANTIZE);

        // Row 2: MIDI In & MIDI Out channels (y = 23)
        gfxPrint(1, 23, "In:");
        gfxStartCursor(17, 23);
        if (midi_in_ch == 0) graphics.print("Om");
        else graphics.printf("%02d", midi_in_ch);
        gfxEndCursor(cursor == CURSOR_MIDI_IN);

        gfxPrint(33, 23, "Out:");
        gfxStartCursor(51, 23);
        if (midi_out_ch == 0) graphics.print("--");
        else graphics.printf("%02d", midi_out_ch);
        gfxEndCursor(cursor == CURSOR_MIDI_OUT);

        // Row 3: Live Visual Timeline / Mini Step Sequencer (y = 33..39)
        gfxFrame(2, 33, 60, 7);
        for (int s = 0; s < total_steps; ++s) {
            if (loop_buffer[s].count > 0) {
                int px = 3 + (s * 57) / total_steps;
                gfxPixel(px, 35);
                gfxPixel(px, 36);
                gfxPixel(px, 37);
            }
        }
        // Animated Playhead
        if (state == STATE_PLAYING || state == STATE_OVERDUB) {
            int head_x = 3 + (current_step * 57) / total_steps;
            gfxInvert(head_x, 34, 1, 5);
        }

        // Row 4: Mode & Status Badge (y = 44)
        gfxPrint(1, 44, "M:");
        gfxStartCursor(11, 44);
        graphics.print(app_mode == MODE_CAPTURE ? "Cap" : "Dub");
        gfxEndCursor(cursor == CURSOR_MODE);

        gfxStartCursor(29, 44);
        if (note_priority == PRIORITY_LAST) graphics.print("1");
        else if (note_priority == PRIORITY_HIGH) graphics.print("H");
        else graphics.print("L");
        gfxEndCursor(cursor == CURSOR_PRIORITY);

        // Status Badge at x = 36, y = 43
        if (capture_flash > 0) {
            capture_flash--;
            gfxPrint(38, 44, "CAPT");
            gfxInvert(36, 43, 26, 9);
        } else if (state == STATE_OVERDUB) {
            gfxPrint(39, 44, "DUB");
            gfxInvert(36, 43, 26, 9);
        } else if (state == STATE_PLAYING) {
            gfxPrint(38, 44, "PLAY");
            gfxFrame(36, 43, 26, 9);
        } else if (state == STATE_MUTED) {
            gfxPrint(38, 44, "MUTE");
            gfxFrame(36, 43, 26, 9);
        } else {
            gfxPrint(38, 44, "LIVE");
            gfxFrame(36, 43, 26, 9);
        }

        // Row 5: Action Buttons (y = 54)
        gfxStartCursor(1, 54);
        graphics.print("[CAP]");
        gfxEndCursor(cursor == CURSOR_ACTION_CAP);

        gfxStartCursor(26, 54);
        graphics.print("[CLR]");
        gfxEndCursor(cursor == CURSOR_ACTION_CLR);

        gfxStartCursor(47, 54);
        if (state == STATE_MUTED) graphics.print("[PLY]");
        else graphics.print("[MUT]");
        gfxEndCursor(cursor == CURSOR_ACTION_MUTE);
    }

    // Physical Aux button: Instant one-touch capture!
    void AuxButton() override {
        Capture();
        CancelEdit();
    }

    void OnButtonPress() override {
        if (cursor == CURSOR_ACTION_CAP) {
            Capture();
        } else if (cursor == CURSOR_ACTION_CLR) {
            ClearLoop();
        } else if (cursor == CURSOR_ACTION_MUTE) {
            ToggleMute();
        } else {
            CursorToggle();
        }
    }

    void OnEncoderMove(int direction) override {
        if (!EditMode()) {
            MoveCursor(cursor, direction, MAX_CURSOR);
            return;
        }

        switch (cursor) {
            case CURSOR_BARS: {
                // 1, 2, 4, 8 bars
                int cur_bars = bars;
                if (direction > 0) {
                    if (cur_bars == 1) bars = 2;
                    else if (cur_bars == 2) bars = 4;
                    else if (cur_bars == 4) bars = 8;
                } else if (direction < 0) {
                    if (cur_bars == 8) bars = 4;
                    else if (cur_bars == 4) bars = 2;
                    else if (cur_bars == 2) bars = 1;
                }
                break;
            }
            case CURSOR_QUANTIZE:
                quantize = (QuantizeMode)constrain((int)quantize + direction, 0, 3);
                break;
            case CURSOR_MIDI_IN:
                midi_in_ch = constrain(midi_in_ch + direction, 0, 16);
                frame.MIDIState.any_channel_omni = true;
                break;
            case CURSOR_MIDI_OUT:
                midi_out_ch = constrain(midi_out_ch + direction, 0, 16);
                break;
            case CURSOR_MODE:
                app_mode = (app_mode == MODE_CAPTURE) ? MODE_OVERDUB : MODE_CAPTURE;
                if (app_mode == MODE_OVERDUB && state == STATE_PLAYING) {
                    state = STATE_OVERDUB;
                } else if (app_mode == MODE_CAPTURE && state == STATE_OVERDUB) {
                    state = STATE_PLAYING;
                }
                break;
            case CURSOR_PRIORITY:
                note_priority = (NotePriority)constrain((int)note_priority + direction, 0, 2);
                break;
            case CURSOR_ACTION_CAP:
                Capture();
                CancelEdit();
                break;
            case CURSOR_ACTION_CLR:
                ClearLoop();
                CancelEdit();
                break;
            case CURSOR_ACTION_MUTE:
                ToggleMute();
                CancelEdit();
                break;
            default:
                break;
        }

        ResetCursor();
    }

    uint64_t OnDataRequest() override {
        uint64_t data = 0;
        Pack(data, PackLocation{0, 4}, bars);
        Pack(data, PackLocation{4, 2}, quantize);
        Pack(data, PackLocation{6, 5}, midi_in_ch);
        Pack(data, PackLocation{11, 5}, midi_out_ch);
        Pack(data, PackLocation{16, 1}, app_mode);
        Pack(data, PackLocation{17, 2}, note_priority);
        return data;
    }

    void OnDataReceive(uint64_t data) override {
        bars = Unpack(data, PackLocation{0, 4});
        quantize = (QuantizeMode)Unpack(data, PackLocation{4, 2});
        midi_in_ch = Unpack(data, PackLocation{6, 5});
        midi_out_ch = Unpack(data, PackLocation{11, 5});
        app_mode = (AppMode)Unpack(data, PackLocation{16, 1});
        note_priority = (NotePriority)Unpack(data, PackLocation{17, 2});

        if (bars != 1 && bars != 2 && bars != 4 && bars != 8) bars = 2;
        if (quantize > 3) quantize = QUANT_16TH;
        if (midi_in_ch > 16) midi_in_ch = 0;
        if (midi_out_ch > 16) midi_out_ch = 1;
        if (note_priority > 2) note_priority = PRIORITY_LAST;
    }

protected:
    void SetHelp() override {
        help[HELP_DIGITAL1] = "Clock";
        help[HELP_DIGITAL2] = "Cap Trig";
        help[HELP_CV1]      = "Transpose";
        help[HELP_CV2]      = "Mod";
        help[HELP_OUT1]     = "Pitch";
        help[HELP_OUT2]     = "Gate";
        help[HELP_EXTRA1]   = "MIDI Capture";
        help[HELP_EXTRA2]   = "Aux: Capture";
    }

private:
    int cursor = 0;
    uint8_t bars = 2;
    QuantizeMode quantize = QUANT_16TH;
    uint8_t midi_in_ch = 0;
    uint8_t midi_out_ch = 1;
    AppMode app_mode = MODE_CAPTURE;
    NotePriority note_priority = PRIORITY_LAST;
    PlaybackState state = STATE_LIVE;

    int last_cv_pitch = 0;
    uint8_t beat_flash = 0;
    uint8_t capture_flash = 0;
    int note_timer = 0;

    int global_clock_step = 0;
    int last_played_step = 0;

    uint32_t last_clock_ms = 0;
    uint32_t step_period_ms = 125;
    uint32_t last_internal_tick_ms = 0;

    LiveNote held_notes[MAX_HELD_NOTES];
    int num_held_notes = 0;

    uint8_t active_notes[MAX_POLY];
    int active_note_count = 0;
    bool was_clock_running = false;

    // Fixed-allocation buffers: zero dynamic malloc during audio execution
    StepNotes history_buffer[MAX_STEPS];
    StepNotes loop_buffer[MAX_STEPS];
};

