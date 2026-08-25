# rnode-ble

```
s.lora.rnode.ble = 0?    yes → nothing advertises, nothing attaches. Stop.
                               (and, with nothing else asking, no radio)
 ↓ no
bleUp() + reserve 1 connection + claim the advertisement
 ↓                       advertising as "RNode xxxx", connectable
the phone is BONDED?     no  → the RNS client will not even look at it. Pair
 ↓ yes                         it first, inside `ble pair 60`.
client connects, discovers the Nordic UART Service, writes the CCCD
 ↓
itsConnect("lora", RNODE_ITS_PORT)
 ↓ refused (a serial or TCP client holds the session, or the endpoint is off)
 ↓   → drop the BLE connection; the client re-dials every 2.5 s
attached                 → KISS bytes both ways, the same stream USB carries
 ↓
client disconnects       → itsDisconnect; the radio stays up for rnsd
```

**rnode-ble** is a Bluetooth door onto the RNode endpoint
[iface-lora](../iface-lora) already has. A phone running Sideband — or any RNS
`RNodeInterface` — attaches to this device over Bluetooth as if it were RNode
hardware, and gets exactly the stream the USB and TCP doors carry.

There is no KISS code here, no radio knowledge and no second session policy.
This straddle moves bytes onto `RNODE_ITS_PORT` and does nothing else.

**Status: never exercised against a real client.**
[spangap-ble](../spangap-ble)'s adapter address rotates as a resolvable
private address over a persisted identity resolving key: the phone receives
the key at bonding and resolves every rotation (and every reboot) back to one
identity, which is exactly what the RNS client's connect-to-the-bonded-address
behaviour needs. What remains is the acceptance test from
[plans/ble.md](../plans/ble.md): a bonded phone staying attached, or
reattaching unaided, across a rotation and across a reboot.

## Origins

The wire is RNode firmware's own: the **Nordic UART Service**
(`6e400001-b5a3-f393-e0a9-e50e24dcca9e`), which every ESP32-S3 target in RNode's
firmware runs, with the KISS stream as its payload and no framing of its own.
RNode's classic-Bluetooth serial path is ESP32-original only and does not exist
on an S3. None of RNode's code is reused — only the protocol it speaks.

## What it does

One FreeRTOS task at priority 1. At boot it queues the Nordic UART Service with
[spangap-ble](../spangap-ble); when enabled it asks for the radio, reserves one
connection, and claims a slice of the advertising instance with the name
`RNode xxxx` (the last two bytes of the adapter address, which the client only
ever prefix-matches — the suffix is there to tell two devices apart in a phone's
pairing list).

A session opens on the **CCCD subscribe**, not on the first byte. The client's
order is connect → discover → subscribe → request MTU → and only then does it
consider itself online and start writing, so opening the endpoint session on the
subscribe has it ready before the first KISS byte arrives.

While a session is attached the advertisement is released: there is one RNode
and one session, so a second client has nothing to find.

**The RNS client only ever considers bonded devices.** It enumerates the phone's
paired devices and matches a name beginning `rnode ` case-insensitively; it
never scans. So pair the phone first — the advertised name matters at pairing
time, and after that only the address does.

### Starts automatically — and only when needed

When `rnode-ble` is in the build it starts on its own; there is no init call to
make. It `requires:` both [iface-lora](../iface-lora) and
[spangap-ble](../spangap-ble), so the endpoint's ITS port and the GATT registry
both exist before its own `onInit()` runs.

## Setting it up

1. Put both straddles in the build:
   `spangap build --with spangap/spangap-ble --with reticulous/rnode-ble`.
   Bluetooth (`s.lora.rnode.ble`) is on by default — a transport nobody has
   paired with reaches nobody.
2. Open the pairing window: **Reticulum Mesh → RNode → Pair for 60 s**, or
   `ble pair 60`. That switch is also what turns the Bluetooth RADIO on: the
   radio has no switch of its own and runs while something asks for it.
3. Pair from the phone's Bluetooth settings inside that window. Pairing is Just
   Works — no passkey to compare.
4. Point an RNS `RNodeInterface` at it. With no `ble_name` or `ble_addr` the
   client matches any bonded device whose name starts with `rnode `:

```ini
[[Device RNode]]
  type = RNodeInterface
  interface_enabled = true
  port = ble
  frequency = 867200000
  bandwidth = 125000
  txpower = 17
  spreadingfactor = 8
  codingrate = 5
```

`rnode-ble` confirms the state, and the radio settings the client sends are
executed by writing the ordinary `s.lora.<n>.*` keys — see
[iface-lora's README](../iface-lora/README.md), including its warning that they
persist.

## Storage variables

### Settings (persisted, `s.`)

| Key | Default | Meaning |
|---|---|---|
| `s.lora.rnode.ble` | `1` | Attach over Bluetooth. Live. The key sits beside the endpoint's other transports (`s.lora.rnode.serial` / `.tcp`) — one namespace for one endpoint, each its own switch — and this straddle owns and reads it; the settings row sits in iface-lora's **RNode** menu under "Connect via", where an operator looks for it. It is also the Bluetooth radio's switch as far as a user is concerned: spangap-ble has none of its own and runs while a consumer asks for it. |
| `s.ble.rnode.txpower` | `9` | Transmit power in dBm for this endpoint's own links (`bleTxPower`). In the `s.ble.*` namespace so an operator sees one Bluetooth namespace whatever pane the row is on. Live. |

The endpoint itself — which radio it exposes, whether it is on at all — is
[iface-lora](../iface-lora)'s `s.lora.rnode.*`. The radio, the pairing window
and the bond store are [spangap-ble](../spangap-ble)'s; this straddle's pane is
where they are reached, because a phone pairs with THIS.

### Runtime readouts (ephemeral)

| Key | Meaning |
|---|---|
| `ble.rnode.state_text` | `disabled`, `waiting for Bluetooth`, `advertising`, `connected`, `attached` |
| `ble.rnode.up` | `1` while a session holds the endpoint |
| `ble.rnode.enabled` | `1` while `s.lora.rnode.ble` is on — the gate the pane's Bluetooth settings hang on, published rather than read off the config key |
| `ble.rnode.name` | The advertised name, once the adapter address is known |
| `ble.rnode.traffic` | Bytes each way and sessions opened, as one finished line |

### Command sentinels

None.

### Secrets

None.

## CLI

| Command | What it does |
|---|---|
| `rnode-ble` | Status: state, advertised name, negotiated MTU, sessions opened and refused, bytes each way, and either carry if one is pending |

Pairing and bonds are `ble pair` / `ble bonds` / `ble forget`, in
[spangap-ble](../spangap-ble).

## When it will not attach

- **`rnode-ble` says `advertising` and the client never connects** — the phone
  is not bonded. `ble bonds`, then `ble pair 60` and pair again.
- **`sessions … refused`** — a USB or TCP client already holds the endpoint.
  There is one RNode and the session is first come, first served across every
  transport.
- **The client goes offline every few seconds** — the radio parameters in the
  interface config do not match what the radio can do. The client compares the
  configuration echo unconditionally and re-dials on a mismatch; see
  [iface-lora's INTERNALS §17.4](../iface-lora/INTERNALS.md).

## Read next

- [INTERNALS.md](INTERNALS.md) — the wire, the client's watchdog and its
  hard MTU failure, and the back-pressure rule in both directions.
