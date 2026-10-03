import { useState, useEffect, useRef } from 'react';
import { X } from 'lucide-react';
import { PUBLISHABLE_KEY, REDIRECT_URI } from './config';
import { startOAuthPopup, handleOAuthCallbackFromPopup } from './tone3000-client';
import { t3kClient } from './client';
import type { Tone, Model, EmbeddedUser } from './types';
import type { PresetSlots } from './lib/flash';
import { analyzeNamFile, isPedalCompatible, emptySlots, isNamFile, isIrFile } from './lib/flash';
import { PEDAL_WEIGHT_COUNT } from './lib/namb';
import { Header } from './components/Header';
import { Splash } from './components/Splash';
import { ToneBlock } from './components/ToneBlock';
import { Pedal } from './components/Pedal';
import { FlashDialog } from './components/FlashDialog';
import { Spinner } from './components/Spinner';
import { T3kMark } from './components/Brand';

// Popup side of the OAuth flow: relay the callback to the opener and close.
// BroadcastChannel covers the case where window.opener was cleared by a cross-origin login.
(function relayPopupCallback() {
  if (!(window.opener || sessionStorage.getItem('t3k_popup_mode') === '1')) return;
  const q = new URLSearchParams(window.location.search);
  if (!q.has('code') && !(q.has('error') && q.has('state')) && !q.has('canceled')) return;
  const msg = {
    type: 't3k_oauth_callback',
    code: q.get('code'),
    state: q.get('state'),
    error: q.get('error'),
    tone_id: q.get('tone_id'),
    canceled: q.get('canceled') === 'true',
  };
  if (window.opener) {
    window.opener.postMessage(msg, window.location.origin);
  } else {
    const bc = new BroadcastChannel('t3k_oauth');
    bc.postMessage(msg);
    bc.close();
  }
  window.close();
})();

const LOGIN_OPTIONS = { menubar: true };
const SELECT_OPTIONS = { prompt: 'select_tone' as const, gears: 'full-rig', platform: 'nam', menubar: true, architecture: 2 };

function NoTone({ busy, onBrowse }: { busy: boolean; onBrowse: () => void }) {
  return (
    <section className="card no-tone">
      <span className="label">Tone</span>
      <p className="muted">Pick a preset on the knob, then browse TONE3000 for a tone to load into it.</p>
      <button className="btn btn-primary" disabled={busy} onClick={onBrowse}>
        <T3kMark />
        {busy ? 'Waiting for TONE3000' : 'Browse TONE3000'}
      </button>
    </section>
  );
}

export default function App() {
  const [user, setUser] = useState<EmbeddedUser | null>(null);
  const [entered, setEntered] = useState(false);
  const [tone, setTone] = useState<Tone | null>(null);
  const [models, setModels] = useState<Model[]>([]);
  const [modelIdx, setModelIdx] = useState(0);
  const [slots, setSlots] = useState<PresetSlots>(emptySlots);
  const [preset, setPreset] = useState(0);
  const [loading, setLoading] = useState(false);
  const [checking, setChecking] = useState(false);
  const [browsing, setBrowsing] = useState(false);
  const [flashOpen, setFlashOpen] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const popup = useRef<Window | null>(null);

  // Restore a stored session.
  useEffect(() => {
    if (!t3kClient.isConnected()) return;
    t3kClient.getUser().then(setUser).catch(() => t3kClient.clearTokens());
  }, []);

  useEffect(() => {
    const onMessage = async (event: MessageEvent) => {
      if (event.data?.type !== 't3k_oauth_callback') return;
      setBrowsing(false);
      const result = await handleOAuthCallbackFromPopup(PUBLISHABLE_KEY, REDIRECT_URI, event);
      if (!result) return;
      if (!result.ok) {
        if (result.error !== 'canceled') setError('Sign in failed. Try again.');
        return;
      }
      t3kClient.setTokens(result.tokens);
      setEntered(true);
      if (!user) t3kClient.getUser().then(setUser).catch(() => {});
      if (result.canceled || !result.toneId) return;

      setLoading(true);
      try {
        const [t, m] = await Promise.all([t3kClient.getTone(result.toneId), t3kClient.listModels(result.toneId, 2)]);
        setTone(t);
        setModels(m.data);
        setModelIdx(0);
      } catch {
        setError('Could not load the tone. Try again.');
      } finally {
        setLoading(false);
      }
    };

    window.addEventListener('message', onMessage);
    const bc = new BroadcastChannel('t3k_oauth');
    bc.onmessage = onMessage;
    return () => {
      window.removeEventListener('message', onMessage);
      bc.close();
    };
  }, [user]);

  // Clear the browsing state if the popup is closed without a result.
  useEffect(() => {
    if (!browsing) return;
    const id = setInterval(() => {
      if (popup.current?.closed) {
        setBrowsing(false);
        popup.current = null;
      }
    }, 500);
    return () => clearInterval(id);
  }, [browsing]);

  const open = async (options: typeof LOGIN_OPTIONS | typeof SELECT_OPTIONS) => {
    setError(null);
    setBrowsing(true);
    popup.current = await startOAuthPopup(PUBLISHABLE_KEY, REDIRECT_URI, options);
  };
  const signedIn = !!user || t3kClient.isConnected();
  const browse = () => open(SELECT_OPTIONS);
  const enter = () => (signedIn ? setEntered(true) : open(LOGIN_OPTIONS));

  const load = async (model: Model) => {
    setChecking(true);
    setError(null);
    try {
      const res = await t3kClient.fetchFile(model.model_url);
      if (!res.ok) throw new Error(`Download failed: ${res.status}`);
      const { namJson, numWeights, architecture } = analyzeNamFile(await res.text());
      if (!isPedalCompatible(numWeights)) {
        setError(
          `${model.name} is a ${architecture} model with ${numWeights} weights. ` +
            `The pedal runs A2 nano models only (${PEDAL_WEIGHT_COUNT} weights).`,
        );
        return;
      }
      setSlots((prev) => prev.map((s, i) => (i === preset ? { model, namJson, ir: s?.ir ?? null } : s)));
    } catch (err) {
      setError(`Could not load ${model.name}: ${(err as Error).message}`);
    } finally {
      setChecking(false);
    }
  };

  const namModels = models.filter(isNamFile);
  const irs = models.filter(isIrFile);

  return (
    <>
      <Header user={user} onBrowse={entered ? browse : null} />

      {error && (
        <div className="banner banner-error banner-top">
          <span>{error}</span>
          <button className="btn-icon plain" onClick={() => setError(null)} aria-label="Dismiss">
            <X size={14} />
          </button>
        </div>
      )}

      {!entered && <Splash signedIn={signedIn} busy={browsing} onContinue={enter} />}

      {entered && (
        <main className="main">
          {loading ? (
            <section className="card no-tone">
              <Spinner />
              <span className="muted">Loading tone</span>
            </section>
          ) : tone ? (
            <ToneBlock
              tone={tone}
              models={namModels}
              index={modelIdx}
              preset={preset}
              busy={checking}
              onIndex={setModelIdx}
              onLoad={load}
            />
          ) : (
            <NoTone busy={browsing} onBrowse={browse} />
          )}
          <Pedal
            slots={slots}
            selected={preset}
            irs={irs}
            onSelect={setPreset}
            onClear={(i) => setSlots((prev) => prev.map((s, j) => (j === i ? null : s)))}
            onSetIr={(i, ir) => setSlots((prev) => prev.map((s, j) => (j === i && s ? { ...s, ir } : s)))}
            onFlash={() => setFlashOpen(true)}
          />
        </main>
      )}

      {flashOpen && <FlashDialog slots={slots} onClose={() => setFlashOpen(false)} />}
    </>
  );
}
