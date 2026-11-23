#!/bin/sh
# Run on the Pi Zero 2 W (Raspberry Pi OS Lite, Bookworm or newer) from the pi/ folder.
set -e
sudo apt-get install -y python3-serial
sudo raspi-config nonint do_serial_hw 0     # enable UART hardware
sudo raspi-config nonint do_serial_cons 1   # disable the login console on serial0
id meshaddon >/dev/null 2>&1 || sudo useradd -r -s /usr/sbin/nologin meshaddon
sudo install -D -m 755 meshaddond.py /opt/meshaddon/meshaddond.py
# Script folder: owned by group meshaddon, writable by you (setgid keeps the group on new files)
sudo install -d -m 2775 -o meshaddon -g meshaddon /opt/meshaddon/scripts
sudo usermod -aG meshaddon "$USER"
for f in scripts/*; do [ -e "/opt/meshaddon/$f" ] || sudo install -m "$( [ -x "$f" ] && echo 775 || echo 664 )" -g meshaddon "$f" /opt/meshaddon/scripts/; done
sudo install -m 644 meshaddond.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now meshaddond
echo "Done. Put executable scripts in /opt/meshaddon/scripts (log out/in once for group access)."
echo "Reboot once if the UART was just enabled."
