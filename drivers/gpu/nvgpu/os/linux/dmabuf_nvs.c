/*
 * Copyright (c) 2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/version.h>
#include <linux/dma-buf.h>
#include <uapi/linux/nvgpu.h>

#ifdef CONFIG_NVGPU_USE_TEGRA_ALLOC_FD
#include <linux/platform/tegra/tegra_fd.h>
#endif

#include <nvgpu/dma.h>
#include <nvgpu/enabled.h>
#include <nvgpu/nvgpu_mem.h>
#include <nvgpu/kmem.h>
#include <nvgpu/gk20a.h>
#include <nvgpu/nvgpu_init.h>

#include <nvgpu/linux/vm.h>
#include <nvgpu/linux/dma.h>
#include <nvgpu/nvs.h>

#include "dmabuf_nvs.h"

/* This constant string is used to determine if the dmabuf belongs
 * to nvgpu.
 */
static const char exporter_name[] = "nvgpu_ctrl_fifo";

static struct nvgpu_nvs_domain_ctrl_fifo_user_vma *
		nvgpu_nvs_domain_ctrl_fifo_user_vma_from_node(struct nvgpu_list_node *node)
{
	nvgpu_assert(node != NULL);
	return container_of(node, struct nvgpu_nvs_domain_ctrl_fifo_user_vma, node);
}

static int zap_vma_entries(struct gk20a *g, struct vm_area_struct *vma)
{
	int err = 0;

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
		mmap_write_lock(vma->vm_mm);
	#else
		down_write(&vma->vm_mm->mmap_sem);
	#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
		zap_vma_ptes(vma, vma->vm_start, vma->vm_end - vma->vm_start);
#else
		err = zap_vma_ptes(vma, vma->vm_start,
				   vma->vm_end - vma->vm_start);
#endif

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
		mmap_write_unlock(vma->vm_mm);
	#else
		up_write(&vma->vm_mm->mmap_sem);
	#endif

	return err;
}

static int nvs_release_user_mappings_locked(struct gk20a *g, struct nvgpu_nvs_linux_buf_priv *linux_buf)
{
	struct nvgpu_nvs_domain_ctrl_fifo_user_vma *current_entry, *next_entry;
	int err = 0;

	nvgpu_list_for_each_entry_safe(current_entry, next_entry,
			&linux_buf->list_mapped_user_vmas,
			nvgpu_nvs_domain_ctrl_fifo_user_vma, node) {
		struct vm_area_struct *vma = current_entry->vma;

		zap_vma_entries(g, vma);
		linux_buf->ref--;
	}

	return err;
}

static void nvs_vma_close(struct vm_area_struct *vma)
{
	struct nvgpu_nvs_domain_ctrl_fifo_user_vma *vma_metadata = vma->vm_private_data;
	struct nvgpu_nvs_ctrl_queue *buf = vma_metadata->buf;
	struct nvgpu_nvs_linux_buf_priv *linux_buf = (struct nvgpu_nvs_linux_buf_priv *)buf->priv;
	struct gk20a *g = buf->g;

	nvgpu_nvs_ctrl_fifo_lock_queues(g);

	linux_buf->ref--;
	nvgpu_list_del(&vma_metadata->node);

	/* This VMA is freed now and points to invalid ptes */
	vma_metadata->vma = NULL;
	nvgpu_kfree(g, vma_metadata);
	vma->vm_private_data = NULL;

	nvgpu_nvs_ctrl_fifo_unlock_queues(g);

	nvgpu_put(g);
}

const struct vm_operations_struct nvs_vma_ops = {
	/* no .open - we use VM_DONTCOPY and don't support fork */
	.close = nvs_vma_close,
};

static int nvgpu_nvs_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct nvgpu_nvs_ctrl_queue *buf = dmabuf->priv;
	struct nvgpu_nvs_linux_buf_priv *linux_buf = (struct nvgpu_nvs_linux_buf_priv *)buf->priv;
	struct gk20a *g = buf->g;
	struct nvgpu_mem *mem = &buf->mem;
	struct nvgpu_nvs_domain_ctrl_fifo_user_vma *vma_metadata = NULL;
	int err;
	const struct vm_operations_struct *vm_ops_old = NULL;

	if (vma->vm_end - vma->vm_start > buf->mem.size) {
		return -EINVAL;
	}

	if (vma->vm_pgoff != 0UL) {
		return -EINVAL;
	}

	/*
	 * This ref is released when the mapping is removed.
	 */
	if (!nvgpu_get(g))
		return -ENODEV;

	nvgpu_nvs_ctrl_fifo_lock_queues(g);

	vma->vm_flags |= VM_DONTCOPY | VM_DONTEXPAND | VM_NORESERVE |
		VM_DONTDUMP;

	if (linux_buf->read_only) {
		vma->vm_flags |= VM_SHARED;
	}

	vm_ops_old = vma->vm_ops;

	vma->vm_ops = &nvs_vma_ops;

	err = nvgpu_dma_mmap_sys(g, vma, mem);
	if (err != 0) {
		goto fail;
	}

	vma_metadata = nvgpu_kzalloc(g, sizeof(*vma_metadata));
	if (vma_metadata == NULL) {
		err = -ENOMEM;
		zap_vma_entries(g, vma);
		goto fail;
	}

	vma_metadata->vma = vma;
	vma_metadata->buf = buf;
	nvgpu_init_list_node(&vma_metadata->node);

	linux_buf->ref++;
	nvgpu_list_add_tail(&vma_metadata->node, &linux_buf->list_mapped_user_vmas);

	vma->vm_private_data = vma_metadata;

	nvgpu_nvs_ctrl_fifo_unlock_queues(g);

	return 0;
fail:
	vma->vm_ops = vm_ops_old;

	if (vma_metadata != NULL) {
		nvgpu_kfree(g, vma_metadata);
	}

	nvgpu_nvs_ctrl_fifo_unlock_queues(g);

	return err;
}

static struct dma_buf_ops gk20a_nvs_ops = {
	.mmap             = nvgpu_nvs_buf_mmap,
};

static struct dma_buf *nvgpu_nvs_buf_export_dmabuf(struct nvgpu_nvs_ctrl_queue *buf,
		bool read_only)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct nvgpu_mem *mem = &buf->mem;

	int flags = (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC;

	exp_info.priv = buf;
	exp_info.ops = &gk20a_nvs_ops;
	exp_info.size = mem->size;
	exp_info.flags = flags;
	exp_info.exp_name = exporter_name;

	return dma_buf_export(&exp_info);
}

static void nvgpu_nvs_destroy_buf_linux_locked(struct gk20a *g, struct nvgpu_nvs_ctrl_queue *buf)
{
	struct nvgpu_nvs_domain_ctrl_fifo *sched_ctrl = g->sched_ctrl_fifo;
	struct nvgpu_nvs_linux_buf_priv *priv;

	priv = (struct nvgpu_nvs_linux_buf_priv *)buf->priv;

	nvs_release_user_mappings_locked(g, priv);

	dma_buf_put(priv->dmabuf);

	nvgpu_nvs_buffer_free(sched_ctrl, buf);
	nvgpu_kfree(g, priv);

	nvgpu_put(g);
}

bool nvgpu_nvs_buf_linux_is_mapped(struct gk20a *g, struct nvgpu_nvs_ctrl_queue *buf)
{
	bool is_mapped;
	struct nvgpu_nvs_linux_buf_priv *priv = NULL;

	priv = (struct nvgpu_nvs_linux_buf_priv *)buf->priv;
	is_mapped = (priv->ref != 0U);

	return is_mapped;
}

int nvgpu_nvs_get_buf_linux(struct gk20a *g, struct nvgpu_nvs_ctrl_queue *buf,
		size_t bytes, u8 mask, bool read_only)
{
	struct nvgpu_nvs_linux_buf_priv *priv;
	int err;
	struct nvgpu_nvs_domain_ctrl_fifo *sched_ctrl = g->sched_ctrl_fifo;

	/*
	 * This ref is released when the dma_buf is closed.
	 */
	if (!nvgpu_get(g))
		return -ENODEV;

	nvs_dbg(g, "Allocating buf: %zu bytes", bytes);

	priv = nvgpu_kzalloc(g, sizeof(*priv));
	if (!priv) {
		err = -ENOMEM;
		goto fail;
	}

	nvgpu_init_list_node(&priv->list_mapped_user_vmas);
	priv->read_only = read_only;

	err = nvgpu_nvs_buffer_alloc(sched_ctrl, bytes, mask, buf);
	if (err != 0) {
		nvgpu_err(g, "Unable to alloc memory");
		goto fail;
	}

	priv->dmabuf = nvgpu_nvs_buf_export_dmabuf(buf, read_only);
	if (IS_ERR(priv->dmabuf)) {
		nvgpu_err(g, "Unable to export dma buf");
		err = PTR_ERR(priv->dmabuf);
		goto fail;
	}

	buf->priv = priv;
	buf->free = nvgpu_nvs_destroy_buf_linux_locked;

	return 0;
fail:
	nvgpu_kfree(g, priv);
	nvgpu_nvs_buffer_free(sched_ctrl, buf);

	nvgpu_put(g);

	return err;
}
