"""Opt-in gdb-stub mode for the harness (`run.py --gdb`) and the SVC-trace post-pass (`--svc-trace`).

The measurements themselves live in a private runtime kit outside this repository (by default
~/Downloads/OOT/ghidra/runtime: rsp_probe.py, svc_trace_parse.py). It carries game-specific
addresses, so the public tree only references it by path and runs it with `python3 -I`. Without
the kit the harness still continues the guest (the stub holds it at boot until a client sends
`c`) with the minimal client below and skips the measurements with a message.

Stub facts this relies on (src/core/debugger/):
- it listens on 0.0.0.0:<gdbstub_port> (debugger.cpp:146), logs "Starting server on port N..."
  (debugger.cpp:138), serves one client at a time and pauses the guest on every connect
  (debugger.cpp:159-168);
- `c` resumes every guest thread (gdbstub.cpp:283-286, debugger.cpp:248-250).
"""
import csv
import re
import socket
import subprocess
import time
from pathlib import Path

DEF_KIT = Path.home() / "Downloads/OOT/ghidra/runtime"
SERVER_LINE = re.compile(r"Starting server on port (\d+)")
MAIN_LINE = re.compile(r"Module main loaded at (0x[0-9a-fA-F]+)")
FRAME_LINE = re.compile(r"^frame (\d+) kick (\d+) join_bp (\d+) join_ms (\d+)\s+wall ([\d.]+) ms")


def kit_tools(kit):
    """{'rsp_probe': Path|None, 'svc_trace_parse': Path|None} for the kit directory."""
    kit = Path(kit).expanduser()
    out = {}
    for name in ("rsp_probe", "svc_trace_parse"):
        p = kit / f"{name}.py"
        out[name] = p if p.is_file() else None
    return out


def wait_log(path, pattern, timeout, alive=lambda: True):
    """First match of `pattern` in the emulator log within `timeout` s, or None."""
    end = time.time() + timeout
    while time.time() < end and alive():
        try:
            m = pattern.search(Path(path).read_text(errors="replace"))
        except OSError:
            m = None
        if m:
            return m
        time.sleep(0.5)
    return None


def raw_continue(port, host="127.0.0.1", wait=30.0):
    """Kit-less fallback: connect and send `c` so a held guest runs. No game data involved."""
    end = time.time() + wait
    while True:
        try:
            s = socket.create_connection((host, port), timeout=5)
            break
        except OSError as e:
            if time.time() > end:
                return f"stub not reachable on {host}:{port}: {e}"
            time.sleep(0.5)
    with s:
        s.sendall(b"$c#63")
        time.sleep(0.3)
    return None


def probe(kit, port, base, args, out_txt, timeout):
    """Run `python3 -I rsp_probe.py --port P [--base B] <args>`; stdout+stderr -> out_txt.
    Returns (returncode or 'timeout', seconds)."""
    tools = kit_tools(kit)
    cmd = ["python3", "-I", str(tools["rsp_probe"]), "--port", str(port)]
    if base:
        cmd += ["--base", base]
    cmd += list(args)
    t = time.time()
    with open(out_txt, "w") as f:
        f.write("$ " + " ".join(cmd) + "\n")
        f.flush()
        try:
            rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout,
                                cwd=str(Path(out_txt).parent)).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    return rc, round(time.time() - t, 1)


# ------------------------------------------------------------------ summaries of the kit's output
def summarize_delta(csv_path):
    try:
        rows = list(csv.DictReader(open(csv_path)))
    except OSError:
        return {}
    vals = [round(float(r["f18_seconds"]), 6) for r in rows if r.get("f18_seconds")]
    if not vals:
        return {"samples": 0}
    hist = {}
    for v in vals:
        hist[v] = hist.get(v, 0) + 1
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:8]
    return {"samples": len(vals), "distinct": len(hist), "min": min(vals), "max": max(vals),
            "top_values": [[v, n] for v, n in top]}


def summarize_pcs(csv_path, top=10):
    try:
        rows = list(csv.DictReader(open(csv_path)))
    except OSError:
        return {}
    if not rows:
        return {"samples": 0}
    by_name = {}
    for r in rows:
        by_name.setdefault(r["name"], []).append(r)
    main = next((n for n in by_name if "MainThread" in n), None) or max(by_name, key=lambda n: len(by_name[n]))
    counts = {}
    for r in by_name[main]:
        key = r["pc_ghidra"] or ("live " + r["pc"])
        counts[key] = counts.get(key, 0) + 1
    n = len(by_name[main])
    return {"thread": main, "samples": n,
            "top": [[k, c, round(100.0 * c / n, 1)] for k, c in sorted(counts.items(), key=lambda kv: -kv[1])[:top]]}


def summarize_count(txt_path):
    try:
        lines = Path(txt_path).read_text(errors="replace").splitlines()
    except OSError:
        return {}
    rows = [tuple(float(x) for x in m.groups()) for m in map(FRAME_LINE.match, lines) if m]
    if not rows:
        return {"frames": 0}
    n = len(rows)
    mean = lambda i: round(sum(r[i] for r in rows) / n, 2)
    return {"frames": n, "kick_per_frame": mean(1), "join_bp_per_frame": mean(2),
            "join_ms_per_frame": mean(3), "wall_ms_per_frame": mean(4),
            "anomalies": sum(1 for ln in lines if ln.strip().startswith("anomaly"))}


def render_md(g, phases):
    """Markdown section for summary.md from run.json's "gdb" block."""
    if not g:
        return []
    L = ["", "## gdb stub (`--gdb`)", ""]
    if g.get("skipped"):
        return L + [f"- skipped: {g['skipped']}"]
    L.append(f"- port {g.get('port')}, main base {g.get('main_base')}, kit `{g.get('kit')}`")
    for k in ("threads", "delta", "pcs", "count"):
        st = g.get("steps", {}).get(k)
        if st:
            L.append(f"- {k}: exit {st.get('rc')} in {st.get('seconds')} s -> `{st.get('file')}`")
    fps = lambda ph: (phases.get(ph, {}).get("log") or {}).get("guest_fps")
    d = g.get("delta") or {}
    if d:
        L += ["", f"**delta** (time+0x18 read while running, {d.get('samples')} samples, guest fps during "
                  f"the read {fps('gdb-delta') if fps('gdb-delta') is not None else '-'}): "
                  f"{d.get('distinct')} distinct values, min {d.get('min')} max {d.get('max')}"]
        L += ["", "| value s | 1/value | samples |", "|---|---|---|"]
        L += [f"| {v} | {round(1 / v, 2) if v else '-'} | {n} |" for v, n in d.get("top_values", [])]
    p = g.get("pcs") or {}
    if p.get("top"):
        L += ["", f"**pcs** (main thread `{p['thread']}`, {p['samples']} runnable samples; guest fps "
                  f"while sampling {fps('gdb-pcs') if fps('gdb-pcs') is not None else '-'}): top 10 pc, Ghidra addresses",
              "", "| pc | samples | % |", "|---|---|---|"]
        L += [f"| `{k}` | {c} | {pct} |" for k, c, pct in p["top"]]
    c = g.get("count") or {}
    if c.get("frames"):
        L += ["", f"**count-frames** ({c['frames']} frames, software breakpoints): kick "
                  f"{c['kick_per_frame']}/frame, join_bp {c['join_bp_per_frame']}/frame, join_ms "
                  f"{c['join_ms_per_frame']}/frame; {c['wall_ms_per_frame']} ms wall per frame under "
                  f"breakpoints (not the real frame time); step anomalies {c['anomalies']}"]
    return L


def svc_trace(kit, log_path, threads_file, out_md, timeout=600):
    tools = kit_tools(kit)
    if not tools["svc_trace_parse"]:
        Path(out_md).write_text(f"svc_trace_parse.py not found in {kit}; the private runtime kit is not "
                                "part of this repository. Re-run it later on eden_log.txt.\n")
        return "kit missing"
    cmd = ["python3", "-I", str(tools["svc_trace_parse"]), str(log_path)]
    if threads_file and Path(threads_file).exists():
        cmd += ["--threads", str(threads_file)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        rc, out = r.returncode, r.stdout + (("\n[stderr]\n" + r.stderr) if r.stderr.strip() else "")
    except subprocess.TimeoutExpired:
        rc, out = "timeout", ""
    Path(out_md).write_text("```\n$ " + " ".join(cmd) + f"\n(exit {rc})\n\n" + out + "\n```\n")
    return rc
