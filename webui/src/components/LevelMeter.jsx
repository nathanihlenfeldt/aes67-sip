//
//  LevelMeter.jsx
//
//  Horizontal dBFS meter over the -60..0 range with a peak-hold marker.
//  `level` may be a number, null, "-inf" or undefined (all treated as -inf).
//

import { useEffect, useRef, useState } from 'react';
import { fmtDbfs, levelToPct, MIN_DBFS, MAX_DBFS } from '../format';

export default function LevelMeter({
  level,
  label,
  min = MIN_DBFS,
  max = MAX_DBFS,
  showValue = true,
  compact = false,
}) {
  const pct = levelToPct(level, min, max);
  const [peak, setPeak] = useState(pct);
  const peakRef = useRef(pct);

  useEffect(() => {
    if (pct >= peakRef.current) {
      peakRef.current = pct;
    }
    setPeak(peakRef.current);
    // Release the held peak shortly after the signal drops.
    const t = setTimeout(() => {
      peakRef.current = pct;
      setPeak(pct);
    }, 1500);
    return () => clearTimeout(t);
  }, [pct]);

  const tone = pct >= 99 ? 'has-clip' : pct >= 90 ? 'is-hot' : '';

  return (
    <div className={`meter${compact ? ' meter-compact' : ''}`}>
      {label ? <span className="meter-label">{label}</span> : null}
      <div className="meter-track" role="meter" aria-valuemin={min} aria-valuemax={max} aria-valuenow={pct}>
        <div className={`meter-fill ${tone}`} style={{ width: `${pct}%` }} />
        <div className="meter-peak" style={{ left: `calc(${peak}% - 1px)` }} />
      </div>
      {showValue ? <span className="meter-value">{fmtDbfs(level)}</span> : null}
    </div>
  );
}
