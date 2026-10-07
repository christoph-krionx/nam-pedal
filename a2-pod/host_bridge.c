#include "nam_model.h"
static nam_state_t state;
void host_reset(void) { nam_init(&state); }
void host_process(const float* input, float* output, int frames)
{
    const float* inputs[] = {input};
    float* outputs[] = {output};
    nam_process(&state, inputs, outputs, frames);
}
