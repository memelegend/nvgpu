/*
 * Virtualized GPU L2
 *
 * Copyright (c) 2014 NVIDIA CORPORATION.  All rights reserved.
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

#include "vgpu/vgpu.h"

static int vgpu_determine_L2_size_bytes(struct gk20a *g)
{
	struct gk20a_platform *platform = gk20a_get_platform(g->dev);
	u32 cache_size = 0;

	gk20a_dbg_fn("");

	if (vgpu_get_attribute(platform->virt_handle,
			TEGRA_VGPU_ATTRIB_L2_SIZE, &cache_size))
		dev_err(dev_from_gk20a(g), "unable to get L2 size");

	return cache_size;
}

static int vgpu_ltc_init_comptags(struct gk20a *g, struct gr_gk20a *gr)
{
	struct gk20a_platform *platform = gk20a_get_platform(g->dev);
	u32 max_comptag_lines = 0;

	gk20a_dbg_fn("");

	vgpu_get_attribute(platform->virt_handle,
			TEGRA_VGPU_ATTRIB_COMPTAG_LINES, &max_comptag_lines);
	if (max_comptag_lines < 2)
		return -ENXIO;

	gk20a_allocator_init(&gr->comp_tags, "comptag",
			      1, /* start */
			      max_comptag_lines - 1, /* length*/
			      1); /* align */
	return 0;
}

static const struct gpu_ltc_ops vgpu_ltc_ops = {
	.determine_L2_size_bytes = vgpu_determine_L2_size_bytes,
	.init_comptags = vgpu_ltc_init_comptags,
};

void vgpu_init_ltc_ops(struct gpu_ops *gops)
{
	gops->ltc = &vgpu_ltc_ops;
}
