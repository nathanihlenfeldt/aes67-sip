# Capability scorecard

The roadmap's product claim is that this project can be **the most complete open source,
software-defined, AES67-native production intercom matrix**. "Most complete" is unfalsifiable
until it is measured, so this document measures it: the roadmap's feature families are the
rows, the incumbent systems and this project are the columns, and every cell is meant to rest
on a primary source.

Its purpose is a **work list**, not marketing. Where this project is behind, the table says so.

Every source's own retrieval date is in the source table at the end (**2026-09-17** for this
run). Method first, because the method is what makes the table worth reading.

## Method

**Grading.** Each cell is one of:

| Mark | Meaning |
| --- | --- |
| **S** | The source states the capability, and nothing in the source conditions it away. |
| **P** | Partial: the source states part of the capability, or states it with a limit that matters to the reference site (a channel ceiling, a separate product, configuration-time only). The limit is named in the row's note. |
| **A** | Absent, on one of two grounds: the source states the capability is not there, **or** the source describes the system's scope and the capability falls outside it while the system's own material leaves no room for it (the daemon's scope is "an ALSA device ... sources and sinks ... PTP"; party-line mixing is not in that scope). Where the mark rests on the second ground, the note says so. |
| **?** | **Unknown.** Nothing in this run's sources supports a verdict - including the common case of a source that simply does not address the capability. Silence is not evidence, so it is `?` rather than `A`: a vendor not mentioning GPIO does not mean there is no GPIO. |

One deliberate deviation from `CONTEXT.md` below: the **row labels are the roadmap's own
words**, so an `_Avoid_` word may appear in a row (the roadmap's "ducking", its "trunk types")
- the alternative would be rows that no longer match the roadmap, and matching the roadmap is
what makes this a scorecard of *its* families. The prose uses the glossary's terms.


**What counts as a source.** A cited cell must rest on that system's *own* documentation - a
datasheet, manual, product documentation page, or (for this project) this repository or a
published commit. Never a review, a reseller page, a conference talk or an inference from
silence. Where the source is this repository, the citation is a path, a commit or a ticket.

**Silence is not evidence.** A vendor not mentioning GPIO does not mean there is no GPIO, so
that cell is `?`, not `A`. `A` is used only where absence is stated.

**Configuration-time is not show-time.** Several systems configure routing, levels and IFB in
a web page; that is a configuration capability, not an operator surface for a live show with
roles and lock-outs. The runtime-operation row distinguishes the two.

**Re-running this.** Every source's URL and retrieval date are in the source list, and the
"could not verify" section names the artefact that would settle each unknown (mostly a
specific datasheet). Re-run it when a vendor ships firmware, or when a new system needs a
column: replace the cell's mark, move the source's date, and leave the rest alone.

## The systems in the columns

| Column | Judged from |
| --- | --- |
| **aes67-sip** | This repository: code, docs, tests, tickets and commits. |
| **Riedel Artist** | *Not reachable in this run.* Riedel's datasheets sit behind a MyRiedel login ([source 5](#sources)); their public site is marketing-level. Every cell is `?` until a datasheet or manual is obtained. |
| **Clear-Com Eclipse HX** | Clear-Com's own Eclipse HX product documentation page ([source 2](#sources)) - enough for interop, redundancy, roles, trunking and control, not for the mixing or signalling features. |
| **RTS/Telex (ODIN/ADAM)** | *Not reachable in this run.* The product pages under `products.rtsintercoms.com` did not serve content to this environment (404/empty), and the downloads library is script-rendered. Every cell is `?`. |
| **Studio Technologies 5422A** | The vendor's own product page ([source 1](#sources)) and the M371A/542A-series facts recorded with their datasheet and user-guide citations in the local research notes ([source 3](#sources)). It is a party-line/IFB *engine*, not a matrix with trunking, so some rows do not apply to its category. |
| **Open-source point solutions** | The AES67 Linux Daemon's own README ([source 4](#sources)). Which project represents the column is a judgement, and this is it, stated as one: the daemon is the point solution that carries an audio plane, discovery and configuration in one project, and the others the roadmap names (Asterisk/FreeSWITCH conferences, PipeWire, RAVENNA/ALSA alone) are narrower - so a capability its README does not claim would be a `?` for them too, and that is why this column cannot make the product claim. |

## The scorecard

Legend: **S** supported, **P** partial (with the limit named below), **A** absent, **?**
unknown. Columns: **this** = aes67-sip, **Artist** = Riedel Artist, **Eclipse** = Clear-Com
Eclipse HX, **RTS** = RTS/Telex ODIN/ADAM, **5422A** = Studio Technologies 5422A, **OSS** =
open-source point solutions (the AES67 Linux Daemon, as the strongest of them).

| Feature family (roadmap) | this | Artist | Eclipse | RTS | 5422A | OSS |
| --- | --- | --- | --- | --- | --- | --- |
| Mix intelligence - auto-mix/NOM, ducking, priority, director talk-down, VOX | **A** | ? | ? | ? | **S** | **A** |
| IFB and program feeds | **A** | ? | ? | ? | **S** | **A** |
| Signalling and control - PTT/mic state, call, mic-kill, tallies, GPIO | **P** | ? | **P** | ? | ? | **A** |
| Endpoint management - discovery, onboarding, profiles, templates | **P** | ? | **P** | ? | ? | **P** |
| Runtime operation - level/mute/route live, roles, lock-outs, audit | **P** | ? | **P** | ? | **P** | **P** |
| Monitoring and alerting - meters, history, dead-endpoint alerts, metrics | **P** | ? | ? | ? | ? | **P** |
| Availability and scale - redundancy, PTP fallback, spare + sync, >64 ch | **P** | ? | **S** | ? | **P** | **P** |
| Interop breadth - codecs, multicast/unicast, RAVENNA, Dante AES67, ST 2110-30 | **P** | ? | **S** | ? | **S** | **P** |
| Multi-site - one matrix across sites, per-site autonomy | **P** | ? | **S** | ? | ? | **?** |
| Recording - bus and participant recording, retention, metadata | **A** | ? | ? | ? | ? | **?** |
| Security and management - API auth, TLS, roles, secrets, signed configs, audit | **A** | ? | ? | ? | ? | **?** |
| Quality engineering - hardware-in-the-loop regression, latency budget, soak | **P** | ? | ? | ? | ? | **P** |

### The cells, with their limits

**Mix intelligence.** `A` for this project, and it is stated rather than inferred: ADR-0002
records "Auto-mix (NOM) is not implemented: a party line is a plain sum with mix-minus per
member", and the roadmap's first family is the hook for it. `S` for the 5422A: the vendor lists
"Auto Mix for enhanced audio performance" [1], and their tech note - quoted in the local
research notes [3] - says Auto Mix "will reduce the signal level on channels that are not
currently designated as an active or holdover 'talker' channel" (second-hand: the vendor's tech
note as quoted in the local research notes [3]; the vendor's datasheet is on the re-run list
below). `A` for the daemon, on the scope ground in the method above: its own README describes it
as an ALSA device fed by sources and sinks, with no mixing and no party-line concept [4]. The
three matrix vendors' materials were not reachable, so their cells are `?` even though all three
are known in the field for mix features
- which is exactly the kind of knowledge this document refuses to record without a source.

**IFB and program feeds.** `A` for this project: none exists (roadmap family, nothing built),
and ADR-0002 accepts the neighbouring concession. `S` for the 5422A: "special audio functions
including summing, IFB, and audio switching" [1], with group modes including "IFB (1 Int-in)"
and "IFB (3 Int-in)" [3] - second-hand, quoted through the local research notes rather than
from the group-mode specification, which is on the re-run list below. `A` for the daemon on the
same scope ground as the row above [4].

**Signalling and control.** `P` for this project: the **call tone** - a 20 kHz signal an
endpoint sums into its talk channel - crosses, because it is in-band (`docs/runbook.md`,
`CONTEXT.md`); mic-kill does not, and ADR-0002 states it cannot ("Mic-kill ('talk off') cannot
be sent to a plain AES67 peer and is dropped"); there are no tallies, no GPIO and no control
channel beside the audio (`docs/ROADMAP.md`); `call_mode: ptt` exists in the code but is a
legacy energy-triggered mode, not a keyed PTT (DECISIONS #1 is superseded by ADR-0002). `P` for
Eclipse HX: the vendor states "alongside GPIO and HCI API for external control" [2] - external
control exists, but nothing fetched states call, mic-kill or tally behaviour. `A` for the daemon
on the scope ground: its README describes audio transport and configuration, with no control
plane [4].


**Endpoint management.** `P` for this project: an endpoint declares its own shape and the
commissioning view binds members to lines (`docs/runbook.md`, `docs/api.md`); the daemon's
SAP/mDNS discovery is browsed from the AES67 page, and a discovered source is chosen by setting
`endpoints[].aes67.remote_source_id` in the configuration - there is no per-endpoint form for it
yet (`docs/runbook.md`, ticket 08's review); and there is no profile registry, no templates and
no bring-up flow (`docs/ROADMAP.md`). `P` for Eclipse HX: "managed via EHX configuration and
optional Dynam-EC" [2] with "a wide range of user stations"; whether endpoints are discovered
or declared per model is not stated on the page. `P` for the daemon: it "implements SAP sources
discovery and advertisement ... and mDNS sources discovery" with a WebUI [4], but has no concept
of an endpoint's shape.

**Runtime operation.** `P` for this project: levels, mute, bindings and party-line membership
are editable while running and are applied (`docs/api.md`, `webui/src/pages/PartyLines.jsx`);
the conference leg has dial and hang up; but there are no roles, no lock-outs and no audit
trail, and **the API has no authentication** - which `docs/ROADMAP.md` calls a blocker for this
family rather than a nicety. `P` for Eclipse HX: "ARC's virtualized Role configuration
facilitates configuration redundancy, allowing station configurations to be picked up anywhere
in the system ... while enhancing security" [2] - roles there are a configuration concept for
failover, real but not show-time lock-outs. `P` for the 5422A: "Webpage management" [1] of the
unit's configuration. `P` for the daemon: REST API and WebUI for configuration and monitoring
[4], with no roles.

**Monitoring and alerting.** `P` for this project: per-member held-peak meters, the derived
party-line summary (`members`/`arriving`/`silent`/`unbound`, with the names) and a self-test
that names who is not arriving (`docs/api.md`, tickets 05 and 07) - but no history, no alerting,
no webhooks or SNMP and no metrics export (`docs/ROADMAP.md`). What a level *means* is bounded by
the code: a reported level is a held peak decaying at a fixed rate per millisecond
(`kPeakDecayDbPerMs` in `src/util.cpp`, used by the router and the matrix), so "silent" is not
"was silent a second ago", and per-member meters alone cannot tell a quiet room from every cable
on a line being pulled (ticket 07's own note). `P` for the daemon: PTP and
sink status are monitorable through its REST API and WebUI [4]; no per-participant history or
alerts. The matrix vendors' monitoring cells are `?`.


**Availability and scale.** `P` for this project: the reference site runs 32 channels per
direction and eight party lines (ADR-0002, `docs/runbook.md`); the installer applies the
real-time tuning - three sysctls and a CPU governor pinned to `performance` - through
`scripts/setup-ravenna.sh`, which `scripts/install.sh` calls; the appliance is a **PTP
slave only** and "making this appliance a grandmaster is deliberately out of scope for now"
(ADR-0001); and there is no dual NIC, no spare with configuration sync and no watchdog
(`docs/ROADMAP.md`). `S` for Eclipse HX: "multiple levels of redundancy ... fully automated and
unattended failover", with "redundant system controllers, backplanes, and power supplies",
"N+1 backup or mirroring of specified cards" and "redundant audio ports" [2]. `P` for the 5422A:
"Three Gigabit Ethernet interfaces support independent redundant Dante and management networks"
[1] - network redundancy, not a redundant engine. `P` for the daemon: its README documents a
64-channel configuration tested and a latency test with measured figures [4]; PTP slave only.

**Interop breadth.** `P` for this project: the source's payload is `L24` by default with `L16`,
`L2432`, `AM824` and `L32` selectable (`README.md`); the AES67 side is the daemon's sinks and
sources, which the appliance creates, subscribes and describes per line and per endpoint
(`docs/api.md`, `config/aes67-sip.conf`, `src/bridge/line_manager.cpp`), and the daemon's own
documentation is the source for what the daemon does with them [4]; a Dante endpoint in AES67
mode is a documented path (`docs/runbook.md`, with the UltimoX caveat); Q-SYS and grandmaster
matching are documented (`README.md`, "Interop notes"); unicast and multicast beyond the
multicast the daemon uses today are on the roadmap's list of what the family is meant to *add*
(`docs/ROADMAP.md`, "Interop breadth") rather than stated as absent anywhere in the code or
docs. `S` for Eclipse HX: "Third-party interfacing is achieved using E-IPA, E-DANTE, and E-MADI
cards, supporting standards like AES67, Dante, MADI, and SIP telephony" [2]. `S` for the 5422A:
"Dante audio-over-IP technology with AES67 and DDM support" [1]. `P` for the daemon: AES67/RTP
with SAP and mDNS discovery [4]. RTS's own pages advertise OMNEO, Dante and SMPTE ST 2110 as
*technologies*, but their matrix product pages did not serve content here, so `?`.

**Multi-site.** `P` for this project: the conference is already a remote participant in the
matrix (`CONTEXT.md`, ADR-0001) and the roadmap's multi-site family is "generalize that
abstraction"; there is no per-site autonomy and no second site's matrix. `S` for Eclipse HX:
"Eclipse Intelligent Linking operates without a central controller. Using self-managed mesh
trunking, the system automatically discovers resources and determines the optimal audio path"
[2]. `?` for the daemon: its README does not address linking two sites either way [4].

**Recording.** `A` for this project (roadmap family, nothing built). `?` for the daemon: its
documentation does not address recording, and silence is not evidence [4]. Every other cell is
`?`.

**Security and management.** `A` for this project, and stated by the project itself: there is no
API authentication and no TLS, `docs/ROADMAP.md` calls the missing authentication "a defect, not
a feature gap", and the conference card added runtime control to that surface (commit `e70a37c`,
whose routes are `docs/api.md`'s `POST /api/conference/config` and `/call`). The configuration
file is `0660 root:aes67-sip` (`scripts/install.sh`) - file permissions, not secret management.
`?` for the daemon: its README documents no authentication for its REST API, and silence is not
evidence [4]. `?` for Eclipse HX and the 5422A: nothing fetched from either vendor addresses
authentication, roles or audit.

**Quality engineering.** `P` for this project: 87 C++ tests (plus 16 web-UI logic tests) and
three GitHub Actions build runners (x86_64 twice and arm64,
`.github/workflows/build.yml`); but no hardware-in-the-loop audio regression, no latency-budget
test, no automated soak and no bench rig (`docs/ROADMAP.md`). The gap is not theoretical: an
untriggered ALSA stream on the appliance was invisible to the suite (that session's own record
is `.scratch/local-matrix/handoff.md`; the substream check it produced is `docs/runbook.md`), and
the inbound-call deadlock needed a real caller to find (commit `56d498a`, ticket 13). `P` for the
daemon: it ships its own latency and platform tests with published figures [4].

## What the completeness claim depends on, ranked

The rows above, ordered by how much the claim costs while they are missing. **The rule for the
order:** the `A` rows come first - each is a capability the claim cannot be made without - and
then the `P` rows by the architectural cost of finishing them. It is a ranking of dependence,
not of difficulty, and it is the order the roadmap can be worked in.

1. **Security and management (A).** An intercom matrix that anyone on the network can
   reconfigure is not a product, and this project's `A` is not a gap in polish - the roadmap
   already calls it a defect. It also gates the family below, because a runtime control surface
   without authentication is a liability.
2. **Mix intelligence (A).** "Auto Mix" is the flagship capability of the incumbent *engine*
   this project replaces (the 5422A's column is `S` here), and ADR-0002 accepted its absence as
   a regression. Until it lands, a party line full of open microphones sums their room noise.
3. **IFB and program feeds (A).** A whole workflow family (a presenter's earpiece, a director
   interrupting it) that every incumbent sells and this project does not have. It needs the
   participant-*direction* hook the roadmap names, which is also what makes multi-site general.
4. **Signalling and control (P).** What makes an intercom usable in a gallery: keyed PTT, mic
   state, mic-kill, tallies and GPIO. In-band audio carries a *call* and nothing else, so this
   family needs a control channel beside the audio - the largest architectural addition on the
   list, and the one a vision-mixer follow would depend on.
5. **Monitoring and alerting (P).** The appliance can say who is silent *now*; it cannot say
   what happened, and it cannot tell anyone when nobody is watching. "Always on" is the promise
   incumbents sell, and the availability family is unprovable without this one.
6. **Availability and scale (P).** ADR-0002 removed the hardware backstop deliberately, which
   makes a spare with configuration sync, a watchdog and PTP-fallback thinking the risk to
   retire. The appliance can never be the site's grandmaster as things stand (ADR-0001).
7. **Runtime operation (P).** Roles, lock-outs and an audit trail: the difference between a
   commissioning tool and something a gallery can use during a show. Its own build is small - a
   role model and a lock on the control surface - but it must not precede the family above it,
   which is exactly why it sits here rather than beside the control-surface work.
8. **Endpoint management (P).** The endpoint-agnostic core is the product's differentiator;
   profiles, templates and a bring-up flow are what make a second endpoint family cheap rather
   than a project. The missing per-endpoint stream form (ticket 08's review) is the smallest
   piece of this.
9. **Interop breadth (P).** The "openness" claim: ST 2110-30, unicast, and codecs beyond the
   ones already selectable. Cheapest when driven by a real device that needs it.
10. **Multi-site (P).** The trunking differentiator, and the conference is already a remote
    participant - but a second site needs the participant abstraction generalized and the
    per-site autonomy question answered.
11. **Quality engineering (P).** Not a feature and the reason to trust the others: the class of
    bug this project actually hit (an untriggered ALSA stream, a deadlock only a real caller
    exposed) is invisible to the current suite.
12. **Recording (A).** Least load-bearing for the claim, most often asked for by a client. A
    recorder is an endpoint with a listen side, so it is cheap once the participant model is
    the general one.

**What the table does not measure**, and what this project's claim actually rests on: no
incumbent column can be software-defined on commodity hardware, none is open source, and none
puts the site's own party lines and the off-site path in one appliance. Those are the
differences the product is sold on. The rows above are what "most complete" still has to earn.

## What this run could not verify

Stated plainly, because a scorecard that hides its holes is worse than no scorecard.

- **Riedel Artist: every cell.** Riedel's datasheets are behind a MyRiedel login and their
  public pages carry marketing, not capability detail; the manuals area serves only their newer
  software (STAGE, SmartPanels, OnAir) [5][6]. Nothing about Artist in this document came from
  Riedel, so nothing is claimed about it.
- **RTS/Telex: every cell.** The product pages under `products.rtsintercoms.com` did not serve
  content to this environment (an empty response, a 502 from their gateway, and 404s for the
  paths the site's own navigation exposes), and the downloads library is script-rendered [7].
- **Clear-Com: most cells.** Interop, redundancy, roles, trunking, control and configuration are
  sourced [2]; mixing, IFB, mic-kill, tallies, monitoring, recording, security and quality
  engineering are `?` because one product page does not state them - the Eclipse HX datasheet and
  the EHX manual would.
- **Studio Technologies: seven cells.** The 5422A column was graded from the vendor's product
  page [1], which states party-line/IFB/auto-mix, the channel counts, the network interfaces, the
  codec support and the management method; signalling, endpoint management, monitoring,
  multi-site, recording, security and quality engineering are `?` because that page does not
  address them. Its datasheets and user guides are on the re-run list, and two details this run
  *does* quote (the group-mode list and the Auto Mix mechanism) came through the local research
  notes rather than from the vendor directly, and are marked second-hand where they appear.
- **The Reachable sources' coverage, stated plainly:** the 5422A column rests on one product page
  plus local notes; the Eclipse column on one product page; the open-source column on the AES67
  daemon's README alone. That is what the `?` cells mean - "not stated on the artefact this run
  could reach" - not "the vendor does not have it".
- **The 5422A's latency figure.** Its internal input-to-output latency is quoted in the local
  research notes [3], which cite the vendor's specifications; the product page fetched here does
  not carry it, so this document leaves it out rather than relay a second-hand figure. The
  datasheet URL in Studio Technologies' documentation index settles it.
- **Open-source cells** come from one project's README [4]. Asterisk's and FreeSWITCH's
  conference bridges and PipeWire are named by the roadmap as point solutions but were not read
  in this run, so no cells were graded from them.
- **This project's own cells** are graded from the repository as it stands on the retrieval
  date, and the tickets they cite are local to the development machine (`.scratch/`, gitignored)
  - a reader outside it can follow the docs and the commits but not the ticket numbers.

### How to re-run it

1. **Riedel Artist**: obtain `Artist-1024` and `Artist-64` datasheets and the Director manual
   (MyRiedel account, or the distributor). Grade the twelve rows from those alone.
2. **RTS/Telex**: obtain the ODIN datasheet and the AZedit/IPedit manuals from the RTS
   downloads library in a browser, or from a distributor; the product hierarchy is
   `products.rtsintercoms.com/r/digital-matrix-frames/` and `.../r/omneo/`.
3. **Clear-Com**: the Eclipse HX datasheet and the EHX manual settle IFB, mic-kill, tallies,
   monitoring, recording and security.
4. **5422A**: the datasheet and user guide in Studio Technologies' documentation index
   (`studio-tech.com/documentation/`, the 542X-series entries) settle the latency figure and the
   group-mode list.
5. **Open source**: read the AES67 daemon's README and its own `daemon/README.md` (this run read
   the former), plus the FreeSWITCH `mod_conference` documentation, and grade that column.
6. Replace each `?` with the graded mark, add the source with its retrieval date, and leave the
   rest of the document alone. The rows and the method are meant to outlive the cells.

## Sources

Each source records its own retrieval date, so a re-run moves one row rather than the whole
table.

| # | Source | Used for | Retrieved |
| --- | --- | --- |
| 1 | Studio Technologies, *Model 5422A Dante Intercom Audio Engine* product page - <https://studio-tech.com/products/m5422a/> | 5422A column: party line, Auto Mix, IFB and switching, 32/64 channels, AES67/DDM, three network interfaces, webpage management | 2026-09-17 |
| 2 | Clear-Com, *Eclipse HX Digital Matrix System* product documentation page - <https://www.clearcom.com/Products/Products-by-Name/Eclipse-HX> | Eclipse column: AES67/Dante/MADI/SIP interfacing, E-IPA/E-DANTE/E-MADI cards, redundancy and ARC roles, Intelligent Linking trunking, GPIO and HCI API, EHX and Dynam-EC | 2026-09-17 |
| 3 | Local research notes: `.scratch/local-matrix/research/m371a-dante-aes67.md` and `matrix-facts.md`, which cite Studio Technologies' M371A/5422A datasheets and user guides and Audinate's help centre (not in the repository, so a reader outside the development machine cannot follow it) | Second-hand only, and labelled where used: the 5422A's Auto Mix behaviour and its IFB group modes; the M371A endpoint facts; the 20 kHz call tone and the limits of mic-kill over AES67 | 2026-09-17 |
| 4 | AES67 Linux Daemon README - <https://raw.githubusercontent.com/bondagit/aes67-linux-daemon/master/README.md> | Open-source column: driver and PTP slave, sources and sinks, SAP and mDNS discovery, REST API and WebUI, 64-channel and latency tests, and its described scope (which is what the `A` marks rest on) | 2026-09-17 |
| 5 | Riedel, *Datasheets* download page - <https://www.riedel.net/en/downloads/datasheets/> | Evidence that Riedel's datasheets require a MyRiedel login | 2026-09-17 |
| 6 | Riedel Online Help Center (manuals) - <https://www.riedel.net/en/service-support/manuals/> | Evidence that the public manuals area covers only newer software, not Artist | 2026-09-17 |
| 7 | RTS Intercom Systems site - <https://rtsintercoms.com/> and its product/download paths, e.g. <https://products.rtsintercoms.com/r/downloads-library/> | Evidence that the RTS product and download pages did not serve content to this environment | 2026-09-17 |

**Vendor names and product names belong to their owners.** This document quotes them from their
own published documentation, for comparison, and nothing here is a claim of endorsement or of
partnership.

