//
//  commissioning.test.js
//
//  Tests for the commissioning page's logic (`npm test`, node's built-in test
//  runner - no extra dependency).  They pin the two places the page can silently
//  write the wrong thing: the mapping between the configuration document and the
//  drafts it edits, and the locating of the appliance's refusals.
//

import assert from 'node:assert/strict';
import test from 'node:test';

import {
  addEndpointDraft,
  addLineDraft,
  buildPatch,
  canAddMember,
  draftFromConfig,
  isLocated,
  locateRefusal,
  matchLiveMember,
  nextId,
  parseChannelList,
  parseSlot,
  runningLine,
  slotOptions,
  UNBOUND,
} from '../src/commissioning.js';

/** The reference site's shape, as the appliance stores it. */
const CONFIG = {
  endpoints: [
    {
      id: 'pack-1',
      name: 'Camera 1',
      talk_channels: [0, 1],
      listen_channels: [0, 1],
      aes67: { stream_name: 'Camera 1', codec: 'L24', remote_sdp: 'v=0' },
    },
    {
      id: 'pack-2',
      name: 'Camera 2',
      talk_channels: [2, 3],
      listen_channels: [2, 3],
    },
  ],
  party_lines: [
    {
      id: 'cameras',
      name: 'Cameras',
      claims_conference: true,
      members: [
        { endpoint: 'pack-1', talk_channel: 0, listen_channel: 0, contribution_db: -3, mute: false },
        { endpoint: 'pack-2', talk_channel: 0, listen_channel: -1, contribution_db: 0, mute: true },
      ],
    },
  ],
};

test('a channel list is parsed as typed, and refused when it is not a list', () => {
  assert.deepEqual(parseChannelList('2, 3'), { error: null, channels: [2, 3] });
  assert.deepEqual(parseChannelList(' 4 5 '), { error: null, channels: [4, 5] });
  // No channels at all is a legal declaration: an endpoint may only listen.
  assert.deepEqual(parseChannelList(''), { error: null, channels: [] });
  assert.match(parseChannelList('2, x').error, /"x" is not a device channel number/);
  assert.match(parseChannelList('-1').error, /"-1" is not a device channel number/);
});

test('an empty slot is unbound, which is what the configuration stores', () => {
  assert.equal(parseSlot(''), UNBOUND);
  assert.equal(parseSlot(null), UNBOUND);
  assert.equal(parseSlot('-1'), UNBOUND);
  assert.equal(parseSlot('0'), 0);
  assert.equal(parseSlot('2'), 2);
});

test('the slots offered are the endpoint own channels, with their device channel', () => {
  assert.deepEqual(slotOptions(CONFIG.endpoints[0], 'talk'), [
    { value: '', label: 'unbound' },
    { value: '0', label: 'slot 0 → device channel 0' },
    { value: '1', label: 'slot 1 → device channel 1' },
  ]);
  // The device channel beside the slot is what a person matches to the pack's
  // buttons: the binding is the slot index, the routing is the device channel.
  assert.deepEqual(slotOptions(CONFIG.endpoints[1], 'listen'), [
    { value: '', label: 'unbound' },
    { value: '0', label: 'slot 0 → device channel 2' },
    { value: '1', label: 'slot 1 → device channel 3' },
  ]);
  // A direction the endpoint does not carry offers only "unbound".
  assert.deepEqual(slotOptions({ talk_channels: [7] }, 'listen'), [
    { value: '', label: 'unbound' },
  ]);
  assert.deepEqual(slotOptions(undefined, 'talk'), [{ value: '', label: 'unbound' }]);
});


test('the draft round-trips the configuration it was loaded from', () => {
  const patch = buildPatch(draftFromConfig(CONFIG));
  assert.equal(patch.error, null);
  assert.deepEqual(patch.patch.endpoints, [
    // The stream description travels with the endpoint: dropping it would
    // unsubscribe the pack's talk stream.
    {
      id: 'pack-1',
      name: 'Camera 1',
      talk_channels: [0, 1],
      listen_channels: [0, 1],
      aes67: { stream_name: 'Camera 1', codec: 'L24', remote_sdp: 'v=0' },
    },
    // An endpoint with no stream description of its own keeps none, so the
    // appliance's defaults apply.
    { id: 'pack-2', name: 'Camera 2', talk_channels: [2, 3], listen_channels: [2, 3] },
  ]);
  assert.deepEqual(patch.patch.party_lines, CONFIG.party_lines);
});

test('renaming an endpoint keeps a defaulted stream name in step', () => {
  const draft = draftFromConfig(CONFIG);
  draft.endpoints[0].name = 'Camera 1 (stage)';
  assert.equal(
    buildPatch(draft).patch.endpoints[0].aes67.stream_name,
    'Camera 1 (stage)',
  );

  // ...but a stream name that was chosen on purpose is left alone.
  const chosen = draftFromConfig(CONFIG);
  chosen.endpoints[0].aes67 = { ...chosen.endpoints[0].aes67, stream_name: 'FOH comms' };
  chosen.endpoints[0].name = 'Camera 1 (stage)';
  assert.equal(buildPatch(chosen).patch.endpoints[0].aes67.stream_name, 'FOH comms');
});

test('a draft that cannot be written as configuration is refused before posting', () => {
  const badChannels = draftFromConfig(CONFIG);
  badChannels.endpoints[1].listenText = '2, three';
  const channels = buildPatch(badChannels);
  assert.equal(channels.patch, null);
  assert.match(channels.error, /endpoint "pack-2" listen channels: "three"/);

  const noEndpointId = draftFromConfig(CONFIG);
  noEndpointId.endpoints[0].id = '  ';
  assert.match(buildPatch(noEndpointId).error, /endpoint 1 needs an id/);

  const noLineId = draftFromConfig(CONFIG);
  noLineId.lines[0].id = '';
  assert.match(buildPatch(noLineId).error, /party line 1 needs an id/);

  const noMember = draftFromConfig(CONFIG);
  noMember.lines[0].members[1].endpoint = '';
  const member = buildPatch(noMember);
  assert.equal(member.patch, null);
  assert.match(member.error, /party line "cameras" member 2 has no endpoint chosen/);
  assert.deepEqual(member.locate, { lineIds: ['cameras'], memberIndex: 1 });
});

test('an unbound direction is written as the configuration sentinel', () => {
  const draft = draftFromConfig(CONFIG);
  draft.lines[0].members[0].listenChannel = '';
  draft.lines[0].members[1].listenChannel = '1';
  const patch = buildPatch(draft);
  assert.equal(patch.patch.party_lines[0].members[0].listen_channel, UNBOUND);
  assert.equal(patch.patch.party_lines[0].members[1].listen_channel, 1);
  // ...and the level and mute flag it did not touch are preserved.
  assert.equal(patch.patch.party_lines[0].members[0].contribution_db, -3);
  assert.equal(patch.patch.party_lines[0].members[1].mute, true);
});


test('a refusal is located against the entries it names', () => {
  // The appliance's message for a member bound outside its endpoint's shape.
  const member = locateRefusal(
    "party line 'cameras' member 2 binds listen_channel 3 of endpoint 'pack-2' "
      + '(Camera 2), which declares 1 listen channel(s)',
  );
  assert.deepEqual(member.lineIds, ['cameras']);
  assert.equal(member.memberIndex, 1);
  assert.deepEqual(member.endpointIds, ['pack-2']);
  assert.equal(isLocated(member), true);

  // Two claimants of one device channel are both named.
  const claimed = locateRefusal(
    "device channel 0 is claimed twice: endpoint 'pack-1' (Camera 1) "
      + "talk_channels[0] and endpoint 'pack-2' (Camera 2) talk_channels[0]",
  );
  assert.deepEqual(claimed.endpointIds, ['pack-1', 'pack-2']);
  assert.equal(claimed.memberIndex, null);

  // A duplicate name names the name, because that is what the person typed.
  const named = locateRefusal("two party lines are called 'Cameras'");
  assert.deepEqual(named.lineNames, ['Cameras']);
  assert.equal(isLocated(named), true);

  const lineIds = locateRefusal("two party lines share the id 'pl1'");
  assert.deepEqual(lineIds.lineIds, ['pl1']);
  const endpointIds = locateRefusal("two endpoints share the id 'pack-1'");
  assert.deepEqual(endpointIds.endpointIds, ['pack-1']);
  const unknownEndpoint = locateRefusal(
    "party line 'cameras' member 2 binds endpoint 'ghost', which no endpoint declares",
  );
  assert.deepEqual(unknownEndpoint.lineIds, ['cameras']);
  assert.deepEqual(unknownEndpoint.endpointIds, ['ghost']);

  // A refusal about the device as a whole points at no single entry, so the page
  // shows it once rather than pretending it belongs to something.
  const capacity = locateRefusal(
    'the declared endpoint shapes need 32 device channels (32 capture, 32 playback) '
      + 'but audio.channels opens 16 - raise audio.channels or declare fewer channels',
  );
  assert.equal(isLocated(capacity), false);
});

test('the empty state starts from nothing and can add the first entries', () => {
  const empty = draftFromConfig({ endpoints: [], party_lines: [] });
  assert.deepEqual(empty, { endpoints: [], lines: [] });
  assert.equal(buildPatch(empty).error, null);
  assert.deepEqual(buildPatch(empty).patch, { endpoints: [], party_lines: [] });
  // ...and a configuration the appliance omitted entirely is the same as empty.
  assert.deepEqual(draftFromConfig(undefined), { endpoints: [], lines: [] });

  // A member needs an endpoint to bind to, so the page says so instead of
  // offering a member row that cannot be filled in.
  assert.equal(canAddMember(empty), false);
  const withEndpoint = addEndpointDraft(empty);
  assert.equal(canAddMember(withEndpoint), true);
  assert.deepEqual(withEndpoint.endpoints[0], {
    id: 'pack-1',
    name: 'pack-1',
    talkText: '0, 1',
    listenText: '0, 1',
  });

  const withLine = addLineDraft(withEndpoint);
  assert.deepEqual(withLine.lines[0], {
    id: 'pl1',
    name: 'pl1',
    claimsConference: false,
    members: [],
  });
  // The added entries are already postable, so a line can be saved first and its
  // members filled in afterwards.
  assert.equal(buildPatch(withLine).error, null);
  assert.deepEqual(buildPatch(withLine).patch.party_lines[0].members, []);

  // Ids never collide with what is already declared.
  assert.equal(nextId('pl', ['pl1', 'pl2', 'pl4']), 'pl3');
  assert.equal(nextId('pack-', []), 'pack-1');
});

test('a patch is built from a draft as it was typed, not from a round trip', () => {
  // The page's own direction: what a person types becomes the configuration
  // document, with no help from draftFromConfig - so the two directions cannot
  // agree on the same mistake.
  const draft = {
    endpoints: [{ id: 'pack-9', name: 'Floor', talkText: '4', listenText: '5' }],
    lines: [
      {
        id: 'foh',
        name: 'FOH',
        claimsConference: true,
        members: [
          {
            endpoint: 'pack-9',
            talkChannel: '0',
            listenChannel: '',
            contributionDb: '-2.5',
            mute: true,
          },
        ],
      },
    ],
  };
  const patch = buildPatch(draft);
  assert.equal(patch.error, null);
  assert.deepEqual(patch.patch, {
    endpoints: [{ id: 'pack-9', name: 'Floor', talk_channels: [4], listen_channels: [5] }],
    party_lines: [
      {
        id: 'foh',
        name: 'FOH',
        claims_conference: true,
        members: [
          {
            endpoint: 'pack-9',
            talk_channel: 0,
            listen_channel: -1,
            contribution_db: -2.5,
            mute: true,
          },
        ],
      },
    ],
  });
  // A new endpoint has no stream description of its own: none is written, so the
  // appliance's defaults apply rather than an empty one.
  assert.equal('aes67' in patch.patch.endpoints[0], false);
});

test('duplicate ids are the appliance\'s rule, so the draft is posted as it stands', () => {
  // The module deliberately does not re-implement this: the appliance refuses it
  // (src/matrix/intercom_matrix.cpp) and locateRefusal pins its message to the
  // entries, so one rule has one authority.
  const endpoints = draftFromConfig(CONFIG);
  endpoints.endpoints[1].id = 'pack-1';
  assert.equal(buildPatch(endpoints).error, null);

  const lines = draftFromConfig(CONFIG);
  lines.lines.push({ ...lines.lines[0] });
  assert.equal(buildPatch(lines).error, null);
});

test('live state is matched on the binding, not the row position', () => {
  const running = {
    id: 'cameras',
    members: [
      { endpoint: 'pack-1', talk_channel: 0, listen_channel: 0, arriving: true, level_dbfs: -9 },
      { endpoint: 'pack-2', talk_channel: 0, listen_channel: -1, arriving: false, level_dbfs: null },
    ],
  };
  assert.equal(runningLine({ party_lines: [running] }, 'cameras'), running);
  assert.equal(runningLine({ party_lines: [running] }, 'ghost'), null);
  assert.equal(runningLine({}, 'cameras'), null);

  const draft = draftFromConfig(CONFIG);
  assert.equal(matchLiveMember(running, draft.lines[0].members[0]).level_dbfs, -9);
  assert.equal(matchLiveMember(running, draft.lines[0].members[1]).arriving, false);
  // A member whose binding changed has no live state of its own: it must not
  // borrow the level of the row it used to be.
  assert.equal(matchLiveMember(running, { endpoint: 'pack-1', talkChannel: '1' }), null);
  assert.equal(matchLiveMember(null, draft.lines[0].members[0]), null);
});
