#!/usr/bin/env bash
# Prepare board-level CAN/RS485 prerequisites before launching linux_app.
set -euo pipefail

CAN_IF="${1:-can0}"
CAN_BITRATE="${2:-500000}"

if [[ ! -d "/sys/class/net/${CAN_IF}" ]]; then
    echo "missing ${CAN_IF}: enable SPI2 MCP2515 SocketCAN support in kernel/DTS first" >&2
    exit 1
fi

if ! command -v ip >/dev/null 2>&1; then
    echo "missing ip command; install iproute2 on the target rootfs" >&2
    exit 1
fi

ip link set "${CAN_IF}" down
ip link set "${CAN_IF}" type can bitrate "${CAN_BITRATE}" restart-ms 100
ip link set "${CAN_IF}" up
ip -details link show "${CAN_IF}"

echo "CAN is ready on ${CAN_IF} at ${CAN_BITRATE} bit/s"
echo "RS485 is expected on UART4 (/dev/ttyS4) with GPIO15 managed by the UART driver via TIOCSRS485."
echo "Start linux_app with: ./linux_app --config linux_app/config/device_config.integration.ini"
