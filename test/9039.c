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
 
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <xf86drm.h>
#include <string.h>
#include <stdio.h>
#include "libpscnv.h"
#include <sys/mman.h>

#define IB_SIZE 0x2000

#define  NVC0_M2MF_OFFSET_OUT_HIGH                              0x00000238
#define  NVC0_M2MF_OFFSET_OUT_LOW                               0x0000023c
#define  NVC0_M2MF_EXEC                                         0x00000300
#define   NVC0_M2MF_EXEC_PUSH                                   (1 <<  0)
#define   NVC0_M2MF_EXEC_LINEAR_IN                              (1 <<  4)
#define   NVC0_M2MF_EXEC_LINEAR_OUT                             (1 <<  8)
#define   NVC0_M2MF_EXEC_NOTIFY                                 (1 << 13)
#define   NVC0_M2MF_EXEC_INC_SHIFT                              20
#define   NVC0_M2MF_EXEC_INC_MASK                               0x00f00000
#define  NVC0_M2MF_DATA                                         0x00000304
#define  NVC0_M2MF_OFFSET_IN_HIGH                               0x0000030c
#define  NVC0_M2MF_OFFSET_IN_LOW                                0x00000310
#define  NVC0_M2MF_PITCH_IN                                     0x00000314
#define  NVC0_M2MF_PITCH_OUT                                    0x00000318
#define  NVC0_M2MF_LINE_LENGTH_IN                               0x0000031c
#define  NVC0_M2MF_LINE_COUNT                                   0x00000320
#define  NVC0_M2MF_NOTIFY_ADDRESS_HIGH                          0x0000032c
#define  NVC0_M2MF_NOTIFY_ADDRESS_LOW                           0x00000330
#define  NVC0_M2MF_NOTIFY                                       0x00000334

struct nvchan {
   uint32_t vid;
   uint32_t cid;
   uint64_t ch_ofst;
   volatile uint32_t *regs;

   volatile uint32_t *pb;
   uint32_t pb_gem;
   uint64_t pb_ofst;

   volatile uint32_t *ib;
   uint32_t ib_gem;
   uint64_t ib_ofst;
   uint64_t ib_virt;

   int pb_base;
   int pb_pos;
   int ib_pos;
} ctx;

struct buf {
   uint64_t virt;
   uint64_t ofst;
   uint32_t *map;
   uint32_t gem;
   uint32_t size;
};

static int buf_new(int fd, struct buf *buf, uint32_t size)
{
   static int serial = 0;
   int ret;

   buf->size = size;

   ret = pscnv_gem_new(fd, 0xb0b00000 + serial++, PSCNV_GEM_CONTIG, 0, size, 0,
                       &buf->gem, &buf->ofst);
   if (ret) {
      fprintf(stderr, "buf%i: gem_new failed: %s\n",
              serial, strerror(-ret));
      return ret;
   }

   buf->map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, buf->ofst);
   if (!buf->map) {
      fprintf(stderr, "buf%i: failed to map\n", serial);
      return -1;
   }

   ret = pscnv_vspace_map(fd, ctx.vid, buf->gem, 0x20000000, 1ULL << 32,
                          0, 0, &buf->virt);
   if (ret) {
      fprintf(stderr, "buf%i: vspace map failed: %s\n", serial, strerror(-ret));
      return ret;
   }
   printf("buf%i: virtual address = 0x%010lx\n", serial, buf->virt);
   return 0;
}

static inline void clflush(volatile void *p)
{
   __asm__ __volatile__ ("clflush %0"
                         : : "m" ((unsigned long)p) : "memory");
}

static inline void mfence()
{
   __asm__ __volatile__ ("mfence");
}

static inline void BEGIN_RING(int c, uint32_t m, int s)
{
   ctx.pb[ctx.pb_pos++] = (0x2 << 28) | (s << 16) | (c << 13) | (m >> 2);
}

static inline void CONST_RING(int c, uint32_t m, int s)
{
   ctx.pb[ctx.pb_pos++] = (0x6 << 28) | (s << 16) | (c << 13) | (m >> 2);
}

static inline void OUT_RING(uint32_t d)
{
   ctx.pb[ctx.pb_pos++] = d;
}

static inline void OUT_RINGf(float f)
{
   union {
      float f32;
      uint32_t u32;
   } u;
   u.f32 = f;
   ctx.pb[ctx.pb_pos++] = u.u32;
}

static inline void FIRE_RING()
{
   uint64_t virt = ctx.ib_virt + ctx.pb_base * 4;
   uint32_t size = (ctx.pb_pos - ctx.pb_base) * 4;

   printf("BFORE: 0x88/0x8c = %i/%i\n",
          ctx.regs[0x88 / 4], ctx.regs[0x8c / 4]);

   if (!size)
      return;
   
   ctx.ib[ctx.ib_pos * 2 + 0] = virt;
   ctx.ib[ctx.ib_pos * 2 + 1] = (virt >> 32) | (size << 8);
   mfence();

   printf("FIRE_RING [%i]: 0x%08x 0x%08x\n", ctx.ib_pos + 1,
          ctx.ib[ctx.ib_pos * 2 + 0],
          ctx.ib[ctx.ib_pos * 2 + 1]);

   ++ctx.ib_pos;

   ctx.regs[0x8c / 4] = ctx.ib_pos;

   usleep(10);

   printf("AFTER: 0x88/0x8c = %i/%i\n",
          ctx.regs[0x88 / 4], ctx.regs[0x8c / 4]);

   ctx.pb_base = ctx.pb_pos;
}

int main(int argc, char **argv)
{
   int fd;
   int i, ret;
   int fd2;

   printf("opening drm device 1st\n");
   fd = drmOpen("pscnv", 0);
   printf("opening drm device 2nd\n");
   fd2 = 0; // drmOpen("pscnv", 0);

   if (fd == -1 || fd2 == -1) {
      perror("failed to open device");
      return 1;
   }

   ctx.pb_pos = 0x1000 / 4;
   ctx.pb_base = ctx.pb_pos;
   ctx.ib_pos = 0;

   ret = pscnv_gem_new(fd, 0xf1f0c0de, PSCNV_GEM_CONTIG, 0,
		       IB_SIZE, 0, &ctx.ib_gem, &ctx.ib_ofst);
   if (ret) {
      fprintf(stderr, "gem_new failed: %s\n", strerror(-ret));
      return 1;
   }
   printf("gem_new: h %d m %lx\n", ctx.ib_gem, ctx.ib_ofst);

   ctx.ib = mmap(0, IB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, ctx.ib_ofst);
   if (!ctx.ib)
      return 1;
   ctx.pb = ctx.ib;
   printf("IB at %p\n", ctx.ib);

   ret = pscnv_vspace_new(fd, &ctx.vid);
   if (ret) {
      fprintf(stderr, "vspace_new: %s\n", strerror(-ret));
      return 1;
   }
   printf("vid %d\n", ctx.vid);

   ret = pscnv_chan_new(fd, ctx.vid, &ctx.cid, &ctx.ch_ofst);
   if (ret) {
      fprintf(stderr, "chan_new failed: %s\n", strerror(-ret));
      return 1;
   }
   printf("cid %d regs %lx\n", ctx.cid, ctx.ch_ofst);


   ret = pscnv_vspace_map(fd, ctx.vid, ctx.ib_gem, 0x20000000, 1ULL << 32,
                          1, 0, &ctx.ib_virt);
   if (ret) {
      fprintf(stderr, "vspace_map of IB failed: %s\n", strerror(-ret));
      return 1;
   }
   printf ("IB virtual %lx, doing fifo_init next\n", ctx.ib_virt);

   ret = pscnv_fifo_init_ib(fd, ctx.cid, 0xbeef, 0, 1, ctx.ib_virt, 10);
   if (ret) {
      fprintf(stderr, "fifo_init failed: %s\n", strerror(-ret));
      return 1;
   }
   printf("FIFO initialized\n");

   ctx.regs = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, ctx.ch_ofst);
   if (!ctx.regs) {
      fprintf(stderr, "ERROR: mmap of FIFO regs failed\n");
      return 1;
   }
   printf("fifo regs at %p\n", ctx.regs);

   struct buf scratch;
   if (buf_new(fd, &scratch, 0x1000))
       return -1;
   if (buf_new(fd, &scratch, 0x1000))
       return -1;
   if (buf_new(fd, &scratch, 0x1000))
       return -1;

   for (i = 0; i < 0x1000 / 4; ++i)
	   scratch.map[i] = 0xfafafafa;

   const int m2mf = 1;

   BEGIN_RING(m2mf, 0x0000, 1);
   OUT_RING  (0x9039);
   BEGIN_RING(m2mf, NVC0_M2MF_NOTIFY_ADDRESS_HIGH, 2);
   OUT_RING  (scratch.virt >> 32);
   OUT_RING  (scratch.virt);

   FIRE_RING();

   usleep(1000000);

   BEGIN_RING(m2mf, NVC0_M2MF_OFFSET_OUT_HIGH, 2);
   OUT_RING  (scratch.virt >> 32);
   OUT_RING  (scratch.virt);
   BEGIN_RING(m2mf, NVC0_M2MF_LINE_LENGTH_IN, 2);
   OUT_RING  (8);
   OUT_RING  (1);
   BEGIN_RING(m2mf, NVC0_M2MF_EXEC, 1);
   OUT_RING  (0x100111);
   CONST_RING(m2mf, NVC0_M2MF_DATA, 2);
   OUT_RING  (0xaffeaffe);
   OUT_RING  (0xcafecafe);

   FIRE_RING();

   for (i = 0; i < 2; ++i)
      printf("BUF[%i] = 0x%08x\n", i, scratch.map[i]);

   close(fd);
   close(fd2);
        
   return 0;
}
