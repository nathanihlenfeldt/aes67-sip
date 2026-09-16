//
//  commissioning.js
//
//  The party-line commissioning page's logic, kept out of the component so it can
//  be tested without a browser (`webui/test/commissioning.test.js`, `npm test`).
//
//  It maps between the configuration document the appliance stores and the drafts
//  the page edits, and locates a refusal from the appliance against the entry that
//  caused it.
//
//  The appliance decides whether a configuration *works* (duplicate names and ids,
//  channels claimed twice, bindings in range, the device width - see docs/api.md,
//  POST /api/config).  This module refuses only what the appliance cannot be asked:
//  a channel list that is not a list of channel numbers, a member with no endpoint,
//  and an entry with no id - the appliance accepts a nameless entry, but no binding
//  could ever refer to one.  Everything else comes back from the appliance and is
//  located with `locateRefusal`, so each rule has one authority.
//

// The explicit `.js` extension is what lets `node --test` load this module
// directly; Vite resolves either form.
import { arr, num } from './format.js';

/** A direction a member does not bind: the sentinel the configuration uses. */
export const UNBOUND = -1;

/** The word the page shows for a channel a member does not bind. */
export const UNBOUND_LABEL = 'unbound';

/** Parse the comma or space separated channel list the page shows per direction. */
export function parseChannelList(text) {
  const parts = String(text ?? '')
    .split(/[\s,]+/)
    .filter((part) => part !== '');
  const channels = [];
  for (const part of parts) {
    const value = Number(part);
    if (!Number.isInteger(value) || value < 0) {
      return { error: `"${part}" is not a device channel number`, channels: [] };
    }
    channels.push(value);
  }
  return { error: null, channels };
}

/** Device channels as the page shows them in a text field. */
export function formatChannelList(channels) {
  return arr(channels).join(', ');
}

/** A slot index, or `UNBOUND` for "not bound" (empty, null, -1). */
export function parseSlot(value) {
  if (value === '' || value === null || value === undefined) return UNBOUND;
  const n = Number(value);
  return Number.isInteger(n) && n >= 0 ? n : UNBOUND;
}

/**
 * The slots a member may bind in one direction, as `<select>` options: the
 * endpoint's own channel indices, which is what the configuration binds, with the
 * device channel each one maps onto beside it so a person can match it to the
 * pack's A and B buttons.
 */
export function slotOptions(endpoint, direction) {
  const channels = arr(
    direction === 'listen' ? endpoint?.listen_channels : endpoint?.talk_channels,
  );
  return [
    { value: '', label: UNBOUND_LABEL },
    ...channels.map((channel, index) => ({
      value: String(index),
      label: `slot ${index} → device channel ${channel}`,
    })),
  ];
}


/** Turn the configuration document into the drafts the page edits. */
export function draftFromConfig(config) {
  const endpoints = arr(config?.endpoints).map((endpoint) => ({
    id: String(endpoint?.id ?? ''),
    name: String(endpoint?.name ?? ''),
    // The name the configuration has now, so a rename can tell a stream name
    // that was defaulted from one that was chosen on purpose (see buildPatch).
    originalName: String(endpoint?.name ?? ''),
    talkText: formatChannelList(endpoint?.talk_channels),
    listenText: formatChannelList(endpoint?.listen_channels),
    // The stream description is not edited on this page, so it travels through
    // untouched - dropping it would unsubscribe the pack's talk stream.
    aes67: endpoint?.aes67,
  }));
  const lines = arr(config?.party_lines).map((line) => ({
    id: String(line?.id ?? ''),
    name: String(line?.name ?? ''),
    claimsConference: !!line?.claims_conference,
    members: arr(line?.members).map((member) => ({
      endpoint: String(member?.endpoint ?? ''),
      talkChannel:
        member?.talk_channel === UNBOUND || member?.talk_channel == null
          ? ''
          : String(member.talk_channel),
      listenChannel:
        member?.listen_channel === UNBOUND || member?.listen_channel == null
          ? ''
          : String(member.listen_channel),
      contributionDb: String(num(member?.contribution_db, 0)),
      mute: !!member?.mute,
    })),
  }));
  return { endpoints, lines };
}

/** The first free id with this prefix, so a new entry is named before it is edited. */
export function nextId(prefix, taken) {
  const used = new Set(arr(taken).map((value) => String(value)));
  for (let n = 1; ; n += 1) {
    const candidate = `${prefix}${n}`;
    if (!used.has(candidate)) return candidate;
  }
}

/** A new, empty party line: named, so the page can show it before it is filled in. */
export function addLineDraft(draft) {
  const id = nextId('pl', arr(draft?.lines).map((line) => line.id));
  return {
    ...draft,
    lines: [...arr(draft?.lines), { id, name: id, claimsConference: false, members: [] }],
  };
}

/** A new endpoint: a two channel pack, the shape the reference site runs. */
export function addEndpointDraft(draft) {
  const endpoints = arr(draft?.endpoints);
  const id = nextId('pack-', endpoints.map((endpoint) => endpoint.id));
  const index = endpoints.length;
  return {
    ...draft,
    endpoints: [
      ...endpoints,
      {
        id,
        name: id,
        talkText: `${2 * index}, ${2 * index + 1}`,
        listenText: `${2 * index}, ${2 * index + 1}`,
      },
    ],
  };
}

/** True while there is an endpoint a member could bind to. */
export function canAddMember(draft) {
  return arr(draft?.endpoints).some((endpoint) => String(endpoint?.id ?? '').trim() !== '');
}


/**
 * Build the configuration patch the page posts: the two arrays it owns, in the
 * shape `POST /api/config` expects (arrays are replaced wholesale).
 *
 * Each entry is rebuilt from the fields the page knows, so a field added to
 * `EndpointConfig` or `PartyLineConfig` later has to be carried through here too -
 * the endpoint's stream description (`aes67`) is the one that already is.
 *
 * `patch` is null when the draft cannot be written down as configuration; `locate`
 * then says which entry to show the reason against.
 */
export function buildPatch(draft) {
  const endpoints = [];
  const endpointDrafts = arr(draft?.endpoints);

  for (let index = 0; index < endpointDrafts.length; index += 1) {
    const endpoint = endpointDrafts[index] || {};
    const id = String(endpoint.id ?? '').trim();
    if (!id) {
      return {
        patch: null,
        error: `endpoint ${index + 1} needs an id: bindings and stream names refer to it`,
      };
    }
    const talk = parseChannelList(endpoint.talkText);
    if (talk.error) {
      return {
        patch: null,
        error: `endpoint "${id}" talk channels: ${talk.error}`,
        locate: { endpointIds: [id] },
      };
    }
    const listen = parseChannelList(endpoint.listenText);
    if (listen.error) {
      return {
        patch: null,
        error: `endpoint "${id}" listen channels: ${listen.error}`,
        locate: { endpointIds: [id] },
      };
    }

    const entry = {
      id,
      name: String(endpoint.name ?? '').trim() || id,
      talk_channels: talk.channels,
      listen_channels: listen.channels,
    };
    if (endpoint.aes67) {
      // Keep the stream name in step with the endpoint's name the way a fresh
      // configuration defaults it, unless it was given a name of its own.
      const aes67 = { ...endpoint.aes67 };
      const previousName = String(endpoint.originalName ?? entry.name).trim() || id;
      if (!aes67.stream_name || aes67.stream_name === previousName) {
        aes67.stream_name = entry.name;
      }
      entry.aes67 = aes67;
    }
    endpoints.push(entry);
  }

  const lines = [];
  const lineDrafts = arr(draft?.lines);

  for (let index = 0; index < lineDrafts.length; index += 1) {
    const line = lineDrafts[index] || {};
    const id = String(line.id ?? '').trim();
    if (!id) {
      return {
        patch: null,
        error: `party line ${index + 1} needs an id: its members are keyed by it`,
      };
    }
    const members = [];
    const memberDrafts = arr(line.members);
    for (let m = 0; m < memberDrafts.length; m += 1) {
      const member = memberDrafts[m] || {};
      const endpoint = String(member.endpoint ?? '').trim();
      if (!endpoint) {
        return {
          patch: null,
          error: `party line "${id}" member ${m + 1} has no endpoint chosen`,
          locate: { lineIds: [id], memberIndex: m },
        };
      }
      members.push({
        endpoint,
        talk_channel: parseSlot(member.talkChannel),
        listen_channel: parseSlot(member.listenChannel),
        contribution_db: num(member.contributionDb, 0),
        mute: !!member.mute,
      });
    }
    lines.push({
      id,
      name: String(line.name ?? '').trim() || id,
      claims_conference: !!line.claimsConference,
      members,
    });
  }

  return { patch: { endpoints, party_lines: lines }, error: null };
}

/**
 * Where the appliance's refusal points, so the page can show it against the entry
 * that caused it instead of only in the journal.  Every field is a list: a refusal
 * about two claimants names two entries.
 *
 * The wordings matched here are built in `src/matrix/intercom_matrix.cpp` (the
 * validators and `plan_from_config`); both sides' tests pin them, and anything
 * this does not recognise falls through to the page's own strip rather than being
 * attached to the wrong entry.
 */
export function locateRefusal(message) {
  const text = String(message ?? '');
  const found = { lineIds: [], lineNames: [], memberIndex: null, endpointIds: [] };
  const push = (list, value) => {
    if (value && !list.includes(value)) list.push(value);
  };

  const member = /party line '([^']*)' member (\d+)/.exec(text);
  if (member) {
    push(found.lineIds, member[1]);
    const index = Number(member[2]) - 1;
    if (Number.isInteger(index) && index >= 0) found.memberIndex = index;
  }
  const called = /party lines are called '([^']*)'/.exec(text);
  if (called) push(found.lineNames, called[1]);
  const shared = /(party lines|endpoints) share the id '([^']*)'/.exec(text);
  if (shared) {
    push(shared[1] === 'endpoints' ? found.endpointIds : found.lineIds, shared[2]);
  }
  for (const match of text.matchAll(/endpoint '([^']*)'/g)) {
    push(found.endpointIds, match[1]);
  }
  return found;
}

/** True when a refusal was located against at least one entry. */
export function isLocated(located) {
  return (
    !!located
    && (located.lineIds.length > 0
      || located.lineNames.length > 0
      || located.endpointIds.length > 0)
  );
}

/** The line the appliance is running, by id. */
export function runningLine(status, lineId) {
  return arr(status?.party_lines).find((line) => line?.id === lineId) || null;
}

/**
 * The running member a draft member corresponds to, matched on the binding rather
 * than on position: a draft that has not been saved has no live state of its own,
 * and must not borrow the level of whatever member used to be in that row.
 */
export function matchLiveMember(running, draftMember) {
  const talk = parseSlot(draftMember?.talkChannel);
  const listen = parseSlot(draftMember?.listenChannel);
  for (const member of arr(running?.members)) {
    if (member?.endpoint !== draftMember?.endpoint) continue;
    if (member?.talk_channel !== talk || member?.listen_channel !== listen) continue;
    return member;
  }
  return null;
}
