#!/usr/bin/env python3
"""Smoke-test the ProofPro MQTT schema v2 contract against a broker.

This script does not flash firmware. By default it only requests retained
status/config and verifies live state shape. Pass --conflict-test to also
publish one non-energizing conflicting-alias command and require command_error.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


DEFAULT_HOST = "10.0.1.203"
DEFAULT_USER = "proof"
DEFAULT_PASSWORD = "proof"
DEFAULT_TOPIC_ID = "791402d5ac0fe1"
TOPIC_ROOT = "smartpidM5/proofpro"


@dataclass
class MqttConfig:
    host: str
    port: int
    username: str | None
    password: str | None
    topic_id: str
    timeout_s: int

    def topic(self, suffix: str) -> str:
        return f"{TOPIC_ROOT}/{self.topic_id}/{suffix}"


def mqtt_auth_args(cfg: MqttConfig) -> list[str]:
    args = ["-h", cfg.host, "-p", str(cfg.port)]
    if cfg.username:
        args.extend(["-u", cfg.username])
    if cfg.password:
        args.extend(["-P", cfg.password])
    return args


def run_checked(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, text=True, capture_output=True, check=True)
    except FileNotFoundError as exc:
        raise SystemExit(f"missing required command: {cmd[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.strip() or exc.stdout.strip()
        raise SystemExit(f"{cmd[0]} failed: {detail}") from exc


def publish(cfg: MqttConfig, suffix: str, payload: dict[str, Any]) -> None:
    cmd = [
        "mosquitto_pub",
        *mqtt_auth_args(cfg),
        "-t",
        cfg.topic(suffix),
        "-m",
        json.dumps(payload, separators=(",", ":")),
    ]
    run_checked(cmd)


def subscribe_one(cfg: MqttConfig, suffix: str) -> tuple[str, dict[str, Any]]:
    cmd = [
        "mosquitto_sub",
        *mqtt_auth_args(cfg),
        "-t",
        cfg.topic(suffix),
        "-C",
        "1",
        "-W",
        str(cfg.timeout_s),
        "-v",
    ]
    result = run_checked(cmd)
    line = result.stdout.strip().splitlines()[-1]
    try:
        topic, payload = line.split(" ", 1)
    except ValueError as exc:
        raise SystemExit(f"unexpected mosquitto_sub output: {line!r}") from exc
    try:
        return topic, json.loads(payload)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"{topic} did not contain valid JSON: {payload}") from exc


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def assert_absent(obj: dict[str, Any], key: str, path: str) -> None:
    assert_true(key not in obj, f"{path}.{key} must be absent")


def require_object(obj: dict[str, Any], key: str, path: str) -> dict[str, Any]:
    value = obj.get(key)
    assert_true(isinstance(value, dict), f"{path}.{key} must be an object")
    return value


def validate_status(status: dict[str, Any]) -> None:
    assert_true(status.get("firmware") == "proofpro", "status.firmware must be proofpro")
    assert_true(status.get("firmware_version") == "0.3.0", "status.firmware_version must be 0.3.0")
    assert_true(status.get("schema_version") == 2, "status.schema_version must be 2")
    assert_true(status.get("unit") in ("F", "C"), "status.unit must be F or C")
    assert_true("remote_enabled" in status, "status.remote_enabled is required")
    assert_true(status.get("remote_state") in ("OFF", "RDY", "ON"), "status.remote_state is invalid")
    assert_true(status.get("device_state") in ("booting", "idle", "running", "paused", "ended", "safe", "fault"),
                "status.device_state is invalid")
    assert_true("workflow" in status, "status.workflow is required")
    assert_true("strategy" in status, "status.strategy is required")


def validate_config(config: dict[str, Any]) -> None:
    distillation = require_object(config, "distillation", "config")
    assert_absent(config, "program", "config")
    for key in (
        "acceleration_enabled",
        "acceleration_end_temp",
        "acceleration_power",
        "run_power",
        "timer_start_temp",
        "timer_s",
        "finish_temp",
        "finish_temp_probe",
        "finish_action",
        "acceleration_relays_enabled",
    ):
        assert_true(key in distillation, f"config.distillation.{key} is required")
    assert_true(distillation["finish_temp_probe"] in ("probe1", "probe2"),
                "config.distillation.finish_temp_probe must be probe1/probe2")
    assert_true(distillation["finish_action"] in ("end", "continue"),
                "config.distillation.finish_action must be end/continue")

    dc_outputs = require_object(config, "dc_outputs", "config")
    assert_true("dc1" in dc_outputs and "dc2" in dc_outputs, "config.dc_outputs must include dc1/dc2")
    assert_absent(dc_outputs, "DC1", "config.dc_outputs")
    assert_absent(dc_outputs, "DC2", "config.dc_outputs")
    for name in ("dc1", "dc2"):
        mode = require_object(dc_outputs, name, "config.dc_outputs").get("mode")
        assert_true(mode in ("off", "element", "auxiliary"), f"config.dc_outputs.{name}.mode is invalid")

    relays = require_object(config, "relays", "config")
    assert_true("rl1" in relays and "rl2" in relays, "config.relays must include rl1/rl2")
    assert_absent(relays, "CH1", "config.relays")
    assert_absent(relays, "CH2", "config.relays")
    for name in ("rl1", "rl2"):
        relay = require_object(relays, name, "config.relays")
        assert_true(relay.get("mode") in ("off", "manual_on_off", "acc_element", "remote_other", "cycle"),
                    f"config.relays.{name}.mode is invalid")
        assert_true("on_ms" in relay and "cycle_ms" in relay, f"config.relays.{name} timing is required")


def validate_state(state: dict[str, Any], status_unit: str) -> None:
    assert_true(state.get("unit") == status_unit, "state.unit must match status.unit")
    assert_true("device_state" in state, "state.device_state is required")
    assert_true("workflow" in state, "state.workflow is required")
    assert_true("strategy" in state, "state.strategy is required")

    probes = require_object(state, "probes", "state")
    for name in ("probe1", "probe2"):
        probe = require_object(probes, name, "state.probes")
        assert_true("temp" in probe, f"state.probes.{name}.temp is required")
        assert_true("temp_valid" in probe, f"state.probes.{name}.temp_valid is required")
        assert_absent(probe, "unit", f"state.probes.{name}")

    dc_outputs = require_object(state, "dc_outputs", "state")
    for name in ("dc1", "dc2"):
        dc = require_object(dc_outputs, name, "state.dc_outputs")
        assert_true("mode" in dc and "power" in dc and "target_power" in dc,
                    f"state.dc_outputs.{name} is incomplete")

    relays = require_object(state, "relays", "state")
    for name in ("rl1", "rl2"):
        relay = require_object(relays, name, "state.relays")
        assert_true("mode" in relay and "state" in relay and "engaged" in relay,
                    f"state.relays.{name} is incomplete")

    program = require_object(state, "program", "state")
    for key in (
        "running",
        "ended",
        "latched",
        "acc_elements_enabled",
        "finish_temp_source",
        "timer_remaining_s",
        "timer_frozen",
    ):
        assert_true(key in program, f"state.program.{key} is required")
    assert_true(program["finish_temp_source"] in ("probe1", "probe2"),
                "state.program.finish_temp_source must be probe1/probe2")


def wait_for_command_error(cfg: MqttConfig) -> dict[str, Any]:
    _topic, event = subscribe_one(cfg, "events/standard")
    assert_true(event.get("type") == "command_error", "expected command_error event")
    assert_true(event.get("reason") == "conflicting_alias", "expected conflicting_alias reason")
    return event


def run_smoke(cfg: MqttConfig, conflict_test: bool) -> None:
    for command in ("mosquitto_pub", "mosquitto_sub"):
        if not shutil.which(command):
            raise SystemExit(f"{command} is required")

    print(f"Requesting retained status/config from {cfg.topic('commands')}")
    publish(cfg, "commands", {"status": True})

    print("Checking retained status")
    _topic, status = subscribe_one(cfg, "status")
    validate_status(status)

    print("Checking retained config")
    _topic, config = subscribe_one(cfg, "config")
    validate_config(config)

    print("Checking live state")
    _topic, state = subscribe_one(cfg, "state")
    validate_state(state, status["unit"])

    if conflict_test:
        print("Checking conflicting alias command_error")
        publish(cfg, "commands", {"dc1_power": 10, "CH1 power": 20})
        wait_for_command_error(cfg)

    print("ProofPro MQTT schema v2 smoke test passed")


def main() -> int:
    parser = argparse.ArgumentParser(description="Smoke-test ProofPro MQTT schema v2.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--user", default=DEFAULT_USER)
    parser.add_argument("--password", default=DEFAULT_PASSWORD)
    parser.add_argument("--topic-id", default=DEFAULT_TOPIC_ID)
    parser.add_argument("--timeout", type=int, default=10, help="mosquitto_sub timeout in seconds")
    parser.add_argument("--conflict-test", action="store_true",
                        help="publish a non-energizing conflicting-alias command and require command_error")
    args = parser.parse_args()

    cfg = MqttConfig(
        host=args.host,
        port=args.port,
        username=args.user or None,
        password=args.password or None,
        topic_id=args.topic_id,
        timeout_s=args.timeout,
    )

    try:
        run_smoke(cfg, args.conflict_test)
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
