"""Benchmark modes and their compiler configurations, shared by all CI runners."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Configuration:
    c: str
    cxx: str
    musl: bool = False
    strict: bool = False
    baseline: bool = False
    id32: bool = False


CONFIGURATIONS = {
    "plain-glibc": Configuration("", "", baseline=True),
    "plain-musl": Configuration("--config=ptsan-musl-baseline-c.cfg",
                                "--config=ptsan-musl-baseline.cfg", musl=True, baseline=True),
    "glibc": Configuration("-fsanitize=ptsan", "--config=ptsan-glibc.cfg"),
    "musl-nonstrict": Configuration("--config=ptsan-musl-nonstrict-c.cfg",
                                   "--config=ptsan-musl-nonstrict.cfg", musl=True),
    "musl-strict": Configuration("--config=ptsan-musl-strict-c.cfg",
                                "--config=ptsan-musl-strict.cfg", musl=True, strict=True),
    "glibc-lam": Configuration("-fsanitize=ptsan -fsanitize-ptsan-lam",
                              "--config=ptsan-glibc-lam.cfg"),
    "musl-nonstrict-lam": Configuration("--config=ptsan-musl-nonstrict-lam-c.cfg",
                                       "--config=ptsan-musl-nonstrict-lam.cfg", musl=True),
    "musl-strict-lam": Configuration("--config=ptsan-musl-strict-lam-c.cfg",
                                    "--config=ptsan-musl-strict-lam.cfg", musl=True, strict=True),
    "glibc-id32": Configuration("-fsanitize=ptsan -fsanitize-ptsan-32-bit-ids",
                               "--config=ptsan-glibc-id32.cfg", id32=True),
    "musl-nonstrict-id32": Configuration("--config=ptsan-musl-id32-nonstrict-c.cfg",
                                        "--config=ptsan-musl-id32-nonstrict.cfg", musl=True, id32=True),
    "musl-strict-id32": Configuration("--config=ptsan-musl-id32-strict-c.cfg",
                                     "--config=ptsan-musl-id32-strict.cfg", musl=True, strict=True, id32=True),
}

BASELINES = ("plain-glibc", "plain-musl")
BASE_MODES = ("glibc", "musl-nonstrict", "musl-strict")
LAM_MODES = tuple(mode + "-lam" for mode in BASE_MODES)
ID32_MODES = tuple(mode + "-id32" for mode in BASE_MODES)
MODES = BASE_MODES + LAM_MODES + ID32_MODES


def selected_modes(group):
    return {"software": BASE_MODES, "lam": LAM_MODES, "id32": ID32_MODES, "all": MODES}[group]
