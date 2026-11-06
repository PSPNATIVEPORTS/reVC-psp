#include "common.h"
#include "ColModel.h"
#include "Collision.h"
#include "Game.h"
#include "MemoryHeap.h"
#include "Pools.h"

CColModel::CColModel(void)
{
	numSpheres = 0;
	spheres = nil;
	numLines = 0;
	lines = nil;
	numBoxes = 0;
	boxes = nil;
	numTriangles = 0;
	vertices = nil;
	triangles = nil;
	trianglePlanes = nil;
	level = LEVEL_GENERIC;	// generic col slot
	ownsCollisionVolumes = true;
}

CColModel::~CColModel(void)
{
	RemoveCollisionVolumes();
}

void*
CColModel::operator new(size_t) throw()
{
	CColModel* node = CPools::GetColModelPool()->New();
	assert(node);
	return node;
}

void
CColModel::operator delete(void *p, size_t) throw()
{
	CPools::GetColModelPool()->Delete((CColModel*)p);
}

void
CColModel::RemoveCollisionVolumes(void)
{
	if(ownsCollisionVolumes){
		RwFree(spheres);
		RwFree(lines);
		RwFree(boxes);
		RwFree(vertices);
		RwFree(triangles);
		CCollision::RemoveTrianglePlanes(this);
	}
	numSpheres = 0;
	numLines = 0;
	numBoxes = 0;
	numTriangles = 0;
	spheres = nil;
	lines = nil;
	boxes = nil;
	vertices = nil;
	triangles = nil;
}

void
CColModel::CalculateTrianglePlanes(void)
{
	PUSH_MEMID(MEMID_COLLISION);

#ifdef PSP
	// HACK: allocate space for one more element to stuff the link pointer into,
	// and 16 bytes in front for the triangle grid pointer (a fixed place: the
	// end of the array moves when numTriangles changes)
	{
		uint8 *base = (uint8*)RwMalloc(16 + sizeof(CColTrianglePlane) * (numTriangles+1));
		*(CColGrid**)base = nil;
		trianglePlanes = (CColTrianglePlane*)(base + 16);
	}
#else
	// HACK: allocate space for one more element to stuff the link pointer into
	trianglePlanes = (CColTrianglePlane*)RwMalloc(sizeof(CColTrianglePlane) * (numTriangles+1));
#endif
	REGISTER_MEMPTR(&trianglePlanes);
	for(int i = 0; i < numTriangles; i++)
		trianglePlanes[i].Set(vertices, triangles[i]);
#ifdef PSP
	*(CColGrid**)((uint8*)trianglePlanes - 16) = BuildGrid();
#endif

	POP_MEMID();
}

#ifdef PSP
void*
CColModel::GetLinkSlot(void) const
{
	return ALIGNPTR(&trianglePlanes[numTriangles]);
}

CColGrid*
CColModel::BuildGrid(void)
{
	if(numTriangles < 48)
		return nil;
	float minx = boundingBox.min.x, miny = boundingBox.min.y;
	float ex = boundingBox.max.x - minx, ey = boundingBox.max.y - miny;
	float cell = Max(ex, ey)/COLGRID_MAX;
	if(cell < 6.0f) cell = 6.0f;
	int w = Min((int)(ex/cell) + 1, COLGRID_MAX), h = Min((int)(ey/cell) + 1, COLGRID_MAX);
	if(w*h < 2)
		return nil;
	CColGrid *g = (CColGrid*)RwMalloc(sizeof(CColGrid) + numTriangles*sizeof(uint16));
	if(g == nil)
		return nil;
	g->numCells = w*h;
	g->numTris = numTriangles;
	g->triangles = triangles;
	uint8 *cellOf = (uint8*)RwMalloc(numTriangles);
	if(cellOf == nil){
		RwFree(g);
		return nil;
	}
	int count[COLGRID_MAX*COLGRID_MAX];
	for(int c = 0; c < w*h; c++){
		count[c] = 0;
		g->cellBox[c][0] = g->cellBox[c][1] = 1.0e30f;
		g->cellBox[c][2] = g->cellBox[c][3] = -1.0e30f;
	}
	for(int i = 0; i < numTriangles; i++){
		CVector a = vertices[triangles[i].a].Get();
		CVector b = vertices[triangles[i].b].Get();
		CVector c = vertices[triangles[i].c].Get();
		float cx = (a.x + b.x + c.x)/3.0f, cy = (a.y + b.y + c.y)/3.0f;
		int gx = Clamp((int)((cx - minx)/cell), 0, w-1);
		int gy = Clamp((int)((cy - miny)/cell), 0, h-1);
		int k = gy*w + gx;
		cellOf[i] = k;
		count[k]++;
		float *bx = g->cellBox[k];
		bx[0] = Min(bx[0], Min(a.x, Min(b.x, c.x)));
		bx[1] = Min(bx[1], Min(a.y, Min(b.y, c.y)));
		bx[2] = Max(bx[2], Max(a.x, Max(b.x, c.x)));
		bx[3] = Max(bx[3], Max(a.y, Max(b.y, c.y)));
	}
	int pos = 0;
	for(int c = 0; c < w*h; c++){
		g->start[c] = pos;
		pos += count[c];
	}
	g->start[w*h] = pos;
	int fill[COLGRID_MAX*COLGRID_MAX];
	for(int c = 0; c < w*h; c++)
		fill[c] = g->start[c];
	for(int i = 0; i < numTriangles; i++)
		g->tris[fill[cellOf[i]]++] = i;
	RwFree(cellOf);
	return g;
}
#endif

void
CColModel::RemoveTrianglePlanes(void)
{
#ifdef PSP
	if(trianglePlanes){
		uint8 *base = (uint8*)trianglePlanes - 16;
		CColGrid *g = *(CColGrid**)base;
		if(g)
			RwFree(g);
		RwFree(base);
	}
#else
	RwFree(trianglePlanes);
#endif
	trianglePlanes = nil;
}

void
CColModel::SetLinkPtr(CLink<CColModel*> *lptr)
{
	assert(trianglePlanes);
	*(CLink<CColModel*>**)ALIGNPTR(&trianglePlanes[numTriangles]) = lptr;
}

CLink<CColModel*>*
CColModel::GetLinkPtr(void)
{
	assert(trianglePlanes);
	return *(CLink<CColModel*>**)ALIGNPTR(&trianglePlanes[numTriangles]);
}

void
CColModel::GetTrianglePoint(CVector &v, int i) const
{
	v = vertices[i].Get();
}

CColModel&
CColModel::operator=(const CColModel &other)
{
	int i;
	int numVerts;

	boundingSphere = other.boundingSphere;
	boundingBox = other.boundingBox;

	// copy spheres
	if(other.numSpheres){
		if(numSpheres != other.numSpheres){
			numSpheres = other.numSpheres;
			if(spheres)
				RwFree(spheres);
			spheres = (CColSphere*)RwMalloc(numSpheres*sizeof(CColSphere));
		}
		for(i = 0; i < numSpheres; i++)
			spheres[i] = other.spheres[i];
	}else{
		numSpheres = 0;
		if(spheres)
			RwFree(spheres);
		spheres = nil;
	}

	// copy lines
	if(other.numLines){
		if(numLines != other.numLines){
			numLines = other.numLines;
			if(lines)
				RwFree(lines);
			lines = (CColLine*)RwMalloc(numLines*sizeof(CColLine));
		}
		for(i = 0; i < numLines; i++)
			lines[i] = other.lines[i];
	}else{
		numLines = 0;
		if(lines)
			RwFree(lines);
		lines = nil;
	}

	// copy boxes
	if(other.numBoxes){
		if(numBoxes != other.numBoxes){
			numBoxes = other.numBoxes;
			if(boxes)
				RwFree(boxes);
			boxes = (CColBox*)RwMalloc(numBoxes*sizeof(CColBox));
		}
		for(i = 0; i < numBoxes; i++)
			boxes[i] = other.boxes[i];
	}else{
		numBoxes = 0;
		if(boxes)
			RwFree(boxes);
		boxes = nil;
	}

	// copy mesh
	if(other.numTriangles){
		// copy vertices
		numVerts = 0;
		for(i = 0; i < other.numTriangles; i++){
			if(other.triangles[i].a > numVerts)
				numVerts = other.triangles[i].a;
			if(other.triangles[i].b > numVerts)
				numVerts = other.triangles[i].b;
			if(other.triangles[i].c > numVerts)
				numVerts = other.triangles[i].c;
		}
		numVerts++;
		if(vertices)
			RwFree(vertices);
		if(numVerts){
			vertices = (CompressedVector*)RwMalloc(numVerts*sizeof(CompressedVector));
			for(i = 0; i < numVerts; i++)
				vertices[i] = other.vertices[i];
		}

		// copy triangles
		if(numTriangles != other.numTriangles){
			numTriangles = other.numTriangles;
			if(triangles)
				RwFree(triangles);
			triangles = (CColTriangle*)RwMalloc(numTriangles*sizeof(CColTriangle));
		}
		for(i = 0; i < numTriangles; i++)
			triangles[i] = other.triangles[i];
	}else{
		numTriangles = 0;
		if(triangles)
			RwFree(triangles);
		triangles = nil;
		if(vertices)
			RwFree(vertices);
		vertices = nil;
	}
	return *this;
}
