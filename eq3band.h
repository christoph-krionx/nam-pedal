#pragma once

#include <cmath>

// Self-contained 3-band EQ: low-shelf (bass), peaking (mid), high-shelf
// (treble), built from RBJ-cookbook biquads. Coefficients are recomputed on
// the control thread via Set*(); Process() runs per-sample in the audio ISR.
// No DaisySP dependency, so the firmware build stays library-free.

class Biquad
{
  public:
    void Reset()
    {
        b0_ = 1.0f;
        b1_ = b2_ = a1_ = a2_ = 0.0f;
        z1_ = z2_ = 0.0f;
    }

    // Transposed Direct Form II — one multiply-add pair of state per sample.
    inline float Process(float x)
    {
        float y = b0_ * x + z1_;
        z1_     = b1_ * x - a1_ * y + z2_;
        z2_     = b2_ * x - a2_ * y;
        return y;
    }

    // Store coefficients normalized by a0 (a0 -> 1).
    void SetCoeffs(float b0, float b1, float b2, float a0, float a1, float a2)
    {
        float inv = 1.0f / a0;
        b0_       = b0 * inv;
        b1_       = b1 * inv;
        b2_       = b2 * inv;
        a1_       = a1 * inv;
        a2_       = a2 * inv;
    }

  private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};

class Eq3Band
{
  public:
    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        low_.Reset();
        mid_.Reset();
        high_.Reset();
        SetBass(0.0f);
        SetMid(0.0f);
        SetTreble(0.0f);
    }

    // gain_db is the boost/cut for each band (0 dB == flat).
    void SetBass(float gain_db) { LowShelf(low_, kBassFreq, gain_db); }
    void SetMid(float gain_db) { Peaking(mid_, kMidFreq, kMidQ, gain_db); }
    void SetTreble(float gain_db) { HighShelf(high_, kTrebleFreq, gain_db); }

    inline float Process(float x)
    {
        return high_.Process(mid_.Process(low_.Process(x)));
    }

  private:
    // Band corner/center frequencies tuned for guitar tone shaping.
    static constexpr float kBassFreq   = 200.0f;
    static constexpr float kMidFreq    = 1000.0f;
    static constexpr float kMidQ       = 0.7f;
    static constexpr float kTrebleFreq = 3200.0f;
    static constexpr float kShelfSlope = 1.0f; // max steepness w/o overshoot
    static constexpr float kPi         = 3.14159265358979323846f;

    void Peaking(Biquad& bq, float f0, float q, float db)
    {
        float A     = powf(10.0f, db / 40.0f);
        float w0    = 2.0f * kPi * f0 / sr_;
        float cw    = cosf(w0);
        float sw    = sinf(w0);
        float alpha = sw / (2.0f * q);

        float b0 = 1.0f + alpha * A;
        float b1 = -2.0f * cw;
        float b2 = 1.0f - alpha * A;
        float a0 = 1.0f + alpha / A;
        float a1 = -2.0f * cw;
        float a2 = 1.0f - alpha / A;
        bq.SetCoeffs(b0, b1, b2, a0, a1, a2);
    }

    void LowShelf(Biquad& bq, float f0, float db)
    {
        float A   = powf(10.0f, db / 40.0f);
        float w0  = 2.0f * kPi * f0 / sr_;
        float cw  = cosf(w0);
        float sw  = sinf(w0);
        float alpha
            = sw / 2.0f
              * sqrtf((A + 1.0f / A) * (1.0f / kShelfSlope - 1.0f) + 2.0f);
        float beta = 2.0f * sqrtf(A) * alpha;
        float Ap1  = A + 1.0f;
        float Am1  = A - 1.0f;

        float b0 = A * (Ap1 - Am1 * cw + beta);
        float b1 = 2.0f * A * (Am1 - Ap1 * cw);
        float b2 = A * (Ap1 - Am1 * cw - beta);
        float a0 = Ap1 + Am1 * cw + beta;
        float a1 = -2.0f * (Am1 + Ap1 * cw);
        float a2 = Ap1 + Am1 * cw - beta;
        bq.SetCoeffs(b0, b1, b2, a0, a1, a2);
    }

    void HighShelf(Biquad& bq, float f0, float db)
    {
        float A   = powf(10.0f, db / 40.0f);
        float w0  = 2.0f * kPi * f0 / sr_;
        float cw  = cosf(w0);
        float sw  = sinf(w0);
        float alpha
            = sw / 2.0f
              * sqrtf((A + 1.0f / A) * (1.0f / kShelfSlope - 1.0f) + 2.0f);
        float beta = 2.0f * sqrtf(A) * alpha;
        float Ap1  = A + 1.0f;
        float Am1  = A - 1.0f;

        float b0 = A * (Ap1 + Am1 * cw + beta);
        float b1 = -2.0f * A * (Am1 + Ap1 * cw);
        float b2 = A * (Ap1 + Am1 * cw - beta);
        float a0 = Ap1 - Am1 * cw + beta;
        float a1 = 2.0f * (Am1 - Ap1 * cw);
        float a2 = Ap1 - Am1 * cw - beta;
        bq.SetCoeffs(b0, b1, b2, a0, a1, a2);
    }

    float  sr_ = 48000.0f;
    Biquad low_, mid_, high_;
};
