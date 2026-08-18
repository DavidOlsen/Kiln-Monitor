#include <Arduino.h>
#include <unity.h>
#include <math.h>

#include "sensors/sensors.h"
#include "Adafruit_MCP9601.h"

void setUp(void) {}
void tearDown(void) {}

// MCP9601 reuses the same status bit position (0x10) that the base MCP9600
// calls "input range" and gives it a more specific meaning: open thermocouple
// circuit. A disconnected probe reports via this bit — see
// MCP9601_STATUS_OPENCIRCUIT in Adafruit_MCP9601.h and MCP960X_STATUS_INPUTRANGE
// in Adafruit_MCP9600.h (same value, 0x10).
static void test_no_probe_open_circuit_bit_flags_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(25.0f, MCP9601_STATUS_OPENCIRCUIT);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Input range fault", r.errMsg);
}

static void test_no_probe_nan_reading_flags_fault(void) {
    // Some firmware/driver states surface a disconnected probe as NaN
    // instead of (or in addition to) the status bit.
    ZoneFaultResult r = evaluateZoneFault(NAN, 0x00);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", r.errMsg);
}

static void test_out_of_range_reading_flags_fault(void) {
    ZoneFaultResult low = evaluateZoneFault(-75.0f, 0x00);
    TEST_ASSERT_TRUE(low.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", low.errMsg);

    ZoneFaultResult high = evaluateZoneFault(1500.0f, 0x00);
    TEST_ASSERT_TRUE(high.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", high.errMsg);
}

static void test_normal_reading_no_fault(void) {
    ZoneFaultResult r = evaluateZoneFault(650.0f, 0x00);
    TEST_ASSERT_FALSE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("", r.errMsg);
}

static void test_bad_reading_takes_priority_over_status(void) {
    // Out-of-range reading should still report as such even if the status
    // byte also happens to carry an unrelated alert bit set.
    ZoneFaultResult r = evaluateZoneFault(NAN, MCP960X_STATUS_ALERT1);
    TEST_ASSERT_TRUE(r.faulted);
    TEST_ASSERT_EQUAL_STRING("Reading out of range", r.errMsg);
}

void setup() {
    delay(2000); // allow serial monitor / test runner to attach
    UNITY_BEGIN();
    RUN_TEST(test_no_probe_open_circuit_bit_flags_fault);
    RUN_TEST(test_no_probe_nan_reading_flags_fault);
    RUN_TEST(test_out_of_range_reading_flags_fault);
    RUN_TEST(test_normal_reading_no_fault);
    RUN_TEST(test_bad_reading_takes_priority_over_status);
    UNITY_END();
}

void loop() {}
