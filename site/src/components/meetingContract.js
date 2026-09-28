// The live meeting summary the Assistant sends to the device's display:
// POST /meeting with this JSON after each summary, and again every
// HEARTBEAT_MS with `hb: true`. The device side, with the full field rules
// and its accept policy, is ../../../code/meeting_types.h; the two must agree.
// scripts/fixtures/meeting-sample.json is a complete example.

export const MEETING_VERSION = 1;
export const MEETING_PATH = '/meeting';

// Caps. The browser keeps to these (the device truncates anything longer).
export const MAX_BODY = 15000; // bytes; the device refuses over 16384
export const SUMMARY_MAX = 560;
export const TEXT_MAX = 180; // one section item
export const FEED_TEXT_MAX = 360; // one feed entry (a summary entry is the long one)
export const T_MAX = 8; // "h:mm:ss"
export const SESSION_MAX = 23;
export const ITEMS_MAX = 12; // per section
export const FEED_MAX = 12;
export const LVL_MAX = 3;

export const HEARTBEAT_MS = 30000;
export const POST_TIMEOUT_MS = 4000;

// The item lists, in device order (after Follow on its mode buttons).
export const SECTION_KEYS = ['key', 'decisions', 'actions', 'questions'];
// Feed sections: the lists plus the summary paragraph.
export const FEED_SECS = [...SECTION_KEYS, 'summary'];
export const FEED_KINDS = ['new', 'edited', 'summary'];

// The device's answer to a 200: what it did with the post.
export const RESULTS = ['replaced', 'touched', 'ignored', 'old'];

/**
 * @typedef {'key'|'decisions'|'actions'|'questions'} SectionKey
 * @typedef {{ text: string, t: string, new: boolean }} MeetingItem
 *   text: printable ASCII, citations and Markdown stripped; t: its [mm:ss]
 *   citation without brackets, or ''; new: changed in this rev
 * @typedef {{ sec: SectionKey|'summary', kind: 'new'|'edited'|'summary',
 *   text: string, t: string, lvl: number, ago: number }} MeetingFeedEntry
 *   lvl: 0 (the latest change set) .. LVL_MAX (faded); ago: seconds since
 *   the change, at send time
 * @typedef {{ v: 1, session: string, rev: number, seq: number, hb: boolean,
 *   coversTo: string, summary: string,
 *   sections: Record<SectionKey, MeetingItem[]>,
 *   feed: MeetingFeedEntry[] }} MeetingPayload
 *   session: [A-Za-z0-9-]{1,23}, "<random6 per page load>-<take>";
 *   rev: summaries completed this session; seq: posts sent (heartbeats too);
 *   feed: newest first
 * @typedef {{ ok: true, result: 'replaced'|'touched'|'ignored'|'old',
 *   session: string, rev: number }} MeetingResponse
 */
