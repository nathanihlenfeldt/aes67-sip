//
//  ConfirmButton.jsx
//
//  Two-step destructive action button: first click arms it ("Confirm?"), a second
//  click within 3 s runs it.  Avoids a toast/modal dependency for destructive ops.
//

import { useEffect, useRef, useState } from 'react';

export default function ConfirmButton({
  label = 'Delete',
  confirmLabel = 'Confirm?',
  onConfirm,
  className = 'btn btn-danger',
  disabled = false,
  title,
}) {
  const [armed, setArmed] = useState(false);
  const [busy, setBusy] = useState(false);
  const timerRef = useRef(null);

  useEffect(() => () => clearTimeout(timerRef.current), []);

  async function handleClick(ev) {
    ev.stopPropagation();
    if (disabled || busy) return;
    if (!armed) {
      setArmed(true);
      clearTimeout(timerRef.current);
      timerRef.current = setTimeout(() => setArmed(false), 3000);
      return;
    }
    clearTimeout(timerRef.current);
    setArmed(false);
    setBusy(true);
    try {
      await onConfirm();
    } finally {
      setBusy(false);
    }
  }

  return (
    <button
      type="button"
      className={armed ? `${className} is-armed` : className}
      onClick={handleClick}
      disabled={disabled || busy}
      title={title || (armed ? 'Click again to confirm' : label)}
    >
      {busy ? '…' : armed ? confirmLabel : label}
    </button>
  );
}
