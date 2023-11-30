/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright (C) 2023 Intel Corporation
 */

#ifndef __fw_regulatory_h__
#define __fw_regulatory_h__

#include "fw/img.h"
#include "fw/api/commands.h"
#include "fw/api/power.h"
#include "fw/api/phy.h"
#include "fw/api/nvm-reg.h"
#include "fw/api/config.h"
#include "fw/img.h"
#include "iwl-trans.h"

#define BIOS_SAR_MAX_PROFILE_NUM	4
/*
 * Each SAR profile has (up to, depends on the table revision) 4 chains:
 * chain A, chain B, chain A when in CDB, chain B when in CDB
 */
#define BIOS_SAR_MAX_CHAINS_PER_PROFILE 4
#define BIOS_SAR_NUM_CHAINS             2
#define BIOS_SAR_MAX_SUB_BANDS_NUM      11

#define BIOS_GEO_NUM_CHAINS		2
#define BIOS_GEO_MAX_NUM_BANDS		3
#define BIOS_GEO_MAX_PROFILE_NUM	8
#define BIOS_GEO_MIN_PROFILE_NUM	3

#define IWL_SAR_ENABLE_MSK		BIT(0)

/*
 * The profile for revision 2 is a superset of revision 1, which is in
 * turn a superset of revision 0.  So we can store all revisions
 * inside revision 2, which is what we represent here.
 */

/*
 * struct iwl_sar_profile_chain - per-chain values of a SAR profile
 * @subbands: the SAR value for each subband
 */
struct iwl_sar_profile_chain {
	u8 subbands[BIOS_SAR_MAX_SUB_BANDS_NUM];
};

/*
 * struct iwl_sar_profile - SAR profile from SAR tables
 * @enabled: whether the profile is enabled or not
 * @chains: per-chain SAR values
 */
struct iwl_sar_profile {
	bool enabled;
	struct iwl_sar_profile_chain chains[BIOS_SAR_MAX_CHAINS_PER_PROFILE];
};

/* Same thing as with SAR, all revisions fit in revision 2 */

/*
 * struct iwl_geo_profile_band - per-band geo SAR offsets
 * @max: the max tx power allowed for the band
 * @chains: SAR offsets values for each chain
 */
struct iwl_geo_profile_band {
	u8 max;
	u8 chains[BIOS_GEO_NUM_CHAINS];
};

/*
 * struct iwl_geo_profile - geo profile
 * @bands: per-band table of the SAR offsets
 */
struct iwl_geo_profile {
	struct iwl_geo_profile_band bands[BIOS_GEO_MAX_NUM_BANDS];
};

struct iwl_fw_runtime;

bool iwl_sar_geo_support(struct iwl_fw_runtime *fwrt);

int iwl_sar_geo_fill_table(struct iwl_fw_runtime *fwrt,
			   struct iwl_per_chain_offset *table,
			   u32 n_bands, u32 n_profiles);

int iwl_sar_fill_profile(struct iwl_fw_runtime *fwrt,
			 __le16 *per_chain, u32 n_tables, u32 n_subbands,
			 int prof_a, int prof_b);

#endif /* __fw_regulatory_h__ */
