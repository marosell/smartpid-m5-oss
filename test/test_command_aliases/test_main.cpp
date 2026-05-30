#include <unity.h>
#include <ArduinoJson.h>
#include "command_handler.h"
#include "mqtt_client.h"
#include "telemetry.h"

static Config testCfg;
static ChannelState testCh1;
static ChannelState testCh2;

static void init_contract_fixture() {
    memset(&testCfg, 0, sizeof(testCfg));
    testCh1 = ChannelState();
    testCh2 = ChannelState();

    strlcpy(testCfg.serial_hex, "000C3BA7C0E8FC", sizeof(testCfg.serial_hex));
    strlcpy(testCfg.topic_id, "791402d5ac0fe1", sizeof(testCfg.topic_id));
    strlcpy(testCfg.temp_unit, "F", sizeof(testCfg.temp_unit));
    strlcpy(testCfg.clock_ntp_host, "pool.ntp.org", sizeof(testCfg.clock_ntp_host));
    testCfg.sample_s = 1;
    testCfg.remote_enabled = true;
    testCfg.auto_resume = true;
    testCfg.clock_tz = 0;
    testCfg.clock_ntp_enabled = true;

    testCfg.pwr_acc_mode = true;
    testCfg.pwr_acc_elements_enabled = true;
    testCfg.pwr_dast = 170.0f;
    testCfg.pwr_dout = 100;
    testCfg.pwr_distill_pct = 35;
    testCfg.pwr_dtsp = 170.0f;
    testCfg.pwr_timer_s = 3600;
    testCfg.pwr_dfsp = 200.0f;
    testCfg.pwr_dfsp_source = 1;
    testCfg.pwr_deo = 1;
    testCfg.pwr_dc1_mode = (uint8_t)DcOutputMode::ELEMENT;
    testCfg.pwr_dc2_mode = (uint8_t)DcOutputMode::AUXILIARY;
    testCfg.pwr_relay1_mode = (uint8_t)RelayMode::REFLUX_TIMER;
    testCfg.pwr_relay2_mode = (uint8_t)RelayMode::OFF;
    testCfg.pwr_r1_on_ms = 1000;
    testCfg.pwr_r1_cycle_ms = 5000;
    testCfg.pwr_r2_on_ms = 1000;
    testCfg.pwr_r2_cycle_ms = 5000;
    testCfg.pwr_wdog_enabled = true;
    testCfg.pwr_wdog_s = 30;

    testCh1.temp = 174.2f;
    testCh1.runmode = Runmode::POWER_DIRECT;
    testCh1.programRunning = true;
    testCh1.power_pct = 100;
    testCh1.distill_power_pct = 35;
    testCh1.relay_mode = RelayMode::REFLUX_TIMER;
    testCh1.relay_command = true;
    testCh1.relay_state = true;
    testCh1.timer_duration_s = 3600;

    testCh2.temp = 88.4f;
    testCh2.runmode = Runmode::POWER_DIRECT;
    testCh2.programRunning = true;
    testCh2.power_pct = 0;
    testCh2.distill_power_pct = 0;
    testCh2.relay_mode = RelayMode::OFF;
    testCh2.timer_duration_s = 3600;

    cmdHandler.begin(testCfg, mqttMgr, telemetry, testCh1, testCh2);
}

static JsonDocument parse_payload(const String& payload) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    TEST_ASSERT_FALSE_MESSAGE((bool)err, err.c_str());
    return doc;
}

static void assert_aliases_valid(const char* payload) {
    TEST_ASSERT_TRUE_MESSAGE(commandPayloadAliasesValidForTest(payload), payload);
}

static void assert_aliases_invalid(const char* payload) {
    TEST_ASSERT_FALSE_MESSAGE(commandPayloadAliasesValidForTest(payload), payload);
}

void test_resource_aliases_accept_equal_values() {
    assert_aliases_valid("{\"dc1_power\":35,\"CH1 power\":35}");
    assert_aliases_valid("{\"dc2_power\":0,\"CH2 power\":0}");
    assert_aliases_valid("{\"rl1\":true,\"CH1 relay\":true}");
    assert_aliases_valid("{\"rl2_cycle_ms\":5000,\"CH2 cycle_ms\":5000}");
}

void test_resource_aliases_reject_conflicting_values() {
    assert_aliases_invalid("{\"dc1_power\":35,\"CH1 power\":40}");
    assert_aliases_invalid("{\"rl1\":true,\"CH1 relay\":false}");
    assert_aliases_invalid("{\"rl2_on_ms\":1000,\"CH2 on_ms\":1200}");
}

void test_semantic_aliases_are_equivalent() {
    assert_aliases_valid("{\"dc2_mode\":\"auxiliary\",\"DC2 dc_mode\":\"auxilary\"}");
    assert_aliases_valid("{\"rl1_mode\":\"cycle\",\"CH1 relay_mode\":\"reflux_timer\"}");
    assert_aliases_valid("{\"rl2_mode\":\"acc_element\",\"CH2 relay_mode\":\"acc_sync\"}");
    assert_aliases_valid("{\"rl2_mode\":\"remote_other\",\"CH2 relay_mode\":\"remote\"}");
    assert_aliases_valid("{\"rl1_mode\":\"manual_on_off\",\"CH1 relay_mode\":\"on_off\"}");
}

void test_distillation_aliases_accept_equal_values() {
    assert_aliases_valid("{\"distillation\":{\"acceleration_enabled\":true},\"acc_mode\":true}");
    assert_aliases_valid("{\"distillation\":{\"finish_temp_probe\":\"probe1\"},\"finish_temp_source\":\"CH1\"}");
    assert_aliases_valid("{\"distillation\":{\"finish_temp_probe\":\"probe2\"},\"finish_temp_source\":\"CH2\"}");
    assert_aliases_valid("{\"distillation\":{\"finish_action\":\"end\"},\"finish_action\":\"shutoff\"}");
    assert_aliases_valid("{\"distillation\":{\"acceleration_relays_enabled\":false},\"acc_elements\":false}");
}

void test_distillation_aliases_reject_conflicting_values() {
    assert_aliases_invalid("{\"distillation\":{\"acceleration_power\":100},\"accel_power\":80}");
    assert_aliases_invalid("{\"distillation\":{\"timer_s\":3600},\"timer_s\":1800}");
    assert_aliases_invalid("{\"distillation\":{\"finish_temp_probe\":\"probe1\"},\"finish_temp_source\":\"CH2\"}");
    assert_aliases_invalid("{\"distillation\":{\"finish_action\":\"continue\"},\"finish_action\":\"end\"}");
}

void test_transition_aliases_accept_equivalent_commands() {
    assert_aliases_valid("{\"workflow\":\"distillation\",\"strategy\":\"program\",\"action\":\"start\",\"program_running\":true}");
    assert_aliases_valid("{\"workflow\":\"distillation\",\"strategy\":\"manual\",\"action\":\"start\",\"program_running\":false}");
    assert_aliases_valid("{\"workflow\":\"distillation\",\"strategy\":\"program\",\"action\":\"start\",\"start\":\"power\"}");
    assert_aliases_valid("{\"action\":\"stop\",\"stop\":true}");
    assert_aliases_valid("{\"action\":\"reset\",\"reset\":true}");
    assert_aliases_valid("{\"resume\":true,\"pause\":false}");
}

void test_transition_aliases_reject_conflicts() {
    assert_aliases_invalid("{\"workflow\":\"distillation\",\"strategy\":\"program\",\"action\":\"start\",\"program_running\":false}");
    assert_aliases_invalid("{\"workflow\":\"distillation\",\"strategy\":\"manual\",\"action\":\"start\",\"program_running\":true}");
    assert_aliases_invalid("{\"workflow\":\"distillation\",\"strategy\":\"manual\",\"action\":\"start\",\"start\":\"power\"}");
    assert_aliases_invalid("{\"start\":\"power\",\"program_running\":false}");
    assert_aliases_invalid("{\"action\":\"stop\",\"stop\":false}");
    assert_aliases_invalid("{\"action\":\"reset\",\"reset\":false}");
    assert_aliases_invalid("{\"resume\":true,\"pause\":true}");
}

void test_status_payload_is_schema_v2() {
    init_contract_fixture();
    JsonDocument doc = parse_payload(mqttStatusPayloadForTest(testCfg));

    TEST_ASSERT_EQUAL_STRING("proofpro", doc["firmware"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("0.3.0", doc["firmware_version"].as<const char*>());
    TEST_ASSERT_EQUAL(2, doc["schema_version"].as<int>());
    TEST_ASSERT_EQUAL_STRING("F", doc["unit"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("running", doc["device_state"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("distillation", doc["workflow"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("program", doc["strategy"].as<const char*>());
    TEST_ASSERT_TRUE(doc["remote_enabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("RDY", doc["remote_state"].as<const char*>());
}

void test_config_payload_uses_v2_resource_names() {
    init_contract_fixture();
    JsonDocument doc = parse_payload(mqttConfigPayloadForTest(testCfg));

    TEST_ASSERT_TRUE(doc["distillation"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["program"].isNull());
    TEST_ASSERT_TRUE(doc["dc_outputs"]["dc1"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["dc_outputs"]["dc2"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["dc_outputs"]["DC1"].isNull());
    TEST_ASSERT_TRUE(doc["dc_outputs"]["DC2"].isNull());
    TEST_ASSERT_TRUE(doc["relays"]["rl1"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["relays"]["rl2"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["relays"]["CH1"].isNull());
    TEST_ASSERT_TRUE(doc["relays"]["CH2"].isNull());

    JsonObject distillation = doc["distillation"];
    TEST_ASSERT_TRUE(distillation["acceleration_enabled"].as<bool>());
    TEST_ASSERT_EQUAL(170, distillation["acceleration_end_temp"].as<int>());
    TEST_ASSERT_EQUAL(100, distillation["acceleration_power"].as<int>());
    TEST_ASSERT_EQUAL(35, distillation["run_power"].as<int>());
    TEST_ASSERT_EQUAL(3600, distillation["timer_s"].as<int>());
    TEST_ASSERT_EQUAL_STRING("probe1", distillation["finish_temp_probe"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("end", distillation["finish_action"].as<const char*>());
    TEST_ASSERT_TRUE(distillation["acceleration_relays_enabled"].as<bool>());

    TEST_ASSERT_EQUAL_STRING("element", doc["dc_outputs"]["dc1"]["mode"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("auxiliary", doc["dc_outputs"]["dc2"]["mode"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("cycle", doc["relays"]["rl1"]["mode"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("off", doc["relays"]["rl2"]["mode"].as<const char*>());
}

void test_state_payload_uses_v2_live_shape_and_unit() {
    init_contract_fixture();
    JsonDocument doc = parse_payload(telemetryStatePayloadForTest(testCfg, testCh1, testCh2));

    TEST_ASSERT_EQUAL_STRING("F", doc["unit"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("running", doc["device_state"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("distillation", doc["workflow"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("program", doc["strategy"].as<const char*>());

    TEST_ASSERT_TRUE(doc["probes"]["probe1"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["probes"]["probe2"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["dc_outputs"]["dc1"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["dc_outputs"]["dc2"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["relays"]["rl1"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["relays"]["rl2"].is<JsonObject>());
    TEST_ASSERT_TRUE(doc["program"].is<JsonObject>());

    TEST_ASSERT_TRUE(doc["probes"]["probe1"]["unit"].isNull());
    TEST_ASSERT_EQUAL_STRING("element", doc["dc_outputs"]["dc1"]["mode"].as<const char*>());
    TEST_ASSERT_EQUAL(100, doc["dc_outputs"]["dc1"]["power"].as<int>());
    TEST_ASSERT_EQUAL(35, doc["dc_outputs"]["dc1"]["target_power"].as<int>());
    TEST_ASSERT_EQUAL_STRING("cycle", doc["relays"]["rl1"]["mode"].as<const char*>());
    TEST_ASSERT_TRUE(doc["relays"]["rl1"]["state"].as<bool>());
    TEST_ASSERT_TRUE(doc["relays"]["rl1"]["engaged"].as<bool>());
    TEST_ASSERT_TRUE(doc["program"]["running"].as<bool>());
    TEST_ASSERT_FALSE(doc["program"]["ended"].as<bool>());
    TEST_ASSERT_FALSE(doc["program"]["latched"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("probe1", doc["program"]["finish_temp_source"].as<const char*>());
    TEST_ASSERT_FALSE(doc["program"]["timer_frozen"].as<bool>());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_resource_aliases_accept_equal_values);
    RUN_TEST(test_resource_aliases_reject_conflicting_values);
    RUN_TEST(test_semantic_aliases_are_equivalent);
    RUN_TEST(test_distillation_aliases_accept_equal_values);
    RUN_TEST(test_distillation_aliases_reject_conflicting_values);
    RUN_TEST(test_transition_aliases_accept_equivalent_commands);
    RUN_TEST(test_transition_aliases_reject_conflicts);
    RUN_TEST(test_status_payload_is_schema_v2);
    RUN_TEST(test_config_payload_uses_v2_resource_names);
    RUN_TEST(test_state_payload_uses_v2_live_shape_and_unit);
    return UNITY_END();
}
