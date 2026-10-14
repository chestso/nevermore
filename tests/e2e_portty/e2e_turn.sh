#!/bin/sh
# e2e_turn.sh — scenario 1: boot, one turn, quit. See ../e2e-driver.sh.
set -u
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$dir/e2e-driver.sh" turn table-fence
