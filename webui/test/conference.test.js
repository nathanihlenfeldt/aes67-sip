//
//  conference.test.js
//
//  Tests for the conference card's logic (`npm test`, node's built-in test runner).
//  They pin the mapping between the configuration document and the form's draft,
//  and the one line that tells "hung up by hand" apart from "waiting to retry".
//

import assert from 'node:assert/strict';
import test from 'node:test';

import {
  conferenceDirty,
  conferenceDraft,
  conferenceNote,
  conferencePatch,
} from '../src/conference.js';

test('the draft comes from the stored block, with a value for every field', () => {
  assert.deepEqual(conferenceDraft(undefined), {
    enabled: false,
    account: '',
    target: '',
    display_name: '',
  });
  assert.deepEqual(
    conferenceDraft({
      enabled: true,
      account: 'pbx',
      target: 'sip:conf@pbx.example.com',
      display_name: 'FreePBX',
    }),
    {
      enabled: true,
      account: 'pbx',
      target: 'sip:conf@pbx.example.com',
      display_name: 'FreePBX',
    },
  );
});

test('the patch carries the block\'s four keys, trimmed', () => {
  assert.deepEqual(
    conferencePatch({
      enabled: 1,
      account: '  pbx  ',
      target: ' sip:conf@pbx.example.com ',
      display_name: ' FreePBX ',
    }),
    {
      enabled: true,
      account: 'pbx',
      target: 'sip:conf@pbx.example.com',
      display_name: 'FreePBX',
    },
  );
});

test('a draft equal to what is stored is not dirty, and a change is', () => {
  const stored = {
    enabled: true,
    account: 'pbx',
    target: 'sip:conf@pbx.example.com',
    display_name: 'FreePBX',
  };
  const draft = conferenceDraft(stored);
  assert.equal(conferenceDirty(draft, stored), false);
  // Whitespace is not a change: it is trimmed on the way out.
  assert.equal(conferenceDirty({ ...draft, target: ` ${draft.target} ` }, stored), false);
  assert.equal(conferenceDirty({ ...draft, target: '' }, stored), true);
  assert.equal(conferenceDirty({ ...draft, enabled: false }, stored), true);
  assert.equal(conferenceDirty({ ...draft, account: 'other' }, stored), true);
});

test('the note says which of the idle-looking states the leg is in', () => {
  assert.equal(conferenceNote({ enabled: false }), 'no off-site leg is configured');
  assert.equal(
    conferenceNote({ enabled: true, state: 'idle', redial_paused: true }),
    'hung up by hand - press dial to raise the call again',
  );
  assert.equal(conferenceNote({ enabled: true, state: 'in_call' }), 'in call');
  assert.equal(conferenceNote({ enabled: true, state: 'dialing' }), 'raising the call');
  // A reason from the PBX wins over the generic word for the state.
  assert.equal(
    conferenceNote({ enabled: true, state: 'error', detail: '404 Not Found' }),
    '404 Not Found',
  );
  assert.equal(conferenceNote({ enabled: true, state: 'idle' }), '');
});
