#include <Arduino.h>
#include <unity.h>
#include <math.h>

#include "sensors/sensors.h"
#include "Adafruit_MCP9601.h"

void setUp(void) {}
void tearDown(void) {}

// Three real disconnected-probe signatures observed in the field, all with
// MCP9601_STATUS_OPENCIRCUIT set and adcRaw railed to ~120,000+ counts (far
// outside any real thermocouple's EMF range), but with wildly different
// hotJunction behavior — which is exactly why hotJunction can't be trusted
// as the open-circuit signal and adcRaw magnitude is used instead.

// Case 1: hotJunction pegs at exactly 0.0 (the chip can't linearize a railed
// ADC input).
static void test_open_circuit_pegged_zero_reading_flags_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(0.0f, MCP9601_STATUS_OPENCIRCUIT, 124942);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Probe disconnected (open circuit)", r.errMsg);
}

// Case 2: hotJunction instead lands on a plausible-looking non-zero value
// (observed: 40.9375C) even though the input is just as railed as case 1.
// Relying on temperature==0.0 alone (an earlier version of this check) missed
// this case entirely — adcRaw is what actually catches it.
static void test_open_circuit_plausible_reading_with_large_adc_flags_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(40.9375f, MCP9601_STATUS_OPENCIRCUIT, 121610);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Probe disconnected (open circuit)", r.errMsg);
}

// Case 3: a probe disconnected mid-run — hotJunction freezes at the last
// good real reading instead of updating at all, while adcRaw immediately
// jumps to the railed range. This is the most dangerous case (a stale-but-
// totally-normal-looking reading with no other indication of a problem), and
// the reason temperature can never be part of this decision.
static void test_open_circuit_stale_reading_with_large_adc_flags_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(650.0f, MCP9601_STATUS_OPENCIRCUIT, 120500);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Probe disconnected (open circuit)", r.errMsg);
}

// Regression test for a real false positive observed in the field: the
// open-circuit bit flickers on/off on a marginal-but-connected probe while
// adcRaw stays small (~15-34 counts) and the reading itself stays normal.
// That should NOT fault the zone.
static void test_open_circuit_bit_with_small_adc_does_not_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(21.1875f, MCP9601_STATUS_OPENCIRCUIT, 32);
    TEST_ASSERT_FALSE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("", r.errMsg);
}

// The open-circuit bit is required too — a large adcRaw alone (e.g. a brief
// noise spike) shouldn't fault the zone without the chip's own status bit
// agreeing something's wrong.
static void test_large_adc_without_status_bit_does_not_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(500.0f, 0x00, 121610);
    TEST_ASSERT_FALSE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("", r.errMsg);
}

// Short-circuit detection via MCP9601_STATUS_SHORTCIRCUIT (0x20) is disabled
// for now: field data showed the bit's duty cycle on a genuinely shorted
// probe overlaps the baseline flicker duty cycle on a healthy probe at rest,
// even over short debounce windows, so it can't be told apart from noise
// without a hardware change. The bit is deliberately ignored until then.
static void test_short_circuit_bit_alone_does_not_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(25.0f, MCP9601_STATUS_SHORTCIRCUIT, 0);
    TEST_ASSERT_FALSE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("", r.errMsg);
}

static void test_no_probe_nan_reading_flags_fault(void) {
    // Some firmware/driver states surface a disconnected probe as NaN
    // instead of (or in addition to) the status bit.
    ZoneFaultResult r = evaluateZoneFault(NAN, 0x00, 0);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", r.errMsg);
}

static void test_out_of_range_reading_flags_fault(void) {
    ZoneFaultResult low = evaluateZoneFault(-75.0f, 0x00, 0);
    TEST_ASSERT_TRUE(low.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", low.errMsg);

    ZoneFaultResult high = evaluateZoneFault(1500.0f, 0x00, 0);
    TEST_ASSERT_TRUE(high.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", high.errMsg);
}

static void test_normal_reading_no_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(650.0f, 0x00, 5000);
    TEST_ASSERT_FALSE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("", r.errMsg);
}

static void test_bad_reading_takes_priority_over_status(void) {
    // Out-of-range reading should still report as such even if the status
    // byte and adcRaw also both scream open circuit — otherwise a genuinely
    // bad reading could get mislabeled as just a disconnected probe.
    ZoneFaultResult r = evaluateZoneFault(NAN, MCP9601_STATUS_OPENCIRCUIT, 121610);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", r.errMsg);
}

void setup() {
    delay(2000); // allow serial monitor / test runner to attach
    UNITY_BEGIN();
    RUN_TEST(test_open_circuit_pegged_zero_reading_flags_fault);
    RUN_TEST(test_open_circuit_plausible_reading_with_large_adc_flags_fault);
    RUN_TEST(test_open_circuit_stale_reading_with_large_adc_flags_fault);
    RUN_TEST(test_open_circuit_bit_with_small_adc_does_not_fault);
    RUN_TEST(test_large_adc_without_status_bit_does_not_fault);
    RUN_TEST(test_short_circuit_bit_alone_does_not_fault);
    RUN_TEST(test_no_probe_nan_reading_flags_fault);
    RUN_TEST(test_out_of_range_reading_flags_fault);
    RUN_TEST(test_normal_reading_no_fault);
    RUN_TEST(test_bad_reading_takes_priority_over_status);
    UNITY_END();
}

void loop() {}
