#!/usr/bin/env bash
# Standard benchmark suite (macOS / Linux). Same cases and output as tools/benchmark.ps1 (Windows),
# so the results of both machines can be put side by side.
#
#   tools/benchmark.sh [path/to/FloatingIsland] [rounds]
#
# Runs every case `rounds` times (default 3), interleaved (case 1, 2, ..., then again), so slow drifts
# such as the GPU heating up hit all cases alike, and reports the median of the per-run means.
# Writes a Markdown table to stdout and to benchmark-<host>.md next to the executable.
# Close any other FloatingIsland window first: it shares the GPU and skews the numbers.
set -euo pipefail

exe="${1:-build-release/FloatingIsland}"
rounds="${2:-3}"
frames=1500

names=("默认（草密度 1）" "草密度 4" "草密度 4，关闭剔除 / LOD、草投射阴影" "不画云海" "IBL 每帧重做")
flags=("" "--grass-density 4" "--grass-density 4 --no-culling --no-lod --grass-shadows" "--no-clouds" "--ibl-every-frame")

if pgrep -x FloatingIsland >/dev/null; then
  echo "Another FloatingIsland is running; close it first." >&2
  exit 1
fi

tmp="$(mktemp -d)"
gpu="$("$exe" --frames 1 2>/dev/null | sed -n 's/^GPU: //p' | head -1)"
for ((r = 1; r <= rounds; ++r)); do
  for i in "${!names[@]}"; do
    echo "round $r/$rounds: ${names[$i]}" >&2
    # shellcheck disable=SC2086
    "$exe" --benchmark "$frames" ${flags[$i]} > "$tmp/$i.$r.txt" 2>&1
  done
done

# The mean column of a row of the benchmark report, e.g. "GPU scene (sh+main)".
mean() { awk -v row="$1" 'index($0, row) == 1 { print $(NF - 3) }' "$2"; }
median() { sort -g | awk '{ v[NR] = $1 } END { print (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'; }
stat() {  # stat <case> <row>
  for ((r = 1; r <= rounds; ++r)); do mean "$2" "$tmp/$1.$r.txt"; done | median
}

out="$(dirname "$exe")/benchmark-$(hostname -s).md"
{
  echo "GPU: $gpu  "
  echo "$(date '+%Y-%m-%d %H:%M')，每项 $rounds 次取中位数，--benchmark $frames（场景每帧画 16 遍，报告单遍）"
  echo
  echo "| 测试 | GPU 场景（阴影 + 主） | 阴影 Pass | 主 Pass | IBL 更新 | CPU 帧 |"
  echo "| --- | --- | --- | --- | --- | --- |"
  for i in "${!names[@]}"; do
    printf '| %s | %s | %s | %s | %s | %s |\n' "${names[$i]}" "$(stat "$i" "GPU scene (sh+main)")" \
      "$(stat "$i" "GPU shadow pass")" "$(stat "$i" "GPU main pass")" "$(stat "$i" "GPU IBL update")" \
      "$(stat "$i" "CPU frame")"
  done
  echo
  echo "单位 ms。CPU 帧包含 16 遍场景的提交，并且不受垂直同步限制，只用来看趋势。"
} | tee "$out"
echo "Saved to $out" >&2
rm -rf "$tmp"
