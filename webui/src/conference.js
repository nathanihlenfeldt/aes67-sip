//
//  conference.js
//
//  The conference card's logic, kept out of the component so it can be tested
//  without a browser (`webui/test/conference.test.js`, `npm test`).
//
//  It maps the stored `conference` block to the draft the form edits, and the
//  draft back to the patch `POST /api/conference/config` takes.
//
//  The appliance decides whether a conference *works* - a target to dial, an
//  account that exists - and refuses with the reason (see docs/api.md).  This
//  module refuses nothing of its own: one authority for what is valid, and the
//  page shows what came back.
//

/** A draft from the stored `conference` block: one form value per field. */
export function conferenceDraft(conference) {
  const block = conference || {};
  return {
    enabled: !!block.enabled,
    account: block.account || '',
    target: block.target || '',
    display_name: block.display_name || '',
  };
}

/** The patch `POST /api/conference/config` takes: the block's own four keys. */
export function conferencePatch(draft) {
  return {
    enabled: !!draft.enabled,
    account: String(draft.account ?? '').trim(),
    target: String(draft.target ?? '').trim(),
    display_name: String(draft.display_name ?? '').trim(),
  };
}

/** True when the draft says something other than what is stored. */
export function conferenceDirty(draft, stored) {
  const edited = conferencePatch(draft);
  const saved = conferencePatch(conferenceDraft(stored));
  // Every key of the patch, so a field added to the block cannot be forgotten here.
  return Object.keys(edited).some((key) => edited[key] !== saved[key]);
}

/**
 * The one line under the state pill.  A leg hung up by hand is idle with no retry
 * coming, which an operator has to be able to tell apart from a leg waiting for
 * its next attempt.
 */
export function conferenceNote(status) {
  const state = status || {};
  if (!state.enabled) return 'no off-site leg is configured';
  if (state.redial_paused) {
    return 'hung up by hand - press dial to raise the call again';
  }
  if (state.detail) return state.detail;
  if (state.state === 'in_call') return 'in call';
  if (state.state === 'dialing' || state.state === 'ringing') {
    return 'raising the call';
  }
  return '';
}
