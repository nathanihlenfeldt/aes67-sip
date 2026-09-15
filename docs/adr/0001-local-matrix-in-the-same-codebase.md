# A site-local matrix lives in this codebase, independent of the SIP engine

Status: accepted (supersedes `docs/DECISIONS.md` #6)

The gateway was built to relay a site's panels into an off-site FreePBX conference,
and DECISIONS.md #6 made that conference the intercom matrix. We now want panels to
talk to each other *on site* — so that site talk paths survive the WAN or PBX
failing, and stop paying two WAN hops for a conversation in the same building.

We add a local **matrix** to this codebase rather than start a second product, and
deploy it as a second module beside the gateway: the matrix owns participants and
routes and never depends on the SIP engine, while the gateway keeps working — and
installing — without it. Routes are first-class, each carrying its own gain and
mute, rather than a reuse of lines; and every participant's mix is **mix-minus by
construction**, so feedback loops are impossible rather than merely discouraged.

## Considered Options

- **A separate product.** Rejected: it duplicates the daemon client, the channel
  inventory, the web UI shell, the installer and the diagnostics for no gain.
- **Reusing lines as the routing primitive.** Rejected: a line is symmetric (the
  same channel list is read and written) and bound to one SIP call, so "hear this,
  be heard on that" is not expressible — and every gain would be per-line rather
  than per-path.

## Consequences

The conference becomes one participant among several instead of being the matrix
itself. The availability this buys is *local planning* — WAN and PBX loss — not full
autonomy: the RAVENNA device only ticks while PTP is locked, and the site's
grandmaster is on-site hardware, so losing the grandmaster still stops local paths
too. Making this appliance a grandmaster is deliberately out of scope for now.
