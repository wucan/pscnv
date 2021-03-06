/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 PathScale Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
  * VRAM can be divide into two parts, non-page and page parts.
  * non-page includes scanout hardware-status page etc which will not be evicted.
  * page parts for user bos.
  * All the alloc, free, evicted, bind/unbind are for the page parts.
  */

/*
  * For bo alloc/free, I use page as unit and current drm_mm (buddy), still need to find a algorithm to find the free page more efficiently and less fragment. Still considering multi-processor, fast, high-usage.
  * For bo evict/replacement, I use bo as unit, modified CAR to do replacement.  Using bo is because it's simple and don't need to spend overhead to check each page. The disadvantage is if we need to swap, large bo is slower than part of bo.
  *
  * Rambling thought:
  * drm_mm bo alloc can't full use the VRAM, has some fragment. For using all, we need add a function to collect the fragment, when free_block_num is OK, but get_new_space failed. 
  * Does CAR good for replacement? This algorithm is for page, so for bo is it still good?
  * Features: I think most of the bos are used only once. swap cost to much.
  */


#include "nouveau_pscmm.h"


#define USE_REFCNT (dev_priv->card_type >= NV_10)
//static timeout_id_t worktimer_id;
#define	DRM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define	DRM_MAX(a, b) ((a) > (b) ? (a) : (b))

static void nouveau_free_blocks(struct nouveau_bo* nvbo);
static uint32_t nouveau_pscmm_add_request(struct drm_device *dev);
int nouveau_pscmm_set_no_evicted(struct drm_nouveau_private *dev_priv,
                                        struct nouveau_bo *nvbo);
uintptr_t *nouveau_get_new_space(struct nouveau_bo *nvbo,
		struct drm_nouveau_private* dev_priv, uint32_t bnum, uint32_t align);
void nouveau_pscmm_free(struct nouveau_bo* nvbo);
int nouveau_pscmm_object_unbind(struct nouveau_bo* nvbo, uint32_t type);
static void
nouveau_pscmm_object_free_page_list(struct nouveau_bo* nvbo);




int
nouveau_gem_object_new(struct drm_gem_object *gem)
{
	return 0;
}

void
nouveau_gem_object_del(struct drm_gem_object *gem)
{
	struct nouveau_bo *nvbo = gem->driver_private;

	if (!nvbo)
		return;

	/* unpin bo */
	if (nvbo->agp_mem) {
		nouveau_pscmm_object_unbind(nvbo, 1);
	}
	/* release bo */
	nouveau_pscmm_free(nvbo);
}

static inline void
nouveau_update_directory(struct nouveau_bo *nvbo, struct drm_nouveau_private* dev_priv)
{
	struct nouveau_bo *temp;

	if ((nvbo->type != B1) && (nvbo->type != B2) &&
		((dev_priv->B1_num + dev_priv->T1_num) >= dev_priv->total_block_num)) {
	
		temp = list_first_entry(&dev_priv->B1_list, struct nouveau_bo, list);
		list_del(&temp->list);
		dev_priv->B1_num -= temp->nblock;
	} else if ((nvbo->type != B1) && (nvbo->type != B2) &&
			((dev_priv->B1_num + dev_priv->T1_num + 
			dev_priv->B2_num + dev_priv->T2_num) >= dev_priv->total_block_num * 2)) {
		temp = list_first_entry(&dev_priv->B2_list, struct nouveau_bo, list);
		list_del(&temp->list);
		dev_priv->B2_num -= temp->nblock;
	}
}


uintptr_t *
nouveau_evict_somthing(struct nouveau_bo *nvbo, struct drm_nouveau_private* dev_priv,
		uint32_t bnum, uint32_t align)
{

	struct nouveau_bo *nvbo_tmp;
	int i, j;
	int num = 0;
	int found;

again:
	
	found = 0;
	do {
		if (dev_priv->T1_num >= DRM_MAX(1, dev_priv->p)) {
			nvbo_tmp = list_first_entry(&dev_priv->T1_list, struct nouveau_bo, list);
			if (nvbo_tmp->bo_ref == 0) {
				found = 1;
				/* swap nvbo */

				list_move_tail(&nvbo_tmp->list, &dev_priv->B1_list);
				dev_priv->T1_num-=nvbo_tmp->nblock;
				dev_priv->B1_num+=nvbo_tmp->nblock;
				nvbo_tmp->type = B1;
				nvbo_tmp->placements;
			} else {
				list_move_tail(&nvbo_tmp->list, &dev_priv->T2_list);
				dev_priv->T1_num-=nvbo_tmp->nblock;
				dev_priv->T2_num+=nvbo_tmp->nblock;
				nvbo_tmp->type = T2;
			}
		} else {
			nvbo_tmp = list_first_entry(&dev_priv->T2_list, struct nouveau_bo, list);
			if (nvbo_tmp->bo_ref == 0) {
				found = 1;
				/* swap nvbo */

				list_move_tail(&nvbo_tmp->list, &dev_priv->B2_list);
				dev_priv->T2_num-=nvbo_tmp->nblock;
				dev_priv->B2_num+=nvbo_tmp->nblock;
				nvbo_tmp->type = B2;
				nvbo_tmp->placements;
			} else {
				list_move_tail(&nvbo->list, &dev_priv->T2_list);
			}
		}
	} while(found);

	nvbo_tmp->swap_out = true;

	nouveau_free_blocks(nvbo_tmp);

	dev_priv->free_block_num += nvbo_tmp->nblock;

	if (num < bnum)
		goto again;

	return nouveau_get_new_space(nvbo, dev_priv, bnum, align);
}

/*
  * need a algorithm to find the free page more efficiently and less fragment.
  */
uintptr_t *
nouveau_get_new_space(struct nouveau_bo *nvbo,
		struct drm_nouveau_private* dev_priv, uint32_t bnum, uint32_t align)
{

/*
  * using drm_mm to do VRAM memory pages part alloc
  * need to modified to avoid the fragment
  */
  	struct drm_mm_node *free_space;
	uintptr_t *block_array;
	int i;

	free_space = drm_mm_search_free(&dev_priv->fb_block->core_manager, bnum, align, 0);

	if (free_space != NULL) {
		nvbo->block_offset_node = drm_mm_get_block(free_space, bnum, align);
	}

	if (nvbo->block_offset_node == NULL) {
		block_array = nouveau_evict_somthing(nvbo, dev_priv, bnum, align);
		nouveau_update_directory(nvbo, dev_priv);

	} else {
		block_array = kzalloc(sizeof(uintptr_t) * bnum, GFP_KERNEL);
		for (i = 0; i < bnum; i++) {
			block_array[i] = nvbo->block_offset_node->start + PAGE_SIZE * i;
		}
	}
	return block_array;

}

uintptr_t *
nouveau_find_mem_space(struct drm_nouveau_private* dev_priv, struct nouveau_bo *nvbo,
					uint32_t bnum, bool no_evicted, uint32_t align)
{

/*
  * Using modified CAR http://www.almaden.ibm.com/cs/people/dmodha/clockfast.pdf
  * Different: 
  * 1. c: total block number
  * 2. bo as schedule unit
  * 3. New list for non-evicted bo.  bo from non-evicted list will be added into tail of T1/T2 according to old_type and ref = 1
  * 4. CAR is used for replacement, need a good algorithm to search free page in VRAM more efficiently, using drm_mm?
  * 5. if the bo was del, it will be removed directly from list.
  */
	struct nouveau_bo *temp;
	uintptr_t *block_array;
	if (bnum <= dev_priv->free_block_num) {
		
		block_array = nouveau_get_new_space(nvbo, dev_priv, bnum, align); /* ret = -1 no mem*/

	} else {

		block_array = nouveau_evict_somthing(nvbo, dev_priv, bnum, align);
		//update cache directory replacemet
		nouveau_update_directory(nvbo, dev_priv);

	}

	if ((nvbo->type != B1) && (nvbo->type != B2)) {
		list_add_tail(&nvbo->list, &dev_priv->T1_list);
		dev_priv->T1_num += bnum;
		nvbo->type = T1;
		nvbo->bo_ref = 0;
	} else if (nvbo->type == B1) {
		/* adapt */
		dev_priv->p = DRM_MIN(dev_priv->p + DRM_MAX(1, dev_priv->B2_num/dev_priv->B1_num), dev_priv->total_block_num);

		list_move_tail(&nvbo->list, &dev_priv->T2_list);
		dev_priv->B1_num-=nvbo->nblock;
		dev_priv->T2_num+=nvbo->nblock;
		nvbo->type = T2;
		nvbo->bo_ref = 0;
		
	} else {
		/* adapt */
		dev_priv->p = DRM_MAX(dev_priv->p - DRM_MAX(1, dev_priv->B1_num/dev_priv->B2_num), 0);

		list_move_tail(&nvbo->list, &dev_priv->T2_list);
		dev_priv->B2_num-=nvbo->nblock;
		dev_priv->T2_num+=nvbo->nblock;
		nvbo->type = T2;
		nvbo->bo_ref = 0;	
	}
	dev_priv->free_block_num -= bnum;

	if (no_evicted) {
		nouveau_pscmm_set_no_evicted(dev_priv, nvbo);
	}
	return block_array;
}

/*
  * Update each block's status - pin/unpin
  */
static void
nouveau_free_blocks(struct nouveau_bo* nvbo)
{
	int i;
	int nblock;

	drm_mm_put_block(nvbo->block_offset_node);
	nvbo->block_offset_node = NULL;
}

static int
nouveau_pscmm_object_get_page_list(struct nouveau_bo* nvbo)
{
	int page_count, i;
	struct address_space *mapping;
	struct inode *inode;
	struct page *page;
	int ret;

	if (nvbo->pages_refcount++ != 0)
		return 0;

        page_count = nvbo->gem->size / PAGE_SIZE;

	nvbo->pages = drm_calloc_large(page_count, sizeof(struct page *));
	if (nvbo->pages == NULL) {
		nvbo->pages_refcount--;
		return -ENOMEM;
	}
	inode = nvbo->gem->filp->f_path.dentry->d_inode;
	mapping = inode->i_mapping;
	for (i = 0; i < page_count; i++) {
		page = read_mapping_page(mapping, i, NULL);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			nouveau_pscmm_object_free_page_list(nvbo);
			return ret;
		}
		nvbo->pages[i] = page;
	}

	return 0;
}

static void
nouveau_pscmm_object_free_page_list(struct nouveau_bo* nvbo)
{
	int page_count = nvbo->gem->size / PAGE_SIZE;
	int i;

	BUG_ON(nvbo->pages_refcount == 0);

	if (--nvbo->pages_refcount != 0)
		return;
	for (i = 0; i < page_count; i++) {
		if (nvbo->pages[i] == NULL)
			break;

		if (nvbo->dirty)
			set_page_dirty(nvbo->pages[i]);
		page_cache_release(nvbo->pages[i]);
	}
	nvbo->dirty = 0;
	drm_free_large(nvbo->pages);
	nvbo->pages = NULL;
}

int
nouveau_pscmm_object_bind_to_gart(struct nouveau_bo* nvbo, uint32_t alignment)
{
	struct drm_device *dev = nvbo->gem->dev;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_mm_node *free_space;
	int page_count;
	int ret;

	if (alignment == 0)
		alignment = PAGE_SIZE;
	if (alignment & (PAGE_SIZE - 1)) {
		DRM_ERROR("Invalid object alignment requested %u\n", alignment);
		return -EINVAL;
	}

	if (nvbo->gart_space) {
		DRM_ERROR("Already bind!!");
		return 0;
	}

search_free:
	free_space = drm_mm_search_free(&dev_priv->fb_block->gart_manager,
				(unsigned long) nvbo->gem->size, alignment, 0);
	if (free_space != NULL) {
		nvbo->gart_space = drm_mm_get_block(free_space,
				(unsigned long) nvbo->gem->size, alignment);
	}
	if (nvbo->gart_space == NULL) {
		/* need to add some code to evict the bo in gart */
		/* fix me!!!! */
		DRM_ERROR("GART Full");
		return -ENOMEM;
	} else {
		nvbo->gart_offset = nvbo->gart_space->start;
	}

	ret = nouveau_pscmm_object_get_page_list(nvbo);
	if (ret) {
		drm_mm_put_block(nvbo->gart_space);
		nvbo->gart_space = NULL;
		DRM_ERROR("bind to gtt failed to get page list");
		return ret;
	}

	page_count = nvbo->gem->size / PAGE_SIZE;
	/* Create an AGP memory structure pointing at our pages, and bind it
	 * into the GTT.
	 */
	nvbo->agp_mem = drm_agp_bind_pages(dev,
				      	 nvbo->pages,
       					 page_count,
       					 nvbo->gart_offset,
       					 AGP_USER_MEMORY);
	if (nvbo->agp_mem == NULL) {
		nouveau_pscmm_object_free_page_list(nvbo);
		drm_mm_put_block(nvbo->gart_space);
		nvbo->gart_space = NULL;
		DRM_ERROR("Failed to bind pages ");
		return -ENOMEM;
	}

	dev_priv->gart_info.aper_free -= nvbo->gem->size;

	return ret;
}

int
nouveau_pscmm_object_unbind(struct nouveau_bo* nvbo, uint32_t type)
{
        struct drm_device *dev = nvbo->gem->dev;
        struct drm_nouveau_private *dev_priv = dev->dev_private;
        int ret;

        if (nvbo->gart_space) {
                return 0;
        }

	/* wait rendering done */



	/* unbind from GART */
	if (nvbo->agp_mem != NULL) {
		drm_unbind_agp(nvbo->agp_mem);
		drm_free_agp(nvbo->agp_mem, nvbo->gem->size / PAGE_SIZE);
		nvbo->agp_mem = NULL;
	}

	nouveau_pscmm_object_free_page_list(nvbo);

	if (nvbo->gart_space) {
		drm_mm_put_block(nvbo->gart_space);
		nvbo->gart_space = NULL;
		dev_priv->gart_info.aper_free += nvbo->gem->size;
	}

	return 0;
}

/* Alloc mem from VRAM */
struct nouveau_bo *
nouveau_pscmm_alloc_vram(struct drm_nouveau_private* dev_priv, size_t bo_size,
		bool no_evicted, uint32_t align)
{
	struct nouveau_bo *nvbo;
	uintptr_t *block_array;
	uint32_t bnum;
	int ret;

	bnum = bo_size / PAGE_SIZE;
	nvbo = kzalloc(sizeof(struct nouveau_bo), GFP_KERNEL);
	INIT_LIST_HEAD(&nvbo->head);
	INIT_LIST_HEAD(&nvbo->list);
	INIT_LIST_HEAD(&nvbo->active_list);
	nvbo->block_offset_node = NULL;
	/* find the blank VRAM and reserve */
	block_array = nouveau_find_mem_space(dev_priv, nvbo, bnum, no_evicted, align);

	nvbo->channel = NULL;

	nvbo->nblock = bnum;		//VRAM is split in block 

	nvbo->block_array = block_array;			//the GPU physical address at which the bo is

	return nvbo;
}

void
nouveau_pscmm_free(struct nouveau_bo* nvbo)
{
	if (nvbo->block_offset_node != NULL) {
	/* remove from T1/T2/B1/B2 */
		nouveau_free_blocks(nvbo);
	}
	if (nvbo->block_array != NULL) {
		kfree(nvbo->block_array);
		nvbo->block_array = NULL;
	}
	nvbo->gem = NULL;
	kfree(nvbo);
}

uintptr_t
nouveau_channel_map(struct drm_device *dev, struct nouveau_channel *chan, struct nouveau_bo *nvbo, 
			uint32_t low, uint32_t tile_flags)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	uintptr_t addr_ptr;
	int ret;

	/* Get free vm from per-channel page table using the function in drm_mm.c or bitmap_block*/
	/* bind the vm with the physical address in block_array */
	/* Since the per-channel page table init at the channel_init and not changed then, */
	addr_ptr = nvbo->block_offset_node->start + dev_priv->vm_vram_base;

	/* bind the vm */
	/* need? */
	ret = nv50_mem_vm_bind_linear(dev,
			nvbo->block_offset_node->start + dev_priv->vm_vram_base,
			nvbo->gem->size, nvbo->tile_flags,
			nvbo->block_offset_node->start);
	if (ret) {
		NV_ERROR(dev, "Failed to bind");
			return NULL;
	}
	/* if use bitmap_block then update in nouveau_channel, no need by now*/


	nvbo->channel = chan;
	nvbo->tile_flags = tile_flags;
	nvbo->firstblock = addr_ptr;
	nvbo->low = low;
		
	return addr_ptr;
}

int
nouveau_channel_unmap(struct drm_device *dev,
							struct nouveau_channel *chan, struct nouveau_bo *nvbo)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	int ret;
	
	/* unbind */
	/* need? */
	nv50_mem_vm_unbind(dev, nvbo->block_offset_node->start + dev_priv->vm_vram_base, nvbo->gem->size);
	/* drm_mm_put_block or update the bitmap_block in nouveau_channel */
	nvbo->channel = NULL;
	
	return ret;
}

static int
nouveau_pscmm_move_memcpy(struct drm_device *dev,  
			struct drm_gem_object* gem, struct nouveau_bo *nvbo, 
			uint32_t old_domain, uint32_t new_domain)
{
NV_ERROR(dev, "not support software copy");
return -1;
#if 0
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	/* RAM->VRAM & VRAM->RAM */
	if (nvbo->virtual == NULL)
		nvbo->virtual = ioremap(dev_priv->fb_block->io_offset + nvbo->block_offset_node->start,  gem->size);
	
	if (new_domain == NOUVEAU_PSCMM_DOMAIN_VRAM) {
		memcpy(gem->kaddr, nvbo->virtual, gem->size);
	} else if (old_domain == NOUVEAU_PSCMM_DOMAIN_VRAM) {
		memcpy(nvbo->virtual, gem->kaddr, gem->size);
	} else 
		NV_ERROR(dev, "Error %d -> %d copy", old_domain, new_domain);
	return 0;
#endif
}

static inline uint32_t
nouveau_bo_mem_ctxdma(struct nouveau_bo *nvbo, struct nouveau_channel *chan,
		      uint32_t domain)
{

	if (chan == nvbo->channel) {
		if (domain != NOUVEAU_PSCMM_DOMAIN_VRAM) {
			return chan->gart_handle;
		}
		return chan->vram_handle;
	}

	if (domain != NOUVEAU_PSCMM_DOMAIN_VRAM) {
			return NvDmaGART;
	}
	return NvDmaVRAM;

}

static int
nouveau_pscmm_move_m2mf(struct drm_device *dev,  
		struct drm_gem_object* gem, struct nouveau_bo *nvbo, 
		uint32_t old_domain, uint32_t new_domain, bool no_evicted)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_channel *chan;
	uint64_t src_offset, dst_offset;
	uint32_t page_count;
	int ret;

	chan = nvbo->channel;
	if (!chan || nvbo->tile_flags || nvbo->no_vm)
		chan = dev_priv->channel;


	if ((old_domain == NOUVEAU_PSCMM_DOMAIN_CPU) ||
		(new_domain == NOUVEAU_PSCMM_DOMAIN_CPU)) {
		nouveau_pscmm_object_bind_to_gart(nvbo, PAGE_SIZE);
	}

	if (old_domain == NOUVEAU_PSCMM_DOMAIN_VRAM) {
		src_offset = nvbo->block_offset_node->start + (chan == dev_priv->channel) ? 0 : dev_priv->vm_vram_base;
		dst_offset = nvbo->gart_offset + (chan == dev_priv->channel) ? 0 : dev_priv->vm_gart_base;

	} else {
		src_offset = nvbo->gart_offset + (chan == dev_priv->channel) ? 0 : dev_priv->vm_gart_base;
		dst_offset = nvbo->block_offset_node->start + (chan == dev_priv->channel) ? 0 : dev_priv->vm_vram_base;
		
	}

	ret = RING_SPACE(chan, 3);
	if (ret)
		return ret;
	BEGIN_RING(chan, NvSubM2MF, NV_MEMORY_TO_MEMORY_FORMAT_DMA_SOURCE, 2);
	OUT_RING(chan, nouveau_bo_mem_ctxdma(nvbo, chan, old_domain));
	OUT_RING(chan, nouveau_bo_mem_ctxdma(nvbo, chan, new_domain));

	if (dev_priv->card_type >= NV_50) {
		ret = RING_SPACE(chan, 4);
		if (ret)
			return ret;
		BEGIN_RING(chan, NvSubM2MF, 0x0200, 1);
		OUT_RING(chan, 1);
		BEGIN_RING(chan, NvSubM2MF, 0x021c, 1);
		OUT_RING(chan, 1);
	}

	page_count = (nvbo->gem->size) / PAGE_SIZE;
	while (page_count) {
		int line_count = (page_count > 2047) ? 2047 : page_count;

		if (dev_priv->card_type >= NV_50) {
			ret = RING_SPACE(chan, 3);
			if (ret)
				return ret;
			BEGIN_RING(chan, NvSubM2MF, 0x0238, 2);
			OUT_RING(chan, upper_32_bits(src_offset));
			OUT_RING(chan, upper_32_bits(dst_offset));
		}
		ret = RING_SPACE(chan, 11);
		if (ret)
			return ret;
		BEGIN_RING(chan, NvSubM2MF,
				 NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
		OUT_RING(chan, lower_32_bits(src_offset));
		OUT_RING(chan, lower_32_bits(dst_offset));
		OUT_RING(chan, PAGE_SIZE); /* src_pitch */
		OUT_RING(chan, PAGE_SIZE); /* dst_pitch */
		OUT_RING(chan, PAGE_SIZE); /* line_length */
		OUT_RING(chan, line_count);
		OUT_RING(chan, (1<<8)|(1<<0));
		OUT_RING(chan, 0);
		BEGIN_RING(chan, NvSubM2MF, NV_MEMORY_TO_MEMORY_FORMAT_NOP, 1);
		OUT_RING(chan, 0);

		page_count -= line_count;
		src_offset += (PAGE_SIZE * line_count);
		dst_offset += (PAGE_SIZE * line_count);
	}

        if ((old_domain == NOUVEAU_PSCMM_DOMAIN_CPU) ||
                (new_domain == NOUVEAU_PSCMM_DOMAIN_CPU)) {
                nouveau_pscmm_object_unbind(nvbo, 1);
        }

        /* Add seqno into command buffer. */
        /* we can check the seqno onec it's done if needed. */
	nvbo->last_rendering_seqno =  nouveau_pscmm_add_request(dev);

	return ret;
	
}

int
nouveau_pscmm_move(struct drm_device *dev, struct drm_gem_object* gem,
	    struct nouveau_bo **pnvbo, uint32_t old_domain, uint32_t new_domain,
	    bool no_evicted, uint32_t align)
{
	struct nouveau_bo *nvbo;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	int ret;

	/* no need to move */
	if (old_domain == new_domain)
		return ret;
	
	if (gem->driver_private == NULL && new_domain == NOUVEAU_PSCMM_DOMAIN_VRAM) {
		/* alloc bo */
		nvbo = nouveau_pscmm_alloc_vram(dev_priv, gem->size, no_evicted, align);
 		/* alloc gem object */
		nvbo->gem = gem;

		nvbo->gem->driver_private = nvbo;
		nvbo->placements = NOUVEAU_PSCMM_DOMAIN_VRAM;
		*pnvbo = nvbo;

	}

	if (nvbo->placements == new_domain) {
		NV_DEBUG(dev, "same as new_domain, just return");
		return ret;
	}

	/* check if the bo is non-evict */
	if (nvbo->type == no_evicted) {
		/* wait rendering or return? */
		NV_ERROR(dev, "nvbo is busy");
		return -1;
	}

	/* bind/unbind only */
	if ((new_domain != NOUVEAU_PSCMM_DOMAIN_VRAM) &&
		(nvbo->placements != NOUVEAU_PSCMM_DOMAIN_VRAM)) {
		if (new_domain == NOUVEAU_PSCMM_DOMAIN_GART)
			nouveau_pscmm_object_bind_to_gart(nvbo, align);
		else
			nouveau_pscmm_object_unbind(nvbo, 1);
		goto out;
	}

	/* Software copy if the card isn't up and running yet. */
	if (dev_priv->init_state != NOUVEAU_CARD_INIT_DONE ||
	    !dev_priv->channel) {
		nouveau_pscmm_move_memcpy(dev, gem, nvbo, nvbo->placements, new_domain);
		goto out;
	}

	/* we should use Hardware assisted copy here*/
	nouveau_pscmm_move_m2mf(dev, gem, nvbo, nvbo->placements, new_domain,align);
	
out:
	nvbo->placements = new_domain;

	if (no_evicted && nvbo->placements == NOUVEAU_PSCMM_DOMAIN_VRAM) {
		nouveau_pscmm_set_no_evicted(dev_priv, nvbo);
	}
	return ret;
}

void
nouveau_pscmm_move_active_list(struct drm_nouveau_private *dev_priv,
		 struct drm_gem_object *gem, struct nouveau_bo *nvbo, uint32_t seqno)
{
	if (!nvbo->active) {
		drm_gem_object_reference(gem);
		nvbo->active = 1;
	}
	list_move_tail(&nvbo->active_list, &dev_priv->active_list);
	nvbo->last_rendering_seqno = seqno;
}

void
nouveau_pscmm_remove_active_list(struct drm_nouveau_private *dev_priv,
							struct drm_gem_object *gem, struct nouveau_bo *nvbo)
{

	list_del_init(&nvbo->active_list);

	nvbo->last_rendering_seqno = 0;	
	if (nvbo->active) {
		nvbo->active = 0;
		drm_gem_object_unreference(gem);
	}
}

int
nouveau_pscmm_set_no_evicted(struct drm_nouveau_private *dev_priv,
					struct nouveau_bo *nvbo)
{
	nvbo->old_type= nvbo->type;
	nvbo->type= no_evicted;

	list_move_tail(&nvbo->list, &dev_priv->no_evicted_list);
	if (nvbo->old_type == T1)
		dev_priv->T1_num-=nvbo->nblock;
	else
		dev_priv->T2_num-=nvbo->nblock;
	return 0;
}
int
nouveau_pscmm_set_normal(struct drm_nouveau_private *dev_priv,
					struct nouveau_bo *nvbo)
{
	nvbo->bo_ref = 1;
	if (nvbo->old_type == T1) {
		list_move_tail(&nvbo->list, &dev_priv->T1_list);
		dev_priv->T1_num+=nvbo->nblock;
	} else {
		list_move_tail(&nvbo->list, &dev_priv->T2_list);
		dev_priv->T2_num+=nvbo->nblock;
	}
	nvbo->type= nvbo->old_type;
	nvbo->old_type= no_evicted;
	return 0;
}

int
nouveau_pscmm_prefault(struct drm_nouveau_private *dev_priv,
		struct nouveau_bo *nvbo, uint32_t align)
{
//this is for object level prefault

	//nvbo swap out
	if (nvbo->swap_out) {
		kfree(nvbo->block_array);
		nvbo->block_array = nouveau_find_mem_space(dev_priv, nvbo, nvbo->gem->size / PAGE_SIZE, true, align);
		/* swap in ?*/
		DRM_ERROR("ERROR!!! need swap in, but not support");
//		copyback();
	} else {
		//hit
		nvbo->bo_ref = 1;
		nouveau_pscmm_set_no_evicted(dev_priv, nvbo);
	}
	return 0;
}
int
nouveau_pscmm_command_prefault(struct drm_device *dev, struct drm_file *file_priv, uint32_t handle, uint32_t align)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	int i, j;
	int ret;
	int nblock;

	gem = drm_gem_object_lookup(dev, file_priv, handle);
	if (gem == NULL) {
		NV_ERROR(dev, "Can't find gem 0x%x", handle);
		return -EINVAL;
	}
	nvbo = gem->driver_private;

	/* check if nvbo is empty? */
	if (nvbo == NULL) {
		ret = nouveau_pscmm_move(dev, gem, &nvbo, 0, NOUVEAU_PSCMM_DOMAIN_VRAM, true, align);
	}

	if (nvbo->type == no_evicted) {
		drm_gem_object_unreference(gem);
		return ret;
	}
		
	ret = nouveau_pscmm_prefault(dev_priv, nvbo, align);

	drm_gem_object_unreference(gem);
	return ret;
}

/**
 * Returns true if seq1 is later than seq2.
 */
static int
nouveau_seqno_passed(uint32_t seq1, uint32_t seq2)
{
	return (int32_t)(seq1 - seq2) >= 0;
}

/**
  * Get seq from globle status page
  */
uint32_t
nouveau_get_pscmm_seqno(struct nouveau_channel* chan)
{

	return nvchan_rd32(chan, 0x48);
}

static void
nouveau_pscmm_retire_request(struct drm_device *dev,
			struct drm_nouveau_pscmm_request *request)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	/* move bo out of no_evicted list */
	while (!list_empty(&dev_priv->active_list)) {
		struct nouveau_bo *nvbo;
		struct list_head *entry;
		entry = &dev_priv->active_list;

		nvbo = list_entry(entry,
				  struct nouveau_bo,
				  active_list);

		/* If the seqno being retired doesn't match the oldest in the
		 * list, then the oldest in the list must still be newer than
		 * this seqno.
		 */
		if (nvbo->last_rendering_seqno != request->seqno)
			break;

		nouveau_pscmm_remove_active_list(dev_priv, nvbo->gem, nvbo);
	}

}

/**
 * This function clears the request list as sequence numbers are passed.
 */
void
nouveau_pscmm_retire_requests(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_channel* chan = dev_priv->channel;
	uint32_t seqno;

	seqno = nouveau_get_pscmm_seqno(chan);

	while (!list_empty(&chan->request_list)) {
		struct drm_nouveau_pscmm_request *request;
		uint32_t retiring_seqno;
		request = list_first_entry(&chan->request_list,
					struct drm_nouveau_pscmm_request,
					list);
		retiring_seqno = request->seqno;

		if (nouveau_seqno_passed(seqno, retiring_seqno) ) {
			nouveau_pscmm_retire_request(dev, request);

			list_del(&request->list);
			kfree(request);
		} else
			break;
	}
}

void
nouveau_pscmm_retire_work_handler(void *device)
{
	struct drm_device *dev = (struct drm_device *)device;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_channel* chan = dev_priv->channel;

DRM_ERROR("fix me");
#if 0
	/* Return if gem idle */
	if (worktimer_id == NULL) {
		return;
	}

	nouveau_pscmm_retire_requests(dev);
	if (!list_empty(&chan->request_list))
	{	
		NV_DEBUG(dev, "schedule_delayed_work");
		worktimer_id = timeout(nouveau_pscmm_retire_work_handler, (void *) dev, DRM_HZ);
	}
#endif
}

static uint32_t
nouveau_pscmm_add_request(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_nouveau_pscmm_request *request;
	uint32_t seqno;
	int ret;
	int was_empty;
	struct nouveau_channel* chan = dev_priv->channel;

	request = kcalloc(1, sizeof(*request), GFP_KERNEL);
	if (request == NULL) {
		DRM_ERROR("Failed to alloc request");
		return 0;
	}
	
	ret = RING_SPACE(chan, 2);
	seqno = chan->next_seqno;		/* globle seqno */
	chan->next_seqno++;
	if (chan->next_seqno == 0)
		chan->next_seqno++;


	BEGIN_RING(chan, NvSubSw, USE_REFCNT ? 0x0050 : 0x0150, 1);
	OUT_RING(chan, seqno);
	FIRE_RING(chan);

	request->seqno = seqno;
	request->emitted_jiffies = jiffies;
	request->chan = chan;

	was_empty = list_empty(&chan->request_list);
	list_add_tail(&request->list, &chan->request_list);

	if (was_empty)
	{
		/* change to delay HZ and then run work (not insert to workqueue of Linux) */ 
//		worktimer_id = timeout(nouveau_pscmm_retire_work_handler, (void *) dev, DRM_HZ);
		DRM_DEBUG("i915_gem: schedule_delayed_work");
	}
	return seqno;
}


void
nouveau_pscmm_remove(struct drm_device *dev,  struct nouveau_bo *nvbo)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	
	if (nvbo->virtual) {
		iounmap(nvbo->virtual);
	}

	nouveau_pscmm_set_normal(dev_priv, nvbo);

}

int
nouveau_pscmm_new(struct drm_device *dev,  struct drm_file *file_priv,
		int size, int align, uint32_t flags,
		bool no_evicted, bool mappable,
		struct nouveau_bo **pnvbo)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_bo *nvbo = NULL;
	u32 handle;
	struct drm_gem_object *gem;
	int ret;

	if (align == 0)
		align = PAGE_SIZE;
	if (align & (PAGE_SIZE - 1)) {
		DRM_ERROR("Invalid object alignment requested %u\n", align);
		return -EINVAL;
	}

 	/* alloc gem object */
	gem = drm_gem_object_alloc(dev, size);
	gem->driver_private = NULL;
	if (file_priv)
		ret = drm_gem_handle_create(file_priv, gem, &handle);

	nouveau_pscmm_move(dev, gem, &nvbo, NOUVEAU_PSCMM_DOMAIN_CPU, flags, no_evicted, align);

	if (mappable) {
		nvbo->virtual = ioremap(dev_priv->fb_block->io_offset + nvbo->block_offset_node->start,  gem->size);
	}

	*pnvbo = nvbo;

	return 0;
}

int
nouveau_pscmm_ioctl_new(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_nouveau_pscmm_new *arg = data;
	struct drm_gem_object *gem;
	int ret;

	/* page-align check */
	
 	/* alloc gem object */
	gem = drm_gem_object_alloc(dev, arg->size);
	gem->driver_private = NULL;
	ret = drm_gem_handle_create(file_priv, gem, &arg->handle);

	return ret;

}

int
nouveau_pscmm_ioctl_mmap(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pscmm_mmap *req = data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	unsigned long vvaddr = NULL;
	int ret;


/* need fix */
NV_ERROR(dev, "Not support VRAM map by now");

	gem = drm_gem_object_lookup(dev, file_priv, req->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", req->handle);
                return -EINVAL;
        }
#if 0
	nvbo = gem->driver_private;

	if (nvbo != NULL) {
		
		if (nvbo->placements == NOUVEAU_PSCMM_DOMAIN_VRAM) {

			/* check if the bo is non-evict */
			if (nvbo->type != no_evicted) {
				/* prefault and mark the bo as non-evict*/
				nouveau_pscmm_prefault(dev_priv, nvbo);
			} else {

				/* bo is accssed by GPU */
				return EBUSY;
			}
				
			/* mmap the VRAM to user space 
			  * may or may not chanmap
			  * use the same FB BAR + tile_flags?
			  */

		}
				
		return ret;

	}
#endif
	
	down_write(&current->mm->mmap_sem);
	vvaddr = do_mmap(gem->filp, 0, req->size,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			req->offset);
	up_write(&current->mm->mmap_sem);
	if (IS_ERR((void *)vvaddr))
		return vvaddr;

	mutex_lock(&dev->struct_mutex);
	drm_gem_object_unreference(gem);
	mutex_unlock(&dev->struct_mutex);

	req->addr_ptr = (uint64_t)vvaddr;
	return 0;

}


int
nouveau_pscmm_ioctl_range_flush(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pscmm_range_flush *arg = data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	int ret;


/* need fix */
NV_ERROR(dev, "Not support by now");
return -1;
#if 0
	gem = drm_gem_object_lookup(dev, file_priv, arg->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", arg->handle);
                return -EINVAL;
        }
        nvbo = gem->driver_private;

	if (nvbo != NULL) {
		
		if (nvbo->placements == NOUVEAU_PSCMM_DOMAIN_VRAM) {
				
			/* flush to VRAM 
			  * may or may not chanmap
			  * use the same FB BAR + tile_flags?
			  */

			
			/* mark the block as normal*/
			nouveau_pscmm_set_normal(dev_priv, nvbo);
        		return ret;
		}

	}
	
	/* Flush the GEM object */
	
	return ret;
#endif
}

int
nouveau_pscmm_ioctl_chan_map(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_nouveau_pscmm_chanmap *arg= data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	struct nouveau_channel* chan;
	int need_sync = 0;
	int ret;

	gem = drm_gem_object_lookup(dev, file_priv, arg->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", arg->handle);
                return -EINVAL;
        }

        nvbo = gem->driver_private;

	/* get channel */
	NOUVEAU_GET_USER_CHANNEL_WITH_RETURN(arg->channel, file_priv, chan);
	
	if (nvbo == NULL) {

		nouveau_pscmm_move(dev, gem, &nvbo, NOUVEAU_PSCMM_DOMAIN_CPU, NOUVEAU_PSCMM_DOMAIN_VRAM, false, PAGE_SIZE);

	}


	if (!nvbo->channel) {
		/* chanmap, need low and tile_flags */
		arg->addr_ptr = nouveau_channel_map(dev, chan, nvbo, arg->low, arg->tile_flags);

	} else {
		/* bo can be shared between channels 
         	  * if bo has mapped to other chan, maybe do something here
	 	  */
	 	NV_DEBUG(dev, "bo shared between channels are not supported by now");
	}

	drm_gem_object_unreference(gem);
	return ret;
}

int
nouveau_pscmm_ioctl_chan_unmap(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct drm_nouveau_pscmm_chanunmap *arg = data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	int ret;
	
	gem = drm_gem_object_lookup(dev, file_priv, arg->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", arg->handle);
                return -EINVAL;
        }
        nvbo = gem->driver_private;

	if (nvbo == NULL)
		return EINVAL;
	
	if (nvbo->channel != NULL) {
	
		/* unmap the channel */
		ret = nouveau_channel_unmap(dev, nvbo->channel, nvbo);
	}

	drm_gem_object_unreference(gem);
	
	return ret;
}

static inline int
slow_shmem_copy(struct page *dst_page,
		int dst_offset,
		struct page *src_page,
		int src_offset,
		int length)
{

	char *dst_vaddr, *src_vaddr;
	dst_vaddr = kmap_atomic(dst_page, KM_USER0);
	if (dst_vaddr == NULL)
		return -ENOMEM;
	src_vaddr = kmap_atomic(src_page, KM_USER1);
	if (src_vaddr == NULL) {
		kunmap_atomic(dst_vaddr, KM_USER0);
		return -ENOMEM;
	}

	memcpy(dst_vaddr + dst_offset, src_vaddr + src_offset, length);

	kunmap_atomic(src_vaddr, KM_USER1);
	kunmap_atomic(dst_vaddr, KM_USER0);

	return 0;
}

int
nouveau_pscmm_ioctl_read(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pscmm_read *arg= data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	caddr_t addr;
	uint32_t *user_data;
	int ret = 0;
	
	gem = drm_gem_object_lookup(dev, file_priv, arg->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", arg->handle);
                return -EINVAL;
        }
        nvbo = gem->driver_private;

	if (nvbo != NULL) {
		
		if (nvbo->placements == NOUVEAU_PSCMM_DOMAIN_VRAM) {

			/* check if the bo is non-evict */
			if (nvbo->type == no_evicted) {
				/* wait rendering */

			}

			/* prefault and mark the bo as non-evict*/
			nouveau_pscmm_prefault(dev_priv, nvbo, PAGE_SIZE);
				
			/* read the VRAM to user space address */
			addr = ioremap(dev_priv->fb_block->io_offset + nvbo->block_offset_node->start,  gem->size);
			if (!addr) {
				NV_ERROR(dev, "bo shared between channels are not supported by now");
				return -ENOMEM;
			}

			user_data = (uint32_t *) (uintptr_t) arg->data_ptr;
			ret = DRM_COPY_TO_USER(user_data, addr + arg->offset, arg->size);
			if (ret) {
                		ret = EFAULT;
                		NV_ERROR(dev, "failed to read, ret %d", ret);
        		}
			
			
			/* mark the block as normal*/
			nouveau_pscmm_set_normal(dev_priv, nvbo);
			drm_gem_object_unreference(gem);
        		return ret;
		}

	}
	
	/* read the GEM object */
	struct mm_struct *mm = current->mm;
	struct page **user_pages;
	ssize_t remain;
	loff_t offset, pinned_pages, i;
	loff_t first_data_page, last_data_page, num_pages;
	int shmem_page_index, shmem_page_offset;
	int data_page_index,  data_page_offset;
	int page_length;
	uint64_t data_ptr = arg->data_ptr;
	int j;

	remain = arg->size;
	first_data_page = data_ptr / PAGE_SIZE;
	last_data_page = (data_ptr + arg->size - 1) / PAGE_SIZE;
	num_pages = last_data_page - first_data_page + 1;

	user_pages = drm_calloc_large(num_pages, sizeof(struct page *));
	if (user_pages == NULL)
		return -ENOMEM;
	
	down_read(&mm->mmap_sem);
	pinned_pages = get_user_pages(current, mm, (uintptr_t)arg->data_ptr,
				num_pages, 1, 0, user_pages, NULL);
	up_read(&mm->mmap_sem);
	if (pinned_pages < num_pages) {
		ret = -EFAULT;
		goto fail_put_user_pages;
	}

	nouveau_pscmm_object_get_page_list(nvbo);

	offset = arg->offset;

	while (remain > 0) {
		shmem_page_index = offset / PAGE_SIZE;
		shmem_page_offset = offset & ~PAGE_MASK;
		data_page_index = data_ptr / PAGE_SIZE - first_data_page;
		data_page_offset = data_ptr & ~PAGE_MASK;

		page_length = remain;
		if ((shmem_page_offset + page_length) > PAGE_SIZE)
			page_length = PAGE_SIZE - shmem_page_offset;
		if ((data_page_offset + page_length) > PAGE_SIZE)
			page_length = PAGE_SIZE - data_page_offset;
		ret = slow_shmem_copy(user_pages[data_page_index],
					data_page_offset,
					nvbo->pages[shmem_page_index],
					shmem_page_offset,
					page_length);
		if (ret)
			goto fail_put_pages;
		remain -= page_length;
		data_ptr += page_length;
		offset += page_length;
	}
fail_put_pages:
	nouveau_pscmm_object_free_page_list(nvbo);
fail_put_user_pages:
	for (i = 0; i < pinned_pages; i++) {
		SetPageDirty(user_pages[i]);
		page_cache_release(user_pages[i]);
	}
	drm_free_large(user_pages);

	drm_gem_object_unreference(gem);
	return ret;
}


int
nouveau_pscmm_ioctl_write(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_pscmm_write *arg = data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	caddr_t addr;
	uint32_t *user_data;
	int ret = 0;
	
	gem = drm_gem_object_lookup(dev, file_priv, arg->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", arg->handle);
                return -EINVAL;
        }
        nvbo = gem->driver_private;

	if (nvbo != NULL) {
		
		if (nvbo->placements == NOUVEAU_PSCMM_DOMAIN_VRAM) {
			/* check if the bo is non-evict */
			if (nvbo->type == no_evicted) {
				/* wait rendering */

			}

			/* prefault and mark the bo as non-evict*/
			nouveau_pscmm_prefault(dev_priv, nvbo, PAGE_SIZE);
				
			/* write the VRAM to user space address */
			addr = ioremap(dev_priv->fb_block->io_offset + nvbo->block_offset_node->start,  gem->size);
			if (!addr) {
				NV_ERROR(dev, "bo shared between channels are not supported by now");
				drm_gem_object_unreference(gem);
				return -ENOMEM;
			}

			user_data = (uint32_t *) (uintptr_t) arg->data_ptr;

			ret = DRM_COPY_FROM_USER(addr + arg->offset, user_data, arg->size);
			if (ret) {
                		ret = EFAULT;
                		NV_ERROR(dev, "failed to write, unwritten %d", ret);
        		}

			/* mark the block as normal*/
			nouveau_pscmm_set_normal(dev_priv, nvbo);
			drm_gem_object_unreference(gem);
        		return ret;
		}

	}


	/* Write the GEM object */
	struct mm_struct *mm = current->mm;
	struct page **user_pages;
	ssize_t remain;
	loff_t offset, pinned_pages, i;
	loff_t first_data_page, last_data_page, num_pages;
	int shmem_page_index, shmem_page_offset;
	int data_page_index,  data_page_offset;
	int page_length;
	uint64_t data_ptr = arg->data_ptr;

	remain = arg->size;

	first_data_page = data_ptr / PAGE_SIZE;
	last_data_page = (data_ptr + arg->size - 1) / PAGE_SIZE;
	num_pages = last_data_page - first_data_page + 1;

	user_pages = drm_calloc_large(num_pages, sizeof(struct page *));
	if (user_pages == NULL)
		return -ENOMEM;

	down_read(&mm->mmap_sem);
	pinned_pages = get_user_pages(current, mm, (uintptr_t)arg->data_ptr,
				num_pages, 0, 0, user_pages, NULL);
	up_read(&mm->mmap_sem);
	if (pinned_pages < num_pages) {
		ret = -EFAULT;
		goto fail_put_user_pages;
	}

	nouveau_pscmm_object_get_page_list(nvbo);

	offset = arg->offset;
	nvbo->dirty = 1;	

	while (remain > 0) {
		shmem_page_index = offset / PAGE_SIZE;
		shmem_page_offset = offset & ~PAGE_MASK;
		data_page_index = data_ptr / PAGE_SIZE - first_data_page;
		data_page_offset = data_ptr & ~PAGE_MASK;

		page_length = remain;
		if ((shmem_page_offset + page_length) > PAGE_SIZE)
			page_length = PAGE_SIZE - shmem_page_offset;
		if ((data_page_offset + page_length) > PAGE_SIZE)
			page_length = PAGE_SIZE - data_page_offset;
		ret = slow_shmem_copy(nvbo->pages[shmem_page_index],
					shmem_page_offset,
					user_pages[data_page_index],
					data_page_offset,
					page_length);
		if (ret)
			goto fail_put_pages;

		remain -= page_length;
		data_ptr += page_length;
		offset += page_length;
	}

fail_put_pages:
	nouveau_pscmm_object_free_page_list(nvbo);
fail_put_user_pages:
	for (i = 0; i < pinned_pages; i++)
		page_cache_release(user_pages[i]);
	drm_free_large(user_pages);

	drm_gem_object_unreference(gem);
	return ret;
}


// The evict function will delete the bos.
// Normally the bo can be used only once, since the VRAM is limited and save the bo cost too much time in moving. 
// if the user want to save their bos, just move the bo to RAM.

int
nouveau_pscmm_ioctl_move(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_nouveau_pscmm_move *arg = data;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	int ret;
	
	gem = drm_gem_object_lookup(dev, file_priv, arg->handle);
        if (gem == NULL) {
                NV_ERROR(dev, "Can't find gem 0x%x", arg->handle);
                return -EINVAL;
        }
        nvbo = gem->driver_private;

	nouveau_pscmm_move(dev, gem, &nvbo, arg->old_domain, arg->new_domain, false, PAGE_SIZE);

end:
	/* think about new is RAM. User space will ignore the firstblock if the bo is not in the VRAM*/
	arg->presumed_offset = nvbo->firstblock;		//bo gpu vm address
	arg->presumed_domain = nvbo->placements;	
	drm_gem_object_unreference(gem);
	return 0;
}


int
nouveau_pscmm_ioctl_exec(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_nouveau_pscmm_exec *args = data;
	struct drm_nouveau_pscmm_exec_object *obj_list;
	struct drm_gem_object *gem;
	struct nouveau_bo *nvbo;
	struct nouveau_channel* chan;
	int i, j;
	int ret = 0;
	

	/* get channel */
	NOUVEAU_GET_USER_CHANNEL_WITH_RETURN(args->channel, file_priv, chan);
	obj_list = kcalloc(args->buffer_count, sizeof(*obj_list), GFP_KERNEL);

	ret = copy_from_user(obj_list,
		     (struct drm_nouveau_pscmm_exec_object *)
		     (uintptr_t) args->buffers_ptr,
			sizeof(*obj_list) * args->buffer_count);

	if (ret != 0) {
		NV_ERROR(dev, "copy to %d obj entries failed %d\n",
			  args->buffer_count, ret);
		goto err1;
	}
	for (j = 0; j < args->buffer_count; j++) {
		/* prefault and mark*/
		nouveau_pscmm_command_prefault(dev, file_priv, obj_list[j].handle, PAGE_SIZE);
				
	}

	/* Copy the new  offsets back to the user's exec_object list. */
	ret = copy_to_user((struct drm_nouveau_pscmm_exec_object *)
			(uintptr_t) args->buffers_ptr, obj_list,
			sizeof(*obj_list) * args->buffer_count);

        if (ret != 0) {
                NV_ERROR(dev, "copy from %d obj entries failed %d\n",
                          args->buffer_count, ret);
                goto err1;
        }

	/* pushbuf is the last obj */
	uint32_t nr_pushbuf = obj_list[args->buffer_count -1].nr_dwords;
	gem = drm_gem_object_lookup(dev, file_priv, obj_list[args->buffer_count -1].handle);
        if (gem == NULL) {
		NV_ERROR(dev, "Can't find gem 0x%x", obj_list[args->buffer_count -1].handle);
		ret = -EINVAL;
                goto err1;
	}
        nvbo = gem->driver_private;


#if 1
/* use batchbuffer */
	ret = nouveau_dma_wait(chan, nr_pushbuf + 1, 6);
	if (ret) {
		NV_INFO(dev, "pscmm_exec_space: %d\n", ret);
		goto err1;
	}

	nv50_dma_push(chan, nvbo, 0, gem->size);
#else
/* use one command by one command */
	uint32_t *pushbuf_data;
	pushbuf_data = (uint32_t *)(uintptr_t)gem->kaddr;
	RING_SPACE(chan, nr_pushbuf);
	for (i = 0; i < nr_pushbuf; i++) {
		OUT_RING(chan, pushbuf_data[i]);
	}
	FIRE_RING(chan);
#endif
	/* Add seqno into command buffer. */ 
	args->seqno = nouveau_pscmm_add_request(dev);
	for (j = 0; j < args->buffer_count; j++) {
		struct drm_gem_object *tmp_gem;
		tmp_gem = drm_gem_object_lookup(dev, file_priv, obj_list[j].handle);

		if (tmp_gem == NULL) {
			NV_ERROR(dev, "Can't find gem 0x%x", obj_list[j].handle);
			ret = -EINVAL;
			goto err2;
		}
		nvbo = tmp_gem->driver_private;

		nouveau_pscmm_move_active_list(dev_priv, tmp_gem, nvbo, args->seqno);
		drm_gem_object_unreference(tmp_gem);
	}
err2:
	drm_gem_object_unreference(gem);
err1:
	kfree(obj_list);

	return ret;
}


/***********************************
 * finally, the ioctl table
 ***********************************/

struct drm_ioctl_desc nouveau_ioctls[] = {
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_GETPARAM, nouveau_ioctl_getparam, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_SETPARAM, nouveau_ioctl_setparam, DRM_AUTH|DRM_MASTER|DRM_ROOT_ONLY),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, nouveau_ioctl_fifo_alloc, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_CHANNEL_FREE, nouveau_ioctl_fifo_free, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_GROBJ_ALLOC, nouveau_ioctl_grobj_alloc, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_NOTIFIEROBJ_ALLOC, nouveau_ioctl_notifier_alloc, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_GPUOBJ_FREE, nouveau_ioctl_gpuobj_free, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_NEW, nouveau_pscmm_ioctl_new, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_MMAP, nouveau_pscmm_ioctl_mmap, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_RANGE_FLUSH, nouveau_pscmm_ioctl_range_flush, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_READ, nouveau_pscmm_ioctl_read, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_WRITE, nouveau_pscmm_ioctl_write, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_MOVE, nouveau_pscmm_ioctl_move, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_EXEC, nouveau_pscmm_ioctl_exec, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_CHAN_MAP, nouveau_pscmm_ioctl_chan_map, DRM_AUTH),
	NOUVEAU_IOCTL_DEF(DRM_IOCTL_NOUVEAU_PSCMM_CHAN_UNMAP, nouveau_pscmm_ioctl_chan_unmap, DRM_AUTH),
};

int nouveau_max_ioctl = DRM_ARRAY_SIZE(nouveau_ioctls);

