#!/usr/bin/env bash
# Prepares the ftrace instance that RADV needs to record RMV memory traces
# (MESA_VK_TRACE=rmv). Must be run again after every reboot.
#
# RADV reads the kernel's memory events from its own ftrace instance
# "amd_rmv" but does NOT create it. Without it, the driver only reports
# "Can't access the tracing instance directory" and writes no trace.
#
# This loosens permissions on /sys/kernel/tracing so that an unprivileged
# game can use the instance. Reboot afterwards to return to the defaults.
#
#   sudo scripts/setup-tracing.sh           set up
#   scripts/setup-tracing.sh --check        only check, as the current user
set -euo pipefail

T=/sys/kernel/tracing
I=$T/instances/amd_rmv

check() {
    # Check as the user who will run the game, not as root: root can read
    # and write everything regardless of the permission bits.
    local ok=1
    [ -d "$I" ] || { echo "missing: $I"; ok=0; }
    [ -w "$I/trace_clock" ] 2>/dev/null || { echo "not writable for $(id -un): $I/trace_clock"; ok=0; }
    if [ "$ok" = 1 ]; then echo "ok: $I is ready for $(id -un)"; return 0; fi
    return 1
}

if [ "${1:-}" = "--check" ]; then
    check
    exit $?
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "error: run as root (sudo $0), or use --check" >&2
    exit 1
fi
if [ ! -d "$T" ]; then
    echo "error: $T not found. Is tracefs mounted? (mount -t tracefs nodev $T)" >&2
    exit 1
fi

mkdir -p "$I"
# The instance is only reachable if every directory on the way can be
# traversed. On some kernels (observed on a 7.3-rc) "instances" itself is 0750.
chmod 755 "$T" "$T/instances"
chmod -R a+rwX "$I"
echo "set up: $I"

# Report the result from the perspective of the user who called sudo.
if [ -n "${SUDO_USER:-}" ]; then
    sudo -u "$SUDO_USER" "$0" --check
fi
