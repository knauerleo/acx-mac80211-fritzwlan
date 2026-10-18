#!/bin/bash
# Extract the TNETW1450 firmware from the AVM FRITZ!WLAN USB Stick v1.1's own
# virtual CD and install it as /lib/firmware/tiacx111usbc1B. The firmware is
# AVM/TI property and is not part of this repository.
#
# Usage (as root):  scripts/fritzwlan-firmware.sh [SOURCE]
#   SOURCE: the stick's CD device (/dev/srN), a mounted copy of the CD, a CD
#           image (.iso) or the file PUSHINST.CAB. Default: find the stick in
#           CD mode (USB 057c:62ff). If the stick already switched to WLAN mode
#           (057c:6201), unplug it, stop the udev eject rule if installed
#           (scripts/fritzwlan-install.sh does this step before installing the
#           rule) and plug it in again; the CD stays for ~25 s.
# Needs: bsdtar (libarchive), mount.
set -eu

SHA256=8ae4243abeb8305ec9fbe454c0d4c10b3e0151fc274cff5368cdb5ffb672e271
DEST=${FIRMWARE_DIR:-/lib/firmware}/tiacx111usbc1B
SRC=${1:-}
TMP=$(mktemp -d)
MNT=
cleanup() { [ -n "$MNT" ] && umount "$MNT" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

find_cd() {
	local d
	for d in /sys/block/sr*; do
		[ -e "$d" ] || continue
		if udevadm info -q property -p "$d" 2>/dev/null | grep -q '^ID_VENDOR_ID=057c' &&
		   udevadm info -q property -p "$d" 2>/dev/null | grep -q '^ID_MODEL_ID=62ff'; then
			echo "/dev/${d##*/}"
			return 0
		fi
	done
	return 1
}

if [ -z "$SRC" ]; then
	echo "Waiting up to 20 s for the stick in CD mode (057c:62ff)..."
	for i in $(seq 20); do SRC=$(find_cd) && break; sleep 1; done
	[ -n "$SRC" ] || { echo "Stick CD not found. Plug the stick in (see the header of this script)." >&2; exit 1; }
fi

CAB=
if [ -b "$SRC" ]; then
	MNT=$TMP/cd; mkdir "$MNT"
	mount -o ro "$SRC" "$MNT"
	CAB=$(find "$MNT" -maxdepth 1 -iname pushinst.cab | head -1)
elif [ -d "$SRC" ]; then
	CAB=$(find "$SRC" -maxdepth 1 -iname pushinst.cab | head -1)
elif [[ "$SRC" == *.[iI][sS][oO] ]]; then
	bsdtar -xf "$SRC" -C "$TMP" PUSHINST.CAB
	CAB=$TMP/PUSHINST.CAB
else
	CAB=$SRC
fi
[ -n "$CAB" ] && [ -f "$CAB" ] || { echo "PUSHINST.CAB not found in $SRC" >&2; exit 1; }

bsdtar -xf "$CAB" -C "$TMP" driver/FwUSB1b.bin
echo "$SHA256  $TMP/driver/FwUSB1b.bin" | sha256sum -c --quiet || {
	echo "Unexpected firmware (sha256 mismatch). Only 'Rev 2.4.0.72' from stick v1.1 is tested." >&2
	exit 1
}
install -Dm644 "$TMP/driver/FwUSB1b.bin" "$DEST"
echo "Installed $DEST"
