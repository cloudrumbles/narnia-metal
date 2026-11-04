#!/bin/bash
# Latency injection without touching lo: a network namespace "nmlat" joined to
# the root namespace by a veth pair (10.77.0.1 root side, 10.77.0.2 namespace
# side), with netem delay on both ends.
#
#   scripts/latency/netns.sh <one-way ms>   create if needed, set the delay
#                                           (RTT = 2x; 0 removes the delay)
#   scripts/latency/netns.sh down           remove the namespace again
#
# Run nm-host inside it (sudo ip netns exec nmlat ... nm-host tcp:7510) and
# point guests at VN_NET=10.77.0.2:7510. Needs sudo.
set -e
NS=${NS:-nmlat}
if [ "$1" = down ]; then
  sudo ip netns del "$NS" 2>/dev/null || true
  sudo ip link del "${NS}0" 2>/dev/null || true
  exit 0
fi
if ! ip netns list | grep -q "^$NS"; then
  sudo ip netns add "$NS"
  sudo ip link add "${NS}0" type veth peer name "${NS}1"
  sudo ip link set "${NS}1" netns "$NS"
  sudo ip addr add 10.77.0.1/24 dev "${NS}0"
  sudo ip link set "${NS}0" up
  sudo ip netns exec "$NS" ip addr add 10.77.0.2/24 dev "${NS}1"
  sudo ip netns exec "$NS" ip link set "${NS}1" up
  sudo ip netns exec "$NS" ip link set lo up
fi
ms=${1:-0}
if [ "$ms" = 0 ]; then
  sudo tc qdisc del dev "${NS}0" root 2>/dev/null || true
  sudo ip netns exec "$NS" tc qdisc del dev "${NS}1" root 2>/dev/null || true
else
  sudo tc qdisc replace dev "${NS}0" root netem delay "${ms}ms"
  sudo ip netns exec "$NS" tc qdisc replace dev "${NS}1" root netem delay "${ms}ms"
fi
