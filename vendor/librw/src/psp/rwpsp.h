// librw PSP backend (sceGu / GE)
// Native rasters and geometry are stored in a GE-ready layout so nothing has
// to be converted while the game is running.

namespace rw {

#ifdef RW_PSP
struct EngineOpenParams
{
	int width, height;
	const char *windowtitle;
};
#endif

namespace psp {
extern uint32 t4Expanded, t4Kept;
extern int32 fxTest;
extern int32 im2DThrough;
// baked models
extern int32 bakeEnabled, bakeAllowed;
extern uint32 bakeEpoch, bakeStats[8];
uint32 bakeContextKey(int32 lit);
unsigned int *bakeRecordStart(int32 lit);
uint32 *bakeRecordEnd(unsigned int *start);
bool32 bakeReplay(uint32 *b);
// the PSP's save/load screens over the last frame (pspdevice.cpp)
void utilityDialog(int32 (*step)(void));
bool32 makeRasterTrueColour(Raster *raster);
void describeDrawState(char *buf, int32 len);
extern int32 logSpriteDraws;	// black squares hunt (cheat menu), 0 = normal

void registerPlatformPlugins(void);

extern Device renderdevice;

// GE pixel formats (same values as GU_PSM_*)
enum {
	PSM_5650 = 0,
	PSM_5551 = 1,
	PSM_4444 = 2,
	PSM_8888 = 3,
	PSM_T4   = 4,
	PSM_T8   = 5
};

enum {
	MAXLEVELS = 8
};

struct PspRaster
{
	uint8 *data;		// one allocation for all levels, 16 byte aligned
	uint32 *palette;	// CLUT, 8888, 16 byte aligned (16 or 256 entries)
	uint32 levelOffset[MAXLEVELS];
	uint16 levelWidth[MAXLEVELS];	// power of two, as given to the GE
	uint16 levelHeight[MAXLEVELS];
	uint16 levelBufWidth[MAXLEVELS];	// texture buffer width in pixels
	uint32 dataSize;
	uint8 psm;
	uint8 numLevels;
	uint8 swizzled;
	uint8 hasAlpha;
	uint8 ownsData;
	uint8 vram;		// data lives in VRAM (camera texture)
#ifdef N3DS
	void *c3dTex;		// C3D_Tex made from this raster (3ds/c3draster.cpp)
	uint8 keepSource;	// don't free the PSP copy after converting
	uint8 shrink;		// draw it smaller (streamed world/ped/car textures)
#endif
};

extern int32 nativeRasterOffset;
#define GETPSPRASTEREXT(raster) PLUGINOFFSET(rw::psp::PspRaster, raster, rw::psp::nativeRasterOffset)

void registerNativeRaster(void);

Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

// Convert a raster from any other platform (D3D8 DXT etc.) into a PSP raster.
// maxSize limits the texture dimension (downscaling by halving).
Raster *convertForeignRaster(Raster *ras, int32 maxSize);
// Build a PSP raster from an image. depth 4/8 -> T4/T8, otherwise 16 or 32 bit.
// palettize: quantize truecolour images to 8 bit (slow, meant for offline tools)
Raster *rasterFromImageEx(Image *img, int32 maxSize, bool32 palettize, int32 numLevels);
extern int32 maxTextureSize;	// default limit for runtime conversion

// helpers shared with tools
void swizzle(uint8 *out, const uint8 *in, uint32 widthBytes, uint32 height);
void unswizzle(uint8 *out, const uint8 *in, uint32 widthBytes, uint32 height);
uint32 psmBitsPerPixel(int32 psm);

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster*, int32 level, int32 lockMode);
void rasterUnlock(Raster*, int32);
uint8 *rasterLockPalette(Raster*, int32 lockMode);
void rasterUnlockPalette(Raster*);
int32 rasterNumLevels(Raster*);
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster*, Image*);
Image *rasterToImage(Raster*);

// Geometry

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;
	int32     numVertices;
	Material *material;
	bool32    vertexAlpha;
	uint32    indexOffset;	// in indices
	// split version for drawing close to the camera (triangle list)
	uint32    tessIndexOffset;
	uint32    tessNumIndex;
	// hardware skinning batches
	uint32    firstBatch;
	uint32    numBatches;
	// pieces of the split triangle list, by area (CPU clipping)
	uint32    firstChunk;
	uint32    numChunks;
};

// CPU clipping by area: a model's triangle list (tessIndexBuffer) is sorted
// by cells of a grid over the model; a chunk is the part of one mesh in one
// cell, the cell has the box (centre, half size in model space)
struct TessCell
{
	float32 cx, cy, cz, hx, hy, hz;
};
struct TessChunk
{
	uint32 first, count;	// in tessIndexBuffer
	uint32 cell;
};

// A skinning batch: a run of triangles that uses at most 8 bones.
struct SkinBatch
{
#ifdef N3DS
	uint8  bones[20];	// hierarchy bone index for each shader bone slot
#else
	uint8  bones[8];	// hierarchy bone index for each GE bone slot
#endif
	uint32 numBones;
	uint32 vertexOffset;	// byte offset into vertex buffer
	uint32 numVertices;
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32      serialNumber;
	uint32      numMeshes;
	uint32      primType;	// GU_TRIANGLES / GU_TRIANGLE_STRIP
	uint32      vertexType;	// GU vertex format bits
	uint32      stride;
	uint8      *vertexBuffer;
	uint16     *indexBuffer;
	uint32      totalNumVertex;
	uint32      totalNumIndex;
	InstanceData *inst;
	uint16     *tessIndexBuffer;	// nil if the model has no split version
	TessChunk  *chunks;		// (tessIndexBuffer by area)
	TessCell   *cells;
	uint32      numCells;
	int32       tessJob;	// background split: -1 none, -2 wanted (queue was full), else the job
	// skin
	SkinBatch  *batches;
	uint32      numBatches;
	// recorded GE commands (pspdevice.cpp, baked models): a few versions
	// (one model drawn with different colours, lights on/off...)
	uint32     *bake[4];
	uint32      bakeKey[4];
	uint32      bakeEpoch[4];
	uint32      bakeNext;	// slot to replace next
	uint32      bakeRecords;	// recorded this epoch (a model that never repeats itself stops)
	uint32      bakeRecEpoch;
#ifdef N3DS
	void       *c3dLayout;	// attribute/buffer setup (3ds/c3dpipe.cpp)
#endif
};

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header);
ObjPipeline *makeDefaultPipeline(void);
ObjPipeline *makeSkinPipeline(void);
ObjPipeline *makeMatFXPipeline(void);
void initSkin(void);
void initMatFX(void);

void *destroyNativeData(void *object, int32, int32);

#ifdef RW_PSP

struct Im3DVertex
{
	V3d     position;
	V3d     normal;
	uint8   r, g, b, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setNormalX(float32 x) { this->normal.x = x; }
	void setNormalY(float32 y) { this->normal.y = y; }
	void setNormalZ(float32 z) { this->normal.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	float getNormalX(void) { return this->normal.x; }
	float getNormalY(void) { return this->normal.y; }
	float getNormalZ(void) { return this->normal.z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};
extern RGBA im3dMaterialColor;
extern SurfaceProperties im3dSurfaceProps;

struct Im2DVertex
{
	float32 x, y, z, w;
	uint8   r, g, b, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { this->u = u; }
	void setV(float32 v, float recipz) { this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

// device internals shared between files
void setWorldMatrix(Matrix *mat);
void setTexture(Texture *tex);
void setRasterForDraw(Raster *raster);
void flushRenderState(void);
bool32 getAlphaBlend(void);
void setMaterialLighting(Atomic *atomic, uint32 geoflags, const RGBA &color,
	const SurfaceProperties &surfprops, bool32 prelit, bool32 lit);
int32 setupLights(Atomic *atomic);
void *allocFrameMemory(uint32 size);	// valid until frame end (display list memory)
void ensureFrame(void);
void evictRaster(Raster *raster);
void prepareGeometryDraw(void);
extern float32 tessMaxEdge;
extern float32 tessNearDist;
extern uint32 tessStats[4];
extern bool32 envMapEnabled;
extern int32 worldDepthBias;	// depth units towards the camera, set by the game for roads
extern int32 decalDepthBias;	// extra for alpha blended geometry
void setLighting(int32 on);
void setBoneMatrix(int32 slot, const Matrix *mat);
void setWorldMatrixCameraSpace(const Matrix *mat);
extern V3d camWorldPos;	// camera relative rendering, see pspdevice.cpp
void getClipCamera(float32 *view16, float32 *projX, float32 *projY, float32 *nearZ);
extern uint32 camSerial;	// changes with every camera update
extern uint32 clipStats[10];	// atomics checked, atomics clipped, triangles clipped, triangles dropped, vertices transformed, areas checked, areas partly in
extern int32 clipEnabled;	// CPU clipping of triangles the GE would drop (holes)
void invalidateLightingCache(void);
void drawArray(int32 prim, int32 vtype, int32 count, const void *indices, const void *vertices);
Image *grabFrameBuffer(void);
void motionBlur(int32 alpha);
extern volatile uint32 geSyncStart;	// when the CPU started waiting for the GE (0: not waiting)
void geRecover(void);	// cancel a hung GE (watchdog)	// VCS style trails, before the HUD; 0 = off
extern uint32 frameCounter;
extern uint32 drawCalls;
void geDeferFree(void *rwmem);	// free once the GE is done with the frames in flight
bool32 geFlushFreesNow(void);	// (low memory) wait for the GE and free them now
extern uint32 vertsDrawn;
extern uint32 instanceBudgetUs, instanceUsedUs, instanceDeferred;	// first-draw conversion time per frame	// vertices sent to the GE this frame
extern uint32 geWaitUs;	// time the CPU waited for the GE to finish the previous frame (accumulates)
extern uint32 lastFrameVerts, lastFrameDraws;
extern uint32 listPeakBytes;
extern uint32 listDroppedDraws;
// 2D overlay (HUD) cache, see pspdevice.cpp
void overlayBegin(void);
void overlayEnd(void);
bool32 overlayReplay(void);
void overlayDiscard(void);
extern uint32 overlayBytes;
#ifdef N3DS
// 3DS backend (3ds/c3d*.cpp)
extern int32 c3dTexShift;	// textures drawn at 1/2^shift size
extern int32 c3dTexMaxSize;
extern bool32 c3dShrinkTextures;	// game sets it while streamed TXDs load
extern uint32 c3dTexBytes;
extern void (*c3dBottomScreenCB)(void);
extern void (*c3dLog)(const char *msg);	// draws the bottom screen (Im2D, 320x240)
void convertRasterNow(Raster *raster);	// make the PICA texture, free the PSP copy
#endif

void im2DRenderLine(void*, int32, int32, int32);
void im2DRenderTriangle(void*, int32, int32, int32, int32);
void im2DRenderPrimitive(PrimitiveType, void*, int32);
void im2DRenderIndexedPrimitive(PrimitiveType, void*, int32, void*, int32);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);

#endif

}
}
