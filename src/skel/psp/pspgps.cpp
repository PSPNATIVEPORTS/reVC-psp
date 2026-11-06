// GPS for the PSP port (like the PC VC.GPS mod): while driving, the road
// route to the map waypoint is drawn on the radar and on the pause map.
// Set the waypoint in the pause menu map with Cross; it is removed when
// you get there. Without a waypoint, during missions it leads to the
// nearest mission blip.
#ifdef PSP
#include "common.h"
#include "Radar.h"
#include "PathFind.h"
#include "World.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "Frontend.h"
#include "Timer.h"
#include "Sprite2d.h"
#include "Camera.h"
#include "main.h"
#include "Script.h"
#include "Pools.h"
#include "Object.h"

#define GPS_MAX_NODES 512

static CPathNode *gpsNodes[GPS_MAX_NODES];
static int16 gpsNumNodes;
static uint32 gpsLastSearch;
static CVector gpsTarget;
static int32 gpsTargetId = -1;

static const CRGBA gpsWaypointColour(190, 60, 240, 255);
// the line has the colour of what it leads to: the waypoint, or the
// mission blip (the same colour as the blip on the radar)
static CRGBA gpsColour = gpsWaypointColour;
static int32 gpsBlip = -1;

// Where the GPS leads: the map waypoint if one is set, else during a mission
// the nearest mission blip (destination, car or person to go to)
static bool gpsActive;

static bool
missionTarget(CVehicle *veh, CVector &out, int32 &id)
{
	gpsBlip = -1;
	if(!CTheScripts::IsPlayerOnAMission())
		return false;
	CVector pos = veh->GetPosition();
	float best = 1e12f;
	bool found = false;
	// Blips of the mission being played only: the side scripts (Sunshine
	// Autos, the asset businesses...) have blips of their own, and when one of
	// those was nearer the GPS went there instead of to the mission
	extern uint8 pspBlipFromMission[NUMRADARBLIPS];
	for(int i = 0; i < NUMRADARBLIPS; i++){
		sRadarTrace &t = CRadar::ms_RadarTrace[i];
		if(!t.m_bInUse || t.m_eRadarSprite != RADAR_SPRITE_NONE || t.m_eBlipDisplay == BLIP_DISPLAY_NEITHER)
			continue;
		// only the mission's own blips: the businesses' blips (Sunshine Autos
		// and the others) were still taken when the mission had none showing
		if(!pspBlipFromMission[i])
			continue;
		if(CRadar::TargetMarkerId != -1 && CRadar::GetActualBlipArrayIndex(CRadar::TargetMarkerId) == i)
			continue;
		CVector p;
		switch(t.m_eBlipType){
		case BLIP_COORD:
		case BLIP_CONTACT_POINT:
			p = t.m_vecPos;
			break;
		case BLIP_CAR: {
			CVehicle *v = CPools::GetVehiclePool()->GetAt(t.m_nEntityHandle);
			if(v == nil || v == veh) continue;
			p = v->GetPosition();
			break;
		}
		case BLIP_CHAR: {
			CPed *pd = CPools::GetPedPool()->GetAt(t.m_nEntityHandle);
			if(pd == nil) continue;
			if(pd->InVehicle() && pd->m_pMyVehicle){
				if(pd->m_pMyVehicle == veh) continue;	// already with you
				p = pd->m_pMyVehicle->GetPosition();
			}else
				p = pd->GetPosition();
			break;
		}
		case BLIP_OBJECT: {
			CObject *o = CPools::GetObjectPool()->GetAt(t.m_nEntityHandle);
			if(o == nil) continue;
			p = o->GetPosition();
			break;
		}
		default:
			continue;
		}
		float d = (CVector2D(p) - CVector2D(pos)).MagnitudeSqr();
		if(d < best){
			best = d;
			out = p;
			id = 0x10000 + i;	// any value that changes with the blip
			gpsBlip = i;
			found = true;
		}
	}
	return found;
}

// path search now and then, drawing every frame
void
pspGpsUpdate(void)
{
	CVehicle *veh = FindPlayerVehicle();
	gpsActive = false;
	if(veh == nil || !veh->IsCar() && !veh->IsBike()){
		gpsNumNodes = 0;
		gpsTargetId = -1;
		return;
	}
	CVector pos = veh->GetPosition();
	CVector target;
	int32 id;
	if(CRadar::TargetMarkerId != -1){
		target = CRadar::TargetMarkerPos;
		id = CRadar::TargetMarkerId;
		// arrived: remove the waypoint like the GPS mod does
		if((CVector2D(pos) - CVector2D(target)).MagnitudeSqr() < SQR(25.0f)){
			CRadar::ToggleTargetMarker(0.0f, 0.0f);
			gpsNumNodes = 0;
			gpsTargetId = -1;
			return;
		}
	}else if(!missionTarget(veh, target, id)){
		gpsNumNodes = 0;
		gpsTargetId = -1;
		return;
	}
	gpsActive = true;
	if(CRadar::TargetMarkerId == -1 && gpsBlip >= 0){
		sRadarTrace &t = CRadar::ms_RadarTrace[gpsBlip];
		uint32 c = CRadar::GetRadarTraceColour(t.m_nColor, t.m_bDim);
		gpsColour = CRGBA(c >> 24, c >> 16, c >> 8, 255);
		static int32 loggedBlip = -2;
		if(gpsBlip != loggedBlip){
			loggedBlip = gpsBlip;
			{ extern uint8 pspBlipFromMission[NUMRADARBLIPS]; debug("GPS to blip %d (colour %u, dim %d, mission %d, type %d at %.0f %.0f): line %06x\n", gpsBlip, (unsigned)t.m_nColor, (int)t.m_bDim, (int)pspBlipFromMission[gpsBlip], (int)t.m_eBlipType, t.m_vecPos.x, t.m_vecPos.y, (unsigned)(c >> 8)); }
		}
	}else
		gpsColour = gpsWaypointColour;
	// close to a mission target: the route is just in the way now
	if((CVector2D(pos) - CVector2D(target)).MagnitudeSqr() < SQR(15.0f)){
		gpsNumNodes = 0;
		return;
	}
	uint32 now = CTimer::GetTimeInMilliseconds();
	gpsTarget = target;	// moving targets: keep the end of the line on them
	if(id == gpsTargetId && now - gpsLastSearch < 1000)
		return;
	gpsLastSearch = now;
	gpsTargetId = id;
	target.z = pos.z;
	float dist;
	ThePaths.DoPathSearch(PATH_CAR, pos, -1, target, gpsNodes, &gpsNumNodes, GPS_MAX_NODES, nil, &dist, 999999.0f, -1);
}

// cut segment a-b to the unit circle (radar space); false if it's all outside
static bool
clipToCircle(CVector2D &a, CVector2D &b)
{
	CVector2D d = b - a;
	float A = d.x*d.x + d.y*d.y;
	if(A < 1e-8f)
		return a.MagnitudeSqr() <= 1.0f;
	float B = 2.0f*(a.x*d.x + a.y*d.y);
	float C = a.MagnitudeSqr() - 1.0f;
	float disc = B*B - 4.0f*A*C;
	if(disc <= 0.0f)
		return false;
	float s = Sqrt(disc);
	float t0 = (-B - s) / (2.0f*A);
	float t1 = (-B + s) / (2.0f*A);
	if(t0 < 0.0f) t0 = 0.0f;
	if(t1 > 1.0f) t1 = 1.0f;
	if(t0 >= t1)
		return false;
	CVector2D na = a + d*t0;
	CVector2D nb = a + d*t1;
	a = na;
	b = nb;
	return true;
}

#ifdef N3DS
// 3DS bottom screen: cut segment a-b to the map rectangle
static bool
clipToRect(CVector2D &a, CVector2D &b, float ex, float ey)
{
	float t0 = 0.0f, t1 = 1.0f;
	float d[2] = { b.x - a.x, b.y - a.y };
	float p0[2] = { a.x, a.y };
	float e[2] = { ex, ey };
	for(int i = 0; i < 2; i++){
		// -e <= p0 + t*d <= e
		if(d[i] == 0.0f){
			if(p0[i] < -e[i] || p0[i] > e[i])
				return false;
			continue;
		}
		float ta = (-e[i] - p0[i]) / d[i];
		float tb = (e[i] - p0[i]) / d[i];
		if(ta > tb){ float t = ta; ta = tb; tb = t; }
		if(ta > t0) t0 = ta;
		if(tb < t1) t1 = tb;
		if(t0 >= t1)
			return false;
	}
	CVector2D dd = b - a;
	CVector2D na = a + dd*t0;
	CVector2D nb = a + dd*t1;
	a = na;
	b = nb;
	return true;
}
#endif

#define GPS_MAX_VERTS (6*96)
static RwIm2DVertex gpsVerts[GPS_MAX_VERTS];
static int gpsNumVerts;

static float gpsScreenZ, gpsCamZ, gpsRecipZ;

static void
addVert(float x, float y)
{
	RwIm2DVertex *v = &gpsVerts[gpsNumVerts++];
	RwIm2DVertexSetScreenX(v, x);
	RwIm2DVertexSetScreenY(v, y);
	RwIm2DVertexSetScreenZ(v, gpsScreenZ);
	RwIm2DVertexSetCameraZ(v, gpsCamZ);
	RwIm2DVertexSetRecipCameraZ(v, gpsRecipZ);
	RwIm2DVertexSetIntRGBA(v, gpsColour.r, gpsColour.g, gpsColour.b, gpsColour.a);
	RwIm2DVertexSetU(v, 0.0f, gpsRecipZ);
	RwIm2DVertexSetV(v, 0.0f, gpsRecipZ);
}

static void
flushVerts(void)
{
	if(gpsNumVerts == 0)
		return;
	RwIm2DRenderPrimitive(rwPRIMTYPETRILIST, gpsVerts, gpsNumVerts);
	gpsNumVerts = 0;
}

// a thick line from a to b (screen space); w = half width
static void
addSegment(const CVector2D &a, const CVector2D &b, float w)
{
	CVector2D d = b - a;
	float len = d.Magnitude();
	if(len < 0.01f)
		return;
	// extend a little at both ends so the joints have no gaps
	CVector2D dir = d * (1.0f/len);
	CVector2D n(-dir.y*w, dir.x*w);
	CVector2D p0 = a - dir*w*0.7f, p1 = b + dir*w*0.7f;
	if(gpsNumVerts + 6 > GPS_MAX_VERTS)
		flushVerts();
	addVert(p0.x + n.x, p0.y + n.y);
	addVert(p1.x + n.x, p1.y + n.y);
	addVert(p1.x - n.x, p1.y - n.y);
	addVert(p0.x + n.x, p0.y + n.y);
	addVert(p1.x - n.x, p1.y - n.y);
	addVert(p0.x - n.x, p0.y - n.y);
}

static CVector2D
toRadar(const CVector2D &world)
{
	CVector2D r;
	CRadar::TransformRealWorldPointToRadarSpace(r, world);
	return r;
}

// on the radar (in game) or the pause map (FrontEndMenuManager.m_bMenuMapActive)
void
pspGpsDraw(void)
{
	if(gpsNumNodes <= 0 || !gpsActive)
		return;
	CVehicle *veh = FindPlayerVehicle();
	if(veh == nil)
		return;
	bool menu = FrontEndMenuManager.m_bMenuMapActive;
	float w = menu ? SCREEN_SCALE_X(1.5f) : SCREEN_SCALE_X(2.0f);

	gpsScreenZ = RwIm2DGetNearScreenZ();
	gpsCamZ = RwCameraGetNearClipPlane(Scene.camera);
	gpsRecipZ = 1.0f / gpsCamZ;
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATESHADEMODE, (void*)rwSHADEMODEFLAT);

	// car -> nodes -> waypoint
	CVector2D prev = toRadar(CVector2D(veh->GetPosition()));
	for(int i = 0; i <= gpsNumNodes; i++){
		CVector2D cur = i < gpsNumNodes ? toRadar(gpsNodes[i]->GetPosition()) : toRadar(CVector2D(gpsTarget));
		CVector2D a = prev, b = cur;
		prev = cur;
#ifdef N3DS
		if(!menu && gN3dsRadarBottom){
			if(!clipToRect(a, b, 1.0f, gN3dsRadarAspect))
				continue;
		}else
#endif
		if(!menu && !clipToCircle(a, b))
			continue;
		CVector2D sa, sb;
		CRadar::TransformRadarPointToScreenSpace(sa, a);
		CRadar::TransformRadarPointToScreenSpace(sb, b);
		addSegment(sa, sb, w);
	}
	flushVerts();

	RwRenderStateSet(rwRENDERSTATESHADEMODE, (void*)rwSHADEMODEGOURAUD);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
}
#endif
