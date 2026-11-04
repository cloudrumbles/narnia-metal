#!/bin/bash
# Start everything the box needs for the iPad, idempotently (after a reboot,
# or to restart one piece): Xvfb, nm-relay, nm-input, pulse + nm-audio.
#
#   box/start-services.sh            # start what isn't running
#   box/start-services.sh restart    # stop ours first
#
# The relay token lives in ~/.config/narnia-metal/token (created once).
# Logs: /tmp/relay-live.log, /tmp/nm-input.log, /tmp/nm-audio.log;
# iPad telemetry: /tmp/nm-ipad.log.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=${PORT:-7398}
SOCK=/tmp/narnia-metal.sock
DISPLAY_NUM=${DISPLAY_NUM:-:9}
SCREEN=${SCREEN:-1280x720x24}
CONF=$HOME/.config/narnia-metal
mkdir -p "$CONF"
[ -s "$CONF/token" ] || head -c 32 /dev/urandom | base64 | tr -dc a-z0-9 | head -c 20 > "$CONF/token"
TOKEN=$(cat "$CONF/token")

running() { pgrep -f "^$1" >/dev/null; }
stop() { for p in $(pgrep -f "^$1"); do kill "$p"; done; }

if [ "${1:-}" = restart ]; then
  stop "$HERE/nm-relay -p $PORT"
  stop "$HERE/nm-input"
  stop "$HERE/nm-audio"
  sleep 0.5
fi

if ! pgrep -x Xvfb >/dev/null; then
  setsid Xvfb "$DISPLAY_NUM" -screen 0 "$SCREEN" >/dev/null 2>&1 </dev/null &
  sleep 1
fi

if ! running "$HERE/nm-relay -p $PORT"; then
  NM_TOKEN=$TOKEN setsid "$HERE/nm-relay" -p "$PORT" -s "$SOCK" -w 600 \
    >/tmp/relay-live.log 2>&1 </dev/null &
fi

# controllers need /dev/uinput (the standard Debian kernel, not -cloud)
[ -e /dev/uinput ] || sudo modprobe uinput 2>/dev/null || true
[ -e /dev/uinput ] && sudo chmod 666 /dev/uinput
if ! running "$HERE/nm-input"; then
  DISPLAY=$DISPLAY_NUM setsid "$HERE/nm-input" -s /tmp/narnia-input.sock \
    >/tmp/nm-input.log 2>&1 </dev/null &
fi

if ! running "$HERE/nm-audio"; then
  setsid "$HERE/nm-audio" -s "$SOCK" >/tmp/nm-audio.log 2>&1 </dev/null &
fi

sleep 1
echo "relay: tcp $PORT, token $TOKEN, apps VN_NET=unix:$SOCK, DISPLAY=$DISPLAY_NUM"
if [ -e /dev/uinput ]; then echo "uinput: yes"; else echo "uinput: no, controllers off"; fi
tail -n 2 /tmp/relay-live.log /tmp/nm-input.log /tmp/nm-audio.log 2>/dev/null
