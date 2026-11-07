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

        // Page 2: MIDI Settings & Reset
        PAGE2_MIDI_CH = 13,
        PAGE2_REC_SCALE = 14,
        PAGE2_RESET_ALL = 15,

        CURSOR_LAST = PAGE2_RESET_ALL
    };

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
        last_note_ms = 0;
        chord_start_ms = 0;
        notes_held = 0;
        tr4_active = false;
        midi_activity_timer = 0;
        cv2_gate = false;
        cv2_was_high = false;
        gate_was_active = false;
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
        // Cycle scale sequentially on Digital 2 (or normalled TR2) trigger
        if (CheckNextScaleClock()) {
            current_scale = (current_scale + 1) % MS_QUANT_SCALES_COUNT;
            scale_page = current_scale;
            SetMask(scale_mask[current_scale]);
            // (Out 2 trigger removed; gate from CV2 routed below)
        }

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

        // Transport Stop check: immediately silence gate output on transport stop
        bool is_clock_running = HS::clock_m.IsRunning();
        if (was_clock_running && !is_clock_running) {
            cv2_gate = false;
            cv2_was_high = false;
            gate_was_active = false;
            frame.clock_countdown[io_offset + 1] = 0;
            Out(1, 0);
            was_clock_running = is_clock_running;
            return;
        }
        was_clock_running = is_clock_running;

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
                rec_scale = (rec_scale + 1) % MS_QUANT_SCALES_COUNT;
            }
        }
    }

    void ToggleBit(const uint8_t bit) {
        scale_mask[scale_page] ^= (0x01 << bit); // toggle bit at position
        if (scale_page == current_scale) {
            SetMask(scale_mask[current_scale]);
        }
    }

    void ResetAllScales() {
        for (uint8_t i = 0; i < MS_QUANT_SCALES_COUNT; i++) {
            scale_mask[i] = 0;
        }
        rec_scale = 0;
        chord_recording = false;
        notes_held = 0;
        SetMask(scale_mask[current_scale]);
    }

    void OnMIDIMessage(uint8_t channel, uint8_t message, uint8_t data1, uint8_t data2) override {
        // Signal MIDI activity regardless of channel filter
        midi_activity_timer = millis();

        // Filter channel (0 = Omni, 1..16)
        if (midi_channel != 0 && channel != 0 && channel != midi_channel) return;

        uint8_t msg_type = message & 0xF0;

        if (msg_type == 0x90 && data2 > 0) { // NoteOn
            uint32_t now = millis();
            notes_held++;

            // If a longer pause passed (e.g. > 8s), restart progression at Scale 1
            if (last_note_ms > 0 && (now - last_note_ms > PROGRESSION_RESET_MS)) {
                rec_scale = 0;
                chord_recording = false;
            }

            // Determine if this note belongs to a NEW chord
            bool is_new_chord = false;
            if (!chord_recording) {
                is_new_chord = true;
            } else if (now - last_note_ms > CHORD_GAP_MS && (notes_held <= 1 || now - chord_start_ms > CHORD_MAX_WINDOW_MS)) {
                // Gap passed while user is striking the next chord
                is_new_chord = true;
                rec_scale = (rec_scale + 1) % MS_QUANT_SCALES_COUNT;
            }

            if (is_new_chord) {
                chord_recording = true;
                chord_start_ms = now;
                // Wipe the scale clean for this new chord
                scale_mask[rec_scale] = 0;
            }

            // Add note to the chord scale (union of all pressed notes in the chord)
            uint8_t semitone = data1 % 12;
            scale_mask[rec_scale] |= (0x01 << semitone);
            last_note_ms = now;

            // Update active scale if recording into current playing scale
            if (rec_scale == current_scale) {
                SetMask(scale_mask[current_scale]);
            }
            scale_page = rec_scale;
        } 
        else if (msg_type == 0x80 || (msg_type == 0x90 && data2 == 0)) { // NoteOff
            if (notes_held > 0) notes_held--;
            last_note_ms = millis();
        }
    }

    void View() {
        if (cursor < PAGE2_MIDI_CH) {
            DrawPage1();
        } else {
            DrawPage2();
        }
    }

    void OnButtonPress() {
        if (cursor == PAGE1_SCALE_SELECT) {
            CursorToggle();
        } else if (cursor >= PAGE1_KEY_FIRST && cursor <= PAGE1_KEY_LAST) {
            const uint8_t bit = cursor - 1;
            ToggleBit(bit);
        } else if (cursor == PAGE2_MIDI_CH || cursor == PAGE2_REC_SCALE) {
            CursorToggle();
        } else if (cursor == PAGE2_RESET_ALL) {
            ResetAllScales();
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
                scale_page = rec_scale;
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
        Pack(data, PackLocation {48,  5}, midi_channel);
        return data;
    }

    void OnDataReceive(uint64_t data) {
        scale_mask[0] = Unpack(data, PackLocation { 0, 12});
        scale_mask[1] = Unpack(data, PackLocation {12, 12});
        scale_mask[2] = Unpack(data, PackLocation {24, 12});
        scale_mask[3] = Unpack(data, PackLocation {36, 12});
        midi_channel  = Unpack(data, PackLocation {48,  5});
        if (midi_channel > 16) midi_channel = 0;
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
        help[HELP_EXTRA1]   = "Aux: Page 1/2";
        help[HELP_EXTRA2]   = "GateIn -> GateOut";
    }

private:
    int cursor = PAGE1_SCALE_SELECT;
    bool continuous = true;
    braids::Quantizer quant;
    uint16_t scale_mask[MS_QUANT_SCALES_COUNT];

    int8_t current_scale = 0;
    int8_t scale_page = 0;
    uint8_t midi_channel = 0; // 0 = Omni, 1..16

    // Chord succession recorder
    int8_t   rec_scale = 0;
    bool     chord_recording = false;
    uint32_t last_note_ms = 0;
    uint32_t chord_start_ms = 0;
    uint8_t  notes_held = 0;
    bool     tr4_active = false;
    uint32_t midi_activity_timer = 0;
    bool     cv2_gate = false;
    bool     cv2_was_high = false;
    bool     gate_was_active = false;
    bool     was_clock_running = false;

    static constexpr uint32_t CHORD_GAP_MS = 320;          // Pause to finalize chord & advance
    static constexpr uint32_t CHORD_MAX_WINDOW_MS = 600;   // Max chord gathering window
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
        gfxPrint(43, 15, "R");
        if (chord_recording) {
            graphics.printf("%d*", rec_scale + 1);
        } else {
            graphics.printf("%d", rec_scale + 1);
        }

        // Blinking dot when MIDI message received
        if (millis() - midi_activity_timer < 250) {
            gfxRect(0, 2, 3, 3);
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
        if (millis() - midi_activity_timer < 250) {
            gfxRect(0, 2, 3, 3);
        }

        // Row 2: Play scale & Rec scale targets
        gfxBitmap(1, 26, 8, PLAY_ICON);
        gfxPrint(10, 27, "Scl");
        graphics.printf("%d", current_scale + 1);

        gfxPrint(32, 27, "Rec:");
        gfxStartCursor(52, 27);
        if (chord_recording) {
            graphics.printf("%d*", rec_scale + 1);
        } else {
            graphics.printf("%d", rec_scale + 1);
        }
        gfxEndCursor(cursor == PAGE2_REC_SCALE);

        // Row 3: Visualizer for targeted recording scale
        DrawMiniKeyboard(4, 37, scale_mask[rec_scale]);

        // Row 4: Reset All Scales Button
        gfxStartCursor(6, 49);
        gfxPrint("[RESET ALL]");
        gfxEndCursor(cursor == PAGE2_RESET_ALL);
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
