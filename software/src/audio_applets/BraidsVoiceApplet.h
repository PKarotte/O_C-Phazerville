#pragma once

// ============================================================================
// BraidsVoiceApplet: Official Faithful Mutable Instruments Braids Polyphonic Synth
// ============================================================================
// Dual-Mono Independent Architecture:
//    Left side  (hemisphere 0) -> Audio L (DAC 1), Pitch CV 1, Gate Digital 1
//    Right side (hemisphere 1) -> Audio R (DAC 2), Pitch CV 3, Gate Digital 3
//
// Features:
//  - All 39 Official Mutable Instruments Braids Macro Oscillator Models
//  - 5-Voice Polyphony: MONO, RR5 (Round-Robin), UNISON (fat detune), CHORD (8 chord families)
//  - Dual Envelope: Attack (snap -> slow ramp) + Decay/Release (snap -> tail -> INF drone)
//  - Physical/Percussive Strike support on all physical models (Bell, Drum, Pluck, etc.)
//  - Dual-Source Input: ALL (CV + MIDI simultaneously), CV only, MID only, or TGL (gate toggle)
//  - Full MIDI Support: Omni/Channels 1-16, 14-bit Pitch Bend, Velocity mapping, Panic/Stop
//  - Live OLED Visuals: voice activity dots, live note flash on M:, header MIDI indicator
// ============================================================================

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>
#include "braids/macro_oscillator.h"
#include "HSUtils.h"
#include "HemisphereAudioApplet.h"
#include "dsputils_arm.h"
#include "CVInputMap.h"
#include "Audio/AudioPassthrough.h"

static const char* const braids_model_names[] = {
    "CSAW",   // 0: Complex Saw (supersaw + multiplier)
    "MORPH",  // 1: Wave Morphing (triangle -> saw -> square)
    "SAW_SQ", // 2: Dual Saw & Square
    "SYNC",   // 3: Hard Sync Square
    "SIN_TR", // 4: Sine / Triangle Waveshaper
    "BUZZ",   // 5: Resynthesized Buzz (variable pulse trains)
    "3xSAW",  // 6: Triple Sawtooth
    "3xSQR",  // 7: Triple Square
    "3xRING", // 8: Triple Ring Modulator
    "SWARM",  // 9: 7-Sawtooth Detuned Swarm
    "COMB",   // 10: Sawtooth with Comb Filter
    "TOY",    // 11: Circuit-Bent 8-bit Toy
    "FLT_LP", // 12: Digital Low-pass Filtered Waveforms
    "FLT_PK", // 13: Peaking Filtered Waveforms
    "FLT_BP", // 14: Bandpass Filtered Waveforms
    "FLT_HP", // 15: High-pass Filtered Waveforms
    "VOSIM",  // 16: VOSIM Vocal Formants
    "VOWEL",  // 17: Vowel Formant Filter
    "VWL_FO", // 18: FOF Vocal Choir Generator
    "FM",     // 19: 2-Operator Phase Modulation
    "FDBKFM", // 20: Feedback FM
    "CHAOS",  // 21: Chaotic Attractor Feedback FM
    "BELL",   // 22: Struck Metallic Bell Physical Model
    "DRUM",   // 23: Struck 2D Membrane / Drum Physical Model
    "PLUCK",  // 24: Karplus-Strong Plucked String
    "BOWED",  // 25: Bowed String Physical Model
    "BLOWN",  // 26: Blown Reed Pipe Physical Model
    "FLUTE",  // 27: Flute Jet Pipe Physical Model
    "WAVTBL", // 28: 256-Wave Continuous Wavetable Synth
    "WAV_MP", // 29: 2D Wavetable Map
    "WAV_LN", // 30: Nonlinear Wavetable Interpolator
    "WAV_PR", // 31: Paraphonic 4-Voice Chord Wavetable
    "NOISE",  // 32: Filtered White / Pink Noise
    "TWIN_P", // 33: Twin Peak Resonant Bandpass Noise
    "CLK_NZ", // 34: Clocked Sample & Hold Digital Noise
    "CLOUDS", // 35: Granular Noise Cloud
    "PARTCL", // 36: Particle Resonator Noise
    "DIG_MD", // 37: Bytebeat / Raw Digital Modulation
    "QM"      // 38: Mystery Question Mark Engine
};

static constexpr int NUM_BRAIDS_MODELS = sizeof(braids_model_names) / sizeof(braids_model_names[0]);
static constexpr int NUM_BRAIDS_POLY = 5;

struct BraidsVoiceUnit {
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

    void trigger(float pitch_val, float vel = 1.0f, bool gate_high = true, bool is_midi = false) {
        active = true;
        gate = gate_high;
        from_midi = is_midi;
        strike_pending = true;
        pitch_offset = pitch_val;
        velocity = vel;
        env_level = 0.0f;
        env_stage = ENV_ATTACK;
        sample_age = 0;
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
        sample_age = 0;
    }
};

enum BraidsVoiceMode : uint8_t {
    BRAIDS_VOICE_MODE_MONO = 0,
    BRAIDS_VOICE_MODE_POLY_RR,
    BRAIDS_VOICE_MODE_UNISON,
    BRAIDS_VOICE_MODE_CHORD
};

class AudioBraidsStream : public AudioStream {
public:
    AudioBraidsStream() : AudioStream(0, NULL), initialized(false) {}

    void init() {
        if (initialized) return;
        for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
            voices[v].reset();
            braids_osc[v].Init();
            braids_osc[v].set_shape(braids::MACRO_OSC_SHAPE_CSAW);
            braids_osc[v].set_pitch(60 << 7);
            braids_osc[v].set_parameters(16384, 16384);
        }
        shape = target_shape = 0;
        pitch_base = 0.0f;
        timbre = 0.5f;
        color = 0.5f;
        attack_step = 1.0f / (0.0005f * AUDIO_SAMPLE_RATE_EXACT);
        release_norm = 0.7f;
        decay_coeff = expf(-6.907755f / (0.5f * AUDIO_SAMPLE_RATE_EXACT));
        drone_mode = false;
        master_gain = 6500.0f;
        voice_mode = BRAIDS_VOICE_MODE_MONO;
        round_robin_index = 0;
        prev_gate_state = false;
        initialized = true;
    }

    void set_shape(int s) {
        if (s >= 0 && s < braids::MACRO_OSC_SHAPE_LAST) {
            target_shape = s;
        }
    }

    void set_pitch_base(float p_units) {
        pitch_base = p_units;
    }

    void set_timbre(float t) {
        timbre = constrain(t, 0.0f, 1.0f);
    }

    void set_color(float c) {
        color = constrain(c, 0.0f, 1.0f);
    }

    void set_attack(float a_norm) {
        if (a_norm <= 0.01f) {
            attack_step = 1.0f / (0.0005f * AUDIO_SAMPLE_RATE_EXACT); // 0.5ms anti-click ramp
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

    void set_voice_mode(BraidsVoiceMode vm) {
        voice_mode = vm;
    }

    uint8_t get_active_voice_mask() const {
        uint8_t mask = 0;
        for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
            if (voices[v].active && (drone_mode || voices[v].env_level > 0.01f || voices[v].sample_age < 9600)) {
                mask |= (1 << v);
            }
        }
        return mask;
    }

    void trigger_cv(float pitch_val, float vel = 1.0f, bool gate_high = true, bool is_midi = false) {
        if (!initialized) return;

        if (voice_mode == BRAIDS_VOICE_MODE_MONO) {
            voices[0].trigger(pitch_val, vel, gate_high, is_midi);
            braids_osc[0].Strike();
            return;
        }

        if (voice_mode == BRAIDS_VOICE_MODE_UNISON) {
            float spread_cents = 2.5f + color * 24.0f;
            static const float offsets[5] = {0.0f, -1.0f, 1.0f, -0.5f, 0.5f};
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
                float detune = offsets[v] * spread_cents * (128.0f / 100.0f);
                voices[v].trigger(pitch_val + detune, vel, gate_high, is_midi);
                braids_osc[v].Strike();
            }
            return;
        }

        if (voice_mode == BRAIDS_VOICE_MODE_CHORD) {
            static const int8_t chords[8][5] = {
                {0, 4, 7, 11, 14},  // Maj9
                {0, 3, 7, 10, 14},  // Min9
                {0, 4, 7, 10, 14},  // Dom9
                {0, 5, 7, 10, 14},  // Sus4 / 9
                {0, 3, 6, 10, 13},  // Half-Dim9
                {0, 4, 7, 12, 16},  // Power / Octaves
                {0, 7, 12, 19, 24}, // 5th Stack
                {0, 3, 7, 12, 15}   // Min Octave
            };
            int c_idx = constrain((int)(timbre * 8.0f), 0, 7);
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
                float c_pitch = pitch_val + (float)chords[c_idx][v] * 128.0f;
                voices[v].trigger(c_pitch, vel, gate_high, is_midi);
                braids_osc[v].Strike();
            }
            return;
        }

        // Polyphonic Round-Robin 5 (RR5)
        int target_v = round_robin_index;
        for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
            int idx = (round_robin_index + v) % NUM_BRAIDS_POLY;
            if (voices[idx].env_stage == BraidsVoiceUnit::ENV_IDLE) {
                target_v = idx;
                break;
            }
        }
        voices[target_v].trigger(pitch_val, vel, gate_high, is_midi);
        braids_osc[target_v].Strike();
        round_robin_index = (target_v + 1) % NUM_BRAIDS_POLY;
    }

    void set_gate_cv(bool gate) {
        if (!initialized) return;
        prev_gate_state = gate;

        if (!gate) {
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
                if (!voices[v].from_midi && voices[v].gate) {
                    voices[v].release();
                }
            }
        }
    }

    void midi_note_on(uint8_t note, uint8_t velocity) {
        if (!initialized || velocity == 0) return;
        float norm_vel = (float)velocity * (1.0f / 127.0f);
        float v_f = 0.20f + 0.80f * norm_vel;
        float pitch_val = (float)(note - 60) * 128.0f;

        if (voice_mode == BRAIDS_VOICE_MODE_MONO) {
            voices[0].midi_note = note;
            voices[0].trigger(pitch_val, v_f, true, true);
            braids_osc[0].Strike();
            return;
        }

        if (voice_mode == BRAIDS_VOICE_MODE_UNISON || voice_mode == BRAIDS_VOICE_MODE_CHORD) {
            trigger_cv(pitch_val, v_f, true, true);
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) voices[v].midi_note = note;
            return;
        }

        int best_v = -1;
        for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
            if (voices[v].active && voices[v].from_midi && voices[v].midi_note == note) {
                best_v = v;
                break;
            }
        }
        if (best_v < 0) {
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
                int idx = (round_robin_index + v) % NUM_BRAIDS_POLY;
                if (voices[idx].env_stage == BraidsVoiceUnit::ENV_IDLE) {
                    best_v = idx;
                    round_robin_index = (idx + 1) % NUM_BRAIDS_POLY;
                    break;
                }
            }
        }
        if (best_v < 0) {
            uint32_t max_age = 0;
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
                if (voices[v].sample_age >= max_age) {
                    max_age = voices[v].sample_age;
                    best_v = v;
                }
            }
            round_robin_index = (best_v + 1) % NUM_BRAIDS_POLY;
        }

        if (best_v >= 0) {
            voices[best_v].midi_note = note;
            voices[best_v].trigger(pitch_val, v_f, true, true);
            braids_osc[best_v].Strike();
        }
    }

    void midi_note_off(uint8_t note) {
        if (!initialized) return;
        for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
            if (voices[v].active && voices[v].from_midi && voices[v].midi_note == note) {
                voices[v].release();
            }
        }
    }

    void all_notes_off() {
        if (!initialized) return;
        for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
            voices[v].silence();
        }
        prev_gate_state = false;
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

        if (target_shape != shape) {
            shape = target_shape;
            for (int v = 0; v < NUM_BRAIDS_POLY; ++v) {
                braids_osc[v].set_shape((braids::MacroOscillatorShape)shape);
            }
        }

        float mix_buf[AUDIO_BLOCK_SAMPLES];
        memset(mix_buf, 0, sizeof(mix_buf));

        const float cur_timbre = timbre;
        const float cur_color = color;
        const float cur_master_gain = master_gain;

        int max_voices = (voice_mode == BRAIDS_VOICE_MODE_MONO) ? 1 : NUM_BRAIDS_POLY;
        int16_t t_param = (int16_t)(cur_timbre * 32767.0f);
        int16_t c_param = (int16_t)(cur_color * 32767.0f);

        int16_t braids_chunk[24];
        uint8_t braids_sync[24];

        for (int v = 0; v < max_voices; ++v) {
            BraidsVoiceUnit& vc = voices[v];
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

                    if (vc.env_stage == BraidsVoiceUnit::ENV_ATTACK) {
                        vc.env_level += attack_step;
                        if (vc.env_level >= 1.0f) {
                            vc.env_level = 1.0f;
                            vc.env_stage = BraidsVoiceUnit::ENV_DECAY;
                        }
                    } else if (vc.env_stage == BraidsVoiceUnit::ENV_DECAY) {
                        if (!vc.gate && !drone_mode) {
                            vc.env_level *= decay_coeff;
                            if (vc.env_level < 0.0002f) {
                                vc.env_level = 0.0f;
                                vc.env_stage = BraidsVoiceUnit::ENV_IDLE;
                                vc.active = false;
                            }
                        }
                    }

                    float env = drone_mode ? 1.0f : vc.env_level;
                    float raw = (float)braids_chunk[i] * (1.0f / 32768.0f);
                    mix_buf[s_idx] += raw * env * vc.velocity;
                }
                samples_rendered += chunk;
            }
        }

        // Master gain & algebraic soft limiter: x / sqrt(1 + x^2)
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            float out_f = mix_buf[i] * cur_master_gain * (1.0f / 6500.0f);
            out_f = out_f / sqrtf(1.0f + out_f * out_f);
            block->data[i] = (int16_t)constrain(out_f * 32767.0f, -32767.0f, 32767.0f);
        }

        transmit(block, 0);
        release(block);
    }

private:
    BraidsVoiceUnit voices[NUM_BRAIDS_POLY];
    braids::MacroOscillator braids_osc[NUM_BRAIDS_POLY];

    int target_shape = 0;
    int shape = 0;
    float pitch_base = 0.0f;
    float timbre = 0.5f;
    float color = 0.5f;

    float attack_step = 0.0f;
    float release_norm = 0.7f;
    float decay_coeff = 0.0f;
    bool drone_mode = false;
    float master_gain = 6500.0f;

    BraidsVoiceMode voice_mode = BRAIDS_VOICE_MODE_MONO;
    int round_robin_index = 0;
    bool prev_gate_state = false;
    bool initialized = false;
};

// Independent dual-mono instance streams in DMAMEM
static DMAMEM AudioBraidsStream braids_streams[2];
static DMAMEM AudioPassthrough<MONO> braids_dummy_input;

class BraidsVoiceApplet : public HemisphereAudioApplet {
public:
    static constexpr const char* applet_name_() { return "Braids"; }
    static constexpr const uint8_t* applet_icon_() { return nullptr; }

    const char* applet_name() const override { return applet_name_(); }
    const uint8_t* applet_icon() const override { return applet_icon_(); }

    inline int get_side() const {
        return (int)hemisphere & 1;
    }

    AudioBraidsStream& stream() {
        return braids_streams[get_side()];
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
        stream().set_shape(shape);
        stream().set_timbre(timbre * 0.01f);
        stream().set_color(color * 0.01f);
        stream().set_attack(attack_time * 0.01f);
        stream().set_release(release_time * 0.01f);
        stream().set_voice_mode((BraidsVoiceMode)voice_mode_idx);
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
        // Universal Stop / Panic handling
        if (message == 0xFC || ((message & 0xF0) == 0xB0 && (data1 == 120 || data1 == 123))) {
            stream().all_notes_off();
            return;
        }

        if (midi_channel == 0) return;
        if (midi_channel >= 1 && midi_channel <= 16 && channel != midi_channel) return;

        uint8_t cur_src = get_effective_input_source();
        if (cur_src == INPUT_SRC_CV) return;

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
            int bend_val = ((int)data2 << 7) | (int)data1; // 0..16383, 8192 center
            midi_pitch_bend_units = ((float)(bend_val - 8192) * (1.0f / 8192.0f)) * 256.0f; // +/- 2 semitones
        }
    }

    void Controller() override {
        if (input_source == INPUT_SRC_TGL && src_gate.enabled() && src_gate.Clock()) {
            toggle_latch = !toggle_latch;
        }

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
            if (trig_cv.Clock()) adc_lag_ = HEMISPHERE_ADC_LAG;
            if (adc_lag_ > 0 && --adc_lag_ == 0) {
                stream().trigger_cv(0.0f, 1.0f, true);
            }
            stream().set_gate_cv(trig_cv.Gate());
        } else {
            adc_lag_ = 0;
            stream().set_gate_cv(false);
        }

        int eff_shape = shape;
        if (shape_cv.enabled()) {
            eff_shape += (int)(shape_cv.InF() * (float)(NUM_BRAIDS_MODELS - 1));
            eff_shape = constrain(eff_shape, 0, NUM_BRAIDS_MODELS - 1);
        }
        eff_disp_shape = eff_shape;
        stream().set_shape(eff_shape);

        float total_pitch = (float)pitch + midi_pitch_bend_units;
        if (cur_src != INPUT_SRC_MID) {
            total_pitch += (float)pitch_cv.In();
        }
        stream().set_pitch_base(total_pitch);

        float t_norm = constrain((float)timbre * 0.01f + timbre_cv.InF(), 0.0f, 1.0f);
        stream().set_timbre(t_norm);

        float c_norm = constrain((float)color * 0.01f + color_cv.InF(), 0.0f, 1.0f);
        stream().set_color(c_norm);

        float a_norm = constrain((float)attack_time * 0.01f + attack_cv.InF(), 0.0f, 1.0f);
        stream().set_attack(a_norm);

        float r_norm = constrain((float)release_time * 0.01f + release_cv.InF(), 0.0f, 1.0f);
        stream().set_release(r_norm);

        stream().set_voice_mode((BraidsVoiceMode)voice_mode_idx);
    }

    void View() override {
        gfxHeader(applet_name());

        // Header voice activity dots
        int max_dots = (voice_mode_idx == BRAIDS_VOICE_MODE_MONO) ? 1 : NUM_BRAIDS_POLY;
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

        // Header MIDI RX indicator dot
        if (millis() - last_midi_rx_tick < 300) {
            graphics.setPixel(56, 2);
            graphics.setPixel(56, 3);
        }

        const bool pg2 = (cursor >= PAGE2_START);

        if (!pg2) {
            // ── Page 1: Timbre & Sound Parameters ───────────────────────────
            // Row 1: Model
            gfxPrint(1, 15, "M:");
            gfxStartCursor(13, 15);
            int disp_shape = (cursor == CURSOR_MODEL && EditMode()) ? shape : eff_disp_shape;
            if (disp_shape >= 0 && disp_shape < NUM_BRAIDS_MODELS) {
                gfxPrint(braids_model_names[disp_shape]);
            }
            gfxEndCursor(cursor == CURSOR_MODEL);

            gfxStartCursor(53, 15);
            gfxPrint(shape_cv);
            gfxEndCursor(cursor == CURSOR_MODEL_CV, false, shape_cv.InputName());

            // Row 2: Pitch
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

            // Row 4: Color
            gfxPrint(1, 45, "Col:");
            gfxStartCursor(25, 45);
            graphics.printf("%3d%%", color);
            gfxEndCursor(cursor == CURSOR_COLOR);

            gfxStartCursor(53, 45);
            gfxPrint(color_cv);
            gfxEndCursor(cursor == CURSOR_COLOR_CV, false, color_cv.InputName());

            // Row 5: Decay / Release
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
            // ── Page 2: Envelopes, Polyphony, Modulation & Inputs ───────────
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
            if (voice_mode_idx == BRAIDS_VOICE_MODE_MONO) graphics.print("MON");
            else if (voice_mode_idx == BRAIDS_VOICE_MODE_POLY_RR) graphics.print("RR5");
            else if (voice_mode_idx == BRAIDS_VOICE_MODE_UNISON) graphics.print("UNI");
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
            IndexedInput(CURSOR_MODEL_CV,        shape_cv),
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
            case CURSOR_MODEL:
                shape = (int8_t)constrain((int)shape + direction, 0, NUM_BRAIDS_MODELS - 1);
                stream().set_shape(shape);
                break;
            case CURSOR_MODEL_CV:
                shape_cv.ChangeSource(direction);
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
                color = (uint8_t)constrain((int)color + direction, 0, 100);
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
                update_gain();
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
        help[HELP_DIGITAL1] = "Clock/Gate";
        help[HELP_CV1]      = "1V/Oct";
        help[HELP_CV2]      = "Timbre CV";
        help[HELP_OUT1]     = "Audio L";
        help[HELP_OUT2]     = "Audio R";
        help[HELP_EXTRA1]   = "MIDI Ch / Poly";
        help[HELP_EXTRA2]   = "In: CV / MIDI";
    }

    AudioStream* InputStream() override {
        return &braids_dummy_input;
    }

    AudioStream* OutputStream() override {
        return &stream();
    }

    void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
        data[0] = PackPackables(
            pack<6>(shape),
            pack<2>(input_source),
            pack<8>(timbre),
            pack<8>(color),
            pack<8>(attack_time),
            pack<8>(release_time),
            pack<8>(level_db),
            pack<3>(voice_mode_idx),
            pack<5>(midi_channel)
        );
        uint16_t dummy_pad = 0;
        data[1] = PackPackables(pitch, dummy_pad, shape_cv, pitch_cv);
        data[2] = PackPackables(timbre_cv, color_cv, attack_cv, release_cv);
        data[3] = PackPackables(trig_cv, src_gate);
    }

    void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
            shape = 0;
            input_source = INPUT_SRC_ALL;
            timbre = 50;
            color = 50;
            attack_time = 0;
            release_time = 70;
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
            pack<6>(shape),
            pack<2>(input_source),
            pack<8>(timbre),
            pack<8>(color),
            pack<8>(attack_time),
            pack<8>(release_time),
            pack<8>(level_db),
            pack<3>(voice_mode_idx),
            pack<5>(midi_channel)
        );
        uint16_t dummy_pad = 0;
        UnpackPackables(data[1], pitch, dummy_pad, shape_cv, pitch_cv);
        UnpackPackables(data[2], timbre_cv, color_cv, attack_cv, release_cv);
        UnpackPackables(data[3], trig_cv, src_gate);

        if (!pitch_cv.enabled()) {
            pitch_cv.SetInput(get_side() == 0 ? 0 : (ADC_CHANNEL_COUNT > 2 ? 2 : 1));
        }
        if (!trig_cv.enabled()) {
            trig_cv.SetGateInput(get_side() == 0 ? 0 : (DIGITAL_INPUT_COUNT > 2 ? 2 : 1));
        }

        shape = (int8_t)constrain((int)shape, 0, NUM_BRAIDS_MODELS - 1);
        input_source = (uint8_t)constrain((int)input_source, 0, 3);
        timbre = (uint8_t)constrain((int)timbre, 0, 100);
        color = (uint8_t)constrain((int)color, 0, 100);
        attack_time = (uint8_t)constrain((int)attack_time, 0, 100);
        release_time = (uint8_t)constrain((int)release_time, 0, 100);
        level_db = (int8_t)constrain((int)level_db, -48, 18);
        voice_mode_idx = (uint8_t)constrain((int)voice_mode_idx, 0, 3);
        midi_channel = (uint8_t)constrain((int)midi_channel, 0, 17);
        pitch = constrain(pitch, MIN_PITCH, MAX_PITCH);
    }

private:
    static constexpr int MAX_PITCH = 7 * 12 * 128;
    static constexpr int MIN_PITCH = -3 * 12 * 128;

    enum Cursor {
        // Page 1: Timbre & Sound Parameters
        CURSOR_MODEL,
        CURSOR_MODEL_CV,
        CURSOR_PITCH_SEMI,
        CURSOR_PITCH_FINE,
        CURSOR_PITCH_CV,
        CURSOR_TIMBRE,
        CURSOR_TIMBRE_CV,
        CURSOR_COLOR,
        CURSOR_COLOR_CV,
        CURSOR_RELEASE,
        CURSOR_RELEASE_CV,

        // Page 2: Envelopes, Polyphony & Inputs
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
    int8_t shape = 0;
    int16_t pitch = 0;
    uint8_t timbre = 50;
    uint8_t color = 50;
    uint8_t attack_time = 0;
    uint8_t release_time = 70;
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

    int eff_disp_shape = 0;
    int adc_lag_ = 0;

    CVInputMap shape_cv;
    CVInputMap pitch_cv;
    CVInputMap timbre_cv;
    CVInputMap color_cv;
    CVInputMap attack_cv;
    CVInputMap release_cv;
    DigitalInputMap trig_cv;
    DigitalInputMap src_gate;
};
