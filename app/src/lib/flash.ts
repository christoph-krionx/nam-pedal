// Preset slots -> .namb conversion -> bank image -> DfuSe write to QSPI.

import { convertNamToNamb, PEDAL_WEIGHT_COUNT } from './namb';
import { buildBank, decodeWavChannel0At48k, processIr, MODEL_BANK_ADDR, MODEL_BANK_MAX_SIZE } from './bank';
import type { BankModel } from './bank';
import { DfuDevice } from './webdfu';
import type { FlashProgress } from './webdfu';
import type { Model } from '../types';

export interface Preset {
  model: Model;
  namJson: unknown;
  ir: Model | null; // optional .wav cab IR
}

// Rotary position N loads bank entry N-1, so the bank always has NUM_PRESETS entries.
export const NUM_PRESETS = 4;
export type PresetSlots = (Preset | null)[];
export const emptySlots = (): PresetSlots => Array(NUM_PRESETS).fill(null);

// Firmware treats a zero-size entry as "no model" and passes audio through.
const EMPTY_PRESET_NAME = '(empty)';

export const isNamFile = (m: Model) => m.model_url.toLowerCase().endsWith('.nam');
export const isIrFile = (m: Model) => m.model_url.toLowerCase().endsWith('.wav');

/** Parse and convert a .nam so incompatible models are rejected before staging. */
export function analyzeNamFile(text: string): { namJson: unknown; numWeights: number; architecture: string } {
  const namJson = JSON.parse(text);
  const { numWeights, architecture } = convertNamToNamb(namJson);
  return { namJson, numWeights, architecture };
}

export const isPedalCompatible = (numWeights: number) => numWeights === PEDAL_WEIGHT_COUNT;

export async function buildBankImage(
  slots: PresetSlots,
  fetchFile: (modelUrl: string) => Promise<ArrayBuffer>,
): Promise<Uint8Array> {
  if (slots.every((s) => s === null)) throw new Error('No presets assigned');

  const models: BankModel[] = [];
  for (const s of slots) {
    if (!s) {
      models.push({ name: EMPTY_PRESET_NAME, namb: new Uint8Array(0), ir: null });
      continue;
    }
    const namb = convertNamToNamb(s.namJson).data;
    const ir = s.ir ? processIr(await decodeWavChannel0At48k(await fetchFile(s.ir.model_url)), 48000) : null;
    models.push({ name: s.model.name, namb, ir });
  }

  const bank = buildBank(models);
  if (bank.length > MODEL_BANK_MAX_SIZE) {
    throw new Error(`Bank is ${(bank.length / 1024).toFixed(0)} KB, limit is ${MODEL_BANK_MAX_SIZE / 1024} KB`);
  }
  return bank;
}

/** Caller puts the pedal in DFU mode first. */
export async function flashBank(bank: Uint8Array, progress?: FlashProgress): Promise<void> {
  const dfu = await DfuDevice.request();
  try {
    await dfu.flash(MODEL_BANK_ADDR, bank, progress);
    await dfu.leave();
  } catch (err) {
    await dfu.close();
    throw err;
  }
}
