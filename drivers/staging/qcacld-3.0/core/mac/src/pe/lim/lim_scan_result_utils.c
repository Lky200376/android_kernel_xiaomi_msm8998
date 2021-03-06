/*
 * Copyright (c) 2011-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/*
 * This file lim_scan_result_utils.cc contains the utility functions
 * LIM uses for maintaining and accessing scan results on STA.
 * Author:        Chandra Modumudi
 * Date:          02/13/02
 * History:-
 * Date           Modified by    Modification Information
 * --------------------------------------------------------------------
 */

#include "lim_types.h"
#include "lim_utils.h"
#include "lim_ser_des_utils.h"
#include "lim_api.h"
#include "lim_ft_defs.h"
#include "lim_session.h"
#include "rrm_api.h"
#include "cds_utils.h"

#ifdef WLAN_FEATURE_FILS_SK
/**
 * lim_update_bss_with_fils_data: update fils data to bss descriptor
 * if available in probe/beacon.
 * @pr: probe response/beacon
 * @bss_descr: pointer to bss descriptor
 *
 * @Return: None
 */
static void lim_update_bss_with_fils_data(tpSirProbeRespBeacon pr,
				tSirBssDescription *bss_descr)
{
	if (!pr->fils_ind.is_present)
		return;

	if (pr->fils_ind.realm_identifier.realm_cnt > SIR_MAX_REALM_COUNT)
		pr->fils_ind.realm_identifier.realm_cnt = SIR_MAX_REALM_COUNT;

	bss_descr->fils_info_element.realm_cnt =
		pr->fils_ind.realm_identifier.realm_cnt;
	qdf_mem_copy(bss_descr->fils_info_element.realm,
		pr->fils_ind.realm_identifier.realm,
		bss_descr->fils_info_element.realm_cnt * SIR_REALM_LEN);
	if (pr->fils_ind.cache_identifier.is_present) {
		bss_descr->fils_info_element.is_cache_id_present = true;
		qdf_mem_copy(bss_descr->fils_info_element.cache_id,
			pr->fils_ind.cache_identifier.identifier, CACHE_ID_LEN);
	}
	if (pr->fils_ind.is_fils_sk_auth_supported)
		bss_descr->fils_info_element.is_fils_sk_supported = true;
}
#else
static inline void lim_update_bss_with_fils_data(tpSirProbeRespBeacon pr,
				tSirBssDescription *bss_descr)
{
}
#endif

#ifdef FEATURE_WLAN_ESE
static inline void populate_qbss_load_status(tSirBssDescription *pBssDescr,
				tpSirProbeRespBeacon pBPR)
{
	if (pBPR->QBSSLoad.present) {
		pBssDescr->QBSSLoad_present = true;
		pBssDescr->QBSSLoad_avail = pBPR->QBSSLoad.avail;
		pBssDescr->qbss_chan_load = pBPR->QBSSLoad.chautil;
		pBssDescr->qbss_stacount = pBPR->QBSSLoad.stacount;
	}
}
#else
static inline void populate_qbss_load_status(tSirBssDescription *pBssDescr,
				tpSirProbeRespBeacon pBPR)
{
}
#endif

/**
 * lim_get_nss_supported_by_ap() - finds out nss from AP
 * @bcn: beacon structure pointer
 *
 * Return: number of nss advertised by AP
 */
static uint8_t lim_get_nss_supported_by_ap(tpSirProbeRespBeacon bcn)
{
	if (bcn->VHTCaps.present) {
		if ((bcn->VHTCaps.rxMCSMap & 0xC0) != 0xC0)
			return 4;

		if ((bcn->VHTCaps.rxMCSMap & 0x30) != 0x30)
			return 3;

		if ((bcn->VHTCaps.rxMCSMap & 0x0C) != 0x0C)
			return 2;
	} else if (bcn->HTCaps.present) {
		if (bcn->HTCaps.supportedMCSSet[3])
			return 4;

		if (bcn->HTCaps.supportedMCSSet[2])
			return 3;

		if (bcn->HTCaps.supportedMCSSet[1])
			return 2;
	}

	return 1;
}

/**
 * lim_collect_bss_description()
 *
 ***FUNCTION:
 * This function is called during scan upon receiving
 * Beacon/Probe Response frame to check if the received
 * frame matches scan criteria, collect BSS description
 * and add it to cached scan results.
 *
 ***LOGIC:
 *
 ***ASSUMPTIONS:
 * NA
 *
 ***NOTE:
 * NA
 *
 * @param  pMac - Pointer to Global MAC structure
 * @param  pBPR - Pointer to parsed Beacon/Probe Response structure
 * @param  pRxPacketInfo  - Pointer to Received frame's BD
 * @param  fScanning - flag to indicate if it is during scan.
 * ---------------------------------------------
 *
 * @return None
 */
void
lim_collect_bss_description(tpAniSirGlobal pMac,
			    tSirBssDescription *pBssDescr,
			    tpSirProbeRespBeacon pBPR,
			    uint8_t *pRxPacketInfo, uint8_t fScanning)
{
	uint8_t *pBody;
	uint32_t ieLen = 0;
	tpSirMacMgmtHdr pHdr;
	uint8_t channelNum;
	uint8_t rxChannel;
	uint8_t rfBand = 0;

	pHdr = WMA_GET_RX_MAC_HEADER(pRxPacketInfo);

	if (SIR_MAC_B_PR_SSID_OFFSET > WMA_GET_RX_PAYLOAD_LEN(pRxPacketInfo)) {
		QDF_ASSERT(WMA_GET_RX_PAYLOAD_LEN(pRxPacketInfo) >=
			   SIR_MAC_B_PR_SSID_OFFSET);
		return;
	}
	ieLen =
		WMA_GET_RX_PAYLOAD_LEN(pRxPacketInfo) - SIR_MAC_B_PR_SSID_OFFSET;
	rxChannel = WMA_GET_RX_CH(pRxPacketInfo);
	pBody = WMA_GET_RX_MPDU_DATA(pRxPacketInfo);
	rfBand = WMA_GET_RX_RFBAND(pRxPacketInfo);

	/**
	 * Length of BSS desription is without length of
	 * length itself and length of pointer that holds ieFields.
	 *
	 * tSirBssDescription
	 * +--------+---------------------------------+---------------+
	 * | length | other fields                    | pointer to IEs|
	 * +--------+---------------------------------+---------------+
	 *                                            ^
	 *                                            ieFields
	 */
	pBssDescr->length = (uint16_t)(offsetof(tSirBssDescription, ieFields[0])
					- sizeof(pBssDescr->length) + ieLen);

	/* Copy BSS Id */
	qdf_mem_copy((uint8_t *) &pBssDescr->bssId,
		     (uint8_t *) pHdr->bssId, sizeof(tSirMacAddr));

	/* Copy Timestamp, Beacon Interval and Capability Info */
	pBssDescr->scansystimensec = qdf_get_bootbased_boottime_ns();

	pBssDescr->timeStamp[0] = pBPR->timeStamp[0];
	pBssDescr->timeStamp[1] = pBPR->timeStamp[1];
	pBssDescr->beaconInterval = pBPR->beaconInterval;
	pBssDescr->capabilityInfo =
		lim_get_u16((uint8_t *) &pBPR->capabilityInfo);

	/* HT capability */
	if (pBPR->HTCaps.present) {
		pBssDescr->ht_caps_present = 1;
		if (pBPR->HTCaps.supportedChannelWidthSet)
			pBssDescr->chan_width = eHT_CHANNEL_WIDTH_40MHZ;
	}

	/* VHT Parameters */
	if (IS_BSS_VHT_CAPABLE(pBPR->VHTCaps) ||
	    IS_BSS_VHT_CAPABLE(pBPR->vendor_vht_ie.VHTCaps)) {
		pBssDescr->vht_caps_present = 1;
		if ((IS_BSS_VHT_CAPABLE(pBPR->VHTCaps) &&
		     pBPR->VHTCaps.suBeamFormerCap) ||
		    (IS_BSS_VHT_CAPABLE(pBPR->vendor_vht_ie.VHTCaps) &&
		     pBPR->vendor_vht_ie.VHTCaps.suBeamFormerCap))
			pBssDescr->beacomforming_capable = 1;
	}
	if (pBPR->VHTOperation.present)
		if (pBPR->VHTOperation.chanWidth == 1)
			pBssDescr->chan_width = eHT_CHANNEL_WIDTH_80MHZ;
	/*
	 * If the ESP metric is transmitting multiple airtime fractions, then
	 * follow the sequence AC_BE, AC_VI, AC_VO, AC_BK and pick whichever is
	 * the first one available
	 */
	if (pBPR->esp_information.is_present) {
		if (pBPR->esp_information.esp_info_AC_BE.access_category
				== ESP_AC_BE)
			pBssDescr->air_time_fraction =
				pBPR->esp_information.esp_info_AC_BE.
				estimated_air_fraction;
		else if (pBPR->esp_information.esp_info_AC_VI.access_category
				== ESP_AC_VI)
			pBssDescr->air_time_fraction =
				pBPR->esp_information.esp_info_AC_VI.
				estimated_air_fraction;
		else if (pBPR->esp_information.esp_info_AC_VO.access_category
				== ESP_AC_VO)
			pBssDescr->air_time_fraction =
				pBPR->esp_information.esp_info_AC_VO.
				estimated_air_fraction;
		else if (pBPR->esp_information.esp_info_AC_BK.access_category
				== ESP_AC_BK)
			pBssDescr->air_time_fraction =
				pBPR->esp_information.esp_info_AC_BK.
				estimated_air_fraction;
	}
	pBssDescr->nss = lim_get_nss_supported_by_ap(pBPR);

	if (!pBssDescr->beaconInterval) {
		pe_warn("Beacon Interval is ZERO, making it to default 100 "
			   MAC_ADDRESS_STR, MAC_ADDR_ARRAY(pHdr->bssId));
		pBssDescr->beaconInterval = 100;
	}
	/*
	 * There is a narrow window after Channel Switch msg is sent to HAL and before the AGC is shut
	 * down and beacons/Probe Rsps can trickle in and we may report the incorrect channel in 5Ghz
	 * band, so not relying on the 'last Scanned Channel' stored in LIM.
	 * Instead use the value returned by RXP in BD. This the the same value which HAL programs into
	 * RXP before every channel switch.
	 * Right now there is a problem in 5Ghz, where we are receiving beacons from a channel different from
	 * the currently scanned channel. so incorrect channel is reported to CSR and association does not happen.
	 * So for now we keep on looking for the channel info in the beacon (DSParamSet IE OR HT Info IE), and only if it
	 * is not present in the beacon, we go for the channel info present in RXP.
	 * This fix will work for 5Ghz 11n devices, but for 11a devices, we have to rely on RXP routing flag to get the correct channel.
	 * So The problem of incorrect channel reporting in 5Ghz will still remain for 11a devices.
	 */
	pBssDescr->channelId = lim_get_channel_from_beacon(pMac, pBPR);

	if (pBssDescr->channelId == 0) {
		/* If the channel Id is not retrieved from Beacon, extract the channel from BD */
		if (!rxChannel) {
			rxChannel = pMac->lim.gLimCurrentScanChannelId;
		}
		pBssDescr->channelId = rxChannel;
	}

	pBssDescr->channelIdSelf = pBssDescr->channelId;
	pBssDescr->rx_channel = rxChannel;

	/* set the network type in bss description */
	channelNum = pBssDescr->channelId;
	pBssDescr->nwType =
		lim_get_nw_type(pMac, channelNum, SIR_MAC_MGMT_FRAME, pBPR);

	/* Copy RSSI & SINR from BD */

	pBssDescr->rssi = (int8_t) WMA_GET_RX_RSSI_NORMALIZED(pRxPacketInfo);
	pBssDescr->rssi_raw = (int8_t) WMA_GET_RX_RSSI_RAW(pRxPacketInfo);

	/* SINR no longer reported by HW */
	pBssDescr->sinr = 0;
	pBssDescr->received_time = (uint64_t)qdf_mc_timer_get_system_time();
	pBssDescr->tsf_delta = WMA_GET_RX_TSF_DELTA(pRxPacketInfo);
	pBssDescr->seq_ctrl = pHdr->seqControl;

	pe_debug(MAC_ADDRESS_STR
		" rssi: norm %d abs %d tsf_delta %u RcvdTime %llu ssid %s",
		MAC_ADDR_ARRAY(pHdr->bssId), pBssDescr->rssi,
		pBssDescr->rssi_raw, pBssDescr->tsf_delta,
		pBssDescr->received_time,
		((pBPR->ssidPresent) ? (char *)pBPR->ssId.ssId : ""));

	if (fScanning) {
		rrm_get_start_tsf(pMac, pBssDescr->startTSF);
		pBssDescr->parentTSF = WMA_GET_RX_TIMESTAMP(pRxPacketInfo);
	}

	/* MobilityDomain */
	pBssDescr->mdie[0] = 0;
	pBssDescr->mdie[1] = 0;
	pBssDescr->mdie[2] = 0;
	pBssDescr->mdiePresent = false;
	/* If mdie is present in the probe resp we */
	/* fill it in the bss description */
	if (pBPR->mdiePresent) {
		pBssDescr->mdiePresent = true;
		pBssDescr->mdie[0] = pBPR->mdie[0];
		pBssDescr->mdie[1] = pBPR->mdie[1];
		pBssDescr->mdie[2] = pBPR->mdie[2];
	}

	populate_qbss_load_status(pBssDescr, pBPR);
	lim_update_bss_with_fils_data(pBPR, pBssDescr);
	/* Copy IE fields */
	qdf_mem_copy((uint8_t *) &pBssDescr->ieFields,
		     pBody + SIR_MAC_B_PR_SSID_OFFSET, ieLen);

	/*set channel number in beacon in case it is not present */
	pBPR->channelNumber = pBssDescr->channelId;

	pe_debug("Collected BSS Description for Channel: %1d length: %u IE Fields: %u",
		pBssDescr->channelId, pBssDescr->length, ieLen);
	pMac->lim.beacon_probe_rsp_cnt_per_scan++;

	return;
} /*** end lim_collect_bss_description() ***/

/**
 * lim_check_and_add_bss_description()
 * @mac_ctx: Pointer to Global MAC structure
 * @bpr: Pointer to parsed Beacon/Probe Response structure
 * @rx_packet_info: Pointer to Received frame's BD
 * @scanning: bool to indicate whether the BSS is from current scan
 *             or just happens to receive a beacon
 *
 * FUNCTION:
 * This function is called during scan upon receiving
 * Beacon/Probe Response frame to check if the received
 * frame matches scan criteria, collect BSS description
 * and add it to cached scan results.
 *
 * Return: None
 */

void
lim_check_and_add_bss_description(tpAniSirGlobal mac_ctx,
	tpSirProbeRespBeacon bpr, uint8_t *rx_packet_info,
	bool scanning, uint8_t fProbeRsp)
{
	tSirBssDescription *bssdescr = NULL;
	uint32_t frame_len, ie_len = 0;
	uint8_t rx_chan_in_beacon = 0;
	QDF_STATUS status;
	uint8_t rf_band = 0;
	uint8_t rx_chan_bd = 0;
	uint32_t flags = 0;
	bool drop_bcn_prb_rsp = true;
	uint8_t freq_diff = 0;

	tSirMacAddr bssid_zero =  {0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
	tpSirMacDataHdr3a hdr;

	hdr = WMA_GET_RX_MPDUHEADER3A((uint8_t *) rx_packet_info);

	/*  Check For Null BSSID and Skip in case of P2P */
	if (!qdf_mem_cmp(bssid_zero, &hdr->addr3, 6))
		return;

	/*
	 * SSID/BSSID policy:
	 * Accept beacons/probe responses with any SSID or BSSID because
	 * multiple scan requests may be running at the same time and
	 * firmware may forward results to CLD from scans requested through a
	 * different path.
	 *
	 * CSA Policy:
	 * There is no point in caching & reporting the scan results for APs
	 * which are in the process of switching the channel. So, we are not
	 * caching the scan results for APs which are adverzing the
	 * channel-switch element in their beacons and probe responses.
	 */
	if (bpr->channelSwitchPresent)
		return;

	/*
	 * If beacon/probe resp DS param channel does not match with
	 * RX BD channel then don't save the results. It might be a beacon
	 * from another channel heard as noise on the current scanning channel
	 */

	if ((bpr->dsParamsPresent) || (bpr->HTInfo.present)) {
		/* This means that we are in 2.4GHz mode or 5GHz 11n mode */
		rx_chan_in_beacon = lim_get_channel_from_beacon(mac_ctx, bpr);
		rf_band = WMA_GET_RX_RFBAND(rx_packet_info);
		rx_chan_bd = WMA_GET_RX_CH(rx_packet_info);

		if (rx_chan_bd != rx_chan_in_beacon) {
			if (mac_ctx->allow_adj_ch_bcn) {
				freq_diff = abs(cds_chan_to_freq(rx_chan_bd) -
						cds_chan_to_freq(
							rx_chan_in_beacon));
				if (freq_diff <= 10)
					drop_bcn_prb_rsp = false;
			}
			/* Drop beacon, if CH do not match, Drop */
			if (!fProbeRsp && drop_bcn_prb_rsp) {
				pe_debug("Beacon Rsp dropped. Channel in BD: %d Channel in beacon: %d",
					rx_chan_bd, rx_chan_in_beacon);
				return;
			}
			/* Probe RSP, do not drop */
			else {
				if (!mac_ctx->allow_adj_ch_bcn)
					flags |= WLAN_SKIP_RSSI_UPDATE;
				pe_debug("SSID: %s CH in ProbeRsp: %d CH in BD: %d mismatch Do Not Drop",
					bpr->ssId.ssId, rx_chan_in_beacon,
					WMA_GET_RX_CH(rx_packet_info));
				WMA_GET_RX_CH(rx_packet_info) =
					rx_chan_in_beacon;
			}
		}
	}

	/*
	 * Allocate buffer to hold BSS description from
	 * received Beacon frame.
	 * Include size of fixed fields and IEs length
	 */

	ie_len = WMA_GET_RX_PAYLOAD_LEN(rx_packet_info);
	if (ie_len <= SIR_MAC_B_PR_SSID_OFFSET) {
		pe_err("RX packet has invalid length: %d", ie_len);
		return;
	}

	ie_len -= SIR_MAC_B_PR_SSID_OFFSET;

	/* IEs will be overlap ieFields field. Adjust the length accordingly */
	frame_len = sizeof(*bssdescr) + ie_len - sizeof(bssdescr->ieFields[1]);
	bssdescr = (tSirBssDescription *) qdf_mem_malloc(frame_len);

	if (NULL == bssdescr) {
		/* Log error */
		pe_err("qdf_mem_malloc(length: %d) failed", frame_len);
		return;
	}

	/* In scan state, store scan result. */
	lim_collect_bss_description(mac_ctx, bssdescr, bpr, rx_packet_info,
				    scanning);
	bssdescr->fProbeRsp = fProbeRsp;

	/*
	 * Send the beacon to CSR with registered callback routine.
	 * scan_id and flags parameters are currently unused and set to 0.
	 */
	if (mac_ctx->lim.add_bssdescr_callback) {
		(mac_ctx->lim.add_bssdescr_callback) (mac_ctx,
			bssdescr, 0, flags);
	} else {
		pe_warn("No CSR callback routine to send beacons");
		status = QDF_STATUS_E_INVAL;
	}
	qdf_mem_free(bssdescr);
}

