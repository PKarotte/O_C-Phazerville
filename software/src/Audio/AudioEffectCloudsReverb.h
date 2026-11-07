#pragma once

// ============================================================================
// Mutable Instruments Clouds Algorithmic Reverb
// Original DSP Copyright 2014 Emilie Gillet (MIT License)
// Adapted for Teensy Audio Library / Phazerville ORN8
// ============================================================================

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>
#include <algorithm>
#include <cmath>

extern "C" uint8_t external_psram_size;
extern "C" void* extmem_malloc(size_t);
extern "C" void extmem_free(void* ptr);

namespace clouds_reverb {

#define TAIL , -1

#define MAKE_INTEGRAL_FRACTIONAL(x) \
    int32_t x ## _integral = static_cast<int32_t>(x); \
    float x ## _fractional = x - static_cast<float>(x ## _integral);

enum LFOIndex {
    LFO_1,
    LFO_2
};

struct DataType16 {
    typedef uint16_t T;
    static inline float Decompress(T value) {
        return static_cast<float>(static_cast<int16_t>(value)) / 32768.0f;
    }
    static inline T Compress(float value) {
        return static_cast<uint16_t>(__SSAT(static_cast<int32_t>(value * 32768.0f), 16));
    }
};

class CosineOscillator {
public:
    CosineOscillator() : iir_coefficient_(0.0f), current_(1.0f), previous_(1.0f) {}
    ~CosineOscillator() {}

    inline void Init(float frequency) {
        iir_coefficient_ = 2.0f * cosf(2.0f * 3.14159265358979323846f * frequency);
        current_ = 1.0f;
        previous_ = cosf(2.0f * 3.14159265358979323846f * frequency);
    }

    inline float Next() {
        float next = iir_coefficient_ * current_ - previous_;
        previous_ = current_;
        current_ = next;
        return next;
    }

    inline float value() const {
        return current_;
    }

private:
    float iir_coefficient_;
    float current_;
    float previous_;
};

template<size_t size>
class FxEngine {
public:
    typedef typename DataType16::T T;
    FxEngine() : write_ptr_(0), buffer_(nullptr) {}
    ~FxEngine() {}

    void Init(T* buffer) {
        buffer_ = buffer;
        Clear();
    }

    void Clear() {
        if (buffer_) {
            std::fill(&buffer_[0], &buffer_[size], 0);
        }
        write_ptr_ = 0;
    }

    struct Empty {};

    template<int32_t l, typename TailType = Empty>
    struct Reserve {
        typedef TailType Tail;
        enum {
            length = l
        };
    };

    template<typename Memory, int32_t index>
    struct DelayLine {
        enum {
            length = DelayLine<typename Memory::Tail, index - 1>::length,
            base = DelayLine<Memory, index - 1>::base + DelayLine<Memory, index - 1>::length + 1
        };
    };

    template<typename Memory>
    struct DelayLine<Memory, 0> {
        enum {
            length = Memory::length,
            base = 0
        };
    };

    class Context {
        friend class FxEngine;
    public:
        Context() : accumulator_(0.0f), previous_read_(0.0f), buffer_(nullptr), write_ptr_(0) {
            lfo_value_[0] = 0.0f;
            lfo_value_[1] = 0.0f;
        }
        ~Context() {}

        inline void Load(float value) {
            accumulator_ = value;
        }

        inline void Read(float value, float scale) {
            accumulator_ += value * scale;
        }

        inline void Read(float value) {
            accumulator_ += value;
        }

        inline void Write(float& value) {
            value = accumulator_;
        }

        inline void Write(float& value, float scale) {
            value = accumulator_;
            accumulator_ *= scale;
        }

        template<typename D>
        inline void Write(D& d, int32_t offset, float scale) {
            static_assert(D::base + D::length <= size, "delay_memory_full");
            T w = DataType16::Compress(accumulator_);
            if (offset == -1) {
                buffer_[(write_ptr_ + D::base + D::length - 1) & MASK] = w;
            } else {
                buffer_[(write_ptr_ + D::base + offset) & MASK] = w;
            }
            accumulator_ *= scale;
        }

        template<typename D>
        inline void Write(D& d, float scale) {
            Write(d, 0, scale);
        }

        template<typename D>
        inline void WriteAllPass(D& d, int32_t offset, float scale) {
            Write(d, offset, scale);
            accumulator_ += previous_read_;
        }

        template<typename D>
        inline void WriteAllPass(D& d, float scale) {
            WriteAllPass(d, 0, scale);
        }

        template<typename D>
        inline void Read(D& d, int32_t offset, float scale) {
            static_assert(D::base + D::length <= size, "delay_memory_full");
            T r;
            if (offset == -1) {
                r = buffer_[(write_ptr_ + D::base + D::length - 1) & MASK];
            } else {
                r = buffer_[(write_ptr_ + D::base + offset) & MASK];
            }
            float r_f = DataType16::Decompress(r);
            previous_read_ = r_f;
            accumulator_ += r_f * scale;
        }

        template<typename D>
        inline void Read(D& d, float scale) {
            Read(d, 0, scale);
        }

        inline void Lp(float& state, float coefficient) {
            state += coefficient * (accumulator_ - state);
            accumulator_ = state;
        }

        inline void Hp(float& state, float coefficient) {
            state += coefficient * (accumulator_ - state);
            accumulator_ -= state;
        }

        template<typename D>
        inline void Interpolate(D& d, float offset, float scale) {
            static_assert(D::base + D::length <= size, "delay_memory_full");
            MAKE_INTEGRAL_FRACTIONAL(offset);
            float a = DataType16::Decompress(
                buffer_[(write_ptr_ + offset_integral + D::base) & MASK]);
            float b = DataType16::Decompress(
                buffer_[(write_ptr_ + offset_integral + D::base + 1) & MASK]);
            float x = a + (b - a) * offset_fractional;
            previous_read_ = x;
            accumulator_ += x * scale;
        }

        template<typename D>
        inline void Interpolate(
            D& d, float offset, LFOIndex index, float amplitude, float scale) {
            static_assert(D::base + D::length <= size, "delay_memory_full");
            offset += amplitude * lfo_value_[index];
            MAKE_INTEGRAL_FRACTIONAL(offset);
            float a = DataType16::Decompress(
                buffer_[(write_ptr_ + offset_integral + D::base) & MASK]);
            float b = DataType16::Decompress(
                buffer_[(write_ptr_ + offset_integral + D::base + 1) & MASK]);
            float x = a + (b - a) * offset_fractional;
            previous_read_ = x;
            accumulator_ += x * scale;
        }

    private:
        float accumulator_;
        float previous_read_;
        float lfo_value_[2];
        T* buffer_;
        int32_t write_ptr_;
    };

    inline void SetLFOFrequency(LFOIndex index, float frequency) {
        lfo_[index].Init(frequency * 32.0f);
    }

    inline void Start(Context* c) {
        --write_ptr_;
        if (write_ptr_ < 0) {
            write_ptr_ += size;
        }
        c->accumulator_ = 0.0f;
        c->previous_read_ = 0.0f;
        c->buffer_ = buffer_;
        c->write_ptr_ = write_ptr_;
        if ((write_ptr_ & 31) == 0) {
            c->lfo_value_[0] = lfo_[0].Next();
            c->lfo_value_[1] = lfo_[1].Next();
        } else {
            c->lfo_value_[0] = lfo_[0].value();
            c->lfo_value_[1] = lfo_[1].value();
        }
    }

private:
    enum {
        MASK = size - 1
    };

    int32_t write_ptr_;
    T* buffer_;
    CosineOscillator lfo_[2];
};

} // namespace clouds_reverb

class AudioEffectCloudsReverb : public AudioStream {
public:
    static const size_t BUFFER_SIZE = 32768; // 64 KB int16 buffer

    typedef clouds_reverb::FxEngine<BUFFER_SIZE> E;
    // Delay lines scaled by 1.5x for 48 kHz (exact reproduction of Clouds at 32 kHz)
    typedef E::Reserve<170,
        E::Reserve<243,
        E::Reserve<361,
        E::Reserve<599,
        E::Reserve<2480,
        E::Reserve<3057,
        E::Reserve<5117,
        E::Reserve<2870,
        E::Reserve<2495,
        E::Reserve<7173> > > > > > > > > > ReverbMemory;

    AudioEffectCloudsReverb()
        : AudioStream(2, input_queue_array),
          buffer_(nullptr),
          arena_in_psram_(false),
          input_gain_(0.5f),
          reverb_time_(0.85f),
          diffusion_(0.625f),
          lp_(0.7f),
          dry_gain_(1.0f),
          wet_gain_(0.5f),
          lp_decay_1_(0.0f),
          lp_decay_2_(0.0f),
          frozen_(false),
          mono_mode_(false) {}

    ~AudioEffectCloudsReverb() { end(); }

    bool begin() {
        if (buffer_) return true;
        const size_t bytes = BUFFER_SIZE * sizeof(int16_t);
        buffer_ = (int16_t*)malloc(bytes);
        arena_in_psram_ = false;
        if (!buffer_ && external_psram_size > 0) {
            buffer_ = (int16_t*)extmem_malloc(bytes);
            arena_in_psram_ = (buffer_ != nullptr);
        }
        if (!buffer_) return false;
        memset(buffer_, 0, bytes);
        engine_.Init((uint16_t*)buffer_);
        engine_.SetLFOFrequency(clouds_reverb::LFO_1, 0.5f / AUDIO_SAMPLE_RATE_EXACT);
        engine_.SetLFOFrequency(clouds_reverb::LFO_2, 0.3f / AUDIO_SAMPLE_RATE_EXACT);
        lp_decay_1_ = 0.0f;
        lp_decay_2_ = 0.0f;
        return true;
    }

    void end() {
        __disable_irq();
        if (buffer_) {
            if (arena_in_psram_) extmem_free(buffer_);
            else free(buffer_);
            buffer_ = nullptr;
            arena_in_psram_ = false;
        }
        __enable_irq();
    }

    bool is_ready() const { return buffer_ != nullptr; }

    void setDecay(float t) { reverb_time_ = t; }
    void setDiffusion(float d) { diffusion_ = d; }
    void setDamping(float lp) { lp_ = lp; }
    void setInputGain(float g) { input_gain_ = g; }
    void setFreeze(bool f) { frozen_ = f; }
    void setMono(bool m) { mono_mode_ = m; }
    void setGains(float dry, float wet) { dry_gain_ = dry; wet_gain_ = wet; }

    void update(void) override {
        audio_block_t* inL = receiveReadOnly(0);
        audio_block_t* inR = receiveReadOnly(1);

        if (!buffer_) {
            if (inL) release(inL);
            if (inR) release(inR);
            return;
        }

        audio_block_t* outL = allocate();
        audio_block_t* outR = mono_mode_ ? nullptr : allocate();
        if (!outL && !outR) {
            if (inL) release(inL);
            if (inR) release(inR);
            return;
        }

        const int16_t* pl = inL ? inL->data : nullptr;
        const int16_t* pr = inR ? inR->data : pl; // if ch1 unconnected, normalizes to ch0

        const float kap = diffusion_;
        const float klp = lp_;
        const float krt = frozen_ ? 1.0f : reverb_time_;
        const float gain = frozen_ ? 0.0f : input_gain_;
        const float dry_gain = dry_gain_;
        const float wet_gain = wet_gain_;
        const bool is_mono = mono_mode_;

        float lp_1 = lp_decay_1_;
        float lp_2 = lp_decay_2_;

        E::DelayLine<ReverbMemory, 0> ap1;
        E::DelayLine<ReverbMemory, 1> ap2;
        E::DelayLine<ReverbMemory, 2> ap3;
        E::DelayLine<ReverbMemory, 3> ap4;
        E::DelayLine<ReverbMemory, 4> dap1a;
        E::DelayLine<ReverbMemory, 5> dap1b;
        E::DelayLine<ReverbMemory, 6> del1;
        E::DelayLine<ReverbMemory, 7> dap2a;
        E::DelayLine<ReverbMemory, 8> dap2b;
        E::DelayLine<ReverbMemory, 9> del2;
        E::Context c;

        for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
            float dry_l = pl ? pl[n] * (1.0f / 32768.0f) : 0.0f;
            float dry_r = pr ? pr[n] * (1.0f / 32768.0f) : 0.0f;

            float wet_l = 0.0f;
            float wet_r = 0.0f;
            float apout = 0.0f;

            engine_.Start(&c);

            // Smear AP1 inside loop
            c.Interpolate(ap1, 15.0f, clouds_reverb::LFO_1, 90.0f, 1.0f);
            c.Write(ap1, 150, 0.0f);

            c.Read(dry_l + dry_r, gain);

            // Diffuse through 4 allpasses
            c.Read(ap1 TAIL, kap);
            c.WriteAllPass(ap1, -kap);
            c.Read(ap2 TAIL, kap);
            c.WriteAllPass(ap2, -kap);
            c.Read(ap3 TAIL, kap);
            c.WriteAllPass(ap3, -kap);
            c.Read(ap4 TAIL, kap);
            c.WriteAllPass(ap4, -kap);
            c.Write(apout);

            // Main reverb loop - Left Tank
            c.Load(apout);
            c.Interpolate(del2, 7020.0f, clouds_reverb::LFO_2, 150.0f, krt);
            c.Lp(lp_1, klp);
            c.Read(dap1a TAIL, -kap);
            c.WriteAllPass(dap1a, kap);
            c.Read(dap1b TAIL, kap);
            c.WriteAllPass(dap1b, -kap);
            c.Write(del1, 2.0f);
            c.Write(wet_l, 0.0f);

            // Main reverb loop - Right Tank
            c.Load(apout);
            c.Read(del1 TAIL, krt);
            c.Lp(lp_2, klp);
            c.Read(dap2a TAIL, kap);
            c.WriteAllPass(dap2a, -kap);
            c.Read(dap2b TAIL, -kap);
            c.WriteAllPass(dap2b, kap);
            c.Write(del2, 2.0f);
            c.Write(wet_r, 0.0f);

            if (is_mono) {
                float mono_wet = 0.5f * (wet_l + wet_r);
                float out_val = dry_l * dry_gain + mono_wet * wet_gain;
                if (outL) outL->data[n] = (int16_t)__SSAT((int32_t)(out_val * 32768.0f), 16);
            } else {
                float out_l_val = dry_l * dry_gain + wet_l * wet_gain;
                float out_r_val = dry_r * dry_gain + wet_r * wet_gain;
                if (outL) outL->data[n] = (int16_t)__SSAT((int32_t)(out_l_val * 32768.0f), 16);
                if (outR) outR->data[n] = (int16_t)__SSAT((int32_t)(out_r_val * 32768.0f), 16);
            }
        }

        lp_decay_1_ = lp_1;
        lp_decay_2_ = lp_2;

        if (outL) {
            transmit(outL, 0);
            release(outL);
        }
        if (outR) {
            transmit(outR, 1);
            release(outR);
        }
        if (inL) release(inL);
        if (inR) release(inR);
    }

private:
    audio_block_t* input_queue_array[2];
    int16_t* buffer_;
    bool arena_in_psram_;

    float input_gain_;
    float reverb_time_;
    float diffusion_;
    float lp_;
    float dry_gain_;
    float wet_gain_;

    float lp_decay_1_;
    float lp_decay_2_;

    bool frozen_;
    bool mono_mode_;

    E engine_;
};

