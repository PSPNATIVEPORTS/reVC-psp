// librw PSP backend: object pipelines (default, hardware skinning, matfx)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwplugins.h"
#include "rwpsp.h"

#ifndef N3DS	// the 3DS has its own pipelines (3ds/c3dpipe.cpp)
#ifdef RW_PSP
#include <pspkernel.h>
#include <pspgu.h>
#endif

namespace rw {
namespace psp {

bool32 envMapEnabled = 1;
float32 tessMaxEdge = 0.0f;	// split world triangles with longer edges (metres), 0 = off
uint32 tessStats[4];		// geometries split, vertices before, vertices after, skipped (low memory)

#ifdef RW_PSP

extern "C" void *__real_malloc(size_t size);
}
}
int pspFreeHeapKB(void);	// psp.cpp
namespace rw {
namespace psp {

static void*
malloc16(uint32 size)
{
	uint8 *p = (uint8*)rwMalloc(size + 16 + sizeof(void*), MEMDUR_EVENT | ID_GEOMETRY);
	if(p == nil)
		return nil;
	uint8 *a = (uint8*)(((uintptr)p + sizeof(void*) + 15) & ~(uintptr)15);
	((void**)a)[-1] = p;
	return a;
}

static void
free16(void *a)
{
	if(a)
		rwFree(((void**)a)[-1]);
}

// for buffers the GE reads
static void
geFree16(void *a)
{
	if(a)
		geDeferFree(((void**)a)[-1]);
}

static void tessCancel(InstanceDataHeader *header);
static void buildChunks(InstanceDataHeader *header);

// is the model within its radius + extra metres of the camera?
static bool
atomicNear(Atomic *atomic, float32 extra)
{
	if(engine->currentCamera == nil)
		return false;
	Sphere *s = atomic->getWorldBoundingSphere();
	V3d *cp = &((Camera*)engine->currentCamera)->getFrame()->getLTM()->pos;
	float32 dx = s->center.x - cp->x, dy = s->center.y - cp->y, dz = s->center.z - cp->z;
	float32 r = s->radius + extra;
	return dx*dx + dy*dy + dz*dz < r*r;
}

static void
freeHeaderNow(InstanceDataHeader *header)
{
	geFree16(header->vertexBuffer);
	geFree16(header->indexBuffer);
	geFree16(header->tessIndexBuffer);
	free16(header->chunks);
	free16(header->cells);
	rwFree(header->inst);
	if(header->batches)
		rwFree(header->batches);
	rwFree(header);
}

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_PSP)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	if(header->tessJob != -1)
		tessCancel(header);
	freeHeaderNow(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static InstanceDataHeader*
instanceMesh(rw::ObjPipeline *rwpipe, Geometry *geo, bool setGeo = true)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header, 0, sizeof(InstanceDataHeader));
	header->tessJob = -1;
	MeshHeader *meshh = geo->meshHeader;
	if(setGeo)
		geo->instData = header;
	header->platform = PLATFORM_PSP;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == MeshHeader::TRISTRIP ? GU_TRIANGLE_STRIP : GU_TRIANGLES;
	header->totalNumVertex = geo->numVertices;
	header->totalNumIndex = meshh->totalIndices;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header->inst, 0, header->numMeshes*sizeof(InstanceData));

	header->indexBuffer = (uint16*)malloc16(header->totalNumIndex*2 + 2);
	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 offset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->indexOffset = offset;
		memcpy(header->indexBuffer + offset, mesh->indices, inst->numIndex*2);
		offset += inst->numIndex;
		mesh++;
		inst++;
	}
	sceKernelDcacheWritebackRange(header->indexBuffer, header->totalNumIndex*2);
	return header;
}

// Drop the PC vertex arrays once the GE copy exists (saves about half
// the memory of every model). Bounding spheres are kept.
static void
freeSourceData(Geometry *geo)
{
	rwFree(geo->triangles);
	geo->triangles = nil;
	geo->colors = nil;
	for(int32 i = 0; i < 8; i++)
		geo->texCoords[i] = nil;
	MorphTarget *mt = rwNewT(MorphTarget, geo->numMorphTargets, MEMDUR_EVENT | ID_GEOMETRY);
	for(int32 i = 0; i < geo->numMorphTargets; i++){
		mt[i] = geo->morphTargets[i];
		mt[i].vertices = nil;
		mt[i].normals = nil;
	}
	rwFree(geo->morphTargets);
	geo->morphTargets = mt;
}

static InstanceDataHeader *instanceTessellated(rw::ObjPipeline *rwpipe, Geometry *geo, float32 maxEdge);

// Splitting the big ground polygons of a model that just streamed in took
// up to 30 ms, all in one frame. It's done by a low priority thread now,
// in the time the game waits for the next vblank anyway; the model is drawn
// unsplit until then (a few frames, it's still far away when it streams).
enum { TJ_FREE, TJ_QUEUED, TJ_RUNNING, TJ_DONE, TJ_CANCELLED };
struct TessJob
{
	Geometry *geo;
	rw::ObjPipeline *pipe;
	float32 maxEdge;
	volatile int32 state;
	InstanceDataHeader *result;	// nil: nothing to split
	uint32 order;
};
#define TESS_JOBS 64
static TessJob tessJobs[TESS_JOBS];
static uint32 tessOrder;
static SceUID tessLock = -1, tessWake = -1, tessThread = -1, tessWork = -1;	// tessWork: the split scratch buffers
static inline void lockTess(void) { sceKernelWaitSema(tessLock, 1, NULL); }
static inline void unlockTess(void) { sceKernelSignalSema(tessLock, 1); }

static int
tessThreadMain(SceSize, void*)
{
	for(;;){
		sceKernelWaitSema(tessWake, 1, NULL);
		for(;;){
			// oldest waiting job
			lockTess();
			TessJob *job = nil;
			for(int32 i = 0; i < TESS_JOBS; i++)
				if(tessJobs[i].state == TJ_QUEUED && (job == nil || (int32)(tessJobs[i].order - job->order) < 0))
					job = &tessJobs[i];
			if(job) job->state = TJ_RUNNING;
			unlockTess();
			if(job == nil)
				break;
			sceKernelWaitSema(tessWork, 1, NULL);
			job->result = instanceTessellated(job->pipe, job->geo, job->maxEdge);
			sceKernelSignalSema(tessWork, 1);
			lockTess();
			job->state = TJ_DONE;
			unlockTess();
		}
	}
	return 0;
}

static bool
tessStart(void)
{
	if(tessThread >= 0)
		return true;
	tessLock = sceKernelCreateSema("tesslock", 0, 1, 1, NULL);
	tessWake = sceKernelCreateSema("tesswake", 0, 0, 1000, NULL);
	tessWork = sceKernelCreateSema("tesswork", 0, 1, 1, NULL);
	// below the game's priority: it only runs while the game waits
	tessThread = sceKernelCreateThread("ground_split", tessThreadMain, 0x30, 0x8000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
	if(tessLock < 0 || tessWake < 0 || tessWork < 0 || tessThread < 0){
		tessThread = -1;
		return false;
	}
	sceKernelStartThread(tessThread, 0, NULL);
	return true;
}

static void
tessQueue(rw::ObjPipeline *pipe, Geometry *geo, InstanceDataHeader *header)
{
	header->tessJob = -2;
	if(!tessStart())
		return;
	lockTess();
	for(int32 i = 0; i < TESS_JOBS; i++)
		if(tessJobs[i].state == TJ_FREE){
			TessJob *j = &tessJobs[i];
			j->geo = geo;
			j->pipe = pipe;
			j->maxEdge = tessMaxEdge;
			j->result = nil;
			j->order = tessOrder++;
			j->state = TJ_QUEUED;
			header->tessJob = i;
			break;
		}
	unlockTess();
	if(header->tessJob >= 0)
		sceKernelSignalSema(tessWake, 1);
}

static void freeHeaderNow(InstanceDataHeader *header);

// the model's geometry is going away: forget its job
static void
tessCancel(InstanceDataHeader *header)
{
	int32 j = header->tessJob;
	header->tessJob = -1;
	if(j < 0)
		return;
	TessJob *job = &tessJobs[j];
	for(;;){
		lockTess();
		int32 st = job->state;
		if(st == TJ_QUEUED)
			job->state = TJ_FREE;
		unlockTess();
		if(st != TJ_RUNNING)
			break;
		sceKernelDelayThread(200);	// rare: let it finish
	}
	if(job->state == TJ_DONE){
		if(job->result)
			freeHeaderNow(job->result);	// never drawn
		job->state = TJ_FREE;
	}
}

static void freeSourceData(Geometry *geo);

// game thread, before drawing: use a finished split
static bool atomicNear(Atomic *atomic, float32 extra);
static void
tessCheck(rw::ObjPipeline *pipe, Geometry *geo, InstanceDataHeader *header, Atomic *atom)
{
	if(header->tessJob == -2){
		tessQueue(pipe, geo, header);
		return;
	}
	TessJob *job = &tessJobs[header->tessJob];
	if(job->state != TJ_DONE){
		// The thread only runs while the game waits; when the game is busy
		// a model can get close to the camera unsplit, and its big ground
		// polygons vanish (holes). Close ones are split right now instead.
		// (with the CPU clipping no hole can appear: it checks the whole
		// model until the list by area is ready)
		if(clipEnabled || !atomicNear(atom, 40.0f))
			return;
		lockTess();
		int32 st = job->state;
		if(st == TJ_QUEUED)
			job->state = TJ_RUNNING;
		unlockTess();
		if(st == TJ_QUEUED){
			sceKernelWaitSema(tessWork, 1, NULL);
			job->result = instanceTessellated(job->pipe, job->geo, job->maxEdge);
			sceKernelSignalSema(tessWork, 1);
			job->state = TJ_DONE;
		}else{
			while(job->state == TJ_RUNNING)
				sceKernelDelayThread(100);	// the thread is on it
		}
		if(job->state != TJ_DONE)
			return;
	}
	InstanceDataHeader *split = job->result;
	job->state = TJ_FREE;
	header->tessJob = -1;
	if(split){
		split->tessJob = -1;
		// the unsplit buffers may still be in a frame the GE is drawing
		geo->instData = split;
		header->tessJob = -1;
		freeHeaderNow(header);	// (its GE buffers go through geDeferFree)
	}
	freeSourceData(geo);
}

static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if(geo->flags & Geometry::NATIVE)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(geo->instData){
		if(header->platform != PLATFORM_PSP || header->serialNumber != geo->meshHeader->serialNum)
			freeInstanceData(geo);
	}
	if(geo->instData == nil){
		if(geo->morphTargets[0].vertices == nil)
			return;	// source data was dropped, can't instance again
		// The GE drops triangles that reach too far outside the screen,
		// which makes big ground polygons vanish next to the camera.
		// They're split into smaller ones by the background thread; the
		// source data stays until that's done.
		bool split = tessMaxEdge > 0.0f && (geo->flags & 0x40000000) &&
		   pipe->instanceCB == defaultInstanceCB && geo->numMorphTargets == 1;
		header = instanceMesh(rwpipe, geo);
		pipe->instanceCB(geo, header, 0);
		if(split)
			tessQueue(rwpipe, geo, header);
		else{
			if(clipEnabled && pipe->instanceCB == defaultInstanceCB && geo->numMorphTargets == 1)
				buildChunks(header);
			if(geo->flags & 0x40000000)
				freeSourceData(geo);
		}
	}else if(geo->lockedSinceInst && geo->morphTargets[0].vertices)
		pipe->instanceCB(geo, header, 1);
	geo->lockedSinceInst = 0;
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	assert(0 && "can't uninstance");
}

void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);

// Models are converted for the GE the first time they're drawn (that's when
// the big ground polygons get split too). A model that just streamed in can
// take a few ms; several in one frame made the frame miss its 30 fps slot.
// So only so much converting per frame: what doesn't fit is drawn one or
// two frames later (it has only just arrived, nobody notices).
uint32 instanceBudgetUs = 3000;
uint32 instanceUsedUs;	// this frame, reset by the device at frame start
uint32 instanceDeferred;	// models put off (statistics)

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if(geo->instData == nil && !(geo->flags & Geometry::NATIVE) && pipe->instanceCB != skinInstanceCB){
		// (never the ground and buildings next to the camera: that would be a hole)
		if(instanceUsedUs >= instanceBudgetUs && !atomicNear(atomic, 60.0f)){
			instanceDeferred++;
			return;
		}
		uint32 t0 = sceKernelGetSystemTimeLow();
		pipe->instance(atomic);
		instanceUsedUs += sceKernelGetSystemTimeLow() - t0;
	}else
		pipe->instance(atomic);
	if(geo->instData == nil || geo->instData->platform != PLATFORM_PSP)
		return;
	if(((InstanceDataHeader*)geo->instData)->tessJob != -1)
		tessCheck(rwpipe, geo, (InstanceDataHeader*)geo->instData, atomic);
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_PSP);
	this->impl.instance = psp::instance;
	this->impl.uninstance = psp::uninstance;
	this->impl.render = psp::render;
	this->instanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

//
// Vertex layout helpers
//

struct VertLayout
{
	uint32 stride;
	int32 offWeights, offUV, offColor, offNormal, offPos;
	uint32 vtype;
};

static uint32
alignUp(uint32 x, uint32 a)
{
	return (x + a-1) & ~(a-1);
}

static void
makeLayout(VertLayout *l, Geometry *geo, int32 numWeights)
{
	uint32 off = 0;
	l->vtype = GU_VERTEX_32BITF | GU_TRANSFORM_3D;
	l->offWeights = l->offUV = l->offColor = l->offNormal = -1;
	if(numWeights){
		l->offWeights = off;
		off += numWeights;
		l->vtype |= GU_WEIGHTS(numWeights) | GU_WEIGHT_8BIT;
	}
	if(geo->numTexCoordSets > 0){
		off = alignUp(off, 4);
		l->offUV = off;
		off += 8;
		l->vtype |= GU_TEXTURE_32BITF;
	}
	if(geo->flags & Geometry::PRELIT){
		off = alignUp(off, 4);
		l->offColor = off;
		off += 4;
		l->vtype |= GU_COLOR_8888;
	}
	if(geo->flags & Geometry::NORMALS){
		l->offNormal = off;
		off += 3;
		l->vtype |= GU_NORMAL_8BIT;
	}
	off = alignUp(off, 4);
	l->offPos = off;
	off += 12;
	l->stride = alignUp(off, 4);
}

static inline int8
packNormal(float f)
{
	int32 i = (int32)(f*127.0f);
	if(i > 127) i = 127;
	if(i < -127) i = -127;
	return (int8)i;
}

static void
writeVertex(uint8 *dst, VertLayout *l, Geometry *geo, int32 i)
{
	if(l->offUV >= 0){
		TexCoords *tc = &geo->texCoords[0][i];
		float *uv = (float*)(dst + l->offUV);
		uv[0] = tc->u;
		uv[1] = tc->v;
	}
	if(l->offColor >= 0){
		RGBA *c = &geo->colors[i];
		*(uint32*)(dst + l->offColor) = c->red | c->green<<8 | c->blue<<16 | (uint32)c->alpha<<24;
	}
	if(l->offNormal >= 0){
		V3d *n = &geo->morphTargets[0].normals[i];
		int8 *pn = (int8*)(dst + l->offNormal);
		pn[0] = packNormal(n->x);
		pn[1] = packNormal(n->y);
		pn[2] = packNormal(n->z);
	}
	V3d *v = &geo->morphTargets[0].vertices[i];
	float *p = (float*)(dst + l->offPos);
	p[0] = v->x;
	p[1] = v->y;
	p[2] = v->z;
}

//
// Tessellation of large world triangles
//

struct TessVert
{
	V3d pos;
	V3d nrm;
	TexCoords uv;
	RGBA col;
};

static void
writeTessVertex(uint8 *dst, VertLayout *l, TessVert *v)
{
	if(l->offUV >= 0){
		float *uv = (float*)(dst + l->offUV);
		uv[0] = v->uv.u;
		uv[1] = v->uv.v;
	}
	if(l->offColor >= 0)
		*(uint32*)(dst + l->offColor) = v->col.red | v->col.green<<8 | v->col.blue<<16 | (uint32)v->col.alpha<<24;
	if(l->offNormal >= 0){
		int8 *pn = (int8*)(dst + l->offNormal);
		pn[0] = packNormal(v->nrm.x);
		pn[1] = packNormal(v->nrm.y);
		pn[2] = packNormal(v->nrm.z);
	}
	float *p = (float*)(dst + l->offPos);
	p[0] = v->pos.x;
	p[1] = v->pos.y;
	p[2] = v->pos.z;
}

struct TessBuilder
{
	TessVert *verts;
	uint32 numVerts, maxVerts;
	uint16 *idx;
	uint32 numIdx, maxIdx;
	// edge midpoint cache (open addressing)
	uint32 *edgeKey;
	uint16 *edgeVal;
	uint32 edgeMask;
	float32 maxEdgeSq;
	bool hasNormals, hasColors, hasUV;
	bool overflow;
};

static inline float32
distSq(const V3d &a, const V3d &b)
{
	float32 x = a.x-b.x, y = a.y-b.y, z = a.z-b.z;
	return x*x + y*y + z*z;
}

static int32
midpoint(TessBuilder *b, uint16 i0, uint16 i1)
{
	uint16 lo = i0 < i1 ? i0 : i1;
	uint16 hi = i0 < i1 ? i1 : i0;
	uint32 key = (uint32)lo<<16 | hi;
	uint32 h = (key * 2654435761u) & b->edgeMask;
	while(b->edgeKey[h] != 0xFFFFFFFF){
		if(b->edgeKey[h] == key)
			return b->edgeVal[h];
		h = (h+1) & b->edgeMask;
	}
	if(b->numVerts >= b->maxVerts){
		b->overflow = true;
		return -1;
	}
	TessVert *a = &b->verts[lo], *c = &b->verts[hi];
	TessVert *m = &b->verts[b->numVerts];
	m->pos.x = (a->pos.x + c->pos.x)*0.5f;
	m->pos.y = (a->pos.y + c->pos.y)*0.5f;
	m->pos.z = (a->pos.z + c->pos.z)*0.5f;
	if(b->hasNormals){
		V3d n = { a->nrm.x + c->nrm.x, a->nrm.y + c->nrm.y, a->nrm.z + c->nrm.z };
		float32 l = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
		if(l > 0.0001f){ n.x /= l; n.y /= l; n.z /= l; }
		m->nrm = n;
	}
	m->uv.u = (a->uv.u + c->uv.u)*0.5f;
	m->uv.v = (a->uv.v + c->uv.v)*0.5f;
	m->col.red = (a->col.red + c->col.red)/2;
	m->col.green = (a->col.green + c->col.green)/2;
	m->col.blue = (a->col.blue + c->col.blue)/2;
	m->col.alpha = (a->col.alpha + c->col.alpha)/2;
	b->edgeKey[h] = key;
	b->edgeVal[h] = b->numVerts;
	return b->numVerts++;
}

// split along the longest edge until all edges are short enough;
// the winding (a,b,c) is kept
static void
emitTriangle(TessBuilder *b, uint16 v0, uint16 v1, uint16 v2, int32 depth)
{
	if(b->overflow)
		return;
	float32 e0 = distSq(b->verts[v0].pos, b->verts[v1].pos);
	float32 e1 = distSq(b->verts[v1].pos, b->verts[v2].pos);
	float32 e2 = distSq(b->verts[v2].pos, b->verts[v0].pos);
	float32 emax = e0 > e1 ? (e0 > e2 ? e0 : e2) : (e1 > e2 ? e1 : e2);
	if(emax <= b->maxEdgeSq || depth >= 10){
		if(b->numIdx + 3 > b->maxIdx){
			b->overflow = true;
			return;
		}
		b->idx[b->numIdx++] = v0;
		b->idx[b->numIdx++] = v1;
		b->idx[b->numIdx++] = v2;
		return;
	}
	// rotate so the longest edge is v0-v1
	if(emax == e1){ uint16 t = v0; v0 = v1; v1 = v2; v2 = t; }
	else if(emax == e2){ uint16 t = v2; v2 = v1; v1 = v0; v0 = t; }
	int32 m = midpoint(b, v0, v1);
	if(m < 0)
		return;
	emitTriangle(b, v0, (uint16)m, v2, depth+1);
	emitTriangle(b, (uint16)m, v1, v2, depth+1);
}


// Scratch buffers for splitting, allocated once: allocating and freeing
// a megabyte for every streamed model fragmented the heap on the PSP.
#ifdef PSP_LITE
#define TESS_MAX_VERTS 8000
#define TESS_MAX_IDX 40000
#else
#define TESS_MAX_VERTS 16000
#define TESS_MAX_IDX 96000
#endif
#define TESS_EDGE_SLOTS 32768	// power of two >= 2*TESS_MAX_VERTS
static TessVert *tessScratchVerts;
static uint16 *tessScratchIdx;
static uint32 *tessScratchEdgeKey;
static uint16 *tessScratchEdgeVal;

static bool
tessScratchAlloc(void)
{
	if(tessScratchVerts)
		return true;
	tessScratchVerts = (TessVert*)__real_malloc(TESS_MAX_VERTS*sizeof(TessVert));
	tessScratchIdx = (uint16*)__real_malloc(TESS_MAX_IDX*2);
	tessScratchEdgeKey = (uint32*)__real_malloc(TESS_EDGE_SLOTS*4);
	tessScratchEdgeVal = (uint16*)__real_malloc(TESS_EDGE_SLOTS*2);
	if(tessScratchVerts && tessScratchIdx && tessScratchEdgeKey && tessScratchEdgeVal)
		return true;
	free(tessScratchVerts); free(tessScratchIdx); free(tessScratchEdgeKey); free(tessScratchEdgeVal);
	tessScratchVerts = nil; tessScratchIdx = nil; tessScratchEdgeKey = nil; tessScratchEdgeVal = nil;
	return false;
}

// 16 byte aligned, outside the game's out-of-memory guard: when it fails
// the model is just drawn unsplit
static void*
malloc16Quiet(uint32 size)
{
	uint8 *p = (uint8*)__real_malloc(size + 16 + sizeof(void*));
	if(p == nil)
		return nil;
	uint8 *a = (uint8*)(((uintptr)p + sizeof(void*) + 15) & ~(uintptr)15);
	((void**)a)[-1] = p;
	return a;
}

static InstanceDataHeader*
tessTry(rw::ObjPipeline *rwpipe, Geometry *geo, float32 maxEdge, bool *tooBig)
{
	MeshHeader *meshh = geo->meshHeader;
	MorphTarget *mt = &geo->morphTargets[0];
	if(meshh == nil || mt->vertices == nil || geo->numVertices <= 0)
		return nil;
	float32 maxEdgeSq = maxEdge*maxEdge;
	bool strip = meshh->flags == MeshHeader::TRISTRIP;

	// anything to split at all, and roughly into how many triangles?
	// (splitting the longest edge until all are below maxEdge gives about
	// (edge/maxEdge)^2 triangles; twice that leaves room for the rest)
	bool needed = false;
	uint32 estTris = 0;
	Mesh *mesh = meshh->getMeshes();
	for(uint32 i = 0; i < meshh->numMeshes; i++, mesh++){
		uint32 n = mesh->numIndices;
		uint32 ntri = strip ? (n >= 3 ? n-2 : 0) : n/3;
		for(uint32 t = 0; t < ntri; t++){
			uint16 a, b, c;
			if(strip){ a = mesh->indices[t]; b = mesh->indices[t+1]; c = mesh->indices[t+2]; }
			else{ a = mesh->indices[t*3]; b = mesh->indices[t*3+1]; c = mesh->indices[t*3+2]; }
			float32 e0 = distSq(mt->vertices[a], mt->vertices[b]);
			float32 e1 = distSq(mt->vertices[b], mt->vertices[c]);
			float32 e2 = distSq(mt->vertices[c], mt->vertices[a]);
			float32 emax = e0 > e1 ? (e0 > e2 ? e0 : e2) : (e1 > e2 ? e1 : e2);
			if(emax > maxEdgeSq){
				needed = true;
				float32 r = emax/maxEdgeSq;	// (edge/maxEdge)^2
				if(r > 1024.0f) r = 1024.0f;	// split depth is limited to 10
				estTris += (uint32)(2.0f*r) + 2;
			}else
				estTris++;
		}
	}
	if(!needed)
		return nil;

	TessBuilder b;
	b.maxVerts = geo->numVertices + estTris + 64;
	b.maxIdx = estTris*3 + 96;
	if(b.maxVerts > TESS_MAX_VERTS || b.maxIdx > TESS_MAX_IDX){
		*tooBig = true;		// try again with bigger pieces
		return nil;
	}
	b.edgeMask = 1;
	while(b.edgeMask < b.maxVerts*2) b.edgeMask <<= 1;
	b.edgeMask -= 1;
	b.verts = tessScratchVerts;
	b.idx = tessScratchIdx;
	b.edgeKey = tessScratchEdgeKey;
	b.edgeVal = tessScratchEdgeVal;
	memset(b.edgeKey, 0xFF, (b.edgeMask+1)*4);
	b.maxEdgeSq = maxEdgeSq;
	b.hasNormals = !!(geo->flags & Geometry::NORMALS) && mt->normals;
	b.hasColors = !!(geo->flags & Geometry::PRELIT) && geo->colors;
	b.hasUV = geo->numTexCoordSets > 0 && geo->texCoords[0];
	b.overflow = false;
	b.numIdx = 0;
	b.numVerts = geo->numVertices;
	for(int32 i = 0; i < geo->numVertices; i++){
		TessVert *v = &b.verts[i];
		v->pos = mt->vertices[i];
		if(b.hasNormals) v->nrm = mt->normals[i];
		else v->nrm.x = v->nrm.y = v->nrm.z = 0.0f;
		if(b.hasUV) v->uv = geo->texCoords[0][i];
		else v->uv.u = v->uv.v = 0.0f;
		if(b.hasColors) v->col = geo->colors[i];
		else v->col.red = v->col.green = v->col.blue = v->col.alpha = 255;
	}

	// per mesh index ranges
	uint32 *meshStart = rwNewT(uint32, meshh->numMeshes+1, MEMDUR_FUNCTION | ID_GEOMETRY);
	mesh = meshh->getMeshes();
	for(uint32 i = 0; i < meshh->numMeshes; i++, mesh++){
		meshStart[i] = b.numIdx;
		uint32 n = mesh->numIndices;
		uint32 ntri = strip ? (n >= 3 ? n-2 : 0) : n/3;
		for(uint32 t = 0; t < ntri; t++){
			uint16 v0, v1, v2;
			if(strip){
				v0 = mesh->indices[t]; v1 = mesh->indices[t+1]; v2 = mesh->indices[t+2];
				if(t & 1){ uint16 tmp = v1; v1 = v2; v2 = tmp; }
			}else{
				v0 = mesh->indices[t*3]; v1 = mesh->indices[t*3+1]; v2 = mesh->indices[t*3+2];
			}
			if(v0 == v1 || v1 == v2 || v2 == v0)
				continue;	// strip stitching
			emitTriangle(&b, v0, v1, v2, 0);
		}
	}
	meshStart[meshh->numMeshes] = b.numIdx;

	if(b.overflow)
		*tooBig = true;

	InstanceDataHeader *header = nil;
	VertLayout l;
	makeLayout(&l, geo, 0);
	uint16 *tessIdx = nil;
	uint8 *tessVerts = nil;
	if(!b.overflow && b.numIdx > 0){
		tessIdx = (uint16*)malloc16Quiet(b.numIdx*2 + 2);
		tessVerts = (uint8*)malloc16Quiet(b.numVerts*l.stride);
		if(tessIdx == nil || tessVerts == nil){
			// no room: draw the model unsplit instead of crashing
			free16(tessIdx);
			free16(tessVerts);
			tessIdx = nil;
			tessStats[3]++;
		}
	}
	if(tessIdx){
		// original indices for normal drawing, the split triangles only
		// close to the camera (they cost the GE a lot more)
		header = instanceMesh(rwpipe, geo, false);
		header->totalNumVertex = b.numVerts;
		header->tessIndexBuffer = tessIdx;
		memcpy(header->tessIndexBuffer, b.idx, b.numIdx*2);

		header->stride = l.stride;
		header->vertexType = l.vtype | GU_INDEX_16BIT;
		header->vertexBuffer = tessVerts;
		for(uint32 i = 0; i < b.numVerts; i++)
			writeTessVertex(header->vertexBuffer + i*l.stride, &l, &b.verts[i]);

		mesh = meshh->getMeshes();
		InstanceData *inst = header->inst;
		for(uint32 i = 0; i < header->numMeshes; i++, inst++, mesh++){
			inst->tessIndexOffset = meshStart[i];
			inst->tessNumIndex = meshStart[i+1] - meshStart[i];
			inst->vertexAlpha = 0;
			if(b.hasColors)
				for(uint32 j = 0; j < inst->tessNumIndex; j++)
					if(b.verts[b.idx[inst->tessIndexOffset+j]].col.alpha != 0xFF){
						inst->vertexAlpha = 1;
						break;
					}
		}
		sceKernelDcacheWritebackRange(header->tessIndexBuffer, b.numIdx*2);
		sceKernelDcacheWritebackRange(header->vertexBuffer, b.numVerts*l.stride);
		tessStats[0]++;
		tessStats[1] += geo->numVertices;
		tessStats[2] += b.numVerts;
	}
	rwFree(meshStart);
	if(header && clipEnabled)
		buildChunks(header);
	return header;
}

// CPU clipping by area: the triangles of big models (ground, roads) are
// sorted into the cells of a grid of at most 8x8 over the model's x/y
// extent, so the clipping only has to look at the cells next to the camera.
// Uses the split triangle list if there is one, else makes a list from the
// model's own triangles.
static void
buildChunks(InstanceDataHeader *header)
{
	if(header->chunks || header->numMeshes == 0)
		return;
	if((header->vertexType & (GU_VERTEX_BITS|GU_WEIGHTS_BITS)) != GU_VERTEX_32BITF)
		return;
	bool haveList = header->tessIndexBuffer != nil;
	bool strip = header->primType == GU_TRIANGLE_STRIP;
	uint32 stride = header->stride;
	const uint8 *vb = header->vertexBuffer + stride - 12;
#define POS(i) ((const float32*)(vb + (i)*stride))
	// triangles per mesh
	uint32 totalTris = 0;
	InstanceData *inst = header->inst;
	for(uint32 m = 0; m < header->numMeshes; m++, inst++){
		if(haveList)
			totalTris += inst->tessNumIndex/3;
		else if(strip){
			const uint16 *ind = header->indexBuffer + inst->indexOffset;
			for(uint32 t = 0; t+2 < inst->numIndex; t++)
				if(ind[t] != ind[t+1] && ind[t+1] != ind[t+2] && ind[t] != ind[t+2])
					totalTris++;
		}else
			totalTris += inst->numIndex/3;
	}
	if(totalTris < 48 || totalTris > 30000)
		return;
	float32 minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
	for(uint32 i = 0; i < header->totalNumVertex; i++){
		const float32 *p = POS(i);
		if(p[0] < minx) minx = p[0]; if(p[0] > maxx) maxx = p[0];
		if(p[1] < miny) miny = p[1]; if(p[1] > maxy) maxy = p[1];
	}
	float32 ext = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
	if(ext < 10.0f)
		return;	// small: checking it whole is cheap enough
	float32 cell = ext/8.0f;
	if(cell < 5.0f) cell = 5.0f;
	int32 w = (int32)((maxx - minx)/cell) + 1, h = (int32)((maxy - miny)/cell) + 1;
	if(w > 8) w = 8;
	if(h > 8) h = 8;

	uint16 *list = haveList ? header->tessIndexBuffer : (uint16*)malloc16Quiet(totalTris*6 + 2);
	uint16 *sorted = (uint16*)malloc16Quiet(totalTris*6 + 2);
	uint8 *cellOf = (uint8*)malloc16Quiet(totalTris + 4);
	TessChunk *tmpChunks = (TessChunk*)malloc16Quiet(header->numMeshes*64*sizeof(TessChunk));
	if(list == nil || sorted == nil || cellOf == nil || tmpChunks == nil){
		if(!haveList) free16(list);
		free16(sorted); free16(cellOf); free16(tmpChunks);
		return;
	}
	// without a split list: the model's triangles as a list
	if(!haveList){
		uint32 pos = 0;
		inst = header->inst;
		for(uint32 m = 0; m < header->numMeshes; m++, inst++){
			const uint16 *ind = header->indexBuffer + inst->indexOffset;
			inst->tessIndexOffset = pos;
			if(strip){
				for(uint32 t = 0; t+2 < inst->numIndex; t++){
					uint16 a = ind[t], b = ind[t+1], c = ind[t+2];
					if(a == b || b == c || a == c) continue;
					if(t & 1){ uint16 x = a; a = b; b = x; }
					list[pos++] = a; list[pos++] = b; list[pos++] = c;
				}
			}else{
				memcpy(list + pos, ind, (inst->numIndex/3*3)*2);
				pos += inst->numIndex/3*3;
			}
			inst->tessNumIndex = pos - inst->tessIndexOffset;
		}
	}
	float32 lo[64][3], hi[64][3];
	for(int c = 0; c < 64; c++){
		lo[c][0] = lo[c][1] = lo[c][2] = 1e30f;
		hi[c][0] = hi[c][1] = hi[c][2] = -1e30f;
	}
	uint32 numChunks = 0;
	inst = header->inst;
	for(uint32 m = 0; m < header->numMeshes; m++, inst++){
		uint32 i0 = inst->tessIndexOffset, nt = inst->tessNumIndex/3;
		inst->firstChunk = numChunks;
		inst->numChunks = 0;
		if(nt == 0) continue;
		uint32 count[64];
		memset(count, 0, sizeof(count));
		for(uint32 t = 0; t < nt; t++){
			const uint16 *tri = &list[i0 + t*3];
			const float32 *a = POS(tri[0]), *b = POS(tri[1]), *c = POS(tri[2]);
			float32 cx = (a[0] + b[0] + c[0])*(1.0f/3.0f), cy = (a[1] + b[1] + c[1])*(1.0f/3.0f);
			int32 gx = (int32)((cx - minx)/cell), gy = (int32)((cy - miny)/cell);
			if(gx < 0) gx = 0; if(gx >= w) gx = w-1;
			if(gy < 0) gy = 0; if(gy >= h) gy = h-1;
			uint32 k = gy*8 + gx;
			cellOf[t] = k;
			count[k]++;
			for(int v = 0; v < 3; v++){
				const float32 *p = POS(tri[v]);
				for(int j = 0; j < 3; j++){
					if(p[j] < lo[k][j]) lo[k][j] = p[j];
					if(p[j] > hi[k][j]) hi[k][j] = p[j];
				}
			}
		}
		uint32 start[64], fill[64], pos = 0;
		for(int c = 0; c < 64; c++){ start[c] = fill[c] = pos; pos += count[c]; }
		for(uint32 t = 0; t < nt; t++){
			uint32 d = (fill[cellOf[t]]++)*3;
			sorted[d] = list[i0 + t*3];
			sorted[d+1] = list[i0 + t*3 + 1];
			sorted[d+2] = list[i0 + t*3 + 2];
		}
		memcpy(&list[i0], sorted, nt*6);
		for(int c = 0; c < 64; c++){
			if(count[c] == 0) continue;
			TessChunk *ch = &tmpChunks[numChunks++];
			ch->first = i0 + start[c]*3;
			ch->count = count[c]*3;
			ch->cell = c;
			inst->numChunks++;
		}
	}
	// the cells in use, numbered compactly
	uint8 cellIdx[64];
	uint32 numCells = 0;
	for(int c = 0; c < 64; c++)
		cellIdx[c] = lo[c][0] <= hi[c][0] ? numCells++ : 0xFF;
	TessCell *cells = (TessCell*)malloc16Quiet(numCells*sizeof(TessCell));
	TessChunk *chunks = (TessChunk*)malloc16Quiet(numChunks*sizeof(TessChunk));
	if(cells && chunks){
		for(int c = 0; c < 64; c++){
			if(cellIdx[c] == 0xFF) continue;
			TessCell *tc = &cells[cellIdx[c]];
			tc->cx = (lo[c][0]+hi[c][0])*0.5f; tc->hx = (hi[c][0]-lo[c][0])*0.5f;
			tc->cy = (lo[c][1]+hi[c][1])*0.5f; tc->hy = (hi[c][1]-lo[c][1])*0.5f;
			tc->cz = (lo[c][2]+hi[c][2])*0.5f; tc->hz = (hi[c][2]-lo[c][2])*0.5f;
		}
		for(uint32 k = 0; k < numChunks; k++){
			chunks[k] = tmpChunks[k];
			chunks[k].cell = cellIdx[tmpChunks[k].cell];
		}
		sceKernelDcacheWritebackRange(list, totalTris*6);
		header->tessIndexBuffer = list;
		header->chunks = chunks;
		header->cells = cells;
		header->numCells = numCells;
		clipStats[9] += totalTris;
	}else{
		free16(cells);
		free16(chunks);
		if(!haveList){
			free16(list);
			inst = header->inst;
			for(uint32 m = 0; m < header->numMeshes; m++, inst++)
				inst->tessIndexOffset = inst->tessNumIndex = 0;
		}
		inst = header->inst;
		for(uint32 m = 0; m < header->numMeshes; m++, inst++)
			inst->numChunks = 0;
	}
	free16(sorted); free16(cellOf); free16(tmpChunks);
#undef POS
}

static InstanceDataHeader*
instanceTessellated(rw::ObjPipeline *rwpipe, Geometry *geo, float32 maxEdge)
{
	if(!tessScratchAlloc())
		return nil;
	// huge polygons that would need more vertices than the scratch buffers
	// hold are split into bigger pieces instead of not at all
	for(int32 k = 0; k < 8; k++){	// (up to 6 m x 1.4^7 = 63 m pieces: still better than a hole)
		bool tooBig = false;
		InstanceDataHeader *h = tessTry(rwpipe, geo, maxEdge, &tooBig);
		if(h || !tooBig)
			return h;
		maxEdge *= 1.4f;
	}
	tessStats[3]++;
	return nil;
}

//
// Default pipeline
//

void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	VertLayout l;
	makeLayout(&l, geo, 0);
	if(!reinstance){
		header->stride = l.stride;
		header->vertexType = l.vtype | GU_INDEX_16BIT;
		header->vertexBuffer = (uint8*)malloc16(header->totalNumVertex*l.stride);
	}
	uint8 *verts = header->vertexBuffer;
	for(uint32 i = 0; i < header->totalNumVertex; i++)
		writeVertex(verts + i*l.stride, &l, geo, i);

	// vertex alpha per mesh
	InstanceData *inst = header->inst;
	for(uint32 n = 0; n < header->numMeshes; n++, inst++){
		inst->vertexAlpha = 0;
		if(geo->flags & Geometry::PRELIT)
			for(int32 j = 0; j < inst->numVertices; j++)
				if(geo->colors[inst->minVert + j].alpha != 0xFF){
					inst->vertexAlpha = 1;
					break;
				}
	}
	sceKernelDcacheWritebackRange(header->vertexBuffer, header->totalNumVertex*l.stride);
}

float32 tessNearDist = 12.0f;
static bool32 drawTess;	// current atomic uses the split triangles

// use the split triangles if the model is close to the camera
static void
chooseTess(Atomic *atomic, InstanceDataHeader *header)
{
	drawTess = 0;
	if(header->tessIndexBuffer == nil || engine->currentCamera == nil)
		return;
	Sphere *s = atomic->getWorldBoundingSphere();
	V3d *cp = &((Camera*)engine->currentCamera)->getFrame()->getLTM()->pos;
	float32 dx = s->center.x - cp->x, dy = s->center.y - cp->y, dz = s->center.z - cp->z;
	float32 r = s->radius + tessNearDist;
	drawTess = dx*dx + dy*dy + dz*dz < r*r;
}

//
// CPU clipping. The GE drops every triangle with a corner that lands more
// than ~2000 pixels outside the screen, and corners close to or behind the
// camera plane always do: big ground polygons next to the camera vanished
// (holes in the ground). The models that reach into that zone have their
// triangles checked on the CPU: the ones the GE would drop are clipped to
// a "guard band" frustum here and drawn as new triangles, the rest go to
// the GE as they are.
//
int32 clipEnabled = 1;
uint32 clipStats[10];
float32 clipGuardPx = 1800.0f;	// how far outside the screen a corner may go (pixels)

static struct {
	bool active;	// the current atomic needs checking
	float32 m[12];	// model -> view (3x3 + translation)
	float32 gx, gy, nearZ;
	float32 vx, vy;	// the visible frustum's slopes (with a little margin)
	uint32 stamp;
	// per camera
	uint32 camSerial;
	float32 view[16], nx, ny;
	// per atomic: |m| for the box tests, cell results
	float32 am[9];
	InstanceDataHeader *cellsOf;
	uint8 cellRes[64];
	uint8 cellDone[64];
} clip;

// per vertex of the current atomic: view position and outcode
struct ClipVert { float32 x, y, z; uint16 stamp; uint16 code; };
static ClipVert *clipCache;
static uint32 clipCacheSize;
#define CLIP_MAX_VERTS 16000

// bits 0-4: outside the near plane / the guard band (the GE would drop the
// triangle), bits 5-8: outside the visible frustum (a triangle completely
// outside one of these planes isn't seen at all)
enum { CL_NEAR = 1, CL_RIGHT = 2, CL_LEFT = 4, CL_TOP = 8, CL_BOTTOM = 16,
	CL_GUARD = 31,
	CL_VRIGHT = 32, CL_VLEFT = 64, CL_VTOP = 128, CL_VBOTTOM = 256,
	CL_INVISIBLE = CL_NEAR | CL_VRIGHT | CL_VLEFT | CL_VTOP | CL_VBOTTOM };

static inline uint32
clipCode(float32 x, float32 y, float32 z)
{
	uint32 c = 0;
	if(z < clip.nearZ) c |= CL_NEAR;
	float32 gzx = clip.gx*z, gzy = clip.gy*z;
	if(x > gzx) c |= CL_RIGHT;
	if(-x > gzx) c |= CL_LEFT;
	if(y > gzy) c |= CL_TOP;
	if(-y > gzy) c |= CL_BOTTOM;
	float32 vzx = clip.vx*z, vzy = clip.vy*z;
	if(x > vzx) c |= CL_VRIGHT;
	if(-x > vzx) c |= CL_VLEFT;
	if(y > vzy) c |= CL_VTOP;
	if(-y > vzy) c |= CL_VBOTTOM;
	return c;
}

// decide whether the atomic needs the CPU check (its bounding sphere reaches
// out of the guard band frustum)
static void
clipSetup(Atomic *atomic)
{
	clip.active = false;
	if(!clipEnabled || engine->currentCamera == nil)
		return;
	clipStats[0]++;
	float32 *v = clip.view;
	if(clip.camSerial != camSerial){
		float32 px, py;
		getClipCamera(v, &px, &py, &clip.nearZ);
		clip.nearZ *= 1.01f;
		clip.gx = clipGuardPx/(240.0f*px);
		clip.gy = clipGuardPx/(136.0f*py);
		clip.vx = 1.05f/px;
		clip.vy = 1.05f/py;
		clip.nx = 1.0f/sqrtf(1.0f + clip.gx*clip.gx);
		clip.ny = 1.0f/sqrtf(1.0f + clip.gy*clip.gy);
		clip.camSerial = camSerial;
	}
	Sphere *s = atomic->getWorldBoundingSphere();
	float32 cx = s->center.x - camWorldPos.x, cy = s->center.y - camWorldPos.y, cz = s->center.z - camWorldPos.z;
	float32 vx = cx*v[0] + cy*v[4] + cz*v[8];
	float32 vy = cx*v[1] + cy*v[5] + cz*v[9];
	float32 vz = cx*v[2] + cy*v[6] + cz*v[10];
	float32 r = s->radius;
	// completely inside: the GE can take it as it is
	float32 nx = clip.nx, ny = clip.ny;
	if(vz - r > clip.nearZ &&
	   (vx - clip.gx*vz)*nx < -r && (-vx - clip.gx*vz)*nx < -r &&
	   (vy - clip.gy*vz)*ny < -r && (-vy - clip.gy*vz)*ny < -r)
		return;
	// completely behind the camera: nothing to draw anyway
	if(vz + r < clip.nearZ)
		return;
	Geometry *geo = atomic->geometry;
	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(header == nil || header->totalNumVertex > CLIP_MAX_VERTS ||
	   (header->vertexType & (GU_VERTEX_BITS|GU_WEIGHTS_BITS)) != GU_VERTEX_32BITF)
		return;
	if(clipCacheSize < header->totalNumVertex){
		// (grows in steps of 2048 vertices up to the biggest model seen)
		uint32 n = (header->totalNumVertex + 2047) & ~2047u;
		ClipVert *c = (ClipVert*)malloc(n*sizeof(ClipVert));
		if(c == nil)
			return;
		memset(c, 0, n*sizeof(ClipVert));
		free(clipCache);
		clipCache = c;
		clipCacheSize = n;
	}
	// model -> camera relative world -> view
	Matrix *ltm = atomic->getFrame()->getLTM();
	float32 rx = ltm->pos.x - camWorldPos.x, ry = ltm->pos.y - camWorldPos.y, rz = ltm->pos.z - camWorldPos.z;
	const V3d *row[3] = { &ltm->right, &ltm->up, &ltm->at };
	for(int i = 0; i < 3; i++){
		clip.m[i*3+0] = row[i]->x*v[0] + row[i]->y*v[4] + row[i]->z*v[8];
		clip.m[i*3+1] = row[i]->x*v[1] + row[i]->y*v[5] + row[i]->z*v[9];
		clip.m[i*3+2] = row[i]->x*v[2] + row[i]->y*v[6] + row[i]->z*v[10];
	}
	clip.m[9] = rx*v[0] + ry*v[4] + rz*v[8];
	clip.m[10] = rx*v[1] + ry*v[5] + rz*v[9];
	clip.m[11] = rx*v[2] + ry*v[6] + rz*v[10];
	if(++clip.stamp >= 0xFFFF){
		for(uint32 i = 0; i < clipCacheSize; i++) clipCache[i].stamp = 0;
		clip.stamp = 1;
	}
	for(int i = 0; i < 9; i++)
		clip.am[i] = fabsf(clip.m[i]);
	clip.cellsOf = nil;
	clip.active = true;
	clipStats[1]++;
}

static inline ClipVert*
clipVertex(InstanceDataHeader *header, uint32 i)
{
	ClipVert *c = &clipCache[i];
	if(c->stamp != clip.stamp){
		const float32 *p = (const float32*)(header->vertexBuffer + i*header->stride + header->stride - 12);
		const float32 *m = clip.m;
		c->x = p[0]*m[0] + p[1]*m[3] + p[2]*m[6] + m[9];
		c->y = p[0]*m[1] + p[1]*m[4] + p[2]*m[7] + m[10];
		c->z = p[0]*m[2] + p[1]*m[5] + p[2]*m[8] + m[11];
		c->code = clipCode(c->x, c->y, c->z);
		c->stamp = clip.stamp;
		clipStats[4]++;
	}
	return c;
}

// a corner of a clipped polygon: view position + the vertex data
struct ClipPoly { float32 x, y, z; uint8 data[48]; };

static inline float32
clipDist(const ClipPoly *p, int plane)
{
	switch(plane){
	case 0: return p->z - clip.nearZ;
	case 1: return clip.gx*p->z - p->x;
	case 2: return clip.gx*p->z + p->x;
	case 3: return clip.gy*p->z - p->y;
	default: return clip.gy*p->z + p->y;
	}
}

// vertex data in between a and b (layout: see makeLayout)
static void
clipLerp(ClipPoly *out, const ClipPoly *a, const ClipPoly *b, float32 t, uint32 vtype, uint32 stride)
{
	out->x = a->x + (b->x - a->x)*t;
	out->y = a->y + (b->y - a->y)*t;
	out->z = a->z + (b->z - a->z)*t;
	uint32 off = 0;
	if(vtype & GU_TEXTURE_BITS){
		const float32 *ua = (const float32*)(a->data + off), *ub = (const float32*)(b->data + off);
		float32 *uo = (float32*)(out->data + off);
		uo[0] = ua[0] + (ub[0] - ua[0])*t;
		uo[1] = ua[1] + (ub[1] - ua[1])*t;
		off += 8;
	}
	if(vtype & GU_COLOR_BITS){
		for(int k = 0; k < 4; k++)
			out->data[off+k] = (uint8)(a->data[off+k] + ((int32)b->data[off+k] - (int32)a->data[off+k])*t + 0.5f);
		off += 4;
	}
	if(vtype & GU_NORMAL_BITS){
		for(int k = 0; k < 3; k++)
			((int8*)out->data)[off+k] = (int8)(((int8*)a->data)[off+k] + (((int8*)b->data)[off+k] - ((int8*)a->data)[off+k])*t);
		off += 3;
	}
	off = stride - 12;
	const float32 *pa = (const float32*)(a->data + off), *pb = (const float32*)(b->data + off);
	float32 *po = (float32*)(out->data + off);
	po[0] = pa[0] + (pb[0] - pa[0])*t;
	po[1] = pa[1] + (pb[1] - pa[1])*t;
	po[2] = pa[2] + (pb[2] - pa[2])*t;
}

// clip a triangle, write the result as a fan of triangles; returns the vertex count
static uint32
clipTriangle(InstanceDataHeader *header, uint16 i0, uint16 i1, uint16 i2, uint32 codes, uint8 *out)
{
	static ClipPoly pool[24];	// the 3 corners + the new ones
	ClipPoly *bufA[10], *bufB[10];
	ClipPoly **in = bufA, **res = bufB;
	uint32 stride = header->stride, vtype = header->vertexType;
	uint16 idx[3] = { i0, i1, i2 };
	for(int k = 0; k < 3; k++){
		ClipVert *c = &clipCache[idx[k]];
		ClipPoly *p = &pool[k];
		p->x = c->x; p->y = c->y; p->z = c->z;
		memcpy(p->data, header->vertexBuffer + idx[k]*stride, stride);
		in[k] = p;
	}
	int np = 3;
	int n = 3;
	for(int plane = 0; plane < 5 && n >= 3; plane++){
		if(!(codes & (1<<plane)))
			continue;
		int m = 0;
		float32 da = clipDist(in[0], plane);
		for(int k = 0; k < n; k++){
			ClipPoly *a = in[k], *b = in[k+1 < n ? k+1 : 0];
			float32 db = clipDist(b, plane);
			if(da >= 0.0f)
				res[m++] = a;
			if((da >= 0.0f) != (db >= 0.0f) && np < 24 && m < 10){
				ClipPoly *o = &pool[np++];
				clipLerp(o, a, b, da/(da - db), vtype, stride);
				res[m++] = o;
			}
			if(m >= 10) break;
			da = db;
		}
		ClipPoly **t = in; in = res; res = t;
		n = m;
	}
	if(n < 3)
		return 0;
	uint32 nv = 0;
	for(int k = 1; k+1 < n; k++){
		memcpy(out + (nv++)*stride, in[0]->data, stride);
		memcpy(out + (nv++)*stride, in[k]->data, stride);
		memcpy(out + (nv++)*stride, in[k+1]->data, stride);
	}
	return nv;
}

// check and draw a range of triangles (strip or list)
static void
drawClippedRange(InstanceDataHeader *header, const uint16 *ind, uint32 ni, bool strip, int32 vtype)
{
	uint32 numTris = strip ? (ni >= 3 ? ni-2 : 0) : ni/3;
	if(numTris == 0)
		return;
	clipStats[strip ? 7 : 8] += numTris;
	// first pass: classify, count
	uint32 keep = 0, cut = 0;
	for(uint32 t = 0; t < numTris; t++){
		uint16 a, b, c;
		if(strip){ a = ind[t]; b = ind[t+1]; c = ind[t+2]; if(a == b || b == c || a == c) continue; }
		else{ a = ind[t*3]; b = ind[t*3+1]; c = ind[t*3+2]; }
		uint32 ca = clipVertex(header, a)->code, cb = clipVertex(header, b)->code, cc = clipVertex(header, c)->code;
		if((ca & cb & cc) & CL_INVISIBLE) continue;
		if(((ca | cb | cc) & CL_GUARD) == 0) keep++;
		else cut++;
	}
	if(cut == 0){
		// nothing the GE would drop: as it is
		drawArray(strip ? GU_TRIANGLE_STRIP : GU_TRIANGLES, vtype, ni, ind, header->vertexBuffer);
		return;
	}
	uint32 stride = header->stride;
	uint16 *outIdx = keep ? (uint16*)allocFrameMemory(keep*3*2) : nil;
	uint8 *outVert = (uint8*)allocFrameMemory(cut*24*stride);	// at most 8 corners -> 6 triangles
	if(outVert == nil || keep && outIdx == nil)
		return;
	uint32 nk = 0, nv = 0;
	for(uint32 t = 0; t < numTris; t++){
		uint16 a, b, c;
		if(strip){
			a = ind[t]; b = ind[t+1]; c = ind[t+2];
			if(a == b || b == c || a == c) continue;
			if(t & 1){ uint16 x = a; a = b; b = x; }	// keep the winding
		}else{ a = ind[t*3]; b = ind[t*3+1]; c = ind[t*3+2]; }
		uint32 ca = clipCache[a].code, cb = clipCache[b].code, cc = clipCache[c].code;
		if((ca & cb & cc) & CL_INVISIBLE){
			clipStats[3]++;
		}else if(((ca | cb | cc) & CL_GUARD) == 0){
			outIdx[nk++] = a; outIdx[nk++] = b; outIdx[nk++] = c;
		}else{
			nv += clipTriangle(header, a, b, c, (ca | cb | cc) & CL_GUARD, outVert + nv*stride);
			clipStats[2]++;
		}
	}
	if(nk)
		drawArray(GU_TRIANGLES, vtype, nk, outIdx, header->vertexBuffer);
	if(nv)
		drawArray(GU_TRIANGLES, vtype & ~GU_INDEX_BITS, nv, nil, outVert);
}

// where is a box (model space) for the GE: 0 inside the guard band, 1 partly
// (needs the check), 2 completely outside the view (nothing to draw)
static int
cellTest(const TessCell *ch)
{
	const float32 *m = clip.m, *am = clip.am;
	float32 x = ch->cx*m[0] + ch->cy*m[3] + ch->cz*m[6] + m[9];
	float32 y = ch->cx*m[1] + ch->cy*m[4] + ch->cz*m[7] + m[10];
	float32 z = ch->cx*m[2] + ch->cy*m[5] + ch->cz*m[8] + m[11];
	float32 ex = ch->hx*am[0] + ch->hy*am[3] + ch->hz*am[6];
	float32 ey = ch->hx*am[1] + ch->hy*am[4] + ch->hz*am[7];
	float32 ez = ch->hx*am[2] + ch->hy*am[5] + ch->hz*am[8];
	if(z + ez < clip.nearZ)
		return 2;
	float32 vzx = clip.vx*z, vrx = ex + clip.vx*ez;
	float32 vzy = clip.vy*z, vry = ey + clip.vy*ez;
	if(x - vzx > vrx || -x - vzx > vrx || y - vzy > vry || -y - vzy > vry)
		return 2;
	float32 gzx = clip.gx*z, rx = ex + clip.gx*ez;
	float32 gzy = clip.gy*z, ry = ey + clip.gy*ez;
	if(z - ez > clip.nearZ && x - gzx < -rx && -x - gzx < -rx && y - gzy < -ry && -y - gzy < -ry)
		return 0;
	return 1;
}

static void
drawClipped(InstanceDataHeader *header, InstanceData *inst, int32 vtype)
{
	if(inst->numChunks == 0 || header->chunks == nil){
		drawClippedRange(header, header->indexBuffer + inst->indexOffset, inst->numIndex,
			header->primType == GU_TRIANGLE_STRIP, vtype);
		return;
	}
	// by area: the cells inside the guard band go to the GE as they are
	// (neighbours in one draw), only the others are checked; the cells are
	// tested once per model, not per mesh
	if(clip.cellsOf != header){
		clip.cellsOf = header;
		memset(clip.cellDone, 0, sizeof(clip.cellDone));
	}
	const uint16 *ind = header->tessIndexBuffer;
	const TessChunk *ch = header->chunks + inst->firstChunk;
	uint32 runFirst = 0, runCount = 0;
	for(uint32 k = 0; k < inst->numChunks; k++, ch++){
		uint32 c = ch->cell;
		if(!clip.cellDone[c]){
			clip.cellRes[c] = cellTest(&header->cells[c]);
			clip.cellDone[c] = 1;
			clipStats[5]++;
			if(clip.cellRes[c] == 1) clipStats[6]++;
		}
		int r = clip.cellRes[c];
		if(r == 0 && runCount && runFirst + runCount == ch->first){
			runCount += ch->count;
			continue;
		}
		if(runCount)
			drawArray(GU_TRIANGLES, vtype, runCount, ind + runFirst, header->vertexBuffer);
		runCount = 0;
		if(r == 0){
			runFirst = ch->first;
			runCount = ch->count;
		}else if(r == 1)
			drawClippedRange(header, ind + ch->first, ch->count, false, vtype);
	}
	if(runCount)
		drawArray(GU_TRIANGLES, vtype, runCount, ind + runFirst, header->vertexBuffer);
}

static void
drawIndexed(InstanceDataHeader *header, InstanceData *inst, int32 vtype)
{
	if(clip.active){
		drawClipped(header, inst, vtype);
		return;
	}
	if(drawTess && !clipEnabled)
		drawArray(GU_TRIANGLES, vtype, inst->tessNumIndex,
			header->tessIndexBuffer + inst->tessIndexOffset, header->vertexBuffer);
	else
		drawArray(header->primType, vtype, inst->numIndex,
			header->indexBuffer + inst->indexOffset, header->vertexBuffer);
}

static void
drawMesh(InstanceDataHeader *header, InstanceData *inst)
{
	prepareGeometryDraw();
	drawIndexed(header, inst, header->vertexType);
	drawCalls++;
}

static void
renderMeshBase(Atomic *atomic, InstanceDataHeader *header, InstanceData *inst, uint32 flags, int32 lit)
{
	Material *m = inst->material;
	setMaterialLighting(atomic, flags, m->color, m->surfaceProps,
		!!(flags & Geometry::PRELIT), lit);
	setTexture(m->texture);
	rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
	drawMesh(header, inst);
}

int32 bakeAllowed = 1;	// the game turns it off around things it changes behind our back

// what the recorded commands depend on: the context (render states,
// ambient light) and every material's colour and texture
static uint32
bakeKeyFor(InstanceDataHeader *header, int32 lit)
{
	uint32 k = bakeContextKey(lit);
	InstanceData *inst = header->inst;
	for(uint32 n = 0; n < header->numMeshes; n++, inst++){
		Material *m = inst->material;
		k = (k ^ (uint32)(uintptr)m->texture) * 0x01000193u;
		k = (k ^ (m->color.red | m->color.green<<8 | m->color.blue<<16 | (uint32)m->color.alpha<<24)) * 0x01000193u;
	}
	return k;
}

// replay a recorded version (1), or start recording (*rec set)
static int32
bakeLookup(InstanceDataHeader *header, int32 lit, uint32 *key, unsigned int **rec)
{
	*rec = nil;
	bakeStats[4]++;
	if(clip.active){
		bakeStats[5]++;
		return 0;
	}
	if(!bakeAllowed || !bakeEnabled || drawTess)
		return 0;
	*key = bakeKeyFor(header, lit);
	for(int i = 0; i < 4; i++)
		if(header->bake[i] && header->bakeEpoch[i] == bakeEpoch && header->bakeKey[i] == *key){
			if(bakeReplay(header->bake[i])){
				drawCalls += header->numMeshes;
				return 1;
			}
			return 0;
		}
	if(header->bakeRecEpoch != bakeEpoch){
		header->bakeRecEpoch = bakeEpoch;
		header->bakeRecords = 0;
	}
	if(header->bakeRecords < 12)
		*rec = bakeRecordStart(lit);
	return 0;
}

static void
bakeStore(InstanceDataHeader *header, uint32 key, unsigned int *rec)
{
	if(rec == nil)
		return;
	uint32 *b = bakeRecordEnd(rec);
	if(b == nil)
		return;
	// a free or out of date slot, else the next in turn
	int slot = -1;
	for(int i = 0; i < 4 && slot < 0; i++)
		if(header->bake[i] == nil || header->bakeEpoch[i] != bakeEpoch)
			slot = i;
	if(slot < 0){
		slot = header->bakeNext;
		header->bakeNext = (header->bakeNext + 1) & 3;
	}
	header->bake[slot] = b;
	header->bakeKey[slot] = key;
	header->bakeEpoch[slot] = bakeEpoch;
	header->bakeRecords++;
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 lit = setupLights(atomic);
	chooseTess(atomic, header);
	clipSetup(atomic);
	uint32 key = 0;
	unsigned int *rec;
	if(bakeLookup(header, lit, &key, &rec)){
		clip.active = false;
		return;
	}
	InstanceData *inst = header->inst;
	for(uint32 n = 0; n < header->numMeshes; n++, inst++)
		renderMeshBase(atomic, header, inst, flags, lit);
	clip.active = false;
	bakeStore(header, key, rec);
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

//
// MatFX: base pass plus an optional additive environment pass
//

static RawMatrix normal2texcoord = {
	{ 0.5f,  0.0f, 0.0f }, 0.0f,
	{ 0.0f, -0.5f, 0.0f }, 0.0f,
	{ 0.0f,  0.0f, 1.0f }, 0.0f,
	{ 0.5f,  0.5f, 0.0f }, 1.0f
};

static void
renderEnvPass(Atomic *atomic, InstanceDataHeader *header, InstanceData *inst, MatFX::Env *env)
{
	if(!envMapEnabled || env->tex == nil || env->coefficient <= 0.0f ||
	   !(atomic->geometry->flags & Geometry::NORMALS))
		return;
	Frame *frame = env->frame;
	if(frame == nil)
		frame = ((Camera*)engine->currentCamera)->getFrame();

	// texture matrix: model normal -> world -> env frame space -> uv
	Matrix invMat;
	Matrix::invert(&invMat, frame->getLTM());
	RawMatrix inv, world, tmp, envMtx;
	convMatrix(&inv, &invMat);
	inv.pos.set(0.0f, 0.0f, 0.0f);
	convMatrix(&world, atomic->getFrame()->getLTM());
	world.pos.set(0.0f, 0.0f, 0.0f);
	float uscale = fabs(normal2texcoord.right.x);
	normal2texcoord.right.x = MatFX::envMapFlipU ? -uscale : uscale;
	RawMatrix::mult(&tmp, &world, &inv);
	RawMatrix::mult(&envMtx, &tmp, &normal2texcoord);

	ScePspFMatrix4 texMat;
	memcpy(&texMat, &envMtx, sizeof(texMat));

	uint32 srcblend = rw::GetRenderState(SRCBLEND);
	uint32 dstblend = rw::GetRenderState(DESTBLEND);
	uint32 zwrite = rw::GetRenderState(ZWRITEENABLE);
	rw::SetRenderState(SRCBLEND, BLENDSRCALPHA);
	rw::SetRenderState(DESTBLEND, BLENDONE);
	rw::SetRenderState(ZWRITEENABLE, 0);
	rw::SetRenderState(VERTEXALPHA, 1);
	setTexture(env->tex);
	prepareGeometryDraw();

	RGBA col = MatFX::envMapUseMatColor ? inst->material->color : MatFX::envMapColor;
	float a = env->coefficient;
	if(a > 1.0f) a = 1.0f;
	uint32 c = col.red | col.green<<8 | col.blue<<16 | (uint32)(a*255.0f)<<24;
	setLighting(0);
	sceGuColor(c);
	sceGuSetMatrix(GU_TEXTURE, &texMat);
	sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 1);
	sceGuTexProjMapMode(GU_NORMALIZED_NORMAL);
	// draw without vertex colours so the material colour is used
	uint32 vtype = header->vertexType & ~GU_COLOR_BITS;
	if(header->vertexType & GU_COLOR_BITS){
		// layout has colours, can't drop them from the stride; just use them
		vtype = header->vertexType;
	}
	drawIndexed(header, inst, vtype);
	drawCalls++;
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 1);
	// sceGuColor changed the material colour, make the next mesh resend it
	invalidateLightingCache();

	rw::SetRenderState(SRCBLEND, srcblend);
	rw::SetRenderState(DESTBLEND, dstblend);
	rw::SetRenderState(ZWRITEENABLE, zwrite);
}

static void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 lit = setupLights(atomic);
	chooseTess(atomic, header);
	clipSetup(atomic);
	// without the reflection pass it's the default pipeline: may be recorded
	uint32 key = 0;
	unsigned int *rec = nil;
	if(!envMapEnabled && bakeLookup(header, lit, &key, &rec)){
		clip.active = false;
		return;
	}
	InstanceData *inst = header->inst;
	for(uint32 n = 0; n < header->numMeshes; n++, inst++){
		renderMeshBase(atomic, header, inst, flags, lit);
		if(!envMapEnabled)
			continue;
		MatFX *matfx = MatFX::get(inst->material);
		if(matfx && matfx->type == MatFX::ENVMAP){
			renderEnvPass(atomic, header, inst, &matfx->fx[0].env);
			// lighting was switched off for the env pass, make sure the
			// next mesh sets it up again
			setupLights(atomic);
		}
	}
	clip.active = false;
	bakeStore(header, key, rec);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->renderCB = matfxRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

//
// Skin: hardware skinning, triangles are split into batches of at most 8 bones
//

struct SkinVertKey
{
	uint32 orig;
};

// get the triangles of a mesh as a list
static uint32
meshTriangles(Mesh *mesh, bool strip, uint16 *out)
{
	uint32 n = 0;
	if(!strip){
		for(uint32 i = 0; i+2 < mesh->numIndices; i += 3){
			out[n++] = mesh->indices[i];
			out[n++] = mesh->indices[i+1];
			out[n++] = mesh->indices[i+2];
		}
		return n;
	}
	for(uint32 i = 0; i+2 < mesh->numIndices; i++){
		uint16 a = mesh->indices[i], b = mesh->indices[i+1], c = mesh->indices[i+2];
		if(a == b || b == c || a == c)
			continue;
		if(i & 1){
			out[n++] = b; out[n++] = a; out[n++] = c;
		}else{
			out[n++] = a; out[n++] = b; out[n++] = c;
		}
	}
	return n;
}

// bones influencing a vertex (weight > 0)
static int32
vertexBones(Skin *skin, int32 v, uint8 *bones)
{
	int32 n = 0;
	for(int32 k = 0; k < 4; k++)
		if(skin->weights[v*4+k] > 0.0f){
			uint8 b = skin->indices[v*4+k];
			int32 j;
			for(j = 0; j < n; j++)
				if(bones[j] == b) break;
			if(j == n)
				bones[n++] = b;
		}
	return n;
}

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	Skin *skin = Skin::get(geo);
	VertLayout l;
	makeLayout(&l, geo, 8);
	MeshHeader *meshh = geo->meshHeader;
	bool strip = meshh->flags == MeshHeader::TRISTRIP;

	if(reinstance){
		// rebuilding the batches is expensive; skinned geometry is never locked in practice
		return;
	}

	// worst case: every triangle gets its own vertices
	uint32 maxTriIdx = 0;
	Mesh *mesh = meshh->getMeshes();
	for(uint32 i = 0; i < meshh->numMeshes; i++)
		maxTriIdx += strip ? (mesh[i].numIndices > 2 ? (mesh[i].numIndices-2)*3 : 0) : mesh[i].numIndices;

	uint16 *tris = rwNewT(uint16, maxTriIdx + 3, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint16 *outIdx = rwNewT(uint16, maxTriIdx + 3, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint8 *outVerts = rwNewT(uint8, (maxTriIdx + 3)*l.stride, MEMDUR_FUNCTION | ID_GEOMETRY);
	int32 *remap = rwNewT(int32, geo->numVertices, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint32 maxBatches = maxTriIdx/3 + header->numMeshes + 1;
	SkinBatch *batches = rwNewT(SkinBatch, maxBatches, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint32 *batchIndexStart = rwNewT(uint32, maxBatches, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint32 *batchIndexCount = rwNewT(uint32, maxBatches, MEMDUR_FUNCTION | ID_GEOMETRY);

	uint32 numBatches = 0;
	uint32 numOutVerts = 0;
	uint32 numOutIdx = 0;

	uint8 prevBones[8];	// slots of the previous batch (across meshes)
	memset(prevBones, 0xFF, 8);
	InstanceData *inst = header->inst;
	for(uint32 mi = 0; mi < meshh->numMeshes; mi++, inst++){
		uint32 ntri = meshTriangles(&mesh[mi], strip, tris);
		inst->firstBatch = numBatches;
		inst->numBatches = 0;
		inst->vertexAlpha = 0;

		SkinBatch *b = nil;
		uint32 batchFirstVert = 0;
		int32 batchUsed = 0;
		for(uint32 t = 0; t < ntri; t += 3){
			// bones this triangle needs
			uint8 tb[12];
			int32 ntb = 0;
			for(int32 k = 0; k < 3; k++){
				uint8 vb[4];
				int32 nvb = vertexBones(skin, tris[t+k], vb);
				for(int32 j = 0; j < nvb; j++){
					int32 m;
					for(m = 0; m < ntb; m++)
						if(tb[m] == vb[j]) break;
					if(m == ntb) tb[ntb++] = vb[j];
				}
			}
			if(ntb > 8) ntb = 8;	// pathological, extra influences get dropped below

			// does it fit the current batch?
			// Bones keep the GE slot they had in the previous batch where
			// possible, so the renderer doesn't upload them again
			// (unused slots hold 0xFF, which the renderer skips).
			bool fits = false;
			if(b){
				int32 need = 0;
				for(int32 j = 0; j < ntb; j++){
					uint32 m;
					for(m = 0; m < 8; m++)
						if(b->bones[m] == tb[j]) break;
					if(m == 8) need++;
				}
				fits = batchUsed + need <= 8;
			}
			if(!fits){
				if(b)
					memcpy(prevBones, b->bones, 8);
				b = &batches[numBatches];
				batchIndexStart[numBatches] = numOutIdx;
				batchIndexCount[numBatches] = 0;
				numBatches++;
				inst->numBatches++;
				memset(b, 0, sizeof(SkinBatch));
				memset(b->bones, 0xFF, 8);
				batchUsed = 0;
				b->vertexOffset = numOutVerts*l.stride;
				batchFirstVert = numOutVerts;
				for(int32 v = 0; v < geo->numVertices; v++)
					remap[v] = -1;
			}
			for(int32 j = 0; j < ntb; j++){
				uint32 m;
				for(m = 0; m < 8; m++)
					if(b->bones[m] == tb[j]) break;
				if(m < 8 || batchUsed >= 8)
					continue;
				// the slot it had before, else a free slot whose old bone
				// isn't wanted here, else any free slot
				int32 slot = -1;
				for(int32 q = 0; q < 8; q++)
					if(prevBones[q] == tb[j] && b->bones[q] == 0xFF){ slot = q; break; }
				if(slot < 0)
					for(int32 q = 0; q < 8; q++)
						if(b->bones[q] == 0xFF && prevBones[q] == 0xFF){ slot = q; break; }
				if(slot < 0)
					for(int32 q = 0; q < 8; q++){
						if(b->bones[q] != 0xFF) continue;
						bool wanted = false;
						for(int32 r = 0; r < ntb; r++)
							if(prevBones[q] == tb[r]) wanted = true;
						if(!wanted){ slot = q; break; }
					}
				if(slot < 0)
					for(int32 q = 0; q < 8; q++)
						if(b->bones[q] == 0xFF){ slot = q; break; }
				b->bones[slot] = tb[j];
				batchUsed++;
				if((uint32)slot + 1 > b->numBones)
					b->numBones = slot + 1;
			}
			// emit vertices
			for(int32 k = 0; k < 3; k++){
				int32 v = tris[t+k];
				if(remap[v] < 0){
					uint8 *dst = outVerts + numOutVerts*l.stride;
					memset(dst, 0, l.stride);
					writeVertex(dst, &l, geo, v);
					// weights into batch slots, 0x80 = 1.0
					float wsum = 0.0f;
					float w8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
					for(int32 q = 0; q < 4; q++){
						float w = skin->weights[v*4+q];
						if(w <= 0.0f) continue;
						uint8 bone = skin->indices[v*4+q];
						for(uint32 m = 0; m < b->numBones; m++)
							if(b->bones[m] == bone){
								w8[m] += w;
								wsum += w;
								break;
							}
					}
					if(wsum <= 0.0f){
						// no influence in this batch: all on the first used slot
						int32 q0 = 0;
						while(q0 < 7 && b->bones[q0] == 0xFF) q0++;
						w8[q0] = 1.0f; wsum = 1.0f;
					}
					int32 total = 0;
					for(int32 q = 0; q < 8; q++){
						int32 iw = (int32)(w8[q]/wsum*128.0f + 0.5f);
						dst[l.offWeights+q] = iw;
						total += iw;
					}
					// fix rounding so the weights add up to exactly 1
					if(total != 128){
						int32 best = 0;
						for(int32 q = 1; q < 8; q++)
							if(dst[l.offWeights+q] > dst[l.offWeights+best]) best = q;
						dst[l.offWeights+best] += 128 - total;
					}
					if(geo->flags & Geometry::PRELIT && geo->colors[v].alpha != 0xFF)
						inst->vertexAlpha = 1;
					remap[v] = numOutVerts - batchFirstVert;
					numOutVerts++;
					b->numVertices++;
				}
				outIdx[numOutIdx++] = remap[v];
				batchIndexCount[numBatches-1]++;
			}
		}
		if(b)
			memcpy(prevBones, b->bones, 8);
	}

	// final buffers
	header->stride = l.stride;
	header->vertexType = l.vtype | GU_INDEX_16BIT;
	header->primType = GU_TRIANGLES;
	free16(header->indexBuffer);
	header->indexBuffer = (uint16*)malloc16(numOutIdx*2 + 2);
	memcpy(header->indexBuffer, outIdx, numOutIdx*2);
	header->vertexBuffer = (uint8*)malloc16(numOutVerts*l.stride + 16);
	memcpy(header->vertexBuffer, outVerts, numOutVerts*l.stride);
	header->batches = rwNewT(SkinBatch, numBatches ? numBatches : 1, MEMDUR_EVENT | ID_GEOMETRY);
	for(uint32 i = 0; i < numBatches; i++){
		header->batches[i] = batches[i];
		// reuse numVertices of the batch as index count, vertexOffset stays
		header->batches[i].numVertices = batchIndexCount[i];
		// store the index start in the high part: keep separate array instead
	}
	header->numBatches = numBatches;
	// index start of each batch, stored after the batches
	inst = header->inst;
	for(uint32 mi = 0; mi < header->numMeshes; mi++, inst++){
		inst->indexOffset = inst->numBatches ? batchIndexStart[inst->firstBatch] : 0;
		inst->numIndex = 0;
		for(uint32 bi = 0; bi < inst->numBatches; bi++)
			inst->numIndex += batchIndexCount[inst->firstBatch+bi];
	}
	header->totalNumIndex = numOutIdx;
	header->totalNumVertex = numOutVerts;
	sceKernelDcacheWritebackRange(header->vertexBuffer, numOutVerts*l.stride);
	sceKernelDcacheWritebackRange(header->indexBuffer, numOutIdx*2);

	rwFree(tris);
	rwFree(outIdx);
	rwFree(outVerts);
	rwFree(remap);
	rwFree(batches);
	rwFree(batchIndexStart);
	rwFree(batchIndexCount);
}

static Matrix skinMatrices[256];

// returns true when the matrices are in world space
static bool
computeSkinMatrices(Atomic *a, Skin *skin)
{
	HAnimHierarchy *hier = Skin::getHierarchy(a);
	int32 n = skin->numBones;
	if(n > 256) n = 256;
	if(hier){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(int32 i = 0; i < hier->numNodes && i < n; i++){
				invMats[i].flags = 0;
				Matrix::mult(&skinMatrices[i], &invMats[i], &hier->matrices[i]);
			}
		}else{
			// bones are in world space: give them to the GE as they are and
			// draw with an identity world matrix (one matrix multiply less
			// per bone than going back into the atomic's space)
			for(int32 i = 0; i < hier->numNodes && i < n; i++){
				invMats[i].flags = 0;
				Matrix::mult(&skinMatrices[i], &invMats[i], &hier->matrices[i]);
				// camera relative, see setWorldMatrix
				skinMatrices[i].pos.x -= camWorldPos.x;
				skinMatrices[i].pos.y -= camWorldPos.y;
				skinMatrices[i].pos.z -= camWorldPos.z;
			}
			return true;
		}
	}else{
		for(int32 i = 0; i < n; i++)
			skinMatrices[i].setIdentity();
	}
	return false;
}

static inline void
toGeMatrix(ScePspFMatrix4 *out, const Matrix *m)
{
	out->x.x = m->right.x; out->x.y = m->right.y; out->x.z = m->right.z; out->x.w = 0.0f;
	out->y.x = m->up.x;    out->y.y = m->up.y;    out->y.z = m->up.z;    out->y.w = 0.0f;
	out->z.x = m->at.x;    out->z.y = m->at.y;    out->z.z = m->at.z;    out->z.w = 0.0f;
	out->w.x = m->pos.x;   out->w.y = m->pos.y;   out->w.z = m->pos.z;   out->w.w = 1.0f;
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Geometry *geo = atomic->geometry;
	uint32 flags = geo->flags;
	Skin *skin = Skin::get(geo);
	int32 lit = setupLights(atomic);
	if(computeSkinMatrices(atomic, skin)){
		static Matrix ident;
		static bool identInit;
		if(!identInit){ ident.setIdentity(); identInit = true; }
		setWorldMatrixCameraSpace(&ident);
	}else
		setWorldMatrix(atomic->getFrame()->getLTM());

	// which bone is loaded in each GE bone slot; unused slots don't matter
	// (their weights are 0) so they are never cleared
	int32 slotBone[8];
	for(int32 i = 0; i < 8; i++) slotBone[i] = -1;

	InstanceData *inst = header->inst;
	for(uint32 mi = 0; mi < header->numMeshes; mi++, inst++){
		if(inst->numBatches == 0)
			continue;
		Material *m = inst->material;
		setMaterialLighting(atomic, flags, m->color, m->surfaceProps,
			!!(flags & Geometry::PRELIT), lit);
		setTexture(m->texture);
		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		prepareGeometryDraw();

		uint32 idxOff = inst->indexOffset;
		for(uint32 bi = 0; bi < inst->numBatches; bi++){
			SkinBatch *b = &header->batches[inst->firstBatch + bi];
			for(uint32 s = 0; s < b->numBones && s < 8; s++){
				int32 bone = b->bones[s];
				if(bone >= skin->numBones || slotBone[s] == bone)
					continue;
				setBoneMatrix(s, &skinMatrices[bone]);
				slotBone[s] = bone;
			}
			drawArray(GU_TRIANGLES, header->vertexType, b->numVertices,
				header->indexBuffer + idxOff, header->vertexBuffer + b->vertexOffset);
			drawCalls++;
			idxOff += b->numVertices;
		}
	}
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

static void*
skinOpen(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_PSP] = makeSkinPipeline();
	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	((ObjPipeline*)skinGlobals.pipelines[PLATFORM_PSP])->destroy();
	skinGlobals.pipelines[PLATFORM_PSP] = nil;
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, ID_SKIN,
	                       skinOpen, skinClose);
}

static void*
matfxOpen(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_PSP] = makeMatFXPipeline();
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_PSP])->destroy();
	matFXGlobals.pipelines[PLATFORM_PSP] = nil;
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_PSP, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

#else

void *destroyNativeData(void *object, int32, int32) { return object; }
void initSkin(void) { }
void initMatFX(void) { }

#endif

}
}

#endif
