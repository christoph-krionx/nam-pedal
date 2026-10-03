// Pedal panel: rotary preset knob with 4 red LEDs, preset rows, flash button.
import { useEffect, useRef, useState } from 'react';
import { X, Zap } from 'lucide-react';
import type { Model } from '../types';
import type { PresetSlots } from '../lib/flash';

// Knob geometry in SVG units. LEDs sit on an arc above the knob, one per preset.
const VIEW_W = 300;
const CX = 150;
const CY = 160;
const LED_R = 104;
const LABEL_R = 128;
const TIP = 76; // pointer length from center
const ANGLES = [-60, -20, 20, 60]; // degrees from 12 o'clock
const SWEEP = 80; // drag limit past the end positions

const polar = (r: number, deg: number) => {
  const a = (deg * Math.PI) / 180;
  return { x: CX + r * Math.sin(a), y: CY - r * Math.cos(a) };
};

const nearest = (deg: number) =>
  ANGLES.reduce((best, a, i) => (Math.abs(a - deg) < Math.abs(ANGLES[best] - deg) ? i : best), 0);

// Chicken-head outline, pointing up: round rear, straight taper, chamfered nose.
const KNOB_PATH = [
  `M ${CX - 26} ${CY + 10}`,
  `L ${CX - 11} ${CY - TIP + 10}`,
  `L ${CX - 6} ${CY - TIP}`,
  `L ${CX + 6} ${CY - TIP}`,
  `L ${CX + 11} ${CY - TIP + 10}`,
  `L ${CX + 26} ${CY + 10}`,
  `A 26 26 0 1 1 ${CX - 26} ${CY + 10}`,
  'Z',
].join(' ');

interface FaceProps {
  selected: number;
  onSelect: (i: number) => void;
}

function Face({ selected, onSelect }: FaceProps) {
  const svg = useRef<SVGSVGElement>(null);
  const [drag, setDrag] = useState<number | null>(null);
  const angle = drag ?? ANGLES[selected];

  // Track the pointer on window while dragging so the knob follows even outside the SVG.
  useEffect(() => {
    if (drag === null) return;
    const onMove = (e: PointerEvent) => {
      const r = svg.current!.getBoundingClientRect();
      const s = r.width / VIEW_W;
      const dx = e.clientX - (r.left + CX * s);
      const dy = e.clientY - (r.top + CY * s);
      const a = Math.max(-SWEEP, Math.min(SWEEP, (Math.atan2(dx, -dy) * 180) / Math.PI));
      setDrag(a);
      const i = nearest(a);
      if (i !== selected) onSelect(i);
    };
    const onUp = () => setDrag(null);
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onUp);
    return () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onUp);
    };
  }, [drag, selected, onSelect]);

  return (
    <svg ref={svg} className="pedal-face" viewBox={`0 0 ${VIEW_W} 212`} role="group" aria-label="Preset selector">
      {ANGLES.map((deg, i) => {
        const led = polar(LED_R, deg);
        const label = polar(LABEL_R, deg);
        const on = i === selected;
        return (
          <g key={i} className="hit" onClick={() => onSelect(i)} role="button" aria-label={`Preset ${i + 1}`}>
            <circle cx={led.x} cy={led.y} r={18} fill="transparent" />
            <circle cx={led.x} cy={led.y} r={7} className={`led${on ? ' on' : ''}`} />
            <text x={label.x} y={label.y + 4} className={`led-label${on ? ' on' : ''}`}>
              {i + 1}
            </text>
          </g>
        );
      })}
      <g
        className={`knob${drag !== null ? ' dragging' : ''}`}
        role="slider"
        aria-label="Preset knob"
        aria-valuemin={1}
        aria-valuemax={ANGLES.length}
        aria-valuenow={selected + 1}
        transform={`rotate(${angle} ${CX} ${CY})`}
        onPointerDown={() => setDrag(angle)}
      >
        <path d={KNOB_PATH} className="knob-body" />
        <line x1={CX} y1={CY - TIP + 6} x2={CX} y2={CY - 22} className="knob-stripe" />
      </g>
    </svg>
  );
}

interface Props {
  slots: PresetSlots;
  selected: number;
  irs: Model[];
  onSelect: (i: number) => void;
  onClear: (i: number) => void;
  onSetIr: (i: number, ir: Model | null) => void;
  onFlash: () => void;
}

export function Pedal({ slots, selected, irs, onSelect, onClear, onSetIr, onFlash }: Props) {
  const filled = slots.filter(Boolean).length;

  return (
    <aside className="card">
      <div className="section-title">
        <span className="label">Pedal</span>
        <span className="label">{filled}/{slots.length}</span>
      </div>

      <Face selected={selected} onSelect={onSelect} />

      <div className="presets">
        {slots.map((p, i) => (
          <div
            key={i}
            className={`preset${i === selected ? ' selected' : ''}`}
            onClick={() => onSelect(i)}
            role="button"
            tabIndex={0}
            onKeyDown={(e) => e.key === 'Enter' && onSelect(i)}
          >
            <span className="preset-num">{i + 1}</span>
            <div className="preset-body">
              <span className={`preset-name${p ? '' : ' empty'}`}>{p ? p.model.name : 'Empty'}</span>
              {p && irs.length > 0 && (
                <label className="preset-ir" onClick={(e) => e.stopPropagation()}>
                  IR
                  <select
                    value={p.ir ? String(p.ir.id) : ''}
                    onChange={(e) => onSetIr(i, irs.find((m) => String(m.id) === e.target.value) ?? null)}
                  >
                    <option value="">None</option>
                    {irs.map((ir) => (
                      <option key={ir.id} value={String(ir.id)}>{ir.name}</option>
                    ))}
                  </select>
                </label>
              )}
            </div>
            {p && (
              <button
                className="btn-icon plain"
                aria-label="Clear preset"
                onClick={(e) => {
                  e.stopPropagation();
                  onClear(i);
                }}
              >
                <X size={14} />
              </button>
            )}
          </div>
        ))}
      </div>

      <button className="btn btn-primary btn-block pedal-flash" disabled={filled === 0} onClick={onFlash}>
        <Zap size={14} />
        Flash to pedal
      </button>
    </aside>
  );
}
