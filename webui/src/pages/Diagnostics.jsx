//
//  Diagnostics.jsx
//
//  Log tail with severity colouring + auto-refresh, self-test results, the full
//  channel level meter grid and the audio overrun/underrun counters.
//

import { useCallback, useEffect, useState } from 'react';

import { useGlobalStatus } from '../StatusContext';
import { getLog, selfTest } from '../api';
import { arr, fmtInt, logSeverity } from '../format';
import Card from '../components/Card';
import StatusPill from '../components/StatusPill';
import LevelMeter from '../components/LevelMeter';

const LOG_LINES = 200;
const LOG_REFRESH_MS = 2000;

export default function Diagnostics() {
  const { status, error: statusError } = useGlobalStatus();

  const [logLines, setLogLines] = useState([]);
  const [logError, setLogError] = useState(null);
  const [logBusy, setLogBusy] = useState(false);
  const [autoRefresh, setAutoRefresh] = useState(true);
  const [updatedAt, setUpdatedAt] = useState(null);

  const [checks, setChecks] = useState(null);
  const [testBusy, setTestBusy] = useState(false);
  const [testError, setTestError] = useState(null);

  const audio = status?.audio || {};
  const levels = arr(audio.levels_dbfs);

  const fetchLog = useCallback(async () => {
    setLogBusy(true);
    try {
      const data = await getLog(LOG_LINES);
      const lines = Array.isArray(data) ? data : arr(data?.lines);
      setLogLines(lines);
      setLogError(null);
      setUpdatedAt(new Date());
    } catch (err) {
      setLogError(err.message);
    } finally {
      setLogBusy(false);
    }
  }, []);

  useEffect(() => {
    fetchLog();
  }, [fetchLog]);

  useEffect(() => {
    if (!autoRefresh) return undefined;
    const id = setInterval(() => {
      if (document.visibilityState !== 'hidden') fetchLog();
    }, LOG_REFRESH_MS);
    return () => clearInterval(id);
  }, [autoRefresh, fetchLog]);

  async function runSelfTest() {
    setTestBusy(true);
    setTestError(null);
    try {
      const res = await selfTest();
      setChecks(arr(res?.checks));
    } catch (err) {
      setChecks(null);
      setTestError(err.message);
    } finally {
      setTestBusy(false);
    }
  }

  const counts = logLines.reduce(
    (acc, line) => {
      const sev = logSeverity(line);
      acc[sev] = (acc[sev] || 0) + 1;
      return acc;
    },
    { error: 0, warn: 0, info: 0, debug: 0, trace: 0 },
  );

  return (
    <div className="page">
      <div className="page-head">
        <h1>Diagnostics</h1>
        <span className="muted small">
          log tail · self-test · channel meters · audio counters
        </span>
      </div>

      {statusError ? <div className="strip strip-error">status: {statusError}</div> : null}

      <div className="grid grid-3">
        <Card title="Audio counters" subtitle={audio.device || 'no device'}>
          <div className="kv">
            <span className="kv-label">State</span>
            <span className="kv-value"><StatusPill state={audio.state} /></span>
          </div>
          <div className="kv">
            <span className="kv-label">RX overruns</span>
            <span className={`kv-value mono${fmtInt(audio.rx_overruns, '0') !== '0' ? ' text-err' : ''}`}>
              {fmtInt(audio.rx_overruns, '0')}
            </span>
          </div>
          <div className="kv">
            <span className="kv-label">TX underruns</span>
            <span className={`kv-value mono${fmtInt(audio.tx_underruns, '0') !== '0' ? ' text-err' : ''}`}>
              {fmtInt(audio.tx_underruns, '0')}
            </span>
          </div>
          <div className="kv">
            <span className="kv-label">Period</span>
            <span className="kv-value mono">{fmtInt(audio.period_frames)} frames</span>
          </div>
        </Card>

        <Card title="Log summary" subtitle={`last ${logLines.length} lines`}>
          {['error', 'warn', 'info', 'debug', 'trace'].map((sev) => (
            <div className="kv" key={sev}>
              <span className="kv-label">
                <span className={`log-sev log-sev-${sev}`}>{sev}</span>
              </span>
              <span className="kv-value mono">{counts[sev] || 0}</span>
            </div>
          ))}
        </Card>

        <Card title="Clock / daemon" subtitle={status?.aes67?.address || 'not configured'}>
          <div className="kv">
            <span className="kv-label">PTP</span>
            <span className="kv-value"><StatusPill state={status?.aes67?.ptp?.status} /></span>
          </div>
          <div className="kv">
            <span className="kv-label">GMID</span>
            <span className="kv-value mono truncate">{status?.aes67?.ptp?.gmid || '—'}</span>
          </div>
          <div className="kv">
            <span className="kv-label">Daemon</span>
            <span className="kv-value">
              <StatusPill
                state={status?.aes67?.connected}
                tone={status?.aes67?.connected ? 'ok' : 'err'}
              >
                {status?.aes67?.connected ? 'connected' : 'disconnected'}
              </StatusPill>
            </span>
          </div>
          <div className="kv">
            <span className="kv-label">Uptime</span>
            <span className="kv-value mono">{fmtInt(status?.uptime_sec, '0')} s</span>
          </div>
        </Card>
      </div>
      <Card title="Channel levels" subtitle={`audio backend · ${levels.length} channels`} bodyClass="flush">
        {levels.length === 0 ? (
          <div className="empty">No channel levels reported by the audio backend.</div>
        ) : (
          <div className="meter-grid">
            {levels.map((lvl, i) => (
              <div className="meter-cell" key={`ch-${i}`}>
                <span className="meter-cell-label mono">ch {i}</span>
                <LevelMeter level={lvl} label={undefined} />
              </div>
            ))}
          </div>
        )}
      </Card>

      <Card
        title="Self-test"
        subtitle="POST /api/system/self-test"
        actions={
          <button type="button" className="btn btn-small btn-primary" onClick={runSelfTest} disabled={testBusy}>
            {testBusy ? 'running…' : 'run self-test'}
          </button>
        }
        bodyClass="flush"
      >
        {testError ? <div className="inline-error">self-test failed: {testError}</div> : null}
        {!checks && !testError ? <div className="empty">No self-test results yet.</div> : null}
        {checks ? (
          <table className="table table-dense">
            <thead>
              <tr>
                <th>Check</th>
                <th>Result</th>
                <th>Detail</th>
              </tr>
            </thead>
            <tbody>
              {checks.map((c, i) => (
                <tr key={`${c?.name || 'check'}-${i}`}>
                  <td>{c?.name || `check ${i}`}</td>
                  <td>
                    <StatusPill state={c?.ok ? 'ok' : 'failed'} tone={c?.ok ? 'ok' : 'err'}>
                      {c?.ok ? 'pass' : 'fail'}
                    </StatusPill>
                  </td>
                  <td className="mono truncate" title={c?.detail || ''}>{c?.detail || '—'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        ) : null}
      </Card>

      <Card
        title="Log"
        subtitle={updatedAt ? `updated ${updatedAt.toLocaleTimeString()}` : 'GET /api/log'}
        bodyClass="flush"
        actions={
          <>
            <label className="checkbox">
              <input
                type="checkbox"
                checked={autoRefresh}
                onChange={(e) => setAutoRefresh(e.target.checked)}
              />
              auto-refresh ({LOG_REFRESH_MS / 1000}s)
            </label>
            <button type="button" className="btn btn-small" onClick={fetchLog} disabled={logBusy}>
              {logBusy ? 'loading…' : 'refresh'}
            </button>
          </>
        }
      >
        {logError ? (
          <div className="inline-error">
            log fetch failed: {logError}
            <button type="button" className="btn btn-small" onClick={fetchLog}>retry</button>
          </div>
        ) : null}
        {logLines.length === 0 && !logError ? (
          <div className="empty">Log is empty.</div>
        ) : (
          <pre className="log">
            {logLines.map((line, i) => (
              <div className={`log-line log-sev-${logSeverity(line)}`} key={`log-${i}`}>
                {String(line)}
              </div>
            ))}
          </pre>
        )}
      </Card>

    </div>
  );
}
