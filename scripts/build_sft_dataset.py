#!/usr/bin/env python3
import argparse
import csv
import json
import random
from collections import Counter, defaultdict
from pathlib import Path


METHODS = [
    "directive",
    "fine",
    "round_robin",
    "dependent",
    "spt",
    "lpt",
    "least_flexible",
    "slack_based",
]

LABELS = {
    "directive": "DIRECTIVE",
    "fine": "FINE",
    "round_robin": "ROUND_ROBIN",
    "dependent": "DEPENDENT",
    "spt": "SPT",
    "lpt": "LPT",
    "least_flexible": "LEAST_FLEXIBLE",
    "slack_based": "SLACK_BASED",
}

SYSTEM_PROMPT = (
    "You choose one front-sorting heuristic for a production scheduling "
    "solver. Available heuristics: "
    "DIRECTIVE prioritizes works with earlier deadlines; it is often "
    "strong when deadlines are tight or slack is small. "
    "FINE prioritizes works with larger lateness penalty; use it when "
    "penalty coefficients are highly uneven, not merely because fines "
    "exist. "
    "ROUND_ROBIN rotates priority between works; use it when many works "
    "compete and the graph/resource structure is dense. "
    "DEPENDENT prioritizes operations that unlock many immediate child "
    "operations in the dependency graph. "
    "SPT prioritizes shorter operations by minimum processing time. "
    "LPT prioritizes longer operations by minimum processing time. "
    "LEAST_FLEXIBLE prioritizes operations with fewer possible tools. "
    "SLACK_BASED prioritizes works with the smallest remaining time "
    "reserve: deadline minus estimated remaining work. "
    "Before answering, internally score each heuristic from 0 to 3 for "
    "expected final score on this task: 3 means likely best, 0 means "
    "likely worst. Consider slack, matrix density, in/out-degree, "
    "processing-time spread, resource load, fine spread, operation "
    "count, and work count. Choose the heuristic with "
    "the highest internal score. Break ties by expected lower final "
    "penalty, not by name order. Return exactly one token from: "
    "DIRECTIVE, FINE, ROUND_ROBIN, DEPENDENT, SPT, LPT, "
    "LEAST_FLEXIBLE, SLACK_BASED. Do not output the scores. No "
    "explanations."
)


def load_prompts(path: Path) -> dict[int, dict]:
    prompts = {}
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            item = json.loads(line)
            prompts[int(item["task_id"])] = item
    return prompts


def user_content(prompt_item: dict) -> str:
    for message in prompt_item["messages"]:
        if message.get("role") == "user":
            return message["content"]
    raise ValueError(f"Prompt for task_id={prompt_item.get('task_id')} has no user message")


def build_examples(wide_csv: Path, prompts_jsonl: Path, epsilon: float) -> tuple[list[dict], dict]:
    prompts = load_prompts(prompts_jsonl)
    examples = []
    stats = {
        "total_rows": 0,
        "valid_non_dummy_rows": 0,
        "invalid_task_ids": [],
        "tied_task_count": 0,
        "tie_size_counts": Counter(),
        "all_same_score_count": 0,
        "all_zero_score_count": 0,
        "missing_prompt_task_ids": [],
    }

    with wide_csv.open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            stats["total_rows"] += 1
            task_id = int(row["task_id"])

            if any(row[f"{method}_valid"] != "1" for method in METHODS):
                stats["invalid_task_ids"].append(task_id)
                continue

            stats["valid_non_dummy_rows"] += 1
            scores = {method: float(row[f"{method}_score"]) for method in METHODS}
            min_score = min(scores.values())
            max_score = max(scores.values())
            best = [
                method
                for method in METHODS
                if abs(scores[method] - min_score) <= epsilon
            ]

            if abs(max_score - min_score) <= epsilon:
                stats["all_same_score_count"] += 1
                if abs(min_score) <= epsilon:
                    stats["all_zero_score_count"] += 1

            if len(best) != 1:
                stats["tied_task_count"] += 1
                stats["tie_size_counts"][len(best)] += 1
                continue

            prompt = prompts.get(task_id)
            if prompt is None:
                stats["missing_prompt_task_ids"].append(task_id)
                continue

            label = LABELS[best[0]]
            examples.append(
                {
                    "task_id": task_id,
                    "seed": int(row["seed"]),
                    "task_source": row.get("task_source", ""),
                    "best_heuristic": label,
                    "best_score": min_score,
                    "messages": [
                        {"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": user_content(prompt)},
                        {"role": "assistant", "content": label},
                    ],
                }
            )

    stats["unique_best_count"] = len(examples)
    stats["label_counts"] = Counter(example["best_heuristic"] for example in examples)
    return examples, stats


def stratified_split(examples: list[dict], validation_ratio: float, seed: int) -> tuple[list[dict], list[dict]]:
    by_label = defaultdict(list)
    for example in examples:
        by_label[example["best_heuristic"]].append(example)

    rng = random.Random(seed)
    train = []
    validation = []
    for label_examples in by_label.values():
        rng.shuffle(label_examples)
        val_count = max(1, round(len(label_examples) * validation_ratio))
        validation.extend(label_examples[:val_count])
        train.extend(label_examples[val_count:])

    train.sort(key=lambda x: x["task_id"])
    validation.sort(key=lambda x: x["task_id"])
    return train, validation


def write_jsonl(path: Path, examples: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for example in examples:
            f.write(json.dumps(example, ensure_ascii=False) + "\n")


def write_labels_csv(path: Path, examples: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["task_id", "seed", "task_source", "best_heuristic", "best_score"],
        )
        writer.writeheader()
        for example in examples:
            writer.writerow(
                {
                    "task_id": example["task_id"],
                    "seed": example["seed"],
                    "task_source": example["task_source"],
                    "best_heuristic": example["best_heuristic"],
                    "best_score": example["best_score"],
                }
            )


def serializable_stats(stats: dict, train: list[dict], validation: list[dict]) -> dict:
    return {
        "total_rows": stats["total_rows"],
        "valid_non_dummy_rows": stats["valid_non_dummy_rows"],
        "invalid_task_ids": stats["invalid_task_ids"],
        "unique_best_count": stats["unique_best_count"],
        "tied_task_count": stats["tied_task_count"],
        "tie_size_counts": {
            str(k): v for k, v in sorted(stats["tie_size_counts"].items())
        },
        "all_same_score_count": stats["all_same_score_count"],
        "all_zero_score_count": stats["all_zero_score_count"],
        "missing_prompt_task_ids": stats["missing_prompt_task_ids"],
        "label_counts": dict(sorted(stats["label_counts"].items())),
        "train_count": len(train),
        "validation_count": len(validation),
        "train_label_counts": dict(sorted(Counter(x["best_heuristic"] for x in train).items())),
        "validation_label_counts": dict(
            sorted(Counter(x["best_heuristic"] for x in validation).items())
        ),
        "methods": METHODS,
    }


def write_summary_md(path: Path, summary: dict) -> None:
    lines = [
        "# SFT Dataset Summary",
        "",
        f"- total rows: {summary['total_rows']}",
        f"- valid non-dummy rows: {summary['valid_non_dummy_rows']}",
        f"- unique-best examples: {summary['unique_best_count']}",
        f"- tied tasks excluded: {summary['tied_task_count']}",
        f"- all non-dummy methods same score: {summary['all_same_score_count']}",
        f"- all non-dummy methods zero score: {summary['all_zero_score_count']}",
        f"- train examples: {summary['train_count']}",
        f"- validation examples: {summary['validation_count']}",
        "",
        "## Labels",
        "",
    ]
    for label, count in summary["label_counts"].items():
        lines.append(f"- {label}: {count}")
    lines.extend(["", "## Train Labels", ""])
    for label, count in summary["train_label_counts"].items():
        lines.append(f"- {label}: {count}")
    lines.extend(["", "## Validation Labels", ""])
    for label, count in summary["validation_label_counts"].items():
        lines.append(f"- {label}: {count}")
    lines.extend(["", "## Tie Sizes Excluded", ""])
    for size, count in summary["tie_size_counts"].items():
        lines.append(f"- {size}: {count}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build no-dummy unique-best SFT dataset from baseline results."
    )
    parser.add_argument("--wide-csv", required=True, type=Path)
    parser.add_argument("--prompts-jsonl", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--validation-ratio", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=777)
    parser.add_argument("--epsilon", type=float, default=1e-9)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    examples, stats = build_examples(args.wide_csv, args.prompts_jsonl, args.epsilon)
    train, validation = stratified_split(examples, args.validation_ratio, args.seed)

    write_jsonl(args.out_dir / "sft_unique_best_no_dummy_all.jsonl", examples)
    write_jsonl(args.out_dir / "sft_unique_best_no_dummy_train.jsonl", train)
    write_jsonl(args.out_dir / "sft_unique_best_no_dummy_validation.jsonl", validation)
    write_labels_csv(args.out_dir / "sft_unique_best_no_dummy_labels.csv", examples)

    summary = serializable_stats(stats, train, validation)
    (args.out_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    write_summary_md(args.out_dir / "summary.md", summary)

    print(f"Wrote {len(examples)} examples to {args.out_dir}")
    print(f"Train: {len(train)}")
    print(f"Validation: {len(validation)}")
    print("Labels:")
    for label, count in summary["label_counts"].items():
        print(f"  {label}: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
