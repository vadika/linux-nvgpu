/*
 * GV11B LTC
 *
 * Copyright (c) 2016-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "gk20a/gk20a.h"

#include "gp10b/ltc_gp10b.h"

#include "ltc_gv11b.h"

#include <nvgpu/hw/gv11b/hw_ltc_gv11b.h>
#include <nvgpu/hw/gv11b/hw_mc_gv11b.h>
#include <nvgpu/hw/gv11b/hw_top_gv11b.h>
#include <nvgpu/hw/gv11b/hw_pri_ringmaster_gv11b.h>

/*
 * Sets the ZBC stencil for the passed index.
 */
static void gv11b_ltc_set_zbc_stencil_entry(struct gk20a *g,
					  struct zbc_entry *stencil_val,
					  u32 index)
{
	u32 real_index = index + GK20A_STARTOF_ZBC_TABLE;

	gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_index_r(),
		     ltc_ltcs_ltss_dstg_zbc_index_address_f(real_index));

	gk20a_writel(g, ltc_ltcs_ltss_dstg_zbc_stencil_clear_value_r(),
		     stencil_val->depth);

	gk20a_readl(g, ltc_ltcs_ltss_dstg_zbc_index_r());
}

static void gv11b_ltc_init_fs_state(struct gk20a *g)
{
	u32 ltc_intr;
	u32 reg;

	gk20a_dbg_info("initialize gv11b l2");

	g->max_ltc_count = gk20a_readl(g, top_num_ltcs_r());
	g->ltc_count = gk20a_readl(g, pri_ringmaster_enum_ltc_r());
	gk20a_dbg_info("%u ltcs out of %u", g->ltc_count, g->max_ltc_count);

	gk20a_writel(g, ltc_ltcs_ltss_dstg_cfg0_r(),
		gk20a_readl(g, ltc_ltc0_lts0_dstg_cfg0_r()) |
		ltc_ltcs_ltss_dstg_cfg0_vdc_4to2_disable_m());

	/* Disable LTC interrupts */
	reg = gk20a_readl(g, ltc_ltcs_ltss_intr_r());
	reg &= ~ltc_ltcs_ltss_intr_en_evicted_cb_m();
	reg &= ~ltc_ltcs_ltss_intr_en_illegal_compstat_access_m();
	gk20a_writel(g, ltc_ltcs_ltss_intr_r(), reg);

	/* Enable ECC interrupts */
	ltc_intr = gk20a_readl(g, ltc_ltcs_ltss_intr_r());
	ltc_intr |= ltc_ltcs_ltss_intr_en_ecc_sec_error_enabled_f() |
		ltc_ltcs_ltss_intr_en_ecc_ded_error_enabled_f();
	gk20a_writel(g, ltc_ltcs_ltss_intr_r(),
				ltc_intr);
}

static void gv11b_ltc_isr(struct gk20a *g)
{
	u32 mc_intr, ltc_intr3;
	unsigned int ltc, slice;
	u32 ltc_stride = nvgpu_get_litter_value(g, GPU_LIT_LTC_STRIDE);
	u32 lts_stride = nvgpu_get_litter_value(g, GPU_LIT_LTS_STRIDE);
	u32 ecc_status, ecc_addr, corrected_cnt, uncorrected_cnt;
	u32 corrected_delta, uncorrected_delta;
	u32 corrected_overflow, uncorrected_overflow;
	u32 ltc_corrected, ltc_uncorrected;

	mc_intr = gk20a_readl(g, mc_intr_ltc_r());
	for (ltc = 0; ltc < g->ltc_count; ltc++) {
		if ((mc_intr & 1 << ltc) == 0)
			continue;
		ltc_corrected = ltc_uncorrected = 0;

		for (slice = 0; slice < g->gr.slices_per_ltc; slice++) {
			u32 offset = ltc_stride * ltc + lts_stride * slice;
			ltc_intr3 = gk20a_readl(g, ltc_ltc0_lts0_intr3_r() +
						offset);

			/* Detect and handle ECC PARITY errors */

			if (ltc_intr3 &
				(ltc_ltcs_ltss_intr3_ecc_uncorrected_m() |
				 ltc_ltcs_ltss_intr3_ecc_corrected_m())) {

				ecc_status = gk20a_readl(g,
					ltc_ltc0_lts0_l2_cache_ecc_status_r() +
					offset);
				ecc_addr = gk20a_readl(g,
					ltc_ltc0_lts0_l2_cache_ecc_address_r() +
					offset);
				corrected_cnt = gk20a_readl(g,
					ltc_ltc0_lts0_l2_cache_ecc_corrected_err_count_r() + offset);
				uncorrected_cnt = gk20a_readl(g,
					ltc_ltc0_lts0_l2_cache_ecc_uncorrected_err_count_r() + offset);

				corrected_delta =
					ltc_ltc0_lts0_l2_cache_ecc_corrected_err_count_total_v(corrected_cnt);
				uncorrected_delta =
					ltc_ltc0_lts0_l2_cache_ecc_uncorrected_err_count_total_v(uncorrected_cnt);
				corrected_overflow = ecc_status &
					ltc_ltc0_lts0_l2_cache_ecc_status_corrected_err_total_counter_overflow_m();

				uncorrected_overflow = ecc_status &
					ltc_ltc0_lts0_l2_cache_ecc_status_uncorrected_err_total_counter_overflow_m();

				/* clear the interrupt */
				if ((corrected_delta > 0) || corrected_overflow) {
					gk20a_writel(g, ltc_ltc0_lts0_l2_cache_ecc_corrected_err_count_r() + offset, 0);
				}
				if ((uncorrected_delta > 0) || uncorrected_overflow) {
					gk20a_writel(g,
						ltc_ltc0_lts0_l2_cache_ecc_uncorrected_err_count_r() + offset, 0);
				}

				gk20a_writel(g, ltc_ltc0_lts0_l2_cache_ecc_status_r() + offset,
					ltc_ltc0_lts0_l2_cache_ecc_status_reset_task_f());

				/* update counters per slice */
				if (corrected_overflow)
					corrected_delta += (0x1UL << ltc_ltc0_lts0_l2_cache_ecc_corrected_err_count_total_s());
				if (uncorrected_overflow)
					uncorrected_delta += (0x1UL << ltc_ltc0_lts0_l2_cache_ecc_uncorrected_err_count_total_s());

				ltc_corrected += corrected_delta;
				ltc_uncorrected += uncorrected_delta;
				nvgpu_log(g, gpu_dbg_intr,
					"ltc:%d lts: %d cache ecc interrupt intr: 0x%x", ltc, slice, ltc_intr3);

				if (ecc_status & ltc_ltc0_lts0_l2_cache_ecc_status_corrected_err_rstg_m())
					nvgpu_log(g, gpu_dbg_intr, "rstg ecc error corrected");
				if (ecc_status & ltc_ltc0_lts0_l2_cache_ecc_status_uncorrected_err_rstg_m())
					nvgpu_log(g, gpu_dbg_intr, "rstg ecc error uncorrected");
				if (ecc_status & ltc_ltc0_lts0_l2_cache_ecc_status_corrected_err_tstg_m())
					nvgpu_log(g, gpu_dbg_intr, "tstg ecc error corrected");
				if (ecc_status & ltc_ltc0_lts0_l2_cache_ecc_status_uncorrected_err_tstg_m())
					nvgpu_log(g, gpu_dbg_intr, "tstg ecc error uncorrected");
				if (ecc_status & ltc_ltc0_lts0_l2_cache_ecc_status_corrected_err_dstg_m())
					nvgpu_log(g, gpu_dbg_intr, "dstg ecc error corrected");
				if (ecc_status & ltc_ltc0_lts0_l2_cache_ecc_status_uncorrected_err_dstg_m())
					nvgpu_log(g, gpu_dbg_intr, "dstg ecc error uncorrected");

				if (corrected_overflow || uncorrected_overflow)
					nvgpu_info(g, "ecc counter overflow!");

				nvgpu_log(g, gpu_dbg_intr,
					"ecc error address: 0x%x", ecc_addr);

			}

		}
		g->ecc.ltc.t19x.l2_cache_corrected_err_count.counters[ltc] +=
			ltc_corrected;
		g->ecc.ltc.t19x.l2_cache_uncorrected_err_count.counters[ltc] +=
			ltc_uncorrected;

	}

	/* fallback to other interrupts  */
	gp10b_ltc_isr(g);
}

static u32 gv11b_ltc_cbc_fix_config(struct gk20a *g, int base)
{
	u32 val = gk20a_readl(g, ltc_ltcs_ltss_cbc_num_active_ltcs_r());

	if (ltc_ltcs_ltss_cbc_num_active_ltcs__v(val) == 2)
		return base * 2;
	else if (ltc_ltcs_ltss_cbc_num_active_ltcs__v(val) != 1) {
		nvgpu_err(g, "Invalid number of active ltcs: %08x", val);
	}
	return base;
}


void gv11b_init_ltc(struct gpu_ops *gops)
{
	gp10b_init_ltc(gops);
	gops->ltc.set_zbc_s_entry = gv11b_ltc_set_zbc_stencil_entry;
	gops->ltc.init_fs_state = gv11b_ltc_init_fs_state;
	gops->ltc.cbc_fix_config = gv11b_ltc_cbc_fix_config;
	gops->ltc.isr = gv11b_ltc_isr;
	gops->ltc.init_cbc = NULL;
}
