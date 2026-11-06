// librw PSP backend: driver registration, native rasters and native textures.
// This file has no GE dependencies so it can be built into PC tools
// (e.g. the texture converter) as well.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwpsp.h"

#if defined(RW_PSP) && !defined(N3DS)
#include <pspkernel.h>
#include <pspgu.h>
#include <pspge.h>
#endif

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace psp {

int32 nativeRasterOffset;
int32 maxTextureSize = 128;

static void*
alignedAlloc(uint32 size)
{
	uint8 *p = (uint8*)rwMalloc(size + 16 + sizeof(void*), MEMDUR_EVENT | ID_DRIVER);
	if(p == nil)
		return nil;
	uint8 *a = (uint8*)(((uintptr)p + sizeof(void*) + 15) & ~(uintptr)15);
	((void**)a)[-1] = p;
	return a;
}

static void
alignedFree(void *a)
{
	if(a)
		rwFree(((void**)a)[-1]);
}

static void
writebackRange(void *p, uint32 size)
{
#if defined(RW_PSP) && !defined(N3DS)
	if(p && size)
		sceKernelDcacheWritebackRange(p, size);
#endif
}

uint32
psmBitsPerPixel(int32 psm)
{
	switch(psm){
	case PSM_T4: return 4;
	case PSM_T8: return 8;
	case PSM_8888: return 32;
	default: return 16;
	}
}

static int32
nextPow2(int32 x)
{
	int32 p = 1;
	while(p < x) p <<= 1;
	return p;
}

// buffer width: rows must be at least 16 bytes and a multiple of 16 bytes
static int32
bufWidthFor(int32 width, int32 psm)
{
	uint32 bpp = psmBitsPerPixel(psm);
	int32 pixPer16 = 128/bpp;
	int32 bw = (width + pixPer16-1) & ~(pixPer16-1);
	return bw;
}

static uint32
levelBytes(int32 bufWidth, int32 height, int32 psm)
{
	// height padded to 8 rows so swizzled blocks are complete
	int32 h = (height + 7) & ~7;
	return bufWidth*h*psmBitsPerPixel(psm)/8;
}

// GE swizzle: 16 byte x 8 row blocks
void
swizzle(uint8 *out, const uint8 *in, uint32 widthBytes, uint32 height)
{
	uint32 blockx, blocky;
	uint32 widthBlocks = widthBytes/16;
	uint32 heightBlocks = height/8;
	uint32 srcPitch = (widthBytes-16)/4;
	uint32 srcRow = widthBytes*8;
	const uint8 *ysrc = in;
	uint32 *dst = (uint32*)out;
	for(blocky = 0; blocky < heightBlocks; blocky++){
		const uint8 *xsrc = ysrc;
		for(blockx = 0; blockx < widthBlocks; blockx++){
			const uint32 *src = (const uint32*)xsrc;
			for(uint32 j = 0; j < 8; j++){
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				src += srcPitch;
			}
			xsrc += 16;
		}
		ysrc += srcRow;
	}
}

void
unswizzle(uint8 *out, const uint8 *in, uint32 widthBytes, uint32 height)
{
	uint32 blockx, blocky;
	uint32 widthBlocks = widthBytes/16;
	uint32 heightBlocks = height/8;
	uint32 dstPitch = (widthBytes-16)/4;
	uint32 dstRow = widthBytes*8;
	uint8 *ydst = out;
	const uint32 *src = (const uint32*)in;
	for(blocky = 0; blocky < heightBlocks; blocky++){
		uint8 *xdst = ydst;
		for(blockx = 0; blockx < widthBlocks; blockx++){
			uint32 *dst = (uint32*)xdst;
			for(uint32 j = 0; j < 8; j++){
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				dst += dstPitch;
			}
			xdst += 16;
		}
		ydst += dstRow;
	}
}

static int32
psmFromRwFormat(int32 format)
{
	if(format & Raster::PAL4) return PSM_T4;
	if(format & Raster::PAL8) return PSM_T8;
	switch(format & 0xF00){
	case Raster::C565: return PSM_5650;
	case Raster::C1555:
	case Raster::C555: return PSM_5551;
	case Raster::C4444: return PSM_4444;
	default: return PSM_8888;
	}
}

static int32
rwFormatFromPsm(int32 psm)
{
	switch(psm){
	case PSM_5650: return Raster::C565;
	case PSM_5551: return Raster::C1555;
	case PSM_4444: return Raster::C4444;
	case PSM_T4: return Raster::PAL4 | Raster::C8888;
	case PSM_T8: return Raster::PAL8 | Raster::C8888;
	default: return Raster::C8888;
	}
}

static int32
depthFromPsm(int32 psm)
{
	switch(psm){
	case PSM_T4: return 4;
	case PSM_T8: return 8;
	case PSM_8888: return 32;
	default: return 16;
	}
}

// Allocate storage for all levels of a native raster
static bool32
allocateLevels(Raster *raster, PspRaster *natras, int32 width, int32 height, int32 numLevels)
{
	int32 psm = natras->psm;
	int32 w = nextPow2(width);
	int32 h = nextPow2(height);
	if(w > 512) w = 512;
	if(h > 512) h = 512;
	uint32 size = 0;
	int32 n;
	for(n = 0; n < numLevels && n < MAXLEVELS; n++){
		natras->levelWidth[n] = w;
		natras->levelHeight[n] = h;
		natras->levelBufWidth[n] = bufWidthFor(w, psm);
		natras->levelOffset[n] = size;
		size += levelBytes(natras->levelBufWidth[n], h, psm);
		size = (size+15) & ~15;
		if(w == 1 && h == 1){
			n++;
			break;
		}
		if(w > 1) w /= 2;
		if(h > 1) h /= 2;
	}
	natras->numLevels = n;
	natras->dataSize = size;
	natras->data = (uint8*)alignedAlloc(size);
	if(natras->data == nil)
		return 0;
	memset(natras->data, 0, size);
	natras->ownsData = 1;
	if(psm == PSM_T4 || psm == PSM_T8){
		int32 n = psm == PSM_T4 ? 16 : 256;
		natras->palette = (uint32*)alignedAlloc(n*4);
		memset(natras->palette, 0, n*4);
	}
	raster->stride = natras->levelBufWidth[0]*psmBitsPerPixel(psm)/8;
	return 1;
}

Raster*
rasterCreate(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	memset(natras, 0, sizeof(PspRaster));
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = 0;
	raster->originalPixels = nil;

	switch(raster->type){
	case Raster::ZBUFFER:
		raster->depth = 16;
		return raster;
	case Raster::CAMERA:
		// the frame buffer lives in VRAM and is managed by the device
		raster->depth = 16;
		raster->format = Raster::C565;
		raster->stride = 512*2;
		return raster;
	case Raster::CAMERATEXTURE:
		// No render-to-texture on PSP; give it a small blank texture
		natras->psm = PSM_5650;
		raster->format = Raster::C565;
		raster->depth = 16;
		if(raster->flags & Raster::DONTALLOCATE)
			return raster;
		if(!allocateLevels(raster, natras, 16, 16, 1))
			return nil;
		raster->originalStride = raster->stride;
		return raster;
	case Raster::NORMAL:
	case Raster::TEXTURE:
	default:
		break;
	}

	if(raster->format == 0)
		raster->format = raster->depth <= 8 ? (raster->depth == 4 ? Raster::PAL4|Raster::C8888 : Raster::PAL8|Raster::C8888) : Raster::C8888;
	natras->psm = psmFromRwFormat(raster->format);
	raster->depth = depthFromPsm(natras->psm);
	natras->hasAlpha = Raster::formatHasAlpha(raster->format) || (raster->format & (Raster::PAL4|Raster::PAL8));

	if(raster->flags & Raster::DONTALLOCATE)
		return raster;

	int32 numLevels = 1;
	if(raster->format & Raster::MIPMAP)
		numLevels = Raster::calculateNumLevels(raster->width, raster->height);
	if(!allocateLevels(raster, natras, raster->width, raster->height, numLevels))
		return nil;
	raster->originalStride = raster->stride;
	return raster;
}

// Locked pixels are presented unswizzled, one byte per pixel for
// palettized rasters and in GE layout (RGBA byte order for 8888) otherwise.
uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	if(natras->data == nil || level >= natras->numLevels)
		return nil;
	int32 w = natras->levelWidth[level];
	int32 h = natras->levelHeight[level];
	int32 bw = natras->levelBufWidth[level];
	int32 psm = natras->psm;
	int32 bpp = psmBitsPerPixel(psm);
	int32 outBpp = psm == PSM_T4 ? 8 : bpp;
	uint32 outStride = w*outBpp/8;
	uint8 *px = (uint8*)rwMalloc(outStride*h + 16, MEMDUR_EVENT | ID_DRIVER);
	if(px == nil)
		return nil;

	if((lockMode & Raster::LOCKREAD) || !(lockMode & Raster::LOCKNOFETCH)){
		uint32 srcStride = bw*bpp/8;
		uint8 *src = natras->data + natras->levelOffset[level];
		uint8 *tmp = nil;
		if(natras->swizzled){
			int32 hh = (h+7)&~7;
			tmp = (uint8*)rwMalloc(srcStride*hh, MEMDUR_FUNCTION | ID_DRIVER);
			unswizzle(tmp, src, srcStride, hh);
			src = tmp;
		}
		for(int32 y = 0; y < h; y++){
			uint8 *s = src + y*srcStride;
			uint8 *d = px + y*outStride;
			if(psm == PSM_T4){
				for(int32 x = 0; x < w; x++)
					d[x] = (s[x/2] >> ((x&1)*4)) & 0xF;
			}else
				memcpy(d, s, outStride);
		}
		if(tmp) rwFree(tmp);
	}

	raster->originalPixels = raster->pixels;
	raster->pixels = px;
	raster->width = w;
	raster->height = h;
	raster->stride = outStride;
	raster->privateFlags = lockMode;
	return px;
}

void
rasterUnlock(Raster *raster, int32 level)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	if(raster->pixels && (raster->privateFlags & Raster::LOCKWRITE) && level < natras->numLevels){
		int32 w = natras->levelWidth[level];
		int32 h = natras->levelHeight[level];
		int32 bw = natras->levelBufWidth[level];
		int32 psm = natras->psm;
		int32 bpp = psmBitsPerPixel(psm);
		uint32 dstStride = bw*bpp/8;
		int32 hh = (h+7)&~7;
		uint8 *dst = natras->data + natras->levelOffset[level];
		uint8 *tmp = (uint8*)rwMalloc(dstStride*hh, MEMDUR_FUNCTION | ID_DRIVER);
		memset(tmp, 0, dstStride*hh);
		for(int32 y = 0; y < h; y++){
			uint8 *s = raster->pixels + y*raster->stride;
			uint8 *d = tmp + y*dstStride;
			if(psm == PSM_T4){
				for(int32 x = 0; x < w; x += 2)
					d[x/2] = (s[x]&0xF) | ((x+1 < w ? s[x+1]&0xF : 0) << 4);
			}else
				memcpy(d, s, w*bpp/8);
		}
		if(natras->swizzled)
			swizzle(dst, tmp, dstStride, hh);
		else
			memcpy(dst, tmp, dstStride*hh);
		rwFree(tmp);
		writebackRange(dst, dstStride*hh);
#ifdef RW_PSP
		evictRaster(raster);
#endif
	}
	if(raster->pixels)
		rwFree(raster->pixels);
	raster->pixels = raster->originalPixels;
	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->privateFlags = 0;
}

uint8*
rasterLockPalette(Raster *raster, int32 lockMode)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	if(natras->palette == nil)
		return nil;
	raster->privateFlags |= lockMode<<3;	// PRIVATELOCK_*_PALETTE
	return (uint8*)natras->palette;
}

void
rasterUnlockPalette(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	if(natras->palette){
		writebackRange(natras->palette, natras->psm == PSM_T4 ? 64 : 1024);
#ifdef RW_PSP
		evictRaster(raster);
#endif
	}
	raster->privateFlags &= ~(Raster::PRIVATELOCK_READ_PALETTE|Raster::PRIVATELOCK_WRITE_PALETTE);
}

int32
rasterNumLevels(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	if(natras->data == nil)
		return 1;
	return natras->numLevels;
}

bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pWidth, int32 *pHeight, int32 *pDepth, int32 *pFormat)
{
	int32 width, height, depth, format;

	assert((type&0xF) == Raster::TEXTURE);

	width = img->width;
	height = img->height;
	depth = img->depth;
	if(depth <= 8){
		format = (depth == 4 ? Raster::PAL4 : Raster::PAL8) | Raster::C8888;
	}else{
		// keep it simple, 32 bit for runtime created textures
		format = Raster::C8888;
		depth = 32;
	}
	format |= type;

	*pWidth = width;
	*pHeight = height;
	*pDepth = depth;
	*pFormat = format;
	return 1;
}

// read one pixel of an image as RGBA
static void
imageGetRGBA(Image *img, int32 x, int32 y, uint8 *out)
{
	uint8 *p = img->pixels + y*img->stride + x*img->bpp;
	switch(img->depth){
	case 4:
	case 8:
		memcpy(out, &img->palette[p[0]*4], 4);
		break;
	case 16:{
		// ARGB1555 as used by librw images
		uint32 c = p[0] | p[1]<<8;
		out[0] = ((c>>10)&0x1F)*255/31;
		out[1] = ((c>>5)&0x1F)*255/31;
		out[2] = (c&0x1F)*255/31;
		out[3] = c&0x8000 ? 255 : 0;
		break;
	}
	case 24:
		out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
		break;
	case 32:
		out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
		break;
	default:
		out[0] = out[1] = out[2] = out[3] = 255;
	}
}

static void
packPixel(uint8 *d, int32 psm, const uint8 *c)
{
	uint32 v;
	switch(psm){
	case PSM_8888:
		d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3];
		break;
	case PSM_5650:
		v = (c[0]>>3) | (c[1]>>2)<<5 | (c[2]>>3)<<11;
		d[0] = v; d[1] = v>>8;
		break;
	case PSM_5551:
		v = (c[0]>>3) | (c[1]>>3)<<5 | (c[2]>>3)<<10 | (c[3] >= 128 ? 0x8000 : 0);
		d[0] = v; d[1] = v>>8;
		break;
	case PSM_4444:
		v = (c[0]>>4) | (c[1]>>4)<<4 | (c[2]>>4)<<8 | (c[3]>>4)<<12;
		d[0] = v; d[1] = v>>8;
		break;
	}
}

static void
unpackPixel(const uint8 *s, int32 psm, uint8 *c)
{
	uint32 v;
	switch(psm){
	case PSM_8888:
		c[0] = s[0]; c[1] = s[1]; c[2] = s[2]; c[3] = s[3];
		break;
	case PSM_5650:
		v = s[0] | s[1]<<8;
		c[0] = (v&0x1F)*255/31; c[1] = ((v>>5)&0x3F)*255/63; c[2] = ((v>>11)&0x1F)*255/31; c[3] = 255;
		break;
	case PSM_5551:
		v = s[0] | s[1]<<8;
		c[0] = (v&0x1F)*255/31; c[1] = ((v>>5)&0x1F)*255/31; c[2] = ((v>>10)&0x1F)*255/31; c[3] = v&0x8000 ? 255 : 0;
		break;
	case PSM_4444:
		v = s[0] | s[1]<<8;
		c[0] = (v&0xF)*17; c[1] = ((v>>4)&0xF)*17; c[2] = ((v>>8)&0xF)*17; c[3] = ((v>>12)&0xF)*17;
		break;
	}
}

// Write an image (already at the right size) into level n of a native raster.
// The image must be palettized for T4/T8 rasters.
static void
writeLevel(PspRaster *natras, int32 level, Image *img)
{
	int32 w = natras->levelWidth[level];
	int32 h = natras->levelHeight[level];
	int32 bw = natras->levelBufWidth[level];
	int32 psm = natras->psm;
	uint32 bpp = psmBitsPerPixel(psm);
	uint32 stride = bw*bpp/8;
	int32 hh = (h+7)&~7;
	uint8 *tmp = (uint8*)rwMalloc(stride*hh, MEMDUR_FUNCTION | ID_DRIVER);
	memset(tmp, 0, stride*hh);
	uint8 c[4];
	for(int32 y = 0; y < h; y++){
		uint8 *d = tmp + y*stride;
		int32 sy = y < img->height ? y : img->height-1;
		for(int32 x = 0; x < w; x++){
			int32 sx = x < img->width ? x : img->width-1;
			if(psm == PSM_T8)
				d[x] = img->pixels[sy*img->stride + sx];
			else if(psm == PSM_T4){
				uint8 idx = img->pixels[sy*img->stride + sx] & 0xF;
				if(x & 1) d[x/2] |= idx<<4;
				else d[x/2] = idx;
			}else{
				imageGetRGBA(img, sx, sy, c);
				packPixel(d + x*bpp/8, psm, c);
			}
		}
	}
	uint8 *dst = natras->data + natras->levelOffset[level];
	// swizzle when the level is big enough (whole 16x8 blocks)
	if(natras->swizzled)
		swizzle(dst, tmp, stride, hh);
	else
		memcpy(dst, tmp, stride*hh);
	rwFree(tmp);
}

static bool
canSwizzle(PspRaster *natras)
{
	// all levels must have at least 8 rows to keep it simple
	for(int i = 0; i < natras->numLevels; i++)
		if(natras->levelHeight[i] < 8)
			return false;
	return true;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	// raster is locked: write into the lock buffer, unlock converts it
	if(raster->pixels && (raster->privateFlags & Raster::LOCKWRITE)){
		int32 psm = natras->psm;
		uint32 bpp = psmBitsPerPixel(psm);
		uint8 c[4];
		for(int32 y = 0; y < raster->height; y++){
			uint8 *d = raster->pixels + y*raster->stride;
			int32 sy = y < image->height ? y : image->height-1;
			for(int32 x = 0; x < raster->width; x++){
				int32 sx = x < image->width ? x : image->width-1;
				if(psm == PSM_T4 || psm == PSM_T8){
					d[x] = image->depth <= 8 ? image->pixels[sy*image->stride + sx] : 0;
				}else{
					imageGetRGBA(image, sx, sy, c);
					packPixel(d + x*bpp/8, psm, c);
				}
			}
		}
		if((psm == PSM_T4 || psm == PSM_T8) && image->depth <= 8){
			int32 n = psm == PSM_T4 ? 16 : 256;
			memcpy(natras->palette, image->palette, (1<<image->depth) < n ? (1<<image->depth)*4 : n*4);
			writebackRange(natras->palette, n*4);
		}
		return 1;
	}
	if((raster->type&0xF) != Raster::TEXTURE && (raster->type&0xF) != Raster::NORMAL)
		return 0;
	if(natras->data == nil){
		int32 numLevels = 1;
		if(!allocateLevels(raster, natras, raster->width, raster->height, numLevels))
			return 0;
		raster->originalStride = raster->stride;
	}
	Image *img = image;
	bool32 pal = natras->psm == PSM_T4 || natras->psm == PSM_T8;
	if(pal && image->depth > 8){
		// shouldn't really happen, fall back to quantizing
		img = Image::create(image->width, image->height, image->depth);
		img->allocate();
		for(int y = 0; y < image->height; y++)
			memcpy(img->pixels + y*img->stride, image->pixels + y*image->stride, image->width*image->bpp);
		img->palettize(natras->psm == PSM_T4 ? 4 : 8);
	}
	if(pal){
		int32 n = natras->psm == PSM_T4 ? 16 : 256;
		memset(natras->palette, 0, n*4);
		memcpy(natras->palette, img->palette, (1<<img->depth)*4 > n*4 ? n*4 : (1<<img->depth)*4);
		natras->hasAlpha = 0;
		for(int32 i = 0; i < n; i++)
			if((natras->palette[i]>>24) != 0xFF){
				natras->hasAlpha = 1;
				break;
			}
		writebackRange(natras->palette, n*4);
	}else
		natras->hasAlpha = (natras->psm != PSM_5650) && image->hasAlpha();
	natras->swizzled = 0;	// runtime rasters may be locked, keep linear
	writeLevel(natras, 0, img);
	writebackRange(natras->data, natras->dataSize);
	if(img != image)
		img->destroy();
#ifdef RW_PSP
	evictRaster(raster);
#endif
	return 1;
}

Image*
rasterToImage(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
#ifdef RW_PSP
	if(raster->type == Raster::CAMERA)
		return grabFrameBuffer();
#endif
	if(natras->data == nil)
		return nil;
	int32 psm = natras->psm;
	int32 w = natras->levelWidth[0];
	int32 h = natras->levelHeight[0];
	int32 bw = natras->levelBufWidth[0];
	uint32 bpp = psmBitsPerPixel(psm);
	uint32 srcStride = bw*bpp/8;
	int32 hh = (h+7)&~7;
	uint8 *src = natras->data;
	uint8 *tmp = nil;
	if(natras->swizzled){
		tmp = (uint8*)rwMalloc(srcStride*hh, MEMDUR_FUNCTION | ID_DRIVER);
		unswizzle(tmp, src, srcStride, hh);
		src = tmp;
	}
	Image *img;
	if(psm == PSM_T4 || psm == PSM_T8){
		img = Image::create(w, h, psm == PSM_T4 ? 4 : 8);
		img->allocate();
		for(int32 y = 0; y < h; y++){
			uint8 *s = src + y*srcStride;
			uint8 *d = img->pixels + y*img->stride;
			for(int32 x = 0; x < w; x++)
				d[x] = psm == PSM_T8 ? s[x] : (s[x/2] >> ((x&1)*4)) & 0xF;
		}
		memcpy(img->palette, natras->palette, (psm == PSM_T4 ? 16 : 256)*4);
	}else{
		img = Image::create(w, h, 32);
		img->allocate();
		for(int32 y = 0; y < h; y++){
			uint8 *s = src + y*srcStride;
			uint8 *d = img->pixels + y*img->stride;
			for(int32 x = 0; x < w; x++)
				unpackPixel(s + x*bpp/8, psm, d + x*4);
		}
	}
	if(tmp) rwFree(tmp);
	return img;
}

//
// Image helpers for conversion
//

static Image*
copyImage(Image *src)
{
	Image *img = Image::create(src->width, src->height, src->depth);
	img->allocate();
	for(int32 y = 0; y < src->height; y++)
		memcpy(img->pixels + y*img->stride, src->pixels + y*src->stride, src->width*src->bpp);
	if(src->depth <= 8)
		memcpy(img->palette, src->palette, (1<<src->depth)*4);
	return img;
}

// make a 32 bit copy of any image
static Image*
toRGBA(Image *src)
{
	Image *img = Image::create(src->width, src->height, 32);
	img->allocate();
	for(int32 y = 0; y < src->height; y++)
		for(int32 x = 0; x < src->width; x++)
			imageGetRGBA(src, x, y, img->pixels + y*img->stride + x*4);
	return img;
}

// Resize to (w,h). Box filter for truecolour, nearest for palettized.
static Image*
resizeImage(Image *src, int32 w, int32 h)
{
	Image *img = Image::create(w, h, src->depth);
	img->allocate();
	if(src->depth <= 8){
		memcpy(img->palette, src->palette, (1<<src->depth)*4);
		for(int32 y = 0; y < h; y++){
			int32 sy = y*src->height/h;
			for(int32 x = 0; x < w; x++){
				int32 sx = x*src->width/w;
				img->pixels[y*img->stride + x] = src->pixels[sy*src->stride + sx];
			}
		}
		return img;
	}
	assert(src->depth == 32);
	for(int32 y = 0; y < h; y++){
		int32 y0 = y*src->height/h;
		int32 y1 = (y+1)*src->height/h;
		if(y1 <= y0) y1 = y0+1;
		for(int32 x = 0; x < w; x++){
			int32 x0 = x*src->width/w;
			int32 x1 = (x+1)*src->width/w;
			if(x1 <= x0) x1 = x0+1;
			uint32 sum[4] = { 0, 0, 0, 0 };
			uint32 n = 0;
			for(int32 sy = y0; sy < y1; sy++)
				for(int32 sx = x0; sx < x1; sx++){
					uint8 *p = src->pixels + sy*src->stride + sx*4;
					sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
					n++;
				}
			uint8 *d = img->pixels + y*img->stride + x*4;
			d[0] = sum[0]/n; d[1] = sum[1]/n; d[2] = sum[2]/n; d[3] = sum[3]/n;
		}
	}
	return img;
}

static int32
countColours(Image *img, int32 limit)
{
	uint32 cols[256];
	int32 n = 0;
	for(int32 y = 0; y < img->height; y++)
		for(int32 x = 0; x < img->width; x++){
			uint32 c = *(uint32*)(img->pixels + y*img->stride + x*4);
			int32 i;
			for(i = 0; i < n; i++)
				if(cols[i] == c) break;
			if(i == n){
				if(n == limit)
					return limit+1;
				cols[n++] = c;
			}
		}
	return n;
}

Raster*
rasterFromImageEx(Image *image, int32 maxSize, bool32 palettize, int32 numLevels)
{
	Image *img;
	int32 w = image->width, h = image->height;

	// power of two, clamped
	int32 pw = nextPow2(w), ph = nextPow2(h);
	if(pw != w) pw = pw/2 >= 1 && pw/2 >= w*3/4 ? pw/2 : pw;
	if(ph != h) ph = ph/2 >= 1 && ph/2 >= h*3/4 ? ph/2 : ph;
	if(maxSize > 512) maxSize = 512;
	while(pw > maxSize || ph > maxSize){
		if(pw > 1) pw /= 2;
		if(ph > 1) ph /= 2;
	}

	if(image->depth <= 8)
		img = copyImage(image);
	else
		img = toRGBA(image);
	if(img->width != pw || img->height != ph){
		Image *r = resizeImage(img, pw, ph);
		img->destroy();
		img = r;
	}

	// palettize 2: force 16 colours (half the memory of 8 bit), for the PSP-1000
	if(palettize == 2 && img->depth == 8){
		Image *rgba = toRGBA(img);
		img->destroy();
		img = rgba;
	}

	int32 psm;
	if(img->depth == 4)
		psm = PSM_T4;
	else if(img->depth == 8)
		psm = PSM_T8;
	else if(palettize){
		int32 ncol = countColours(img, 16);
		img->palettize(ncol <= 16 || palettize == 2 ? 4 : 8);
		psm = img->depth == 4 ? PSM_T4 : PSM_T8;
	}else{
		// pick a 16 bit format
		bool hasAlpha = false, binaryAlpha = true;
		for(int32 y = 0; y < img->height; y++)
			for(int32 x = 0; x < img->width; x++){
				uint8 a = img->pixels[y*img->stride + x*4 + 3];
				if(a != 255) hasAlpha = true;
				if(a != 0 && a != 255) binaryAlpha = false;
			}
		psm = !hasAlpha ? PSM_5650 : binaryAlpha ? PSM_5551 : PSM_4444;
	}

	if(numLevels < 1) numLevels = 1;
	int32 format = rwFormatFromPsm(psm) | Raster::TEXTURE | Raster::DONTALLOCATE;
	if(numLevels > 1) format |= Raster::MIPMAP;
	Raster *raster = Raster::create(pw, ph, depthFromPsm(psm), format, PLATFORM_PSP);
	if(raster == nil){
		img->destroy();
		return nil;
	}
	PspRaster *natras = GETPSPRASTEREXT(raster);
	natras->psm = psm;
	raster->depth = depthFromPsm(psm);
	if(!allocateLevels(raster, natras, pw, ph, numLevels)){
		raster->destroy();
		img->destroy();
		return nil;
	}
	raster->originalStride = raster->stride;
	natras->swizzled = canSwizzle(natras);

	if(psm == PSM_T4 || psm == PSM_T8){
		int32 n = psm == PSM_T4 ? 16 : 256;
		memcpy(natras->palette, img->palette, n*4);
		natras->hasAlpha = 0;
		for(int32 i = 0; i < n; i++)
			if(((uint8*)natras->palette)[i*4+3] != 0xFF){
				natras->hasAlpha = 1;
				break;
			}
	}else
		natras->hasAlpha = psm != PSM_5650;

	// level 0 and mipmaps
	Image *lvl = img;
	for(int32 i = 0; i < natras->numLevels; i++){
		if(i > 0){
			Image *src = img;
			Image *rgba = nil;
			Image *next;
			if(src->depth <= 8)
				next = resizeImage(src, natras->levelWidth[i], natras->levelHeight[i]);
			else{
				next = resizeImage(src, natras->levelWidth[i], natras->levelHeight[i]);
			}
			if(rgba) rgba->destroy();
			if(lvl != img) lvl->destroy();
			lvl = next;
		}
		writeLevel(natras, i, lvl);
	}
	if(lvl != img) lvl->destroy();
	img->destroy();
	writebackRange(natras->data, natras->dataSize);
	if(natras->palette)
		writebackRange(natras->palette, psm == PSM_T4 ? 64 : 1024);
	return raster;
}

Raster*
convertForeignRaster(Raster *ras, int32 maxSize)
{
	if(ras->platform == PLATFORM_PSP)
		return ras;
	Image *img = ras->toImage();
	if(img == nil)
		return nil;
	int32 levels = 1;
	Raster *n = rasterFromImageEx(img, maxSize, 0, levels);
	img->destroy();
	if(n){
		n->format |= ras->format & Raster::AUTOMIPMAP;
	}
	return n;
}

//
// Native texture streaming
//

// Native PSP texture layout (inside the struct chunk):
//  uint32 platform (PLATFORM_PSP)
//  uint32 filterAddressing
//  char name[32], mask[32]
//  uint32 rasterFormat
//  uint16 width, height
//  uint8  psm, numLevels, swizzled, hasAlpha
//  [palette 16 or 256 * 4 bytes]
//  uint32 dataSize, data
enum { NATIVE_VERSION = 1 };

uint32 t4Expanded, t4Kept;	// statistics

#if !defined(N3DS)
static Texture*
readNativeT4AsT8(Stream *stream, Texture *tex, uint32 format, int32 width, int32 height, uint8 *info)
{
	// level 0 of a 4 bit texture as stored (power of two dimensions)
	int32 pw = nextPow2(width), ph = nextPow2(height);
	if(pw > 512) pw = 512;
	if(ph > 512) ph = 512;
	int32 bw4 = bufWidthFor(pw, PSM_T4);
	uint32 size4 = levelBytes(bw4, ph, PSM_T4);
	uint32 alloc4 = (size4+15) & ~15;
	uint32 pal[16];
	stream->read8(pal, 64);
	uint32 size = stream->readU32();
	if(size != alloc4 && size != size4){
		RWERROR((ERR_GENERAL, "PSP texture size mismatch"));
		tex->destroy();
		return nil;
	}
	int32 hh = (ph + 7) & ~7;
	uint8 *src = (uint8*)rwMalloc(size, MEMDUR_FUNCTION | ID_DRIVER);
	uint8 *lin = (uint8*)rwMalloc(size, MEMDUR_FUNCTION | ID_DRIVER);
	stream->read8(src, size);
	uint32 stride4 = bw4/2;
	if(info[2])
		unswizzle(lin, src, stride4, hh);
	else
		memcpy(lin, src, size4);

	Raster *raster = Raster::create(width, height, 8,
		(format & ~0xFF & ~Raster::PAL4) | Raster::PAL8 | Raster::TEXTURE | Raster::DONTALLOCATE, PLATFORM_PSP);
	if(raster == nil){
		rwFree(src); rwFree(lin);
		tex->destroy();
		return nil;
	}
	PspRaster *natras = GETPSPRASTEREXT(raster);
	natras->psm = PSM_T8;
	raster->depth = 8;
	if(!allocateLevels(raster, natras, width, height, 1)){
		rwFree(src); rwFree(lin);
		raster->destroy();
		tex->destroy();
		return nil;
	}
	raster->originalStride = raster->stride;
	natras->hasAlpha = info[3];
	memcpy(natras->palette, pal, 64);
	writebackRange(natras->palette, 256*4);
	uint32 stride8 = natras->levelBufWidth[0];
	uint8 *lin8 = (uint8*)rwMalloc(stride8*hh, MEMDUR_FUNCTION | ID_DRIVER);
	memset(lin8, 0, stride8*hh);
	for(int32 y = 0; y < ph; y++)
		for(int32 x = 0; x < pw; x++)
			lin8[y*stride8 + x] = (lin[y*stride4 + x/2] >> ((x&1)*4)) & 0xF;
	natras->swizzled = info[2];
	if(natras->swizzled)
		swizzle(natras->data, lin8, stride8, hh);
	else
		memcpy(natras->data, lin8, stride8*hh);
	writebackRange(natras->data, natras->dataSize);
	rwFree(src); rwFree(lin); rwFree(lin8);
	tex->raster = raster;
	t4Expanded++;
	return tex;
}
#endif

Texture*
readNativeTexture(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_PSP){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	tex->filterAddressing = stream->readU32();
	stream->read8(tex->name, 32);
	stream->read8(tex->mask, 32);

	uint32 format = stream->readU32();
	int32 width = stream->readU16();
	int32 height = stream->readU16();
	uint8 info[4];
	stream->read8(info, 4);
	int32 psm = info[0];
	int32 numLevels = info[1];
#if !defined(N3DS)
	// 4 bit textures with alpha (skid marks, shadows, light pools...) came
	// out as solid dark squares on a real PSP (not in PPSSPP): they're made
	// 8 bit here, like the textures that draw fine. (Opaque ones stay 4 bit,
	// memory.)
	if(psm == PSM_T4 && numLevels == 1 && info[3])
		return readNativeT4AsT8(stream, tex, format, width, height, info);
	if(psm == PSM_T4)
		t4Kept++;
#endif

	Raster *raster = Raster::create(width, height, depthFromPsm(psm),
		(format & ~0xFF) | Raster::TEXTURE | Raster::DONTALLOCATE, PLATFORM_PSP);
	if(raster == nil){
		tex->destroy();
		return nil;
	}
	PspRaster *natras = GETPSPRASTEREXT(raster);
	natras->psm = psm;
	raster->depth = depthFromPsm(psm);
	if(!allocateLevels(raster, natras, width, height, numLevels)){
		raster->destroy();
		tex->destroy();
		return nil;
	}
	raster->originalStride = raster->stride;
	natras->swizzled = info[2];
	natras->hasAlpha = info[3];
	if(natras->palette){
		int32 n = psm == PSM_T4 ? 16 : 256;
		stream->read8(natras->palette, n*4);
		writebackRange(natras->palette, n*4);
	}
	uint32 size = stream->readU32();
	if(size != natras->dataSize){
		RWERROR((ERR_GENERAL, "PSP texture size mismatch"));
		raster->destroy();
		tex->destroy();
		return nil;
	}
	stream->read8(natras->data, size);
	writebackRange(natras->data, size);
	tex->raster = raster;
#ifdef N3DS
	convertRasterNow(raster);
#endif
	return tex;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	Raster *raster = tex->raster;
	PspRaster *natras = GETPSPRASTEREXT(raster);
	int32 chunksize = getSizeNativeTexture(tex);
	writeChunkHeader(stream, ID_STRUCT, chunksize-12);
	stream->writeU32(PLATFORM_PSP);
	stream->writeU32(tex->filterAddressing);
	stream->write8(tex->name, 32);
	stream->write8(tex->mask, 32);
	stream->writeU32(raster->format);
	stream->writeU16(natras->levelWidth[0]);
	stream->writeU16(natras->levelHeight[0]);
	uint8 info[4] = { natras->psm, natras->numLevels, natras->swizzled, natras->hasAlpha };
	stream->write8(info, 4);
	if(natras->palette)
		stream->write8(natras->palette, (natras->psm == PSM_T4 ? 16 : 256)*4);
	stream->writeU32(natras->dataSize);
	stream->write8(natras->data, natras->dataSize);
}

uint32
getSizeNativeTexture(Texture *tex)
{
	PspRaster *natras = GETPSPRASTEREXT(tex->raster);
	uint32 size = 12 + 4 + 4 + 64 + 4 + 4 + 4;
	if(natras->palette)
		size += (natras->psm == PSM_T4 ? 16 : 256)*4;
	size += 4 + natras->dataSize;
	return size;
}

#if defined(RW_PSP) && !defined(N3DS)
// A palette texture made full colour (8888) and not swizzled. The small
// 8 bit smoke and steam textures showed as solid squares on a real PSP
// (PPSSPP drew them fine); full colour avoids the colour table and the
// swizzled layout. Only for small textures, it's 4x the memory.
bool32
makeRasterTrueColour(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	if(raster->platform != PLATFORM_PSP || natras->data == nil || natras->palette == nil ||
	   (natras->psm != PSM_T8 && natras->psm != PSM_T4) || natras->numLevels != 1)
		return 0;
	int32 w = natras->levelWidth[0], h = natras->levelHeight[0];
	int32 hh = (h + 7) & ~7;
	int32 bpp = psmBitsPerPixel(natras->psm);
	uint32 srcStride = natras->levelBufWidth[0]*bpp/8;
	uint8 *lin = (uint8*)rwMalloc(srcStride*hh, MEMDUR_FUNCTION | ID_DRIVER);
	if(lin == nil)
		return 0;
	if(natras->swizzled)
		unswizzle(lin, natras->data, srcStride, hh);
	else
		memcpy(lin, natras->data, srcStride*hh);
	int32 bw = bufWidthFor(w, PSM_8888);
	uint32 size = levelBytes(bw, h, PSM_8888);
	uint32 *out = (uint32*)alignedAlloc(size);
	if(out == nil){
		rwFree(lin);
		return 0;
	}
	memset(out, 0, size);
	for(int32 y = 0; y < h; y++)
		for(int32 x = 0; x < w; x++){
			uint32 idx = bpp == 8 ? lin[y*srcStride + x] : (lin[y*srcStride + x/2] >> ((x&1)*4)) & 0xF;
			out[y*bw + x] = natras->palette[idx];
		}
	rwFree(lin);
	writebackRange(out, size);
	evictRaster(raster);
	if(natras->ownsData){
		geDeferFree(((void**)natras->data)[-1]);
		geDeferFree(((void**)natras->palette)[-1]);
	}
	natras->data = (uint8*)out;
	natras->palette = nil;
	natras->ownsData = 1;
	natras->psm = PSM_8888;
	natras->swizzled = 0;
	natras->levelBufWidth[0] = bw;
	natras->levelOffset[0] = 0;
	natras->dataSize = size;
	raster->depth = 32;
	raster->format = (raster->format & ~(Raster::PAL4 | Raster::PAL8 | 0xF00)) | Raster::C8888;
	raster->stride = bw*4;
	raster->originalStride = raster->stride;
	return 1;
}
#endif

#ifdef N3DS
// the PICA texture has been made, the PSP copy isn't needed any more
void
releaseRasterSource(PspRaster *natras)
{
	if(natras->ownsData){
		alignedFree(natras->data);
		alignedFree(natras->palette);
	}
	natras->data = nil;
	natras->palette = nil;
}
#endif

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	PspRaster *ras = PLUGINOFFSET(PspRaster, object, offset);
	memset(ras, 0, sizeof(PspRaster));
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	Raster *raster = (Raster*)object;
	PspRaster *natras = PLUGINOFFSET(PspRaster, object, offset);
	if(raster->platform == PLATFORM_PSP){
#ifdef RW_PSP
		evictRaster(raster);
#endif
		if(natras->ownsData){
#if defined(RW_PSP) && !defined(N3DS)
			// the GE may still be drawing with it
			if(natras->data) geDeferFree(((void**)natras->data)[-1]);
			if(natras->palette) geDeferFree(((void**)natras->palette)[-1]);
#else
			alignedFree(natras->data);
			alignedFree(natras->palette);
#endif
		}
		natras->data = nil;
		natras->palette = nil;
	}
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	PspRaster *d = PLUGINOFFSET(PspRaster, dst, offset);
	memset(d, 0, sizeof(PspRaster));
	return dst;
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(PspRaster),
	                                            ID_RASTERPSP,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

static void*
driverOpen(void *o, int32, int32)
{
#ifdef RW_PSP
	engine->driver[PLATFORM_PSP]->defaultPipeline = makeDefaultPipeline();
#endif
	engine->driver[PLATFORM_PSP]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_PSP]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_PSP]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_PSP]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_PSP]->rasterLockPalette  = rasterLockPalette;
	engine->driver[PLATFORM_PSP]->rasterUnlockPalette = rasterUnlockPalette;
	engine->driver[PLATFORM_PSP]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_PSP]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_PSP]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_PSP]->rasterToImage      = rasterToImage;
	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, PLATFORM_PSP,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

}
}
