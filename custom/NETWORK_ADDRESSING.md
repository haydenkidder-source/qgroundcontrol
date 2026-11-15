# Network addressing: one flat network

## Network layout

The ground control station (GCS) and all vehicles share the private Internet Protocol (IP) network
`10.0.0.0/16`. Every device needs a static IP address and netmask `255.255.0.0`. Leave the gateway
empty. There is no router, gateway or Dynamic Host Configuration Protocol (DHCP) address service.
Turn DHCP off on the radios.

The Microhard pMDDL2450 radios form a bridged mesh: they act as plain Ethernet links. Every device
is on the same Layer 2 network, meaning devices communicate directly over those links without
a router between units.

An address has the form `10.0.N.R`. Each dot-separated number is an octet:

- The third octet, `N`, is the **unit number**, identifying a station or vehicle. Unit 1 is the
  GCS; vehicles use units 2 and up. Use the recommended range 1-254.
- The fourth octet, `R`, identifies the device's role within that unit.

Unit numbers are a naming convention only. They do not create separate subnets: all units use
the same `/16` netmask.

## Device roles

Use the same role numbers on every unit. Assign a unique IP address to every device.

| Fourth octet | Role |
| ------------ | ---- |
| `.1` | Microhard radio |
| `.2` | Companion computer (Raspberry Pi) |
| `.3` to `.9` | Reserved for other network gear |
| `.100` and up | Operator laptops and test devices |

The GCS unmanaged switch is planned as `.5` (`10.0.1.5`); this is a reserved assignment, not a
claim that it is configured. Apply the same netmask to any switch management address.

Generic examples:

- GCS radio: `10.0.1.1`.
- GCS companion computer: `10.0.1.2`.
- Operator laptop: `10.0.1.100`.
- Vehicle in unit `N`: radio `10.0.N.1`, companion computer `10.0.N.2`.

## Per-vehicle ports

A port selects a receiving service at an IP address. User Datagram Protocol (UDP) carries both
video and MAVLink, the vehicle telemetry and command protocol, to the GCS on separate ports.
For each vehicle, derive the destination ports from its unit number `N`:

- **Video:** UDP `5600 + N` to the GCS companion computer at `10.0.1.2`.
- **MAVLink:** UDP `14550 + N` to the IP address of the computer running QGroundControl (QGC),
  for example the operator laptop in the generic list above. The existing
  [Field radio setup](FIELD_RADIO_SETUP.md#required-setup) calls this the ground station IP.
  This is separate from the video relay destination.

Unit 1 is the GCS, not a vehicle. No vehicle uses `5601` or `14551`.
No two vehicles may ever share a destination port, even temporarily during testing.

The following table is a **worked example only**, not a fleet assignment record.

| Example vehicle | Unit | Radio IP | Pi IP | Video UDP port | MAVLink UDP port |
| --------------- | ---- | -------- | ----- | -------------- | ---------------- |
| Vehicle A | 2 | `10.0.2.1` | `10.0.2.2` | 5602 | 14552 |
| Vehicle B | 3 | `10.0.3.1` | `10.0.3.2` | 5603 | 14553 |
| Vehicle C | 4 | `10.0.4.1` | `10.0.4.2` | 5604 | 14554 |

Set each vehicle radio's serial MAVLink forward destination to the GCS IP and its own MAVLink
port. In QGC, create one UDP communication link per vehicle, listening on that port. Follow
[Field radio setup](FIELD_RADIO_SETUP.md#required-setup) for the procedure and port-isolation rule.
See [Video relay setup](VIDEO_RELAY_SETUP.md) for the companion computers' video configuration.
Keep real fleet port, system identifier (sysid) and link-name assignments in an internal-only
record, as described in [Current port assignments](FIELD_RADIO_SETUP.md#current-port-assignments).

## Changing addresses safely

Radios of the same model commonly ship with the same factory address, so check each radio's
address before connecting a second one. This is unconfirmed for this hardware. Readdress one
radio at a time to avoid duplicate IPs and losing access to the wrong device.

1. Connect to one radio at its current address.
2. Add a second address in the new range to the connected Pi or laptop before changing the
   radio. Keep the old address while making the change. Use `255.255.0.0` for the new address
   and leave its gateway empty.
3. Set the radio's unique static address and netmask `255.255.0.0`, leave its gateway empty,
   and turn DHCP off.
4. Reconnect to the radio at its new address. Confirm access before removing the old address
   from the Pi or laptop.
5. Repeat for the next radio. Check the addresses and masks on every Pi, switch management
   interface and laptop as well.
6. From the operator laptop, run a **ping sweep**: ping every device in the internal address
   record, not just one radio. Every device must respond. This is the acceptance check after
   addressing changes.

## Troubleshooting

- **Pings fail or radio web pages are unreachable:** check for a `/24` mask (`255.255.255.0`).
  A device at `10.0.2.x` with that mask treats `10.0.1.x` as off-network and tries to send it
  through a gateway that does not route it. Set `255.255.0.0` on every radio, Pi, switch
  management interface and laptop, and leave the gateway empty.
- **Access changes when another radio powers on:** check for duplicate IP addresses. Radios
  of the same model commonly ship with the same factory address, so check each radio's address
  before connecting a second one; connect and readdress them one at a time.
- **Unexpected address changes:** check for two DHCP servers. Turn DHCP off on the radios;
  every device on this network must use a static address.

Repeat the laptop ping sweep after correcting any addressing problem.
