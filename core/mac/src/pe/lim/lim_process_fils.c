/*
 * Copyright (c) 2017-2018 The Linux Foundation. All rights reserved.
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

#include "lim_process_fils.h"
#include <lim_send_messages.h>
#include <lim_types.h>
#include <lim_utils.h>
#include <lim_prop_exts_utils.h>
#include <lim_assoc_utils.h>
#include <lim_session.h>
#include <cds_ieee80211_defines.h>
#include <qdf_crypto.h>

#ifdef WLAN_FEATURE_FILS_SK

#ifdef WLAN_FILS_DEBUG
/**
 * lim_fils_data_dump()- dump fils data
 * @type: Data name
 * @data: pointer to data buffer
 * @len: data len
 *
 * Return: None
 */
static void lim_fils_data_dump(char *type, uint8_t *data, uint32_t len)
{

	QDF_TRACE(QDF_MODULE_ID_PE, QDF_TRACE_LEVEL_DEBUG,
		 ("%s : length %d"), type, len);
	qdf_trace_hex_dump(QDF_MODULE_ID_PE, QDF_TRACE_LEVEL_INFO, data, len);
}
#else
static void lim_fils_data_dump(char *type, uint8_t *data, uint32_t len)
{ }
#endif

/**
 * lim_get_crypto_digest_len()- Returns hash length based on crypto type
 * @type: Crypto type
 *
 * Return: hash length
 */
static int lim_get_crypto_digest_len(uint8_t *type)
{
	if (!strcmp(type, HMAC_SHA386_CRYPTO_TYPE))
		return SHA384_DIGEST_SIZE;
	else if (!strcmp(type, HMAC_SHA256_CRYPTO_TYPE))
		return SHA256_DIGEST_SIZE;
	return -EINVAL;
}

/**
 * lim_get_auth_tag_len()- This API returns auth tag len based on crypto suit
 * used to encrypt erp packet.
 * @crypto_suite: Crtpto suit
 *
 * Return: tag length
 */
static uint8_t lim_get_auth_tag_len(enum fils_erp_cryptosuite crypto_suite)
{
	switch (crypto_suite) {
	case HMAC_SHA256_64:
		return -EINVAL;
	case HMAC_SHA256_128:
		return FILS_SHA256_128_AUTH_TAG;
	case HMAC_SHA256_256:
		return FILS_SHA256_256_AUTH_TAG;
	default:
		return -EINVAL;
	}
}


/**
 * lim_get_crypto_type()- This API returns crypto type based on akm suite used
 * @akm: akm used for authentication
 *
 * Return: Crypto type
 */
static uint8_t *lim_get_crypto_type(uint8_t akm)
{
	switch (akm) {
	case eCSR_AUTH_TYPE_FILS_SHA384:
	case eCSR_AUTH_TYPE_FT_FILS_SHA384:
		return FILS_SHA384_CRYPTO_TYPE;
	case eCSR_AUTH_TYPE_FILS_SHA256:
	case eCSR_AUTH_TYPE_FT_FILS_SHA256:
	default:
		return FILS_SHA256_CRYPTO_TYPE;
	}
}

/**
 * lim_get_pmk_length()- This API returns pmk length based on akm used
 * @akm: akm used for authentication
 *
 * Return: PMK length
 */
static uint8_t lim_get_pmk_length(int akm_type)
{
	switch (akm_type) {
	case eCSR_AUTH_TYPE_FILS_SHA256:
	case eCSR_AUTH_TYPE_FT_FILS_SHA256:
		return FILS_SHA256_PKM_LEN;
	case eCSR_AUTH_TYPE_FILS_SHA384:
	case eCSR_AUTH_TYPE_FT_FILS_SHA384:
		return FILS_SHA384_PKM_LEN;
	default:
		return FILS_SHA256_PKM_LEN;
	}
}

/**
 * lim_get_kek_len()- This API returns kek length based on akm used
 * @akm: akm used for authentication
 *
 * Return: KEK length
 */
static uint8_t lim_get_kek_len(uint8_t akm)
{
	switch (akm) {
	case eCSR_AUTH_TYPE_FILS_SHA384:
	case eCSR_AUTH_TYPE_FT_FILS_SHA384:
		return FILS_SHA384_KEK_LEN;
	case eCSR_AUTH_TYPE_FILS_SHA256:
	case eCSR_AUTH_TYPE_FT_FILS_SHA256:
		return FILS_SHA256_KEK_LEN;
	default:
		return FILS_SHA256_KEK_LEN;
	}
}

/**
 * lim_get_tk_len()- This API returns tk length based on cypher used
 * @akm: cypher used
 *
 * Return: TK length
 */
static uint8_t lim_get_tk_len(int cypher_suite)
{
	switch (cypher_suite) {
	case eSIR_ED_TKIP:
		return TK_LEN_TKIP;
	case eSIR_ED_CCMP:
		return TK_LEN_CCMP;
	case eSIR_ED_GCMP:
		return TK_LEN_GCMP;
	case eSIR_ED_GCMP_256:
		return TK_LEN_GCMP_256;
	case eSIR_ED_AES_128_CMAC:
		return TK_LEN_AES_128_CMAC;
	default:
		return 0;
	}
}

/**
 * lim_get_ick_len()- This API returns ick length based on akm used
 * @akm: akm used for authentication
 *
 * Return: ICK length
 */
static int lim_get_ick_len(uint8_t akm)
{
	switch (akm) {
	case eCSR_AUTH_TYPE_FILS_SHA384:
	case eCSR_AUTH_TYPE_FT_FILS_SHA384:
		return FILS_SHA384_ICK_LEN;
	case eCSR_AUTH_TYPE_FILS_SHA256:
	case eCSR_AUTH_TYPE_FT_FILS_SHA256:
	default:
		return FILS_SHA256_ICK_LEN;
	}
}

/**
 * lim_get_key_from_prf()- This API returns key data using PRF-X as defined in
 * 11.6.1.7.2 ieee-80211-2012.
 * @type: crypto type needs to be used
 * @secret: key which needs to be used in crypto
 * @secret_len: key_len of secret
 * @label: PRF label
 * @optional_data: Data used for hash
 * @optional_data_len: data length
 * @key: key data output
 * @keylen: key data length
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS lim_get_key_from_prf(uint8_t *type, uint8_t *secret,
		uint32_t secret_len, uint8_t *label, uint8_t *optional_data,
		uint32_t optional_data_len, uint8_t *key, uint32_t keylen)
{
	uint8_t count[2];
	uint8_t *addr[4];
	uint32_t len[4];
	uint16_t key_bit_length = keylen * 8;
	uint8_t key_length[2];
	uint32_t i = 0, remain_len;
	uint16_t interation;
	uint8_t crypto_digest_len = lim_get_crypto_digest_len(type);
	uint8_t tmp_hash[SHA384_DIGEST_SIZE] = {0};

	addr[0] = count;
	len[0] = sizeof(count);

	addr[1] = label;
	len[1] = strlen(label);

	addr[2] = optional_data;
	len[2] = optional_data_len;

	qdf_mem_copy(key_length, &key_bit_length, sizeof(key_bit_length));
	addr[3] = key_length;
	len[3] = sizeof(key_length);

	for (interation = 1; i < keylen; interation++) {
		remain_len = keylen - i;
		qdf_mem_copy(count, &interation, sizeof(interation));

		if (remain_len >= crypto_digest_len)
			remain_len = crypto_digest_len;

		if (qdf_get_hmac_hash(type, secret, secret_len, 4,
				addr, len, tmp_hash) < 0) {
			pe_err("qdf_get_hmac_hash failed");
			return QDF_STATUS_E_FAILURE;
		}
		qdf_mem_copy(&key[i], tmp_hash, remain_len);
		i += crypto_digest_len;
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * lim_default_hmac_sha256_kdf()- This API calculates key data using default kdf
 * defined in RFC4306.
 * @secret: key which needs to be used in crypto
 * @secret_len: key_len of secret
 * @label: PRF label
 * @optional_data: Data used for hash
 * @optional_data_len: data length
 * @key: key data output
 * @keylen: key data length
 *
 * This API creates default KDF as defined in RFC4306
 * PRF+ (K,S) = T1 | T2 | T3 | T4 | ...
 * T1 = PRF (K, S | 0x01)
 * T2 = PRF (K, T1 | S | 0x02)
 * T3 = PRF (K, T2 | S | 0x03)
 * T4 = PRF (K, T3 | S | 0x04)
 *
 * for every iteration its creates 32 bit of hash
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS
lim_default_hmac_sha256_kdf(uint8_t *secret, uint32_t secret_len,
		uint8_t *label, uint8_t *optional_data,
		uint32_t optional_data_len, uint8_t *key, uint32_t keylen)
{
	uint8_t tmp_hash[SHA256_DIGEST_SIZE] = {0};
	uint8_t count = 1;
	uint8_t *addr[4];
	uint32_t len[4];
	uint32_t current_position = 0, remaining_data = SHA256_DIGEST_SIZE;

	addr[0] = tmp_hash;
	len[0] = SHA256_DIGEST_SIZE;
	addr[1] = label;
	len[1] = strlen(label) + 1;
	addr[2] = optional_data;
	len[2] = optional_data_len;
	addr[3] = &count;
	len[3] = 1;

	if (keylen == 0 ||
	   (keylen > (MAX_PRF_INTERATIONS_COUNT * SHA256_DIGEST_SIZE))) {
		pe_err("invalid key length %d", keylen);
		return QDF_STATUS_E_FAILURE;
	}

	/* Create T1 */
	if (qdf_get_hmac_hash(FILS_SHA256_CRYPTO_TYPE, secret, secret_len, 3,
			&addr[1], &len[1], tmp_hash) < 0) {
		pe_err("failed to get hmac hash");
		return QDF_STATUS_E_FAILURE;
	}

	/* Update hash from tmp_hash */
	qdf_mem_copy(key + current_position, tmp_hash, remaining_data);
	current_position += remaining_data;

	for (count = 2; current_position < keylen; count++) {
		remaining_data = keylen - current_position;
		if (remaining_data > SHA256_DIGEST_SIZE)
			remaining_data = SHA256_DIGEST_SIZE;

		/* Create T-n */
		if (qdf_get_hmac_hash(FILS_SHA256_CRYPTO_TYPE, secret,
				secret_len, 4, addr, len, tmp_hash) < 0) {
			pe_err("failed to get hmac hash");
			return QDF_STATUS_E_FAILURE;
		}
		/* Update hash from tmp_hash */
		qdf_mem_copy(key + current_position, tmp_hash, remaining_data);
		current_position += remaining_data;
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * lim_process_fils_eap_tlv()- This API process eap tlv available in auth resp
 * and returns remaining length.
 * @pe_session: PE session
 * @wrapped_data: wrapped data
 * @data_len: length of tlv
 *
 * Return: remaining length
 */
static uint32_t lim_process_fils_eap_tlv(tpPESession pe_session,
				uint8_t *wrapped_data, uint32_t data_len)
{
	struct fils_eap_tlv *tlv;
	struct fils_auth_rsp_info *auth_info;
	uint8_t auth_tag_len;

	auth_info = &pe_session->fils_info->auth_info;
	/* Minimum */
	auth_tag_len = lim_get_auth_tag_len(HMAC_SHA256_128);

	while (data_len > (auth_tag_len + 1)) {
		tlv = (struct fils_eap_tlv *) wrapped_data;

		pe_debug("tlv type %x len %u total %u",
			tlv->type, tlv->length, data_len);
		switch (tlv->type) {
		case SIR_FILS_EAP_TLV_KEYNAME_NAI:
			auth_info->keyname = qdf_mem_malloc(tlv->length);
			if (!auth_info->keyname) {
				pe_err("failed to alloc memory");
				return 0;
			}
			qdf_mem_copy(auth_info->keyname,
				     tlv->data, tlv->length);
			auth_info->keylength = tlv->length;
			data_len -= (tlv->length + 2);
			wrapped_data += (tlv->length + 2);
			break;
		case SIR_FILS_EAP_TLV_R_RK_LIFETIME:
			/* TODO check this */
			auth_info->r_rk_lifetime = lim_get_u32(tlv->data);
			data_len -= (tlv->length + 2);
			wrapped_data += (tlv->length + 2);
			break;
		case SIR_FILS_EAP_TLV_R_MSK_LIFETIME:
			/* TODO check this */
			auth_info->r_msk_lifetime = lim_get_u32(tlv->data);
			data_len -= (tlv->length + 2);
			wrapped_data += (tlv->length + 2);
			break;
		case SIR_FILS_EAP_TLV_DOMAIN_NAME:
			auth_info->domain_name = qdf_mem_malloc(tlv->length);
			if (!auth_info->domain_name) {
				pe_err("failed to alloc memory");
				return 0;
			}
			qdf_mem_copy(auth_info->domain_name,
				     tlv->data, tlv->length);
			auth_info->domain_len = tlv->length;
			data_len -= (tlv->length + 2);
			wrapped_data += (tlv->length + 2);
			break;
			/* TODO process these now */
		case SIR_FILS_EAP_TLV_CRYPTO_LIST:
		case SIR_FILS_EAP_TLV_AUTH_INDICATION:
			data_len -= (tlv->length + 2);
			wrapped_data += (tlv->length + 2);
			break;
		default:
			pe_debug("Unknown type");
			return data_len;
		}
	}
	return data_len;
}

/**
 * lim_generate_key_data()- This API generates key data using prf
 * FILS-Key-Data = KDF-X(PMK, "FILS PTK Derivation", SPA||AA||SNonce||ANonce)
 * @fils_info: fils session info
 * @key_label: label used
 * @data: data buffer
 * @data_len: data buffer len
 * @key_data: hash data needs to be generated
 * @key_data_len: hash data len
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS lim_generate_key_data(struct pe_fils_session *fils_info,
			uint8_t *key_label, uint8_t *data, uint32_t data_len,
			uint8_t *key_data, uint32_t key_data_len)
{
	QDF_STATUS status;

	if (!fils_info)
		return QDF_STATUS_E_FAILURE;

	status = lim_get_key_from_prf(lim_get_crypto_type(fils_info->akm),
			fils_info->fils_pmk,
			fils_info->fils_pmk_len,
			key_label, data, data_len, key_data, key_data_len);
	if (status != QDF_STATUS_SUCCESS)
		pe_err("failed to generate keydata");
	return status;
}

/**
 * lim_generate_ap_key_auth()- This API generates ap auth data which needs to be
 * verified in assoc response.
 * @pe_session: pe session pointer
 *
 * Return: None
 */
static void lim_generate_ap_key_auth(tpPESession pe_session)
{
	uint8_t *buf, *addr[1];
	uint32_t len;
	struct pe_fils_session *fils_info = pe_session->fils_info;
	uint8_t data[SIR_FILS_NONCE_LENGTH + SIR_FILS_NONCE_LENGTH
			+ QDF_MAC_ADDR_SIZE + QDF_MAC_ADDR_SIZE] = {0};

	if (!fils_info)
		return;

	len = SIR_FILS_NONCE_LENGTH + SIR_FILS_NONCE_LENGTH +
			QDF_MAC_ADDR_SIZE +  QDF_MAC_ADDR_SIZE;
	addr[0] = data;
	buf = data;
	qdf_mem_copy(buf, fils_info->auth_info.fils_nonce,
			SIR_FILS_NONCE_LENGTH);
	buf += SIR_FILS_NONCE_LENGTH;
	qdf_mem_copy(buf, fils_info->fils_nonce, SIR_FILS_NONCE_LENGTH);
	buf += SIR_FILS_NONCE_LENGTH;
	qdf_mem_copy(buf, pe_session->bssId, QDF_MAC_ADDR_SIZE);
	buf += QDF_MAC_ADDR_SIZE;
	qdf_mem_copy(buf, pe_session->selfMacAddr, QDF_MAC_ADDR_SIZE);
	buf += QDF_MAC_ADDR_SIZE;

	if (qdf_get_hmac_hash(lim_get_crypto_type(fils_info->akm),
				fils_info->ick, fils_info->ick_len, 1, &addr[0],
				&len, fils_info->ap_key_auth_data) < 0)
		pe_err("failed to generate PMK id");
	fils_info->ap_key_auth_len = lim_get_crypto_digest_len(
					lim_get_crypto_type(fils_info->akm));
	lim_fils_data_dump("AP Key Auth", fils_info->ap_key_auth_data,
		fils_info->ap_key_auth_len);
}

/**
 * lim_generate_key_auth()- This API generates sta auth data which needs to be
 * send to AP in assoc request, AP will generate the same the verify it.
 * @pe_session: pe session pointer
 *
 * Return: None
 */
static void lim_generate_key_auth(tpPESession pe_session)
{
	uint8_t *buf, *addr[1];
	uint32_t len;
	struct pe_fils_session *fils_info = pe_session->fils_info;
	uint8_t data[SIR_FILS_NONCE_LENGTH + SIR_FILS_NONCE_LENGTH
			+ QDF_MAC_ADDR_SIZE + QDF_MAC_ADDR_SIZE] = {0};

	if (!fils_info)
		return;

	len = SIR_FILS_NONCE_LENGTH + SIR_FILS_NONCE_LENGTH +
			QDF_MAC_ADDR_SIZE +  QDF_MAC_ADDR_SIZE;

	addr[0] = data;
	buf = data;
	qdf_mem_copy(buf, fils_info->fils_nonce, SIR_FILS_NONCE_LENGTH);
	buf += SIR_FILS_NONCE_LENGTH;
	qdf_mem_copy(buf, fils_info->auth_info.fils_nonce,
			SIR_FILS_NONCE_LENGTH);
	buf += SIR_FILS_NONCE_LENGTH;
	qdf_mem_copy(buf, pe_session->selfMacAddr, QDF_MAC_ADDR_SIZE);
	buf += QDF_MAC_ADDR_SIZE;
	qdf_mem_copy(buf, pe_session->bssId, QDF_MAC_ADDR_SIZE);
	buf += QDF_MAC_ADDR_SIZE;

	if (qdf_get_hmac_hash(lim_get_crypto_type(fils_info->akm),
				fils_info->ick, fils_info->ick_len, 1,
				&addr[0], &len, fils_info->key_auth) < 0)
		pe_err("failed to generate key auth");
	fils_info->key_auth_len = lim_get_crypto_digest_len(
				lim_get_crypto_type(fils_info->akm));
	lim_fils_data_dump("STA Key Auth",
			fils_info->key_auth, fils_info->key_auth_len);
}

/**
 * lim_get_keys()- This API generates keys keydata which is generated after
 * parsing of auth response.
 * KCK = L(FILS-Key-Data, 0, KCK_bits)
 * KEK = L(FILS-Key-Data, KCK_bits, KEK_bits)
 * TK = L(FILS-Key-Data, KCK_bits + KEK_bits, TK_bits)
 * FILS-FT = L(FILS-Key-Data, KCK_bits + KEK_bits + TK_bits, FILS-FT_bits)
 * @pe_session: pe session pointer
 *
 * Return: None
 */
static void lim_get_keys(tpPESession pe_session)
{
	uint8_t key_label[] = PTK_KEY_LABEL;
	uint8_t *data;
	uint8_t data_len;
	struct pe_fils_session *fils_info = pe_session->fils_info;
	uint8_t key_data[MAX_ICK_LEN + MAX_KEK_LEN + MAX_TK_LEN] = {0};
	uint8_t key_data_len;
	uint8_t ick_len;
	uint8_t kek_len;
	uint8_t tk_len = lim_get_tk_len(pe_session->encryptType);
	uint8_t *buf;

	if (!fils_info)
		return;

	ick_len = lim_get_ick_len(fils_info->akm);
	kek_len = lim_get_kek_len(fils_info->akm);

	key_data_len = ick_len + kek_len + tk_len;

	data_len = 2 * SIR_FILS_NONCE_LENGTH + 2 * QDF_MAC_ADDR_SIZE;
	data = qdf_mem_malloc(data_len);
	if (!data) {
		pe_err("failed to alloc memory");
		return;
	}

	/* Update data */
	buf = data;
	qdf_mem_copy(buf, pe_session->selfMacAddr, QDF_MAC_ADDR_SIZE);
	buf += QDF_MAC_ADDR_SIZE;
	qdf_mem_copy(buf, pe_session->bssId, QDF_MAC_ADDR_SIZE);
	buf += QDF_MAC_ADDR_SIZE;
	qdf_mem_copy(buf, fils_info->fils_nonce, SIR_FILS_NONCE_LENGTH);
	buf += SIR_FILS_NONCE_LENGTH;
	qdf_mem_copy(buf, fils_info->auth_info.fils_nonce,
			SIR_FILS_NONCE_LENGTH);
	lim_generate_key_data(fils_info, key_label, data, data_len,
				key_data, key_data_len);
	buf = key_data;
	qdf_mem_copy(fils_info->ick, buf, ick_len);
	fils_info->ick_len = ick_len;
	buf += ick_len;
	qdf_mem_copy(fils_info->kek, buf, kek_len);
	fils_info->kek_len = kek_len;
	buf += kek_len;
	qdf_mem_copy(fils_info->tk, buf, tk_len);
	fils_info->tk_len = tk_len;
	qdf_mem_free(data);
}

/**
 * lim_generate_pmkid()- This API generates PMKID using hash of erp auth packet
 * parsing of auth response.
 * PMKID = Truncate-128(Hash(EAP-Initiate/Reauth))
 * @pe_session: pe session pointer
 *
 * Return: None
 */
static void lim_generate_pmkid(tpPESession pe_session)
{
	uint8_t hash[SHA384_DIGEST_SIZE];
	struct pe_fils_session *fils_info = pe_session->fils_info;

	if (!fils_info)
		return;

	qdf_get_hash(lim_get_crypto_type(fils_info->akm), 1,
		&fils_info->fils_erp_reauth_pkt,
		&fils_info->fils_erp_reauth_pkt_len, hash);
	qdf_mem_copy(fils_info->fils_pmkid, hash, PMKID_LEN);
	lim_fils_data_dump("PMKID", fils_info->fils_pmkid, PMKID_LEN);
}

/**
 * lim_generate_pmk()- This API generates PMK using hmac hash of rmsk data
 * anonce, snonce will be used as key for this
 * PMK = HMAC-Hash(SNonce || ANonce, rMSK [ || DHss ])
 * @pe_session: pe session pointer
 *
 * Return: None
 */
static void lim_generate_pmk(tpPESession pe_session)
{
	uint8_t nounce[2 * SIR_FILS_NONCE_LENGTH] = {0};
	uint8_t nounce_len = 2 * SIR_FILS_NONCE_LENGTH;
	uint8_t *addr[1];
	uint32_t len[1];
	struct pe_fils_session *fils_info = pe_session->fils_info;

	/* Snounce */
	qdf_mem_copy(nounce, fils_info->fils_nonce,
			SIR_FILS_NONCE_LENGTH);
	/* anounce */
	qdf_mem_copy(nounce + SIR_FILS_NONCE_LENGTH,
			fils_info->auth_info.fils_nonce,
			SIR_FILS_NONCE_LENGTH);
	fils_info->fils_pmk_len = lim_get_pmk_length(fils_info->akm);

	if (fils_info->fils_pmk)
		qdf_mem_free(fils_info->fils_pmk);

	fils_info->fils_pmk = qdf_mem_malloc(fils_info->fils_pmk_len);
	if (!fils_info->fils_pmk) {
		pe_err("failed to alloc memory");
		return;
	}

	addr[0] = fils_info->fils_rmsk;
	len[0] = fils_info->fils_rmsk_len;
	lim_fils_data_dump("Nonce", nounce, nounce_len);
	if (qdf_get_hmac_hash(lim_get_crypto_type(fils_info->akm), nounce,
				nounce_len, 1,
				&addr[0], &len[0], fils_info->fils_pmk) < 0)
		pe_err("failed to generate PMK");
}

/**
 * lim_generate_rmsk_data()- This API generates RMSK data using
 * default kdf as defined in RFC4306.
 * @pe_session: pe session pointer
 *
 * Return: None
 */
static void lim_generate_rmsk_data(tpPESession pe_session)
{
	uint8_t optional_data[4] = {0};
	uint8_t rmsk_label[] = RMSK_LABEL;
	struct pe_fils_session *fils_info = pe_session->fils_info;
	struct fils_auth_rsp_info *auth_info;

	if (!fils_info)
		return;

	auth_info = &(pe_session->fils_info->auth_info);
	fils_info->fils_rmsk_len = fils_info->fils_rrk_len;
	fils_info->fils_rmsk = qdf_mem_malloc(fils_info->fils_rrk_len);
	if (!fils_info->fils_rmsk) {
		pe_err("failed to alloc memory");
		return;
	}

	/*
	 * Sequence number sent in EAP-INIT packet,
	 * it should be in network byte order
	 */
	lim_copy_u16_be(&optional_data[0], fils_info->sequence_number);
	lim_copy_u16_be(&optional_data[2], fils_info->fils_rrk_len);
	lim_default_hmac_sha256_kdf(fils_info->fils_rrk,
			fils_info->fils_rrk_len, rmsk_label,
			optional_data, sizeof(optional_data),
			fils_info->fils_rmsk, fils_info->fils_rmsk_len);
}

/**
 * lim_process_auth_wrapped_data()- This API process wrapped data element
 * of auth response.
 * @pe_session: pe session pointer
 * @wrapped_data: wrapped data pointer
 * @data_len: wrapped data len
 *
 * Return: None
 */
static QDF_STATUS lim_process_auth_wrapped_data(tpPESession pe_session,
			uint8_t *wrapped_data, uint32_t data_len)
{
	uint8_t code;
	uint8_t identifier;
	uint16_t length;
	uint8_t type;
	unsigned long flags;
	struct pe_fils_session *fils_info;
	uint8_t hash[32] = {0}, crypto;
	uint32_t remaining_len = data_len, new_len;
	uint8_t *input_data[1];
	uint32_t input_len[1];
	uint8_t auth_tag_len;

	fils_info = pe_session->fils_info;
	if (!fils_info)
		return QDF_STATUS_E_FAILURE;

	input_data[0] = wrapped_data;
	input_len[0] = data_len;

	pe_debug("trying to process the wrappped data");

	code = *wrapped_data;
	wrapped_data++;
	remaining_len--;
	identifier = *wrapped_data;
	wrapped_data++;
	remaining_len--;

	length = lim_get_u16(wrapped_data);
	wrapped_data += sizeof(uint16_t);
	remaining_len -= sizeof(uint16_t);

	type = *wrapped_data; /* val should be 2 here */
	wrapped_data++;
	remaining_len--;

	flags = *wrapped_data;
	if (test_bit(7, &flags)) {
		pe_err("R bit is set in flag, error");
		return QDF_STATUS_E_FAILURE;
	}
	wrapped_data++;
	remaining_len--;

	fils_info->auth_info.sequence = lim_get_u16_be(wrapped_data);
	wrapped_data += sizeof(uint16_t);
	remaining_len -= sizeof(uint16_t);
	/* Validate Auth sequence number */
	if (fils_info->auth_info.sequence < fils_info->sequence_number) {
		pe_err("sequence EAP-finish:%d is less than EAP-init:%d",
			fils_info->auth_info.sequence,
			fils_info->sequence_number);
		return QDF_STATUS_E_FAILURE;
	}

	/* Parse attached TLVs */
	new_len = lim_process_fils_eap_tlv(pe_session,
				wrapped_data, remaining_len);

	wrapped_data += remaining_len - new_len;
	remaining_len = new_len;
	/* Remove cryptosuite */
	crypto = *wrapped_data;
	wrapped_data++;
	remaining_len--;

	auth_tag_len = lim_get_auth_tag_len(crypto);
	input_len[0] -= auth_tag_len;
	/* if we have auth tag remaining */
	if (remaining_len == auth_tag_len) {
		qdf_get_hmac_hash(FILS_SHA256_CRYPTO_TYPE,
				fils_info->fils_rik,
				fils_info->fils_rik_len,
				SINGLE_ELEMENT_HASH_CNT,
				input_data, input_len, hash);
	} else {
		pe_err("invalid remaining len %d",
			remaining_len);
	}
	if (qdf_mem_cmp(wrapped_data, hash, auth_tag_len)) {
		pe_err("integratity check failed for auth, crypto %d",
			crypto);
		return QDF_STATUS_E_FAILURE;
	}

	lim_generate_rmsk_data(pe_session);
	lim_generate_pmk(pe_session);
	lim_generate_pmkid(pe_session);
	return QDF_STATUS_SUCCESS;
}

/**
 * lim_is_valid_fils_auth_frame()- This API check whether auth frame is a
 * valid frame.
 * @mac_ctx: mac context
 * @pe_session: pe session pointer
 * @rx_auth_frm_body: pointer to autherntication frame
 *
 * Return: true if frame is valid or fils is disable, false otherwise
 */
bool lim_is_valid_fils_auth_frame(tpAniSirGlobal mac_ctx,
		tpPESession pe_session,
		tSirMacAuthFrameBody *rx_auth_frm_body)
{
	if (!pe_session->fils_info)
		return true;

	if (pe_session->fils_info->is_fils_connection == false)
		return true;

	if (qdf_mem_cmp(rx_auth_frm_body->session,
				pe_session->fils_info->fils_session,
				SIR_FILS_SESSION_LENGTH)) {
		lim_fils_data_dump("Current FILS session",
				pe_session->fils_info->fils_session,
				SIR_FILS_SESSION_LENGTH);
		lim_fils_data_dump("FILS Session in pkt",
				rx_auth_frm_body->session,
				SIR_FILS_SESSION_LENGTH);
		return false;
	}
	qdf_mem_copy(pe_session->fils_info->auth_info.fils_nonce,
			rx_auth_frm_body->nonce, SIR_FILS_NONCE_LENGTH);
	pe_session->fils_info->auth_info.assoc_delay =
			rx_auth_frm_body->assoc_delay_info;
	return true;
}

QDF_STATUS lim_create_fils_rik(uint8_t *rrk, uint8_t rrk_len,
					uint8_t *rik, uint32_t *rik_len)
{
	uint8_t optional_data[SIR_FILS_OPTIONAL_DATA_LEN];
	uint8_t label[] = SIR_FILS_RIK_LABEL;

	if (!rrk || !rik)
		return QDF_STATUS_E_FAILURE;

	optional_data[0] = HMAC_SHA256_128;
	/* basic validation */
	if (rrk_len <= 0) {
		pe_err("invalid r_rk length %d", rrk_len);
		return QDF_STATUS_E_FAILURE;
	}
	lim_copy_u16_be(&optional_data[1], rrk_len);
	if (lim_default_hmac_sha256_kdf(rrk, rrk_len, label,
				optional_data, sizeof(optional_data),
				rik, rrk_len)
			!= QDF_STATUS_SUCCESS) {
		pe_err("failed to create rik");
		return QDF_STATUS_E_FAILURE;
	}
	*rik_len = rrk_len;

	return QDF_STATUS_SUCCESS;
}

/**
 * lim_create_fils_wrapper_data()- This API create warpped data which will be
 * sent in auth request.
 * @fils_info: fils session info
 *
 * Return: length of the created wrapped data
 */
static int lim_create_fils_wrapper_data(struct pe_fils_session *fils_info)
{
	uint8_t *buf;
	uint8_t auth_tag[FILS_AUTH_TAG_MAX_LENGTH] = {0};
	uint32_t length = 0;
	QDF_STATUS status;
	int buf_len;

	if (!fils_info)
		return 0;

	buf_len =
		/* code + identifier */
		sizeof(uint8_t) * 2 +
		/* length */
		sizeof(uint16_t) +
		/* type + flags */
		sizeof(uint8_t) * 2 +
		/* sequence */
		sizeof(uint16_t) +
		/* tlv : type, length, data */
		sizeof(uint8_t) * 2 + fils_info->keyname_nai_length +
		/* cryptosuite + auth_tag */
		sizeof(uint8_t) + lim_get_auth_tag_len(HMAC_SHA256_128);

	fils_info->fils_erp_reauth_pkt = qdf_mem_malloc(buf_len);
	if (!fils_info->fils_erp_reauth_pkt) {
		pe_err("failed to allocate memory");
		return -EINVAL;
	}
	buf = fils_info->fils_erp_reauth_pkt;
	*buf = 5;
	buf++;
	/* Identifier */
	*buf = 0;
	buf++;
	/* Length */
	lim_copy_u16_be(buf, buf_len);
	buf += sizeof(uint16_t);
	/* type */
	*buf = SIR_FILS_EAP_INIT_PACKET_TYPE;
	buf++;

	/**
	 *  flag
	 *  0 1 2  <-- 5 -->
	 * ----------------
	 * |R|B|L| Reserved|
	 * -----------------
	 */
	*buf = 0x20; /* l=1, b=0, r=0 */
	buf++;
	/* sequence */
	lim_copy_u16_be(buf, fils_info->sequence_number);
	buf += sizeof(uint16_t);

	/* tlv */
	/* Type */
	*buf = SIR_FILS_EAP_TLV_KEYNAME_NAI;
	buf++;
	/* NAI Length */
	*buf = fils_info->keyname_nai_length;
	buf++;
	/* NAI Data */
	qdf_mem_copy(buf, fils_info->keyname_nai_data,
			fils_info->keyname_nai_length);
	buf += fils_info->keyname_nai_length;

	/* cryptosuite */
	*buf = HMAC_SHA256_128;
	buf++;

	/*
	 * This should be moved to just after sending probe to save time
	 * lim_process_switch_channel_join_req ??
	 */
	fils_info->fils_rik = qdf_mem_malloc(fils_info->fils_rrk_len);
	if (!fils_info->fils_rik) {
		qdf_mem_free(fils_info->fils_erp_reauth_pkt);
		fils_info->fils_erp_reauth_pkt = NULL;
		pe_err("failed to alloc memory");
		return -EINVAL;
	}
	status = lim_create_fils_rik(fils_info->fils_rrk,
				     fils_info->fils_rrk_len,
				     fils_info->fils_rik,
				     &fils_info->fils_rik_len);
	if (QDF_IS_STATUS_ERROR(status)) {
		pe_err("RIK create fails");
		qdf_mem_free(fils_info->fils_erp_reauth_pkt);
		qdf_mem_free(fils_info->fils_rik);
		fils_info->fils_erp_reauth_pkt = NULL;
		fils_info->fils_rik = NULL;
		return -EINVAL;
	}

	fils_info->fils_erp_reauth_pkt_len = buf_len;
	length = fils_info->fils_erp_reauth_pkt_len -
			lim_get_auth_tag_len(HMAC_SHA256_128);
	qdf_get_hmac_hash(FILS_SHA256_CRYPTO_TYPE,
			fils_info->fils_rik, fils_info->fils_rik_len, 1,
			&fils_info->fils_erp_reauth_pkt, &length, auth_tag);

	lim_fils_data_dump("Auth tag", auth_tag,
			lim_get_auth_tag_len(HMAC_SHA256_128));
	lim_fils_data_dump("EAP init pkt", fils_info->fils_erp_reauth_pkt,
			fils_info->fils_erp_reauth_pkt_len);

	qdf_mem_copy(buf, auth_tag, lim_get_auth_tag_len(HMAC_SHA256_128));
	buf += lim_get_auth_tag_len(HMAC_SHA256_128);

	return buf_len;
}

/**
 * lim_add_fils_data_to_auth_frame()- This API add fils data to auth frame.
 * Following will be added in this.
 * RSNIE
 * SNonce
 * Session
 * Wrapped data
 * @session: PE session
 * @body: pointer to auth frame where data needs to be added
 *
 * Return: None
 */
void lim_add_fils_data_to_auth_frame(tpPESession session,
		uint8_t *body)
{
	struct pe_fils_session *fils_info;

	fils_info = session->fils_info;
	if (!fils_info)
		return;

	/* RSN IE */
	qdf_mem_copy(body, fils_info->rsn_ie, fils_info->rsn_ie_len);
	body += fils_info->rsn_ie_len;
	lim_fils_data_dump("FILS RSN", fils_info->rsn_ie,
			fils_info->rsn_ie_len);

	/* ***Nounce*** */
	/* Add element id */
	*body = SIR_MAX_ELEMENT_ID;
	body++;
	/* Add nounce length + 1 for ext element id */
	*body = SIR_FILS_NONCE_LENGTH + 1;
	body++;
	/* Add ext element */
	*body = SIR_FILS_NONCE_EXT_EID;
	body++;
	/* Add data */
	cds_rand_get_bytes(0, fils_info->fils_nonce, SIR_FILS_NONCE_LENGTH);
	qdf_mem_copy(body, fils_info->fils_nonce, SIR_FILS_NONCE_LENGTH);
	body = body + SIR_FILS_NONCE_LENGTH;
	/* Dump data */
	lim_fils_data_dump("fils anonce", fils_info->fils_nonce,
			SIR_FILS_NONCE_LENGTH);

	/*   *** Session ***  */
	/* Add element id */
	*body = SIR_MAX_ELEMENT_ID;
	body++;
	/* Add nounce length + 1 for ext element id */
	*body = SIR_FILS_SESSION_LENGTH + 1;
	body++;
	/* Add ext element */
	*body = SIR_FILS_SESSION_EXT_EID;
	body++;
	/* Add data */
	cds_rand_get_bytes(0, fils_info->fils_session, SIR_FILS_SESSION_LENGTH);
	qdf_mem_copy(body, fils_info->fils_session, SIR_FILS_SESSION_LENGTH);
	body = body + SIR_FILS_SESSION_LENGTH;
	/* dump data */
	lim_fils_data_dump("Fils Session",
		fils_info->fils_session, SIR_FILS_SESSION_LENGTH);

	/*  ERP Packet  */
	/* Add element id */
	*body = SIR_MAX_ELEMENT_ID;
	body++;
	/* Add packet length + 1 for ext element id */
	*body = fils_info->fils_erp_reauth_pkt_len + 1;
	body++;
	/* Add ext element */
	*body = SIR_FILS_WRAPPED_DATA_EXT_EID;
	body++;
	/* Copy data */
	qdf_mem_copy(body, fils_info->fils_erp_reauth_pkt,
			fils_info->fils_erp_reauth_pkt_len);
	lim_fils_data_dump("Fils ERP reauth Pkt",
			fils_info->fils_erp_reauth_pkt,
			fils_info->fils_erp_reauth_pkt_len);
	body = body + fils_info->fils_erp_reauth_pkt_len;
}

/**
 * lim_process_fils_auth_frame2()- This API process fils data from auth response
 * @mac_ctx: mac context
 * @session: PE session
 * @rx_auth_frm_body: pointer to auth frame
 *
 * Return: true if fils data needs to be processed else false
 */
bool lim_process_fils_auth_frame2(tpAniSirGlobal mac_ctx,
		tpPESession pe_session,
		tSirMacAuthFrameBody *rx_auth_frm_body)
{
	int i;
	uint32_t ret;
	bool pmkid_found = false;
	tDot11fIERSN dot11f_ie_rsn = {0};

	if (rx_auth_frm_body->authAlgoNumber != eSIR_FILS_SK_WITHOUT_PFS)
		return false;

	if (!pe_session->fils_info)
		return false;

	ret = dot11f_unpack_ie_rsn(mac_ctx,
				   &rx_auth_frm_body->rsn_ie.info[0],
				   rx_auth_frm_body->rsn_ie.length,
				   &dot11f_ie_rsn, 0);
	if (!DOT11F_SUCCEEDED(ret)) {
		pe_err("unpack failed, ret: %d", ret);
		return false;
	}

	for (i = 0; i < dot11f_ie_rsn.pmkid_count; i++) {
		if (qdf_mem_cmp(dot11f_ie_rsn.pmkid[i],
		    pe_session->fils_info->fils_pmkid,
		    PMKID_LEN) == 0) {
			pmkid_found = true;
			pe_debug("pmkid match in rsn ie total_count %d",
				dot11f_ie_rsn.pmkid_count);
			break;
		}
	}
	if (!pmkid_found) {
		if (QDF_STATUS_SUCCESS !=
		    lim_process_auth_wrapped_data(pe_session,
			rx_auth_frm_body->wrapped_data,
			rx_auth_frm_body->wrapped_data_len))
			return false;
	}
	lim_get_keys(pe_session);
	lim_generate_key_auth(pe_session);
	lim_generate_ap_key_auth(pe_session);
	return true;
}

/**
 * lim_update_fils_config()- This API update fils session info to csr config
 * from join request.
 * @session: PE session
 * @sme_join_req: pointer to join request
 *
 * Return: None
 */
void lim_update_fils_config(tpPESession session,
		tpSirSmeJoinReq sme_join_req)
{
	struct pe_fils_session *csr_fils_info;
	struct cds_fils_connection_info *fils_config_info;

	fils_config_info = &sme_join_req->fils_con_info;
	csr_fils_info = session->fils_info;
	if (!csr_fils_info)
		return;

	if (fils_config_info->is_fils_connection == false)
		return;

	csr_fils_info->is_fils_connection =
		fils_config_info->is_fils_connection;
	csr_fils_info->keyname_nai_length =
		fils_config_info->key_nai_length;
	csr_fils_info->fils_rrk_len =
		fils_config_info->r_rk_length;
	csr_fils_info->akm = fils_config_info->akm_type;
	csr_fils_info->auth = fils_config_info->auth_type;
	csr_fils_info->sequence_number = fils_config_info->sequence_number;
	if (fils_config_info->key_nai_length > FILS_MAX_KEYNAME_NAI_LENGTH) {
		pe_err("Restricting the key_nai_length of  %d to max %d",
		       fils_config_info->key_nai_length,
		       FILS_MAX_KEYNAME_NAI_LENGTH);
		fils_config_info->key_nai_length = FILS_MAX_KEYNAME_NAI_LENGTH;
	}
	csr_fils_info->keyname_nai_data =
		qdf_mem_malloc(fils_config_info->key_nai_length);
	if (!csr_fils_info->keyname_nai_data) {
		pe_err("failed to alloc memory");
		return;
	}
	qdf_mem_copy(csr_fils_info->keyname_nai_data,
			fils_config_info->keyname_nai,
			fils_config_info->key_nai_length);
	csr_fils_info->fils_rrk =
		qdf_mem_malloc(fils_config_info->r_rk_length);
	if (!csr_fils_info->fils_rrk) {
		pe_err("failed to alloc memory");
		qdf_mem_free(csr_fils_info->keyname_nai_data);
		return;
	}

	if (fils_config_info->r_rk_length <= FILS_MAX_RRK_LENGTH)
		qdf_mem_copy(csr_fils_info->fils_rrk,
				fils_config_info->r_rk,
				fils_config_info->r_rk_length);

	qdf_mem_copy(csr_fils_info->fils_pmkid,
			fils_config_info->pmkid, PMKID_LEN);
	csr_fils_info->rsn_ie_len = sme_join_req->rsnIE.length;
	qdf_mem_copy(csr_fils_info->rsn_ie,
			sme_join_req->rsnIE.rsnIEdata,
			sme_join_req->rsnIE.length);

	csr_fils_info->fils_pmk_len = fils_config_info->pmk_len;
	if (fils_config_info->pmk_len) {
		csr_fils_info->fils_pmk =
			qdf_mem_malloc(fils_config_info->pmk_len);
		if (!csr_fils_info->fils_pmk) {
			qdf_mem_free(csr_fils_info->keyname_nai_data);
			qdf_mem_free(csr_fils_info->fils_rrk);
			pe_err("failed to alloc memory");
			return;
		}
		qdf_mem_copy(csr_fils_info->fils_pmk, fils_config_info->pmk,
			fils_config_info->pmk_len);
	}
	pe_debug("fils=%d nai-len=%d rrk_len=%d akm=%d auth=%d pmk_len=%d",
		fils_config_info->is_fils_connection,
		fils_config_info->key_nai_length,
		fils_config_info->r_rk_length,
		fils_config_info->akm_type,
		fils_config_info->auth_type,
		fils_config_info->pmk_len);
}

#define EXTENDED_IE_HEADER_LEN 3
/**
 * lim_create_fils_auth_data()- This API creates the fils auth data
 * which needs to be sent in auth req.
 * @mac_ctx: mac context
 * @auth_frame: pointer to auth frame
 * @session: PE session
 *
 * Return: length of fils data
 */
uint32_t lim_create_fils_auth_data(tpAniSirGlobal mac_ctx,
		tpSirMacAuthFrameBody auth_frame,
		tpPESession session)
{
	uint32_t frame_len = 0;
	uint32_t wrapped_data_len;

	if (!session->fils_info)
		return 0;

	/* These memory may already been allocated if auth retry */
	if (session->fils_info->fils_rik) {
		qdf_mem_free(session->fils_info->fils_rik);
		session->fils_info->fils_rik = NULL;
	}
	if  (session->fils_info->fils_erp_reauth_pkt) {
		qdf_mem_free(session->fils_info->fils_erp_reauth_pkt);
		session->fils_info->fils_erp_reauth_pkt = NULL;
	}
	if (auth_frame->authAlgoNumber == eSIR_FILS_SK_WITHOUT_PFS) {
		frame_len += session->fils_info->rsn_ie_len;
		/* FILS nounce */
		frame_len += SIR_FILS_NONCE_LENGTH + EXTENDED_IE_HEADER_LEN;
		/* FILS Session */
		frame_len += SIR_FILS_SESSION_LENGTH + EXTENDED_IE_HEADER_LEN;
		/* Calculate data/length for FILS Wrapped Data */
		wrapped_data_len =
			lim_create_fils_wrapper_data(session->fils_info);
		if (wrapped_data_len < 0) {
			pe_err("failed to create warpped data");
			return 0;
		}
		frame_len += wrapped_data_len + EXTENDED_IE_HEADER_LEN;
	}
	return frame_len;
}

void populate_fils_connect_params(tpAniSirGlobal mac_ctx,
				  tpPESession session,
				  tpSirSmeJoinRsp sme_join_rsp)
{
	struct fils_join_rsp_params *fils_join_rsp;
	struct pe_fils_session *fils_info = (session->fils_info);

	if (!fils_info)
		return;

	if (!lim_is_fils_connection(session))
		return;

	if (!fils_info->fils_pmk_len ||
			!fils_info->tk_len || !fils_info->gtk_len ||
			!fils_info->fils_pmk || !fils_info->kek_len) {
		pe_err("Invalid FILS info pmk len %d kek len %d tk len %d gtk len %d",
			fils_info->fils_pmk_len,
			fils_info->kek_len,
			fils_info->tk_len,
			fils_info->gtk_len);
		return;
	}

	sme_join_rsp->fils_join_rsp = qdf_mem_malloc(sizeof(*fils_join_rsp));
	if (!sme_join_rsp->fils_join_rsp) {
		pe_err("fils_join_rsp malloc fails!");
		pe_delete_fils_info(session);
		return;
	}

	fils_join_rsp = sme_join_rsp->fils_join_rsp;
	fils_join_rsp->fils_pmk = qdf_mem_malloc(fils_info->fils_pmk_len);
	if (!fils_join_rsp->fils_pmk) {
		pe_err("fils_pmk malloc fails!");
		qdf_mem_free(fils_join_rsp);
		pe_delete_fils_info(session);
		return;
	}

	fils_join_rsp->fils_pmk_len = fils_info->fils_pmk_len;
	qdf_mem_copy(fils_join_rsp->fils_pmk, fils_info->fils_pmk,
			fils_info->fils_pmk_len);

	qdf_mem_copy(fils_join_rsp->fils_pmkid, fils_info->fils_pmkid,
			IEEE80211_PMKID_LEN);

	fils_join_rsp->kek_len = fils_info->kek_len;
	qdf_mem_copy(fils_join_rsp->kek, fils_info->kek, fils_info->kek_len);

	fils_join_rsp->tk_len = fils_info->tk_len;
	qdf_mem_copy(fils_join_rsp->tk, fils_info->tk, fils_info->tk_len);

	fils_join_rsp->gtk_len = fils_info->gtk_len;
	qdf_mem_copy(fils_join_rsp->gtk, fils_info->gtk, fils_info->gtk_len);

	cds_copy_hlp_info(&fils_info->dst_mac, &fils_info->src_mac,
			  fils_info->hlp_data_len, fils_info->hlp_data,
			  &fils_join_rsp->dst_mac, &fils_join_rsp->src_mac,
			  &fils_join_rsp->hlp_data_len,
			  fils_join_rsp->hlp_data);

	pe_debug("FILS connect params copied lim");
}

/**
 * lim_parse_kde_elements() - Parse Key Delivery Elements
 * @mac_ctx: mac context
 * @fils_info: FILS info
 * @kde_list: KDE list buffer
 * @kde_list_len: Length of @kde_list
 *
 * This API is used to parse the Key Delivery Elements from buffer
 * and populate them in PE FILS session struct i.e @fils_info
 *
 *             Key Delivery Element[KDE] format
 * +----------+--------+-----------+------------+----------+
 * | ID(0xDD) | length |  KDE OUI  |  data type |  IE data |
 * |----------|--------|-----------|------------|----------|
 * |  1 byte  | 1 byte |  3 bytes  |   1 byte   | variable |
 * +----------+--------+-----------+------------+----------+
 *
 * there can be multiple KDE present inside KDE list.
 * the IE data could be GTK, IGTK etc based on the data type
 *
 * Return: QDF_STATUS_SUCCESS if we parse GTK successfully,
 *         QDF_STATUS_E_FAILURE otherwise
 */
static QDF_STATUS lim_parse_kde_elements(tpAniSirGlobal mac_ctx,
					 struct pe_fils_session *fils_info,
					 uint8_t *kde_list,
					 uint8_t kde_list_len)
{
	uint8_t rem_len = kde_list_len;
	uint8_t *temp_ie = kde_list;
	uint8_t elem_id, data_type, data_len, *ie_data = NULL, *current_ie;
	uint16_t elem_len;

	if (!kde_list_len || !kde_list) {
		pe_err("kde_list NULL or kde_list_len %d", kde_list_len);
		return QDF_STATUS_E_FAILURE;
	}

	while (rem_len >= 2) {
		current_ie = temp_ie;
		elem_id = *temp_ie++;
		elem_len = *temp_ie++;
		rem_len -= 2;

		if (rem_len < elem_len || elem_len > kde_list_len) {
			pe_err("Invalid elem_len %d rem_len %d list_len %d",
				elem_len, rem_len, kde_list_len);
			return QDF_STATUS_E_FAILURE;
		}

		if (elem_len < KDE_IE_DATA_OFFSET) {
			pe_err("Not enough len to parse elem_len %d",
				elem_len);
			return QDF_STATUS_E_FAILURE;
		}

		if (lim_check_if_vendor_oui_match(mac_ctx, KDE_OUI_TYPE,
				KDE_OUI_TYPE_SIZE, current_ie, elem_len)) {

			data_type = *(temp_ie + KDE_DATA_TYPE_OFFSET);
			ie_data = (temp_ie + KDE_IE_DATA_OFFSET);
			data_len = (elem_len - KDE_IE_DATA_OFFSET);

			switch (data_type) {
			case DATA_TYPE_GTK:
				if (data_len < GTK_OFFSET) {
					pe_err("Invalid KDE data_len %d",
						data_len);
					return QDF_STATUS_E_FAILURE;
				}
				qdf_mem_copy(fils_info->gtk, (ie_data +
					     GTK_OFFSET), (data_len -
					     GTK_OFFSET));
				fils_info->gtk_len = (data_len - GTK_OFFSET);
				break;

			case DATA_TYPE_IGTK:
				if (data_len < IGTK_OFFSET) {
					pe_err("Invalid KDE data_len %d",
						data_len);
					return QDF_STATUS_E_FAILURE;
				}
				fils_info->igtk_len = (data_len - IGTK_OFFSET);
				qdf_mem_copy(fils_info->igtk, (ie_data +
					     IGTK_OFFSET), (data_len -
					     IGTK_OFFSET));
				qdf_mem_copy(fils_info->ipn, (ie_data +
					     IPN_OFFSET), IPN_LEN);
			break;
			default:
				pe_err("Unknown KDE data type %x", data_type);
			break;
			}
		}

		temp_ie += elem_len;
		rem_len -= elem_len;
		ie_data = NULL;
	}

	/* Expecting GTK in KDE */
	if (!fils_info->gtk_len) {
		pe_err("GTK not found in KDE");
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

bool lim_verify_fils_params_assoc_rsp(tpAniSirGlobal mac_ctx,
				      tpPESession session_entry,
				      tpSirAssocRsp assoc_rsp,
				      tLimMlmAssocCnf *assoc_cnf)
{
	struct pe_fils_session *fils_info = (session_entry->fils_info);
	tDot11fIEfils_session fils_session = assoc_rsp->fils_session;
	tDot11fIEfils_key_confirmation fils_key_auth = assoc_rsp->fils_key_auth;
	tDot11fIEfils_kde fils_kde = assoc_rsp->fils_kde;
	QDF_STATUS status;

	if (!lim_is_fils_connection(session_entry))
		return true;

	if (!fils_info) {
		pe_err("FILS Info not present");
		goto verify_fils_params_fails;
	}

	if (!assoc_rsp->fils_session.present) {
		pe_err("FILS IE not present");
		goto verify_fils_params_fails;
	}

	/* Compare FILS session */
	if (qdf_mem_cmp(fils_info->fils_session,
			fils_session.session, DOT11F_IE_FILS_SESSION_MAX_LEN)) {
		pe_err("FILS session mismatch");
		goto verify_fils_params_fails;
	}

	/* Compare FILS key auth */
	if ((fils_key_auth.num_key_auth != fils_info->key_auth_len) ||
		qdf_mem_cmp(fils_info->ap_key_auth_data, fils_key_auth.key_auth,
					 fils_info->ap_key_auth_len)) {
		lim_fils_data_dump("session keyauth",
				   fils_info->ap_key_auth_data,
				   fils_info->ap_key_auth_len);
		lim_fils_data_dump("Pkt keyauth",
				   fils_key_auth.key_auth,
				   fils_key_auth.num_key_auth);
		goto verify_fils_params_fails;
	}

	/* Verify the Key Delivery Element presence */
	if (!fils_kde.num_kde_list) {
		pe_err("FILS KDE list absent");
		goto verify_fils_params_fails;
	}

	/* Derive KDE elements */
	status = lim_parse_kde_elements(mac_ctx, fils_info, fils_kde.kde_list,
					fils_kde.num_kde_list);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		pe_err("KDE parsing fails");
		goto verify_fils_params_fails;
	}

	if (assoc_rsp->hlp_data_len) {
		fils_info->hlp_data = qdf_mem_malloc(assoc_rsp->hlp_data_len);

		if (!fils_info->hlp_data) {
			pe_err("FILS session HLP data malloc fails");
			return true;
		}

		/* Save the HLP container IE data if present*/
		cds_copy_hlp_info(&assoc_rsp->dst_mac, &assoc_rsp->src_mac,
				  assoc_rsp->hlp_data_len, assoc_rsp->hlp_data,
				  &fils_info->dst_mac, &fils_info->src_mac,
				  &fils_info->hlp_data_len,
				  fils_info->hlp_data);
	}
	return true;

verify_fils_params_fails:
	assoc_cnf->resultCode = eSIR_SME_ASSOC_REFUSED;
	assoc_cnf->protStatusCode = eSIR_MAC_UNSPEC_FAILURE_STATUS;
	return false;
}

/**
 * find_ie_data_after_fils_session_ie() - Find IE pointer after FILS Session IE
 * @mac_ctx: MAC context
 * @buf: IE buffer
 * @buf_len: Length of @buf
 * @ie: Pointer to update the found IE pointer after FILS session IE
 * @ie_len: length of the IE data after FILS session IE
 *
 * This API is used to find the IE data ptr and length after FILS session IE
 *
 * Return: QDF_STATUS_SUCCESS if found, else QDF_STATUS_E_FAILURE
 */
static QDF_STATUS find_ie_data_after_fils_session_ie(tpAniSirGlobal mac_ctx,
						     uint8_t *buf,
						     uint32_t buf_len,
						     uint8_t **ie,
						     uint32_t *ie_len)
{
	uint32_t left = buf_len;
	uint8_t *ptr = buf;
	uint8_t elem_id, elem_len;

	if (NULL == buf || 0 == buf_len)
		return QDF_STATUS_E_FAILURE;

	while (left >= 2) {
		elem_id = ptr[0];
		elem_len = ptr[1];
		left -= 2;
		if (elem_len > left)
			return QDF_STATUS_E_FAILURE;

		if (elem_id == SIR_MAC_REQUEST_EID_MAX &&
			ptr[2] == SIR_FILS_SESSION_EXT_EID) {
			(*ie) = ((&ptr[1]) + ptr[1] + 1);
			(*ie_len) = (left - elem_len);
			return QDF_STATUS_SUCCESS;
		}
		left -= elem_len;
		ptr += (elem_len + 2);
	}
	return QDF_STATUS_E_FAILURE;
}

/**
 * fils_aead_encrypt() - API to do FILS AEAD encryption
 *
 * @kek: Pointer to KEK
 * @kek_len: KEK length
 * @own_mac: Pointer to own MAC address
 * @bssid: Bssid
 * @snonce: Supplicant Nonce
 * @anonce: Authenticator Nonce
 * @data: Pointer to data after MAC header
 * @data_len: length of @data
 * @plain_text: Pointer to data after FILS Session IE
 * @plain_text_len: length of @plain_text
 * @out: Pointer to the encrypted data
 *
 * length of AEAD encryption @out is @plain_text_len + AES_BLOCK_SIZE[16 bytes]
 *
 * Return: zero on success, error otherwise
 */
static int fils_aead_encrypt(const u8 *kek, unsigned int kek_len,
			     const u8 *own_mac, const u8 *bssid,
			     const u8 *snonce, const u8 *anonce,
			     const u8 *data, size_t data_len, u8 *plain_text,
			     size_t plain_text_len, u8 *out)
{
	u8 v[AES_BLOCK_SIZE];
	const u8 *aad[6];
	size_t aad_len[6];
	u8 *buf;
	int ret;

	/* SIV Encrypt/Decrypt takes input key of length 256, 384 or 512 bits */
	if (kek_len != 32 && kek_len != 48 && kek_len != 64) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  FL("Invalid key length: %u"), kek_len);
		return -EINVAL;
	}

	if (own_mac == NULL || bssid == NULL || snonce == NULL ||
	    anonce == NULL || data_len == 0 || plain_text_len == 0 ||
	    out == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  FL("Error missing params mac:%pK bssid:%pK snonce:%pK anonce:%pK data_len:%zu plain_text_len:%zu out:%pK"),
			  own_mac, bssid, snonce, anonce, data_len,
			  plain_text_len, out);
		return -EINVAL;
	}

	if (plain_text == out) {
		buf = qdf_mem_malloc(plain_text_len);
		if (buf == NULL) {
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  FL("Failed to allocate memory"));
			return -ENOMEM;
		}
		qdf_mem_copy(buf, plain_text, plain_text_len);
	} else {
		buf = plain_text;
	}

	aad[0] = own_mac;
	aad_len[0] = QDF_MAC_ADDR_SIZE;
	aad[1] = bssid;
	aad_len[1] = QDF_MAC_ADDR_SIZE;
	aad[2] = snonce;
	aad_len[2] = SIR_FILS_NONCE_LENGTH;
	aad[3] = anonce;
	aad_len[3] = SIR_FILS_NONCE_LENGTH;
	aad[4] = data;
	aad_len[4] = data_len;
	/* Plain text, P, is Sn in AES-SIV */
	aad[5] = buf;
	aad_len[5] = plain_text_len;

	/* AES-SIV S2V */
	/* K1 = leftmost(K, len(K)/2) */
	ret = qdf_aes_s2v(kek, kek_len/2, aad, aad_len, 6, v);
	if (ret)
		goto error;

	/* out = SIV || C (Synthetic Initialization Vector || Ciphered text) */
	qdf_mem_copy(out, v, AES_BLOCK_SIZE);

	/* AES-SIV CTR */
	/* K2 = rightmost(K, len(K)/2) */
	/* Clear 31st and 63rd bits in counter synthetic iv */
	v[12] &= 0x7F;
	v[8] &= 0x7F;

	ret = qdf_aes_ctr(kek + kek_len/2, kek_len/2, v, buf, plain_text_len,
		      out + AES_BLOCK_SIZE, true);

error:
	if (plain_text == out)
		qdf_mem_free(buf);
	return ret;
}

QDF_STATUS aead_encrypt_assoc_req(tpAniSirGlobal mac_ctx,
				  tpPESession pe_session,
				  uint8_t *frm, uint32_t *frm_len)
{
	uint8_t *plain_text = NULL, *data;
	uint32_t plain_text_len = 0, data_len;
	QDF_STATUS status;
	struct pe_fils_session *fils_info = (pe_session->fils_info);

	if (!fils_info)
		return QDF_STATUS_E_FAILURE;

	/*
	 * data is the packet data after MAC header till
	 * FILS session IE(inclusive)
	 */
	data = frm + sizeof(tSirMacMgmtHdr);

	/*
	 * plain_text is the packet data after FILS session IE
	 * which needs to be encrypted. Get plain_text ptr and
	 * plain_text_len values using find_ptr_aft_fils_session_ie()
	 */
	status = find_ie_data_after_fils_session_ie(mac_ctx, data +
					      FIXED_PARAM_OFFSET_ASSOC_REQ,
					      (*frm_len -
					      FIXED_PARAM_OFFSET_ASSOC_REQ),
					      &plain_text, &plain_text_len);
	if (QDF_IS_STATUS_ERROR(status)) {
		pe_err("Could not find FILS session IE");
		return QDF_STATUS_E_FAILURE;
	}
	data_len = ((*frm_len) - plain_text_len);

	lim_fils_data_dump("Plain text: ", plain_text, plain_text_len);

	/* Overwrite the AEAD encrypted output @ plain_text */
	if (fils_aead_encrypt(fils_info->kek, fils_info->kek_len,
			      pe_session->selfMacAddr, pe_session->bssId,
			      fils_info->fils_nonce,
			      fils_info->auth_info.fils_nonce,
			      data, data_len, plain_text, plain_text_len,
			      plain_text)) {
		pe_err("AEAD Encryption fails!");
		return QDF_STATUS_E_FAILURE;
	}

	/*
	 * AEAD encrypted output(cipher_text) will have length equals to
	 * plain_text_len + AES_BLOCK_SIZE(AEAD encryption header info).
	 * Add this to frm_len
	 */
	(*frm_len) += (AES_BLOCK_SIZE);

	return QDF_STATUS_SUCCESS;
}

/**
 * fils_aead_decrypt() - API to do AEAD decryption
 *
 * @kek: Pointer to KEK
 * @kek_len: KEK length
 * @own_mac: Pointer to own MAC address
 * @bssid: Bssid
 * @snonce: Supplicant Nonce
 * @anonce: Authenticator Nonce
 * @data: Pointer to data after MAC header
 * @data_len: length of @data
 * @plain_text: Pointer to data after FILS Session IE
 * @plain_text_len: length of @plain_text
 * @out: Pointer to the encrypted data
 *
 * Return: zero on success, error otherwise
 */
static int fils_aead_decrypt(const u8 *kek, unsigned int kek_len,
			     const u8 *own_mac, const u8 *bssid,
			     const u8 *snonce, const u8 *anonce,
			     const u8 *data, size_t data_len, u8 *ciphered_text,
			     size_t ciphered_text_len, u8 *plain_text)
{
	const u8 *aad[6];
	size_t aad_len[6];
	u8 *buf;
	size_t buf_len;
	u8 v[AES_BLOCK_SIZE];
	u8 siv[AES_BLOCK_SIZE];
	int ret;

	/* SIV Encrypt/Decrypt takes input key of length 256, 384 or 512 bits */
	if (kek_len != 32 && kek_len != 48 && kek_len != 64) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  FL("Invalid key length: %u"), kek_len);
		return -EINVAL;
	}

	if (own_mac == NULL || bssid == NULL || snonce == NULL ||
	    anonce == NULL || data_len == 0 || ciphered_text_len == 0 ||
	    plain_text == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  FL("Error missing params mac:%pK bssid:%pK snonce:%pK anonce:%pK data_len:%zu ciphered_text_len:%zu plain_text:%pK"),
			  own_mac, bssid, snonce, anonce, data_len,
			  ciphered_text_len, plain_text);
		return -EINVAL;
	}

	qdf_mem_copy(v, ciphered_text, AES_BLOCK_SIZE);
	qdf_mem_copy(siv, ciphered_text, AES_BLOCK_SIZE);
	v[12] &= 0x7F;
	v[8] &= 0x7F;

	buf_len = ciphered_text_len - AES_BLOCK_SIZE;
	if (ciphered_text == plain_text) {
		/* in place decryption */
		buf = qdf_mem_malloc(buf_len);
		if (buf == NULL) {
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  FL("Failed to allocate memory"));
			return -ENOMEM;
		}
		qdf_mem_copy(buf, ciphered_text + AES_BLOCK_SIZE, buf_len);
	} else {
		buf = ciphered_text + AES_BLOCK_SIZE;
	}

	/* AES-SIV CTR */
	/* K2 = rightmost(K, len(K)/2) */
	ret = qdf_aes_ctr(kek + kek_len/2, kek_len/2, v, buf, buf_len,
			  plain_text, false);
	if (ret)
		goto error;

	aad[0] = bssid;
	aad_len[0] = QDF_MAC_ADDR_SIZE;
	aad[1] = own_mac;
	aad_len[1] = QDF_MAC_ADDR_SIZE;
	aad[2] = anonce;
	aad_len[2] = SIR_FILS_NONCE_LENGTH;
	aad[3] = snonce;
	aad_len[3] = SIR_FILS_NONCE_LENGTH;
	aad[4] = data;
	aad_len[4] = data_len;
	aad[5] = plain_text;
	aad_len[5] = buf_len;

	/* AES-SIV S2V */
	/* K1 = leftmost(K, len(K)/2) */
	ret = qdf_aes_s2v(kek, kek_len/2, aad, aad_len, 6, v);
	if (ret)
		goto error;

	/* compare the iv generated against the one sent by AP */
	if (memcmp(v, siv, AES_BLOCK_SIZE) != 0) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  FL("siv not same as frame siv"));
		ret = -EINVAL;
	}

error:
	if (ciphered_text == plain_text)
		qdf_mem_free(buf);
	return ret;
}

QDF_STATUS aead_decrypt_assoc_rsp(tpAniSirGlobal mac_ctx,
				  tpPESession session,
				  tDot11fAssocResponse *ar,
				  uint8_t *p_frame, uint32_t *n_frame)
{
	QDF_STATUS status;
	uint32_t data_len, fils_ies_len;
	uint8_t *fils_ies;
	struct pe_fils_session *fils_info = (session->fils_info);

	status = find_ie_data_after_fils_session_ie(mac_ctx, p_frame +
					      FIXED_PARAM_OFFSET_ASSOC_RSP,
					      ((*n_frame) -
					      FIXED_PARAM_OFFSET_ASSOC_RSP),
					      &fils_ies, &fils_ies_len);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		pe_err("FILS session IE not present");
		return status;
	}

	data_len = (*n_frame) - fils_ies_len;

	if (fils_aead_decrypt(fils_info->kek, fils_info->kek_len,
			      session->selfMacAddr, session->bssId,
			      fils_info->fils_nonce,
			      fils_info->auth_info.fils_nonce,
			      p_frame, data_len,
			      fils_ies, fils_ies_len, fils_ies)){
		pe_err("AEAD decryption fails");
		return QDF_STATUS_E_FAILURE;
	}

	/* Dump the output of AEAD decrypt */
	lim_fils_data_dump("Plain text: ", fils_ies,
			   fils_ies_len - AES_BLOCK_SIZE);

	(*n_frame) -= AES_BLOCK_SIZE;
	return status;
}

void lim_update_fils_rik(tpPESession pe_session,
			 tSirRoamOffloadScanReq *req_buffer)
{
	struct pe_fils_session *pe_fils_info = pe_session->fils_info;
	struct roam_fils_params *roam_fils_params =
		&req_buffer->roam_fils_params;

	if (!roam_fils_params)
		return;

	/*
	 * If it is first connection, LIM session entries will not be
	 * set with FILS. However in RSO, CSR filled the RRK, realm
	 * info and is_fils_connection to true in req_buffer, RIK
	 * can be created with RRK and send all the FILS info to fw
	 */
	if ((!lim_is_fils_connection(pe_session) ||
	     !pe_fils_info) && (req_buffer->is_fils_connection)) {
		if (roam_fils_params->rrk_length > FILS_MAX_RRK_LENGTH) {
			pe_err("FILS rrk len(%d) max (%d)",
					roam_fils_params->rrk_length,
					FILS_MAX_RRK_LENGTH);
			return;
		}

		lim_create_fils_rik(roam_fils_params->rrk,
				    roam_fils_params->rrk_length,
				    roam_fils_params->rik,
				    &roam_fils_params->rik_length);
		pe_debug("Fils created rik len %d",
					roam_fils_params->rik_length);
		return;
	}

	if ((pe_fils_info->fils_rik_len > FILS_MAX_RIK_LENGTH) ||
	    !pe_fils_info->fils_rik) {
		pe_err("Fils rik len(%d) max %d", pe_fils_info->fils_rik_len,
				FILS_MAX_RIK_LENGTH);
		return;
	}

	roam_fils_params->rik_length = pe_fils_info->fils_rik_len;
	qdf_mem_copy(roam_fils_params->rik, pe_fils_info->fils_rik,
			roam_fils_params->rik_length);
	pe_debug("fils rik len %d", roam_fils_params->rik_length);
}
#endif
