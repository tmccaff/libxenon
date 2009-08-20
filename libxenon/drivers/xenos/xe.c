#include "xe.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <cache.h>
#include <xenon_smc/xenon_smc.h>
#include <stdint.h>
#include <time/time.h>

#define RINGBUFFER_BASE 0x08000000
#define RINGBUFFER_SIZE 0x07000000

#define RPTR_WRITEBACK 0x10000
#define SCRATCH_WRITEBACK 0x10100

#define RINGBUFFER_PRIMARY_SIZE (0x8000/4)
#define RINGBUFFER_SECONDARY_SIZE (0x100000/4)

static inline int FLOAT(float f)
{
	union {
		float f;
		u32 d;
	} u = {f};
	
	return u.d;
}

#define rput32(d) *(volatile u32*)(xe->rb_secondary + xe->rb_secondary_wptr++ * 4) = (d);
#define rput(base, len) memcpy(((void*)xe->rb_secondary) + xe->rb_secondary_wptr * 4, (base), (len) * 4); xe->rb_secondary_wptr += (len);
#define rput32p(d) do { *(volatile u32*)(xe->rb_primary + xe->rb_primary_wptr++ * 4) = d; if (xe->rb_primary_wptr == RINGBUFFER_PRIMARY_SIZE) xe->rb_primary_wptr = 0; } while (0)

#define rputf(d) rput32(FLOAT(d));

#define r32(o) xe->regs[(o)/4]
#define w32(o, v) xe->regs[(o)/4] = (v)

#define RADEON_CP_PACKET0               0x00000000
#define RADEON_ONE_REG_WR                (1 << 15)

#define CP_PACKET0( reg, n )                                            \
        (RADEON_CP_PACKET0 | ((n) << 16) | ((reg) >> 2))
#define CP_PACKET0_TABLE( reg, n )                                      \
        (RADEON_CP_PACKET0 | RADEON_ONE_REG_WR | ((n) << 16) | ((reg) >> 2))

static inline void Xe_pWriteReg(struct XenosDevice *xe, u32 reg, u32 val)
{
	rput32(CP_PACKET0(reg, 0));
	rput32(val);
}

static inline u32 SurfaceInfo(int surface_pitch, int msaa_samples, int hi_zpitch)
{
	return surface_pitch | (msaa_samples << 16) | (hi_zpitch << 18);
}

static inline u32 xy32(int x, int y)
{
	return x | (y << 16);
}


void Xe_pSyncToDevice(struct XenosDevice *xe, volatile void *data, int len)
{
	memdcbst(data, len);
}

void Xe_pSyncFromDevice(struct XenosDevice *xe, volatile void *data, int len)
{
	memdcbf(data, len);
}

void *Xe_pAlloc(struct XenosDevice *xe, u32 *phys, int size, int align)
{
	void *r;
	if (!align)
		align = size;
	xe->alloc_ptr += (-xe->alloc_ptr) & (align-1);
	xe->alloc_ptr += align;
	r = ((void*)xe->rb) + xe->alloc_ptr;

	if (phys)
		*phys = RINGBUFFER_BASE + xe->alloc_ptr;
	xe->alloc_ptr += size;
//	printf("Xe_pAlloc: at %d kb\n", xe->alloc_ptr / 1024);
	if (xe->alloc_ptr > (RINGBUFFER_SIZE))
		Xe_Fatal(xe, "FATAL: out of memory. (alloc_ptr: %d kb, RINGBUFFER_SIZE: %d kb)\n", xe->alloc_ptr / 1024, RINGBUFFER_SIZE / 1024);
	return r;
}

void Xe_pInvalidateGpuCache_Primary(struct XenosDevice *xe, int base, int size)
{
	size +=  0x1FFF;
	size &= ~0x1FFF;

	rput32p(0x00000a31); 
		rput32p(0x01000000);
	rput32p(0x00010a2f); 
		rput32p(size); rput32p(base);
	rput32p(0xc0043c00); 
		rput32p(0x00000003); rput32p(0x00000a31); rput32p(0x00000000); rput32p(0x80000000); rput32p(0x00000008);
}


void Xe_pRBCommitPrimary(struct XenosDevice *xe)
{
	int i;
	for (i=0; i<0x20; ++i)
		rput32p(0x80000000);
	Xe_pSyncToDevice(xe, xe->rb_primary, RINGBUFFER_PRIMARY_SIZE * 4);
	__asm__ ("sync");
	w32(0x0714, xe->rb_primary_wptr);
//	printf("committed to %08x\n", rb_primary_wptr);
}

void Xe_pRBKickSegment(struct XenosDevice *xe, int base, int len)
{
//	printf("kick_segment: %x, len=%x\n", base, len * 4);
	Xe_pSyncToDevice(xe, xe->rb_secondary + base * 4, len * 4);
	Xe_pInvalidateGpuCache_Primary(xe, xe->rb_secondary_base + base * 4, len * 4 + 0x1000);
	rput32p(0xc0013f00);
		rput32p(xe->rb_secondary_base + base * 4); rput32p(len);
}

#define RINGBUFFER_SECONDARY_GUARD 0x20000

void Xe_pRBKick(struct XenosDevice *xe)
{
//	printf("kick: wptr = %x, last_wptr = %x\n", rb_secondary_wptr, last_wptr);
	
	Xe_pRBKickSegment(xe, xe->last_wptr, xe->rb_secondary_wptr - xe->last_wptr);

	xe->rb_secondary_wptr += (-xe->rb_secondary_wptr)&0x1F; /* 128byte align */
	
	if (xe->rb_secondary_wptr >= RINGBUFFER_SECONDARY_SIZE)
		Xe_Fatal(xe, "increase guardband");

	if (xe->rb_secondary_wptr > (RINGBUFFER_SECONDARY_SIZE - RINGBUFFER_SECONDARY_GUARD))
		xe->rb_secondary_wptr = 0;
	
	xe->last_wptr = xe->rb_secondary_wptr;

	Xe_pRBCommitPrimary(xe);
}

#define SEGMENT_SIZE 1024

void Xe_pRBMayKick(struct XenosDevice *xe)
{
//	printf("may kick: wptr = %x, last_wptr = %x\n", rb_secondary_wptr, last_wptr);
	int distance = xe->rb_secondary_wptr - xe->last_wptr;
	if (distance < 0)
		distance += RINGBUFFER_SECONDARY_SIZE;
	
	if (distance >= SEGMENT_SIZE)
		Xe_pRBKick(xe);
}

u32 Xe_pRBAlloc(struct XenosDevice *xe)
{
	u32 rb_primary_phys;
	xe->rb_primary = Xe_pAlloc(xe, &rb_primary_phys, RINGBUFFER_PRIMARY_SIZE * 4, 0);
	xe->rb_secondary = Xe_pAlloc(xe, &xe->rb_secondary_base, RINGBUFFER_SECONDARY_SIZE * 4, 0x100);
	return rb_primary_phys;
}

void Xe_pSetSurfaceClip(struct XenosDevice *xe, int offset_x, int offset_y, int sc_left, int sc_top, int sc_right, int sc_bottom)
{
	rput32(0x00022080);
		rput32(xy32(offset_x, offset_y));
		rput32(xy32(sc_left, sc_top));
		rput32(xy32(sc_right, sc_bottom));
}

void Xe_pSetBin(struct XenosDevice *xe, u32 mask_low, u32 select_low, u32 mask_hi, u32 select_hi)
{
	rput32(0xc0006000);
		rput32(mask_low);
	rput32(0xc0006200);
		rput32(select_low);
	rput32(0xc0006100);
		rput32(mask_hi);
	rput32(0xc0006300);
		rput32(select_hi); 
}

void Xe_pWaitUntilIdle(struct XenosDevice *xe, u32 what)
{
	rput32(0x000005c8);
		rput32(what);
}

void Xe_pDrawNonIndexed(struct XenosDevice *xe, int num_points, int primtype)
{
	rput32(0xc0012201);
	rput32(0x00000000);
	rput32(0x00000080 | (num_points << 16) | primtype);
}

void Xe_pDrawIndexedPrimitive(struct XenosDevice *xe, int primtype, int num_points, u32 indexbuffer, u32 indexbuffer_size, int indextype)
{
	assert(num_points < 65536);
	int type = 0;
	
	rput32(0xc0032201);
		rput32(0x00000000);
		rput32(0x00000000 | (type << 6) | primtype | (num_points << 16) | (indextype << 11));
		rput32(indexbuffer);
		rput32(indexbuffer_size | 0x40000000);
}

void Xe_pSetIndexOffset(struct XenosDevice *xe, int offset)
{
	rput32(0x00002102);
		rput32(offset);
}

void Xe_pResetRingbuffer(struct XenosDevice *xe)
{
	w32(0x0704, r32(0x0704) | 0x80000000);
	w32(0x017c, 0);
	w32(0x0714, 0);
	w32(0x0704, r32(0x0704) &~0x80000000);
}

void Xe_pSetupRingbuffer(struct XenosDevice *xe, u32 buffer_base, u32 size_in_l2qw)
{
	Xe_pResetRingbuffer(xe);
	w32(0x0704, size_in_l2qw | 0x8020000);
	w32(0x0700, buffer_base);
	w32(0x0718, 0x10);
}

void Xe_pLoadUcodes(struct XenosDevice *xe, const u32 *ucode0, const u32 *ucode1)
{
	int i;
	
	w32(0x117c, 0);
	udelay(100);
	
	for (i = 0; i < 0x120; ++i)
		w32(0x1180, ucode0[i]);

	w32(0x117c, 0);
	udelay(100);
	for (i = 0; i < 0x120; ++i)
		r32(0x1180);

	w32(0x07e0, 0);
	for (i = 0; i < 0x900; ++i)
		w32(0x07e8, ucode1[i]);

	w32(0x07e4, 0);
	for (i = 0; i < 0x900; ++i)
		if (r32(0x07e8) != ucode1[i])
		{
			printf("%04x: %08x %08x\n", i, r32(0x07e8), ucode1[i]);
			break;
		}

	if (i != 0x900)
		Xe_Fatal(xe, "ucode1 microcode verify error\n");	
}

void Xe_pWaitReady(struct XenosDevice *xe)
{
	int timeout = 1<<24;
	while (r32(0x1740) & 0x80000000)
	{
		if (!timeout--)
			Xe_Fatal(xe, "timeout in init, likely the GPU was already hung before we started\n");
	}
}

void Xe_pWaitReady2(struct XenosDevice *xe)
{
	while (!(r32(0x1740) & 0x00040000));
}

void Xe_pInit1(struct XenosDevice *xe)
{
	w32(0x01a8, 0);
	w32(0x0e6c, 0xC0F0000);
	w32(0x3400, 0x40401);
	udelay(1000);
	w32(0x3400, 0x40400);
	w32(0x3300, 0x3A22);
	w32(0x340c, 0x1003F1F);
	w32(0x00f4, 0x1E);
}

void Xe_pReset(struct XenosDevice *xe)
{
	Xe_pWaitReady2(xe);
	Xe_pWaitReady(xe);
#if 1
	printf("waiting for reset.\n");
	do {
		w32(0x00f0, 0x8064); r32(0x00f0);
		w32(0x00f0, 0);
		w32(0x00f0, 0x11800); r32(0x00f0);
		w32(0x00f0, 0);
		udelay(1000);
	} while (r32(0x1740) & 0x80000000);
#endif

	if (r32(0x00e0) != 0x10)
		Xe_Fatal(xe, "value after reset not ok (%08x)\n", r32(0xe0));

	Xe_pInit1(xe);
}

void Xe_pInit0(struct XenosDevice *xe, u32 buffer_base, u32 size_in_l2qw)
{
	w32(0x07d8, 0x1000FFFF);
	udelay(2000);
	w32(0x00f0, 1);
	(void)r32(0x00f0);
	udelay(1000);
	w32(0x00f0, 0);
	udelay(1000);
	Xe_pSetupRingbuffer(xe, buffer_base, size_in_l2qw);
	Xe_pWaitReady(xe);

	if (!(r32(0x07d8) & 0x10000000))
		Xe_Fatal(xe, "something wrong (1)\n");

	w32(0x07d8, 0xFFFF);
	udelay(1000);

	w32(0x3214, 7);
	w32(0x3294, 1);
	w32(0x3408, 0x800);
	
	Xe_pWaitReady(xe);
	
	if (r32(0x0714))
		Xe_Fatal(xe, "[WARN] something wrong (2)\n");
	
	if (r32(0x0710))
		Xe_Fatal(xe, "[WARN] something wrong (3)\n");

	w32(0x07ec, 0x1A);
}

void Xe_pSetup(struct XenosDevice *xe, u32 buffer_base, u32 buffer_size, const u32 *ucode0, const u32 *ucode1)
{
	Xe_pWaitReady(xe);

	w32(0x07d8, 0x1000FFFF);
	
	Xe_pSetupRingbuffer(xe, buffer_base, buffer_size);
	Xe_pLoadUcodes(xe, ucode0, ucode1);
	Xe_pWaitReady(xe);
	
	w32(0x07d8, 0xFFFF);
	w32(0x07d0, 0xFFFF);
	w32(0x07f0, 0);
	w32(0x0774, 0);
	w32(0x0770, 0);
	w32(0x3214, 7);
	w32(0x3294, 1);
	w32(0x3408, 0x800);
	Xe_pInit0(xe, buffer_base, buffer_size);
	Xe_pWaitReady(xe);
}

static u32 ucode0[] = {
	0xC60400,
	0x7E424B, 0xA00000, 0x7E828B, 0x800001, 0xC60400, 0xCC4003, 0x800000, 0xD60003, 0xC60800, 0xC80C1D, 0x98C007, 0xC61000, 0x978003, 0xCC4003, 0xD60004, 0x800000, 0xCD0003, 0x9783EF, 0xC60400, 0x800000, 0xC60400, 0xC60800, 0x348C08,
	0x98C006, 0xC80C1E, 0x98C000, 0xC80C1E, 0x80001F, 0xCC8007, 0xCC8008, 0xCC4003, 0x800000, 0xCC8003, 0xC60400, 0x1AAC07, 0xCA8821, 0x96C015, 0xC8102C, 0x98800A, 0x329418, 0x9A4004, 0xCC6810, 0x42401, 0xD00143, 0xD00162, 0xCD0002,
	0x7D514C, 0xCD4003, 0x9B8007, 0x6A801, 0x964003, 0xC28000, 0xCF4003, 0x800001, 0xC60400, 0x800023, 0xC60400, 0x964003, 0x7E424B, 0xD00283, 0xC8102B, 0xC60800, 0x99000E, 0xC80C29, 0x98C00A, 0x345002, 0xCD0002, 0xCC8002, 0xD001E3,
	0xD00183, 0xCC8003, 0xCC4018, 0x80004D, 0xCC8019, 0xD00203, 0xD00183, 0x9783B4, 0xC60400, 0xC8102B, 0xC60800, 0x9903AF, 0xC80C2A, 0x98C00A, 0x345002, 0xCD0002, 0xCC8002, 0xD001E3, 0xD001A3, 0xCC8003, 0xCC401A, 0x800000, 0xCC801B,
	0xD00203, 0xD001A3, 0x800001, 0xC60400, 0xC60800, 0xC60C00, 0xC8102D, 0x349402, 0x99000B, 0xC8182E, 0xCD4002, 0xCD8002, 0xD001E3, 0xD001C3, 0xCCC003, 0xCC801C, 0xCD801D, 0x800001, 0xC60400, 0xD00203, 0x800000, 0xD001C3, 0xC8081F,
	0xC60C00, 0xC80C20, 0x988000, 0xC8081F, 0xCC4003, 0xCCC003, 0xD60003, 0x800000, 0xCCC022, 0xC81C2F, 0xC60400, 0xC60800, 0xC60C00, 0xC81030, 0x99C000, 0xC81C2F, 0xCC8021, 0xCC4020, 0x990011, 0xC107FF, 0xD00223, 0xD00243, 0x345402,
	0x7CB18B, 0x7D95CC, 0xCDC002, 0xCCC002, 0xD00263, 0x978005, 0xCCC003, 0xC60800, 0x80008B, 0xC60C00, 0x800000, 0xD00283, 0x97836A, 0xC60400, 0xD6001F, 0x800001, 0xC60400, 0xC60800, 0xC60C00, 0xC61000, 0x348802, 0xCC8002, 0xCC4003,
	0xCCC003, 0xCD0002, 0x800000, 0xCD0003, 0xD2000D, 0xCC000D, 0x800000, 0xCC000D, 0xC60800, 0xC60C00, 0xCA1433, 0xD022A0, 0xCCE000, 0x994351, 0xCCE005, 0x800000, 0x62001, 0xC60800, 0xC60C00, 0xD022A0, 0xCCE000, 0xD022AE, 0xCCE029,
	0xCCE005, 0x800000, 0x62001, 0x964000, 0xC82435, 0xCA0838, 0x366401, 0x964340, 0xCA0C3A, 0xCCA000, 0xCCE000, 0xCCE029, 0xCCE005, 0x800000, 0x62001, 0xC60800, 0xC60C00, 0xD202C3, 0xCC8003, 0xCCC003, 0xCCE027, 0x800000, 0x62001, 0xCA0831,
	0x9883FF, 0xCA0831, 0xD6001F, 0x800001, 0xC60400, 0xD02360, 0xD02380, 0xD02385, 0x800000, 0x62001, 0xA2001, 0xCA0436, 0x9843DF, 0xC82435, 0x800001, 0xC60400, 0xD20009, 0xD2000A, 0xCC001F, 0x800000, 0xCC001F, 0xD2000B, 0xD2000C, 0xCC001F,
	0x800000, 0xCC001F, 0xCC0023, 0xCC4003, 0x800000, 0xD60003, 0xD00303, 0xCC0024, 0xCC4003, 0x800000, 0xD60003, 0xD00323, 0xCC0025, 0xCC4003, 0x800000, 0xD60003, 0xD00343, 0xCC0026, 0xCC4003, 0x800000, 0xD60003, 0x800000, 0xD6001F,
	0x100EF, 0x200F4, 0x300F9, 0x50004, 0x600D6, 0x1000FE, 0x1700DB, 0x220009, 0x230016, 0x250022, 0x270061, 0x2D0073, 0x2E007D, 0x2F009C, 0x3700C8, 0x3800B3, 0x3B00A6, 0x3F00AA, 0x4800EB, 0x5000E1, 0x5100E6, 0x5500F0, 0x5600F5, 0x5700FA,
	0x5D00D0, 6, 6, 6, 6, 6, 6, 6};

static u32 ucode1[] = {
	0,
	0xC0200400, 0, 0,	0xA0000A, 0, 0x1F3, 0x204411, 0, 0x1000000, 0x204811, 0, 0,
	0x400000, 4, 0xFFFF, 0x284621, 0, 0,
	0xD9004800, 0, 0, 0x400000, 0, 0,
	0x34E00000, 0, 0, 0x600000, 0x24A, 0xFFFF, 0xC0280A20, 0, 0,
	0x294582, 0, 0, 0xD9004800, 0, 0,
	0x400000, 0, 0, 0x600000, 0x24A, 0xFFFF, 0xC0284620, 0, 0,
	0xD9004800, 0, 0,
	0x400000, 0, 0,
	0x600000, 0x267, 0x21FC, 0x29462C, 0, 0,
	0xC0204800, 0, 0,
	0x400000, 0, 0,
	0x600000, 0x267, 0x21FC, 0x29462C, 0, 0,
	0xC0204800, 0,
	0x3FFF, 0x2F022F, 0, 0,
	0xCE00000, 0,
	0xA1FD, 0x29462C, 0, 0,
	0xD9004800, 0, 0,
	0x400000, 0,
	0x394, 0x204411, 0,
	1, 0xC0404811, 0, 0,
	0x600000, 0x267, 0x21F9, 0x29462C, 0,
	8, 0xC0210A20, 0, 0,
	0x14E00000, 0x25, 7, 0x404811, 0,
	8, 0x404811, 0, 0,
	0x600000, 0x267, 0x21FC, 0x29462C, 0, 0,
	0xC0204800, 0,
	0xA1FD, 0x29462C, 0, 0,
	0xC0200800, 0, 0,
	0x2F0222, 0, 0,
	0xCE00000, 0, 0,
	0x40204800, 0,
	1, 0x40304A20, 0,
	2, 0xC0304A20, 0,
	1, 0x530A22, 0x2B, 0x80000000, 0xC0204411, 0,
	1, 0x604811, 0x281, 0,
	0x400000, 0, 0,
	0xC0200000, 0,
	0x12B9B0A1, 0xC02F0220, 0, 0,
	0xCC00000, 0x3A, 0x1033C4D6, 0xC02F0220, 0, 0,
	0xCC00000, 0x3A, 0,
	0x400000, 0,
	0x1F3, 0x204411, 0,
	0x8000000, 0x204811, 0, 0,
	0x400000, 0x3C, 0x80000000, 0xC0204411, 0, 0,
	0x604811, 0x281, 0,
	0x400000, 0,
	0x1F, 0x40280A20, 0,
	0x1B, 0x2F0222, 0, 0,
	0xCE00000, 0x57, 2, 0x2F0222, 0, 0,
	0xCE00000, 0x5E, 3, 0x2F0222, 0, 0,
	0xCE00000, 0x65, 4, 0x2F0222, 0, 0,
	0xCE00000, 0x6C, 0x14, 0x2F0222, 0, 0,
	0xCE00000, 0x6C, 0x1A, 0x2F0222, 0, 0,
	0xCE00000, 0x74, 0x15, 0x2F0222, 0, 0,
	0xCE00000, 0x79, 0x21F9, 0x29462C, 0, 0,
	0xC0404802, 0,
	0x1F, 0x40280A20, 0,
	0x1B, 0x2F0222, 0, 0,
	0xCE00000, 0x57, 2, 0x2F0222, 0, 0,
	0xCE00000, 0x5E, 0,
	0x400000, 0x65, 0x1F, 0xC0210E20, 0,
	0x612, 0x204411, 0, 0,
	0x204803, 0, 0,
	0xC0204800, 0, 0,
	0xC0204800, 0,
	0x21F9, 0x29462C, 0, 0,
	0x404802, 0,
	0x1E, 0xC0210E20, 0,
	0x600, 0x204411, 0, 0,
	0x204803, 0, 0,
	0xC0204800, 0, 0,
	0xC0204800, 0,
	0x21F9, 0x29462C, 0, 0,
	0x404802, 0,
	0x1E, 0xC0210E20, 0,
	0x605, 0x204411, 0, 0,
	0x204803, 0, 0,
	0xC0204800, 0, 0,
	0xC0204800, 0,
	0x21F9, 0x29462C, 0, 0,
	0x404802, 0,
	0x1F, 0x40280A20, 0,
	0x1F, 0xC0210E20, 0,
	0x60A, 0x204411, 0, 0,
	0x204803, 0, 0,
	0xC0204800, 0, 0,
	0xC0204800, 0,
	0x21F9, 0x29462C, 0, 0,
	0x404802, 0,
	0x1F, 0xC0280A20, 0,
	0x611, 0x204411, 0, 0,
	0xC0204800, 0,
	0x21F9, 0x29462C, 0, 0,
	0x404802, 0,
	0x1F, 0xC0280A20, 0, 0,
	0x600000, 0x267, 0x21F9, 0x29462C, 0, 0,
	0x404802, 0,
	0x81000000, 0x204411, 0,
	1, 0x204811, 0,
	0x1FFF, 0x40280A20, 0,
	0x80000000, 0x40280E20, 0,
	0x40000000, 0xC0281220, 0,
	0x40000, 0x294622, 0, 0,
	0x600000, 0x282, 0,
	0x201410, 0, 0,
	0x2F0223, 0, 0,
	0xCC00000, 0x88, 0,
	0xC0401800, 0x8C, 0x1FFF, 0xC0281A20, 0,
	0x40000, 0x294626, 0, 0,
	0x600000, 0x282, 0,
	0x201810, 0, 0,
	0x2F0224, 0, 0,
	0xCC00000, 0x8F, 0,
	0xC0401C00, 0x93, 0x1FFF, 0xC0281E20, 0,
	0x40000, 0x294627, 0, 0,
	0x600000, 0x282, 0,
	0x201C10, 0, 0,
	0x204402, 0, 0,
	0x2820C5, 0, 0,
	0x4948E8, 0, 0,
	0x600000, 0x24A, 0x10, 0x40210A20, 0,
	0xFF, 0x280A22, 0,
	0x7FF, 0x40280E20, 0,
	2, 0x221E23, 0,
	5, 0xC0211220, 0,
	0x80000, 0x281224, 0,
	0x13, 0x210224, 0, 0,
	0x14C00000, 0xA1, 0xA1000000, 0x204411, 0, 0,
	0x204811, 0, 0,
	0x2F0222, 0, 0,
	0xCC00000, 0xA5, 8, 0x20162D, 0,
	0x4000, 0x500E23, 0xB4, 1, 0x2F0222, 0, 0,
	0xCC00000, 0xA9, 9, 0x20162D, 0,
	0x4800, 0x500E23, 0xB4, 2, 0x2F0222, 0, 0,
	0xCC00000, 0xAD, 0x37, 0x20162D, 0,
	0x4900, 0x500E23, 0xB4, 3, 0x2F0222, 0, 0,
	0xCC00000, 0xB1, 0x36, 0x20162D, 0,
	0x4908, 0x500E23, 0xB4, 0x29, 0x20162D, 0,
	0x2000, 0x300E23, 0, 0,
	0x290D83, 0,
	0x94000000, 0x204411, 0, 0,
	0x2948E5, 0, 0,
	0x294483, 0, 0,
	0x40201800, 0, 0,
	0xD9004800, 0,
	0x13, 0x210224, 0, 0,
	0x14C00000, 0,
	0x94000000, 0x204411, 0, 0,
	0x2948E5, 0,
	0x93000000, 0x204411, 0, 0,
	0x404806, 0, 0,
	0x600000, 0x24A, 0,
	0xC0200800, 0, 0,
	0xC0201400, 0,
	0x1F, 0x211A25, 0, 0,
	0x14E00000, 0,
	0x7FF, 0x280E25, 0,
	0x10, 0x211225, 0,
	0x83000000, 0x204411, 0, 0,
	0x2F0224, 0, 0,
	0xAE00000, 0xCB, 8, 0x203622, 0,
	0x4000, 0x504A23, 0xDA, 1, 0x2F0224, 0, 0,
	0xAE00000, 0xCF, 9, 0x203622, 0,
	0x4800, 0x504A23, 0xDA, 2, 0x2F0224, 0, 0,
	0xAE00000, 0xD3, 0x37, 0x203622, 0,
	0x4900, 0x504A23, 0xDA, 3, 0x2F0224, 0, 0,
	0xAE00000, 0xD7, 0x36, 0x203622, 0,
	0x4908, 0x504A23, 0xDA, 0x29, 0x203622, 0, 0,
	0x290D83, 0,
	0x2000, 0x304A23, 0,
	0x84000000, 0x204411, 0, 0,
	0xC0204800, 0, 0,
	0x21000000, 0, 0,
	0x400000, 0xC1, 0,
	0x600000, 0x24A, 0x83000000, 0x204411, 0,
	0x4000, 0xC0304A20, 0,
	0x84000000, 0x204411, 0, 0,
	0xC0204800, 0, 0,
	0x21000000, 0, 0,
	0x400000, 0,
	0x81000000, 0x204411, 0,
	1, 0x204811, 0,
	0x40578, 0x204411, 0, 0,
	0x600000, 0x282, 0,
	0xC0400000, 0, 0,
	0xC0200C00, 0, 0,
	0xC0201000, 0, 0,
	0xC0201400, 0, 0,
	0xC0201800, 0,
	0x7F00, 0x280A21, 0,
	0x4500, 0x2F0222, 0, 0,
	0xCE00000, 0xF2, 0,
	0xC0201C00, 0, 0,
	0x17000000, 0,
	0x10, 0x280A23, 0,
	0x10, 0x2F0222, 0, 0,
	0xCE00000, 0xFB, 0x81000000, 0x204411, 0,
	1, 0x204811, 0,
	0x40000, 0x294624, 0, 0,
	0x600000, 0x282, 0,
	0x400000, 0x103, 0x81000000, 0x204411, 0, 0,
	0x204811, 0,
	0x1EA, 0x204411, 0, 0,
	0x204804, 0, 0,
	0x1AC00000, 0xFF, 0x9E000000, 0x204411, 0,
	0xDEADBEEF, 0x204811, 0, 0,
	0x1AE00000, 0x102, 0,
	0x2820D0, 0,
	7, 0x280A23, 0,
	1, 0x2F0222, 0, 0,
	0xAE00000, 0x10A, 0,
	0x2F00A8, 0, 0,
	0x4E00000, 0x123, 0,
	0x400000, 0x12A, 2, 0x2F0222, 0, 0,
	0xAE00000, 0x10F, 0,
	0x2F00A8, 0, 0,
	0x2E00000, 0x123, 0,
	0x400000, 0x12A, 3, 0x2F0222, 0, 0,
	0xAE00000, 0x114, 0,
	0x2F00A8, 0, 0,
	0xCE00000, 0x123, 0,
	0x400000, 0x12A, 4, 0x2F0222, 0, 0,
	0xAE00000, 0x119, 0,
	0x2F00A8, 0, 0,
	0xAE00000, 0x123, 0,
	0x400000, 0x12A, 5, 0x2F0222, 0, 0,
	0xAE00000, 0x11E, 0,
	0x2F00A8, 0, 0,
	0x6E00000, 0x123, 0,
	0x400000, 0x12A, 6, 0x2F0222, 0, 0,
	0xAE00000, 0x123, 0,
	0x2F00A8, 0, 0,
	0x8E00000, 0x123, 0,
	0x400000, 0x12A, 0x7F00, 0x280A21, 0,
	0x4500, 0x2F0222, 0, 0,
	0xAE00000, 0,
	8, 0x210A23, 0, 0,
	0x14E00000, 0x14A, 0,
	0xC0204400, 0, 0,
	0xC0404800, 0,
	0x7F00, 0x280A21, 0,
	0x4500, 0x2F0222, 0, 0,
	0xAE00000, 0x12F, 0,
	0xC0200000, 0, 0,
	0xC0400000, 0, 0,
	0x404C07, 0xF2, 0,
	0xC0201000, 0, 0,
	0xC0201400, 0, 0,
	0xC0201800, 0, 0,
	0xC0201C00, 0, 0,
	0x17000000, 0,
	0x81000000, 0x204411, 0,
	1, 0x204811, 0,
	0x40000, 0x294624, 0, 0,
	0x600000, 0x282, 0,
	0x2820D0, 0, 0,
	0x2F00A8, 0, 0,
	0xCE00000, 0, 0,
	0x404C07, 0x134, 0,
	0xC0201000, 0, 0,
	0xC0201400, 0, 0,
	0xC0201800, 0, 0,
	0xC0201C00, 0, 0,
	0x17000000, 0,
	0x81000000, 0x204411, 0,
	1, 0x204811, 0,
	0x40000, 0x294624, 0, 0,
	0x600000, 0x282, 0,
	0x2820D0, 0, 0,
	0x2F00A8, 0, 0,
	0x6E00000, 0, 0,
	0x404C07, 0x141, 0x60D, 0x204411, 0, 0,
	0xC0204800, 0, 0,
	0xC0404800, 0,
	0x81000000, 0x204411, 0,
	9, 0x204811, 0,
	0x60D, 0x204411, 0, 0,
	0xC0204800, 0, 0,
	0x404810, 0,
	0x1FFF, 0xC0280A20, 0,
	0x20000, 0x294622, 0,
	0x18, 0xC0424A20, 0,
	0x81000000, 0x204411, 0,
	1, 0x204811, 0,
	0x40000, 0xC0294620, 0, 0,
	0x600000, 0x282, 0x60D, 0x204411, 0, 0,
	0xC0204800, 0, 0,
	0x404810, 0,
	0x1F3, 0x204411, 0,
	0xE0000000, 0xC0484A20, 0, 0,
	0xD9000000, 0, 0,
	0x400000, 0,
	0x45D, 0x204411, 0,
	0x3F, 0xC0484A20, 0, 0,
	0x600000, 0x24A, 0x81000000, 0x204411, 0,
	2, 0x204811, 0,
	0xFF, 0x280E30, 0, 0,
	0x2F0223, 0, 0,
	0xCC00000, 0x165, 0,
	0x200411, 0,
	0x1D, 0x203621, 0,
	0x1E, 0x203621, 0, 0,
	0xC0200800, 0,
	9, 0x210222, 0, 0,
	0x14C00000, 0x171, 0,
	0x600000, 0x275, 0,
	0x200C11, 0,
	0x38, 0x203623, 0, 0,
	0x210A22, 0, 0,
	0x14C00000, 0x17A, 0,
	0xC02F0220, 0, 0,
	0x400000, 0x177, 0,
	0x600000, 0x1D8, 0,
	0x400000, 0x178, 0,
	0x600000, 0x1DC, 0xA0000000, 0x204411, 0, 0,
	0x204811, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x17F, 0xF1FFFFFF, 0x283A2E, 0,
	0x1A, 0xC0220E20, 0, 0,
	0x29386E, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x189, 0xE, 0xC0203620, 0,
	0xF, 0xC0203620, 0,
	0x10, 0xC0203620, 0,
	0x11, 0xC0203620, 0,
	0x12, 0xC0203620, 0,
	0x13, 0xC0203620, 0,
	0x14, 0xC0203620, 0,
	0x15, 0xC0203620, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x1AC, 0,
	0xC0200C00, 0,
	0x8C000000, 0x204411, 0, 0,
	0x204803, 0,
	0xFFF, 0x281223, 0,
	0x19, 0x203624, 0,
	3, 0x381224, 0,
	0x5000, 0x301224, 0,
	0x18, 0x203624, 0,
	0x87000000, 0x204411, 0, 0,
	0x204804, 0,
	1, 0x331224, 0,
	0x86000000, 0x204411, 0, 0,
	0x204804, 0,
	0x88000000, 0x204411, 0,
	0x7FFF, 0x204811, 0,
	0x10, 0x211623, 0,
	0xFFF, 0x281A23, 0, 0,
	0x331CA6, 0,
	0x8F000000, 0x204411, 0,
	3, 0x384A27, 0,
	0x10, 0x211223, 0,
	0x17, 0x203624, 0,
	0x8B000000, 0x204411, 0, 0,
	0x204804, 0,
	3, 0x381224, 0,
	0x5000, 0x301224, 0,
	0x16, 0x203624, 0,
	0x85000000, 0x204411, 0, 0,
	0x204804, 0,
	0x1000, 0x331CD1, 0,
	0x90000000, 0x204411, 0,
	3, 0x384A27, 0,
	0x300000, 0x293A2E, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x1B9, 0xA3000000, 0x204411, 0, 0,
	0x40204800, 0,
	0xA, 0xC0220E20, 0,
	0x21, 0x203623, 0, 0,
	0x600000, 0x1DC, 0xFFFFE000, 0x200411, 0,
	0x2E, 0x203621, 0,
	0x2F, 0x203621, 0,
	0x1FFF, 0x200411, 0,
	0x30, 0x203621, 0,
	0x31, 0x203621, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x1BC, 0,
	0xC0200000, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x1C2, 0x9C000000, 0x204411, 0,
	0x1F, 0x40214A20, 0,
	0x96000000, 0x204411, 0, 0,
	0xC0204800, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x1CB, 0x3FFFFFFF, 0x283A2E, 0,
	0xC0000000, 0x40280E20, 0, 0,
	0x29386E, 0,
	0x18000000, 0x40280E20, 0,
	0x38, 0x203623, 0,
	0xA4000000, 0x204411, 0, 0,
	0xC0204800, 0,
	1, 0x210A22, 0, 0,
	0x14C00000, 0x1D7, 0,
	0xC0200C00, 0,
	0x2B, 0x203623, 0,
	0x2D, 0x203623, 0,
	2, 0x40221220, 0, 0,
	0x301083, 0,
	0x2C, 0x203624, 0,
	3, 0xC0210E20, 0,
	0x10000000, 0x280E23, 0,
	0xEFFFFFFF, 0x283A2E, 0, 0,
	0x29386E, 0, 0,
	0x400000, 0,
	0x25F4, 0x204411, 0,
	0xA, 0x214A2C, 0, 0,
	0x600000, 0x273, 0,
	0x800000, 0,
	0x21F4, 0x204411, 0,
	0xA, 0x214A2C, 0, 0,
	0x600000, 0x275, 0,
	0x800000, 0, 0,
	0x600000, 0x24A, 0,
	0xC0200800, 0,
	0x1F, 0x210E22, 0, 0,
	0x14E00000, 0,
	0x3FF, 0x280E22, 0,
	0x18, 0x211222, 0,
	0xE, 0x301224, 0, 0,
	0x20108D, 0,
	0x2000, 0x291224, 0,
	0x83000000, 0x204411, 0, 0,
	0x294984, 0,
	0x84000000, 0x204411, 0, 0,
	0x204803, 0, 0,
	0x21000000, 0, 0,
	0x400000, 0x1E1, 0x82000000, 0x204411, 0,
	1, 0x204811, 0, 0,
	0xC0200800, 0,
	0x3FFF, 0x40280E20, 0,
	0x10, 0xC0211220, 0, 0,
	0x2F0222, 0, 0,
	0xAE00000, 0x1FE, 0,
	0x2AE00000, 0x208, 0x20000080, 0x281E2E, 0,
	0x80, 0x2F0227, 0, 0,
	0xCE00000, 0x1FB, 0,
	0x401C0C, 0x1FC, 0x20, 0x201E2D, 0,
	0x21F9, 0x294627, 0, 0,
	0x404811, 0x208, 1, 0x2F0222, 0, 0,
	0xAE00000, 0x23D, 0,
	0x28E00000, 0x208, 0x800080, 0x281E2E, 0,
	0x80, 0x2F0227, 0, 0,
	0xCE00000, 0x205, 0,
	0x401C0C, 0x206, 0x20, 0x201E2D, 0,
	0x21F9, 0x294627, 0,
	1, 0x204811, 0,
	0x81000000, 0x204411, 0, 0,
	0x2F0222, 0, 0,
	0xAE00000, 0x20F, 3, 0x204811, 0,
	0x16, 0x20162D, 0,
	0x17, 0x201A2D, 0,
	0xFFDFFFFF, 0x483A2E, 0x213, 4, 0x204811, 0,
	0x18, 0x20162D, 0,
	0x19, 0x201A2D, 0,
	0xFFEFFFFF, 0x283A2E, 0, 0,
	0x201C10, 0, 0,
	0x2F0067, 0, 0,
	0x6C00000, 0x208, 0x81000000, 0x204411, 0,
	6, 0x204811, 0,
	0x83000000, 0x204411, 0, 0,
	0x204805, 0,
	0x89000000, 0x204411, 0, 0,
	0x204806, 0,
	0x84000000, 0x204411, 0, 0,
	0x204803, 0, 0,
	0x21000000, 0, 0,
	0x601010, 0x24A, 0xC, 0x221E24, 0, 0,
	0x2F0222, 0, 0,
	0xAE00000, 0x230, 0x20000000, 0x293A2E, 0,
	0x21F7, 0x29462C, 0, 0,
	0x2948C7, 0,
	0x81000000, 0x204411, 0,
	5, 0x204811, 0,
	0x16, 0x203630, 0,
	7, 0x204811, 0,
	0x17, 0x203630, 0,
	0x91000000, 0x204411, 0, 0,
	0x204803, 0, 0,
	0x23000000, 0,
	0x8D000000, 0x204411, 0, 0,
	0x404803, 0x243, 0x800000, 0x293A2E, 0,
	0x21F6, 0x29462C, 0, 0,
	0x2948C7, 0,
	0x81000000, 0x204411, 0,
	5, 0x204811, 0,
	0x18, 0x203630, 0,
	7, 0x204811, 0,
	0x19, 0x203630, 0,
	0x92000000, 0x204411, 0, 0,
	0x204803, 0, 0,
	0x25000000, 0,
	0x8E000000, 0x204411, 0, 0,
	0x404803, 0x243, 0x83000000, 0x204411, 0,
	3, 0x381224, 0,
	0x5000, 0x304A24, 0,
	0x84000000, 0x204411, 0, 0,
	0x204803, 0, 0,
	0x21000000, 0,
	0x82000000, 0x204411, 0, 0,
	0x404811, 0,
	0x1F3, 0x204411, 0,
	0x4000000, 0x204811, 0, 0,
	0x400000, 0x247, 0,
	0xC0600000, 0x24A, 0,
	0x400000, 0, 0,
	0xEE00000, 0x281, 0x21F9, 0x29462C, 0,
	5, 0x204811, 0, 0,
	0x202C0C, 0,
	0x21, 0x20262D, 0, 0,
	0x2F012C, 0, 0,
	0xCC00000, 0x252, 0,
	0x403011, 0x253, 0x400, 0x30322C, 0,
	0x81000000, 0x204411, 0,
	2, 0x204811, 0,
	0xA, 0x21262C, 0, 0,
	0x210130, 0, 0,
	0x14C00000, 0x25B, 0xA5000000, 0x204411, 0,
	1, 0x204811, 0, 0,
	0x400000, 0x256, 0xA5000000, 0x204411, 0, 0,
	0x204811, 0, 0,
	0x2F016C, 0, 0,
	0xCE00000, 0x263, 0x21F4, 0x29462C, 0,
	0xA, 0x214A2B, 0,
	0x4940, 0x204411, 0,
	0xDEADBEEF, 0x204811, 0, 0,
	0x600000, 0x26E, 0xDFFFFFFF, 0x283A2E, 0,
	0xFF7FFFFF, 0x283A2E, 0,
	0x20, 0x80362B, 0,
	0x97000000, 0x204411, 0, 0,
	0x20480C, 0,
	0xA2000000, 0x204411, 0, 0,
	0x204811, 0,
	0x81000000, 0x204411, 0,
	2, 0x204811, 0, 0,
	0x810130, 0,
	0xA2000000, 0x204411, 0,
	1, 0x204811, 0,
	0x81000000, 0x204411, 0,
	2, 0x204811, 0, 0,
	0x810130, 0,
	0x400, 0x203011, 0,
	0x20, 0x80362C, 0, 0,
	0x203011, 0,
	0x20, 0x80362C, 0,
	0x1F, 0x201E2D, 0,
	4, 0x291E27, 0,
	0x1F, 0x803627, 0,
	0x21F9, 0x29462C, 0,
	6, 0x204811, 0,
	0x5C8, 0x204411, 0,
	0x10000, 0x204811, 0,
	0xE00, 0x204411, 0,
	1, 0x804811, 0, 0,
	0xC0400000, 0, 0,
	0x800000, 0, 0,
	0x1AC00000, 0x282, 0x9F000000, 0x204411, 0,
	0xDEADBEEF, 0x204811, 0, 0,
	0x1AE00000, 0x285, 0,
	0x800000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0x2015E, 0x20002, 0,
	0x20002, 0x3D0031, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x1E00002, 0,
	0x7D01F1, 0x200012, 0,
	0x20002, 0x2001E, 0,
	0x20002, 0x1EF0002, 0,
	0x960002, 0xDE0002, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20016, 0,
	0x20002, 0x20026, 0,
	0x14A00EA, 0x20155, 0,
	0x2015C, 0x15E0002, 0,
	0xEA0002, 0x15E0040, 0,
	0xBF0162, 0x20002, 0,
	0x1520002, 0x14D0002, 0,
	0x20002, 0x13D0130, 0,
	0x90160, 0xE000E, 0,
	0x6C0051, 0x790074, 0,
	0x200E5, 0x20248, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20002, 0,
	0x20002, 0x20002, 0,
	0x50280, 0x20008, 0,
};


void Xe_pMasterInit(struct XenosDevice *xe, u32 buffer_base)
{
	if ((r32(0x0e6c) & 0xF00) != 0xF00)
		printf("something wrong (3)\n");

	Xe_pSetup(xe, buffer_base, 0xC, ucode0, ucode1);

	w32(0x07d4, 0);
	w32(0x07d4, 1);

	w32(0x2054, 0x1E);
	w32(0x2154, 0x1E);
	
	w32(0x3c10, 0xD);
	
	w32(0x3c40, 0x17);
	w32(0x3c48, 0);
	while (r32(0x3c4c) & 0x80000000);

	w32(0x3c40, 0x1017);
	w32(0x3c48, 0);
	while (r32(0x3c4c) & 0x80000000);

	w32(0x87e4, 0x17);
}

void Xe_pEnableWriteback(struct XenosDevice *xe, u32 addr, int blocksize)
{
	u32 v = r32(0x0704);

	v &= ~0x8003F00;
	w32(0x0704, v);
	
	w32(0x070c, addr | 2);
	w32(0x0704, v | (blocksize << 8));
}

void Xe_pGInit0(struct XenosDevice *xe)
{
	rput32(0xc0003b00);
		rput32(0x00000300);
	
	rput32(0xc0192b00);
		rput32(0x00000000); rput32(0x00000018); 
		rput32(0x00001003); rput32(0x00001200); rput32(0xc4000000); rput32(0x00001004); 
		rput32(0x00001200); rput32(0xc2000000); rput32(0x00001005); rput32(0x10061200); 
		rput32(0x22000000); rput32(0xc8000000); rput32(0x00000000); rput32(0x02000000); 
		rput32(0xc800c000); rput32(0x00000000); rput32(0xc2000000); rput32(0xc888c03e); 
		rput32(0x00000000); rput32(0xc2010100); rput32(0xc8000000); rput32(0x00000000); 
		rput32(0x02000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000);
	rput32(0xc00a2b00);
		rput32(0x00000001); rput32(0x00000009); 
		
		rput32(0x00000000); rput32(0x1001c400); rput32(0x22000000); rput32(0xc80f8000); 
		rput32(0x00000000); rput32(0xc2000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000);

	rput32(0x00012180);
		rput32(0x1000000e); rput32(0x00000000);
	rput32(0x00022100);
		rput32(0x0000ffff); rput32(0x00000000); rput32(0x00000000);
	rput32(0x00022204); 
		rput32(0x00010000); rput32(0x00010000); rput32(0x00000300);
	rput32(0x00002312); 
		rput32(0x0000ffff);
	rput32(0x0000200d); 
		rput32(0x00000000);
	rput32(0x00002200); 
		rput32(0x00000000);
	rput32(0x00002203); 
		rput32(0x00000000);
	rput32(0x00002208); 
		rput32(0x00000004);
	rput32(0x00002104); 
		rput32(0x00000000);
	rput32(0x00002280); 
		rput32(0x00080008);
	rput32(0x00002302); 
		rput32(0x00000004);
	Xe_pSetSurfaceClip(xe, 0, 0, 0, 0, 16, 16);
}

void Xe_pGInit1(struct XenosDevice *xe, int arg)
{
	rput32(0x000005c8); 
		rput32(0x00020000);
	rput32(0x00078d00); 
		rput32(arg | 1); rput32(arg | 1); rput32(arg | 1); rput32(arg | 1); 
		rput32(arg | 1); rput32(arg | 1); rput32(arg | 1); rput32(arg | 1);
	rput32(0x00000d00); 
		rput32(arg);
}

void Xe_pGInit2(struct XenosDevice *xe)
{
	int i;
	for (i=0; i<24; ++i)
	{
		rput32(0xc0003600);
			rput32(0x00010081);
	}
}

void Xe_pGInit3(struct XenosDevice *xe)
{
	rput32(0x000005c8); 
		rput32(0x00020000);
	rput32(0x00000d04); 
		rput32(0x00000000);
}

void Xe_pGInit4(struct XenosDevice *xe) /* "init_0" */
{
	rput32(0x00000d02);  /* ? */
		rput32(0x00010800);
	rput32(0x00030a02); 
		rput32(0xc0100000); rput32(0x07f00000); rput32(0xc0000000); rput32(0x00100000);

	Xe_pGInit3(xe);
}

void Xe_pGInit5(struct XenosDevice *xe) /* "init_1" */
{
	rput32(0x00000d01); 
		rput32(0x04000000);
	rput32(0xc0022100); 
		rput32(0x00000081); rput32(0xffffffff); rput32(0x80010000);
	rput32(0xc0022100); 
		rput32(0x00000082); rput32(0xffffffff); rput32(0x00000000);
	rput32(0x00000e42); 
		rput32(0x00001f60);
	rput32(0x00000c85); 
		rput32(0x00000003);
	rput32(0x0000057c); 
		rput32(0x0badf00d);
	rput32(0x0000057b); 
		rput32(0x00000000);
}

void Xe_pGInit6(struct XenosDevice *xe)
{
	Xe_pSetSurfaceClip(xe, 0, 0, 0, 0, 1024, 720);
	rput32(0x0002857e); 
		rput32(0x00010017); rput32(0x00000000); rput32(0x03ff02cf);
	rput32(0x0002857e); 
		rput32(0x00010017); rput32(0x00000004); rput32(0x03ff02cf);
}

void Xe_pGInit7(struct XenosDevice *xe)
{
	rput32(0x000005c8); 
		rput32(0x00020000);
	rput32(0x00000f01); 
		rput32(0x0000200e);
}

void Xe_pGInit8(struct XenosDevice *xe)
{
	Xe_pSetSurfaceClip(xe, 0, 0, 0, 0, 1024, 720);
}

void Xe_pGInit9(struct XenosDevice *xe)
{
	int i;
	rput32(0x0000057e);
		rput32(0x00010019);
		
	Xe_pGInit0(xe);
	
	for (i = 0x10; i <= 0x70; ++i)
		Xe_pGInit1(xe, 0x00000000 | (i << 12) | ((0x80 - i) << 4));

	Xe_pGInit2(xe);
	rput32(0x0000057e); 
		rput32(0x0001001a);

	Xe_pGInit8(xe);
}

void Xe_pGInit10(struct XenosDevice *xe)
{
	Xe_pSetSurfaceClip(xe, 0, 0, 0, 0, 1024, 720);

	rput32(0x0000057e); 
		rput32(0x00010019);
	rput32(0xc0003b00); 
		rput32(0x00000300);

	Xe_pGInit7(xe);

	Xe_pGInit9(xe);
}

void Xe_pGInit(struct XenosDevice *xe)
{
	rput32(0xc0114800); 
		rput32(0x000003ff); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000080); rput32(0x00000100); rput32(0x00000180); rput32(0x00000200); 
		rput32(0x00000280); rput32(0x00000300); rput32(0x00000380); rput32(0x00010800); 
		rput32(0x00000007); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000);

	Xe_pGInit4(xe);

	Xe_pGInit5(xe);
	Xe_pGInit6(xe);
	Xe_pGInit10(xe);
}

void Xe_DirtyAluConstant(struct XenosDevice *xe, int base, int len)
{
	len += base & 15;
	base >>= 4;
	while (len > 0)
	{
		xe->alu_dirty |= 1 << base;
		++base;
		len -= 16;
	}
	xe->dirty |= DIRTY_ALU;
}

void Xe_DirtyFetch(struct XenosDevice *xe, int base, int len)
{
	len += base % 3;
	base /= 3;
	while (len > 0)
	{
		xe->fetch_dirty |= 1 << base;
		++base;
		len -= 3;
	}
	xe->dirty |= DIRTY_FETCH;
}

struct XenosShader *Xe_LoadShader(struct XenosDevice *xe, const char *filename)
{
	FILE *f = fopen(filename, "rb");
	if (!f)
		Xe_Fatal(xe, "FATAL: shader %s not found!\n", filename);

	fseek(f, 0, SEEK_END);
	int size = ftell(f);
	fseek(f, 0, SEEK_SET);
	
	void *m = malloc(size);
	fread(m, size, 1, f);
	fclose(f);
	
	return Xe_LoadShaderFromMemory(xe, m);
}

struct XenosShader *Xe_LoadShaderFromMemory(struct XenosDevice *xe, void *m)
{
	struct XenosShaderHeader *hdr = m;
	
	if ((hdr->magic >> 16) != 0x102a)
		Xe_Fatal(xe, "shader version: %08x, expected: something with 102a.\n", hdr->magic);

	struct XenosShader *s = malloc(sizeof(struct XenosShader));
	memset(s, 0, sizeof(*s));
	s->shader = m;

	struct XenosShaderData *data = m + hdr->off_shader;
	s->program_control = data->program_control;
	s->context_misc = data->context_misc;

	return s;
}

void Xe_pUploadShaderConstants(struct XenosDevice *xe, struct XenosShader *s)
{
	struct XenosShaderHeader *hdr = s->shader;
	
	if (hdr->off_constants)
	{
			/* upload shader constants */
//		printf("off_constants: %d\n", hdr->off_constants);
		void *constants = s->shader + hdr->off_constants;
		
		constants += 16;
	
		int size = *(u32*)constants; constants += 4;
		size -= 0xC;
		
//		printf("uploading shader constants..\n");
		
		while (size)
		{
			u16 start = *(u16*)constants; constants += 2;
			u16 count = *(u16*)constants; constants += 2;
			u32 offset = *(u32*)constants; constants += 4;
			
			float *c = s->shader + hdr->offset + offset;
//			printf("start: %d, count: %d, off: %d\n", start, count, hdr->offset + offset);
//			int i;
//			for (i=0; i<count / 4; ++i)
//				printf("%d: %3.3f %3.3f %3.3f %3.3f\n", start + i, c[i*4+0], c[i*4+1], c[i*4+2], c[i*4+3]);
			memcpy(xe->alu_constants + start * 4, c, count * 4);
			Xe_DirtyAluConstant(xe, start, 4);

			size -= 8;
		}
	}
}

int Xe_VBFCalcSize(struct XenosDevice *xe, const struct XenosVBFElement *fmt)
{
	switch (fmt->fmt)
	{
	case 6: // char4
		return 4;
	case 37: // float2
		return 8;
	case 38: // float4
		return 16;
	case 57: // float3
		return 12;
	default:
		Xe_Fatal(xe, "Unknown VBF %d!\n", fmt->fmt);
	}
}

int Xe_pVBFNrComponents(struct XenosDevice *xe, const struct XenosVBFElement *fmt)
{
	switch (fmt->fmt)
	{
	case 6: // char4
		return 4;
	case 37: // float2
		return 2;
	case 38: // float4
		return 4;
	case 57: // float3
		return 3;
	default:
		Xe_Fatal(xe, "Unknown VBF %d!\n", fmt->fmt);
	}
}

int Xe_VBFCalcStride(struct XenosDevice *xe, const struct XenosVBFFormat *fmt)
{
	int i;
	int total_size = 0;
	for (i=0; i<fmt->num; ++i)
		total_size += Xe_VBFCalcSize(xe, &fmt->e[i]);
	return total_size;
}

void Xe_pInvalidateGpuCache(struct XenosDevice *xe, int base, int size)
{
	rput32(0x00000a31);
		rput32(0x01000000);
	rput32(0x00010a2f);
		rput32(size); rput32(base);
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008);
}

void Xe_pInvalidateGpuCacheAll(struct XenosDevice *xe, int base, int size)
{
	rput32(0x00000a31);
		rput32(0x03000100);
	rput32(0x00010a2f);
		rput32(size); rput32(base);
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008);
}

void Xe_pUnlock(struct XenosDevice *xe, struct XenosLock *lock)
{
	if (!lock->start)
		Xe_Fatal(xe, "unlock without lock");
	if (lock->flags & XE_LOCK_WRITE)
	{
		Xe_pSyncToDevice(xe, lock->start, lock->size);
		Xe_pInvalidateGpuCache(xe, lock->phys, lock->size);
	}
	lock->start = 0;
}

void Xe_pLock(struct XenosDevice *xe, struct XenosLock *lock, void *addr, u32 phys, int size, int flags)
{
	if (!flags)
		Xe_Fatal(xe, "flags=0");
	if (lock->start)
		Xe_Fatal(xe, "locked twice");
	if (lock->flags & XE_LOCK_READ)
	{
			/* *you* must make sure that the GPU already flushed this content. (usually, it is, though) */
		Xe_pSyncFromDevice(xe, addr, size);
	}
	lock->start = addr;
	lock->phys = phys;
	lock->size = size;
	lock->flags = flags;
}


	/* shaders are not specific to a vertex input format.
	   the vertex format specified in a vertex shader is just
	   dummy. Thus we need to patch the vfetch instructions to match
	   our defined structure. */
void Xe_ShaderApplyVFetchPatches(struct XenosDevice *xe, struct XenosShader *sh, unsigned int index, const struct XenosVBFFormat *fmt)
{
	assert(index < XE_SHADER_MAX_INSTANCES);
	assert(sh->shader_phys[index]);

	struct XenosLock lock;
	memset(&lock, 0, sizeof(lock));
	Xe_pLock(xe, &lock, sh->shader_instance[index], sh->shader_phys[index], sh->shader_phys_size, XE_LOCK_READ|XE_LOCK_WRITE);

	int stride = Xe_VBFCalcStride(xe, fmt);

	if (stride & 3)
		Xe_Fatal(xe, "your vertex buffer format is not DWORD aligned.\n");

	stride /= 4;

	struct XenosShaderHeader *hdr = sh->shader;
	struct XenosShaderData *data = sh->shader + hdr->off_shader;
	
	void *shader_code = sh->shader_instance[index];
	u32 *c = (u32*)(data + 1);
	int skip = *c++;
	int num_vfetch = *c;
	++c;

	c += skip * 2;
	int i;

	int fetched_to = 0;

	for (i=0; i<num_vfetch; ++i)
	{
		u32 vfetch_patch = *c++;
		int type = (vfetch_patch >> 12) & 0xF;
		int stream = (vfetch_patch >> 16) & 0xF;
		int insn = vfetch_patch & 0xFFF;
		
//		printf("raw: %08x\n", vfetch_patch);
//		printf("type=%d, stream=%d, insn=%d\n", type, stream, insn);
		u32 *vfetch = shader_code + insn * 12;
//		printf("  old vfetch: %08x %08x %08x\n", vfetch[0], vfetch[1], vfetch[2]);
//		printf("    old swizzle: %08x\n", vfetch[1] & 0xFFF);
		
		int Offset = (vfetch[2] & 0x7fffff00) >> 8;
		int DataFormat = (vfetch[1] & 0x003f0000) >> 16;
		int Stride= (vfetch[2] & 0x000000ff);
		int Signed= (vfetch[1] & 0x00001000) >> 12;
		int NumFormat = (vfetch[1] & 0x00002000) >> 13;
		int PrefetchCount= (vfetch[0] & 0x38000000) >> 27;
//		printf("  old Offset=%08x, DataFormat=%d, Stride=%d, Signed=%d, NumFormat=%d, PrefetchCount=%d\n",
//			Offset,DataFormat, Stride, Signed, NumFormat, PrefetchCount);
		
			/* let's find the element which applies for this. */
		int j;
		int offset = 0;
		for (j=0; j < fmt->num; ++j)
		{
			if ((fmt->e[j].usage == type) && (fmt->e[j].index == stream))
				break;
			offset += Xe_VBFCalcSize(xe, &fmt->e[j]);
		}
		
		offset /= 4;
		
		if (j == fmt->num)
			Xe_Fatal(xe, "shader requires input type %d_%d, which wasn't found in vertex format.\n", type, stream);

		Offset = offset;
		DataFormat = fmt->e[j].fmt;
		
		Signed = 0;
		Stride = stride;
		NumFormat = 0; // fraction

		if (DataFormat != 6)
			NumFormat = 1;
		
		int to_fetch = 0;

			/* if we need fetching... */
		if (fetched_to <= offset + ((Xe_VBFCalcSize(xe, &fmt->e[j])+3)/4))
			to_fetch = stride - fetched_to;

		if (to_fetch > 8)
			to_fetch = 8;
		to_fetch = 1; /* FIXME: prefetching doesn't always work. */

		int is_mini = 0;
		
		if (to_fetch == 0)
		{
			PrefetchCount = 0;
			is_mini = 1;
		} else
			PrefetchCount = to_fetch - 1;
		
		fetched_to += to_fetch;

			/* patch vfetch instruction */
		vfetch[0] &= ~(0x00000000|0x00000000|0x00000000|0x00000000|0x00000000|0x38000000|0x00000000);
		vfetch[1] &= ~(0x00000000|0x003f0000|0x00000000|0x00001000|0x00002000|0x00000000|0x40000000);
		vfetch[2] &= ~(0x7fffff00|0x00000000|0x000000ff|0x00000000|0x00000000|0x00000000|0x00000000);

		vfetch[2] |= Offset << 8;
		vfetch[1] |= DataFormat << 16;
		vfetch[2] |= Stride;
		vfetch[1] |= Signed << 12;
		vfetch[1] |= NumFormat << 13;
		vfetch[0] |= PrefetchCount << 27;
		vfetch[1] |= is_mini << 30;
		
//		printf("specified swizzle: %08x\n", fmt->e[j].swizzle);
		
		int comp;
		int nrcomp = Xe_pVBFNrComponents(xe, &fmt->e[j]);
		for (comp = 0; comp < 4; comp++)
		{
			int shift = comp * 3;
			int sw = (vfetch[1] >> shift) & 7; /* see original swizzle, xyzw01_? */
//			printf("comp%d sw=%c ", comp, "xyzw01?_"[sw]);
			if ((sw < 4) && (sw >= nrcomp)) /* refer to an unavailable position? */
			{
				if (sw == 3) // a/w
					sw = 5; // 1
				else
					sw = 4; // 0
			}
//			printf(" -> %c\n", "xyzw01?_"[sw]);
			vfetch[1] &= ~(7<<shift);
			vfetch[1] |= sw << shift;
		}
		

		Offset = (vfetch[2] & 0x7fffff00) >> 8;
		DataFormat = (vfetch[1] & 0x003f0000) >> 16;
		Stride= (vfetch[2] & 0x000000ff);
		Signed= (vfetch[1] & 0x00001000) >> 12;
		NumFormat = (vfetch[1] & 0x00002000) >> 13;
		PrefetchCount= (vfetch[0] & 0x38000000) >> 27;
//		printf("  new Offset=%08x, DataFormat=%d, Stride=%d, Signed=%d, NumFormat=%d, PrefetchCount=%d\n",
//			Offset,DataFormat, Stride, Signed, NumFormat, PrefetchCount);
//		printf("  new vfetch: %08x %08x %08x\n", vfetch[0], vfetch[1], vfetch[2]);
	}

	Xe_pUnlock(xe, &lock);
}

void Xe_InstantiateShader(struct XenosDevice *xe, struct XenosShader *sh, unsigned int index)
{
	assert(index < XE_SHADER_MAX_INSTANCES);
	struct XenosShaderHeader *hdr = sh->shader;
	struct XenosShaderData *data = sh->shader + hdr->off_shader;
	void *shader_code = sh->shader + data->sh_off + hdr->offset;
	
	sh->shader_phys_size = data->sh_size;
	printf("allocating %d bytes\n", data->sh_size);
	void *p = Xe_pAlloc(xe, &sh->shader_phys[index], data->sh_size, 0x100);
	memcpy(p, shader_code, data->sh_size);
	Xe_pSyncToDevice(xe, p, data->sh_size);
	sh->shader_instance[index] = p;
}

int Xe_GetShaderLength(struct XenosDevice *xe, void *sh)
{
	struct XenosShaderHeader *hdr = sh;
	struct XenosShaderData *data = sh + hdr->off_shader;
	return data->sh_off + hdr->offset + data->sh_size;
}

void Xe_Init(struct XenosDevice *xe)
{
	memset(xe, 0, sizeof(*xe));
	xe->regs = (void*)0xec800000;
	xe->rb = xe->rb_primary = (void*)(RINGBUFFER_BASE | 0x80000000);
	
// optimize framebuffer to the end, so we have a bit more space: 
	w32(0x6110, 0x1fc00000);
	
	xe->tex_fb.ptr = r32(0x6110);
	xe->tex_fb.pitch = r32(0x6120) * 4;
	xe->tex_fb.width = r32(0x6134);
	xe->tex_fb.height = r32(0x6138);
	xe->tex_fb.bypp = 4;
	xe->tex_fb.base = (void*)(long)xe->tex_fb.ptr;
	xe->tex_fb.format = XE_FMT_BGRA | XE_FMT_8888;
	xe->tex_fb.tiled = 1;
	
	printf("Framebuffer %d x %d @ %08x\n", xe->tex_fb.width, xe->tex_fb.height, xe->tex_fb.ptr);

#if 0
	time_t t = time(0);
	while (t == time(0));
	t = time(0) + 10;
	int nr = 0;
	while (t > time(0))
	{
		memcpy(rb, rb + RINGBUFFER_SIZE / 2, RINGBUFFER_SIZE / 2);
		++nr;
	}
	
	printf("%d kB/s (%d)\n", nr * (RINGBUFFER_SIZE/1024)/2 / 10, nr);
	return 0;
#endif

	u32 rb_primary_phys = Xe_pRBAlloc(xe);

//	memset((void*)xe->rb, 0xCC, RINGBUFFER_SIZE);

	Xe_pMasterInit(xe, rb_primary_phys);
	Xe_pEnableWriteback(xe, RINGBUFFER_BASE + RPTR_WRITEBACK, 6);
	
	Xe_pSyncFromDevice(xe, xe->rb + RPTR_WRITEBACK, 4);
	
	Xe_pWriteReg(xe, 0x0774, RINGBUFFER_BASE + SCRATCH_WRITEBACK);
	Xe_pWriteReg(xe, 0x0770, 0x20033);

	Xe_pWriteReg(xe, 0x15e0, 0x1234567);
	
	Xe_pGInit(xe);
	
	Xe_pInvalidateGpuCache(xe, RINGBUFFER_BASE, RINGBUFFER_SIZE);
}

void Xe_SetRenderTarget(struct XenosDevice *xe, struct XenosSurface *rt)
{
	xe->rt = rt;
	xe->vp_xres = rt->width;
	xe->vp_yres = rt->height;

	xe->msaa_samples = 0;
	xe->edram_colorformat = 0;

	int tile_size_x = (xe->msaa_samples < 2) ? 80 : 40, tile_size_y = (xe->msaa_samples > 0) ? 8 : 16;
	if ((xe->edram_colorformat == 15) || (xe->edram_colorformat == 7) || (xe->edram_colorformat == 5))
		tile_size_x /= 2;
	int tiles_per_line = (xe->vp_xres + tile_size_x - 1) / tile_size_x;
	tiles_per_line += 1;
	tiles_per_line &= ~1;
	
	int tiles_height = (xe->vp_yres + tile_size_y - 1) / tile_size_y;

	// what about 64bit targets?

	xe->edram_pitch = tiles_per_line * tile_size_x;
	xe->edram_hizpitch = tiles_per_line * tile_size_x;
	xe->edram_color0base = 0;
	xe->edram_depthbase = tiles_per_line * tiles_height;
}

void Xe_pSetEDRAMLayout(struct XenosDevice *xe)
{
	rput32(0x00022000);
		rput32(SurfaceInfo(xe->edram_pitch, xe->msaa_samples, xe->edram_hizpitch));  // SurfaceInfo
		rput32((xe->edram_colorformat << 16) | xe->edram_color0base);
		rput32(xe->edram_depthbase | (0<<16) ); // depth info, float Z
}

void Xe_ResolveInto(struct XenosDevice *xe, struct XenosSurface *surface, int source, int clear)
{
	Xe_pSetSurfaceClip(xe, 0, 0, 0, 0, surface->width, surface->height);
	
	Xe_VBBegin(xe, 2);
	float vbdata[] = 
		{-.5, -.5, /* never ever dare to mess with these values. NO, you can not resolve arbitrary areas or even shapes. */
		 surface->width - .5,
		 0,
		 surface->width - .5,
		 surface->height - .5
		};
	Xe_VBPut(xe, vbdata, sizeof(vbdata) / 4);
	struct XenosVertexBuffer *vb = Xe_VBEnd(xe);
	Xe_VBPoolAdd(xe, vb);

	Xe_pSetEDRAMLayout(xe);
	rput32(0x00002104); 
		rput32(0x0000000f); // colormask 
	rput32(0x0005210f); 
		rput32(0x44000000); rput32(0x44000000); 
		rput32(0xc3b40000); rput32(0x43b40000); 
		rput32(0x3f800000); rput32(0x00000000); 

	int msaavals[] = {0,4,6};
	int pitch;
	switch (surface->format & XE_FMT_MASK)
	{
	case XE_FMT_8888: pitch = surface->pitch / 4; break;
	case XE_FMT_16161616: pitch = surface->pitch / 8; break;
	default: Xe_Fatal(xe, "unsupported resolve target format");
	}
	rput32(0x00032318); 
		rput32(0x00100000 | (msaavals[xe->msaa_samples]<<4) | (clear << 8) | source ); // 300 = color,depth clear enabled!
		rput32(surface->ptr);
		rput32(xy32(pitch, surface->height));
		rput32(0x01000000 | ((surface->format&XE_FMT_MASK)<<7) | ((surface->format&~XE_FMT_MASK)>>6));

	Xe_pWriteReg(xe, 0x8c74, 0xffffff00); // zbuffer / stencil clear: z to -1, stencil to 0
	
	unsigned int clearv[2];
	
	switch (xe->edram_colorformat)
	{
	case 0:
	case 1:
		clearv[0] = clearv[1] = xe->clearcolor;
		break;
	case 4:
	case 5:
		clearv[0]  = (xe->clearcolor & 0xFF000000);
		clearv[0] |= (xe->clearcolor & 0x00FF0000)>>8;
		clearv[0] >>= 1;
		clearv[0] |= (clearv[0] >> 8) & 0x00FF00FF;
		clearv[1]  = (xe->clearcolor & 0x0000FF00)<<16;
		clearv[1] |= (xe->clearcolor & 0x000000FF)<<8;
		clearv[1] >>= 1;
		clearv[1] |= (clearv[1] >> 8) & 0x00FF00FF;
		break;
	default:
		clearv[0] = clearv[1] = 0;
	}
	
	Xe_pWriteReg(xe, 0x8c78, clearv[0]);
	Xe_pWriteReg(xe, 0x8c7c, clearv[1]);

	rput32(0xc0003b00); rput32(0x00000100);

	rput32(0xc0102b00); rput32(0x00000000);
		rput32(0x0000000f); 

		rput32(0x10011002); rput32(0x00001200); rput32(0xc4000000);
		rput32(0x00000000); rput32(0x1003c200); rput32(0x22000000); 
		rput32(0x00080000);	rput32(0x00253b48); rput32(0x00000002); 
		rput32(0xc80f803e); rput32(0x00000000);	rput32(0xc2000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000);

	rput32(0x00012180); rput32(0x00010002); rput32(0x00000000); 
	if (surface->ptr)
	{
		rput32(0x00002208); rput32(0x00000006);
	} else
	{
		rput32(0x00002208); rput32(0x00000005);
	}

	rput32(0x00002200); rput32(0x8777);

	rput32(0x000005c8); rput32(0x00020000);
	rput32(0x00002203); rput32(0x00000000);
	rput32(0x00022100); rput32(0x0000ffff); rput32(0x00000000); rput32(0x00000000); 
	rput32(0x00022204); rput32(0x00010000); rput32(0x00010000); rput32(0x00000300); 
	rput32(0x00002312); rput32(0x0000ffff); 
	rput32(0x0000200d); rput32(0x00000000);

	rput32(0x00054800); rput32((vb->phys_base) | 3); rput32(0x1000001a); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000);

	rput32(0x00025000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000);
 
	rput32(0xc0003600); rput32(0x00030088); 

	rput32(0xc0004600); rput32(0x00000006); 
	rput32(0x00002007); rput32(0x00000000); 
	Xe_pInvalidateGpuCacheAll(xe, surface->ptr, surface->pitch * surface->height);

	rput32(0x0000057e); rput32(0x00010001); 
	rput32(0x00002318); rput32(0x00000000);
	rput32(0x0000231b); rput32(0x00000000);

#if 0
	rput32(0x00001844); rput32(surface->ptr); 
	rput32(0xc0022100); rput32(0x00001841); rput32(0xfffff8ff); rput32(0x00000000);
	rput32(0x00001930); rput32(0x00000000);
	rput32(0xc0003b00); rput32(0x00007fff);
#endif

#if 0
	rput32(0xc0025800); rput32(0x00000003); // event zeugs
		rput32(0x1fc4e006); rput32(0xbfb75313);
	rput32(0xc0025800); rput32(0x00000003);
		rput32(0x1fc4e002); rput32(0x000286d1);
#endif

	xe->dirty |= DIRTY_MISC;
}

void Xe_Clear(struct XenosDevice *xe, int flags)
{
	struct XenosSurface surface = *xe->rt;
	surface.ptr = 0;
	
	Xe_ResolveInto(xe, &surface, 0, flags);
}

void Xe_Resolve(struct XenosDevice *xe)
{
	struct XenosSurface *surface = xe->rt;
	
	Xe_ResolveInto(xe, surface, XE_SOURCE_COLOR, XE_CLEAR_COLOR|XE_CLEAR_DS);
}


void VERTEX_FETCH(u32 *dst, u32 base, int len)
{
	dst[0] = base | 3;
	dst[1] = 0x10000002 | (len << 2);
}

void TEXTURE_FETCH(u32 *dst, u32 base, int width, int height, int pitch, int tiled, int format, u32 base_mip, int anisop)
{
	switch (format & XE_FMT_MASK)
	{
	case XE_FMT_565: pitch /= 64; break;
	case XE_FMT_16161616: pitch /= 256; break;
	case XE_FMT_8888: pitch /= 128; break;
	default: abort();
	}

	dst[0] = 0x00000002 | (pitch << 22) | (tiled << 31);
	dst[1] = 0x00000000 | base | format; /* BaseAddress */
	dst[2] = (height << 13) | width;
	dst[3] = 0x00a80c14 | (anisop << 25);
	if (base_mip)
		dst[4] = 0x00000e03;
	else
		dst[4] = 0;
	dst[5] = 0x00000a00 | base_mip; /* MipAddress */
}

void Xe_pLoadShader(struct XenosDevice *xe, int base, int type, int size)
{
	rput32(0xc0012700);
		rput32(base | type); 
		rput32(size);
}

void Xe_pAlign(struct XenosDevice *xe)
{
	while ((xe->rb_secondary_wptr&3) != 3)
		rput32(0x80000000);
}

void Xe_pBlockUntilIdle(struct XenosDevice *xe)
{
	Xe_pWriteReg(xe, 0x1720, 0x20000);
}

void Xe_pStep(struct XenosDevice *xe, int x)
{
	Xe_pWriteReg(xe, 0x15e0, x);
}

void Xe_pStuff(struct XenosDevice *xe)
{
	rput32(0x00072380);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000);
}


void Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...)
{
	va_list arg;
	va_start(arg, fmt);
	vprintf(fmt, arg);
	va_end(arg);
	abort();
}

struct XenosSurface *Xe_GetFramebufferSurface(struct XenosDevice *xe)
{
	return &xe->tex_fb;
}

void Xe_Execute(struct XenosDevice *xe)
{
	Xe_pBlockUntilIdle(xe);
	Xe_pRBKick(xe);
}

void Xe_pDebugSync(struct XenosDevice *xe)
{
	Xe_pWriteReg(xe, 0x15e0, xe->frameidx);

	Xe_Execute(xe);
	
//	printf("waiting for frameidx %08x\n", xe->frameidx);
	int timeout = 1<<24;
	do {
		Xe_pSyncFromDevice(xe, xe->rb + SCRATCH_WRITEBACK, 4);
		if (!timeout--)
			Xe_Fatal(xe, "damn, the GPU seems to hang. There is no (known) way to recover, you have to reboot.\n");
//		udelay(1000);
	} while (*(volatile u32*)(xe->rb + SCRATCH_WRITEBACK) != xe->frameidx) ;
	xe->frameidx++;
//	printf("done\n");
}

void Xe_Sync(struct XenosDevice *xe)
{
	Xe_pDebugSync(xe);
	Xe_VBReclaim(xe);
}

int stat_alu_uploaded = 0;

void Xe_pUploadALUConstants(struct XenosDevice *xe)
{
	while (xe->alu_dirty)
	{
		int start, end;
		for (start = 0; start < 32; ++start)
			if (xe->alu_dirty & (1<<start))
				break;
		for (end = start; end < 32; ++end)
			if (!(xe->alu_dirty & (1<<end)))
				break;
			else
				xe->alu_dirty &= ~(1<<end);
		
		int base = start * 16;
		int num = (end - start) * 16 * 4;
		
		stat_alu_uploaded += num;
		Xe_pAlign(xe);
		rput32(0x00004000 | (base * 4) | ((num-1) << 16));
			rput(xe->alu_constants + base * 4, num);
	}
}

void Xe_pUploadFetchConstants(struct XenosDevice *xe)
{
	while (xe->fetch_dirty)
	{
		int start, end;
		for (start = 0; start < 32; ++start)
			if (xe->fetch_dirty & (1<<start))
				break;
		for (end = start; end < 32; ++end)
			if (!(xe->fetch_dirty & (1<<end)))
				break;
			else
				xe->fetch_dirty &= ~(1<<end);
		
		int base = start * 3;
		int num = (end - start) * 3 * 2;
		
		stat_alu_uploaded += num;
		Xe_pAlign(xe);
		rput32(0x00004800 | (base * 2) | ((num-1) << 16));
			rput(xe->fetch_constants + base * 2, num);
	}
}

void Xe_pUploadClipPlane(struct XenosDevice *xe)
{
	Xe_pAlign(xe);
	rput32(0x00172388);
		rput(xe->clipplane, 6*4);
}

void Xe_pUploadIntegerConstants(struct XenosDevice *xe)
{
	Xe_pAlign(xe);
	rput32(0x00274900);
		rput(xe->integer_constants, 10*4);
}

void Xe_pUploadControl(struct XenosDevice *xe)
{
	rput32(0x00082200);
		rput(xe->controlpacket, 9);
}

void Xe_pUploadShader(struct XenosDevice *xe)
{
	u32 program_control = 0, context_misc = 0;
	if (xe->ps)
	{
		Xe_pLoadShader(xe, xe->ps->shader_phys[0], SHADER_TYPE_PIXEL, xe->ps->shader_phys_size);
		Xe_pUploadShaderConstants(xe, xe->ps);
		program_control |= xe->ps->program_control;
		context_misc |= xe->ps->context_misc;
	}

	if (xe->vs)
	{
		Xe_pLoadShader(xe, xe->vs->shader_phys[xe->vs_index], SHADER_TYPE_VERTEX, xe->vs->shader_phys_size);
		Xe_pUploadShaderConstants(xe, xe->vs);
		program_control |= xe->vs->program_control;
		context_misc |= xe->vs->context_misc;
	}
	
	rput32(0x00022180);
		rput32(program_control);
		rput32(context_misc);
		rput32(0xFFFFFFFF);  /* interpolation mode */
}

void Xe_pInitControl(struct XenosDevice *xe)
{
	xe->controlpacket[0] = 0x00700736|0x80;  // DEPTH
	xe->controlpacket[1] = 0x00010001;  // BLEND
	xe->controlpacket[2] = 0x87000007;  // COLOR
	xe->controlpacket[3] = 0x00000000;  // HI
	xe->controlpacket[4] = 0x00080000;  // CLIP
	xe->controlpacket[5] = 0x00010006;  // MODE
	if (xe->msaa_samples)
		xe->controlpacket[5] |= 1<<15;
	xe->controlpacket[6] = 0x0000043f;  // VTE
	xe->controlpacket[7] = 0;
	xe->controlpacket[8] = 0x00000004; // EDRAM
	
	xe->stencildata[0] = 0xFFFF00;
	xe->stencildata[1] = 0xFFFF00;
	
	xe->dirty |= DIRTY_CONTROL|DIRTY_MISC;
}

void Xe_SetZFunc(struct XenosDevice *xe, int z_func)
{
	xe->controlpacket[0] = (xe->controlpacket[0]&~0x70) | (z_func<<4);
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetZWrite(struct XenosDevice *xe, int zw)
{
	xe->controlpacket[0] = (xe->controlpacket[0]&~4) | (zw<<2);
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetZEnable(struct XenosDevice *xe, int ze)
{
	xe->controlpacket[0] = (xe->controlpacket[0]&~2) | (ze<<1);
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetFillMode(struct XenosDevice *xe, int front, int back)
{
	xe->controlpacket[5] &= ~(0x3f<<5);
	xe->controlpacket[5] |= front << 5;
	xe->controlpacket[5] |= back << 8;
	xe->controlpacket[5] |= 1<<3;

	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetBlendControl(struct XenosDevice *xe, int col_src, int col_op, int col_dst, int alpha_src, int alpha_op, int alpha_dst)
{
	xe->controlpacket[1] = col_src | (col_op << 5) | (col_dst << 8) | (alpha_src << 16) | (alpha_op << 21) | (alpha_dst << 24);
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetSrcBlend(struct XenosDevice *xe, unsigned int blend)
{
	assert(blend < 32);
	xe->controlpacket[1] &= ~0x1F;
	xe->controlpacket[1] |= blend;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetDestBlend(struct XenosDevice *xe, unsigned int blend)
{
	assert(blend < 32);
	xe->controlpacket[1] &= ~(0x1F<<8);
	xe->controlpacket[1] |= blend<<8;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetBlendOp(struct XenosDevice *xe, unsigned int blendop)
{
	assert(blendop < 8);
	xe->controlpacket[1] &= ~(0x7<<5);
	xe->controlpacket[1] |= blendop<<5;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetSrcBlendAlpha(struct XenosDevice *xe, unsigned int blend)
{
	assert(blend < 32);
	xe->controlpacket[1] &= ~(0x1F<<16);
	xe->controlpacket[1] |= blend << 16;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetDestBlendAlpha(struct XenosDevice *xe, unsigned int blend)
{
	assert(blend < 32);
	xe->controlpacket[1] &= ~(0x1F<<24);
	xe->controlpacket[1] |= blend<< 24;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetBlendOpAlpha(struct XenosDevice *xe, unsigned int blendop)
{
	assert(blendop < 8);
	xe->controlpacket[1] &= ~(0x7<<21);
	xe->controlpacket[1] |= blendop<<21;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetCullMode(struct XenosDevice *xe, unsigned int cullmode)
{
	assert(cullmode < 8);
	xe->controlpacket[5] &= ~7;
	xe->controlpacket[5] |= cullmode;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetAlphaTestEnable(struct XenosDevice *xe, int enable)
{
	xe->controlpacket[2] &= ~8;
	xe->controlpacket[2] |= (!!enable) << 3;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetAlphaFunc(struct XenosDevice *xe, unsigned int func)
{
	assert(func <= 7);
	xe->controlpacket[2] &= ~7;
	xe->controlpacket[2] |= func;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetAlphaRef(struct XenosDevice *xe, float alpharef)
{
	xe->alpharef = alpharef;
	xe->dirty |= DIRTY_MISC;
}

void Xe_SetStencilFunc(struct XenosDevice *xe, int bfff, unsigned int func)
{
	assert(func <= 7);
	if (bfff & 1)
	{
		xe->controlpacket[0] &= ~(7<<8);
		xe->controlpacket[0] |= func << 8;
	}
	if (bfff & 2)
	{
		xe->controlpacket[0] &= ~(7<<20);
		xe->controlpacket[0] |= func << 20;
	}
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetStencilEnable(struct XenosDevice *xe, unsigned int enable)
{
	assert(enable <= 1);
	xe->controlpacket[0] &= ~1;
	xe->controlpacket[0] |= enable;
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetStencilOp(struct XenosDevice *xe, int bfff, int fail, int zfail, int pass)
{
	assert(fail <= 7);
	assert(zfail <= 7);
	assert(pass <= 7);
	
	if (bfff & 1)
	{
		if (fail >= 0)
		{
			xe->controlpacket[0] &= ~(7<<11);
			xe->controlpacket[0] |= fail << 11;
		}
		if (pass >= 0)
		{
			xe->controlpacket[0] &= ~(7<<14);
			xe->controlpacket[0] |= pass << 14;
		}
		if (zfail >= 0)
		{
			xe->controlpacket[0] &= ~(7<<17);
			xe->controlpacket[0] |= zfail << 17;
		}
	}
	if (bfff & 2)
	{
		if (fail >= 0)
		{
			xe->controlpacket[0] &= ~(7<<23);
			xe->controlpacket[0] |= fail << 23;
		}
		if (pass >= 0)
		{
			xe->controlpacket[0] &= ~(7<<26);
			xe->controlpacket[0] |= pass << 26;
		}
		if (zfail >= 0)
		{
			xe->controlpacket[0] &= ~(7<<29);
			xe->controlpacket[0] |= zfail << 29;
		}
	}
	xe->dirty |= DIRTY_CONTROL;
}

void Xe_SetStencilRef(struct XenosDevice *xe, int bfff, int ref)
{
	if (bfff & 1)
		xe->stencildata[1] = (xe->stencildata[1] & ~0xFF) | ref;

	if (bfff & 2)
		xe->stencildata[0] = (xe->stencildata[0] & ~0xFF) | ref;
	xe->dirty |= DIRTY_MISC;
}

void Xe_SetStencilMask(struct XenosDevice *xe, int bfff, int mask)
{
	if (bfff & 1)
		xe->stencildata[1] = (xe->stencildata[1] & ~0xFF00) | (mask<<8);

	if (bfff & 2)
		xe->stencildata[0] = (xe->stencildata[0] & ~0xFF00) | (mask<<8);
	xe->dirty |= DIRTY_MISC;
}

void Xe_SetStencilWriteMask(struct XenosDevice *xe, int bfff, int writemask)
{
	if (bfff & 1)
		xe->stencildata[1] = (xe->stencildata[1] & ~0xFF0000) | (writemask<<16);

	if (bfff & 2)
		xe->stencildata[0] = (xe->stencildata[0] & ~0xFF0000) | (writemask<<16);
	xe->dirty |= DIRTY_MISC;
}

void Xe_InvalidateState(struct XenosDevice *xe)
{
	xe->dirty = ~0;
	xe->alu_dirty = ~0;
	xe->fetch_dirty = ~0;
	Xe_pInitControl(xe);
}

void Xe_SetShader(struct XenosDevice *xe, int type, struct XenosShader *sh, int index)
{
	struct XenosShader **s;
	int *i = 0;
	if (type == SHADER_TYPE_PIXEL)
	{
		s = &xe->ps;
	} else
	{
		s = &xe->vs;
		i = &xe->vs_index;
		assert(sh->shader_instance[index]);
	}

	if ((*s != sh) || (i && *i != index))
	{
		*s = sh;
		if (i)
			*i = index;
		xe->dirty |= DIRTY_SHADER;
	}
}

void Xe_pSetState(struct XenosDevice *xe)
{
	if (xe->dirty & DIRTY_CONTROL)
		Xe_pUploadControl(xe);

	if (xe->dirty & DIRTY_SHADER)
		Xe_pUploadShader(xe);

	if (xe->dirty & DIRTY_ALU)
		Xe_pUploadALUConstants(xe);
	
	if (xe->dirty & DIRTY_FETCH)
	{
		Xe_pUploadFetchConstants(xe);
		rput32(0x00025000);
			rput32(0x00000000); rput32(0x00025000); rput32(0x00000000); 
	}
	
	if (xe->dirty & DIRTY_CLIP)
		Xe_pUploadClipPlane(xe);
	
	if (xe->dirty & DRITY_INTEGER)
		Xe_pUploadIntegerConstants(xe);

//	if (xe->dirty & DIRTY_MISC)
	{
		Xe_pSetSurfaceClip(xe, 0, 0, 0, 0, xe->vp_xres, xe->vp_yres);
		Xe_pSetEDRAMLayout(xe);
		rput32(0x0000200d); 
			rput32(0x00000000);
		rput32(0x00012100); 
			rput32(0x00ffffff);
			rput32(0x00000000);
		rput32(0x00002104);
			rput32(0x0000000f);
		rput32(0x0008210c);
			rput32(xe->stencildata[0]);
			rput32(xe->stencildata[1]);
			rputf(xe->alpharef); /* this does not work. */
			rputf(xe->vp_xres / 2.0);
			rputf(xe->vp_xres / 2.0);
			rputf(-xe->vp_yres / 2.0);
			rputf(xe->vp_yres / 2.0);
			rputf(1.0);
			rputf(0.0);

		int vals[] = {0, 2 | (4 << 13), 4 | (6 << 13)};
		rput32(0x00002301);
			rput32(vals[xe->msaa_samples]);
		rput32(0x00002312);
			rput32(0x0000ffff);
	}
	
	xe->dirty = 0;
}

void Xe_SetTexture(struct XenosDevice *xe, int index, struct XenosSurface *tex)
{
	TEXTURE_FETCH(xe->fetch_constants + index * 6, tex->ptr, tex->width - 1, tex->height - 1, tex->pitch, tex->tiled, tex->format, tex->ptr_mip, 2);
	Xe_DirtyFetch(xe, index + index * 3, 3);
}

void Xe_SetClearColor(struct XenosDevice *xe, u32 clearcolor)
{
	xe->clearcolor = clearcolor;
}

struct XenosVertexBuffer *Xe_CreateVertexBuffer(struct XenosDevice *xe, int size)
{
	struct XenosVertexBuffer *vb = malloc(sizeof(struct XenosVertexBuffer));
	memset(vb, 0, sizeof(struct XenosVertexBuffer));
	printf("--- alloc new vb, at %p\n", vb);
	vb->base = Xe_pAlloc(xe, &vb->phys_base, size, 0x1000);
	vb->size = 0;
	vb->space = size;
	vb->next = 0;
	vb->vertices = 0;
	printf("alloc done, at %p %x\n", vb->base, vb->phys_base);
	return vb;
}

struct XenosVertexBuffer *Xe_VBPoolAlloc(struct XenosDevice *xe, int size)
{
	struct XenosVertexBuffer **vbp = &xe->vb_pool;
	
	while (*vbp)
	{
		struct XenosVertexBuffer *vb = *vbp;
		if (vb->space >= size)
		{
			*vbp = vb->next;
			vb->next = 0;
			vb->size = 0;
			vb->vertices = 0;
			return vb;
		}
		vbp = &vb->next;
	}
	
	return Xe_CreateVertexBuffer(xe, size);
}

void Xe_VBPoolAdd(struct XenosDevice *xe, struct XenosVertexBuffer *vb)
{
	struct XenosVertexBuffer **vbp = xe->vb_pool_after_frame ? &xe->vb_pool_after_frame->next : &xe->vb_pool_after_frame;
	while (*vbp)
		vbp = &(*vbp)->next;

	*vbp = vb;
}

void Xe_VBReclaim(struct XenosDevice *xe)
{
	struct XenosVertexBuffer **vbp = xe->vb_pool ? &xe->vb_pool->next : &xe->vb_pool;
	while (*vbp)
		vbp = &(*vbp)->next;
	
	*vbp = xe->vb_pool_after_frame;
	xe->vb_pool_after_frame = 0;
}

void Xe_VBBegin(struct XenosDevice *xe, int pitch)
{
	if (xe->vb_head || xe->vb_current)
		Xe_Fatal(xe, "FATAL: VertexBegin without VertexEnd! (head %08x, current %08x)\n", xe->vb_head, xe->vb_current);
	xe->vb_current_pitch = pitch;
}

void Xe_VBPut(struct XenosDevice *xe, void *data, int len)
{
	if (len % xe->vb_current_pitch)
		Xe_Fatal(xe, "FATAL: VertexPut with non-even len\n");
	
	while (len)
	{
		int remaining = xe->vb_current ? (xe->vb_current->space - xe->vb_current->size) : 0;
		
		remaining -= remaining % xe->vb_current_pitch;
		
		if (remaining > len)
			remaining = len;
		
		if (!remaining)
		{
			struct XenosVertexBuffer **n = xe->vb_head ? &xe->vb_current->next : &xe->vb_head;
			xe->vb_current = Xe_VBPoolAlloc(xe, 0x10000);
			*n = xe->vb_current;
			continue;
		}
		
		memcpy(xe->vb_current->base + xe->vb_current->size * 4, data, remaining * 4);
		xe->vb_current->size += remaining;
		xe->vb_current->vertices += remaining / xe->vb_current_pitch;
		data += remaining * 4;
		len -= remaining;
	}
}

struct XenosVertexBuffer *Xe_VBEnd(struct XenosDevice *xe)
{
	struct XenosVertexBuffer *res;
	res = xe->vb_head;
	
	while (xe->vb_head)
	{
		Xe_pSyncToDevice(xe, xe->vb_head->base, xe->vb_head->space * 4);
		Xe_pInvalidateGpuCache(xe, xe->vb_head->phys_base, (xe->vb_head->space * 4) + 0x1000);
		xe->vb_head = xe->vb_head->next;
	}

	xe->vb_head = xe->vb_current = 0;

	return res;
}

void Xe_Draw(struct XenosDevice *xe, struct XenosVertexBuffer *vb, struct XenosIndexBuffer *ib)
{
	Xe_pStuff(xe);
	
	if (vb->lock.start)
		Xe_Fatal(xe, "cannot draw locked VB");
	if (ib && ib->lock.start)
		Xe_Fatal(xe, "cannot draw locked IB");

	while (vb)
	{
		Xe_SetStreamSource(xe, 0, vb, 0, 0);
		Xe_pSetState(xe);

		rput32(0x00002007);
		rput32(0x00000000);

		Xe_pSetIndexOffset(xe, 0);
		if (!ib)
		{
			Xe_pDrawNonIndexed(xe, vb->vertices, XE_PRIMTYPE_TRIANGLELIST);
		} else
			Xe_pDrawIndexedPrimitive(xe, XE_PRIMTYPE_TRIANGLELIST, ib->indices, ib->phys_base, ib->indices, ib->fmt);

		xe->tris_drawn += vb->vertices / 3;
		vb = vb->next;
	}
}

int Xe_pCalcVtxCount(struct XenosDevice *xe, int primtype, int primcnt)
{
	switch (primtype)
	{
	case XE_PRIMTYPE_POINTLIST: return primcnt;
	case XE_PRIMTYPE_LINELIST: return primcnt * 2;
	case XE_PRIMTYPE_LINESTRIP: return 1 + primcnt;
	case XE_PRIMTYPE_TRIANGLELIST: return primcnt * 3;
	case XE_PRIMTYPE_TRIANGLESTRIP:  /* fall trough */
	case XE_PRIMTYPE_TRIANGLEFAN: return 2 + primcnt;
	case XE_PRIMTYPE_RECTLIST: return primcnt * 3; 
	default:
		Xe_Fatal(xe, "unknown primitive type");
	}
}

void Xe_DrawIndexedPrimitive(struct XenosDevice *xe, int type, int base_index, int min_index, int num_vertices, int start_index, int primitive_count)
{
	int cnt;

	assert(xe->ps); assert(xe->vs);

	Xe_pStuff(xe); /* fixme */
	Xe_pSetState(xe);
	rput32(0x00002007);
	rput32(0x00000000);

	Xe_pSetIndexOffset(xe, base_index);
	cnt = Xe_pCalcVtxCount(xe, type, primitive_count);
	int bpi = 2 << xe->current_ib->fmt;
	Xe_pDrawIndexedPrimitive(xe, type, cnt, xe->current_ib->phys_base + bpi * start_index, cnt, xe->current_ib->fmt);
}

void Xe_DrawPrimitive(struct XenosDevice *xe, int type, int start, int primitive_count)
{
	int cnt;
	
	assert(xe->ps); assert(xe->vs);

	Xe_pStuff(xe); /* fixme */
	Xe_pSetState(xe);
	rput32(0x00002007);
	rput32(0x00000000);

	Xe_pSetIndexOffset(xe, start); /* ?? */
	cnt = Xe_pCalcVtxCount(xe, type, primitive_count);
	Xe_pDrawNonIndexed(xe, 6, 4); // cnt, type);
}

void Xe_SetStreamSource(struct XenosDevice *xe, int index, struct XenosVertexBuffer *vb, int offset, int stride)
{
	if (vb->lock.start)
		Xe_Fatal(xe, "cannot use locked VB");

	xe->current_vb = vb;
	VERTEX_FETCH(xe->fetch_constants + (95 + index) * 2, vb->phys_base + offset, vb->space - offset);
	Xe_DirtyFetch(xe, 95 + index, 1);
}

void Xe_SetIndices(struct XenosDevice *xe, struct XenosIndexBuffer *ib)
{
	xe->current_ib = ib;
}

struct XenosIndexBuffer *Xe_CreateIndexBuffer(struct XenosDevice *xe, int length, int format)
{
	struct XenosIndexBuffer *ib = malloc(sizeof(struct XenosIndexBuffer));
	memset(ib, 0, sizeof(struct XenosIndexBuffer));
	ib->base = Xe_pAlloc(xe, &ib->phys_base, length, 32);
	ib->size = length;
	ib->indices = 0;
	ib->fmt = format;
	return ib;
}

void *Xe_VB_Lock(struct XenosDevice *xe, struct XenosVertexBuffer *vb, int offset, int size, int flags)
{
	Xe_pLock(xe, &vb->lock, vb->base + offset, vb->phys_base + offset, size, flags);
	return vb->base + offset;
}

void Xe_VB_Unlock(struct XenosDevice *xe, struct XenosVertexBuffer *vb)
{
	Xe_pUnlock(xe, &vb->lock);
}

void *Xe_IB_Lock(struct XenosDevice *xe, struct XenosIndexBuffer *ib, int offset, int size, int flags)
{
	Xe_pLock(xe, &ib->lock, ib->base + offset, ib->phys_base + offset, size, flags);
	return ib->base + offset;
}

void Xe_IB_Unlock(struct XenosDevice *xe, struct XenosIndexBuffer *ib)
{
	Xe_pUnlock(xe, &ib->lock);
}

void Xe_SetVertexShaderConstantF(struct XenosDevice *xe, int start, const float *data, int count)
{
//	printf("SetVertexShaderConstantF\n");
	memcpy(xe->alu_constants + start * 4, data, count * 16);
	Xe_DirtyAluConstant(xe, start, count);
//	while (count--)
//	{
//		printf("%.3f %.3f %.3f %.3f\n", data[0], data[1], data[2], data[3]);
//		data += 4;
//	}
}

void Xe_SetPixelShaderConstantF(struct XenosDevice *xe, int start, const float *data, int count)
{
	start += 256;
//	printf("SetPixelShaderConstantF (%d+)\n", start);
	memcpy(xe->alu_constants + start * 4, data, count * 16);
	Xe_DirtyAluConstant(xe, start, count);
//	while (count--)
//	{
//		printf("%.3f %.3f %.3f %.3f\n", data[0], data[1], data[2], data[3]);
//		data += 4;
//	}
}

struct XenosSurface *Xe_CreateTexture(struct XenosDevice *xe, unsigned int width, unsigned int height, unsigned int levels, int format, int tiled)
{
	struct XenosSurface *surface = malloc(sizeof(struct XenosSurface));
	memset(surface, 0, sizeof(struct XenosSurface));
	int bypp = 0;
	
	switch (format & XE_FMT_MASK)
	{
	case XE_FMT_565: bypp = 2; break;
	case XE_FMT_8888: bypp = 4; break;
	case XE_FMT_16161616: bypp = 8; break;
	}
	assert(bypp);
	
	int pitch = (width * bypp + 127) &~127;
	
	surface->width = width;
	surface->height = height;
	surface->pitch = pitch;
	surface->tiled = tiled;
	surface->format = format;
	surface->ptr_mip = 0;
	surface->bypp = bypp;
	surface->base = Xe_pAlloc(xe, &surface->ptr, height * pitch, 1024 * bypp); // 4k seems right

	return surface;
}

void *Xe_Surface_LockRect(struct XenosDevice *xe, struct XenosSurface *surface, int x, int y, int w, int h, int flags)
{
#if 0
	if (surface == xe->rt) /* current render target? sync. */
	{
		Xe_Resolve(xe);
		Xe_Sync(xe);
	}
#endif
	if (!w)
		w = surface->width;
	if (!h)
		h = surface->height;

	int offset = y * surface->pitch + x * surface->bypp;
	int size = h * surface->pitch;

	Xe_pLock(xe, &surface->lock, surface->base + offset, surface->ptr + offset, size, flags);
	return surface->base + offset;
}

void Xe_Surface_Unlock(struct XenosDevice *xe, struct XenosSurface *surface)
{
	Xe_pUnlock(xe, &surface->lock);
}


extern void xenos_edram_init(void);

int reloc[10];

void edram_72c(struct XenosDevice *xe)
{
 // 0000072c: (+0)
	rput32(0x00002000);
		rput32(0x00800050); 
 // 00000734: (+2)
	rput32(0x00002001);
		rput32(0x00000000); 
 // 0000073c: (+4)
	rput32(0x00002301);
		rput32(0x00000000); 
 // 00000744: (+6)
	rput32(0x0005210f);
		rput32(0x41000000); rput32(0x41000000); rput32(0xc0800000); rput32(0x40800000); rput32(0x3f800000); rput32(0x00000000); 
 // 00000760: (+d)
	rput32(0x00022080);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00080010); 
 // 00000770: (+11)
	rput32(0xc0192b00);
		rput32(0x00000000); rput32(0x00000018); rput32(0x30052003); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00001005); rput32(0x00001200); rput32(0xc2000000); 
		rput32(0x00001006); rput32(0x10071200); rput32(0x22000000); rput32(0x1df81000); 
		rput32(0x00253b08); rput32(0x00000004); rput32(0x00080000); rput32(0x40253908); 
		rput32(0x00000200); rput32(0xc8038000); rput32(0x00b0b000); rput32(0xc2000000); 
		rput32(0xc80f803e); rput32(0x00000000); rput32(0xc2010100); rput32(0xc8000000); 
		rput32(0x00000000); rput32(0x02000000); 
 // 000007dc: (+2c)
	rput32(0xc00d2b00);
		rput32(0x00000001); rput32(0x0000000c); rput32(0x00011002); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00001003); rput32(0x00002200); rput32(0x00000000); 
		rput32(0x50080001); rput32(0x1f1ff688); rput32(0x00004000); rput32(0xc80f8000); 
		rput32(0x00000000); rput32(0xc2000000); 
 // 00000818: (+3b)
	rput32(0x00012180);
		rput32(0x10030001); rput32(0x00000000); 
 // 00000824: (+3e)
	rput32(0x000548ba);
		rput32(0x00000003); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(reloc[2]); rput32(0x10000042); 
 // 00000840: (+45)
	rput32(0x00054800);
		rput32(0x80400002); rput32(reloc[8]); rput32(0x0000e00f); rput32(0x01000c14); rput32(0x00000000); rput32(0x00000200); 
 // 0000085c: (+4c)
	rput32(0xc0003601);
		rput32(0x00040086); 


					 /* resolve */
 // 00000864: (+4e)
	rput32(0x00002318);
		rput32(0x00100000); 
 // 0000086c: (+50)
	rput32(0x00002319);
		rput32(reloc[9]); 
 // 00000874: (+52)
	rput32(0x0000231a);
		rput32(0x00080020);
 // 0000087c: (+54)
	rput32(0x0000231b);
		rput32(0x01000302); 
 // 00000884: (+56)
	rput32(0xc00d2b00);
		rput32(0x00000000); rput32(0x0000000c); rput32(0x10011002); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00000000); rput32(0x1003c200); rput32(0x22000000); 
		rput32(0x05f80000); rput32(0x00253b48); rput32(0x00000002); rput32(0xc80f803e); 
		rput32(0x00000000); rput32(0xc2000000); 
 // 000008c0: (+65)
	rput32(0x00012180);
		rput32(0x00010002); rput32(0x00000000); 
 // 000008cc: (+68)
	rput32(0x00002208);
		rput32(0x00000006); 
 // 000008d4: (+6a)
	rput32(0x00002200);
		rput32(0x00000000); 
 // 000008dc: (+6c)
	rput32(0x00002203);
		rput32(0x00000000); 
 // 000008e4: (+6e)
	rput32(0x00022100);
		rput32(0x0000ffff); rput32(0x00000000); rput32(0x00000000); 
 // 000008f4: (+72)
	rput32(0x00022204);
		rput32(0x00010000); rput32(0x00010000); rput32(0x00000300); 
 // 00000904: (+76)
	rput32(0x00002312);
		rput32(0x0000ffff); 
 // 0000090c: (+78)
	rput32(0x000548ba);
		rput32(0x00000003); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(reloc[3]); rput32(0x1000001a); 
 // 00000928: (+7f)
	rput32(0xc0003601);
		rput32(0x00030088); 

				/* sync memory */
 // 00000930: (+81)
	rput32(0x00002007);
		rput32(0x00000000); 
 // 00000938: (+83)
	rput32(0x00000a31);
		rput32(0x03000100); 
 // 00000940: (+85)
	rput32(0x00010a2f);
		rput32(0x00002000); rput32(reloc[9]); 
 // 0000094c: (+88)
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008); 
 // 00000964: (+8e)
	rput32(0x00002208);
		rput32(0x00000004); 
 // 0000096c: (+90)
	rput32(0x00002206);
		rput32(0x00000000); 
}

void edram_974(struct XenosDevice *xe)
{
		///////////////// part 2
 // 00000974: (+0)
	rput32(0x00000a31);
		rput32(0x02000000); 
 // 0000097c: (+2)
	rput32(0x00010a2f);
		rput32(0x00001000); rput32(reloc[0]); 
 // 00000988: (+5)
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008); 
 // 000009a0: (+b)
	rput32(0x00002000);
		rput32(0x00800050); 
 // 000009a8: (+d)
	rput32(0x00002001);
		rput32(0x00000000); 
 // 000009b0: (+f)
	rput32(0x00002301);
		rput32(0x00000000); 
 // 000009b8: (+11)
	rput32(0x0005210f);
		rput32(0x41800000); rput32(0x41800000); rput32(0xc1800000); rput32(0x41800000); rput32(0x3f800000); rput32(0x00000000); 
 // 000009d4: (+18)
	rput32(0x00022080);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00200020); 
}

void edram_9e4(struct XenosDevice *xe)
{
							//////////
 // 000009e4: (+0)
	rput32(0xc0192b00);
		rput32(0x00000000); rput32(0x00000018); rput32(0x30052003); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00001005); rput32(0x00001200); rput32(0xc2000000); 
		rput32(0x00001006); rput32(0x10071200); rput32(0x22000000); rput32(0x1df81000); 
		rput32(0x00253b08); rput32(0x00000004); rput32(0x00080000); rput32(0x40253908); 
		rput32(0x00000200); rput32(0xc8038000); rput32(0x00b0b000); rput32(0xc2000000); 
		rput32(0xc80f803e); rput32(0x00000000); rput32(0xc2010100); rput32(0xc8000000); 
		rput32(0x00000000); rput32(0x02000000); 
 // 00000a50: (+1b)
	rput32(0xc00d2b00);
		rput32(0x00000001); rput32(0x0000000c); rput32(0x00011002); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00001003); rput32(0x00002200); rput32(0x00000000); 
		rput32(0x50080001); rput32(0x1f1ff688); rput32(0x00004000); rput32(0xc80f8000); 
		rput32(0x00000000); rput32(0xc2000000); 
 // 00000a8c: (+2a)
	rput32(0x00012180);
		rput32(0x10030001); rput32(0x00000000); 
 // 00000a98: (+2d)
	rput32(0x000548ba);
		rput32(0x00000003); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(reloc[4]); rput32(0x10000042); 
 // 00000ab4: (+34)
	rput32(0x00054800);
		rput32(0x80400002); rput32(reloc[8]); rput32(0x0003e01f); rput32(0x01000c14); 
		rput32(0x00000000); rput32(0x00000200); 
 // 00000ad0: (+3b)
	rput32(0xc0003601);
		rput32(0x00040086); 
 // 00000ad8: (+3d)

	rput32(0x00002318);
		rput32(0x00100000); 
 // 00000ae0: (+3f)
	rput32(0x00002319);
		rput32(reloc[9]); 
 // 00000ae8: (+41)
	rput32(0x0000231a);
		rput32(0x00200020); 
 // 00000af0: (+43)
	rput32(0x0000231b);
		rput32(0x01000302); 
 // 00000af8: (+45)
	rput32(0xc00d2b00);
		rput32(0x00000000); rput32(0x0000000c); rput32(0x10011002); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00000000); rput32(0x1003c200); rput32(0x22000000); 
		rput32(0x05f80000); rput32(0x00253b48); rput32(0x00000002); rput32(0xc80f803e); 
		rput32(0x00000000); rput32(0xc2000000); 
 // 00000b34: (+54)
	rput32(0x00012180);
		rput32(0x00010002); rput32(0x00000000); 
 // 00000b40: (+57)
	rput32(0x00002208);
		rput32(0x00000006); 
 // 00000b48: (+59)
	rput32(0x00002200);
		rput32(0x00000000); 
 // 00000b50: (+5b)
	rput32(0x00002203);
		rput32(0x00000000); 
 // 00000b58: (+5d)
	rput32(0x00022100);
		rput32(0x0000ffff); rput32(0x00000000); rput32(0x00000000); 
 // 00000b68: (+61)
	rput32(0x00022204);
		rput32(0x00010000); rput32(0x00010000); rput32(0x00000300); 
 // 00000b78: (+65)
	rput32(0x00002312);
		rput32(0x0000ffff); 
 // 00000b80: (+67)
	rput32(0x000548ba);
		rput32(0x00000003); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(reloc[6]); rput32(0x1000001a); 
 // 00000b9c: (+6e)
	rput32(0xc0003601);
		rput32(0x00030088); 

 // 00000ba4: (+70)
	rput32(0x00002007);
		rput32(0x00000000); 
 // 00000bac: (+72)
	rput32(0x00000a31);
		rput32(0x03000100); 
 // 00000bb4: (+74)
	rput32(0x00010a2f);
		rput32(0x00002000); rput32(reloc[9]); 
 // 00000bc0: (+77)
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008); 
 // 00000bd8: (+7d)
	rput32(0x00002208);
		rput32(0x00000004); 
 // 00000be0: (+7f)
	rput32(0x00002206);
		rput32(0x00000000); 
}

void edram_bec(struct XenosDevice *xe)
{
					////////////////
 // 00000bec: (+0)
	rput32(0x00002000);
		rput32(0x19000640); 
 // 00000bf4: (+2)
	rput32(0x00002001);
		rput32(0x00000000); 
 // 00000bfc: (+4)
	rput32(0x00002301);
		rput32(0x00000000); 
 // 00000c04: (+6)
	rput32(0x0005210f);
		rput32(0x44480000); rput32(0x44480000); rput32(0xc44c0000); rput32(0x444c0000); rput32(0x3f800000); rput32(0x00000000); 
 // 00000c20: (+d)
	rput32(0x00022080);
		rput32(0x00000000); rput32(0x00000000); rput32(0x06600640); 
 // 00000c30: (+11)
	rput32(0xc0162b00);
		rput32(0x00000000); rput32(0x00000015); rput32(0x10011003); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00001004); rput32(0x00001200); rput32(0xc2000000); 
		rput32(0x00001005); rput32(0x10061200); rput32(0x22000000); rput32(0x0df81000); 
		rput32(0x00253b08); rput32(0x00000002); rput32(0xc8038000); rput32(0x00b06c00); 
		rput32(0x81010000); rput32(0xc80f803e); rput32(0x00000000); rput32(0xc2010100); 
		rput32(0xc8000000); rput32(0x00000000); rput32(0x02000000); 
 // 00000c90: (+29)
	rput32(0xc0522b00);
		rput32(0x00000001); rput32(0x00000051); rput32(0x00000000); rput32(0x6003c400); 
		rput32(0x12000000); rput32(0x00006009); rput32(0x600f1200); rput32(0x12000000); 
		rput32(0x00006015); rput32(0x00002200); rput32(0x00000000); rput32(0xc8030000); 
		rput32(0x00b06cc6); rput32(0x8b000002); rput32(0x4c2c0000); rput32(0x00ac00b1); 
		rput32(0x8a000001); rput32(0x08100100); rput32(0x00000031); rput32(0x02000000); 
		rput32(0x4c100000); rput32(0x0000006c); rput32(0x02000001); rput32(0xc8030001); 
		rput32(0x006c1ac6); rput32(0xcb010002); rput32(0xc8030001); rput32(0x00b00000); 
		rput32(0x8a010000); rput32(0xc8030001); rput32(0x00b0c600); rput32(0x81010000); 
		rput32(0xa8430101); rput32(0x00b00080); rput32(0x8a010000); rput32(0xc80f0001); 
		rput32(0x00c00100); rput32(0xc1010000); rput32(0xc80f0000); rput32(0x00000000); 
		rput32(0x88810000); rput32(0xc80f0000); rput32(0x01000000); rput32(0xed010000); 
		rput32(0xc80f0004); rput32(0x00aabc00); rput32(0x81000100); rput32(0xc8060000); 
		rput32(0x00166c00); rput32(0x86040100); rput32(0xc8090000); rput32(0x04c56c00); 
		rput32(0x80000100); rput32(0xc8030002); rput32(0x04b06c00); rput32(0x80040100); 
		rput32(0xc8070003); rput32(0x00bc6cb1); rput32(0x6c020000); rput32(0xc8020001); 
		rput32(0x00b06d6c); rput32(0xd1040002); rput32(0xc8010001); rput32(0x00b0b26c); 
		rput32(0xd1020302); rput32(0xc8080001); rput32(0x006d6e6c); rput32(0xd1040302); 
		rput32(0xc8040001); rput32(0x006d6d6c); rput32(0xd1020002); rput32(0xc8018000); 
		rput32(0x001a1a6c); rput32(0xd1010002); rput32(0xc8028000); rput32(0x00b01a6c); 
		rput32(0xd1010002); rput32(0xc8048000); rput32(0x00c71a6c); rput32(0xd1010002); 
		rput32(0xc8088000); rput32(0x006d1a6c); rput32(0xd1010002); 
 // 00000de0: (+7d)
	rput32(0x00012180);
		rput32(0x10010401); rput32(0x00000000); 
 // 00000dec: (+80)
	rput32(0x000548ba);
		rput32(0x00000003); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(reloc[5]); rput32(0x10000022); 
 // 00000e08: (+87)
	rput32(0x000f4000);
		rput32(0x3b800000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 00000e4c: (+98)
	rput32(0x000f4400);
		rput32(0x47c35000); rput32(0x3727c5ac); rput32(0x3eaaaaab); rput32(0x43800000); 
		rput32(0x3f800000); rput32(0x40000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x3f800000); rput32(0x3f000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 00000e90: (+a9)
	rput32(0xc0003601);
		rput32(0x00040086); 
 // 00000e98: (+ab)
	rput32(0x00002201);
		rput32(0x0b0a0b0a); 
 // 00000ea0: (+ad)
	rput32(0xc0003601);
		rput32(0x00040086); 
 // 00000ea8: (+af)
	rput32(0x00002201);
		rput32(0x00010001); 
 // 00000eb0: (+b1)
	rput32(0x00002318);
		rput32(0x00300000); 
 // 00000eb8: (+b3)
	rput32(0x00002319);
		rput32(0x00000000); 
 // 00000ec0: (+b5)
	rput32(0x0000231a);
		rput32(0x06600640); 
 // 00000ec8: (+b7)
	rput32(0x0000231b);
		rput32(0x01000302); 
 // 00000ed0: (+b9)
	rput32(0xc00d2b00);
		rput32(0x00000000); rput32(0x0000000c); rput32(0x10011002); rput32(0x00001200); 
		rput32(0xc4000000); rput32(0x00000000); rput32(0x1003c200); rput32(0x22000000); 
		rput32(0x05f80000); rput32(0x00253b48); rput32(0x00000002); rput32(0xc80f803e); 
		rput32(0x00000000); rput32(0xc2000000); 
 // 00000f0c: (+c8)
	rput32(0x00012180);
		rput32(0x00010002); rput32(0x00000000); 
 // 00000f18: (+cb)
	rput32(0x00002208);
		rput32(0x00000006); 
 // 00000f20: (+cd)
	rput32(0x00002200);
		rput32(0x00000000); 
 // 00000f28: (+cf)
	rput32(0x00002203);
		rput32(0x00000000); 
 // 00000f30: (+d1)
	rput32(0x00022100);
		rput32(0x0000ffff); rput32(0x00000000); rput32(0x00000000); 
 // 00000f40: (+d5)
	rput32(0x00022204);
		rput32(0x00010000); rput32(0x00010000); rput32(0x00000300); 
 // 00000f50: (+d9)
	rput32(0x00002312);
		rput32(0x0000ffff); 
 // 00000f58: (+db)
	rput32(0x000548ba);
		rput32(0x00000003); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(reloc[7]); rput32(0x1000001a); 
 // 00000f74: (+e2)
	rput32(0xc0003601);
		rput32(0x00030088); 
 // 00000f7c: (+e4)
	rput32(0x00002007);
		rput32(0x00000000); 
 // 00000f84: (+e6)
	rput32(0x00002208);
		rput32(0x00000004); 
 // 00000f8c: (+e8)
	rput32(0x00002206);
		rput32(0x00000000); 

}

void edram_4c(struct XenosDevice *xe)
{
	 // 0000004c: (+0)
	rput32(0xc0015000);
		rput32(0xffffffff); rput32(0x00000000); 
 // 00000058: (+3)
	rput32(0xc0015100);
		rput32(0xffffffff); rput32(0xffffffff); 
 // 00000064: (+6)
	rput32(0x00022080);
		rput32(0x00000000); rput32(0x00000000); rput32(0x01e00280); 
 // 00000074: (+a)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 0000007c: (+c)
	rput32(0x00032388);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 00000090: (+11)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 00000098: (+13)
	rput32(0x0003238c);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 000000ac: (+18)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 000000b4: (+1a)
	rput32(0x00032390);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 000000c8: (+1f)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 000000d0: (+21)
	rput32(0x00032394);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 000000e4: (+26)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 000000ec: (+28)
	rput32(0x00032398);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 00000100: (+2d)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 00000108: (+2f)
	rput32(0x0003239c);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 0000011c: (+34)
	rput32(0x000005c8);
		rput32(0x00020000); 
 // 00000124: (+36)
	rput32(0x00000f01);
		rput32(0x0000200e); 
 // 0000012c: (+38)
	rput32(0x00252300);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000004); rput32(0x40000000); 
		rput32(0x3f800000); rput32(0x40000000); rput32(0x3f800000); rput32(0x000ff000); 
		rput32(0x000ff100); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x0000ffff); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x0000000e); rput32(0x00000010); 
		rput32(0x00100000); rput32(0x1f923000); rput32(0x01e00280); rput32(0x01000300); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000002); rput32(0x00000000); 
 // 000001c8: (+5f)
	rput32(0x00072380);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 000001ec: (+68)
	rput32(0x00bf4800);
		rput32(0x04000002); rput32(0x1f90b04f); rput32(0x000a61ff); rput32(0x01000c14); 
		rput32(0x00000000); rput32(0x00000a00); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x01000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x01000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
		rput32(0x00000001); rput32(0x00000000); rput32(0x00000001); rput32(0x00000000); 
 // 000004f0: (+129)
	rput32(0x00274900);
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
 // 00000594: (+152)
	rput32(0x000f2000);
		rput32(0x0a000280); rput32(0x00000000); rput32(0x000000f0); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x20002000); 
 // 000005d8: (+163)
	rput32(0x00142100);
		rput32(0x00ffffff); rput32(0x00000000); rput32(0x00000000); rput32(0x0000ffff); 
		rput32(0x0000000f); rput32(0x3f800000); rput32(0x3f800000); rput32(0x3f800000); 
		rput32(0x3f800000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00ffff00); rput32(0x00ffff00); rput32(0x00000000); rput32(0x43a00000); 
		rput32(0x43a00000); rput32(0xc3700000); rput32(0x43700000); rput32(0x3f800000); 
		rput32(0x00000000); 
 // 00000630: (+179)
	rput32(0x00042180);
		rput32(0x10130200); rput32(0x00000004); rput32(0x00010001); rput32(0x00000000); 
		rput32(0x00000000); 
 // 00000648: (+17f)
	rput32(0x000b2200);
		rput32(0x00700730); rput32(0x00010001); rput32(0x87000007); rput32(0x00000001); 
		rput32(0x00090000); rput32(0x00018006); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000004); rput32(0x00010001); rput32(0x00010001); rput32(0x00010001); 
 // 0000067c: (+18c)
	rput32(0x00142280);
		rput32(0x00080008); rput32(0x04000010); rput32(0x00000008); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000001); rput32(0x3f800000); rput32(0x3f800000); 
		rput32(0x0000000e); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); rput32(0x00000000); 
		rput32(0x00000000); 
 // 000006d4: (+1a2)
	rput32(0x00000a31);
		rput32(0x02000000); 
 // 000006dc: (+1a4)
	rput32(0x00010a2f);
		rput32(0x00001000); rput32(reloc[0]);
 // 000006e8: (+1a7)
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008); 
 // 00000700: (+1ad)
	rput32(0x00000a31);
		rput32(0x01000000); 
 // 00000708: (+1af)
	rput32(0x00010a2f);
		rput32(0x00000100); rput32(reloc[1]); 
 // 00000714: (+1b2)
	rput32(0xc0043c00);
		rput32(0x00000003); rput32(0x00000a31); rput32(0x00000000); rput32(0x80000000); rput32(0x00000008); 
}

int edram_compare_crc(uint32_t *crc);

static uint32_t determine_broken(uint32_t v0)
{
/*
00006000
00018000
00410000
00082000
00700000
00180000
*/
	int res = 0;
	if ((v0 & 0x00006000) == 0x00006000)
		res |= 1;
	if ((v0 & 0x00018000) == 0x00018000)
		res |= 2;
	if ((v0 & 0x00410000) == 0x00410000)
		res |= 4;
	if ((v0 & 0x00082000) == 0x00082000)
		res |= 8;
	if ((v0 & 0x00700000) == 0x00700000)
		res |= 16;
	if ((v0 & 0x00180000) == 0x00180000)
		res |= 32;
	return res;
}

void do_edram_foo(struct XenosDevice *xe, int complete)
{
	int tries = 4;
retry:
	if (!tries--)
		Xe_Fatal(xe, "damnit, EDRAM init failed, again and again.");
	

//	if (complete)
	{
		/* state 1*/
		xenos_edram_init();
	}
	
	int i;
#if 0
	printf("waiting for temperature to stabilize...\n");

	for (i = 0; i < 40; ++i)
	{
		uint16_t data[4];
		xenon_smc_query_sensors(data);
		
		printf("%f %f %f %f\n", data[0] / 256.0, data[1] / 256.0, data[2] / 256.0, data[3] / 256.0);
		delay(1);
	}
#endif	

	static u32 base;
	static void *ptr;
	if (!ptr)
		ptr = Xe_pAlloc(xe, &base, 0x4000, 0x100000);
	
			/* state 2 */
	reloc[0] = base + 0x0000;
	reloc[1] = base + 0x2000;
	reloc[2] = base + 0x2003;
	reloc[3] = base + 0x2083;
	reloc[4] = base + 0x2043;
	reloc[5] = base + 0x20b3;
	reloc[6] = base + (0x2098|3);
	reloc[7] = base + 0x20d3;
	reloc[8] = (base + 0x0000) | (0x43 << 1);
	reloc[9] = base + 0x1000;
	
	memset(ptr + 0, 1, 0x1000); /* debug only */

	memset(ptr + 0x1000, 0x99, 0x1000);

	*(u32*)(ptr + 0x108) = 0x1000;
	*(u32*)(ptr + 0x118) = 0x1;
	*(u32*)(ptr + 0x128) = 0x1100;
	*(u32*)(ptr + 0x12c) = 0x1000;
	*(u32*)(ptr + 0x13c) = 0;
	
	float reg_00[] = {
		-0.5,  7.5, 0.0, 1.0, // top left
		-0.5, -0.5, 0.0, 0.0, // bottom left
		15.5,  7.5, 1.0, 1.0, // bottom right
		15.5, -0.5, 1.0, 0.0  // top right
	};
	float reg_40[] = {
		-0.5, 31.5, 0.0, 1.0, 
		-0.5, -0.5, 0.0, 0.0, 
		31.5, 31.5, 1.0, 1.0, 
		31.5, -0.5, 1.0, 0.0
	};
	float reg_b0[] = {
		-0.5,   1631.5, 
		-0.5,     -0.5, 
		1599.5, 1631.5, 
		1599.5,   -0.5
	};
	float reg_80[] = {-.5, 7.5,  -.5,  -.5, 15.5, -.5};
	float reg_98[] = {-.5, 31.5, -.5,  -.5, 31.5, -.5};
	float reg_d0[] = {-.5, -.5, -.5, 1599.5, -.5};
	
	memcpy(ptr + 0x2000 + 0x00, reg_00, sizeof(reg_00));
	memcpy(ptr + 0x2000 + 0x40, reg_40, sizeof(reg_40));
	memcpy(ptr + 0x2000 + 0xb0, reg_b0, sizeof(reg_b0));
	memcpy(ptr + 0x2000 + 0x80, reg_80, sizeof(reg_80));
	memcpy(ptr + 0x2000 + 0x98, reg_98, sizeof(reg_98));
	memcpy(ptr + 0x2000 + 0xd0, reg_d0, sizeof(reg_d0));
	
	Xe_pSyncToDevice(xe, ptr, 0x4000);

	w32(0x3c04, r32(0x3c04) &~ 0x100);

//	assert(r32(0x3c04) == 0x200e);
	udelay(1000);
	int var_1 = 0x111111;
	w32(0x3cb4, 0x888888 | var_1);
	udelay(1000);
	edram_pc();


	edram_4c(xe);
	
	memset(ptr + 0x1000, 'Z', 0x1000);
	Xe_pSyncToDevice(xe, ptr, 0x4000);

	w32(0x3c94, (r32(0x3c94) &~0x800000FF) | 0xb);
	udelay(1000);
	w32(0x3c94, r32(0x3c94) | 0x80000000);
	udelay(1000);

	memset(ptr + 0x1000, 0, 0x1000);
	Xe_pSyncToDevice(xe, ptr, 0x4000);

	int var_2;	
	
	int fail = 0;
	for (var_2 = 0xb; var_2 < 0x13; ++var_2)
	{
			/* state 4 */
		int seed = 0x425a;
		for (i = 0; i < 0x20 * 0x20; ++i)
		{
			seed *= 0x41c64e6d;
			seed += 12345;
			
			int p1 = (seed >> 16) & 0x7fff;

			seed *= 0x41c64e6d;
			seed += 12345;
			
			int p2 = (seed >> 16) & 0x7fff;
			
			int v = (p2<<16) + p1;
			
			((int*)ptr) [i] = v;
		}
		memset(ptr + 0x1000, 0x44, 0x1000);
		Xe_pSyncToDevice(xe, ptr, 0x4000);

		w32(0x3c94, (r32(0x3c94) &~0x800000FF) | var_2);
		udelay(1000);
		w32(0x3c94, r32(0x3c94) | 0x80000000);
		udelay(1000);

		edram_pc();
//		edram_72c(xe);
		edram_974(xe);
//			printf("before 9e4\n");
		edram_9e4(xe);
//			printf("after 9e4\n");
			
		Xe_pDebugSync(xe);
		Xe_pSyncFromDevice(xe, ptr, 0x4000);

		uint32_t good_crc[] = {
			0xEBBCB7D0, 0xB7599E02, 0x0AEA2A7A, 0x2CABD6B8, 
			0xA5A5A5A5, 0xA5A5A5A5, 0xE57C27BE, 0x43FA90AA, 
			0x9D065F66, 0x360A6AD8, 0xA5A5A5A5, 0xA5A5A5A5, 
			0xA5A5A5A5, 0xEBBCB7D0, 0xB7599E02, 0x0AEA2A7A, 
			0x2CABD6B8, 0xA5A5A5A5, 0xA5A5A5A5, 0xE57C27BE,
			0x43FA90AA, 0x9D065F66, 0x360A6AD8, 0xA5A5A5A5
		};

		fail = edram_compare_crc(good_crc);
		fail = determine_broken(fail);
		
		if (fail != 0x3f)
			goto fix_xxx;

		if (!fail) goto worked; /* OMGWTF IT WORKED!!1 */
	}
//	printf("great, our base var_2 is %d, let's fix remaining problems\n", var_2);

fix_xxx:;

	int ixxx;
	for (ixxx = 0; ixxx < 6; ++ixxx)
	{
		int vxxx;
		if (!(fail & (1<<ixxx)))
		{
//			printf("not touching, should be ok\n");
			continue;
		} 
		for (vxxx = 0; vxxx < 4; ++vxxx)
		{
			var_1 &= ~(0xF<<(ixxx*4));
			var_1 |= vxxx << (ixxx * 4);

			memset(ptr + 0, 1, 0x1000); /* debug only */
			*(u32*)(ptr + 0x108) = 0x1000;
			*(u32*)(ptr + 0x118) = 0x1;
			*(u32*)(ptr + 0x128) = 0x1100;
			*(u32*)(ptr + 0x12c) = 0x1000;
			*(u32*)(ptr + 0x13c) = 0;

			Xe_pSyncToDevice(xe, ptr, 0x4000);

			udelay(1000);
			w32(0x3cb4, var_1 | 0x888888);
			udelay(1000);

			edram_pc();
			edram_72c(xe);
			Xe_pDebugSync(xe);
			Xe_pSyncFromDevice(xe, ptr, 0x4000);
			

				/* state 4 */
			int seed = 0x425a;
			for (i = 0; i < 0x20 * 0x20; ++i)
			{
				seed *= 0x41c64e6d;
				seed += 12345;
				
				int p1 = (seed >> 16) & 0x7fff;

				seed *= 0x41c64e6d;
				seed += 12345;
				
				int p2 = (seed >> 16) & 0x7fff;
				
				int v = (p2<<16) + p1;
				
				((int*)ptr) [i] = v;
			}
			memset(ptr + 0x1000, 0x44, 0x1000);
			Xe_pSyncToDevice(xe, ptr, 0x4000);

			w32(0x3c94, (r32(0x3c94) &~0x800000FF) | var_2);
			udelay(1000);
			w32(0x3c94, r32(0x3c94) | 0x80000000);
			udelay(1000);

			edram_pc();
	//		edram_72c(xe);
			edram_974(xe);
	//			printf("before 9e4\n");
			edram_9e4(xe);
	//			printf("after 9e4\n");
				
			Xe_pDebugSync(xe);
			Xe_pSyncFromDevice(xe, ptr, 0x4000);

			uint32_t good_crc[] = {
				0xEBBCB7D0, 0xB7599E02, 0x0AEA2A7A, 0x2CABD6B8, 
				0xA5A5A5A5, 0xA5A5A5A5, 0xE57C27BE, 0x43FA90AA, 
				0x9D065F66, 0x360A6AD8, 0xA5A5A5A5, 0xA5A5A5A5, 
				0xA5A5A5A5, 0xEBBCB7D0, 0xB7599E02, 0x0AEA2A7A, 
				0x2CABD6B8, 0xA5A5A5A5, 0xA5A5A5A5, 0xE57C27BE,
				0x43FA90AA, 0x9D065F66, 0x360A6AD8, 0xA5A5A5A5
			};

			fail = determine_broken(edram_compare_crc(good_crc));
//			printf("[%08x]=%08x ", var_1, fail);
			if (!(fail & (1<< ixxx)))
			{
//				printf("cool");
				break;
			}	
		}
		if (vxxx == 4)
		{
//			printf("can't fix that :(\n");
			break;
		}

//		printf("\n");
	}
	if (fail)
		goto retry;
worked:
	printf("EDRAM %08x, %08x\n", var_1, var_2);
}