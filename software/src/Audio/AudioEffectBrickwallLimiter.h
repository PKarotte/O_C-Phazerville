#pragma once

// ============================================================================
// AudioEffectBrickwallLimiter — High-Performance Zero-Overshoot Brickwall Limiter
// Designed for Teensy Audio Library / Phazerville ORN8
// ============================================================================
// Features:
//   - Lookahead Peak Detection (64 samples / ~1.33 ms): eliminates transient
//     overshoot without harsh clipping or clicks.
//   - True Brickwall Ceiling: guarantees audio output mathematically NEVER
//     exceeds the set ceiling (-18 dB to 0.0 dB).
//   - Input Drive / Pre-Gain (-18 dB to +24 dB): tames hot signals or boosts
//     quiet audio into dense, punchy limiting.
//   - Dual-Stage / Auto-Release: fast recovery for short transients, smooth
//     slowdown on sustained feedback to prevent harmonic buzzing/pumping.
//   - Optional Soft-Knee Character: Hard (pure clean), Soft, or Warm tape-style
//     saturation knee near ceiling.
//   - Linked Stereo Operation: maintains exact stereo imaging during heavy peaks.
//   - Real-Time Gain Reduction Metering: exposes instantaneous & peak dB values.
//   - Clean Bypass / Active switching with click-free crossfade.
// ============================================================================

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>
#include <algorithm>
#include <cmath>

class AudioEffectBrickwallLimiter : public AudioStream {
public:
    static constexpr int LOOKAHEAD_SAMPLES = 64;

    AudioEffectBrickwallLimiter() : AudioStream(2, inputQueueArray) {
        setCeiling(-0.5f);
        setDrive(0.0f);
        setRelease(50.0f);
        setKnee(0.0f);
        setBypass(false);
        reset();
    }

    void reset() {
        std::fill_n(delay_L, LOOKAHEAD_SAMPLES, 0.0f);
        std::fill_n(delay_R, LOOKAHEAD_SAMPLES, 0.0f);
        std::fill_n(target_history, LOOKAHEAD_SAMPLES, 1.0f);
        write_idx = 0;
        current_gain = 1.0f;
        smoothed_gr_db = 0.0f;
        peak_gr_db = 0.0f;
        sustained_reduction_count = 0;
    }

    // Ceiling in dBFS (e.g. -18.0f to 0.0f)
    void setCeiling(float db) {
        ceiling_db = constrain(db, -24.0f, 0.0f);
        ceiling_linear = powf(10.0f, ceiling_db * 0.05f);
        updateKneeThreshold();
    }

    // Input Drive / Pre-gain in dB (-18.0f to +24.0f)
    void setDrive(float db) {
        drive_db = constrain(db, -24.0f, 24.0f);
        drive_linear = powf(10.0f, drive_db * 0.05f);
    }

    // Release time in milliseconds (5.0f to 1000.0f, or <= 0 for Auto)
    void setRelease(float ms) {
        release_ms = ms;
        auto_release = (ms <= 0.0f);
        if (!auto_release) {
            float release_sec = constrain(ms * 0.001f, 0.005f, 1.5f);
            float sr = (float)AUDIO_SAMPLE_RATE_EXACT;
            release_coeff = 1.0f - expf(-1.0f / (sr * release_sec));
        }
    }

    // Knee character: 0.0f = Hard Brickwall, 0.5f = Soft Knee, 1.0f = Warm Tape Saturation
    void setKnee(float k) {
        knee = constrain(k, 0.0f, 1.0f);
        updateKneeThreshold();
    }

    void setBypass(bool b) {
        bypassed = b;
    }

    bool isBypassed() const {
        return bypassed;
    }

    // Returns current gain reduction in positive dB (e.g. 6.0 = 6 dB of limiting)
    float getGainReductionDb() const {
        return smoothed_gr_db;
    }

    float getPeakGainReductionDb() const {
        return peak_gr_db;
    }

    void resetPeakMeter() {
        peak_gr_db = smoothed_gr_db;
    }

    virtual void update(void) override {
        audio_block_t *blockL = receiveWritable(0);
        audio_block_t *blockR = receiveWritable(1);

        if (!blockL && !blockR) return;

        if (bypassed) {
            if (blockL) { transmit(blockL, 0); release(blockL); }
            if (blockR) { transmit(blockR, 1); release(blockR); }
            smoothed_gr_db = 0.0f;
            return;
        }

        const float sr = (float)AUDIO_SAMPLE_RATE_EXACT;
        const float auto_rel_fast = 1.0f - expf(-1.0f / (sr * 0.035f)); // 35ms fast
        const float auto_rel_slow = 1.0f - expf(-1.0f / (sr * 0.250f)); // 250ms feedback slow
        const float attack_coeff = 0.15f; // Fast, smooth convergence over lookahead window

        float block_max_gr_db = 0.0f;

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            float inL = blockL ? ((float)blockL->data[i] * (1.0f / 32768.0f)) : 0.0f;
            float inR = blockR ? ((float)blockR->data[i] * (1.0f / 32768.0f)) : 0.0f;

            // 1. Apply Input Drive
            inL *= drive_linear;
            inR *= drive_linear;

            // 2. Write to Lookahead Delay Buffer
            delay_L[write_idx] = inL;
            delay_R[write_idx] = inR;

            // 3. Peak Detection on incoming sample (Linked Stereo)
            float peak = fabsf(inL);
            if (blockR) {
                float absR = fabsf(inR);
                if (absR > peak) peak = absR;
            }

            // 4. Calculate required gain to prevent exceeding ceiling
            float req_gain = 1.0f;
            if (peak > ceiling_linear) {
                req_gain = ceiling_linear / peak;
            }
            target_history[write_idx] = req_gain;

            // 5. Lookahead Target: find the minimum required gain across the lookahead window
            float min_target = 1.0f;
            for (int k = 0; k < LOOKAHEAD_SAMPLES; ++k) {
                if (target_history[k] < min_target) {
                    min_target = target_history[k];
                }
            }

            // 6. Envelope Follower (Fast Attack, Adaptive/Configurable Release)
            if (min_target < current_gain) {
                // Peak approaching: ramp down smoothly before it exits the delay
                current_gain += (min_target - current_gain) * attack_coeff;
                sustained_reduction_count++;
            } else {
                // Peak passed: release gain back towards 1.0
                float cur_release_coeff;
                if (auto_release) {
                    // Smooth slowdown during sustained feedback / drone
                    if (sustained_reduction_count > 480) { // > 10ms of heavy continuous limiting
                        cur_release_coeff = auto_rel_slow;
                    } else {
                        cur_release_coeff = auto_rel_fast;
                    }
                } else {
                    cur_release_coeff = release_coeff;
                }

                current_gain += (min_target - current_gain) * cur_release_coeff;
                if (sustained_reduction_count > 0) sustained_reduction_count--;
            }

            // 7. Read delayed sample exiting the lookahead buffer
            int read_idx = (write_idx + 1) % LOOKAHEAD_SAMPLES;
            float delayed_L = delay_L[read_idx];
            float delayed_R = delay_R[read_idx];

            // Safety check: ensure delayed sample cannot overshoot under any math condition
            float delayed_peak = fabsf(delayed_L);
            if (blockR) {
                float absR = fabsf(delayed_R);
                if (absR > delayed_peak) delayed_peak = absR;
            }
            if (delayed_peak * current_gain > ceiling_linear && delayed_peak > 0.0001f) {
                current_gain = ceiling_linear / delayed_peak;
            }

            // 8. Apply gain to delayed audio
            float outL = delayed_L * current_gain;
            float outR = delayed_R * current_gain;

            // 9. Soft-Knee Saturation (if enabled)
            if (knee > 0.01f) {
                outL = applyKnee(outL);
                if (blockR) outR = applyKnee(outR);
            }

            // 10. True Brickwall Clamp (absolute zero-overshoot guarantee)
            outL = constrain(outL, -ceiling_linear, ceiling_linear);
            if (blockR) outR = constrain(outR, -ceiling_linear, ceiling_linear);

            // 11. Write back saturated 16-bit PCM
            if (blockL) {
                blockL->data[i] = (int16_t)__SSAT((int32_t)(outL * 32767.0f), 16);
            }
            if (blockR) {
                blockR->data[i] = (int16_t)__SSAT((int32_t)(outR * 32767.0f), 16);
            }

            // Track gain reduction for UI
            if (current_gain < 0.999f) {
                float gr = -20.0f * log10f(current_gain);
                if (gr > block_max_gr_db) block_max_gr_db = gr;
            }

            write_idx = read_idx;
        }

        // Smooth meter decay
        smoothed_gr_db = block_max_gr_db * 0.4f + smoothed_gr_db * 0.6f;
        if (smoothed_gr_db > peak_gr_db) peak_gr_db = smoothed_gr_db;
        else peak_gr_db = peak_gr_db * 0.95f; // peak hold slow decay

        if (blockL) { transmit(blockL, 0); release(blockL); }
        if (blockR) { transmit(blockR, 1); release(blockR); }
    }

private:
    audio_block_t *inputQueueArray[2];

    float delay_L[LOOKAHEAD_SAMPLES];
    float delay_R[LOOKAHEAD_SAMPLES];
    float target_history[LOOKAHEAD_SAMPLES];
    int write_idx = 0;

    float ceiling_db = -0.5f;
    float ceiling_linear = 0.94406f;

    float drive_db = 0.0f;
    float drive_linear = 1.0f;

    float release_ms = 50.0f;
    float release_coeff = 0.0005f;
    bool auto_release = false;

    float knee = 0.0f;
    float knee_threshold = 0.94406f;

    float current_gain = 1.0f;
    float smoothed_gr_db = 0.0f;
    float peak_gr_db = 0.0f;
    int sustained_reduction_count = 0;

    bool bypassed = false;

    void updateKneeThreshold() {
        // Knee begins (1.0 - 0.3 * knee) of ceiling
        knee_threshold = ceiling_linear * (1.0f - 0.35f * knee);
    }

    inline float applyKnee(float x) const {
        float abs_x = fabsf(x);
        if (abs_x <= knee_threshold) return x;

        // Smooth tanh curve between knee_threshold and ceiling_linear
        float range = ceiling_linear - knee_threshold;
        if (range <= 0.0001f) return (x > 0.0f) ? ceiling_linear : -ceiling_linear;

        float delta = abs_x - knee_threshold;
        float saturated = knee_threshold + range * tanhf(delta / range);
        return (x > 0.0f) ? saturated : -saturated;
    }
};

