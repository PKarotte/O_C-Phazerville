#pragma once

// ============================================================================
// VoiceApplet: Complete Multi-Engine Synthesizer (Ornament & Crime / Phazerville)
// ============================================================================
//
// Dual-Mono Independent Architecture:
//    Left side  (hemisphere 0) -> Audio L (DAC 1), Pitch CV 1, Gate Digital 1
//    Right side (hemisphere 1) -> Audio R (DAC 2), Pitch CV 3, Gate Digital 3
//
// Features:
//  - DX7 6-Operator FM as Engine 0 (32 genuine Yamaha ROM1A patches)
//  - 1:1 Karplus-Strong string physical model from synth_advanced_karplus
//  - 16-Bank 2D morphing wavetable synthesizer
//  - Curated high-impact Braids macro oscillators & physical models
//  - Anti-freeze audio interrupt design with strict 1-voice mono processing
//  - Dual Envelope: Attack + Decay/Release (snap -> tail -> INF drone)
//  - Polyphonic MIDI: Note-On/Off, Velocity, Pitch Bend, Omni/Channel selection
//  - Line-Level & Modular Headroom: -48 dB to +18 dB (0 dB = ~1.4Vpp line level)
//  - Pitch-safe 48 kHz tuning + Fine Hz frequency adjustment
//  - Preset Seed (0..999) + [RND] button + Aux button + CV morph scrub
// ============================================================================

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>
#include "../braids/macro_oscillator.h"
#include "../HSUtils.h"
#include "../HemisphereAudioApplet.h"
#include "../dsputils_arm.h"
#include "../CVInputMap.h"
#include "dx7_data.h"

static const char* const voice_engine_names[] = {
    // 0: DX7 6-Op FM (first engine)
    "DX7",     // 0: Authentic Yamaha DX7 6-Op FM (32 ROM1A patches)

    // 1: 1:1 Karplus-Strong Physical String Model
    "KRPS+",   // 1: Exact port of synth_advanced_karplus (Brightness, Body, Pitch-compensated Decay)

    // 2: 16-Bank Morphing Wavetables
    "WAVTBL",  // 2: 16-wave continuous 2D morphing wavetable synth

    // 3: TB-303 Acid Bassline
    "ACID",    // 3: TB-303 Diode ladder with screaming resonance & drive

    // 4..10: Multi-Oscillator & Classic Analog Synthesizers
    "CSAW",    // 4: Supersaw with multiplier (7 detuned saws)
    "MORPH",   // 5: Continuous triangle / saw / square morph
    "SAW_SQ",  // 6: Dual saw & square waves
    "FOLD",    // 7: Multi-stage diode wavefolder
    "SYNC",    // 8: Hard-synced oscillator
    "TWIN_S",  // 9: Twin detuned saw waves (unison beat)
    "DBL_SQ",  // 10: Dual detuned square waves

    // 11..13: Physical Models & Acoustic Resonators
    "SWARM",   // 11: 6-oscillator supersaw swarm with stereo spread
    "EPNO",    // 12: Physical Electric Piano (Rhodes/Wurli tine & reed)
    "MODAL",   // 13: 4-mode modal resonator (material damping)

    // 14..23: FM, Formants & Expressive Oscillators
    "FM",      // 14: 2-operator phase modulation
    "FDBKFM",  // 15: Feedback FM
    "CHAOS",   // 16: Chaotic attractor dynamics
    "BELL",    // 17: Struck metallic bell physical model
    "VOWEL",   // 18: Formant filter vocal vowel
    "VWL_FO",  // 19: FOF vocal choir generator
    "COMB",    // 20: Tuned comb-filtered resonator
    "TWIN_P",  // 21: Twin peak resonant bandpass
    "PLUCK",   // 22: Plucked string model
    "BLOWN"    // 23: Blown pipe physical model
};

static constexpr int NUM_VOICE_ENGINES = sizeof(voice_engine_names) / sizeof(voice_engine_names[0]);
static constexpr int NUM_VOICE_POLY = 4;
static constexpr int STRING_DELAY_SIZE = 1024;

static inline int get_braids_shape(int eng) {
    switch (eng) {
        case 4:  return braids::MACRO_OSC_SHAPE_CSAW;
        case 5:  return braids::MACRO_OSC_SHAPE_MORPH;
        case 6:  return braids::MACRO_OSC_SHAPE_SAW_SQUARE;
        case 7:  return braids::MACRO_OSC_SHAPE_SINE_TRIANGLE;
        case 8:  return braids::MACRO_OSC_SHAPE_SQUARE_SYNC;
        case 9:  return braids::MACRO_OSC_SHAPE_TRIPLE_SAW;
        case 10: return braids::MACRO_OSC_SHAPE_TRIPLE_SQUARE;
        case 14: return braids::MACRO_OSC_SHAPE_FM;
        case 15: return braids::MACRO_OSC_SHAPE_FEEDBACK_FM;
        case 16: return braids::MACRO_OSC_SHAPE_CHAOTIC_FEEDBACK_FM;
        case 17: return braids::MACRO_OSC_SHAPE_STRUCK_BELL;
        case 18: return braids::MACRO_OSC_SHAPE_VOWEL;
        case 19: return braids::MACRO_OSC_SHAPE_VOWEL_FOF;
        case 20: return braids::MACRO_OSC_SHAPE_SAW_COMB;
        case 21: return braids::MACRO_OSC_SHAPE_TWIN_PEAKS_NOISE;
        case 22: return braids::MACRO_OSC_SHAPE_PLUCKED;
        case 23: return braids::MACRO_OSC_SHAPE_BLOWN;
        default: return -1;
    }
}

enum VoiceMode : uint8_t {
    VOICE_MODE_MONO = 0,
    VOICE_MODE_POLY_RR,
    VOICE_MODE_UNISON,
    VOICE_MODE_CHORD
};

// 16-Bank Morphing Wavetable Library (256 samples x 16 waveforms = 16 KB)
static constexpr int WT_TABLE_SIZE = 256;
static constexpr int WT_NUM_TABLES = 16;
DMAMEM static float wt_tables[WT_NUM_TABLES][WT_TABLE_SIZE];
static bool wt_tables_initialized = false;

// Branchless algebraic soft saturation
static inline float fast_tanh(float x) {
    return x / sqrtf(1.0f + x * x);
}

// High-speed 512-point sine lookup table with linear interpolation
static constexpr int SINE_LUT_SIZE = 512;
DMAMEM static float sine_lut[SINE_LUT_SIZE + 1];
static bool sine_lut_initialized = false;

FLASHMEM static void init_voice_sine_lut() {
    if (sine_lut_initialized) return;
    for (int i = 0; i <= SINE_LUT_SIZE; ++i) {
        float phase = (float)i / (float)SINE_LUT_SIZE;
        sine_lut[i] = sinf(phase * 2.0f * 3.141592653589793f);
    }
    sine_lut_initialized = true;
}

// Table sine where 1.0 = 2*pi
static inline float table_sin(float phase) {
    phase -= floorf(phase);
    float p_scaled = phase * (float)SINE_LUT_SIZE;
    int idx0 = (int)p_scaled;
    float frac = p_scaled - (float)idx0;
    float s0 = sine_lut[idx0];
    float s1 = sine_lut[idx0 + 1];
    return s0 + frac * (s1 - s0);
}

FLASHMEM static void init_voice_wavetables() {
    if (wt_tables_initialized) return;
    for (int i = 0; i < WT_TABLE_SIZE; ++i) {
        float phase = (float)i / (float)WT_TABLE_SIZE;
        float rad = phase * 2.0f * 3.14159265f;

        // Table 0: Pure Sine (Fundamental only)
        wt_tables[0][i] = sinf(rad);

        // Table 1: Warm Bass Sine (1st + 2nd harmonic warmth)
        wt_tables[1][i] = 0.75f * sinf(rad) + 0.35f * sinf(2.0f * rad);

        // Table 2: Triangle (Warm odd harmonics 1/n^2)
        wt_tables[2][i] = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);

        // Table 3: Analog Sawtooth (Full harmonic spectrum)
        wt_tables[3][i] = 2.0f * phase - 1.0f;

        // Table 4: Square 50% (Hollow classic odd harmonics)
        wt_tables[4][i] = (phase < 0.5f) ? 0.9f : -0.9f;

        // Table 5: Pulse 25% (Reedy, nasal pulse)
        wt_tables[5][i] = (phase < 0.25f) ? 0.9f : -0.3f;

        // Table 6: Pulse 12.5% (Thin, biting clavinet spike)
        wt_tables[6][i] = (phase < 0.125f) ? 0.95f : -0.15f;

        // Table 7: Drawbar Organ (Partials 1, 2, 3, 4)
        wt_tables[7][i] = 0.5f * sinf(rad) + 0.3f * sinf(2.0f * rad) + 0.15f * sinf(3.0f * rad) + 0.1f * sinf(4.0f * rad);

        // Table 8: Glockenspiel / Bell (Inharmonic metallic partials: 1, 2.76, 5.4)
        wt_tables[8][i] = 0.5f * sinf(rad) + 0.3f * sinf(2.76f * rad) + 0.2f * sinf(5.4f * rad);

        // Table 9: Formant "Aah" (Throat vowel resonance)
        wt_tables[9][i] = 0.5f * sinf(rad) + 0.4f * sinf(3.0f * rad) * (0.5f - 0.5f * cosf(rad));

        // Table 10: Formant "Eee" (Bright vocal formant)
        wt_tables[10][i] = 0.4f * sinf(rad) + 0.45f * sinf(5.0f * rad) * (0.5f - 0.5f * cosf(rad));

        // Table 11: Formant "Ooh" (Round hollow vowel)
        wt_tables[11][i] = 0.6f * sinf(rad) + 0.35f * sinf(2.0f * rad) * (0.5f - 0.5f * cosf(rad));

        // Table 12: Resonant Peak / Hard Sync sweep simulation
        wt_tables[12][i] = sinf(rad) * cosf(4.0f * rad);

        // Table 13: Lo-Fi 8-Step Staircase (Vintage DAC quantization)
        wt_tables[13][i] = floorf((2.0f * phase - 1.0f) * 4.0f + 0.5f) / 4.0f;

        // Table 14: Dual Octave Saw (Detuned saw stack)
        wt_tables[14][i] = 0.65f * (2.0f * phase - 1.0f) + 0.35f * (2.0f * fmodf(phase * 2.0f, 1.0f) - 1.0f);

        // Table 15: Distorted Hard Clipper (Saturated, aggressive waveshaped saw)
        float s = 2.0f * phase - 1.0f;
        wt_tables[15][i] = fast_tanh(s * 3.5f);
    }
    wt_tables_initialized = true;
}

struct VoiceUnit {
    bool active = false;
    bool gate = false;
    bool from_midi = false;
    bool strike_pending = false;
    float pitch_offset = 0.0f;
    float velocity = 1.0f;
    uint32_t sample_age = 0;
    uint8_t midi_note = 60;

    enum EnvStage : uint8_t {
        ENV_IDLE = 0,
        ENV_ATTACK,
        ENV_DECAY
    };
    EnvStage env_stage = ENV_IDLE;
    float env_level = 0.0f;

    // 6-operator phase accumulator for DX7 & FM
    float phase[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // TB-303 4-pole diode ladder filter states
    float ladder_s[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // 2-pole State Variable Filter integrator states
    float svf_ic1 = 0.0f;
    float svf_ic2 = 0.0f;

    // Modal resonator states (4 normalized bandpass sections)
    float modal_bp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float modal_lp[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // Electric Piano tine transient
    float tine_bell_env = 0.0f;

    // String physical model state (1:1 synth_advanced_karplus)
    uint16_t string_head = 0;
    float string_lp = 0.0f;
    int ks_excite_remaining = 0;
    float ks_excite_phase = 0.0f;
    float ks_excite_inc = 0.0f;
    uint32_t ks_noise_seed = 0x12345678;

    // Fast PRNG state
    uint16_t lfsr = 0xACE1u;

    // Feedback memory for 2-sample averaged feedback
    float prev_op_fb[2] = {0.0f, 0.0f};

    void trigger(float pitch_val, float vel = 1.0f, bool gate_high = true, bool is_midi = false) {
        active = true;
        gate = gate_high;
        from_midi = is_midi;
        strike_pending = true;
        pitch_offset = pitch_val;
        velocity = vel;
        env_level = 0.0f;
        env_stage = ENV_ATTACK;
        tine_bell_env = 1.0f;
        sample_age = 0;
        prev_op_fb[0] = prev_op_fb[1] = 0.0f;
        ks_excite_remaining = 0;
        ks_excite_phase = 0.0f;
        ks_noise_seed = 0x12345678 + sample_age;
    }

    void release() {
        gate = false;
        if (env_stage != ENV_ATTACK || env_level > 0.001f) {
            env_stage = ENV_DECAY;
        }
    }

    void silence() {
        gate = false;
        from_midi = false;
        env_level = 0.0f;
        env_stage = ENV_IDLE;
        active = false;
        strike_pending = false;
    }

    void reset() {
        active = false;
        gate = false;
        from_midi = false;
        strike_pending = false;
        pitch_offset = 0.0f;
        env_level = 0.0f;
        env_stage = ENV_IDLE;
        tine_bell_env = 0.0f;
        sample_age = 0;
        svf_ic1 = svf_ic2 = 0.0f;
        for (int i = 0; i < 4; ++i) {
            ladder_s[i] = 0.0f;
            modal_bp[i] = 0.0f;
            modal_lp[i] = 0.0f;
        }
        prev_op_fb[0] = prev_op_fb[1] = 0.0f;
        string_head = 0;
        string_lp = 0.0f;
        ks_excite_remaining = 0;
        ks_excite_phase = 0.0f;
        ks_excite_inc = 0.0f;
    }

    inline float next_noise() {
        uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
        lfsr = (lfsr >> 1) | (bit << 15);
        return (float)lfsr * (2.0f / 65535.0f) - 1.0f;
    }
};

// ============================================================================
// AudioVoiceStream: Dedicated Stream per Hemisphere side (Dual-Mono Architecture)
// ============================================================================
class AudioVoiceStream : public AudioStream {
public:
    AudioVoiceStream() : AudioStream(0, NULL), initialized(false) {}

    void init() {
        if (initialized) return;
        init_voice_sine_lut();
        init_voice_wavetables();

        for (int v = 0; v < NUM_VOICE_POLY; ++v) {
            voices[v].reset();
            braids_osc[v].Init();
            braids_osc[v].set_shape(braids::MACRO_OSC_SHAPE_CSAW);
            braids_osc[v].set_pitch(60 << 7);
            braids_osc[v].set_parameters(16384, 16384);
            memset(string_delay[v], 0, sizeof(string_delay[v]));
        }

        master_gain = 6500.0f; // Calibrated line level at 0 dB (~1.4Vpp)
        initialized = true;
    }

    void set_engine(int eng) {
        target_engine = constrain(eng, 0, NUM_VOICE_ENGINES - 1);
    }

    void set_pitch_base(float p) { pitch_base = p; }
    void set_timbre(float t)     { timbre = constrain(t, 0.0f, 1.0f); }
    void set_color(float c)      { color = constrain(c, 0.0f, 1.0f); }
    void set_gain(float g)       { master_gain = g; }
    void set_dx7_patch(int p)    { dx7_patch = constrain(p, 0, 31); }
    void set_voice_mode(VoiceMode m) { voice_mode = m; }

    void set_attack(float a_norm) {
        a_norm = constrain(a_norm, 0.0f, 1.0f);
        if (a_norm <= 0.001f) {
            attack_step = 1.0f; // Instantaneous attack
        } else {
            float t_sec = 0.001f + a_norm * a_norm * 3.0f;
            attack_step = 1.0f / (t_sec * AUDIO_SAMPLE_RATE_EXACT);
        }
    }

    void set_release(float r_norm) {
        release_norm = constrain(r_norm, 0.0f, 1.0f);
        if (release_norm >= 0.995f) {
            drone_mode = true;
            decay_coeff = 1.0f;
        } else {
            drone_mode = false;
            float t = 0.015f * powf(1000.0f, release_norm);
            decay_coeff = expf(-6.907755f / (t * AUDIO_SAMPLE_RATE_EXACT));
        }
    }

    void trigger_cv(float pitch_val, float vel = 1.0f, bool gate_high = true, bool is_midi = false) {
        if (!initialized) return;

        if (voice_mode == VOICE_MODE_MONO) {
            voices[0].trigger(pitch_val, vel, gate_high, is_midi);
            int shape = get_braids_shape(engine);
            if (shape >= 0) braids_osc[0].Strike();
            return;
        }

        if (voice_mode == VOICE_MODE_UNISON) {
            // 4-voice fat unison spread (center-left/right, wide spread)
            float spread_cents = 2.5f + color * 24.0f;
            static const float offsets[4] = {-1.0f, 1.0f, -0.4f, 0.4f};
            for (int v = 0; v < NUM_VOICE_POLY; ++v) {
                float detune = offsets[v] * spread_cents * (128.0f / 100.0f);
                voices[v].trigger(pitch_val + detune, vel, gate_high, is_midi);
                int shape = get_braids_shape(engine);
                if (shape >= 0) braids_osc[v].Strike();
            }
            return;
        }

        if (voice_mode == VOICE_MODE_CHORD) {
            // 4-voice chords mapped across timbre knob
            static const int8_t chords[8][4] = {
                {0, 4, 7, 11},  // 0: Maj7
                {0, 3, 7, 10},  // 1: Min7
                {0, 4, 7, 10},  // 2: Dom7
                {0, 5, 7, 10},  // 3: Sus4 / 7
                {0, 3, 7, 14},  // 4: Min9
                {0, 4, 7, 14},  // 5: Add9
                {0, 7, 12, 19}, // 6: 5th across 2 octaves
                {0, 12, 24, 36} // 7: 4 Octaves stack
            };
            int c_idx = (int)(timbre * 7.99f);
            for (int v = 0; v < NUM_VOICE_POLY; ++v) {
                float chord_pitch = (float)chords[c_idx][v] * 128.0f;
                voices[v].trigger(pitch_val + chord_pitch, vel, gate_high, is_midi);
                int shape = get_braids_shape(engine);
                if (shape >= 0) braids_osc[v].Strike();
            }
            return;
        }

        // Polyphonic Round-Robin 5 (RR5)
        int target_v = round_robin_index;
        for (int v = 0; v < NUM_VOICE_POLY; ++v) {
            int idx = (round_robin_index + v) % NUM_VOICE_POLY;
            if (voices[idx].env_stage == VoiceUnit::ENV_IDLE) {
                target_v = idx;
                break;
            }
        }
        voices[target_v].trigger(pitch_val, vel, gate_high, is_midi);
        int shape = get_braids_shape(engine);
        if (shape >= 0) braids_osc[target_v].Strike();
        round_robin_index = (target_v + 1) % NUM_VOICE_POLY;
    }

    void set_gate_cv(bool gate) {
        if (!initialized) return;
        prev_gate_state = gate;

        if (!gate) {
            for (int v = 0; v < NUM_VOICE_POLY; ++v) {
                if (!voices[v].from_midi && voices[v].gate) {
                    voices[v].release();
                }
            }
        }
    }

    void midi_note_on(uint8_t note, uint8_t velocity) {
        if (!initialized || velocity == 0) return;
        float norm_vel = (float)velocity * (1.0f / 127.0f);
        // Musical velocity curve: 20% floor + 80% linear ensures normal playing matches full CV punch,
        // while 127 exactly matches CV 1.0f
        float v_f = 0.20f + 0.80f * norm_vel;
        float pitch_val = (float)(note - 60) * 128.0f;

        if (voice_mode == VOICE_MODE_MONO) {
            voices[0].midi_note = note;
            voices[0].trigger(pitch_val, v_f, true, true);
            int shape = get_braids_shape(engine);
            if (shape >= 0) braids_osc[0].Strike();
            return;
        }

        if (voice_mode == VOICE_MODE_UNISON || voice_mode == VOICE_MODE_CHORD) {
            trigger_cv(pitch_val, v_f, true, true);
            for (int v = 0; v < NUM_VOICE_POLY; ++v) voices[v].midi_note = note;
            return;
        }

        // Polyphonic Note-On (RR5): reuse existing voice if same note is already active
        int best_v = -1;
        for (int v = 0; v < NUM_VOICE_POLY; ++v) {
            if (voices[v].active && voices[v].from_midi && voices[v].midi_note == note) {
                best_v = v;
                break;
            }
        }
        if (best_v < 0) {
            for (int v = 0; v < NUM_VOICE_POLY; ++v) {
                int idx = (round_robin_index + v) % NUM_VOICE_POLY;
                if (voices[idx].env_stage == VoiceUnit::ENV_IDLE) {
                    best_v = idx;
                    break;
                }
            }
        }
        if (best_v < 0) {
            uint32_t oldest_age = 0;
            for (int v = 0; v < NUM_VOICE_POLY; ++v) {
                if (voices[v].sample_age > oldest_age) {
                    oldest_age = voices[v].sample_age;
                    best_v = v;
                }
            }
        }
        if (best_v < 0) best_v = round_robin_index;

        voices[best_v].midi_note = note;
        voices[best_v].trigger(pitch_val, v_f, true, true);
        int shape = get_braids_shape(engine);
        if (shape >= 0) braids_osc[best_v].Strike();
        round_robin_index = (best_v + 1) % NUM_VOICE_POLY;
    }

    void midi_note_off(uint8_t note) {
        if (!initialized) return;
        for (int v = 0; v < NUM_VOICE_POLY; ++v) {
            if (voices[v].active && voices[v].midi_note == note) {
                voices[v].release();
            }
        }
    }

    void all_notes_off() {
        if (!initialized) return;
        for (int v = 0; v < NUM_VOICE_POLY; ++v) {
            voices[v].silence();
        }
        prev_gate_state = false;
    }

    uint8_t get_active_voice_mask() const {
        uint8_t mask = 0;
        int max_v = (voice_mode == VOICE_MODE_MONO) ? 1 : NUM_VOICE_POLY;
        for (int v = 0; v < max_v; ++v) {
            if (voices[v].active) mask |= (1 << v);
        }
        return mask;
    }

    void update() override {
        audio_block_t* block = allocate();
        if (!block) return;

        if (!initialized) {
            memset(block->data, 0, sizeof(block->data));
            transmit(block, 0);
            release(block);
            return;
        }

        // Apply engine switch safely at block boundary (never inside 16kHz tick ISR)
        if (target_engine != engine) {
            engine = target_engine;
            int shape = get_braids_shape(engine);
            if (shape >= 0) {
                for (int v = 0; v < NUM_VOICE_POLY; ++v) {
                    braids_osc[v].set_shape((braids::MacroOscillatorShape)shape);
                }
            }
        }

        float mix_buf[AUDIO_BLOCK_SAMPLES];
        memset(mix_buf, 0, sizeof(mix_buf));

        const float cur_timbre = timbre;
        const float cur_color = color;
        const float cur_master_gain = master_gain;
        const float cur_decay_norm = release_norm;

        // Block-rate coefficient updates for DX7
        const DX7PatchData& dx7_p = dx7_rom1a_patches[dx7_patch];
        float dx7_op_gain[6];
        float dx7_op_mod[6];
        float dx7_fb_scale = 0.0f;
        if (engine == 0) {
            for (int k = 0; k < 6; ++k) {
                float lvl = (float)dx7_p.level[k];
                if (lvl <= 15.0f) {
                    dx7_op_gain[k] = 0.0f;
                    dx7_op_mod[k] = 0.0f;
                } else {
                    dx7_op_gain[k] = powf(2.0f, (lvl - 99.0f) * (1.0f / 16.0f));
                    float t_scale = 0.4f + 1.6f * cur_timbre;
                    dx7_op_mod[k] = 0.6366f * powf(2.0f, (lvl - 99.0f) * (1.0f / 8.0f)) * t_scale;
                }
            }
            if (dx7_p.feedback > 0) {
                dx7_fb_scale = 0.5f * powf(2.0f, (float)dx7_p.feedback - 7.0f);
            }
        }

        // Strictly enforce 1-voice processing in MONO mode (prevents CPU freeze!)
        int max_voices = (voice_mode == VOICE_MODE_MONO) ? 1 : NUM_VOICE_POLY;
        int braids_shape = get_braids_shape(engine);

        if (braids_shape >= 0) {
            // ── Braids Engine Family ─────────────────────────────────────────
            int16_t t_param = (int16_t)(cur_timbre * 32767.0f);
            int16_t c_param = (int16_t)(cur_color * 32767.0f);

            int16_t braids_chunk[24];
            uint8_t braids_sync[24];

            for (int v = 0; v < max_voices; ++v) {
                VoiceUnit& vc = voices[v];
                if (!vc.active) continue;

                int32_t final_pitch = (48 << 7) + (int32_t)(pitch_base + vc.pitch_offset);
                final_pitch = constrain(final_pitch, 0, 16383);
                braids_osc[v].set_pitch(final_pitch);
                braids_osc[v].set_parameters(t_param, c_param);

                int samples_rendered = 0;
                while (samples_rendered < AUDIO_BLOCK_SAMPLES) {
                    int chunk = AUDIO_BLOCK_SAMPLES - samples_rendered;
                    if (chunk > 24) chunk = 24;

                    memset(braids_sync, 0, sizeof(braids_sync));
                    if (vc.strike_pending && samples_rendered == 0) {
                        braids_sync[0] = 1;
                        braids_osc[v].Strike();
                        vc.strike_pending = false;
                    }

                    braids_osc[v].Render(braids_sync, braids_chunk, chunk);

                    for (int i = 0; i < chunk; ++i) {
                        int s_idx = samples_rendered + i;
                        vc.sample_age++;

                        float env = vc.env_level;
                        if (vc.env_stage == VoiceUnit::ENV_ATTACK) {
                            vc.env_level += attack_step;
                            if (vc.env_level >= 1.0f) {
                                vc.env_level = 1.0f;
                                vc.env_stage = VoiceUnit::ENV_DECAY;
                            }
                        } else if (vc.env_stage == VoiceUnit::ENV_DECAY) {
                            if (!vc.gate && !drone_mode) {
                                vc.env_level *= decay_coeff;
                                if (vc.env_level < 0.0002f) {
                                    vc.env_level = 0.0f;
                                    vc.env_stage = VoiceUnit::ENV_IDLE;
                                    vc.active = false;
                                }
                            }
                        }
                        env = vc.env_level;

                        float raw = (float)braids_chunk[i] * (1.0f / 32768.0f);
                        mix_buf[s_idx] += raw * env * vc.velocity;
                    }
                    samples_rendered += chunk;
                }
            }
        } else {
            // ── Custom Synthesizer Engines (DX7, KRPS+, WAVTBL, ACID, SWARM, EPNO, MODAL)
            for (int v = 0; v < max_voices; ++v) {
                VoiceUnit& vc = voices[v];
                if (!vc.active) continue;

                float total_pitch = pitch_base + vc.pitch_offset;
                float f0 = SemitonesToRatio(total_pitch * (1.0f / 128.0f)) * C3;
                f0 = constrain(f0, 10.0f, 18000.0f);
                float phase_inc = f0 / AUDIO_SAMPLE_RATE_EXACT;

                for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                    vc.sample_age++;

                    float env = vc.env_level;
                    if (vc.env_stage == VoiceUnit::ENV_ATTACK) {
                        vc.env_level += attack_step;
                        if (vc.env_level >= 1.0f) {
                            vc.env_level = 1.0f;
                            vc.env_stage = VoiceUnit::ENV_DECAY;
                        }
                    } else if (vc.env_stage == VoiceUnit::ENV_DECAY) {
                        if (!vc.gate && !drone_mode) {
                            vc.env_level *= decay_coeff;
                            if (vc.env_level < 0.0002f) {
                                vc.env_level = 0.0f;
                                vc.env_stage = VoiceUnit::ENV_IDLE;
                                vc.active = false;
                            }
                        }
                    }
                    env = vc.env_level;

                    float sample = 0.0f;

                    switch (engine) {
                        // 0: DX7 6-Op FM (32 Genuine Yamaha ROM1A Patches)
                        case 0: {
                            float p0 = vc.phase[0]; float p1 = vc.phase[1]; float p2 = vc.phase[2];
                            float p3 = vc.phase[3]; float p4 = vc.phase[4]; float p5 = vc.phase[5];

                            // Op 6 with 2-sample averaged feedback
                            float fb_in = 0.5f * (vc.prev_op_fb[0] + vc.prev_op_fb[1]) * dx7_fb_scale;
                            float op6_out = table_sin(p5 + fb_in);
                            vc.prev_op_fb[1] = vc.prev_op_fb[0];
                            vc.prev_op_fb[0] = op6_out;
                            float m6 = op6_out * dx7_op_mod[5];

                            float raw_fm = 0.0f;
                            switch (dx7_p.algorithm) {
                                case 5: { // 3 Pairs: 6->5, 4->3, 2->1 (E.PIANO 1, TUB BELLS, HARPSICH, ORCH-CHIME, TRAIN)
                                    float c5 = table_sin(p4 + m6) * dx7_op_gain[4];
                                    float op4 = table_sin(p3);
                                    float c3 = table_sin(p2 + op4 * dx7_op_mod[3]) * dx7_op_gain[2];
                                    float op2 = table_sin(p1);
                                    float c1 = table_sin(p0 + op2 * dx7_op_mod[1]) * dx7_op_gain[0];
                                    raw_fm = (c1 + c3 + c5) * 0.35f;
                                    break;
                                }
                                case 16: // 6-Op Stack: 6->5->4->3->2->1 (BASS 1, GUITAR 2, FLUTE 1, TIMPANI)
                                case 17: {
                                    float o5 = table_sin(p4 + m6) * dx7_op_mod[4];
                                    float o4 = table_sin(p3 + o5) * dx7_op_mod[3];
                                    float o3 = table_sin(p2 + o4) * dx7_op_mod[2];
                                    float o2 = table_sin(p1 + o3) * dx7_op_mod[1];
                                    raw_fm = table_sin(p0 + o2) * dx7_op_gain[0] * 0.75f;
                                    break;
                                }
                                case 18: { // Op 6->5->4->3; Op 2; Op 1 (BRASS 3, PIANO 2, SYN-LEAD 1, REFS WHISL)
                                    float o5 = table_sin(p4 + m6) * dx7_op_mod[4];
                                    float o4 = table_sin(p3 + o5) * dx7_op_mod[3];
                                    float c3 = table_sin(p2 + o4) * dx7_op_gain[2];
                                    float c2 = table_sin(p1) * dx7_op_gain[1];
                                    float c1 = table_sin(p0) * dx7_op_gain[0];
                                    raw_fm = (c1 + c2 + c3) * 0.35f;
                                    break;
                                }
                                case 1:
                                case 2: { // Single modulator with feedback branches
                                    float o5 = table_sin(p4 + m6) * dx7_op_mod[4];
                                    float o4 = table_sin(p3 + o5) * dx7_op_mod[3];
                                    float c3 = table_sin(p2 + o4) * dx7_op_gain[2];
                                    float o2 = table_sin(p1) * dx7_op_mod[1];
                                    float c1 = table_sin(p0 + o2) * dx7_op_gain[0];
                                    raw_fm = (c1 + c3) * 0.5f;
                                    break;
                                }
                                case 3: { // 6->5->4, 3->2->1
                                    float o5 = table_sin(p4 + m6) * dx7_op_mod[4];
                                    float c4 = table_sin(p3 + o5) * dx7_op_gain[3];
                                    float o2 = table_sin(p1 + table_sin(p2) * dx7_op_mod[2]) * dx7_op_mod[1];
                                    float c1 = table_sin(p0 + o2) * dx7_op_gain[0];
                                    raw_fm = (c1 + c4) * 0.5f;
                                    break;
                                }
                                case 7:
                                case 8: { // Dual parallel modulators into single carrier
                                    float o5 = table_sin(p4 + m6) * dx7_op_mod[4];
                                    float o4 = table_sin(p3) * dx7_op_mod[3];
                                    float o3 = table_sin(p2 + o4 + o5) * dx7_op_mod[2];
                                    float c1 = table_sin(p0 + o3) * dx7_op_gain[0];
                                    raw_fm = c1 * 0.75f;
                                    break;
                                }
                                case 10:
                                case 15: { // 3 Carriers
                                    float o6 = m6;
                                    float c5 = table_sin(p4 + o6) * dx7_op_gain[4];
                                    float c3 = table_sin(p2 + table_sin(p3) * dx7_op_mod[3]) * dx7_op_gain[2];
                                    float c1 = table_sin(p0 + table_sin(p1) * dx7_op_mod[1]) * dx7_op_gain[0];
                                    raw_fm = (c1 + c3 + c5) * 0.35f;
                                    break;
                                }
                                case 19:
                                case 22:
                                case 23: { // Multi-carrier pad algorithms
                                    float o6 = m6;
                                    float c5 = table_sin(p4 + o6) * dx7_op_gain[4];
                                    float c4 = table_sin(p3) * dx7_op_gain[3];
                                    float c3 = table_sin(p2) * dx7_op_gain[2];
                                    float c1 = table_sin(p0 + table_sin(p1) * dx7_op_mod[1]) * dx7_op_gain[0];
                                    raw_fm = (c1 + c3 + c4 + c5) * 0.28f;
                                    break;
                                }
                                case 32: { // All 6 operators are parallel carriers (additive organ)
                                    raw_fm = (table_sin(p0)*dx7_op_gain[0] + table_sin(p1)*dx7_op_gain[1] +
                                              table_sin(p2)*dx7_op_gain[2] + table_sin(p3)*dx7_op_gain[3] +
                                              table_sin(p4)*dx7_op_gain[4] + table_sin(p5 + fb_in)*dx7_op_gain[5]) * 0.18f;
                                    break;
                                }
                                default: { // Standard FM default
                                    float o5 = table_sin(p4 + m6) * dx7_op_mod[4];
                                    float o4 = table_sin(p3 + o5) * dx7_op_mod[3];
                                    float c3 = table_sin(p2 + o4) * dx7_op_gain[2];
                                    float o2 = table_sin(p1) * dx7_op_mod[1];
                                    float c1 = table_sin(p0 + o2) * dx7_op_gain[0];
                                    raw_fm = (c1 + c3) * 0.5f;
                                    break;
                                }
                            }

                            // Advance all 6 operator phase accumulators
                            float base_inc = phase_inc;
                            p0 += base_inc * dx7_p.ratio[0]; if (p0 >= 1.0f) p0 -= 1.0f; vc.phase[0] = p0;
                            p1 += base_inc * dx7_p.ratio[1]; if (p1 >= 1.0f) p1 -= 1.0f; vc.phase[1] = p1;
                            p2 += base_inc * dx7_p.ratio[2]; if (p2 >= 1.0f) p2 -= 1.0f; vc.phase[2] = p2;
                            p3 += base_inc * dx7_p.ratio[3]; if (p3 >= 1.0f) p3 -= 1.0f; vc.phase[3] = p3;
                            p4 += base_inc * dx7_p.ratio[4]; if (p4 >= 1.0f) p4 -= 1.0f; vc.phase[4] = p4;
                            p5 += base_inc * dx7_p.ratio[5]; if (p5 >= 1.0f) p5 -= 1.0f; vc.phase[5] = p5;

                            sample = fast_tanh(raw_fm * 2.0f) * env;
                            break;
                        }

                        // 1: KRPS+ (1:1 with synth_advanced_karplus)
                        case 1: {
                            float iir_alpha = 0.05f + cur_timbre * 0.95f; // Timbre = Brightness
                            float filter_delay = (1.0f - iir_alpha) / iir_alpha;
                            float d = (AUDIO_SAMPLE_RATE_EXACT / f0) - filter_delay;
                            if (d < 2.0f) d = 2.0f;
                            if (d > (float)(STRING_DELAY_SIZE - 2)) d = (float)(STRING_DELAY_SIZE - 2);

                            float T_s = 0.02f * expf(cur_decay_norm * 8.699f);
                            float loop_gain = expf(-d / (T_s * AUDIO_SAMPLE_RATE_EXACT));
                            if (loop_gain > 0.999999f) loop_gain = 0.999999f;

                            if (vc.strike_pending) {
                                int n = (int)d;
                                if (n < 2) n = 2;
                                if (n >= STRING_DELAY_SIZE) n = STRING_DELAY_SIZE - 1;
                                vc.ks_excite_remaining = n;
                                vc.ks_excite_phase = 0.0f;
                                vc.ks_excite_inc = 6.283185307f / (float)n;
                                vc.string_lp = 0.0f;
                                vc.strike_pending = false;
                            }

                            float read_pos = (float)vc.string_head - d;
                            while (read_pos < 0.0f) read_pos += (float)STRING_DELAY_SIZE;

                            uint32_t r0 = ((uint32_t)read_pos) & (STRING_DELAY_SIZE - 1);
                            uint32_t r1 = (r0 + 1) & (STRING_DELAY_SIZE - 1);
                            float frac = read_pos - floorf(read_pos);
                            float raw = string_delay[v][r0] + frac * (string_delay[v][r1] - string_delay[v][r0]);

                            float new_sample;
                            if (vc.ks_excite_remaining > 0) {
                                vc.ks_noise_seed = vc.ks_noise_seed * 1664525u + 1013904223u;
                                float noise = (float)((int32_t)vc.ks_noise_seed) * (1.0f / 2147483648.0f);
                                float sine = table_sin(vc.ks_excite_phase * 0.15915494f);
                                float excite = ((1.0f - cur_color) * noise + cur_color * sine) * 32767.0f; // Color = Body
                                if (excite > 32767.0f) excite = 32767.0f;
                                if (excite < -32767.0f) excite = -32767.0f;
                                vc.string_lp = iir_alpha * excite + (1.0f - iir_alpha) * vc.string_lp;
                                new_sample = excite;
                                vc.ks_excite_phase += vc.ks_excite_inc;
                                vc.ks_excite_remaining--;
                            } else {
                                float filtered = iir_alpha * raw + (1.0f - iir_alpha) * vc.string_lp;
                                vc.string_lp = filtered;
                                new_sample = filtered * loop_gain;
                            }

                            string_delay[v][vc.string_head] = new_sample;
                            vc.string_head = (vc.string_head + 1) & (STRING_DELAY_SIZE - 1);
                            sample = new_sample * (1.0f / 32768.0f);
                            break;
                        }

                        // 2: WAVTBL (16-Bank Morphing Wavetable Synthesizer)
                        case 2: {
                            float wt_pos = cur_color * 14.999f;
                            int t0 = (int)wt_pos;
                            int t1 = t0 + 1;
                            float t_frac = wt_pos - (float)t0;

                            float p = vc.phase[0];
                            float s_pos = p * (float)WT_TABLE_SIZE;
                            int s0 = (int)s_pos & (WT_TABLE_SIZE - 1);
                            int s1 = (s0 + 1) & (WT_TABLE_SIZE - 1);
                            float s_frac = s_pos - (float)s0;

                            float v0 = wt_tables[t0][s0] + s_frac * (wt_tables[t0][s1] - wt_tables[t0][s0]);
                            float v1 = wt_tables[t1][s0] + s_frac * (wt_tables[t1][s1] - wt_tables[t1][s0]);
                            float raw_wt = v0 + t_frac * (v1 - v0);

                            float drive = 1.0f + cur_timbre * 3.5f;
                            sample = fast_tanh(raw_wt * drive) * env;

                            p += phase_inc;
                            if (p >= 1.0f) p -= 1.0f;
                            vc.phase[0] = p;
                            break;
                        }

                        // 3: ACID (TB-303 Diode Ladder)
                        case 3: {
                            float p = vc.phase[0];
                            float raw = (p < 0.5f) ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
                            raw += (p - 0.5f) * 0.8f;

                            float acid_cutoff = 60.0f + cur_timbre * 6500.0f;
                            float acid_omega = 2.0f * 3.14159265f * (acid_cutoff / AUDIO_SAMPLE_RATE_EXACT);
                            if (acid_omega > 0.85f) acid_omega = 0.85f;
                            float acid_res = cur_color * 3.92f;

                            float input = raw - acid_res * vc.ladder_s[3];
                            input = fast_tanh(input * 1.5f);
                            for (int stage = 0; stage < 4; ++stage) {
                                vc.ladder_s[stage] += acid_omega * (input - vc.ladder_s[stage]);
                                input = vc.ladder_s[stage];
                            }
                            sample = fast_tanh(vc.ladder_s[3] * 1.8f) * env;

                            p += phase_inc;
                            if (p >= 1.0f) p -= 1.0f;
                            vc.phase[0] = p;
                            break;
                        }

                        // 11: SWARM (6-Oscillator Supersaw Swarm)
                        case 11: {
                            float spread = 0.001f + cur_timbre * 0.025f;
                            float sum = 0.0f;
                            static const float swarm_detunes[6] = {-1.0f, -0.6f, -0.2f, 0.2f, 0.6f, 1.0f};
                            for (int k = 0; k < 6; ++k) {
                                float pk = vc.phase[k];
                                sum += (2.0f * pk - 1.0f);
                                pk += phase_inc * (1.0f + swarm_detunes[k] * spread);
                                if (pk >= 1.0f) pk -= 1.0f;
                                vc.phase[k] = pk;
                            }
                            float hp_cut = 0.1f + cur_color * 0.85f;
                            vc.svf_ic1 += hp_cut * (sum * (1.0f / 6.0f) - vc.svf_ic1);
                            sample = fast_tanh(sum * 0.25f) * env;
                            break;
                        }

                        // 12: EPNO (Physical Electric Piano)
                        case 12: {
                            float p = vc.phase[0];
                            float tine_p = vc.phase[1];
                            float tine_inc = phase_inc * (3.0f + cur_timbre * 4.0f);

                            float reed = table_sin(p);
                            float tine = table_sin(tine_p) * vc.tine_bell_env;
                            vc.tine_bell_env *= 0.9992f;

                            float pickup = reed + tine * (0.3f + cur_color * 0.7f);
                            sample = fast_tanh(pickup * 1.4f) * env;

                            p += phase_inc; if (p >= 1.0f) p -= 1.0f; vc.phase[0] = p;
                            tine_p += tine_inc; if (tine_p >= 1.0f) tine_p -= 1.0f; vc.phase[1] = tine_p;
                            break;
                        }

                        // 13: MODAL (4-Mode Inharmonic Resonator)
                        case 13: {
                            float impulse = 0.0f;
                            if (vc.strike_pending) {
                                impulse = 1.0f;
                                vc.strike_pending = false;
                            }
                            static const float m_ratios[4] = {1.0f, 2.76f, 5.40f, 8.93f};
                            float mode_sum = 0.0f;
                            for (int m = 0; m < 4; ++m) {
                                float m_ratio = m_ratios[m] * (1.0f + cur_color * 0.4f * (float)m);
                                float m_inc = phase_inc * m_ratio;
                                if (m_inc >= 0.45f) continue;
                                float q = 15.0f + 600.0f * cur_decay_norm;
                                float damp = 1.0f / q;
                                float frad = 2.0f * sinf(3.14159265f * m_inc);
                                float hp = impulse - damp * vc.modal_bp[m] - vc.modal_lp[m];
                                vc.modal_bp[m] += frad * hp;
                                vc.modal_lp[m] += frad * vc.modal_bp[m];
                                mode_sum += vc.modal_bp[m] * (1.0f / (1.0f + (float)m * 0.7f));
                            }
                            sample = fast_tanh(mode_sum * 2.0f) * env;
                            break;
                        }

                        default:
                            break;
                    }

                    mix_buf[i] += sample * vc.velocity;
                }
            }
        }

        // Master Output Stage: Clean algebraic soft saturation + calibrated master gain
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            float s = mix_buf[i];
            float out_f = fast_tanh(s) * cur_master_gain;
            block->data[i] = (int16_t)constrain(out_f, -32767.0f, 32767.0f);
        }

        transmit(block, 0);
        release(block);
    }

private:
    VoiceUnit voices[NUM_VOICE_POLY];
    braids::MacroOscillator braids_osc[NUM_VOICE_POLY];
    float string_delay[NUM_VOICE_POLY][STRING_DELAY_SIZE];

    VoiceMode voice_mode = VOICE_MODE_MONO;
    int round_robin_index = 0;
    volatile int target_engine = 0;
    int engine = 0;
    int dx7_patch = 10; // Default: E.PIANO 1
    float pitch_base = 0.0f;
    float timbre = 0.5f;
    float color = 0.5f;
    float attack_step = 1.0f;
    float release_norm = 0.7f;
    float decay_coeff = 0.999f;
    float master_gain = 6500.0f; // Calibrated line level at 0 dB (~1.4Vpp)
    bool drone_mode = false;
    bool prev_gate_state = false;
    bool initialized = false;
};

// Two completely independent audio streams for dual-mono operation
static DMAMEM AudioVoiceStream voice_streams[2];

// ============================================================================
// VoiceApplet: Hemisphere Audio Applet Wrapper
// ============================================================================
class VoiceApplet : public HemisphereAudioApplet {
public:
    static constexpr const char* applet_name_() { return "Voice"; }
    static constexpr const uint8_t* applet_icon_() { return nullptr; }

    const char* applet_name() const override { return applet_name_(); }
    const uint8_t* applet_icon() const override { return applet_icon_(); }

    inline int get_side() const {
        return (int)hemisphere & 1;
    }

    AudioVoiceStream& stream() {
        return voice_streams[get_side()];
    }

    void Start() override {
        HS::frame.MIDIState.any_channel_omni = true;
        if (!pitch_cv.enabled()) {
            pitch_cv.SetInput(get_side() == 0 ? 0 : (ADC_CHANNEL_COUNT > 2 ? 2 : 1));
        }
        if (!trig_cv.enabled()) {
            trig_cv.SetGateInput(get_side() == 0 ? 0 : (DIGITAL_INPUT_COUNT > 2 ? 2 : 1));
        }
        stream().init();
        stream().set_engine(engine_idx);
        stream().set_dx7_patch(dx7_patch_idx);
        stream().set_timbre(timbre * 0.01f);
        stream().set_color(color * 0.01f);
        stream().set_attack(attack_time * 0.01f);
        stream().set_release(release_time * 0.01f);
        stream().set_voice_mode((VoiceMode)voice_mode_idx);
        update_gain();
    }

    void Unload() override {
        AllowRestart();
    }

    void update_gain() {
        float gain = 6500.0f * powf(10.0f, (float)level_db * (1.0f / 20.0f));
        stream().set_gain(gain);
    }

    void AuxButton() override {
        if (EditMode()) {
            CancelEdit();
            return;
        }
    }

    uint8_t get_effective_input_source() const {
        if (input_source == INPUT_SRC_TGL) {
            if (src_gate.enabled()) {
                return src_gate.Gate() ? INPUT_SRC_MID : INPUT_SRC_CV;
            }
            return toggle_latch ? INPUT_SRC_MID : INPUT_SRC_CV;
        }
        return input_source;
    }

    void OnMIDIMessage(uint8_t channel, uint8_t message, uint8_t data1, uint8_t data2) override {
        // Universal Stop / Panic handling: always silence voices immediately
        if (message == 0xFC || ((message & 0xF0) == 0xB0 && (data1 == 120 || data1 == 123))) {
            stream().all_notes_off();
            return;
        }

        // Check channel filtering (0 = OFF, 1..16 = specific channel, 17 = OMNI)
        if (midi_channel == 0) return;
        if (midi_channel >= 1 && midi_channel <= 16 && channel != midi_channel) return;

        uint8_t cur_src = get_effective_input_source();
        if (cur_src == INPUT_SRC_CV) return; // Completely ignore MIDI note playback in CV mode

        uint8_t status = message & 0xF0;
        if (status == 0x90) {
            if (data2 > 0) {
                last_midi_rx_tick = millis();
                last_midi_rx_ch = channel;
                last_midi_rx_note = data1;
                stream().midi_note_on(data1, data2);
            } else {
                stream().midi_note_off(data1);
            }
        } else if (status == 0x80) {
            stream().midi_note_off(data1);
        } else if (status == 0xE0) {
            last_midi_rx_tick = millis();
            last_midi_rx_ch = channel;
            int bend_val = ((int)data2 << 7) | (int)data1; // 0..16383, 8192 is center
            midi_pitch_bend_units = ((float)(bend_val - 8192) * (1.0f / 8192.0f)) * 256.0f; // +/- 2 semitones
        }
    }

    void Controller() override {
        if (input_source == INPUT_SRC_TGL && src_gate.enabled() && src_gate.Clock()) {
            toggle_latch = !toggle_latch;
        }

        // Immediate note-off when transport clock stops
        bool is_clock_running = HS::clock_m.IsRunning();
        if (was_clock_running && !is_clock_running) {
            adc_lag_ = 0;
            stream().all_notes_off();
        }
        was_clock_running = is_clock_running;

        uint8_t cur_src = get_effective_input_source();
        if (cur_src != last_effective_src) {
            last_effective_src = cur_src;
            adc_lag_ = 0;
            stream().all_notes_off();
        }

        if (cur_src != INPUT_SRC_MID) {
            // Trigger CV with ADC lag settling countdown
            if (trig_cv.Clock()) adc_lag_ = HEMISPHERE_ADC_LAG;
            if (adc_lag_ > 0 && --adc_lag_ == 0) {
                stream().trigger_cv(0.0f, 1.0f, true); // Trigger voice with gate high
            }
            stream().set_gate_cv(trig_cv.Gate());
        } else {
            adc_lag_ = 0;
            stream().set_gate_cv(false);
        }

        int eff_eng = engine_idx;

        // Engine CV modulation
        if (engine_cv.enabled()) {
            eff_eng += (int)(engine_cv.InF() * (float)(NUM_VOICE_ENGINES - 1));
            eff_eng = constrain(eff_eng, 0, NUM_VOICE_ENGINES - 1);
        }
        eff_disp_eng = eff_eng;
        stream().set_engine(eff_eng);

        // Pitch CV modulation (1V/Oct) - only apply CV pitch when not purely in MIDI mode
        float total_pitch = (float)pitch + midi_pitch_bend_units;
        if (cur_src != INPUT_SRC_MID) {
            total_pitch += (float)pitch_cv.In();
        }
        stream().set_pitch_base(total_pitch);

        // Timbre CV modulation
        float t_norm = constrain((float)timbre * 0.01f + timbre_cv.InF(), 0.0f, 1.0f);
        stream().set_timbre(t_norm);

        // Color / DX7 Patch CV modulation
        if (eff_eng == 0) {
            int p_idx = dx7_patch_idx;
            if (color_cv.enabled()) {
                p_idx += (int)(color_cv.InF() * 31.0f);
                p_idx = constrain(p_idx, 0, 31);
            }
            stream().set_dx7_patch(p_idx);
            stream().set_color((float)p_idx / 31.0f);
        } else {
            float c_norm = constrain((float)color * 0.01f + color_cv.InF(), 0.0f, 1.0f);
            stream().set_color(c_norm);
        }

        // Attack CV modulation
        float a_norm = constrain((float)attack_time * 0.01f + attack_cv.InF(), 0.0f, 1.0f);
        stream().set_attack(a_norm);

        // Release CV modulation
        float r_norm = constrain((float)release_time * 0.01f + release_cv.InF(), 0.0f, 1.0f);
        stream().set_release(r_norm);

        update_gain();
        stream().set_voice_mode((VoiceMode)voice_mode_idx);
    }

    void View() override {
        gfxHeader(applet_name());
        if (millis() - last_midi_rx_tick < 300) {
            graphics.setPixel(56, 2);
            graphics.setPixel(56, 3);
        }

        // Voice activity dots in the header
        int max_dots = (voice_mode_idx == VOICE_MODE_MONO) ? 1 : NUM_VOICE_POLY;
        uint8_t v_mask = stream().get_active_voice_mask();
        for (int v = 0; v < max_dots; ++v) {
            int vx = 38 + v * 3;
            if (v_mask & (1 << v)) {
                graphics.setPixel(vx, 2);
                graphics.setPixel(vx, 3);
            } else {
                graphics.setPixel(vx, 3);
            }
        }

        const bool pg2 = (cursor >= PAGE2_START);

        if (!pg2) {
            // ── Page 1: Main Performance (Eng, Pitch, Tmb, Col/Pch, Dec) ────
            // Row 1: Engine & Engine CV
            gfxPrint(1, 15, "E:");
            gfxStartCursor(13, 15);
            int disp_eng = engine_cv.enabled() ? eff_disp_eng : engine_idx;
            if (disp_eng >= 0 && disp_eng < NUM_VOICE_ENGINES) {
                graphics.print(voice_engine_names[disp_eng]);
            }
            gfxEndCursor(cursor == CURSOR_ENGINE);

            gfxStartCursor(53, 15);
            gfxPrint(engine_cv);
            gfxEndCursor(cursor == CURSOR_ENGINE_CV, false, engine_cv.InputName());

            // Row 2: Pitch & Fine Tuning
            gfxStartCursor(1, 25);
            gfxPrintTuningIndicator(pitch);
            gfxEndCursor(cursor == CURSOR_PITCH_SEMI);

            gfxStartCursor(11, 25);
            gfxPrintPitchHz(pitch);
            gfxEndCursor(cursor == CURSOR_PITCH_FINE);

            gfxStartCursor(53, 25);
            gfxPrint(pitch_cv);
            gfxEndCursor(cursor == CURSOR_PITCH_CV, false, pitch_cv.InputName());

            // Row 3: Timbre
            gfxPrint(1, 35, "Tmb:");
            gfxStartCursor(25, 35);
            graphics.printf("%3d%%", timbre);
            gfxEndCursor(cursor == CURSOR_TIMBRE);

            gfxStartCursor(53, 35);
            gfxPrint(timbre_cv);
            gfxEndCursor(cursor == CURSOR_TIMBRE_CV, false, timbre_cv.InputName());

            // Row 4: Color (or DX7 Patch / Body / Wavetable)
            if (engine_idx == 0) { // DX7
                gfxPrint(1, 45, "Pch:");
                gfxStartCursor(23, 45);
                graphics.printf("%-6.6s", dx7_rom1a_patches[dx7_patch_idx].name);
                gfxEndCursor(cursor == CURSOR_COLOR);
            } else if (engine_idx == 1) { // KRPS+
                gfxPrint(1, 45, "Bdy:");
                gfxStartCursor(25, 45);
                graphics.printf("%3d%%", color);
                gfxEndCursor(cursor == CURSOR_COLOR);
            } else if (engine_idx == 2) { // WAVTBL
                gfxPrint(1, 45, "Wav:");
                gfxStartCursor(25, 45);
                int wt_num = (color * 15) / 100 + 1;
                graphics.printf("W%02d", wt_num);
                gfxEndCursor(cursor == CURSOR_COLOR);
            } else {
                gfxPrint(1, 45, "Col:");
                gfxStartCursor(25, 45);
                graphics.printf("%3d%%", color);
                gfxEndCursor(cursor == CURSOR_COLOR);
            }

            gfxStartCursor(53, 45);
            gfxPrint(color_cv);
            gfxEndCursor(cursor == CURSOR_COLOR_CV, false, color_cv.InputName());

            // Row 5: Decay (Release)
            gfxPrint(1, 55, "Dec:");
            gfxStartCursor(25, 55);
            if (release_time >= 100) graphics.print("INF");
            else graphics.printf("%3d%%", release_time);
            gfxEndCursor(cursor == CURSOR_RELEASE);

            gfxStartCursor(53, 55);
            gfxPrint(release_cv);
            gfxEndCursor(cursor == CURSOR_RELEASE_CV, false, release_cv.InputName());

            gfxPrint(58, 2, "1");
        } else {
            // ── Page 2: Envelopes, Polyphony, Modulation & Output ────────────
            // Row 1: Attack + Attack CV
            gfxPrint(1, 15, "Atk:");
            gfxStartCursor(25, 15);
            graphics.printf("%3d%%", attack_time);
            gfxEndCursor(cursor == CURSOR_ATTACK);

            gfxStartCursor(53, 15);
            gfxPrint(attack_cv);
            gfxEndCursor(cursor == CURSOR_ATTACK_CV, false, attack_cv.InputName());

            // Row 2: Polyphony Mode & MIDI Channel
            gfxPrint(1, 25, "Ply:");
            gfxStartCursor(21, 25);
            if (voice_mode_idx == VOICE_MODE_MONO) graphics.print("MON");
            else if (voice_mode_idx == VOICE_MODE_POLY_RR) graphics.print("RR4");
            else if (voice_mode_idx == VOICE_MODE_UNISON) graphics.print("UNI");
            else graphics.print("CHD");
            gfxEndCursor(cursor == CURSOR_VOICE_MODE);

            gfxPrint(38, 25, "M:");
            gfxStartCursor(47, 25);
            if (midi_channel == 0) graphics.print("OFF");
            else if (midi_channel == 17) graphics.print("OMN");
            else graphics.printf("%2d", midi_channel);
            gfxEndCursor(cursor == CURSOR_MIDI_CHANNEL);

            // Row 3: Trigger CV & dB Level Control
            gfxPrint(1, 35, "Trg:");
            gfxStartCursor(20, 35);
            gfxPrint(trig_cv);
            gfxEndCursor(cursor == CURSOR_TRIG_CV, false, trig_cv.InputName(), "Gate");

            gfxStartCursor(44, 35);
            graphics.printf("%+2ddB", level_db);
            gfxEndCursor(cursor == CURSOR_LEVEL);

            // Row 4: Input Source & Gate/Button Map
            gfxPrint(1, 45, "In:");
            gfxStartCursor(23, 45);
            uint8_t eff_src = get_effective_input_source();
            if (cursor == CURSOR_INPUT_SOURCE && EditMode()) {
                if (input_source == INPUT_SRC_CV) graphics.print("CV ");
                else if (input_source == INPUT_SRC_MID) graphics.print("MID");
                else if (input_source == INPUT_SRC_TGL) graphics.print("TGL");
                else graphics.print("ALL");
            } else {
                if (input_source == INPUT_SRC_TGL) {
                    graphics.print(eff_src == INPUT_SRC_MID ? "T:MD" : "T:CV");
                } else if (input_source == INPUT_SRC_ALL) {
                    graphics.print("ALL");
                } else {
                    graphics.print(eff_src == INPUT_SRC_MID ? "MID" : "CV ");
                }
            }
            gfxEndCursor(cursor == CURSOR_INPUT_SOURCE);

            gfxStartCursor(53, 45);
            gfxPrint(src_gate);
            gfxEndCursor(cursor == CURSOR_INPUT_SRC_GATE, false, src_gate.InputName(), "Gate");

            gfxPrint(58, 2, "2");
        }

        gfxDisplayInputMapEditor();
    }

    void OnButtonPress() override {
        if (CheckEditInputMapPress(
            cursor,
            IndexedInput(CURSOR_ENGINE_CV,       engine_cv),
            IndexedInput(CURSOR_PITCH_CV,        pitch_cv),
            IndexedInput(CURSOR_TIMBRE_CV,       timbre_cv),
            IndexedInput(CURSOR_COLOR_CV,        color_cv),
            IndexedInput(CURSOR_RELEASE_CV,      release_cv),
            IndexedInput(CURSOR_ATTACK_CV,       attack_cv),
            IndexedInput(CURSOR_TRIG_CV,         trig_cv),
            IndexedInput(CURSOR_INPUT_SRC_GATE,  src_gate)
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
            case CURSOR_ENGINE:
                engine_idx = (int8_t)constrain((int)engine_idx + direction, 0, NUM_VOICE_ENGINES - 1);
                stream().set_engine(engine_idx);
                break;
            case CURSOR_ENGINE_CV:
                engine_cv.ChangeSource(direction);
                break;
            case CURSOR_PITCH_SEMI:
                pitch = constrain(pitch + direction * 128, MIN_PITCH, MAX_PITCH);
                break;
            case CURSOR_PITCH_FINE:
                pitch = constrain(pitch + direction * 4, MIN_PITCH, MAX_PITCH);
                break;
            case CURSOR_PITCH_CV:
                pitch_cv.ChangeSource(direction);
                break;
            case CURSOR_TIMBRE:
                timbre = (uint8_t)constrain((int)timbre + direction, 0, 100);
                break;
            case CURSOR_TIMBRE_CV:
                timbre_cv.ChangeSource(direction);
                break;
            case CURSOR_COLOR:
                if (engine_idx == 0) { // DX7
                    dx7_patch_idx = (int8_t)constrain((int)dx7_patch_idx + direction, 0, 31);
                    stream().set_dx7_patch(dx7_patch_idx);
                } else {
                    color = (uint8_t)constrain((int)color + direction, 0, 100);
                }
                break;
            case CURSOR_COLOR_CV:
                color_cv.ChangeSource(direction);
                break;
            case CURSOR_RELEASE:
                release_time = (uint8_t)constrain((int)release_time + direction, 0, 100);
                break;
            case CURSOR_RELEASE_CV:
                release_cv.ChangeSource(direction);
                break;
            case CURSOR_ATTACK:
                attack_time = (uint8_t)constrain((int)attack_time + direction, 0, 100);
                break;
            case CURSOR_ATTACK_CV:
                attack_cv.ChangeSource(direction);
                break;
            case CURSOR_VOICE_MODE:
                voice_mode_idx = (uint8_t)constrain((int)voice_mode_idx + direction, 0, 3);
                break;
            case CURSOR_MIDI_CHANNEL:
                midi_channel = (uint8_t)constrain((int)midi_channel + direction, 0, 17);
                break;
            case CURSOR_TRIG_CV:
                trig_cv.ChangeSource(direction);
                break;
            case CURSOR_LEVEL:
                level_db = (int8_t)constrain((int)level_db + direction, -48, 18);
                break;
            case CURSOR_INPUT_SOURCE:
                input_source = (uint8_t)constrain((int)input_source + direction, 0, 3);
                break;
            case CURSOR_INPUT_SRC_GATE:
                src_gate.ChangeSource(direction);
                break;
            default:
                break;
        }
    }

    void SetHelp() override {
        help[HELP_DIGITAL1] = (get_side() == 0) ? "Gate 1" : "Gate 3";
        help[HELP_CV1]      = (get_side() == 0) ? "1V/Oct 1" : "1V/Oct 3";
        help[HELP_CV2]      = (get_side() == 0) ? "Mod/Fdr 2" : "Mod/Fdr 4";
        help[HELP_OUT1]     = (get_side() == 0) ? "Audio L" : "Audio R";
        help[HELP_EXTRA1]   = "RND / M-Ch";
        help[HELP_EXTRA2]   = "In: CV / MIDI";
    }

    void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
        data[0] = PackPackables(
            pack<6>(engine_idx),
            pack<2>(input_source),
            pack<8>(timbre),
            pack<8>(color),
            pack<8>(attack_time),
            pack<8>(release_time),
            pack<8>(dx7_patch_idx),
            pack<8>(level_db),
            pack<3>(voice_mode_idx),
            pack<5>(midi_channel)
        );
        uint16_t dummy_pad = 0;
        data[1] = PackPackables(pitch, dummy_pad, engine_cv, pitch_cv);
        data[2] = PackPackables(timbre_cv, color_cv, attack_cv, release_cv);
        data[3] = PackPackables(trig_cv, src_gate);
    }

    void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
            engine_idx = 0;
            input_source = INPUT_SRC_ALL;
            timbre = 50;
            color = 50;
            attack_time = 0;
            release_time = 70;
            dx7_patch_idx = 10;
            level_db = 0;
            voice_mode_idx = 0;
            midi_channel = 17; // OMNI
            pitch = 0;
            pitch_cv.SetInput(get_side() == 0 ? 0 : (ADC_CHANNEL_COUNT > 2 ? 2 : 1));
            trig_cv.SetGateInput(get_side() == 0 ? 0 : (DIGITAL_INPUT_COUNT > 2 ? 2 : 1));
            return;
        }
        UnpackPackables(
            data[0],
            pack<6>(engine_idx),
            pack<2>(input_source),
            pack<8>(timbre),
            pack<8>(color),
            pack<8>(attack_time),
            pack<8>(release_time),
            pack<8>(dx7_patch_idx),
            pack<8>(level_db),
            pack<3>(voice_mode_idx),
            pack<5>(midi_channel)
        );
        uint16_t dummy_pad = 0;
        UnpackPackables(data[1], pitch, dummy_pad, engine_cv, pitch_cv);
        UnpackPackables(data[2], timbre_cv, color_cv, attack_cv, release_cv);
        UnpackPackables(data[3], trig_cv, src_gate);

        if (!pitch_cv.enabled()) {
            pitch_cv.SetInput(get_side() == 0 ? 0 : (ADC_CHANNEL_COUNT > 2 ? 2 : 1));
        }
        if (!trig_cv.enabled()) {
            trig_cv.SetGateInput(get_side() == 0 ? 0 : (DIGITAL_INPUT_COUNT > 2 ? 2 : 1));
        }

        // Defensive sanitization: protect against corrupted or stale EEPROM data
        engine_idx = (int8_t)constrain((int)engine_idx, 0, NUM_VOICE_ENGINES - 1);
        input_source = (uint8_t)constrain((int)input_source, 0, 3);
        timbre = (uint8_t)constrain((int)timbre, 0, 100);
        color = (uint8_t)constrain((int)color, 0, 100);
        attack_time = (uint8_t)constrain((int)attack_time, 0, 100);
        release_time = (uint8_t)constrain((int)release_time, 0, 100);
        dx7_patch_idx = (int8_t)constrain((int)dx7_patch_idx, 0, 31);
        level_db = (int8_t)constrain((int)level_db, -48, 18);
        voice_mode_idx = (uint8_t)constrain((int)voice_mode_idx, 0, 3);
        midi_channel = (uint8_t)constrain((int)midi_channel, 0, 17);
        pitch = constrain(pitch, MIN_PITCH, MAX_PITCH);
    }

    AudioStream* InputStream() override {
        return &input_stream;
    }

    AudioStream* OutputStream() override {
        return &stream();
    }

private:
    enum Cursor {
        // Page 1: Main Performance
        CURSOR_ENGINE,
        CURSOR_ENGINE_CV,
        CURSOR_PITCH_SEMI,
        CURSOR_PITCH_FINE,
        CURSOR_PITCH_CV,
        CURSOR_TIMBRE,
        CURSOR_TIMBRE_CV,
        CURSOR_COLOR,
        CURSOR_COLOR_CV,
        CURSOR_RELEASE,
        CURSOR_RELEASE_CV,

        // Page 2: Envelopes, Polyphony & Mod
        PAGE2_START,
        CURSOR_ATTACK = PAGE2_START,
        CURSOR_ATTACK_CV,
        CURSOR_VOICE_MODE,
        CURSOR_MIDI_CHANNEL,
        CURSOR_TRIG_CV,
        CURSOR_LEVEL,
        CURSOR_INPUT_SOURCE,
        CURSOR_INPUT_SRC_GATE,
        CURSOR_COUNT
    };

    int8_t cursor = 0;
    int8_t engine_idx = 0; // Default: DX7
    int16_t pitch = 0;
    uint8_t timbre = 50;
    uint8_t color = 50;
    uint8_t attack_time = 0;
    uint8_t release_time = 70;
    int8_t dx7_patch_idx = 10; // Default: E.PIANO 1
    int8_t level_db = 0;
    uint8_t voice_mode_idx = 0; // MONO
    uint8_t midi_channel = 17; // OMNI


    enum InputSourceMode : uint8_t {
        INPUT_SRC_CV  = 0,
        INPUT_SRC_MID = 1,
        INPUT_SRC_TGL = 2,
        INPUT_SRC_ALL = 3,
    };

    uint8_t input_source = INPUT_SRC_ALL;
    bool toggle_latch = false;
    uint8_t last_effective_src = 255;
    bool was_clock_running = false;
    uint32_t last_midi_rx_tick = 0;
    uint8_t last_midi_rx_ch = 0;
    uint8_t last_midi_rx_note = 0;
    float midi_pitch_bend_units = 0.0f;

    int eff_disp_eng = 0;
    int adc_lag_ = 0;

    CVInputMap engine_cv;
    CVInputMap pitch_cv;
    CVInputMap timbre_cv;
    CVInputMap color_cv;
    CVInputMap attack_cv;
    CVInputMap release_cv;
    DigitalInputMap trig_cv;
    DigitalInputMap src_gate;
    AudioPassthrough<1> input_stream;

    static constexpr int16_t MIN_PITCH = -60 * 128;
    static constexpr int16_t MAX_PITCH =  60 * 128;
};
