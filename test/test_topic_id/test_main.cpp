// test_main.cpp — Unit tests for topic ID scrambler
// Run with: pio test -e test
//
// The reference vector was confirmed against a physical device on 2026-05-22:
//   Serial printed on device back label: 040531000000E0
//   MQTT ID observed on wire:            6e345245af3704

#include <unity.h>
#include "util/topic_id.h"

// ── Reference vector (locked — do not change) ─────────────────────────────────
void test_reference_vector() {
    String result = scrambleSerialToId("040531000000E0");
    TEST_ASSERT_EQUAL_STRING("6e345245af3704", result.c_str());
}

// ── Case insensitivity ────────────────────────────────────────────────────────
void test_lowercase_input() {
    // Same serial in lowercase — result must be identical
    String result = scrambleSerialToId("040531000000e0");
    TEST_ASSERT_EQUAL_STRING("6e345245af3704", result.c_str());
}

// ── Zero serial ───────────────────────────────────────────────────────────────
void test_zero_serial() {
    // All-zero input should produce a deterministic (non-empty) result
    String result = scrambleSerialToId("00000000000000");
    TEST_ASSERT_EQUAL(14, (int)result.length());
    // XOR constants only: bitOrderInvert(0) = 0, so output bytes equal XOR constants
    // scrambled[0] = 0 ^ 0x6E = 0x6E → "6e"
    TEST_ASSERT_TRUE(result.startsWith("6e"));
}

// ── Invalid input: wrong length ────────────────────────────────────────────────
void test_invalid_too_short() {
    String result = scrambleSerialToId("0405310000");
    TEST_ASSERT_EQUAL_STRING("", result.c_str());
}

void test_invalid_too_long() {
    String result = scrambleSerialToId("040531000000E0FF");
    TEST_ASSERT_EQUAL_STRING("", result.c_str());
}

// ── Invalid input: non-hex chars ──────────────────────────────────────────────
void test_invalid_non_hex() {
    String result = scrambleSerialToId("ZZZZZZZZZZZZZZ");
    TEST_ASSERT_EQUAL_STRING("", result.c_str());
}

// ── Output format: 14 lowercase hex chars ─────────────────────────────────────
void test_output_format() {
    String result = scrambleSerialToId("AABBCCDDEEFF11");
    TEST_ASSERT_EQUAL(14, (int)result.length());
    for (char c : result) {
        TEST_ASSERT_TRUE(isxdigit(c));
        // Must be lowercase
        if (isalpha(c)) TEST_ASSERT_TRUE(islower(c));
    }
}

// ── Idempotent (same input → same output) ────────────────────────────────────
void test_idempotent() {
    String a = scrambleSerialToId("040531000000E0");
    String b = scrambleSerialToId("040531000000E0");
    TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());
}

// ── Entry points ─────────────────────────────────────────────────────────────
void setup() {
    delay(2000);  // allow serial to stabilise
    UNITY_BEGIN();

    RUN_TEST(test_reference_vector);   // LOCKED — must always pass
    RUN_TEST(test_lowercase_input);
    RUN_TEST(test_zero_serial);
    RUN_TEST(test_invalid_too_short);
    RUN_TEST(test_invalid_too_long);
    RUN_TEST(test_invalid_non_hex);
    RUN_TEST(test_output_format);
    RUN_TEST(test_idempotent);

    UNITY_END();
}

void loop() {}
