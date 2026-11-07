#pragma once

// ============================================================================
// CloudverbApplet — Mutable Instruments Clouds Algorithmic Stereo Reverb
// ============================================================================
//
// A lush Dattorro-topology stereo reverberator ported directly from the
// open-source firmware of Mutable Instruments Clouds (by Emilie Gillet).
//
// Features:
//   - 4-stage input diffuser with LFO modulation
//   - Dual cross-coupled allpass + long delay tank with slow shimmer modulation
//   - Decay time control (0% snappy room to 99% cathedral, 100% / Freeze = INF)
//   - High-frequency damping filter (dark/warm to bright shimmer)
//   - Diffusion coefficient (tap delay to seamless cloud)
//   - Input gain / drive control
//   - Equal-power dry/wet crossfade
//   - AuxButton: Instant Freeze toggle for infinite ambient hold
//   - Operates in Mono (single hemisphere) or Stereo (both hemispheres)
//
// Controls:
//   Row 1: Dec (Decay Time) 0..100% / INF  + CV
//   Row 2: Dmp (Damping)    0..100%        + CV
//   Row 3: Dif (Diffusion)  0..100%        + CV
//   Row 4: Gan (Input Gain) 0..100%        + CV
//   Row 5: Mix (Dry/Wet)    0..100%        + CV
//   Aux Button: Toggle Freeze
// ============================================================================

#include "../Audio/AudioEffectCloudsReverb.h"

template <AudioChannels Channels>
class CloudverbApplet : public HemisphereAudioApplet {
public:
    const char* applet_name() {
        return "Cloudverb";
    }

    void Start() override {
        alloc_ok = reverb.begin();
        reverb.setMono(Channels == MONO);
        if (Channels == MONO) {
            PatchCable(input, 0, reverb, 0);
            PatchCable(reverb, 0, output, 0);
        } else {
            PatchCable(input, 0, reverb, 0);
            PatchCable(input, 1, reverb, 1);
            PatchCable(reverb, 0, output, 0);
            PatchCable(reverb, 1, output, 1);
        }
    }

    void Unload() override {
        reverb.end();
        AllowRestart();
    }

    void Controller() override {
        if (!alloc_ok) return;

        // Effective Decay Time: 0..100% -> feedback 0.30f to 0.995f, or 1.0f at INF
        float eff_time = constrain(0.01f * time + time_cv.InF(), 0.0f, 1.0f);
        float krt = (eff_time >= 0.99f || frozen) ? 1.0f : (0.30f + 0.68f * eff_time);

        // Effective Damping: 0% = bright (lp = 0.95), 100% = dark (lp = 0.15)
        float eff_damp = constrain(0.01f * damp + damp_cv.InF(), 0.0f, 1.0f);
        float klp = 0.95f - 0.80f * eff_damp;

        // Effective Diffusion: 0% = 0.10, 100% = 0.90 (default 65% ~ 0.62)
        float eff_diff = constrain(0.01f * diff + diff_cv.InF(), 0.0f, 1.0f);
        float kap = 0.10f + 0.80f * eff_diff;

        // Effective Input Gain: 0..100% -> 0.0 to 1.5 (default 50% = 0.5f)
        float eff_gain = constrain(0.01f * gain + gain_cv.InF(), 0.0f, 1.5f);

        // Equal-Power Dry/Wet Mix
        float eff_mix = constrain(0.01f * mix + mix_cv.InF(), 0.0f, 1.0f);
        float dry_gain, wet_gain;
        EqualPowerFade(dry_gain, wet_gain, eff_mix);

        reverb.setDecay(krt);
        reverb.setDamping(klp);
        reverb.setDiffusion(kap);
        reverb.setInputGain(eff_gain);
        reverb.setFreeze(frozen);
        reverb.setGains(dry_gain, wet_gain);
    }

    void View() override {
        if (!alloc_ok) {
            gfxPrint(1, 15, "Out of RAM!");
            return;
        }

        if (frozen) {
            gfxHeader("*FREEZE*");
        } else {
            gfxHeader("Cloudverb");
        }

        // Row 1: Decay Time (0..100%, 100% = INF)
        gfxPrint(1, 15, "Dec:");
        gfxStartCursor(25, 15);
        if (time >= 100) graphics.print("INF");
        else graphics.printf("%3d%%", time);
        gfxEndCursor(cursor == CURSOR_TIME);

        gfxStartCursor(53, 15);
        gfxPrint(time_cv);
        gfxEndCursor(cursor == CURSOR_TIME_CV, false, time_cv.InputName());

        // Row 2: Damping (0..100%)
        gfxPrint(1, 25, "Dmp:");
        gfxStartCursor(25, 25);
        graphics.printf("%3d%%", damp);
        gfxEndCursor(cursor == CURSOR_DAMP);

        gfxStartCursor(53, 25);
        gfxPrint(damp_cv);
        gfxEndCursor(cursor == CURSOR_DAMP_CV, false, damp_cv.InputName());

        // Row 3: Diffusion (0..100%)
        gfxPrint(1, 35, "Dif:");
        gfxStartCursor(25, 35);
        graphics.printf("%3d%%", diff);
        gfxEndCursor(cursor == CURSOR_DIFF);

        gfxStartCursor(53, 35);
        gfxPrint(diff_cv);
        gfxEndCursor(cursor == CURSOR_DIFF_CV, false, diff_cv.InputName());

        // Row 4: Gain / Drive (0..100%)
        gfxPrint(1, 45, "Gan:");
        gfxStartCursor(25, 45);
        graphics.printf("%3d%%", gain);
        gfxEndCursor(cursor == CURSOR_GAIN);

        gfxStartCursor(53, 45);
        gfxPrint(gain_cv);
        gfxEndCursor(cursor == CURSOR_GAIN_CV, false, gain_cv.InputName());

        // Row 5: Mix (0..100%)
        gfxPrint(1, 55, "Mix:");
        gfxStartCursor(25, 55);
        graphics.printf("%3d%%", mix);
        gfxEndCursor(cursor == CURSOR_MIX);

        gfxStartCursor(53, 55);
        gfxPrint(mix_cv);
        gfxEndCursor(cursor == CURSOR_MIX_CV, false, mix_cv.InputName());

        gfxDisplayInputMapEditor();
    }

    void AuxButton() override {
        frozen = !frozen;
    }

    void OnButtonPress() override {
        if (CheckEditInputMapPress(
            cursor,
            IndexedInput(CURSOR_TIME_CV, time_cv),
            IndexedInput(CURSOR_DAMP_CV, damp_cv),
            IndexedInput(CURSOR_DIFF_CV, diff_cv),
            IndexedInput(CURSOR_GAIN_CV, gain_cv),
            IndexedInput(CURSOR_MIX_CV,  mix_cv)
        )) return;

        CursorToggle();
    }

    void OnEncoderMove(int direction) override {
        if (!EditMode()) {
            MoveCursor(cursor, direction, CURSOR_COUNT - 1);
            return;
        }

        if (EditSelectedInputMap(direction)) return;

        switch (cursor) {
            case CURSOR_TIME:
                time = constrain(time + direction, 0, 100);
                break;
            case CURSOR_TIME_CV:
                time_cv.ChangeSource(direction);
                break;
            case CURSOR_DAMP:
                damp = constrain(damp + direction, 0, 100);
                break;
            case CURSOR_DAMP_CV:
                damp_cv.ChangeSource(direction);
                break;
            case CURSOR_DIFF:
                diff = constrain(diff + direction, 0, 100);
                break;
            case CURSOR_DIFF_CV:
                diff_cv.ChangeSource(direction);
                break;
            case CURSOR_GAIN:
                gain = constrain(gain + direction, 0, 100);
                break;
            case CURSOR_GAIN_CV:
                gain_cv.ChangeSource(direction);
                break;
            case CURSOR_MIX:
                mix = constrain(mix + direction, 0, 100);
                break;
            case CURSOR_MIX_CV:
                mix_cv.ChangeSource(direction);
                break;
            default:
                break;
        }
    }

    void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
        data[0] = PackPackables(
            pack<8>(time),
            pack<8>(damp),
            pack<8>(diff),
            pack<8>(gain),
            pack<8>(mix),
            pack<1>(frozen)
        );
        data[1] = PackPackables(time_cv, damp_cv, diff_cv, gain_cv);
        data[2] = PackPackables(mix_cv);
    }

    void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
        UnpackPackables(
            data[0],
            pack<8>(time),
            pack<8>(damp),
            pack<8>(diff),
            pack<8>(gain),
            pack<8>(mix),
            pack<1>(frozen)
        );
        UnpackPackables(data[1], time_cv, damp_cv, diff_cv, gain_cv);
        UnpackPackables(data[2], mix_cv);
    }

    AudioStream* InputStream() override {
        return &input;
    }

    AudioStream* OutputStream() override {
        return &output;
    }

protected:
    void SetHelp() override {}

private:
    enum Cursor {
        CURSOR_TIME,
        CURSOR_TIME_CV,
        CURSOR_DAMP,
        CURSOR_DAMP_CV,
        CURSOR_DIFF,
        CURSOR_DIFF_CV,
        CURSOR_GAIN,
        CURSOR_GAIN_CV,
        CURSOR_MIX,
        CURSOR_MIX_CV,
        CURSOR_COUNT
    };

    int8_t cursor = 0;
    uint8_t time = 70;
    uint8_t damp = 40;
    uint8_t diff = 65;
    uint8_t gain = 50;
    uint8_t mix = 40;
    bool frozen = false;
    bool alloc_ok = false;

    CVInputMap time_cv;
    CVInputMap damp_cv;
    CVInputMap diff_cv;
    CVInputMap gain_cv;
    CVInputMap mix_cv;

    AudioPassthrough<Channels> input;
    AudioEffectCloudsReverb reverb;
    AudioPassthrough<Channels> output;
};
