# Roadmap: an endpoint-agnostic AES67 intercom matrix

The product claim to aim at: **the most complete open source, software-defined,
AES67-native production intercom matrix**. That category has no real incumbent —
the confident systems are proprietary hardware (Riedel Artist, Clear-Com Eclipse,
RTS/Telex, the Studio Technologies intercom engines), while open source offers
point solutions (the AES67 daemon, RAVENNA/ALSA, FreeSWITCH or Asterisk
conferences, PipeWire). No single open project puts the audio plane, the matrix,
the off-site path and the operational tooling in one place.

## What makes that claim reachable

- **Endpoint-agnostic core.** An endpoint declares how many talk and listen
  channels it has; the matrix never assumes a shape. This is the property that lets
  one build serve a two-channel beltpack, a fixed panel, a console or a DAW — and
  it is cheap to get right now and expensive to retrofit later.
- **Software-defined on commodity hardware.** No proprietary cards.
- **Local party lines and the off-site path in one product** — the trunking
  capability that incumbents sell as an add-on.
- **Configuration as a file, editable in a browser, rebuildable from one document.**
- **Openness as a design goal, not a licence**: a new endpoint family or a new mix
  feature should be addable without touching the core.

## Milestone 1 — the endpoint-agnostic core (this spec)

Party lines with membership and contribution levels, mix-minus by construction,
endpoints of arbitrary declared shape, the conference as a participant, configuration
editable in the web UI, and diagnostics that name a dead participant. Proven by at
least two differently-shaped endpoints working through the same core, and deployed
at the reference site where it replaces the 5422A.

**Discipline for everything below:** milestone 1 does not grow. Each family earns
its own spec, and nothing enters it without a driver from the reference site.

## Feature families, with the hook each one needs

| Family | What it adds | Architectural hook needed now |
| --- | --- | --- |
| Mix intelligence | Auto-mix (NOM) gain sharing, ducking, priority and director talk-down, VOX gating | A **bus mode**: the per-bus mix is a stage, so an algorithm can replace the plain sum; priority needs a per-member priority and holdover attribute |
| IFB and program feeds | Listen-only feeds, interruptible foldback with one or many interrupt inputs, program distribution | Participants need **direction** (talk, listen, or both) instead of assuming both |
| Signalling and control | PTT and mic state, call tones beyond the in-band case, talk-off/mic-kill, tallies, GPIO triggers (follow a vision mixer) | A **control channel beside the audio** plus endpoint capabilities, because in-band only carries what is already in-band |
| Endpoint management | Discovery and onboarding, per-family capability profiles, templates, a bring-up flow, stream naming conventions | An **endpoint profile registry** keyed by family, holding the channel shape and the commissioning steps |
| Runtime operation | Level, mute and route control during a show, with roles, lock-outs and an audit trail | The matrix's snapshot-and-reconfigure path, plus **authorization** — the API has none today, which makes that a blocker rather than a nicety for this family |
| Monitoring and alerting | Per-participant meters and history, dead-endpoint alerts, webhooks or SNMP, metrics export | The status contract plus a per-participant **health record** |
| Availability and scale | Dual NIC, PTP fallback (the appliance as grandmaster), spare appliance with configuration sync, watchdog and stream rebuild, beyond 64 channels | Channel and stream mapping as data (already true); PTP fallback is a daemon concern |
| Interop breadth | AM824 and other codecs, unicast and multicast, RAVENNA, Dante in AES67 mode, ST 2110-30, further trunk types | Endpoint shapes and codecs are already per-participant data |
| Multi-site | One matrix across sites without leaving the show silent when the link drops, per-site autonomy | The conference is already a remote participant — generalize that abstraction |
| Recording | Bus and participant recording for QC and disputes, with retention and metadata | A recorder is just another endpoint with a listen side |
| Security and management | API authentication and TLS, roles, secret handling, signed configurations, audit logs | The API layer; a runtime control surface makes this urgent rather than optional |
| Quality engineering | Hardware-in-the-loop audio regression, latency budget tests, automated soak, a bench rig | The class of bug this project already hit — an untriggered ALSA stream — is invisible to the current test suite |

## Making "complete" measurable

"Most complete" needs a frame, or it is unfalsifiable. The artifact is the
**[capability scorecard](capability-scorecard.md)**: the family list above as rows, the
incumbent systems and this project as columns, each cell resting on that vendor's own
documentation. It turns the ambition into a work list, shows honestly where the project is
behind, and stops the claim drifting into marketing.

Its first run (2026-09-17) grades every row for this project and for three systems it could
reach - Clear-Com Eclipse HX, the Studio Technologies 5422A and the AES67 daemon - and leaves
Riedel Artist and RTS/Telex entirely `?` rather than guessing: their own documentation is behind
logins and script-rendered pages from this environment. The document
names the artefact that would settle each hole and is written to be re-run. What it already
shows is in its ranking: security and management first (an unauthenticated control surface is
not a product), then mix intelligence and IFB - the two capabilities of the 5422A this
appliance replaces and does not yet have.


## The honest risks

- **Resources.** Every family above is real engineering. The roadmap is a direction,
  not a schedule, and the reference site's deployment is what funds it.
- **Reliability on a show.** Coming for installed systems means being trustworthy
  when it matters; the availability, monitoring and quality families are what
  justify that, and none of them are optional for the claim.
- **Security.** An intercom matrix that anyone on the network can reconfigure is not
  a product. The missing API authentication is a defect, not a feature gap.
