#!/bin/bash
# Runner script for Dreamcart 64
set -eu
: "${KOS_BASE:?KOS_BASE is not set — source the KOS environ.sh}"
recomp_dir="$(cd "$(dirname "$0")" && pwd)"
dc_ip="${DC_IP:?Set DC_IP to the address dcload-ip shows}"
dc_tool="${DC_TOOL:-$(dirname "$KOS_BASE")/bin/dc-tool-ip}"
want_verbose=0
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) want_verbose=1 ;;
        -q|--quiet) ;;  # the default now
        *) echo "usage: run.sh [-v]  (-v: the console's logging, build and dcload output)" >&2; exit 2 ;;
    esac
done
target=n64-live-quiet
(( want_verbose )) && target=n64-live

if (( want_verbose )); then
    make -C "$recomp_dir/dc" "$target" >&2
else
    make -s --no-print-directory -C "$recomp_dir/dc" "$target" >&2
fi
elf_file="$recomp_dir/build/dc-any/$target.elf"

# The terminal drops the per-frame "fr" lines and starts at the KOS banner
show() {
    awk -v shown="$want_verbose" '{ sub(/^\[ *[0-9.]+\] /, "") }
                                  /^fr / { next }
                                  shown || /^KallistiOS v/ { shown = 1; print; fflush(); next }
                                  { held = held $0 "\n" }
                                  END { if (!shown) printf "%s", held > "/dev/stderr" }'
}

if [ "$target" = n64-live-quiet ]; then
    echo "→ $elf_file to $dc_ip" >&2
    stdbuf -oL -eL "$dc_tool" -t "$dc_ip" -x "$elf_file" 2>&1 | show || true
    exit 0
fi

mkdir -p "$recomp_dir/log"
log_file="$recomp_dir/log/live-$(date +%Y%m%d-%H%M%S).log"
echo "→ $elf_file to $dc_ip, log $log_file" >&2
stdbuf -oL -eL "$dc_tool" -t "$dc_ip" -x "$elf_file" 2>&1 \
    | python3 -I -u "$recomp_dir/tools/stamp.py" | tee "$log_file" | show || true

echo "→ summary of $log_file" >&2
python3 -I "$recomp_dir/tools/playlog.py" "$log_file" --brief
