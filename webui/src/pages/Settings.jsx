//
//  Settings.jsx
//
//  Raw JSON configuration editor for GET/POST /api/config with local validation,
//  the system restart control and the built-in self-test runner.
//

import { useCallback, useEffect, useState } from 'react';

import { getConfig, restartSystem, selfTest, setConfig } from '../api';
import { arr } from '../format';
import Card from '../components/Card';
import ConfirmButton from '../components/ConfirmButton';
import StatusPill from '../components/StatusPill';

export default function Settings() {
  const [text, setText] = useState('');
  const [loaded, setLoaded] = useState(false);
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState(null);
  const [parseError, setParseError] = useState(null);
  const [saveError, setSaveError] = useState(null);
  const [notice, setNotice] = useState(null);
  const [busy, setBusy] = useState(false);

  const [restartBusy, setRestartBusy] = useState(false);
  const [restartMessage, setRestartMessage] = useState(null);

  const [checks, setChecks] = useState(null);
  const [testBusy, setTestBusy] = useState(false);
  const [testError, setTestError] = useState(null);

  const load = useCallback(async () => {
    setLoading(true);
    setLoadError(null);
    try {
      const cfg = await getConfig();
      setText(JSON.stringify(cfg || {}, null, 2));
      setLoaded(true);
      setParseError(null);
    } catch (err) {
      setLoadError(err.message);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    load();
  }, [load]);

  function validate(value = text) {
    try {
      const parsed = JSON.parse(value);
      if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return { error: 'config must be a JSON object' };
      }
      return { error: null, value: parsed };
    } catch (err) {
      return { error: err.message };
    }
  }

  function handleValidate() {
    const { error } = validate();
    setParseError(error);
    setSaveError(null);
    setNotice(error ? null : 'JSON is valid — not saved yet');
  }

  async function handleSave() {
    const { error, value } = validate();
    if (error) {
      setParseError(error);
      setNotice(null);
      return;
    }
    setParseError(null);
    setSaveError(null);
    setBusy(true);
    try {
      const effective = await setConfig(value);
      if (effective && typeof effective === 'object' && !Array.isArray(effective)) {
        setText(JSON.stringify(effective, null, 2));
      }
      setNotice('configuration saved — restart to apply fields marked restart-required');
    } catch (err) {
      setSaveError(err.message);
    } finally {
      setBusy(false);
    }
  }

  async function handleRestart() {
    setRestartBusy(true);
    setRestartMessage(null);
    try {
      await restartSystem();
      setRestartMessage('restart requested — audio and SIP engines are reloading');
    } catch (err) {
      setRestartMessage(`restart failed: ${err.message}`);
    } finally {
      setRestartBusy(false);
    }
  }

  async function handleSelfTest() {
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

  const lines = text.split('\n').length;
  const bytes = text.length;

  return (
    <div className="page">
      <div className="page-head">
        <h1>Settings</h1>
        <span className="muted small">runtime configuration · restart · self-test</span>
      </div>

      <Card
        title="Configuration (JSON)"
        subtitle={`GET /api/config · ${lines} lines · ${bytes} bytes`}
        actions={
          <>
            <button type="button" className="btn btn-small" onClick={load} disabled={loading || busy}>
              reload
            </button>
            <button type="button" className="btn btn-small" onClick={handleValidate} disabled={busy}>
              validate
            </button>
            <button
              type="button"
              className="btn btn-small btn-primary"
              onClick={handleSave}
              disabled={busy || !loaded}
            >
              {busy ? 'saving…' : 'save'}
            </button>
          </>
        }
      >
        {loadError ? (
          <div className="inline-error">
            failed to load config: {loadError}
            <button type="button" className="btn btn-small" onClick={load}>retry</button>
          </div>
        ) : null}
        {parseError ? <div className="inline-error">JSON parse error: {parseError}</div> : null}
        {saveError ? <div className="inline-error">save failed: {saveError}</div> : null}
        {notice ? <div className="inline-ok">{notice}</div> : null}
        {loading && !loaded ? (
          <div className="empty">Loading configuration…</div>
        ) : (
          <textarea
            className="input mono textarea config-textarea"
            rows={26}
            spellCheck={false}
            value={text}
            onChange={(e) => {
              setText(e.target.value);
              setNotice(null);
              setParseError(validate(e.target.value).error);
            }}
          />
        )}
        <div className="hint">
          The document is posted verbatim to POST /api/config (object merge, arrays replaced
          wholesale). Invalid JSON is rejected locally before any request is made.
        </div>
      </Card>
      <div className="grid grid-2">
        <Card title="Restart" subtitle="POST /api/system/restart">
          <div className="hint">
            Restarts the audio backend and SIP engine so restart-required configuration changes take
            effect. Active calls are dropped.
          </div>
          {restartMessage ? <div className="inline-ok">{restartMessage}</div> : null}
          <div className="field-row">
            <ConfirmButton
              className="btn btn-danger"
              label="Restart engines"
              confirmLabel="Confirm restart?"
              disabled={restartBusy}
              onConfirm={handleRestart}
            />
            {restartBusy ? <span className="muted small">requesting restart…</span> : null}
          </div>
        </Card>

        <Card
          title="Self-test"
          subtitle="POST /api/system/self-test"
          actions={
            <button type="button" className="btn btn-small btn-primary" onClick={handleSelfTest} disabled={testBusy}>
              {testBusy ? 'running…' : 'run self-test'}
            </button>
          }
        >
          {testError ? <div className="inline-error">self-test failed: {testError}</div> : null}
          {!checks && !testError ? (
            <div className="hint">Run the built-in checks: audio backend, aes67-daemon, PTP, SIP accounts and line loopback.</div>
          ) : null}
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
                      <StatusPill
                        state={c?.ok ? 'ok' : 'failed'}
                        tone={c?.ok ? 'ok' : 'err'}
                      >
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
      </div>

    </div>
  );
}
