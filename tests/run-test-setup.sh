#!/bin/bash
# tests/run-test-setup.sh DOM.img [OUT.img]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOM=$(realpath "$1")
OUTARG=()
if [ -n "${2:-}" ]; then touch "$2"; OUTARG=(-e OUT=1 -v "$(realpath "$2")":/out.img); fi
mkdir -p "$ROOT/build/captures"
exec docker run --rm --privileged ${MAC:+--mac-address $MAC} -e NET -e REPO -e SYNODEB_TEST_MODEL -e CAPTURE -e DOWNLOAD -v "$ROOT/build/captures":/captures "${OUTARG[@]}" -v "$ROOT":/work -v "$DOM":/dom.img:ro debian:trixie bash /work/tests/test-setup.sh
