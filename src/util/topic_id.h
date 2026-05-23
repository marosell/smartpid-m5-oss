#pragma once
// topic_id.h — SmartPID M5 PRO serial-number-to-MQTT-topic-ID scrambler
//
// The OEM firmware derives a 14-char device ID from the 7-byte hardware serial
// using a fixed bit-inversion + XOR + permutation algorithm. This ID appears in
// all MQTT topic paths: smartpidM5/pro/<id>/...
//
// Reference vector (confirmed against hardware, 2026-05-22):
//   Input:  "040531000000E0"
//   Output: "6e345245af3704"
//
// Algorithm (ported from vendor C source):
//   1. Bit-invert each serial byte (reverse bit order)
//   2. XOR each byte with a per-position constant
//   3. Reorder the 7 bytes per a fixed permutation
//   4. Lowercase hex-encode each byte and concatenate

#include <Arduino.h>

// Scramble a 7-byte serial (14-char hex string) into the 14-char MQTT topic ID.
// Returns empty String on invalid input (length != 14 or non-hex chars).
String scrambleSerialToId(const String& serialHex);

// Derive a stable 7-byte serial (14-char hex string) from the ESP32 MAC address.
// Used as the default serial on first boot when no serial is stored in NVS.
// The derived serial is stable across reboots but unique per device.
String macDerivedSerial();
