#include "noise_gate.h"
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    NoiseGate gate;
    gate.Init(48000);
    const float threshold = 0.01f;
    for(int i = 0; i < 4800; ++i)
        assert(gate.Process(0.001f, threshold, false) == 1.0f);
    float g = 1.0f;
    for(int i = 0; i < 4800; ++i) g = gate.Process(0.001f, threshold, true);
    assert(g == 0.0f); // Noise floor is suppressed.
    float previous = 0.0f;
    for(int i = 0; i < 50; ++i)
    {
        g = gate.Process(0.02f, threshold, true);
        assert(g >= previous && g - previous <= 1.0f / 48.0f + 1e-6f);
        previous = g;
    }
    assert(g == 1.0f); // Attack opens in about 1 ms.
    for(int i = 0; i < 4800; ++i)
        assert(gate.Process(0.007f, threshold, true) == 1.0f); // Hysteresis.
    for(int i = 0; i < 960; ++i)
        assert(gate.Process(0, threshold, true) == 1.0f); // Minimum hold.
    previous = 1.0f;
    for(int i = 0; i < 9600; ++i)
    {
        g = gate.Process(0, threshold, true);
        assert(g <= previous && previous-g <= 1.0f / 2400.0f + 1e-6f);
        previous = g;
    }
    assert(g == 0.0f);
    for(int i = 0; i < 50; ++i) g = gate.Process(0, threshold, false);
    assert(g == 1.0f); // Disabling restores unity smoothly.
    std::puts("PASS: disabled transparency, noise rejection, attack, hysteresis, hold, smooth release, disable recovery");
}
