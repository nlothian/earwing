// Utterance-based transcript merger.
//
// A port of Keet's UtteranceBasedMerger (github.com/ysdede/keet) — the "v4"
// real-time merging strategy it runs on top of parakeet.js. The transcription
// side re-transcribes an OVERLAPPING audio window each round (from the mature
// cursor to the live edge), so the un-finalized tail is always decoded with
// full encoder context — no mid-word boundary artifacts. This merger turns
// those repeated, overlapping utterances into a stable transcript:
//
//   - Each process() call is a full-window replacement for the immature tail.
//   - The window text is split into sentences (the model emits . ! ? itself).
//   - Every sentence EXCEPT the last is a finalization candidate; the last
//     stays "pending" (it may still grow next round).
//   - Finalized sentences are de-duplicated by (normalized text + end-time),
//     so the overlap between consecutive windows isn't emitted twice.
//   - The mature cursor advances to the end-time of the last finalized
//     sentence; the caller starts the next window there.
//
// Keet uses wink-nlp for sentence boundary detection and falls back to a
// punctuation heuristic. We keep only the heuristic to avoid pulling in an NLP
// model — parakeet-tdt-v3's punctuation makes it accurate enough in practice.

const SENTENCE_END_RE = /[.!?]$/;
const DEDUP_TOLERANCE_SEC = 0.15;

// Strip whitespace + lowercase, for length-based sentence→word alignment.
function normalizeForBoundary(value) {
  return value.replace(/\s+/g, '').toLowerCase();
}

function normalizeSentenceText(value) {
  return value.trim().toLowerCase();
}

function joinWords(words) {
  return words
    .map((w) => w.text)
    .join(' ')
    .trim();
}

// Drop empty words and clamp times, mirroring the reference normalizeWords.
function normalizeWords(words) {
  if (!Array.isArray(words)) return [];
  const result = [];
  for (const word of words) {
    const text = String((word && word.text) || '').trim();
    if (!text) continue;
    const start = Math.max(0, Number((word && word.start_time) || 0));
    const end = Number((word && word.end_time) || 0);
    result.push({ text, start_time: start, end_time: Math.max(start, end) });
  }
  return result;
}

// Heuristic sentence splitter: greedy runs ending in . ! ? (plus a trailing
// fragment with no terminal punctuation).
function splitSentences(text) {
  const trimmed = text.trim();
  if (!trimmed) return [];
  const matched = trimmed.match(/[^.!?]+[.!?]+|[^.!?]+$/g) || [trimmed];
  return matched.map((s) => s.trim()).filter(Boolean);
}

// For each sentence, find the word index that completes it by accumulating
// normalized word characters until they cover the sentence's length.
function mapSentencesToWordBoundaries(words, sentences) {
  const boundaries = [];
  let wordIdx = 0;
  for (const sentence of sentences) {
    const target = normalizeForBoundary(sentence);
    let accumulated = '';
    for (let i = wordIdx; i < words.length; i++) {
      accumulated += normalizeForBoundary(words[i].text);
      if (accumulated.length >= target.length) {
        wordIdx = i + 1;
        break;
      }
    }
    boundaries.push(wordIdx);
  }
  return boundaries;
}

export class UtteranceMerger {
  constructor() {
    this.reset();
  }

  reset() {
    this.matureWords = []; // finalized words (the stable transcript)
    this.pendingWords = []; // current immature tail (last sentence)
    this.matureCursorTime = 0; // end-time (s) of the last finalized sentence
    this.finalizedMeta = []; // {normalizedText, endTime} for dedup
  }

  isDuplicate(text, endTime) {
    const norm = normalizeSentenceText(text);
    for (let i = this.finalizedMeta.length - 1; i >= 0; i--) {
      const m = this.finalizedMeta[i];
      if (m.normalizedText === norm && Math.abs(m.endTime - endTime) < DEDUP_TOLERANCE_SEC) {
        return true;
      }
    }
    return false;
  }

  finalizeSentence(words) {
    const text = joinWords(words);
    const endTime = words[words.length - 1].end_time;
    this.matureWords.push(...words);
    this.finalizedMeta.push({ normalizedText: normalizeSentenceText(text), endTime });
    if (endTime > this.matureCursorTime) this.matureCursorTime = endTime;
  }

  // Process one full (overlapping) window's ASR words. Returns a snapshot.
  process(words) {
    const incoming = normalizeWords(words);
    if (incoming.length === 0) return this.snapshot();

    const sentences = splitSentences(joinWords(incoming));

    if (sentences.length > 1) {
      const boundaries = mapSentencesToWordBoundaries(incoming, sentences);
      let prev = 0;
      let lastConsumed = 0;
      // Every sentence but the last is a finalization candidate.
      for (let i = 0; i < sentences.length - 1; i++) {
        const end = boundaries[i] != null ? boundaries[i] : prev;
        const sentenceWords = incoming.slice(prev, end);
        prev = end;
        if (sentenceWords.length === 0) continue;

        const text = joinWords(sentenceWords);
        const endTime = sentenceWords[sentenceWords.length - 1].end_time;
        if (this.isDuplicate(text, endTime)) {
          lastConsumed = end;
          continue;
        }
        this.finalizeSentence(sentenceWords);
        lastConsumed = end;
      }
      this.pendingWords = incoming.slice(lastConsumed);
    } else if (sentences.length === 1) {
      const text = joinWords(incoming);
      const endTime = incoming[incoming.length - 1].end_time;
      this.pendingWords = this.isDuplicate(text, endTime) ? [] : incoming;
    } else {
      this.pendingWords = incoming;
    }

    return this.snapshot();
  }

  // Stale-tail finalize: only commit a pending tail that is punctuation-complete
  // (the speaker finished a sentence and went quiet).
  finalizeTimeout() {
    if (this.pendingWords.length === 0) return this.snapshot();
    const text = joinWords(this.pendingWords);
    if (text.endsWith('...') || !SENTENCE_END_RE.test(text)) return this.snapshot();

    const endTime = this.pendingWords.reduce((m, w) => Math.max(m, w.end_time), 0);
    if (!this.isDuplicate(text, endTime)) this.finalizeSentence(this.pendingWords);
    this.pendingWords = [];
    return this.snapshot();
  }

  // Stop: commit whatever is pending regardless of punctuation.
  finalizeAll() {
    if (this.pendingWords.length > 0) {
      const text = joinWords(this.pendingWords);
      const endTime = this.pendingWords.reduce((m, w) => Math.max(m, w.end_time), 0);
      if (!this.isDuplicate(text, endTime)) this.finalizeSentence(this.pendingWords);
      this.pendingWords = [];
    }
    return this.snapshot();
  }

  snapshot() {
    return {
      matureText: joinWords(this.matureWords),
      immatureText: joinWords(this.pendingWords),
      matureCursorTime: this.matureCursorTime,
    };
  }
}
