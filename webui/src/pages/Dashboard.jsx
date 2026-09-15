//
//  Dashboard.jsx
//
//  Operator at-a-glance view: gateway identity, audio backend, PTP, aes67-daemon
//  connectivity, SIP account registration and the per-line call/level table.
//

import { useState } from 'react';
import { Link } from 'react-router-dom';

import { useGlobalStatus } from '../StatusContext';
import { setLineConfig, lineCall } from '../api';
import { arr, fmtDb, fmtInt, fmtSampleRate, fmtUptime, fmtDuration } from '../format';
import Card from '../components/Card';
import StatusPill from '../components/StatusPill';
import LevelMeter from '../components/LevelMeter';

function KV({ label, children, mono = false }) {
  return (
    <div className="kv">
      <span className="kv-label">{label}</span>
      <span className={mono ? 'kv-value mono' : 'kv-value'}>{children}</span>
    </div>
  );
}

export default function Dashboard() {
  const { status, error } = useGlobalStatus();
  const [actionError, setActionError] = useState(null);
  const [busyLine, setBusyLine] = useState(null);

  const audio = status?.audio || {};
  const aes67 = status?.aes67 || {};
  const ptp = aes67.ptp || {};
  const sip = status?.sip || {};
  const lines = arr(status?.lines);
  const accounts = arr(sip.accounts);

  async function run(lineId, fn) {
    setBusyLine(lineId);
    setActionError(null);
    try {
      await fn();
    } catch (err) {
      setActionError(`Line ${lineId}: ${err.message}`);
    } finally {
      setBusyLine(null);
    }
  }

  const toggleMute = (line) => run(line.id, () => setLineConfig(line.id, { mute: !line.mute }));
  const hangup = (line) => run(line.id, () => lineCall(line.id, 'hangup'));

  return (
    <div className="page">
      <div className="page-head">
        <h1>Dashboard</h1>
        <span className="muted small">
          uptime {fmtUptime(status?.uptime_sec)} · {lines.length} line{lines.length === 1 ? '' : 's'}
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
      {!status && error ? (
        <div className="empty">No status available — the gateway is not responding.</div>
      ) : null}

      <div className="grid grid-3">
        <Card title="Gateway" subtitle="service identity">
          <KV label="Version">{status?.version || '—'}</KV>
          <KV label="Uptime" mono>{fmtUptime(status?.uptime_sec)}</KV>
          <KV label="Audio backend">{audio.backend || 'n/a'}</KV>
          <KV label="SIP engine">
            {sip.engine || 'n/a'}{' '}
            <StatusPill state={sip.enabled} tone={sip.enabled ? 'ok' : 'off'}>
              {sip.enabled === false ? 'disabled' : sip.enabled ? 'enabled' : 'n/a'}
            </StatusPill>
          </KV>
          <KV label="Lines">{lines.length}</KV>
        </Card>

        <Card title="Audio backend" subtitle={audio.device || 'no device'}>
          <KV label="Device" mono>{audio.device || 'n/a'}</KV>
          <KV label="State"><StatusPill state={audio.state} /></KV>
          <KV label="Sample rate" mono>{fmtSampleRate(audio.sample_rate)}</KV>
          <KV label="Channels">{fmtInt(audio.channels)}</KV>
          <KV label="Period">
            {audio.period_frames ? `${fmtInt(audio.period_frames)} frames` : 'n/a'}
          </KV>
          <KV label="RX overruns">
            <span className={fmtInt(audio.rx_overruns, '0') !== '0' ? 'text-err' : ''}>
              {fmtInt(audio.rx_overruns, '0')}
            </span>
          </KV>
          <KV label="TX underruns">
            <span className={fmtInt(audio.tx_underruns, '0') !== '0' ? 'text-err' : ''}>
              {fmtInt(audio.tx_underruns, '0')}
            </span>
          </KV>
          {audio.error ? (
            <KV label="Error">
              <span className="text-err">{audio.error}</span>
            </KV>
          ) : null}
        </Card>

        <Card title="PTP" subtitle={ptp.gmid || 'no grandmaster'}>
          <KV label="Status"><StatusPill state={ptp.status} /></KV>
          <KV label="Grandmaster" mono>{ptp.gmid || 'n/a'}</KV>
          <KV label="Jitter" mono>
            {ptp.jitter === undefined || ptp.jitter === null ? 'n/a' : fmtInt(ptp.jitter)}
          </KV>
          <KV label="Daemon">
            <StatusPill state={aes67.connected} tone={aes67.connected ? 'ok' : 'err'}>
              {aes67.connected ? 'connected' : 'disconnected'}
            </StatusPill>
          </KV>
          <KV label="Daemon address" mono>{aes67.address || 'n/a'}</KV>
          {aes67.error ? <div className="inline-error">{aes67.error}</div> : null}
        </Card>
      </div>
      <div className="grid grid-2">
        <Card
          title="SIP accounts"
          subtitle={
            sip.enabled === false
              ? 'SIP engine disabled'
              : `${sip.transport || 'udp'} :${fmtInt(sip.local_port)}`
          }
          bodyClass="flush"
        >
          {accounts.length === 0 ? (
            <div className="empty">No SIP accounts configured.</div>
          ) : (
            <table className="table">
              <thead>
                <tr>
                  <th>ID</th>
                  <th>State</th>
                  <th>URI</th>
                  <th>Error</th>
                </tr>
              </thead>
              <tbody>
                {accounts.map((a, i) => (
                  <tr key={a.id ?? i}>
                    <td className="mono">{a.id ?? i}</td>
                    <td><StatusPill state={a.state} /></td>
                    <td className="mono truncate" title={a.uri}>{a.uri || 'n/a'}</td>
                    <td className="text-err">{a.error || ''}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </Card>

        <Card title="PTP / clock" subtitle="IEEE 1588 slave state">
          <div className="big-status">
            <StatusPill state={ptp.status} />
            <span className="big-value mono">{ptp.gmid || '—'}</span>
          </div>
          <div className="hint">
            The audio backend clock is derived from the aes67-daemon PTP slave. <em>locked</em> is
            required for Ravenna playback and for SIP media to stay sample accurate.
          </div>
        </Card>
      </div>
      <Card title="Lines" subtitle="live call state and levels" bodyClass="flush">
        {lines.length === 0 ? (
          <div className="empty">No lines reported by the gateway.</div>
        ) : (
          <table className="table table-dense">
            <thead>
              <tr>
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
                <th className="col-actions">Actions</th>
              </tr>
            </thead>
            <tbody>
              {lines.map((line) => {
                const call = line.call || {};
                const levels = line.levels || {};
                return (
                  <tr key={line.id} className={line.state === 'in_call' ? 'row-active' : ''}>
                    <td className="mono">{line.id}</td>
                    <td className="truncate" title={line.name}>
                      <Link to="/lines">{line.name || `Line ${line.id}`}</Link>
                    </td>
                    <td>
                      <StatusPill state={line.state} title={`state_code ${line.state_code ?? 'n/a'}`} />
                    </td>
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
                    <td className="col-actions">
                      <button
                        type="button"
                        className="btn btn-small"
                        disabled={busyLine === line.id}
                        onClick={() => toggleMute(line)}
                      >
                        {line.mute ? 'unmute' : 'mute'}
                      </button>
                      <button
                        type="button"
                        className="btn btn-small btn-danger"
                        disabled={busyLine === line.id || !call.remote_uri}
                        onClick={() => hangup(line)}
                      >
                        hangup
                      </button>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </Card>
      <Card title="Line channel mapping" subtitle="AES67 sink/source bound to each SIP line" bodyClass="flush">
        <table className="table table-dense">
          <thead>
            <tr>
              <th>Line</th>
              <th>Name</th>
              <th>Sink</th>
              <th>Source</th>
              <th>Channels</th>
              <th>Receiving</th>
            </tr>
          </thead>
          <tbody>
            {lines.length === 0 ? (
              <tr>
                <td colSpan={6} className="empty">No lines.</td>
              </tr>
            ) : (
              lines.map((line) => {
                const a = line.aes67 || {};
                const channels = arr(a.channels);
                return (
                  <tr key={line.id}>
                    <td className="mono">{line.id}</td>
                    <td>{line.name || '—'}</td>
                    <td className="mono">{a.sink_id ?? '—'}</td>
                    <td className="mono">{a.source_id ?? '—'}</td>
                    <td className="mono">{channels.length ? channels.join(', ') : '—'}</td>
                    <td>
                      <StatusPill state={a.receiving} tone={a.receiving ? 'ok' : 'off'}>
                        {a.receiving ? 'receiving' : 'silent'}
                      </StatusPill>
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </Card>



    </div>
  );
}
