#include "common.h"

#include "General.h"
#include "Camera.h"
#include "Renderer.h"
#include "ModelInfo.h"
#include "AnimManager.h"
#include "custompipes.h"

void
CSimpleModelInfo::DeleteRwObject(void)
{
	int i;
	RwFrame *f;
	for(i = 0; i < m_numAtomics; i++)
		if(m_atomics[i]){
			f = RpAtomicGetFrame(m_atomics[i]);
			RpAtomicDestroy(m_atomics[i]);
			RwFrameDestroy(f);
			m_atomics[i] = nil;
			RemoveTexDictionaryRef();
			if(GetAnimFileIndex() != -1)
				CAnimManager::RemoveAnimBlockRef(GetAnimFileIndex());
		}
}

RwObject*
CSimpleModelInfo::CreateInstance(void)
{
	RpAtomic *atomic;
	if(m_atomics[0] == nil)
		return nil;
	atomic = RpAtomicClone(m_atomics[0]);
	RpAtomicSetFrame(atomic, RwFrameCreate());
	return (RwObject*)atomic;
}

RwObject*
CSimpleModelInfo::CreateInstance(RwMatrix *matrix)
{
	RpAtomic *atomic;
	RwFrame *frame;

	if(m_atomics[0] == nil)
		return nil;
	atomic = RpAtomicClone(m_atomics[0]);
	frame = RwFrameCreate();
	*RwFrameGetMatrix(frame) = *matrix;
	RpAtomicSetFrame(atomic, frame);
	return (RwObject*)atomic;
}

void
CSimpleModelInfo::Init(void)
{
	m_atomics[0] = nil;
	m_atomics[1] = nil;
	m_atomics[2] = nil;
	m_numAtomics = 0;
	m_firstDamaged  = 0;
	m_wetRoadReflection    = 0;
	m_isDamaged     = 0;
	m_isBigBuilding = 0;
	m_noFade        = 0;
	m_drawLast      = 0;
	m_additive      = 0;
	m_isSubway      = 0;
	m_ignoreLight   = 0;
	m_noZwrite      = 0;
	m_noShadows     = 0;
	m_ignoreDrawDist  = 0;
	m_isCodeGlass     = 0;
	m_isArtistGlass   = 0;
}

void
CSimpleModelInfo::SetAtomic(int n, RpAtomic *atomic)
{
	AddTexDictionaryRef();
	m_atomics[n] = atomic;
	if(GetAnimFileIndex() != -1)
		CAnimManager::AddAnimBlockRef(GetAnimFileIndex());
	RpGeometry *geo = RpAtomicGetGeometry(atomic);
	if(m_ignoreLight)
		RpGeometrySetFlags(geo, RpGeometryGetFlags(geo) & ~rpGEOMETRYLIGHT);
	if(RpGeometryGetFlags(geo) & rpGEOMETRYNORMALS &&
	   RpGeometryGetNumTriangles(geo) > 200)
		debug("%s has %d polys\n", m_name, RpGeometryGetNumTriangles(geo));

#ifdef EXTENDED_PIPELINES
	if(m_wetRoadReflection)
		CustomPipes::AttachGlossPipe(atomic);
	else
		CustomPipes::AttachWorldPipe(atomic);
#endif
}

void
CSimpleModelInfo::SetLodDistances(float *dist)
{
	m_lodDistances[0] = dist[0];
	m_lodDistances[1] = dist[1];
	m_lodDistances[2] = dist[2];
}

void
CSimpleModelInfo::IncreaseAlpha(void)
{
#ifdef PSP
	// no fading on the PSP: alpha blended world objects are expensive to
	// draw (sorted, material colours rewritten), so objects just appear
	m_alpha = 0xFF;
	return;
#endif
	if(m_alpha >= 0xEF)
		m_alpha = 0xFF;
	else
		m_alpha += 0x10;
}

#ifdef PSP
// lod_scale makes detailed models switch to their low detail version
// sooner. Big ones (a multi storey car park) would then show their
// closed low detail box while you're already driving in: never switch
// closer than the model's own size plus a margin.
static inline float
ScaledLodDistance(CSimpleModelInfo *mi, float d)
{
	float r = d * TheCamera.LODDistMultiplier;
	CColModel *col = mi->GetColModel();
	if(col){
		float m = col->boundingSphere.radius + 30.0f;
		if(m > d) m = d;
		if(r < m) r = m;
	}
	return r;
}
#define LODDIST(d) ScaledLodDistance(this, d)
#else
#define LODDIST(d) ((d) * TheCamera.LODDistMultiplier)
#endif

float
CSimpleModelInfo::GetLodDistance(int i)
{
	return LODDIST(m_lodDistances[i]);
}

float
CSimpleModelInfo::GetNearDistance(void)
{
#ifdef PSP
	// a low detail model hides exactly where its detailed model ends,
	// including the PSP's size minimum (otherwise both were drawn in between:
	// overdraw and the two fighting in the depth buffer)
	if(m_isBigBuilding && GetRelatedModel())
		return GetRelatedModel()->GetLargestLodDistance();
#endif
	return m_lodDistances[2] * TheCamera.LODDistMultiplier;
}

float
CSimpleModelInfo::GetLargestLodDistance(void)
{
	float d;
	if(m_firstDamaged == 0 || m_isDamaged)
		d = m_lodDistances[m_numAtomics-1];
	else
		d = m_lodDistances[m_firstDamaged-1];
	return LODDIST(d);
}

RpAtomic*
CSimpleModelInfo::GetAtomicFromDistance(float dist)
{
	int i;
	i = 0;
	if(m_isDamaged)
		i = m_firstDamaged;
	for(; i < m_numAtomics; i++)
		if(dist < LODDIST(m_lodDistances[i]))
			return m_atomics[i];
	return nil;
}

RpAtomic*
CSimpleModelInfo::GetFirstAtomicFromDistance(float dist)
{
	if(dist < LODDIST(m_lodDistances[0]))
		return m_atomics[0];
	return nil;
}

void
CSimpleModelInfo::FindRelatedModel(int32 minID, int32 maxID)
{
	int i;
	CBaseModelInfo *mi;
	for(i = minID; i <= maxID; i++){
		mi = CModelInfo::GetModelInfo(i);
		if(mi && mi != this &&
		   !CGeneral::faststrcmp(GetModelName()+3, mi->GetModelName()+3)){
			assert(mi->IsSimple());
			this->SetRelatedModel((CSimpleModelInfo*)mi);
			return;
		}
	}
}

#define NEAR_DRAW_DIST 0.0f	// 100.0f in liberty city

void
CSimpleModelInfo::SetupBigBuilding(int32 minID, int32 maxID)
{
	CSimpleModelInfo *related;
	if(m_lodDistances[0] > LOD_DISTANCE && GetRelatedModel() == nil){
		m_isBigBuilding = 1;
		FindRelatedModel(minID, maxID);
		related = GetRelatedModel();
		if(related){
#ifdef PSP
			// the plain distance (GetLargestLodDistance has a minimum on the PSP)
			m_lodDistances[2] = related->m_firstDamaged == 0 || related->m_isDamaged ?
				related->m_lodDistances[related->m_numAtomics-1] : related->m_lodDistances[related->m_firstDamaged-1];
#else
			m_lodDistances[2] = related->GetLargestLodDistance()/TheCamera.LODDistMultiplier;
#endif
			if(m_drawLast){
				m_drawLast = false;
				debug("%s was draw last\n", GetModelName());
			}
		}else
			m_lodDistances[2] = NEAR_DRAW_DIST;
	}
}
