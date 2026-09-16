//
//  PartyLines.jsx
//
//  The commissioning engineer's page: every party line with its members, their
//  contribution levels and their live levels, and an editor for membership, those
//  levels and the per-channel bindings.  Saving goes through POST /api/config -
//  the path the Settings editor uses - so the appliance validates the whole
//  document, writes the file and applies it.  A refusal is shown against the entry
//  that caused it, and nothing is applied.
//
//  The logic behind the drafts, the patch and that locating lives in
//  ../commissioning.js, which `npm test` exercises without a browser.
//

import { useCallback, useEffect, useState } from 'react';

import { useGlobalStatus } from '../StatusContext';
import { getConfig, setConfig } from '../api';
import {
  addEndpointDraft,
  addLineDraft,
  buildPatch,
  canAddMember,
  draftFromConfig,
  isLocated,
  locateRefusal,
  matchLiveMember,
  parseChannelList,
  runningLine,
  slotOptions,
} from '../commissioning';
import Card from '../components/Card';
import ConfirmButton from '../components/ConfirmButton';
import LevelMeter from '../components/LevelMeter';
import StatusPill from '../components/StatusPill';

/** Strip the status api.js prefixes, so the person reads the reason itself. */
function reasonOf(message) {
  return String(message ?? '').replace(/^HTTP \d+:\s*/, '');
}

/** One member's live state: what the appliance runs for that binding, if anything. */
function LiveCell({ live }) {
  if (!live) {
    return (
      <StatusPill
        state="unsaved"
        tone="off"
        title="not in the running configuration yet - save to apply"
      >
        not applied
      </StatusPill>
    );
  }
  return (
    <span className="live-cell">
      <LevelMeter level={live.level_dbfs} compact />
      <StatusPill
        state={live.arriving}
        tone={live.arriving ? 'ok' : 'off'}
        title="whether this member's audio is reaching the appliance"
      >
        {live.arriving ? 'arriving' : 'silent'}
      </StatusPill>
    </span>
  );
}

/**
 * The options for a `<select>` that must go on showing the value it has: a binding
 * can point at an endpoint or a slot the draft no longer declares, and the
 * appliance is the one that refuses that - the page must not blank it silently.
 */
function optionsIncluding(options, value, label) {
  if (value === '' || options.some((option) => option.value === value)) return options;
  return [...options, { value, label }];
}

/**
 * The shape a member's slots are chosen from: the draft's own declaration, so a
 * binding can be picked as soon as the channels are typed, before it is saved.
 */
function shapeOf(endpoints, id) {
  const endpoint = endpoints.find((entry) => entry.id === id);
  if (!endpoint) return undefined;
  return {
    talk_channels: parseChannelList(endpoint.talkText).channels,
    listen_channels: parseChannelList(endpoint.listenText).channels,
  };
}


/** A member row: the binding, the level, the live state and the mute. */
function MemberRow({
  member,
  live,
  endpoints,
  offending,
  onChange,
  onRemove,
}) {
  const shape = shapeOf(endpoints, member.endpoint);
  const talkOptions = optionsIncluding(
    slotOptions(shape, 'talk'),
    member.talkChannel,
    `slot ${member.talkChannel} (not declared)`,
  );
  const listenOptions = optionsIncluding(
    slotOptions(shape, 'listen'),
    member.listenChannel,
    `slot ${member.listenChannel} (not declared)`,
  );
  const endpointOptions = optionsIncluding(
    endpoints.map((endpoint) => ({ value: endpoint.id, label: endpoint.name || endpoint.id })),
    member.endpoint,
    `${member.endpoint} (not declared)`,
  );
  return (
    <tr className={offending ? 'row-offending' : ''}>
      <td>
        <select
          className="input select"
          value={member.endpoint}
          onChange={(e) => onChange({ endpoint: e.target.value })}
          title="the endpoint this member binds to the line"
        >
          {endpointOptions.map((option) => (
            <option key={option.value} value={option.value}>
              {option.label}
            </option>
          ))}
        </select>
      </td>
      <td>
        <select
          className="input select"
          value={member.talkChannel}
          onChange={(e) => onChange({ talkChannel: e.target.value })}
          title="which of the endpoint's talk channels this member talks on"
        >
          {talkOptions.map((option) => (
            <option key={option.value} value={option.value}>
              {option.label}
            </option>
          ))}
        </select>
      </td>
      <td>
        <select
          className="input select"
          value={member.listenChannel}
          onChange={(e) => onChange({ listenChannel: e.target.value })}
          title="which of the endpoint's listen channels this member hears the line on"
        >
          {listenOptions.map((option) => (
            <option key={option.value} value={option.value}>
              {option.label}
            </option>
          ))}
        </select>
      </td>
      <td className="col-meter">
        <LiveCell live={live} />
      </td>
      <td>
        <input
          className="input number"
          type="number"
          step="0.5"
          value={member.contributionDb}
          onChange={(e) => onChange({ contributionDb: e.target.value })}
          title="how loudly this member is heard by the others, in dB"
        />
      </td>
      <td>
        <label className="checkbox" title="mute stops this member being heard, never what it hears">
          <input type="checkbox" checked={member.mute} onChange={(e) => onChange({ mute: e.target.checked })} />
        </label>
      </td>
      <td>
        <button type="button" className="btn btn-small btn-ghost" onClick={onRemove} title="remove this member">
          ✕
        </button>
      </td>
    </tr>
  );
}


/** One party line: its identity, its members and the controls that change them. */
function LineItem({
  line,
  lineIndex,
  running,
  endpoints,
  draft,
  offending,
  offendingMember,
  saveError,
  onEditLine,
  onEditMember,
  onEditLines,
}) {
  return (
    <div className={`pl-line${offending ? ' is-offending' : ''}`}>
      <div className="pl-line-head">
        <label className="field">
          <span className="field-label">id</span>
          <input
            className="input input-narrow mono"
            value={line.id}
            onChange={(e) => onEditLine(lineIndex, { id: e.target.value })}
            title="the stable key the members and the status view refer to"
          />
        </label>
        <label className="field">
          <span className="field-label">name</span>
          <input
            className="input"
            value={line.name}
            onChange={(e) => onEditLine(lineIndex, { name: e.target.value })}
          />
        </label>
        <label className="checkbox" title="whether this line's members are on the off-site conference call">
          <input
            type="checkbox"
            checked={line.claimsConference}
            onChange={(e) => onEditLine(lineIndex, { claimsConference: e.target.checked })}
          />
          conference
        </label>
        <StatusPill
          state={running ? 'running' : 'unsaved'}
          tone={running ? 'ok' : 'off'}
          title="whether the appliance is running this line"
        >
          {running ? 'running' : 'not applied'}
        </StatusPill>
        <ConfirmButton
          label="remove line"
          className="btn btn-small btn-danger"
          onConfirm={() => onEditLines(draft.lines.filter((_, i) => i !== lineIndex))}
        />
      </div>
      {offending ? <div className="inline-error">{saveError}</div> : null}
      <table className="table table-dense">
        <thead>
          <tr>
            <th>Endpoint</th>
            <th>Talk slot</th>
            <th>Listen slot</th>
            <th className="col-meter">Live</th>
            <th>Contribution (dB)</th>
            <th>Mute</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {line.members.map((member, memberIndex) => (
            <MemberRow
              key={`member-${lineIndex}-${memberIndex}`}
              member={member}
              live={matchLiveMember(running, member)}
              endpoints={endpoints}
              offending={offending && offendingMember === memberIndex}
              onChange={(patch) => onEditMember(lineIndex, memberIndex, patch)}
              onRemove={() =>
                onEditLine(lineIndex, {
                  members: line.members.filter((_, i) => i !== memberIndex),
                })}
            />
          ))}
        </tbody>
      </table>
      <div className="editor-foot">
        <button
          type="button"
          className="btn btn-small"
          disabled={!canAddMember(draft)}
          title={canAddMember(draft)
            ? 'add a member to this line'
            : 'declare an endpoint first'}
          onClick={() =>
            onEditLine(lineIndex, {
              members: [
                ...line.members,
                {
                  endpoint: endpoints[0]?.id ?? '',
                  talkChannel: '',
                  listenChannel: '',
                  contributionDb: '0',
                  mute: false,
                },
              ],
            })}
        >
          + add member
        </button>
      </div>
    </div>
  );
}

export default function PartyLines() {
  const { status } = useGlobalStatus();
  const [draft, setDraft] = useState(null);
  const [loadError, setLoadError] = useState(null);
  const [saveError, setSaveError] = useState(null);
  const [located, setLocated] = useState(null);
  const [notice, setNotice] = useState(null);
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    setLoadError(null);
    try {
      setDraft(draftFromConfig(await getConfig()));
      setDirty(false);
      setSaveError(null);
      setLocated(null);
    } catch (err) {
      setLoadError(reasonOf(err.message));
    }
  }, []);

  useEffect(() => {
    load();
  }, [load]);

  const endpoints = draft?.endpoints ?? [];
  const lines = draft?.lines ?? [];

  function edit(change) {
    setDraft((current) => change(current));
    setDirty(true);
    setNotice(null);
    // The last refusal was about the last attempt: the person is fixing it, so the
    // note and its highlight go with the edit.
    setSaveError(null);
    setLocated(null);
  }

  const editEndpoints = (next) => edit((current) => ({ ...current, endpoints: next }));
  const editLines = (next) => edit((current) => ({ ...current, lines: next }));
  const editEndpoint = (index, patch) =>
    editEndpoints(endpoints.map((endpoint, i) => (i === index ? { ...endpoint, ...patch } : endpoint)));
  const editLine = (index, patch) =>
    editLines(lines.map((line, i) => (i === index ? { ...line, ...patch } : line)));
  const editMember = (lineIndex, memberIndex, patch) =>
    editLine(lineIndex, {
      members: lines[lineIndex].members.map((member, i) =>
        (i === memberIndex ? { ...member, ...patch } : member)),
    });

  async function save() {
    const built = buildPatch(draft);
    if (built.error) {
      // Refused here rather than sent: the draft cannot be written as
      // configuration at all (a channel list that is not numbers, a missing id).
      setSaveError(built.error);
      setLocated(built.locate || null);
      return;
    }
    setBusy(true);
    setSaveError(null);
    setLocated(null);
    setNotice(null);
    try {
      await setConfig(built.patch);
      // Read back what the appliance holds now: it validated the document, wrote
      // the file and applied it, and the file is what a rebuild restores.
      setDraft(draftFromConfig(await getConfig()));
      setDirty(false);
      setNotice('saved and applied - the configuration file is what a rebuild restores');
    } catch (err) {
      const reason = reasonOf(err.message);
      setSaveError(reason);
      const where = locateRefusal(reason);
      setLocated(isLocated(where) ? where : null);
    } finally {
      setBusy(false);
    }
  }

  // Where the appliance's refusal points, so it can be shown against the entry
  // that caused it rather than only in the strip.
  const lineFlags = new Set([...(located?.lineIds ?? []), ...(located?.lineNames ?? [])]);
  const endpointFlags = new Set(located?.endpointIds ?? []);
  const offendsLine = (line) => lineFlags.has(line.id) || lineFlags.has(line.name);
  const offendsEndpoint = (endpoint) => endpointFlags.has(endpoint.id);
  // Whether any member of an endpoint is reaching the appliance right now, from
  // the status contract the page already polls.
  const endpointArriving = (id) =>
    (status?.party_lines ?? []).some((line) =>
      (line.members ?? []).some((member) => member.endpoint === id && member.arriving));
  const hasEndpointSdp = (endpoint) =>
    !!(endpoint.aes67?.remote_sdp || endpoint.aes67?.remote_source_id);

  if (loadError) {
    return (
      <div className="page">
        <div className="page-head">
          <h1>Party lines</h1>
        </div>
        <div className="strip strip-error">
          cannot read the configuration: {loadError}
          <button type="button" className="btn btn-small btn-ghost" onClick={load}>
            retry
          </button>
        </div>
      </div>
    );
  }

  if (!draft) {
    return (
      <div className="page">
        <div className="page-head">
          <h1>Party lines</h1>
        </div>
        <div className="empty">Loading configuration…</div>
      </div>
    );
  }

  const empty = lines.length === 0 && endpoints.length === 0;
  const nothingToSave = !dirty;

  return (
    <div className="page">
      <div className="page-head">
        <h1>Party lines</h1>
        <span className="muted small">
          {lines.length} line(s) · {endpoints.length} endpoint(s) ·{' '}
          {dirty ? 'unsaved changes' : 'the configuration file is the source of truth'}
        </span>
        <div className="page-actions">
          <button type="button" className="btn btn-small" onClick={load} disabled={busy}>
            reload
          </button>
          <button
            type="button"
            className="btn btn-primary btn-small"
            onClick={save}
            disabled={busy || nothingToSave}
            title={nothingToSave ? 'nothing to save' : 'validate, save and apply the configuration'}
          >
            {busy ? 'saving…' : nothingToSave ? 'saved' : 'save changes'}
          </button>
        </div>
      </div>

      {saveError ? (
        <div className="strip strip-error">
          not saved: {saveError}
          <button
            type="button"
            className="btn btn-small btn-ghost"
            onClick={() => {
              setSaveError(null);
              setLocated(null);
            }}
          >
            dismiss
          </button>
        </div>
      ) : null}
      {notice ? (
        <div className="strip strip-ok">
          {notice}
          <button type="button" className="btn btn-small btn-ghost" onClick={() => setNotice(null)}>
            dismiss
          </button>
        </div>
      ) : null}

      <Card
        title="Party lines"
        subtitle="one line per department: every member hears the others at their own contribution level, never themselves"
        actions={
          <button
            type="button"
            className="btn btn-small"
            onClick={() => edit(addLineDraft)}
            disabled={!canAddMember(draft)}
            title={canAddMember(draft)
              ? 'add a party line'
              : 'declare an endpoint first: a member binds one of its channels'}
          >
            + add line
          </button>
        }
      >
        {empty ? (
          <div className="empty">
            <p>No party lines are configured, so nothing is being mixed on the appliance.</p>
            <p className="muted small">
              A party line is a set of members: every member hears every other member and never
              themselves. Each member binds one of its endpoint&apos;s talk channels and one of its
              listen channels, so the first step is declaring the endpoints — one per pack, with how
              many talk and listen channels it carries.
            </p>
            <div className="editor-foot">
              <button type="button" className="btn btn-primary btn-small" onClick={() => edit(addEndpointDraft)}>
                + add the first endpoint
              </button>
            </div>
          </div>
        ) : lines.length === 0 ? (
          <div className="empty">
            No party lines yet — {endpoints.length} endpoint(s) declared.{' '}
            <button type="button" className="btn btn-small" onClick={() => edit(addLineDraft)}>
              + add the first line
            </button>
          </div>
        ) : (
          lines.map((line, lineIndex) => (
            <LineItem
              key={`line-${lineIndex}`}
              line={line}
              lineIndex={lineIndex}
              running={runningLine(status, line.id)}
              endpoints={endpoints}
              draft={draft}
              offending={offendsLine(line)}
              offendingMember={located?.memberIndex ?? null}
              saveError={saveError}
              onEditLine={editLine}
              onEditMember={editMember}
              onEditLines={editLines}
            />
          ))
        )}
      </Card>

      <Card
        title="Endpoints"
        subtitle="what each endpoint declares: one stream per direction, one entry per channel"
        actions={
          <button type="button" className="btn btn-small" onClick={() => edit(addEndpointDraft)}>
            + add endpoint
          </button>
        }
      >
        {endpoints.length === 0 ? (
          <div className="empty">
            No endpoints declared. An endpoint says how many talk channels it sends and how many
            listen channels it receives — the matrix never assumes a shape, so a two channel beltpack
            and a console with many channels are declared the same way.
          </div>
        ) : (
          endpoints.map((endpoint, index) => (
            <div
              key={`endpoint-${index}`}
              className={`pl-line${offendsEndpoint(endpoint) ? ' is-offending' : ''}`}
            >
              <div className="pl-line-head">
                <label className="field">
                  <span className="field-label">id</span>
                  <input
                    className="input input-narrow mono"
                    value={endpoint.id}
                    onChange={(e) => editEndpoint(index, { id: e.target.value })}
                    title="the key the party-line members bind to"
                  />
                </label>
                <label className="field">
                  <span className="field-label">name</span>
                  <input
                    className="input"
                    value={endpoint.name}
                    onChange={(e) => editEndpoint(index, { name: e.target.value })}
                    title="the name its provisioned streams are built from"
                  />
                </label>
                <label className="field">
                  <span className="field-label">talk channels</span>
                  <input
                    className="input input-wide mono"
                    value={endpoint.talkText}
                    placeholder="0, 1"
                    onChange={(e) => editEndpoint(index, { talkText: e.target.value })}
                    title="the device channels this endpoint talks on, in the order its own slots appear"
                  />
                </label>
                <label className="field">
                  <span className="field-label">listen channels</span>
                  <input
                    className="input input-wide mono"
                    value={endpoint.listenText}
                    placeholder="0, 1"
                    onChange={(e) => editEndpoint(index, { listenText: e.target.value })}
                    title="the device channels this endpoint listens on, in the order its own slots appear"
                  />
                </label>
                <StatusPill
                  state={endpointArriving(endpoint.id)}
                  tone={endpointArriving(endpoint.id) ? 'ok' : 'off'}
                  title="whether any member of this endpoint is reaching the appliance right now"
                >
                  {endpointArriving(endpoint.id) ? 'audio' : 'quiet'}
                </StatusPill>
                <ConfirmButton
                  label="remove endpoint"
                  className="btn btn-small btn-danger"
                  onConfirm={() => editEndpoints(endpoints.filter((_, i) => i !== index))}
                />
              </div>
              <p className="muted small">
                one talk stream and one listen stream carry its channels, named after{' '}
                <span className="mono">{endpoint.name || endpoint.id}</span>{' '}
                {hasEndpointSdp(endpoint)
                  ? '· its talk stream is described, so the appliance can subscribe to it'
                  : '· its talk stream has no description yet — pick or paste the endpoint SDP on the AES67 page'}
              </p>
              {offendsEndpoint(endpoint) ? <div className="inline-error">{saveError}</div> : null}
            </div>
          ))
        )}
      </Card>
    </div>
  );
}
