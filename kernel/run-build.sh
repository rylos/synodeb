#!/bin/bash
# Runs build-kernel.sh in a Debian trixie container with the project mounted on /work.
# SERIES=longterm (default) or stable: kernel/<series>/VERSION
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec docker run --rm -v "$ROOT":/work -w /work -e HOME=/tmp -e FULL -e SERIES \
    -e KBUILD_BUILD_USER=synodeb -e KBUILD_BUILD_HOST=synodeb \
    debian:trixie bash /work/kernel/build-kernel.sh
