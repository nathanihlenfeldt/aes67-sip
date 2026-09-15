//
//  Card.jsx
//
//  Simple titled panel with an optional right aligned action area.
//

export default function Card({ title, subtitle, actions, children, className = '', bodyClass = '' }) {
  return (
    <section className={`card ${className}`}>
      {(title || actions) && (
        <header className="card-head">
          <div>
            <h2 className="card-title">{title}</h2>
            {subtitle ? <div className="card-subtitle">{subtitle}</div> : null}
          </div>
          {actions ? <div className="card-actions">{actions}</div> : null}
        </header>
      )}
      <div className={`card-body ${bodyClass}`}>{children}</div>
    </section>
  );
}
