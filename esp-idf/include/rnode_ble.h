/**
 * rnode_ble — the RNode endpoint's Bluetooth door.
 *
 * A Nordic UART Service over BLE, wired straight onto iface-lora's existing
 * RNode endpoint. Sideband and any RNS RNodeInterface with a `ble_name` (or
 * none) find this device among their bonded peers, connect, and get the same
 * KISS stream a USB cable or the TCP door carries — this straddle moves bytes
 * and knows nothing about KISS, the radio, or Reticulum.
 *
 * Self-registers with the endpoint on the first CCCD subscribe; the endpoint's
 * one-session policy is what decides whether it is let in.
 */
#pragma once

#include "service.h"

/** Boot-registered service: onInit() queues the Nordic UART Service with
 *  spangap-ble, registers the `rnode-ble` CLI verb and spawns the door task.
 *  The door itself follows s.lora.rnode.ble (default on). */
class RnodeBleService : public Service {
public:
    void onInit() override;
};
