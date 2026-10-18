# TNETW1450 USB protocol notes (AVM FRITZ!WLAN USB Stick v1.1)

Reverse-engineered 2026-10-18 while porting acx-mac80211 to Linux 7.2.
Device: AVM FRITZ!WLAN USB Stick v1.1, TI TNETW1450 + TNETW3422 radio
(radio type 0x1B), firmware `Rev 2.4.0.72`, EEPROM 0x05.

Sources:
- **Live device traffic** (hex dumps from this driver), the strongest evidence.
- **AVM Linux driver** `fwlanusb-1.00.00` (2007), `fritz/lib/fwlanusb-lib.o`.
  This is TI's own stack and it is **not stripped**, so function names are
  available (`usbTx_SendOneMsdu`, `usbTxDesc_FieldsInfoSet`, `MSDU2Buff`, `usbRx_*`,
  `whalBus_*`, `usbCmdMbox_*`). Prefer this over the Windows driver.
- **Windows driver** `fwlanusb.sys` 2.0.6.1647, same code without symbols.
  Functions are named here by their Ghidra address.

Where a field is marked *unknown*, the driver sets it to 0 and it still works.

## USB identity and endpoints

| State | VID:PID | Notes |
|---|---|---|
| CD-ROM (power-on) | 057c:62ff | usb-storage "FRITZ! WLAN selfinstall" with the Windows driver. `eject /dev/sr0` switches mode |
| WLAN, firmware not loaded | 057c:6201 | vendor-class interface |
| after firmware boot | 057c:6201 | same ID, no re-enumeration |
| booted, then USB port reset | 0451:60c5 | half-dead, see "Rebooting a running firmware" |

Endpoints: EP1 OUT bulk 512, EP2 IN bulk 512 (high speed). Commands
(configure/interrogate) also go over EP0 control (`ACX_USB_REQ_CMD`) as in acx.

## Firmware boot (acx's code is correct)

1. Read the "ready" message from EP2. Expected: `u16[0] & 0xc000 == 0xc000`,
   `u16[1] == 1`, `u32[1] == 0x40000001`, radio type in byte 8. **The stick sends
   this only once after plug-in**, so you must replug before every boot attempt.
2. Firmware file `tiacx111usbc1B` = `FwUSB1b.bin` from the CD:
   `le32 checksum, le32 size, data`. Checksum = byte sum over bytes 4..end.
3. Header to EP1: `u16 0xc000, u16 0x000b, u32 len, u32 size, u32 checksum`.
   `len = size-4` if padding else `size-8`. **Pad when the image takes an even
   number of wMaxPacketSize packets** (AVM `os_usb_need_file_padding`, also
   fwlanusb.sys): `((size-1)/512+1) % 2 == 0`. acx's rule (2048-byte transfers)
   pads FwUSB1b.bin wrongly; the firmware still boots, but on every other boot
   it then sends each tx frame only when the next bulk-out transfer arrives.
4. Data in 2048-byte blocks, each u32 byte-swapped (be32). Optionally 4 zero
   bytes for padding.
5. Read a 20-byte response: `u32[1] == 0` and `u16[8] == 1`. `0x40000003` means error.

### Rebooting a running firmware

The "ready" message comes only from the boot ROM. To get back there without
replugging, send **`ACX1xx_CMD_RESET` (0x0000) with no parameters over EP0**
(vendor request 0x12, 4 zero bytes, don't wait for a response). The ROM then
sends a fresh ready message. TI's stack does the same on shutdown
(`whalCtrl_StopHal` → `whalBus_FwCtrl_Reset` → `OsUsb_WriteCmdAsync`).

A **USB port reset** (`usb_reset_device`, which AVM's glue code calls in probe)
does *not* work on a booted stick. The chip re-enumerates as **0451:60c5**
(class 0xe0, bcdDevice 1.84, no strings). In that state it stalls EP0 commands,
sends no ready message, and accepts about 6 KB of a firmware upload before the
bulk-out times out. Only replugging recovers it.

Milder host-side resets tried instead, all **worse than doing nothing**: a
`usb_clear_halt()` on both bulk endpoints before re-uploading the firmware
makes the upload itself time out (-110); `usb_clear_halt()` on the OUT
endpoint only makes the upload succeed only sometimes; `usb_reset_endpoint()`
(host-side toggle reset, no bus traffic) makes the upload fail outright. The
boot ROM simply ignores `CLEAR_FEATURE` for the data toggle. After any of
these a replug is needed, same as after a port reset.

## Bulk-IN records (device → host)

A transfer contains one or more records. The first word of each record is:

```
le16  w = type << 14 | total_len      (total_len includes this word)
```

Types per `fwlanusb.sys` FUN_000427f2 / FUN_00042a70:

| type | length check | meaning |
|---|---|---|
| 0 | `total >= payload_len + 20` | received 802.11 frame |
| 1 | `== 100` | info block (payload at +20), unused |
| 2 | `== 24` | tx status |
| 3 | `<= 0x414`, `u16[8] == 0x14` | command response (same family as the boot messages) |

### Type 0: rx frame

```
+0   le16 total_len (type 0)
+2   u8   seq, u8 0xc6
+4   le16 payload_len (= total_len - 20)
+6   le16 0x000b
+8   le32 device address of the rx descriptor (steps 0x28 through rx_queue_start ring)
+12  ...  rest of the chip's rx descriptor (unknown)
+20  rxbuffer_t (classic acx: mac_cnt_rcvd, mblks, mac_status, phy_stat, plcp, level, snr, le32 time)
     level → dBm per TI whalCtrl_convertRxLevelToRSSI: radio 0x1b/0x1a (s8)level - 80,
     0x16/0x17 level - 0x5d, 0x19 level - 0x62 (gives -24 dBm next to the router)
+32  8-byte phy header
+40  802.11 frame (mac_cnt_rcvd & 0xfff includes the 8 phy bytes)
```

The old acx parsed +0 as `rxbuffer_t`. That gave garbage lengths, including
`-8` = 65528, and a 64 KiB memcpy over kernel memory. This was the historical
"driver kills kernel on inbound scans from fritz box" bug.

### Type 2: tx status (24 bytes)

Example: `18 80 3d ea 00 00 00 00 40 00 01 00 00 00 00 00 00 00 00 00 00 00 00 0a`

```
+0   0x8018           type 2, len 24
+2   u8 seq (= tx descriptor byte 14 of the frame), u8 0xea
+8   le16 frame control of the sent frame
+10  u16 rate bitmask (RATE111_*) actually allowed
+0x12 u8 status       0x00 ok, 0x20 failed after retries (8), 0x08 seen as well
+0x13 u8 ack failures / retries
+0x16 u8 hw queue
+0x17 u8 PLCP rate used (0x0a = 1M, 0x0b = 6M OFDM, ...)
```

There is no host cookie in it, but **byte 2 echoes the tx descriptor's
sequence byte 14** (seen on every frame of a 1000-frame trace), so a record
can be matched to its frame exactly. Records come in submit order; under
overload (see credits below) some get lost or arrive late.

**The records are tx credits.** TI's `usbTx_SendOneMsdu` counts `sent++` per
hw queue (+0x2c), every record runs `usbTx_Clean` (`completed++`, +0x30; a
record with nothing outstanding counts as the firmware's own frame, +0x40),
and `usbTx_CheckSize` refuses to send while `sent - completed > 7`. So at most
**8 frames per hw queue** may be in the firmware without a status. A host that
keeps sending anyway (acx released its buffers on URB completion) makes the
firmware stop transmitting under sustained load, without any USB error.
`usbTx_TxCmplt` passes status, retries and both rate bytes to the upper layer.

The driver completes each skb from its record (ACK, retry count, rate) and
stops the mac80211 queue at 8 frames without status (module parameters
`txstat`, `txcredits`).

The firmware itself sometimes transmits frames the host never queued: type-2
status records with frame control `0x0040` (probe request) show up on an
idle, associated link. Seen with the monitor sniffer (`tools/rx-sniff.py`)
and matching type-2 dumps; harmless, but a reason not to assume every tx
status corresponds to a host-submitted skb.

## Bulk-OUT tx frame (host → device)

`usbTx_SendOneMsdu` appends the 16-byte descriptor as the **last** buffer of the
chain, and `MSDU2Buff` copies the chain in order. **The descriptor is a trailer:**

```
[802.11 frame][0x00 if (frame_len + 16) % wMaxPacketSize == 0][16-byte descriptor]
```

Descriptor (`usbTxDesc_FieldsInfoSet` = `fwlanusb.sys` FUN_00043014):

| off | size | content |
|---|---|---|
| 0 | le16 | 802.11 frame length (without descriptor/pad) |
| 2 | u8 | hw queue (0 is used and works) |
| 3 | u8 | flags: 0x10 always; 0x20 = RTS (len > RTS threshold); 0x01/0x02 from tx-param bits 1/2 (*unknown*); 0x80 special mode when the cookie's bit 7 is set |
| 4 | le32 | per-packet context +0x3c (*unknown*, 0) |
| 8 | le16 | allowed rates, acx111 `RATE111_*` bitmask (1M=0x1, 2M=0x2, 5.5M=0x4, 6M=0x8, 9M=0x10, 11M=0x20, 12M=0x40, 18M=0x80, 22M=0x100, 24M=0x200, 36M=0x400, 48M=0x800, 54M=0x1000); \|0x8000 and \|0x4000 when tx-param fields are set (*unknown*) |
| 10 | u8 | tx-param byte 0x12 (*unknown*, 0) |
| 11 | u8 | PLCP rate: 1M 0x0a, 2M 0x14, 5.5M 0x37, 11M 0x6e, 22M 0xdc; OFDM 6M 0x0b, 9M 0x0f, 12M 0x0a, 18M 0x0e, 24M 0x09, 36M 0x0d, 48M 0x08, 54M 0x0c |
| 12 | le16 | frame control (copied from the frame) |
| 14 | u8 | running sequence counter |
| 15 | u8 | 0 |

acx's classic 14-byte `usb_txbuffer_t` header (it starts with `0a 00`) is read by
the firmware as a 10-byte type-0 record and wedges it: rx stops after about two
tx status records.

## Init differences vs. the PCI-derived acx code

- `ACX111_IE_MEMORY_CONFIG_OPTIONS` (IE 0x0003, len 0x28): USB uses **`options = 4`**
  and `rx_queue1_host_rx_start = 0` (FUN_0004221e → FUN_00030ea2). acx sent 0
  (busmaster indirect). Offset 14 (le16) in Windows = config-table value >> 2.
  acx writes 0 there, which still works.
- Do **not** send `ACX1xx_CMD_SLEEP` on stop. Without it, NetworkManager's
  down/up cycle (MAC randomisation) works. Not proven harmful, only kept out.
- Crypto: the tx descriptor has no key fields we understand, so `set_key` is
  refused (mac80211 software CCMP). Firmware crypto is switched off
  (`FEATURE2_NO_TXCRYPT | FEATURE2_SNIFFER`), otherwise encrypted rx frames are
  dropped and DHCP fails.

## Information elements seen on fw 2.4.0.72

IDs and lengths from `fwlanusb-lib.o` `whal_hwInfoElem*`, values read live
(debugfs `ie`) while associated:

| IE | TI function | len | live value / notes |
|---|---|---|---|
| 0x0002 | AcxPwrMgmtOptions | 0x18 | interrogate fails |
| 0x0003 | ConfigMemory | 0x28 | interrogate fails. TI defaults: 32 rx descriptors (`NumACXRxDescriptors`), 16 tx, 4 tx data queues; acx sends 16 rx, 5×16 tx |
| 0x000a | Aid | 2 | 0 (acx never sets it) |
| 0x000e | FcsErrorRead | 4 | FCS error counter, **clear on read** |
| 0x0010 | RxConfig | 4 | `10 00 f5 0f` |
| 0x0012 | BssPowerSave | 1 | 0 (off) |
| 0x0013 | statistics | 0x134 | longer than acx's struct; acx111 field names fit: +0x14 rx_dropped_frame (~FCS errors), +0x18 rx_frame_ptr_err (~30/10 s, meaning unknown), +0x28 rx_dma_req, +0x30 tx_dma_req, +0x38 cmd_cmplt (== +0x68 commands), +0x44 rx_cmplt. These count **interrupts/DMA transfers, not frames** (mac80211 saw 238 frames while rx_dma_req went +147) |
| 0x001f | AcxBeaconFilterOptions | 2 | 0 (off) |
| 0x0023 | PacketDetectionThreshold | 4 | 0x70 |
| 0x0025 | CcaThreshold | 2 | 0xffef |
| 0x0027 | DtimPeriod | 2 | interrogate fails |
| 0x000b | TxPowerLevelTable (`whal_hwInfoElemTxPowerLevelTable`) | 0x18 | interrogate fails; TI writes `u32 1` + a 20-byte table |
| 0x100d | TxPowerLevel (`whal_hwInfoElemTxPowerLevel`) | 2 | **1 after boot = full power**. 1 byte level 1..5 (+1 pad byte); 5 = lowest. See "Tx power" |
| 0x100b | CurrAntenna | 2 | `4a 04` (fw default). TI writes rx byte from `dot11RxAntenna` (default 2): 1→0x0a, 2→0x2a, 3→0x4a, 4→0x6a; tx byte from `dot11TxAntenna` (default 2 → 0x04). Changing rx made no measurable difference |

## Radio control in TI's host stack (not in acx)

Registry defaults (`regFillInitTable`) point to host-side radio management that
acx lacks: `CalibrationEnable` 1, `TddCalibrationInterval` 300,
`CrtCalibrationInterval` 2, an LNA control loop (`whal_hwMboxCmd_LNAControl`;
`LnaAgcHighThresh` 45, `LnaAgcLowThresh` 51, `LnaFftBadCorrCountThresh` 50,
`LnaCheckTime` 2, `LnaRestoreTime` 600, `LnaPacketDetectionUnderACIHigh` 213) and
hardware ACI settings (`HardwareACIMode` 0, `InputCCA` 1, `QualifiedCCA` 3,
`StompForRx` 2, `TxCCA` 1; IE via `whal_hwInfoElemACIConfigurationSet`),
`EnergyDetection` 255. Once suspected for the time-varying rx loss, but that was
the driver's signal reporting (garbage CQM RSSI → bgscan loop, fixed in 5662f64).
Still relevant if sensitivity at a distance turns out to be poor.

## Tx power and the "AP falls back to 1M" collapse

acx never set the tx power on USB, so the firmware stays at its boot level 1
(full power). Measured with the stick about 1 m from the router (-26…-33 dBm):

| tx level (IE 0x100d) | sustained OFDM traffic | result |
|---|---|---|
| 1 (boot default) | download, tx at 54M | AP→stick rate drops to 1 Mbit/s after ~20 s, download ~0.1 MB/s |
| 3 | download | held ~60 s, then the same |
| 5 | download, 4 × 20 s | held: 1.8–2.2 MB/s, AP stays at 48/54M |
| 5 | 20 s upload at 0.7 MB/s | collapsed afterwards |

In the collapsed state only the AP→stick direction is broken: our own upload
keeps running at 0.7 MB/s, but the AP sends to us at 1M (the 802.11 rx bitrate
during a download is 1.0), the medium is busy with those slow frames, our TCP
ACKs wait, and their status records run into the 500 ms timeout. The state
lasts minutes and survives a firmware reboot (module reload); cooling the
stick with a fan brought it back without a reload. The installed driver
before the tx status work never left 1M/6M for its own frames and did not
show it.

Not solved. Candidates: receiver overload next to the AP (OFDM needs a linear
receiver, 1M DSSS does not) with gain control (LNA/AGC) stuck after tx bursts,
heat, or both. TI's host stack has the tools that acx lacks: per-rate power
levels (`TxPowerLevel_24_11B`, `_6_9_12`, `_18_24_36`, `_48_54`), a power
control that lowers tx power near the AP (`TxPowerControlOn`,
`TxPowerRssiThresh`, `TxPowerRssiRestoreThresh`) and the LNA/ACI control
below. Not yet tested at a distance from the router.

The driver now sets the level and caps it at 5 by default (`txpower_max`).

## Driver-side pitfalls fixed during the port (acx-mac80211 branch `port-7.2`)

- USB-only build is required. With PCI built in, PCI-only functions run on the
  USB device → oops in `acx_create_desc_queues`.
- Stack buffers handed to USB (fw upload cmdbuf); ready-loop off-by-one.
- `acx_is_hw_tx_queue_stop_limit` checked 5 hw queues while USB only accounts
  `hw_tx_queue[0]` → the queue stopped forever after the first frame.
- Async `usb_unlink_urb` on stop + resubmit on start (`-EBUSY` ignored); HW_UP set
  after the rx submit → rx dead after down/up. Now `usb_kill_urb`, HW_UP first.
- rx resubmit livelock on persistent URB errors; unchecked txstat `hostdata`
  index; rx stats use-after-free; tx rate stored in u8 (540 = 54M overflowed).
- rx signal reported as `SIGNAL_UNSPEC` 0..100: mac80211's beacon EWMA wrapped,
  garbage CQM RSSI events made wpa_supplicant bgscan scan nonstop (~50 % loss).
  Now dBm (`acx_signal_to_dbm`, commit 5662f64).
- Firmware upload padding counted 2048-byte transfers instead of
  wMaxPacketSize (512-byte) packets (see "Firmware boot" step 3). The two
  rules disagree for `FwUSB1b.bin`, so on every other boot (padding state
  survives `CMD_RESET`) the firmware held each bulk-out tx frame until the
  next one arrived: exactly 50 % ping loss, alternating, at any tx rate.
  Now counts packets like AVM's glue (commit 64efc85).
- `debugflags=` name table was shifted against the real bit values (fixed;
  numeric `debug=` works too: 0x8282 = USBRXTX|XFER|DEBUG|INIT, 0x100 = L_DATA,
  0x2000 = L_TXSTAT, one line per tx frame / status record / rx rate).
- Every tx frame was completed on URB completion with ACK set and rate count 0:
  minstrel never learned anything (tx stuck at 1M/6M), and nothing limited the
  frames in flight → tx stall under sustained upload. Now completed from the
  type-2 records with TI's credit limit (commit eb96475).
- rx `rate_idx` was never set (the old helper used the wrong index order).
