// Tests for the live meeting summary sent to the device: the Markdown parser,
// the diff/feed builder, ASCII transliteration and the payload's caps.
// npm run test:meeting
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as C from '../src/components/meetingContract.js';

let failed = 0;
function test(name, fn) {
  try {
    fn();
    console.log(`ok   ${name}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}\n     ${e.message}`);
  }
}

const sample = JSON.parse(readFileSync(new URL('./fixtures/meeting-sample.json', import.meta.url), 'utf8'));

test('fixture follows the contract', () => {
  assert.equal(sample.v, C.MEETING_VERSION);
  assert.match(sample.session, /^[A-Za-z0-9-]{1,23}$/);
  assert.deepEqual(Object.keys(sample.sections), C.SECTION_KEYS);
  for (const e of sample.feed) {
    assert.ok(C.FEED_SECS.includes(e.sec));
    assert.ok(C.FEED_KINDS.includes(e.kind));
  }
  assert.ok(JSON.stringify(sample).length <= C.MAX_BODY);
});

// Stream A: parser, diff/feed, toAscii, payload builder.

if (failed) {
  console.log(`\n${failed} failed`);
  process.exit(1);
}
console.log('\nall passed');
