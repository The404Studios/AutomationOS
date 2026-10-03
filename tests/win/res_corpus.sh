#!/usr/bin/env bash
# res_corpus.sh -- READ-ONLY scan of a sample of real PE files with pe_resources / pe_manifest (not part of the
# default test run; needs a Windows install visible from WSL).
#
#   wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/res_corpus.sh [dir] [sample_count]
#
#   dir           default /mnt/c/Windows/System32
#   sample_count  default 300; ALL = every *.exe/*.dll/*.sys/*.ocx/*.cpl in the directory
#   env: RES_MAXDEPTH (default 1) find depth, RES_WORKDIR, RES_REJECTS=1 (list every rejected file)
#
# The sample is deterministic (sorted list, every k-th file).  Files are only opened for reading.  res_scan is built
# with ASan+UBSan: a crash or an out-of-bounds read on a real file aborts the run and the culprit is re-found.
# Output: $WORK/corpus.tsv (one line per file) and the summary from res_corpus_report.py.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$ROOT/userspace/lib/pe
DIR=${1:-/mnt/c/Windows/System32}
N=${2:-300}
WORK=${RES_WORKDIR:-/tmp/res_tests_work}
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
export ASAN_OPTIONS=${ASAN_OPTIONS:-strict_string_checks=1:detect_stack_use_after_return=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:abort_on_error=1}
mkdir -p "$WORK" || exit 1

"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/res_scan" "$HERE/res_scan.c" \
    "$SRC/pe.c" "$SRC/pe_resources.c" "$SRC/pe_manifest.c" || { echo "cannot build res_scan"; exit 1; }

LIST=$WORK/corpus_list.txt
find "$DIR" -maxdepth "${RES_MAXDEPTH:-1}" -type f \( -iname '*.exe' -o -iname '*.dll' -o -iname '*.sys' -o -iname '*.ocx' -o -iname '*.cpl' \) | LC_ALL=C sort > "$LIST.all"
TOTAL=$(wc -l < "$LIST.all")
if [ "$N" = "ALL" ] || [ "$N" -ge "$TOTAL" ]; then
    cp "$LIST.all" "$LIST"
else
    K=$(( TOTAL / N ))
    [ "$K" -ge 1 ] || K=1
    awk -v k="$K" 'NR % k == 1 || k == 1' "$LIST.all" | head -n "$N" > "$LIST"
fi
echo "[corpus] $DIR: $TOTAL candidate files, scanning $(wc -l < "$LIST")"

: > "$WORK/corpus.tsv"
CRASHES=0
split -l 25 "$LIST" "$WORK/corpus_chunk_"
for c in "$WORK"/corpus_chunk_*; do
    if xargs -d '\n' -a "$c" "$WORK/res_scan" >> "$WORK/corpus.tsv" 2> "$WORK/corpus.err"; then
        :
    else
        # a sanitizer abort: find the culprit file by file
        while IFS= read -r f; do
            if ! "$WORK/res_scan" "$f" >> "$WORK/corpus.tsv" 2> "$WORK/corpus.err1"; then
                CRASHES=$((CRASHES + 1))
                echo "[corpus] CRASH on $f"
                head -20 "$WORK/corpus.err1"
            fi
        done < "$c"
    fi
done
rm -f "$WORK"/corpus_chunk_*
echo "[corpus] crashes/sanitizer aborts: $CRASHES"
python3 "$HERE/res_corpus_report.py" "$WORK/corpus.tsv" ${RES_REJECTS:+--rejects}
[ "$CRASHES" -eq 0 ]
