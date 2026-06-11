#!/usr/bin/env python3
"""Telnet autotest runner with pluggable check scripts."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import socket
import sys
import time
from dataclasses import dataclass
from typing import Any


class ConfigError(Exception):
    """Raised when the config file is invalid."""


class RunnerFailure(Exception):
    """Raised when the autotest should stop immediately."""

    def __init__(
        self,
        message: str,
        *,
        round_index: int | None = None,
        check_name: str | None = None,
        details: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.round_index = round_index
        self.check_name = check_name
        self.details = details or {}


class CheckFailure(Exception):
    """Raised by external check scripts to stop the runner."""

    def __init__(self, message: str, details: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.details = details or {}


@dataclass
class CheckResult:
    passed: bool
    message: str = ""
    details: dict[str, Any] | None = None

    @classmethod
    def ok(cls, message: str = "", details: dict[str, Any] | None = None) -> "CheckResult":
        return cls(True, message, details or {})

    @classmethod
    def fail(cls, message: str, details: dict[str, Any] | None = None) -> "CheckResult":
        return cls(False, message, details or {})


class Logger:
    """Simple console + file logger."""

    def __init__(self, log_path: str) -> None:
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        self.log_path = log_path
        self._fp = open(log_path, "a", encoding="utf-8")

    def log(self, message: str) -> None:
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{timestamp}] {message}"
        print(line, flush=True)
        self._fp.write(line + "\n")
        self._fp.flush()

    def log_block(self, header: str, content: str) -> None:
        self.log(header)
        for line in content.splitlines() or ["<empty>"]:
            self.log(f"  {line}")

    def close(self) -> None:
        self._fp.close()


def resolve_path(base_dir: str, path_value: str) -> str:
    if os.path.isabs(path_value):
        return path_value
    return os.path.abspath(os.path.join(base_dir, path_value))


def show_windows_popup(title: str, message: str) -> bool:
    if os.name != "nt":
        return False

    try:
        import ctypes

        ctypes.windll.user32.MessageBoxW(0, message, title, 0x1000 | 0x40000 | 0x30 | 0x01)
        return True
    except Exception:
        return False


def load_json_file(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as fp:
        return json.load(fp)


def load_checks_from_file(config_dir: str, checks_file_value: str) -> list[dict[str, Any]]:
    checks_path = resolve_path(config_dir, checks_file_value)
    checks_data = load_json_file(checks_path)

    if isinstance(checks_data, dict):
        checks = checks_data.get("checks")
    else:
        checks = checks_data

    if not isinstance(checks, list) or not checks:
        raise ConfigError(f"checks file must contain a non-empty checks array: {checks_path}")

    return checks


def load_config(config_path: str) -> dict[str, Any]:
    config = load_json_file(config_path)

    if not isinstance(config, dict):
        raise ConfigError("config root must be a JSON object")

    config_dir = os.path.dirname(os.path.abspath(config_path))
    connection = config.setdefault("connection", {})
    runtime = config.setdefault("runtime", {})
    alert = config.setdefault("alert", {})
    checks = config.get("checks")
    checks_file = config.get("checks_file")

    if not isinstance(connection, dict):
        raise ConfigError("connection must be an object")
    if not isinstance(runtime, dict):
        raise ConfigError("runtime must be an object")
    if not isinstance(alert, dict):
        raise ConfigError("alert must be an object")

    if checks_file is not None:
        if checks is not None:
            raise ConfigError("config cannot define both checks and checks_file")
        if not isinstance(checks_file, str) or not checks_file.strip():
            raise ConfigError("checks_file must be a non-empty string")
        checks = load_checks_from_file(config_dir, checks_file.strip())
        config["checks"] = checks

    if not isinstance(checks, list) or not checks:
        raise ConfigError("checks must be a non-empty array")

    required_connection_fields = ["host", "username", "password"]
    for field_name in required_connection_fields:
        if not connection.get(field_name):
            raise ConfigError(f"connection.{field_name} is required")

    connection.setdefault("port", 23)
    connection.setdefault("login_prompt", "login: ")
    connection.setdefault("password_prompt", "password: ")
    connection.setdefault("shell_prompt", "#")
    connection.setdefault("connect_timeout", 15)
    connection.setdefault("read_timeout", 5)
    connection.setdefault("command_timeout", 10)
    connection.setdefault("post_login_wait_seconds", 1.0)
    connection.setdefault("encoding", "utf-8")
    connection.setdefault("command_terminator", "\n")
    connection.setdefault("drain_wait_seconds", 0.15)
    connection.setdefault(
        "connect_retry_count",
        int(runtime.get("connect_retry_count", 3)),
    )
    connection.setdefault(
        "connect_retry_interval_seconds",
        float(runtime.get("connect_retry_interval_seconds", 2)),
    )

    runtime.setdefault("round_interval_seconds", 60)
    runtime.setdefault("max_rounds", 0)

    alert.setdefault("enable_windows_popup", False)
    alert.setdefault("title", "Autotest Runner Alert")
    alert.setdefault("log_dir", "logs")
    alert.setdefault("failure_output_dir", os.path.join("logs", "failures"))

    seen_names: set[str] = set()
    for index, check in enumerate(checks, start=1):
        if not isinstance(check, dict):
            raise ConfigError(f"checks[{index - 1}] must be an object")

        check.setdefault("name", f"check_{index}")
        check.setdefault("enabled", True)
        check.setdefault("config", {})

        if not check.get("script"):
            raise ConfigError(f"checks[{index - 1}].script is required")
        if check["name"] in seen_names:
            raise ConfigError(f"duplicate check name: {check['name']}")
        if not isinstance(check["config"], dict):
            raise ConfigError(f"checks[{index - 1}].config must be an object")
        seen_names.add(check["name"])

    return config


def clean_command_output(raw_text: str, command: str, prompt: str | None) -> str:
    text = raw_text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")

    while lines and not lines[0].strip():
        lines.pop(0)

    if command and lines and lines[0].strip() == command.strip():
        lines.pop(0)

    while lines and not lines[-1].strip():
        lines.pop()

    if prompt and lines:
        prompt_marker = prompt.strip()
        tail = lines[-1].strip()
        if tail == prompt_marker:
            lines.pop()
        elif prompt_marker and tail.endswith(prompt_marker) and len(tail) <= len(prompt_marker) + 32:
            lines.pop()

    return "\n".join(lines).strip()


class RawTelnetClient:
    """Small Telnet client without telnetlib dependency."""

    IAC = 255
    DONT = 254
    DO = 253
    WONT = 252
    WILL = 251
    SB = 250
    SE = 240

    def __init__(
        self,
        host: str,
        port: int,
        *,
        connect_timeout: float,
        read_timeout: float,
        encoding: str,
    ) -> None:
        self.host = host
        self.port = port
        self.encoding = encoding
        self.sock = socket.create_connection((host, port), timeout=connect_timeout)
        self.sock.settimeout(read_timeout)
        self._display_buffer = bytearray()

    def close(self) -> None:
        self.sock.close()

    def write_text(self, text: str) -> None:
        self.sock.sendall(text.encode(self.encoding))

    def read_until_text(self, marker: str, timeout: float) -> str:
        marker_bytes = marker.encode(self.encoding)
        deadline = time.time() + timeout

        while time.time() < deadline:
            index = self._display_buffer.find(marker_bytes)
            if index != -1:
                end = index + len(marker_bytes)
                data = bytes(self._display_buffer[:end])
                del self._display_buffer[:end]
                return data.decode(self.encoding, errors="ignore")

            self._recv_once(deadline)

        raise TimeoutError(f"timed out waiting for marker {marker!r}")

    def read_available_text(self, quiet_seconds: float) -> str:
        deadline = time.time() + quiet_seconds
        chunks: list[bytes] = []

        if self._display_buffer:
            chunks.append(bytes(self._display_buffer))
            self._display_buffer.clear()

        while time.time() < deadline:
            remaining = deadline - time.time()
            self.sock.settimeout(max(min(remaining, 0.2), 0.05))
            try:
                payload = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not payload:
                break
            display = self._consume_telnet_bytes(payload)
            if display:
                chunks.append(display)

        return b"".join(chunks).decode(self.encoding, errors="ignore")

    def _recv_once(self, deadline: float) -> None:
        remaining = deadline - time.time()
        if remaining <= 0:
            return

        self.sock.settimeout(max(min(remaining, 0.5), 0.05))
        try:
            payload = self.sock.recv(4096)
        except socket.timeout:
            return
        if not payload:
            raise EOFError("remote closed Telnet connection")

        display = self._consume_telnet_bytes(payload)
        if display:
            self._display_buffer.extend(display)

    def _consume_telnet_bytes(self, payload: bytes) -> bytes:
        reply = bytearray()
        display = bytearray()
        index = 0

        while index < len(payload):
            byte = payload[index]
            if byte != self.IAC:
                display.append(byte)
                index += 1
                continue

            if index + 1 >= len(payload):
                break

            command = payload[index + 1]
            if command in (self.DO, self.DONT, self.WILL, self.WONT):
                if index + 2 >= len(payload):
                    break
                option = payload[index + 2]
                if command == self.DO:
                    reply.extend([self.IAC, self.WONT, option])
                elif command == self.WILL:
                    reply.extend([self.IAC, self.DONT, option])
                index += 3
                continue

            if command == self.SB:
                seek = index + 2
                while seek + 1 < len(payload):
                    if payload[seek] == self.IAC and payload[seek + 1] == self.SE:
                        break
                    seek += 1
                index = seek + 2 if seek + 1 < len(payload) else len(payload)
                continue

            if command == self.IAC:
                display.append(self.IAC)
                index += 2
                continue

            index += 2

        if reply:
            self.sock.sendall(bytes(reply))
        return bytes(display)


class RunnerContext:
    """Exposed to pluggable check scripts."""

    def __init__(
        self,
        *,
        client: RawTelnetClient,
        connection: dict[str, Any],
        logger: Logger,
        state: dict[str, Any],
    ) -> None:
        self.client = client
        self.connection = connection
        self.logger = logger
        self.state = state

    def run_command(
        self,
        command: str,
        *,
        capture_mode: str = "until_prompt",
        wait_seconds: float = 0.5,
        timeout_seconds: float | None = None,
        strip_prompt: bool = True,
    ) -> dict[str, str]:
        self.drain_unsolicited_output()
        self.client.write_text(command + self.connection["command_terminator"])

        if timeout_seconds is None:
            timeout_seconds = float(self.connection["command_timeout"])

        if capture_mode == "until_prompt":
            prompt = self.connection.get("shell_prompt")
            if not prompt:
                raise RunnerFailure("connection.shell_prompt is required for until_prompt mode")
            raw_text = self.client.read_until_text(prompt, timeout_seconds)
            output = clean_command_output(raw_text, command, prompt if strip_prompt else None)
        elif capture_mode == "fixed_wait":
            time.sleep(wait_seconds)
            raw_text = self.client.read_available_text(float(self.connection["drain_wait_seconds"]))
            output = clean_command_output(raw_text, command, self.connection.get("shell_prompt"))
        else:
            raise ConfigError(f"unsupported capture_mode: {capture_mode}")

        return {
            "command": command,
            "raw_output": raw_text,
            "output": output,
        }

    def drain_unsolicited_output(self) -> str:
        text = self.client.read_available_text(float(self.connection["drain_wait_seconds"])).strip()
        if text:
            self.logger.log_block("Unsolicited output:", text)
        return text

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)

    def fail(self, message: str, details: dict[str, Any] | None = None) -> None:
        raise CheckFailure(message, details)

    def get_state(self, namespace: str, default: Any = None) -> Any:
        return self.state.get(namespace, default)

    def set_state(self, namespace: str, value: Any) -> None:
        self.state[namespace] = value


class CheckModule:
    """Loaded check script wrapper."""

    def __init__(self, name: str, path: str) -> None:
        self.name = name
        self.path = path
        self.module = self._load(path)

        if not hasattr(self.module, "run"):
            raise ConfigError(f"check script {path} must export run(context, config)")

    def run(self, context: RunnerContext, config: dict[str, Any]) -> CheckResult:
        result = self.module.run(context, config)
        if result is None:
            return CheckResult.ok()
        if isinstance(result, CheckResult):
            return result
        if isinstance(result, bool):
            return CheckResult.ok() if result else CheckResult.fail("check returned False")
        raise ConfigError(
            f"check script {self.path} returned unsupported result type: {type(result).__name__}"
        )

    def _load(self, path: str) -> Any:
        module_name = f"autotest_check_{self.name}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise ConfigError(f"failed to load check script: {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


class AutotestRunner:
    """Long-lived Telnet runner with pluggable checks."""

    def __init__(
        self,
        config: dict[str, Any],
        config_path: str,
        *,
        selected_check_names: list[str] | None = None,
    ) -> None:
        self.config = config
        self.connection = config["connection"]
        self.runtime = config["runtime"]
        self.alert = config["alert"]
        self.selected_check_names = selected_check_names or []
        self.checks = self._select_checks(config["checks"])
        self.config_path = os.path.abspath(config_path)
        self.base_dir = os.path.dirname(self.config_path)
        self.session_started_at = time.time()
        self.client: RawTelnetClient | None = None
        self.shared_state: dict[str, Any] = {}
        self.loaded_checks = self._load_checks()

        log_dir = resolve_path(self.base_dir, self.alert["log_dir"])
        session_stamp = time.strftime("%Y%m%d_%H%M%S")
        self.log_path = os.path.join(log_dir, f"autotest_runner_{session_stamp}.log")
        self.logger = Logger(self.log_path)

    def _select_checks(self, all_checks: list[dict[str, Any]]) -> list[dict[str, Any]]:
        enabled_checks = [item for item in all_checks if item.get("enabled", True)]
        if not self.selected_check_names:
            return enabled_checks

        check_map = {item["name"]: item for item in enabled_checks}
        missing = [name for name in self.selected_check_names if name not in check_map]
        if missing:
            raise ConfigError(f"selected check name(s) not found or disabled: {', '.join(missing)}")

        return [check_map[name] for name in self.selected_check_names]

    def _load_checks(self) -> list[tuple[dict[str, Any], CheckModule]]:
        loaded: list[tuple[dict[str, Any], CheckModule]] = []
        for item in self.checks:
            script_path = resolve_path(self.base_dir, item["script"])
            if not os.path.exists(script_path):
                raise ConfigError(f"check script not found: {script_path}")
            loaded.append((item, CheckModule(item["name"], script_path)))
        return loaded

    def connect(self) -> None:
        retries = int(self.connection["connect_retry_count"])
        retry_interval = float(self.connection["connect_retry_interval_seconds"])
        last_error: Exception | None = None

        for attempt in range(1, retries + 1):
            if attempt > 1:
                self.logger.log(
                    f"Retrying Telnet connect in {retry_interval} seconds "
                    f"(attempt {attempt}/{retries})"
                )
                time.sleep(retry_interval)

            try:
                self._connect_once()
                return
            except (TimeoutError, OSError, EOFError, RunnerFailure) as exc:
                last_error = exc
                self.logger.log(f"Connect attempt {attempt}/{retries} failed: {exc}")

        raise RunnerFailure(f"Failed to connect after {retries} attempts: {last_error}")

    def _connect_once(self) -> None:
        host = self.connection["host"]
        port = int(self.connection["port"])
        self.logger.log(f"Connecting to {host}:{port}")

        self.client = RawTelnetClient(
            host,
            port,
            connect_timeout=float(self.connection["connect_timeout"]),
            read_timeout=float(self.connection["read_timeout"]),
            encoding=self.connection["encoding"],
        )

        login_prompt = self.connection.get("login_prompt")
        if login_prompt:
            self.client.read_until_text(login_prompt, float(self.connection["connect_timeout"]))
        self.client.write_text(self.connection["username"] + self.connection["command_terminator"])

        password_prompt = self.connection.get("password_prompt")
        if password_prompt:
            self.client.read_until_text(password_prompt, float(self.connection["connect_timeout"]))
        self.client.write_text(self.connection["password"] + self.connection["command_terminator"])

        time.sleep(float(self.connection["post_login_wait_seconds"]))

        shell_prompt = self.connection.get("shell_prompt")
        if shell_prompt:
            banner = self.client.read_until_text(shell_prompt, float(self.connection["command_timeout"]))
            cleaned = clean_command_output(banner, "", shell_prompt)
            if cleaned:
                self.logger.log_block("Login banner/output:", cleaned)

        drained = self.client.read_available_text(float(self.connection["drain_wait_seconds"])).strip()
        if drained:
            self.logger.log_block("Drained pending output after login:", drained)

        self.logger.log("Telnet login succeeded")

    def close(self) -> None:
        if self.client is None:
            return

        try:
            self.client.write_text("quit" + self.connection["command_terminator"])
            time.sleep(0.2)
        except Exception:
            pass
        finally:
            try:
                self.client.close()
            except Exception:
                pass
            self.client = None

    def run(self) -> int:
        round_index = 0
        max_rounds = int(self.runtime["max_rounds"])

        try:
            self.connect()

            self.logger.log("=" * 60)
            self.logger.log(f"Session log: {self.log_path}")
            self.logger.log(f"Config path: {self.config_path}")
            if self.selected_check_names:
                self.logger.log(f"Selected checks: {', '.join(self.selected_check_names)}")
            self.logger.log("Autotest runner started")
            self.logger.log("=" * 60)

            while max_rounds <= 0 or round_index < max_rounds:
                round_index += 1
                self.logger.log("")
                self.logger.log(f"Round {round_index} started")

                for check_config, check_module in self.loaded_checks:
                    self.run_check(round_index, check_config, check_module)

                self.logger.log(f"Round {round_index} passed")

                if max_rounds > 0 and round_index >= max_rounds:
                    break

                sleep_seconds = float(self.runtime["round_interval_seconds"])
                self.logger.log(f"Sleeping {sleep_seconds} seconds before next round")
                time.sleep(sleep_seconds)

            elapsed = time.time() - self.session_started_at
            self.logger.log(f"Completed normally after {round_index} rounds in {elapsed:.1f} seconds")
            return 0

        except KeyboardInterrupt:
            elapsed = time.time() - self.session_started_at
            self.logger.log(f"Stopped by user after {round_index} rounds in {elapsed:.1f} seconds")
            return 130

        except RunnerFailure as exc:
            elapsed = time.time() - self.session_started_at
            self.handle_failure(exc, elapsed)
            return 1

        except ConfigError as exc:
            self.logger.log(f"Config error during runtime: {exc}")
            return 2

        finally:
            self.close()
            self.logger.close()

    def run_check(self, round_index: int, check_config: dict[str, Any], check_module: CheckModule) -> None:
        if self.client is None:
            raise RunnerFailure("Telnet connection is not established")

        check_name = check_config["name"]
        self.logger.log(f"Check {check_name}: script={check_config['script']}")

        context = RunnerContext(
            client=self.client,
            connection=self.connection,
            logger=self.logger,
            state=self.shared_state,
        )

        try:
            result = check_module.run(context, check_config.get("config", {}))
        except CheckFailure as exc:
            raise RunnerFailure(
                str(exc),
                round_index=round_index,
                check_name=check_name,
                details=exc.details,
            ) from exc
        except (TimeoutError, OSError, EOFError) as exc:
            raise RunnerFailure(
                f"check {check_name} hit Telnet I/O error: {exc}",
                round_index=round_index,
                check_name=check_name,
            ) from exc

        if not result.passed:
            raise RunnerFailure(
                result.message or f"check {check_name} failed",
                round_index=round_index,
                check_name=check_name,
                details=result.details or {},
            )

        if result.message:
            self.logger.log(f"Check {check_name}: {result.message}")
        self.logger.log(f"Check {check_name}: passed")

    def handle_failure(self, exc: RunnerFailure, elapsed: float) -> None:
        detail_lines = [f"Test stopped after {elapsed:.1f} seconds"]
        if exc.round_index is not None:
            detail_lines.append(f"round={exc.round_index}")
        if exc.check_name:
            detail_lines.append(f"check={exc.check_name}")
        detail_lines.append(f"reason={exc}")
        detail_message = ", ".join(detail_lines)

        self.logger.log("")
        self.logger.log("=" * 60)
        self.logger.log("AUTOTEST FAILURE")
        self.logger.log(detail_message)

        snapshot_path = self.save_failure_snapshot(exc)
        if snapshot_path:
            self.logger.log(f"Failure snapshot: {snapshot_path}")

        self.logger.log("=" * 60)

        if self.alert.get("enable_windows_popup", False):
            popup_message = detail_message
            if snapshot_path:
                popup_message += f"\n\nSnapshot: {snapshot_path}"
            shown = show_windows_popup(self.alert["title"], popup_message)
            if not shown:
                self.logger.log("Windows popup requested but not available on this platform")

    def save_failure_snapshot(self, exc: RunnerFailure) -> str:
        failure_dir = resolve_path(self.base_dir, self.alert["failure_output_dir"])
        os.makedirs(failure_dir, exist_ok=True)

        timestamp = time.strftime("%Y%m%d_%H%M%S")
        check_fragment = exc.check_name or "session"
        file_name = f"failure_{timestamp}_{check_fragment}.json"
        snapshot_path = os.path.join(failure_dir, file_name)

        payload: dict[str, Any] = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "config_path": self.config_path,
            "reason": str(exc),
            "round_index": exc.round_index,
            "check_name": exc.check_name,
            "log_path": self.log_path,
            "details": exc.details,
        }

        with open(snapshot_path, "w", encoding="utf-8") as fp:
            json.dump(payload, fp, ensure_ascii=False, indent=2)

        return snapshot_path


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Long-lived Telnet autotest runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 autotest_runner.py
  python3 autotest_runner.py interrupt_monitor_template
  python3 autotest_runner.py irq_uart2 eth_verify
  python3 autotest_runner.py -c config.all.json eth_verify
        """,
    )
    parser.add_argument(
        "check_names",
        nargs="*",
        help="optional check name list; if omitted, run all enabled checks in the config",
    )
    parser.add_argument(
        "-c",
        "--config",
        default="config.json",
        help="path to JSON config file (default: ./config.json)",
    )
    return parser


def main() -> int:
    parser = build_argument_parser()
    args = parser.parse_args()

    try:
        config = load_config(args.config)
    except (OSError, json.JSONDecodeError, ConfigError) as exc:
        print(f"Config error: {exc}", file=sys.stderr)
        return 2

    runner = AutotestRunner(config, args.config, selected_check_names=args.check_names)
    return runner.run()


if __name__ == "__main__":
    sys.exit(main())
