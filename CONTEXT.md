# aes67-sip

The gateway that bridges a production site's AES67 intercom audio to SIP, so site
panels can talk to an off-site FreePBX conference — and, in future, to each other
on site without leaving the site.

## Language

**Site**:
The production location whose intercom devices share an AES67 VLAN. "Local" means
"on the site", as opposed to the off-site side of the link.
_Avoid_: local environment, venue, LAN

**Beltpack**:
The site's intercom endpoint: a worn, PoE-powered device with **two talk and two
listen channels**. It transmits both talk channels and sums its listen channels
inside the device for the headset, so what it hears has been mixed for it — and
because it has only those two channels, **one person is on at most two party lines
at once**, fixed by subscription rather than chosen at runtime.
_Avoid_: panel, endpoint, station, intercom

**Channel**:
One channel of the gateway's RAVENNA audio device: the unit a daemon sink or source
is mapped onto.
_Avoid_: port, lane, bus

**Stream**:
One AES67/RTP flow in one direction, described by an SDP. The gateway either
transmits it as a source or receives it as a sink; a panel has one in each
direction.
_Avoid_: feed, flow, connection

**Sink**:
The daemon object that subscribes the gateway to a remote stream — what the gateway
receives.
_Avoid_: input, receiver, subscription

**Source**:
The daemon object that publishes one of the gateway's streams — what the gateway
transmits.
_Avoid_: output, sender

**Line**:
The gateway's routing unit: a set of channels bound to one SIP account and one call.
A line is configuration for the SIP-facing path, not a participant in the matrix.
_Avoid_: circuit, trunk, channel

**Matrix**:
The set of party lines the appliance runs, with the conference as one more
participant. It is the only mixing point on the site, so beltpacks talk to each
other without the audio leaving the site.
_Avoid_: mixer, bridge, switch

**Participant**:
Anything the matrix routes to or from: a beltpack, or the conference. Each
participant has an outgoing side — what it contributes — and an incoming side —
what it hears; a beltpack has two channels on each side.
_Avoid_: node, endpoint, member

**Route**:
How a party line's membership is expressed in configuration: one participant's
outgoing stream contributing to another participant's mix, carrying a contribution
level and mute. Operators think in party lines; the appliance routes.
_Avoid_: crosspoint, connection, link

**Party line**:
A named bus with a set of members — the unit the site actually runs, as in "PL 1".
Every member hears every other member with their own audio excluded, and each
member's contribution has its own level. A beltpack's two talk and two listen
channels are how one person joins up to two party lines.
_Avoid_: group, channel, bus (as a bare noun), conference

**Auto-mix**:
The gain-sharing rule that attenuates members who are not currently talking, so a
party line with many open microphones stays usable instead of summing every
member's room noise. Historically called NOM, "number of open microphones".
_Avoid_: gating, ducking, AGC

**IFB**:
Interruptible foldback: a listen-only feed, such as a presenter's earpiece, that a
director can interrupt with their own voice.
_Avoid_: foldback, cue, talkback

**Conference**:
The off-site FreePBX conference: the cross-site talk path, and the one participant
that is not on the site.
_Avoid_: bridge, mixer, PBX room
