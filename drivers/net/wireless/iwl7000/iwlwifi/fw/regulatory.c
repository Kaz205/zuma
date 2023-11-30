// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (C) 2023 Intel Corporation
 */
#include <linux/dmi.h>
#include "iwl-drv.h"
#include "iwl-debug.h"
#include "regulatory.h"
#include "fw/runtime.h"
#include "fw/uefi.h"

#define IWL_BIOS_TABLE_LOADER(__name)					\
int iwl_bios_get_ ## __name ## _table(struct iwl_fw_runtime *fwrt)	\
{									\
	int ret = -ENOENT;						\
	if (fwrt->uefi_tables_lock_status > UEFI_WIFI_GUID_UNLOCKED)	\
		ret = iwl_uefi_get_ ## __name ## _table(fwrt);		\
	if (ret < 0)							\
		ret = iwl_acpi_get_ ## __name ## _table(fwrt);		\
	return ret;							\
}									\
IWL_EXPORT_SYMBOL(iwl_bios_get_ ## __name ## _table)

IWL_BIOS_TABLE_LOADER(wrds);
IWL_BIOS_TABLE_LOADER(ewrd);
IWL_BIOS_TABLE_LOADER(wgds);

static const struct dmi_system_id dmi_ppag_approved_list[] = {
	{ .ident = "HP",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
		},
	},
	{ .ident = "SAMSUNG",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD"),
		},
	},
	{ .ident = "MSFT",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Microsoft Corporation"),
		},
	},
	{ .ident = "ASUS",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
		},
	},
	{ .ident = "GOOGLE-HP",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Google"),
			DMI_MATCH(DMI_BOARD_VENDOR, "HP"),
		},
	},
	{ .ident = "GOOGLE-ASUS",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Google"),
			DMI_MATCH(DMI_BOARD_VENDOR, "ASUSTek COMPUTER INC."),
		},
	},
	{ .ident = "GOOGLE-SAMSUNG",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Google"),
			DMI_MATCH(DMI_BOARD_VENDOR, "SAMSUNG ELECTRONICS CO., LTD"),
		},
	},
	{ .ident = "DELL",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
		},
	},
	{ .ident = "DELL",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
		},
	},
	{ .ident = "RAZER",
	  .matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Razer"),
		},
	},
	{}
};

bool iwl_sar_geo_support(struct iwl_fw_runtime *fwrt)
{
	/*
	 * The PER_CHAIN_LIMIT_OFFSET_CMD command is not supported on
	 * earlier firmware versions.  Unfortunately, we don't have a
	 * TLV API flag to rely on, so rely on the major version which
	 * is in the first byte of ucode_ver.  This was implemented
	 * initially on version 38 and then backported to 17.  It was
	 * also backported to 29, but only for 7265D devices.  The
	 * intention was to have it in 36 as well, but not all 8000
	 * family got this feature enabled.  The 8000 family is the
	 * only one using version 36, so skip this version entirely.
	 */
	return IWL_UCODE_SERIAL(fwrt->fw->ucode_ver) >= 38 ||
		(IWL_UCODE_SERIAL(fwrt->fw->ucode_ver) == 17 &&
		 fwrt->trans->hw_rev != CSR_HW_REV_TYPE_3160) ||
		(IWL_UCODE_SERIAL(fwrt->fw->ucode_ver) == 29 &&
		 ((fwrt->trans->hw_rev & CSR_HW_REV_TYPE_MSK) ==
		  CSR_HW_REV_TYPE_7265D));
}
IWL_EXPORT_SYMBOL(iwl_sar_geo_support);

int iwl_sar_geo_fill_table(struct iwl_fw_runtime *fwrt,
			   struct iwl_per_chain_offset *table,
			   u32 n_bands, u32 n_profiles)
{
	int i, j;

	if (!fwrt->geo_enabled)
		return -ENODATA;

	if (!iwl_sar_geo_support(fwrt))
		return -EOPNOTSUPP;

	for (i = 0; i < n_profiles; i++) {
		for (j = 0; j < n_bands; j++) {
			struct iwl_per_chain_offset *chain =
				&table[i * n_bands + j];

			chain->max_tx_power =
				cpu_to_le16(fwrt->geo_profiles[i].bands[j].max);
			chain->chain_a =
				fwrt->geo_profiles[i].bands[j].chains[0];
			chain->chain_b =
				fwrt->geo_profiles[i].bands[j].chains[1];
			IWL_DEBUG_RADIO(fwrt,
					"SAR geographic profile[%d] Band[%d]: chain A = %d chain B = %d max_tx_power = %d\n",
					i, j,
					fwrt->geo_profiles[i].bands[j].chains[0],
					fwrt->geo_profiles[i].bands[j].chains[1],
					fwrt->geo_profiles[i].bands[j].max);
		}
	}

	return 0;
}
IWL_EXPORT_SYMBOL(iwl_sar_geo_fill_table);

static int iwl_sar_fill_table(struct iwl_fw_runtime *fwrt,
			      __le16 *per_chain, u32 n_subbands,
			      int prof_a, int prof_b)
{
	int profs[BIOS_SAR_NUM_CHAINS] = { prof_a, prof_b };
	int i, j;

	for (i = 0; i < BIOS_SAR_NUM_CHAINS; i++) {
		struct iwl_sar_profile *prof;

		/* don't allow SAR to be disabled (profile 0 means disable) */
		if (profs[i] == 0)
			return -EPERM;

		/* we are off by one, so allow up to BIOS_SAR_MAX_PROFILE_NUM */
		if (profs[i] > BIOS_SAR_MAX_PROFILE_NUM)
			return -EINVAL;

		/* profiles go from 1 to 4, so decrement to access the array */
		prof = &fwrt->sar_profiles[profs[i] - 1];

		/* if the profile is disabled, do nothing */
		if (!prof->enabled) {
			IWL_DEBUG_RADIO(fwrt, "SAR profile %d is disabled.\n",
					profs[i]);
			/*
			 * if one of the profiles is disabled, we
			 * ignore all of them and return 1 to
			 * differentiate disabled from other failures.
			 */
			return 1;
		}

		IWL_DEBUG_INFO(fwrt,
			       "SAR EWRD: chain %d profile index %d\n",
			       i, profs[i]);
		IWL_DEBUG_RADIO(fwrt, "  Chain[%d]:\n", i);
		for (j = 0; j < n_subbands; j++) {
			per_chain[i * n_subbands + j] =
				cpu_to_le16(prof->chains[i].subbands[j]);
			IWL_DEBUG_RADIO(fwrt, "    Band[%d] = %d * .125dBm\n",
					j, prof->chains[i].subbands[j]);
		}
	}

	return 0;
}

int iwl_sar_fill_profile(struct iwl_fw_runtime *fwrt,
			 __le16 *per_chain, u32 n_tables, u32 n_subbands,
			 int prof_a, int prof_b)
{
	int i, ret = 0;

	for (i = 0; i < n_tables; i++) {
		ret = iwl_sar_fill_table(fwrt,
			&per_chain[i * n_subbands * BIOS_SAR_NUM_CHAINS],
			n_subbands, prof_a, prof_b);
		if (ret)
			break;
	}

#ifdef CPTCFG_IWLMVM_VENDOR_CMDS
	fwrt->sar_chain_a_profile = prof_a;
	fwrt->sar_chain_b_profile = prof_b;
#endif
	return ret;
}
IWL_EXPORT_SYMBOL(iwl_sar_fill_profile);

static bool iwl_ppag_value_valid(struct iwl_fw_runtime *fwrt, int chain,
				 int subband)
{
	s8 ppag_val = fwrt->ppag_chains[chain].subbands[subband];

	if ((subband == 0 &&
	     (ppag_val > IWL_PPAG_MAX_LB || ppag_val < IWL_PPAG_MIN_LB)) ||
	    (subband != 0 &&
	     (ppag_val > IWL_PPAG_MAX_HB || ppag_val < IWL_PPAG_MIN_HB))) {
		IWL_DEBUG_RADIO(fwrt, "Invalid PPAG value: %d\n", ppag_val);
		return false;
	}
	return true;
}

int iwl_fill_ppag_table(struct iwl_fw_runtime *fwrt,
			union iwl_ppag_table_cmd *cmd, int *cmd_size)
{
	u8 cmd_ver;
	int i, j, num_sub_bands;
	s8 *gain;
	bool send_ppag_always;

	/* many firmware images for JF lie about this */
	if (CSR_HW_RFID_TYPE(fwrt->trans->hw_rf_id) ==
	    CSR_HW_RFID_TYPE(CSR_HW_RF_ID_TYPE_JF))
		return -EOPNOTSUPP;

	if (!fw_has_capa(&fwrt->fw->ucode_capa, IWL_UCODE_TLV_CAPA_SET_PPAG)) {
		IWL_DEBUG_RADIO(fwrt,
				"PPAG capability not supported by FW, command not sent.\n");
		return -EINVAL;
	}

	cmd_ver = iwl_fw_lookup_cmd_ver(fwrt->fw,
					WIDE_ID(PHY_OPS_GROUP,
						PER_PLATFORM_ANT_GAIN_CMD), 1);
	/*
	 * Starting from ver 4, driver needs to send the PPAG CMD regradless
	 * if PPAG is enabled/disabled or valid/invalid.
	 */
	send_ppag_always = cmd_ver > 3;

	/* Don't send PPAG if it is disabled */
	if (!send_ppag_always && !fwrt->ppag_flags) {
		IWL_DEBUG_RADIO(fwrt, "PPAG not enabled, command not sent.\n");
		return -EINVAL;
	}

	/* The 'flags' field is the same in v1 and in v2 so we can just
	 * use v1 to access it.
	 */
	cmd->v1.flags = cpu_to_le32(fwrt->ppag_flags);

	IWL_DEBUG_RADIO(fwrt, "PPAG cmd ver is %d\n", cmd_ver);
	if (cmd_ver == 1) {
		num_sub_bands = IWL_NUM_SUB_BANDS_V1;
		gain = cmd->v1.gain[0];
		*cmd_size = sizeof(cmd->v1);
		if (fwrt->ppag_ver == 1 || fwrt->ppag_ver == 2) {
			/* in this case FW supports revision 0 */
			IWL_DEBUG_RADIO(fwrt,
					"PPAG table rev is %d, send truncated table\n",
					fwrt->ppag_ver);
		}
	} else if (cmd_ver >= 2 && cmd_ver <= 4) {
		num_sub_bands = IWL_NUM_SUB_BANDS_V2;
		gain = cmd->v2.gain[0];
		*cmd_size = sizeof(cmd->v2);
		if (fwrt->ppag_ver == 0) {
			/* in this case FW supports revisions 1 or 2 */
			IWL_DEBUG_RADIO(fwrt,
					"PPAG table rev is 0, send padded table\n");
		}
	} else {
		IWL_DEBUG_RADIO(fwrt, "Unsupported PPAG command version\n");
		return -EINVAL;
	}

	/* ppag mode */
	IWL_DEBUG_RADIO(fwrt,
			"PPAG MODE bits were read from bios: %d\n",
			cmd->v1.flags);
	if ((cmd_ver == 1 &&
	     !fw_has_capa(&fwrt->fw->ucode_capa,
			  IWL_UCODE_TLV_CAPA_PPAG_CHINA_BIOS_SUPPORT)) ||
	    (cmd_ver == 2 && fwrt->ppag_ver == 2)) {
		cmd->v1.flags &= cpu_to_le32(IWL_PPAG_ETSI_MASK);
		IWL_DEBUG_RADIO(fwrt, "masking ppag China bit\n");
	} else {
		IWL_DEBUG_RADIO(fwrt, "isn't masking ppag China bit\n");
	}

	IWL_DEBUG_RADIO(fwrt,
			"PPAG MODE bits going to be sent: %d\n",
			cmd->v1.flags);

	for (i = 0; i < IWL_NUM_CHAIN_LIMITS; i++) {
		for (j = 0; j < num_sub_bands; j++) {
			if (!send_ppag_always &&
			    !iwl_ppag_value_valid(fwrt, i, j))
				return -EINVAL;

			gain[i * num_sub_bands + j] =
				fwrt->ppag_chains[i].subbands[j];
			IWL_DEBUG_RADIO(fwrt,
					"PPAG table: chain[%d] band[%d]: gain = %d\n",
					i, j, gain[i * num_sub_bands + j]);
		}
	}

	return 0;
}
IWL_EXPORT_SYMBOL(iwl_fill_ppag_table);

bool iwl_is_ppag_approved(struct iwl_fw_runtime *fwrt)
{
	if (!dmi_check_system(dmi_ppag_approved_list)) {
		IWL_DEBUG_RADIO(fwrt,
				"System vendor '%s' is not in the approved list, disabling PPAG.\n",
				dmi_get_system_info(DMI_SYS_VENDOR));
				fwrt->ppag_flags = 0;
				return false;
	}

	return true;
}
IWL_EXPORT_SYMBOL(iwl_is_ppag_approved);
