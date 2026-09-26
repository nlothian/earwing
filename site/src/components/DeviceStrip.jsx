import { useEffect, useState } from 'react';

// Live device strip: polls the firmware's /status.json (statusJson() in
// code/main.cpp) about once a second while the page is visible. Same-origin on
// the device; proxied to it by the dev server. Fails quietly: a dim
// "device offline" instead of an error banner.

const POLL_MS = 1000;
const TIMEOUT_MS = 2500;

function fmtDoa(s) {
  if (!s || s.doaAvailable === false) return null; // inbuilt mic: no DOA at all
  if (!s.trackedValid || typeof s.tracked !== 'number') return '—';
  return `${Math.round(s.tracked)}°`;
}

export default function DeviceStrip() {
  const [status, setStatus] = useState(null);
  const [offline, setOffline] = useState(false);

  useEffect(() => {
    let timer = null;
    let alive = true;
    let ctl = null;

    const schedule = () => {
      clearTimeout(timer);
      if (alive && document.visibilityState === 'visible') timer = setTimeout(poll, POLL_MS);
    };

    // One request at a time, and the next only after this one finishes: the
    // device's web server is single-threaded, so never let polls pile up.
    async function poll() {
      ctl = new AbortController();
      const t = setTimeout(() => ctl.abort(), TIMEOUT_MS);
      try {
        const res = await fetch('/status.json', { cache: 'no-store', signal: ctl.signal });
        if (!res.ok) throw new Error(String(res.status));
        const json = await res.json();
        if (!alive) return;
        setStatus(json);
        setOffline(false);
      } catch {
        if (alive) setOffline(true);
      } finally {
        clearTimeout(t);
        schedule();
      }
    }

    const onVis = () => {
      if (document.visibilityState === 'visible') {
        clearTimeout(timer);
        poll();
      } else {
        clearTimeout(timer);
      }
    };

    document.addEventListener('visibilitychange', onVis);
    poll();
    return () => {
      alive = false;
      clearTimeout(timer);
      if (ctl) ctl.abort();
      document.removeEventListener('visibilitychange', onVis);
    };
  }, []);

  if (offline || !status) {
    return (
      <div className="strip strip--offline" aria-live="polite">
        <span className="strip__dot" aria-hidden="true" />
        {offline ? 'device offline' : 'connecting to device…'}
      </div>
    );
  }

  const doa = fmtDoa(status);
  const beamDb = typeof status.beamDb === 'number' ? `${status.beamDb.toFixed(1)} dB` : '—';
  return (
    <div className="strip" aria-label="Device status">
      <span className="strip__dot strip__dot--on" aria-hidden="true" />
      <span className="strip__item">
        <span className="strip__k">source</span> {status.source || '—'}
        {status.switching ? '…' : ''}
      </span>
      <span className="strip__item">
        <span className="strip__k">mode</span> {status.mode || '—'}
      </span>
      {doa !== null && (
        <span className="strip__item">
          <span className="strip__k">DOA</span> {doa}
        </span>
      )}
      <span className="strip__item">
        <span className="strip__k">speech</span>{' '}
        <span className={`strip__gate${status.speech ? ' strip__gate--on' : ''}`}>
          {status.speech ? 'yes' : 'no'}
        </span>
      </span>
      <span className="strip__item">
        <span className="strip__k">beam</span> {beamDb}
      </span>
    </div>
  );
}
