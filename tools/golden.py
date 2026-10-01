#!/usr/bin/env python3
"""golden.py: golden format v2 tooling, shared by FunkGui and FCompressor.

Specification: FCompressor docs/design/03-build-verify-process.md §3.2.3-§3.2.5 (format, statuses, blessing) and §3.3
(base + per-arch overlay layout). FunkGui's tools/verify.sh runs `report`; FCompressor's Scripts/golden.py wraps this
file with --golden-root tests/golden --allow-env FCMP_ALLOW_BLESS. Probes never write the golden tree: they write
candidates into <build>/golden-candidates, and `adopt`, run by the lead from the main checkout, is the only way in.

  golden.py report <build> [--junit <ctest junit xml>] [--golden-root <dir>] [--allow-env <NAME>]
      BLOCKING (spec_fail, harness_error, missing results, crashed/timed-out/disabled/not-run tests), DRIFT (with the
      .diff), MISSING (no golden yet), IMPROVED (le:/ge: rows that moved the good way), the 10 slowest tests.
      Exit 0 when nothing is blocking.
  golden.py diff <build> [--only <glob>]... [--golden-root <dir>] [--allow-env <NAME>]
      Print each selected candidate's .diff (drift) or its rows (no golden yet).
  golden.py adopt <build> [--x86 <build-x86>] --only <glob> [--only <glob>]... --reason "<text>"
                  [--golden-root <dir>] [--allow-env <NAME>] [--allow-geometry]
      Move the selected candidates into the golden tree. Refused unless <NAME> (default FUNKGUI_ALLOW_BLESS) matches
      ^[A-Z][A-Z0-9_]*_ALLOW_BLESS$ and is 1 in the environment; when run from a linked worktree; when the golden tree
      has uncommitted changes; when a selected candidate has no results or a status other than pass, golden_drift or
      golden_missing; when its files do not hold the golden_rows its results report (stale candidates: run verify,
      which wipes them, first); when it belongs to a provisional Mode (a results JSON member "provisional": a list of
      Mode keys, written by dsp.registry through Probe::note on every run, [] when none), or is Mode-scoped while no
      results JSON of the build carries that list; or when it is a ui.geometry probe without --allow-geometry (before
      the UI freeze FZ5). Merge rules: arm64 candidates alone overwrite base/ (a stale arm64 overlay is removed, x86_64
      overlay rows no longer in base are pruned); with --x86, a row equal on both arches (or, for a numeric tolerance,
      x86 within tolerance of arm64) goes to base/, any other row to base/ (arm64 value) and both overlays, and an
      xarch. row that differs aborts: it is a determinism bug. wasm32 (v0.12.0: a probe compiled to WebAssembly and
      run under node, --arch wasm32) is an architecture like the others to report and diff, and its overlay
      <root>/wasm32/ is pruned like x86_64's whenever base moves; adopt does not write a wasm32 overlay yet, so
      wasm32 candidates are refused there.
  golden.py selftest
      This file's own checks, in a scratch directory (never the golden tree, never the allow variable).

--only globs match "<scope>/<probe>" (e.g. global/fg.font.probe, modes/clean/dsp.static) or "<probe>", with * ? [].
Every subcommand but selftest accepts --allow-env, so a wrapper can pass it to all of them (FCompressor's
Scripts/golden.py does); only adopt reads it. Golden rows never hold a non-finite number: a value spelled
[+-]inf|infinity|nan|nan(...) (any case) is refused, and so is a numeric-tolerance value that reads as one (1e999),
as in the harness.
Exit codes: 0 ok, 1 blocking results (report), 2 refused or usage error.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

KEY_RE = re.compile(r"^[a-z0-9][a-z0-9._:+-]{0,119}$")
ARCHES = ("arm64", "x86_64", "wasm32")
BLOCKING = ("spec_fail", "harness_error")
ADOPTABLE = ("pass", "golden_drift", "golden_missing")        # every other status (unreadable, ...) is refused
ALLOW_ENV_RE = re.compile(r"^[A-Z][A-Z0-9_]*_ALLOW_BLESS$")
# The harness's detail::nonFiniteSpelling: what strtod reads as a non-finite number. Hash values never match.
NONFINITE_RE = re.compile(r"^[+-]?(inf|infinity|nan(\([0-9a-z_]*\))?)$", re.IGNORECASE)
HEADER = "# funkgui-golden 2  probe={probe}  scope={scope}\n# key\tvalue\ttolerance\n"


class Refused(Exception):
    """A refusal or a usage error: printed, exit 2."""


# ---- golden rows -----------------------------------------------------------------------------------------------------

def fmt_num(x: float) -> str:
    """printf("%.9g"), as the harness writes numbers."""
    return "%.9g" % x


@dataclass(frozen=True)
class Tol:
    kind: str          # exact | abs | rel | absrel | le | ge
    a: float = 0.0
    b: float = 0.0

    def text(self) -> str:
        if self.kind == "exact":
            return "exact"
        if self.kind == "absrel":
            return "absrel:%s:%s" % (fmt_num(self.a + 0.0), fmt_num(self.b + 0.0))
        return "%s:%s" % (self.kind, fmt_num(self.a + 0.0))

    def accepts(self, got: float, golden: float) -> bool:
        d = abs(got - golden)
        if self.kind == "exact":
            return got == golden
        if self.kind == "abs":
            return d <= self.a
        if self.kind == "rel":
            return d <= self.a * abs(golden)
        if self.kind == "absrel":
            return d <= max(self.a, self.b * abs(golden))
        if self.kind == "le":
            return got <= golden + self.a
        return got >= golden - self.a                      # ge


def _tol_num(s: str) -> float | None:
    if not s or s[0] in " \t" or len(s) > 64:
        return None
    try:
        x = float(s)
    except ValueError:
        return None
    if not math.isfinite(x) or x < 0.0 or s.strip() != s or s.lower() in ("nan", "inf", "infinity"):
        return None
    return x + 0.0


def parse_tol(s: str) -> Tol | None:
    """The harness's Tol::parse: exact | abs:a | rel:r | absrel:a:r | le:s | ge:s | <a> (bare number = abs)."""
    if s == "exact":
        return Tol("exact")
    for kind in ("abs", "rel", "le", "ge"):
        if s.startswith(kind + ":"):
            x = _tol_num(s[len(kind) + 1:])
            return None if x is None else Tol(kind, x)
    if s.startswith("absrel:"):
        parts = s[7:].split(":")
        if len(parts) != 2:
            return None
        x, r = _tol_num(parts[0]), _tol_num(parts[1])
        return None if x is None or r is None else Tol("absrel", x, r)
    x = _tol_num(s)
    return None if x is None else Tol("abs", x)


def value_problem(v: str) -> str | None:
    if not v:
        return "empty value"
    if len(v.encode()) > 64:
        return "value longer than 64 bytes"
    if any(ord(c) <= 0x20 or ord(c) == 0x7F for c in v):
        return "value contains whitespace or a control character"
    if NONFINITE_RE.fullmatch(v):
        return "value spells a non-finite number"
    return None


def parse_number(v: str) -> float | None:
    try:
        return float(v)
    except ValueError:
        return None


@dataclass
class Row:
    key: str
    value: str
    tol: Tol

    def line(self) -> str:
        return "%s\t%s\t%s\n" % (self.key, self.value, self.tol.text())


def read_rows(path: Path) -> dict[str, Row]:
    """A golden (or candidate) file, with the harness's rules (03 §3.2.3). Raises Refused on a malformed file."""
    rows: dict[str, Row] = {}
    for n, raw in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
        line = raw
        if not line or line.startswith("#"):
            continue
        where = "%s:%d" % (path, n)
        fields = line.split("\t")
        if len(fields) != 3:
            raise Refused("%s: malformed row (need key<TAB>value<TAB>tolerance)" % where)
        key, value, tol_text = fields
        tol = parse_tol(tol_text)
        why = ("invalid key" if not KEY_RE.fullmatch(key) else value_problem(value) or
               ("bad tolerance" if tol is None else None))
        if why is None and tol.kind != "exact":
            x = parse_number(value)
            if x is None:
                why = "numeric tolerance on a non-numeric value"
            elif not math.isfinite(x):
                why = "numeric tolerance on a non-finite value"
        if why is None and key in rows:
            why = "duplicate key '%s'" % key
        if why is not None:
            raise Refused("%s: %s" % (where, why))
        rows[key] = Row(key, value, tol)
    return rows


def write_rows(path: Path, probe: str, scope: str, rows: dict[str, Row]) -> str:
    return HEADER.format(probe=probe, scope=scope) + "".join(r.line() for r in rows.values())


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp.%d" % os.getpid())
    tmp.write_text(content, encoding="utf-8")
    os.replace(tmp, path)


# ---- builds: results and candidates ----------------------------------------------------------------------------------

def test_name(result: dict) -> str:
    """The CTest name of a probe run: <probe> for a global probe, <probe>.<mode> per Mode (03 §2.9)."""
    return result["probe"] + ("." + result["mode"] if result.get("mode") else "")


def scope_of(result: dict) -> str:
    return "modes/" + result["mode"] if result.get("mode") else "global"


def load_results(build: Path) -> dict[str, dict]:
    out: dict[str, dict] = {}
    rdir = build / "probe-results"
    if not rdir.is_dir():
        return out
    for p in sorted(rdir.glob("*.json")):
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            out[test_name(data)] = data
        except (ValueError, KeyError) as e:
            out[p.stem] = {"probe": p.stem, "mode": "", "status": "unreadable", "error": str(e)}
    return out


@dataclass
class Candidate:
    arch: str
    scope: str
    probe: str
    txt: Path | None
    sidecars: dict[str, Path] = field(default_factory=dict)     # key -> <probe>.<key>.lines
    diff: Path | None = None

    @property
    def label(self) -> str:
        return "%s/%s" % (self.scope, self.probe)

    @property
    def mode(self) -> str:
        return self.scope[len("modes/"):] if self.scope.startswith("modes/") else ""

    @property
    def test(self) -> str:
        return self.probe + ("." + self.mode if self.mode else "")


def discover(build: Path, arch: str | None = None) -> list[Candidate]:
    """Every candidate under <build>/golden-candidates/<arch>/<scope>/."""
    root = build / "golden-candidates"
    found: dict[tuple[str, str, str], Candidate] = {}
    if not root.is_dir():
        return []
    for a in ARCHES:
        if arch is not None and a != arch:
            continue
        adir = root / a
        if not adir.is_dir():
            continue
        for p in sorted(adir.rglob("*")):
            if not p.is_file() or ".tmp." in p.name:
                continue
            scope = p.parent.relative_to(adir).as_posix()
            if p.suffix == ".txt":
                c = found.setdefault((a, scope, p.stem), Candidate(a, scope, p.stem, None))
                c.txt = p
            elif p.suffix == ".diff":
                c = found.setdefault((a, scope, p.stem), Candidate(a, scope, p.stem, None))
                c.diff = p
    # sidecars: <probe>.<key>.lines, attributed to the longest matching probe name in the same directory. A probe with
    # lines only and no rows has no .txt to name it, and its keys may hold dots (FCompressor ui.a11y: "chars.colour"),
    # so its name comes from the build's results (v0.8.1), not from the file name's last dot.
    known = {r["probe"] for r in load_results(build).values() if isinstance(r.get("probe"), str)}
    for a in ARCHES:
        adir = root / a
        if not adir.is_dir() or (arch is not None and a != arch):
            continue
        for p in sorted(adir.rglob("*.lines")):
            scope = p.parent.relative_to(adir).as_posix()
            stem = p.name[:-len(".lines")]
            owners = [c for (ca, cs, cp), c in found.items() if ca == a and cs == scope and stem.startswith(cp + ".")]
            if owners:
                owner = max(owners, key=lambda c: len(c.probe))
            else:                                               # a probe with lines only and no rows
                named = [k for k in known if stem.startswith(k + ".")]
                probe = max(named, key=len) if named else stem.rpartition(".")[0]
                owner = found.setdefault((a, scope, probe), Candidate(a, scope, probe, None))
            owner.sidecars[stem[len(owner.probe) + 1:]] = p
    return sorted(found.values(), key=lambda c: (c.arch, c.scope, c.probe))


def selected(c: Candidate, globs: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(c.label, g) or fnmatch.fnmatchcase(c.probe, g) for g in globs)


def golden_rows(root: Path, arch: str, scope: str, probe: str) -> dict[str, Row]:
    """base, then the arch overlay on top (the harness's merge)."""
    rows: dict[str, Row] = {}
    base = root / "base" / scope / (probe + ".txt")
    over = root / arch / scope / (probe + ".txt")
    if base.is_file():
        rows = read_rows(base)
    if over.is_file():
        rows.update(read_rows(over))
    return rows


# ---- report ----------------------------------------------------------------------------------------------------------

@dataclass
class Case:
    name: str
    status: str            # run | fail | disabled | notrun
    time: float
    message: str


def parse_junit(path: Path) -> list[Case]:
    tree = ET.parse(path)
    cases = []
    for tc in tree.getroot().iter("testcase"):
        status = tc.get("status", "run")
        message = ""
        for tag in ("failure", "skipped", "error"):
            el = tc.find(tag)
            if el is not None:
                message = el.get("message", tag)
        cases.append(Case(tc.get("name", "?"), status, float(tc.get("time", "0") or 0), message))
    return cases


def improved_rows(root: Path, result: dict, build: Path) -> list[str]:
    cand = build / "golden-candidates" / result["arch"] / scope_of(result) / (result["probe"] + ".txt")
    if not cand.is_file():
        return []
    try:
        gold = golden_rows(root, result["arch"], scope_of(result), result["probe"])
        rows = read_rows(cand)
    except Refused:
        return []
    out = []
    for key, r in rows.items():
        g = gold.get(key)
        if g is None or r.tol.kind not in ("le", "ge") or g.tol != r.tol:
            continue
        got, want = parse_number(r.value), parse_number(g.value)
        if got is None or want is None:
            continue
        if (r.tol.kind == "le" and got < want) or (r.tol.kind == "ge" and got > want):
            out.append("%s  %s: golden %s -> %s (%s)" % (test_name(result), key, g.value, r.value, r.tol.text()))
    return out


def cmd_report(a: argparse.Namespace) -> int:
    build, root = Path(a.build).resolve(), Path(a.golden_root).resolve()
    results = load_results(build)
    blocking: list[str] = []
    drift: list[str] = []
    missing: list[str] = []
    improved: list[str] = []
    counts = {"pass": 0, "drift": 0, "missing": 0, "disabled": 0}
    judged: set[str] = set()                                    # decided by CTest alone
    cases: list[Case] = []

    if a.junit:
        jpath = Path(a.junit)
        if not jpath.is_file():
            blocking.append("(ctest)  no JUnit output at %s: ctest did not run" % jpath)
        else:
            cases = parse_junit(jpath)
            if not cases:
                blocking.append("(ctest)  no tests ran")
        for c in cases:
            res = results.get(c.name)
            if c.status in ("disabled", "notrun"):              # a disabled test means a broken configuration
                counts["disabled"] += 1 if c.status == "disabled" else 0
                blocking.append("%s  %s%s" % (c.name, c.status, (": " + c.message) if c.message else ""))
                judged.add(c.name)
            elif c.status == "fail":
                judged.add(c.name)
                if res is None:
                    blocking.append("%s  failed without results (%s): crash, timeout or a non-probe test failure"
                                    % (c.name, c.message or "failed"))
                elif res.get("status") in BLOCKING:
                    blocking.append("%s  %s (spec %s pass / %s fail)"
                                    % (c.name, res["status"], res.get("spec_pass"), res.get("spec_fail")))
                else:
                    blocking.append("%s  ctest %s although the probe reported %s" % (c.name, c.message or "failed",
                                                                                   res.get("status")))
            elif res is None:
                counts["pass"] += 1                             # a plain test (lint, script) that passed
                judged.add(c.name)
    tests = len(cases) if a.junit else len(results)

    for name, res in sorted(results.items()):
        if name in judged:
            continue
        st = res.get("status")
        if st in BLOCKING:
            blocking.append("%s  %s (spec %s pass / %s fail)" % (name, st, res.get("spec_pass"), res.get("spec_fail")))
        elif st == "golden_drift":
            d = build / "golden-candidates" / res["arch"] / scope_of(res) / (res["probe"] + ".diff")
            drift.append("%s  %s differ, %s new, %s missing  %s" % (name, res.get("golden_fail"), res.get("golden_new"),
                                                                    res.get("golden_missing_rows"), d))
            counts["drift"] += 1
        elif st == "golden_missing":
            cand = build / "golden-candidates" / res["arch"] / scope_of(res) / (res["probe"] + ".txt")
            missing.append("%s  %s rows, no golden yet  %s" % (name, res.get("golden_rows"), cand))
            counts["missing"] += 1
        elif st == "pass":
            counts["pass"] += 1
        else:
            blocking.append("%s  unexpected status %r" % (name, st))
        if st in ("pass", "golden_drift"):
            improved.extend(improved_rows(root, res, build))

    print("golden.py report  %s" % build)
    for title, items in (("BLOCKING", blocking), ("DRIFT", drift), ("MISSING", missing), ("IMPROVED", improved)):
        print("%s (%d)" % (title, len(items)))
        for it in items:
            print("  " + it)
    if cases:
        print("SLOWEST (10)")
        for c in sorted(cases, key=lambda c: -c.time)[:10]:
            print("  %8.2f s  %s" % (c.time, c.name))
    print("SUMMARY  tests %d: pass %d, drift %d, missing %d, blocking %d (disabled %d)"
          % (tests, counts["pass"], counts["drift"], counts["missing"], len(blocking), counts["disabled"]))
    return 1 if blocking else 0


# ---- diff ------------------------------------------------------------------------------------------------------------

def cmd_diff(a: argparse.Namespace) -> int:
    build, root = Path(a.build).resolve(), Path(a.golden_root).resolve()
    shown = 0
    for c in discover(build):
        if a.only and not selected(c, a.only):
            continue
        if c.diff is not None:
            print("==== %s [%s] drift: %s" % (c.label, c.arch, c.diff))
            sys.stdout.write(c.diff.read_text(encoding="utf-8"))
            shown += 1
        elif c.txt is not None and not (root / "base" / c.scope / (c.probe + ".txt")).is_file():
            print("==== %s [%s] no golden yet: %s" % (c.label, c.arch, c.txt))
            sys.stdout.write(c.txt.read_text(encoding="utf-8"))
            shown += 1
    print("golden.py diff: %d candidate group(s) with changes" % shown)
    return 0


# ---- adopt -----------------------------------------------------------------------------------------------------------

def git(repo: Path, *args: str) -> str:
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise Refused("git %s failed in %s: %s" % (" ".join(args), repo, r.stderr.strip()))
    return r.stdout


def existing_parent(p: Path) -> Path:
    while not p.exists():
        p = p.parent
    return p


def check_repository(root: Path) -> Path:
    """The golden tree's repository must be the main checkout and its golden tree clean. Returns the repo root."""
    anchor = existing_parent(root)
    dirs = git(anchor, "rev-parse", "--path-format=absolute", "--git-dir", "--git-common-dir").split("\n")
    if len(dirs) < 2 or Path(dirs[0]).resolve() != Path(dirs[1]).resolve():
        raise Refused("run from the main checkout, not a linked worktree (%s)" % anchor)
    repo = Path(git(anchor, "rev-parse", "--show-toplevel").strip())
    if git(repo, "status", "--porcelain", "--", str(root)).strip():
        raise Refused("%s has uncommitted changes: commit or discard them first, so each adopt is one diff" % root)
    return repo


@dataclass
class Plan:
    writes: dict[Path, str] = field(default_factory=dict)
    deletes: set[Path] = field(default_factory=set)
    notes: list[str] = field(default_factory=list)


def plan_single(root: Path, c: Candidate) -> Plan:
    """arm64 candidates alone: rows go to base/, overwriting it (03 §3.2.5)."""
    plan = Plan()
    rows = read_rows(c.txt) if c.txt is not None else {}
    base = root / "base" / c.scope / (c.probe + ".txt")
    if rows:
        plan.writes[base] = write_rows(base, c.probe, c.scope, rows)
    elif base.is_file():
        plan.deletes.add(base)
    arm = root / "arm64" / c.scope / (c.probe + ".txt")
    if arm.is_file():
        plan.deletes.add(arm)
        plan.notes.append("%s: removed the arm64 overlay (base now holds the arm64 values)" % c.label)
    for arch in ARCHES[1:]:
        prune_overlay(root, arch, c, rows, plan)
    plan_sidecars(root, c, c.sidecars, plan)
    return plan


def prune_overlay(root: Path, arch: str, c: Candidate, rows: dict[str, Row], plan: Plan) -> None:
    """An overlay this adoption does not write keeps only the rows whose keys base still holds (an overlay-only key is
    a harness error), and never an xarch. row."""
    over = root / arch / c.scope / (c.probe + ".txt")
    if not over.is_file():
        return
    kept = {k: r for k, r in read_rows(over).items() if k in rows and not k.startswith("xarch.")}
    if not kept:
        plan.deletes.add(over)
        plan.notes.append("%s: removed the %s overlay (none of its keys is left in base)" % (c.label, arch))
    else:
        plan.writes[over] = write_rows(over, c.probe, c.scope, kept)
        dropped = len(read_rows(over)) - len(kept)
        if dropped:
            plan.notes.append("%s: pruned %d %s overlay row(s) no longer in base" % (c.label, dropped, arch))


def plan_sidecars(root: Path, c: Candidate, sidecars: dict[str, Path], plan: Plan) -> None:
    bdir = root / "base" / c.scope
    for key, p in sidecars.items():
        dst = bdir / ("%s.%s.lines" % (c.probe, key))
        plan.writes[dst] = p.read_text(encoding="utf-8")
    if bdir.is_dir():
        for old in bdir.glob("%s.*.lines" % c.probe):
            key = old.name[len(c.probe) + 1:-len(".lines")]
            if key not in sidecars and KEY_RE.fullmatch(key):
                plan.deletes.add(old)


def plan_merge(root: Path, arm: Candidate, x86: Candidate) -> Plan:
    """--x86 merge rules (03 §3.2.5)."""
    plan = Plan()
    ra = read_rows(arm.txt) if arm.txt is not None else {}
    rx = read_rows(x86.txt) if x86.txt is not None else {}
    if set(ra) != set(rx):
        only_a, only_x = sorted(set(ra) - set(rx)), sorted(set(rx) - set(ra))
        raise Refused("%s: the arches produce different rows (arm64 only: %s; x86_64 only: %s)"
                      % (arm.label, only_a[:5], only_x[:5]))
    base, over_a, over_x = {}, {}, {}
    for key, r in ra.items():
        q = rx[key]
        if r.tol != q.tol:
            raise Refused("%s: %s has tolerance %s on arm64 and %s on x86_64" % (arm.label, key, r.tol.text(),
                                                                                 q.tol.text()))
        if r.tol.kind == "exact":
            same = r.value == q.value
        else:
            va, vx = parse_number(r.value), parse_number(q.value)
            same = va is not None and vx is not None and r.tol.accepts(vx, va)
        if key.startswith("xarch.") and r.value != q.value:
            raise Refused("%s: xarch. row %s differs (arm64 %s, x86_64 %s): a determinism bug, not a golden update"
                          % (arm.label, key, r.value, q.value))
        base[key] = r
        if not same:
            over_a[key], over_x[key] = r, q
    for path, rows in ((root / "base" / arm.scope / (arm.probe + ".txt"), base),
                       (root / "arm64" / arm.scope / (arm.probe + ".txt"), over_a),
                       (root / "x86_64" / arm.scope / (arm.probe + ".txt"), over_x)):
        if rows:
            plan.writes[path] = write_rows(path, arm.probe, arm.scope, rows)
        elif path.is_file():
            plan.deletes.add(path)
    if over_a:
        plan.notes.append("%s: %d row(s) differ between the arches -> both overlays" % (arm.label, len(over_a)))
    for arch in ARCHES[2:]:                                     # overlays this merge does not write (wasm32)
        prune_overlay(root, arch, arm, base, plan)
    for key in set(arm.sidecars) | set(x86.sidecars):
        pa, px = arm.sidecars.get(key), x86.sidecars.get(key)
        if pa is None or px is None or pa.read_text(encoding="utf-8") != px.read_text(encoding="utf-8"):
            raise Refused("%s: lines sidecar %s differs between the arches (sidecars are base-only)"
                          % (arm.label, key))
    plan_sidecars(root, arm, arm.sidecars, plan)
    return plan


def provisional_modes(build: Path) -> set[str] | None:
    """The Mode keys the build's results list as provisional (dsp.registry's note, 03 §3.2.5), or None when no results
    JSON carries a "provisional" member: then nothing tells a provisional Mode apart, and adopt fails closed."""
    keys: set[str] = set()
    found = False
    for name, res in load_results(build).items():
        if "provisional" not in res:
            continue
        prov = res["provisional"]
        if not isinstance(prov, list) or not all(isinstance(k, str) for k in prov):
            raise Refused("%s: the results of %s have a \"provisional\" member that is not a list of Mode keys"
                          % (build / "probe-results", name))
        found = True
        keys.update(prov)
    return keys if found else None


def check_run(c: Candidate, res: dict | None, label: str) -> None:
    """The results of the run a candidate came from: present, adoptable, and matching the candidate's files."""
    if res is None:
        raise Refused("%s: no %sresults for %s (run verify first)" % (c.label, label, c.test))
    status = res.get("status")
    if status not in ADOPTABLE:
        raise Refused("%s: %sstatus %r is not adoptable (only %s)" % (c.label, label, status, ", ".join(ADOPTABLE)))
    held = (len(read_rows(c.txt)) if c.txt is not None else 0) + len(c.sidecars)
    want = res.get("golden_rows")
    if isinstance(want, bool) or not isinstance(want, int) or held != want:
        raise Refused("%s: the %scandidate holds %d golden row(s) but its results report golden_rows %r: stale "
                      "candidate files (run verify, which wipes them, first)" % (c.label, label, held, want))


def adopt(a: argparse.Namespace, environ) -> int:
    if not ALLOW_ENV_RE.fullmatch(a.allow_env or ""):
        raise Refused("--allow-env %r is not a bless variable (^[A-Z][A-Z0-9_]*_ALLOW_BLESS$, e.g. FUNKGUI_ALLOW_BLESS"
                      " or FCMP_ALLOW_BLESS)" % a.allow_env)
    if environ.get(a.allow_env) != "1":
        raise Refused("%s=1 is not set: only the lead blesses, from the main checkout (03 §3.2.5)" % a.allow_env)
    if not a.only:
        raise Refused("--only <glob> is required (bless one reason group at a time)")
    if not a.reason or not a.reason.strip():
        raise Refused("--reason \"<text>\" is required")
    build, root = Path(a.build).resolve(), Path(a.golden_root).resolve()
    repo = check_repository(root)
    x86_build = Path(a.x86).resolve() if a.x86 else None

    chosen = [c for c in discover(build) if selected(c, a.only)]
    if not chosen:
        raise Refused("no candidate in %s matches --only %s" % (build / "golden-candidates", a.only))
    wrong = sorted({c.arch for c in chosen} - {"arm64"})
    if wrong:
        raise Refused("the primary build must hold arm64 candidates (found %s); pass an x86_64 build with --x86 "
                      "(a wasm32 overlay is not adopted by this tool yet)" % wrong)
    results = load_results(build)
    x86_results = load_results(x86_build) if x86_build else {}
    x86_cands = {(c.scope, c.probe): c for c in discover(x86_build, "x86_64")} if x86_build else {}
    # Fail closed (R-G1 #5): a Mode-scoped candidate needs every build's provisional list, even an empty one.
    provisional: set[str] = set()
    if any(c.mode for c in chosen):
        for b in [build] + ([x86_build] if x86_build else []):
            keys = provisional_modes(b)
            if keys is None:
                raise Refused("no results JSON in %s lists the provisional Modes (dsp.registry notes \"provisional\" "
                              "on every run, [] when none), so Mode-scoped candidates cannot be checked (K3 #9)"
                              % (b / "probe-results"))
            provisional |= keys

    plan = Plan()
    for c in chosen:
        check_run(c, results.get(c.test), "")
        other = None
        if x86_build:
            other = x86_cands.get((c.scope, c.probe))
            if other is None:
                raise Refused("%s: no x86_64 candidate in %s" % (c.label, x86_build))
            check_run(other, x86_results.get(c.test), "x86_64 ")
        if c.mode and c.mode in provisional:
            raise Refused("%s: Mode %s is provisional (01 §4.3; K3 #9)" % (c.label, c.mode))
        if c.probe.startswith("ui.geometry") and not a.allow_geometry:
            raise Refused("%s: ui.geometry rows are adopted only after the UI freeze FZ5 (--allow-geometry)" % c.label)
        if other is not None:
            part = plan_merge(root, c, other)
        else:
            part = plan_single(root, c)
        plan.writes.update(part.writes)
        plan.deletes |= part.deletes
        plan.notes.extend(part.notes)

    for path, content in sorted(plan.writes.items()):
        write_atomic(path, content)
    for path in sorted(plan.deletes - set(plan.writes)):
        path.unlink()
    for n in plan.notes:
        print("note: " + n)
    status = git(repo, "status", "--short", "--untracked-files=all", "--", str(root))
    print(status + git(repo, "diff", "--stat", "--", str(root)) if status else "(no change to %s)" % root)
    groups = "\n".join("  %s" % c.label for c in chosen)
    print("---- commit message stub ----\ngoldens: %s\n\ngolden.py adopt %s%s --only %s\n%s"
          % (a.reason.strip(), build, (" --x86 %s" % x86_build) if x86_build else "", " --only ".join(a.only), groups))
    return 0


def cmd_adopt(a: argparse.Namespace) -> int:
    return adopt(a, os.environ)


# ---- selftest --------------------------------------------------------------------------------------------------------

def cmd_selftest(_: argparse.Namespace) -> int:
    import contextlib
    import io

    failures: list[str] = []

    def check(name: str, ok: bool) -> None:
        print("%s  %s" % ("PASS" if ok else "FAIL", name))
        if not ok:
            failures.append(name)

    def refused(name: str, fn, why: str) -> None:
        """fn must raise Refused, and for the reason `why` (a substring of the message), not some earlier one."""
        try:
            fn()
        except Refused as e:
            check("%s (%s)" % (name, str(e)[:90]), why in str(e))
            return
        check(name + " (not refused)", False)

    # tolerances, as the harness writes and parses them
    check("tol.text", [parse_tol(s).text() for s in ("exact", "abs:0.01", "absrel:0.001:0.01", "0.5", "abs:-0")]
          == ["exact", "abs:0.01", "absrel:0.001:0.01", "abs:0.5", "abs:0"])
    check("tol.bad", all(parse_tol(s) is None for s in ("", "abs:", "abs:-1", "abs:nan", "absrel:1", "foo", "le:x",
                                                          "abs:1 ", "ge:1e999")))
    check("tol.accepts", Tol("le", 1).accepts(3, 10) and not Tol("ge", 1).accepts(8.5, 10)
          and Tol("rel", 0.01).accepts(100.9, 100) and not Tol("abs", 0.1).accepts(1.2, 1))
    check("fmt.num", [fmt_num(x) for x in (0.1, 1.0 / 3.0, 1e-5, 32.0)] == ["0.1", "0.333333333", "1e-05", "32"])
    check("allow_env.names", all(ALLOW_ENV_RE.fullmatch(n) for n in ("FCMP_ALLOW_BLESS", "FUNKGUI_ALLOW_BLESS"))
          and not any(ALLOW_ENV_RE.fullmatch(n) for n in ("SHLVL", "CLICOLOR", "ALLOW_BLESS", "fcmp_ALLOW_BLESS",
                                                            "FCMP_ALLOW_BLESS_X", "")))

    with tempfile.TemporaryDirectory(prefix="golden-selftest-") as tmp:
        t = Path(tmp)
        repo = t / "repo"
        root = repo / "test" / "golden"

        def sh(*args: str, cwd: Path = repo) -> None:
            subprocess.run(list(args), cwd=cwd, check=True, capture_output=True)

        repo.mkdir()
        sh("git", "init", "-q")
        sh("git", "config", "user.email", "selftest@example.invalid")
        sh("git", "config", "user.name", "golden selftest")
        sh("git", "config", "commit.gpgsign", "false")                 # the user's global git config is not ours
        sh("git", "config", "core.hooksPath", str(t / "no-hooks"))
        write_atomic(root / "base" / "global" / "p.txt", "a\t1\texact\nb\t2\tabs:0.1\n")
        write_atomic(root / "arm64" / "global" / "p.txt", "b\t2.5\tabs:0.1\n")
        write_atomic(root / "x86_64" / "global" / "p.txt", "b\t2.4\tabs:0.1\n")
        write_atomic(root / "wasm32" / "global" / "p.txt", "a\t1.5\texact\nb\t2.3\tabs:0.1\n")
        write_atomic(root / "base" / "global" / "p.old.lines", "stale\n")
        sh("git", "add", "-A")
        sh("git", "commit", "-q", "-m", "init")

        # malformed golden files are refused, and so are non-finite values (R-G1 #7)
        bad = t / "bad.txt"
        for content, why in (("a\t1\n", "malformed row"), ("a\t1\texact\na\t2\texact\n", "duplicate key"),
                             ("A\t1\texact\n", "invalid key"), ("a\tone\tabs:1\n", "non-numeric value"),
                             ("a\tnan\texact\n", "non-finite"), ("a\t-Infinity\texact\n", "non-finite"),
                             ("a\tNaN(1)\texact\n", "non-finite"), ("a\tinf\tabs:1\n", "non-finite"),
                             ("a\t1e999\tabs:1\n", "non-finite")):
            bad.write_text(content)
            refused("rows.refused %r" % content, lambda: read_rows(bad), why)
        bad.write_text("a\tnano\texact\nb\tinfo\texact\nh\t1e40000000000000\texact\nx\t-0.5\tabs:1\n")
        check("rows.lookalikes_ok", len(read_rows(bad)) == 4)

        def build(name: str, arch: str, rows: str, status: str = "golden_drift", mode: str = "",
                  probe: str = "p", extra: dict | None = None, sidecars: dict | None = None) -> Path:
            b = t / name
            scope = "modes/" + mode if mode else "global"
            write_atomic(b / "golden-candidates" / arch / scope / (probe + ".txt"),
                         HEADER.format(probe=probe, scope=scope) + rows)
            for k, v in (sidecars or {}).items():
                write_atomic(b / "golden-candidates" / arch / scope / ("%s.%s.lines" % (probe, k)), v)
            count = len([r for r in rows.split("\n") if r and not r.startswith("#")]) + len(sidecars or {})
            res = {"probe": probe, "mode": mode, "arch": arch, "status": status, "spec_pass": 1, "spec_fail": 0,
                   "golden_rows": count, "golden_fail": 1, "golden_new": 0, "golden_missing_rows": 0, "ms": 1}
            res.update(extra or {})
            write_atomic(b / "probe-results" / (probe + ("." + mode if mode else "") + ".json"), json.dumps(res))
            return b

        def registry(b: Path, provisional) -> None:
            """dsp.registry's results in build b: its "provisional" note."""
            write_atomic(b / "probe-results" / "dsp.registry.json",
                         json.dumps({"probe": "dsp.registry", "mode": "", "arch": "arm64", "status": "pass",
                                     "spec_pass": 1, "spec_fail": 0, "golden_rows": 0, "golden_fail": 0,
                                     "golden_new": 0, "golden_missing_rows": 0, "ms": 1,
                                     "provisional": provisional}))

        allow = "GOLDEN_SELFTEST_ALLOW_BLESS"

        def ns(b: Path, only=("p",), x86: Path | None = None, geometry=False,
               allow_env: str = allow) -> argparse.Namespace:
            return argparse.Namespace(build=str(b), golden_root=str(root), only=list(only), reason="selftest",
                                      allow_env=allow_env, x86=str(x86) if x86 else None, allow_geometry=geometry)

        env = {allow: "1"}
        arm = build("arm", "arm64", "a\t1\texact\nb\t3\tabs:0.1\n", sidecars={"a11y": "x\ny\n"})

        refused("adopt.no_allow_env", lambda: adopt(ns(arm), {}), "=1 is not set")
        # Any variable that happens to be 1 is not a bless switch (R-G1 #8).
        refused("adopt.allow_env_any_name", lambda: adopt(ns(arm, allow_env="SHLVL"), {"SHLVL": "1"}),
                "is not a bless variable")
        refused("adopt.no_only", lambda: adopt(ns(arm, only=()), env), "--only <glob> is required")
        refused("adopt.nothing_selected", lambda: adopt(ns(arm, only=("q",)), env), "no candidate")
        refused("adopt.blocking_status", lambda: adopt(ns(build("blk", "arm64", "a\t1\texact\n", "spec_fail")), env),
                "'spec_fail' is not adoptable")
        # Only pass / golden_drift / golden_missing are adoptable: an unreadable or unknown status is refused (R-G1 #5).
        unread = build("unread", "arm64", "a\t1\texact\n")
        (unread / "probe-results" / "p.json").write_text("{not json")
        refused("adopt.unreadable_results", lambda: adopt(ns(unread), env), "'unreadable' is not adoptable")
        refused("adopt.unknown_status", lambda: adopt(ns(build("odd", "arm64", "a\t1\texact\n", "crashed")), env),
                "'crashed' is not adoptable")
        nores = build("nores", "arm64", "a\t1\texact\n", probe="q")
        (nores / "probe-results" / "q.json").unlink()
        refused("adopt.no_results", lambda: adopt(ns(nores, only=("q",)), env), "no results")
        # A candidate whose files do not hold the rows its run reported: a stale sidecar from an earlier run (R-G1 #4).
        stale = build("stale", "arm64", "a\t1\texact\nb\t3\tabs:0.1\n", sidecars={"a11y": "x\n", "gone": "old\n"},
                      extra={"golden_rows": 3})
        refused("adopt.stale_candidate_rows", lambda: adopt(ns(stale), env), "stale candidate files")
        # Provisional Modes (K3 #9), failing closed when nothing lists them (R-G1 #5).
        prov = build("prov", "arm64", "a\t1\texact\n", mode="bus-g", extra={"provisional": ["bus-g"]})
        refused("adopt.provisional_mode", lambda: adopt(ns(prov, only=("modes/bus-g/*",)), env), "is provisional")
        registry(prov, [])
        noprov = build("noprov", "arm64", "a\t1\texact\n", mode="bus-g")
        refused("adopt.provisional_unknown", lambda: adopt(ns(noprov, only=("modes/bus-g/*",)), env),
                "lists the provisional Modes")
        registry(noprov, "bus-g")
        refused("adopt.provisional_not_a_list", lambda: adopt(ns(noprov, only=("modes/bus-g/*",)), env),
                "not a list of Mode keys")
        geo = build("geo", "arm64", "a\t1\texact\n", mode="clean", probe="ui.geometry")
        registry(geo, [])
        refused("adopt.geometry_before_fz5", lambda: adopt(ns(geo, only=("ui.geometry",)), env), "--allow-geometry")
        refused("adopt.x86_as_primary", lambda: adopt(ns(build("x86only", "x86_64", "a\t1\texact\n")), env),
                "must hold arm64 candidates")
        # wasm32 (v0.12.0): discovered and reported like the other arches; adopt writes no wasm32 overlay yet
        wasm = build("wasmonly", "wasm32", "a\t1\texact\n", sidecars={"a11y": "x\n"})
        check("discover.wasm32", [(c.arch, c.label, sorted(c.sidecars)) for c in discover(wasm)]
              == [("wasm32", "global/p", ["a11y"])])
        check("golden_rows.wasm32_overlay", golden_rows(root, "wasm32", "global", "p") ==
              {"a": Row("a", "1.5", Tol("exact")), "b": Row("b", "2.3", Tol("abs", 0.1))})
        refused("adopt.wasm32_as_primary", lambda: adopt(ns(wasm), env), "must hold arm64 candidates")

        (root / "base" / "global" / "dirty.txt").write_text("a\t1\texact\n")
        refused("adopt.uncommitted_golden_tree", lambda: adopt(ns(arm), env), "uncommitted changes")
        (root / "base" / "global" / "dirty.txt").unlink()

        wt = t / "linked"
        sh("git", "worktree", "add", "-q", "--detach", str(wt))
        linked = ns(arm)
        linked.golden_root = str(wt / "test" / "golden")
        refused("adopt.linked_worktree", lambda: adopt(linked, env), "linked worktree")
        sh("git", "worktree", "remove", "--force", str(wt))

        # arm64 alone: base overwritten, arm64 overlay removed, x86_64 overlay rows kept only if still in base
        with contextlib.redirect_stdout(io.StringIO()):
            rc = adopt(ns(arm), env)
        check("adopt.single.exit", rc == 0)
        check("adopt.single.base", read_rows(root / "base" / "global" / "p.txt") ==
              {"a": Row("a", "1", Tol("exact")), "b": Row("b", "3", Tol("abs", 0.1))})
        check("adopt.single.arm_overlay_removed", not (root / "arm64" / "global" / "p.txt").exists())
        check("adopt.single.x86_overlay_kept", read_rows(root / "x86_64" / "global" / "p.txt") ==
              {"b": Row("b", "2.4", Tol("abs", 0.1))})
        check("adopt.single.wasm_overlay_kept", read_rows(root / "wasm32" / "global" / "p.txt") ==
              {"a": Row("a", "1.5", Tol("exact")), "b": Row("b", "2.3", Tol("abs", 0.1))})
        check("adopt.single.sidecar", (root / "base" / "global" / "p.a11y.lines").read_text() == "x\ny\n"
              and not (root / "base" / "global" / "p.old.lines").exists())
        check("adopt.single.header", (root / "base" / "global" / "p.txt").read_text().startswith(
            "# funkgui-golden 2  probe=p  scope=global\n"))
        sh("git", "add", "-A")
        sh("git", "commit", "-q", "-m", "adopt 1")

        # lines only, dotted keys (v0.8.1): every sidecar belongs to the probe its results name
        lo = t / "linesonly"
        lo_dir = lo / "golden-candidates" / "arm64" / "modes" / "clean"
        for k in ("chars.colour", "chars.sidechain", "panel"):
            write_atomic(lo_dir / ("ui.a11y.%s.lines" % k), "%s\n" % k)
        write_atomic(lo / "probe-results" / "ui.a11y.clean.json",
                     json.dumps({"probe": "ui.a11y", "mode": "clean", "arch": "arm64", "status": "golden_missing",
                                 "spec_pass": 1, "spec_fail": 0, "golden_rows": 3, "golden_fail": 0,
                                 "golden_new": 3, "golden_missing_rows": 0, "ms": 1}))
        registry(lo, [])
        lo_c = [c for c in discover(lo) if c.scope == "modes/clean"]
        check("discover.lines_only_dotted", [(c.probe, sorted(c.sidecars)) for c in lo_c] ==
              [("ui.a11y", ["chars.colour", "chars.sidechain", "panel"])])
        with contextlib.redirect_stdout(io.StringIO()):
            rc = adopt(ns(lo, only=("modes/clean/ui.*",)), env)
        check("adopt.lines_only_dotted", rc == 0 and
              (root / "base" / "modes" / "clean" / "ui.a11y.chars.colour.lines").read_text() == "chars.colour\n")
        sh("git", "add", "-A")
        sh("git", "commit", "-q", "-m", "adopt lines only")

        # --x86: equal / within tolerance -> base; differing -> both overlays; xarch. differing -> abort
        a2 = build("arm2", "arm64", "a\t1\texact\nb\t3\tabs:0.1\nc\t5\texact\nh\tff\texact\n")
        x2 = build("x862", "x86_64", "a\t1\texact\nb\t3.05\tabs:0.1\nc\t6\texact\nh\tff\texact\n")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = adopt(ns(a2, x86=x2), env)
        check("adopt.x86.exit", rc == 0)
        check("adopt.x86.base", read_rows(root / "base" / "global" / "p.txt") ==
              {"a": Row("a", "1", Tol("exact")), "b": Row("b", "3", Tol("abs", 0.1)),
               "c": Row("c", "5", Tol("exact")), "h": Row("h", "ff", Tol("exact"))})
        check("adopt.x86.overlays", read_rows(root / "arm64" / "global" / "p.txt") == {"c": Row("c", "5", Tol("exact"))}
              and read_rows(root / "x86_64" / "global" / "p.txt") == {"c": Row("c", "6", Tol("exact"))})
        sh("git", "add", "-A")
        sh("git", "commit", "-q", "-m", "adopt 2")
        a3 = build("arm3", "arm64", "xarch.h\taa\texact\n")
        x3 = build("x863", "x86_64", "xarch.h\tbb\texact\n")
        refused("adopt.x86.xarch_differs", lambda: adopt(ns(a3, x86=x3), env), "determinism bug")
        x4 = build("x864", "x86_64", "a\t1\texact\n")
        refused("adopt.x86.row_sets_differ", lambda: adopt(ns(a2, x86=x4), env), "different rows")
        x5 = build("x865", "x86_64", "a\t1\texact\nb\t3\tabs:0.2\nc\t5\texact\nh\tff\texact\n")
        refused("adopt.x86.tolerance_differs", lambda: adopt(ns(a2, x86=x5), env), "has tolerance")
        x6 = build("x866", "x86_64", "a\t1\texact\nb\t3\tabs:0.1\nc\t5\texact\nh\tff\texact\n", "unreadable")
        refused("adopt.x86.status_checked", lambda: adopt(ns(a2, x86=x6), env), "x86_64 status 'unreadable'")
        # Mode-scoped with --x86: each build must list its provisional Modes.
        a7 = build("arm7", "arm64", "a\t1\texact\n", mode="clean")
        registry(a7, ["bus-g"])
        x7 = build("x867", "x86_64", "a\t1\texact\n", mode="clean")
        refused("adopt.x86.provisional_unknown", lambda: adopt(ns(a7, only=("modes/clean/*",), x86=x7), env),
                "x867")
        check("adopt.refusals_left_tree_clean", git(repo, "status", "--porcelain").strip() == "")

        # A Mode-scoped candidate of a non-provisional Mode, with the registry's list present: adopted.
        registry(x7, ["bus-g"])
        with contextlib.redirect_stdout(io.StringIO()):
            rc = adopt(ns(a7, only=("modes/clean/*",), x86=x7), env)
        check("adopt.mode_not_provisional", rc == 0 and read_rows(root / "base" / "modes" / "clean" / "p.txt")
              == {"a": Row("a", "1", Tol("exact"))})
        sh("git", "add", "-A")
        sh("git", "commit", "-q", "-m", "adopt 3")

        # report: classification from results + JUnit
        rep = t / "rep"
        for name, st in (("fg.a", "pass"), ("fg.b", "golden_drift"), ("fg.c", "golden_missing"),
                         ("fg.d", "spec_fail")):
            write_atomic(rep / "probe-results" / (name + ".json"),
                         json.dumps({"probe": name, "mode": "", "arch": "arm64", "status": st, "spec_pass": 1,
                                     "spec_fail": 1 if st == "spec_fail" else 0, "golden_rows": 1,
                                     "golden_fail": 1, "golden_new": 0, "golden_missing_rows": 0, "ms": 1}))
        junit = rep / "j.xml"
        junit.write_text('<testsuite>'
                         '<testcase name="fg.a" time="1" status="run"/>'
                         '<testcase name="fg.b" time="2" status="run"/>'
                         '<testcase name="fg.c" time="3" status="run"/>'
                         '<testcase name="fg.d" time="4" status="fail"><failure message="Failed"/></testcase>'
                         '<testcase name="fg.e" time="5" status="fail"><failure message="Timeout"/></testcase>'
                         '<testcase name="fg.f" time="0" status="disabled"/>'
                         '<testcase name="fg.lint" time="1" status="run"/>'
                         '</testsuite>')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = cmd_report(argparse.Namespace(build=str(rep), golden_root=str(root), junit=str(junit)))
        text = out.getvalue()
        check("report.exit_blocking", rc == 1)
        check("report.groups", "BLOCKING (3)" in text and "DRIFT (1)" in text and "MISSING (1)" in text)
        check("report.summary", "SUMMARY  tests 7: pass 2, drift 1, missing 1, blocking 3 (disabled 1)" in text)
        (rep / "probe-results" / "fg.d.json").unlink()
        junit.write_text('<testsuite><testcase name="fg.a" time="1" status="run"/>'
                         '<testcase name="fg.c" time="3" status="run"/></testsuite>')
        with contextlib.redirect_stdout(io.StringIO()):
            rc = cmd_report(argparse.Namespace(build=str(rep), golden_root=str(root), junit=str(junit)))
        check("report.exit_clean", rc == 0)

        # The command line exactly as FCompressor's Scripts/golden.py builds it: every subcommand takes --allow-env
        # (R-G1 #1); only adopt reads it.
        def cli(argv: list[str]) -> int:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                try:
                    return main(argv)
                except SystemExit as e:                          # argparse's usage error
                    return 100 + (e.code if isinstance(e.code, int) else 1)

        wrapped = ["--allow-env", "FCMP_ALLOW_BLESS", "--golden-root", str(root)]
        check("cli.report_accepts_allow_env", cli(["report", str(rep)] + wrapped + ["--junit", str(junit)]) == 0)
        check("cli.diff_accepts_allow_env", cli(["diff", str(rep)] + wrapped + ["--only", "fg.*"]) == 0)
        check("cli.adopt_refuses_other_names",
              cli(["adopt", str(arm), "--allow-env", "SHLVL", "--golden-root", str(root), "--only", "p",
                   "--reason", "selftest"]) == 2)
        check("cli.unknown_flag_is_usage_error", cli(["report", str(rep), "--bogus"]) == 102)

    print("golden.py selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---- CLI -------------------------------------------------------------------------------------------------------------

def default_golden_root() -> str:
    return str(Path(__file__).resolve().parent.parent / "test" / "golden")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="golden.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("build")
        p.add_argument("--golden-root", default=default_golden_root())
        # Accepted by report and diff too (and ignored there), so a wrapper can pass it to every subcommand (R-G1 #1).
        p.add_argument("--allow-env", default="FUNKGUI_ALLOW_BLESS")

    p = sub.add_parser("report")
    common(p)
    p.add_argument("--junit")
    p.set_defaults(fn=cmd_report)
    p = sub.add_parser("diff")
    common(p)
    p.add_argument("--only", action="append", default=[])
    p.set_defaults(fn=cmd_diff)
    p = sub.add_parser("adopt")
    common(p)
    p.add_argument("--x86")
    p.add_argument("--only", action="append", default=[])
    p.add_argument("--reason")
    p.add_argument("--allow-geometry", action="store_true")
    p.set_defaults(fn=cmd_adopt)
    p = sub.add_parser("selftest")
    p.set_defaults(fn=cmd_selftest)

    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except Refused as e:
        print("golden.py %s: REFUSED: %s" % (a.cmd, e), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
