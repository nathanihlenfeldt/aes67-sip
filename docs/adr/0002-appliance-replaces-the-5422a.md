# The appliance replaces the site's 5422A, with no hardware fallback

Status: accepted

The site's party-line mixing is a Studio Technologies 5422A configured as a single
bus, so every beltpack hears every other one. The requirement is **eight party
lines, separated by department**, with the off-site FreePBX conference in the same
appliance — so the 5422A is removed rather than kept wired as a fallback, and the
appliance becomes the only mixing point on the site.

## Consequences

- **No hardware backstop for site comms.** With the 5422A gone, recovery from an
  appliance failure is a rebuild — installer plus a configuration restore — or a
  cold spare. Accepted deliberately.
- **Regressions accepted.** Mic-kill ("talk off") cannot be sent to a plain AES67
  peer and is dropped. The box's <100 µs internal latency is replaced by one ALSA
  period plus the 1 ms RTP cadence in each direction; that figure is to be
  *measured*, not asserted. Auto-mix (NOM) is not implemented: a party line is a
  plain sum with mix-minus per member.
- **Acceptance becomes absolute, not comparative.** Once the box is gone there is
  nothing to A/B against, so acceptance is zero xruns over a soak, a measured
  latency figure, working call tones and the eight lines separating as intended.
  Measure against the 5422A before it is removed if a comparison is wanted.
- **`docs/DECISIONS.md` #1 is now wrong.** "4-wire, no PTT. Endpoint audio is
  continuous; there is no keying signal" describes a device this site does not
  have: the endpoints are M371A beltpacks with two talk and two listen channels.
- Sizing follows the beltpacks, not the buses: each pack needs two talk and two
  listen channels, so the RAVENNA device has to run at 32 channels per direction
  (against a 64 ceiling) for 16 packs, and the soak is a 32-channel load test.
