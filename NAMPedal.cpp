// NAMPedal — Neural Amp Modeler running on a PedalPCB Terrarium (Daisy Seed,
// pure C inference)
//
// Reads A2-nano NAM models (.namb format) from a model bank in QSPI flash at
// MODEL_BANK_ADDR.  The bank is built and flashed with pack_models.py,
// independently of the firmware, so models can be swapped without recompiling.
//
// Signal chain: input gain -> NAM model -> 3-band EQ -> output volume.
//
// Terrarium controls:
//   KNOB_1 = input gain   KNOB_2 = output volume   KNOB_3 = (unused)
//   KNOB_4 = bass         KNOB_5 = mid             KNOB_6 = treble
//   FOOTSWITCH_1 = next model   FOOTSWITCH_2 = previous model
//   both footswitches together  = bypass toggle
//   LED_1 = lit when effect active   LED_2 = blinks to confirm model slot

#include "daisy_seed.h"
#include "terrarium.h"
#include "eq3band.h"
#include <cstring>

extern "C"
{
#include "nam_model.h"
}

using namespace daisy;
using namespace terrarium;

#ifdef LOGGING
#  define LOG(fmt, ...) hw.PrintLine(fmt, ##__VA_ARGS__)
#else
#  define LOG(fmt, ...) do {} while(0)
#endif

// ── Model bank ────────────────────────────────────────────────────────────
//
// The bank lives at 0x90800000 — the first address past the firmware's
// declared QSPI region (0x90040000 + 7936 KB).  pack_models.py targets this.
//
// Layout (all little-endian):
//   Header (64 bytes):
//     u32 magic      0x504D414E "NAMP"
//     u32 version    1
//     u32 num_models
//     u32 reserved[13]
//   ModelEntry[num_models] (48 bytes each):
//     char name[32]  null-padded ASCII
//     u32  offset    from start of bank to this model's .namb data
//     u32  size      .namb byte count
//     u32  reserved[2]
//   .namb data blobs (4-byte aligned)

static constexpr uint32_t MODEL_BANK_ADDR  = 0x90600000UL;
static constexpr uint32_t MODEL_BANK_MAGIC = 0x504D414E; // "NAMP"

// Maximum IR length supported.  At 48 kHz: 256 taps ≈ 5.3 ms.
// Time-domain FIR with DTCM buffers costs n × L × 3.75 cycles per block.
// At 256 taps: 256 × 48 × 3.75 ≈ 46K cycles ≈ 10% of the 480K-cycle budget.
// Combined with NAM (~65%) and EQ (~0.5%) this leaves ~25% headroom.
// 512 taps pushed real-time cost to ~87% avg, causing callback overruns.
static constexpr uint32_t MAX_IR_TAPS = 256;

struct ModelBankHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t num_models;
    uint32_t reserved[13];
};
static_assert(sizeof(ModelBankHeader) == 64,
              "ModelBankHeader must be 64 bytes");

struct ModelEntry
{
    char     name[32];
    uint32_t offset;         // .namb data offset from bank start
    uint32_t size;           // .namb data size in bytes
    uint32_t ir_offset;      // IR float32 data offset (0 = no IR)
    uint32_t ir_num_samples; // IR length in float32 samples
};
static_assert(sizeof(ModelEntry) == 48, "ModelEntry must be 48 bytes");

// ── Backup SRAM ───────────────────────────────────────────────────────────
//
// Selected model index persists across resets in 4 KB backup SRAM at
// 0x38800000.  Sentinel word guards against uninitialized reads.

static constexpr uint32_t BKPSRAM_BASE  = 0x38800000UL;
static constexpr uint32_t BKPSRAM_MAGIC = 0xA55AA55AUL;

// ── Hardware objects ──────────────────────────────────────────────────────

static DaisySeed hw;

static constexpr Pin kKnobPins[6]
    = {seed::D16, seed::D17, seed::D18, seed::D19, seed::D20, seed::D21};
static Switch footswitch1, footswitch2;
static Switch sw1, sw2, sw3, sw4;
static GPIO   led1, led2;

static Eq3Band eq;

// Set by the USB receive callback when a DFU-trigger byte ('D') is received.
// Checked in the main loop so ResetToBootloader() runs outside the ISR context.
static volatile bool dfu_requested = false;

static void OnUsbReceive(uint8_t* buf, uint32_t* len)
{
    for(uint32_t i = 0; i < *len; i++)
    {
        if(buf[i] == 'D')
        {
            dfu_requested = true;
            return;
        }
    }
}

static volatile float gain          = 1.0f;
static volatile float volume        = 1.0f;
static volatile bool  effect_active = true;

static nam_state_t   nam_state;
static volatile bool model_loaded = false;

static constexpr size_t kMaxBlockSize = NAM_MAX_BUFFER_SIZE;
static float            mono_in[kMaxBlockSize];
static float            mono_out[kMaxBlockSize];

// Time-domain FIR cabinet convolution.
//
// IR convolution buffers in DTCM (zero wait-state access).
// Coefficients are stored time-reversed for a cache-friendly forward scan.
// State buffer uses a double-write circular scheme (see IrProcess).
static NAM_DTCM float    g_ir_coeffs[MAX_IR_TAPS];
static NAM_DTCM float    g_ir_state[2 * MAX_IR_TAPS + NAM_MAX_BUFFER_SIZE - 1];
static volatile uint32_t g_ir_num_taps = 0;
static uint32_t          g_ir_head = 0; // write pos, cycles [0, MAX_IR_TAPS)

// A2-nano receptive field. Dilations × (kernel_size − 1) across all layers:
//   layers  0–13: kernel=6,  dilations [1,3,7,17,41,101,239,×2] → 4090
//   layers 14–15: kernel=15, dilations [1,13]                    →  196
//   layers 16–22: kernel=6,  dilations [1,3,7,17,41,101,239]     → 2045
static constexpr int kPrewarmSamples = 1 + 4090 + 196 + 2045;

static constexpr uint32_t NAMB_MAGIC = 0x4E414D42; // "NAMB"

// ── Backup SRAM helpers ───────────────────────────────────────────────────

static void EnableBackupSRAM()
{
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_BKPRAM_CLK_ENABLE();
}

static uint32_t GetSavedModelIndex()
{
    volatile uint32_t* bkp = reinterpret_cast<volatile uint32_t*>(BKPSRAM_BASE);
    if(bkp[0] != BKPSRAM_MAGIC)
        return 0;
    return bkp[1];
}

static void SaveModelIndex(uint32_t idx)
{
    volatile uint32_t* bkp = reinterpret_cast<volatile uint32_t*>(BKPSRAM_BASE);
    bkp[0]                 = BKPSRAM_MAGIC;
    bkp[1]                 = idx;
}

// ── Model loading ─────────────────────────────────────────────────────────

static void Prewarm(int prewarm_samples)
{
    float zero_buf[NAM_MAX_BUFFER_SIZE];
    float out_buf[NAM_MAX_BUFFER_SIZE];
    memset(zero_buf, 0, sizeof(zero_buf));

    float* in_ptrs[NAM_IN_CHANNELS];
    float* out_ptrs[NAM_OUT_CHANNELS];
    for(int i = 0; i < NAM_IN_CHANNELS; i++)
        in_ptrs[i] = zero_buf;
    for(int i = 0; i < NAM_OUT_CHANNELS; i++)
        out_ptrs[i] = out_buf;

    int processed = 0;
    while(processed < prewarm_samples)
    {
        int n = NAM_MAX_BUFFER_SIZE;
        if(n > prewarm_samples - processed)
            n = prewarm_samples - processed;
        nam_process(&nam_state, (const float* const*)in_ptrs, out_ptrs, n);
        processed += n;
    }
}

// Load the model at the saved index from the QSPI model bank.
// Returns true on success; on failure the audio callback stays in bypass.
static bool LoadModel()
{
    const ModelBankHeader* bank
        = reinterpret_cast<const ModelBankHeader*>(MODEL_BANK_ADDR);

    if(bank->magic != MODEL_BANK_MAGIC)
    {
        LOG("  no model bank at 0x%08lX (magic=0x%08lX)",
                     (unsigned long)MODEL_BANK_ADDR,
                     (unsigned long)bank->magic);
        return false;
    }
    if(bank->num_models == 0)
    {
        LOG("  model bank is empty");
        return false;
    }

    uint32_t idx = GetSavedModelIndex();
    if(idx >= bank->num_models)
        idx = 0;

    const ModelEntry* entries = reinterpret_cast<const ModelEntry*>(
        reinterpret_cast<const uint8_t*>(bank) + sizeof(ModelBankHeader));
    const ModelEntry& e = entries[idx];

    // Model data in QSPI is 4-byte aligned: MODEL_BANK_ADDR is aligned,
    // header is 64 bytes (mult of 4), entries are 48 bytes (mult of 4),
    // and pack_models.py pads blobs to 4-byte boundaries.
    const uint8_t* data
        = reinterpret_cast<const uint8_t*>(MODEL_BANK_ADDR) + e.offset;
    uint32_t size = e.size;

    LOG("  model %lu/%lu: \"%s\" (%lu bytes)",
                 (unsigned long)(idx + 1),
                 (unsigned long)bank->num_models,
                 e.name,
                 (unsigned long)size);

    if(size < 32)
    {
        LOG("  data too small for NAMB header");
        return false;
    }

    uint32_t magic;
    memcpy(&magic, data, 4);
    if(magic != NAMB_MAGIC)
    {
        LOG("  bad NAMB magic: 0x%08lX", (unsigned long)magic);
        return false;
    }

    uint32_t weights_offset, num_weights;
    memcpy(&weights_offset, data + 12, 4);
    memcpy(&num_weights, data + 16, 4);

    LOG("  weights: offset=%lu count=%lu",
                 (unsigned long)weights_offset,
                 (unsigned long)num_weights);

    if(weights_offset + num_weights * 4u > size)
    {
        LOG("  weights extend beyond data");
        return false;
    }

    nam_init(&nam_state);

    const float* weights
        = reinterpret_cast<const float*>(data + weights_offset);
    int rc = nam_load_weights(weights, (int)num_weights);
    if(rc != 0)
    {
        LOG("  nam_load_weights failed (not an A2-nano model?)");
        return false;
    }

    // ── Cabinet IR ────────────────────────────────────────────────────────
    // Clear state regardless of whether a new IR is present.
    g_ir_num_taps = 0;
    g_ir_head     = 0;
    memset(g_ir_state, 0, sizeof(g_ir_state));

    if(e.ir_offset != 0 && e.ir_num_samples > 0)
    {
        uint32_t taps = e.ir_num_samples;
        if(taps > MAX_IR_TAPS)
        {
            LOG("  IR: %lu taps truncated to %lu",
                         (unsigned long)taps,
                         (unsigned long)MAX_IR_TAPS);
            taps = MAX_IR_TAPS;
        }

        // IR float32 data sits directly in XIP QSPI (4-byte aligned by packer).
        const float* ir_src
            = reinterpret_cast<const float*>(MODEL_BANK_ADDR + e.ir_offset);

        // Store time-reversed so the inner loop scans forwards through both
        // the coefficient and state arrays (cache-friendly, compiler-vectorisable).
        for(uint32_t j = 0; j < taps; j++)
            g_ir_coeffs[j] = ir_src[taps - 1 - j];

        g_ir_num_taps = taps;
        LOG("  IR: %lu taps (%.1f ms)",
                     (unsigned long)taps,
                     (float)taps / 48.0f);
    }
    else
    {
        LOG("  IR: none");
    }

    LOG("  prewarming (%d samples)...", kPrewarmSamples);
    uint32_t t0 = System::GetNow();
    Prewarm(kPrewarmSamples);
    LOG("  model ready (%lu ms)",
                 (unsigned long)(System::GetNow() - t0));
    return true;
}

// ── UI helpers ────────────────────────────────────────────────────────────

static void ToggleBypass()
{
    effect_active = !effect_active;
    led1.Write(effect_active);
}

// Blink LED2 to confirm the active model slot (1-based count).
static void BlinkModelSlot(int slot)
{
    for(int i = 0; i < slot; i++)
    {
        led2.Write(true);
        System::Delay(150);
        led2.Write(false);
        System::Delay(150);
    }
}

static uint32_t GetBankNumModels()
{
    const ModelBankHeader* bank
        = reinterpret_cast<const ModelBankHeader*>(MODEL_BANK_ADDR);
    if(bank->magic != MODEL_BANK_MAGIC)
        return 0;
    return bank->num_models;
}

static void SwitchToModel(uint32_t idx)
{
    model_loaded = false; // audio falls through to bypass while we reload
    SaveModelIndex(idx);
    LOG("Switching to model %lu/%lu",
                 (unsigned long)(idx + 1),
                 (unsigned long)GetBankNumModels());
    model_loaded = LoadModel();
    LOG("Model load: %s", model_loaded ? "OK" : "FAILED");
    BlinkModelSlot((int)(idx + 1));
}

static void NextModel()
{
    uint32_t n = GetBankNumModels();
    if(n < 2)
        return;
    uint32_t idx = (GetSavedModelIndex() + 1) % n;
    SwitchToModel(idx);
}

static void PrevModel()
{
    uint32_t n = GetBankNumModels();
    if(n < 2)
        return;
    uint32_t idx = (GetSavedModelIndex() + n - 1) % n;
    SwitchToModel(idx);
}

// ── IR convolution ────────────────────────────────────────────────────────
//
// Double-write circular buffer FIR with time-reversed coefficients.
// All data (coefficients and state) lives in DTCM for zero wait-state access.
//
// Each new sample is written at position p and p + MAX_IR_TAPS so that any
// n-sample read window is always contiguous — no wrap-around logic in the
// inner loop, no memmove on block exit.  src and dst must be distinct.

static void IrProcess(const float* __restrict__ src,
                      float* __restrict__ dst,
                      uint32_t block_size)
{
    const uint32_t n    = g_ir_num_taps;
    const uint32_t head = g_ir_head;

    memcpy(g_ir_state + head, src, block_size * sizeof(float));
    memcpy(g_ir_state + head + MAX_IR_TAPS, src, block_size * sizeof(float));

    for(uint32_t i = 0; i < block_size; i++)
    {
        const float* s      = g_ir_state + (head + i + MAX_IR_TAPS - (n - 1));
        const float* coeffs = g_ir_coeffs;
        float        acc    = 0.0f;

        uint32_t k = n >> 2;
        while(k--)
        {
            acc += coeffs[0] * s[0] + coeffs[1] * s[1] + coeffs[2] * s[2]
                   + coeffs[3] * s[3];
            coeffs += 4;
            s += 4;
        }
        k = n & 3;
        while(k--)
            acc += *coeffs++ * *s++;

        dst[i] = acc;
    }

    g_ir_head = head + block_size;
    if(g_ir_head >= MAX_IR_TAPS)
        g_ir_head -= MAX_IR_TAPS;
}

// ── Audio callback ────────────────────────────────────────────────────────

static volatile uint32_t cb_count          = 0;
static volatile uint32_t cb_process_cycles = 0;
static volatile uint32_t cb_max_cycles     = 0;

static void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                          AudioHandle::InterleavingOutputBuffer out,
                          size_t                                size)
{
    // Force FZ/DN in this ISR context.
    __set_FPSCR(__get_FPSCR() | (1U << 24) | (1U << 25));

    cb_count++;
    size_t num_frames = size / 2;

    if(model_loaded && effect_active)
    {
        for(size_t i = 0; i < num_frames; i++)
            mono_in[i] = in[i * 2] * gain;

        float* input_ptr  = mono_in;
        float* output_ptr = mono_out;

        uint32_t cyc0 = DWT->CYCCNT;

        // Signal chain matches the NAM plugin: NAM → EQ → IR.
        nam_process(&nam_state,
                    (const float* const*)&input_ptr,
                    &output_ptr,
                    num_frames);

        // EQ in-place on mono_out (NAM output).
        for(size_t i = 0; i < num_frames; i++)
            mono_out[i] = eq.Process(mono_out[i]);

        // Cabinet IR (if loaded): mono_out → mono_in.
        // mono_in is free — NAM has consumed it — so use it as scratch.
        const float* ir_out = mono_out;
        if(g_ir_num_taps > 0)
        {
            IrProcess(mono_out, mono_in, num_frames);
            ir_out = mono_in;
        }

        uint32_t cyc1 = DWT->CYCCNT;

        uint32_t elapsed  = cyc1 - cyc0;
        cb_process_cycles = elapsed;
        if(elapsed > cb_max_cycles)
            cb_max_cycles = elapsed;

        for(size_t i = 0; i < num_frames; i++)
        {
            float s        = ir_out[i] * volume;
            out[i * 2]     = s;
            out[i * 2 + 1] = s;
        }
    }
    else
    {
        for(size_t i = 0; i < num_frames; i++)
        {
            float s        = in[i * 2] * volume;
            out[i * 2]     = s;
            out[i * 2 + 1] = s;
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────────────

int main(void)
{
    hw.Init();
    EnableBackupSRAM();

    // FPDSCR sets the default FPSCR for all new FPU contexts (including ISRs).
    uint32_t fpscr = __get_FPSCR();
    fpscr |= (1U << 24) | (1U << 25);
    __set_FPSCR(fpscr);
    volatile uint32_t* FPDSCR
        = reinterpret_cast<volatile uint32_t*>(0xE000EF3C);
    *FPDSCR |= (1U << 24) | (1U << 25);

    // Always init USB CDC so the DFU serial command works regardless of LOGGING.
    hw.StartLog(false);
    hw.usb_handle.SetReceiveCallback(OnUsbReceive, UsbHandle::FS_INTERNAL);
#ifdef LOGGING
    System::Delay(2000);
    LOG("NAMPedal (A2-nano C): booting...");
#endif

    hw.SetAudioBlockSize(kMaxBlockSize);
    eq.Init(hw.AudioSampleRate());

    AdcChannelConfig adc_cfg[6];
    for(size_t i = 0; i < 6; i++)
        adc_cfg[i].InitSingle(kKnobPins[i]);
    hw.adc.Init(adc_cfg, 6);

    footswitch1.Init(hw.GetPin(Terrarium::FOOTSWITCH_1));
    footswitch2.Init(hw.GetPin(Terrarium::FOOTSWITCH_2));
    sw1.Init(hw.GetPin(Terrarium::SWITCH_1),
             0.f,
             Switch::TYPE_TOGGLE,
             Switch::POLARITY_INVERTED,
             Switch::PULL_UP);
    sw2.Init(hw.GetPin(Terrarium::SWITCH_2),
             0.f,
             Switch::TYPE_TOGGLE,
             Switch::POLARITY_INVERTED,
             Switch::PULL_UP);
    sw3.Init(hw.GetPin(Terrarium::SWITCH_3),
             0.f,
             Switch::TYPE_TOGGLE,
             Switch::POLARITY_INVERTED,
             Switch::PULL_UP);
    sw4.Init(hw.GetPin(Terrarium::SWITCH_4),
             0.f,
             Switch::TYPE_TOGGLE,
             Switch::POLARITY_INVERTED,
             Switch::PULL_UP);
    led1.Init(seed::D22, GPIO::Mode::OUTPUT);
    led2.Init(seed::D23, GPIO::Mode::OUTPUT);

    LOG("Loading model from bank at 0x%08lX",
                 (unsigned long)MODEL_BANK_ADDR);
    model_loaded = LoadModel();
    LOG("Model load: %s", model_loaded ? "OK" : "FAILED");

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if(model_loaded)
    {
        constexpr int      kRuns   = 100;
        constexpr uint32_t kBudget = 480000;

        uint32_t nam_sum = 0, nam_max = 0;
        uint32_t eq_sum = 0, eq_max = 0;
        uint32_t ir_sum = 0, ir_max = 0;
        uint32_t tot_max = 0;

        for(int run = 0; run < kRuns; run++)
        {
            float* input_ptr  = mono_in;
            float* output_ptr = mono_out;
            memset(mono_in, 0, sizeof(mono_in));

            DWT->CYCCNT = 0;
            nam_process(&nam_state,
                        (const float* const*)&input_ptr,
                        &output_ptr,
                        kMaxBlockSize);
            uint32_t cy_nam = DWT->CYCCNT;

            DWT->CYCCNT = 0;
            for(size_t i = 0; i < kMaxBlockSize; i++)
                mono_out[i] = eq.Process(mono_out[i]);
            uint32_t cy_eq = DWT->CYCCNT;

            uint32_t cy_ir = 0;
            if(g_ir_num_taps > 0)
            {
                DWT->CYCCNT = 0;
                IrProcess(mono_out, mono_in, kMaxBlockSize);
                cy_ir = DWT->CYCCNT;
            }

            nam_sum += cy_nam;
            if(cy_nam > nam_max)
                nam_max = cy_nam;
            eq_sum += cy_eq;
            if(cy_eq > eq_max)
                eq_max = cy_eq;
            ir_sum += cy_ir;
            if(cy_ir > ir_max)
                ir_max = cy_ir;

            uint32_t tot = cy_nam + cy_eq + cy_ir;
            if(tot > tot_max)
                tot_max = tot;
        }

        uint32_t nam_avg = nam_sum / kRuns;
        uint32_t eq_avg  = eq_sum / kRuns;
        uint32_t ir_avg  = ir_sum / kRuns;
        uint32_t tot_avg = nam_avg + eq_avg + ir_avg;

        LOG("Benchmark (%d blocks x %u frames, budget=%lu cy):",
                     kRuns,
                     (unsigned)kMaxBlockSize,
                     (unsigned long)kBudget);
        LOG("           avg cy   max cy   avg%%   max%%");
        LOG("  NAM  : %7lu  %7lu  %4.1f%%  %4.1f%%",
                     (unsigned long)nam_avg,
                     (unsigned long)nam_max,
                     100.0f * (float)nam_avg / kBudget,
                     100.0f * (float)nam_max / kBudget);
        LOG("  EQ   : %7lu  %7lu  %4.1f%%  %4.1f%%",
                     (unsigned long)eq_avg,
                     (unsigned long)eq_max,
                     100.0f * (float)eq_avg / kBudget,
                     100.0f * (float)eq_max / kBudget);
        if(g_ir_num_taps > 0)
            LOG("  IR   : %7lu  %7lu  %4.1f%%  %4.1f%%  (%lu taps)",
                         (unsigned long)ir_avg,
                         (unsigned long)ir_max,
                         100.0f * (float)ir_avg / kBudget,
                         100.0f * (float)ir_max / kBudget,
                         (unsigned long)g_ir_num_taps);
        else
            LOG("  IR   :      --       --     --     --  (none)");
        LOG("  TOTAL: %7lu  %7lu  %4.1f%%  %4.1f%%  (%.3f ms avg)",
                     (unsigned long)tot_avg,
                     (unsigned long)tot_max,
                     100.0f * (float)tot_avg / kBudget,
                     100.0f * (float)tot_max / kBudget,
                     (float)tot_avg / 480000.0f);
    }

    hw.adc.Start();
    hw.StartAudio(AudioCallback);
    LOG("Audio engine started");

    led1.Write(true); // start active
    led2.Write(false);

    float    last_bass = 999.0f, last_mid = 999.0f, last_treble = 999.0f;
#ifdef LOGGING
    uint32_t last_print = System::GetNow();
#endif

    // Chord detection: pressing both switches together toggles bypass.
    // both_active prevents repeated triggers while both are held.
    bool both_active = false;

    for(;;)
    {
        if(dfu_requested)
        {
            for(int i = 0; i < 2; i++)
            {
                led1.Write(true);
                led2.Write(true);
                System::Delay(150);
                led1.Write(false);
                led2.Write(false);
                System::Delay(150);
            }
            System::ResetToBootloader(System::BootloaderMode::DAISY_SKIP_TIMEOUT);
        }

        footswitch1.Debounce();
        footswitch2.Debounce();
        sw1.Debounce();
        sw2.Debounce();
        sw3.Debounce();
        sw4.Debounce();

        gain   = hw.adc.GetFloat(Terrarium::KNOB_1) * 2.0f;
        volume = hw.adc.GetFloat(Terrarium::KNOB_2);

        // Ranges match the NAM plugin ToneStack: bass ±20 dB, mid ±15 dB, treble ±10 dB.
        float bass   = (hw.adc.GetFloat(Terrarium::KNOB_4) - 0.5f) * 40.0f;
        float mid    = (hw.adc.GetFloat(Terrarium::KNOB_3) - 0.5f) * 30.0f;
        float treble = (hw.adc.GetFloat(Terrarium::KNOB_6) - 0.5f) * 20.0f;
        if(bass != last_bass)
        {
            eq.SetBass(bass);
            last_bass = bass;
        }
        if(mid != last_mid)
        {
            eq.SetMid(mid);
            last_mid = mid;
        }
        if(treble != last_treble)
        {
            eq.SetTreble(treble);
            last_treble = treble;
        }

        if(footswitch1.RisingEdge())
        {
            if(footswitch2.Pressed())
            {
                if(!both_active)
                {
                    both_active = true;
                    ToggleBypass();
                }
            }
            else
                NextModel();
        }

        if(footswitch2.RisingEdge())
        {
            if(footswitch1.Pressed())
            {
                if(!both_active)
                {
                    both_active = true;
                    ToggleBypass();
                }
            }
            else
                PrevModel();
        }

        if(footswitch1.FallingEdge() && !footswitch2.Pressed())
            both_active = false;
        if(footswitch2.FallingEdge() && !footswitch1.Pressed())
            both_active = false;

#ifdef LOGGING
        uint32_t now = System::GetNow();
        if(now - last_print >= 1000)
        {
            float k1 = hw.adc.GetFloat(Terrarium::KNOB_1);
            float k2 = hw.adc.GetFloat(Terrarium::KNOB_2);
            float k3 = hw.adc.GetFloat(Terrarium::KNOB_3);
            float k4 = hw.adc.GetFloat(Terrarium::KNOB_4);
            float k5 = hw.adc.GetFloat(Terrarium::KNOB_5);
            float k6 = hw.adc.GetFloat(Terrarium::KNOB_6);
            LOG("cb=%lu  cycles=%lu  max=%lu  %s",
                (unsigned long)cb_count,
                (unsigned long)cb_process_cycles,
                (unsigned long)cb_max_cycles,
                effect_active ? "ACTIVE" : "BYPASS");
            LOG("  knobs k1=%.3f k2=%.3f k3=%.3f k4=%.3f k5=%.3f k6=%.3f",
                k1, k2, k3, k4, k5, k6);
            LOG("  ftsw1=%d ftsw2=%d  sw1=%d sw2=%d sw3=%d sw4=%d",
                footswitch1.Pressed() ? 1 : 0,
                footswitch2.Pressed() ? 1 : 0,
                sw1.Pressed() ? 1 : 0,
                sw2.Pressed() ? 1 : 0,
                sw3.Pressed() ? 1 : 0,
                sw4.Pressed() ? 1 : 0);
            LOG("  gain=%.2f vol=%.2f eq[%.1f %.1f %.1f]",
                gain, volume, bass, mid, treble);
            last_print = now;
        }
#endif
    }
}
