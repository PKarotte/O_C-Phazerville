// Copyright (c) 2024
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

#define MS_QUANT_SCALES_COUNT 4

class MultiScale : public HemisphereApplet {
public:
    enum Cursor : int8_t {
        // Page 1: Scale Keyboard Editor
        PAGE1_SCALE_SELECT = 0,
        PAGE1_KEY_FIRST = 1,
        PAGE1_KEY_LAST = 12,

        // Page 2: MIDI In Settings & Reset
        PAGE2_MIDI_CH = 13,
        PAGE2_REC_SCALE = 14,
        PAGE2_LIVE = 15,
        PAGE2_RESET_ALL = 16,

        // Page 3: MIDI Chord Out Settings
        PAGE3_MIDI_OUT_CH = 17,
        PAGE3_GATE_LEN = 18,
        PAGE3_GATE_OFS = 19,
        PAGE3_PLAY_MODE = 20,
        PAGE3_OCTAVE = 21,
        PAGE3_INVERSION = 22,

        CURSOR_LAST = PAGE3_INVERSION
    };

    enum PlayMode : uint8_t {
        MODE_CHORD = 0, // all notes at once
        MODE_STRUM = 1, // notes one after another, low to high
        MODE_COUNT = 2
    };

    // Gate lengths in 32nds of a bar. 0 = 1/1 = hold for as long as the scale is active
    static constexpr uint8_t GATE_LEN_COUNT = 6;
    static constexpr uint8_t GATE_LEN_32[GATE_LEN_COUNT] = { 0, 16, 8, 4, 2, 1 };
    static constexpr const char* GATE_LEN_NAME[GATE_LEN_COUNT] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };

    // Gate start offsets in 32nds of a bar
    static constexpr uint8_t GATE_OFS_COUNT = 5;
    static constexpr uint8_t GATE_OFS_32[GATE_OFS_COUNT] = { 0, 2, 4, 8, 16 };
    static constexpr const char* GATE_OFS_NAME[GATE_OFS_COUNT] = { "0", "1/16", "1/8", "1/4", "1/2" };

    static constexpr uint8_t MAX_CHORD_NOTES = 12;
    static constexpr int8_t OCTAVE_MIN = 0;
    static constexpr int8_t OCTAVE_MAX = 8;
    static constexpr int8_t INV_MIN = -6;
    static constexpr int8_t INV_MAX = 6;

    const char* applet_name() {
        return "MultiScale";
    }
    const uint8_t* applet_icon() { return PhzIcons::multiscale; }

    void SetMask(uint16_t &mask) {
        quant.Configure(OC::Scales::GetScale(OC::Scales::SCALE_SEMI), mask);
    }

    void Start() {
        // init all scales with root note
        for (uint8_t i = 0; i < MS_QUANT_SCALES_COUNT; i++) {
            scale_mask[i] = 0x0001;
        }
        quant.Init();
        SetMask(scale_mask[0]);
        rec_scale = 0;
        chord_recording = false;
        chord_active = false;
        prog_started = false;
        last_note_ms = 0;
        chord_start_ms = 0;
        notes_held = 0;
        tr4_active = false;
        midi_activity_timer = 0;
        last_rx_channel = 0;
        cv2_gate = false;
        cv2_was_high = false;
        gate_was_active = false;
        chord_offset_waiting = false;
        chord_gate_active = false;
        chord_hold_full_scale = false;
        strum_active = false;
        active_chord_count = 0;
        for (uint8_t i = 0; i < MAX_CHORD_NOTES; i++) chord_sounding[i] = false;
        // Enable Omni reception so MIDI messages reach applets
        frame.MIDIState.any_channel_omni = true;
    }

    bool CheckNextScaleClock() {
        bool clk = Clock(1);
        if (clk) tr4_active = true;
        // If in Right Hemisphere and TR4 is not physically clocked,
        // normal TR2 so both applets advance together in lockstep
        if (!clk && hemisphere == RIGHT_HEMISPHERE && !tr4_active) {
            clk = frame.clocked[1];
        }
        return clk;
    }

    void Controller() {
        bool is_clock_running = HS::clock_m.IsRunning();

        // Transport Stop check: immediately silence gate & MIDI chord outputs on transport stop
        if (was_clock_running && !is_clock_running) {
            cv2_gate = false;
            cv2_was_high = false;
            gate_was_active = false;
            frame.clock_countdown[io_offset + 1] = 0;
            Out(1, 0);
            SendChordNotesOff(true);
            was_clock_running = is_clock_running;
            return;
        }

        was_clock_running = is_clock_running;

        // Cycle scale sequentially on Digital 2 (or normalled TR2) trigger
        // (Ignored in Live Mode so incoming playing directly controls the harmony)
        if (!live_mode && CheckNextScaleClock()) {
            current_scale = (current_scale + 1) % MS_QUANT_SCALES_COUNT;
            scale_page = current_scale;
            SetMask(scale_mask[current_scale]);
            // Send the new scale as a chord on the MIDI Out channel
            TriggerScaleChord();
        }

        // Update MIDI Chord Out timers (Offset -> Strum / Chord -> Gate Length)
        UpdateChordPlayback();

        // Quantize notes from input 1, clocked by Digital 1 (Clock tempo)
        if (Clock(0)) {
            continuous = false;
            StartADCLag(0);
        }

        // Quantize continuously if no clock arrived or when lag ends
        if (continuous || EndOfADCLag(0)) {
            int32_t quantized = quant.Process(In(0), 0, 0);
            Out(0, quantized);
        }

        // Voice Gate/Trigger pass-through from GateIn (CV 2) to GateOut (Out 2)
        // Works with both triggers and sustained gates (completely independent of Clock)
        bool cv2_high = (In(1) > GATE_THRESHOLD); // > 1.25V
        if (cv2_gate && In(1) < (ONE_OCTAVE * 4 / 5)) { // < ~0.8V hysteresis
            cv2_gate = false;
        } else if (cv2_high) {
            cv2_gate = true;
        }

        bool cv2_trig = (cv2_gate && !cv2_was_high);
        cv2_was_high = cv2_gate;

        // If GateIn receives a trigger / rising edge, fire calibrated ~5ms pulse on Out 2
        if (cv2_trig) {
            ClockOut(1);
        }

        // If GateIn receives a sustained gate, hold Out 2 high for the full gate duration
        if (cv2_gate) {
            Out(1, HEMISPHERE_MAX_CV);
            gate_was_active = true;
        } else {
            if (gate_was_active) {
                if (frame.clock_countdown[io_offset + 1] <= 0) {
                    Out(1, 0);
                    gate_was_active = false;
                }
            } else if (frame.clock_countdown[io_offset + 1] <= 0) {
                Out(1, 0);
            }
        }

        uint32_t now = millis();

        // Watchdog: reset stuck notes_held after 1.2s silence
        if (notes_held > 0 && last_note_ms > 0 && (now - last_note_ms > 1200)) {
            notes_held = 0;
        }

        // Finalize chord when keys are released and pause has elapsed
        if (chord_recording) {
            if (notes_held == 0 && (now - last_note_ms > CHORD_GAP_MS)) {
                chord_recording = false;
                if (!live_mode) {
                    rec_scale = (rec_scale + 1) % MS_QUANT_SCALES_COUNT;
                }
            }
        }

        // Close active chord gathering window when all keys released
        if (chord_active && notes_held == 0 && (now - last_note_ms > 60)) {
            chord_active = false;
        }
    }

    void ToggleBit(const uint8_t bit) {
        scale_mask[scale_page] ^= (0x01 << bit); // toggle bit at position
        if (scale_page == current_scale) {
            SetMask(scale_mask[current_scale]);
            if (chord_gate_active) {
                RefreshActiveChord();
            }
        }
    }

    void ResetAllScales() {
        SendChordNotesOff();
        for (uint8_t i = 0; i < MS_QUANT_SCALES_COUNT; i++) {
            scale_mask[i] = 0;
            scale_mask[i] = 0x0001; // default to root note
        }
        rec_scale = 0;
        chord_recording = false;
        chord_active = false;
        prog_started = false;
        notes_held = 0;
        SetMask(scale_mask[current_scale]);
    }

    // ------------------------------------------------------------------
    // MIDI Chord Out engine
    // ------------------------------------------------------------------
    void SendChordMidi(uint8_t status, uint8_t note, uint8_t vel) {
        if (midi_out_ch == 0 || midi_out_ch > 16) return;
        const uint8_t ch = midi_out_ch;
        if (status == 0x90) HS::frame.MIDIState.SendNoteOn(ch - 1, note, vel);
        else HS::frame.MIDIState.SendNoteOff(ch - 1, note);
        HS::frame.MIDIState.ProcessMIDIMsg({ch, status, note, vel});
        HS::frame.MIDIState.QueueInternalMsg({ch, status, note, vel});
    }

    // Build the voiced chord from the active scale mask, octave, and inversion
    void BuildChord() {
        int notes[MAX_CHORD_NOTES];
        int count = 0;
        const int base = 12 * (chord_octave + 1); // octave 3 -> C3 = MIDI 48
        const uint16_t mask = scale_mask[current_scale];
        for (int s = 0; s < 12; s++) {
            if ((mask >> s) & 0x01) notes[count++] = base + s;
        }

        // Inversions: positive moves the lowest note up an octave, negative moves the highest down
        if (count > 0) {
            for (int i = 0; i < chord_inversion; i++) {
                int low = notes[0] + 12;
                for (int k = 0; k < count - 1; k++) notes[k] = notes[k + 1];
                notes[count - 1] = low;
            }
            for (int i = 0; i > chord_inversion; i--) {
                int high = notes[count - 1] - 12;
                for (int k = count - 1; k > 0; k--) notes[k] = notes[k - 1];
                notes[0] = high;
            }
        }

        active_chord_count = count;
        for (int i = 0; i < count; i++) {
            chord_notes[i] = (uint8_t)constrain(notes[i], 0, 127);
            chord_sounding[i] = false;
        }
    }

    void PlayChordNote(uint8_t i) {
        if (i >= active_chord_count || chord_sounding[i]) return;
        SendChordMidi(0x90, chord_notes[i], CHORD_VELOCITY);
        chord_sounding[i] = true;
    }

    void StartChordNotes() {
        if (chord_play_mode == MODE_CHORD) {
            for (uint8_t i = 0; i < active_chord_count; i++) PlayChordNote(i);
            strum_active = false;
        } else {
            strum_active = true;
            strum_idx = 0;
            next_strum_tick = chord_tick;
        }
    }

    void SendChordNotesOff(bool panic = false) {
        for (uint8_t i = 0; i < active_chord_count; i++) {
            if (chord_sounding[i]) {
                SendChordMidi(0x80, chord_notes[i], 0);
                chord_sounding[i] = false;
            }
        }
        if (panic && midi_out_ch > 0 && midi_out_ch <= 16) {
            HS::frame.MIDIState.SendCC(midi_out_ch - 1, 123, 0);
        }
        chord_gate_active = false;
        chord_offset_waiting = false;
        strum_active = false;
    }

    // Called whenever the active scale changes
    void TriggerScaleChord() {
        SendChordNotesOff();
        if (midi_out_ch == 0) return;

        BuildChord();
        if (active_chord_count == 0) return;

        uint32_t tpb = HS::clock_m.GetTempoTicks(); // ticks per quarter note
        uint32_t unit = tpb / 8;                     // ticks per 1/32 bar (4/4)
        if (unit == 0) unit = 1;

        chord_ofs_ticks = GATE_OFS_32[chord_ofs_idx] * unit;
        chord_hold_full_scale = (GATE_LEN_32[chord_len_idx] == 0);
        chord_off_ticks = chord_ofs_ticks + GATE_LEN_32[chord_len_idx] * unit;
        strum_ticks = tpb / 16; // 1/64 note between strummed notes
        if (strum_ticks == 0) strum_ticks = 1;

        chord_tick = 0;
        chord_offset_waiting = true;
        chord_gate_active = true;
        strum_active = false;
    }

    // Re-voice a currently sounding chord (after editing notes/octave/inversion)
    void RefreshActiveChord() {
        if (!chord_gate_active || chord_offset_waiting) return;
        for (uint8_t i = 0; i < active_chord_count; i++) {
            if (chord_sounding[i]) {
                SendChordMidi(0x80, chord_notes[i], 0);
                chord_sounding[i] = false;
            }
        }
        BuildChord();
        StartChordNotes();
    }

    void UpdateChordPlayback() {
        if (!chord_gate_active) return;
        chord_tick++;

        if (chord_offset_waiting) {
            if (chord_tick < chord_ofs_ticks) return;
            chord_offset_waiting = false;
            StartChordNotes();
        }

        while (strum_active && chord_tick >= next_strum_tick) {
            PlayChordNote(strum_idx);
            strum_idx++;
            next_strum_tick += strum_ticks;
            if (strum_idx >= active_chord_count) strum_active = false;
        }

        if (!chord_hold_full_scale && chord_tick >= chord_off_ticks) {
            SendChordNotesOff();
        }
    }

    void OnMIDIMessage(uint8_t channel, uint8_t message, uint8_t data1, uint8_t data2) override {
        // Record channel of incoming traffic & reset activity timer
        last_rx_channel = channel;
        midi_activity_timer = millis();

        // Filter channel (0 = Omni, 1..16)
        if (midi_channel > 0 && channel > 0 && channel != midi_channel) return;

        uint8_t msg_type = message & 0xF0;

        if (msg_type == 0x90 && data2 > 0) { // NoteOn
            uint32_t now = millis();
            bool was_all_released = (notes_held == 0);
            notes_held++;

            // Silence timeout (8s): restart progression at Scale 1
            if (last_note_ms > 0 && (now - last_note_ms > PROGRESSION_RESET_MS)) {
                rec_scale = 0;
                prog_started = false;
            }

            // A NoteOn begins a NEW chord if:
            // 1. No chord is currently being gathered (!chord_active)
            // 2. OR all keys were previously released and at least 50ms passed since chord started
            // 3. OR more than MIN_CHORD_GAP_MS has passed since chord started (legato transitions)
            bool is_new_chord = false;
            if (!chord_active) {
                is_new_chord = true;
            } else if (now - chord_start_ms > CHORD_GATHER_WINDOW_MS) {
                if (was_all_released || (now - chord_start_ms > MIN_CHORD_GAP_MS)) {
                    is_new_chord = true;
                } else if (now - last_note_ms > CHORD_GAP_MS && (notes_held <= 1 || now - chord_start_ms > CHORD_MAX_WINDOW_MS)) {
                    is_new_chord = true;
                }
            }

            if (is_new_chord) {
                chord_active = true;
                chord_start_ms = now;

                if (live_mode) {
                    // LIVE MODE: Wipe previous chord completely and set first note of new chord
                    scale_mask[current_scale] = (0x01 << (data1 % 12));
                    SetMask(scale_mask[current_scale]);
                    scale_page = current_scale;
                    TriggerScaleChord();
                } else {
                    // PROGRESSION RECORDING MODE:
                    if (prog_started) {
                        rec_scale = (rec_scale + 1) % MS_QUANT_SCALES_COUNT;
                    }
                    prog_started = true;
                    scale_mask[rec_scale] = (0x01 << (data1 % 12));
                    if (rec_scale == current_scale) {
                        SetMask(scale_mask[current_scale]);
                        TriggerScaleChord();
                    }
                    scale_page = rec_scale;
                }
            } else {
                // Subsequent note within active chord gathering window: union into active scale mask
                uint8_t semitone = data1 % 12;
                if (live_mode) {
                    scale_mask[current_scale] |= (0x01 << semitone);
                    SetMask(scale_mask[current_scale]);
                    scale_page = current_scale;
                    if (chord_gate_active) RefreshActiveChord();
                } else {
                    scale_mask[rec_scale] |= (0x01 << semitone);
                    if (rec_scale == current_scale) {
                        SetMask(scale_mask[current_scale]);
                        if (chord_gate_active) RefreshActiveChord();
                    }
                    scale_page = rec_scale;
                }
            }

            last_note_ms = now;
        } 
        else if (msg_type == 0x80 || (msg_type == 0x90 && data2 == 0)) { // NoteOff
            if (notes_held > 0) notes_held--;
            last_note_ms = millis();
        }
    }

    void View() {
        if (cursor < PAGE2_MIDI_CH) {
            DrawPage1();
        } else if (cursor < PAGE3_MIDI_OUT_CH) {
            DrawPage2();
        } else {
            DrawPage3();
        }
    }

    void OnButtonPress() {
        if (cursor == PAGE1_SCALE_SELECT) {
            CursorToggle();
        } else if (cursor >= PAGE1_KEY_FIRST && cursor <= PAGE1_KEY_LAST) {
            const uint8_t bit = cursor - 1;
            ToggleBit(bit);
        } else if (cursor == PAGE2_LIVE) {
            live_mode = !live_mode;
            if (live_mode) SetMask(scale_mask[current_scale]);
        } else if (cursor == PAGE2_RESET_ALL) {
            ResetAllScales();
        } else {
            CursorToggle();
        }
    }

    void OnEncoderMove(int direction) {
        if (!EditMode()) {
            MoveCursor(cursor, direction, CURSOR_LAST);
            return;
        }

        switch (cursor) {
            case PAGE1_SCALE_SELECT:
                scale_page = constrain(scale_page + direction, 0, MS_QUANT_SCALES_COUNT - 1);
                break;
            case PAGE2_MIDI_CH:
                midi_channel = constrain(midi_channel + direction, 0, 16);
                break;
            case PAGE2_REC_SCALE:
                rec_scale = constrain(rec_scale + direction, 0, MS_QUANT_SCALES_COUNT - 1);
                chord_recording = false;
                chord_active = false;
                prog_started = false;
                scale_page = rec_scale;
                break;
            case PAGE2_LIVE:
                if (direction != 0) {
                    live_mode = !live_mode;
                    if (live_mode) SetMask(scale_mask[current_scale]);
                }
                break;
            case PAGE3_MIDI_OUT_CH: {
                uint8_t new_ch = constrain(midi_out_ch + direction, 0, 16);
                if (new_ch != midi_out_ch) {
                    SendChordNotesOff(); // release notes on the old channel
                    midi_out_ch = new_ch;
                }
                break;
            }
            case PAGE3_GATE_LEN:
                chord_len_idx = constrain(chord_len_idx + direction, 0, GATE_LEN_COUNT - 1);
                break;
            case PAGE3_GATE_OFS:
                chord_ofs_idx = constrain(chord_ofs_idx + direction, 0, GATE_OFS_COUNT - 1);
                break;
            case PAGE3_PLAY_MODE:
                chord_play_mode = constrain(chord_play_mode + direction, 0, MODE_COUNT - 1);
                break;
            case PAGE3_OCTAVE:
                chord_octave = constrain(chord_octave + direction, OCTAVE_MIN, OCTAVE_MAX);
                RefreshActiveChord();
                break;
            case PAGE3_INVERSION:
                chord_inversion = constrain(chord_inversion + direction, INV_MIN, INV_MAX);
                RefreshActiveChord();
                break;
            default:
                break;
        }
    }

    void AuxButton() override {
        if (EditMode()) {
            CancelEdit();
            return;
        }
        if (cursor < PAGE2_MIDI_CH) {
            cursor = PAGE2_MIDI_CH;
        } else if (cursor < PAGE3_MIDI_OUT_CH) {
            cursor = PAGE3_MIDI_OUT_CH;
        } else {
            cursor = PAGE1_SCALE_SELECT;
        }
        ResetCursor();
    }

    uint64_t OnDataRequest() {
        uint64_t data = 0;
        Pack(data, PackLocation { 0, 12}, scale_mask[0]);
        Pack(data, PackLocation {12, 12}, scale_mask[1]);
        Pack(data, PackLocation {24, 12}, scale_mask[2]);
        Pack(data, PackLocation {36, 12}, scale_mask[3]);
        
        // Retain exact 5-bit MIDI In channel at {48, 5} for full backwards compatibility
        Pack(data, PackLocation {48, 5}, midi_channel % 17);

        // Pack Chord Out & Live mode into bits 53..63 (11 bits, 0..2047):
        // 17 * 2 * 2 * 6 * 5 = 2040 <= 2047
        uint16_t combo = (midi_out_ch % 17) + 
                         17 * ((live_mode ? 1 : 0) + 
                         2 * ((chord_play_mode % MODE_COUNT) + 
                         MODE_COUNT * ((chord_len_idx % GATE_LEN_COUNT) + 
                         GATE_LEN_COUNT * (chord_ofs_idx % GATE_OFS_COUNT))));
        Pack(data, PackLocation {53, 11}, combo);
        return data;
    }

    void OnDataReceive(uint64_t data) {
        SendChordNotesOff();
        scale_mask[0] = Unpack(data, PackLocation { 0, 12});
        scale_mask[1] = Unpack(data, PackLocation {12, 12});
        scale_mask[2] = Unpack(data, PackLocation {24, 12});
        scale_mask[3] = Unpack(data, PackLocation {36, 12});

        midi_channel = Unpack(data, PackLocation {48, 5});
        if (midi_channel > 16) midi_channel = 0;

        uint16_t combo = Unpack(data, PackLocation {53, 11});
        if (combo < 2040) {
            midi_out_ch = combo % 17;
            combo /= 17;
            live_mode = (combo % 2) != 0;
            combo /= 2;
            chord_play_mode = combo % MODE_COUNT;
            combo /= MODE_COUNT;
            chord_len_idx = combo % GATE_LEN_COUNT;
            combo /= GATE_LEN_COUNT;
            chord_ofs_idx = combo % GATE_OFS_COUNT;
        } else {
            // Flash was erased / all 1s (0x7FF = 2047) or older preset without extra fields
            midi_out_ch = 0;
            live_mode = false;
            chord_play_mode = MODE_CHORD;
            chord_len_idx = 0;
            chord_ofs_idx = 0;
        }

        // Default to root note if scale mask is empty on load
        if (scale_mask[0] == 0 && scale_mask[1] == 0 && scale_mask[2] == 0 && scale_mask[3] == 0) {
            for (uint8_t i = 0; i < MS_QUANT_SCALES_COUNT; i++) scale_mask[i] = 0x0001;
        }
        SetMask(scale_mask[0]);
    }

protected:
    void SetHelp() {
        help[HELP_DIGITAL1] = "Clock";
        help[HELP_DIGITAL2] = "NxtScl";
        help[HELP_CV1]      = "Pitch";
        help[HELP_CV2]      = "GateIn";
        help[HELP_OUT1]     = "Quant";
        help[HELP_OUT2]     = "GateOut";
        help[HELP_EXTRA1]   = "Aux: Page 1/2/3";
        help[HELP_EXTRA2]   = "P2: Live / P3: Chord";
    }

private:
    int cursor = PAGE1_SCALE_SELECT;
    bool continuous = true;
    braids::Quantizer quant;
    uint16_t scale_mask[MS_QUANT_SCALES_COUNT];

    int8_t current_scale = 0;
    int8_t scale_page = 0;
    uint8_t midi_channel = 0; // 0 = Omni, 1..16
    bool live_mode = false;

    // MIDI Chord Out settings
    uint8_t midi_out_ch = 0;     // 0 = Off, 1..16
    uint8_t chord_len_idx = 0;   // index into GATE_LEN_32 (0 = 1/1 hold)
    uint8_t chord_ofs_idx = 0;   // index into GATE_OFS_32
    uint8_t chord_play_mode = MODE_CHORD;
    int8_t  chord_octave = 3;    // 3 -> chord root at C3 (MIDI 48)
    int8_t  chord_inversion = 0; // -6..+6

    // MIDI Chord Out runtime state
    uint8_t  chord_notes[MAX_CHORD_NOTES];
    bool     chord_sounding[MAX_CHORD_NOTES];
    uint8_t  active_chord_count = 0;
    bool     chord_gate_active = false;
    bool     chord_offset_waiting = false;
    bool     chord_hold_full_scale = false;
    bool     strum_active = false;
    uint8_t  strum_idx = 0;
    uint32_t chord_tick = 0;
    uint32_t chord_ofs_ticks = 0;
    uint32_t chord_off_ticks = 0;
    uint32_t next_strum_tick = 0;
    uint32_t strum_ticks = 1;
    static constexpr uint8_t CHORD_VELOCITY = 100;

    // Chord succession recorder
    int8_t   rec_scale = 0;
    bool     chord_recording = false;
    bool     chord_active = false;
    bool     prog_started = false;
    uint32_t last_note_ms = 0;
    uint32_t chord_start_ms = 0;
    uint8_t  notes_held = 0;
    bool     tr4_active = false;
    uint32_t midi_activity_timer = 0;
    bool     cv2_gate = false;
    bool     cv2_was_high = false;
    bool     gate_was_active = false;
    bool     was_clock_running = false;
    uint8_t  last_rx_channel = 0;

    static constexpr uint32_t CHORD_GAP_MS = 320;          // Pause to finalize chord & advance
    static constexpr uint32_t CHORD_MAX_WINDOW_MS = 600;   // Max chord gathering window
    static constexpr uint32_t CHORD_GATHER_WINDOW_MS = 90; // Max note spread within a single chord
    static constexpr uint32_t MIN_CHORD_GAP_MS = 110;      // Beyond 110ms, incoming notes start a new chord
    static constexpr uint32_t PROGRESSION_RESET_MS = 8000; // Silence before restarting at Scale 1

    void DrawPage1() {
        DrawKeyboard();
        DrawIndicators();
        gfxPrint(58, 2, "1");
    }

    void DrawKeyboard() {
        gfxFrame(4, 27, 56, 32);

        // white keys
        for (uint8_t x = 0; x < 7; x++) {
            gfxLine(x * 8 + 4, 27, x * 8 + 4, 58);
        }

        // black keys
        for (uint8_t i = 0; i < 6; i++) {
            if (i != 2) {
                uint8_t x = (i * 8) + 10;
                gfxRect(x, 27, 5, 16);
            }
        }

        // Play scale indicator
        gfxBitmap(1, 14, 8, PLAY_ICON);
        gfxPrint(11, 15, current_scale + 1);

        // Edit/View scale indicator
        gfxBitmap(23, 14, 8, EDIT_ICON);
        gfxPrint(33, 15, scale_page + 1);

        // Rec status
        if (live_mode) {
            gfxPrint(40, 15, "LIVE");
        } else {
            gfxPrint(43, 15, "R");
            if (chord_active) {
                graphics.printf("%d*", rec_scale + 1);
            } else {
                graphics.printf("%d", rec_scale + 1);
            }
        }

        // Blinking indicator in header when MIDI message received
        if (millis() - midi_activity_timer < 200) {
            gfxRect(49, 2, 4, 4);
        }
    }

    void DrawIndicators() {
        if (cursor == PAGE1_SCALE_SELECT) {
            gfxCursor(22, 23, 18);
        }
        uint8_t x[12] = {2, 7, 10, 15, 18, 26, 31, 34, 39, 42, 47, 50};
        uint8_t p[12] = {0, 1,  0,  1,  0,  0,  1,  0,  1,  0,  1,  0};
        for (uint8_t i = 0; i < 12; i++) {
            if ((scale_mask[scale_page] >> i) & 0x01) {
                gfxInvert(x[i] + 4, (p[i] ? 37 : 51), 4 - p[i], 4 - p[i]);
            }
            if (i == (cursor - 1)) {
                gfxCursor(x[i] + 3, p[i] ? 25 : 60, 6);
            }
        }
    }

    void DrawPage2() {
        gfxHeader("MultiScale");
        gfxPrint(58, 2, "2");

        // Row 1: MIDI Channel Setting
        gfxPrint(1, 15, "MIDI Ch:");
        gfxStartCursor(44, 15);
        if (midi_channel == 0) {
            graphics.print("Omni");
        } else {
            graphics.printf("%2d", midi_channel);
        }
        gfxEndCursor(cursor == PAGE2_MIDI_CH);

        // Blinking indicator when MIDI message received
        if (millis() - midi_activity_timer < 200) {
            gfxRect(49, 2, 4, 4);
            gfxRect(58, 16, 4, 4);
        }

        // Row 2: Play scale & Rec scale targets
        gfxBitmap(1, 26, 8, PLAY_ICON);
        gfxPrint(10, 27, "Scl");
        graphics.printf("%d", current_scale + 1);

        gfxPrint(32, 27, "Rec:");
        gfxStartCursor(52, 27);
        if (live_mode) {
            graphics.print("LV");
        } else if (chord_active) {
            graphics.printf("%d*", rec_scale + 1);
        } else {
            graphics.printf("%d", rec_scale + 1);
        }
        gfxEndCursor(cursor == PAGE2_REC_SCALE);

        // Row 3: Visualizer for targeted recording scale
        DrawMiniKeyboard(4, 37, scale_mask[live_mode ? current_scale : rec_scale]);

        // Row 4: Live Mode Toggle & Reset All Scales Button
        gfxPrint(1, 49, "Live:");
        gfxStartCursor(26, 49);
        graphics.print(live_mode ? "ON" : "OFF");
        gfxEndCursor(cursor == PAGE2_LIVE);

        gfxStartCursor(47, 49);
        gfxPrint("CLR");
        gfxEndCursor(cursor == PAGE2_RESET_ALL);
    }

    void DrawPage3() {
        gfxHeader("Chord Out");
        gfxPrint(58, 2, "3");

        // Blinking indicator in header when MIDI message received
        if (millis() - midi_activity_timer < 200) {
            gfxRect(49, 2, 4, 4);
        }

        // Chord playing indicator
        if (chord_gate_active && !chord_offset_waiting) {
            gfxRect(52, 15, 7, 7);
        } else if (chord_gate_active) {
            gfxFrame(52, 15, 7, 7);
        }

        // Row 1: MIDI Out channel
        gfxPrint(1, 15, "Out:");
        gfxStartCursor(26, 15);
        if (midi_out_ch == 0) graphics.print("Off");
        else graphics.printf("%2d", midi_out_ch);
        gfxEndCursor(cursor == PAGE3_MIDI_OUT_CH);

        // Row 2: Gate length (bar divisions; 1/1 = hold while scale is active)
        gfxPrint(1, 25, "Len:");
        gfxStartCursor(26, 25);
        graphics.print(GATE_LEN_NAME[chord_len_idx]);
        gfxEndCursor(cursor == PAGE3_GATE_LEN);

        // Row 3: Gate start offset (bar divisions)
        gfxPrint(1, 35, "Ofs:");
        gfxStartCursor(26, 35);
        graphics.print(GATE_OFS_NAME[chord_ofs_idx]);
        gfxEndCursor(cursor == PAGE3_GATE_OFS);

        // Row 4: Play mode
        gfxPrint(1, 45, "Mode:");
        gfxStartCursor(32, 45);
        graphics.print(chord_play_mode == MODE_CHORD ? "Chord" : "Strum");
        gfxEndCursor(cursor == PAGE3_PLAY_MODE);

        // Row 5: Octave + Inversion
        gfxPrint(1, 55, "O");
        gfxStartCursor(8, 55);
        graphics.printf("%d", chord_octave);
        gfxEndCursor(cursor == PAGE3_OCTAVE);

        gfxPrint(22, 55, "Inv");
        gfxStartCursor(42, 55);
        graphics.printf("%+d", chord_inversion);
        gfxEndCursor(cursor == PAGE3_INVERSION);
    }

    void DrawMiniKeyboard(int x_start, int y_start, uint16_t mask) {
        gfxFrame(x_start, y_start, 56, 10);

        // White key divider lines
        for (uint8_t k = 1; k < 7; k++) {
            gfxLine(x_start + k * 8, y_start, x_start + k * 8, y_start + 9);
        }

        // Black keys
        static const uint8_t black_offsets[5] = {6, 14, 30, 38, 46};
        for (uint8_t b = 0; b < 5; b++) {
            gfxRect(x_start + black_offsets[b], y_start, 5, 5);
        }

        // Active note markers
        static const uint8_t note_x[12] = {2, 7, 10, 15, 18, 26, 31, 34, 39, 42, 47, 50};
        static const uint8_t is_black[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
        for (uint8_t i = 0; i < 12; i++) {
            if ((mask >> i) & 0x01) {
                if (is_black[i]) {
                    gfxInvert(x_start + note_x[i] - 1, y_start + 1, 3, 3);
                } else {
                    gfxInvert(x_start + note_x[i] - 1, y_start + 5, 4, 4);
                }
            }
        }
    }
};
