#!/usr/bin/env python3
"""Check MultiSource outcomes without treating arbitrary failures as expected."""
import argparse
from ptsan_ci_modes import MODES
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import sys


EXHAUSTION = "PTSan: ID space exhausted in strict mode"
COMPLAINT = re.compile(r"Found bad \w+ access\b")


def expectations(manifest, mode):
    rules = {}
    for rule in manifest["rules"]:
        if mode not in rule["configurations"]:
            continue
        for name in rule["tests"]:
            name += ".test"
            if name in rules:
                raise ValueError(f"Duplicate expectation: {mode}: {name}")
            rules[name] = rule
    return rules


def check_build(log, status, rules, build_system="ninja"):
    expected = {rule["build_directory"] for rule in rules.values() if rule.get("phase") == "build"}
    observed = [target for line in re.findall(r"^FAILED: ([^\n]+)", log, re.M)
                for target in line.split()]
    errors = []
    failure_status = 1
    if build_system == "make":
        failure_status = 2
        observed = []
        propagated = []
        for line in log.splitlines():
            if not re.match(r"(?:\S*/)?g?make(?:\[\d+\])?: \*\*\* ", line):
                continue
            match = re.search(r"\*\*\* \[(.+):\d+: (.+)\] Error \d+\s*$", line)
            if not match:
                errors.append(f"Unrecognized Make failure: {line}")
                continue
            makefile, target = match.groups()
            if makefile.endswith("/build.make"):
                observed.append(target)
            elif makefile == "CMakeFiles/Makefile2" or makefile == "Makefile":
                propagated.append(target)
            else:
                errors.append(f"Unexpected Make failure: {line}")
        # Ignore only ancestor 'all' failures caused by a recorded leaf failure.
        # Missing rules, regeneration failures, and unrelated parents fail closed.
        for target in propagated:
            if not observed or not (target == "all" or
                    (target.endswith("/all") and any(
                        leaf.startswith(target[:-3]) for leaf in observed))):
                errors.append(f"Unattributed Make failure: {target}")
    for target in observed:
        if not any(target.startswith(directory) for directory in expected):
            errors.append(f"Unexpected build failure: {target}")
    for directory in expected:
        if not any(target.startswith(directory) for target in observed):
            errors.append(f"Missing expected build failure in {directory}")
    if status != (failure_status if expected else 0):
        errors.append(f"Unexpected {build_system} exit status: {status}")
    return errors


def read_execution(suite, name):
    test = Path(name)
    stem = suite / "MultiSource" / test.parent / "Output" / test.name
    # These expected failures each have one RUN command. Fail closed if that
    # changes instead of guessing which invocation supplied a diagnostic.
    times = list(stem.parent.glob(stem.name + "*.time"))
    if times != [Path(str(stem) + ".time")]:
        raise ValueError(f"Expected one execution record, got {times}")
    exits = re.findall(r"^exit (-?\d+)$", times[0].read_text(), re.M)
    if len(exits) != 1:
        raise ValueError("Missing or ambiguous child exit status")
    raw = Path(str(stem) + ".out.ptsan-raw")
    if not raw.exists():
        raw = Path(str(stem) + ".out")
    output = raw.read_text(errors="replace")
    if re.fullmatch(r"[0-9a-f]{32}\s*", output):
        raise ValueError("Only an output hash remains; the raw diagnostic was not preserved")
    return int(exits[0]), output


def performance_diagnostics(suite, name):
    """Find runtime complaints even when a program prints one and exits zero."""
    test = Path(name)
    directory = suite / "MultiSource" / test.parent / "Output"
    outputs = sorted(directory.glob(test.name + "*.out"))
    if not outputs:
        raise ValueError("Missing benchmark output for performance diagnostic check")
    lines = []
    for output in outputs:
        raw = Path(str(output) + ".ptsan-raw")
        if raw.exists():
            output = raw
        with output.open(errors="replace") as stream:
            first = stream.readline()
            if re.fullmatch(r"[0-9a-f]{32}\s*", first) and not stream.read(1):
                raise ValueError("Only an output hash remains; cannot check runtime diagnostics")
            stream.seek(0)
            for line in stream:
                if "Found bad" in line or "PTSan" in line:
                    lines.append(f"{output.name}: {line.rstrip()}")
    return lines


def preserve_execution(suite, results, name):
    test = Path(name)
    source = suite / "MultiSource" / test.parent / "Output"
    target = results / "failure-output" / test.parent
    target.mkdir(parents=True, exist_ok=True)
    for path in source.glob(test.name + "*"):
        if path.is_file() and path.suffix in (".out", ".ptsan-raw", ".time", ".script"):
            shutil.copyfile(path, target / path.name)


def check_failure(test, rule, child_exit, raw, *, match_diagnostics=True):
    if test["code"] != "FAIL":
        return f"Expected FAIL, got {test['code']}"
    if not match_diagnostics:
        # Performance excludes known failing workloads regardless of their
        # diagnostic. Still require a child failure or an output mismatch.
        if child_exit != 0 or re.search(r"/fpcmp: Comparison failed,", test.get("output", "")):
            return None
        return "No workload failure or output mismatch; possible harness failure"
    complaints = COMPLAINT.findall(raw)
    # Reject unknown/malformed PTSan diagnostics as well as known complaints.
    ptsan_lines = [line[line.index("PTSan:"):] for line in raw.splitlines() if "PTSan:" in line]
    category = rule["category"]
    if category == "out-of-ids":
        if child_exit != 1 or ptsan_lines != [EXHAUSTION] or "Found bad" in raw:
            return "Expected only ID exhaustion with child exit 1"
    elif category == "ptsan-complaint":
        if child_exit != 1 or not complaints or ptsan_lines:
            return "Expected a PTSan access complaint with child exit 1, not ID exhaustion"
    elif category == "other" and rule["phase"] == "verify":
        # Name + normal execution + output mismatch. An arbitrary crash,
        # timeout, missing executable or new sanitizer report is not accepted.
        if (child_exit != 0 or "Found bad" in raw or ptsan_lines
                or not re.search(r"/fpcmp: Comparison failed,", test.get("output", ""))):
            return "Expected normal child exit 0 and fpcmp output mismatch without a PTSan diagnostic"
    else:
        return f"Unknown runtime expectation: {rule}"
    return None


def check_suite(suite, results, manifest, inventory, mode, build_exit, lit_exit, build_system="ninja",
                *, match_diagnostics=True):
    rules = expectations(manifest, mode)
    if not set(rules) <= inventory:
        raise ValueError("Expectation names are absent from the test inventory")
    tests = json.loads((results / "results.json").read_text())["tests"]
    names = [test["name"].removeprefix("test-suite :: MultiSource/") for test in tests]
    build_errors = check_build((results / "build.log").read_text(), build_exit, rules, build_system)
    errors = list(build_errors)
    generated = set((results / "generated-tests.txt").read_text().splitlines())
    for label, actual in [("generated", generated), ("executed", set(names))]:
        if actual != inventory:
            errors.append(f"{label} inventory: missing {sorted(inventory - actual)}; extra {sorted(actual - inventory)}")
    if len(names) != len(set(names)):
        errors.append("Duplicate test results")
    if lit_exit != (1 if any(test["code"] != "PASS" for test in tests) else 0):
        errors.append(f"Unexpected lit exit status: {lit_exit}")

    checked = []
    for name, test in zip(names, tests):
        problem = None
        rule = rules.get(name)
        category = rule["category"] if rule else "pass"
        diagnostic_lines = []
        if not match_diagnostics and name in inventory and test["code"] == "PASS":
            try:
                diagnostic_lines = performance_diagnostics(suite, name)
            except (OSError, ValueError) as exc:
                problem = str(exc)
        if name in inventory and test["code"] != "PASS":
            preserve_execution(suite, results, name)
        if diagnostic_lines:
            preserve_execution(suite, results, name)
        if problem:
            pass
        elif name not in inventory:
            problem = "Test is absent from the approved inventory"
        elif rule is None:
            if test["code"] != "PASS":
                problem = f"Expected PASS, got {test['code']}"
            elif diagnostic_lines:
                problem = "Runtime error reported despite successful exit/output check"
        elif test["code"] == "PASS":
            if match_diagnostics or not diagnostic_lines:
                problem = "Unexpected pass; review before removing the expected failure"
        elif rule.get("phase") == "build":
            if test["code"] != "NOEXE":
                problem = f"Expected NOEXE from a build failure, got {test['code']}"
            elif build_errors:
                problem = "NOEXE does not have the required matching build failure"
            # check_build attributes failures to the expected target directory.
        elif not match_diagnostics and test["code"] == "TIMEOUT":
            # Lit killed this known excluded workload; no complete .time file
            # need exist. A timeout of the whole lit invocation still fails.
            pass
        else:
            try:
                child_exit, raw = read_execution(suite, name)
                problem = check_failure(test, rule, child_exit, raw,
                                        match_diagnostics=match_diagnostics)
            except (OSError, ValueError) as exc:
                problem = str(exc)
        if problem:
            errors.append(f"{name}: {problem}")
        checked.append({"name": name, "lit_code": test["code"], "category": category,
                        "matched": problem is None, "detail": problem,
                        "diagnostic_lines": diagnostic_lines})
    return checked, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--build-exit", type=int, required=True)
    parser.add_argument("--build-system", choices=["ninja", "make"], default="ninja")
    parser.add_argument("--lit-exit", type=int, required=True)
    args = parser.parse_args()
    status = {"build_exit": args.build_exit, "lit_exit": args.lit_exit, "build_system": args.build_system}
    (args.results / "status.json").write_text(json.dumps(status, indent=2) + "\n")
    try:
        manifest = json.loads((args.results / "expectations.json").read_text())
        inventory = set((args.results / "expected-tests.txt").read_text().splitlines())
        if manifest["schema"] != 1 or not inventory:
            raise ValueError("Invalid expectation schema or empty inventory")
        checked, errors = check_suite(args.suite, args.results, manifest, inventory,
                                      args.mode, args.build_exit, args.lit_exit, args.build_system)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        checked, errors = [], [f"Checker could not validate results: {exc}"]
    counts = Counter(test["category"] for test in checked if test["matched"])
    report = {**status, "mode": args.mode, "success": not errors,
              "matched": dict(counts), "tests": checked, "errors": errors}
    (args.results / "checked-results.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = [f"### MultiSource: {args.mode}", "",
             "Correctness validation; benchmark-only train inputs. Not performance data.", "",
             f"Expectation check: {'FAIL' if errors else 'PASS'}", "",
             *[f"- Matched {category}: {count}" for category, count in sorted(counts.items())]]
    lines += ["", "Expected nonpasses:", "", "```"]
    lines += [f"{t['lit_code']}: {t['name']} [{t['category']}; {'matched' if t['matched'] else 'MISMATCH'}]"
              for t in checked if t["category"] != "pass"]
    lines += ["```"]
    if errors:
        lines += ["", "Errors:", "", *[f"- {error}" for error in errors]]
    summary = "\n".join(lines) + "\n"
    (args.results / "summary.md").write_text(summary)
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as stream:
            stream.write(summary)
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
