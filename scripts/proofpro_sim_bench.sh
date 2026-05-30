#!/usr/bin/env bash
set -euo pipefail

HOST="${MQTT_HOST:-10.0.1.203}"
USER="${MQTT_USER:-proof}"
PASS="${MQTT_PASSWORD:-proof}"
TOPIC_ID="${TOPIC_ID:-791402d5ac0fe1}"
ROOT="smartpidM5/proofpro/${TOPIC_ID}"
DOCKER_MQTT_CONTAINER="${DOCKER_MQTT_CONTAINER:-mosquitto}"
EVENT_PID=""
EVENT_FILE=""

mqtt_host() {
  local host="$HOST"
  if [[ -n "$DOCKER_MQTT_CONTAINER" && "$host" == "10.0.1.203" ]]; then
    host="127.0.0.1"
  fi
  echo "$host"
}

mqtt_pub() {
  if [[ -n "$DOCKER_MQTT_CONTAINER" ]]; then
    docker exec "$DOCKER_MQTT_CONTAINER" mosquitto_pub "$@"
  else
    mosquitto_pub "$@"
  fi
}

mqtt_sub() {
  if [[ -n "$DOCKER_MQTT_CONTAINER" ]]; then
    docker exec "$DOCKER_MQTT_CONTAINER" mosquitto_sub "$@"
  else
    mosquitto_sub "$@"
  fi
}

pub() {
  mqtt_pub -h "$(mqtt_host)" -u "$USER" -P "$PASS" -t "${ROOT}/commands" -m "$1" </dev/null
}

state_once() {
  mqtt_sub -h "$(mqtt_host)" -u "$USER" -P "$PASS" -t "${ROOT}/state" -C 1 -W 10 -v \
    </dev/null | sed 's/^[^ ]* //'
}

event_log_start() {
  event_log_stop
  EVENT_FILE="$(mktemp -t proofpro-events.XXXXXX)"
  mqtt_sub -h "$(mqtt_host)" -u "$USER" -P "$PASS" -t "${ROOT}/events/standard" -v >"$EVENT_FILE" &
  EVENT_PID="$!"
  sleep 0.3
}

event_log_stop() {
  if [[ -n "$EVENT_PID" ]]; then
    kill "$EVENT_PID" 2>/dev/null || true
    wait "$EVENT_PID" 2>/dev/null || true
    EVENT_PID=""
  fi
}

cleanup() {
  event_log_stop
  if [[ -n "$EVENT_FILE" ]]; then
    rm -f "$EVENT_FILE"
  fi
}

trap cleanup EXIT

wait_event() {
  local label="$1"
  local jq_expr="$2"
  local timeout_s="${3:-10}"
  local end=$((SECONDS + timeout_s))
  local line=""
  local payload=""
  while (( SECONDS < end )); do
    if [[ -n "$EVENT_FILE" && -f "$EVENT_FILE" ]]; then
      while IFS= read -r line; do
        payload="${line#* }"
        if [[ -n "$payload" ]] && jq -e "$jq_expr" >/dev/null <<<"$payload" 2>/dev/null; then
          printf 'PASS %-24s %s\n' "$label" "$(jq -c '{type,event,reason,source}' <<<"$payload")"
          return 0
        fi
      done <"$EVENT_FILE"
    fi
    sleep 1
  done
  printf 'FAIL %-24s no matching event\n' "$label" >&2
  if [[ -n "$EVENT_FILE" && -f "$EVENT_FILE" ]]; then
    sed 's/^[^ ]* //' "$EVENT_FILE" >&2
  fi
  return 1
}

wait_state() {
  local label="$1"
  local jq_expr="$2"
  local timeout_s="${3:-30}"
  local end=$((SECONDS + timeout_s))
  local last=""
  while (( SECONDS < end )); do
    last="$(state_once || true)"
    if [[ -n "$last" ]] && jq -e "$jq_expr" >/dev/null <<<"$last"; then
      printf 'PASS %-24s %s\n' "$label" "$(jq -c '{workflow,strategy,remote_state,simulation,probes,dc_outputs,relays,program}' <<<"$last")"
      return 0
    fi
  done
  printf 'FAIL %-24s %s\n' "$label" "$last" >&2
  return 1
}

safe_off() {
  pub '{"action":"stop"}'
  sleep 1
  pub '{"simulation":{"enabled":false}}'
  wait_state "safe/off" '.remote_state=="RDY" and .program.running==false and .dc_outputs.dc1.power==0 and .dc_outputs.dc1.target_power==0 and .relays.rl1.state==false and .relays.rl2.state==false' 12
}

case_accel_transition() {
  echo
  echo "CASE B accel transition"
  safe_off >/dev/null
  event_log_start
  pub '{"distillation":{"acceleration_enabled":true,"acceleration_end_temp":126,"acceleration_power":100,"run_power":35,"timer_start_temp":126,"timer_s":120,"finish_action":"continue","finish_temp":0,"finish_temp_probe":"probe1","acceleration_relays_enabled":true},"dc1_mode":"element","dc2_mode":"off","rl1_mode":"acc_element","rl2_mode":"remote_other"}'
  pub '{"simulation":{"enabled":true,"scenario":"distillation","duration_s":24,"probe1_start":123,"probe1_end":145,"probe2_start":75,"probe2_end":90}}'
  pub '{"workflow":"distillation","strategy":"program","action":"start"}'
  wait_state "accel active" '.strategy=="program" and .program.running==true and .dc_outputs.dc1.power==100 and .relays.rl1.state==true' 15
  wait_state "run after accel" '.strategy=="program" and .program.running==true and .dc_outputs.dc1.power==35 and .dc_outputs.dc1.target_power==35 and .relays.rl1.state==false' 25
  wait_event "event accel complete" '.type=="accel_complete"' 3
  wait_state "timer started" '.program.timer_remaining_s < 120' 10
  wait_event "event timer started" '.type=="timer_started"' 3
  event_log_stop
  safe_off >/dev/null
}

case_timer_end() {
  echo
  echo "CASE C timer END"
  safe_off >/dev/null
  event_log_start
  pub '{"distillation":{"acceleration_enabled":true,"acceleration_end_temp":126,"acceleration_power":100,"run_power":35,"timer_start_temp":126,"timer_s":5,"finish_action":"end","finish_temp":0,"finish_temp_probe":"probe1","acceleration_relays_enabled":true},"dc1_mode":"element","dc2_mode":"off","rl1_mode":"acc_element","rl2_mode":"remote_other"}'
  pub '{"simulation":{"enabled":true,"scenario":"distillation","duration_s":20,"probe1_start":123,"probe1_end":145,"probe2_start":75,"probe2_end":90}}'
  pub '{"workflow":"distillation","strategy":"program","action":"start"}'
  wait_state "timer running" '.program.running==true and .program.timer_remaining_s < 5' 20
  wait_state "timer ended safe" '.program.running==false and .program.ended==true and .dc_outputs.dc1.power==0 and .dc_outputs.dc1.target_power==0 and .relays.rl1.state==false and .remote_state=="RDY"' 15
  wait_event "event timer end" '.type=="program_ended" and .reason=="finish_timer"' 3
  event_log_stop
  pub '{"action":"reset"}'
  safe_off >/dev/null
}

case_finish_probe1() {
  echo
  echo "CASE D finish temp probe1"
  safe_off >/dev/null
  event_log_start
  pub '{"distillation":{"acceleration_enabled":false,"timer_s":300,"finish_temp":130,"finish_temp_probe":"probe1","finish_action":"end"},"dc1_mode":"element","dc2_mode":"off","rl1_mode":"remote_other","rl2_mode":"remote_other"}'
  pub '{"simulation":{"enabled":true,"scenario":"distillation","duration_s":20,"probe1_start":123,"probe1_end":145,"probe2_start":75,"probe2_end":90}}'
  pub '{"workflow":"distillation","strategy":"program","action":"start"}'
  wait_state "probe1 finish" '.program.running==false and .program.ended==true and .dc_outputs.dc1.power==0 and .dc_outputs.dc1.target_power==0 and .program.finish_temp_probe=="probe1"' 25
  wait_event "event probe1 end" '.type=="program_ended" and .reason=="finish_temp"' 3
  event_log_stop
  pub '{"action":"reset"}'
  safe_off >/dev/null
}

case_finish_probe2() {
  echo
  echo "CASE E finish temp probe2"
  safe_off >/dev/null
  event_log_start
  pub '{"distillation":{"acceleration_enabled":false,"timer_s":300,"finish_temp":88,"finish_temp_probe":"probe2","finish_action":"end"},"dc1_mode":"element","dc2_mode":"off","rl1_mode":"remote_other","rl2_mode":"remote_other"}'
  pub '{"simulation":{"enabled":true,"scenario":"distillation","duration_s":20,"probe1_start":123,"probe1_end":135,"probe2_start":75,"probe2_end":95}}'
  pub '{"workflow":"distillation","strategy":"program","action":"start"}'
  wait_state "probe2 finish" '.program.running==false and .program.ended==true and .dc_outputs.dc1.power==0 and .dc_outputs.dc1.target_power==0 and .program.finish_temp_probe=="probe2"' 25
  wait_event "event probe2 end" '.type=="program_ended" and .reason=="finish_temp"' 3
  event_log_stop
  pub '{"action":"reset"}'
  safe_off >/dev/null
}

case_cycle_relay() {
  echo
  echo "CASE F cycle relay"
  safe_off >/dev/null
  pub '{"rl2_mode":"cycle","rl2_on_ms":1000,"rl2_cycle_ms":3000}'
  pub '{"workflow":"distillation","strategy":"manual","action":"start"}'
  pub '{"rl2":true}'
  wait_state "cycle engaged" '.workflow=="distillation" and .strategy=="manual" and .relays.rl2.mode=="cycle" and .relays.rl2.engaged==true' 10
  wait_state "cycle on" '.workflow=="distillation" and .strategy=="manual" and .relays.rl2.mode=="cycle" and .relays.rl2.engaged==true and .relays.rl2.state==true' 8
  wait_state "cycle off blink" '.workflow=="distillation" and .strategy=="manual" and .relays.rl2.mode=="cycle" and .relays.rl2.engaged==true and .relays.rl2.state==false' 8
  pub '{"rl2":false}'
  wait_state "cycle disengaged" '.relays.rl2.mode=="cycle" and .relays.rl2.engaged==false and .relays.rl2.state==false' 10
  safe_off >/dev/null
}

case_accel_transition
case_timer_end
case_finish_probe1
case_finish_probe2
case_cycle_relay
safe_off

echo
echo "Simulation bench protocol complete."
