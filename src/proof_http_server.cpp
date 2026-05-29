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
    _server.on("/config", HTTP_GET, [this] { _handleConfig(); });
    _server.on("/commands", HTTP_POST, [this] { _handleCommand(); });
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

void ProofHttpServer::_handleConfig() {
    _server.send(200, "application/json", _configJson());
}

void ProofHttpServer::_handleCommand() {
    if (!_commands) {
        _server.send(503, "application/json", "{\"ok\":false,\"error\":\"commands_unavailable\"}");
        return;
    }

    String body = _server.arg("plain");
    body.trim();
    if (body.length() == 0) {
        _server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty_body\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        JsonDocument out;
        out["ok"] = false;
        out["error"] = "invalid_json";
        out["detail"] = err.c_str();
        String payload;
        serializeJson(out, payload);
        _server.send(400, "application/json", payload);
        return;
    }

    _commands->handle(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
    JsonDocument out;
    out["ok"] = true;
    out["source"] = "http";
    out["status"] = "/status";
    String payload;
    serializeJson(out, payload);
    _server.send(202, "application/json", payload);
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

    JsonObject ch1 = doc["channels"]["CH1"].to<JsonObject>();
    if (_ch1) {
        ch1["temp"] = _ch1->temp;
        ch1["power"] = _ch1->power_pct;
        ch1["target_power"] = _ch1->distill_power_pct;
        ch1["program_running"] = _ch1->programRunning;
        ch1["ended"] = _ch1->finishEnd || _ch1->finishLatch;
        ch1["relay_state"] = _ch1->relay_state;
    }

    JsonObject ch2 = doc["channels"]["CH2"].to<JsonObject>();
    if (_ch2) {
        ch2["temp"] = _ch2->temp;
        ch2["power"] = _ch2->power_pct;
        ch2["target_power"] = _ch2->distill_power_pct;
        ch2["program_running"] = _ch2->programRunning;
        ch2["ended"] = _ch2->finishEnd || _ch2->finishLatch;
        ch2["relay_state"] = _ch2->relay_state;
    }

    String payload;
    serializeJson(doc, payload);
    return payload;
}

String ProofHttpServer::_configJson() const {
    JsonDocument doc;
    if (_cfg) {
        JsonObject program = doc["program"].to<JsonObject>();
        program["acc_mode"] = _cfg->pwr_acc_mode;
        program["accel_temp"] = _cfg->pwr_dast;
        program["accel_power"] = _cfg->pwr_dout;
        program["post_accel_power"] = _cfg->pwr_distill_pct;
        program["timer_start_temp"] = _cfg->pwr_dtsp;
        program["timer_s"] = _cfg->pwr_timer_s;
        program["finish_temp"] = _cfg->pwr_dfsp;
        program["finish_temp_source"] = (_cfg->pwr_dfsp_source == 2) ? "CH2" : "CH1";
        program["finish_action"] = (_cfg->pwr_deo == 1) ? "end" : "continue";

        JsonObject clock = doc["clock"].to<JsonObject>();
        clock["timezone_label"] = clockCurrentTimeZoneLabel(*_cfg);
        clock["timezone_posix"] = clockCurrentTimeZonePosix(*_cfg);
        clock["ntp_enabled"] = _cfg->clock_ntp_enabled;
        clock["ntp_host"] = _cfg->clock_ntp_host;
        clock["clock_24h"] = _cfg->clock_24h;
        clock["synced"] = clockTimeIsSynced();

        JsonObject relays = doc["relays"].to<JsonObject>();
        JsonObject r1 = relays["CH1"].to<JsonObject>();
        r1["mode"] = relayModeStr((RelayMode)_cfg->pwr_relay1_mode);
        r1["on_ms"] = _cfg->pwr_r1_on_ms;
        r1["cycle_ms"] = _cfg->pwr_r1_cycle_ms;
        JsonObject r2 = relays["CH2"].to<JsonObject>();
        r2["mode"] = relayModeStr((RelayMode)_cfg->pwr_relay2_mode);
        r2["on_ms"] = _cfg->pwr_r2_on_ms;
        r2["cycle_ms"] = _cfg->pwr_r2_cycle_ms;

        JsonObject dc = doc["dc_outputs"].to<JsonObject>();
        dc["DC1"] = dcOutputModeStr(normalizeDcOutputMode(_cfg->pwr_dc1_mode));
        dc["DC2"] = dcOutputModeStr(normalizeDcOutputMode(_cfg->pwr_dc2_mode));
    }
    String payload;
    serializeJson(doc, payload);
    return payload;
}

String ProofHttpServer::_htmlPage() const {
    String page;
    page.reserve(1800);
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
    page += F("<p><a href='/status'>/status</a> · <a href='/config'>/config</a> · <a href='/healthz'>/healthz</a></p>");
    page += F("<h2>Command</h2><form method='post' action='/commands'>");
    page += F("<textarea name='plain'>{\"status\":true}</textarea><p><button type='submit'>Send</button></p></form>");
    page += F("<p class='muted'>HTTP commands use the same JSON schema as MQTT commands.</p>");
    page += F("</body></html>");
    return page;
}
