#!/usr/bin/env bash
# =============================================================================
# run: start LPQ Atlas (FastAPI on :8000, Next.js on :3000) and wait until both
# answer. Runs setup first when this clone has not been set up yet.
#
#   scripts/setup/run.sh [--test] [--dev]
#
#   --test   run scripts/setup/test.sh before launching (stops if it fails)
#   --dev    serve the UI with `next dev` instead of a production build
#
# Ports: ATLAS_API_PORT (default 8000), ATLAS_WEB_PORT (default 3000).
# Press Ctrl+C to stop both. Logs: results/atlas/launcher/{api,web}.log
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

run_tests=0 dev=0
for arg in "$@"; do
    case "$arg" in
        --test) run_tests=1 ;;
        --dev) dev=1 ;;
        -h | --help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $arg" ;;
    esac
done
cd "$ROOT"

# --- set up on first use -------------------------------------------------------
if [ -z "$(venv_python)" ] || [ ! -d "$WEB_DIR/node_modules" ] || ! engine_built ||
    [ ! -f "$ROOT/datasets/processed/demo_road.lpqgraph" ]; then
    say "this clone is not set up yet; running setup first"
    bash "$SETUP_DIR/setup.sh" $([ "$dev" = 1 ] && echo --skip-web)
fi
VPY="$(venv_python)"
export LPQ_PYTHON="$VPY"

if [ "$run_tests" = 1 ]; then bash "$SETUP_DIR/test.sh"; fi

# --- ports ---------------------------------------------------------------------
for p in "$API_PORT" "$WEB_PORT"; do
    if port_in_use "$p"; then
        die "port $p is already in use. Stop whatever is using it, or choose other ports:
       ATLAS_API_PORT=8001 ATLAS_WEB_PORT=3001 scripts/setup/run.sh"
    fi
done

mkdir -p "$RUN_DIR"
[ "$dev" = 0 ] && ensure_web_built

pids=()
cleanup() {
    trap - EXIT
    echo
    say "stopping"
    local p
    for p in "${pids[@]:-}"; do
        [ -n "$p" ] || continue
        pkill -P "$p" 2> /dev/null || true # next spawns child processes
        kill "$p" 2> /dev/null || true
    done
}
trap cleanup EXIT
trap 'exit 0' INT TERM # Ctrl+C is a normal way to stop; EXIT then runs cleanup

# --- API -------------------------------------------------------------------------
say "starting the API on http://$HOST:$API_PORT"
PYTHONPATH="$ROOT/python${PYTHONPATH:+:$PYTHONPATH}" \
    "$VPY" -m uvicorn atlas_api.app:app --app-dir atlas/api --host "$HOST" --port "$API_PORT" \
    > "$RUN_DIR/api.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 60); do
    http_ok "http://$HOST:$API_PORT/api/health" && break
    kill -0 "${pids[0]}" 2> /dev/null || { tail -n 20 "$RUN_DIR/api.log" >&2; die "the API exited during start-up (log: $RUN_DIR/api.log)"; }
    sleep 1
done
http_ok "http://$HOST:$API_PORT/api/health" || die "the API did not answer within 60 s (log: $RUN_DIR/api.log)"

# --- Web -------------------------------------------------------------------------
next_cli="$WEB_DIR/node_modules/next/dist/bin/next"
if [ "$dev" = 1 ]; then web_cmd=(dev); else web_cmd=(start); fi
say "starting the web UI ($([ "$dev" = 1 ] && echo 'next dev' || echo 'next start')) on http://$HOST:$WEB_PORT"
(cd "$WEB_DIR" && ATLAS_API_URL="http://$HOST:$API_PORT" exec node "$next_cli" "${web_cmd[@]}" -p "$WEB_PORT" -H "$HOST") \
    > "$RUN_DIR/web.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 120); do
    http_ok "http://$HOST:$WEB_PORT/" && break
    kill -0 "${pids[1]}" 2> /dev/null || { tail -n 20 "$RUN_DIR/web.log" >&2; die "the web UI exited during start-up (log: $RUN_DIR/web.log)"; }
    sleep 1
done
http_ok "http://$HOST:$WEB_PORT/" || die "the web UI did not answer within 120 s (log: $RUN_DIR/web.log)"

cat << EOF

  LPQ Atlas is running.

    Open        http://$HOST:$WEB_PORT
    API         http://$HOST:$API_PORT/api/health   (docs: http://$HOST:$API_PORT/docs)
    Dataset     demo_road (synthetic, 19,600 nodes) is ready; pick it on the Explore page
    Logs        $RUN_DIR/api.log, web.log

  Press Ctrl+C to stop.
EOF

# Exit as soon as either server dies, so a crash is not mistaken for "running".
while kill -0 "${pids[0]}" 2> /dev/null && kill -0 "${pids[1]}" 2> /dev/null; do sleep 2; done
die "a server process exited unexpectedly; see $RUN_DIR/*.log"
