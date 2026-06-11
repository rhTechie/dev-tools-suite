#!/usr/bin/env python3
"""Run a command and validate output with required/forbidden keywords."""

from __future__ import annotations


def run(context, config):
    command = config.get("command")
    if not command:
        context.fail("keyword_check requires config.command")

    pre_commands = list(config.get("pre_commands", []))
    capture_mode = config.get("capture_mode", "until_prompt")
    wait_seconds = float(config.get("wait_seconds", 0.5))
    timeout_seconds = float(config.get("timeout_seconds", context.connection["command_timeout"]))
    required_keywords = list(config.get("required_keywords", []))
    forbidden_keywords = list(config.get("forbidden_keywords", []))
    match_all_required = bool(config.get("match_all_required", True))

    for pre_command in pre_commands:
        context.logger.log(f"keyword_check pre_command: {pre_command}")
        pre_result = context.run_command(
            pre_command,
            capture_mode="until_prompt",
            wait_seconds=wait_seconds,
            timeout_seconds=timeout_seconds,
        )
        pre_output = pre_result["output"]
        if pre_output:
            context.logger.log_block("keyword_check pre_command output", pre_output)

    result = context.run_command(
        command,
        capture_mode=capture_mode,
        wait_seconds=wait_seconds,
        timeout_seconds=timeout_seconds,
    )
    output = result["output"]

    context.logger.log_block("keyword_check command output", output or "<empty>")

    if required_keywords:
        if match_all_required:
            missing = [item for item in required_keywords if item not in output]
            if missing:
                context.fail(
                    f"output missing required keywords: {missing}",
                    {
                        "command": command,
                        "required_keywords": required_keywords,
                        "missing_keywords": missing,
                        "output": output,
                    },
                )
        else:
            if not any(item in output for item in required_keywords):
                context.fail(
                    "output did not contain any required keyword",
                    {
                        "command": command,
                        "required_keywords": required_keywords,
                        "output": output,
                    },
                )

    hit_forbidden = [item for item in forbidden_keywords if item in output]
    if hit_forbidden:
        context.fail(
            f"output contains forbidden keywords: {hit_forbidden}",
            {
                "command": command,
                "forbidden_keywords": forbidden_keywords,
                "hit_forbidden_keywords": hit_forbidden,
                "output": output,
            },
        )

    success_hint = config.get("success_message")
    if success_hint:
        context.logger.log(success_hint)

    return None
