// Testing aid: a camera tour of the city for screenshots and frame statistics.
//
// autostart.txt containing "tour" (for example "load 1 tour") reads tour.txt
// from the game folder, one stop per line:
//     x y heading [hour] [drive]
// At each stop the player is put on the ground there, facing the heading,
// the world around is loaded, and after 240 frames the screen is saved as
// tourNN.ppm and a TOUR line with the frame statistics goes to the log.
// "drive" puts the player in a car first (the vehicle camera sees further).
#include "common.h"
#include "General.h"
#include "World.h"
#include "PlayerPed.h"
#include "Streaming.h"
#include "Camera.h"
#include "Clock.h"
#include "Weather.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "Pools.h"
#include "ModelIndices.h"
#include "Automobile.h"
#include "CarCtrl.h"
#include "Renderer.h"
#include "psptune.h"
#include "TxdStore.h"
#include "ModelInfo.h"
#include "rwcore.h"

#if defined(PSP) || defined(N3DS)

struct TourStop { float x, y, heading, hour; int drive, flick; };
static TourStop stops[64];
static int numStops = -1, cur, wait, shotPending, calm, flickFrame;

static void
readTour(void)
{
	numStops = 0;
	FILE *f = fopen("autostart.txt", "r");
	if(f == nil) return;
	char buf[128] = { 0 };
	fread(buf, 1, 127, f);
	fclose(f);
	if(strstr(buf, "tour") == nil) return;
	f = fopen("tour.txt", "r");
	if(f == nil){ debug("TOUR no tour.txt\n"); return; }
	char line[128];
	while(numStops < 64 && fgets(line, sizeof(line), f)){
		TourStop t = { 0, 0, 0, 12, 0, 0 };
		if(sscanf(line, "%f %f %f %f", &t.x, &t.y, &t.heading, &t.hour) < 3) continue;
		t.drive = strstr(line, "drive") != nil;
		t.flick = strstr(line, "flick") != nil;	// 4 frames in a row (flicker hunting)
		stops[numStops++] = t;
	}
	fclose(f);
	debug("TOUR %d stops\n", numStops);
	cur = -1;
}

static void
gotoStop(const TourStop &t)
{
	CPlayerPed *pl = FindPlayerPed();
	// outside (a save made in a safehouse starts in an interior)
	CGame::currArea = AREA_MAIN_MAP;
	pl->m_area = AREA_MAIN_MAP;
	CStreaming::RemoveBuildingsNotInArea(AREA_MAIN_MAP);
	CVector tp(t.x, t.y, 20.0f);
	CStreaming::LoadScene(tp);
	bool found;
	float z = CWorld::FindGroundZFor3DCoord(t.x, t.y, 100.0f, &found);
	if(t.drive && !pl->InVehicle()){
		int mi = MI_INFERNUS;
		CStreaming::RequestModel(mi, STREAMFLAGS_DONT_REMOVE);
		CStreaming::LoadAllRequestedModels(false);
		if(CStreaming::ms_aInfoForModel[mi].m_loadState == STREAMSTATE_LOADED){
			CAutomobile *car = new CAutomobile(mi, MISSION_VEHICLE);
			car->SetPosition(CVector(t.x, t.y, z + 1.0f));
			car->SetStatus(STATUS_ABANDONED);
			CWorld::Add(car);
			pl->WarpPedIntoCar(car);
			car->SetStatus(STATUS_PLAYER);
			car->bEngineOn = true;
		}
	}
	if(pl->InVehicle() && pl->m_pMyVehicle){
		CVehicle *v = pl->m_pMyVehicle;
		v->Teleport(CVector(t.x, t.y, z + 1.5f));
		v->SetHeading(DEGTORAD(t.heading));
		v->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	}else{
		pl->Teleport(CVector(t.x, t.y, z + 1.0f));
		pl->m_fRotationCur = pl->m_fRotationDest = DEGTORAD(t.heading);
		pl->SetHeading(DEGTORAD(t.heading));
	}
	TheCamera.SetCameraDirectlyBehindForFollowPed_CamOnAString();
	CClock::SetGameClock((int)t.hour, (int)((t.hour - (int)t.hour)*60.0f));
	CWeather::ForceWeatherNow(WEATHER_SUNNY);
	debug("TOUR stop %d at %d %d heading %d hour %d%s\n", cur, (int)t.x, (int)t.y, (int)t.heading, (int)t.hour,
		t.drive ? " (driving)" : "");
}

// called once per frame from the pad code
void
pspTourTick(void)
{
	if(numStops < 0) readTour();
	if(numStops == 0 || cur >= numStops) return;
	if(FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::IsRunning()) return;
	CPlayerPed *pl = FindPlayerPed();
	if(pl == nil) return;
	if(cur < 0){
		if(++calm < 300) return;
		cur = 0;
		gotoStop(stops[0]);
		wait = 0;
		return;
	}
	// hold the clock: comparable lighting between runs
	CClock::SetGameClock((int)stops[cur].hour, (int)((stops[cur].hour - (int)stops[cur].hour)*60.0f));
	if(++wait == 240)
		shotPending = 1;
}

extern uint32 pspBusyAvgUs, pspBusyMaxUs;
extern int pspVisibleEntities;

// called after a frame's display list is closed
void
pspTourShot(void)
{
	if(!shotPending) return;
	rw::Image *img = rw::psp::grabFrameBuffer();
	char name[32];
	if(stops[cur].flick){
		sprintf(name, "tour%02d_%d.ppm", cur, flickFrame);
		if(++flickFrame < 4){
			FILE *f = fopen(name, "wb");
			if(f && img){
				fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
				for(int y = 0; y < img->height; y++)
					for(int x = 0; x < img->width; x++)
						fwrite(img->pixels + y*img->stride + x*4, 1, 3, f);
			}
			if(f) fclose(f);
			if(img) img->destroy();
			return;
		}
		flickFrame = 0;
	}
	shotPending = 0;
	sprintf(name, "tour%02d.ppm", cur);
	FILE *f = fopen(name, "wb");
	if(f && img){
		fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
		for(int y = 0; y < img->height; y++)
			for(int x = 0; x < img->width; x++)
				fwrite(img->pixels + y*img->stride + x*4, 1, 3, f);
	}
	if(f) fclose(f);
	if(img) img->destroy();
	debug("TOUR shot %d: draw calls %u verts %u, entities visible %d, draw distance %d, streaming %d KB\n", cur,
#ifndef N3DS
		(unsigned)rw::psp::lastFrameDraws, (unsigned)rw::psp::lastFrameVerts,
#else
		0u, 0u,
#endif
		pspVisibleEntities, (int)pspDynDrawDistance, (int)(CStreaming::ms_memoryUsed/1024));
	// what is drawn: model, distance, low detail or not, its textures loaded
	{
		CVector cam = TheCamera.GetPosition();
		for(int i = 0; i < CRenderer::ms_nNoOfVisibleEntities && i < 80; i++){
			CEntity *e = CRenderer::ms_aVisibleEntityPtrs[i];
			CBaseModelInfo *mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			int txd = mi->GetTxdSlot();
			TxdDef *td = CTxdStore::GetSlot(txd);
			debug("TOUR  vis %s dist %d lod %d txd %s loaded %d\n", mi->GetModelName(),
				(int)(e->GetPosition() - cam).Magnitude(), e->bIsBIGBuilding ? 1 : 0,
				td ? td->name : "-", td && td->texDict ? 1 : 0);
			// materials of the close ones
			if(e->m_rwObject && RwObjectGetType(e->m_rwObject) == rpATOMIC &&
			   (e->GetPosition() - cam).Magnitude() < 120.0f){
				rw::Atomic *at = (rw::Atomic*)e->m_rwObject;
				rw::Geometry *g = at->geometry;
				int nv = g->numVertices; int sr = 0, sg = 0, sb = 0;
				rw::RGBA *pc = g->colors;
				if(pc) for(int v = 0; v < nv; v++){ sr += pc[v].red; sg += pc[v].green; sb += pc[v].blue; }
				debug("TOUR    geo verts %d tris %d prelit %d avg %d %d %d flags %x\n", nv, g->numTriangles, pc != nil,
					nv && pc ? sr/nv : 0, nv && pc ? sg/nv : 0, nv && pc ? sb/nv : 0, g->flags);
				for(int m = 0; m < g->matList.numMaterials && m < 8; m++){
					rw::Material *mt = g->matList.materials[m];
					rw::Texture *tx = mt->texture;
					rw::Raster *r = tx ? tx->raster : nil;
					debug("TOUR    mat %d col %d %d %d %d tex %s ras %dx%d fmt %x surf %d %d %d\n", m,
						mt->color.red, mt->color.green, mt->color.blue, mt->color.alpha,
						tx ? tx->name : "-", r ? r->width : 0, r ? r->height : 0, r ? r->format : 0,
						(int)(mt->surfaceProps.ambient*100), (int)(mt->surfaceProps.diffuse*100), (int)(mt->surfaceProps.specular*100));
				}
			}
		}
	}
	cur++;
	if(cur < numStops){
		gotoStop(stops[cur]);
		wait = 0;
	}else
		debug("TOUR done\n");
}

#else
void pspTourTick(void) {}
void pspTourShot(void) {}
#endif
