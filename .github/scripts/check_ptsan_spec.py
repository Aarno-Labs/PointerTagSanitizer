#!/usr/bin/env python3
"""Read CPU2017 evidence and check explicit input-specific expectations."""
from pathlib import Path
import re
import shlex

EXHAUSTION = "PTSan: ID space exhausted in strict mode"
MUNMAP = ("PTSan rejected unsupported munmap before syscall: nonempty ranges "
          "must exactly match one tracked mmap allocation")
ACCESS = re.compile(r"Found bad \w+ access\b")


def diagnostic(line):
    line = line.strip()
    if line == EXHAUSTION:
        return "exhaustion"
    if line == MUNMAP:
        return "munmap"
    if ACCESS.match(line):
        return "access"
    if "PTSan" in line or "Found bad" in line:
        return "unknown"
    return None


def collect_execution(run):
    """Keep commands/statuses and diagnostic lines, not licensed input/output data."""
    command_text = (run / "speccmds.cmd").read_text()
    commands = []
    command_outputs = []
    for line in command_text.splitlines():
        # Environment and directory directives are not benchmark invocations.
        if not line.startswith(("-o ", "-e ", "-i ")):
            continue
        commands.append(line)
        outputs = set()
        command_outputs.append(outputs)
        words = shlex.split(line)
        # Only the leading options belong to specinvoke. After the program
        # name, -o/-e may be application flags (Blender uses -e 1 for a frame).
        i = 0
        while i < len(words) and words[i] in ("-o", "-e", "-i"):
            if i + 1 == len(words):
                raise ValueError("Missing specinvoke redirection operand")
            if words[i] in ("-o", "-e"):
                path = run / words[i + 1]
                if not path.resolve().is_relative_to(run.resolve()):
                    raise ValueError(f"Output outside run directory: {path}")
                outputs.add(path)
            i += 2
        if i == len(words):
            raise ValueError("Missing benchmark command")
    invocation = (run / "speccmds.out").read_text()
    statuses = [int(rc) for rc in re.findall(r"^child finished:.*\brc=(-?\d+)\s*$", invocation, re.M)]
    started = re.findall(r"^child started:", invocation, re.M)
    if not commands or not statuses or len(started) != len(statuses) or len(statuses) != len(commands):
        raise ValueError("Missing, duplicate, or incomplete benchmark invocations")
    if re.findall(r"^specinvoke exit: rc=(-?\d+)$", invocation, re.M) != ["0"]:
        raise ValueError("specinvoke did not complete normally")
    lines = []
    kinds = set()
    outputs = set().union(*command_outputs)
    for path in sorted(outputs):
        with path.open(errors="replace") as stream:
            for line in stream:
                kind = diagnostic(line)
                if kind:
                    kinds.add(kind)
                    lines.append(f"{path.name}: {line.rstrip()}")
    return {"commands": commands, "executed_commands": len(statuses),
            "skipped_commands": [], "stopped_early": False,
            "child_statuses": statuses,
            "diagnostics": sorted(kinds), "diagnostic_lines": lines}


def read_valids(root, benchmark, size="test"):
    records = []
    for path in sorted((root / "result").glob("*.rsf")):
        text = path.read_text(errors="replace")
        sizes = re.findall(r"^spec\.cpu2017\.size:[ \t]*([^\n]+)$", text, re.M)
        sizes = ["ref" if value.strip() == "refrate (ref)" else value.strip() for value in sizes]
        if sizes != [size]:
            if len(sizes) != 1 or sizes[0] not in ("test", "ref"):
                raise ValueError(f"Missing or ambiguous RSF input size: {path}")
            continue
        for name, tune, iteration, valid in re.findall(
                r"^spec\.cpu2017\.results\.([^.]+)\.([^.]+)\.(\d+)\.valid:\s*(\S+)\s*$",
                text, re.M):
            records.append({"name": name, "tune": tune, "iteration": iteration,
                            "valid": valid, "source": path.name})
    if benchmark.startswith(("997.", "999.")):
        if records:
            raise ValueError("Unexpected RSF benchmark records for specrand")
        return None
    if (len(records) != 1 or records[0]["name"] != benchmark.replace(".", "_")
            or records[0]["tune"] != "base" or records[0]["iteration"] != "000"):
        raise ValueError(f"Expected exactly one matching base/{size} result: {records}")
    return records[0]["valid"]


def build_problem(benchmark, status, log):
    if status != 0:
        return f"Build harness exit {status} (including timeout/infrastructure failures)"
    successes = re.findall(r"^Build successes for \w+: (.*)$", log, re.M)
    errors = re.findall(r"^Build errors for \w+: (.*)$", log, re.M)
    if successes != [f"{benchmark}(base)"] or errors != ["None"]:
        return "Missing successful build record or unexpected build errors"
    return None


def check_result(benchmark, rule, evidence, run_status, log, *, match_diagnostics=True):
    if run_status != 0 or "runcpu finished at" not in log:
        return f"Run harness did not finish normally (exit {run_status})"
    valid = evidence["valid"]
    statuses = evidence["child_statuses"]
    kinds = set(evidence["diagnostics"])
    if rule is None:
        if any(statuses) or kinds:
            return f"Expected clean execution, got child statuses {statuses}, diagnostics {sorted(kinds)}"
        if benchmark.startswith(("997.", "999.")):
            if re.findall(r"^Success: (.*)$", log, re.M) != [f"1x{benchmark}"]:
                return "Missing or ambiguous specrand Success record"
        elif valid != "S":
            return f"Expected SPEC valid S, got {valid}"
    else:
        if not match_diagnostics:
            # Complete execution may still print a complaint and return zero.
            if valid not in ("S", "RE", "VE"):
                return f"Unknown SPEC failure validity: {valid}"
            if valid in ("RE", "VE") or any(statuses) or kinds:
                return None
            return "Unexpected pass; review before changing expectations"
        if valid == "S":
            return "Unexpected pass; review before changing expectations"
        if rule.get("child_status_policy") == "exhaustion":
            statuses_match = bool(statuses) and 256 in statuses and all(s in (0, 256) for s in statuses)
        else:
            statuses_match = statuses == rule["child_statuses"]
        if (valid != rule["valid"] or not statuses_match
                or kinds != set(rule["diagnostics"])):
            return (f"Expected valid={rule['valid']}, child statuses={rule.get('child_statuses', rule.get('child_status_policy'))}, "
                    f"diagnostics={rule['diagnostics']}; got valid={valid}, "
                    f"child statuses={statuses}, diagnostics={sorted(kinds)}")
    return None


def inspect_case(root, benchmark, mode, size="test"):
    run_size = "refrate" if size == "ref" else size
    runs = list((root / "benchspec" / "CPU" / benchmark / "run").glob(
        f"run_base_{run_size}_ci-{mode}.*"))
    if len(runs) != 1:
        raise ValueError(f"Expected one fresh run directory, got {runs}")
    evidence = collect_execution(runs[0])
    evidence["valid"] = read_valids(root, benchmark, size)
    evidence["run_directory"] = str(runs[0])
    return evidence
