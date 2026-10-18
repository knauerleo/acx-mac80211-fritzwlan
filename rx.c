/*
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2012
 * The ACX100 Open Source Project <acx100-devel@lists.sourceforge.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "acx_debug.h"

#include "acx.h"
#include "pci.h"
#include "mem.h"
#include "merge.h"
#include "usb.h"
#include "utils.h"
#include "rx.h"

/*
 * Calculate level like the feb 2003 windows driver seems to do
 *
 * Note: the FreeBSD and DragonFlyBSD drivers seems to use different
 * so-called correction constants depending on the chip. They will be
 * defined for now, but as it is still unknown whether they are correct
 * or not, only the original value will be used. Something else to take
 * into account is that the OpenBSD driver uses another approach and
 * defines the maximum RSSI value depending on the chip, rather than
 * using a value of 100 for all of them, as it is currently done here.
 */
#define ACX100_RSSI_CORR 8
#define ACX111_RSSI_CORR 5
static u8 acx_signal_to_winlevel(u8 rawlevel)
{
	/* u8 winlevel = (u8) (0.5 + 0.625 * rawlevel); */
	u8 winlevel = (((ACX100_RSSI_CORR / 2) + (rawlevel * 5)) /
			ACX100_RSSI_CORR);

	if (winlevel > 100)
		winlevel = 100;
	return winlevel;
}

/*
 * mac80211 wants dBm (<= 0): its beacon-signal EWMA adds -signal as an
 * unsigned value, so positive levels wrap into garbage CQM RSSI events,
 * and wpa_supplicant's bgscan then scans nonstop.  Radio formulas from TI's
 * whalCtrl_convertRxLevelToRSSI (AVM fwlanusb-lib.o); for the older radios
 * no formula is known, so shift the 0..100 level below 0.
 */
static int acx_signal_to_dbm(acx_device_t *adev, u8 rawlevel)
{
	switch (adev->radio_type) {
	case RADIO_16_RADIA_RC2422:
	case RADIO_17_UNKNOWN:
		return rawlevel - 0x5d;
	case RADIO_19_UNKNOWN:
		return rawlevel - 0x62;
	case 0x1a:
	case RADIO_1B_TI_TNETW3422:
		return (s8)rawlevel - 0x50;
	default:
		return acx_signal_to_winlevel(rawlevel) - 100;
	}
}

u8 acx_signal_determine_quality(u8 signal, u8 noise)
{
	int qual;

	qual = (((signal - 30) * 100 / 70) + (100 - noise * 4)) / 2;

	if (qual > 100)
		return 100;
	if (qual < 0)
		return 0;
	return qual;
}

const char* acx_get_packet_type_string(u16 fc)
{
	static const char * const mgmt_arr[] = {
		"MGMT/AssocReq", "MGMT/AssocResp", "MGMT/ReassocReq",
		"MGMT/ReassocResp", "MGMT/ProbeReq", "MGMT/ProbeResp",
		"MGMT/UNKNOWN", "MGMT/UNKNOWN", "MGMT/Beacon", "MGMT/ATIM",
		"MGMT/Disassoc", "MGMT/Authen", "MGMT/Deauthen",
		"MGMT/UNKNOWN"
	};
	static const char * const ctl_arr[] = {
		"CTL/PSPoll", "CTL/RTS", "CTL/CTS", "CTL/Ack", "CTL/CFEnd",
		"CTL/CFEndCFAck", "CTL/UNKNOWN"
	};
	static const char * const data_arr[] = {
		"DATA/DataOnly", "DATA/Data CFAck", "DATA/Data CFPoll",
		"DATA/Data CFAck/CFPoll", "DATA/Null", "DATA/CFAck",
		"DATA/CFPoll", "DATA/CFAck/CFPoll", "DATA/UNKNOWN"
	};
	const char *str;
	u8 fstype = (IEEE80211_FCTL_STYPE & fc) >> 4;
	u8 ctl;

	switch (IEEE80211_FCTL_FTYPE & fc) {
	case IEEE80211_FTYPE_MGMT:
		str = mgmt_arr[min((size_t)fstype, ARRAY_SIZE(mgmt_arr) - 1)];
		break;
	case IEEE80211_FTYPE_CTL:
		ctl = fstype - 0x0a;
		str = ctl_arr[min((size_t)ctl, ARRAY_SIZE(ctl_arr) - 1)];
		break;
	case IEEE80211_FTYPE_DATA:
		str = data_arr[min((size_t)fstype, ARRAY_SIZE(data_arr) - 1)];
		break;
	default:
		str = "UNKNOWN";
		break;
	}
	return str;
}


/*
 * acx_l_rx
 *
 * The end of the Rx path. Pulls data from a rxhostdesc into a socket
 * buffer and feeds it to the network stack via netif_rx().
 */
static void acx_rx(acx_device_t *adev, rxbuffer_t *rxbuf)
{
	struct ieee80211_rx_status *status;

	struct ieee80211_hdr *w_hdr;
	struct sk_buff *skb;
	int buflen;

	if (unlikely(!test_bit(ACX_FLAG_HW_UP, &adev->flags))) {
		pr_info("asked to receive a packet while hw down\n");
		return;
	}

	w_hdr = acx_get_wlan_hdr(adev, rxbuf);
	buflen = RXBUF_BYTES_RCVD(adev, rxbuf);

	/* Allocate our skb */
	skb = dev_alloc_skb(buflen);
	if (!skb) {
		pr_info("skb allocation FAILED\n");
		return;
	}

	skb_put(skb, buflen);
	memcpy(skb->data, w_hdr, buflen);

	status = IEEE80211_SKB_RXCB(skb);
	memset(status, 0, sizeof(*status));

	status->mactime = rxbuf->time;

	status->signal = acx_signal_to_dbm(adev, rxbuf->phy_level);

	if (adev->hw_encrypt_enabled && !(IS_USB(adev) && IS_ACX111(adev)))
		status->flag = RX_FLAG_DECRYPTED | RX_FLAG_IV_STRIPPED;

	status->freq = adev->rx_status.freq;
	status->band = adev->rx_status.band;

	status->antenna = 1;

	/* TODO I'm not sure whether this is (currently) really required. In tests
	 * this didn't made a difference. Maybe compare what other drivers do.
	 * TODO Verify translation to rate_idx.
	 */
	if (IS_ACX111(adev)) {
		u8 plcp = rxbuf->phy_plcp_signal;
		/* phy_stat_baseband bit 3 = OFDM (acx); only 0x0a needs it */
		int ofdm = plcp == 0x0a ? !!(rxbuf->phy_stat_baseband & 0x08) :
			   acx111_plcp_to_rate_idx(plcp, 0) < 0;
		int idx = acx111_plcp_to_rate_idx(plcp, ofdm);

		if (idx >= 0)
			status->rate_idx = idx;
		if (acx_debug & L_TXSTAT)
			printk_ratelimited(KERN_INFO "acx rx: plcp %02x bb %02x -> idx %d len %d\n",
					   plcp, rxbuf->phy_stat_baseband, idx, buflen);
	}

	adev->stats.rx_packets++;
	adev->stats.rx_bytes += skb->len;

	if (IS_PCI(adev)) {
#if CONFIG_ACX_MAC80211_VERSION <= KERNEL_VERSION(2, 6, 32)
		local_bh_disable();
		ieee80211_rx(adev->hw, skb);
		local_bh_enable();
#else
		ieee80211_rx_ni(adev->hw, skb);
#endif
	}
	/* Usb Rx is happening in_interupt() */
	else if (IS_USB(adev) || IS_MEM(adev))
		ieee80211_rx_irqsafe(adev->hw, skb);
	else
		logf0(L_ANY, "ERROR: Undefined device type !?\n");

}

/*
 * acx_l_process_rxbuf
 *
 * NB: used by USB code also
 */
void acx_process_rxbuf(acx_device_t *adev, rxbuffer_t *rxbuf)
{
	struct ieee80211_hdr *hdr;
	u16 fc, buf_len;
	int raw_len = le16_to_cpu(rxbuf->mac_cnt_rcvd) & 0xfff;

	/* A bogus length would turn into a huge memcpy in acx_rx() and
	 * overwrite kernel memory -- the old "kills kernel on scans" bug. */
	if (unlikely(raw_len <= adev->phy_header_len + 10 ||
		     raw_len - adev->phy_header_len > WLAN_A4FR_MAXLEN_FCS)) {
		printk_ratelimited(KERN_WARNING "acx: rx: dropping frame with bogus length %d\n",
				   raw_len);
		return;
	}

	hdr = acx_get_wlan_hdr(adev, rxbuf);
	fc = le16_to_cpu(hdr->frame_control);
	/* length of frame from control field to first byte of FCS */
	buf_len = RXBUF_BYTES_RCVD(adev, rxbuf);

	/* For debugging */
	if (((IEEE80211_FCTL_STYPE & fc) != IEEE80211_STYPE_BEACON)
		&& (acx_debug & (L_XFER|L_DATA))) {

		printk_ratelimited(
			"acx: rx: %s time:%u len:%u signal:%u,raw=%u"
			"SNR:%u,raw=%u macstat:%02X "
			"phystat:%02X phyrate:%u status:%u\n",
			acx_get_packet_type_string(fc),
			le32_to_cpu(rxbuf->time), buf_len,
			acx_signal_to_winlevel(rxbuf->phy_level),
			rxbuf->phy_level,
			acx_signal_to_winlevel(rxbuf->phy_snr),
			rxbuf->phy_snr, rxbuf->mac_status,
			rxbuf->phy_stat_baseband,
			rxbuf->phy_plcp_signal,
			adev->status);
	}

	if (unlikely(acx_debug & L_DATA)) {
		pr_info("rx: 802.11 buf[%u]: \n", buf_len);
		acx_dump_bytes(hdr, buf_len);
	}

	acx_rx(adev, rxbuf);

	/* Now check Rx quality level, AFTER processing packet.  I
	 * tried to figure out how to map these levels to dBm values,
	 * but for the life of me I really didn't manage to get
	 * it. Either these values are not meant to be expressed in
	 * dBm, or it's some pretty complicated calculation. */

	/* FIXME OW 20100619 Is this still required. Only for adev local use.
	 * Mac80211 signal level is reported in acx_l_rx for each skb.
	 */
	/* TODO: only the RSSI seems to be reported */
	adev->rx_status.signal = acx_signal_to_winlevel(rxbuf->phy_level);


}

/*
 * PLCP signal byte -> index into acx111_rates[] (main.c: 1 2 5.5 6 9 11 12 18
 * 24 36 48 54). CCK codes are the rate in 100 kbit/s, OFDM codes are the
 * 802.11a SIGNAL rate nibble. 0x0a is 1M CCK and 12M OFDM; the caller says
 * which (rx: phy_stat_baseband, tx status: the rate mask). -1 if unknown.
 */
int acx111_plcp_to_rate_idx(u8 plcp, int ofdm)
{
	static const u8 cck[] = { 0x0a, 0x14, 0x37, 0x00, 0x00, 0x6e };
	static const u8 ofdm_sig[12] = {
		[3] = 0xb, [4] = 0xf, [6] = 0xa, [7] = 0xe,
		[8] = 0x9, [9] = 0xd, [10] = 0x8, [11] = 0xc,
	};
	int i;

	if (!ofdm) {
		for (i = 0; i < ARRAY_SIZE(cck); i++)
			if (cck[i] && cck[i] == plcp)
				return i;
		return -1;
	}
	for (i = 0; i < ARRAY_SIZE(ofdm_sig); i++)
		if (ofdm_sig[i] && ofdm_sig[i] == (plcp & 0xf))
			return i;
	return -1;
}


