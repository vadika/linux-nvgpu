/*
 * Copyright (c) 2015-2020, NVIDIA CORPORATION.  All rights reserved.
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

#include <nvgpu/timers.h>
#include <nvgpu/nvgpu_mem.h>
#include <nvgpu/soc.h>
#include <nvgpu/barrier.h>
#include <nvgpu/ptimer.h>
#include <nvgpu/io.h>
#include <nvgpu/fifo.h>
#include <nvgpu/rc.h>
#include <nvgpu/runlist.h>
#include <nvgpu/gk20a.h>
#include <nvgpu/channel.h>
#include <nvgpu/nvgpu_err.h>
#include <nvgpu/pbdma_status.h>
#include <nvgpu/engine_status.h>
#include <nvgpu/preempt.h>
#include <nvgpu/nvgpu_err.h>
#ifdef CONFIG_NVGPU_LS_PMU
#include <nvgpu/pmu/mutex.h>
#endif
#include <nvgpu/gops_mc.h>

#include "preempt_gv11b.h"

#include <nvgpu/hw/gv11b/hw_fifo_gv11b.h>


void gv11b_fifo_preempt_trigger(struct gk20a *g, u32 id, unsigned int id_type)
{
	if (id_type == ID_TYPE_TSG) {
		nvgpu_writel(g, fifo_preempt_r(),
			fifo_preempt_id_f(id) |
			fifo_preempt_type_tsg_f());
	} else {
		nvgpu_log_info(g, "channel preempt is noop");
	}
}

static void gv11b_fifo_issue_runlist_preempt(struct gk20a *g,
					 u32 runlists_mask)
{
	u32 reg_val;

	/* issue runlist preempt */
	reg_val = nvgpu_readl(g, fifo_runlist_preempt_r());
	reg_val |= runlists_mask;
	nvgpu_writel(g, fifo_runlist_preempt_r(), reg_val);
}

static int gv11b_fifo_preempt_locked(struct gk20a *g, u32 id,
		unsigned int id_type)
{
	nvgpu_log_fn(g, "preempt id: %d id_type: %d", id, id_type);

	g->ops.fifo.preempt_trigger(g, id, id_type);

	/* poll for preempt done */
	return g->ops.fifo.is_preempt_pending(g, id, id_type);

}

/*
 * This should be called with runlist_lock held for all the
 * runlists set in runlists_mask
 */
void gv11b_fifo_preempt_runlists_for_rc(struct gk20a *g, u32 runlists_mask)
{
#ifdef CONFIG_NVGPU_LS_PMU
	u32 token = PMU_INVALID_MUTEX_OWNER_ID;
	int mutex_ret = 0;
#endif
#ifdef CONFIG_NVGPU_RECOVERY
	struct nvgpu_fifo *f = &g->fifo;
	u32 i;
#endif

	/* runlist_lock are locked by teardown and sched are disabled too */
	nvgpu_log_fn(g, "preempt runlists_mask:0x%08x", runlists_mask);
#ifdef CONFIG_NVGPU_LS_PMU
	mutex_ret = nvgpu_pmu_lock_acquire(g, g->pmu,
			PMU_MUTEX_ID_FIFO, &token);
#endif
	/* issue runlist preempt */
	gv11b_fifo_issue_runlist_preempt(g, runlists_mask);

#ifdef CONFIG_NVGPU_RECOVERY
	/*
	 * Preemption will never complete in RC due to some fatal condition.
	 * Do not poll for preemption to complete. Reset engines served by
	 * runlists.
	 */
	for (i = 0U; i < f->num_runlists; i++) {
		struct nvgpu_runlist_info *runlist;

		runlist = &f->active_runlist_info[i];

		if ((fifo_runlist_preempt_runlist_m(runlist->runlist_id) &
				runlists_mask) != 0U) {
			runlist->reset_eng_bitmask = runlist->eng_bitmask;
		}
	}
#endif
#ifdef CONFIG_NVGPU_LS_PMU
	if (mutex_ret == 0) {
		int err = nvgpu_pmu_lock_release(g, g->pmu, PMU_MUTEX_ID_FIFO,
				&token);
		if (err != 0) {
			nvgpu_err(g, "PMU_MUTEX_ID_FIFO not released err=%d",
					err);
		}
	}
#endif
}

static int fifo_preempt_check_tsg_on_pbdma(u32 tsgid,
		struct nvgpu_pbdma_status_info *pbdma_status)
{
	int ret = -EBUSY;

	if (nvgpu_pbdma_status_is_chsw_valid(pbdma_status) ||
		nvgpu_pbdma_status_is_chsw_save(pbdma_status)) {

		if (tsgid != pbdma_status->id) {
			ret = 0;
		}

	} else if (nvgpu_pbdma_status_is_chsw_load(pbdma_status)) {

		if (tsgid != pbdma_status->next_id) {
			ret = 0;
		}

	} else if (nvgpu_pbdma_status_is_chsw_switch(pbdma_status)) {

		if ((tsgid != pbdma_status->next_id) &&
			 (tsgid != pbdma_status->id)) {
			ret = 0;
		}
	} else {
		/* pbdma status is invalid i.e. it is not loaded */
		ret = 0;
	}

	return ret;
}

int gv11b_fifo_preempt_poll_pbdma(struct gk20a *g, u32 tsgid,
				 u32 pbdma_id)
{
	struct nvgpu_timeout timeout;
	u32 delay = POLL_DELAY_MIN_US;
	int ret;
	unsigned int loop_count = 0;
	struct nvgpu_pbdma_status_info pbdma_status;

	/* timeout in milli seconds */
	ret = nvgpu_timeout_init(g, &timeout,
			nvgpu_preempt_get_timeout(g),
			NVGPU_TIMER_CPU_TIMER);
	if (ret != 0) {
		nvgpu_err(g, "timeout_init failed: %d", ret);
		return ret;
	}

	/* Default return value */
	ret = -EBUSY;

	nvgpu_log(g, gpu_dbg_info, "wait preempt pbdma %d", pbdma_id);
	/* Verify that ch/tsg is no longer on the pbdma */
	do {
		if (!nvgpu_platform_is_silicon(g)) {
			if (loop_count >= PREEMPT_PENDING_POLL_PRE_SI_RETRIES) {
				nvgpu_err(g, "preempt pbdma retries: %u",
					loop_count);
				break;
			}
			loop_count++;
		}
		/*
		 * If the PBDMA has a stalling interrupt and receives a NACK,
		 * the PBDMA won't save out until the STALLING interrupt is
		 * cleared. Stalling interrupt need not be directly addressed,
		 * as simply clearing of the interrupt bit will be sufficient
		 * to allow the PBDMA to save out. If the stalling interrupt
		 * was due to a SW method or another deterministic failure,
		 * the PBDMA will assert it when the channel is reloaded
		 * or resumed. Note that the fault will still be
		 * reported to SW.
		 */

		/* Ignore un-needed return value "recover" */
		(void)g->ops.pbdma.handle_intr(g, pbdma_id, NULL, &pbdma_status);

		ret = fifo_preempt_check_tsg_on_pbdma(tsgid, &pbdma_status);
		if (ret == 0) {
			break;
		}

		nvgpu_usleep_range(delay, delay * 2U);
		delay = min_t(u32, delay << 1, POLL_DELAY_MAX_US);
	} while (nvgpu_timeout_expired(&timeout) == 0);

	if (ret != 0) {
		nvgpu_err(g, "preempt timeout pbdma: %u pbdma_stat: %u "
				"tsgid: %u", pbdma_id,
				pbdma_status.pbdma_reg_status, tsgid);
	}
	return ret;
}

static int fifo_check_eng_intr_pending(struct gk20a *g, u32 id,
			 u32 eng_stat, u32 ctx_stat, u32 eng_intr_pending,
			 u32 engine_id, u32 *reset_eng_bitmask)
{
	int ret = -EBUSY;

	if (ctx_stat == fifo_engine_status_ctx_status_ctxsw_switch_v()) {
		/* Eng save hasn't started yet. Continue polling */
		if (eng_intr_pending != 0U) {
			/* if eng intr, stop polling */
			*reset_eng_bitmask |= BIT32(engine_id);
			ret = 0;
		}

	} else if ((ctx_stat == fifo_engine_status_ctx_status_valid_v()) ||
		(ctx_stat == fifo_engine_status_ctx_status_ctxsw_save_v())) {

		if (id == fifo_engine_status_id_v(eng_stat)) {
			if (eng_intr_pending != 0U) {
				/* preemption will not finish */
				*reset_eng_bitmask |= BIT32(engine_id);
				ret = 0;
			}
		} else {
			/* context is not running on the engine */
			ret = 0;
		}

	} else if (ctx_stat == fifo_engine_status_ctx_status_ctxsw_load_v()) {

		if (id == fifo_engine_status_next_id_v(eng_stat)) {
			if (eng_intr_pending != 0U) {
				/* preemption will not finish */
				*reset_eng_bitmask |= BIT32(engine_id);
				ret = 0;
			}
		} else {
			/* context is not running on the engine */
			ret = 0;
		}

	} else {
		/* Preempt should be finished */
		ret = 0;
	}

	return ret;
}

static int gv11b_fifo_preempt_poll_eng(struct gk20a *g, u32 id,
			 u32 engine_id, u32 *reset_eng_bitmask)
{
	struct nvgpu_timeout timeout;
	u32 delay = POLL_DELAY_MIN_US;
	u32 eng_stat;
	u32 ctx_stat;
	int ret;
	unsigned int loop_count = 0;
	u32 eng_intr_pending;

	/* timeout in milli seconds */
	ret = nvgpu_timeout_init(g, &timeout,
			nvgpu_preempt_get_timeout(g),
			NVGPU_TIMER_CPU_TIMER);
	if (ret != 0) {
		nvgpu_err(g, "timeout_init failed: %d", ret);
		return ret;
	}

	/* Default return value */
	ret = -EBUSY;

	nvgpu_log(g, gpu_dbg_info, "wait preempt act engine id: %u",
			engine_id);
	/* Check if ch/tsg has saved off the engine or if ctxsw is hung */
	do {
		if (!nvgpu_platform_is_silicon(g)) {
			if (loop_count >= PREEMPT_PENDING_POLL_PRE_SI_RETRIES) {
				nvgpu_err(g, "preempt eng retries: %u",
					loop_count);
				break;
			}
			loop_count++;
		}
		eng_stat = nvgpu_readl(g, fifo_engine_status_r(engine_id));
		ctx_stat  = fifo_engine_status_ctx_status_v(eng_stat);

		if (g->ops.mc.is_stall_and_eng_intr_pending(g, engine_id,
					&eng_intr_pending)) {
		/*
		 * From h/w team
		 * Engine save can be blocked by eng  stalling interrupts.
		 * FIFO interrupts shouldn’t block an engine save from
		 * finishing, but could block FIFO from reporting preempt done.
		 * No immediate reason to reset the engine if FIFO interrupt is
		 * pending.
		 * The hub, priv_ring, and ltc interrupts could block context
		 * switch (or memory), but doesn’t necessarily have to.
		 * For Hub interrupts they just report access counters and page
		 * faults. Neither of these necessarily block context switch
		 * or preemption, but they could.
		 * For example a page fault for graphics would prevent graphics
		 * from saving out. An access counter interrupt is a
		 * notification and has no effect.
		 * SW should handle page faults though for preempt to complete.
		 * PRI interrupt (due to a failed PRI transaction) will result
		 * in ctxsw failure reported to HOST.
		 * LTC interrupts are generally ECC related and if so,
		 * certainly don’t block preemption/ctxsw but they could.
		 * Bus interrupts shouldn’t have anything to do with preemption
		 * state as they are part of the Host EXT pipe, though they may
		 * exhibit a symptom that indicates that GPU is in a bad state.
		 * To be completely fair, when an engine is preempting SW
		 * really should just handle other interrupts as they come in.
		 * It’s generally bad to just poll and wait on a preempt
		 * to complete since there are many things in the GPU which may
		 * cause a system to hang/stop responding.
		 */
			nvgpu_log(g, gpu_dbg_info | gpu_dbg_intr,
					"stall intr set, "
					"preemption might not finish");
		}
		ret = fifo_check_eng_intr_pending(g, id, eng_stat, ctx_stat,
				eng_intr_pending, engine_id,
				reset_eng_bitmask);
		if (ret == 0) {
			break;
		}

		nvgpu_usleep_range(delay, delay * 2U);
		delay = min_t(u32, delay << 1, POLL_DELAY_MAX_US);
	} while (nvgpu_timeout_expired(&timeout) == 0);

	if (ret != 0) {
		/*
		 * The reasons a preempt can fail are:
		 * 1.Some other stalling interrupt is asserted preventing
		 *   channel or context save.
		 * 2.The memory system hangs.
		 * 3.The engine hangs during CTXSW.
		 */
		nvgpu_err(g, "preempt timeout eng: %u ctx_stat: %u tsgid: %u",
			engine_id, ctx_stat, id);
		*reset_eng_bitmask |= BIT32(engine_id);
	}

	return ret;
}

int gv11b_fifo_is_preempt_pending(struct gk20a *g, u32 id,
		 unsigned int id_type)
{
	struct nvgpu_fifo *f = &g->fifo;
	unsigned long runlist_served_pbdmas;
	unsigned long runlist_served_engines;
	unsigned long bit;
	u32 pbdma_id;
	u32 engine_id;
	u32 runlist_id;
	int err, ret = 0;
	u32 tsgid;

	if (id_type == ID_TYPE_TSG) {
		runlist_id = f->tsg[id].runlist_id;
		tsgid = id;
	} else {
		runlist_id = f->channel[id].runlist_id;
		tsgid = f->channel[id].tsgid;
	}

	nvgpu_log_info(g, "Check preempt pending for tsgid = %u", tsgid);

	runlist_served_pbdmas = f->runlist_info[runlist_id]->pbdma_bitmask;
	runlist_served_engines = f->runlist_info[runlist_id]->eng_bitmask;

	for_each_set_bit(bit, &runlist_served_pbdmas, f->num_pbdma) {
		pbdma_id = U32(bit);
		err = gv11b_fifo_preempt_poll_pbdma(g, tsgid,
				pbdma_id);
		if (err != 0) {
			ret = err;
		}
	}

	f->runlist_info[runlist_id]->reset_eng_bitmask = 0U;

	for_each_set_bit(bit, &runlist_served_engines, f->max_engines) {
		engine_id = U32(bit);
		err = gv11b_fifo_preempt_poll_eng(g,
			tsgid, engine_id,
			&f->runlist_info[runlist_id]->reset_eng_bitmask);
		if ((err != 0) && (ret == 0)) {
			ret = err;
		}
	}
	return ret;
}

int gv11b_fifo_preempt_channel(struct gk20a *g, struct nvgpu_channel *ch)
{
	struct nvgpu_tsg *tsg = NULL;

	tsg = nvgpu_tsg_from_ch(ch);

	if (tsg == NULL) {
		nvgpu_log_info(g, "chid: %d is not bound to tsg", ch->chid);
		return 0;
	}

	nvgpu_log_info(g, "chid:%d tsgid:%d", ch->chid, tsg->tsgid);

	/* Preempt tsg. Channel preempt is NOOP */
	return g->ops.fifo.preempt_tsg(g, tsg);
}

int gv11b_fifo_preempt_tsg(struct gk20a *g, struct nvgpu_tsg *tsg)
{
	struct nvgpu_fifo *f = &g->fifo;
	int ret = 0;
#ifdef CONFIG_NVGPU_LS_PMU
	u32 token = PMU_INVALID_MUTEX_OWNER_ID;
	int mutex_ret = 0;
#endif
	u32 runlist_id;

	nvgpu_log_fn(g, "tsgid: %d", tsg->tsgid);

	runlist_id = tsg->runlist_id;
	nvgpu_log_fn(g, "runlist_id: %d", runlist_id);
	if (runlist_id == NVGPU_INVALID_RUNLIST_ID) {
		return 0;
	}

	nvgpu_mutex_acquire(&f->runlist_info[runlist_id]->runlist_lock);

	/* WAR for Bug 2065990 */
	nvgpu_tsg_disable_sched(g, tsg);
#ifdef CONFIG_NVGPU_LS_PMU
	mutex_ret = nvgpu_pmu_lock_acquire(g, g->pmu,
						PMU_MUTEX_ID_FIFO, &token);
#endif
	ret = gv11b_fifo_preempt_locked(g, tsg->tsgid, ID_TYPE_TSG);
#ifdef CONFIG_NVGPU_LS_PMU
	if (mutex_ret == 0) {
		int err = nvgpu_pmu_lock_release(g, g->pmu, PMU_MUTEX_ID_FIFO,
				&token);
		if (err != 0) {
			nvgpu_err(g, "PMU_MUTEX_ID_FIFO not released err=%d",
					err);
		}
	}
#endif
	/* WAR for Bug 2065990 */
	nvgpu_tsg_enable_sched(g, tsg);

	nvgpu_mutex_release(&f->runlist_info[runlist_id]->runlist_lock);

	if (ret != 0) {
		if (nvgpu_platform_is_silicon(g)) {
			nvgpu_err(g, "preempt timed out for tsgid: %u, "
			"ctxsw timeout will trigger recovery if needed",
			tsg->tsgid);
		} else {
			nvgpu_rc_preempt_timeout(g, tsg);
		}
	}

	return ret;
}
