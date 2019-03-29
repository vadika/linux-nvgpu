/*
 * Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <nvgpu/gk20a.h>

#include <nvgpu/gr/gr.h>
#include <nvgpu/gr/config.h>

static int gr_load_sm_id_config(struct gk20a *g)
{
	int err;
	u32 *tpc_sm_id;
	u32 sm_id_size = g->ops.gr.init.get_sm_id_size();
	struct nvgpu_gr_config *gr_config = g->gr.config;

	tpc_sm_id = nvgpu_kcalloc(g, sm_id_size, sizeof(u32));
	if (tpc_sm_id == NULL) {
		return -ENOMEM;
	}

	err = g->ops.gr.init.sm_id_config(g, tpc_sm_id, gr_config);

	nvgpu_kfree(g, tpc_sm_id);

	return err;
}

static void gr_load_tpc_mask(struct gk20a *g)
{
	u32 pes_tpc_mask = 0, fuse_tpc_mask;
	u32 gpc, pes, val;
	u32 num_tpc_per_gpc = nvgpu_get_litter_value(g,
						     GPU_LIT_NUM_TPC_PER_GPC);
	u32 max_tpc_count = nvgpu_gr_config_get_max_tpc_count(g->gr.config);

	/* gv11b has 1 GPC and 4 TPC/GPC, so mask will not overflow u32 */
	for (gpc = 0; gpc < nvgpu_gr_config_get_gpc_count(g->gr.config);
								gpc++) {
		for (pes = 0;
		     pes < nvgpu_gr_config_get_pe_count_per_gpc(g->gr.config);
		     pes++) {
			pes_tpc_mask |= nvgpu_gr_config_get_pes_tpc_mask(
						g->gr.config, gpc, pes) <<
					num_tpc_per_gpc * gpc;
		}
	}

	nvgpu_log_info(g, "pes_tpc_mask %u\n", pes_tpc_mask);

	fuse_tpc_mask = g->ops.gr.config.get_gpc_tpc_mask(g, g->gr.config, 0);
	if ((g->tpc_fs_mask_user != 0U) &&
	    (g->tpc_fs_mask_user != fuse_tpc_mask) &&
	    (fuse_tpc_mask == BIT32(max_tpc_count) - U32(1))) {
		val = g->tpc_fs_mask_user;
		val &= BIT32(max_tpc_count) - U32(1);
		/* skip tpc to disable the other tpc cause channel timeout */
		val = BIT32(hweight32(val)) - U32(1);
		pes_tpc_mask = val;
	}
	g->ops.gr.init.tpc_mask(g, 0, pes_tpc_mask);
}

u32 nvgpu_gr_gpc_offset(struct gk20a *g, u32 gpc)
{
	u32 gpc_stride = nvgpu_get_litter_value(g, GPU_LIT_GPC_STRIDE);
	u32 gpc_offset = gpc_stride * gpc;

	return gpc_offset;
}

u32 nvgpu_gr_tpc_offset(struct gk20a *g, u32 tpc)
{
	u32 tpc_in_gpc_stride = nvgpu_get_litter_value(g,
					GPU_LIT_TPC_IN_GPC_STRIDE);
	u32 tpc_offset = tpc_in_gpc_stride * tpc;

	return tpc_offset;
}

int nvgpu_gr_suspend(struct gk20a *g)
{
	int ret = 0;

	nvgpu_log_fn(g, " ");

	ret = g->ops.gr.init.wait_empty(g);
	if (ret != 0) {
		return ret;
	}

	/* Disable fifo access */
	g->ops.gr.init.fifo_access(g, false);

	/* disable gr intr */
	g->ops.gr.intr.enable_interrupts(g, false);

	/* disable all exceptions */
	g->ops.gr.intr.enable_exceptions(g, g->gr.config, false);

	nvgpu_gr_flush_channel_tlb(g);

	g->gr.initialized = false;

	nvgpu_log_fn(g, "done");
	return ret;
}

/* invalidate channel lookup tlb */
void nvgpu_gr_flush_channel_tlb(struct gk20a *g)
{
	nvgpu_spinlock_acquire(&g->gr.ch_tlb_lock);
	(void) memset(g->gr.chid_tlb, 0,
		sizeof(struct gr_channel_map_tlb_entry) *
		GR_CHANNEL_MAP_TLB_SIZE);
	nvgpu_spinlock_release(&g->gr.ch_tlb_lock);
}

int nvgpu_gr_init_fs_state(struct gk20a *g)
{
	u32 tpc_index, gpc_index;
	u32 sm_id = 0;
	u32 fuse_tpc_mask;
	u32 gpc_cnt, tpc_cnt, max_tpc_cnt;
	int err = 0;
	struct nvgpu_gr_config *gr_config = g->gr.config;

	nvgpu_log_fn(g, " ");

	err = g->ops.gr.init.fs_state(g);
	if (err != 0) {
		return err;
	}

	if (g->ops.gr.config.init_sm_id_table != NULL) {
		err = g->ops.gr.config.init_sm_id_table(gr_config);
		if (err != 0) {
			return err;
		}

		/* Is table empty ? */
		if (nvgpu_gr_config_get_no_of_sm(gr_config) == 0U) {
			return -EINVAL;
		}
	}

	for (sm_id = 0; sm_id < nvgpu_gr_config_get_no_of_sm(gr_config);
	     sm_id++) {
		struct sm_info *sm_info =
			nvgpu_gr_config_get_sm_info(gr_config, sm_id);
		tpc_index = sm_info->tpc_index;
		gpc_index = sm_info->gpc_index;

		g->ops.gr.init.sm_id_numbering(g, gpc_index, tpc_index, sm_id);
	}

	g->ops.gr.init.pd_tpc_per_gpc(g, gr_config);

	/* gr__setup_pd_mapping */
	g->ops.gr.init.rop_mapping(g, gr_config);

	g->ops.gr.init.pd_skip_table_gpc(g, gr_config);

	fuse_tpc_mask = g->ops.gr.config.get_gpc_tpc_mask(g, gr_config, 0);
	gpc_cnt = nvgpu_gr_config_get_gpc_count(gr_config);
	tpc_cnt = nvgpu_gr_config_get_tpc_count(gr_config);
	max_tpc_cnt = nvgpu_gr_config_get_max_tpc_count(gr_config);

	if ((g->tpc_fs_mask_user != 0U) &&
		(fuse_tpc_mask == BIT32(max_tpc_cnt) - 1U)) {
		u32 val = g->tpc_fs_mask_user;
		val &= BIT32(max_tpc_cnt) - U32(1);
		tpc_cnt = (u32)hweight32(val);
	}
	g->ops.gr.init.cwd_gpcs_tpcs_num(g, gpc_cnt, tpc_cnt);

	gr_load_tpc_mask(g);

	err = gr_load_sm_id_config(g);
	if (err != 0) {
		nvgpu_err(g, "load_smid_config failed err=%d", err);
	}

	return err;
}

/* Wait until GR is initialized */
void nvgpu_gr_wait_initialized(struct gk20a *g)
{
	NVGPU_COND_WAIT(&g->gr.init_wq, g->gr.initialized, 0U);
}
