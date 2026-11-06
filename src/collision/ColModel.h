#ifndef __GTA_COLMODEL_H__
#define __GTA_COLMODEL_H__

#include "templates.h"
#include "ColBox.h"
#include "ColSphere.h"
#include "ColLine.h"
#include "ColPoint.h"
#include "ColTriangle.h"

#ifdef PSP
// PSP: the triangles of a big collision model sorted into the cells of a
// grid over its x/y extent, so a car or a wheel only tests the triangles
// near it instead of the whole ground model
#define COLGRID_MAX 8
struct CColGrid
{
	int32 numCells;
	int32 numTris;	// what it was made for (the model may change)
	void *triangles;
	float cellBox[COLGRID_MAX*COLGRID_MAX][4];	// x/y bounds of the cell's triangles
	uint16 start[COLGRID_MAX*COLGRID_MAX+1];
	uint16 tris[1];	// (numTriangles)
};
#endif

struct CColModel
{
	CSphere boundingSphere;
	CBox boundingBox;
	int16 numSpheres;
	int16 numBoxes;
	int16 numTriangles;
	int8 numLines;
	uint8 level;	// colstore slot but probably still named level
	bool ownsCollisionVolumes;
	CColSphere *spheres;
	CColLine *lines;
	CColBox *boxes;
	CompressedVector *vertices;
	CColTriangle *triangles;
	CColTrianglePlane *trianglePlanes;

	CColModel(void);
	~CColModel(void);
	void RemoveCollisionVolumes(void);
	void CalculateTrianglePlanes(void);
	void RemoveTrianglePlanes(void);
	CLink<CColModel*> *GetLinkPtr(void);
	void SetLinkPtr(CLink<CColModel*>*);
	void GetTrianglePoint(CVector &v, int i) const;
#ifdef PSP
	CColGrid *GetGrid(void) const {
		if(trianglePlanes == nil) return nil;
		CColGrid *g = *(CColGrid**)((uint8*)trianglePlanes - 16);
		return g && g->numTris == numTriangles && g->triangles == (void*)triangles ? g : nil;
	}
	void *GetLinkSlot(void) const;
	CColGrid *BuildGrid(void);
#endif

	void *operator new(size_t) throw();
	void operator delete(void *p, size_t) throw();
	CColModel& operator=(const CColModel& other);
};

#endif // __GTA_COLMODEL_H__
