//
//  Aes67.jsx
//
//  aes67-daemon passthrough view: daemon config JSON editor, sinks and sources
//  tables (with live flags merged in from /api/status) and the remote source
//  browser with a "use for sink" helper.
//

import { useCallback, useEffect, useMemo, useState } from 'react';

import { useGlobalStatus } from '../StatusContext';
import {
  browseRemoteSources,
  deleteAes67Sink,
  deleteAes67Source,
  getAes67Config,
  getAes67Sinks,
  getAes67Sources,
  putAes67Sink,
  putAes67Source,
  setAes67Config,
} from '../api';
import { arr, asList, fmtInt, num } from '../format';
import Card from '../components/Card';
import Modal from '../components/Modal';
import StatusPill from '../components/StatusPill';
import ConfirmButton from '../components/ConfirmButton';

const DELAY_CHOICES = [192, 384, 576, 768, 960];
const BROWSE_KINDS = ['all', 'mdns', 'sap'];

function Field({ label, children, wide = false }) {
  return (
    <label className={`field${wide ? ' field-wide' : ''}`}>
      <span className="field-label">{label}</span>
      {children}
    </label>
  );
}

function parseMap(text) {
  const parts = String(text || '')
    .split(/[,\s]+/)
    .filter((p) => p !== '');
  const out = [];
  for (const p of parts) {
    const n = Number(p);
    if (!Number.isInteger(n) || n < 0 || n > 63) return { error: `invalid channel "${p}" (0-63)`, map: [] };
    out.push(n);
  }
  return { error: null, map: out };
}

/* ------------------------------------------------------------------ */
/* Sink dialog                                                        */
/* ------------------------------------------------------------------ */

function SinkDialog({ open, sink, onClose, onSubmit, busy, error }) {
  const isNew = !sink || sink.__new;
  const [form, setForm] = useState(() => ({
    id: sink?.id ?? 0,
    name: sink?.name ?? '',
    io: sink?.io ?? 'Audio Device',
    delay: num(sink?.delay, 576),
    use_sdp: !!sink?.use_sdp,
    source: sink?.source ?? '',
    sdp: sink?.sdp ?? '',
    ignore_refclk_gmid: sink?.ignore_refclk_gmid !== false,
    mapText: arr(sink?.map).join(', '),
  }));
  const [localError, setLocalError] = useState(null);
  const set = (patch) => setForm((f) => ({ ...f, ...patch }));

  function submit() {
    const { error: mapError, map } = parseMap(form.mapText);
    if (mapError) {
      setLocalError(mapError);
      return;
    }
    const id = Number(form.id);
    if (!Number.isInteger(id) || id < 0 || id > 63) {
      setLocalError('sink id must be 0-63');
      return;
    }
    setLocalError(null);
    onSubmit(id, {
      name: form.name,
      io: form.io,
      delay: num(form.delay, 576),
      use_sdp: !!form.use_sdp,
      source: form.use_sdp ? '' : form.source,
      sdp: form.use_sdp ? form.sdp : '',
      ignore_refclk_gmid: !!form.ignore_refclk_gmid,
      map,
    });
  }

  return (
    <Modal
      open={open}
      wide
      title={isNew ? `Add sink ${form.id}` : `Edit sink ${form.id}`}
      onClose={onClose}
      footer={
        <>
          <span className="muted small">sink {form.id}</span>
          <button type="button" className="btn" onClick={onClose} disabled={busy}>
            Cancel
          </button>
          <button type="button" className="btn btn-primary" onClick={submit} disabled={busy}>
            {busy ? 'saving…' : 'Save sink'}
          </button>
        </>
      }
    >
      {localError || error ? <div className="inline-error">{localError || error}</div> : null}
      <div className="form-grid">
        <Field label="id">
          <input
            type="number"
            className="input number"
            min="0"
            max="63"
            value={form.id}
            disabled={!isNew}
            onChange={(e) => set({ id: e.target.value })}
          />
        </Field>
        <Field label="name">
          <input type="text" className="input" value={form.name} onChange={(e) => set({ name: e.target.value })} />
        </Field>
        <Field label="io">
          <input type="text" className="input" value={form.io} onChange={(e) => set({ io: e.target.value })} />
        </Field>
        <Field label="delay (samples)">
          <select className="input select" value={form.delay} onChange={(e) => set({ delay: e.target.value })}>
            {DELAY_CHOICES.map((d) => (
              <option key={d} value={d}>{d} — {((d / 48000) * 1000).toFixed(1)} ms @48k</option>
            ))}
          </select>
        </Field>
        <Field label="map (AES67 -> ALSA channels)" wide>
          <input
            type="text"
            className="input mono"
            value={form.mapText}
            placeholder="0, 1"
            onChange={(e) => set({ mapText: e.target.value })}
          />
        </Field>
        <label className="checkbox">
          <input type="checkbox" checked={form.use_sdp} onChange={(e) => set({ use_sdp: e.target.checked })} />
          wire from remote SDP
        </label>
        <label className="checkbox">
          <input
            type="checkbox"
            checked={form.ignore_refclk_gmid}
            onChange={(e) => set({ ignore_refclk_gmid: e.target.checked })}
          />
          ignore refclk gmid
        </label>
        <Field label="source URL" wide>
          <input
            type="text"
            className="input mono"
            value={form.source}
            disabled={form.use_sdp}
            placeholder="http://…/api/source/sdp/0"
            onChange={(e) => set({ source: e.target.value })}
          />
        </Field>
        <Field label="remote SDP" wide>
          <textarea
            className="input mono textarea"
            rows={8}
            value={form.sdp}
            disabled={!form.use_sdp}
            onChange={(e) => set({ sdp: e.target.value })}
          />
        </Field>
      </div>
    </Modal>
  );
}
/* ------------------------------------------------------------------ */
/* Source dialog                                                      */
/* ------------------------------------------------------------------ */

function SourceDialog({ open, source, onClose, onSubmit, busy, error }) {
  const isNew = !source || source.__new;
  const [form, setForm] = useState(() => ({
    id: source?.id ?? 0,
    enabled: source?.enabled !== false,
    name: source?.name ?? '',
    io: source?.io ?? 'Audio Device',
    max_samples_per_packet: num(source?.max_samples_per_packet, 48),
    codec: source?.codec ?? 'L24',
    ttl: num(source?.ttl, 15),
    payload_type: num(source?.payload_type, 98),
    dscp: num(source?.dscp, 34),
    refclk_ptp_traceable: !!source?.refclk_ptp_traceable,
    mapText: arr(source?.map).join(', '),
  }));
  const [localError, setLocalError] = useState(null);
  const set = (patch) => setForm((f) => ({ ...f, ...patch }));

  function submit() {
    const { error: mapError, map } = parseMap(form.mapText);
    if (mapError) {
      setLocalError(mapError);
      return;
    }
    const id = Number(form.id);
    if (!Number.isInteger(id) || id < 0 || id > 63) {
      setLocalError('source id must be 0-63');
      return;
    }
    setLocalError(null);
    onSubmit(id, {
      enabled: !!form.enabled,
      name: form.name,
      io: form.io,
      max_samples_per_packet: num(form.max_samples_per_packet, 48),
      codec: form.codec,
      ttl: num(form.ttl, 15),
      payload_type: num(form.payload_type, 98),
      dscp: num(form.dscp, 34),
      refclk_ptp_traceable: !!form.refclk_ptp_traceable,
      map,
    });
  }

  return (
    <Modal
      open={open}
      wide
      title={isNew ? `Add source ${form.id}` : `Edit source ${form.id}`}
      onClose={onClose}
      footer={
        <>
          <span className="muted small">source {form.id}</span>
          <button type="button" className="btn" onClick={onClose} disabled={busy}>
            Cancel
          </button>
          <button type="button" className="btn btn-primary" onClick={submit} disabled={busy}>
            {busy ? 'saving…' : 'Save source'}
          </button>
        </>
      }
    >
      {localError || error ? <div className="inline-error">{localError || error}</div> : null}
      <div className="form-grid">
        <Field label="id">
          <input
            type="number"
            className="input number"
            min="0"
            max="63"
            value={form.id}
            disabled={!isNew}
            onChange={(e) => set({ id: e.target.value })}
          />
        </Field>
        <Field label="name">
          <input type="text" className="input" value={form.name} onChange={(e) => set({ name: e.target.value })} />
        </Field>
        <Field label="io">
          <input type="text" className="input" value={form.io} onChange={(e) => set({ io: e.target.value })} />
        </Field>
        <Field label="codec">
          <select className="input select" value={form.codec} onChange={(e) => set({ codec: e.target.value })}>
            {['L24', 'L16', 'L32'].map((c) => (
              <option key={c} value={c}>{c}</option>
            ))}
          </select>
        </Field>
        <Field label="payload type">
          <input
            type="number"
            className="input number"
            min="0"
            max="127"
            value={form.payload_type}
            onChange={(e) => set({ payload_type: e.target.value })}
          />
        </Field>
        <Field label="dscp">
          <input
            type="number"
            className="input number"
            min="0"
            max="63"
            value={form.dscp}
            onChange={(e) => set({ dscp: e.target.value })}
          />
        </Field>
        <Field label="ttl">
          <input
            type="number"
            className="input number"
            min="1"
            max="255"
            value={form.ttl}
            onChange={(e) => set({ ttl: e.target.value })}
          />
        </Field>
        <Field label="samples/packet">
          <input
            type="number"
            className="input number"
            min="1"
            max="192"
            value={form.max_samples_per_packet}
            onChange={(e) => set({ max_samples_per_packet: e.target.value })}
          />
        </Field>
        <Field label="map (ALSA -> AES67 channels)" wide>
          <input
            type="text"
            className="input mono"
            value={form.mapText}
            placeholder="0, 1"
            onChange={(e) => set({ mapText: e.target.value })}
          />
        </Field>
        <label className="checkbox">
          <input type="checkbox" checked={form.enabled} onChange={(e) => set({ enabled: e.target.checked })} />
          enabled
        </label>
        <label className="checkbox">
          <input
            type="checkbox"
            checked={form.refclk_ptp_traceable}
            onChange={(e) => set({ refclk_ptp_traceable: e.target.checked })}
          />
          refclk ptp traceable
        </label>
      </div>
    </Modal>
  );
}
/* ------------------------------------------------------------------ */
/* Remote source browser                                              */
/* ------------------------------------------------------------------ */

function rtpOf(sdp) {
  const text = String(sdp || '');
  const addr = text.match(/c=IN IP4 ([0-9.]+)/);
  const port = text.match(/m=audio ([0-9]+)/);
  return { address: addr ? addr[1] : '—', port: port ? port[1] : '—' };
}

function RemoteBrowser({ onUseForSink }) {
  const [kind, setKind] = useState('all');
  const [list, setList] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);

  const load = useCallback(async (k) => {
    setLoading(true);
    setError(null);
    try {
      const data = await browseRemoteSources(k);
      setList(asList(data, 'remote_sources'));
    } catch (err) {
      setList([]);
      setError(err.message);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    load(kind);
  }, [kind, load]);

  return (
    <Card
      title="Remote sources"
      subtitle="mDNS / SAP discovery from the aes67-daemon"
      bodyClass="flush"
      actions={
        <>
          <div className="segmented">
            {BROWSE_KINDS.map((k) => (
              <button
                key={k}
                type="button"
                className={`seg${kind === k ? ' seg-active' : ''}`}
                onClick={() => setKind(k)}
              >
                {k}
              </button>
            ))}
          </div>
          <button type="button" className="btn btn-small" onClick={() => load(kind)} disabled={loading}>
            refresh
          </button>
        </>
      }
    >
      {error ? (
        <div className="inline-error">
          browse failed: {error}
          <button type="button" className="btn btn-small" onClick={() => load(kind)}>retry</button>
        </div>
      ) : null}
      {loading ? (
        <div className="empty">Browsing…</div>
      ) : list.length === 0 ? (
        <div className="empty">No remote sources discovered for “{kind}”.</div>
      ) : (
        <table className="table table-dense">
          <thead>
            <tr>
              <th>Discovery</th>
              <th>Name</th>
              <th>Announce address</th>
              <th>Domain</th>
              <th>RTP</th>
              <th>Seen</th>
              <th>Period</th>
              <th className="col-actions" />
            </tr>
          </thead>
          <tbody>
            {list.map((src, i) => {
              const rtp = rtpOf(src.sdp);
              return (
                <tr key={src.id ?? `${src.source}-${i}`}>
                  <td><StatusPill state={src.source} tone="info" /></td>
                  <td className="truncate" title={src.name}>{src.name || '—'}</td>
                  <td className="mono truncate" title={src.address}>{src.address || '—'}</td>
                  <td className="mono truncate" title={src.domain}>{src.domain || '—'}</td>
                  <td className="mono">{rtp.address}:{rtp.port}</td>
                  <td className="mono">{src.last_seen === undefined ? '—' : `${fmtInt(src.last_seen)} s`}</td>
                  <td className="mono">{src.announce_period === undefined ? '—' : fmtInt(src.announce_period)}</td>
                  <td className="col-actions">
                    <button
                      type="button"
                      className="btn btn-small"
                      onClick={() => onUseForSink(src)}
                      title="pre-fill the sink dialog with this source SDP"
                    >
                      use for sink
                    </button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </Card>
  );
}
/* ------------------------------------------------------------------ */
/* Page                                                               */
/* ------------------------------------------------------------------ */

export default function Aes67() {
  const { status } = useGlobalStatus();

  const [config, setConfig] = useState(null);
  const [configText, setConfigText] = useState('');
  const [configError, setConfigError] = useState(null);
  const [configNotice, setConfigNotice] = useState(null);
  const [configBusy, setConfigBusy] = useState(false);
  const [configOpen, setConfigOpen] = useState(false);

  const [sinks, setSinks] = useState([]);
  const [sources, setSources] = useState([]);
  const [listError, setListError] = useState(null);
  const [loading, setLoading] = useState(true);

  const [actionError, setActionError] = useState(null);
  const [busyId, setBusyId] = useState(null);

  const [sinkDialog, setSinkDialog] = useState(null);
  const [sinkDialogError, setSinkDialogError] = useState(null);
  const [sourceDialog, setSourceDialog] = useState(null);
  const [sourceDialogError, setSourceDialogError] = useState(null);

  const aes67 = status?.aes67 || {};
  const liveSinks = arr(aes67.sinks);
  const liveSources = arr(aes67.sources);
  const lines = arr(status?.lines);

  const loadConfig = useCallback(async () => {
    try {
      const data = await getAes67Config();
      setConfig(data || {});
      setConfigText(JSON.stringify(data || {}, null, 2));
      setConfigError(null);
    } catch (err) {
      setConfigError(err.message);
    }
  }, []);

  const loadLists = useCallback(async () => {
    setLoading(true);
    const problems = [];
    try {
      const data = await getAes67Sinks();
      setSinks(asList(data, 'sinks'));
    } catch (err) {
      setSinks([]);
      problems.push(`sinks: ${err.message}`);
    }
    try {
      const data = await getAes67Sources();
      setSources(asList(data, 'sources'));
    } catch (err) {
      setSources([]);
      problems.push(`sources: ${err.message}`);
    }
    setListError(problems.length ? problems.join(' · ') : null);
    setLoading(false);
  }, []);

  useEffect(() => {
    loadConfig();
    loadLists();
  }, [loadConfig, loadLists]);

  /** Live daemon flags + which SIP lines consume this sink. */
  function sinkLive(id) {
    const live = liveSinks.find((s) => Number(s?.id) === Number(id)) || {};
    const usedBy = lines.filter((l) => Number(l?.aes67?.sink_id) === Number(id));
    const flags = [];
    if (live.seq_error) flags.push('seq');
    if (live.ssrc_error) flags.push('ssrc');
    if (live.pt_error) flags.push('pt');
    return { live, usedBy, flags };
  }

  function sourceLive(id) {
    const live = liveSources.find((s) => Number(s?.id) === Number(id)) || {};
    const usedBy = lines.filter((l) => Number(l?.aes67?.source_id) === Number(id));
    return { live, usedBy };
  }

  function freeId(list) {
    const used = new Set(list.map((x) => Number(x?.id)));
    for (let i = 0; i < 64; i += 1) if (!used.has(i)) return i;
    return 0;
  }

  async function guard(id, fn, okMessage) {
    setBusyId(id);
    setActionError(null);
    try {
      await fn();
      if (okMessage) setActionError(null);
    } catch (err) {
      setActionError(err.message);
    } finally {
      setBusyId(null);
    }
  }

  const saveDaemonConfig = async () => {
    setConfigBusy(true);
    setConfigError(null);
    setConfigNotice(null);
    let parsed;
    try {
      parsed = JSON.parse(configText);
    } catch (err) {
      setConfigError(`JSON parse error: ${err.message}`);
      setConfigBusy(false);
      return;
    }
    try {
      const res = await setAes67Config(parsed);
      const effective = res && typeof res === 'object' && !Array.isArray(res) ? res : parsed;
      setConfig(effective);
      setConfigText(JSON.stringify(effective, null, 2));
      setConfigNotice('daemon config accepted');
      loadLists();
    } catch (err) {
      setConfigError(err.message);
    } finally {
      setConfigBusy(false);
    }
  };

  const submitSink = async (id, payload) => {
    setSinkDialogError(null);
    setBusyId(`sink-${id}`);
    try {
      await putAes67Sink(id, payload);
      setSinkDialog(null);
      await loadLists();
    } catch (err) {
      setSinkDialogError(err.message);
    } finally {
      setBusyId(null);
    }
  };

  const submitSource = async (id, payload) => {
    setSourceDialogError(null);
    setBusyId(`source-${id}`);
    try {
      await putAes67Source(id, payload);
      setSourceDialog(null);
      await loadLists();
    } catch (err) {
      setSourceDialogError(err.message);
    } finally {
      setBusyId(null);
    }
  };

  const removeSink = (id) =>
    guard(id, async () => {
      await deleteAes67Sink(id);
      await loadLists();
    });

  const removeSource = (id) =>
    guard(id, async () => {
      await deleteAes67Source(id);
      await loadLists();
    });

  const toggleSourceEnabled = (src) =>
    guard(`src-${src.id}`, async () => {
      await putAes67Source(src.id, { ...src, enabled: !src.enabled });
      await loadLists();
    });

  const useForSink = useMemo(
    () => (src) =>
      setSinkDialog({
        __new: true,
        id: freeId(sinks),
        name: src?.name || '',
        io: 'Audio Device',
        delay: 576,
        use_sdp: true,
        source: '',
        sdp: src?.sdp || '',
        ignore_refclk_gmid: true,
        map: [0, 1],
      }),
    [sinks],
  );

  const daemonConnected = aes67.connected;
  return (
    <div className="page">
      <div className="page-head">
        <h1>AES67</h1>
        <span className="muted small">
          daemon {aes67.address || '—'} · {sinks.length} sink{sinks.length === 1 ? '' : 's'} ·{' '}
          {sources.length} source{sources.length === 1 ? '' : 's'}
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
      {listError ? <div className="strip strip-error">{listError}</div> : null}

      <Card
        title="aes67-daemon"
        subtitle={aes67.address || 'not configured'}
        actions={
          <>
            <StatusPill
              state={daemonConnected}
              tone={daemonConnected ? 'ok' : 'err'}
            >
              {daemonConnected ? 'connected' : 'disconnected'}
            </StatusPill>
            <button type="button" className="btn btn-small" onClick={() => setConfigOpen((v) => !v)}>
              {configOpen ? 'hide config' : 'config JSON'}
            </button>
          </>
        }
      >
        <div className="kv-row">
          <div className="kv">
            <span className="kv-label">PTP</span>
            <span className="kv-value"><StatusPill state={aes67.ptp?.status} /></span>
          </div>
          <div className="kv">
            <span className="kv-label">GMID</span>
            <span className="kv-value mono">{aes67.ptp?.gmid || '—'}</span>
          </div>
          <div className="kv">
            <span className="kv-label">Jitter</span>
            <span className="kv-value mono">
              {aes67.ptp?.jitter === undefined || aes67.ptp?.jitter === null ? '—' : fmtInt(aes67.ptp.jitter)}
            </span>
          </div>
          <div className="kv">
            <span className="kv-label">Live sinks receiving</span>
            <span className="kv-value">
              {liveSinks.filter((s) => s?.receiving).length} / {liveSinks.length || 0}
            </span>
          </div>
        </div>
        {aes67.error ? <div className="inline-error">{aes67.error}</div> : null}

        {configOpen ? (
          <div className="config-editor">
            {configError ? <div className="inline-error">{configError}</div> : null}
            {configNotice ? <div className="inline-ok">{configNotice}</div> : null}
            <textarea
              className="input mono textarea config-textarea"
              rows={18}
              spellCheck={false}
              value={configText}
              onChange={(e) => setConfigText(e.target.value)}
            />
            <div className="field-row">
              <button type="button" className="btn btn-primary" onClick={saveDaemonConfig} disabled={configBusy}>
                {configBusy ? 'saving…' : 'Save daemon config'}
              </button>
              <button type="button" className="btn" onClick={loadConfig} disabled={configBusy}>
                Reload
              </button>
              <span className="muted small">
                POST /api/aes67/config{config ? ` · ${Object.keys(config).length} top level keys` : ''}
              </span>
            </div>
          </div>
        ) : null}
      </Card>
      <Card
        title="Sinks"
        subtitle="AES67 -> ALSA receive streams"
        bodyClass="flush"
        actions={
          <>
            <button type="button" className="btn btn-small" onClick={loadLists} disabled={loading}>
              reload
            </button>
            <button
              type="button"
              className="btn btn-small btn-primary"
              onClick={() =>
                setSinkDialog({
                  __new: true,
                  id: freeId(sinks),
                  name: `Sink ${freeId(sinks)}`,
                  io: 'Audio Device',
                  delay: 576,
                  use_sdp: false,
                  source: '',
                  sdp: '',
                  ignore_refclk_gmid: true,
                  map: [0, 1],
                })
              }
            >
              add sink
            </button>
          </>
        }
      >
        {loading && sinks.length === 0 ? (
          <div className="empty">Loading…</div>
        ) : sinks.length === 0 ? (
          <div className="empty">No sinks configured on the daemon.</div>
        ) : (
          <table className="table table-dense">
            <thead>
              <tr>
                <th>ID</th>
                <th>Name</th>
                <th>Map</th>
                <th>Delay</th>
                <th>SDP</th>
                <th>Source</th>
                <th>Receiving</th>
                <th>Muted</th>
                <th>Errors</th>
                <th>Lines</th>
                <th className="col-actions" />
              </tr>
            </thead>
            <tbody>
              {sinks
                .slice()
                .sort((a, b) => Number(a?.id) - Number(b?.id))
                .map((sink) => {
                  const { live, usedBy, flags } = sinkLive(sink.id);
                  const map = arr(sink.map);
                  return (
                    <tr key={sink.id}>
                      <td className="mono">{sink.id}</td>
                      <td className="truncate" title={sink.name}>{sink.name || '—'}</td>
                      <td className="mono">{map.length ? map.join(', ') : '—'}</td>
                      <td className="mono">{sink.delay === undefined ? '—' : `${fmtInt(sink.delay)} smp`}</td>
                      <td><StatusPill state={sink.use_sdp} tone={sink.use_sdp ? 'info' : 'off'} /></td>
                      <td className="mono truncate" title={sink.source || ''}>{sink.source || '—'}</td>
                      <td>
                        <StatusPill
                          state={live.receiving}
                          tone={live.receiving ? 'ok' : 'off'}
                        >
                          {live.receiving === undefined ? 'n/a' : live.receiving ? 'receiving' : 'silent'}
                        </StatusPill>
                      </td>
                      <td>
                        <StatusPill state={live.muted} tone={live.muted ? 'warn' : 'off'}>
                          {live.muted === undefined ? 'n/a' : live.muted ? 'muted' : 'open'}
                        </StatusPill>
                      </td>
                      <td>
                        {flags.length ? (
                          <span className="text-err mono">{flags.join(' ')}</span>
                        ) : (
                          <span className="muted">none</span>
                        )}
                      </td>
                      <td className="mono">
                        {usedBy.length ? usedBy.map((l) => l.id).join(', ') : '—'}
                      </td>
                      <td className="col-actions">
                        <button
                          type="button"
                          className="btn btn-small"
                          onClick={() => setSinkDialog(sink)}
                          disabled={busyId === sink.id}
                        >
                          edit
                        </button>
                        <ConfirmButton
                          className="btn btn-small btn-danger"
                          label="remove"
                          disabled={busyId === sink.id}
                          onConfirm={() => removeSink(sink.id)}
                        />
                      </td>
                    </tr>
                  );
                })}
            </tbody>
          </table>
        )}
      </Card>
      <Card
        title="Sources"
        subtitle="ALSA -> AES67 transmit streams"
        bodyClass="flush"
        actions={
          <>
            <button type="button" className="btn btn-small" onClick={loadLists} disabled={loading}>
              reload
            </button>
            <button
              type="button"
              className="btn btn-small btn-primary"
              onClick={() =>
                setSourceDialog({
                  __new: true,
                  id: freeId(sources),
                  enabled: true,
                  name: `Source ${freeId(sources)}`,
                  io: 'Audio Device',
                  codec: 'L24',
                  ttl: 15,
                  payload_type: 98,
                  dscp: 34,
                  max_samples_per_packet: 48,
                  refclk_ptp_traceable: false,
                  map: [0, 1],
                })
              }
            >
              add source
            </button>
          </>
        }
      >
        {loading && sources.length === 0 ? (
          <div className="empty">Loading…</div>
        ) : sources.length === 0 ? (
          <div className="empty">No sources configured on the daemon.</div>
        ) : (
          <table className="table table-dense">
            <thead>
              <tr>
                <th>ID</th>
                <th>Enabled</th>
                <th>Name</th>
                <th>Address</th>
                <th>PT</th>
                <th>TTL</th>
                <th>DSCP</th>
                <th>Map</th>
                <th>Lines</th>
                <th className="col-actions" />
              </tr>
            </thead>
            <tbody>
              {sources
                .slice()
                .sort((a, b) => Number(a?.id) - Number(b?.id))
                .map((src) => {
                  const { live, usedBy } = sourceLive(src.id);
                  const map = arr(src.map);
                  const enabled = src.enabled !== undefined ? src.enabled : live.enabled;
                  return (
                    <tr key={src.id}>
                      <td className="mono">{src.id}</td>
                      <td>
                        <button
                          type="button"
                          className={`btn btn-small btn-toggle${enabled ? ' is-on' : ''}`}
                          disabled={busyId === `src-${src.id}`}
                          onClick={() => toggleSourceEnabled(src)}
                          title="toggle enabled (PUT /api/aes67/sources/{id})"
                        >
                          {enabled ? 'enabled' : 'disabled'}
                        </button>
                      </td>
                      <td className="truncate" title={src.name}>{src.name || '—'}</td>
                      <td className="mono truncate" title={src.address || live.address || ''}>
                        {src.address || live.address || '—'}
                      </td>
                      <td className="mono">{fmtInt(src.payload_type ?? live.payload_type)}</td>
                      <td className="mono">{src.ttl === undefined ? '—' : fmtInt(src.ttl)}</td>
                      <td className="mono">{src.dscp === undefined ? '—' : fmtInt(src.dscp)}</td>
                      <td className="mono">{map.length ? map.join(', ') : '—'}</td>
                      <td className="mono">{usedBy.length ? usedBy.map((l) => l.id).join(', ') : '—'}</td>
                      <td className="col-actions">
                        <button
                          type="button"
                          className="btn btn-small"
                          onClick={() => setSourceDialog(src)}
                          disabled={busyId === src.id}
                        >
                          edit
                        </button>
                        <ConfirmButton
                          className="btn btn-small btn-danger"
                          label="remove"
                          disabled={busyId === src.id}
                          onConfirm={() => removeSource(src.id)}
                        />
                      </td>
                    </tr>
                  );
                })}
            </tbody>
          </table>
        )}
      </Card>

      <RemoteBrowser onUseForSink={useForSink} />

      {sinkDialog ? (
        <SinkDialog
          key={`sink-${sinkDialog.__new ? 'new' : sinkDialog.id}`}
          open
          sink={sinkDialog}
          busy={busyId !== null}
          error={sinkDialogError}
          onClose={() => {
            setSinkDialog(null);
            setSinkDialogError(null);
          }}
          onSubmit={submitSink}
        />
      ) : null}

      {sourceDialog ? (
        <SourceDialog
          key={`source-${sourceDialog.__new ? 'new' : sourceDialog.id}`}
          open
          source={sourceDialog}
          busy={busyId !== null}
          error={sourceDialogError}
          onClose={() => {
            setSourceDialog(null);
            setSourceDialogError(null);
          }}
          onSubmit={submitSource}
        />
      ) : null}


    </div>
  );
}



