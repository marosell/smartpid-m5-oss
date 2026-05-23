// topic_id.cpp — SmartPID serial → MQTT ID scrambler
// Ported from vendor C source; confirmed against hardware 2026-05-22.

#include "topic_id.h"
#include <esp_efuse.h>    // esp_efuse_mac_get_default
#include <esp_mac.h>

// ── bitOrderInvert ────────────────────────────────────────────────────────────
// Reverses the bit order within a byte.
// C source:
//   for (i = 0; i < 4; i++) invertedByte |= (byte & (1 << i)) << (7 - 2*i);
//   for (i = 4; i < 8; i++) invertedByte |= (byte & (1 << i)) >> (2*i - 7);
static uint8_t bitOrderInvert(uint8_t b) {
    uint8_t result = 0;
    for (int i = 0; i < 4; i++) {
        result |= (uint8_t)((b & (1u << i)) << (7 - 2 * i));
    }
    for (int i = 4; i < 8; i++) {
        result |= (uint8_t)((b & (1u << i)) >> (2 * i - 7));
    }
    return result;
}

// ── scrambleSerialToId ────────────────────────────────────────────────────────
// C source uses 1-based indexing: serNo[1] through serNo[7].
// Python port (confirmed correct): serial_bytes[N-1] for C index N.
//
// Permutation + XOR table (from vendor C source):
//   scrambled[0] = bitOrderInvert(serial[5]) ^ 0x6E   // C: serNo[6]
//   scrambled[1] = bitOrderInvert(serial[0]) ^ 0x14   // C: serNo[1]
//   scrambled[2] = bitOrderInvert(serial[2]) ^ 0xDE   // C: serNo[3]
//   scrambled[3] = bitOrderInvert(serial[1]) ^ 0xE5   // C: serNo[2]
//   scrambled[4] = bitOrderInvert(serial[4]) ^ 0xAF   // C: serNo[5]
//   scrambled[5] = bitOrderInvert(serial[6]) ^ 0x30   // C: serNo[7]
//   scrambled[6] = bitOrderInvert(serial[3]) ^ 0x04   // C: serNo[4]
String scrambleSerialToId(const String& serialHex) {
    if (serialHex.length() != 14) return "";
    for (char c : serialHex) {
        if (!isxdigit(c)) return "";
    }

    uint8_t s[7];
    for (int i = 0; i < 7; i++) {
        s[i] = (uint8_t)strtoul(serialHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    }

    uint8_t out[7] = {
        (uint8_t)(bitOrderInvert(s[5]) ^ 0x6E),
        (uint8_t)(bitOrderInvert(s[0]) ^ 0x14),
        (uint8_t)(bitOrderInvert(s[2]) ^ 0xDE),
        (uint8_t)(bitOrderInvert(s[1]) ^ 0xE5),
        (uint8_t)(bitOrderInvert(s[4]) ^ 0xAF),
        (uint8_t)(bitOrderInvert(s[6]) ^ 0x30),
        (uint8_t)(bitOrderInvert(s[3]) ^ 0x04),
    };

    char buf[15];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x",
             out[0], out[1], out[2], out[3], out[4], out[5], out[6]);
    return String(buf);
}

// ── macDerivedSerial ──────────────────────────────────────────────────────────
// Builds a 7-byte serial from the ESP32 base MAC address:
//   Byte 0: 0x00 (padding prefix)
//   Bytes 1-6: MAC[0]–MAC[5]
// This gives a stable, device-unique serial without hardware serial access.
String macDerivedSerial() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char buf[15];
    snprintf(buf, sizeof(buf), "00%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}
