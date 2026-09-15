//
//  App.jsx
//
//  Browser router, operator nav shell, global connection banner and the
//  non-blocking error strip.  The single /api/status poll lives in StatusProvider.
//

import { useEffect, useState } from 'react';
import { BrowserRouter, NavLink, Navigate, Route, Routes } from 'react-router-dom';

import { StatusProvider, useGlobalStatus } from './StatusContext';
import { getVersion } from './api';
import StatusPill from './components/StatusPill';

import Dashboard from './pages/Dashboard';
import Lines from './pages/Lines';
import Aes67 from './pages/Aes67';
import Settings from './pages/Settings';
import Diagnostics from './pages/Diagnostics';

const NAV = [
  { to: '/dashboard', label: 'Dashboard' },
  { to: '/lines', label: 'Lines' },
  { to: '/aes67', label: 'AES67' },
  { to: '/diagnostics', label: 'Diagnostics' },
  { to: '/settings', label: 'Settings' },
];

function ConnectionBanner() {
  const { status, error, loading, refresh } = useGlobalStatus();

  if (error) {
    return (
      <div className="banner banner-error" role="alert">
        <span className="banner-icon">!</span>
        <span>
          <strong>Gateway unreachable</strong> — {error}
          {status ? ' · showing last known state' : ''}
        </span>
        <button type="button" className="btn btn-small" onClick={refresh}>
          Retry
        </button>
      </div>
    );
  }

  if (loading && !status) {
    return (
      <div className="banner banner-info" role="status">
        <span className="spinner" aria-hidden="true" /> Connecting to gateway…
      </div>
    );
  }

  return null;
}

function Shell() {
  const { status } = useGlobalStatus();
  const [version, setVersion] = useState(null);
  const [versionError, setVersionError] = useState(null);

  useEffect(() => {
    let alive = true;
    getVersion()
      .then((v) => alive && setVersion(v))
      .catch((e) => alive && setVersionError(e.message));
    return () => {
      alive = false;
    };
  }, []);

  const gwVersion = status?.version || version?.version || (versionError ? '—' : '…');
  const backend = status?.audio?.backend;
  const daemonUp = status?.aes67?.connected;

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true" />
          <span className="brand-name">AES67 SIP Gateway</span>
          <span className="brand-version">v{gwVersion}</span>
        </div>

        <nav className="nav">
          {NAV.map((item) => (
            <NavLink
              key={item.to}
              to={item.to}
              className={({ isActive }) => (isActive ? 'nav-link nav-link-active' : 'nav-link')}
            >
              {item.label}
            </NavLink>
          ))}
        </nav>

        <div className="topbar-status">
          {backend ? <span className="muted small">audio: {backend}</span> : null}
          <StatusPill
            state={daemonUp === undefined ? 'unknown' : daemonUp}
            tone={daemonUp === true ? 'ok' : daemonUp === false ? 'err' : 'off'}
            title="aes67-daemon connectivity"
          >
            {daemonUp === true ? 'daemon up' : daemonUp === false ? 'daemon down' : 'daemon ?'}
          </StatusPill>
        </div>
      </header>

      <ConnectionBanner />

      <main className="content">
        <Routes>
          <Route path="/" element={<Navigate to="/dashboard" replace />} />
          <Route path="/dashboard" element={<Dashboard />} />
          <Route path="/lines" element={<Lines />} />
          <Route path="/aes67" element={<Aes67 />} />
          <Route path="/diagnostics" element={<Diagnostics />} />
          <Route path="/settings" element={<Settings />} />
          <Route path="*" element={<Navigate to="/dashboard" replace />} />
        </Routes>
      </main>

      <footer className="footer">
        <span>
          {version?.name || 'aes67-sip'} {status?.version ? `v${status.version}` : ''}
          {version?.build ? ` · ${version.build}` : ''}
        </span>
        <span className="muted">polling /api/status every 500 ms · paused while tab hidden</span>
      </footer>
    </div>
  );
}

export default function App() {
  return (
    <StatusProvider>
      <BrowserRouter>
        <Shell />
      </BrowserRouter>
    </StatusProvider>
  );
}
