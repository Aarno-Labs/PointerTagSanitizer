#!/bin/sh
# Preserve diagnostics without changing upstream's output verification.
set -eu
test "$#" -eq 1
cp -- "$1" "$1.ptsan-raw"
exec /bin/sh "$(dirname "$0")/HashProgramOutput.original.sh" "$1"
