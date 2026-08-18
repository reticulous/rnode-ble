/**
 * rnode_ble — the RNode endpoint's Bluetooth door.
 *
 *   Sideband / RNS RNodeInterface (central)      this device (peripheral)
 *     |                                           advertising, connectable,
 *     |                                           name "RNode xxxx"
 *     |  ---- connect (to a BONDED address) ----->
 *     |  ---- discover 6e400001-…-e50e24dcca9e -->
 *     |  ---- write CCCD on …0003 (subscribe) --->  → itsConnect(lora:0x524E)
 *     |  ---- exchange MTU, requests 512 -------->
 *     |  ---- write KISS bytes to …0002 --------->  → ITS stream
 *     |  <--- notify KISS bytes on …0003 --------   ← ITS stream
 *
 * WHY SUBSCRIBE IS THE TRIGGER. The client's order is connect → discover →
 * subscribe → request MTU → and only then does it consider itself online and
 * start writing. Opening the endpoint session on the CCCD write therefore has
 * it ready before the first KISS byte, and costs nothing if the client never
 * gets that far.
 *
 * THE CLIENT ONLY EVER CONSIDERS BONDED DEVICES. It enumerates the phone's
 * paired devices and matches a name beginning "rnode " case-insensitively — it
 * never scans. So the advertised name matters at pairing time, and after that
 * only the address does, which is why the round-robin in spangap-ble is
 * harmless here: a bonded client's connect lands during whichever slot is up.
 *
 * BACK-PRESSURE, BOTH WAYS, AND NEVER A DROP. The KISS stream has no
 * resynchronisation point short of the client's 9 s watchdog, so a byte dropped
 * in either direction costs a whole session:
 *
 *   - inbound (RX write → ITS): stream-mode itsSend can write short on
 *     timeout, so the remainder is carried and re-sent first on the next pass,
 *     and while a carry is pending nothing new is taken out of the ring.
 *   - outbound (ITS → notify): ble_gatts_notify_custom returns BLE_HS_ENOMEM
 *     when NimBLE's buffers are full. We stop calling itsRecv and let the bytes
 *     sit in the ITS ring — that ring IS the back-pressure — and arm
 *     bleNotifyArm() to be woken when a buffer frees.
 *
 * ENCRYPTED, NOT AUTHENTICATED. The characteristics carry the _ENC access
 * flags and not the _AUTHEN ones. RNS asks only that the device be bonded, and
 * spangap-ble pairs Just Works inside a timed window, which produces an
 * UNauthenticated bond — an _AUTHEN flag would reject every write from a peer
 * that paired exactly as intended.
 */
#include "rnode_ble.h"

#include "ble.h"
#include "rnode_door.h"
#include "spangap.h"

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "rnode-ble";

/* Nordic UART Service, as the RNS client discovers it. */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);   /* 6e400001-…-e50e24dcca9e */
static const ble_uuid128_t NUS_RX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);   /* host → device, write */
static const ble_uuid128_t NUS_TX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);   /* device → host, notify */

/* The largest ATT write the client makes is min(mtu-5, 512), so the inbound
 * ring and the carry are sized to the ceiling of that and never to the
 * negotiated MTU — the carry must be at least one whole write or a chunk
 * truncates mid-stream. */
#define DOOR_MAX_WRITE   512
#define DOOR_RX_RING     2048
#define DOOR_TX_CHUNK    512

static uint16_t s_rxHandle = 0;   /* filled by NimBLE at GATT registration */
static uint16_t s_txHandle = 0;

static int gattAccess(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt* ctxt, void* arg);

static const struct ble_gatt_chr_def NUS_CHRS[] = {
    {
        .uuid       = &NUS_RX.u,
        .access_cb  = gattAccess,
        .arg        = nullptr,
        .descriptors = nullptr,
        /* WRITE_NO_RSP as well as WRITE: the client picks whichever the
         * characteristic offers, and the unacknowledged form is what keeps a
         * KISS burst from paying a round trip per chunk. */
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                      BLE_GATT_CHR_F_WRITE_ENC,
        .min_key_size = 0,
        .val_handle = &s_rxHandle,
        .cpfd       = nullptr,
    },
    {
        .uuid       = &NUS_TX.u,
        .access_cb  = gattAccess,
        .arg        = nullptr,
        .descriptors = nullptr,
        .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .min_key_size = 0,
        .val_handle = &s_txHandle,
        .cpfd       = nullptr,
    },
    {},
};

static const struct ble_gatt_svc_def NUS_SVCS[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &NUS_SVC.u,
        .includes        = nullptr,
        .characteristics = NUS_CHRS,
    },
    {},
};

/* ─────────────── state (door task, unless noted) ─────────────── */

static TaskHandle_t     s_task    = nullptr;
static StreamBufferHandle_t s_rxRing = nullptr;   /* host cb → door task */

static bool     s_enabled  = false;
static int      s_advSlot  = -1;
static uint16_t s_conn     = BLE_HS_CONN_HANDLE_NONE;   /* the subscribed central */
static uint16_t s_mtu      = 23;
static int      s_its      = -1;                        /* lora:RNODE_ITS_PORT */
static char     s_advName[BLE_ADV_NAME_MAX] = "";

/* Inbound carry: what itsSend would not take last pass. */
static uint8_t  s_carry[DOOR_MAX_WRITE];
static size_t   s_carryLen = 0;

/* Outbound carry: the chunk a refused notification took out of the ITS ring.
 * ble_gatts_notify_custom consumes its mbuf on every path, success or not, so
 * bytes handed to it are gone — the chunk is held here and re-sent first. */
static uint8_t  s_txCarry[DOOR_TX_CHUNK];
static size_t   s_txCarryLen = 0;
static bool     s_notifyBlocked = false;

static uint64_t s_bytesIn = 0, s_bytesOut = 0;
static uint32_t s_sessions = 0, s_refused = 0;

static volatile bool s_configDirty = true;

static void wake(void) { if (s_task) xTaskNotifyGive(s_task); }

/* ─────────────── publish ─────────────── */

static const char* stateWord(void) {
    if (!s_enabled)                            return "disabled";
    if (s_its >= 0)                            return "attached";
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)     return "connected";
    if (s_advSlot >= 0)                        return "advertising";
    return "waiting for Bluetooth";
}

static void publish(void) {
    char buf[96];
    storageBegin();
    storageSet("ble.rnode.state_text", stateWord());
    storageSet("ble.rnode.up", s_its >= 0 ? 1 : 0);
    if (s_advName[0]) storageSet("ble.rnode.name", s_advName);
    snprintf(buf, sizeof(buf), "in %u B \xC2\xB7 out %u B \xC2\xB7 %u session%s",
             (unsigned)(s_bytesIn & 0x7fffffff), (unsigned)(s_bytesOut & 0x7fffffff),
             (unsigned)s_sessions, s_sessions == 1 ? "" : "s");
    storageSet("ble.rnode.traffic", buf);
    storageEnd();
}

/* ─────────────── GATT access (HOST CONTEXT) ───────────────
 *
 * Copy and post, and nothing else. No storage, no ITS, no logging, no
 * teardown — this runs on the NimBLE host task. */

static int gattAccess(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt* ctxt, void* /*arg*/) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (attr != s_rxHandle || conn != s_conn || !s_rxRing) return 0;

    uint8_t  buf[DOOR_MAX_WRITE];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0) return 0;
    if (n == 0) return 0;
    /* A short write into the ring would tear the stream, so the ring either
     * takes the whole chunk or the chunk is refused at the ATT layer and the
     * client's own retry carries it. */
    if (xStreamBufferSpacesAvailable(s_rxRing) < n) return BLE_ATT_ERR_INSUFFICIENT_RES;
    xStreamBufferSend(s_rxRing, buf, n, 0);
    if (s_task) xTaskNotifyGive(s_task);
    return 0;
}

/* ─────────────── the endpoint session ─────────────── */

static void onLoraRecv(int handle, size_t bytesAvail);
static void onLoraDisconnect(int ref);

static void sessionClose(const char* why) {
    if (s_its < 0) return;
    itsDisconnect(s_its);
    s_its = -1;
    s_carryLen = s_txCarryLen = 0;
    s_notifyBlocked = false;
    if (s_rxRing) xStreamBufferReset(s_rxRing);
    info("session closed (%s)", why);
    publish();
}

static bool sessionOpen(void) {
    if (s_its >= 0) return true;
    rnode_door_connect_t c = {};
    c.magic    = RNODE_DOOR_MAGIC;
    c.addrType = 0;
    /* The endpoint refuses when a serial or TCP client is already attached, or
     * when the endpoint itself is off. That refusal is the whole session
     * policy: there is one RNode, first come first served. */
    s_its = itsConnect("lora", RNODE_ITS_PORT, &c, sizeof c, pdMS_TO_TICKS(500),
                       0, onLoraRecv, onLoraDisconnect);
    if (s_its < 0) {
        s_refused++;
        warn("RNode endpoint refused the session (busy or disabled) — dropping "
             "the connection; the client retries every 2.5 s");
        return false;
    }
    s_sessions++;
    info("attached to the RNode endpoint (mtu %u)", (unsigned)s_mtu);
    publish();
    return true;
}

static void onLoraDisconnect(int /*ref*/) {
    s_its = -1;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) bleDisconnect(s_conn);
    publish();
}

/* ─────────────── outbound: ITS → notify ─────────────── */

/* One notification. False means NimBLE had no buffer — the caller holds the
 * bytes and stops producing; it never drops them. */
static bool notifyChunk(const uint8_t* d, size_t n) {
    struct os_mbuf* om = ble_hs_mbuf_from_flat(d, n);
    if (!om) return false;                       /* msys pool exhausted */
    return ble_gatts_notify_custom(s_conn, s_txHandle, om) == 0;
}

static void pumpOutbound(void) {
    if (s_its < 0 || s_conn == BLE_HS_CONN_HANDLE_NONE || !s_txHandle) return;
    if (s_notifyBlocked) return;

    if (s_txCarryLen) {
        if (!notifyChunk(s_txCarry, s_txCarryLen)) {
            s_notifyBlocked = true;
            bleNotifyArm(s_conn);
            return;
        }
        s_bytesOut += s_txCarryLen;
        s_txCarryLen = 0;
    }

    uint8_t buf[DOOR_TX_CHUNK];
    /* attMtu - 3 is what fits in one notification's value. */
    size_t chunk = (s_mtu > 3 ? (size_t)s_mtu - 3 : 20);
    if (chunk > sizeof(buf)) chunk = sizeof(buf);

    while (itsBytesAvailable(s_its) > 0) {
        size_t n = itsRecv(s_its, buf, chunk, 0);
        if (n == 0) break;
        if (!notifyChunk(buf, n)) {
            std::memcpy(s_txCarry, buf, n);
            s_txCarryLen    = n;
            s_notifyBlocked = true;
            bleNotifyArm(s_conn);
            return;
        }
        s_bytesOut += n;
    }
}

/* ─────────────── inbound: RX ring → ITS ─────────────── */

static void pumpInbound(void) {
    if (s_its < 0 || !s_rxRing) return;

    /* Carry first. Until it drains nothing new is taken out of the ring — that
     * unread ring is the back-pressure, and the client's own ATT flow control
     * is what it eventually reaches. */
    if (s_carryLen) {
        size_t sent = itsSend(s_its, s_carry, s_carryLen, 0);
        if (sent < s_carryLen) {
            std::memmove(s_carry, s_carry + sent, s_carryLen - sent);
            s_carryLen -= sent;
            return;
        }
        s_carryLen = 0;
    }

    uint8_t buf[DOOR_MAX_WRITE];
    for (;;) {
        size_t n = xStreamBufferReceive(s_rxRing, buf, sizeof(buf), 0);
        if (n == 0) return;
        s_bytesIn += n;
        size_t sent = itsSend(s_its, buf, n, 0);
        if (sent < n) {
            std::memcpy(s_carry, buf + sent, n - sent);
            s_carryLen = n - sent;
            return;
        }
    }
}

static void onLoraRecv(int /*handle*/, size_t /*bytesAvail*/) { pumpOutbound(); }

/* ─────────────── BLE events (door task) ─────────────── */

static void onBleUp(const ble_event_t*) {
    s_configDirty = true;
    wake();
}

static void onBleDown(const ble_event_t*) {
    s_advSlot = -1;
    s_conn    = BLE_HS_CONN_HANDLE_NONE;
    sessionClose("Bluetooth stopped");
}

static void onBleConnect(const ble_event_t* ev) {
    if (ev->central) return;                 /* not our side of the world */
    /* 15-30 ms. The client's watchdog is transport-independent: 3 s with no
     * inbound byte re-sends CMD_DETECT, 9 s takes the interface offline. A
     * sleepy interval would kill an otherwise healthy idle session. */
    bleConnParams(ev->conn, 12, 24, 0, 4000);
}

static void onBleDisconnect(const ble_event_t* ev) {
    /* A failed dial elsewhere on the radio arrives with conn none — the same
     * value s_conn holds while no session is attached, so both must be real. */
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || ev->conn != s_conn) return;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_mtu  = 23;
    /* The endpoint's own onRnodeDisconnect clears its deferred radio-off, so a
     * client that simply walked away leaves the radio up for rnsd. */
    sessionClose("client gone");
    s_configDirty = true;                    /* re-claim the advertisement */
    wake();
}

static void onBleMtu(const ble_event_t* ev) {
    if (ev->conn != s_conn) return;
    s_mtu = ev->mtu;
    pumpOutbound();
}

static void onBleSubscribe(const ble_event_t* ev) {
    if (!s_txHandle || ev->attr != s_txHandle) return;
    if (!ev->notify) {                       /* CCCD cleared */
        if (ev->conn == s_conn) sessionClose("unsubscribed");
        return;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_conn != ev->conn) {
        /* A second client while one holds the session. The endpoint would
         * refuse it anyway; refusing here keeps the first session undisturbed. */
        warn("a second client subscribed while a session is attached — dropped");
        bleDisconnect(ev->conn);
        return;
    }
    s_conn = ev->conn;
    s_mtu  = ev->mtu ? ev->mtu : bleConnMtu(ev->conn);
    if (!sessionOpen()) { bleDisconnect(ev->conn); s_conn = BLE_HS_CONN_HANDLE_NONE; return; }
    /* One client at a time: stop being discoverable while a session is up. */
    if (s_advSlot >= 0) { bleAdvRelease(s_advSlot); s_advSlot = -1; }
    publish();
}

static void onBleNotifyTx(const ble_event_t* ev) {
    if (ev->conn != s_conn) return;
    s_notifyBlocked = false;
    pumpOutbound();
}

/* ─────────────── reconcile ─────────────── */

static void applyConfig(void) {
    bool en = storageGetInt("s.ble.rnode.enable", 0) != 0;
    s_enabled = en;

    if (!en) {
        if (s_advSlot >= 0) { bleAdvRelease(s_advSlot); s_advSlot = -1; }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) { bleDisconnect(s_conn); s_conn = BLE_HS_CONN_HANDLE_NONE; }
        sessionClose("disabled");
        bleSlotRelease(TAG);
        bleDown(TAG);
        publish();
        return;
    }

    bleSlotReserve(TAG, 1);
    bleUp(TAG);
    if (!bleIsUp()) { publish(); return; }

    /* The name is derived once the adapter address is known: the client
     * prefix-matches "rnode " case-insensitively, so the suffix is cosmetic and
     * only exists to tell two devices apart in a phone's pairing list. */
    if (!s_advName[0]) {
        uint8_t addr[6];
        if (bleOwnAddr(addr, nullptr))
            snprintf(s_advName, sizeof(s_advName), "RNode %02X%02X", addr[1], addr[0]);
    }

    /* Advertise only while nobody holds the session. */
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && s_advSlot < 0) {
        ble_adv_req_t r = {};
        safeStrncpy(r.name, s_advName, sizeof(r.name));
        std::memcpy(r.uuid128, NUS_SVC.value, 16);
        r.includeName = 1;
        r.connectable = 1;
        r.priority    = 1;
        s_advSlot = bleAdvRequest(&r);
        if (s_advSlot >= 0) info("advertising as \"%s\"", s_advName);
    }
    publish();
}

static void onCfgChange(const char*, const char*) { s_configDirty = true; wake(); }

/* ─────────────── CLI ─────────────── */

static void cliRnodeBle(const char* args) {
    if (args && strcmp(args, "help") == 0) {
        cliPrintf("%-*s RNode-over-Bluetooth door status\n", CLI_HELP_COL, "rnode-ble");
        return;
    }
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s RNode-over-Bluetooth door status\n", CLI_HELP_COL, "rnode-ble");
        cliPrintf("Pair a phone with `ble pair 60`, then point an RNS "
                  "RNodeInterface at it.\n");
        return;
    }
    cliPrintf("state:    %s\n", stateWord());
    cliPrintf("name:     %s\n", s_advName[0] ? s_advName : "(not yet known)");
    cliPrintf("mtu:      %u\n", (unsigned)s_mtu);
    cliPrintf("sessions: %u opened, %u refused by the endpoint\n",
              (unsigned)s_sessions, (unsigned)s_refused);
    cliPrintf("traffic:  in %u B, out %u B\n",
              (unsigned)(s_bytesIn & 0x7fffffff), (unsigned)(s_bytesOut & 0x7fffffff));
    if (s_carryLen)      cliPrintf("carry:    %u B waiting for the endpoint\n", (unsigned)s_carryLen);
    if (s_txCarryLen)    cliPrintf("held:     %u B waiting for a notify buffer\n", (unsigned)s_txCarryLen);
    if (s_notifyBlocked) cliPrintf("notify:   blocked, waiting for a buffer\n");
}

/* ─────────────── task ─────────────── */

static void doorTaskMain(void*) {
    info("[%s] task up", TAG);

    itsClientInit(2);
    s_rxRing = xStreamBufferCreate(DOOR_RX_RING, 1);
    if (!s_rxRing) { err("rx ring alloc failed"); killSelf(); }

    /* Registered once for the process, on this task, so every callback below
     * runs here under our own itsPoll — never in NimBLE host context. */
    bleRegister(BLE_EV_UP,         onBleUp);
    bleRegister(BLE_EV_DOWN,       onBleDown);
    bleRegister(BLE_EV_CONNECT,    onBleConnect);
    bleRegister(BLE_EV_DISCONNECT, onBleDisconnect);
    bleRegister(BLE_EV_MTU,        onBleMtu);
    bleRegister(BLE_EV_SUBSCRIBE,  onBleSubscribe);
    bleRegister(BLE_EV_NOTIFY_TX,  onBleNotifyTx);

    storageSubscribeChanges("s.ble.rnode", onCfgChange);

    for (;;) {
        while (itsPoll(0)) {}
        if (s_configDirty) { s_configDirty = false; applyConfig(); }
        /* A blocked notify retries every pass. BLE_EV_NOTIFY_TX is the fast
         * wake, but it only fires when a notification COMPLETES on this
         * connection — a refusal with nothing in flight (msys drained by
         * another consumer's traffic) would otherwise never wake us again. */
        if (s_notifyBlocked) s_notifyBlocked = false;
        pumpInbound();
        pumpOutbound();
        /* A pending carry clears when the endpoint drains, which produces no
         * notification of its own on this side; a blocked notify may have no
         * completion coming. Both poll short. Everything else parks. */
        itsPoll((s_carryLen || s_notifyBlocked) ? pdMS_TO_TICKS(20) : portMAX_DELAY);
    }
}

void RnodeBleService::onInit() {
    /* Queued, not registered: the Bluetooth host starts lazily, and NimBLE's
     * GATT registry only exists between nimble_port_init() and the host start.
     * NUS_SVCS is static const and outlives the process, which is what makes
     * handing over a pointer legal. */
    bleGattAdd(NUS_SVCS);

    cliRegisterCmd("rnode-ble", cliRnodeBle);

    storageBegin();
    storageSet("ble.rnode.state_text", "disabled");
    storageSet("ble.rnode.traffic", "in 0 B \xC2\xB7 out 0 B \xC2\xB7 0 sessions");
    storageEnd();

    s_task = spawnTask(doorTaskMain, TAG, 5120, nullptr, 1, CORE_PRIMARY, STACK_PSRAM);
}
