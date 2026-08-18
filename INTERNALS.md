# rnode-ble — internals

Maintainer reference. The [README](README.md) is the operator guide; this is for
changing the code without breaking it. It is self-authoritative.

```
Sideband / RNS RNodeInterface (central)      this device (peripheral)
  |                                            advertising, connectable,
  |                                            name "RNode xxxx"
  |  ---- connect (to a BONDED address) ----->
  |  ---- discover 6e400001-…-e50e24dcca9e -->
  |  ---- write CCCD on …0003 (subscribe) --->  → itsConnect(lora:0x524E)
  |  ---- exchange MTU, requests 512 -------->
  |         (the client goes online ONLY after the MTU exchange succeeds)
  |  ---- write KISS bytes to …0002 --------->  → ITS stream → the radio
  |  <--- notify KISS bytes on …0003 --------   ← ITS stream ← the radio
  |  ---- write 0 bytes for 3 s ------------->  the client re-sends CMD_DETECT
  |  ---- write 0 bytes for 9 s ------------->  the client goes OFFLINE
```

The three rules the code exists to keep:

- **Never drop a byte in either direction.** The KISS stream has no
  resynchronisation point short of the client's 9 s watchdog. Back-pressure is
  carried, never discarded.
- **The endpoint's session policy is the only one.** This straddle asks; a
  refusal is final for that attempt and the client's own retry is the recovery.
- **Nothing but a copy runs in NimBLE host context.**

## 1. What this straddle adds

One source file, `esp-idf/src/rnode_ble.cpp`, plus a two-line header declaring
its `Service`. Nothing is forked or vendored. It contributes:

- **A Nordic UART Service** as a `static const ble_gatt_svc_def[]`, queued with
  `bleGattAdd()` from `onInit()` — RX `…6e400002` (write / write-without-
  response, encrypted), TX `…6e400003` (notify, encrypted read).
- **One FreeRTOS task**, `rnode-ble`, priority 1 on `CORE_PRIMARY`, PSRAM stack,
  running the canonical ITS loop.
- **A session** on `RNODE_ITS_PORT` (`include/rnode_door.h` in
  [iface-lora](../iface-lora)) opened with a 12-byte `rnode_door_connect_t`.
- **An inbound ring** (2 KB stream buffer) fed by the GATT write callback in
  host context and drained on the task, plus a 512-byte carry for what ITS would
  not take.
- **An outbound path** chunked to `attMtu - 3`, with a 512-byte carry for a
  chunk NimBLE had no buffer for and `bleNotifyArm()` as the wake.
- **The `rnode-ble` CLI verb** and the `ble.rnode.*` readouts.
- **A settings row** contributed into iface-lora's existing `RNode endpoint`
  section — the label must match that string exactly, because sections merge on
  exact match and a near-miss silently becomes a second heading.

## 2. Tasks

| Task | What it does here |
|---|---|
| NimBLE host | `gattAccess` on an RX write: flatten the ATT value, push it into the ring, notify. Nothing else. |
| `rnode-ble` (prio 1) | Everything: the spangap-ble events, the ITS session, both pumps, the reconcile. |
| `lora` | The endpoint at the far end of the ITS handle. |

`bleRegister` is called from the door task, so every `BLE_EV_*` handler runs
under this task's own `itsPoll`. The task's only wait point is that `itsPoll`,
woken by the ring's notify, an ITS recv, a config change, or a spangap-ble
event. It parks forever unless an inbound carry is pending or a notify is
blocked, in which case it polls at 20 ms — a carry clears when the endpoint
drains, which produces no notification on this side, and a blocked notify may
have no completion coming (§5).

## 3. Why the subscribe is the trigger

Verified against `RNS/Interfaces/Android/RNodeInterface.py`, `class
BLEConnection`: the client's order is connect → discover services → enable
notifications (CCCD) → request MTU → and `connected = True` is set **only**
inside `on_mtu_changed`, on success. No KISS byte flows before that.

So `BLE_EV_SUBSCRIBE` on our TX value handle is both the earliest and the last
quiet moment to open the endpoint session: earlier there is nothing to attach
to, later the first bytes would arrive with nowhere to go. A refusal (a serial
or TCP client holds the session, or the endpoint is off) drops the BLE
connection, and the client re-dials on its own `RECONNECT_WAIT` of 2.5 s until
the endpoint frees.

A second client that subscribes while a session is attached is dropped here as
well as by the endpoint, so the first session is not disturbed by the attempt.

## 4. The client's two hard edges

**MTU failure is fatal and permanent.** On a connect timeout with an MTU request
outstanding the client appends `ERROR_INVALID_BLE_MTU (0x20)`, sets
`should_run = False` and `awaiting_ble_reset = True`, and its connection job
stops entirely until Reticulum is restarted. NimBLE answers every ATT MTU
exchange automatically with `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`, which
[spangap-ble](../spangap-ble) sets to 517, so this can only fire if that number
is moved down near 23. There is nothing for us to do about it at runtime except
log the MTU we got.

**The watchdog is transport-independent.** `readLoop` gives 3 s with no inbound
byte before re-sending `CMD_DETECT`, and 9 s before raising `IOError` and taking
the interface offline (`PORT_IO_TIMEOUT = 3`). The endpoint answers every detect
statelessly, so this is satisfied by keeping notification latency well under a
second — which is why the connection interval is requested at 15–30 ms
(`bleConnParams(conn, 12, 24, 0, 4000)`) as soon as a peripheral connection
lands. A sleepy interval would kill sessions that are otherwise perfectly
healthy.

`CONNECT_TIMEOUT = 7.0`, `RECONNECT_WAIT = 2.5`, `MTU_TIMEOUT = 4.0` are the
client's other constants worth knowing when reading a log.

## 5. Back-pressure, both directions

**Inbound (ATT write → ITS).** `gattAccess` refuses a write with
`BLE_ATT_ERR_INSUFFICIENT_RES` when the whole chunk will not fit the ring: a
partial copy would tear the stream, and the client's own ATT retry carries it.
On the task, `pumpInbound` sends the carry first and **returns without reading
the ring** if it did not fully drain — that unread ring is the back-pressure,
and it is the same shape spangap-core's serial pump uses. Stream-mode `itsSend`
has FreeRTOS stream-buffer semantics: a short write on timeout leaves the
accepted bytes in the ring with no rollback, so the remainder must be carried.

The carry is sized to the **largest ATT write** (512), not to the negotiated
MTU. The client's own chunk to RX is `min(mtu - 5, 512)`, so 512 is the ceiling;
a carry smaller than one read would truncate mid-stream.

**Outbound (ITS → notify).** `itsRecv` into chunks of `attMtu - 3`, one
`ble_gatts_notify_custom` per chunk. When NimBLE has no buffer the call fails —
and **consumes the mbuf regardless**, so the bytes are already gone from the ITS
ring and cannot simply be left there. They go into `s_txCarry`, `s_notifyBlocked`
stops the pump, and `bleNotifyArm(conn)` asks for one `BLE_EV_NOTIFY_TX`. The
carry is re-sent first when that arrives. Nothing is dropped, and the ITS ring
fills behind it, which is what reaches rnsd as back-pressure.

`BLE_EV_NOTIFY_TX` only fires when a notification *completes* on this
connection. A refusal can also happen with nothing in flight — the shared msys
pool drained by another consumer's traffic — and then no completion is coming,
so the arm is a fast path and never the only one: while blocked, the task also
retries on its own 20 ms tick.

If sustained throughput ever needs more notify buffers the knob is
`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` — measure before touching it.

## 6. Encrypted, not authenticated

The RX and TX characteristics carry `BLE_GATT_CHR_F_WRITE_ENC` /
`BLE_GATT_CHR_F_READ_ENC` and **not** the `_AUTHEN` variants.
[spangap-ble](../spangap-ble)'s policy is Just Works inside a timed pairing
window, which produces an *unauthenticated* bond; an `_AUTHEN` flag would reject
every write from a peer that paired exactly as intended. RNS itself requires
only that the device be bonded, not that the bond was authenticated.

RNode's own firmware marks these characteristics encrypted *and* MITM and pairs
with a displayed passkey. Matching that would mean spangap-ble growing a
passkey-display path and a dependency on there being a screen; the timed window
is what stands in for it.

The bond's real weakness is the adapter address: spangap-ble presents a fresh
random address every Bluetooth start and after every disconnect, and the
client connects strictly by bonded address, so a bond points nowhere almost
immediately.
Until spangap-ble implements resolvable-private-address privacy — an identity
resolving key distributed at bonding, letting bonded peers resolve every new
address — this door cannot keep a client, and the pairing flow in the README
holds only until the next rotation.

## 7. The advertised name

`RNode %02X%02X` from the last two bytes of the adapter address, built once
the address is known and cached — the address is random per Bluetooth start
and rotates on every disconnect, so the suffix matches only the address the
name was born under, and changes across restarts. The client prefix-matches
`"rnode "` case-insensitively when no `ble_name` is configured, so everything
after the space is cosmetic — it exists so two devices are distinguishable in
a phone's pairing list.

The service UUID goes in the advertisement and the name in the scan response: 31
bytes will not hold a 128-bit UUID and a name together. The advertisement is
released while a session is attached and re-claimed on disconnect.

## 8. Pitfalls

- **`NUS_SVCS` must stay `static const`.** `bleGattAdd` records the pointer and
  dereferences it at host bring-up, arbitrarily later.
- **`ble_gatts_notify_custom` frees its mbuf on failure too.** Any rewrite of
  the outbound pump that assumes otherwise loses a chunk and takes the client
  offline nine seconds later.
- **Do not widen `gattAccess`.** It runs on the NimBLE host task. Copy into the
  ring, notify, return — no storage, no ITS, no logging.
- **Do not add a second session policy.** `maxHandles = 1` on the endpoint's
  port plus its own reject is the whole thing. A check here that disagreed with
  it would produce a door that refuses a client the endpoint would have taken.
- **The connect payload is 12 bytes with a magic, and that is load-bearing.**
  Length is the endpoint's only transport discriminator, and `net_connect_t` is
  8 bytes in an IPv6-off build — the size of the natural
  `{ magic, addr[6], type }` struct. See
  [iface-lora INTERNALS §17.2](../iface-lora/INTERNALS.md).
- **A clean detach must leave the radio up.** The endpoint's
  `onRnodeDisconnect` clears its deferred radio-off for exactly this reason, so
  a phone walking out of range does not take rnsd's radio with it. Closing the
  ITS handle is all this straddle should ever do.
