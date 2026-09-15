//
//  useStatus.js
//
//  Polls GET /api/status every `intervalMs` (default 500 ms).  Polling is paused
//  while the tab is hidden and resumed (with an immediate fetch) when it becomes
//  visible again.  Never throws: transport / HTTP errors are surfaced as a string
//  in `error` while the last good status is retained so tables keep rendering.
//

import { useCallback, useEffect, useRef, useState } from 'react';
import { getStatus } from '../api';

export const POLL_INTERVAL_MS = 500;

export default function useStatus(intervalMs = POLL_INTERVAL_MS) {
  const [status, setStatus] = useState(null);
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(true);
  const [reloadToken, setReloadToken] = useState(0);

  const aliveRef = useRef(true);
  const inflightRef = useRef(false);
  const timerRef = useRef(null);

  const refresh = useCallback(() => setReloadToken((t) => t + 1), []);

  useEffect(() => {
    aliveRef.current = true;
    return () => {
      aliveRef.current = false;
    };
  }, []);

  useEffect(() => {
    let stopped = false;
    timerRef.current = null;

    async function tick() {
      // Skip work entirely while the tab is in the background.
      if (document.visibilityState === 'hidden') return;
      if (inflightRef.current) return;
      inflightRef.current = true;
      try {
        const data = await getStatus();
        if (!stopped && aliveRef.current) {
          setStatus(data || {});
          setError(null);
        }
      } catch (err) {
        if (!stopped && aliveRef.current) {
          setError(err && err.message ? err.message : 'status request failed');
        }
      } finally {
        inflightRef.current = false;
        if (!stopped && aliveRef.current) setLoading(false);
      }
    }

    function schedule() {
      if (stopped) return;
      timerRef.current = setTimeout(async () => {
        await tick();
        schedule();
      }, intervalMs);
    }

    function start() {
      clearTimeout(timerRef.current);
      tick().then(() => schedule());
    }

    function onVisibilityChange() {
      if (document.visibilityState === 'hidden') {
        clearTimeout(timerRef.current);
        timerRef.current = null;
      } else {
        start();
      }
    }

    start();
    document.addEventListener('visibilitychange', onVisibilityChange);

    return () => {
      stopped = true;
      clearTimeout(timerRef.current);
      document.removeEventListener('visibilitychange', onVisibilityChange);
    };
  }, [intervalMs, reloadToken]);

  return { status, error, loading, refresh };
}
