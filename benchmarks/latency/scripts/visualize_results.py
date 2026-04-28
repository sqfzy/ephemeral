#!/usr/bin/env python3
"""Visualize latency benchmark results from ``benchmarks/latency/outputs/``.

Two viewing axes:

  1. **Kernel vs DPDK** — for each (scenario, measurement) pair the latest
     kernel and dpdk numbers sit side-by-side with a speedup column.
  2. **History** — a per-group time-series chart shows p50 / p99 evolution
     across every timestamped JSON in the input directory.

The script consumes the JSON shape written by ``bench::Recorder``::

    {"name": "lat_tcp_kernel_rtt",
     "timestamp": "2026-04-22_10-19-44",
     "samples": {...},
     "latency_ns": {"avg":..., "p50":..., "p99":..., ...},
     "histogram_memory_bytes": ...}

Filenames follow ``lat_<scenario>_<backend>_<suffix>_YYYY-MM-DD_HH-MM-SS.json``
with ``<backend>`` always ``kernel`` or ``dpdk``. The substring before
``_kernel_`` / ``_dpdk_`` is the scenario stem; the substring after is the
measurement / variant suffix. Grouping by ``(stem, suffix)`` puts the two
backends on the same row so they can be compared directly.

Outputs:

  • Terminal table written to stdout (machine-friendly TSV) + a human
    table to stderr.
  • Optional self-contained HTML report (charts inlined as base64 PNGs)
    written to ``--output``. Skip with ``--no-html``.

Usage::

    python3 visualize_results.py                       # default report
    python3 visualize_results.py --dry-run             # show plan only
    python3 visualize_results.py -v --output /tmp/r.html

Exit codes:
  0  success
  1  usage / argument error
  2  runtime error (no inputs, IO failure, ...)
  3  user declined overwrite confirmation
"""
from __future__ import annotations

import argparse
import base64
import datetime as _dt
import io
import json
import os
import re
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Iterable

# ----- constants -------------------------------------------------------------

SCRIPT_NAME = Path(__file__).name
# Reflect schema: latency_ns keys we know how to chart.
KNOWN_METRICS: tuple[str, ...] = (
    "avg", "min", "max", "stddev", "p50", "p90", "p99", "p99_9",
)
DEFAULT_METRICS: tuple[str, ...] = ("p50", "p99")
BACKENDS: tuple[str, ...] = ("kernel", "dpdk")

# Filename: lat_<...>_(kernel|dpdk)_<...>_YYYY-MM-DD_HH-MM-SS.json
# We don't anchor the stem because some scenarios contain underscores
# (lat_ex_market_2p, lat_ex_md_udp, ...).
_TS_RE = re.compile(
    r"^(?P<stem>.+?)_(?P<backend>kernel|dpdk)_(?P<suffix>.+?)"
    r"_(?P<ts>\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})\.json$"
)
_TS_FMT = "%Y-%m-%d_%H-%M-%S"

# HTML output marker — lets us recognize our own previous report and
# silently overwrite it without a confirmation prompt.
_HTML_MARKER = "<!-- generated-by: visualize_results.py -->"

# ----- logging ---------------------------------------------------------------

_VERBOSE = False


def log_debug(msg: str) -> None:
    if _VERBOSE:
        print(f"[DEBUG] {msg}", file=sys.stderr)


def log_info(msg: str) -> None:
    print(f"[INFO]  {msg}", file=sys.stderr)


def log_warn(msg: str) -> None:
    print(f"[WARN]  {msg}", file=sys.stderr)


def log_error(msg: str) -> None:
    print(f"[ERROR] {msg}", file=sys.stderr)


def die(msg: str, code: int = 2) -> None:
    log_error(msg)
    sys.exit(code)


# ----- model -----------------------------------------------------------------

class Run:
    """A single benchmark JSON file, parsed."""

    __slots__ = ("path", "stem", "backend", "suffix", "ts", "latency", "samples")

    def __init__(
        self,
        path: Path,
        stem: str,
        backend: str,
        suffix: str,
        ts: _dt.datetime,
        latency: dict,
        samples: dict,
    ) -> None:
        self.path = path
        self.stem = stem
        self.backend = backend
        self.suffix = suffix
        self.ts = ts
        self.latency = latency
        self.samples = samples

    @property
    def group_key(self) -> tuple[str, str]:
        return (self.stem, self.suffix)

    @property
    def group_label(self) -> str:
        return f"{self.stem}__{self.suffix}"


def parse_run(path: Path) -> Run | None:
    """Parse one JSON file. Returns None on any malformed input (with WARN)."""
    name = path.name
    m = _TS_RE.match(name)
    if not m:
        log_warn(f"skipping {name}: filename doesn't match pattern")
        return None
    try:
        ts = _dt.datetime.strptime(m.group("ts"), _TS_FMT)
    except ValueError as e:
        log_warn(f"skipping {name}: bad timestamp ({e})")
        return None

    try:
        with path.open("r", encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        log_warn(f"skipping {name}: cannot read ({e})")
        return None
    except json.JSONDecodeError as e:
        log_warn(f"skipping {name}: bad JSON ({e})")
        return None

    latency = doc.get("latency_ns")
    samples = doc.get("samples", {})
    if not isinstance(latency, dict):
        log_warn(f"skipping {name}: missing latency_ns block")
        return None

    return Run(
        path=path,
        stem=m.group("stem"),
        backend=m.group("backend"),
        suffix=m.group("suffix"),
        ts=ts,
        latency=latency,
        samples=samples if isinstance(samples, dict) else {},
    )


def load_runs(input_dir: Path) -> list[Run]:
    files = sorted(input_dir.glob("*.json"))
    if not files:
        die(f"no *.json files under {input_dir}")
    runs: list[Run] = []
    total = len(files)
    log_info(f"parsing {total} JSON file(s) from {input_dir}")
    for idx, p in enumerate(files, start=1):
        if idx % 50 == 0 or idx == total:
            log_debug(f"  progress: {idx}/{total}")
        r = parse_run(p)
        if r is not None:
            runs.append(r)
    if not runs:
        die("all input files were unparseable")
    log_info(f"loaded {len(runs)} run(s) across {len({r.group_key for r in runs})} group(s)")
    return runs


# ----- aggregation -----------------------------------------------------------

class Group:
    """All runs sharing (stem, suffix), split by backend."""

    __slots__ = ("stem", "suffix", "by_backend")

    def __init__(self, stem: str, suffix: str) -> None:
        self.stem = stem
        self.suffix = suffix
        # Keep history sorted ascending by timestamp.
        self.by_backend: dict[str, list[Run]] = {}

    @property
    def label(self) -> str:
        return f"{self.stem}__{self.suffix}"

    def latest(self, backend: str) -> Run | None:
        runs = self.by_backend.get(backend)
        return runs[-1] if runs else None


def group_runs(runs: list[Run]) -> list[Group]:
    bucket: dict[tuple[str, str], Group] = {}
    for r in runs:
        key = r.group_key
        g = bucket.get(key)
        if g is None:
            g = Group(r.stem, r.suffix)
            bucket[key] = g
        g.by_backend.setdefault(r.backend, []).append(r)
    for g in bucket.values():
        for runs_ in g.by_backend.values():
            runs_.sort(key=lambda x: x.ts)
    return sorted(bucket.values(), key=lambda g: (g.stem, g.suffix))


# ----- terminal table --------------------------------------------------------

def _fmt_ns(v: float | None) -> str:
    """Format ns value to a compact human string."""
    if v is None:
        return "-"
    if v >= 1_000_000:
        return f"{v / 1_000_000:.2f}ms"
    if v >= 1_000:
        return f"{v / 1_000:.2f}us"
    return f"{v:.0f}ns"


def _speedup(kernel_ns: float | None, dpdk_ns: float | None) -> str:
    if kernel_ns is None or dpdk_ns is None:
        return "-"
    if dpdk_ns <= 0:
        return "inf"
    return f"{kernel_ns / dpdk_ns:.2f}x"


def render_terminal_table(groups: list[Group], metric: str) -> str:
    """Pretty-print the kernel-vs-dpdk comparison for one metric."""
    if metric not in KNOWN_METRICS:
        raise ValueError(f"unknown metric {metric}")

    headers = ("scenario", "suffix", f"kernel.{metric}", f"dpdk.{metric}", "speedup", "k.runs", "d.runs")
    rows: list[tuple[str, ...]] = []
    for g in groups:
        kr = g.latest("kernel")
        dr = g.latest("dpdk")
        kv = kr.latency.get(metric) if kr else None
        dv = dr.latency.get(metric) if dr else None
        rows.append((
            g.stem,
            g.suffix,
            _fmt_ns(kv),
            _fmt_ns(dv),
            _speedup(kv, dv),
            str(len(g.by_backend.get("kernel", []))),
            str(len(g.by_backend.get("dpdk", []))),
        ))

    widths = [
        max(len(h), max((len(r[i]) for r in rows), default=0))
        for i, h in enumerate(headers)
    ]
    sep = "  "

    def fmt_row(row: Iterable[str]) -> str:
        return sep.join(c.ljust(w) for c, w in zip(row, widths))

    out = [fmt_row(headers), sep.join("-" * w for w in widths)]
    out.extend(fmt_row(r) for r in rows)
    return "\n".join(out)


def render_tsv(groups: list[Group]) -> str:
    """Machine-readable TSV: one row per (group, backend) latest run."""
    cols = ["scenario", "suffix", "backend", "ts", "samples", *KNOWN_METRICS]
    lines = ["\t".join(cols)]
    for g in groups:
        for backend in BACKENDS:
            r = g.latest(backend)
            if not r:
                continue
            row = [g.stem, g.suffix, backend, r.ts.strftime(_TS_FMT),
                   str(r.samples.get("recorded", ""))]
            for m in KNOWN_METRICS:
                v = r.latency.get(m)
                row.append(f"{v:.2f}" if isinstance(v, (int, float)) else "")
            lines.append("\t".join(row))
    return "\n".join(lines)


# ----- charts ----------------------------------------------------------------

# Color / line style mapping shared by both backends.
_BACKEND_COLORS = {"kernel": "#1f77b4", "dpdk": "#d62728"}


def _build_axes(g: Group, metrics: tuple[str, ...]):
    """Common pre-processing for both engines.

    Returns (all_ts, ts_index, series) where ``series`` is a list of dicts:
    {backend, metric, xs (indices), ys (floats), labels (date strings)}.
    Skips metrics that are entirely missing for a backend.
    """
    all_ts = sorted({r.ts for runs in g.by_backend.values() for r in runs})
    ts_index = {t: i for i, t in enumerate(all_ts)}
    series: list[dict] = []
    for backend in BACKENDS:
        runs = g.by_backend.get(backend, [])
        if not runs:
            continue
        for m in metrics:
            ys_raw = [r.latency.get(m) for r in runs]
            if not any(isinstance(y, (int, float)) for y in ys_raw):
                continue
            series.append({
                "backend": backend,
                "metric": m,
                "xs": [ts_index[r.ts] for r in runs],
                "ys": [y if isinstance(y, (int, float)) else None for y in ys_raw],
                "labels": [r.ts.strftime(_TS_FMT) for r in runs],
                "samples": [r.samples.get("recorded", 0) for r in runs],
            })
    return all_ts, ts_index, series


# ---- matplotlib (PNG) -------------------------------------------------------

def _import_matplotlib():
    """Import matplotlib lazily with the headless Agg backend."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    return plt


def render_chart_matplotlib(g: Group, metrics: tuple[str, ...]) -> bytes:
    """PNG renderer (fallback when plotly unavailable). Returns raw PNG bytes."""
    plt = _import_matplotlib()
    all_ts, _, series = _build_axes(g, metrics)
    fig, ax = plt.subplots(figsize=(9.5, 4.0), dpi=110)
    styles = ("-", "--", ":", "-.")
    metric_idx = {m: i for i, m in enumerate(metrics)}
    for s in series:
        ax.plot(
            s["xs"],
            [y if y is not None else float("nan") for y in s["ys"]],
            color=_BACKEND_COLORS[s["backend"]],
            linestyle=styles[metric_idx[s["metric"]] % len(styles)],
            marker="o", markersize=4, linewidth=1.5,
            label=f'{s["backend"]} {s["metric"]}',
        )
    ax.set_xticks(range(len(all_ts)))
    ax.set_xticklabels([t.strftime(_TS_FMT) for t in all_ts],
                       rotation=30, ha="right", fontsize=8)
    ax.set_title(g.label, fontsize=11)
    ax.set_xlabel(f"report # (n={len(all_ts)}, evenly spaced)")
    ax.set_ylabel("latency (ns)")
    ax.grid(True, which="both", linestyle=":", alpha=0.4)
    if series:
        ax.legend(loc="best", fontsize=8, framealpha=0.85)
    fig.tight_layout()
    buf = io.BytesIO()
    fig.savefig(buf, format="png")
    plt.close(fig)
    return buf.getvalue()


# ---- plotly (interactive HTML) ---------------------------------------------

def _import_plotly():
    import plotly.graph_objects as go  # noqa: F401  (lazy import)
    return go


def render_chart_plotly(g: Group, metrics: tuple[str, ...]):
    """Build a plotly Figure for one group. Categorical x-axis (each report
    is one tick), hover tooltips show metric / sample count / latency."""
    go = _import_plotly()
    all_ts, _, series = _build_axes(g, metrics)
    dashes = ("solid", "dash", "dot", "dashdot")
    metric_idx = {m: i for i, m in enumerate(metrics)}

    fig = go.Figure()
    for s in series:
        fig.add_trace(go.Scatter(
            x=s["xs"],
            y=s["ys"],
            mode="lines+markers",
            name=f'{s["backend"]} {s["metric"]}',
            line=dict(
                color=_BACKEND_COLORS[s["backend"]],
                dash=dashes[metric_idx[s["metric"]] % len(dashes)],
                width=2,
            ),
            marker=dict(size=7),
            customdata=list(zip(s["labels"], s["samples"])),
            hovertemplate=(
                f'<b>{s["backend"]} {s["metric"]}</b><br>'
                "ts: %{customdata[0]}<br>"
                "latency: %{y:,.0f} ns<br>"
                "samples: %{customdata[1]:,}"
                "<extra></extra>"
            ),
            connectgaps=False,
        ))

    fig.update_layout(
        title=dict(text=g.label, x=0.02, xanchor="left", font=dict(size=14)),
        xaxis=dict(
            title=f"report # (n={len(all_ts)}, evenly spaced)",
            tickmode="array",
            tickvals=list(range(len(all_ts))),
            ticktext=[t.strftime(_TS_FMT) for t in all_ts],
            tickangle=-30,
            tickfont=dict(size=10),
            showgrid=True, gridcolor="#eee",
        ),
        yaxis=dict(
            title="latency (ns)",
            showgrid=True, gridcolor="#eee",
            rangemode="tozero",
        ),
        legend=dict(orientation="h", yanchor="bottom", y=1.02,
                    xanchor="right", x=1.0),
        margin=dict(l=60, r=20, t=60, b=80),
        height=420,
        plot_bgcolor="white",
        paper_bgcolor="white",
        hovermode="x unified",
        # User can flip y-axis between linear and log via the menu.
        updatemenus=[dict(
            type="buttons", direction="left",
            buttons=[
                dict(label="linear", method="relayout",
                     args=[{"yaxis.type": "linear"}]),
                dict(label="log", method="relayout",
                     args=[{"yaxis.type": "log"}]),
            ],
            pad={"r": 4, "t": 4}, showactive=True,
            x=0.0, xanchor="left", y=1.12, yanchor="top",
            font=dict(size=10),
        )],
    )
    return fig


# ----- HTML report -----------------------------------------------------------

_HTML_HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>ephemeral latency benchmark report</title>
<style>
  body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI',
                       Roboto, sans-serif;
          margin: 24px; color: #222; background: #fafafa; }}
  h1, h2, h3 {{ color: #111; }}
  .meta {{ color: #666; font-size: 0.9em; margin-bottom: 18px; }}
  table {{ border-collapse: collapse; margin: 12px 0 24px 0; font-size: 0.9em; }}
  th, td {{ border: 1px solid #ccc; padding: 4px 10px; text-align: right; }}
  th {{ background: #eee; }}
  td.scenario, td.suffix {{ text-align: left; font-family: monospace; }}
  td.faster {{ background: #e7f7e7; }}
  td.slower {{ background: #fde7e7; }}
  td.tie    {{ background: #fffbe6; }}
  .group {{ margin-bottom: 28px; }}
  .group img {{ border: 1px solid #ddd; background: #fff; max-width: 100%;
                height: auto; }}
  .legend {{ font-size: 0.85em; color: #666; margin-top: 4px; }}
  details summary {{ cursor: pointer; font-weight: 600; }}
</style>
</head>
<body>
{marker}
<h1>ephemeral latency benchmark report</h1>
<div class="meta">
  generated {generated}<br>
  input: <code>{input_dir}</code><br>
  groups: {n_groups} &middot; runs: {n_runs}
</div>
"""


def _td_speedup(kv: float | None, dv: float | None) -> str:
    if kv is None or dv is None:
        return '<td>-</td>'
    if dv <= 0:
        return '<td>inf</td>'
    s = kv / dv
    if s > 1.10:
        cls = "faster"   # dpdk is faster than kernel
    elif s < 0.90:
        cls = "slower"   # dpdk is slower (rare but possible)
    else:
        cls = "tie"
    return f'<td class="{cls}">{s:.2f}x</td>'


def render_summary_html(groups: list[Group], metrics: tuple[str, ...]) -> str:
    head = "<th>scenario</th><th>suffix</th>"
    for m in metrics:
        head += (f"<th>kernel {m}</th><th>dpdk {m}</th>"
                 f"<th>speedup ({m})</th>")
    head += "<th>k.runs</th><th>d.runs</th>"

    rows: list[str] = []
    for g in groups:
        kr = g.latest("kernel")
        dr = g.latest("dpdk")
        cells = [f'<td class="scenario">{g.stem}</td>',
                 f'<td class="suffix">{g.suffix}</td>']
        for m in metrics:
            kv = kr.latency.get(m) if kr else None
            dv = dr.latency.get(m) if dr else None
            cells.append(f'<td>{_fmt_ns(kv)}</td>')
            cells.append(f'<td>{_fmt_ns(dv)}</td>')
            cells.append(_td_speedup(kv, dv))
        cells.append(f'<td>{len(g.by_backend.get("kernel", []))}</td>')
        cells.append(f'<td>{len(g.by_backend.get("dpdk", []))}</td>')
        rows.append("<tr>" + "".join(cells) + "</tr>")

    return ("<h2>latest kernel vs dpdk (speedup = kernel / dpdk; "
            "&gt;1.10x highlighted green)</h2>\n"
            "<table>\n<thead><tr>" + head + "</tr></thead>\n<tbody>\n"
            + "\n".join(rows) + "\n</tbody>\n</table>\n")


def render_history_html_matplotlib(
    groups: list[Group], metrics: tuple[str, ...]
) -> str:
    parts = ["<h2>history (kernel = blue, dpdk = red; "
             "solid = first metric, dashed = second)</h2>"]
    total = len(groups)
    for idx, g in enumerate(groups, start=1):
        log_debug(f"  rendering chart {idx}/{total}: {g.label}")
        try:
            png = render_chart_matplotlib(g, metrics)
        except Exception as e:
            log_warn(f"chart failed for {g.label}: {e}")
            continue
        b64 = base64.b64encode(png).decode("ascii")
        parts.append(
            f'<div class="group">\n'
            f'  <h3>{g.label}</h3>\n'
            f'  <img alt="{g.label}" src="data:image/png;base64,{b64}">\n'
            f'</div>'
        )
    return "\n".join(parts)


def render_history_html_plotly(
    groups: list[Group], metrics: tuple[str, ...]
) -> tuple[str, str]:
    """Returns (head_extra, body) where head_extra carries the CDN script tag."""
    try:
        import plotly  # noqa: F401
        from plotly.io import to_html as _to_html
    except ImportError as e:
        raise RuntimeError(
            "plotly not installed. `pip install --user plotly` "
            "or pass --engine matplotlib"
        ) from e

    parts = ["<h2>history (kernel = blue, dpdk = red; "
             "hover for details, drag to zoom, click legend to toggle, "
             "use the linear/log buttons above each chart)</h2>"]
    total = len(groups)
    for idx, g in enumerate(groups, start=1):
        log_debug(f"  rendering plotly chart {idx}/{total}: {g.label}")
        try:
            fig = render_chart_plotly(g, metrics)
        except Exception as e:
            log_warn(f"chart failed for {g.label}: {e}")
            continue
        # First chart includes plotly.js (CDN); the rest reuse the loaded lib.
        include_js = "cdn" if idx == 1 else False
        snippet = _to_html(
            fig,
            include_plotlyjs=include_js,
            full_html=False,
            config={
                "displaylogo": False,
                "responsive": True,
                "modeBarButtonsToRemove": ["select2d", "lasso2d"],
                "toImageButtonOptions": {
                    "format": "png", "filename": g.label, "scale": 2,
                },
            },
        )
        parts.append(
            f'<div class="group">\n  <h3>{g.label}</h3>\n  {snippet}\n</div>'
        )
    return "", "\n".join(parts)


def render_html(
    groups: list[Group],
    metrics: tuple[str, ...],
    input_dir: Path,
    engine: str,
) -> str:
    n_runs = sum(len(rs) for g in groups for rs in g.by_backend.values())
    head = _HTML_HEAD.format(
        marker=_HTML_MARKER,
        generated=_dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        input_dir=str(input_dir),
        n_groups=len(groups),
        n_runs=n_runs,
    )
    body_parts = [render_summary_html(groups, metrics)]
    if engine == "plotly":
        _, body = render_history_html_plotly(groups, metrics)
        body_parts.append(body)
    else:
        body_parts.append(render_history_html_matplotlib(groups, metrics))
    return head + "\n".join(body_parts) + "\n</body></html>\n"


# ----- output writing --------------------------------------------------------

def _looks_like_our_report(path: Path) -> bool:
    """Cheap check so we silently overwrite our own previous output."""
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            head = f.read(2048)
    except OSError:
        return False
    return _HTML_MARKER in head


def confirm_overwrite(path: Path, assume_yes: bool) -> bool:
    if not path.exists():
        return True
    if assume_yes:
        log_info(f"overwriting existing {path} (--yes)")
        return True
    if _looks_like_our_report(path):
        log_info(f"overwriting previous report at {path}")
        return True
    if not sys.stdin.isatty():
        log_error(f"{path} exists and stdin is not a TTY; pass --yes to overwrite")
        return False
    try:
        ans = input(f"{path} exists. Overwrite? [y/N] ").strip().lower()
    except EOFError:
        return False
    return ans in ("y", "yes")


def atomic_write_text(path: Path, content: str) -> None:
    """Write via tmp file + rename so a crash never leaves a half-written report."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# ----- CLI -------------------------------------------------------------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    default_input = (here / ".." / "outputs").resolve()
    # Output sits next to (not inside) outputs/ — outputs/ is typically
    # root-owned because `sudo lat ...` writes the JSONs, so writing into
    # it would need sudo too. The sibling stays user-writable.
    default_output = (here / ".." / "report.html").resolve()

    p = argparse.ArgumentParser(
        prog=SCRIPT_NAME,
        description="Visualize ephemeral latency benchmark results.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Layout:\n"
            "  outputs/lat_<scenario>_<backend>_<suffix>_TS.json   inputs\n"
            "  outputs/report.html                                  generated\n"
        ),
    )
    p.add_argument("--input", "-i", type=Path, default=default_input,
                   help=f"directory of *.json files (default: {default_input})")
    p.add_argument("--output", "-o", type=Path, default=default_output,
                   help=f"HTML report path (default: {default_output})")
    p.add_argument("--metrics", "-m", nargs="+", default=list(DEFAULT_METRICS),
                   metavar="METRIC",
                   help=("latency metrics to chart and tabulate. "
                         f"choose from: {', '.join(KNOWN_METRICS)}. "
                         f"default: {' '.join(DEFAULT_METRICS)}"))
    p.add_argument("--engine", "-e", choices=("plotly", "matplotlib"),
                   default="plotly",
                   help=("chart engine. 'plotly' = interactive (zoom, hover, "
                         "log toggle, ~2 MB CDN-loaded); 'matplotlib' = static "
                         "PNGs embedded as base64. default: plotly"))
    p.add_argument("--no-html", action="store_true",
                   help="skip HTML report; print terminal summary only")
    p.add_argument("--dry-run", action="store_true",
                   help="describe what would be written, don't touch disk")
    p.add_argument("--yes", "-y", action="store_true",
                   help="overwrite existing output without prompting")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="enable DEBUG logging to stderr")
    return p.parse_args(argv)


def validate_args(args: argparse.Namespace) -> None:
    # 5: input validation. Bail before doing real work.
    if not args.input.exists():
        die(f"--input directory does not exist: {args.input}", code=1)
    if not args.input.is_dir():
        die(f"--input is not a directory: {args.input}", code=1)
    bad = [m for m in args.metrics if m not in KNOWN_METRICS]
    if bad:
        die(f"unknown metric(s): {bad}. valid: {KNOWN_METRICS}", code=1)
    if not args.no_html:
        # Output path sanity: must end in .html so we don't clobber random files.
        if args.output.suffix.lower() != ".html":
            die(f"--output must end in .html, got {args.output}", code=1)
        # Reject obviously dangerous paths.
        if str(args.output).startswith("/dev"):
            die(f"refusing to write to {args.output}", code=1)


def main(argv: list[str] | None = None) -> int:
    global _VERBOSE
    args = parse_args(sys.argv[1:] if argv is None else argv)
    _VERBOSE = args.verbose
    validate_args(args)

    args.input = args.input.resolve()
    args.output = args.output.resolve()

    log_info(f"input dir: {args.input}")
    log_info(f"metrics:   {args.metrics}")
    if args.no_html:
        log_info("HTML output: disabled (--no-html)")
    else:
        log_info(f"output:    {args.output}")
    if args.dry_run:
        log_info("DRY RUN — no files will be written")

    runs = load_runs(args.input)
    groups = group_runs(runs)

    # Always emit terminal summary (stderr human, stdout TSV).
    metric_for_table = args.metrics[0]
    log_info(f"summary table uses metric: {metric_for_table}")
    print(render_terminal_table(groups, metric_for_table), file=sys.stderr)
    print(render_tsv(groups))  # stdout: machine-readable

    if args.no_html:
        log_info("done (no HTML)")
        return 0

    if args.dry_run:
        log_info(
            f"[dry-run] would render {len(groups)} chart(s) "
            f"and write {args.output}"
        )
        return 0

    if not confirm_overwrite(args.output, args.yes):
        log_warn("aborted by user / non-tty refusal")
        return 3

    log_info(f"rendering {len(groups)} chart(s) with engine={args.engine} ...")
    try:
        html = render_html(groups, tuple(args.metrics), args.input, args.engine)
    except ImportError as e:
        die(f"missing dependency for engine={args.engine}: {e}. "
            f"try `pip install --user {args.engine}` or pass --no-html.")
    except RuntimeError as e:
        die(str(e))
    except Exception as e:
        die(f"render failed: {e}")

    try:
        atomic_write_text(args.output, html)
    except OSError as e:
        die(f"failed to write {args.output}: {e}")

    log_info(f"wrote {args.output} ({len(html):,} bytes)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log_warn("interrupted")
        sys.exit(130)
