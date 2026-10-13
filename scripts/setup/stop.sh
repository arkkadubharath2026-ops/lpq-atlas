#!/usr/bin/env bash
# =============================================================================
# stop: stop a LPQ Atlas started by scripts/setup/run.sh, from any terminal.
#
#   scripts/setup/stop.sh
#
# Ctrl+C in the run.sh terminal is the usual way to stop. This is for when that terminal is not at hand
# (run.sh was started in the background, from an IDE, or over ssh). It reads the process id run.sh wrote to
# results/atlas/launcher/run.pid, checks that it really is run.sh, and sends it SIGTERM, which stops the
# API and the web server it started. It never kills a process it cannot identify as run.sh.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

pidfile="$RUN_DIR/run.pid"
if [ ! -f "$pidfile" ]; then
    echo "no launcher is recorded as running (no $pidfile)."
    for p in "$API_PORT" "$WEB_PORT"; do
        if [ -n "$(venv_python)" ] && port_in_use "$p"; then
            warn "something is listening on port $p, but not a launcher started by run.sh; it was left alone."
        fi
    done
    exit 0
fi
pid="$(tr -dc '0-9' < "$pidfile")"
if [ -z "$pid" ] || ! kill -0 "$pid" 2> /dev/null; then
    rm -f "$pidfile"
    echo "the recorded launcher (pid ${pid:-?}) is not running; removed the stale pid file."
    exit 0
fi
# Is that pid really run.sh? Match an argv ELEMENT that ends in setup/run.sh, not a substring of some longer command line.
is_launcher() {
    if [ -r "/proc/$1/cmdline" ]; then
        tr '\0' '\n' < "/proc/$1/cmdline" | grep -qE '(^|/)setup/run\.sh$'
    else
        ps -o command= -p "$1" 2> /dev/null | grep -qE '(^|[[:space:]])[^[:space:]]*setup/run\.sh([[:space:]]|$)'
    fi
}
is_launcher "$pid" || die "pid $pid is not scripts/setup/run.sh, so it was NOT stopped. Remove $pidfile if it is stale."
say "stopping LPQ Atlas (launcher pid $pid)"
kill -TERM "$pid"
for _ in $(seq 1 30); do
    kill -0 "$pid" 2> /dev/null || { echo "stopped."; exit 0; }
    sleep 1
done
die "the launcher (pid $pid) did not exit within 30 s. Stop it by hand: kill $pid"
