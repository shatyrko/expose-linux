#!/usr/bin/env bash
# Install Expose Linux for the current user: builds the screenshot helper, then the command, the icon, the launcher
# entry and the background service (starts at login, no tray icon). Safe to run again.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="$HOME/.local/bin"
"$here/build-tools.sh"
mkdir -p "$bin" "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/scalable/apps" "$HOME/.config/systemd/user"
ln -sf "$here/expose-linux" "$bin/expose-linux"
ln -sf "$here/expose-toggle" "$bin/expose-toggle"
ln -sf "$here/tools/expose-shots" "$bin/expose-shots"
install -m644 "$here/data/dev.alecsh.ExposeLinux.svg" "$HOME/.local/share/icons/hicolor/scalable/apps/"
install -m644 "$here/data/dev.alecsh.ExposeLinux.desktop" "$HOME/.local/share/applications/"
install -m644 "$here/data/expose-linux.service" "$HOME/.config/systemd/user/"
gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
systemctl --user daemon-reload
systemctl --user enable --now expose-linux.service
systemctl --user restart expose-linux.service
echo "Expose Linux installed: run 'expose-toggle' (instant) or 'expose-linux' (it toggles)."
