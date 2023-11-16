// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2021-2023 Intel Corporation
 */

#include "iwl-drv.h"
#include "pnvm.h"
#include "iwl-prph.h"
#include "iwl-io.h"

#include "fw/uefi.h"
#include "fw/api/alive.h"
#include <linux/efi.h>
#include "fw/runtime.h"

/*
 * This is known to be broken on v4.19 and to work on v5.4.  Until we
 * figure out why this is the case and how to make it work, simply
 * disable the feature in old kernels.
 */
#if LINUX_VERSION_IS_GEQ(5,4,0)
#define IWL_EFI_VAR_GUID EFI_GUID(0x92daaf2f, 0xc02b, 0x455b,	\
				  0xb2, 0xec, 0xf5, 0xa3,	\
				  0x59, 0x4f, 0x4a, 0xea)

struct iwl_uefi_pnvm_mem_desc {
	__le32 addr;
	__le32 size;
	const u8 data[];
} __packed;

static void *iwl_uefi_get_variable(efi_char16_t *name, efi_guid_t *guid,
				   unsigned long *data_size)
{
	efi_status_t status;
	void *data;

	if (!data_size)
		return ERR_PTR(-EINVAL);

	if (!efi_rt_services_supported(EFI_RT_SUPPORTED_GET_VARIABLE))
		return ERR_PTR(-ENODEV);

	/* first call with NULL data to get the exact entry size */
	*data_size = 0;
	status = efi.get_variable(name, guid, NULL, data_size, NULL);
	if (status != EFI_BUFFER_TOO_SMALL || !*data_size)
		return ERR_PTR(-EIO);

	data = kmalloc(*data_size, GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	status = efi.get_variable(name, guid, NULL, data_size, data);
	if (status != EFI_SUCCESS) {
		kfree(data);
		return ERR_PTR(-ENOENT);
	}

	return data;
}

void *iwl_uefi_get_pnvm(struct iwl_trans *trans, size_t *len)
{
	unsigned long package_size;
	void *data;

	*len = 0;

	data = iwl_uefi_get_variable(IWL_UEFI_OEM_PNVM_NAME, &IWL_EFI_VAR_GUID,
				     &package_size);
	if (IS_ERR(data)) {
		IWL_DEBUG_FW(trans,
			     "PNVM UEFI variable not found 0x%lx (len %lu)\n",
			     PTR_ERR(data), package_size);
		return data;
	}

	IWL_DEBUG_FW(trans, "Read PNVM from UEFI with size %lu\n", package_size);
	*len = package_size;

	return data;
}

static
void *iwl_uefi_get_verified_variable(struct iwl_trans *trans,
				     efi_char16_t *uefi_var_name,
				     char *var_name,
				     unsigned int expected_size,
				     unsigned long *size)
{
	void *var;
	unsigned long var_size;

	var = iwl_uefi_get_variable(uefi_var_name, &IWL_EFI_VAR_GUID,
				    &var_size);

	if (IS_ERR(var)) {
		IWL_DEBUG_RADIO(trans,
				"%s UEFI variable not found 0x%lx\n", var_name,
				PTR_ERR(var));
		return var;
	}

	if (var_size < expected_size) {
		IWL_DEBUG_RADIO(trans,
				"Invalid %s UEFI variable len (%lu)\n",
				var_name, var_size);
		kfree(var);
		return ERR_PTR(-EINVAL);
	}

	IWL_DEBUG_RADIO(trans, "%s from UEFI with size %lu\n", var_name,
			var_size);

	if (size)
		*size = var_size;
	return var;
}

int iwl_uefi_handle_tlv_mem_desc(struct iwl_trans *trans, const u8 *data,
				 u32 tlv_len, struct iwl_pnvm_image *pnvm_data)
{
	const struct iwl_uefi_pnvm_mem_desc *desc = (const void *)data;
	u32 data_len;

	if (tlv_len < sizeof(*desc)) {
		IWL_DEBUG_FW(trans, "TLV len (%d) is too small\n", tlv_len);
		return -EINVAL;
	}

	data_len = tlv_len - sizeof(*desc);

	IWL_DEBUG_FW(trans,
		     "Handle IWL_UCODE_TLV_MEM_DESC, len %d data_len %d\n",
		     tlv_len, data_len);

	if (le32_to_cpu(desc->size) != data_len) {
		IWL_DEBUG_FW(trans, "invalid mem desc size %d\n", desc->size);
		return -EINVAL;
	}

	if (pnvm_data->n_chunks == IPC_DRAM_MAP_ENTRY_NUM_MAX) {
		IWL_DEBUG_FW(trans, "too many payloads to allocate in DRAM.\n");
		return -EINVAL;
	}

	IWL_DEBUG_FW(trans, "Adding data (size %d)\n", data_len);

	pnvm_data->chunks[pnvm_data->n_chunks].data = desc->data;
	pnvm_data->chunks[pnvm_data->n_chunks].len = data_len;
	pnvm_data->n_chunks++;

	return 0;
}

static int iwl_uefi_reduce_power_section(struct iwl_trans *trans,
					 const u8 *data, size_t len,
					 struct iwl_pnvm_image *pnvm_data)
{
	const struct iwl_ucode_tlv *tlv;

	IWL_DEBUG_FW(trans, "Handling REDUCE_POWER section\n");
	memset(pnvm_data, 0, sizeof(*pnvm_data));

	while (len >= sizeof(*tlv)) {
		u32 tlv_len, tlv_type;

		len -= sizeof(*tlv);
		tlv = (const void *)data;

		tlv_len = le32_to_cpu(tlv->length);
		tlv_type = le32_to_cpu(tlv->type);

		if (len < tlv_len) {
			IWL_ERR(trans, "invalid TLV len: %zd/%u\n",
				len, tlv_len);
			return -EINVAL;
		}

		data += sizeof(*tlv);

		switch (tlv_type) {
		case IWL_UCODE_TLV_MEM_DESC:
			if (iwl_uefi_handle_tlv_mem_desc(trans, data, tlv_len,
							 pnvm_data))
				return -EINVAL;
			break;
		case IWL_UCODE_TLV_PNVM_SKU:
			IWL_DEBUG_FW(trans,
				     "New REDUCE_POWER section started, stop parsing.\n");
			goto done;
		default:
			IWL_DEBUG_FW(trans, "Found TLV 0x%0x, len %d\n",
				     tlv_type, tlv_len);
			break;
		}

		len -= ALIGN(tlv_len, 4);
		data += ALIGN(tlv_len, 4);
	}

done:
	if (!pnvm_data->n_chunks) {
		IWL_DEBUG_FW(trans, "Empty REDUCE_POWER, skipping.\n");
		return -ENOENT;
	}
	return 0;
}

int iwl_uefi_reduce_power_parse(struct iwl_trans *trans,
				const u8 *data, size_t len,
				struct iwl_pnvm_image *pnvm_data)
{
	const struct iwl_ucode_tlv *tlv;

	IWL_DEBUG_FW(trans, "Parsing REDUCE_POWER data\n");

	while (len >= sizeof(*tlv)) {
		u32 tlv_len, tlv_type;

		len -= sizeof(*tlv);
		tlv = (const void *)data;

		tlv_len = le32_to_cpu(tlv->length);
		tlv_type = le32_to_cpu(tlv->type);

		if (len < tlv_len) {
			IWL_ERR(trans, "invalid TLV len: %zd/%u\n",
				len, tlv_len);
			return -EINVAL;
		}

		if (tlv_type == IWL_UCODE_TLV_PNVM_SKU) {
			const struct iwl_sku_id *sku_id =
				(const void *)(data + sizeof(*tlv));

			IWL_DEBUG_FW(trans,
				     "Got IWL_UCODE_TLV_PNVM_SKU len %d\n",
				     tlv_len);
			IWL_DEBUG_FW(trans, "sku_id 0x%0x 0x%0x 0x%0x\n",
				     le32_to_cpu(sku_id->data[0]),
				     le32_to_cpu(sku_id->data[1]),
				     le32_to_cpu(sku_id->data[2]));

			data += sizeof(*tlv) + ALIGN(tlv_len, 4);
			len -= ALIGN(tlv_len, 4);

			if (trans->sku_id[0] == le32_to_cpu(sku_id->data[0]) &&
			    trans->sku_id[1] == le32_to_cpu(sku_id->data[1]) &&
			    trans->sku_id[2] == le32_to_cpu(sku_id->data[2])) {
				int ret = iwl_uefi_reduce_power_section(trans,
								    data, len,
								    pnvm_data);
				if (!ret)
					return 0;
			} else {
				IWL_DEBUG_FW(trans, "SKU ID didn't match!\n");
			}
		} else {
			data += sizeof(*tlv) + ALIGN(tlv_len, 4);
			len -= ALIGN(tlv_len, 4);
		}
	}

	return -ENOENT;
}

u8 *iwl_uefi_get_reduced_power(struct iwl_trans *trans, size_t *len)
{
	struct pnvm_sku_package *package;
	unsigned long package_size;
	u8 *data;

	package = (struct pnvm_sku_package *)
		iwl_uefi_get_verified_variable(trans,
					       IWL_UEFI_REDUCED_POWER_NAME,
					       "Reduced Power",
						sizeof(*package),
						&package_size);
	if (IS_ERR(package))
		return ERR_CAST(package);

	IWL_DEBUG_FW(trans, "rev %d, total_size %d, n_skus %d\n",
		     package->rev, package->total_size, package->n_skus);

	*len = package_size - sizeof(*package);
	data = kmemdup(package->data, *len, GFP_KERNEL);
	if (!data) {
		kfree(package);
		return ERR_PTR(-ENOMEM);
	}

	kfree(package);

	return data;
}

static int iwl_uefi_step_parse(struct uefi_cnv_common_step_data *common_step_data,
			       struct iwl_trans *trans)
{
	if (common_step_data->revision != 1)
		return -EINVAL;

	trans->mbx_addr_0_step = (u32)common_step_data->revision |
		(u32)common_step_data->cnvi_eq_channel << 8 |
		(u32)common_step_data->cnvr_eq_channel << 16 |
		(u32)common_step_data->radio1 << 24;
	trans->mbx_addr_1_step = (u32)common_step_data->radio2;
	return 0;
}

void iwl_uefi_get_step_table(struct iwl_trans *trans)
{
	struct uefi_cnv_common_step_data *data;
	int ret;

	if (trans->trans_cfg->device_family < IWL_DEVICE_FAMILY_AX210)
		return;

	data = (struct uefi_cnv_common_step_data *)
		iwl_uefi_get_verified_variable(trans, IWL_UEFI_STEP_NAME,
					       "STEP", sizeof(*data), NULL);
	if (IS_ERR(data))
		return;

	ret = iwl_uefi_step_parse(data, trans);
	if (ret < 0)
		IWL_DEBUG_FW(trans, "Cannot read STEP tables. rev is invalid\n");

	kfree(data);
}
IWL_EXPORT_SYMBOL(iwl_uefi_get_step_table);

#ifdef CONFIG_ACPI
static int iwl_uefi_sgom_parse(struct uefi_cnv_wlan_sgom_data *sgom_data,
			       struct iwl_fw_runtime *fwrt)
{
	int i, j;

	if (sgom_data->revision != 1)
		return -EINVAL;

	memcpy(fwrt->sgom_table.offset_map, sgom_data->offset_map,
	       sizeof(fwrt->sgom_table.offset_map));

	for (i = 0; i < MCC_TO_SAR_OFFSET_TABLE_ROW_SIZE; i++) {
		for (j = 0; j < MCC_TO_SAR_OFFSET_TABLE_COL_SIZE; j++) {
			/* since each byte is composed of to values, */
			/* one for each letter, */
			/* extract and check each of them separately */
			u8 value = fwrt->sgom_table.offset_map[i][j];
			u8 low = value & 0xF;
			u8 high = (value & 0xF0) >> 4;

			if (high > fwrt->geo_num_profiles)
				high = 0;
			if (low > fwrt->geo_num_profiles)
				low = 0;
			fwrt->sgom_table.offset_map[i][j] = (high << 4) | low;
		}
	}

	fwrt->sgom_enabled = true;
	return 0;
}

void iwl_uefi_get_sgom_table(struct iwl_trans *trans,
			     struct iwl_fw_runtime *fwrt)
{
	struct uefi_cnv_wlan_sgom_data *data;
	int ret;

	if (!fwrt->geo_enabled)
		return;

	data = (struct uefi_cnv_wlan_sgom_data *)
		iwl_uefi_get_verified_variable(trans, IWL_UEFI_SGOM_NAME,
					       "SGOM", sizeof(*data), NULL);
	if (IS_ERR(data))
		return;

	ret = iwl_uefi_sgom_parse(data, fwrt);
	if (ret < 0)
		IWL_DEBUG_FW(trans, "Cannot read SGOM tables. rev is invalid\n");

	kfree(data);
}
IWL_EXPORT_SYMBOL(iwl_uefi_get_sgom_table);

static int iwl_uefi_uats_parse(struct uefi_cnv_wlan_uats_data *uats_data,
			       struct iwl_fw_runtime *fwrt)
{
	if (uats_data->revision != 1)
		return -EINVAL;

	memcpy(fwrt->uats_table.offset_map, uats_data->offset_map,
	       sizeof(fwrt->uats_table.offset_map));
	return 0;
}

int iwl_uefi_get_uats_table(struct iwl_trans *trans,
			    struct iwl_fw_runtime *fwrt)
{
	struct uefi_cnv_wlan_uats_data *data;
	int ret;

	data = (struct uefi_cnv_wlan_uats_data *)
		iwl_uefi_get_verified_variable(trans, IWL_UEFI_UATS_NAME,
					       "UATS", sizeof(*data), NULL);
	if (IS_ERR(data))
		return -EINVAL;

	ret = iwl_uefi_uats_parse(data, fwrt);
	if (ret < 0) {
		IWL_DEBUG_FW(trans, "Cannot read UATS table. rev is invalid\n");
		kfree(data);
		return ret;
	}

	kfree(data);
	return 0;
}
IWL_EXPORT_SYMBOL(iwl_uefi_get_uats_table);
#endif /* CONFIG_ACPI */
#endif /* >= 5.4 */
