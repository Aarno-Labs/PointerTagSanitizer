"""Shared benchmark configuration; scheduling and execution policy live in runners."""

from ptsan_ci_modes import CONFIGURATIONS


def spec_benchmarks(manifest, mode):
    """Select the declared inventory without treating unsupported libc pairs as failures."""
    restrictions = manifest.get("benchmark_configurations", {})
    if mode not in CONFIGURATIONS or not set(restrictions) <= set(manifest["benchmarks"]):
        raise ValueError("Unknown SPEC benchmark or configuration")
    for allowed in restrictions.values():
        if (not isinstance(allowed, list) or not allowed or len(set(allowed)) != len(allowed)
                or not set(allowed) <= set(CONFIGURATIONS)):
            raise ValueError("Invalid SPEC benchmark configuration selection")
    return [name for name in manifest["benchmarks"]
            if name not in restrictions or mode in restrictions[name]]


def stack_temporal_safety(mode, value=None):
    if value is None:
        return 1
    if value not in (0, 1):
        raise ValueError("Stack temporal safety must be 0 or 1")
    return value


def benchmark_flags(mode, *, runtime_validate=False, stack_temporal=None, preserve_ids=False):
    """Common C/C++ flags for MultiSource and Fleetbench; SPEC uses ptsan.cfg."""
    stack_temporal = stack_temporal_safety(mode, stack_temporal)
    config = CONFIGURATIONS[mode]
    extra = "-g"
    if runtime_validate and config.baseline:
        raise ValueError("An uninstrumented baseline cannot enable PTSan validation")
    if not config.baseline:
        if runtime_validate:
            extra += " -fsanitize-ptsan-runtime-validate"
        # Match the dependency stacks explicitly, including correctness builds
        # which previously inherited the compiler's strong-stack default.
        extra += f" -mllvm -ptsan-abort-on-error -mllvm -ptsan-stack-temporal-safety={stack_temporal}"
        if preserve_ids:
            extra += " -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0"
    return tuple(f"{flags} {extra}".strip() for flags in (config.c, config.cxx))


def multisource_command(build, workspace, output, mode, *, runtime_validate=False,
                        stack_temporal=None, preserve_ids=False):
    c, cxx = benchmark_flags(mode, runtime_validate=runtime_validate,
                             stack_temporal=stack_temporal, preserve_ids=preserve_ids)
    return ["cmake", "-S", str(build / "test-suite-src"), "-B", str(output),
            "-G", "Unix Makefiles", "-C", str(workspace / ".github/multisource/ptsan.cmake"),
            f"-DCMAKE_C_COMPILER={build}/bin/clang", f"-DCMAKE_CXX_COMPILER={build}/bin/clang++",
            f"-DCMAKE_C_FLAGS={c}", f"-DCMAKE_CXX_FLAGS={cxx}",
            f"-DTEST_SUITE_LIT={build}/bin/llvm-lit"]


def spec_arguments(build, workspace, mode, config_path, output, *, size, stack_temporal=None):
    stack_temporal = stack_temporal_safety(mode, stack_temporal)
    if size not in ("test", "ref"):
        raise ValueError(f"Unknown SPEC input size: {size}")
    config = CONFIGURATIONS[mode]
    # ptsan.cfg owns optimization, stack-temporal, abort-on-error and compatibility
    # flags. Neither correctness nor performance enables runtime validation.
    definitions = {
        "compiler_dir": build / "bin", "mode": mode, "stack_temporal": stack_temporal,
        "musl": int(config.musl), "strict": int(config.strict),
        # Keep SPEC macro values nonempty for plain glibc, with no toolchain override.
        "baseline": int(config.baseline), "c_config": config.c or "-g",
        "id32": int(config.id32),
        "cxx_config": config.cxx or "-g",
        "compat_dir": workspace / ".github/spec/musl_compat",
    }
    arguments = [f"--config={config_path}"]
    for key, value in definitions.items():
        arguments += ["--define", f"{key}={value}"]
    return [*arguments, "--tune=base", f"--size={size}", "--copies=1", "--iterations=1",
            "--noreportable", f"--output_root={output}"]
