#!/bin/bash
# cc_recover.sh — catch the ClearCore's post-reset USB window and flash a
# known-safe sketch, recovering from firmware that starved USB in loop().
# The bad sketch enumerates USB during setup() (~14s) before loop() kills it;
# we poll for /dev/ttyACM0 and run flash.sh the instant it appears. The
# bootloader has no timeout, so once flash.sh touches it we can't miss.
SKETCH="${1:-$HOME/dev/KegWasher/tools/cc_serial_heartbeat}"
echo "RECOVERY: power-cycle the ClearCore NOW (cut its power, restore)."
echo "Waiting up to 90s for /dev/ttyACM0 to appear..."
for i in $(seq 1 900); do
  if [ -e /dev/ttyACM0 ]; then
    echo ">>> device appeared (t=$((i/10))s) — flashing $(basename "$SKETCH")"
    "$HOME/dev/teknic-clearcore-cli/scripts/flash.sh" "$SKETCH" /dev/ttyACM0
    rc=$?
    echo ">>> flash.sh exit=$rc"
    exit $rc
  fi
  sleep 0.1
done
echo "TIMED OUT — device never appeared. Board may not be resetting; check its power."
exit 1
