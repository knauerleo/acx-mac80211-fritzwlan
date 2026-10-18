#!/bin/bash
# Install acx-mac80211 for the AVM FRITZ!WLAN USB Stick v1.1 (TNETW1450):
#   1. firmware   /lib/firmware/tiacx111usbc1B, extracted from the stick's own
#                 virtual CD (scripts/fritzwlan-firmware.sh), unless present
#   2. udev rule  /etc/udev/rules.d/70-fritzwlan-usb.rules (CD mode -> eject)
#   3. module     via DKMS (rebuilt on kernel updates), autoloads on 057c:6201
#
# Usage (as root, from a clone of this repository):
#   scripts/fritzwlan-install.sh [FIRMWARE-SOURCE]   install / update
#   scripts/fritzwlan-install.sh uninstall           remove all three
# Needs: dkms, kernel headers, bsdtar (libarchive), eject (util-linux), systemd.
set -eu
SRC=$(cd "$(dirname "$0")/.." && pwd)
NAME=acx-mac80211
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$SRC/dkms.conf")
KVER=$(uname -r)
RULE=/etc/udev/rules.d/70-fritzwlan-usb.rules
FW=/lib/firmware/tiacx111usbc1B

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

remove_dkms() {
	local v
	for v in $(dkms status -m $NAME 2>/dev/null | sed -n "s|^$NAME/\([^,:]*\).*|\1|p" | sort -u); do
		dkms remove -m $NAME -v "$v" --all || true
		rm -rf "/usr/src/$NAME-$v"
	done
}

if [ "${1:-}" = uninstall ]; then
	rmmod $NAME 2>/dev/null || true
	remove_dkms
	rm -f "$RULE" "$FW"
	udevadm control --reload
	depmod -a "$KVER"
	echo "uninstalled"
	exit 0
fi

# 1. firmware first: the udev rule ejects the CD it comes from
if [ ! -f "$FW" ] || [ -n "${1:-}" ]; then
	rm -f "$RULE"; udevadm control --reload
	"$SRC/scripts/fritzwlan-firmware.sh" "${1:-}"
fi

# 2. udev rule
install -Dm644 "$SRC/udev/70-fritzwlan-usb.rules" "$RULE"
udevadm control --reload

# 3. DKMS, from the tracked source files only (no build products)
rmmod $NAME 2>/dev/null || true
remove_dkms
mkdir -p "/usr/src/$NAME-$VER"
if git -C "$SRC" rev-parse 2>/dev/null; then
	git -C "$SRC" ls-files -z | (cd "$SRC" && xargs -0 cp --parents -t "/usr/src/$NAME-$VER")
else
	cp -r "$SRC"/. "/usr/src/$NAME-$VER"
fi
dkms add -m $NAME -v "$VER"
dkms build -m $NAME -v "$VER" -k "$KVER"
dkms install -m $NAME -v "$VER" -k "$KVER"

modinfo -F filename $NAME
echo "Installed. Replug the stick: it ejects its CD and comes up as a wlan interface."
