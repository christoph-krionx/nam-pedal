#pragma once
#include <cmath>
#include <cstdint>

// Input detector with hysteresis, hold, and smooth gain transitions.
class NoiseGate
{
  public:
    void Init(float sample_rate)
    {
        detector_release_ = std::exp(-1.0f / (0.010f * sample_rate));
        open_step_ = 1.0f / (0.001f * sample_rate);
        close_step_ = 1.0f / (0.050f * sample_rate);
        hold_samples_ = static_cast<uint32_t>(0.020f * sample_rate);
    }

    float Process(float input, float threshold, bool enabled)
    {
        const float magnitude = std::fabs(input);
        envelope_ = magnitude > envelope_ ? magnitude : envelope_ * detector_release_;
        if(envelope_ >= threshold)
        {
            open_ = true;
            remaining_hold_ = hold_samples_;
        }
        else if(open_)
        {
            if(envelope_ >= threshold * 0.50118723f) // 6 dB hysteresis
                remaining_hold_ = hold_samples_;
            else if(remaining_hold_ > 0)
                --remaining_hold_;
            else
                open_ = false;
        }
        const float target = !enabled || open_ ? 1.0f : 0.0f;
        if(gain_ < target)
        {
            gain_ += open_step_;
            if(gain_ > target) gain_ = target;
        }
        else if(gain_ > target)
        {
            gain_ -= close_step_;
            if(gain_ < target) gain_ = target;
        }
        return gain_;
    }

  private:
    float envelope_ = 0.0f;
    float gain_ = 1.0f;
    float detector_release_ = 0.0f;
    float open_step_ = 1.0f;
    float close_step_ = 1.0f;
    uint32_t hold_samples_ = 0;
    uint32_t remaining_hold_ = 0;
    bool open_ = false;
};
