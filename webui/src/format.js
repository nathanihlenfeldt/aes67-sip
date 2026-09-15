//
//  format.js
//
//  Defensive formatting helpers.  Status documents may omit whole sub-objects and
//  level values are `null` (or the string "-inf") for digital silence, so every
//  helper accepts `undefined | null | number | string` and never throws.
//

export const MIN_DBFS = -60;
export const MAX_DBFS = 0;

export function isNum(v) {
  return typeof v === 'number' && Number.isFinite(v);
}

/** Coerce a JSON value into a finite number, else null. */
export function num(v, fallback = null) {
  if (isNum(v)) return v;
  if (typeof v === 'string' && v.trim() !== '') {
    const n = Number(v);
    if (Number.isFinite(n)) return n;
  }
  return fallback;
}

/** dBFS value (null / -inf aware). Returns e.g. "-18.2" or "-inf". */
export function fmtDbfs(v) {
  const n = num(v);
  if (n === null || n <= MIN_DBFS) return n === null ? '-inf' : n.toFixed(1);
  if (n > 0) return `+${n.toFixed(1)}`;
  return n.toFixed(1);
}

/** Any dB value, 1 decimal place. */
export function fmtDb(v, fallback = 'n/a') {
  const n = num(v);
  if (n === null) return fallback;
  return `${n > 0 ? '+' : ''}${n.toFixed(1)}`;
}

/** Seconds as mm:ss (or hh:mm:ss above one hour). */
export function fmtDuration(sec) {
  const n = num(sec);
  if (n === null || n < 0) return '--:--';
  const total = Math.floor(n);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const pad = (x) => String(x).padStart(2, '0');
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${pad(m)}:${pad(s)}`;
}

/** Uptime, same shape as durations but "0:00:05" style for hours. */
export function fmtUptime(sec) {
  const n = num(sec);
  if (n === null) return 'n/a';
  return fmtDuration(n);
}

/** Sample rate -> "48 kHz". */
export function fmtSampleRate(v) {
  const n = num(v);
  if (n === null) return 'n/a';
  if (n % 1000 === 0) return `${n / 1000} kHz`;
  return `${(n / 1000).toFixed(1)} kHz`;
}

/** Generic integer display. */
export function fmtInt(v, fallback = 'n/a') {
  const n = num(v);
  if (n === null) return fallback;
  return String(Math.round(n));
}

/** Percentage clamp to 0..100. */
export function clampPct(v) {
  if (!Number.isFinite(v)) return 0;
  return Math.min(100, Math.max(0, v));
}

/** Map a dBFS level onto 0..100 % of the -60..0 range. */
export function levelToPct(level, min = MIN_DBFS, max = MAX_DBFS) {
  const n = num(level);
  if (n === null || n <= min) return 0;
  if (n >= max) return 100;
  return clampPct(((n - min) / (max - min)) * 100);
}

/** "rgb" helper for the numeric fields coming from the gateway. */
export function arr(v) {
  return Array.isArray(v) ? v : [];
}

/** Extract a list out of a daemon response that may be an array or {key: []}. */
export function asList(data, key) {
  if (Array.isArray(data)) return data;
  if (data && Array.isArray(data[key])) return data[key];
  if (data && Array.isArray(data.sinks)) return data.sinks;
  if (data && Array.isArray(data.sources)) return data.sources;
  return [];
}

/**
 * Best effort severity detection for a log line produced by the C++ logger.
 * Matches "ERROR", "WARN", "[E]", "W:", "debug" ... case insensitively.
 */
export function logSeverity(line) {
  const s = String(line || '');
  const patterns = [
    ['error', /(\berror\b|\bfatal\b|\bcritical\b|\[e\]|^\s*e[:\/]|level\s*=\s*3)/i],
    ['warn', /(\bwarn(ing)?\b|\[w\]|^\s*w[:\/]|level\s*=\s*2)/i],
    ['debug', /(\bdebug\b|\[d\]|^\s*d[:\/]|level\s*=\s*4)/i],
    ['trace', /(\btrace\b|\[t\]|^\s*t[:\/])/i],
  ];
  for (const [name, re] of patterns) {
    if (re.test(s)) return name;
  }
  return 'info';
}
