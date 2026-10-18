# acx-mac80211 for the AVM FRITZ!WLAN USB Stick v1.1 (TI TNETW1450)

A port of the long-dead [acx-mac80211](https://sourceforge.net/projects/acx100/)
driver to current Linux (tested on **7.2**), made to work with the
**AVM FRITZ!WLAN USB Stick v1.1** (USB `057c:6201`, TI TNETW1450 + TNETW3422
radio, 802.11b/g). This stick never had a working mainline driver.

> **Status: experimental.** It works for everyday use on the one stick it was
> developed with, and has known problems (below). Use at your own risk.

## What works

- Firmware upload and reboot (no replug needed between module reloads)
- Scan, WPA2-PSK (software CCMP via mac80211), DHCP, NetworkManager
- Signal in dBm, real rx/tx bitrates, minstrel rate control fed with the
  firmware's tx status (ACK, retries, rate)
- Measured about 1 m from the router: download ~2 MB/s, upload ~0.7 MB/s,
  0 % ping loss over 10 minutes

## Known problems

- **Download collapse next to the router.** After sustained traffic with the
  stick very close to the access point (-26…-33 dBm), the AP falls back to
  1 Mbit/s towards the stick (download ~0.1 MB/s) for minutes. Lowering the tx
  power delays it (the driver now defaults to the lowest level), cooling helps.
  Suspected receiver overload and/or heat; not tested at a distance yet.
  Details in [TNETW1450-USB.md](TNETW1450-USB.md).
- Range at the default (lowest) tx power is untested. Raise it with
  `txpower_max=20` if you are far from the access point.
- Station mode only. No hardware crypto, no power saving, no AP/IBSS mode.
- Three tx descriptor fields are not understood and sent as 0.
- Only firmware `Rev 2.4.0.72` from stick v1.1 is tested. Other acx USB
  devices (ACX100 USB) are untested with this port; the build is USB-only.

## Firmware

The firmware is AVM/TI property and **not included**. Every stick carries it
on its built-in virtual CD (the stick first shows up as a CD-ROM, `057c:62ff`).
`scripts/fritzwlan-firmware.sh` extracts it from there (or from a CD image or
`PUSHINST.CAB`), checks its SHA-256 and installs it as
`/lib/firmware/tiacx111usbc1B`.

## Install

Needs: kernel headers, `dkms`, `bsdtar` (libarchive), `eject` (util-linux),
systemd/udev.

```sh
git clone https://github.com/knauerleo/acx-mac80211-fritzwlan && cd acx-mac80211-fritzwlan
sudo scripts/fritzwlan-install.sh        # plug the stick in when asked
# later: replug the stick -> wlan interface (e.g. wlp0s20u9)
sudo scripts/fritzwlan-install.sh uninstall
```

The installer extracts the firmware, installs a udev rule that ejects the
virtual CD (the stick then switches to WLAN mode; without it, it switches by
itself after ~25 s) and builds the module with DKMS.

Build only:

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

## Module parameters

| parameter | default | meaning |
|---|---|---|
| `txpower_max` | 4 | tx power cap, dBm on acx's 0..20 scale (4 per level): 4 = lowest level, 20 = full power |
| `txstat` | 1 | complete tx frames from the firmware's tx status (0 = old behaviour, no rate control feedback) |
| `txcredits` | 8 | max frames in the firmware without a tx status (TI's limit; 1..16) |
| `debug` | 0xa | log mask (0xa = init + association), e.g. add `0x2000` for one line per tx frame / tx status |

Firmware IEs can be read and written live through debugfs
(`/sys/kernel/debug/acx_mac80211/phy*/ie`), see the protocol notes.

## Documentation

[TNETW1450-USB.md](TNETW1450-USB.md) describes the USB protocol as far as it
is known: firmware boot and upload padding, bulk-in record types, the tx
descriptor (a trailer, not a header), tx status records as credits, the
information elements seen on this firmware, tx power, and the driver bugs
fixed during the port. It was worked out for interoperability from live
traffic of the device and from studying AVM's drivers (function names,
structure layouts); it contains no vendor code.

The branch history keeps the original acx100/acx-mac80211 history; the port
starts after upstream `master`.

## License and credits

Same as upstream acx-mac80211: dual **MPL 1.1 / GPL v2** (see `README` and
`COPYING`). Original driver by the ACX100 Open Source Project and its
contributors.

FRITZ! is a trademark of AVM GmbH. This project is not affiliated with or
endorsed by AVM or Texas Instruments.
