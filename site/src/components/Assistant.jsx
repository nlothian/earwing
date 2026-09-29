// The assistant panel: summarise the transcript and answer questions about it, with
// an LFM2.5 model running on WebGPU in this browser (llm.worker.js via llmClient.js).
// This file is only the UI. Prompt building, KV-cache reuse, token budgets and
// pausing around live transcription all live in llmClient.js / gpuGate.js.
//
// The panel starts collapsed so nobody downloads 1–2 GB by accident, and nothing
// here touches the GPU until the panel is opened (the adapter probe) or Load is
// clicked (the model).
//
// Once a model is loaded the summary also re-runs on its own (useAutoSummary.js) and
// goes to the device's Meeting tab (meetingSync.js). A question asked while one of
// those runs stops it and goes first.

import { Fragment, memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { defaultLlmModel, LLM_MODEL_KEY, llmSupport, probeLlmSupport, useLlm } from './llmClient.js';
import { fmtTs as fmtTime, parseCitations } from './llmPrompt.js';
import { fsAccessSupported, resolveLlm, webkitDirectorySupported } from './localModels.js';
import { llmBytes, LLMS } from './models.js';
import { useMeetingSync } from './meetingSync.js';
import { useAutoSummary } from './useAutoSummary.js';
import { IconCheck, IconChevron, IconCopy, IconDownload, IconSend, IconSparkle, IconStop, IconTrash } from './Icons.jsx';
import { downloadText, exportName } from './download.js';

const OPEN_KEY = 'earwing-assistant-open';
const KEYS = ['large', 'small'].filter((k) => LLMS && LLMS[k]);
const NAMES = { large: 'Large', small: 'Small' };
const bytesOf = (k) => llmBytes(LLMS[k]);
const fmtGb = (n) => `${(n / 1e9).toFixed(2)} GB`;
const optionLabel = (k) => `${NAMES[k] || k} · ${LLMS[k].label} (${fmtGb(bytesOf(k))})`;
const fmtK = (n) => (n >= 1000 ? `${(n / 1000).toFixed(1)}k` : String(Math.round(n || 0)));

function readPref(key) {
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}
function writePref(key, value) {
  try {
    window.localStorage.setItem(key, value);
  } catch {
    /* private mode: just this visit */
  }
}

// navigator.clipboard needs a secure context; the fallback is for plain http (the
// assistant itself can't run there, but a chat can't exist there either, so this is
// belt and braces).
async function copyToClipboard(text) {
  try {
    await navigator.clipboard.writeText(text);
  } catch {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try {
      document.execCommand('copy');
    } catch {
      /* give up quietly */
    }
    ta.remove();
  }
}

// The hook's progress may be a 0..1 fraction (the worker's unit) or an object with
// bytes; accept both so the bar doesn't depend on that detail.
function progressInfo(p, plan) {
  const total = (p && typeof p === 'object' && p.total) || (plan && plan.bytes) || 0;
  let frac = typeof p === 'number' ? p : p && typeof p === 'object' ? p.progress : 0;
  frac = Number(frac) || 0;
  if (frac > 1) frac /= 100;
  frac = Math.min(1, Math.max(0, frac));
  const loaded = p && typeof p === 'object' && p.loaded != null ? p.loaded : frac * total;
  return { pct: Math.round(frac * 100), loaded, total };
}

function sourceText(plan) {
  if (!plan) return '';
  if (plan.source === 'local') return `from folder “${plan.folder}”`;
  if (plan.source === 'hf-cache') return 'from Hugging Face (cached)';
  if (plan.source === 'hf-download') return 'from Hugging Face (downloaded)';
  return '';
}

// ------------------------------------------------------------------ tiny markdown
// Just what the prompts ask the model for: ##/### headings, -/*/1. lists (one level
// of nesting), **bold**, paragraphs, and [m:ss] citations as buttons. Built as React
// elements, never HTML strings, so model output can't inject markup.

function withCites(text, key, onCite, maxSec) {
  const cites = parseCitations(text) || [];
  if (!cites.length) return [text];
  const out = [];
  let at = 0;
  cites.forEach((c, j) => {
    if (c.start > at) out.push(text.slice(at, c.start));
    const beyond = c.sec > maxSec + 1;
    out.push(
      <button
        type="button"
        className="cite"
        // eslint-disable-next-line react/no-array-index-key
        key={`${key}c${j}`}
        onClick={() => onCite(c.sec)}
        disabled={beyond}
        title={beyond ? 'Past the end of the transcript' : `Show ${fmtTime(c.sec)} in the transcript`}
      >
        {String(c.label || fmtTime(c.sec)).replace(/^\[|\]$/g, '')}
      </button>,
    );
    at = c.end;
  });
  if (at < text.length) out.push(text.slice(at));
  return out;
}

function inline(text, key, onCite, maxSec) {
  // Bold first, citations inside each piece (a citation inside **…** still works).
  // A trailing, still-unclosed "**" while streaming is hidden rather than flashed.
  const parts = text.replace(/\*\*(?!.*\*\*)\s*$/, '').split(/\*\*(.+?)\*\*/g);
  return parts.map((p, i) =>
    i % 2 ? (
      // eslint-disable-next-line react/no-array-index-key
      <strong key={`${key}b${i}`}>{withCites(p, `${key}b${i}`, onCite, maxSec)}</strong>
    ) : (
      // eslint-disable-next-line react/no-array-index-key
      <Fragment key={`${key}t${i}`}>{withCites(p, `${key}t${i}`, onCite, maxSec)}</Fragment>
    ),
  );
}

function parseBlocks(md) {
  const blocks = [];
  let para = null;
  let list = null;
  for (const raw of String(md || '').split('\n')) {
    const line = raw.replace(/\s+$/, '');
    if (!line.trim() || /^\s*([-*_])\1{2,}\s*$/.test(line)) {
      para = null;
      list = null;
      continue;
    }
    let m = /^\s{0,3}(#{1,6})\s+(.*?)\s*#*$/.exec(line);
    if (m) {
      para = null;
      list = null;
      blocks.push({ t: m[1].length <= 2 ? 'h3' : 'h4', text: m[2] });
      continue;
    }
    m = /^(\s*)([-*•]|\d+[.)])\s+(.*)$/.exec(line);
    if (m) {
      para = null;
      const sub = m[1].replace(/\t/g, '  ').length >= 2;
      const ordered = /\d/.test(m[2]);
      if (sub && list && list.items.length) {
        list.items[list.items.length - 1].sub.push(m[3]);
        continue;
      }
      if (!list || list.ordered !== ordered) {
        list = { t: 'list', ordered, items: [] };
        blocks.push(list);
      }
      list.items.push({ text: m[3], sub: [] });
      continue;
    }
    if (list && /^\s{2,}\S/.test(raw)) {
      // wrapped continuation of the last item
      const it = list.items[list.items.length - 1];
      if (it.sub.length) it.sub[it.sub.length - 1] += ` ${line.trim()}`;
      else it.text += ` ${line.trim()}`;
      continue;
    }
    list = null;
    if (!para) {
      para = { t: 'p', lines: [] };
      blocks.push(para);
    }
    para.lines.push(line.trim());
  }
  return blocks;
}

function Answer({ text, onCite, maxSec }) {
  const blocks = parseBlocks(text);
  return blocks.map((b, i) => {
    const k = `a${i}`;
    if (b.t === 'h3' || b.t === 'h4') {
      const H = b.t;
      return (
        <H key={k} className="ans__h">
          {inline(b.text, k, onCite, maxSec)}
        </H>
      );
    }
    if (b.t === 'list') {
      const L = b.ordered ? 'ol' : 'ul';
      return (
        <L key={k}>
          {b.items.map((it, j) => (
            // eslint-disable-next-line react/no-array-index-key
            <li key={j}>
              {inline(it.text, `${k}.${j}`, onCite, maxSec)}
              {it.sub.length ? (
                <ul>
                  {it.sub.map((s, n) => (
                    // eslint-disable-next-line react/no-array-index-key
                    <li key={n}>{inline(s, `${k}.${j}.${n}`, onCite, maxSec)}</li>
                  ))}
                </ul>
              ) : null}
            </li>
          ))}
        </L>
      );
    }
    return <p key={k}>{inline(b.lines.join(' '), k, onCite, maxSec)}</p>;
  });
}

function metaParts(m) {
  const meta = m.meta || {};
  const parts = [];
  if (meta.newTokens != null) parts.push(`${meta.newTokens} tok`);
  if (meta.tokPerSec) parts.push(`${meta.tokPerSec < 10 ? meta.tokPerSec.toFixed(1) : Math.round(meta.tokPerSec)} tok/s`);
  if (meta.coversTo != null) parts.push(`covers to [${fmtTime(meta.coversTo)}]`);
  if (meta.rebuilt) parts.push('re-read transcript');
  if (m.stopped) parts.push('stopped');
  return parts;
}

function metaTitle(meta) {
  if (!meta || meta.promptTokens == null) return undefined;
  const reused = meta.reusedTokens ? `, ${meta.reusedTokens} reused from cache` : '';
  const pre = meta.prefillMs != null ? `; read in ${(meta.prefillMs / 1000).toFixed(1)} s` : '';
  return `prompt ${meta.promptTokens} tokens${reused}${pre}`;
}

const Message = memo(function Message({ m, onCite, maxSec }) {
  const [copied, setCopied] = useState(false);
  if (m.role === 'user') {
    return (
      <li className="msg msg--user">
        <div className="msg__bubble">{m.kind === 'summary' ? m.display || 'Summary' : m.display}</div>
      </li>
    );
  }
  const text = m.display || '';
  const parts = metaParts(m);
  const onCopy = async () => {
    await copyToClipboard(text);
    setCopied(true);
    setTimeout(() => setCopied(false), 1500);
  };
  return (
    <li className="msg msg--bot">
      {m.thinking && !text ? <p className="msg__status">thinking…</p> : null}
      {text ? (
        <div className="ans">
          <Answer text={text} onCite={onCite} maxSec={maxSec} />
          {m.streaming ? <span className="ans__caret" aria-hidden="true" /> : null}
        </div>
      ) : m.streaming && !m.thinking ? (
        <p className="msg__status">…</p>
      ) : null}
      {m.error ? <p className="notice notice--error">{typeof m.error === 'string' ? m.error : m.error.message}</p> : null}
      {!m.streaming && (text || parts.length) ? (
        <div className="msg__foot">
          {text ? (
            <button
              type="button"
              className={`iconbtn msg__copy${copied ? ' iconbtn--ok' : ''}`}
              onClick={onCopy}
              title={copied ? 'Copied' : 'Copy this answer'}
              aria-label={copied ? 'Copied' : 'Copy this answer'}
            >
              {copied ? <IconCheck size={14} /> : <IconCopy size={14} />}
            </button>
          ) : null}
          {text && m.kind === 'summary' ? (
            <button
              type="button"
              className="iconbtn msg__copy"
              onClick={() => downloadText(exportName('summary', 'md'), text, 'text/markdown')}
              title="Download this summary (.md)"
              aria-label="Download this summary (.md)"
            >
              <IconDownload size={14} />
            </button>
          ) : null}
          {parts.length ? (
            <span className="msg__meta" title={metaTitle(m.meta)}>
              {parts.join(' · ')}
            </span>
          ) : null}
        </div>
      ) : null}
    </li>
  );
});

const fmtAgo = (ms) => {
  const s = Math.max(0, Math.round(ms / 1000));
  return s < 60 ? `${s}s` : s < 3600 ? `${Math.floor(s / 60)}m` : `${Math.floor(s / 3600)}h`;
};

// "live summary rev 3 · synced 12s ago": what the device's Meeting tab has. Ticks on
// its own, so the panel doesn't re-render every second.
function LiveStatus({ sync }) {
  const [, setNow] = useState(0);
  useEffect(() => {
    if (!sync.syncedAt) return undefined;
    const t = setInterval(() => setNow((n) => n + 1), 1000);
    return () => clearInterval(t);
  }, [sync.syncedAt]);
  if (!sync.rev) return null;
  const parts = [`live summary rev ${sync.rev}`];
  if (sync.off) parts.push('this device’s firmware has no meeting display');
  else {
    if (sync.syncedAt) parts.push(`synced ${fmtAgo(Date.now() - sync.syncedAt)} ago`);
    if (sync.result === 'ignored') parts.push('display is showing another tab');
    else if (sync.error) parts.push(sync.error);
    else if (!sync.syncedAt) parts.push('sending to the display…');
  }
  return <p className="meta">{parts.join(' · ')}</p>;
}

// ------------------------------------------------------------------ the panel

export default function Assistant({
  words,
  wordsVersion,
  timeline,
  labels,
  live,
  final,
  take,
  onCite,
  folder,
  getLocalEntries,
  regrantFolder,
  onPickFolder,
  asrOnGpu,
  onOpenChange,
}) {
  const llm = useLlm();
  const [open, setOpen] = useState(false);
  const [support, setSupport] = useState(null); // null (not checked) | { ok, reason }
  const [localErr, setLocalErr] = useState(null); // { message } | { missing, folder }
  const [draft, setDraft] = useState('');
  const [showPaused, setShowPaused] = useState(false);
  const chatRef = useRef(null);
  const stickRef = useRef(true); // follow new tokens unless the user scrolled up
  const taRef = useRef(null);
  const typedAt = useRef(0); // last keystroke in the question box: auto runs wait

  const model = llm.model || defaultLlmModel();
  const state = llm.state;
  const loaded = llm.loadedModel;
  const messages = llm.messages || [];
  // words is Transcriber’s live (mutated) array; a new wordsVersion is what re-renders us.
  const nWords = words ? words.length : 0;
  // Citations past the last transcribed word can't be shown.
  const maxSec = nWords ? Math.ceil(words[nWords - 1].end_time || 0) : 0;

  // Restore the open state after mount (SSR-free island, but keep the first render
  // deterministic), and tell the page so it can switch to the two-column layout.
  useEffect(() => {
    if (readPref(OPEN_KEY) === 'open') setOpen(true);
  }, []);
  useEffect(() => {
    if (onOpenChange) onOpenChange(open);
  }, [open, onOpenChange]);

  // WebGPU check: the cheap synchronous one at once, the adapter probe only once
  // the panel is opened.
  useEffect(() => {
    if (!open || support) return undefined;
    const quick = llmSupport();
    if (!quick.ok) {
      setSupport(quick);
      return undefined;
    }
    let alive = true;
    probeLlmSupport().then(
      (r) => alive && setSupport(r),
      (e) => alive && setSupport({ ok: false, reason: (e && e.message) || String(e) }),
    );
    return () => {
      alive = false;
    };
  }, [open, support]);

  const toggle = useCallback(() => {
    setOpen((o) => {
      writePref(OPEN_KEY, o ? 'closed' : 'open');
      return !o;
    });
  }, []);

  // A new take (Start or File) starts a new chat: the old answers were about other audio.
  const takeRef = useRef(take);
  useEffect(() => {
    if (takeRef.current === take) return;
    takeRef.current = take;
    if (llm.state === 'busy') llm.stop();
    if ((llm.messages || []).length) llm.clear();
    // llm is a fresh object each render; only the take matters here.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [take]);

  // "paused for live transcription" only once a pause lasts: most last a few
  // hundred ms (one Parakeet window) and would just flicker.
  const paused = llm.phase && llm.phase.kind === 'paused';
  useEffect(() => {
    if (!paused) {
      setShowPaused(false);
      return undefined;
    }
    const t = setTimeout(() => setShowPaused(true), 500);
    return () => clearTimeout(t);
  }, [paused]);

  // Auto-scroll while answers stream, unless the user has scrolled up to read.
  const onChatScroll = useCallback(() => {
    const el = chatRef.current;
    if (el) stickRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
  }, []);
  const lastMsg = messages[messages.length - 1];
  useEffect(() => {
    const el = chatRef.current;
    if (el && stickRef.current) el.scrollTop = el.scrollHeight;
  }, [messages.length, lastMsg && lastMsg.display, lastMsg && lastMsg.thinking]);

  const onModelChange = useCallback(
    (e) => {
      const key = e.target.value;
      writePref(LLM_MODEL_KEY, key);
      setLocalErr(null);
      llm.setModel(key);
    },
    [llm],
  );

  // Load (or switch to) the selected model: from the local folder if there is one and
  // it has the files, else from Hugging Face. regrantFolder() must run before any
  // await, while the click's user activation still counts.
  const onLoad = useCallback(() => {
    const regrant = folder && regrantFolder ? regrantFolder() : Promise.resolve();
    setLocalErr(null);
    (async () => {
      try {
        await regrant;
      } catch {
        /* getLocalEntries reports the permission problem */
      }
      let entries = null;
      try {
        entries = getLocalEntries ? await getLocalEntries() : null;
      } catch (e) {
        setLocalErr({ message: (e && e.message) || String(e) });
        return;
      }
      if (!entries) {
        llm.load(null);
        return;
      }
      const r = resolveLlm(entries, model);
      if (r && r.files) llm.load({ name: folder ? folder.name : 'folder', files: r.files });
      else setLocalErr({ missing: (r && r.missing) || [], folder: folder ? folder.name : 'folder' });
    })();
  }, [folder, getLocalEntries, llm, model, regrantFolder]);

  const onUseHf = useCallback(() => {
    setLocalErr(null);
    llm.load(null);
  }, [llm]);

  const src = () => ({ words: (words || []).slice(), timeline, labels, final, take });

  const auto = useAutoSummary(llm, src, () => typedAt.current, { wordsVersion, take });
  const sync = useMeetingSync(llm.lastSummary, take);

  const hasSummary = messages.some((m) => m.role === 'assistant' && m.kind === 'summary' && !m.error);
  const busy = state === 'busy';
  const autoBusy = busy && llm.busyKind === 'auto';
  const canChat = !!loaded && (state === 'ready' || state === 'busy' || state === 'error');
  const canSend = canChat && !busy && nWords > 0;
  // A question may interrupt the live summary.
  const canAsk = canChat && (!busy || autoBusy) && nWords > 0;

  const onSummarise = () => {
    if (canSend) llm.summarise(src());
  };
  const onAsk = async () => {
    const q = draft.trim();
    if (!q || !canAsk) return;
    const request = src();
    setDraft('');
    stickRef.current = true;
    if (taRef.current) taRef.current.style.height = '';
    if (autoBusy) await llm.preemptAuto();
    llm.ask(q, request);
  };
  const onStop = () => {
    if (llm.busyKind === 'auto') auto.suspend(); // and leave the live summary alone a while
    llm.stop();
  };
  const onKey = (e) => {
    if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
      e.preventDefault();
      onAsk();
    }
  };
  const onDraft = (e) => {
    typedAt.current = Date.now();
    setDraft(e.target.value);
    // Grow with the text up to the CSS max-height, then scroll.
    const ta = e.target;
    ta.style.height = 'auto';
    ta.style.height = `${ta.scrollHeight + 2}px`;
  };

  const unsupported = state === 'unsupported' || (support && !support.ok);
  const secure = typeof window === 'undefined' || window.isSecureContext;
  const host = typeof window === 'undefined' ? 'earwing.local' : window.location.hostname || 'earwing.local';
  const canFolder = fsAccessSupported() || !!folder || webkitDirectorySupported();
  const label = LLMS[model] ? LLMS[model].label : model;
  const loadLabel = !loaded ? 'Load' : loaded !== model ? `Switch to ${(NAMES[model] || model).toLowerCase()}` : null;
  const prog = progressInfo(llm.progress, llm.plan);
  const phase = llm.phase;
  const stats = llm.stats || {};

  const phaseText = showPaused
    ? 'paused for live transcription'
    : phase && phase.kind === 'prefill'
      ? `reading transcript ${fmtK(phase.done)}/${fmtK(phase.total)} tokens`
      : phase && phase.kind === 'rebuild'
        ? `the transcript changed earlier on, so reading it again${phase.tokens ? ` (${fmtK(phase.tokens)} tokens)` : ''}`
        : phase && phase.kind === 'generate'
          ? 'writing…'
          : '';

  const statsText = (() => {
    if (!asrOnGpu || !(stats.samples || stats.samplesWithLlm)) return '';
    const med = (ms) => `${(ms / 1000).toFixed(2)} s`;
    let s = stats.samples
      ? `ASR window ${med(stats.asrMedianMs)} median${stats.samplesWithLlm ? ` (${med(stats.asrMedianMsWithLlm)} while assistant runs)` : ''}`
      : `ASR window ${med(stats.asrMedianMsWithLlm)} median while assistant runs`;
    if (stats.llmActiveMs) s += ` · LLM paused ${Math.round((100 * (stats.pausedMs || 0)) / stats.llmActiveMs)}%`;
    return s;
  })();

  const err = llm.error;

  return (
    <section className={`card assistant${open ? ' assistant--open' : ''}`} aria-label="Assistant">
      <div className="assistant__head">
        <button type="button" className="assistant__toggle" onClick={toggle} aria-expanded={open}>
          <IconSparkle size={18} />
          <span className="assistant__title">Assistant</span>
          <span className="assistant__sub">summarise &amp; ask, runs in this browser</span>
          <span className="assistant__chev">
            <IconChevron size={18} />
          </span>
        </button>
      </div>

      {!open ? null : unsupported ? (
        <p className="notice">
          {!secure ? (
            <>
              The assistant runs a language model on this computer’s GPU, which needs WebGPU, and
              plain http pages don’t get it. Open{' '}
              <a href={`https://${host}/`}>https://{host}/</a> to use it.
            </>
          ) : (
            <>
              The assistant needs WebGPU with 16-bit floats (shader-f16), and this browser doesn’t offer
              it{support && support.reason ? ` (${support.reason})` : ''}. Try a recent Chrome or Edge on
              a computer with a GPU.
            </>
          )}
        </p>
      ) : (
        <>
          <div className="assistant__model">
            <label className="select">
              <span className="visually-hidden">Assistant model</span>
              <select
                name="llm-model"
                value={model}
                onChange={onModelChange}
                disabled={state === 'loading' || busy}
                title="Larger answers better; smaller loads faster and needs less memory"
              >
                {KEYS.map((k) => (
                  <option key={k} value={k}>
                    {optionLabel(k)}
                  </option>
                ))}
              </select>
            </label>
            {loadLabel ? (
              <button
                type="button"
                className={`btn ${loaded ? 'btn--ghost' : 'btn--primary'}`}
                onClick={onLoad}
                disabled={state === 'loading' || busy || !support}
              >
                {loadLabel}
              </button>
            ) : null}
          </div>

          <p className="folder">
            {loaded && llm.plan ? (
              <>
                <span title={llm.adapter || undefined}>
                  {LLMS[loaded] ? LLMS[loaded].label : loaded} {sourceText(llm.plan)}
                </span>
                {!busy ? (
                  <button
                    type="button"
                    className="linkbtn linkbtn--quiet"
                    onClick={() => llm.unload()}
                    title="Free the GPU memory the model uses"
                  >
                    unload
                  </button>
                ) : null}
              </>
            ) : state === 'loading' ? null : folder ? (
              <span>Model files from folder “{folder.name}”, if it has them</span>
            ) : canFolder ? (
              <>
                <button type="button" className="linkbtn" onClick={onPickFolder}>
                  Load model from folder…
                </button>
                <span> or download it from Hugging Face ({fmtGb(bytesOf(model))}, once)</span>
              </>
            ) : null}
          </p>

          {state === 'loading' && (
            <div>
              <div className="progress" role="progressbar" aria-valuenow={prog.pct}>
                <div className="progress__bar" style={{ width: `${prog.pct}%` }} />
              </div>
              <p className="meta">
                {!llm.plan
                  ? 'Starting WebGPU…'
                  : llm.plan.source === 'local'
                    ? `Loading ${label} from the folder “${llm.plan.folder}” (${fmtGb(llm.plan.bytes)})… ${prog.pct}%`
                    : llm.plan.source === 'hf-cache'
                      ? `Loading ${label} from this browser’s cache… ${prog.pct}%`
                      : `Downloading ${label}… ${prog.pct}% — ${fmtGb(prog.loaded)} of ${fmtGb(prog.total)} from Hugging Face on first use, then cached in this browser.`}
              </p>
              {live ? (
                <p className="meta">Loading the model can briefly delay live transcription.</p>
              ) : null}
            </div>
          )}

          {localErr ? (
            <p className="notice notice--error">
              {localErr.missing
                ? `Folder “${localErr.folder}” has no ${label} files (missing ${localErr.missing.join(', ') || 'files'}). Run `
                : `${localErr.message} `}
              {localErr.missing ? (
                <>
                  <code>npm run models -- llm-{model}</code>.{' '}
                </>
              ) : null}
              <button type="button" className="linkbtn" onClick={onUseHf}>
                Download from Hugging Face instead
              </button>
            </p>
          ) : null}

          {err && state !== 'loading' ? (
            <p className="notice notice--error">
              {err.message || String(err)}
              {err.missing && err.missing.length ? ` (missing ${err.missing.join(', ')})` : ''}{' '}
              {!loaded ? (
                <>
                  <button type="button" className="linkbtn" onClick={onLoad}>
                    Retry
                  </button>
                  {err.code === 'local-missing' || (llm.plan && llm.plan.source === 'local') ? (
                    <>
                      {' · '}
                      <button type="button" className="linkbtn" onClick={onUseHf}>
                        Download from Hugging Face instead
                      </button>
                    </>
                  ) : null}
                </>
              ) : null}
            </p>
          ) : null}

          {canChat || messages.length ? (
            <ol className="chat" ref={chatRef} onScroll={onChatScroll} aria-live="polite">
              {messages.length ? (
                messages.map((m) => <Message key={m.id} m={m} onCite={onCite} maxSec={maxSec} />)
              ) : (
                <li className="chat__empty">
                  {nWords
                    ? 'Summarise the transcript, or ask about it. Times like 12:34 in answers jump to that point in the transcript.'
                    : 'Once something has been transcribed, summarise it or ask about it here.'}
                </li>
              )}
            </ol>
          ) : (
            <p className="chat__intro">
              Summaries and answers about the transcript, from a LiquidAI LFM2.5 model running on this
              computer’s GPU. The model downloads once; nothing you say leaves the browser.
            </p>
          )}

          {canChat ? (
            <div className="assistant__foot">
              <div className="assistant__actions">
                <button
                  type="button"
                  className="btn btn--ghost"
                  onClick={onSummarise}
                  disabled={!canSend}
                  title={nWords ? undefined : 'Nothing transcribed yet'}
                >
                  {hasSummary ? 'Update summary' : 'Summarise'}
                </button>
                <button
                  type="button"
                  className="iconbtn assistant__clear"
                  onClick={() => llm.clear()}
                  disabled={busy || !messages.length}
                  title="Clear the chat"
                  aria-label="Clear the chat"
                >
                  <IconTrash size={16} />
                </button>
              </div>
              <div className="ask">
                <label className="ask__box">
                  <span className="visually-hidden">Question about the transcript</span>
                  <textarea
                    ref={taRef}
                    name="llm-question"
                    rows={1}
                    value={draft}
                    placeholder="Ask about the transcript…"
                    onChange={onDraft}
                    onKeyDown={onKey}
                  />
                </label>
                {busy && (!autoBusy || !draft.trim()) ? (
                  <button
                    type="button"
                    className="btn btn--stop ask__btn"
                    onClick={onStop}
                    title={autoBusy ? 'Stop the live summary (it resumes in 2 minutes)' : 'Stop'}
                  >
                    <IconStop size={16} />
                    <span>Stop</span>
                  </button>
                ) : (
                  <button
                    type="button"
                    className="btn btn--primary ask__btn"
                    onClick={onAsk}
                    disabled={!canAsk || !draft.trim()}
                    title={
                      autoBusy
                        ? 'Ask now, stopping the live summary (Enter; Shift+Enter for a new line)'
                        : 'Ask (Enter; Shift+Enter for a new line)'
                    }
                  >
                    <IconSend size={16} />
                    <span>Ask</span>
                  </button>
                )}
              </div>
              {phaseText ? <p className="meta">{phaseText}</p> : null}
              <LiveStatus sync={sync} />
              {statsText ? <p className="meta assistant__stats">{statsText}</p> : null}
            </div>
          ) : null}
        </>
      )}
    </section>
  );
}
