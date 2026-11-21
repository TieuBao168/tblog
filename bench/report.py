#!/usr/bin/env python3
"""Render the JSON results of bench_vs_printf / bench_crash_loss as Markdown.

    python3 -I bench/report.py bench/results/*.json            # full tables
    python3 -I bench/report.py --compact bench/results/*.json  # README-sized tables
    python3 -I bench/report.py --title "CI ubuntu" a.json b.json

Python 3 standard library only. Every number printed comes from the files
given (medians across runs, with the min-max range where it matters).
"""
import argparse
import json
import sys


def si(v, unit=""):
    """1234567 -> '1.23 M'."""
    if v is None:
        return "-"
    for div, suf in ((1e9, " G"), (1e6, " M"), (1e3, " k")):
        if abs(v) >= div:
            return "%.3g%s%s" % (v / div, suf, unit)
    return "%.3g%s" % (v, (" " + unit) if unit else "")


def ns(v):
    """Nanoseconds -> '190 ns' / '3.0 µs' / '1.2 ms' / '1.1 s'."""
    if v is None:
        return "-"
    if v >= 1e9:
        return "%.3g s" % (v / 1e9)
    if v >= 1e6:
        return "%.3g ms" % (v / 1e6)
    if v >= 1e3:
        return "%.3g µs" % (v / 1e3)
    return "%.0f ns" % v


def med(summary, key):
    s = summary.get(key)
    return s["median"] if s else None


def rng(summary, key, fmt):
    s = summary.get(key)
    if not s:
        return "-"
    return "%s [%s–%s]" % (fmt(s["median"]), fmt(s["min"]), fmt(s["max"]))


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def machine_line(env):
    return "%s, %s logical CPUs, %s GiB RAM; %s; %s; build: %s; tblog %s; %s" % (
        env.get("cpu"), env.get("logical_cpus"), env.get("ram_gib"), env.get("os"),
        env.get("compiler"), env.get("build"), env.get("tblog_commit"), env.get("date_utc"))


def cells(doc, scenario):
    return [r for r in doc["results"] if r["scenario"] == scenario]


def grid(doc, scenario):
    """{(variant, threads): cell}, ordered variants, ordered thread counts."""
    g, vs, ts = {}, [], []
    for r in cells(doc, scenario):
        g[(r["variant"], r["threads"])] = r
        if r["variant"] not in vs:
            vs.append(r["variant"])
        if r["threads"] not in ts:
            ts.append(r["threads"])
    return g, vs, sorted(ts)


def perf_full(doc):
    env, cfg = doc["env"], doc["config"]
    out = ["### Performance: bench_vs_printf", "",
           "Machine: " + machine_line(env), "",
           "Label: %s. Runs per cell: %s (after %s warm-up, discarded); median [min–max] across runs. "
           "Per-call timer: %s, overhead %s per call (included in every latency); steady_clock step %s ns." % (
               env.get("label") or "-", cfg["runs"], cfg["warmup_runs"], cfg.get("per_call_timer"),
               ns(cfg["timer_overhead_ns"]["median"]), cfg.get("steady_clock_step_ns")), ""]
    g, vs, ts = grid(doc, "flood")
    if g:
        out += ["**Flood, end-to-end** (lines/s until everything is written and fsynced; "
                "%s lines per run split over the threads):" % si(cfg["flood_lines_per_run"]), ""]
        out.append(table(["threads"] + vs, [[t] + [rng(g[(v, t)]["summary"], "e2e_lines_per_s", si)
                                                   if (v, t) in g else "-" for v in vs] for t in ts]))
        out += ["", "**Flood, logging threads only** (lines/s until the slowest thread left its loop):", ""]
        out.append(table(["threads"] + vs, [[t] + [si(med(g[(v, t)]["summary"], "loop_lines_per_s"))
                                                   if (v, t) in g else "-" for v in vs] for t in ts]))
        out += ["", "**Flood, caller latency per call** (p50 / p99 / p99.9 / max, medians across runs):", ""]
        out.append(table(["threads"] + vs, [[t] + [
            " / ".join(ns(med(g[(v, t)]["summary"], k)) for k in ("lat_p50_ns", "lat_p99_ns", "lat_p999_ns", "lat_max_ns"))
            if (v, t) in g else "-" for v in vs] for t in ts]))
        drops = [(v, t, med(c["summary"], "dropped")) for (v, t), c in g.items() if med(c["summary"], "dropped")]
        if drops:
            out += ["", "Dropped lines (full_policy = drop, median per run of %s): %s." % (
                si(cfg["flood_lines_per_run"]),
                ", ".join("%s %d thr: %s" % (v, t, si(d)) for v, t, d in sorted(drops, key=lambda x: (x[0], x[1]))))]
        bad = [(v, t) for (v, t), c in g.items() if not c["all_runs_ok"]]
        if bad:
            out += ["", "**Incomplete log files in:** " + ", ".join("%s/%d" % x for x in bad)]
        out.append("")
    g, vs, ts = grid(doc, "paced")
    if g:
        out += ["**Paced (open loop), %s lines/s per thread**: latency from the scheduled send time "
                "(p50 / p99 / p99.9 / max) and the rate achieved:" % si(cfg["paced_rate_per_thread"]), ""]
        out.append(table(["threads"] + vs, [[t] + [
            " / ".join(ns(med(g[(v, t)]["summary"], k)) for k in ("lat_p50_ns", "lat_p99_ns", "lat_p999_ns", "lat_max_ns"))
            + " (%s/s)" % si(med(g[(v, t)]["summary"], "achieved_lines_per_s"))
            if (v, t) in g else "-" for v in vs] for t in ts]))
        out.append("")
    fl = flush_rows(doc)
    if fl:
        out += ["**flush.on_level = ERROR** (flood; %s lines per run split over the threads, at most %s ERROR "
                "lines per thread; the ERROR lines evenly spread). sync.on_level: ERROR = the default (each batch "
                "with an ERROR line is also fsynced, on the sync thread), OFF = periodic sync only. Latency "
                "p50 / p99 / p99.9 / max:" % (si(cfg.get("flush_lines_per_run")),
                                              si(cfg.get("flush_max_errors_per_thread"))), ""]
        rows = []
        for t, pct, nosync, c in fl:
            m = c["summary"]
            rows.append([t, "%d%%" % pct, "OFF" if nosync else "ERROR", rng(m, "e2e_lines_per_s", si),
                         si(med(m, "loop_lines_per_s")),
                         " / ".join(ns(med(m, k)) for k in ("info_p50_ns", "info_p99_ns", "info_p999_ns", "info_max_ns"))
                         if "info_p50_ns" in m else "-",
                         " / ".join(ns(med(m, k)) for k in ("error_p50_ns", "error_p99_ns", "error_p999_ns", "error_max_ns"))
                         if "error_p50_ns" in m else "-"])
        out.append(table(["threads", "ERROR lines", "sync.on_level", "end-to-end lines/s", "logging threads lines/s",
                          "INFO call", "ERROR call"], rows))
        bad = [c["variant"] + "/%d" % t for t, _, _, c in fl if not c["all_runs_ok"]]
        if bad:
            out += ["", "**Incomplete log files in:** " + ", ".join(bad)]
        out.append("")
    d = cells(doc, "disabled")
    if d:
        out += ["**Disabled level** (one call that is filtered out, %s calls per run):" % si(cfg["disabled_calls"]), ""]
        out.append(table(["variant", "ns per call"], [[c["variant"], rng(c["summary"], "ns_per_call",
                                                                          lambda x: "%.2f" % x)] for c in d]))
        out.append("")
    return "\n".join(out)


def flush_rows(doc):
    """flush_level cells as (threads, error pct, nosync, cell), sorted."""
    rows = []
    for c in cells(doc, "flush_level"):
        v = c["variant"]
        nosync = "_nosync_" in v
        try:
            pct = int(v.rsplit("_", 1)[1].replace("pct", ""))
        except ValueError:
            continue
        rows.append((c["threads"], pct, nosync, c))
    return sorted(rows, key=lambda r: (r[0], r[2], r[1]))


def flush_compact(doc, threads):
    rows = []
    for t, pct, nosync, c in flush_rows(doc):
        if t not in threads or pct == 0:
            continue
        m = c["summary"]
        rows.append([t, "%d%%" % pct, "OFF" if nosync else "ERROR", si(med(m, "e2e_lines_per_s")),
                     "%s / %s" % (ns(med(m, "error_p50_ns")), ns(med(m, "error_p99_ns"))),
                     "%s / %s" % (ns(med(m, "info_p50_ns")), ns(med(m, "info_p99_ns"))) if "info_p50_ns" in m else "-"])
    return table(["threads", "ERROR lines", "sync.on_level", "end-to-end lines/s", "ERROR call p50 / p99",
                  "INFO call p50 / p99"], rows)


def crash_full(doc):
    env, cfg = doc["env"], doc["config"]
    res = doc["results"]
    vs, ss = [], []
    for r in res:
        if r["variant"] not in vs:
            vs.append(r["variant"])
        if r["scenario"] not in ss:
            ss.append(r["scenario"])
    g = {(r["scenario"], r["variant"]): r for r in res}
    out = ["### Crash / abnormal exit: bench_crash_loss", "",
           "Machine: " + machine_line(env), "",
           "%s trials per cell, %s lines/s per logging thread. Lost = acknowledged lines (the logging call "
           "returned) missing from the log file. Cell: mean / max lines lost (share of trials with any loss)." % (
               cfg["trials"], si(cfg["rate_per_thread"])), ""]
    rows = []
    for s in ss:
        row = [s, g[(s, vs[0])]["threads"]]
        for v in vs:
            c = g.get((s, v))
            if not c:
                row.append("-")
                continue
            m = c["summary"]
            row.append("%.1f / %d (%d%%)" % (m["mean_lost"], m["max_lost"], round(m["pct_trials_with_loss"])))
        rows.append(row)
    out.append(table(["scenario", "threads"] + vs, rows))
    out += ["", "Outcome as expected / tblog crash marker present / duplicates, corrupt, torn lines (sums over "
            "trials) / time from the fatal action to the end of the process (mean, max):", ""]
    rows = []
    for s in ss:
        row = [s, g[(s, vs[0])]["expected_outcome"]]
        for v in vs:
            c = g.get((s, v))
            if not c:
                row.append("-")
                continue
            m = c["summary"]
            mk = (" marker %d/%d" % (m["trials_with_marker"], m["trials"])) if v.startswith("tblog") else ""
            tte = ""
            if m.get("mean_trigger_to_exit_ms", -1) >= 0:
                tte = "; %.1f / %.1f ms" % (m["mean_trigger_to_exit_ms"], m["max_trigger_to_exit_ms"])
            row.append("%d/%d%s; %d, %d, %d%s" % (m["trials_outcome_expected"], m["trials"], mk,
                                                  m["duplicates"], m["corrupt"], m["torn"], tte))
        rows.append(row)
    out.append(table(["scenario", "expected outcome"] + vs, rows))
    ev = [v for v in vs if any(r["variant"] == v and r.get("error_every") for r in res)]
    for v in ev:
        every = next(r["error_every"] for r in res if r["variant"] == v)
        out += ["", "**%s** (every %dth line of each thread is an ERROR line): acknowledged INFO lines lost / "
                "acknowledged ERROR lines lost / lines lost up to each thread's last acknowledged ERROR line "
                "(mean / max over the trials):" % (v, every), ""]
        rows = []
        for s in ss:
            c = g.get((s, v))
            if not c:
                continue
            m = c["summary"]
            rows.append([s, g[(s, v)]["threads"]] + ["%.1f / %d" % (m["mean_" + k], m["max_" + k])
                                                     for k in ("info_missing", "error_missing", "missing_to_last_error")])
        out.append(table(["scenario", "threads", "INFO lost", "ERROR lost", "lost up to the last ERROR"], rows))
    out.append("")
    return "\n".join(out)


def perf_compact(doc, variants, threads):
    g, _, _ = grid(doc, "flood")
    rows = []
    for t in threads:
        for v in variants:
            c = g.get((v, t))
            if not c:
                continue
            m = c["summary"]
            rows.append([t, v, ns(med(m, "lat_p50_ns")), ns(med(m, "lat_p99_ns")), si(med(m, "e2e_lines_per_s"))])
    return table(["threads", "variant", "caller p50", "caller p99", "end-to-end lines/s"], rows)


def crash_compact(doc, variants):
    g = {(r["scenario"], r["variant"]): r for r in doc["results"]}
    ss = []
    for r in doc["results"]:
        if r["scenario"] not in ss:
            ss.append(r["scenario"])
    rows = []
    for s in ss:
        row = [s]
        for v in variants:
            c = g.get((s, v))
            row.append("-" if not c else "%.0f (max %d)" % (c["summary"]["mean_lost"], c["summary"]["max_lost"]))
        rows.append(row)
    return table(["scenario"] + variants, rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--compact", action="store_true", help="README-sized tables")
    ap.add_argument("--title", default="")
    ap.add_argument("--variants", default="fprintf_buffered,fprintf_flush,tblog_block,tblog_drop",
                    help="compact perf table variants")
    ap.add_argument("--crash-variants", default="fprintf_buffered,fprintf_flush,tblog,tblog_flush_error")
    ap.add_argument("--threads", default="1,8", help="compact perf table thread counts")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")  # "µs", "–" also on a Windows console / runner
    parts = []
    if a.title:
        parts.append("## " + a.title + "\n")
    for path in a.files:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        schema = doc.get("schema", "")
        if a.compact:
            parts.append("Machine: " + machine_line(doc["env"]) + "\n")
            if schema.startswith("tblog-bench-vs-printf"):
                parts.append(perf_compact(doc, a.variants.split(","), [int(x) for x in a.threads.split(",")]))
                if flush_rows(doc):
                    parts += ["", flush_compact(doc, [int(x) for x in a.threads.split(",")])]
            elif schema.startswith("tblog-bench-crash-loss"):
                parts.append(crash_compact(doc, a.crash_variants.split(",")))
            parts.append("")
        elif schema.startswith("tblog-bench-vs-printf"):
            parts.append(perf_full(doc))
        elif schema.startswith("tblog-bench-crash-loss"):
            parts.append(crash_full(doc))
        else:
            print("%s: unknown schema %r" % (path, schema), file=sys.stderr)
            return 1
    sys.stdout.write("\n".join(parts) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
