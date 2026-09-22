// T3K Pedal Header
// Pin definitions for the T3K Pedal board (Daisy Seed).
// Values below are placeholders — update to match the T3K schematic.

namespace t3k_pedal {
class T3kPedal {
public:
  enum Sw {
    FOOTSWITCH = 30,
    // Rotary preset switch: one common pin per position, grounded
    // when that position is selected, floating otherwise.
    ROTARY_1 = 5,
    ROTARY_2 = 4,
    ROTARY_3 = 3,
    ROTARY_4 = 2
  };

  // Index into the ADC channel array (see kKnobPins in NAMPedal.cpp) —
  // not a Seed pin number. kKnobPins must list pins in this same order.
  enum Knob {
    INPUT_GAIN = 0,
    NOISE_GATE_THRESHOLD = 1,
    OUTPUT_VOLUME = 2,
    BASS = 3,
    MID = 4,
    TREBLE = 5,
  };

  enum LED {
    LED_STATUS = 31, // on/bypass indicator, blinks on clip
    LED_PRESET_1 = 32,
    LED_PRESET_2 = 33,
    LED_PRESET_3 = 34,
    LED_PRESET_4 = 35
  };
};
} // namespace t3k_pedal
