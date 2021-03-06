/*
 * Copyright (C) 2010 Christoph Bumiller.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "drmP.h"
#include "drm.h"
#include "nouveau_drv.h"
#include "nouveau_reg.h"
#include "pscnv_fifo.h"
#include "pscnv_chan.h"

struct nvc0_fifo_engine {
	struct pscnv_fifo_engine base;
	spinlock_t lock;
	struct pscnv_bo *playlist[2];
	int cur_playlist;
	struct pscnv_bo *ctrl_bo;
	volatile uint32_t *fifo_ctl;
};

#define nvc0_fifo(x) container_of(x, struct nvc0_fifo_engine, base)

void nvc0_fifo_takedown(struct drm_device *dev);
void nvc0_fifo_irq_handler(struct drm_device *dev, int irq);
int nvc0_fifo_chan_init_ib (struct pscnv_chan *ch, uint32_t pb_handle, uint32_t flags, uint32_t slimask, uint64_t ib_start, uint32_t ib_order);
void nvc0_fifo_chan_kill(struct pscnv_chan *ch);

int nvc0_fifo_init(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nvc0_fifo_engine *res = kzalloc(sizeof *res, GFP_KERNEL);

	if (!res) {
		NV_ERROR(dev, "PFIFO: Couldn't allocate engine!\n");
		return -ENOMEM;
	}

	res->base.takedown = nvc0_fifo_takedown;
	res->base.chan_kill = nvc0_fifo_chan_kill;
	res->base.chan_init_ib = nvc0_fifo_chan_init_ib;
	spin_lock_init(&res->lock);

	res->ctrl_bo = pscnv_mem_alloc(dev, 128 * 0x1000,
					     PSCNV_GEM_CONTIG, 0, 0xf1f03e95);

	if (!res->ctrl_bo) {
		NV_ERROR(dev, "PFIFO: Couldn't allocate control area!\n");
		kfree(res);
		return -ENOMEM;
	}

	res->playlist[0] = pscnv_mem_alloc(dev, 0x1000, PSCNV_GEM_CONTIG, 0, 0x91a71157);
	res->playlist[1] = pscnv_mem_alloc(dev, 0x1000, PSCNV_GEM_CONTIG, 0, 0x91a71157);
	if (!res->playlist[0] || !res->playlist[1]) {
		NV_ERROR(dev, "PFIFO: Couldn't allocate playlists!\n");
		if (res->playlist[0])
			pscnv_mem_free(res->playlist[0]);
		if (res->playlist[1])
			pscnv_mem_free(res->playlist[1]);
		pscnv_mem_free(res->ctrl_bo);
		kfree(res);
		return -ENOMEM;
	}
	dev_priv->vm->map_kernel(res->playlist[0]);
	dev_priv->vm->map_kernel(res->playlist[1]);
	res->cur_playlist = 0;

	dev_priv->vm->map_user(res->ctrl_bo);

	if (!res->ctrl_bo->map1) {
		NV_ERROR(dev, "PFIFO: Couldn't map control area!\n");
		pscnv_mem_free(res->playlist[0]);
		pscnv_mem_free(res->playlist[1]);
		pscnv_mem_free(res->ctrl_bo);
		kfree(res);
		return -ENOMEM;
	}
	res->fifo_ctl = ioremap(pci_resource_start(dev->pdev, 1) +
				     res->ctrl_bo->map1->start, 128 << 12);
	if (!res->fifo_ctl) {
		NV_ERROR(dev, "PFIFO: Couldn't ioremap control area!\n");
		pscnv_mem_free(res->playlist[0]);
		pscnv_mem_free(res->playlist[1]);
		pscnv_mem_free(res->ctrl_bo);
		kfree(res);
		return -ENOMEM;
	}
	
	nv_wr32(dev, 0x2100, 0xffffffff); /* PFIFO_INTR */

	nv_wr32(dev, 0x204, 0);
	nv_wr32(dev, 0x204, 7);

	nv_wr32(dev, 0x2204, 7);

        nv_wr32(dev, 0x2208, 0xfffffffe);
        nv_wr32(dev, 0x220c, 0xfffffffd);
        nv_wr32(dev, 0x2210, 0xfffffffd);
        nv_wr32(dev, 0x2214, 0xfffffffd);
        nv_wr32(dev, 0x2218, 0xfffffffb);
        nv_wr32(dev, 0x221c, 0xfffffffd);

	nv_wr32(dev, 0x4010c, 0xffffffff);
	nv_wr32(dev, 0x4210c, 0xffffffff);
	nv_wr32(dev, 0x4410c, 0xffffffff);

	nv_wr32(dev, 0x2200, 1);

	nv_wr32(dev, 0x2254,
		(1 << 28) | (res->ctrl_bo->map1->start >> 12));

	dev_priv->fifo = &res->base;

	nouveau_irq_register(dev, 8, nvc0_fifo_irq_handler);
	nv_wr32(dev, 0x2140, 0xbfffffff); /* PFIFO_INTR_EN */

	return 0;
}

void nvc0_fifo_takedown(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nvc0_fifo_engine *fifo = nvc0_fifo(dev_priv->fifo);
	nv_wr32(dev, 0x2140, 0);
	nouveau_irq_unregister(dev, 8);
	/* XXX */
	pscnv_mem_free(fifo->playlist[0]);
	pscnv_mem_free(fifo->playlist[1]);
	iounmap(fifo->fifo_ctl);
	pscnv_mem_free(fifo->ctrl_bo);
	kfree(fifo);
	dev_priv->fifo = 0;
}

void nvc0_fifo_playlist_update(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nvc0_fifo_engine *fifo = nvc0_fifo(dev_priv->fifo);
	int i, pos;
	struct pscnv_bo *vo;
	fifo->cur_playlist ^= 1;
	vo = fifo->playlist[fifo->cur_playlist];
	for (i = 0, pos = 0; i < 128; i++) {
		if (nv_rd32(dev, 0x3004 + i * 8) & 1) {
			nv_wv32(vo, pos, i);
			nv_wv32(vo, pos + 4, 0x4);
			pos += 8;
		}
	}
	dev_priv->vm->bar_flush(dev);

	nv_wr32(dev, 0x2270, vo->start >> 12);
	nv_wr32(dev, 0x2274, 0x1f00000 | pos / 8);

	if (!nv_wait(dev, 0x227c, (1 << 20), 0))
		NV_WARN(dev, "WARNING: PFIFO 227c = 0x%08x\n",
			nv_rd32(dev, 0x227c));
}

void nvc0_fifo_chan_kill(struct pscnv_chan *ch)
{
	struct drm_device *dev = ch->dev;
	/* bit 28: active,
	 * bit 12: loaded,
	 * bit  0: enabled
	 */
	uint32_t status;

	status = nv_rd32(dev, 0x3004 + ch->cid * 8);
	nv_wr32(dev, 0x3004 + ch->cid * 8, status & ~1);
	nv_wr32(dev, 0x2634, ch->cid);
	if (!nv_wait(dev, 0x2634, ~0, ch->cid))
		NV_WARN(dev, "WARNING: 2634 = 0x%08x\n", nv_rd32(dev, 0x2634));

	nvc0_fifo_playlist_update(dev);

	if (nv_rd32(dev, 0x3004 + ch->cid * 8) & 0x1110) {
		NV_WARN(dev, "WARNING: PFIFO kickoff fail :(\n");
	}
}

#define nvchan_wr32(chan, ofst, val)					\
	fifo->fifo_ctl[((chan)->cid * 0x1000 + ofst) / 4] = val

int nvc0_fifo_chan_init_ib (struct pscnv_chan *ch, uint32_t pb_handle, uint32_t flags, uint32_t slimask, uint64_t ib_start, uint32_t ib_order) {
	struct drm_device *dev = ch->dev;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nvc0_fifo_engine *fifo = nvc0_fifo(dev_priv->fifo);
	unsigned long irqflags;

	int i;
	uint64_t fifo_regs = fifo->ctrl_bo->start + (ch->cid << 12);

	if (ib_order > 29)
		return -EINVAL;

	spin_lock_irqsave(&fifo->lock, irqflags);

	for (i = 0x40; i <= 0x50; i += 4)
		nvchan_wr32(ch, i, 0);
	for (i = 0x58; i <= 0x60; i += 4)
		nvchan_wr32(ch, i, 0);
	nvchan_wr32(ch, 0x88, 0);
	nvchan_wr32(ch, 0x8c, 0);

	for (i = 0; i < 0x100; i += 4)
		nv_wv32(ch->bo, i, 0);

	dev_priv->vm->bar_flush(dev);

	nv_wv32(ch->bo, 0x08, fifo_regs);
	nv_wv32(ch->bo, 0x0c, fifo_regs >> 32);

	nv_wv32(ch->bo, 0x48, ib_start); /* IB */
	nv_wv32(ch->bo, 0x4c,
		(ib_start >> 32) | (ib_order << 16));
	nv_wv32(ch->bo, 0x10, 0xface);
	nv_wv32(ch->bo, 0x54, 0x2);
	nv_wv32(ch->bo, 0x9c, 0x100);
	nv_wv32(ch->bo, 0x84, 0x20400000);
	nv_wv32(ch->bo, 0x94, 0x30000000 ^ slimask);
	nv_wv32(ch->bo, 0xa4, 0x1f1f1f1f);
	nv_wv32(ch->bo, 0xa8, 0x1f1f1f1f);
	nv_wv32(ch->bo, 0xac, 0x1f);
	nv_wv32(ch->bo, 0x30, 0xfffff902);
	/* nv_wv32(chan->vo, 0xb8, 0xf8000000); */ /* previously omitted */
	nv_wv32(ch->bo, 0xf8, 0x10003080);
	nv_wv32(ch->bo, 0xfc, 0x10000010);
	dev_priv->vm->bar_flush(dev);

	nv_wr32(dev, 0x3000 + ch->cid * 8, 0xc0000000 | ch->bo->start >> 12);
	nv_wr32(dev, 0x3004 + ch->cid * 8, 0x1f0001);

	nvc0_fifo_playlist_update(dev);

	spin_unlock_irqrestore(&fifo->lock, irqflags);

	return 0;
}

static const char *pgf_unit_str(int unit)
{
	switch (unit) {
	case 0: return "PGRAPH";
	case 3: return "PEEPHOLE";
	case 4: return "FB BAR";
	case 5: return "RAMIN BAR";
	case 7: return "PUSHBUF";
	default:
		break;
	}
	return "(unknown unit)";
}

static const char *pgf_cause_str(uint32_t flags)
{
	switch (flags & 0xf) {
	case 0x0: return "PDE not present";
	case 0x1: return "PT too short";
	case 0x2: return "PTE not present";
	case 0x3: return "LIMIT exceeded";
	case 0x5: return "NOUSER";
	case 0x6: return "PTE set read-only";
	default:
		break;
	}
	return "unknown cause";
}

void nvc0_pfifo_page_fault(struct drm_device *dev, int unit)
{
	uint64_t virt;
	uint32_t chan, flags;

	chan = nv_rd32(dev, 0x2800 + unit * 0x10) << 12;
	virt = nv_rd32(dev, 0x2808 + unit * 0x10);
	virt = (virt << 32) | nv_rd32(dev, 0x2804 + unit * 0x10);
	flags = nv_rd32(dev, 0x280c + unit * 0x10);

	NV_INFO(dev, "%s PAGE FAULT at 0x%010llx (%c, %s)\n",
		pgf_unit_str(unit), virt,
		(flags & 0x80) ? 'w' : 'r', pgf_cause_str(flags));
}

void nvc0_pfifo_subfifo_fault(struct drm_device *dev, int unit)
{
	int cid = nv_rd32(dev, 0x40120 + unit * 0x2000) & 0x7f;
	int status = nv_rd32(dev, 0x40108 + unit * 0x2000);
	uint32_t addr = nv_rd32(dev, 0x400c0 + unit * 0x2000);
	uint32_t data = nv_rd32(dev, 0x400c4 + unit * 0x2000);
	int sub = addr >> 16 & 7;
	int mthd = addr & 0x3ffc;
	int mode = addr >> 21 & 7;

	if (status & 0x200000) {
		NV_INFO(dev, "PSUBFIFO %d ILLEGAL_MTHD: ch %d sub %d mthd %04x%s [mode %d] data %08x\n", unit, cid, sub, mthd, ((addr & 1)?" NI":""), mode, data);
		nv_wr32(dev, 0x400c0 + unit * 0x2000, 0x80600008);
		nv_wr32(dev, 0x40108 + unit * 0x2000, 0x200000);
		status &= ~0x200000;
	}
	if (status & 0x800000) {
		NV_INFO(dev, "PSUBFIFO %d EMPTY_SUBCHANNEL: ch %d sub %d mthd %04x%s [mode %d] data %08x\n", unit, cid, sub, mthd, ((addr & 1)?" NI":""), mode, data);
		nv_wr32(dev, 0x400c0 + unit * 0x2000, 0x80600008);
		nv_wr32(dev, 0x40108 + unit * 0x2000, 0x800000);
		status &= ~0x800000;
	}
	if (status) {
		NV_INFO(dev, "unknown PSUBFIFO INTR: 0x%08x\n", status);
		nv_wr32(dev, 0x4010c + unit * 0x2000, nv_rd32(dev, 0x4010c + unit * 0x2000) & ~status);
	}
}

void nvc0_fifo_irq_handler(struct drm_device *dev, int irq)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nvc0_fifo_engine *fifo = nvc0_fifo(dev_priv->fifo);
	uint32_t status;
	unsigned long flags;

	spin_lock_irqsave(&fifo->lock, flags);
	status = nv_rd32(dev, 0x2100) & nv_rd32(dev, 0x2140);

	if (status & 1) {
		NV_INFO(dev, "PFIFO INTR 1!\n");
		nv_wr32(dev, 0x2100, 1);
		status &= ~1;
	}
	
	if (status & 0x10000000) {
		uint32_t bits = nv_rd32(dev, 0x259c);
		uint32_t units = bits;

		while (units) {
			int i = ffs(units) - 1;
			units &= ~(1 << i);
			nvc0_pfifo_page_fault(dev, i);
		}
		nv_wr32(dev, 0x259c, bits); /* ack */
		status &= ~0x10000000;
	}

	if (status & 0x20000000) {
		uint32_t bits = nv_rd32(dev, 0x25a0);
		uint32_t units = bits;
		while (units) {
			int i = ffs(units) - 1;
			units &= ~(1 << i);
			nvc0_pfifo_subfifo_fault(dev, i);
		}
		nv_wr32(dev, 0x25a0, bits); /* ack */
		status &= ~0x20000000;
	}

	if (status & 0x00000100) {
		uint32_t ibpk[2];
		uint32_t data = nv_rd32(dev, 0x400c4);

		ibpk[0] = nv_rd32(dev, 0x40110);
		ibpk[1] = nv_rd32(dev, 0x40114);

		NV_INFO(dev, "PFIFO FUCKUP: DATA = 0x%08x\n"
			"IB PACKET = 0x%08x 0x%08x\n", data, ibpk[0], ibpk[1]);
//		status &= ~0x100;
	}

	if (status) {
		NV_INFO(dev, "unknown PFIFO INTR: 0x%08x\n", status);
		/* disable interrupts */
		nv_wr32(dev, 0x2140, nv_rd32(dev, 0x2140) & ~status);
	}

	spin_unlock_irqrestore(&fifo->lock, flags);
}

uint64_t nvc0_fifo_ctrl_offs(struct drm_device *dev, int cid) {
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nvc0_fifo_engine *fifo = nvc0_fifo(dev_priv->fifo);
	return fifo->ctrl_bo->map1->start + cid * 0x1000;
}
