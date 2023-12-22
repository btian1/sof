// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/audio/buffer.h>
#include <sof/audio/component_ext.h>
#include <sof/audio/pipeline.h>
#include <sof/ipc/msg.h>
#include <rtos/interrupt.h>
#include <sof/lib/mm_heap.h>
#include <sof/lib/uuid.h>
#include <sof/compiler_attributes.h>
#include <sof/list.h>
#include <rtos/spinlock.h>
#include <rtos/string.h>
#include <rtos/clk.h>
#include <ipc/header.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <ipc4/module.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(pipe, CONFIG_SOF_LOG_LEVEL);

/* 4e934adb-b0ec-4d33-a086-c1022f921321 */
DECLARE_SOF_RT_UUID("pipe", pipe_uuid, 0x4e934adb, 0xb0ec, 0x4d33,
		    0xa0, 0x86, 0xc1, 0x02, 0x2f, 0x92, 0x13, 0x21);

DECLARE_TR_CTX(pipe_tr, SOF_UUID(pipe_uuid), LOG_LEVEL_INFO);

/* the pipeline position lookup table */
static SHARED_DATA struct pipeline_posn pipeline_posn_shared;

void pipeline_posn_init(struct sof *sof)
{
	sof->pipeline_posn = platform_shared_get(&pipeline_posn_shared,
						 sizeof(pipeline_posn_shared));
	k_spinlock_init(&sof->pipeline_posn->lock);
}

/* create new pipeline - returns pipeline id or negative error */
struct pipeline *pipeline_new(uint32_t pipeline_id, uint32_t priority, uint32_t comp_id)
{
	struct sof_ipc_stream_posn posn;
	struct pipeline *p;
	int ret;

	pipe_cl_info("pipeline new pipe_id %d priority %d",
		     pipeline_id, priority);

	/* show heap status */
	heap_trace_all(0);

	/* allocate new pipeline */
	p = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*p));
	if (!p) {
		pipe_cl_err("pipeline_new(): Out of Memory");
		return NULL;
	}

	/* init pipeline */
	p->comp_id = comp_id;
	p->priority = priority;
	p->pipeline_id = pipeline_id;
	p->status = COMP_STATE_INIT;
	p->trigger.cmd = COMP_TRIGGER_NO_ACTION;
	ret = memcpy_s(&p->tctx, sizeof(struct tr_ctx), &pipe_tr,
		       sizeof(struct tr_ctx));
	if (ret < 0) {
		pipe_err(p, "pipeline_new(): failed to copy trace settings");
		goto free;
	}

	ret = pipeline_posn_offset_get(&p->posn_offset);
	if (ret < 0) {
		pipe_err(p, "pipeline_new(): pipeline_posn_offset_get failed %d",
			 ret);
		goto free;
	}

	/* just for retrieving valid ipc_msg header */
	ipc_build_stream_posn(&posn, SOF_IPC_STREAM_TRIG_XRUN, p->comp_id);

	if (posn.rhdr.hdr.size) {
		p->msg = ipc_msg_init(posn.rhdr.hdr.cmd, posn.rhdr.hdr.size);
		if (!p->msg) {
			pipe_err(p, "pipeline_new(): ipc_msg_init failed");
			goto free;
		}
	}

	return p;
free:
	rfree(p);
	return NULL;
}

static void buffer_set_comp(struct comp_buffer *buffer, struct comp_dev *comp,
			    int dir)
{
	if (dir == PPL_CONN_DIR_COMP_TO_BUFFER)
		buffer->source = comp;
	else
		buffer->sink = comp;
}

int pipeline_connect(struct comp_dev *comp, struct comp_buffer *buffer,
		     int dir)
{
	struct list_item *comp_list;
	uint32_t flags;

	if (dir == PPL_CONN_DIR_COMP_TO_BUFFER)
		comp_info(comp, "connect buffer %d as sink", buf_get_id(buffer));
	else
		comp_info(comp, "connect buffer %d as source", buf_get_id(buffer));

	irq_local_disable(flags);

	comp_list = comp_buffer_list(comp, dir);
	buffer_attach(buffer, comp_list, dir);
	buffer_set_comp(buffer, comp, dir);

	irq_local_enable(flags);

	return 0;
}

void pipeline_disconnect(struct comp_dev *comp, struct comp_buffer *buffer, int dir)
{
	struct list_item *comp_list;
	uint32_t flags;

	if (dir == PPL_CONN_DIR_COMP_TO_BUFFER)
		comp_dbg(comp, "disconnect buffer %d as sink", buf_get_id(buffer));
	else
		comp_dbg(comp, "disconnect buffer %d as source", buf_get_id(buffer));

	irq_local_disable(flags);

	comp_list = comp_buffer_list(comp, dir);
	buffer_detach(buffer, comp_list, dir);
	buffer_set_comp(buffer, NULL, dir);

	irq_local_enable(flags);
}

/* pipelines must be inactive */
int pipeline_free(struct pipeline *p)
{
	pipe_dbg(p, "pipeline_free()");

	/*
	 * pipeline_free should always be called only after all the widgets in the pipeline have
	 * been freed.
	 */

	/* remove from any scheduling */
	if (p->pipe_task) {
#if !CONFIG_LIBRARY || UNIT_TEST
		schedule_task_free(p->pipe_task);
#endif
		rfree(p->pipe_task);
	}

	ipc_msg_free(p->msg);

	pipeline_posn_offset_put(p->posn_offset);

	/* now free the pipeline */
	rfree(p);

	/* show heap status */
	heap_trace_all(0);

	return 0;
}

static int pipeline_comp_complete(struct comp_dev *current,
				  struct comp_buffer *calling_buf,
				  struct pipeline_walk_context *ctx, int dir)
{
	struct pipeline_data *ppl_data = ctx->comp_data;

	pipe_dbg(ppl_data->p, "pipeline_comp_complete(), current->comp.id = %u, dir = %u",
		 dev_comp_id(current), dir);

	if (!comp_is_single_pipeline(current, ppl_data->start)) {
		pipe_dbg(ppl_data->p, "pipeline_comp_complete(), current is from another pipeline");
		return 0;
	}

	/* complete component init */
	current->pipeline = ppl_data->p;
	/* LL module has its period always eq period of the pipeline
	 * DP period is set to 0 as sink format may not yet been set
	 * It will be calculated during module prepare operation
	 * either by the module or to default value based on module's OBS
	 */
	if (current->ipc_config.proc_domain == COMP_PROCESSING_DOMAIN_LL)
		current->period = ppl_data->p->period;

	current->priority = ppl_data->p->priority;

	return pipeline_for_each_comp(current, ctx, dir);
}

int pipeline_complete(struct pipeline *p, struct comp_dev *source,
		      struct comp_dev *sink)
{
	struct pipeline_data data;
	struct pipeline_walk_context walk_ctx = {
		.comp_func = pipeline_comp_complete,
		.comp_data = &data,
	};

#if !UNIT_TEST && !CONFIG_LIBRARY
	int freq = clock_get_freq(cpu_get_id());
#else
	int freq = 0;
#endif
	int ret;

	pipe_dbg(p, "pipeline complete, clock freq %dHz", freq);

	/* check whether pipeline is already completed */
	if (p->status != COMP_STATE_INIT) {
		pipe_err(p, "pipeline_complete(): Pipeline already completed");
		return -EINVAL;
	}

	data.start = source;
	data.p = p;

	/* now walk downstream from source component and
	 * complete component task and pipeline initialization
	 */
	ret = walk_ctx.comp_func(source, NULL, &walk_ctx, PPL_DIR_DOWNSTREAM);

	p->source_comp = source;
	p->sink_comp = sink;
	p->status = COMP_STATE_READY;

	/* show heap status */
	heap_trace_all(0);

	return ret;
}

static int pipeline_comp_reset(struct comp_dev *current,
			       struct comp_buffer *calling_buf,
			       struct pipeline_walk_context *ctx, int dir)
{
	struct pipeline *p = ctx->comp_data;
	struct pipeline *p_current = current->pipeline;
	int is_single_ppl = comp_is_single_pipeline(current, p->source_comp);
	int is_same_sched = pipeline_is_same_sched_comp(p_current, p);
	int err;

	pipe_dbg(p_current, "pipeline_comp_reset(), current->comp.id = %u, dir = %u",
		 dev_comp_id(current), dir);

	/*
	 * Reset should propagate to the connected pipelines, which need to be
	 * scheduled together, except for IPC4, where each pipeline receives
	 * commands from the host separately
	 */
	if (!is_single_ppl && IPC4_MOD_ID(current->ipc_config.id))
		return 0;

	/* Propagate reset across pipelines only in the same direction
	 * and with the same scheduling behavior
	 */
	if (!is_single_ppl && !is_same_sched && !comp_same_dir(current, dir))
		return 0;

	/* two cases for a component still being active here:
	 * 1. trigger function failed to handle stop event
	 * 2. trigger functon skipped due to error of other component's trigger function
	 */
	if (current->state == COMP_STATE_ACTIVE) {
		pipe_warn(current->pipeline, "pipeline_comp_reset(): component is in active state, try to stop it");
		err = comp_trigger(current, COMP_TRIGGER_STOP);
		if (err)
			pipe_err(current->pipeline, "pipeline_comp_reset(): failed to recover");
	}

	err = comp_reset(current);
	if (err < 0 || err == PPL_STATUS_PATH_STOP)
		return err;

	return pipeline_for_each_comp(current, ctx, dir);
}

/* reset the whole pipeline */
int pipeline_reset(struct pipeline *p, struct comp_dev *host)
{
	struct pipeline_walk_context walk_ctx = {
		.comp_func = pipeline_comp_reset,
		.comp_data = p,
		.buff_func = buffer_reset_params,
		.skip_incomplete = true,
	};
	int ret;

	pipe_dbg(p, "pipe reset");

	ret = walk_ctx.comp_func(host, NULL, &walk_ctx, host->direction);
	if (ret < 0) {
		pipe_err(p, "pipeline_reset(): ret = %d, host->comp.id = %u",
			 ret, dev_comp_id(host));
	} else {
		 /* pipeline is reset to default state */
		p->status = COMP_STATE_READY;
	}

	return ret;
}

/* Generic method for walking the graph upstream or downstream.
 * It requires function pointer for recursion.
 */
int pipeline_for_each_comp(struct comp_dev *current,
			   struct pipeline_walk_context *ctx, int dir)
{
	struct list_item *buffer_list = comp_buffer_list(current, dir);
	struct list_item *clist;

	/* run this operation further */
	list_for_item(clist, buffer_list) {
		struct comp_buffer *buffer = buffer_from_list(clist, dir);
		struct comp_dev *buffer_comp;
		int err = 0;

		if (ctx->incoming == buffer)
			continue;

		/* don't go back to the buffer which already walked */
		/*
		 * Note, that this access must be performed unlocked via
		 * uncached address. Trying to lock before checking the flag
		 * understandably leads to a deadlock when this function is
		 * called recursively from .comp_func() below. We do it in a
		 * safe way: this flag must *only* be accessed in this function
		 * only in these three cases: testing, setting and clearing.
		 * Note, that it is also assumed that buffers aren't shared
		 * across CPUs. See further comment below.
		 */
		dcache_writeback_invalidate_region(uncache_to_cache(buffer), sizeof(*buffer));
		if (buffer->walking)
			continue;

		buffer_comp = buffer_get_comp(buffer, dir);

		/* execute operation on buffer */
		if (ctx->buff_func)
			ctx->buff_func(buffer, ctx->buff_data);

		/* don't go further if this component is not connected */
		if (buffer_comp &&
		    (!ctx->skip_incomplete || buffer_comp->pipeline) &&
		    ctx->comp_func) {
			buffer->walking = true;

			err = ctx->comp_func(buffer_comp, buffer,
					     ctx, dir);

			buffer->walking = false;
		}

		if (err < 0 || err == PPL_STATUS_PATH_STOP)
			return err;
	}

	return 0;
}

/* visit connected pipeline to find the dai comp */
struct comp_dev *pipeline_get_dai_comp(uint32_t pipeline_id, int dir)
{
	struct ipc_comp_dev *crt;
	struct ipc *ipc = ipc_get();

	crt = ipc_get_ppl_comp(ipc, pipeline_id, dir);
	while (crt) {
		struct comp_buffer *buffer;
		struct comp_dev *comp;
		struct list_item *blist = comp_buffer_list(crt->cd, dir);

		/* if buffer list is empty then we have found a DAI */
		if (list_is_empty(blist))
			return crt->cd;

		buffer = buffer_from_list(blist->next, dir);
		comp = buffer_get_comp(buffer, dir);

		/* buffer_comp is in another pipeline and it is not complete */
		if (!comp->pipeline)
			return NULL;

		crt = ipc_get_ppl_comp(ipc, comp->pipeline->pipeline_id, dir);
	}

	return NULL;
}
