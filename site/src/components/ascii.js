// toAscii(): text for the device's display, whose TFT fonts have printable ASCII
// (0x20–0x7E) and nothing else. Typographic punctuation, arrows and the few letters
// NFKD can't split get a lookup; accented letters lose their accents (NFKD, then the
// combining marks dropped); whatever is left becomes '?'. Whitespace collapses.
// Pure: meetingSummary.js runs it on the parsed summary, and the tests in Node.

const TABLE = {
  // quotes and primes
  '\u2018': "'", '\u2019': "'", '\u201A': "'", '\u201B': "'", '\u2032': "'", '\u2039': "'", '\u203A': "'",
  '\u201C': '"', '\u201D': '"', '\u201E': '"', '\u201F': '"', '\u2033': '"', '\u00AB': '"', '\u00BB': '"',
  // dashes and minus
  '\u2010': '-', '\u2011': '-', '\u2012': '-', '\u2013': '-', '\u2014': '-', '\u2015': '-', '\u2212': '-',
  // ellipsis, bullets, middle dot
  '\u2026': '...', '\u2022': '*', '\u2023': '*', '\u25CF': '*', '\u25E6': '*', '\u2043': '-', '\u00B7': '*',
  // spaces (NBSP and friends) and invisible ones
  '\u00A0': ' ', '\u2007': ' ', '\u2009': ' ', '\u200A': ' ', '\u202F': ' ', '\u3000': ' ',
  '\u200B': '', '\u200C': '', '\u200D': '', '\u2060': '', '\uFEFF': '', '\u00AD': '',
  // arrows and comparisons
  '\u2192': '->', '\u2190': '<-', '\u2194': '<->', '\u21D2': '=>', '\u21D0': '<=', '\u21D4': '<=>',
  '\u2191': '^', '\u2193': 'v', '\u2265': '>=', '\u2264': '<=', '\u2260': '!=', '\u2248': '~',
  '\u00D7': 'x', '\u00F7': '/', '\u2044': '/', '\u00B1': '+/-', '\u00B0': ' deg',
  // currencies and marks
  '\u20AC': 'EUR', '\u00A3': 'GBP', '\u00A5': 'JPY', '\u00A2': 'c',
  '\u00A9': '(c)', '\u00AE': '(R)', '\u2122': '(TM)', '\u00A7': 'S', '\u00B6': 'P',
  // letters NFKD leaves whole
  '\u00DF': 'ss', '\u1E9E': 'SS', '\u00E6': 'ae', '\u00C6': 'AE', '\u0153': 'oe', '\u0152': 'OE',
  '\u00F8': 'o', '\u00D8': 'O', '\u0142': 'l', '\u0141': 'L', '\u0111': 'd', '\u0110': 'D',
  '\u00F0': 'd', '\u00D0': 'D', '\u00FE': 'th', '\u00DE': 'Th', '\u0131': 'i',
};

const MAP = new Map(Object.entries(TABLE));
const MARKS = /\p{M}/gu;

function one(ch) {
  const code = ch.codePointAt(0);
  if (code >= 0x20 && code <= 0x7e) return ch;
  if (code < 0x20 || code === 0x7f) return ' '; // tabs, newlines, controls
  return MAP.get(ch) ?? '?';
}

export function toAscii(text) {
  let out = '';
  for (const ch of String(text ?? '')) {
    // The table first: NFKD would turn ≠ into = plus a combining stroke.
    if (MAP.has(ch)) out += MAP.get(ch);
    else for (const c of ch.normalize('NFKD').replace(MARKS, '')) out += one(c);
  }
  return out.replace(/\s+/g, ' ').trim();
}
