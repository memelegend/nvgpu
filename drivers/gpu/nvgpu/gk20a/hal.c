/*
 * NVIDIA GPU HAL interface.
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
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

#include "gk20a.h"
#include "hal_gk20a.h"
#include "gm20b/hal_gm20b.h"

int gpu_init_hal(struct gk20a *g)
{
	u32 ver = g->gpu_characteristics.arch + g->gpu_characteristics.impl;
	gk20a_dbg_fn("ver=0x%x", ver);
	switch (ver) {
	case NVGPU_GPUID_GK20A:
		if (gk20a_init_hal(&g->ops))
			return -ENODEV;
		break;
	case NVGPU_GPUID_GM20B:
		if (gm20b_init_hal(&g->ops))
			return -ENODEV;
		break;
	default:
		gk20a_err(&g->dev->dev, "no support for %x", ver);
		return -ENODEV;
	}

	return 0;
}
