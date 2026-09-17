# Runbook: commissioning an endpoint at the reference site

**This is one endpoint family's procedure, not the product's.** The appliance is
endpoint-agnostic: an endpoint declares how many talk and listen channels it has and the
matrix never assumes a shape (`CONTEXT.md`). The steps below are what the reference site's
first deployment uses - a **Studio Technologies M371A beltpack in Dante AES67 mode** - and
another family (a desktop intercom unit, a console, a DAW, an AES67 box with its own web UI)
will differ in the endpoint-specific parts: how AES67 mode is enabled, how flows are created,
and how a subscription is made. The appliance's half of the procedure is the same for all of
them.

The factual basis for this family is primary sources: Studio Technologies' M371A datasheet
and user guide, and Audinate's help centre articles on AES67 mode and Dante subscriptions
(the notes that gather them, with quotations, live in the local working records - `.scratch/`,
see `AGENTS.md`). **Read the caveat before committing to this family**: the mechanism for a
SAP-announced third-party AES67 flow to appear in Dante Controller's routing grid is
confirmed, but Audinate's own documentation says the UltimoX chipset "will not be able to send
or receive AES67 RTP flows" while Studio Technologies labels the current M371A "AES67
Supported". Confirm with Studio Technologies before the site buys belts around it.

## What has to be true before you start

- The appliance is installed and its services are up (`scripts/install.sh`; see the README).
- **A PTP grandmaster exists on the VLAN** and the appliance's daemon reports `locked`. The
  reference site's is the Q-SYS Core. Audio only flows while the daemon is locked - this is
  the first thing to check whenever "nothing works". For a bench without a grandmaster the
  README shows how to run `ptp4l` from the appliance; making the appliance the *site's*
  grandmaster is out of scope (ADR-0001).
- The AES67 VLAN carries **multicast** with IGMP working, and the appliance's interface can
  reach the endpoints' subnet.
- A SIP account on the PBX for each line that needs one, and - if the site runs the
  off-site path - a conference the appliance can dial.
- **The endpoints are on the network and their shapes are known**: how many talk channels
  each sends and how many listen channels it receives. For an M371A that is **2 talk and 2
  listen**, carried in up to two flows per direction.
- The device channel budget: every endpoint's talk and listen channels occupy a channel of
  the RAVENNA device, so `audio.channels` must cover the highest channel any endpoint or
  line uses. The shipped sample opens **8**; the reference site runs **32** (16 beltpacks'
  worth, two channels each way), and `audio.channels` has to be set to the site's plan before
  commissioning - validation refuses an endpoint shape wider than the device opens.

## The steps

### 1. Declare the endpoints and the party lines (in the appliance's UI)

Open `http://<appliance>:8081` → **Party lines** (the commissioning view and editor).

1. Add an **endpoint** per device: its id (a stable key, e.g. `pack-1`), its name, and its
   **shape** - how many talk channels it sends and how many listen channels it receives, and
   which device channels those map onto. A beltpack is two talk and two listen channels, e.g.
   talk 0/1 and listen 2/3. The shape is the contract: everything else refers to an endpoint's
   own channel numbers, not to device channels.
2. Add the **party lines** the site runs (e.g. `pl1`), and give each line its **members**:
   which endpoint, which of that **endpoint's own** talk channels contributes, which of its
   listen channels hears the line, and that member's contribution level in dB. Membership is
   what routes audio: every member hears every other member at their own level, never
   themselves (mix-minus is built into the mix, not configured).
3. Save. The appliance refuses a configuration that could not work, naming the entry at
   fault (two endpoints claiming one device channel, a binding outside the shape the endpoint
   declared, a shape wider than the device). Nothing is written or applied until it validates.
   Applying it replaces each line's account, so **make configuration changes when nothing is
   on air** - a call that is up is dropped and raised again.

If the site needs the off-site path, tick **conference** on the party lines that should hear
it, and set the leg up on the **Lines** page (account + target; `dial now` raises it
immediately, `hang up` holds it down).

### 2. Enable AES67 mode on the endpoints

For this family, in **Dante Controller**: Device View → the device → **AES67 Config** → enable
AES67 mode, and leave the device **not enrolled in a DDM domain**. 48 kHz, and the site's
PTP domain (**domain 0** at the reference site).

The appliance's own sources are published by the daemon and announced over **SAP/mDNS**, so a
Dante device in AES67 mode shows them in Dante Controller's routing grid as non-Dante
sources. The appliance does not create subscriptions on the endpoints - that is Dante
Controller's job, which is why the endpoint family matters here.

### 3. Subscribe both directions

- **Endpoint → appliance.** Each endpoint's talk channels must leave the device as
  **multicast AES67 flows** (create them in Dante Controller; an M371A can carry two transmit
  flows, so a 2-channel beltpack fits in one flow of two channels or two flows of one). The
  appliance then needs that stream's SDP for the endpoint's **sink**, and today that is
  **configuration rather than a form**: set `endpoints[].aes67.remote_source_id` to the
  discovered source's id, or paste the stream's SDP into `endpoints[].aes67.remote_sdp`. Get
  both from the **AES67** page's SAP/mDNS browser (or `GET /api/aes67/browse/sources/all`),
  and set them in the **Settings** page's configuration JSON or by editing
  `/etc/aes67-sip.conf` before it is applied. Without one of the two the appliance leaves that
  sink alone and the endpoint is never heard. There is no per-endpoint form for this yet: the
  Party lines page reports whether an endpoint's talk stream is described, and does not edit
  it.
- **Appliance → endpoint.** The appliance publishes one source per listen direction (the
  daemon creates it from the endpoint's shape and announces it over SAP). In Dante Controller,
  subscribe each of the endpoint's **receive** channels to the appliance's corresponding
  source channel. Each receive channel is its own subscription: an M371A's two listen
  channels are two Dante receiver subscriptions, not one pre-mixed stream, and the beltpack
  sums them in its own headphone path - the appliance's matrix is what produces the mix-minus
  each of those channels carries.

### 4. Check the clock before you trust anything

```bash
curl -s http://<appliance>:8080/api/ptp/status     # {"status":"locked","gmid":"..."}
curl -s http://<appliance>:8081/api/status | jq .aes67.ptp
```

The appliance and the endpoints must agree on the **PTP domain and the grandmaster**. Our
sources advertise the explicit grandmaster clock ID in `a=ts-refclk`; a receiver that compares
that against the PTP domain's grandmaster (Q-SYS shows this as *grandmaster mismatch*) refuses
the flow. `GET /api/status` -> `aes67.ptp.gmid` must equal the `a=ts-refclk` value in the
endpoint's SDP, and `aes67.ignore_refclk_gmid` exists only for the case where an endpoint
legitimately advertises a different grandmaster.

## What proves success

Steps are not evidence. These are, in the order they will fail:

1. **PTP locked, both ends**: the daemon `locked`, and the endpoints' clock status showing the
   same grandmaster. Without this nothing else can pass, and the self-test says so.
2. **Every member's audio is arriving.** `GET /api/status` -> the party line's `summary`:
   `arriving` matches the members you expect, with `arriving_names` naming them. A member whose
   talk channel is bound but silent shows as `silent` and is named; a member with no talk
   channel bound is `unbound`, the one configuration state that means "cannot be heard".
   Speak into one beltpack and watch its member's level move - the meters are held peaks that
   decay, so a level that has moved and fallen back is still evidence.
3. **Each endpoint hears its line.** Say something into one member and listen on another. The
   appliance deliberately does not use its own `commissioning_loopback` sink to prove this: it
   subscribes the sink to our own source, and the appliance does not receive its own multicast
   (measured on the Pi with tcpdump), so that path exercises configuration rather than audio.
   For a check without people, `POST /api/lines/{id}/tone` puts a tone on that line's RAVENNA
   output channels, and whoever is subscribed to the stream carrying them hears it.
4. **The call signal crosses.** A beltpack's *call* is a 20 kHz tone summed into its talk
   channel, so it rides the ordinary audio path: if the other members of the line hear the
   call, the path is transparent end to end. (A plain sum passes it; an auto-mix bus attenuates
   it - that family is on the roadmap, not in this build.)
5. **The self-test is green**, and its per-line and per-member detail names anyone who is not
   arriving: `POST /api/system/self-test`. This is the check to run again after any change, and
   the one to quote when the site asks whether the appliance is healthy.

Record the result: what was tested, by whom, with which endpoints and channels, and the latency
figure if one was measured. The evidence the 5422A decision needs - 32 channels per direction,
the lines separating exactly as configured, call tones reaching the other members, a
local-path latency figure with its method, and a soak with zero xruns, with the 5422A still
wired for comparison - is the site's acceptance run, not part of this procedure.

## When it does not work: symptom -> where to look

| Symptom | First look |
| --- | --- |
| Nothing anywhere | PTP `locked`; `GET /api/status` -> `audio.state` and `audio.detail` (the ALSA substream must be `RUNNING` with a moving `hw_ptr`); is the daemon reachable |
| One endpoint silent | That member's `arriving`/`silent`, its `talk_channel` binding, then the endpoint itself (AES67 mode enabled? flow multicast? does Dante Controller show the subscription?) |
| The appliance hears nothing from an endpoint | That endpoint's **sink**: is an SDP configured for it (`endpoints[].aes67.remote_source_id`/`remote_sdp`, or a line's `aes67.sdp_source` where a line is what carries it - `pasted`/`discovered` mean the appliance manages the sink, `unmanaged` means none is configured and the daemon sink may be wired by hand, `external` means the daemon reports a subscription that is not ours, `loopback` is the bench aid, `none` means there is no sink at all), then the endpoint's own talk flow (AES67 mode? multicast? subscription visible in Dante Controller?) |
| An endpoint hears nothing | That endpoint's **source** (our stream towards it): does the daemon have it (`GET /api/aes67/sources`), is it announced over SAP, and is the endpoint's receive channel actually subscribed to it? This is the appliance's *source* side - its sink state says nothing about it |
| A receiver refuses our stream | Grandmaster/domain mismatch (step 4); and leave `aes67.refclk_ptp_traceable: false` unless the receiver insists on the RFC 7273 `traceable` keyword |
| Registration fails | `sip.accounts[].code`/`error`: `401` credentials, `403` refused by the PBX, `408` no answer, `502` the registrar did not resolve. A registrar must carry its scheme (`sip:10.10.50.2`) |
| `config saved in place: the atomic replace ... is not permitted (systemd ReadWritePaths)` in the journal | Not a failure: the save succeeded, and the unit's `ReadWritePaths` only permits in-place writes to `/etc/aes67-sip.conf` |
| `cannot write config file '<path>'` (HTTP `500`) | The save did **not** happen - neither the file nor the running configuration changed. Fix the path's permissions or the unit's `ReadWritePaths` and save again |
| A party line goes silent after an edit | A configuration save applies it and replaces each line's account, so a call that is up is dropped: make changes when nothing is on air |

## What this family cannot do (so nobody promises it)

- **Mic kill ("talk off") will not cross.** It is a Dante/STcontroller control command, not
  AES67 RTP; a beltpack's mic kill needs a Dante-aware peer. *Call* does cross - it is in-band.
- **The beltpack does no mix-minus of its own**: it generates sidetone locally and expects its
  listen feed to have had its own talk removed. The appliance's matrix is what produces that,
  per listen channel.
- **A beltpack sums its two listen channels in hardware**, so those two channels are at most two
  memberships: one person is on at most two party lines, fixed by subscription.
- **Auto-mix is not in this build.** A party line is a plain sum with per-member contribution
  levels; until the NOM gain-sharing family lands, a line with many open microphones sums their
  room noise.

Reusable parts for other families belong here as they are learned. The appliance-side steps -
declare the shape, bind the members, verify the clock, read the self-test - do not change with
the device.

