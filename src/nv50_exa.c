/*
 * Copyright 2007 NVIDIA, Corporation
 * Copyright 2008 Ben Skeggs
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
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nv_include.h"
#include "nv_rop.h"

#include "nv50_accel.h"
#include "nv50_texture.h"

struct nv50_exa_state {
	Bool have_mask;

	struct {
		PictTransformPtr transform;
		float width;
		float height;
	} unit[2];
};
static struct nv50_exa_state exa_state;

#define NV50EXA_LOCALS(p)                                              \
	ScrnInfoPtr pScrn = xf86Screens[(p)->drawable.pScreen->myNum]; \
	NVPtr pNv = NVPTR(pScrn);                                      \
	struct nouveau_channel *chan = pNv->chan; (void)chan;          \
	struct nouveau_grobj *eng2d = pNv->Nv2D; (void)eng2d;          \
	struct nouveau_grobj *tesla = pNv->Nv3D; (void)tesla;          \
	struct nv50_exa_state *state = &exa_state; (void)state

#define BF(f) (NV50TCL_BLEND_FUNC_SRC_RGB_##f | 0x4000)

struct nv50_blend_op {
	unsigned src_alpha;
	unsigned dst_alpha;
	unsigned src_blend;
	unsigned dst_blend;
};

static struct nv50_blend_op
NV50EXABlendOp[] = {
/* Clear       */ { 0, 0, BF(               ZERO), BF(               ZERO) },
/* Src         */ { 0, 0, BF(                ONE), BF(               ZERO) },
/* Dst         */ { 0, 0, BF(               ZERO), BF(                ONE) },
/* Over        */ { 1, 0, BF(                ONE), BF(ONE_MINUS_SRC_ALPHA) },
/* OverReverse */ { 0, 1, BF(ONE_MINUS_DST_ALPHA), BF(                ONE) },
/* In          */ { 0, 1, BF(          DST_ALPHA), BF(               ZERO) },
/* InReverse   */ { 1, 0, BF(               ZERO), BF(          SRC_ALPHA) },
/* Out         */ { 0, 1, BF(ONE_MINUS_DST_ALPHA), BF(               ZERO) },
/* OutReverse  */ { 1, 0, BF(               ZERO), BF(ONE_MINUS_SRC_ALPHA) },
/* Atop        */ { 1, 1, BF(          DST_ALPHA), BF(ONE_MINUS_SRC_ALPHA) },
/* AtopReverse */ { 1, 1, BF(ONE_MINUS_DST_ALPHA), BF(          SRC_ALPHA) },
/* Xor         */ { 1, 1, BF(ONE_MINUS_DST_ALPHA), BF(ONE_MINUS_SRC_ALPHA) },
/* Add         */ { 0, 0, BF(                ONE), BF(                ONE) },
};

static Bool
NV50EXA2DSurfaceFormat(PixmapPtr ppix, uint32_t *fmt)
{
	NV50EXA_LOCALS(ppix);

	switch (ppix->drawable.depth) {
	case 8 : *fmt = NV50_2D_SRC_FORMAT_R8_UNORM; break;
	case 15: *fmt = NV50_2D_SRC_FORMAT_X1R5G5B5_UNORM; break;
	case 16: *fmt = NV50_2D_SRC_FORMAT_R5G6B5_UNORM; break;
	case 24: *fmt = NV50_2D_SRC_FORMAT_X8R8G8B8_UNORM; break;
	case 30: *fmt = NV50_2D_SRC_FORMAT_A2B10G10R10_UNORM; break;
	case 32: *fmt = NV50_2D_SRC_FORMAT_A8R8G8B8_UNORM; break;
	default:
		 NOUVEAU_FALLBACK("Unknown surface format for bpp=%d\n",
				  ppix->drawable.depth);
		 return FALSE;
	}

	return TRUE;
}

static void NV50EXASetClip(PixmapPtr ppix, int x, int y, int w, int h)
{
	NV50EXA_LOCALS(ppix);

	BEGIN_RING(chan, eng2d, NV50_2D_CLIP_X, 4);
	OUT_RING  (chan, x);
	OUT_RING  (chan, y);
	OUT_RING  (chan, w);
	OUT_RING  (chan, h);
}

static Bool
NV50EXAAcquireSurface2D(PixmapPtr ppix, int is_src)
{
	NV50EXA_LOCALS(ppix);
	struct nouveau_bo *bo = nouveau_pixmap_bo(ppix);
	unsigned delta = nouveau_pixmap_offset(ppix);
	int mthd = is_src ? NV50_2D_SRC_FORMAT : NV50_2D_DST_FORMAT;
	uint32_t fmt, bo_flags;

	if (!NV50EXA2DSurfaceFormat(ppix, &fmt))
		return FALSE;

	bo_flags  = NOUVEAU_BO_VRAM;
	bo_flags |= is_src ? NOUVEAU_BO_RD : NOUVEAU_BO_WR;

	if (!nv50_style_tiled_pixmap(ppix)) {
		BEGIN_RING(chan, eng2d, mthd, 2);
		OUT_RING  (chan, fmt);
		OUT_RING  (chan, 1);
		BEGIN_RING(chan, eng2d, mthd + 0x14, 1);
		OUT_RING  (chan, (uint32_t)exaGetPixmapPitch(ppix));
	} else {
		BEGIN_RING(chan, eng2d, mthd, 5);
		OUT_RING  (chan, fmt);
		OUT_RING  (chan, 0);
		OUT_RING  (chan, bo->tile_mode << 4);
		OUT_RING  (chan, 1);
		OUT_RING  (chan, 0);
	}

	BEGIN_RING(chan, eng2d, mthd + 0x18, 4);
	OUT_RING  (chan, ppix->drawable.width);
	OUT_RING  (chan, ppix->drawable.height);
	if (OUT_RELOCh(chan, bo, delta, bo_flags) ||
	    OUT_RELOCl(chan, bo, delta, bo_flags))
		return FALSE;

	if (is_src == 0)
		NV50EXASetClip(ppix, 0, 0, ppix->drawable.width, ppix->drawable.height);

	return TRUE;
}

static void
NV50EXASetPattern(PixmapPtr pdpix, int col0, int col1, int pat0, int pat1)
{
	NV50EXA_LOCALS(pdpix);

	BEGIN_RING(chan, eng2d, NV50_2D_PATTERN_COLOR(0), 4);
	OUT_RING  (chan, col0);
	OUT_RING  (chan, col1);
	OUT_RING  (chan, pat0);
	OUT_RING  (chan, pat1);
}

static void
NV50EXASetROP(PixmapPtr pdpix, int alu, Pixel planemask)
{
	NV50EXA_LOCALS(pdpix);
	int rop;

	if (planemask != ~0)
		rop = NVROP[alu].copy_planemask;
	else
		rop = NVROP[alu].copy;

	BEGIN_RING(chan, eng2d, NV50_2D_OPERATION, 1);
	if (alu == GXcopy && EXA_PM_IS_SOLID(&pdpix->drawable, planemask)) {
		OUT_RING  (chan, NV50_2D_OPERATION_SRCCOPY);
		return;
	} else {
		OUT_RING  (chan, NV50_2D_OPERATION_SRCCOPY_PREMULT);
	}

	BEGIN_RING(chan, eng2d, NV50_2D_PATTERN_FORMAT, 2);
	switch (pdpix->drawable.depth) {
		case  8: OUT_RING  (chan, 3); break;
		case 15: OUT_RING  (chan, 1); break;
		case 16: OUT_RING  (chan, 0); break;
		case 24:
		case 32:
		default:
			 OUT_RING  (chan, 2);
			 break;
	}
	OUT_RING  (chan, 1);

	/* There are 16 alu's.
	 * 0-15: copy
	 * 16-31: copy_planemask
	 */

	if (!EXA_PM_IS_SOLID(&pdpix->drawable, planemask)) {
		alu += 16;
		NV50EXASetPattern(pdpix, 0, planemask, ~0, ~0);
	} else {
		if (pNv->currentRop > 15)
			NV50EXASetPattern(pdpix, ~0, ~0, ~0, ~0);
	}

	if (pNv->currentRop != alu) {
		BEGIN_RING(chan, eng2d, NV50_2D_ROP, 1);
		OUT_RING  (chan, rop);
		pNv->currentRop = alu;
	}
}

static void
NV50EXAStateSolidResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);

	NV50EXAPrepareSolid(pNv->pdpix, pNv->alu, pNv->planemask,
			    pNv->fg_colour);
}

Bool
NV50EXAPrepareSolid(PixmapPtr pdpix, int alu, Pixel planemask, Pixel fg)
{
	NV50EXA_LOCALS(pdpix);
	uint32_t fmt;

	if (!NV50EXA2DSurfaceFormat(pdpix, &fmt))
		NOUVEAU_FALLBACK("rect format\n");

	if (MARK_RING(chan, 64, 4))
		NOUVEAU_FALLBACK("ring space\n");

	if (!NV50EXAAcquireSurface2D(pdpix, 0)) {
		MARK_UNDO(chan);
		NOUVEAU_FALLBACK("dest pixmap\n");
	}

	NV50EXASetROP(pdpix, alu, planemask);

	BEGIN_RING(chan, eng2d, NV50_2D_DRAW_SHAPE, 3);
	OUT_RING  (chan, NV50_2D_DRAW_SHAPE_RECTANGLES);
	OUT_RING  (chan, fmt);
	OUT_RING  (chan, fg);

	pNv->pdpix = pdpix;
	pNv->alu = alu;
	pNv->planemask = planemask;
	pNv->fg_colour = fg;
	chan->flush_notify = NV50EXAStateSolidResubmit;
	return TRUE;
}

void
NV50EXASolid(PixmapPtr pdpix, int x1, int y1, int x2, int y2)
{
	NV50EXA_LOCALS(pdpix);

	WAIT_RING (chan, 5);
	BEGIN_RING(chan, eng2d, NV50_2D_DRAW_POINT32_X(0), 4);
	OUT_RING  (chan, x1);
	OUT_RING  (chan, y1);
	OUT_RING  (chan, x2);
	OUT_RING  (chan, y2);

	if((x2 - x1) * (y2 - y1) >= 512)
		FIRE_RING (chan);
}

void
NV50EXADoneSolid(PixmapPtr pdpix)
{
	NV50EXA_LOCALS(pdpix);

	chan->flush_notify = NULL;
}

static void
NV50EXAStateCopyResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);

	NV50EXAPrepareCopy(pNv->pspix, pNv->pdpix, 0, 0, pNv->alu,
			   pNv->planemask);
}

Bool
NV50EXAPrepareCopy(PixmapPtr pspix, PixmapPtr pdpix, int dx, int dy,
		   int alu, Pixel planemask)
{
	NV50EXA_LOCALS(pdpix);

	if (MARK_RING(chan, 64, 4))
		NOUVEAU_FALLBACK("ring space\n");

	if (!NV50EXAAcquireSurface2D(pspix, 1)) {
		MARK_UNDO(chan);
		NOUVEAU_FALLBACK("src pixmap\n");
	}

	if (!NV50EXAAcquireSurface2D(pdpix, 0)) {
		MARK_UNDO(chan);
		NOUVEAU_FALLBACK("dest pixmap\n");
	}

	NV50EXASetROP(pdpix, alu, planemask);

	pNv->pspix = pspix;
	pNv->pdpix = pdpix;
	pNv->alu = alu;
	pNv->planemask = planemask;
	chan->flush_notify = NV50EXAStateCopyResubmit;
	return TRUE;
}

void
NV50EXACopy(PixmapPtr pdpix, int srcX , int srcY,
			     int dstX , int dstY,
			     int width, int height)
{
	NV50EXA_LOCALS(pdpix);

	WAIT_RING (chan, 17);
	BEGIN_RING(chan, eng2d, 0x0110, 1);
	OUT_RING  (chan, 0);
	BEGIN_RING(chan, eng2d, 0x088c, 1);
	OUT_RING  (chan, 0);
	BEGIN_RING(chan, eng2d, NV50_2D_BLIT_DST_X, 12);
	OUT_RING  (chan, dstX);
	OUT_RING  (chan, dstY);
	OUT_RING  (chan, width);
	OUT_RING  (chan, height);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, 1);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, 1);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, srcX);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, srcY);

	if(width * height >= 512)
		FIRE_RING (chan);
}

void
NV50EXADoneCopy(PixmapPtr pdpix)
{
	NV50EXA_LOCALS(pdpix);

	chan->flush_notify = NULL;
}

static void
NV50EXAStateSIFCResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);
	
	if (MARK_RING(pNv->chan, 32, 2))
		return;

	if (NV50EXAAcquireSurface2D(pNv->pdpix, 0))
		MARK_UNDO(pNv->chan);
}

Bool
NV50EXAUploadSIFC(const char *src, int src_pitch,
		  PixmapPtr pdpix, int x, int y, int w, int h, int cpp)
{
	NV50EXA_LOCALS(pdpix);
	int line_dwords = (w * cpp + 3) / 4;
	uint32_t sifc_fmt;

	if (!NV50EXA2DSurfaceFormat(pdpix, &sifc_fmt))
		NOUVEAU_FALLBACK("hostdata format\n");

	if (MARK_RING(chan, 64, 2))
		return FALSE;

	if (!NV50EXAAcquireSurface2D(pdpix, 0)) {
		MARK_UNDO(chan);
		NOUVEAU_FALLBACK("dest pixmap\n");
	}

	/* If the pitch isn't aligned to a dword, then you can get corruption at the end of a line. */
	NV50EXASetClip(pdpix, x, y, w, h);

	BEGIN_RING(chan, eng2d, NV50_2D_OPERATION, 1);
	OUT_RING  (chan, NV50_2D_OPERATION_SRCCOPY);
	BEGIN_RING(chan, eng2d, NV50_2D_SIFC_BITMAP_ENABLE, 2);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, sifc_fmt);
	BEGIN_RING(chan, eng2d, NV50_2D_SIFC_WIDTH, 10);
	OUT_RING  (chan, (line_dwords * 4) / cpp);
	OUT_RING  (chan, h);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, 1);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, 1);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, x);
	OUT_RING  (chan, 0);
	OUT_RING  (chan, y);

	pNv->pdpix = pdpix;
	chan->flush_notify = NV50EXAStateSIFCResubmit;

	while (h--) {
		int count = line_dwords;
		const char *p = src;

		while(count) {
			int size = count > 1792 ? 1792 : count;

			WAIT_RING (chan, size + 1);
			BEGIN_RING(chan, eng2d,
					 NV50_2D_SIFC_DATA | 0x40000000, size);
			OUT_RINGp (chan, p, size);

			p += size * 4;
			count -= size;
		}

		src += src_pitch;
	}

	chan->flush_notify = NULL;
	return TRUE;
}

static Bool
NV50EXACheckRenderTarget(PicturePtr ppict)
{
	if (ppict->pDrawable->width > 8192 ||
	    ppict->pDrawable->height > 8192)
		NOUVEAU_FALLBACK("render target dimensions exceeded %dx%d\n",
				 ppict->pDrawable->width,
				 ppict->pDrawable->height);

	switch (ppict->format) {
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 2, 10, 10, 10):
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 0, 10, 10, 10):
	case PICT_a8r8g8b8:
	case PICT_x8r8g8b8:
	case PICT_r5g6b5:
	case PICT_a8:
		break;
	default:
		NOUVEAU_FALLBACK("picture format 0x%08x\n", ppict->format);
	}

	return TRUE;
}

static Bool
NV50EXARenderTarget(PixmapPtr ppix, PicturePtr ppict)
{
	NV50EXA_LOCALS(ppix);
	struct nouveau_bo *bo = nouveau_pixmap_bo(ppix);
	unsigned delta = nouveau_pixmap_offset(ppix);
	unsigned format;

	/*XXX: Scanout buffer not tiled, someone needs to figure it out */
	if (!nv50_style_tiled_pixmap(ppix))
		NOUVEAU_FALLBACK("pixmap is scanout buffer\n");

	switch (ppict->format) {
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 2, 10, 10, 10):
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 0, 10, 10, 10):
		format = NV50TCL_RT_FORMAT_A2B10G10R10_UNORM;
		break;
	case PICT_a8r8g8b8: format = NV50TCL_RT_FORMAT_A8R8G8B8_UNORM; break;
	case PICT_x8r8g8b8: format = NV50TCL_RT_FORMAT_X8R8G8B8_UNORM; break;
	case PICT_r5g6b5  : format = NV50TCL_RT_FORMAT_R5G6B5_UNORM; break;
	case PICT_a8      : format = NV50TCL_RT_FORMAT_A8_UNORM; break;
	default:
		NOUVEAU_FALLBACK("invalid picture format\n");
	}

	BEGIN_RING(chan, tesla, NV50TCL_RT_ADDRESS_HIGH(0), 5);
	if (OUT_RELOCh(chan, bo, delta, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR) ||
	    OUT_RELOCl(chan, bo, delta, NOUVEAU_BO_VRAM | NOUVEAU_BO_WR))
		return FALSE;
	OUT_RING  (chan, format);
	OUT_RING  (chan, bo->tile_mode << 4);
	OUT_RING  (chan, 0x00000000);
	BEGIN_RING(chan, tesla, NV50TCL_RT_HORIZ(0), 2);
	OUT_RING  (chan, ppix->drawable.width);
	OUT_RING  (chan, ppix->drawable.height);
	BEGIN_RING(chan, tesla, NV50TCL_RT_ARRAY_MODE, 1);
	OUT_RING  (chan, 0x00000001);

	return TRUE;
}

static Bool
NV50EXACheckTexture(PicturePtr ppict, PicturePtr pdpict, int op)
{
	if (!ppict->pDrawable)
		NOUVEAU_FALLBACK("Solid and gradient pictures unsupported\n");

	if (ppict->pDrawable->width > 8192 ||
	    ppict->pDrawable->height > 8192)
		NOUVEAU_FALLBACK("texture dimensions exceeded %dx%d\n",
				 ppict->pDrawable->width,
				 ppict->pDrawable->height);

	switch (ppict->format) {
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 2, 10, 10, 10):
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 0, 10, 10, 10):
	case PICT_a8r8g8b8:
	case PICT_a8b8g8r8:
	case PICT_x8r8g8b8:
	case PICT_x8b8g8r8:
	case PICT_r5g6b5:
	case PICT_a8:
		break;
	default:
		NOUVEAU_FALLBACK("picture format 0x%08x\n", ppict->format);
	}

	switch (ppict->filter) {
	case PictFilterNearest:
	case PictFilterBilinear:
		break;
	default:
		NOUVEAU_FALLBACK("picture filter %d\n", ppict->filter);
	}

	/* Opengl and Render disagree on what should be sampled outside an XRGB 
	 * texture (with no repeating). Opengl has a hardcoded alpha value of 
	 * 1.0, while render expects 0.0. We assume that clipping is done for 
	 * untranformed sources.
	 */
	if (NV50EXABlendOp[op].src_alpha && !ppict->repeat &&
		ppict->transform && (PICT_FORMAT_A(ppict->format) == 0)
		&& (PICT_FORMAT_A(pdpict->format) != 0))
		NOUVEAU_FALLBACK("REPEAT_NONE unsupported for XRGB source\n");

	return TRUE;
}

static Bool
NV50EXATexture(PixmapPtr ppix, PicturePtr ppict, unsigned unit)
{
	NV50EXA_LOCALS(ppix);
	struct nouveau_bo *bo = nouveau_pixmap_bo(ppix);
	unsigned delta = nouveau_pixmap_offset(ppix);
	const unsigned tcb_flags = NOUVEAU_BO_RDWR | NOUVEAU_BO_VRAM;

	/*XXX: Scanout buffer not tiled, someone needs to figure it out */
	if (!nv50_style_tiled_pixmap(ppix))
		NOUVEAU_FALLBACK("pixmap is scanout buffer\n");

	BEGIN_RING(chan, tesla, NV50TCL_TIC_ADDRESS_HIGH, 3);
	if (OUT_RELOCh(chan, pNv->tesla_scratch, TIC_OFFSET, tcb_flags) ||
	    OUT_RELOCl(chan, pNv->tesla_scratch, TIC_OFFSET, tcb_flags))
		return FALSE;
	OUT_RING  (chan, 0x00000800);
	BEGIN_RING(chan, tesla, NV50TCL_CB_DEF_ADDRESS_HIGH, 3);
	if (OUT_RELOCh(chan, pNv->tesla_scratch, TIC_OFFSET, tcb_flags) ||
	    OUT_RELOCl(chan, pNv->tesla_scratch, TIC_OFFSET, tcb_flags))
		return FALSE;
	OUT_RING  (chan, (CB_TIC << NV50TCL_CB_DEF_SET_BUFFER_SHIFT) | 0x4000);
	BEGIN_RING(chan, tesla, NV50TCL_CB_ADDR, 1);
	OUT_RING  (chan, CB_TIC | ((unit * 8) << NV50TCL_CB_ADDR_ID_SHIFT));
	BEGIN_RING(chan, tesla, NV50TCL_CB_DATA(0) | 0x40000000, 8);
	switch (ppict->format) {
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 2, 10, 10, 10):
		OUT_RING  (chan, NV50TIC_0_0_MAPA_C3 | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C2 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C0 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_2_10_10_10);
		break;
	case PICT_FORMAT(32, PICT_TYPE_ABGR, 0, 10, 10, 10):
		OUT_RING  (chan, NV50TIC_0_0_MAPA_ONE | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C2 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C0 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_2_10_10_10);
		break;
	case PICT_a8r8g8b8:
		OUT_RING  (chan, NV50TIC_0_0_MAPA_C3 | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C0 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C2 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_8_8_8_8);
		break;
	case PICT_a8b8g8r8:
		OUT_RING  (chan, NV50TIC_0_0_MAPA_C3 | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C2 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C0 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_8_8_8_8);
		break;
	case PICT_x8r8g8b8:
		OUT_RING  (chan, NV50TIC_0_0_MAPA_ONE | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C0 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C2 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_8_8_8_8);
		break;
	case PICT_x8b8g8r8:
		OUT_RING  (chan, NV50TIC_0_0_MAPA_ONE | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C2 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C0 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_8_8_8_8);
		break;
	case PICT_r5g6b5:
		OUT_RING  (chan, NV50TIC_0_0_MAPA_ONE | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_C0 | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_C1 | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_C2 | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_5_6_5);
		break;
	case PICT_a8:
		OUT_RING  (chan, NV50TIC_0_0_MAPA_C0 | NV50TIC_0_0_TYPEA_UNORM |
			 NV50TIC_0_0_MAPR_ZERO | NV50TIC_0_0_TYPER_UNORM |
			 NV50TIC_0_0_MAPG_ZERO | NV50TIC_0_0_TYPEB_UNORM |
			 NV50TIC_0_0_MAPB_ZERO | NV50TIC_0_0_TYPEG_UNORM |
			 NV50TIC_0_0_FMT_8);
		break;
	default:
		NOUVEAU_FALLBACK("invalid picture format, this SHOULD NOT HAPPEN. Expect trouble.\n");
	}
	if (OUT_RELOCl(chan, bo, delta, NOUVEAU_BO_VRAM | NOUVEAU_BO_RD))
		return FALSE;
	OUT_RING  (chan, 0xd0005000 | (bo->tile_mode << 22));
	OUT_RING  (chan, 0x00300000);
	OUT_RING  (chan, ppix->drawable.width);
	OUT_RING  (chan, (1 << NV50TIC_0_5_DEPTH_SHIFT) | ppix->drawable.height);
	OUT_RING  (chan, 0x03000000);
	if (OUT_RELOCh(chan, bo, delta, NOUVEAU_BO_VRAM | NOUVEAU_BO_RD))
		return FALSE;

	BEGIN_RING(chan, tesla, NV50TCL_TSC_ADDRESS_HIGH, 3);
	if (OUT_RELOCh(chan, pNv->tesla_scratch, TSC_OFFSET, tcb_flags) ||
	    OUT_RELOCl(chan, pNv->tesla_scratch, TSC_OFFSET, tcb_flags))
		return FALSE;
	OUT_RING  (chan, 0x00000000);
	BEGIN_RING(chan, tesla, NV50TCL_CB_DEF_ADDRESS_HIGH, 3);
	if (OUT_RELOCh(chan, pNv->tesla_scratch, TSC_OFFSET, tcb_flags) ||
	    OUT_RELOCl(chan, pNv->tesla_scratch, TSC_OFFSET, tcb_flags))
		return FALSE;
	OUT_RING  (chan, (CB_TSC << NV50TCL_CB_DEF_SET_BUFFER_SHIFT) | 0x4000);
	BEGIN_RING(chan, tesla, NV50TCL_CB_ADDR, 1);
	OUT_RING  (chan, CB_TSC | ((unit * 8) << NV50TCL_CB_ADDR_ID_SHIFT));
	BEGIN_RING(chan, tesla, NV50TCL_CB_DATA(0) | 0x40000000, 8);
	if (ppict->repeat) {
		switch (ppict->repeatType) {
		case RepeatPad:
			OUT_RING  (chan, NV50TSC_1_0_WRAPS_CLAMP |
				 NV50TSC_1_0_WRAPT_CLAMP |
				 NV50TSC_1_0_WRAPR_CLAMP | 0x00024000);
			break;
		case RepeatReflect:
			OUT_RING  (chan, NV50TSC_1_0_WRAPS_MIRROR_REPEAT |
				 NV50TSC_1_0_WRAPT_MIRROR_REPEAT |
				 NV50TSC_1_0_WRAPR_MIRROR_REPEAT | 0x00024000);
			break;
		case RepeatNormal:
		default:
			OUT_RING  (chan, NV50TSC_1_0_WRAPS_REPEAT |
				 NV50TSC_1_0_WRAPT_REPEAT |
				 NV50TSC_1_0_WRAPR_REPEAT | 0x00024000);
			break;
		}
	} else {
		OUT_RING  (chan, NV50TSC_1_0_WRAPS_CLAMP_TO_BORDER |
			 NV50TSC_1_0_WRAPT_CLAMP_TO_BORDER |
			 NV50TSC_1_0_WRAPR_CLAMP_TO_BORDER | 0x00024000);
	}
	if (ppict->filter == PictFilterBilinear) {
		OUT_RING  (chan, NV50TSC_1_1_MAGF_LINEAR |
			 NV50TSC_1_1_MINF_LINEAR |
			 NV50TSC_1_1_MIPF_NONE);
	} else {
		OUT_RING  (chan, NV50TSC_1_1_MAGF_NEAREST |
			 NV50TSC_1_1_MINF_NEAREST |
			 NV50TSC_1_1_MIPF_NONE);
	}
	OUT_RING  (chan, 0x00000000);
	OUT_RING  (chan, 0x00000000);
	OUT_RING  (chan, 0x00000000);
	OUT_RING  (chan, 0x00000000);
	OUT_RING  (chan, 0x00000000);
	OUT_RING  (chan, 0x00000000);

	state->unit[unit].width = ppix->drawable.width;
	state->unit[unit].height = ppix->drawable.height;
	state->unit[unit].transform = ppict->transform;
	return TRUE;
}

static Bool
NV50EXACheckBlend(int op)
{
	if (op > PictOpAdd)
		NOUVEAU_FALLBACK("unsupported blend op %d\n", op);
	return TRUE;
}

static void
NV50EXABlend(PixmapPtr ppix, PicturePtr ppict, int op, int component_alpha)
{
	NV50EXA_LOCALS(ppix);
	struct nv50_blend_op *b = &NV50EXABlendOp[op];
	unsigned sblend = b->src_blend;
	unsigned dblend = b->dst_blend;

	if (b->dst_alpha) {
		if (!PICT_FORMAT_A(ppict->format)) {
			if (sblend == BF(DST_ALPHA))
				sblend = BF(ONE);
			else
			if (sblend == BF(ONE_MINUS_DST_ALPHA))
				sblend = BF(ZERO);
		}
	}

	if (b->src_alpha && component_alpha) {
		if (dblend == BF(SRC_ALPHA))
			dblend = BF(SRC_COLOR);
		else
		if (dblend == BF(ONE_MINUS_SRC_ALPHA))
			dblend = BF(ONE_MINUS_SRC_COLOR);
	}

	if (sblend == BF(ONE) && dblend == BF(ZERO)) {
		BEGIN_RING(chan, tesla, NV50TCL_BLEND_ENABLE(0), 1);
		OUT_RING  (chan, 0);
	} else {
		BEGIN_RING(chan, tesla, NV50TCL_BLEND_ENABLE(0), 1);
		OUT_RING  (chan, 1);
		BEGIN_RING(chan, tesla, NV50TCL_BLEND_EQUATION_RGB, 5);
		OUT_RING  (chan, NV50TCL_BLEND_EQUATION_RGB_FUNC_ADD);
		OUT_RING  (chan, sblend);
		OUT_RING  (chan, dblend);
		OUT_RING  (chan, NV50TCL_BLEND_EQUATION_ALPHA_FUNC_ADD);
		OUT_RING  (chan, sblend);
		BEGIN_RING(chan, tesla, NV50TCL_BLEND_FUNC_DST_ALPHA, 1);
		OUT_RING  (chan, dblend);
	}
}

Bool
NV50EXACheckComposite(int op,
		      PicturePtr pspict, PicturePtr pmpict, PicturePtr pdpict)
{
	if (!NV50EXACheckBlend(op))
		NOUVEAU_FALLBACK("blend not supported\n");

	if (!NV50EXACheckRenderTarget(pdpict))
		NOUVEAU_FALLBACK("render target invalid\n");

	if (!NV50EXACheckTexture(pspict, pdpict, op))
		NOUVEAU_FALLBACK("src picture invalid\n");

	if (pmpict) {
		if (pmpict->componentAlpha &&
		    PICT_FORMAT_RGB(pmpict->format) &&
		    NV50EXABlendOp[op].src_alpha &&
		    NV50EXABlendOp[op].src_blend != BF(ZERO))
			NOUVEAU_FALLBACK("component-alpha not supported\n");

		if (!NV50EXACheckTexture(pmpict, pdpict, op))
			NOUVEAU_FALLBACK("mask picture invalid\n");
	}

	return TRUE;
}

static void
NV50EXAStateCompositeResubmit(struct nouveau_channel *chan)
{
	ScrnInfoPtr pScrn = chan->user_private;
	NVPtr pNv = NVPTR(pScrn);

	NV50EXAPrepareComposite(pNv->alu, pNv->pspict, pNv->pmpict, pNv->pdpict,
				pNv->pspix, pNv->pmpix, pNv->pdpix);
}

Bool
NV50EXAPrepareComposite(int op,
			PicturePtr pspict, PicturePtr pmpict, PicturePtr pdpict,
			PixmapPtr pspix, PixmapPtr pmpix, PixmapPtr pdpix)
{
	NV50EXA_LOCALS(pspix);
	const unsigned shd_flags = NOUVEAU_BO_VRAM | NOUVEAU_BO_RD;

	if (MARK_RING (chan, 128, 4 + 2 + 2 * 10))
		NOUVEAU_FALLBACK("ring space\n");

	BEGIN_RING(chan, eng2d, 0x0110, 1);
	OUT_RING  (chan, 0);

	if (!NV50EXARenderTarget(pdpix, pdpict)) {
		MARK_UNDO(chan);
		NOUVEAU_FALLBACK("render target invalid\n");
	}

	NV50EXABlend(pdpix, pdpict, op, pmpict && pmpict->componentAlpha &&
		     PICT_FORMAT_RGB(pmpict->format));

	BEGIN_RING(chan, tesla, NV50TCL_VP_ADDRESS_HIGH, 2);
	if (OUT_RELOCh(chan, pNv->tesla_scratch, PVP_OFFSET, shd_flags) ||
	    OUT_RELOCl(chan, pNv->tesla_scratch, PVP_OFFSET, shd_flags)) {
		MARK_UNDO(chan);
		return FALSE;
	}

	BEGIN_RING(chan, tesla, NV50TCL_FP_ADDRESS_HIGH, 2);
	if (OUT_RELOCh(chan, pNv->tesla_scratch, PFP_OFFSET, shd_flags) ||
	    OUT_RELOCl(chan, pNv->tesla_scratch, PFP_OFFSET, shd_flags)) {
		MARK_UNDO(chan);
		return FALSE;
	}

	if (!NV50EXATexture(pspix, pspict, 0)) {
		MARK_UNDO(chan);
		NOUVEAU_FALLBACK("src picture invalid\n");
	}

	if (pmpict) {
		if (!NV50EXATexture(pmpix, pmpict, 1)) {
			MARK_UNDO(chan);
			NOUVEAU_FALLBACK("mask picture invalid\n");
		}
		state->have_mask = TRUE;

		BEGIN_RING(chan, tesla, NV50TCL_FP_START_ID, 1);
		if (pdpict->format == PICT_a8) {
			OUT_RING  (chan, PFP_C_A8);
		} else {
			if (pmpict->componentAlpha &&
			    PICT_FORMAT_RGB(pmpict->format)) {
				if (NV50EXABlendOp[op].src_alpha)
					OUT_RING  (chan, PFP_CCASA);
				else
					OUT_RING  (chan, PFP_CCA);
			} else {
				OUT_RING  (chan, PFP_C);
			}
		}
	} else {
		state->have_mask = FALSE;

		BEGIN_RING(chan, tesla, NV50TCL_FP_START_ID, 1);
		if (pdpict->format == PICT_a8)
			OUT_RING  (chan, PFP_S_A8);
		else
			OUT_RING  (chan, PFP_S);
	}

	BEGIN_RING(chan, tesla, 0x1334, 1);
	OUT_RING  (chan, 0);

	BEGIN_RING(chan, tesla, NV50TCL_BIND_TIC(2), 1);
	OUT_RING  (chan, 1);
	BEGIN_RING(chan, tesla, NV50TCL_BIND_TIC(2), 1);
	OUT_RING  (chan, 0x203);

	pNv->alu = op;
	pNv->pspict = pspict;
	pNv->pmpict = pmpict;
	pNv->pdpict = pdpict;
	pNv->pspix = pspix;
	pNv->pmpix = pmpix;
	pNv->pdpix = pdpix;
	chan->flush_notify = NV50EXAStateCompositeResubmit;
	return TRUE;
}

#define xFixedToFloat(v) \
	((float)xFixedToInt((v)) + ((float)xFixedFrac(v) / 65536.0))
static inline void
NV50EXATransform(PictTransformPtr t, int x, int y, float sx, float sy,
		 float *x_ret, float *y_ret)
{
	if (t) {
		PictVector v;

		v.vector[0] = IntToxFixed(x);
		v.vector[1] = IntToxFixed(y);
		v.vector[2] = xFixed1;
		PictureTransformPoint(t, &v);
		*x_ret = xFixedToFloat(v.vector[0]) / sx;
		*y_ret = xFixedToFloat(v.vector[1]) / sy;
	} else {
		*x_ret = (float)x / sx;
		*y_ret = (float)y / sy;
	}
}

void
NV50EXAComposite(PixmapPtr pdpix, int sx, int sy, int mx, int my,
		 int dx, int dy, int w, int h)
{
	NV50EXA_LOCALS(pdpix);
	float sX0, sX1, sX2, sY0, sY1, sY2;

	WAIT_RING (chan, 64);
	BEGIN_RING(chan, tesla, NV50TCL_SCISSOR_HORIZ(0), 2);
	OUT_RING  (chan, (dx + w) << 16 | dx);
	OUT_RING  (chan, (dy + h) << 16 | dy);
	BEGIN_RING(chan, tesla, NV50TCL_VERTEX_BEGIN, 1);
	OUT_RING  (chan, NV50TCL_VERTEX_BEGIN_TRIANGLES);

	NV50EXATransform(state->unit[0].transform, sx, sy + (h * 2),
			 state->unit[0].width, state->unit[0].height,
			 &sX0, &sY0);
	NV50EXATransform(state->unit[0].transform, sx, sy,
			 state->unit[0].width, state->unit[0].height,
			 &sX1, &sY1);
	NV50EXATransform(state->unit[0].transform, sx + (w * 2), sy,
			 state->unit[0].width, state->unit[0].height,
			 &sX2, &sY2);

	if (state->have_mask) {
		float mX0, mX1, mX2, mY0, mY1, mY2;

		NV50EXATransform(state->unit[1].transform, mx, my + (h * 2),
				 state->unit[1].width, state->unit[1].height,
				 &mX0, &mY0);
		NV50EXATransform(state->unit[1].transform, mx, my,
				 state->unit[1].width, state->unit[1].height,
				 &mX1, &mY1);
		NV50EXATransform(state->unit[1].transform, mx + (w * 2), my,
				 state->unit[1].width, state->unit[1].height,
				 &mX2, &mY2);

		VTX2s(pNv, sX0, sY0, mX0, mY0, dx, dy + (h * 2));
		VTX2s(pNv, sX1, sY1, mX1, mY1, dx, dy);
		VTX2s(pNv, sX2, sY2, mX2, mY2, dx + (w * 2), dy);
	} else {
		VTX1s(pNv, sX0, sY0, dx, dy + (h * 2));
		VTX1s(pNv, sX1, sY1, dx, dy);
		VTX1s(pNv, sX2, sY2, dx + (w * 2), dy);
	}

	BEGIN_RING(chan, tesla, NV50TCL_VERTEX_END, 1);
	OUT_RING  (chan, 0);
}

void
NV50EXADoneComposite(PixmapPtr pdpix)
{
	NV50EXA_LOCALS(pdpix);

	chan->flush_notify = NULL;
}

