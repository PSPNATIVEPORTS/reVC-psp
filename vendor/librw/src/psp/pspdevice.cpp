// librw PSP backend: render device on the GE via sceGu.
//
// Conventions (kept identical to the gl3 backend so game code behaves the same):
//  - view matrix is left handed (x flipped), camera looks down +z
//  - GL style projection, depth range (65535, 0), depth test GEQUAL
//  - Im2D screen z in [0,1] (near..far) maps to depth 65535*(1-z)
//  - colours are RGBA bytes = GE ABGR words

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwpsp.h"

#if defined(RW_PSP) && !defined(N3DS)

#include <pspkernel.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspge.h>

// sceGu internals, needed to batch stall address updates
extern "C" {
	struct GuDisplayListInternal { unsigned int *start; unsigned int *current; int parent_context; };
	extern GuDisplayListInternal *gu_list;
	extern int ge_list_executed[2];
	int __real_sceGeListUpdateStallAddr(int qid, void *stall);
	int __wrap_sceGeListUpdateStallAddr(int qid, void *stall);
}

// The display list is written through the cache. sceGuStart hands out the
// uncached mirror of the list, so every command word used to be a separate
// uncached write to RAM; on a PSP whose memory is slow that made drawing the
// world 10x slower. Now the list stays cached and the new part is written
// back to RAM just before the GE is allowed to read it (every stall address
// update, including the ones inside sceGu).
static uint32_t listBegin, listEndAddr;	// the frame list, cached addresses
static uint32_t wbFrom;	// written back up to here
static uint32_t wbHold;	// sceGuGetMemory data from here on may still be filled in
static bool listCached;

static inline uint32_t cachedAddr(const void *p) { return (uint32_t)(uintptr_t)p & ~0x40000000u; }

extern "C" int
__wrap_sceGeListUpdateStallAddr(int qid, void *stall)
{
	uint32_t to = cachedAddr(stall);
	if(listCached && to > listBegin && to <= listEndAddr){
		uint32_t from = wbFrom;
		if(wbHold && wbHold < from) from = wbHold;
		if(from < listBegin || from > to) from = listBegin;
		from &= ~63u;
		if(to > from)
			sceKernelDcacheWritebackRange((void*)from, to - from);
		wbFrom = to;
	}
	return __real_sceGeListUpdateStallAddr(qid, stall);
}

namespace rw {
namespace psp {

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#ifdef PSP_LITE
#define LIST_SIZE (640*1024)	// PSP-1000: short draw distance, fewer draws
#else
#define LIST_SIZE (1536*1024)
#endif

static uint32 __attribute__((aligned(64))) displayList[LIST_SIZE/4];
static int32 curDepthBias;
extern int32 worldDepthBias, decalDepthBias;
static void bakeFrameStart(void);
int32 fxTest;

// right after sceGuStart(GU_DIRECT, displayList): write the list through the cache
static void
cacheTheList(void)
{
	gu_list->start = (unsigned int*)cachedAddr(gu_list->start);
	gu_list->current = (unsigned int*)cachedAddr(gu_list->current);
	listBegin = cachedAddr(displayList);
	listEndAddr = listBegin + LIST_SIZE;
	wbFrom = cachedAddr(gu_list->current);
	wbHold = 0;
	listCached = true;
}

static bool frameOpen;
static bool geInitialised;
// Triple buffering: the CPU never waits for the GE or for vblank at the end
// of a frame. The finished frame is flipped at the start of the next one
// (normally long done by then, the game logic runs in between).
#define FB_SIZE (BUF_WIDTH*SCR_HEIGHT*2)
static void *fbRel[3];		// VRAM relative colour buffers
static void *zbp;
static int32 drawIdx;		// buffer the GE draws into
static int32 dispIdx;		// buffer on screen (or queued for the next vblank)
static int32 pendingIdx = -1;	// finished frame waiting to be shown
static uint32 flipVcount;
uint32 frameCounter;
uint32 drawCalls;
uint32 vertsDrawn, geWaitUs, lastFrameVerts, lastFrameDraws;
static bool listFull;
// past this the 3D draws are dropped, so the display list can never
// overflow (64 KB stay free for the state commands and the frame end)
static unsigned int *listLimit;
uint32 listPeakBytes;	// most display list used in a frame (log)
uint32 listDroppedDraws;

RGBA im3dMaterialColor = { 255, 255, 255, 255 };
SurfaceProperties im3dSurfaceProps = { 1.0f, 1.0f, 1.0f };

static inline uint32 rgbaToGE(const RGBA &c) { return c.red | c.green<<8 | c.blue<<16 | (uint32)c.alpha<<24; }

static uint8 clampByte(float f) { if(f <= 0.0f) return 0; if(f >= 1.0f) return 255; return (uint8)(f*255.0f + 0.5f); }

//
// Render state
//

struct RwStateCache {
	Raster *raster;
	uint32 addressU, addressV;
	uint32 filter;
	bool32 vertexAlpha;
	bool32 textureAlpha;
	uint32 alphaFunc;
	uint32 alphaRef;
	uint32 srcblend, destblend;
	bool32 ztest, zwrite;
	uint32 cullmode;
	bool32 fogEnable;
	uint32 fogColor;
	float32 fogStart, fogEnd;
	bool32 gsalpha;
	uint32 gsalpharef;
};
static RwStateCache rs;
// something in rs changed since the last flushRenderState (most draws in a
// row change nothing: the flush is skipped then)
static bool rsDirty = true;	// texture part
static bool rsStateDirty = true;	// blending, alpha test, depth, cull

// what we last sent to the GE
struct GeState {
	int32 texEnable;
	Raster *texRaster;
	int32 texFilter, texWrapU, texWrapV;
	int32 blend;
	int32 blendSrc, blendDst;
	uint32 fixA, fixB;
	int32 alphaTest, alphaFunc, alphaRef;
	int32 depthTest, depthFunc, depthMask;
	int32 cull, frontFace;
	int32 fog;
	float32 fogStart, fogEnd;
	uint32 fogColor;
	int32 lighting;
};
static GeState ge;
static bool geStateValid;
// Forget everything we sent: the next draw sends all of it again. (Only
// clearing geStateValid wasn't enough: blend factors, alpha test values and
// fog were still compared with old values, so after the motion blur or a HUD
// replay changed them behind our back, the light glows (coronas) were drawn
// with the blur's blend factors: black squares around car lights, street
// lights and the sun.)
static inline void
invalidateGeState(void)
{
	geStateValid = 0;
	memset(&ge, 0xFF, sizeof(ge));
	// (not NaN for the floats: built with -ffast-math, NaN compares can't be trusted)
	ge.fogStart = ge.fogEnd = -1.0e30f;
}


// matrices
static ScePspFMatrix4 camView, camProj;
static float32 camNearZ = 1.0f;
uint32 camSerial;

// for the CPU clipping of triangles the GE would drop (psppipe.cpp): the
// camera-relative view matrix, projection scales and near plane
void
getClipCamera(float32 *view16, float32 *projX, float32 *projY, float32 *nearZ)
{
	memcpy(view16, &camView, 16*sizeof(float32));
	*projX = camProj.x.x;
	*projY = camProj.y.y;
	*nearZ = camNearZ;
}
static int32 matrixMode;	// 0 = 3D camera, 1 = 2D identity
static ScePspFMatrix4 identityMat;

static int32 viewportX, viewportY, viewportW, viewportH;

static inline void
convMatrix(ScePspFMatrix4 *out, const Matrix *m)
{
	out->x.x = m->right.x; out->x.y = m->right.y; out->x.z = m->right.z; out->x.w = 0.0f;
	out->y.x = m->up.x;    out->y.y = m->up.y;    out->y.z = m->up.z;    out->y.w = 0.0f;
	out->z.x = m->at.x;    out->z.y = m->at.y;    out->z.z = m->at.z;    out->z.w = 0.0f;
	out->w.x = m->pos.x;   out->w.y = m->pos.y;   out->w.z = m->pos.z;   out->w.w = 1.0f;
}

// Waiting for the GE to finish. The time the wait started is visible to the
// freeze watchdog: a GE that hangs on a broken display list would otherwise
// freeze the game for good (the sound threads keep playing).
volatile uint32 geSyncStart;	// 0: not waiting
static void
geSync(void)
{
	uint32 t = sceKernelGetSystemTimeLow();
	geSyncStart = t ? t : 1;
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	geSyncStart = 0;
}

// watchdog thread: the GE has been busy for seconds, cancel what it's doing
void
geRecover(void)
{
	PspGeBreakParam bp;
	memset(&bp, 0, sizeof(bp));
	sceGeBreak(1, &bp);	// cancel the queued lists; the waiting sync returns
}

// Memory the GE may still read (vertex/index buffers, textures, palettes)
// is only given back once the GE is done with the frames that used it:
// freeing it while the previous frame was still being drawn let new data
// (another model streaming in) appear as garbage polygons or wrong
// textures for a frame. Freed after the GE sync at the start of the next
// frame, or at once (after a sync) when an allocation fails.
#define GEFREE_MAX 4096
static void *geFreeList[GEFREE_MAX];
static int32 geFreeCount;
uint32 geFreedBytesPending;

static void
geFlushFrees(void)
{
	for(int32 i = 0; i < geFreeCount; i++)
		rwFree(geFreeList[i]);
	geFreeCount = 0;
}

// rwmem: a pointer from rwMalloc (not an aligned one)
void
geDeferFree(void *rwmem)
{
	if(rwmem == nil)
		return;
	if(!geInitialised){
		rwFree(rwmem);
		return;
	}
	if(geFreeCount == GEFREE_MAX){
		geSync();
		geFlushFrees();
	}
	geFreeList[geFreeCount++] = rwmem;
}

// an allocation failed: wait for the GE and free everything waiting
bool32
geFlushFreesNow(void)
{
	if(geFreeCount == 0)
		return 0;
	geSync();
	geFlushFrees();
	return 1;
}

static void
presentPending(void)
{
	if(pendingIdx < 0)
		return;
	{
		uint32 t0 = sceKernelGetSystemTimeLow();
		geSync();
		geWaitUs += sceKernelGetSystemTimeLow() - t0;
	}
	geFlushFrees();	// every list sent so far is done
	// the buffer we'll draw into next may still be scanned out if the
	// previous flip hasn't happened yet
	if(sceDisplayGetVcount() == flipVcount)
		sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf((void*)((uintptr)sceGeEdramGetAddr() + (uintptr)fbRel[pendingIdx]),
		BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
	flipVcount = sceDisplayGetVcount();
	int32 freeIdx = 3 - pendingIdx - dispIdx;
	dispIdx = pendingIdx;
	drawIdx = freeIdx;
	pendingIdx = -1;
}

// Like sceGuDrawArray but without a kernel call per draw:
// the GE stall address is only advanced every few draws.
static unsigned int *stallSent;
static int32 drawsSinceStall;

static void
kickGE(void)
{
	unsigned int *cur = gu_list->current;
	if(cur == stallSent)
		return;
	// (the wrapper writes the new commands back to RAM first)
	if(fxTest == 107)
		sceKernelDcacheWritebackAll();
	sceGeListUpdateStallAddr(ge_list_executed[0], cur);
	wbHold = 0;	// vertex data handed out before these draws is filled in by now
	stallSent = cur;
	drawsSinceStall = 0;
}

void
drawArray(int32 prim, int32 vtype, int32 count, const void *indices, const void *vertices)
{
	unsigned int *p = gu_list->current;
	if(p >= listLimit){
		listFull = true;
		listDroppedDraws++;
		return;
	}
	if(vtype)
		*p++ = (18 << 24) | (vtype & 0xffffff);
	if(indices){
		*p++ = (16 << 24) | (((uintptr)indices >> 8) & 0xf0000);
		*p++ = (2 << 24) | ((uintptr)indices & 0xffffff);
	}
	if(vertices){
		*p++ = (16 << 24) | (((uintptr)vertices >> 8) & 0xf0000);
		*p++ = (1 << 24) | ((uintptr)vertices & 0xffffff);
	}
	*p++ = (4 << 24) | (prim << 16) | count;
	gu_list->current = p;
	vertsDrawn += count;
	if(++drawsSinceStall >= 16)
		kickGE();
}

void
ensureFrame(void)
{
	if(frameOpen)
		return;
	presentPending();
	if(geFreeCount){	// (no frame was pending)
		geSync();
		geFlushFrees();
	}
	sceGuStart(GU_DIRECT, displayList);
	cacheTheList();
	// (the list may be addressed through the uncached mirror: start from
	// where sceGu writes, not from displayList)
	listLimit = gu_list->current + (LIST_SIZE - 64*1024)/4;
	sceGuDrawBufferList(GU_PSM_5650, fbRel[drawIdx], BUF_WIDTH);
	stallSent = gu_list->current;
	drawsSinceStall = 0;
	frameOpen = true;
	listFull = false;
	bakeFrameStart();
	lastFrameDraws = drawCalls;
	lastFrameVerts = vertsDrawn;
	drawCalls = 0;
	vertsDrawn = 0;
	instanceUsedUs = 0;
	// GE keeps its registers between lists, but our cache may be stale
	// if anything else touched the GE, so just resend everything at frame start
	invalidateGeState();
	invalidateLightingCache();
}

void*
allocFrameMemory(uint32 size)
{
	ensureFrame();
	if(listFull)
		return nil;
	// keep some space for the commands that end the frame
	if((uint32)sceGuCheckList() + size + 64*1024 > LIST_SIZE){
		listFull = true;
		return nil;
	}
	// filled in after this: keep it in the next write back too
	if(wbHold == 0)
		wbHold = cachedAddr(gu_list->current);
	return sceGuGetMemory(size);
}

static bool
haveListSpace(void)
{
	ensureFrame();
	if(listFull)
		return false;
	if((uint32)sceGuCheckList() + 64*1024 > LIST_SIZE){
		listFull = true;
		return false;
	}
	return true;
}

static void
set3DMatrices(void)
{
	if(matrixMode != 0){
		sceGuSetMatrix(GU_PROJECTION, &camProj);
		sceGuSetMatrix(GU_VIEW, &camView);
		matrixMode = 0;
	}
}

static void
set2DMatrices(void)
{
	if(matrixMode != 1){
		sceGuSetMatrix(GU_PROJECTION, &identityMat);
		sceGuSetMatrix(GU_VIEW, &identityMat);
		sceGuSetMatrix(GU_MODEL, &identityMat);
		matrixMode = 1;
	}
}

// 3x4 matrix straight into the display list (what sceGuSetMatrix(GU_MODEL)
// and sceGuBoneMatrix do, without the per-float function calls)
static inline uint32
geFloat(float32 f)
{
	union { float32 f; uint32 i; } u;
	u.f = f;
	return u.i >> 8;
}

static inline unsigned int*
writeMatrix43(unsigned int *p, uint32 dataCmd, const Matrix *m)
{
	*p++ = dataCmd | geFloat(m->right.x);
	*p++ = dataCmd | geFloat(m->right.y);
	*p++ = dataCmd | geFloat(m->right.z);
	*p++ = dataCmd | geFloat(m->up.x);
	*p++ = dataCmd | geFloat(m->up.y);
	*p++ = dataCmd | geFloat(m->up.z);
	*p++ = dataCmd | geFloat(m->at.x);
	*p++ = dataCmd | geFloat(m->at.y);
	*p++ = dataCmd | geFloat(m->at.z);
	*p++ = dataCmd | geFloat(m->pos.x);
	*p++ = dataCmd | geFloat(m->pos.y);
	*p++ = dataCmd | geFloat(m->pos.z);
	return p;
}

// Camera relative rendering: the GE keeps matrices as 24 bit floats (16 bit
// mantissa), so world positions ~1000 m away from the origin only have
// 1.5-3 cm steps. Characters (bones in world space) jiggled, the camera
// shook and markers wobbled. The camera position is taken out on the CPU
// in full float precision instead: the view matrix has no translation and
// every world matrix is relative to the camera.
V3d camWorldPos;

void
setWorldMatrixCameraSpace(const Matrix *mat)
{
	ensureFrame();
	set3DMatrices();
	unsigned int *p = gu_list->current;
	if(p >= listLimit)
		return;
	*p++ = 0x3a << 24;	// WORLD_MATRIX_NUMBER 0
	p = writeMatrix43(p, 0x3b << 24, mat);
	gu_list->current = p;
}

void
setWorldMatrix(Matrix *mat)
{
	Matrix m;
	m.right = mat->right;
	m.up = mat->up;
	m.at = mat->at;
	m.pos.x = mat->pos.x - camWorldPos.x;
	m.pos.y = mat->pos.y - camWorldPos.y;
	m.pos.z = mat->pos.z - camWorldPos.z;
	setWorldMatrixCameraSpace(&m);
}

void
setBoneMatrix(int32 slot, const Matrix *mat)
{
	unsigned int *p = gu_list->current;
	if(p >= listLimit)
		return;
	*p++ = (0x2a << 24) | (slot*12);	// BONE_MATRIX_NUMBER
	p = writeMatrix43(p, 0x2b << 24, mat);
	gu_list->current = p;
}

static uint32 blendSrcMap[] = {
	GU_FIX,	// invalid
	GU_FIX,	// zero
	GU_FIX,	// one
	GU_FIX,	// src color (invalid for src)
	GU_FIX,	// inv src color (invalid for src)
	GU_SRC_ALPHA,
	GU_ONE_MINUS_SRC_ALPHA,
	GU_DST_ALPHA,
	GU_ONE_MINUS_DST_ALPHA,
	GU_DST_COLOR,
	GU_ONE_MINUS_DST_COLOR,
	GU_SRC_ALPHA,
};
static uint32 blendDstMap[] = {
	GU_FIX,
	GU_FIX,
	GU_FIX,
	GU_SRC_COLOR,
	GU_ONE_MINUS_SRC_COLOR,
	GU_SRC_ALPHA,
	GU_ONE_MINUS_SRC_ALPHA,
	GU_DST_ALPHA,
	GU_ONE_MINUS_DST_ALPHA,
	GU_FIX,	// dst color (invalid for dst)
	GU_FIX,
	GU_SRC_ALPHA,
};

static uint32
blendFix(uint32 rwblend)
{
	switch(rwblend){
	case BLENDZERO: return 0;
	case BLENDONE: return 0xFFFFFF;
	case BLENDSRCCOLOR: case BLENDINVSRCCOLOR:
	case BLENDDESTCOLOR: case BLENDINVDESTCOLOR:
		return 0xFFFFFF;
	}
	return 0;
}

static int32 alphaFuncMap[] = { GU_ALWAYS, GU_GEQUAL, GU_LESS };

static int32
filterMap(uint32 filter, bool mip)
{
	switch(filter){
	case Texture::NEAREST: return GU_NEAREST;
	case Texture::LINEAR: return GU_LINEAR;
	case Texture::MIPNEAREST: return mip ? GU_NEAREST_MIPMAP_NEAREST : GU_NEAREST;
	case Texture::MIPLINEAR: return mip ? GU_NEAREST_MIPMAP_LINEAR : GU_NEAREST;
	case Texture::LINEARMIPNEAREST: return mip ? GU_LINEAR_MIPMAP_NEAREST : GU_LINEAR;
	case Texture::LINEARMIPLINEAR: return mip ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR;
	}
	return GU_LINEAR;
}

static inline uint32
log2Exp(uint32 v)
{
	uint32 e = 0;
	while((1u << e) < v) e++;
	return e;
}

// The texture commands straight into the display list: sceGuTexMode and
// sceGuTexImage each send a texture flush of their own (three per bind
// before), and every sceGu call costs a function call per word.
static void
bindRaster(Raster *raster)
{
	PspRaster *natras = GETPSPRASTEREXT(raster);
	int32 n = natras->numLevels;
	if(n < 1) n = 1;
	unsigned int *p = gu_list->current;
	*p++ = (0xC2u << 24) | ((n-1) << 16) | (natras->swizzled ? 1 : 0);	// TEX_MODE
	*p++ = (0xC3u << 24) | natras->psm;					// TEX_FORMAT
	if(natras->psm == PSM_T4 || natras->psm == PSM_T8){
		uint32 cbp = (uint32)(uintptr)natras->palette;
		*p++ = (0xC5u << 24) | GU_PSM_8888 | (0xFF << 8);		// CLUT_FORMAT
		*p++ = (0xB0u << 24) | (cbp & 0xFFFFFF);			// CLUT_BUF_PTR
		*p++ = (0xB1u << 24) | ((cbp >> 8) & 0xF0000);			// CLUT_BUF_WIDTH
		*p++ = (0xC4u << 24) | (natras->psm == PSM_T4 ? 2 : 32);	// CLUT_LOAD
	}
	for(int32 i = 0; i < n; i++){
		uint32 tbp = (uint32)(uintptr)(natras->data + natras->levelOffset[i]);
		*p++ = ((0xA0u + i) << 24) | (tbp & 0xFFFFFF);					// TEX_ADDR
		*p++ = ((0xA8u + i) << 24) | ((tbp >> 8) & 0xF0000) | natras->levelBufWidth[i];	// TEX_BUF_WIDTH
		*p++ = ((0xB8u + i) << 24) | (log2Exp(natras->levelHeight[i]) << 8) | log2Exp(natras->levelWidth[i]);	// TEX_SIZE
	}
	gu_list->current = p;
	if(n > 1)
		sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
	*gu_list->current++ = 0xCBu << 24;	// TEX_FLUSH
}

void
evictRaster(Raster *raster)
{
	if(rs.raster == raster){
		rs.raster = nil;
		rs.textureAlpha = 0;
		rsDirty = true;
		rsStateDirty = true;
	}
	if(ge.texRaster == raster)
		ge.texRaster = nil;
}

// Bring GE state in line with the RW state
void
flushRenderState(void)
{
	ensureFrame();
	if(!rsDirty && !rsStateDirty && geStateValid)
		return;
	bool force = !geStateValid;
	bool doTex = rsDirty || force, doState = rsStateDirty || force;
	rsDirty = false;
	rsStateDirty = false;

	if(doTex){
	// texture
		Raster *ras = rs.raster;
		PspRaster *natras = ras ? GETPSPRASTEREXT(ras) : nil;
		if(natras && natras->data == nil)
			natras = nil, ras = nil;
		int32 texEnable = ras != nil;
		if(force || texEnable != ge.texEnable){
			if(texEnable) sceGuEnable(GU_TEXTURE_2D);
			else sceGuDisable(GU_TEXTURE_2D);
			ge.texEnable = texEnable;
		}
		if(texEnable){
			if(force || ras != ge.texRaster){
				bindRaster(ras);
				ge.texRaster = ras;
			}
			bool mip = natras->numLevels > 1;
			int32 f = filterMap(rs.filter, mip);
			if(force || f != ge.texFilter){
				sceGuTexFilter(f, (rs.filter == Texture::NEAREST || rs.filter == Texture::MIPNEAREST || rs.filter == Texture::MIPLINEAR) ? GU_NEAREST : GU_LINEAR);
				ge.texFilter = f;
			}
			int32 wu = rs.addressU == Texture::CLAMP || rs.addressU == Texture::BORDER ? GU_CLAMP : GU_REPEAT;
			int32 wv = rs.addressV == Texture::CLAMP || rs.addressV == Texture::BORDER ? GU_CLAMP : GU_REPEAT;
			if(force || wu != ge.texWrapU || wv != ge.texWrapV){
				sceGuTexWrap(wu, wv);
				ge.texWrapU = wu;
				ge.texWrapV = wv;
			}
		}

	}
	if(doState){
	// blending
		int32 blend = rs.vertexAlpha || rs.textureAlpha;
		if(force || blend != ge.blend){
			if(blend) sceGuEnable(GU_BLEND);
			else sceGuDisable(GU_BLEND);
			ge.blend = blend;
		}
		if(blend){
			int32 src = blendSrcMap[rs.srcblend];
			int32 dst = blendDstMap[rs.destblend];
			uint32 fixA = blendFix(rs.srcblend);
			uint32 fixB = blendFix(rs.destblend);
			if(force || src != ge.blendSrc || dst != ge.blendDst || fixA != ge.fixA || fixB != ge.fixB){
				sceGuBlendFunc(GU_ADD, src, dst, fixA, fixB);
				ge.blendSrc = src;
				ge.blendDst = dst;
				ge.fixA = fixA;
				ge.fixB = fixB;
			}
		}

		// alpha test, only together with blending like the other backends
		int32 at = blend && rs.alphaFunc != ALPHAALWAYS;
		if(force || at != ge.alphaTest){
			if(at) sceGuEnable(GU_ALPHA_TEST);
			else sceGuDisable(GU_ALPHA_TEST);
			ge.alphaTest = at;
		}
		if(at){
			int32 func = alphaFuncMap[rs.alphaFunc];
			int32 ref = rs.alphaRef;
			if(force || func != ge.alphaFunc || ref != ge.alphaRef){
				sceGuAlphaFunc(func, ref, 0xFF);
				ge.alphaFunc = func;
				ge.alphaRef = ref;
			}
		}

		// depth
		int32 depthTest = rs.ztest || rs.zwrite;
		int32 depthFunc = rs.ztest ? GU_GEQUAL : GU_ALWAYS;
		int32 depthMask = rs.zwrite ? 0 : 1;	// 1 disables writes
		if(force || depthTest != ge.depthTest){
			if(depthTest) sceGuEnable(GU_DEPTH_TEST);
			else sceGuDisable(GU_DEPTH_TEST);
			ge.depthTest = depthTest;
		}
		if(force || depthFunc != ge.depthFunc){
			sceGuDepthFunc(depthFunc);
			ge.depthFunc = depthFunc;
		}
		if(force || depthMask != ge.depthMask){
			sceGuDepthMask(depthMask);
			ge.depthMask = depthMask;
		}

		// culling
		int32 cull = rs.cullmode != CULLNONE;
		if(force || cull != ge.cull){
			if(cull) sceGuEnable(GU_CULL_FACE);
			else sceGuDisable(GU_CULL_FACE);
			ge.cull = cull;
		}
		if(cull){
			// (sceGuFrontFace(GU_CCW) is the GE's start-up state, and the one
			// that keeps the front faces of RW geometry: GU_CW here used to
			// cull the wrong side whenever it actually got sent)
			int32 ff = rs.cullmode == CULLBACK ? GU_CCW : GU_CW;
			if(force || ff != ge.frontFace){
				sceGuFrontFace(ff);
				ge.frontFace = ff;
			}
		}

	}
	geStateValid = true;
}

static void
setFogState(bool32 enable)
{
	// fog only makes sense for transformed geometry
	int32 fog = enable && rs.fogEnable && rs.fogEnd > rs.fogStart;
	if(!geStateValid || fog != ge.fog){
		if(fog) sceGuEnable(GU_FOG);
		else sceGuDisable(GU_FOG);
		ge.fog = fog;
	}
	if(fog && (rs.fogStart != ge.fogStart || rs.fogEnd != ge.fogEnd || rs.fogColor != ge.fogColor)){
		// GE fog = (viewZ + fog1) * fog2, our view z is positive into the screen
		sceGuSendCommandi(0xCF, rs.fogColor & 0xFFFFFF);	// FOG_COLOR
		sceGuSendCommandf(0xCD, -rs.fogEnd);			// FOG1
		sceGuSendCommandf(0xCE, -1.0f/(rs.fogEnd - rs.fogStart));	// FOG2
		ge.fogStart = rs.fogStart;
		ge.fogEnd = rs.fogEnd;
		ge.fogColor = rs.fogColor;
	}
}

void
setLighting(int32 on)
{
	if(!geStateValid || on != ge.lighting){
		if(on) sceGuEnable(GU_LIGHTING);
		else sceGuDisable(GU_LIGHTING);
		ge.lighting = on;
	}
}

void
setRasterForDraw(Raster *raster)
{
	if(raster != rs.raster){
		rs.raster = raster;
		rsDirty = true;
		bool32 alpha = 0;
		if(raster){
			PspRaster *natras = GETPSPRASTEREXT(raster);
			alpha = natras->hasAlpha;
		}
		if(alpha != rs.textureAlpha)
			rsStateDirty = true;
		rs.textureAlpha = alpha;
	}
}

void
setTexture(Texture *tex)
{
	if(tex == nil || tex->raster == nil){
		setRasterForDraw(nil);
		return;
	}
	Raster *ras = tex->raster;
	if(ras->platform != PLATFORM_PSP){
		setRasterForDraw(nil);
		return;
	}
	setRasterForDraw(ras);
	uint32 f = tex->getFilter(), u = tex->getAddressU(), v = tex->getAddressV();
	if(f != rs.filter || u != rs.addressU || v != rs.addressV){
		rs.filter = f;
		rs.addressU = u;
		rs.addressV = v;
		rsDirty = true;
	}
}

// diagnostics: what the next draw will use (the squares hunt)
void
describeDrawState(char *buf, int32 len)
{
	Raster *r = rs.raster;
	PspRaster *n = r ? GETPSPRASTEREXT(r) : nil;
	snprintf(buf, len, "raster %p plat %d data %p psm %d %dx%d lv %d swz %d texalpha %d | vtxalpha %d blend %d/%d atest %d ref %d ztest %d zwrite %d filter %d addr %d/%d fog %d cull %d | ge tex %d blend %d %d/%d fix %06x/%06x atest %d",
		(void*)r, r ? r->platform : -1, n ? (void*)n->data : nil, n ? n->psm : -1,
		n ? n->levelWidth[0] : 0, n ? n->levelHeight[0] : 0, n ? n->numLevels : 0, n ? n->swizzled : 0, (int)rs.textureAlpha,
		(int)rs.vertexAlpha, rs.srcblend, rs.destblend, rs.alphaFunc, rs.alphaRef, (int)rs.ztest, (int)rs.zwrite,
		rs.filter, rs.addressU, rs.addressV, (int)rs.fogEnable, rs.cullmode,
		ge.texEnable, ge.blend, ge.blendSrc, ge.blendDst, ge.fixA, ge.fixB, ge.alphaTest);
}

bool32
getAlphaBlend(void)
{
	return rs.vertexAlpha || rs.textureAlpha;
}

static void *getRenderState(int32 state);
static void
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	if(state != TEXTURERASTER){
		// most calls set what is already set (every mesh sets vertex alpha)
		if(getRenderState(state) == pvalue)
			return;
		if(state == TEXTUREADDRESS || state == TEXTUREADDRESSU || state == TEXTUREADDRESSV || state == TEXTUREFILTER)
			rsDirty = true;
		else
			rsStateDirty = true;
	}
	switch(state){
	case TEXTURERASTER:
		setRasterForDraw((Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		rs.addressU = rs.addressV = value;
		break;
	case TEXTUREADDRESSU:
		rs.addressU = value;
		break;
	case TEXTUREADDRESSV:
		rs.addressV = value;
		break;
	case TEXTUREFILTER:
		rs.filter = value;
		break;
	case VERTEXALPHA:
		rs.vertexAlpha = value;
		break;
	case SRCBLEND:
		if(value < nelem(blendSrcMap)) rs.srcblend = value;
		break;
	case DESTBLEND:
		if(value < nelem(blendDstMap)) rs.destblend = value;
		break;
	case ZTESTENABLE:
		rs.ztest = value;
		break;
	case ZWRITEENABLE:
		rs.zwrite = value;
		break;
	case FOGENABLE:
		rs.fogEnable = value;
		break;
	case FOGCOLOR:
		rs.fogColor = value;
		break;
	case CULLMODE:
		rs.cullmode = value;
		break;
	case ALPHATESTFUNC:
		if(value <= ALPHALESS) rs.alphaFunc = value;
		break;
	case ALPHATESTREF:
		rs.alphaRef = value;
		break;
	case GSALPHATEST:
		rs.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rs.gsalpharef = value;
		break;
	default:
		break;
	}
}

static void*
getRenderState(int32 state)
{
	uint32 val;
	switch(state){
	case TEXTURERASTER:
		return rs.raster;
	case TEXTUREADDRESS:
		val = rs.addressU == rs.addressV ? rs.addressU : 0;
		break;
	case TEXTUREADDRESSU: val = rs.addressU; break;
	case TEXTUREADDRESSV: val = rs.addressV; break;
	case TEXTUREFILTER: val = rs.filter; break;
	case VERTEXALPHA: val = rs.vertexAlpha; break;
	case SRCBLEND: val = rs.srcblend; break;
	case DESTBLEND: val = rs.destblend; break;
	case ZTESTENABLE: val = rs.ztest; break;
	case ZWRITEENABLE: val = rs.zwrite; break;
	case FOGENABLE: val = rs.fogEnable; break;
	case FOGCOLOR: val = rs.fogColor; break;
	case CULLMODE: val = rs.cullmode; break;
	case ALPHATESTFUNC: val = rs.alphaFunc; break;
	case ALPHATESTREF: val = rs.alphaRef; break;
	case GSALPHATEST: val = rs.gsalpha; break;
	case GSALPHATESTREF: val = rs.gsalpharef; break;
	default: val = 0;
	}
	return (void*)(uintptr)val;
}

static void
resetRenderState(void)
{
	memset(&rs, 0, sizeof(rs));
	rsDirty = true;
	rsStateDirty = true;
	rs.alphaFunc = ALPHAGREATEREQUAL;
	rs.alphaRef = 10;
	rs.srcblend = BLENDSRCALPHA;
	rs.destblend = BLENDINVSRCALPHA;
	rs.ztest = 1;
	rs.zwrite = 1;
	rs.cullmode = CULLNONE;
	rs.filter = Texture::LINEAR;
	rs.addressU = rs.addressV = Texture::WRAP;
	rs.fogColor = 0xFFFFFFFF;
	rs.gsalpharef = 128;
	invalidateGeState();
	invalidateLightingCache();
}

//
// Lighting
//

static RGBAf ambientLight;
static int32 numGeLights;

// what was last sent for lights / material, to skip redundant GE commands
struct GeLightCache {
	int32 n;		// -1 = invalid
	int32 type[4];
	float32 v[4][3];
	uint32 col[4];
	float32 att[4];
};
static GeLightCache lightCache = { -1 };
static uint32 matCache[6];
static bool matCacheValid;

void
invalidateLightingCache(void)
{
	lightCache.n = -1;
	matCacheValid = false;
}

//
// Baked models. Most of a frame's CPU time on the PSP went into turning
// every mesh of every building into GE commands again, the same commands
// each frame: material, texture, blending, draw (a few dozen cache misses
// per mesh on the real hardware). A building's commands are now recorded
// the first time it's drawn and the next frames just CALL the copy, after
// the world matrix and the lights.
// The copy is only valid in the same context (lighting, blend modes, depth
// bias...): a key of those goes with it. The pool is emptied when it's full
// (at the start of a frame, once the GE is done) and the models record
// themselves again.
//
int32 bakeEnabled = 1;
uint32 bakeEpoch = 1;
uint32 bakeStats[8];	// recorded, replayed, rejected, pool resets, atomics allowed, of those clipped
static uint32 *bakePool;
static uint32 bakePoolWords, bakeUsed;
static bool bakeFull;
#define BAKE_POOL_BYTES (448*1024)

static void
bakeFrameStart(void)
{
	if(bakePool == nil && bakeEnabled){
		bakePool = (uint32*)memalign(64, BAKE_POOL_BYTES);
		bakePoolWords = bakePool ? BAKE_POOL_BYTES/4 : 0;
		if(bakePool == nil)
			bakeEnabled = 0;
	}
	if(bakeFull){
		// the GE is done with the last frame (it was synced before)
		bakeUsed = 0;
		bakeFull = false;
		bakeEpoch++;
		bakeStats[3]++;
	}
}

// everything a mesh draw may set, except the matrices, the lights and the
// fog distances (set outside the recorded commands)
static void
bakeForgetState(void)
{
	float32 fs = ge.fogStart, fe = ge.fogEnd;
	uint32 fc = ge.fogColor;
	memset(&ge, 0xFF, sizeof(ge));
	ge.fogStart = fs; ge.fogEnd = fe; ge.fogColor = fc;
	rsDirty = true;
	rsStateDirty = true;
	matCacheValid = false;
	curDepthBias = -1;
}

uint32
bakeContextKey(int32 lit)
{
	uint32 amb = lit ? (clampByte(ambientLight.red) | clampByte(ambientLight.green)<<8 | clampByte(ambientLight.blue)<<16) : 0;
	uint32 k = (lit ? 1 : 0) | rs.srcblend<<1 | rs.destblend<<5 | rs.alphaFunc<<9 | (rs.ztest ? 1 : 0)<<11 |
		(rs.zwrite ? 1 : 0)<<12 | rs.cullmode<<13 | (rs.fogEnable ? 1 : 0)<<15 | (rs.alphaRef & 0xFF)<<16 |
		(worldDepthBias & 0xF)<<24 | (decalDepthBias & 0xF)<<28;
	return k ^ amb*0x9E3779B1u;
}

// start recording: returns where the commands start, nil when not possible
unsigned int*
bakeRecordStart(int32 lit)
{
	ensureFrame();
	if(!bakeEnabled || bakePool == nil || bakeFull || listFull)
		return nil;
	set3DMatrices();
	bakeForgetState();
	if(!lit)
		lightCache.n = -1;	// (an unlit mesh then switches the lights off itself)
	return gu_list->current;
}

uint32*
bakeRecordEnd(unsigned int *start)
{
	unsigned int *end = gu_list->current;
	if(start == nil || listFull || end <= start)
		return nil;
	uint32 n = end - start;
	uint32 lo = (uintptr)listBegin & 0x0FFFFFFF, hi = lo + LIST_SIZE;
	for(uint32 i = 0; i < n; i++){
		uint32 c = start[i] >> 24;
		// no jumps, calls or list ends, no data in the frame's list
		if(c == 8 || c == 9 || c == 10 || c == 11 || c == 12 || c == 15){
			bakeStats[2]++;
			return nil;
		}
		if(c == 16 && i+1 < n){
			uint32 c2 = start[i+1] >> 24;
			uint32 a = ((start[i] & 0xF0000) << 8) | (start[i+1] & 0xFFFFFF);
			if((c2 == 1 || c2 == 2) && a >= lo && a < hi){
				bakeStats[2]++;
				return nil;
			}
		}
	}
	if(bakeUsed + n + 1 > bakePoolWords){
		bakeFull = true;
		return nil;
	}
	uint32 *b = bakePool + bakeUsed;
	memcpy(b, start, n*4);
	b[n] = 11u << 24;	// RET
	sceKernelDcacheWritebackRange(b, (n+1)*4);
	bakeUsed += (n + 1 + 15) & ~15u;	// (64 byte steps, nothing else shares a cache line)
	bakeStats[0]++;
	return b;
}

bool32
bakeReplay(uint32 *b)
{
	ensureFrame();
	if(listFull || gu_list->current + 8 >= listLimit)
		return 0;
	unsigned int *p = gu_list->current;
	uint32 a = (uintptr)b & 0x0FFFFFFF;
	*p++ = (16u << 24) | ((a >> 8) & 0xF0000);
	*p++ = (10u << 24) | (a & 0xFFFFFF);	// CALL
	gu_list->current = p;
	// the copy changed the GE state behind the caches
	bakeForgetState();
	lightCache.n = -1;
	bakeStats[1]++;
	if(++drawsSinceStall >= 16)
		kickGE();
	return 1;
}

// returns whether the geometry is lit
int32
setupLights(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	ensureFrame();
	int32 lit = 0;
	if(atomic == nil || (atomic->geometry->flags & Geometry::LIGHT)){
		if(atomic)
			((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
		else
			((World*)engine->currentWorld)->enumerateLights(&lightData);
		lit = 1;
	}else{
		lightData.numAmbients = 0;
		lightData.numDirectionals = 0;
		lightData.numLocals = 0;
	}

	if(!lit){
		numGeLights = 0;
		return 0;
	}

	ambientLight = lightData.numAmbients ? lightData.ambient : makeRGBAf(0.0f, 0.0f, 0.0f, 1.0f);

	GeLightCache lc;
	int32 n = 0;
	for(int32 i = 0; i < lightData.numDirectionals && n < 4; i++){
		Light *l = lightData.directionals[i];
		V3d *at = &l->getFrame()->getLTM()->at;
		// GE wants the direction towards the light
		lc.type[n] = GU_DIRECTIONAL;
		lc.v[n][0] = -at->x; lc.v[n][1] = -at->y; lc.v[n][2] = -at->z;
		lc.col[n] = clampByte(l->color.red) | clampByte(l->color.green)<<8 | clampByte(l->color.blue)<<16;
		lc.att[n] = 0.0f;
		n++;
	}
	for(int32 i = 0; i < lightData.numLocals && n < 4; i++){
		Light *l = lightData.locals[i];
		if(l->getType() != Light::POINT)
			continue;
		V3d *pos = &l->getFrame()->getLTM()->pos;
		lc.type[n] = GU_POINTLIGHT;
		lc.v[n][0] = pos->x - camWorldPos.x; lc.v[n][1] = pos->y - camWorldPos.y; lc.v[n][2] = pos->z - camWorldPos.z;
		lc.col[n] = clampByte(l->color.red) | clampByte(l->color.green)<<8 | clampByte(l->color.blue)<<16;
		// RW falls off linearly to zero at the radius, approximate it
		float r = l->radius > 0.0f ? l->radius : 1.0f;
		lc.att[n] = 2.0f/(r*r);
		n++;
	}
	lc.n = n;
	bool allSame = lightCache.n == n;
	for(int32 i = 0; i < n; i++){
		bool same = lightCache.n >= 0 && i < lightCache.n &&
			lightCache.type[i] == lc.type[i] && lightCache.col[i] == lc.col[i] &&
			lightCache.v[i][0] == lc.v[i][0] && lightCache.v[i][1] == lc.v[i][1] &&
			lightCache.v[i][2] == lc.v[i][2] && lightCache.att[i] == lc.att[i];
		if(same)
			continue;
		allSame = false;
		ScePspFVector3 v = { lc.v[i][0], lc.v[i][1], lc.v[i][2] };
		sceGuLight(i, lc.type[i], GU_DIFFUSE, &v);
		sceGuLightColor(i, GU_DIFFUSE, lc.col[i]);
		if(lc.type[i] == GU_POINTLIGHT)
			sceGuLightAtt(i, 1.0f, 0.0f, lc.att[i]);
		else if(lightCache.n < 0 || i >= lightCache.n || lightCache.type[i] == GU_POINTLIGHT)
			sceGuLightAtt(i, 1.0f, 0.0f, 0.0f);
	}
	if(!allSame){
		for(int32 i = 0; i < 4; i++){
			bool was = lightCache.n >= 0 && i < lightCache.n;
			bool now = i < n;
			if(lightCache.n < 0 || was != now){
				if(now) sceGuEnable(GU_LIGHT0+i);
				else sceGuDisable(GU_LIGHT0+i);
			}
		}
		lightCache = lc;
	}
	numGeLights = n;
	return 1;
}

static inline uint32
scaleColor(const RGBA &c, float s, uint8 alpha)
{
	return clampByte(c.red/255.0f*s) | clampByte(c.green/255.0f*s)<<8 |
		clampByte(c.blue/255.0f*s)<<16 | (uint32)alpha<<24;
}

// RW lighting: col = clamp(prelight + amb*surfAmb + sum(diffuse)*surfDiff) * matColor
// which maps exactly onto GE lighting:
//   emissive + matAmbient*globalAmbient + sum(light*matDiffuse*NdotL)
void
setMaterialLighting(Atomic *atomic, uint32 geoflags, const RGBA &color,
	const SurfaceProperties &surfprops, bool32 prelit, bool32 lit)
{
	static RGBA white = { 255, 255, 255, 255 };
	const RGBA &mat = (geoflags & Geometry::MODULATE) ? color : white;
	uint32 matGE = rgbaToGE(mat);

	if(!lit){
		if(!prelit){
			// no vertex colour: GE uses the material ambient colour
			setLighting(0);
			if(!matCacheValid || matCache[0] != 1 || matCache[1] != matGE){
				sceGuColor(matGE);
				matCache[0] = 1;
				matCache[1] = matGE;
				matCacheValid = true;
			}
		}else if(matGE == 0xFFFFFFFF){
			setLighting(0);
		}else{
			// vertex colour * material colour
			setLighting(1);
			if(lightCache.n != 0){
				for(int32 i = 0; i < 4; i++)
					sceGuDisable(GU_LIGHT0+i);
				lightCache.n = 0;
			}
			numGeLights = 0;
			if(!matCacheValid || matCache[0] != 2 || matCache[1] != matGE){
				sceGuColorMaterial(GU_AMBIENT);
				sceGuAmbient(matGE);
				sceGuModelColor(0, matGE, 0, 0);
				matCache[0] = 2;
				matCache[1] = matGE;
				matCacheValid = true;
			}
		}
		return;
	}

	setLighting(1);
	// most meshes in a row have the same material colour, surface
	// properties and ambient light: skip the colour maths then
	static uint32 lastCol, lastPrelit;
	static float32 lastAmb, lastDiff, lastR, lastG, lastB;
	uint32 col = mat.red | mat.green<<8 | mat.blue<<16 | (uint32)mat.alpha<<24;
	if(matCacheValid && col == lastCol && (uint32)prelit == lastPrelit &&
	   surfprops.ambient == lastAmb && surfprops.diffuse == lastDiff &&
	   ambientLight.red == lastR && ambientLight.green == lastG && ambientLight.blue == lastB &&
	   matCache[0] == (prelit ? 3u : 4u))
		return;
	lastCol = col; lastPrelit = prelit;
	lastAmb = surfprops.ambient; lastDiff = surfprops.diffuse;
	lastR = ambientLight.red; lastG = ambientLight.green; lastB = ambientLight.blue;
	RGBA amb;
	amb.red = clampByte(ambientLight.red*surfprops.ambient);
	amb.green = clampByte(ambientLight.green*surfprops.ambient);
	amb.blue = clampByte(ambientLight.blue*surfprops.ambient);
	amb.alpha = 255;
	uint32 diff = scaleColor(mat, surfprops.diffuse, mat.alpha);
	if(prelit){
		// emissive = ambient light * material, vertex colour * material via ambient
		// x/255 for x <= 65025 without a division
#define DIV255(x) (((uint32)(x)*0x8081u) >> 23)
		uint32 emis = DIV255(mat.red*amb.red) | DIV255(mat.green*amb.green)<<8 | DIV255(mat.blue*amb.blue)<<16;
#undef DIV255
		if(!matCacheValid || matCache[0] != 3 || matCache[1] != matGE || matCache[2] != emis || matCache[3] != diff){
			sceGuColorMaterial(GU_AMBIENT);
			sceGuAmbient(matGE);
			sceGuModelColor(emis, matGE, diff, 0);
			matCache[0] = 3; matCache[1] = matGE; matCache[2] = emis; matCache[3] = diff;
			matCacheValid = true;
		}
	}else{
		uint32 ambGE = rgbaToGE(amb);
		if(!matCacheValid || matCache[0] != 4 || matCache[1] != matGE || matCache[2] != ambGE || matCache[3] != diff){
			sceGuColorMaterial(0);
			sceGuAmbient(ambGE);
			sceGuModelColor(0, matGE, diff, 0);
			sceGuAmbientColor(matGE);
			matCache[0] = 4; matCache[1] = matGE; matCache[2] = ambGE; matCache[3] = diff;
			matCacheValid = true;
		}
	}
}

//
// Camera
//

static void
beginUpdate(Camera *cam)
{
	ensureFrame();

	// View Matrix
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	float view[16], proj[16];
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  =  -inv.at.x;
	view[9]  =   inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, view, sizeof(RawMatrix));

	// Projection Matrix
	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane-cam->nearPlane);

	proj[0] = invwx;
	proj[1] = 0.0f;
	proj[2] = 0.0f;
	proj[3] = 0.0f;

	proj[4] = 0.0f;
	proj[5] = invwy;
	proj[6] = 0.0f;
	proj[7] = 0.0f;

	proj[8] = cam->viewOffset.x*invwx;
	proj[9] = cam->viewOffset.y*invwy;
	proj[12] = -proj[8];
	proj[13] = -proj[9];
	if(cam->projection == Camera::PERSPECTIVE){
		proj[10] = (cam->farPlane+cam->nearPlane)*invz;
		proj[11] = 1.0f;

		proj[14] = -2.0f*cam->nearPlane*cam->farPlane*invz;
		proj[15] = 0.0f;
	}else{
		proj[10] = 2.0f*invz;
		proj[11] = 0.0f;

		proj[14] = -(cam->farPlane+cam->nearPlane)*invz;
		proj[15] = 1.0f;
	}
	memcpy(&cam->devProj, proj, sizeof(RawMatrix));

	memcpy(&camView, view, sizeof(camView));
	memcpy(&camProj, proj, sizeof(camProj));
	camNearZ = cam->nearPlane;
	camSerial++;
	// camera relative (see setWorldMatrix): no translation in the view
	camView.w.x = camView.w.y = camView.w.z = 0.0f;
	camWorldPos = cam->getFrame()->getLTM()->pos;
	lightCache.n = -1;	// point lights are camera relative too
	matrixMode = -1;
	set3DMatrices();

	rs.fogStart = cam->fogPlane;
	rs.fogEnd = cam->farPlane;

	// viewport / scissor for sub rasters
	Raster *fb = cam->frameBuffer;
	int32 x = 0, y = 0, w = SCR_WIDTH, h = SCR_HEIGHT;
	if(fb && fb != fb->parent){
		x = fb->offsetX;
		y = fb->offsetY;
		w = fb->width;
		h = fb->height;
	}
	if(x != viewportX || y != viewportY || w != viewportW || h != viewportH){
		sceGuOffset(2048 - (w/2) - x, 2048 - (h/2) - y);
		sceGuViewport(2048, 2048, w, h);
		sceGuScissor(x, y, x+w, y+h);
		viewportX = x; viewportY = y; viewportW = w; viewportH = h;
	}
}

static void
endUpdate(Camera *cam)
{
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	ensureFrame();
	int flags = 0;
	if(mode & Camera::CLEARIMAGE){
		sceGuClearColor(rgbaToGE(*col));
		flags |= GU_COLOR_BUFFER_BIT;
	}
	if(mode & Camera::CLEARZ){
		sceGuClearDepth(0);
		flags |= GU_DEPTH_BUFFER_BIT;
	}
	if(mode & Camera::CLEARSTENCIL)
		flags |= GU_STENCIL_BUFFER_BIT;
	if(flags){
		// sceGuClear fills its vertices in after sceGuGetMemory moved the
		// stall address: without this they stayed in the CPU cache and the
		// GE cleared with whatever was in RAM, leaving strips of the colour
		// and depth buffers uncleared (black squares and rectangles on a
		// real PSP, PPSSPP has no cache)
		if(wbHold == 0)
			wbHold = cachedAddr(gu_list->current);
		sceGuClear(flags | GU_FAST_CLEAR_BIT);
	}
}

static void
showRaster(Raster *raster, uint32 flags)
{
	ensureFrame();
	uint32 used = (uint32)sceGuCheckList();
	if(used > listPeakBytes)
		listPeakBytes = used;
	sceGuFinish();	// GE keeps working while the CPU starts the next frame
	frameOpen = false;
	pendingIdx = drawIdx;
	(void)flags;	// no tearing either way: flips happen on vblank
	frameCounter++;
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	// no copies from the frame buffer
	return 0;
}

//
// Im2D: drawn in transform mode with identity matrices, so the GE clips
// and texture coordinates stay normalized.
//

struct GeVert2D
{
	float32 u, v;
	uint32 color;
	float32 x, y, z;
};
#define VTYPE_2D (GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D)

static int32 primTypeMap[] = {
	GU_POINTS,	// none
	GU_LINES,
	GU_LINE_STRIP,
	GU_TRIANGLES,
	GU_TRIANGLE_STRIP,
	GU_TRIANGLE_FAN,
	GU_POINTS
};

static GeVert2D*
convertIm2D(void *vertices, int32 numVertices)
{
	GeVert2D *out = (GeVert2D*)allocFrameMemory(numVertices*sizeof(GeVert2D));
	if(out == nil)
		return nil;
	Im2DVertex *in = (Im2DVertex*)vertices;
	Camera *cam = (Camera*)engine->currentCamera;
	float32 sx = 2.0f/SCR_WIDTH, sy = 2.0f/SCR_HEIGHT;
	float32 ox = 0.0f, oy = 0.0f;
	if(cam && cam->frameBuffer){
		sx = 2.0f/cam->frameBuffer->width;
		sy = 2.0f/cam->frameBuffer->height;
	}
	for(int32 i = 0; i < numVertices; i++){
		out[i].u = in[i].u;
		out[i].v = in[i].v;
		out[i].color = in[i].r | in[i].g<<8 | in[i].b<<16 | (uint32)in[i].a<<24;
		out[i].x = (in[i].x + ox)*sx - 1.0f;
		out[i].y = 1.0f - (in[i].y + oy)*sy;
		float32 z = in[i].z*2.0f - 1.0f;
		if(z < -0.99999f) z = -0.99999f;
		if(z > 0.99999f) z = 0.99999f;
		out[i].z = z;
	}
	return out;
}

// Constant depth offset (polygon offset) through the viewport z centre.
// The 16 bit depth buffer can't separate ground layers that are only a few
// cm apart at a distance, so roads (set by the game) and alpha blended
// decals/shadows are pulled slightly towards the camera.
int32 worldDepthBias = 0;
int32 decalDepthBias = 2;
// (curDepthBias: declared at the top)

static void
applyDepthBias(int32 is3D)
{
	int32 b = 0;
	if(is3D)
		b = worldDepthBias + (getAlphaBlend() ? decalDepthBias : 0);
	if(b == curDepthBias)
		return;
	curDepthBias = b;
	sceGuDepthOffset((unsigned int)b);
	sceGuDepthRange(65535, 0);
}

// 2D overlay cache: the commands of the HUD are copied out of the display
// list after they were built and the next frame just calls the copy
// instead of building them again (the game draws its HUD every other
// frame). The copy is self contained: every cached GE state is sent at
// its start, and the CPU side state cache is thrown away after it.
#define OVERLAY_MAX_WORDS (64*1024/4)
static uint32 *overlayBuf;
static uint32 overlayWords;
static bool overlayValid;
static unsigned int *overlayStart;
uint32 overlayBytes;	// size of the last copy (log)

void invalidateLightingCache(void);
static void
forgetGeState(void)
{
	invalidateGeState();
	invalidateLightingCache();	// (material colour, lights)
	matrixMode = -1;
	curDepthBias = -1;
}

void
overlayBegin(void)
{
	ensureFrame();
	overlayStart = nil;
	if(listFull || gu_list->current + 256 >= listLimit)
		return;
	if(overlayBuf == nil){
		overlayBuf = (uint32*)memalign(64, OVERLAY_MAX_WORDS*4);
		if(overlayBuf == nil)
			return;
	}
	overlayStart = gu_list->current;
	forgetGeState();
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_FOG);
	ge.lighting = 0;
	ge.fog = 0;
}

void
overlayEnd(void)
{
	unsigned int *end = gu_list->current;
	overlayValid = false;
	if(overlayStart == nil || listFull || end <= overlayStart)
		return;
	uint32 n = end - overlayStart;
	if(n + 1 > OVERLAY_MAX_WORDS)
		return;
	const uint32 *src = (const uint32*)overlayStart;
	uint32 *dst = overlayBuf;
	uint32 from = (uintptr)overlayStart & 0x0FFFFFFF;
	uint32 to = (uintptr)end & 0x0FFFFFFF;
	uint32 base = (uintptr)overlayBuf & 0x0FFFFFFF;
	uint32 i = 0;
	while(i < n){
		uint32 w = src[i];
		if((w >> 24) == 16 && i+1 < n){
			uint32 w2 = src[i+1];
			uint32 c2 = w2 >> 24;
			if(c2 == 1 || c2 == 2 || c2 == 8 || c2 == 10){
				// BASE + vertex/index address, jump or call
				uint32 a = ((w & 0xF0000) << 8) | (w2 & 0xFFFFFF);
				if(a >= from && a <= to){
					uint32 na = a - from + base;
					dst[i] = (16 << 24) | ((na >> 8) & 0xF0000);
					dst[i+1] = (c2 << 24) | (na & 0xFFFFFF);
					if(c2 == 8){
						// jump over data (sceGuGetMemory): copy it as it is
						uint32 j = (a - from) / 4;
						if(j < i+2 || j > n)
							return;	// not what we expect, don't use the copy
						memcpy(&dst[i+2], &src[i+2], (j - i - 2)*4);
						i = j;
						continue;
					}
				}else{
					dst[i] = w;
					dst[i+1] = w2;
				}
				i += 2;
				continue;
			}
		}
		dst[i++] = w;
	}
	dst[n] = 11 << 24;	// RET
	sceKernelDcacheWritebackRange(overlayBuf, (n+1)*4);
	overlayWords = n + 1;
	overlayBytes = overlayWords*4;
	overlayValid = true;
}

bool32
overlayReplay(void)
{
	if(!overlayValid || fxTest == 108)
		return 0;
	ensureFrame();
	if(listFull || gu_list->current + 16 >= listLimit)
		return 0;
	unsigned int *p = gu_list->current;
	uint32 a = (uintptr)overlayBuf & 0x0FFFFFFF;
	*p++ = (16 << 24) | ((a >> 8) & 0xF0000);
	*p++ = (10 << 24) | (a & 0xFFFFFF);	// CALL
	gu_list->current = p;
	// the GE now has whatever state the copy left behind
	forgetGeState();
	ge.lighting = 0;
	ge.fog = 0;
	return 1;
}

void
overlayDiscard(void)
{
	overlayValid = false;
}

// testing (squares hunt): the game's sprite buffer drawn in through mode
// (screen pixels, texel UVs) instead of through the transform
int32 im2DThrough;
int32 logSpriteDraws;
struct GeVert2DT
{
	float32 u, v;
	uint32 color;
	float32 x, y, z;
};
#define VTYPE_2DT (GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D)
static GeVert2DT*
convertIm2DThrough(void *vertices, int32 numVertices)
{
	Im2DVertex *in = (Im2DVertex*)vertices;
	for(int32 i = 0; i < numVertices; i++)
		if(in[i].x < 0.0f || in[i].y < 0.0f || in[i].x > 1000.0f || in[i].y > 1000.0f)
			return nil;	// through mode can't do negative coordinates
	GeVert2DT *out = (GeVert2DT*)allocFrameMemory(numVertices*sizeof(GeVert2DT));
	if(out == nil)
		return nil;
	float32 tw = 1.0f, th = 1.0f;
	if(rs.raster){
		PspRaster *natras = GETPSPRASTEREXT(rs.raster);
		tw = natras->levelWidth[0];
		th = natras->levelHeight[0];
	}
	for(int32 i = 0; i < numVertices; i++){
		out[i].u = in[i].u*tw;
		out[i].v = in[i].v*th;
		out[i].color = in[i].r | in[i].g<<8 | in[i].b<<16 | (uint32)in[i].a<<24;
		out[i].x = in[i].x;
		out[i].y = in[i].y;
		float32 z = in[i].z;
		if(z < 0.0f) z = 0.0f;
		if(z > 1.0f) z = 1.0f;
		out[i].z = 65535.0f*(1.0f - z);
	}
	return out;
}

static void
prepare2D(void)
{
	flushRenderState();
	applyDepthBias(0);
	set2DMatrices();
	setLighting(0);
	setFogState(0);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	if(numVertices <= 0 || !haveListSpace())
		return;
	if(im2DThrough){
		GeVert2DT *t = convertIm2DThrough(vertices, numVertices);
		if(t){
			prepare2D();
			drawArray(primTypeMap[primType], VTYPE_2DT, numVertices, nil, t);
			drawCalls++;
			return;
		}
	}
	GeVert2D *v = convertIm2D(vertices, numVertices);
	if(v == nil)
		return;
	prepare2D();
	drawArray(primTypeMap[primType], VTYPE_2D, numVertices, nil, v);
	drawCalls++;
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	if(numVertices <= 0 || numIndices <= 0 || !haveListSpace())
		return;
	GeVert2D *v = convertIm2D(vertices, numVertices);
	if(v == nil)
		return;
	uint16 *idx = (uint16*)allocFrameMemory(numIndices*2);
	if(idx == nil)
		return;
	memcpy(idx, indices, numIndices*2);
	prepare2D();
	drawArray(primTypeMap[primType], VTYPE_2D|GU_INDEX_16BIT, numIndices, idx, v);
	drawCalls++;
}

static Im2DVertex tmpprimbuf[3];

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	im2DRenderPrimitive(PRIMTYPELINELIST, tmpprimbuf, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	tmpprimbuf[2] = verts[vert3];
	im2DRenderPrimitive(PRIMTYPETRILIST, tmpprimbuf, 3);
}

//
// Im3D
//

struct GeVert3D
{
	float32 u, v;
	uint32 color;
	float32 x, y, z;
};
#define VTYPE_3D (GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D)

static GeVert3D *im3dVerts;
static int32 im3dNumVerts;

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	im3dVerts = nil;
	im3dNumVerts = 0;
	if(numVertices <= 0 || !haveListSpace())
		return;
	GeVert3D *out = (GeVert3D*)allocFrameMemory(numVertices*sizeof(GeVert3D));
	if(out == nil)
		return;
	Im3DVertex *in = (Im3DVertex*)vertices;
	for(int32 i = 0; i < numVertices; i++){
		out[i].u = in[i].u;
		out[i].v = in[i].v;
		out[i].color = in[i].r | in[i].g<<8 | in[i].b<<16 | (uint32)in[i].a<<24;
		out[i].x = in[i].position.x;
		out[i].y = in[i].position.y;
		out[i].z = in[i].position.z;
	}
	if(world)
		setWorldMatrix(world);
	else{
		// world space vertices: make them camera relative here
		for(int32 i = 0; i < numVertices; i++){
			out[i].x -= camWorldPos.x;
			out[i].y -= camWorldPos.y;
			out[i].z -= camWorldPos.z;
		}
		static Matrix ident;
		ident.setIdentity();
		setWorldMatrixCameraSpace(&ident);
	}
	if((flags & im3d::VERTEXUV) == 0)
		setRasterForDraw(nil);
	im3dVerts = out;
	im3dNumVerts = numVertices;
}

static void
prepare3DIm(void)
{
	flushRenderState();
	applyDepthBias(1);
	set3DMatrices();
	setLighting(0);
	setFogState(1);
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	if(im3dVerts == nil || fxTest == 104)
		return;
	prepare3DIm();
	drawArray(primTypeMap[primType], VTYPE_3D, im3dNumVerts, nil, im3dVerts);
	drawCalls++;
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	if(im3dVerts == nil || numIndices <= 0 || fxTest == 104)
		return;
	uint16 *idx = (uint16*)allocFrameMemory(numIndices*2);
	if(idx == nil)
		return;
	memcpy(idx, indices, numIndices*2);
	prepare3DIm();
	drawArray(primTypeMap[primType], VTYPE_3D|GU_INDEX_16BIT, numIndices, idx, im3dVerts);
	drawCalls++;
}

void
im3DEnd(void)
{
	im3dVerts = nil;
	im3dNumVerts = 0;
}

// used by the pipelines before a draw
void
prepareGeometryDraw(void)
{
	flushRenderState();
	applyDepthBias(1);
	set3DMatrices();
	setFogState(1);
}

//
// Frame buffer grab (screenshots)
//

// Motion blur like VCS (the PS2 "trails"): the blur buffer, the frame
// before, is blended over the new frame, then the GE copies the result
// back into the blur buffer. Both run on the GE (a full screen sprite and a
// VRAM copy, well under a millisecond); nothing for the CPU.
// Call before the HUD is drawn; alpha 0 (off) .. 255.
#define BLUR_REL (4*FB_SIZE)	// after the three colour buffers and the depth buffer
static bool blurValid;
static const uint32 BLUR_GAIN5 = 13, BLUR_GAIN6 = 8;	// (measured in PPSSPP's GE emulation)

struct BlurVert { uint16 u, v; int16 x, y, z; };

// A system dialog (the PSP's save / load screens) on top of the last game
// frame, dimmed. step() is called once per frame after the background was
// drawn (it calls the utility's update, which draws the dialog into the
// same buffer) and returns 0 when the dialog is gone.
static uint32 __attribute__((aligned(64))) dialogList[2048];
void
utilityDialog(int32 (*step)(void))
{
	if(frameOpen){
		// what the game had started of this frame isn't shown
		sceGuFinish();
		frameOpen = false;
	}
	geSync();
	geFlushFrees();
	if(pendingIdx >= 0){
		sceDisplayWaitVblankStart();
		sceDisplaySetFrameBuf((void*)((uintptr)sceGeEdramGetAddr() + (uintptr)fbRel[pendingIdx]),
			BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_IMMEDIATE);
		dispIdx = pendingIdx;
		pendingIdx = -1;
	}
	listCached = false;
	uint8 *edram = (uint8*)sceGeEdramGetAddr();
	int32 bg = dispIdx;
	int32 bufs[2], nb = 0;
	for(int32 i = 0; i < 3; i++)
		if(i != bg) bufs[nb++] = i;
	int32 k = 0;
	for(;;){
		int32 cur = bufs[k];
		sceGuStart(GU_DIRECT, dialogList);
		sceGuDrawBufferList(GU_PSM_5650, fbRel[cur], BUF_WIDTH);
		sceGuCopyImage(GU_PSM_5650, 0, 0, SCR_WIDTH, SCR_HEIGHT, BUF_WIDTH, edram + (uintptr)fbRel[bg],
			0, 0, BUF_WIDTH, edram + (uintptr)fbRel[cur]);
		sceGuTexSync();
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDepthMask(GU_TRUE);
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_FOG);
		sceGuDisable(GU_LIGHTING);
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
		struct DimVert { uint32 color; int16 x, y, z; int16 pad; };
		DimVert *v = (DimVert*)sceGuGetMemory(2*sizeof(DimVert));
		v[0].color = v[1].color = 0x90000000;
		v[0].x = 0; v[0].y = 0; v[0].z = 0;
		v[1].x = SCR_WIDTH; v[1].y = SCR_HEIGHT; v[1].z = 0;
		sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, nil, v);
		sceGuFinish();
		sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
		int32 more = step();
		sceDisplayWaitVblankStart();
		sceDisplaySetFrameBuf(edram + (uintptr)fbRel[cur], BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_IMMEDIATE);
		dispIdx = cur;
		k ^= 1;
		if(!more)
			break;
	}
	// the game's next frame goes into a buffer that isn't on screen
	drawIdx = dispIdx == 0 ? 1 : 0;
	pendingIdx = -1;
	flipVcount = sceDisplayGetVcount() - 1;
	blurValid = false;
	overlayValid = false;
	invalidateGeState();
	invalidateLightingCache();
	matrixMode = -1;
	curDepthBias = -1;
}

void
motionBlur(int32 alpha)
{
	ensureFrame();
	if(alpha <= 0 || listFull){
		blurValid = false;	// start from a fresh copy next time
		return;
	}
	if(alpha > 230) alpha = 230;
	uint8 *edram = (uint8*)sceGeEdramGetAddr();
	void *cur = edram + (uintptr)fbRel[drawIdx];
	void *acc = edram + BLUR_REL;
	if(blurValid){
		// 32 pixel wide strips: the texture cache likes them
		const int32 strips = SCR_WIDTH/32;
		BlurVert *v = (BlurVert*)allocFrameMemory(strips*2*sizeof(BlurVert));
		if(v == nil)
			return;
		for(int32 i = 0; i < strips; i++){
			int16 x0 = i*32, x1 = x0 + 32;
			v[i*2+0] = { (uint16)x0, 0, x0, 0, 0 };
			v[i*2+1] = { (uint16)x1, SCR_HEIGHT, x1, SCR_HEIGHT, 0 };
		}
		forgetGeState();
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDepthMask(GU_TRUE);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_FOG);
		sceGuDisable(GU_LIGHTING);
		ge.fog = 0;
		ge.lighting = 0;
		sceGuEnable(GU_TEXTURE_2D);
		sceGuTexMode(GU_PSM_5650, 0, 0, 0);
		sceGuTexImage(0, 512, 512, BUF_WIDTH, acc);
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
		sceGuTexFilter(GU_NEAREST, GU_NEAREST);
		sceGuTexWrap(GU_CLAMP, GU_CLAMP);
		sceGuTexFlush();
		sceGuEnable(GU_BLEND);
		// The 16 bit frame buffer rounds every blend down a little, and the
		// blur feeds back into itself: without a small extra weight the picture
		// settles about one colour step darker.
		// (red and blue have 5 bits, green 6: they need more)
		uint32 a = alpha;
		uint32 b5 = 255 - alpha + BLUR_GAIN5, b6 = 255 - alpha + BLUR_GAIN6;
		if(b5 > 255) b5 = 255;
		if(b6 > 255) b6 = 255;
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, a | a<<8 | a<<16, b5 | b6<<8 | b5<<16);
		drawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, strips*2, nil, v);
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);	// what the rest of the renderer expects
		forgetGeState();
	}
	// the blended frame becomes the next blur buffer
	sceGuCopyImage(GU_PSM_5650, 0, 0, SCR_WIDTH, SCR_HEIGHT, BUF_WIDTH, cur, 0, 0, BUF_WIDTH, acc);
	sceGuTexSync();
	blurValid = true;
}

Image*
grabFrameBuffer(void)
{
	Image *img = Image::create(SCR_WIDTH, SCR_HEIGHT, 32);
	img->allocate();
	// the last finished frame
	geSync();
	int32 idx = pendingIdx >= 0 ? pendingIdx : dispIdx;
	uint16 *vram = (uint16*)((uintptr)sceGeEdramGetAddr() + (uintptr)fbRel[idx]);
	vram = (uint16*)((uintptr)vram | 0x40000000);
	for(int y = 0; y < SCR_HEIGHT; y++)
		for(int x = 0; x < SCR_WIDTH; x++){
			uint16 c = vram[y*BUF_WIDTH + x];
			uint8 *d = img->pixels + y*img->stride + x*4;
			d[0] = (c&0x1F)*255/31;
			d[1] = ((c>>5)&0x3F)*255/63;
			d[2] = ((c>>11)&0x1F)*255/31;
			d[3] = 255;
		}
	return img;
}

//
// Device system
//

static int
initGE(void)
{
	if(geInitialised)
		return 1;
	// VRAM layout: two 16 bit colour buffers and a 16 bit depth buffer
	fbRel[0] = (void*)0;
	fbRel[1] = (void*)FB_SIZE;
	fbRel[2] = (void*)(2*FB_SIZE);
	zbp = (void*)(3*FB_SIZE);
	drawIdx = 0;
	dispIdx = 1;
	pendingIdx = -1;

	sceGuInit();
	sceGuStart(GU_DIRECT, displayList);
	cacheTheList();
	sceGuDrawBuffer(GU_PSM_5650, fbRel[0], BUF_WIDTH);
	sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbRel[1], BUF_WIDTH);
	sceGuDepthBuffer(zbp, BUF_WIDTH);
	sceGuFrontFace(GU_CCW);
	sceGuOffset(2048 - (SCR_WIDTH/2), 2048 - (SCR_HEIGHT/2));
	sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
	viewportX = 0; viewportY = 0; viewportW = SCR_WIDTH; viewportH = SCR_HEIGHT;
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuEnable(GU_DITHER);
	sceGuShadeModel(GU_SMOOTH);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 1);
	sceGuSpecular(0.0f);
	for(int i = 0; i < 4; i++){
		sceGuLightColor(i, GU_AMBIENT, 0);
		sceGuLightColor(i, GU_SPECULAR, 0);
		sceGuLightAtt(i, 1.0f, 0.0f, 0.0f);
	}
	sceGuLightMode(GU_SINGLE_COLOR);
	{
		// unused bone slots get weight 0, keep them finite
		ScePspFMatrix4 id;
		memset(&id, 0, sizeof(id));
		id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
		for(int i = 0; i < 8; i++)
			sceGuBoneMatrix(i, &id);
	}
	sceGuClearColor(0);
	sceGuClearDepth(0);
	if(wbHold == 0)
		wbHold = cachedAddr(gu_list->current);
	sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
	sceGuFinish();
	geSync();
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	flipVcount = sceDisplayGetVcount() - 1;

	memset(&identityMat, 0, sizeof(identityMat));
	identityMat.x.x = identityMat.y.y = identityMat.z.z = identityMat.w.w = 1.0f;
	camView = identityMat;
	camProj = identityMat;
	matrixMode = -1;
	frameOpen = false;
	invalidateGeState();
	geInitialised = true;
	resetRenderState();
	return 1;
}

static int
termGE(void)
{
	if(geInitialised){
		geSync();
		sceGuTerm();
		geInitialised = false;
	}
	return 1;
}

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	VideoMode *rwmode;
	switch(req){
	case DEVICEOPEN:
		return 1;
	case DEVICECLOSE:
		return 1;
	case DEVICEINIT:
		return initGE();
	case DEVICETERM:
		return termGE();
	case DEVICEFINALIZE:
		resetRenderState();
		return 1;

	case DEVICEGETNUMSUBSYSTEMS:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return n == 0;
	case DEVICEGETSUBSSYSTEMINFO:
		if(n != 0)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, "PSP GE", sizeof(SubSystemInfo::name));
		return 1;

	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETVIDEOMODE:
		return n == 0;
	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		rwmode->width = SCR_WIDTH;
		rwmode->height = SCR_HEIGHT;
		rwmode->depth = 16;
		rwmode->flags = VIDEOMODEEXCLUSIVE;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		return 1;
	default:
		return 0;
	}
	return 1;
}

Device renderdevice = {
	0.0f, 1.0f,
	psp::beginUpdate,
	psp::endUpdate,
	psp::clearCamera,
	psp::showRaster,
	psp::rasterRenderFast,
	psp::setRenderState,
	psp::getRenderState,
	psp::im2DRenderLine,
	psp::im2DRenderTriangle,
	psp::im2DRenderPrimitive,
	psp::im2DRenderIndexedPrimitive,
	psp::im3DTransform,
	psp::im3DRenderPrimitive,
	psp::im3DRenderIndexedPrimitive,
	psp::im3DEnd,
	psp::deviceSystem
};

}
}

#endif
