#pragma once

#include "HemisphereAudioApplet.h"
#include "../Audio/AudioPassthrough.h"
#include "dx7_data.h"
#include "braids/resources.h"
#include <Arduino.h>
#include <arm_math.h>

// ============================================================================
// OXI Coral / Plaits Polyphonic Synthesizer Engine for ORN8 / Phazerville
// 5-Voice Polyphony with 12 High-Performance Synthesis Engines:
//  0: V-ANLG  (Virtual Analog: PolyBLEP Saw/Pulse + Sub + Resonant SVF)
//  1: ADDITV  (Additive: 8-Harmonic Partial Bank with Spectral Tilt & Inharm)
//  2: W-SHAP  (Waveshaper: Non-linear Multi-Stage Wavefolder & Drive)
//  3: FMX7    (Yamaha DX7 6-Op FM: Selectable 32 Genuine ROM 1A Patches)
//  4: W-TABL  (Morphing Wavetable with Continuous Sub & Phase Drive)
//  5: SWARM   (Wavetable Detuned Unison Swarm / Supersaw Cluster)
//  6: ACID    (Roland TB-303 Diode Ladder Emulation & Squelch Filter)
//  7: PIANOZ  (Physical Electric Piano: Rhodes/Wurli Tine Transient + Body)
//  8: MODAL   (Modal Resonator: 4 Normalized SVF Resonators & Inharmonicity)
//  9: VOX     (Formant / Vowel Speech Engine: 3 Formants + Glottal Pulse)
// 10: STRNGS  (Karplus-Strong Plucked String Physical Modeling)
// 11: PARTCL  (Particle / Granular Noise Cloud / Rain / Dust Resonator)
//
// Sound Shaping Triumvirate:
//  - HARMONICS / DX7 PATCH SELECTOR (0..31 / 1..32 on screen)
//  - TIMBRE    (Primary Spectral Brightness / Wave Morph / Vowel)
//  - COLOR     (Morph / Resonance / Material Structure / Formant Shift)
//
// Dual Envelope Shaping:
//  - ATTACK    (Snap Pluck -> Long Swell)
//  - DECAY     (Snappy Release -> 15s Ambient Tail -> Drone INF)
//
// Output Headroom & Level:
//  - dB Output Control (-48 dB to +6 dB, -12 dB default sweet spot)
//
// All parameters modulatable via CVInputMap / DigitalInputMap.
// 5-Voice Polyphony: Round-Robin (RR5), Unison (UNI), and Chord (CHD) modes.
// Full MIDI Note-On/Off, Velocity, Pitch Bend, CC1 (Timbre), CC74 (Color).
// ============================================================================

static const char* const coral_engine_names[] = {
    "V-ANLG",  // 0: Virtual Analog
    "ADDITV",  // 1: Additive Harmonics
    "W-SHAP",  // 2: Waveshaper / Folder
    "FMX7",    // 3: DX7 6-Op FM
    "W-TABL",  // 4: Morphing Wavetable
    "SWARM",   // 5: Wavetable Swarm
    "ACID",    // 6: TB-303 Diode Ladder
    "PIANOZ",  // 7: Physical Electric Piano
    "MODAL",   // 8: Physical Modal Resonator
    "VOX",     // 9: Formant / Vowel Speech
    "STRNGS",  // 10: Karplus-Strong String
    "PARTCL"   // 11: Particle / Granular Dust
};

static constexpr int NUM_CORAL_ENGINES = sizeof(coral_engine_names) / sizeof(coral_engine_names[0]);
static constexpr int NUM_CORAL_VOICES = 5;

// High-resolution morphing wavetable bank: 8 waveforms x 256 samples
static constexpr int WT_TABLE_SIZE = 256;
static constexpr int WT_NUM_TABLES = 8;

static float wt_tables[WT_NUM_TABLES][WT_TABLE_SIZE];
static bool wt_tables_initialized = false;

static inline void init_coral_wavetables() {
    if (wt_tables_initialized) return;
    for (int i = 0; i < WT_TABLE_SIZE; ++i) {
        float phase = (float)i / (float)WT_TABLE_SIZE;
        float rad = phase * 2.0f * 3.14159265f;

        // Table 0: Pure Sine
        wt_tables[0][i] = sinf(rad);

        // Table 1: Triangle / Warm
        wt_tables[1][i] = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);

        // Table 2: Organ / Dual Sine (1x + 2x + 3x)
        wt_tables[2][i] = 0.6f * sinf(rad) + 0.3f * sinf(2.0f * rad) + 0.15f * sinf(3.0f * rad);

        // Table 3: Formant "Aah" (periodic windowed 3x sine)
        wt_tables[3][i] = 0.5f * sinf(rad) + 0.4f * sinf(3.0f * rad) * (0.5f - 0.5f * cosf(rad));

        // Table 4: Formant "Ooh" (periodic windowed 5x sine)
        wt_tables[4][i] = 0.6f * sinf(rad) + 0.35f * sinf(5.0f * rad) * (0.5f - 0.5f * cosf(rad));

        // Table 5: Saw / Pulse Hybrid
        wt_tables[5][i] = (2.0f * phase - 1.0f) * 0.7f + ((phase < 0.5f) ? 0.3f : -0.3f);

        // Table 6: Hollow Square / Odd Harmonics (1x + 3x + 5x + 7x)
        wt_tables[6][i] = 0.6f * sinf(rad) + 0.25f * sinf(3.0f * rad) + 0.15f * sinf(5.0f * rad) + 0.1f * sinf(7.0f * rad);

        // Table 7: Metallic Chime (integer harmonic overtones: 1x + 3x + 6x + 8x)
        wt_tables[7][i] = 0.45f * sinf(rad) + 0.3f * sinf(3.0f * rad) + 0.15f * sinf(6.0f * rad) + 0.1f * sinf(8.0f * rad);
    }
    wt_tables_initialized = true;
}

// Fast 1-Cycle Table Lookup Sine with linear interpolation
static inline float table_sin(float p) {
    p = p - floorf(p);
    float s_pos = p * (float)WT_TABLE_SIZE;
    int idx0 = (int)s_pos & (WT_TABLE_SIZE - 1);
    int idx1 = (idx0 + 1) & (WT_TABLE_SIZE - 1);
    float frac = s_pos - (float)idx0;
    return wt_tables[0][idx0] + frac * (wt_tables[0][idx1] - wt_tables[0][idx0]);
}

// PolyBLEP anti-aliasing residual
static inline float poly_blep(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// Fast, branchless, algebraic soft saturation x / sqrt(1 + x^2)
// Completely linear near 0 (Taylor: x - 0.5*x^3), smoothly limiting to [-1, 1] without transcendental math
static inline float fast_tanh(float x) {
    return x / sqrtf(1.0f + x * x);
}

enum CoralVoiceMode : uint8_t {
    VOICE_MODE_POLY_RR = 0,
    VOICE_MODE_UNISON,
    VOICE_MODE_CHORD
};

// String physical model delay buffer (2048 samples per voice in DMAMEM for full 48kHz bass range)
static constexpr int STRING_DELAY_SIZE = 2048;
static DMAMEM float string_delay[NUM_CORAL_VOICES][STRING_DELAY_SIZE];

struct CoralVoice {
    enum EnvStage : uint8_t {
        ENV_IDLE = 0,
        ENV_ATTACK,
        ENV_DECAY
    };

    bool active = false;
    bool gate = false;
    bool from_midi = false;
    uint8_t midi_note = 60;
    float pitch_offset = 0.0f;
    float velocity = 1.0f;
    float env_level = 0.0f;
    EnvStage env_stage = ENV_IDLE;
    uint32_t sample_age = 0;

    // 6 Oscillator phase accumulators (used by DX7, VA, FM, Swarm)
    float phase[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // Resonant SVF filter states
    float svf_ic1 = 0.0f;
    float svf_ic2 = 0.0f;

    // TB-303 Diode Ladder filter states
    float ladder_s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float acid_env = 0.0f;

    // Modal resonator states (4 normalized state-variable bandpass sections)
    float modal_bp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float modal_lp[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // Pianoz tine bell transient envelope
    float tine_bell_env = 0.0f;

    // VOX (Vocal Formant Engine: 3 Formants, VOSIM/FOF)
    uint32_t vox_phase = 0;
    uint32_t vox_formant_phase[3] = {0, 0, 0};

    // String physical model state
    uint16_t string_head = 0;
    float string_lp = 0.0f;

    // Particle generator state
    int32_t particle_timer = 10;
    float particle_bp = 0.0f;
    float particle_lp = 0.0f;

    // Fast PRNG state (Galois LFSR)
    uint16_t lfsr = 0xACE1u;

    // Feedback memory
    float prev_op_fb[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    void trigger(float pitch_val, float vel = 1.0f, bool gate_high = true, bool is_midi = false) {
        active = true;
        gate = gate_high;
        from_midi = is_midi;
        pitch_offset = pitch_val;
        velocity = vel;
        env_level = 0.0f;
        env_stage = ENV_ATTACK;
        acid_env = 1.0f;
        tine_bell_env = 1.0f;
        sample_age = 0;
        string_lp = 0.0f;
        particle_timer = 4;
        vox_phase = 0;
        vox_formant_phase[0] = vox_formant_phase[1] = vox_formant_phase[2] = 0;
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
    }

    void reset() {
        active = false;
        gate = false;
        pitch_offset = 0.0f;
        env_level = 0.0f;
        env_stage = ENV_IDLE;
        acid_env = 0.0f;
        tine_bell_env = 0.0f;
        sample_age = 0;
        svf_ic1 = svf_ic2 = 0.0f;
        for (int i = 0; i < 4; ++i) {
            ladder_s[i] = 0.0f;
            modal_bp[i] = 0.0f;
            modal_lp[i] = 0.0f;
        }
        vox_phase = 0;
        vox_formant_phase[0] = vox_formant_phase[1] = vox_formant_phase[2] = 0;
        for (int i = 0; i < 6; ++i) {
            prev_op_fb[i] = 0.0f;
        }
        string_head = 0;
        string_lp = 0.0f;
        particle_timer = 10;
        particle_bp = particle_lp = 0.0f;
    }

    inline float next_noise() {
        uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
        lfsr = (lfsr >> 1) | (bit << 15);
        return (float)lfsr * (2.0f / 65535.0f) - 1.0f;
    }
};

struct CoralPhoneme {
    uint8_t formant_frequency[3];
    uint8_t formant_amplitude[3];
};

static const CoralPhoneme coral_vowels[9] = {
    { { 27,  40,  89 }, { 15,  13,  1 } }, // /a/ (Ah)
    { { 18,  51,  62 }, { 13,  12,  6 } }, // /e/ (Eh)
    { { 15,  69,  93 }, { 14,  12,  7 } }, // /i/ (Ee)
    { { 10,  84, 110 }, { 13,  10,  8 } }, // /o/ (Oh)
    { { 23,  44,  87 }, { 15,  12,  1 } }, // /u/ (Oo)
    { { 13,  29,  80 }, { 13,   8,  0 } }, // /ae/ (Cat)
    { {  6,  46,  81 }, { 12,   3,  0 } }, // /oe/ (Bird)
    { {  9,  51,  95 }, { 15,   3,  0 } }, // /y/ (French U)
    { {  6,  73,  99 }, {  7,   3,  14 } } // /uh/ (Up)
};

class AudioCoralStream : public AudioStream {
public:
    AudioCoralStream() : AudioStream(0, NULL), initialized(false) {}

    void init() {
        if (initialized) return;
        init_coral_wavetables();
        memset(string_delay, 0, sizeof(string_delay));
        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            voices[v].reset();
            voices[v].phase[0] = (float)v / (float)NUM_CORAL_VOICES;
        }
        initialized = true;
    }

    void set_engine(int eng) {
        if (eng >= 0 && eng < NUM_CORAL_ENGINES) {
            engine = eng;
        }
    }

    void set_dx7_patch(int p) {
        if (p >= 0 && p < 32) {
            dx7_patch = p;
        }
    }

    void set_pitch_base(float p_units) {
        pitch_base = p_units;
    }

    void set_harmonics(float h) {
        harmonics = constrain(h, 0.0f, 1.0f);
    }

    void set_timbre(float t) {
        timbre = constrain(t, 0.0f, 1.0f);
    }

    void set_color(float c) {
        color = constrain(c, 0.0f, 1.0f);
    }

    void set_attack(float a_norm) {
        if (a_norm <= 0.01f) {
            // Smooth 0.5ms anti-click ramp (24 samples at 48kHz)
            attack_step = 1.0f / (0.0005f * AUDIO_SAMPLE_RATE_EXACT);
        } else {
            float t = 0.001f * powf(2500.0f, a_norm);
            attack_step = 1.0f / (t * AUDIO_SAMPLE_RATE_EXACT);
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

    void set_gain(float g) {
        master_gain = g;
    }

    void set_voice_mode(CoralVoiceMode vm) {
        voice_mode = vm;
    }

    void trigger_cv(float pitch_offset_val, bool gate_high = true) {
        if (voice_mode == VOICE_MODE_UNISON) {
            for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
                // Symmetric detune in 128 units: -20, -10, 0, +10, +20 units (mean = 0)
                float detune = (float)(v - 2) * 10.0f;
                voices[v].trigger(pitch_offset_val + detune, 1.0f, gate_high);
            }
        } else if (voice_mode == VOICE_MODE_CHORD) {
            // Root (0), 5th (+7 semi), Octave (+12 semi), Maj10 (+16 semi), 12th (+19 semi)
            static const float chord_offsets[5] = {0.0f, 7.0f * 128.0f, 12.0f * 128.0f, 16.0f * 128.0f, 19.0f * 128.0f};
            for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
                voices[v].trigger(pitch_offset_val + chord_offsets[v], 0.9f, gate_high);
            }
        } else {
            int v = round_robin_index;
            round_robin_index = (round_robin_index + 1) % NUM_CORAL_VOICES;
            voices[v].trigger(pitch_offset_val, 1.0f, gate_high);
        }
    }

    void set_gate_cv(bool gate_state) {
        if (drone_mode) return;
        prev_gate_state = gate_state;
        if (!gate_state) {
            for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
                if (!voices[v].from_midi && voices[v].gate) {
                    voices[v].release();
                }
            }
        }
    }

    void midi_note_on(uint8_t note, uint8_t vel) {
        if (vel == 0) {
            midi_note_off(note);
            return;
        }
        float vel_f = (float)vel / 127.0f;
        // Direct MIDI: note 60 is middle C (C4), offset in 128 units per semitone
        float note_offset = (float)(note - 60) * 128.0f;

        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            if (voices[v].active && voices[v].midi_note == note) {
                voices[v].trigger(note_offset, vel_f, true, true);
                return;
            }
        }

        int free_voice = -1;
        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            if (!voices[v].active || voices[v].env_level < 0.001f) {
                free_voice = v;
                break;
            }
        }

        if (free_voice == -1) {
            uint32_t max_age = 0;
            for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
                if (voices[v].sample_age >= max_age) {
                    max_age = voices[v].sample_age;
                    free_voice = v;
                }
            }
        }

        if (free_voice >= 0) {
            voices[free_voice].midi_note = note;
            voices[free_voice].trigger(note_offset, vel_f, true, true);
        }
    }

    void midi_note_off(uint8_t note) {
        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            if (voices[v].active && voices[v].midi_note == note) {
                voices[v].release();
            }
        }
    }

    void midi_all_notes_off() {
        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            voices[v].silence();
        }
        prev_gate_state = false;
    }

    uint8_t get_active_voice_mask() const {
        uint8_t mask = 0;
        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            if (voices[v].active && (voices[v].env_level > 0.01f || voices[v].sample_age < (uint32_t)(2.0f * AUDIO_SAMPLE_RATE_EXACT))) {
                mask |= (1 << v);
            }
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

        const float inv_sr = 1.0f / AUDIO_SAMPLE_RATE_EXACT;

        float mix_buf[AUDIO_BLOCK_SAMPLES];
        memset(mix_buf, 0, sizeof(mix_buf));

        // Global block-rate macro copies
        float cur_harm = harmonics;
        float cur_timbre = timbre;
        float cur_color = color;
        float cur_decay_norm = release_norm;
        float cur_master_gain = master_gain;

        // Block-rate precomputations for Additive Engine (100% Tune-Safe Integer Harmonics)
        float additv_amps[8];
        if (engine == 1) {
            float roll_off = 0.4f + 2.6f * (1.0f - cur_timbre);
            float odd_even = cur_color * 2.0f - 1.0f;
            float center_h = 1.0f + cur_harm * 7.0f;
            float total_amp = 0.0001f;
            for (int h = 1; h <= 8; ++h) {
                float a = 1.0f / powf((float)h, roll_off);
                if (h % 2 == 0) a *= (1.0f + odd_even);
                // Resonant harmonic peak sweep (formant/drawbar peak sweeping over integer partials)
                float dist = fabsf((float)h - center_h);
                float boost = 1.0f + 2.5f * expf(-dist * dist * 0.8f);
                a *= boost;
                additv_amps[h - 1] = a;
                total_amp += a;
            }
            float inv_tot = 1.0f / total_amp;
            for (int h = 0; h < 8; ++h) {
                additv_amps[h] *= inv_tot;
            }
        }

        // DX7 active patch data
        const DX7PatchData& dx7_p = dx7_rom1a_patches[dx7_patch];
        float dx7_mod_scale = cur_timbre * 7.0f;
        float dx7_fb_scale = (float)dx7_p.feedback * (0.5f / 7.0f) * cur_color;
        float dx7_op_gain[6];
        float dx7_op_mod[6];
        if (engine == 3) {
            for (int k = 0; k < 6; ++k) {
                uint8_t lvl = dx7_p.level[k];
                float lin = (lvl > 15) ? powf(2.0f, (float)(lvl - 99) * 0.125f) : 0.0f;
                dx7_op_gain[k] = lin;
                dx7_op_mod[k] = lin * dx7_mod_scale;
            }
        }

        // Block-rate SVF coefficients for Engine 0 (V-ANLG)
        float va_svf_a1 = 0.0f, va_svf_a2 = 0.0f, va_svf_a3 = 0.0f;
        if (engine == 0) {
            float cutoff = 150.0f + 14000.0f * cur_color * cur_color;
            cutoff = constrain(cutoff, 30.0f, 16000.0f);
            float q = 0.7f + 2.5f * cur_color;
            float g = tanf(3.14159265f * cutoff * inv_sr);
            if (g > 0.95f) g = 0.95f;
            float k = 1.0f / q;
            va_svf_a1 = 1.0f / (1.0f + g * (g + k));
            va_svf_a2 = g * va_svf_a1;
            va_svf_a3 = g * va_svf_a2;
        }

        // Block-rate SVF coefficients for Engine 5 (SWARM)
        float swarm_svf_a1 = 0.0f, swarm_svf_a2 = 0.0f, swarm_svf_a3 = 0.0f;
        if (engine == 5) {
            float cutoff = 250.0f + 14000.0f * cur_color * cur_color;
            cutoff = constrain(cutoff, 40.0f, 16000.0f);
            float g = tanf(3.14159265f * cutoff * inv_sr);
            if (g > 0.95f) g = 0.95f;
            float k = 1.0f / 1.5f;
            swarm_svf_a1 = 1.0f / (1.0f + g * (g + k));
            swarm_svf_a2 = g * swarm_svf_a1;
            swarm_svf_a3 = g * swarm_svf_a2;
        }

        // Block-rate precomputations for VOX (Formant / Vowel Engine)
        uint32_t vox_f_inc[3];
        uint8_t  vox_f_amp[3];
        if (engine == 9) {
            float v_scaled = cur_timbre * 7.999f;
            size_t v_idx = (size_t)v_scaled;
            if (v_idx > 7) v_idx = 7;
            uint32_t balance = (uint32_t)((v_scaled - (float)v_idx) * 4096.0f);

            // Formant shift: 180 to 800 Hz base scale (deep male chest -> soprano / child)
            // Scaled dynamically to runtime sample rate AUDIO_SAMPLE_RATE_EXACT
            uint32_t formant_shift = (uint32_t)((180.0f + 620.0f * cur_color) * (96000.0f / AUDIO_SAMPLE_RATE_EXACT));

            for (size_t k = 0; k < 3; ++k) {
                uint32_t f_val = (coral_vowels[v_idx].formant_frequency[k] * (4096 - balance) +
                                  coral_vowels[v_idx + 1].formant_frequency[k] * balance);
                vox_f_inc[k] = (f_val * formant_shift) >> 4;

                uint32_t a_val = (coral_vowels[v_idx].formant_amplitude[k] * (4096 - balance) +
                                  coral_vowels[v_idx + 1].formant_amplitude[k] * balance) >> 12;
                if (k > 0) a_val = constrain((int)a_val + (int)(cur_harm * 4.0f), 0, 15);
                vox_f_amp[k] = (uint8_t)a_val;
            }
        }

        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
            CoralVoice& vc = voices[v];
            if (!vc.active && !drone_mode) continue;

            float total_pitch = pitch_base + vc.pitch_offset;
            float freq = SemitonesToRatio(total_pitch * (1.0f / 128.0f)) * C3;
            freq = constrain(freq, 10.0f, 16000.0f);
            float phase_inc = freq * inv_sr;
            uint32_t vox_fund_inc = (uint32_t)(freq * (4294967296.0f / AUDIO_SAMPLE_RATE_EXACT));

            // Per-voice block-rate precomputations
            float va_dt0 = 0.0f, va_dt1 = 0.0f, va_dt_oct = 0.0f, va_pw = 0.5f;
            if (engine == 0) {
                float detune_amt = cur_harm * 0.012f;
                va_dt0 = phase_inc * (1.0f - detune_amt);
                va_dt1 = phase_inc * (1.0f + detune_amt);
                va_dt_oct = phase_inc * 2.0f;
                va_pw = 0.08f + 0.84f * cur_timbre;
            }

            float sw_dt0 = 0.0f, sw_dt1 = 0.0f, sw_dt2 = 0.0f;
            if (engine == 5) {
                float detune_spread = 0.002f + 0.045f * cur_timbre;
                sw_dt0 = phase_inc * (1.0f - detune_spread);
                sw_dt1 = phase_inc;
                sw_dt2 = phase_inc * (1.0f + detune_spread);
            }

            float acid_omega = 0.0f, acid_res = 0.0f, acid_drive = 1.0f;
            if (engine == 6) {
                float env_sweep = vc.acid_env * 0.7f;
                float cut_hz = constrain(120.0f + 8000.0f * (cur_timbre * 0.5f + env_sweep * 0.5f), 40.0f, 14000.0f);
                acid_omega = constrain(6.2831853f * cut_hz * inv_sr, 0.0f, 0.85f);
                acid_res = 0.5f + 3.2f * cur_color;
                acid_drive = 1.0f + cur_harm * 2.2f;
            }

            float modal_damping[4] = {0.0f}, modal_f_rad[4] = {0.0f}, modal_weight[4] = {0.0f};
            bool modal_skip[4] = {false};
            if (engine == 8) {
                float q_base = drone_mode ? 4000.0f : (4.0f + 380.0f * cur_decay_norm * cur_decay_norm);
                static const float all_harm_ratios[4] = {1.0f, 2.0f, 3.0f, 4.0f};
                static const float odd_harm_ratios[4] = {1.0f, 3.0f, 5.0f, 7.0f};
                for (int m = 0; m < 4; ++m) {
                    float ratio = (1.0f - cur_harm) * all_harm_ratios[m] + cur_harm * odd_harm_ratios[m];
                    float mode_f = freq * ratio;
                    if (mode_f > 17500.0f) {
                        modal_skip[m] = true;
                        continue;
                    }
                    float mode_q = q_base / (1.0f + (float)m * (1.2f - 0.9f * cur_color));
                    modal_damping[m] = 1.0f / mode_q;
                    float f_rad = 3.14159265f * mode_f * inv_sr;
                    if (f_rad > 0.95f) f_rad = 0.95f;
                    modal_f_rad[m] = f_rad;
                    modal_weight[m] = 1.0f / (1.0f + (float)m * 0.35f);
                }
            }

            float str_damp = 0.08f, str_delay = 4.0f, str_fb = 0.8f;
            if (engine == 10) {
                str_damp = 0.08f + 0.88f * cur_timbre;
                float filter_delay = (1.0f - str_damp) / str_damp;
                str_delay = constrain(AUDIO_SAMPLE_RATE_EXACT / freq - filter_delay, 4.0f, (float)(STRING_DELAY_SIZE - 6));
                str_fb = drone_mode ? 1.0f : (0.80f + 0.198f * cur_decay_norm);
            }

            float part_damp = 0.25f, part_frad = 0.1f;
            if (engine == 11) {
                float q = 4.0f + 75.0f * cur_color;
                part_damp = 1.0f / q;
                part_frad = 3.14159265f * freq * inv_sr;
                if (part_frad > 0.95f) part_frad = 0.95f;
            }

            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                vc.sample_age++;

                // Dual Attack/Decay Envelope
                if (vc.env_stage == CoralVoice::ENV_ATTACK) {
                    vc.env_level += attack_step;
                    if (vc.env_level >= vc.velocity) {
                        vc.env_level = vc.velocity;
                        vc.env_stage = CoralVoice::ENV_DECAY;
                    }
                } else if (vc.env_stage == CoralVoice::ENV_DECAY) {
                    if (!drone_mode && !vc.gate) {
                        vc.env_level *= decay_coeff;
                        if (vc.env_level < 0.0001f) {
                            vc.env_level = 0.0f;
                            vc.active = false;
                            vc.env_stage = CoralVoice::ENV_IDLE;
                            vc.reset();
                        }
                    }
                }
                float env = drone_mode ? 1.0f : vc.env_level;

                float sample = 0.0f;

                switch (engine) {
                    // ========================================================
                    // 0: VIRTUAL ANALOG (PolyBLEP Saw/Pulse + Symmetric Unison + Resonant SVF)
                    // ========================================================
                    case 0: {
                        float p0 = vc.phase[0];
                        float p1 = vc.phase[1];
                        float p_oct = vc.phase[2];

                        // Osc 0 (PolyBLEP Saw & Pulse)
                        float saw0 = (2.0f * p0 - 1.0f) - poly_blep(p0, va_dt0);
                        float pulse_raw0 = (p0 < va_pw) ? 1.0f : -1.0f;
                        float u0 = p0 + (1.0f - va_pw);
                        if (u0 >= 1.0f) u0 -= 1.0f;
                        float pulse0 = pulse_raw0 + poly_blep(p0, va_dt0) - poly_blep(u0, va_dt0);
                        float osc0 = (1.0f - cur_timbre) * saw0 + cur_timbre * pulse0;

                        // Osc 1 (Symmetric Detuned Companion)
                        float saw1 = (2.0f * p1 - 1.0f) - poly_blep(p1, va_dt1);
                        float pulse_raw1 = (p1 < va_pw) ? 1.0f : -1.0f;
                        float u1 = p1 + (1.0f - va_pw);
                        if (u1 >= 1.0f) u1 -= 1.0f;
                        float pulse1 = pulse_raw1 + poly_blep(p1, va_dt1) - poly_blep(u1, va_dt1);
                        float osc1 = (1.0f - cur_timbre) * saw1 + cur_timbre * pulse1;

                        // Octave-Up Harmonic (2x phase_inc, never drops pitch!)
                        float saw_oct = (2.0f * p_oct - 1.0f) - poly_blep(p_oct, va_dt_oct);

                        float dual_mix = 0.5f * (osc0 + osc1);
                        float raw_osc = dual_mix * (1.0f - cur_harm * 0.3f) + saw_oct * (cur_harm * 0.4f);

                        // Resonant SVF Lowpass Filter using precomputed coefficients
                        float v3 = raw_osc - vc.svf_ic2;
                        float v1 = va_svf_a1 * vc.svf_ic1 + va_svf_a2 * v3;
                        float v2 = vc.svf_ic2 + va_svf_a2 * vc.svf_ic1 + va_svf_a3 * v3;
                        vc.svf_ic1 = 2.0f * v1 - vc.svf_ic1;
                        vc.svf_ic2 = 2.0f * v2 - vc.svf_ic2;
                        sample = v2 * env;

                        p0 += va_dt0;
                        if (p0 >= 1.0f) p0 -= 1.0f;
                        vc.phase[0] = p0;

                        p1 += va_dt1;
                        if (p1 >= 1.0f) p1 -= 1.0f;
                        vc.phase[1] = p1;

                        p_oct += va_dt_oct;
                        if (p_oct >= 1.0f) p_oct -= 1.0f;
                        vc.phase[2] = p_oct;
                        break;
                    }

                    // ========================================================
                    // 1: ADDITIVE (Strictly Integer Harmonic Partials + Formant Sweep)
                    // ========================================================
                    case 1: {
                        float p = vc.phase[0];
                        float acc = 0.0f;

                        // All partials are 100% exact integer multiples of fundamental
                        for (int h = 1; h <= 8; ++h) {
                            float part_p = p * (float)h;
                            acc += additv_amps[h - 1] * table_sin(part_p);
                        }
                        sample = acc * env;

                        p += phase_inc;
                        if (p >= 1.0f) p -= 1.0f;
                        vc.phase[0] = p;
                        break;
                    }

                    // ========================================================
                    // 2: WAVESHAPE (Sine -> Variable Drive -> Dual-Stage Wavefolder)
                    // ========================================================
                    case 2: {
                        float p = vc.phase[0];
                        float base_sine = table_sin(p);
                        float drive = 1.0f + 10.0f * cur_timbre;

                        // Zero-mean 2nd harmonic injection controlled by Color (even harmonics without DC offset!)
                        float oct_sine = table_sin(p * 2.0f);
                        float driven = (base_sine + cur_color * 0.5f * oct_sine) * drive;

                        // Multi-stage wavefolding with Stage 2 fold depth governed by Harmonics
                        float fold1 = sinf(driven);
                        float fold2 = sinf(fold1 * (1.2f + 2.8f * cur_harm));
                        sample = fast_tanh(fold2 * 1.3f) * env;

                        p += phase_inc;
                        if (p >= 1.0f) p -= 1.0f;
                        vc.phase[0] = p;
                        break;
                    }

                    // ========================================================
                    // 3: FMX7 (Genuine Yamaha DX7 6-Op FM: Selectable Patches)
                    // ========================================================
                    case 3: {
                        float p0 = vc.phase[0];
                        float p1 = vc.phase[1];
                        float p2 = vc.phase[2];
                        float p3 = vc.phase[3];
                        float p4 = vc.phase[4];
                        float p5 = vc.phase[5];

                        // Op 6 has feedback
                        float fb_in = vc.prev_op_fb[5] * dx7_fb_scale;
                        float op5_out = table_sin(p5 + fb_in);
                        vc.prev_op_fb[5] = op5_out;
                        float m5 = op5_out * dx7_op_mod[5];

                        // Algorithm evaluation
                        float raw_fm = 0.0f;
                        switch (dx7_p.algorithm) {
                            case 5: { // 3 Pairs: 6->5, 4->3, 2->1 (E.PIANO, TUB BELLS, HARPSICH)
                                float op3 = table_sin(p3);
                                float op1 = table_sin(p1);
                                float c5 = table_sin(p4 + m5) * dx7_op_gain[4];
                                float c3 = table_sin(p2 + op3 * dx7_op_mod[3]) * dx7_op_gain[2];
                                float c1 = table_sin(p0 + op1 * dx7_op_mod[1]) * dx7_op_gain[0];
                                raw_fm = (c1 + c3 + c5) * 0.85f;
                                break;
                            }
                            case 16: // 6-Op Stack: 6->5->4->3->2->1 (BASS 1, GUITAR 2, FLUTE 1)
                            case 17: {
                                float o4 = table_sin(p4 + m5) * dx7_op_mod[4];
                                float o3 = table_sin(p3 + o4) * dx7_op_mod[3];
                                float o2 = table_sin(p2 + o3) * dx7_op_mod[2];
                                float o1 = table_sin(p1 + o2) * dx7_op_mod[1];
                                raw_fm = table_sin(p0 + o1) * dx7_op_gain[0] * 1.5f;
                                break;
                            }
                            case 18: { // Op 6->5->4->3 (Carrier); Op 2 (Carrier); Op 1 (Carrier) (BRASS 3, PIANO 2)
                                float o4 = table_sin(p4 + m5) * dx7_op_mod[4];
                                float c3 = table_sin(p3 + o4) * dx7_op_gain[3];
                                float c2 = table_sin(p1) * dx7_op_gain[1];
                                float c1 = table_sin(p0) * dx7_op_gain[0];
                                raw_fm = (c1 + c2 + c3) * 0.85f;
                                break;
                            }
                            case 19: { // Op 6->5->4 (Carrier); Op 3 (Carrier); Op 2->1 (Carrier) (PIANO 1, PIPES 1)
                                float o4 = table_sin(p4 + m5) * dx7_op_mod[4];
                                float o1 = table_sin(p1) * dx7_op_mod[1];
                                float c4 = table_sin(p3 + o4) * dx7_op_gain[3];
                                float c3 = table_sin(p2) * dx7_op_gain[2];
                                float c1 = table_sin(p0 + o1) * dx7_op_gain[0];
                                raw_fm = (c1 + c3 + c4) * 0.85f;
                                break;
                            }
                            case 22: { // Op 6->5->4 & 3 (Carriers); Op 2 (Carrier); Op 1 (Carrier) (BRASS 1, 2)
                                float o4 = table_sin(p4 + m5) * dx7_op_mod[4];
                                float c4 = table_sin(p3 + o4) * dx7_op_gain[3];
                                float c3 = table_sin(p2 + o4) * dx7_op_gain[2];
                                float c2 = table_sin(p1) * dx7_op_gain[1];
                                float c1 = table_sin(p0) * dx7_op_gain[0];
                                raw_fm = (c1 + c2 + c3 + c4) * 0.7f;
                                break;
                            }
                            case 32: { // All 6 Carriers (E.ORGAN 1)
                                float sum_c = 0.0f;
                                for (int k = 0; k < 6; ++k) sum_c += table_sin(vc.phase[k]) * dx7_op_gain[k];
                                raw_fm = sum_c * 0.45f;
                                break;
                            }
                            default: { // Standard fallback
                                float op3 = table_sin(p3);
                                float op1 = table_sin(p1);
                                float c5 = table_sin(p4 + m5) * dx7_op_gain[4];
                                float c3 = table_sin(p2 + op3 * dx7_op_mod[3]) * dx7_op_gain[2];
                                float c1 = table_sin(p0 + op1 * dx7_op_mod[1]) * dx7_op_gain[0];
                                raw_fm = (c1 + c3 + c5) * 0.85f;
                                break;
                            }
                        }

                        sample = fast_tanh(raw_fm * 1.3f) * env;

                        // Phase accumulation across all 6 operators
                        for (int k = 0; k < 6; ++k) {
                            vc.phase[k] += phase_inc * dx7_p.ratio[k];
                            if (vc.phase[k] >= 1.0f) vc.phase[k] -= 1.0f;
                        }
                        break;
                    }

                    // ========================================================
                    // 4: WAVETABLE (Continuous Bi-Linear Morphing + Octave-Up Harmonic + Drive)
                    // ========================================================
                    case 4: {
                        float p = vc.phase[0];
                        float table_pos = cur_timbre * (WT_NUM_TABLES - 1);
                        int t_idx = (int)table_pos;
                        if (t_idx > WT_NUM_TABLES - 2) t_idx = WT_NUM_TABLES - 2;
                        float t_frac = table_pos - (float)t_idx;

                        float s_pos = p * (float)WT_TABLE_SIZE;
                        int s_idx0 = (int)s_pos;
                        int s_idx1 = (s_idx0 + 1) & (WT_TABLE_SIZE - 1);
                        float s_frac = s_pos - (float)s_idx0;

                        float s0_t0 = wt_tables[t_idx][s_idx0];
                        float s1_t0 = wt_tables[t_idx][s_idx1];
                        float val_t0 = s0_t0 + s_frac * (s1_t0 - s0_t0);

                        float s0_t1 = wt_tables[t_idx + 1][s_idx0];
                        float s1_t1 = wt_tables[t_idx + 1][s_idx1];
                        float val_t1 = s0_t1 + s_frac * (s1_t1 - s0_t1);

                        float raw_wt = val_t0 + t_frac * (val_t1 - val_t0);

                        // Octave-Up Harmonic (2x) presence + Phase distortion (never drops fundamental!)
                        float oct2 = table_sin(p * 2.0f);
                        float driven = raw_wt + cur_harm * 0.5f * oct2;
                        sample = fast_tanh(driven * (1.0f + cur_color * 1.5f)) * env;

                        p += phase_inc;
                        if (p >= 1.0f) p -= 1.0f;
                        vc.phase[0] = p;
                        break;
                    }

                    // ========================================================
                    // 5: WT_SWARM (Wavetable Detuned Unison Swarm / Supersaw Cluster)
                    // ========================================================
                    case 5: {
                        float p0 = vc.phase[0];
                        float p1 = vc.phase[1];
                        float p2 = vc.phase[2];

                        float saw0 = (2.0f * p0 - 1.0f) - poly_blep(p0, sw_dt0);
                        float saw1 = (2.0f * p1 - 1.0f) - poly_blep(p1, sw_dt1);
                        float saw2 = (2.0f * p2 - 1.0f) - poly_blep(p2, sw_dt2);

                        // Harmonics: saw-to-pulse morph across the swarm
                        float u1 = p1 + 0.5f;
                        if (u1 >= 1.0f) u1 -= 1.0f;
                        float pulse1 = (p1 < 0.5f ? 1.0f : -1.0f) + poly_blep(p1, sw_dt1) - poly_blep(u1, sw_dt1);
                        float osc1 = (1.0f - cur_harm * 0.7f) * saw1 + (cur_harm * 0.7f) * pulse1;

                        float sum = (saw0 + osc1 + saw2) * 0.4f;

                        // Resonant lowpass tone filter using precomputed swarm_svf_a1..a3
                        float v3 = sum - vc.svf_ic2;
                        float v1 = swarm_svf_a1 * vc.svf_ic1 + swarm_svf_a2 * v3;
                        float v2 = vc.svf_ic2 + swarm_svf_a2 * vc.svf_ic1 + swarm_svf_a3 * v3;
                        vc.svf_ic1 = 2.0f * v1 - vc.svf_ic1;
                        vc.svf_ic2 = 2.0f * v2 - vc.svf_ic2;

                        sample = fast_tanh(v2 * 1.5f) * env;

                        p0 += sw_dt0; if (p0 >= 1.0f) p0 -= 1.0f; vc.phase[0] = p0;
                        p1 += sw_dt1; if (p1 >= 1.0f) p1 -= 1.0f; vc.phase[1] = p1;
                        p2 += sw_dt2; if (p2 >= 1.0f) p2 -= 1.0f; vc.phase[2] = p2;
                        break;
                    }

                    // ========================================================
                    // 6: ACID (TB-303 Diode Ladder Emulation & Drive Punch)
                    // ========================================================
                    case 6: {
                        float p = vc.phase[0];
                        float dt = phase_inc;

                        float saw = (2.0f * p - 1.0f) - poly_blep(p, dt);
                        float u = p + 0.5f;
                        if (u >= 1.0f) u -= 1.0f;
                        float sqr = (p < 0.5f ? 1.0f : -1.0f) + poly_blep(p, dt) - poly_blep(u, dt);

                        // Waveform blend controlled by cur_timbre
                        float osc_303 = (1.0f - cur_timbre) * saw + cur_timbre * sqr;

                        // Harmonics controls pre-filter drive/overdrive
                        osc_303 = fast_tanh(osc_303 * acid_drive);

                        vc.acid_env *= 0.9982f;

                        float fb = acid_res * vc.ladder_s[3];
                        float in_node = osc_303 - fast_tanh(fb * 0.85f);

                        for (int st = 0; st < 4; ++st) {
                            vc.ladder_s[st] += acid_omega * (in_node - vc.ladder_s[st]);
                            in_node = vc.ladder_s[st];
                        }
                        sample = vc.ladder_s[3] * 1.5f * env;

                        p += phase_inc;
                        if (p >= 1.0f) p -= 1.0f;
                        vc.phase[0] = p;
                        break;
                    }

                    // ========================================================
                    // 7: PIANOZ (Rhodes / Wurli Physical Tine: Decay Governs Ring)
                    // ========================================================
                    case 7: {
                        float p_c = vc.phase[0];
                        float p_body = vc.phase[1];
                        float p_tine = vc.phase[2];

                        // Tine ring decay directly controlled by Decay parameter!
                        float tine_decay_rate = 0.985f + 0.0145f * cur_decay_norm;
                        vc.tine_bell_env *= tine_decay_rate;

                        // Fixed integer harmonic tine ratio (7.0f) - never slides inharmonically!
                        constexpr float tine_ratio = 7.0f;
                        float tine_depth = cur_harm * 3.5f * vc.tine_bell_env;
                        float tine_chime = table_sin(p_tine) * tine_depth;

                        float body_depth = 0.4f + 2.2f * cur_timbre;
                        float body_mod = table_sin(p_body) * body_depth;

                        float raw_ep = table_sin(p_c + body_mod + tine_chime);
                        // Physical tine decay allows acoustic ringing
                        sample = fast_tanh(raw_ep * (1.0f + 1.8f * cur_color)) * (drone_mode ? 1.0f : env);

                        p_c += phase_inc;
                        if (p_c >= 1.0f) p_c -= 1.0f;
                        p_body += phase_inc;
                        if (p_body >= 1.0f) p_body -= 1.0f;
                        p_tine += phase_inc * tine_ratio;
                        if (p_tine >= 1.0f) p_tine -= 1.0f;
                        vc.phase[0] = p_c;
                        vc.phase[1] = p_body;
                        vc.phase[2] = p_tine;
                        break;
                    }

                    // ========================================================
                    // 8: MODAL (Decay Directly Controls Modal Q & Ring Time)
                    // ========================================================
                    case 8: {
                        float excitation = 0.0f;
                        if (vc.sample_age < 28) {
                            float env_pulse = 1.0f - ((float)vc.sample_age / 28.0f);
                            float noise_val = vc.next_noise();
                            // Timbre controls mallet hardness: soft sine impulse to sharp noise plectrum
                            excitation = (noise_val * cur_timbre + (1.0f - cur_timbre)) * env_pulse * 0.5f;
                        }

                        float acc_modes = 0.0f;
                        for (int m = 0; m < 4; ++m) {
                            if (modal_skip[m]) continue;
                            float hp = excitation - modal_damping[m] * vc.modal_bp[m] - vc.modal_lp[m];
                            vc.modal_bp[m] += modal_f_rad[m] * hp;
                            vc.modal_lp[m] += modal_f_rad[m] * vc.modal_bp[m];
                            acc_modes += vc.modal_bp[m] * modal_weight[m];
                        }

                        // Resonator naturally rings out, smoothly enveloped to absolute silence
                        sample = fast_tanh(acc_modes * 1.6f) * env;
                        break;
                    }

                    // ========================================================
                    // 9: VOX (Vocal Formant Engine: 3 Formants, 9 Vowels, FOF/VOSIM)
                    // ========================================================
                    case 9: {
                        uint32_t old_phase = vc.vox_phase;
                        vc.vox_phase += vox_fund_inc;

                        vc.vox_formant_phase[0] += vox_f_inc[0];
                        vc.vox_formant_phase[1] += vox_f_inc[1];
                        vc.vox_formant_phase[2] += vox_f_inc[2];

                        size_t ph0 = (vc.vox_formant_phase[0] >> 24) & 0xf0;
                        size_t ph1 = (vc.vox_formant_phase[1] >> 24) & 0xf0;
                        size_t ph2 = (vc.vox_formant_phase[2] >> 24) & 0xf0;

                        int32_t s0 = braids::wav_formant_sine[ph0 | vox_f_amp[0]];
                        int32_t s1 = braids::wav_formant_sine[ph1 | vox_f_amp[1]];
                        int32_t s2 = braids::wav_formant_square[ph2 | vox_f_amp[2]];
                        int32_t vox_sum = s0 + s1 + s2;

                        // Glottal excitation envelope: decays over each pitch period
                        vox_sum = (vox_sum * (int32_t)(255 - (vc.vox_phase >> 24))) >> 8;

                        // Vocal cord pitch sync: reset formant phases on fundamental cycle reset
                        if (vc.vox_phase < old_phase) {
                            vc.vox_formant_phase[0] = 0;
                            vc.vox_formant_phase[1] = 0;
                            vc.vox_formant_phase[2] = 0;
                        }

                        sample = ((float)vox_sum * (1.0f / 32768.0f)) * env;
                        break;
                    }

                    // ========================================================
                    // 10: STRNGS (Karplus-Strong: Decay Governs String Feedback)
                    // ========================================================
                    case 10: {
                        float r_pos = (float)vc.string_head - str_delay;
                        while (r_pos < 0.0f) r_pos += (float)STRING_DELAY_SIZE;
                        int idx0 = (int)r_pos & (STRING_DELAY_SIZE - 1);
                        int idx1 = (idx0 + 1) & (STRING_DELAY_SIZE - 1);
                        float frac = r_pos - (float)idx0;
                        float delayed = string_delay[v][idx0] + frac * (string_delay[v][idx1] - string_delay[v][idx0]);

                        vc.string_lp += str_damp * (delayed - vc.string_lp);
                        float fb = vc.string_lp * str_fb;

                        float excitation = 0.0f;
                        if (vc.sample_age < 32) {
                            float pick_t = (float)vc.sample_age / 32.0f;
                            float noise_val = vc.next_noise();
                            // Harmonics controls pick excitation texture: noise plectrum vs smooth pulse
                            excitation = (noise_val * cur_harm + (1.0f - cur_harm)) * sinf(pick_t * 3.14159265f) * 0.85f;
                        }

                        string_delay[v][vc.string_head] = fb + excitation;
                        vc.string_head = (vc.string_head + 1) & (STRING_DELAY_SIZE - 1);

                        // Color adds warm acoustic body resonance without detuning
                        float body_out = delayed + cur_color * 0.4f * fast_tanh(delayed * 2.0f);
                        sample = fast_tanh(body_out * 1.5f) * env;
                        break;
                    }

                    // ========================================================
                    // 11: PARTCL (Particle / Granular Dust Cloud / Rain)
                    // ========================================================
                    case 11: {
                        float grain_impulse = 0.0f;
                        vc.particle_timer--;
                        if (vc.particle_timer <= 0) {
                            grain_impulse = (vc.next_noise() > 0.0f ? 1.0f : -1.0f);
                            // Timbre controls particle grain density / burst rate
                            int min_interval = 4;
                            int max_interval = (int)(200.0f * (1.0f - cur_timbre * 0.9f)) + 8;
                            vc.particle_timer = min_interval + (abs((int)(vc.next_noise() * 1000.0f)) % max_interval);
                        }

                        float hp = grain_impulse - part_damp * vc.particle_bp - vc.particle_lp;
                        vc.particle_bp += part_frad * hp;
                        vc.particle_lp += part_frad * vc.particle_bp;

                        // Harmonics controls resonator presence drive and saturation
                        float res_out = vc.particle_bp * (1.6f + 2.2f * cur_color);
                        sample = fast_tanh(res_out * (1.0f + cur_harm * 1.5f)) * env;
                        break;
                    }

                    default:
                        break;
                }

                mix_buf[i] += sample;
            }
        }

        // Output stage: clean algebraic soft saturation (never clips 16-bit range) + master gain calibrated to Braids
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            float s = mix_buf[i];
            float out_f = fast_tanh(s) * cur_master_gain;
            block->data[i] = (int16_t)constrain(out_f, -32767.0f, 32767.0f);
        }

        transmit(block, 0);
        release(block);
    }

private:
    CoralVoice voices[NUM_CORAL_VOICES];
    int round_robin_index = 0;
    int engine = 0;
    int dx7_patch = 10; // Default: E.PIANO 1
    float pitch_base = 0.0f;
    float harmonics = 0.5f;
    float timbre = 0.5f;
    float color = 0.5f;
    float attack_step = 1.0f;
    float release_norm = 0.7f;
    float decay_coeff = 0.999f;
    float master_gain = 6500.0f; // 0dB sweet spot = normal audio line level (~1.4Vpp)
    bool drone_mode = false;
    bool prev_gate_state = false;
    CoralVoiceMode voice_mode = VOICE_MODE_POLY_RR;
    bool initialized = false;
};

static DMAMEM AudioCoralStream coral_global_stream;
static DMAMEM AudioPassthrough<MONO> coral_dummy_input;

// ============================================================================
// Hemisphere Audio Applet: CoralVoiceApplet
// ============================================================================
class CoralVoiceApplet : public HemisphereAudioApplet {
public:
    static constexpr const char* applet_name_() { return "Coral"; }
    static constexpr const uint8_t* applet_icon_() { return nullptr; }

    const char* applet_name() const override { return applet_name_(); }
    const uint8_t* applet_icon() const override { return applet_icon_(); }

    static inline uint32_t hash_seed(uint32_t k) {
        k ^= k >> 16;
        k *= 0x85ebca6b;
        k ^= k >> 13;
        k *= 0xc2b2ae35;
        k ^= k >> 16;
        return k;
    }

    void decode_seed(uint32_t s, int8_t& out_eng, int8_t& out_hrm, int8_t& out_tmb, int8_t& out_col, int8_t& out_rel) {
        uint32_t h = hash_seed(s);
        out_eng = (int8_t)(h % NUM_CORAL_ENGINES);

        h = hash_seed(h ^ 0x9e3779b9);
        out_hrm = (int8_t)(h % 101);

        h = hash_seed(h ^ 0x6f2dec5f);
        out_tmb = (int8_t)(h % 101);

        h = hash_seed(h ^ 0x517cc1b7);
        out_col = (int8_t)(h % 101);

        h = hash_seed(h ^ 0x27d4eb2d);
        out_rel = (int8_t)(5 + (h % 96));
    }

    void Start() override {
        coral_global_stream.init();
        coral_global_stream.set_engine(engine_idx);
        coral_global_stream.set_dx7_patch(dx7_patch_idx);
        coral_global_stream.set_pitch_base((float)pitch);
        coral_global_stream.set_harmonics((float)harmonics * 0.01f);
        coral_global_stream.set_timbre((float)timbre * 0.01f);
        coral_global_stream.set_color((float)color * 0.01f);
        coral_global_stream.set_attack((float)attack_time * 0.01f);
        coral_global_stream.set_release((float)release_time * 0.01f);
        update_gain();
        coral_global_stream.set_voice_mode((CoralVoiceMode)voice_mode_idx);
    }

    void Unload() override {
        coral_global_stream.midi_all_notes_off();
        midi_pitch_bend_units = 0.0f;
        AllowRestart();
    }

    void update_gain() {
        float eff_db = (float)level_db;
        if (level_cv.enabled()) {
            eff_db += level_cv.InF() * 24.0f;
        }
        eff_db = constrain(eff_db, -60.0f, 18.0f);
        // 6500.0f gives ~4,600 counts for single voice at 0dB (standard ~1.4Vpp line/headphone audio level)
        float lin = dbToScalar(eff_db) * 6500.0f;
        coral_global_stream.set_gain(lin);
    }

    void RandomizeSeed() {
        base_seed = (uint32_t)(micros() ^ ((uint32_t)random(1, 1000000000) + base_seed * 1664525UL + 1013904223UL));
        seed = (int16_t)(base_seed % 1000);
        decode_seed(base_seed, engine_idx, harmonics, timbre, color, release_time);
        dx7_patch_idx = (int8_t)(base_seed % 32);
        coral_global_stream.set_dx7_patch(dx7_patch_idx);
        coral_global_stream.trigger_cv(0.0f, false);
    }

    void AuxButton() override {
        if (EditMode()) {
            CancelEdit();
            return;
        }
        RandomizeSeed();
    }

    void OnMIDIMessage(uint8_t channel, uint8_t message, uint8_t data1, uint8_t data2) override {
        // midi_channel: 0 = OFF, 1..16 = CH 1..16, 17 = OMNI (default)
        if (midi_channel == 0) return;
        if (midi_channel >= 1 && midi_channel <= 16 && channel != midi_channel) return;

        uint8_t status = message & 0xF0;
        if (status == 0x90) {
            if (data2 > 0) {
                has_direct_midi = true;
                coral_global_stream.midi_note_on(data1, data2);
            } else {
                coral_global_stream.midi_note_off(data1);
            }
        } else if (status == 0x80) {
            coral_global_stream.midi_note_off(data1);
        } else if (status == 0xB0) {
            if (data1 == 1) {
                timbre = (int8_t)map(data2, 0, 127, 0, 100);
            } else if (data1 == 74) {
                color = (int8_t)map(data2, 0, 127, 0, 100);
            } else if (data1 == 71) {
                harmonics = (int8_t)map(data2, 0, 127, 0, 100);
            } else if (data1 == 123 || data1 == 120) {
                coral_global_stream.midi_all_notes_off();
            }
        } else if (message == 0xFC) {
            coral_global_stream.midi_all_notes_off();
        } else if (status == 0xE0) {
            int bend = (data2 << 7) | data1;
            midi_pitch_bend_units = ((float)(bend - 8192) / 8192.0f) * (2.0f * 128.0f);
        }
    }

    void Controller() override {
        bool is_clock_running = HS::clock_m.IsRunning();
        if (was_clock_running && !is_clock_running) {
            coral_global_stream.midi_all_notes_off();
        }
        was_clock_running = is_clock_running;

        // Trigger via hardware/internal CV clock only if direct MIDI isn't actively playing,
        // or if MIDI is set to OFF
        if (trig_cv.Clock()) {
            if (!has_direct_midi || midi_channel == 0) {
                coral_global_stream.trigger_cv(0.0f, trig_cv.Gate());
            }
        }
        coral_global_stream.set_gate_cv(trig_cv.Gate());

        int eff_engine = engine_idx;
        int eff_harm = harmonics;
        int eff_timbre = timbre;
        int eff_color = color;
        int eff_release = release_time;

        if (seed_cv.enabled()) {
            float in_f = seed_cv.InF();
            filtered_seed_cv += 0.2f * (in_f - filtered_seed_cv);
            float scaled = filtered_seed_cv * 100.0f;
            int target_step = (int)floorf(scaled);
            float diff = scaled - (float)last_cv_step;
            if (diff > 1.2f || diff < -0.2f) {
                last_cv_step = target_step;
            }
            uint32_t active_seed = (uint32_t)(base_seed + last_cv_step);
            int8_t s_eng, s_hrm, s_tmb, s_col, s_rel;
            decode_seed(active_seed, s_eng, s_hrm, s_tmb, s_col, s_rel);
            eff_engine = s_eng;
            eff_harm = s_hrm;
            eff_timbre = s_tmb;
            eff_color = s_col;
            eff_release = s_rel;
        }

        if (engine_cv.enabled()) {
            eff_engine += (int)(engine_cv.InF() * (NUM_CORAL_ENGINES - 1));
            eff_engine = constrain(eff_engine, 0, NUM_CORAL_ENGINES - 1);
        }
        coral_global_stream.set_engine(eff_engine);
        coral_global_stream.set_dx7_patch(dx7_patch_idx);

        // Safeguard against double-pitching when using direct MIDI
        float pitch_cv_offset = 0.0f;
        if (pitch_cv.enabled()) {
            bool is_internal_midi_note = (pitch_cv.source_type() == CVInputMap::TYPE_MIDI);
            // If user assigned pitch_cv to internal MIDI map while direct MIDI is playing, ignore to prevent octave jump
            if (!(has_direct_midi && midi_channel != 0 && is_internal_midi_note)) {
                pitch_cv_offset = (float)pitch_cv.In();
            }
        }
        float total_pitch = (float)pitch + pitch_cv_offset + midi_pitch_bend_units;
        coral_global_stream.set_pitch_base(total_pitch);

        float h_norm = constrain((float)eff_harm * 0.01f + harm_cv.InF(), 0.0f, 1.0f);
        coral_global_stream.set_harmonics(h_norm);

        float t_norm = constrain((float)eff_timbre * 0.01f + timbre_cv.InF(), 0.0f, 1.0f);
        coral_global_stream.set_timbre(t_norm);

        float c_norm = constrain((float)eff_color * 0.01f + color_cv.InF(), 0.0f, 1.0f);
        coral_global_stream.set_color(c_norm);

        float a_norm = constrain((float)attack_time * 0.01f + attack_cv.InF(), 0.0f, 1.0f);
        coral_global_stream.set_attack(a_norm);

        float r_norm = constrain((float)eff_release * 0.01f + release_cv.InF(), 0.0f, 1.0f);
        coral_global_stream.set_release(r_norm);

        update_gain();

        coral_global_stream.set_voice_mode((CoralVoiceMode)voice_mode_idx);
    }

    void View() override {
        gfxHeader(applet_name());

        // 5-Voice activity indicator dots in the header
        uint8_t v_mask = coral_global_stream.get_active_voice_mask();
        for (int v = 0; v < NUM_CORAL_VOICES; ++v) {
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
            // ── Page 1: Sound Engines & Macros (Eng, Pitch, Harm/Patch, Tmb, Col)
            // Row 1: Engine
            gfxPrint(1, 15, "Eng:");
            gfxStartCursor(23, 15);
            if (engine_idx >= 0 && engine_idx < NUM_CORAL_ENGINES) {
                gfxPrint(coral_engine_names[engine_idx]);
            }
            gfxEndCursor(cursor == CURSOR_ENGINE);

            gfxStartCursor(53, 15);
            gfxPrint(engine_cv);
            gfxEndCursor(cursor == CURSOR_ENGINE_CV, false, engine_cv.InputName());

            // Row 2: Pitch (Semi note indicator / Fine Hz) + Pitch CV (1V/Oct)
            gfxStartCursor(1, 25);
            gfxPrintTuningIndicator(pitch);
            gfxEndCursor(cursor == CURSOR_PITCH_SEMI);

            gfxStartCursor(11, 25);
            gfxPrintPitchHz(pitch);
            gfxEndCursor(cursor == CURSOR_PITCH_FINE);

            gfxStartCursor(53, 25);
            gfxPrint(pitch_cv);
            gfxEndCursor(cursor == CURSOR_PITCH_CV, false, pitch_cv.InputName(), "1V/Oct");

            // Row 3: DX7 Patch Selector (in FMX7 mode) or Harmonics (in other modes)
            if (engine_idx == 3) {
                gfxPrint(1, 35, "Ptc:");
                gfxStartCursor(22, 35);
                graphics.printf("%2d", dx7_patch_idx + 1);
                gfxEndCursor(cursor == CURSOR_HARMONICS);

                gfxStartCursor(37, 35);
                graphics.printf("%-6.6s", dx7_rom1a_patches[dx7_patch_idx].name);
                gfxEndCursor(false);
            } else {
                gfxPrint(1, 35, "Hrm:");
                gfxStartCursor(25, 35);
                graphics.printf("%3d", harmonics);
                gfxEndCursor(cursor == CURSOR_HARMONICS);
            }

            gfxStartCursor(53, 35);
            gfxPrint(harm_cv);
            gfxEndCursor(cursor == CURSOR_HARMONICS_CV, false, harm_cv.InputName());

            // Row 4: Timbre
            gfxPrint(1, 45, "Tmb:");
            gfxStartCursor(25, 45);
            graphics.printf("%3d", timbre);
            gfxEndCursor(cursor == CURSOR_TIMBRE);

            gfxStartCursor(53, 45);
            gfxPrint(timbre_cv);
            gfxEndCursor(cursor == CURSOR_TIMBRE_CV, false, timbre_cv.InputName());

            // Row 5: Color
            gfxPrint(1, 55, "Col:");
            gfxStartCursor(25, 55);
            graphics.printf("%3d", color);
            gfxEndCursor(cursor == CURSOR_COLOR);

            gfxStartCursor(53, 55);
            gfxPrint(color_cv);
            gfxEndCursor(cursor == CURSOR_COLOR_CV, false, color_cv.InputName());

            gfxPrint(58, 2, "1");
        } else {
            // ── Page 2: Envelopes, Polyphony & Modulations ──────────────────
            // Row 1: Attack + Attack CV
            gfxPrint(1, 15, "Atk:");
            gfxStartCursor(25, 15);
            graphics.printf("%3d", attack_time);
            gfxEndCursor(cursor == CURSOR_ATTACK);

            gfxStartCursor(53, 15);
            gfxPrint(attack_cv);
            gfxEndCursor(cursor == CURSOR_ATTACK_CV, false, attack_cv.InputName());

            // Row 2: Decay / Release + Decay CV
            gfxPrint(1, 25, "Dec:");
            gfxStartCursor(25, 25);
            if (release_time >= 100) graphics.print("INF");
            else graphics.printf("%3d", release_time);
            gfxEndCursor(cursor == CURSOR_RELEASE);

            gfxStartCursor(53, 25);
            gfxPrint(release_cv);
            gfxEndCursor(cursor == CURSOR_RELEASE_CV, false, release_cv.InputName());

            // Row 3: Polyphony Mode & MIDI Channel
            gfxPrint(1, 35, "Ply:");
            gfxStartCursor(21, 35);
            if (voice_mode_idx == VOICE_MODE_POLY_RR) graphics.print("RR5");
            else if (voice_mode_idx == VOICE_MODE_UNISON) graphics.print("UNI");
            else graphics.print("CHD");
            gfxEndCursor(cursor == CURSOR_VOICE_MODE);

            gfxPrint(38, 35, "M:");
            gfxStartCursor(47, 35);
            if (midi_channel == 0) graphics.print("OFF");
            else if (midi_channel == 17) graphics.print("OMN");
            else graphics.printf("%2d", midi_channel);
            gfxEndCursor(cursor == CURSOR_MIDI_CHANNEL);

            // Row 4: Trigger CV & dB Level Control
            gfxPrint(1, 45, "Trg:");
            gfxStartCursor(20, 45);
            gfxPrint(trig_cv);
            gfxEndCursor(cursor == CURSOR_TRIG_CV, false, trig_cv.InputName(), "Gate");

            gfxStartCursor(44, 45);
            graphics.printf("%+2ddB", level_db);
            gfxEndCursor(cursor == CURSOR_LEVEL);

            // Row 5: Seed & Randomize
            gfxPrint(1, 55, "Sd:");
            gfxStartCursor(15, 55);
            int disp_seed = seed_cv.enabled() ? (int)((base_seed + last_cv_step) % 1000) : seed;
            if (disp_seed < 0) disp_seed = (disp_seed % 1000 + 1000) % 1000;
            graphics.printf("%3d", disp_seed);
            gfxEndCursor(cursor == CURSOR_SEED);

            gfxStartCursor(38, 55);
            gfxPrint("[RND]");
            gfxEndCursor(cursor == CURSOR_RANDOMIZE);

            gfxPrint(58, 2, "2");
        }

        gfxDisplayInputMapEditor();
    }

    void OnButtonPress() override {
        if (CheckEditInputMapPress(
            cursor,
            IndexedInput(CURSOR_ENGINE_CV,    engine_cv),
            IndexedInput(CURSOR_PITCH_CV,     pitch_cv),
            IndexedInput(CURSOR_HARMONICS_CV, harm_cv),
            IndexedInput(CURSOR_TIMBRE_CV,    timbre_cv),
            IndexedInput(CURSOR_COLOR_CV,     color_cv),
            IndexedInput(CURSOR_ATTACK_CV,    attack_cv),
            IndexedInput(CURSOR_RELEASE_CV,   release_cv),
            IndexedInput(CURSOR_TRIG_CV,      trig_cv)
        )) return;

        if (cursor == CURSOR_RANDOMIZE) {
            RandomizeSeed();
            return;
        }

        CursorToggle();
    }

    void OnEncoderMove(int direction) override {
        if (!EditMode()) {
            MoveCursor(cursor, direction, CURSOR_LAST);
            return;
        }
        if (EditSelectedInputMap(direction)) return;

        switch (cursor) {
            case CURSOR_ENGINE:
                engine_idx = constrain(engine_idx + direction, 0, NUM_CORAL_ENGINES - 1);
                coral_global_stream.set_engine(engine_idx);
                break;
            case CURSOR_ENGINE_CV:
                engine_cv.ChangeSource(direction);
                break;
            case CURSOR_PITCH_SEMI:
                pitch = constrain(pitch + direction * 128, MIN_PITCH, MAX_PITCH);
                break;
            case CURSOR_PITCH_FINE:
                pitch = constrain(pitch + direction * 1, MIN_PITCH, MAX_PITCH);
                break;
            case CURSOR_PITCH_CV:
                pitch_cv.ChangeSource(direction);
                break;
            case CURSOR_HARMONICS:
                if (engine_idx == 3) {
                    dx7_patch_idx = constrain(dx7_patch_idx + direction, 0, 31);
                    coral_global_stream.set_dx7_patch(dx7_patch_idx);
                } else {
                    harmonics = constrain(harmonics + direction, 0, 100);
                }
                break;
            case CURSOR_HARMONICS_CV:
                harm_cv.ChangeSource(direction);
                break;
            case CURSOR_TIMBRE:
                timbre = constrain(timbre + direction, 0, 100);
                break;
            case CURSOR_TIMBRE_CV:
                timbre_cv.ChangeSource(direction);
                break;
            case CURSOR_COLOR:
                color = constrain(color + direction, 0, 100);
                break;
            case CURSOR_COLOR_CV:
                color_cv.ChangeSource(direction);
                break;
            case CURSOR_ATTACK:
                attack_time = constrain(attack_time + direction, 0, 100);
                break;
            case CURSOR_ATTACK_CV:
                attack_cv.ChangeSource(direction);
                break;
            case CURSOR_RELEASE:
                release_time = constrain(release_time + direction, 0, 100);
                break;
            case CURSOR_RELEASE_CV:
                release_cv.ChangeSource(direction);
                break;
            case CURSOR_VOICE_MODE:
                voice_mode_idx = (uint8_t)constrain((int)voice_mode_idx + direction, 0, 2);
                coral_global_stream.set_voice_mode((CoralVoiceMode)voice_mode_idx);
                break;
            case CURSOR_MIDI_CHANNEL:
                midi_channel = (uint8_t)constrain((int)midi_channel + direction, 0, 17);
                break;
            case CURSOR_TRIG_CV:
                trig_cv.ChangeSource(direction);
                break;
            case CURSOR_LEVEL:
                level_db = constrain(level_db + direction, -48, 18);
                update_gain();
                break;
            case CURSOR_SEED:
                seed = (seed + direction + 1000) % 1000;
                decode_seed((uint32_t)(base_seed + seed), engine_idx, harmonics, timbre, color, release_time);
                coral_global_stream.trigger_cv(0.0f, false);
                break;
            case CURSOR_RANDOMIZE:
                RandomizeSeed();
                break;
            default:
                break;
        }
    }

    void SetHelp() override {
        help[HELP_DIGITAL1] = "Clock/Gate";
        help[HELP_CV1]      = "1V/Oct";
        help[HELP_CV2]      = "Mod CV";
        help[HELP_OUT1]     = "Audio L";
        help[HELP_OUT2]     = "Audio R";
        help[HELP_EXTRA1]   = "MIDI Ch / Poly";
        help[HELP_EXTRA2]   = "Aux: Rnd Seed";
    }

    AudioStream* OutputStream() override {
        return &coral_global_stream;
    }

    AudioStream* InputStream() override {
        return &coral_dummy_input;
    }

#define CORAL_PARAMS engine_idx, pitch, harmonics, timbre, color, attack_time, release_time

    void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
        data[0] = PackPackables(CORAL_PARAMS);
        data[1] = PackPackables(engine_cv, pitch_cv, harm_cv, timbre_cv);
        data[2] = PackPackables(color_cv, attack_cv, release_cv, level_cv);
        data[3] = PackPackables(pack<2>(voice_mode_idx), level_db, pack<5>(dx7_patch_idx), pack<5>(midi_channel), pack<10>(seed), trig_cv);
    }

    void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
            return;
        }
        UnpackPackables(data[0], CORAL_PARAMS);
        UnpackPackables(data[1], engine_cv, pitch_cv, harm_cv, timbre_cv);
        UnpackPackables(data[2], color_cv, attack_cv, release_cv, level_cv);
        UnpackPackables(data[3], pack<2>(voice_mode_idx), level_db, pack<5>(dx7_patch_idx), pack<5>(midi_channel), pack<10>(seed), trig_cv);
        midi_channel = constrain(midi_channel, (uint8_t)0, (uint8_t)17);
    }

private:
    static constexpr int MAX_PITCH = 7 * 12 * 128;
    static constexpr int MIN_PITCH = -3 * 12 * 128;

    enum Cursor : int8_t {
        CURSOR_ENGINE,
        CURSOR_ENGINE_CV,
        CURSOR_PITCH_SEMI,
        CURSOR_PITCH_FINE,
        CURSOR_PITCH_CV,
        CURSOR_HARMONICS,
        CURSOR_HARMONICS_CV,
        CURSOR_TIMBRE,
        CURSOR_TIMBRE_CV,
        CURSOR_COLOR,
        CURSOR_COLOR_CV,

        PAGE2_START,
        CURSOR_ATTACK = PAGE2_START,
        CURSOR_ATTACK_CV,
        CURSOR_RELEASE,
        CURSOR_RELEASE_CV,
        CURSOR_VOICE_MODE,
        CURSOR_MIDI_CHANNEL,
        CURSOR_TRIG_CV,
        CURSOR_LEVEL,
        CURSOR_SEED,
        CURSOR_RANDOMIZE,

        CURSOR_LAST = CURSOR_RANDOMIZE
    };

    int8_t cursor = CURSOR_ENGINE;

    int8_t  engine_idx     = 0;
    int16_t pitch          = 1 * 12 * 128; // C4 default (1536)
    int8_t  harmonics      = 50;
    int8_t  timbre         = 50;
    int8_t  color          = 50;
    int8_t  attack_time    = 0;
    int8_t  release_time   = 70;
    uint8_t voice_mode_idx = 0;
    int8_t  level_db       = 0;   // 0 dB standard default matching Braids / OscApplet
    uint8_t dx7_patch_idx  = 10;  // 10 = E.PIANO 1
    uint8_t midi_channel   = 17;  // 0 = OFF, 1..16 = CH 1..16, 17 = OMNI (default)

    float    midi_pitch_bend_units = 0.0f;
    uint16_t seed          = 0;
    uint32_t base_seed     = 0;
    float    filtered_seed_cv = 0.0f;
    int      last_cv_step  = 0;
    bool     has_direct_midi = false;
    bool     was_clock_running = false;

    CVInputMap      engine_cv;
    CVInputMap      pitch_cv;
    CVInputMap      harm_cv;
    CVInputMap      timbre_cv;
    CVInputMap      color_cv;
    CVInputMap      attack_cv;
    CVInputMap      release_cv;
    CVInputMap      level_cv;
    CVInputMap      seed_cv;
    DigitalInputMap trig_cv;
};
