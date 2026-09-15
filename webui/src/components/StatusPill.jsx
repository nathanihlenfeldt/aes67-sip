//
//  StatusPill.jsx
//
//  Small coloured status chip.  `tone` may be passed explicitly
//  (ok | warn | err | off | info) otherwise it is derived from `state`.
//

const OK = ['ok', 'up', 'on', 'true', 'yes', 'running', 'registered', 'locked', 'in_call',
  'connected', 'receiving', 'active', 'enabled', 'loaded', 'stable', 'pass', 'passed', 'good'];
const WARN = ['idle', 'dialing', 'ringing', 'locking', 'hold', 'held', 'on_hold', 'pending',
  'connecting', 'warning', 'degraded', 'partial', 'unstable', 'unknown', 'warn'];
const ERR = ['error', 'failed', 'fail', 'unlocked', 'unreachable', 'offline', 'down', 'fault',
  'lost', 'invalid', 'critical', 'err'];
const OFF = ['disabled', 'off', 'none', 'inactive', 'na', 'n/a', 'null', 'unknown_state'];

export function stateTone(state) {
  if (state === undefined || state === null || state === '') return 'off';
  if (typeof state === 'boolean') return state ? 'ok' : 'off';
  if (typeof state === 'number') return state === 0 ? 'off' : 'ok';
  const s = String(state).trim().toLowerCase().replace(/[\s-]+/g, '_');
  if (OK.includes(s)) return 'ok';
  if (WARN.includes(s)) return 'warn';
  if (ERR.includes(s)) return 'err';
  if (OFF.includes(s)) return 'off';
  return 'info';
}

export default function StatusPill({ state, tone, children, title }) {
  const resolved = tone || stateTone(state);
  const text = children !== undefined ? children : state === undefined || state === null || state === ''
    ? 'n/a'
    : String(state);
  return (
    <span className={`pill pill-${resolved}`} title={title || text}>
      {text}
    </span>
  );
}
