#pragma once

// ============================================================================
// LimiterApplet — Zero-Overshoot Brickwall Limiter & Feedback Protector
// (Phazerville ORN8 / Teensy Audio)
// ============================================================================
//
// Dual-Mode Architecture:
//    MONO   -> Single hemisphere: Audio In -> Audio Out (Out 1), GR CV (Out 2)
//    STEREO -> Dual hemisphere: Audio L/R In -> Audio L/R Out (Linked Stereo)
//
// Features:
//   - Lookahead Peak Limiting (64 samples / ~1.33 ms lookahead)
//   - Strict Brickwall Ceiling: -18 dB to 0.0 dB
//   - Input Drive: -18 dB to +24 dB with CV modulation
//   - Dual-Stage / Auto-Release for anti-feedback suppression
//   - Real-Time Gain Reduction (GR) Meter with peak hold
//   - Optional Soft-Knee / Warm Analog Saturation
//   - Instant Bypass toggle via Aux button or UI
//   - Out 2 Gain Reduction CV output in Mono mode for sidechain ducking
// ============================================================================

#include "../Audio/AudioEffectBrickwallLimiter.h"

static constexpr float limiter_ceiling_values[] = {
    0.0f, -0.2f, -0.5f, -1.0f, -2.0f, -3.0f, -4.5f, -6.0f, -9.0f, -12.0f, -18.0f
};
static constexpr int NUM_LIMITER_CEILINGS = sizeof(limiter_ceiling_values) / sizeof(limiter_ceiling_values[0]);

static constexpr float limiter_release_values[] = {
    10.0f, 25.0f, 50.0f, 100.0f, 200.0f, 400.0f, 800.0f, 0.0f // 0 = Auto
};
static const char* const limiter_release_names[] = {
    " 10ms", " 25ms", " 50ms", "100ms", "200ms", "400ms", "800ms", "Auto "
};
static constexpr int NUM_LIMITER_RELEASES = sizeof(limiter_release_values) / sizeof(limiter_release_values[0]);

static const char* const limiter_knee_names[] = {
    "Hard", "Soft", "Warm"
};

template <AudioChannels Channels>
class LimiterApplet : public HemisphereAudioApplet {
public:
    const char* applet_name() {
        return "Limiter";
    }

    void Start() override {
        limiter.reset();
        UpdateLimiterParams();

        if (Channels == MONO) {
            PatchCable(input, 0, limiter, 0);
            PatchCable(limiter, 0, output, 0);
        } else {
            PatchCable(input, 0, limiter, 0);
            PatchCable(input, 1, limiter, 1);
            PatchCable(limiter, 0, output, 0);
            PatchCable(limiter, 1, output, 1);
        }
        alloc_ok = true;
    }

    void Unload() override {
        Disconnect();
        alloc_ok = false;
        AllowRestart();
    }

    void Controller() override {
        if (!alloc_ok) return;

        // Apply CV Modulations
        float eff_ceil_db = limiter_ceiling_values[ceiling_idx];
        if (ceil_cv.enabled()) {
            eff_ceil_db = constrain(eff_ceil_db + ceil_cv.InF(12.0f), -24.0f, 0.0f);
        }

        float eff_drive_db = (float)drive_db;
        if (drive_cv.enabled()) {
            eff_drive_db = constrain(eff_drive_db + drive_cv.InF(18.0f), -24.0f, 24.0f);
        }

        float eff_rel_ms = limiter_release_values[release_idx];
        if (rel_cv.enabled() && eff_rel_ms > 0.0f) {
            eff_rel_ms = constrain(eff_rel_ms * (1.0f + rel_cv.InF(1.0f)), 5.0f, 1000.0f);
        }

        limiter.setCeiling(eff_ceil_db);
        limiter.setDrive(eff_drive_db);
        limiter.setRelease(eff_rel_ms);
        limiter.setKnee(knee_idx == 1 ? 0.5f : (knee_idx == 2 ? 1.0f : 0.0f));
        limiter.setBypass(bypassed);

        // In MONO mode, output Gain Reduction CV on Out 2 (0..5V)
        if (Channels == MONO) {
            float gr = limiter.getGainReductionDb();
            // Scale 0..18 dB GR to 0..HEMISPHERE_MAX_CV
            int cv_val = Proportion((int)(gr * 10.0f), 180, HEMISPHERE_MAX_CV);
            Out(1, constrain(cv_val, 0, HEMISPHERE_MAX_CV));
        }
    }

    void View() override {
        if (!alloc_ok) {
            gfxPrint(1, 15, "Out of RAM!");
            return;
        }

        // Header
        if (bypassed) {
            gfxHeader("*BYPASS*");
        } else {
            gfxHeader("Limiter");
        }

        float gr = limiter.getGainReductionDb();
        float peak_gr = limiter.getPeakGainReductionDb();

        // Limiting activity indicator dot in header (x = 55, y = 2)
        if (gr > 0.3f && !bypassed) {
            gfxPixel(55, 2);
            gfxPixel(56, 2);
            gfxPixel(55, 3);
            gfxPixel(56, 3);
        }

        // ── Row 1: Gain Reduction (GR) Meter (y = 13..21) ────────────────────
        gfxPrint(1, 13, "GR:");
        gfxPos(19, 13);
        if (bypassed) {
            graphics.print("  OFF");
        } else if (gr < 0.1f) {
            graphics.print(" 0.0dB");
        } else {
            graphics.printf("-%2.1fdB", gr);
        }

        // Horizontal Gain Reduction Bar (fills Right-to-Left, 0 to 18 dB)
        gfxFrame(1, 21, 62, 3);
        if (!bypassed && gr > 0.1f) {
            int bar_len = constrain((int)((gr * 60.0f) / 18.0f), 1, 60);
            // Draw filled bar from right edge (x = 61) towards left
            int start_x = 61 - bar_len;
            graphics.drawHLine(start_x, 22, bar_len);

            // Peak tick
            if (peak_gr > 0.2f) {
                int peak_x = 61 - constrain((int)((peak_gr * 60.0f) / 18.0f), 1, 60);
                gfxPixel(peak_x, 20);
            }
        }

        // ── Row 2: Ceiling (y = 26) ──────────────────────────────────────────
        gfxPrint(1, 26, "Clg:");
        gfxStartCursor(23, 26);
        float c = limiter_ceiling_values[ceiling_idx];
        if (c == 0.0f) graphics.print(" 0.0dB");
        else graphics.printf("%4.1fdB", c);
        gfxEndCursor(cursor == CURSOR_CEILING);

        gfxStartCursor(53, 26);
        gfxPrint(ceil_cv);
        gfxEndCursor(cursor == CURSOR_CEIL_CV, false, ceil_cv.InputName());

        // ── Row 3: Drive / Pre-Gain (y = 36) ─────────────────────────────────
        gfxPrint(1, 36, "Drv:");
        gfxStartCursor(23, 36);
        if (drive_db >= 0) graphics.printf("+%2ddB", drive_db);
        else graphics.printf("%3ddB", drive_db);
        gfxEndCursor(cursor == CURSOR_DRIVE);

        gfxStartCursor(53, 36);
        gfxPrint(drive_cv);
        gfxEndCursor(cursor == CURSOR_DRIVE_CV, false, drive_cv.InputName());

        // ── Row 4: Release Time (y = 46) ─────────────────────────────────────
        gfxPrint(1, 46, "Rel:");
        gfxStartCursor(23, 46);
        graphics.print(limiter_release_names[release_idx]);
        gfxEndCursor(cursor == CURSOR_RELEASE);

        gfxStartCursor(53, 46);
        gfxPrint(rel_cv);
        gfxEndCursor(cursor == CURSOR_REL_CV, false, rel_cv.InputName());

        // ── Row 5: Knee Character & Bypass Button (y = 56) ───────────────────
        gfxPrint(1, 56, "Kne:");
        gfxStartCursor(23, 56);
        graphics.print(limiter_knee_names[knee_idx]);
        gfxEndCursor(cursor == CURSOR_KNEE);

        gfxStartCursor(47, 56);
        if (bypassed) graphics.print("[BYP]");
        else graphics.print("[ACT]");
        gfxEndCursor(cursor == CURSOR_BYPASS);

        gfxDisplayInputMapEditor();
    }

    // Aux Button: One-touch instant Bypass toggle (perfect for killswitch on feedback!)
    void AuxButton() override {
        bypassed = !bypassed;
        limiter.setBypass(bypassed);
        CancelEdit();
    }

    void OnButtonPress() override {
        if (CheckEditInputMapPress(
            cursor,
            IndexedInput(CURSOR_CEIL_CV, ceil_cv),
            IndexedInput(CURSOR_DRIVE_CV, drive_cv),
            IndexedInput(CURSOR_REL_CV, rel_cv)
        )) return;

        if (cursor == CURSOR_BYPASS) {
            bypassed = !bypassed;
            limiter.setBypass(bypassed);
        } else {
            CursorToggle();
        }
    }

    void OnEncoderMove(int direction) override {
        if (!EditMode()) {
            MoveCursor(cursor, direction, CURSOR_COUNT - 1);
            return;
        }

        if (EditSelectedInputMap(direction)) return;

        switch (cursor) {
            case CURSOR_CEILING:
                // Clockwise decreases ceiling / increases limiting threshold
                ceiling_idx = constrain(ceiling_idx + direction, 0, NUM_LIMITER_CEILINGS - 1);
                break;
            case CURSOR_CEIL_CV:
                ceil_cv.ChangeSource(direction);
                break;
            case CURSOR_DRIVE:
                drive_db = constrain(drive_db + direction, -18, 24);
                break;
            case CURSOR_DRIVE_CV:
                drive_cv.ChangeSource(direction);
                break;
            case CURSOR_RELEASE:
                release_idx = constrain(release_idx + direction, 0, NUM_LIMITER_RELEASES - 1);
                break;
            case CURSOR_REL_CV:
                rel_cv.ChangeSource(direction);
                break;
            case CURSOR_KNEE:
                knee_idx = constrain(knee_idx + direction, 0, 2);
                break;
            case CURSOR_BYPASS:
                bypassed = !bypassed;
                limiter.setBypass(bypassed);
                CancelEdit();
                break;
            default:
                break;
        }

        UpdateLimiterParams();
        ResetCursor();
    }

    void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
        uint8_t raw_drv = (uint8_t)(drive_db + 24);
        uint8_t raw_byp = bypassed ? 1 : 0;
        data[0] = PackPackables(
            pack<4>(ceiling_idx),
            pack<6>(raw_drv),
            pack<4>(release_idx),
            pack<2>(knee_idx),
            pack<1>(raw_byp)
        );
        data[1] = PackPackables(ceil_cv, drive_cv);
        data[2] = PackPackables(rel_cv);
        data[3] = 0;
    }

    void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
        uint8_t raw_drv = 24;
        uint8_t raw_byp = 0;
        UnpackPackables(
            data[0],
            pack<4>(ceiling_idx),
            pack<6>(raw_drv),
            pack<4>(release_idx),
            pack<2>(knee_idx),
            pack<1>(raw_byp)
        );
        UnpackPackables(data[1], ceil_cv, drive_cv);
        UnpackPackables(data[2], rel_cv);

        ceiling_idx = constrain(ceiling_idx, 0, NUM_LIMITER_CEILINGS - 1);
        drive_db = (int8_t)raw_drv - 24;
        drive_db = constrain((int)drive_db, -18, 24);
        release_idx = constrain(release_idx, 0, NUM_LIMITER_RELEASES - 1);
        knee_idx = constrain(knee_idx, 0, 2);
        bypassed = (raw_byp != 0);

        UpdateLimiterParams();
    }

    AudioStream* InputStream() override {
        return &input;
    }

    AudioStream* OutputStream() override {
        return &output;
    }

protected:
    void SetHelp() override {
        help[HELP_DIGITAL1] = (Channels == STEREO) ? "Audio L" : "Audio In";
        help[HELP_DIGITAL2] = (Channels == STEREO) ? "Audio R" : "Aux / CV";
        help[HELP_OUT1]     = (Channels == STEREO) ? "Lim L"   : "Lim Out";
        help[HELP_OUT2]     = (Channels == STEREO) ? "Lim R"   : "GR CV";
        help[HELP_EXTRA1]   = "Brickwall Limiter";
        help[HELP_EXTRA2]   = "Aux: Bypass";
    }

private:
    enum Cursor {
        CURSOR_CEILING = 0,
        CURSOR_CEIL_CV,
        CURSOR_DRIVE,
        CURSOR_DRIVE_CV,
        CURSOR_RELEASE,
        CURSOR_REL_CV,
        CURSOR_KNEE,
        CURSOR_BYPASS,
        CURSOR_COUNT
    };

    int8_t cursor = 0;
    int8_t ceiling_idx = 2; // Default: -0.5 dB
    int8_t drive_db = 0;    // Default: 0 dB
    int8_t release_idx = 2; // Default: 50 ms
    int8_t knee_idx = 0;    // Default: Hard brickwall
    bool bypassed = false;
    bool alloc_ok = false;

    CVInputMap ceil_cv;
    CVInputMap drive_cv;
    CVInputMap rel_cv;

    AudioPassthrough<Channels> input;
    AudioEffectBrickwallLimiter limiter;
    AudioPassthrough<Channels> output;

    void UpdateLimiterParams() {
        limiter.setCeiling(limiter_ceiling_values[ceiling_idx]);
        limiter.setDrive((float)drive_db);
        limiter.setRelease(limiter_release_values[release_idx]);
        limiter.setKnee(knee_idx == 1 ? 0.5f : (knee_idx == 2 ? 1.0f : 0.0f));
        limiter.setBypass(bypassed);
    }
};
