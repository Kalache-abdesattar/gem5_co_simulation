#!/bin/bash
# Fetch the workload sources at pinned upstream versions into src/.
#   ./fetch.sh            all workloads
#   ./fetch.sh xsbench    one or more by name
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
mkdir -p "$SRC"

# name  url  ref(tag or commit)
REPOS="
hpcg        https://github.com/hpcg-benchmark/hpcg.git            HPCG-release-3-1-0
npb         https://github.com/GMAP/NPB-CPP.git                   v1.0
lulesh      https://github.com/LLNL/LULESH.git                    2.0.3
xsbench     https://github.com/ANL-CESAR/XSBench.git              v20
rsbench     https://github.com/ANL-CESAR/RSBench.git              v13
gapbs       https://github.com/sbeamer/gapbs.git                  v1.5
polybench   https://github.com/MatthiasJReisinger/PolyBenchC-4.2.1.git 3e872547cef7
comd        https://github.com/ECP-copa/CoMD.git                  v1.1
minife      https://github.com/Mantevo/miniFE.git                 2.2.0
"
# STREAM is a single file with no versioned URL: pin its checksum (v5.10).
STREAM_URL=https://www.cs.virginia.edu/stream/FTP/Code/stream.c
STREAM_SHA256=a52bae5e175bea3f7832112af9c085adab47117f7d2ce219165379849231692b

want() { [ $# -eq 0 ] && return 0; for w in "$@"; do [ "$w" = "$NAME" ] && return 0; done; return 1; }

echo "$REPOS" | while read -r NAME URL REF; do
    [ -z "$NAME" ] && continue
    want "$@" || continue
    if [ -d "$SRC/$NAME/.git" ]; then
        echo "== $NAME: present ($(git -C "$SRC/$NAME" rev-parse --short HEAD))"
        continue
    fi
    echo "== $NAME: $URL @ $REF"
    git clone -q "$URL" "$SRC/$NAME"
    git -C "$SRC/$NAME" -c advice.detachedHead=false checkout -q "$REF"
done

NAME=stream
if want "$@"; then
    mkdir -p "$SRC/stream"
    if [ ! -f "$SRC/stream/stream.c" ]; then
        echo "== stream: $STREAM_URL"
        curl -fsSL -o "$SRC/stream/stream.c" "$STREAM_URL"
    fi
    echo "$STREAM_SHA256  $SRC/stream/stream.c" | sha256sum -c --quiet - \
        || { echo "stream.c checksum mismatch (upstream changed?)"; exit 1; }
fi
