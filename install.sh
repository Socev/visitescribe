#!/bin/bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo: sudo ./install.sh" >&2
  exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="visitescribe"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y alsa-utils flac python3-cryptography python3-requests python3-luma.oled python3-rpi.gpio >/dev/null

if ! getent group "$USER_NAME" >/dev/null 2>&1; then
  groupadd --system "$USER_NAME"
fi
if ! id "$USER_NAME" >/dev/null 2>&1; then
  useradd --system --gid "$USER_NAME" --home /nonexistent --shell /usr/sbin/nologin "$USER_NAME"
fi
for g in audio gpio spi i2c; do
  getent group "$g" >/dev/null && usermod -aG "$g" "$USER_NAME" || true
done

systemctl disable --now visitescribe.service >/dev/null 2>&1 || true

mkdir -p /opt/visitescribe /etc/visitescribe /var/lib/visitescribe/sessions
chmod 700 /etc/visitescribe /var/lib/visitescribe
chown "$USER_NAME:$USER_NAME" /var/lib/visitescribe

if [ -d /opt/visitescribe ] && [ -n "$(ls -A /opt/visitescribe 2>/dev/null || true)" ]; then
  mkdir -p /var/backups/visitescribe
  tar -czf "/var/backups/visitescribe/opt-before-v02-$(date +%Y%m%dT%H%M%S).tgz" -C /opt visitescribe || true
fi

install -o root -g root -m 0755 "$SRC_DIR/recorder.py" /opt/visitescribe/recorder.py
install -o root -g root -m 0755 "$SRC_DIR/uploader.py" /opt/visitescribe/uploader.py
install -o root -g root -m 0755 "$SRC_DIR/visitescribe-admin" /usr/local/sbin/visitescribe-admin

if [ ! -f /etc/visitescribe/config.json ]; then
  install -o root -g "$USER_NAME" -m 0640 "$SRC_DIR/config.json" /etc/visitescribe/config.json
else
  echo "Bestaande /etc/visitescribe/config.json behouden"
fi

if [ ! -f /etc/visitescribe/device.key ]; then
  head -c 32 /dev/urandom > /etc/visitescribe/device.key
fi
chown root:"$USER_NAME" /etc/visitescribe/device.key
chmod 0640 /etc/visitescribe/device.key

install -o root -g root -m 0644 "$SRC_DIR/visitescribe-recorder.service" /etc/systemd/system/visitescribe-recorder.service
install -o root -g root -m 0644 "$SRC_DIR/visitescribe-uploader.service" /etc/systemd/system/visitescribe-uploader.service

systemctl daemon-reload
systemctl enable visitescribe-recorder.service visitescribe-uploader.service >/dev/null
systemctl restart visitescribe-uploader.service
systemctl restart visitescribe-recorder.service
sleep 2

echo "recorder: $(systemctl is-active visitescribe-recorder.service || true)"
echo "uploader: $(systemctl is-active visitescribe-uploader.service || true)"
echo "VISITESCRIBE_V02_INSTALL_OK"
