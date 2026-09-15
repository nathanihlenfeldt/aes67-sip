//
//  Lines.jsx
//
//  Per-line operator page: live state + expandable configuration editor
//  (enable / mute / gain / AES67 channel mapping / SIP dial behaviour),
//  call controls and a DTMF pad.
//

import { useState } from 'react';

import { useGlobalStatus } from '../StatusContext';
import { getLineConfig, setLineConfig, lineCall } from '../api';
import { arr, fmtDb, fmtDuration, num } from '../format';
import Card from '../components/Card';
import StatusPill from '../components/StatusPill';
import LevelMeter from '../components/LevelMeter';

const CALL_MODES = ['auto_answer', 'manual', 'dial_out', 'ptt'];
const DTMF_KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#'];

function parseChannels(text) {
  const parts = String(text || '')
    .split(/[,\s]+/)
    .filter((p) => p !== '');
  const out = [];
  for (const p of parts) {
    const n = Number(p);
    if (!Number.isInteger(n) || n < 0) return { error: `invalid channel "${p}"`, channels: [] };
    out.push(n);
  }
  if (out.length === 0) return { error: 'at least one channel is required', channels: [] };
  return { error: null, channels: out };
}

function parseSinkSourcePair(sinkId, sourceId) {
  const sink = Number(sinkId);
  const source = Number(sourceId);
  return {
    sink_id: Number.isInteger(sink) && sink >= 0 ? sink : 0,
    source_id: Number.isInteger(source) && source >= 0 ? source : 0,
  };
}

function Section({ title, children }) {
  return (
    <div className="subcard">
      <h4 className="subcard-title">{title}</h4>
      {children}
    </div>
  );
}

function DtmfPad({ disabled, onDigit }) {
  return (
    <div className="dtmf">
      {DTMF_KEYS.map((k) => (
        <button
          key={k}
          type="button"
          className="btn dtmf-key"
          disabled={disabled}
          onClick={() => onDigit(k)}
        >
          {k}
        </button>
      ))}
    </div>
  );
}

export {
  CALL_MODES,
  DTMF_KEYS,
  parseChannels,
  parseSinkSourcePair,
  Section,
  DtmfPad,
};

function LineEditor({ line, config, onSave, onCall, onReload, busy }) {
  const cfgSip = config?.sip || {};
  const cfgAes = config?.aes67 || {};
  const liveAes = line.aes67 || {};
  const liveCall = line.call || {};

  const [form, setForm] = useState(() => {
    const channels = arr(cfgAes.channels).length ? arr(cfgAes.channels) : arr(liveAes.channels);
    return {
      enabled: config?.enabled ?? line.enabled ?? true,
      mute: config?.mute ?? line.mute ?? false,
      ptt: config?.ptt ?? line.ptt ?? false,
      gain_db: num(config?.gain_db, num(line.gain_db, 0)),
      channelsText: channels.join(', '),
      sink_id: cfgAes.sink_id ?? liveAes.sink_id ?? 0,
      source_id: cfgAes.source_id ?? liveAes.source_id ?? 0,
      dial_target: cfgSip.dial_target ?? liveCall.remote_uri ?? '',
      call_mode: CALL_MODES.includes(cfgSip.call_mode) ? cfgSip.call_mode : 'manual',
      ptt_threshold_dbfs: num(cfgSip.ptt_threshold_dbfs, -30),
      ptt_hangup_ms: num(cfgSip.ptt_hangup_ms, 400),
      hold: false,
    };
  });
  const [dialTarget, setDialTarget] = useState(form.dial_target);
  const [formError, setFormError] = useState(null);

  const set = (patch) => setForm((f) => ({ ...f, ...patch }));

  function handleSave() {
    const parsed = parseChannels(form.channelsText);
    if (parsed.error) {
      setFormError(parsed.error);
      return;
    }
    const gain = num(form.gain_db, 0);
    if (gain < -24 || gain > 12) {
      setFormError('gain must be between -24 and +12 dB');
      return;
    }
    setFormError(null);
    const pair = parseSinkSourcePair(form.sink_id, form.source_id);
    onSave({
      enabled: !!form.enabled,
      mute: !!form.mute,
      ptt: !!form.ptt,
      gain_db: gain,
      aes67: {
        channels: parsed.channels,
        sink_id: pair.sink_id,
        source_id: pair.source_id,
      },
      sip: {
        dial_target: form.dial_target,
        call_mode: form.call_mode,
        ptt_threshold_dbfs: num(form.ptt_threshold_dbfs, -30),
        ptt_hangup_ms: num(form.ptt_hangup_ms, 400),
      },
    });
  }

  const inCall = line.state === 'in_call';

  return (
    <div className="editor">
      <Section title="Line behaviour">
        <div className="field-row">
          <label className="checkbox">
            <input
              type="checkbox"
              checked={!!form.enabled}
              onChange={(e) => set({ enabled: e.target.checked })}
            />
            enabled
          </label>
          <label className="checkbox">
            <input type="checkbox" checked={!!form.mute} onChange={(e) => set({ mute: e.target.checked })} />
            mute
          </label>
          <label className="checkbox">
            <input type="checkbox" checked={!!form.ptt} onChange={(e) => set({ ptt: e.target.checked })} />
            push-to-talk
          </label>
        </div>

        <div className="field-row">
          <label className="field">
            <span className="field-label">gain (dB, -24 … +12)</span>
            <input
              type="range"
              min="-24"
              max="12"
              step="0.5"
              value={form.gain_db}
              onChange={(e) => set({ gain_db: Number(e.target.value) })}
            />
          </label>
          <input
            type="number"
            className="input number"
            min="-24"
            max="12"
            step="0.5"
            value={form.gain_db}
            onChange={(e) => set({ gain_db: e.target.value === '' ? '' : Number(e.target.value) })}
          />
          <span className="mono muted">{fmtDb(form.gain_db, '0.0')} dB</span>
        </div>
      </Section>

      <Section title="AES67 channel mapping">
        <div className="field-row">
          <label className="field">
            <span className="field-label">channels (comma separated)</span>
            <input
              type="text"
              className="input mono"
              value={form.channelsText}
              placeholder="0, 1"
              onChange={(e) => set({ channelsText: e.target.value })}
            />
          </label>
          <label className="field">
            <span className="field-label">sink id</span>
            <input
              type="number"
              className="input number"
              min="0"
              max="63"
              value={form.sink_id}
              onChange={(e) => set({ sink_id: e.target.value })}
            />
          </label>
          <label className="field">
            <span className="field-label">source id</span>
            <input
              type="number"
              className="input number"
              min="0"
              max="63"
              value={form.source_id}
              onChange={(e) => set({ source_id: e.target.value })}
            />
          </label>
        </div>
      </Section>
      <Section title="SIP behaviour">
        <div className="field-row">
          <label className="field">
            <span className="field-label">dial target</span>
            <input
              type="text"
              className="input mono"
              value={form.dial_target}
              placeholder="sip:1001@pbx.example.com"
              onChange={(e) => {
                set({ dial_target: e.target.value });
                setDialTarget(e.target.value);
              }}
            />
          </label>
          <label className="field">
            <span className="field-label">call mode</span>
            <select
              className="input select"
              value={form.call_mode}
              onChange={(e) => set({ call_mode: e.target.value })}
            >
              {CALL_MODES.map((m) => (
                <option key={m} value={m}>{m}</option>
              ))}
            </select>
          </label>
          <label className="field">
            <span className="field-label">PTT threshold (dBFS)</span>
            <input
              type="number"
              className="input number"
              step="0.5"
              min="-60"
              max="0"
              value={form.ptt_threshold_dbfs}
              onChange={(e) => set({ ptt_threshold_dbfs: e.target.value })}
            />
          </label>
          <label className="field">
            <span className="field-label">PTT hangup (ms)</span>
            <input
              type="number"
              className="input number"
              step="50"
              min="0"
              value={form.ptt_hangup_ms}
              onChange={(e) => set({ ptt_hangup_ms: e.target.value })}
            />
          </label>
        </div>
        <div className="hint">
          {form.call_mode === 'ptt'
            ? 'PTT: the line is keyed from AES67 input level crossing the threshold; it hangs up after the silence timer.'
            : form.call_mode === 'auto_answer'
              ? 'auto_answer: inbound INVITEs are answered without operator action.'
              : form.call_mode === 'dial_out'
                ? 'dial_out: the gateway places an outbound call to the dial target when the line goes active.'
                : 'manual: operator drives dial / answer / hangup from this page.'}
        </div>
      </Section>

      <Section title="Call control">
        <div className="field-row">
          <input
            type="text"
            className="input mono grow"
            value={dialTarget}
            placeholder="sip:1001@pbx.example.com"
            onChange={(e) => setDialTarget(e.target.value)}
          />
          <button
            type="button"
            className="btn btn-primary"
            disabled={busy}
            onClick={() => onCall('dial', dialTarget ? { target: dialTarget } : {})}
          >
            Dial
          </button>
          <button type="button" className="btn" disabled={busy} onClick={() => onCall('answer')}>
            Answer
          </button>
          <button
            type="button"
            className="btn btn-danger"
            disabled={busy}
            onClick={() => onCall('hangup')}
          >
            Hangup
          </button>
          <button
            type="button"
            className={`btn${form.hold ? ' is-armed' : ''}`}
            disabled={busy}
            onClick={() => {
              const next = !form.hold;
              set({ hold: next });
              onCall('hold', { hold: next });
            }}
          >
            {form.hold ? 'Resume' : 'Hold'}
          </button>
        </div>
        <div className="field-row align-center">
          <span className="field-label">DTMF</span>
          <DtmfPad disabled={busy || !inCall} onDigit={(d) => onCall('dtmf', { digits: d })} />
          <span className="muted small">requires an active call</span>
        </div>
      </Section>

      <div className="editor-foot">
        {formError ? <span className="inline-error">{formError}</span> : null}
        <button type="button" className="btn btn-small btn-ghost" onClick={onReload} disabled={busy}>
          reload config
        </button>
        <button type="button" className="btn btn-primary" onClick={handleSave} disabled={busy}>
          {busy ? 'saving…' : 'Save line config'}
        </button>
      </div>

    </div>
  );
}

export default function Lines() {
  const { status, error: statusError } = useGlobalStatus();
  const [openId, setOpenId] = useState(null);
  const [configs, setConfigs] = useState({});
  const [errors, setErrors] = useState({});
  const [actionError, setActionError] = useState(null);
  const [notice, setNotice] = useState(null);
  const [busyId, setBusyId] = useState(null);

  const lines = arr(status?.lines);

  async function loadConfig(id) {
    setConfigs((c) => ({ ...c, [id]: { __loading: true } }));
    try {
      const cfg = await getLineConfig(id);
      setConfigs((c) => ({ ...c, [id]: cfg || {} }));
      setErrors((e) => ({ ...e, [id]: null }));
    } catch (err) {
      setErrors((e) => ({ ...e, [id]: err.message }));
      setConfigs((c) => {
        const next = { ...c };
        delete next[id];
        return next;
      });
    }
  }

  function toggle(id) {
    const next = openId === id ? null : id;
    setOpenId(next);
    if (next !== null) loadConfig(id);
  }

  async function withBusy(id, fn, okMessage) {
    setBusyId(id);
    setActionError(null);
    setNotice(null);
    try {
      await fn();
      if (okMessage) setNotice(`Line ${id}: ${okMessage}`);
    } catch (err) {
      setActionError(`Line ${id}: ${err.message}`);
    } finally {
      setBusyId(null);
    }
  }

  const saveConfig = (id, payload) =>
    withBusy(
      id,
      async () => {
        await setLineConfig(id, payload);
        const cfg = await getLineConfig(id);
        setConfigs((c) => ({ ...c, [id]: cfg || {} }));
      },
      'configuration saved',
    );

  const doCall = (id, action, extra) =>
    withBusy(id, () => lineCall(id, action, extra), `${action} sent`);

  return (
    <div className="page">
      <div className="page-head">
        <h1>Lines</h1>
        <span className="muted small">
          {lines.length} configured · expand a row to edit its AES67 mapping and SIP behaviour
        </span>
      </div>

      {actionError ? (
        <div className="strip strip-error">
          {actionError}
          <button type="button" className="btn btn-small btn-ghost" onClick={() => setActionError(null)}>
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
      {statusError && !status ? <div className="empty">No status from the gateway.</div> : null}
      <Card bodyClass="flush">
        {lines.length === 0 ? (
          <div className="empty">No lines reported by the gateway.</div>
        ) : (
          <table className="table table-dense">
            <thead>
              <tr>
                <th className="col-toggle" />
                <th>#</th>
                <th>Name</th>
                <th>State</th>
                <th>Remote URI</th>
                <th>Duration</th>
                <th className="col-meter">RX</th>
                <th className="col-meter">TX</th>
                <th>Gain</th>
                <th>Mute</th>
                <th>PTT</th>
              </tr>
            </thead>
            <tbody>
              {lines.map((line) => {
                const call = line.call || {};
                const levels = line.levels || {};
                const isOpen = openId === line.id;
                const cfg = configs[line.id];
                const busy = busyId === line.id;
                return [
                  <tr
                    key={`row-${line.id}`}
                    className={`${isOpen ? 'row-open' : ''} ${line.state === 'in_call' ? 'row-active' : ''}`}
                    onClick={() => toggle(line.id)}
                  >
                    <td className="col-toggle">
                      <span className={`caret${isOpen ? ' caret-open' : ''}`} aria-hidden="true">▸</span>
                    </td>
                    <td className="mono">{line.id}</td>
                    <td className="truncate" title={line.name}>{line.name || `Line ${line.id}`}</td>
                    <td><StatusPill state={line.state} /></td>
                    <td className="mono truncate" title={call.remote_uri || call.last_error || ''}>
                      {call.remote_uri || (call.last_error ? <span className="text-err">{call.last_error}</span> : '—')}
                    </td>
                    <td className="mono">{call.duration_sec ? fmtDuration(call.duration_sec) : '--:--'}</td>
                    <td className="col-meter"><LevelMeter level={levels.rx_dbfs} compact /></td>
                    <td className="col-meter"><LevelMeter level={levels.tx_dbfs} compact /></td>
                    <td className="mono">{fmtDb(line.gain_db, '0.0')}</td>
                    <td><StatusPill state={line.mute} tone={line.mute ? 'warn' : 'off'}>
                      {line.mute ? 'muted' : 'open'}
                    </StatusPill></td>
                    <td><StatusPill state={line.ptt} tone={line.ptt ? 'warn' : 'off'}>
                      {line.ptt ? 'active' : 'idle'}
                    </StatusPill></td>
                  </tr>,
                  isOpen ? (
                    <tr key={`edit-${line.id}`} className="row-editor">
                      <td colSpan={11}>
                        {errors[line.id] ? (
                          <div className="inline-error">
                            failed to load line config: {errors[line.id]}
                            <button
                              type="button"
                              className="btn btn-small"
                              onClick={(e) => {
                                e.stopPropagation();
                                loadConfig(line.id);
                              }}
                            >
                              retry
                            </button>
                          </div>
                        ) : !cfg || cfg.__loading ? (
                          <div className="empty">Loading configuration…</div>
                        ) : (
                          <LineEditor
                            key={`${line.id}-${JSON.stringify(cfg)}`}
                            line={line}
                            config={cfg}
                            busy={busy}
                            onSave={(payload) => saveConfig(line.id, payload)}
                            onCall={(action, extra) => doCall(line.id, action, extra)}
                            onReload={() => loadConfig(line.id)}
                          />
                        )}
                      </td>
                    </tr>
                  ) : null,
                ];
              })}
            </tbody>
          </table>
        )}
      </Card>

    </div>
  );
}
