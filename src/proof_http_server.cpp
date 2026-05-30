#include "proof_http_server.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "command_handler.h"
#include "clock_sync.h"
#include "mqtt_client.h"
#include "telemetry.h"

ProofHttpServer proofHttpServer;

void ProofHttpServer::begin(Config& cfg,
                            MQTTManager& mqtt,
                            TelemetryPublisher& telemetry,
                            CommandHandler& commands,
                            ChannelState& ch1,
                            ChannelState& ch2) {
    if (_running) return;

    _cfg = &cfg;
    _mqtt = &mqtt;
    _telemetry = &telemetry;
    _commands = &commands;
    _ch1 = &ch1;
    _ch2 = &ch2;

    _server.on("/", HTTP_GET, [this] { _handleRoot(); });
    _server.on("/healthz", HTTP_GET, [this] { _handleHealthz(); });
    _server.on("/status", HTTP_GET, [this] { _handleStatus(); });
    _server.on("/debug/config-summary", HTTP_GET, [this] { _handleConfigSummary(); });
    _server.onNotFound([this] { _handleNotFound(); });
    _server.begin();
    _running = true;
    log_i("[HTTP] ProofPro server listening on http://%s/",
          WiFi.localIP().toString().c_str());
}

void ProofHttpServer::loop() {
    if (_running) _server.handleClient();
}

void ProofHttpServer::_handleRoot() {
    _server.send(200, "text/html; charset=UTF-8", _htmlPage());
}

void ProofHttpServer::_handleHealthz() {
    _server.send(200, "text/plain; charset=UTF-8", "ok\n");
}

void ProofHttpServer::_handleStatus() {
    _server.send(200, "application/json", _statusJson());
}

void ProofHttpServer::_handleConfigSummary() {
    _server.send(200, "application/json", _configSummaryJson());
}

void ProofHttpServer::_handleNotFound() {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "not_found";
    doc["path"] = _server.uri();
    String payload;
    serializeJson(doc, payload);
    _server.send(404, "application/json", payload);
}

String ProofHttpServer::_statusJson() const {
    JsonDocument doc;
    doc["firmware"] = "proofpro";
    doc["firmware_version"] = "0.2.0";
    doc["schema_version"] = 1;
    if (_cfg) {
        doc["serial"] = _cfg->serial_hex;
        doc["topic_id"] = _cfg->topic_id;
        doc["unit"] = _cfg->temp_unit;
        doc["watchdog_enabled"] = _cfg->pwr_wdog_enabled;
        doc["watchdog_s"] = _cfg->pwr_wdog_s;
        doc["auto_resume_enabled"] = _cfg->auto_resume;
    }
    doc["ssid"] = WiFi.SSID();
    doc["client"] = WiFi.localIP().toString();
    doc["mqtt_connected"] = _mqtt && _mqtt->connected();
    doc["remote_enabled"] = mqttRemoteEnabled();
    doc["remote_state"] = !mqttRemoteEnabled() ? "OFF" : (mqttRemoteActive() ? "ON" : "RDY");
    const bool watchdogFired = (_ch1 && _ch1->watchdogFired) || (_ch2 && _ch2->watchdogFired);
    const bool programRunning = (_ch1 && _ch1->programRunning) || (_ch2 && _ch2->programRunning);
    const bool accelActive = (_ch1 && _ch1->accelPhaseActive) || (_ch2 && _ch2->accelPhaseActive);
    const bool ended = (_ch1 && (_ch1->finishEnd || _ch1->finishLatch)) ||
                       (_ch2 && (_ch2->finishEnd || _ch2->finishLatch));
    doc["watchdog_fired"] = watchdogFired;
    doc["device_state"] = watchdogFired ? "safe" : ended ? "ended" :
        ((_ch1 && (_ch1->runmode != Runmode::IDLE)) ||
         (_ch2 && (_ch2->runmode != Runmode::IDLE))) ? "running" : "idle";
    const char* workflow = ((_ch1 && _ch1->runmode == Runmode::POWER_DIRECT) ||
                            (_ch2 && _ch2->runmode == Runmode::POWER_DIRECT)) ? "distillation" :
                           ((_ch1 && _ch1->runmode == Runmode::MONITOR) ||
                            (_ch2 && _ch2->runmode == Runmode::MONITOR)) ? "monitor" : nullptr;
    doc["workflow"] = workflow;
    doc["strategy"] = (workflow && strcmp(workflow, "distillation") == 0)
        ? (programRunning ? "program" : "manual")
        : nullptr;

    JsonObject program = doc["program"].to<JsonObject>();
    program["running"] = programRunning;
    program["accel_active"] = accelActive;
    program["ended"] = ended;
    program["target_power"] = max(_ch1 ? _ch1->distill_power_pct : 0, _ch2 ? _ch2->distill_power_pct : 0);
    program["power"] = max(_ch1 ? _ch1->power_pct : 0, _ch2 ? _ch2->power_pct : 0);
    if (_cfg) {
        program["finish_temp_source"] = (_cfg->pwr_dfsp_source == 2) ? "probe2" : "probe1";
    }

    JsonObject probes = doc["probes"].to<JsonObject>();
    if (_ch1) {
        probes["probe1"]["temp"] = _ch1->temp;
    }
    if (_ch2) {
        probes["probe2"]["temp"] = _ch2->temp;
    }

    JsonObject outputs = doc["outputs"].to<JsonObject>();
    if (_ch1) {
        outputs["dc1"]["power"] = _ch1->power_pct;
        outputs["dc1"]["target_power"] = _ch1->distill_power_pct;
    }
    if (_ch2) {
        outputs["dc2"]["power"] = _ch2->power_pct;
        outputs["dc2"]["target_power"] = _ch2->distill_power_pct;
    }

    JsonObject relays = doc["relays"].to<JsonObject>();
    if (_ch1) {
        relays["rl1"]["state"] = _ch1->relay_state;
    }
    if (_ch2) {
        relays["rl2"]["state"] = _ch2->relay_state;
    }

    JsonObject lanes = doc["lanes"].to<JsonObject>();
    if (_ch1) {
        JsonObject lane1 = lanes["lane1"].to<JsonObject>();
        lane1["program_running"] = _ch1->programRunning;
        lane1["accel_active"] = _ch1->accelPhaseActive;
        lane1["watchdog_fired"] = _ch1->watchdogFired;
        lane1["ended"] = _ch1->finishEnd || _ch1->finishLatch;
    }
    if (_ch2) {
        JsonObject lane2 = lanes["lane2"].to<JsonObject>();
        lane2["program_running"] = _ch2->programRunning;
        lane2["accel_active"] = _ch2->accelPhaseActive;
        lane2["watchdog_fired"] = _ch2->watchdogFired;
        lane2["ended"] = _ch2->finishEnd || _ch2->finishLatch;
    }

    String payload;
    serializeJson(doc, payload);
    return payload;
}

String ProofHttpServer::_configSummaryJson() const {
    JsonDocument doc;
    doc["diagnostic_only"] = true;
    if (_cfg) {
        JsonObject program = doc["program"].to<JsonObject>();
        program["acc_mode"] = _cfg->pwr_acc_mode;
        program["accel_temp"] = _cfg->pwr_dast;
        program["accel_power"] = _cfg->pwr_dout;
        program["post_accel_power"] = _cfg->pwr_distill_pct;
        program["timer_start_temp"] = _cfg->pwr_dtsp;
        program["timer_s"] = _cfg->pwr_timer_s;
        program["finish_temp"] = _cfg->pwr_dfsp;
        program["finish_temp_source"] = (_cfg->pwr_dfsp_source == 2) ? "probe2" : "probe1";
        program["finish_action"] = (_cfg->pwr_deo == 1) ? "end" : "continue";

        JsonObject clock = doc["clock"].to<JsonObject>();
        clock["timezone_label"] = clockCurrentTimeZoneLabel(*_cfg);
        clock["timezone_posix"] = clockCurrentTimeZonePosix(*_cfg);
        clock["ntp_enabled"] = _cfg->clock_ntp_enabled;
        clock["ntp_host"] = _cfg->clock_ntp_host;
        clock["clock_24h"] = _cfg->clock_24h;
        clock["synced"] = clockTimeIsSynced();

        JsonObject relays = doc["relays"].to<JsonObject>();
        JsonObject r1 = relays["rl1"].to<JsonObject>();
        r1["mode"] = relayModeStr((RelayMode)_cfg->pwr_relay1_mode);
        r1["on_ms"] = _cfg->pwr_r1_on_ms;
        r1["cycle_ms"] = _cfg->pwr_r1_cycle_ms;
        JsonObject r2 = relays["rl2"].to<JsonObject>();
        r2["mode"] = relayModeStr((RelayMode)_cfg->pwr_relay2_mode);
        r2["on_ms"] = _cfg->pwr_r2_on_ms;
        r2["cycle_ms"] = _cfg->pwr_r2_cycle_ms;

        JsonObject dc = doc["dc_outputs"].to<JsonObject>();
        dc["dc1"] = dcOutputModeStr(normalizeDcOutputMode(_cfg->pwr_dc1_mode));
        dc["dc2"] = dcOutputModeStr(normalizeDcOutputMode(_cfg->pwr_dc2_mode));
    }
    String payload;
    serializeJson(doc, payload);
    return payload;
}

String ProofHttpServer::_htmlPage() const {
    String page;
    page.reserve(1400);
    page += F("<!doctype html><html><head><meta charset='utf-8'>");
    page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    page += F("<title>ProofPro</title><style>");
    page += F("body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#111;color:#eee;margin:24px;max-width:760px}");
    page += F("h1{margin:0 0 4px;color:#ffe000}.muted{color:#999}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:20px 0}");
    page += F(".box{border:1px solid #333;padding:12px;border-radius:6px;background:#181818}.k{color:#999;font-size:12px}.v{font-size:20px;margin-top:4px}");
    page += F("code,textarea{background:#050505;color:#ddd;border:1px solid #333;border-radius:4px}textarea{width:100%;height:92px;padding:8px}");
    page += F("button{background:#ffe000;color:#000;border:0;border-radius:4px;padding:10px 14px;font-weight:700}");
    page += F("a{color:#ffe000}</style></head><body>");
    page += F("<h1>ProofPro</h1><div class='muted'>");
    if (_cfg) {
        page += _cfg->topic_id;
        page += F(" · ");
    }
    page += WiFi.localIP().toString();
    page += F("</div><div class='grid'>");
    page += F("<div class='box'><div class='k'>Remote</div><div class='v'>");
    page += !mqttRemoteEnabled() ? "OFF" : (mqttRemoteActive() ? "ON" : "RDY");
    page += F("</div></div><div class='box'><div class='k'>MQTT</div><div class='v'>");
    page += (_mqtt && _mqtt->connected()) ? "OK" : "Down";
    page += F("</div></div><div class='box'><div class='k'>Watchdog</div><div class='v'>");
    page += (_cfg && _cfg->pwr_wdog_enabled) ? String(_cfg->pwr_wdog_s) + "s" : "Off";
    page += F("</div></div></div>");
    page += F("<p><a href='/status'>/status</a> · <a href='/healthz'>/healthz</a></p>");
    page += F("</body></html>");
    return page;
}
