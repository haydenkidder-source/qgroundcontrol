# Field radio setup: one UDP port per vehicle

## Why this matters

Each Microhard pMDDL2450 radio bridges its vehicle's serial MAVLink stream onto the mesh as
UDP traffic. The bridge chunks bytes into UDP datagrams by a timer/buffer threshold, **not** by
MAVLink message boundary. If two or more radios are configured to forward to the *same*
destination port at the ground station, their byte streams land on one shared UDP socket, and a
single datagram can contain the tail of one vehicle's message followed by the start of another's.
QGC's parser then tries to stitch those together and produces malformed/corrupted packets - even
though the radio itself reports zero drops, because the corruption happens downstream of
successful RF delivery, at the socket-multiplexing layer.

Giving each vehicle's radio its own destination port isolates the streams into independent
sockets, so they can never interleave. This was field-validated on 2026-09-22: before the port
split, malformed packets appeared consistently; after it, a ~19-minute test showed zero
corruption. See `custom/test/LINK_LOSS_MESH_FINDINGS.md` for the related (separate) investigation
into command-timeout behavior on this same mesh, which also flagged this exact risk in
`src/Comms/UDPLink.cc` before it was confirmed in the field.

## Required setup

1. **On each Microhard radio**, set its serial-to-IP forward destination to the *same* ground
   station IP, but a **distinct** destination port per vehicle. Do not let two radios share a
   destination port, even temporarily during testing.
2. **In QGC**, add one UDP comm link per vehicle, each listening on its assigned port. Do not
   rely on a single UDP link (or autoconnect) to receive multiple vehicles' traffic on one port.
3. Vehicle identification (which sysid is Rover/Stallion/Hex) is handled separately via the
   Vehicle Roles page (Analyze > Vehicle Roles) - port assignment is purely to prevent stream
   corruption and is independent of that sysid mapping. Reassigning a radio's port does not
   require touching the Vehicle Roles mapping, and vice versa.

## Current port assignments

Tracked internally, not in this repo - actual sysid/port/link-name assignments for the fleet are
security-sensitive network configuration and shouldn't be published. Keep that mapping in an
internal-only document or password manager instead.

## Pre-flight checklist

- [ ] Confirm each radio's serial-to-IP destination port matches your internal assignment record,
      not a shared default.
- [ ] Confirm QGC has one UDP link per vehicle, each bound to the matching port, before
      connecting.
- [ ] If autoconnect is enabled, confirm it isn't also listening on one of these ports (avoid
      a second listener colliding with a manually configured link on the same port).
- [ ] After connecting all vehicles, check Analyze > App Log Viewer (or Settings > App Log
      Viewer) briefly for `malformed`/CRC/parse-related warnings - there should be none. Their
      presence again would indicate a port got reused/misconfigured.
- [ ] If a radio is swapped or reconfigured, re-verify its destination port before flying -
      nothing in QGC enforces that ports stay unique across radios.
