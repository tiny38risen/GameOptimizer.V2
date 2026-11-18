"""PresentMon 원자료로 GameOptimizer 적용 전후(off/on)를 비교한다.

사용법:
    python measurements/analyze.py measurements/20261118-1

폴더 구성:
    presentmon/PresentMon-*.csv   60초 측정 하나당 파일 하나
    logs/*.txt                    GameOptimizer 진단 로그 (적용한 회차마다 하나)

off/on 구분: 직전 CSV 시작 이후, 해당 CSV 시작 이전에 진단 로그가 시작됐으면 on.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.stats import wilcoxon

BOOTSTRAP_ROUNDS = 20_000
SEED = 20261118

METRICS = {
    "fps": ("평균 FPS", "FPS"),
    "frametime": ("평균 프레임타임", "ms"),
    "p99": ("p99 프레임타임", "ms"),
    "low1": ("1% low FPS", "FPS"),
    "jitter": ("프레임 간 변동", "ms"),
    "cpu_busy": ("CPU Busy", "ms"),
    "gpu_busy": ("GPU Busy", "ms"),
}


@dataclass(frozen=True)
class Run:
    start: datetime
    file: Path
    on: bool
    metrics: dict[str, float]


def csv_start(path: Path) -> datetime:
    return datetime.strptime(path.stem.removeprefix("PresentMon-"), "%Y-%m-%dT%H%M%S")


def log_start(path: Path) -> datetime:
    # {PID}_{생성시각}_{yyyyMMdd}_{HHmmss}.txt
    date, time = path.stem.split("_")[-2:]
    return datetime.strptime(date + time, "%Y%m%d%H%M%S")


def frame_metrics(path: Path) -> dict[str, float]:
    frames = pd.read_csv(path, encoding="utf-8-sig", na_values=["NA"])
    ft = frames["MsBetweenPresents"].dropna().to_numpy()
    slowest = np.sort(ft)[-max(1, int(np.ceil(ft.size * 0.01))):]
    return {
        "fps": 1000.0 / ft.mean(),
        "frametime": ft.mean(),
        "p99": float(np.percentile(ft, 99)),
        "low1": 1000.0 / slowest.mean(),
        "jitter": float(np.abs(np.diff(ft)).mean()),
        "cpu_busy": frames["MsCPUBusy"].mean(),
        "gpu_busy": frames["MsGPUBusy"].mean(),
    }


def load_runs(folder: Path) -> list[Run]:
    csvs = sorted(folder.glob("presentmon/PresentMon-*.csv"), key=csv_start)
    logs = sorted(log_start(p) for p in folder.glob("logs/*.txt"))
    runs: list[Run] = []
    previous = datetime.min
    for path in csvs:
        start = csv_start(path)
        on = any(previous < log < start for log in logs)
        runs.append(Run(start, path, on, frame_metrics(path)))
        previous = start
    return runs


def paired(runs: list[Run], metric: str, on_first: bool) -> np.ndarray:
    """인접한 off/on 쌍의 차이(on − off). on_first면 on→off 순서 쌍을 쓴다."""
    diffs = []
    for a, b in zip(runs, runs[1:]):
        first_on, second_on = a.on, b.on
        if first_on == second_on or first_on != on_first:
            continue
        on, off = (a, b) if first_on else (b, a)
        diffs.append(on.metrics[metric] - off.metrics[metric])
    return np.array(diffs)


def bootstrap_ci(diffs: np.ndarray, rng: np.random.Generator) -> tuple[float, float]:
    samples = rng.choice(diffs, size=(BOOTSTRAP_ROUNDS, diffs.size), replace=True).mean(axis=1)
    low, high = np.percentile(samples, [2.5, 97.5])
    return float(low), float(high)


def report(folder: Path) -> str:
    runs = load_runs(folder)
    rng = np.random.default_rng(SEED)
    alpha = 0.05 / len(METRICS)
    lines = [f"# {folder.name}", ""]

    lines += ["## 회차", "", "| 시작 | 상태 | 평균 FPS |", "|---|---|---|"]
    lines += [f"| {r.start:%H:%M:%S} | {'on' if r.on else 'off'} | {r.metrics['fps']:.2f} |" for r in runs]

    lines += ["", f"## off→on 쌍 비교 (Bonferroni 기준 p<{alpha:.4f})", "",
              "| 지표 | off 평균 | on 평균 | 변화 | 쌍별 차이 평균 | 95% CI | on 우세 쌍 | p |",
              "|---|---|---|---|---|---|---|---|"]
    for key, (name, unit) in METRICS.items():
        off = np.mean([r.metrics[key] for r in runs if not r.on])
        on = np.mean([r.metrics[key] for r in runs if r.on])
        diffs = paired(runs, key, on_first=False)
        low, high = bootstrap_ci(diffs, rng)
        better = (diffs > 0) if key in ("fps", "low1") else (diffs < 0)
        p = wilcoxon(diffs, method="exact").pvalue
        lines.append(f"| {name} ({unit}) | {off:.2f} | {on:.2f} | {(on - off) / off:+.1%} | "
                     f"{diffs.mean():+.3f} | {low:+.3f} ~ {high:+.3f} | "
                     f"{better.sum()}/{diffs.size} | {p:.4f}{' ✔' if p < alpha else ''} |")

    # 순서 효과 점검: 같은 데이터를 on→off 쌍으로 다시 묶어도 같은 방향이면
    # 측정 순서(시간 흐름)보다 적용 여부가 차이를 만든 것으로 볼 수 있다.
    forward = paired(runs, "fps", on_first=False)
    backward = paired(runs, "fps", on_first=True)
    lines += ["", "## 순서 효과 점검 (평균 FPS, on − off)", "",
              "| 쌍 순서 | 쌍 수 | 평균 차이 | on 우세 쌍 |", "|---|---|---|---|",
              f"| off→on | {forward.size} | {forward.mean():+.2f} | {(forward > 0).sum()}/{forward.size} |",
              f"| on→off | {backward.size} | {backward.mean():+.2f} | {(backward > 0).sum()}/{backward.size} |"]
    return "\n".join(lines) + "\n"


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("usage: python analyze.py <measurement-folder>")
    folder = Path(sys.argv[1])
    text = report(folder)
    (folder / "RESULTS.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
