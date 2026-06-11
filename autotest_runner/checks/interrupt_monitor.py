#!/usr/bin/env python3
"""IRQ interrupt counter monitor check."""

from __future__ import annotations

import re


def run(context, config):
    command = config.get("command", "ints")
    irq_name = config.get("irq_name")
    capture_mode = config.get("capture_mode", "fixed_wait")
    wait_seconds = float(config.get("wait_seconds", 0.5))
    timeout_seconds = float(config.get("timeout_seconds", context.connection["command_timeout"]))
    sample_interval_seconds = float(config.get("sample_interval_seconds", 1))
    samples = int(config.get("samples", 2))

    if not irq_name:
        context.fail("interrupt_monitor requires config.irq_name")
    if samples < 2:
        context.fail("interrupt_monitor requires config.samples >= 2")

    explicit_pattern = config.get("pattern")
    if explicit_pattern:
        pattern = explicit_pattern
    else:
        pattern = (
            rf"{re.escape(irq_name)}\s+.+?\s+\d+\s+(\d+)\s+\d+\s+\d+\s+\d+"
        )

    extracted_values = []
    outputs = []

    for index in range(samples):
        result = context.run_command(
            command,
            capture_mode=capture_mode,
            wait_seconds=wait_seconds,
            timeout_seconds=timeout_seconds,
        )
        output = result["output"]
        outputs.append(output)
        context.logger.log_block(
            f"interrupt_monitor sample {index + 1}/{samples} output",
            output or "<empty>",
        )

        match = re.search(pattern, output, re.DOTALL)
        if not match:
            context.fail(
                f"failed to extract IRQ count for {irq_name}",
                {
                    "irq_name": irq_name,
                    "command": command,
                    "sample_index": index + 1,
                    "output": output,
                    "pattern": pattern,
                },
            )

        value = int(match.group(1))
        extracted_values.append(value)
        context.logger.log(f"interrupt_monitor sample {index + 1}: {irq_name}={value}")

        if index + 1 < samples:
            context.sleep(sample_interval_seconds)

    if extracted_values[0] == extracted_values[-1]:
        context.fail(
            f"IRQ {irq_name} did not change between samples: {extracted_values[0]} -> {extracted_values[-1]}",
            {
                "irq_name": irq_name,
                "command": command,
                "values": extracted_values,
                "outputs": outputs,
            },
        )

    return None
