#!/usr/bin/env python3
import csv
import math
from collections import defaultdict
from pathlib import Path
import argparse


def to_float(v: str) -> float:
    try:
        return float(v)
    except Exception:
        return math.nan


def to_int(v: str) -> int:
    try:
        return int(v)
    except Exception:
        return 0


def load_rows(path: Path):
    rows = []
    with path.open('r', encoding='utf-8', newline='') as f:
        reader = csv.DictReader(f)
        required = {"task_id", "seed", "method", "valid", "score", "runtime_ms"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise RuntimeError(f"Missing required columns: {sorted(missing)}")
        for r in reader:
            rows.append(
                {
                    "task_id": to_int(r.get("task_id", "0")),
                    "seed": r.get("seed", ""),
                    "method": r.get("method", "").strip(),
                    "valid": to_int(r.get("valid", "0")),
                    "score": to_float(r.get("score", "nan")),
                    "runtime_ms": to_float(r.get("runtime_ms", "nan")),
                }
            )
    return rows


def method_summary(rows):
    stats = defaultdict(lambda: {"total": 0, "valid": 0, "scores": [], "runtimes": []})
    for r in rows:
        m = r["method"]
        st = stats[m]
        st["total"] += 1
        st["runtimes"].append(r["runtime_ms"])
        if r["valid"] == 1:
            st["valid"] += 1
            st["scores"].append(r["score"])

    out = []
    for m, st in sorted(stats.items()):
        total = st["total"]
        valid = st["valid"]
        valid_rate = (valid / total * 100.0) if total else 0.0
        mean_score = sum(st["scores"]) / len(st["scores"]) if st["scores"] else math.nan
        mean_runtime = sum(st["runtimes"]) / len(st["runtimes"]) if st["runtimes"] else math.nan
        out.append(
            {
                "method": m,
                "total_runs": total,
                "valid_runs": valid,
                "valid_rate_pct": round(valid_rate, 4),
                "mean_score_valid_only": round(mean_score, 6) if not math.isnan(mean_score) else "nan",
                "mean_runtime_ms": round(mean_runtime, 6) if not math.isnan(mean_runtime) else "nan",
            }
        )
    return out


def build_task_method_map(rows):
    tm = defaultdict(dict)
    for r in rows:
        tm[(r["task_id"], r["seed"])][r["method"]] = r
    return tm


def compare_two(a, b):
    # lower score is better; valid beats invalid
    if a["valid"] == 1 and b["valid"] == 0:
        return "win"
    if a["valid"] == 0 and b["valid"] == 1:
        return "lose"
    if a["valid"] == 0 and b["valid"] == 0:
        return "tie"

    if a["score"] < b["score"]:
        return "win"
    if a["score"] > b["score"]:
        return "lose"
    return "tie"


def pairwise_vs_target(rows, target_method: str):
    tm = build_task_method_map(rows)
    methods = sorted({r["method"] for r in rows})
    baselines = [m for m in methods if m != target_method]

    out = []
    for base in baselines:
        wins = loses = ties = compared = 0
        for _, per_method in tm.items():
            if target_method not in per_method or base not in per_method:
                continue
            compared += 1
            res = compare_two(per_method[target_method], per_method[base])
            if res == "win":
                wins += 1
            elif res == "lose":
                loses += 1
            else:
                ties += 1
        if compared == 0:
            continue
        out.append(
            {
                "target_method": target_method,
                "baseline_method": base,
                "compared_tasks": compared,
                "wins": wins,
                "loses": loses,
                "ties": ties,
                "win_rate_pct": round(wins / compared * 100.0, 4),
                "lose_rate_pct": round(loses / compared * 100.0, 4),
                "tie_rate_pct": round(ties / compared * 100.0, 4),
            }
        )
    return out


def write_csv(path: Path, rows):
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


def write_markdown(path: Path, summary_rows, pairwise_rows, target):
    lines = []
    lines.append("# Experiment Summary")
    lines.append("")
    lines.append("## Method summary")
    lines.append("")
    if summary_rows:
        hdr = list(summary_rows[0].keys())
        lines.append("| " + " | ".join(hdr) + " |")
        lines.append("|" + "|".join(["---"] * len(hdr)) + "|")
        for r in summary_rows:
            lines.append("| " + " | ".join(str(r[k]) for k in hdr) + " |")
    else:
        lines.append("No rows")

    lines.append("")
    lines.append(f"## Pairwise: {target} vs baselines")
    lines.append("")
    if pairwise_rows:
        hdr = list(pairwise_rows[0].keys())
        lines.append("| " + " | ".join(hdr) + " |")
        lines.append("|" + "|".join(["---"] * len(hdr)) + "|")
        for r in pairwise_rows:
            lines.append("| " + " | ".join(str(r[k]) for k in hdr) + " |")
    else:
        lines.append("No comparable rows for target method")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_html(path: Path, summary_rows, pairwise_rows, target):
    def table_html(table_id: str, rows):
        if not rows:
            return f"<p>No rows for {table_id}</p>"
        headers = list(rows[0].keys())
        thead = "".join(
            f"<th onclick=\"sortTable('{table_id}', {idx})\">{h}</th>"
            for idx, h in enumerate(headers)
        )
        body = []
        for r in rows:
            body.append(
                "<tr>" + "".join(f"<td>{r[h]}</td>" for h in headers) + "</tr>"
            )
        return (
            f"<div class='table-wrap'>"
            f"<input class='filter' placeholder='Filter rows...' "
            f"oninput=\"filterTable('{table_id}', this.value)\"/>"
            f"<table id='{table_id}'><thead><tr>{thead}</tr></thead>"
            f"<tbody>{''.join(body)}</tbody></table></div>"
        )

    html = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Experiment Summary</title>
  <style>
    body {{ font-family: Segoe UI, Arial, sans-serif; margin: 24px; }}
    h1, h2 {{ margin: 0 0 12px; }}
    .table-wrap {{ margin: 12px 0 28px; }}
    .filter {{ margin: 0 0 8px; width: 360px; max-width: 100%; padding: 6px 8px; }}
    table {{ border-collapse: collapse; width: 100%; }}
    th, td {{ border: 1px solid #d7d7d7; padding: 6px 8px; font-size: 14px; }}
    th {{ background: #f5f7fa; cursor: pointer; user-select: none; }}
    tr:nth-child(even) td {{ background: #fafafa; }}
    .hint {{ color: #666; font-size: 13px; margin-bottom: 8px; }}
  </style>
</head>
<body>
  <h1>Experiment Summary</h1>
  <h2>Method summary</h2>
  <p class="hint">Click a column header to sort. Use filter input to search rows.</p>
  {table_html("summary_table", summary_rows)}
  <h2>Pairwise: {target} vs baselines</h2>
  <p class="hint">Click a column header to sort. Use filter input to search rows.</p>
  {table_html("pairwise_table", pairwise_rows)}
  <script>
    function filterTable(tableId, query) {{
      const q = (query || '').toLowerCase();
      const rows = document.querySelectorAll('#' + tableId + ' tbody tr');
      rows.forEach((row) => {{
        const text = row.innerText.toLowerCase();
        row.style.display = text.includes(q) ? '' : 'none';
      }});
    }}
    function sortTable(tableId, colIdx) {{
      const table = document.getElementById(tableId);
      const tbody = table.querySelector('tbody');
      const rows = Array.from(tbody.querySelectorAll('tr'));
      const dirKey = 'sortDir_' + tableId + '_' + colIdx;
      const asc = !(window[dirKey] === true);
      rows.sort((a, b) => {{
        const av = a.children[colIdx].innerText.trim();
        const bv = b.children[colIdx].innerText.trim();
        const an = Number(av), bn = Number(bv);
        let cmp = 0;
        if (!Number.isNaN(an) && !Number.isNaN(bn)) cmp = an - bn;
        else cmp = av.localeCompare(bv);
        return asc ? cmp : -cmp;
      }});
      rows.forEach((r) => tbody.appendChild(r));
      window[dirKey] = asc;
    }}
  </script>
</body>
</html>
"""
    path.write_text(html, encoding="utf-8")


def main():
    p = argparse.ArgumentParser(description="Summarize long experiment CSV")
    p.add_argument("--input", default="build-gcc/baseline_results_long.csv")
    p.add_argument("--target", default="llm")
    p.add_argument("--out-dir", default="build-gcc/summary")
    args = p.parse_args()

    in_path = Path(args.input)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(in_path)
    summary_rows = method_summary(rows)
    pairwise_rows = pairwise_vs_target(rows, args.target)

    write_csv(out_dir / "method_summary.csv", summary_rows)
    write_csv(out_dir / "pairwise_vs_target.csv", pairwise_rows)
    write_markdown(out_dir / "summary_report.md", summary_rows, pairwise_rows, args.target)
    write_html(out_dir / "summary_report.html", summary_rows, pairwise_rows, args.target)

    print(f"Input: {in_path}")
    print(f"Rows: {len(rows)}")
    print(f"Saved: {out_dir / 'method_summary.csv'}")
    print(f"Saved: {out_dir / 'pairwise_vs_target.csv'}")
    print(f"Saved: {out_dir / 'summary_report.md'}")
    print(f"Saved: {out_dir / 'summary_report.html'}")


if __name__ == "__main__":
    main()
