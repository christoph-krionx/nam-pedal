# TONE3000 DIY Pedal Loader

Web app that loads NAM models from [TONE3000](https://www.tone3000.com) onto the
four presets of the DIY pedal (Daisy Seed) over USB.

## Flow

1. **Log in.** TONE3000 OAuth (PKCE) in a popup. The session is stored, so returning
   users go straight to the presets.
2. **Browse tones.** TONE3000 select flow in a popup.
3. **Pick a model.** Step through the tone's A2 models with the selector and preview
   them in the browser (`neural-amp-modeler-wasm`).
4. **Load into a preset.** Drag the on-screen rotary knob (or click an LED) to pick
   preset 1 to 4, then load the model into it. Models are validated against the
   pedal's fixed A2 nano architecture (1871 weights). Each preset can take a cab IR
   (.wav) from the same tone.
5. **Flash.** The app converts each `.nam` to `.namb`, packs a 4-entry bank in rotary
   order (empty presets are zero-size entries), and writes it to QSPI at `0x90600000`
   over WebUSB (ST DfuSe). The pedal loads whichever preset the rotary points at and
   lights the LED above it.

## Requirements

- Chrome or Edge (WebUSB and Web Serial).
- Pedal running the NAMPedal firmware with the Daisy bootloader
  (`make program-boot`, `make program-dfu`).
- A TONE3000 publishable key (`t3k_pub_...`) from Settings > API Keys with
  `http://localhost:3001` as a registered redirect URI.

## Setup

```bash
cd app
cp env.example .env   # fill in VITE_PUBLISHABLE_KEY
npm install
npm run dev           # http://localhost:3001
```

## Flashing

1. **Flash to pedal** builds the bank image.
2. **DFU mode:** with the pedal plugged in and running, **Send DFU trigger** and pick
   its serial port. The firmware gets a `'D'` byte, blinks the LEDs twice, and resets
   into the bootloader. Or hold BOOT and tap RESET on the Seed.
3. **Connect and flash:** pick the `DFU in FS Mode` device. The app erases and writes
   the bank region, then reboots the pedal.

## Code

- `src/App.tsx`: auth, tone loading, preset slots.
- `src/components/`: `Splash`, `Header`, `ToneBlock` (tone details + model selector),
  `Pedal` (rotary knob, LEDs, presets, flash), `FlashDialog`.
- `src/tone3000-client.ts`: PKCE select flow and authenticated API client.
- `src/lib/namb.ts`: port of nam-binary-loader `nam2namb`.
- `src/lib/bank.ts`: port of `pack_models.py` (bank layout, IR pipeline).
- `src/lib/webdfu.ts`: WebUSB port of `pydfu.py`.
- `src/lib/serial.ts`: Web Serial DFU trigger.

## Design

Black, greys, pure yellow accents, pure red LEDs. Square corners, no hover effects.
Arial body, Roboto Mono labels. Logos are
from the [TONE3000 API design requirements](https://www.tone3000.com/api#design-requirements).
Icons are [Lucide](https://lucide.dev). COOP/COEP headers in `vite.config.ts` are
required by the WASM preview player (SharedArrayBuffer).
