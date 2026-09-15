//
//  api.js
//
//  Thin, dependency-free wrapper around the aes67-sip REST API (see docs/api.md).
//  One exported function per documented endpoint.  Every call:
//    * checks res.ok
//    * throws an Error carrying the text/plain error body on failure
//    * returns the parsed JSON body (or null for 204 / empty responses)
//

export const API_BASE = '/api';

async function request(path, { method = 'GET', body } = {}) {
  const init = { method, headers: { Accept: 'application/json' } };

  if (body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = typeof body === 'string' ? body : JSON.stringify(body);
  }

  let res;
  try {
    res = await fetch(API_BASE + path, init);
  } catch (err) {
    // Network level failure: the gateway is down or unreachable.
    throw new Error(`gateway unreachable (${err && err.message ? err.message : 'network error'})`);
  }

  if (!res.ok) {
    let detail = '';
    try {
      detail = await res.text();
    } catch (_) {
      /* body already consumed or unreadable */
    }
    detail = (detail || '').trim();
    throw new Error(`HTTP ${res.status}${detail ? `: ${detail}` : ''}`);
  }

  if (res.status === 204) return null;

  const text = await res.text();
  if (!text) return null;

  try {
    return JSON.parse(text);
  } catch (_) {
    throw new Error('invalid JSON in response');
  }
}

/* ------------------------------------------------------------------ */
/* Version / configuration                                             */
/* ------------------------------------------------------------------ */

/** GET /api/version -> { name, version, build } */
export function getVersion() {
  return request('/version');
}

/** GET /api/config -> full runtime configuration document */
export function getConfig() {
  return request('/config');
}

/** POST /api/config -> effective configuration after the (partial) update */
export function setConfig(config) {
  return request('/config', { method: 'POST', body: config });
}

/* ------------------------------------------------------------------ */
/* Status / lines                                                      */
/* ------------------------------------------------------------------ */

/** GET /api/status -> single poll document used by the UI */
export function getStatus() {
  return request('/status');
}

/** GET /api/lines -> array of line status objects */
export function getLines() {
  return request('/lines');
}

/** GET /api/lines/{id} -> one line status object */
export function getLine(id) {
  return request(`/lines/${encodeURIComponent(id)}`);
}

/** GET /api/lines/{id}/config -> line configuration object */
export function getLineConfig(id) {
  return request(`/lines/${encodeURIComponent(id)}/config`);
}

/** POST /api/lines/{id}/config -> persisted line configuration (partial merge) */
export function setLineConfig(id, config) {
  return request(`/lines/${encodeURIComponent(id)}/config`, { method: 'POST', body: config });
}

/**
 * POST /api/lines/{id}/call -> { ok, line }
 * action: dial | answer | hangup | hold | dtmf
 */
export function lineCall(id, action, extra = {}) {
  return request(`/lines/${encodeURIComponent(id)}/call`, {
    method: 'POST',
    body: { action, ...extra },
  });
}

/** GET /api/lines/{id}/levels -> { rx_dbfs, tx_dbfs } */
export function getLineLevels(id) {
  return request(`/lines/${encodeURIComponent(id)}/levels`);
}

/* ------------------------------------------------------------------ */
/* aes67-daemon passthrough                                            */
/* ------------------------------------------------------------------ */

/** GET /api/aes67/config -> daemon configuration */
export function getAes67Config() {
  return request('/aes67/config');
}

/** POST /api/aes67/config -> daemon configuration update */
export function setAes67Config(config) {
  return request('/aes67/config', { method: 'POST', body: config });
}

/** GET /api/aes67/ptp/status -> { status, gmid, jitter } */
export function getPtpStatus() {
  return request('/aes67/ptp/status');
}

/** GET /api/aes67/sinks -> daemon sink list */
export function getAes67Sinks() {
  return request('/aes67/sinks');
}

/** GET /api/aes67/sources -> daemon source list */
export function getAes67Sources() {
  return request('/aes67/sources');
}

/** PUT /api/aes67/sinks/{id} -> create or update a sink */
export function putAes67Sink(id, sink) {
  return request(`/aes67/sinks/${encodeURIComponent(id)}`, { method: 'PUT', body: sink });
}

/** DELETE /api/aes67/sinks/{id} -> remove a sink */
export function deleteAes67Sink(id) {
  return request(`/aes67/sinks/${encodeURIComponent(id)}`, { method: 'DELETE' });
}

/** PUT /api/aes67/sources/{id} -> create or update a source */
export function putAes67Source(id, source) {
  return request(`/aes67/sources/${encodeURIComponent(id)}`, { method: 'PUT', body: source });
}

/** DELETE /api/aes67/sources/{id} -> remove a source */
export function deleteAes67Source(id) {
  return request(`/aes67/sources/${encodeURIComponent(id)}`, { method: 'DELETE' });
}

/** GET /api/aes67/browse/sources/{all|mdns|sap} -> discovered remote sources */
export function browseRemoteSources(kind = 'all') {
  return request(`/aes67/browse/sources/${encodeURIComponent(kind)}`);
}

/** GET /api/aes67/source/sdp/{id} -> SDP of a local source (used for sink wiring) */
export function getSourceSdp(id) {
  return request(`/aes67/source/sdp/${encodeURIComponent(id)}`);
}

/* ------------------------------------------------------------------ */
/* System                                                              */
/* ------------------------------------------------------------------ */

/** GET /api/log?lines=N -> { lines: [...] } */
export function getLog(lines = 200) {
  return request(`/log?lines=${encodeURIComponent(lines)}`);
}

/** POST /api/system/restart -> restart the audio + SIP engines */
export function restartSystem() {
  return request('/system/restart', { method: 'POST' });
}

/** POST /api/system/self-test -> { ok, checks: [{ name, ok, detail }] } */
export function selfTest() {
  return request('/system/self-test', { method: 'POST' });
}

export default {
  getVersion,
  getConfig,
  setConfig,
  getStatus,
  getLines,
  getLine,
  getLineConfig,
  setLineConfig,
  lineCall,
  getLineLevels,
  getAes67Config,
  setAes67Config,
  getPtpStatus,
  getAes67Sinks,
  getAes67Sources,
  putAes67Sink,
  deleteAes67Sink,
  putAes67Source,
  deleteAes67Source,
  browseRemoteSources,
  getSourceSdp,
  getLog,
  restartSystem,
  selfTest,
};
