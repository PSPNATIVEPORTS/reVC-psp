// Simple in-game cheat menu for the PSP port.
// Open/close: hold L + R and press Select (not in menus or cutscenes).
// D-pad up/down chooses, left/right changes the weapon set or car (L / R:
// 10 cars at a time),
// Cross applies, Circle closes.
#ifdef PSP
#include <pspctrl.h>
#include "common.h"
#include "Font.h"
#include "Sprite2d.h"
#include "Frontend.h"
#include "CutsceneMgr.h"
#include "PlayerPed.h"
#include "World.h"
#include "Streaming.h"
#include "ModelIndices.h"
#include "VehicleModelInfo.h"
#include "Automobile.h"
#include "Bike.h"
#include "Hud.h"
#include "General.h"
#include "Camera.h"
#include "psptune.h"
#include "Pools.h"
#include "CarCtrl.h"
#include "PathFind.h"
#include "main.h"
#include "Coronas.h"
#include "Sprite.h"

// the game's own cheats (Pad.cpp)
void HealthCheat();
void ArmourCheat();
void WeaponCheat1();
void WeaponCheat2();
void WeaponCheat3();
void WantedLevelDownCheat();

static bool menuOpen;
static int sel;
static int weaponSet;
static int carSel;
static uint32 oldButtons;
static char message[64];
static uint32 messageTime;

enum { CM_HEALTH, CM_WANTED, CM_WEAPONS, CM_CAR, CM_MONEY, CM_CLOSE, CM_NUM_ITEMS, CM_FXTEST = 100, CM_SHOT, CM_CRASHTEST };

// black squares hunt: each mode turns one thing off or tries one fix
static const char *fxTestNames[] = {
	"off (normal)",
	"1: no smoke",
	"2: smoke + glows without picture",
	"3: smoke + glows new method",
	"4: no light glows",
	"5: test pictures",
};
#define NUM_FXTEST 6

static const char *weaponSetNames[] = { "Light (pistol, shotgun)", "Heavy (M4, rocket launcher)", "Nutter (minigun)" };

// every car, bike and helicopter a player can drive (no boats, planes, RC toys
// or the police's AI helicopter)
struct CheatCar { int id; const char *name; };
static CheatCar cars[] = {
	{ MI_INFERNUS, "Infernus" }, { MI_CHEETAH, "Cheetah" }, { MI_BANSHEE, "Banshee" },
	{ MI_COMET, "Comet" }, { MI_STINGER, "Stinger" }, { MI_DELUXO, "Deluxo" },
	{ MI_PHEONIX, "Phoenix" }, { MI_SABRE, "Sabre" }, { MI_SABRETUR, "Sabre Turbo" },
	{ MI_STALLION, "Stallion" }, { MI_VOODOO, "Voodoo" }, { MI_ESPERANT, "Esperanto" },
	{ MI_HERMES, "Hermes" }, { MI_CUBAN, "Cuban Hermes" }, { MI_HOTRING, "Hotring Racer" },
	{ MI_HOTRINA, "Hotring Racer 2" }, { MI_HOTRINB, "Hotring Racer 3" }, { MI_BLOODRA, "Bloodring Banger" },
	{ MI_BLOODRB, "Bloodring Banger 2" }, { MI_SENTINEL, "Sentinel" }, { MI_SENTXS, "Sentinel XS" },
	{ MI_ADMIRAL, "Admiral" }, { MI_WASHING, "Washington" }, { MI_STRETCH, "Stretch" },
	{ MI_LANDSTAL, "Landstalker" }, { MI_IDAHO, "Idaho" }, { MI_PEREN, "Perennial" },
	{ MI_MANANA, "Manana" }, { MI_MOONBEAM, "Moonbeam" }, { MI_GLENDALE, "Glendale" },
	{ MI_OCEANIC, "Oceanic" }, { MI_WALTON, "Walton" }, { MI_REGINA, "Regina" },
	{ MI_VIRGO, "Virgo" }, { MI_GREENWOO, "Greenwood" }, { MI_BLISTAC, "Blista Compact" },
	{ MI_RANCHER, "Rancher" }, { MI_SANDKING, "Sandking" }, { MI_MESA, "Mesa Grande" },
	{ MI_PATRIOT, "Patriot" }, { MI_BFINJECT, "BF Injection" }, { MI_BOBCAT, "Bobcat" },
	{ MI_TAXI, "Taxi" }, { MI_CABBIE, "Cabbie" }, { MI_ZEBRA, "Zebra Cab" },
	{ MI_KAUFMAN, "Kaufman Cab" }, { MI_POLICE, "Police" }, { MI_VICECHEE, "Vice Squad Cheetah" },
	{ MI_FBICAR, "FBI Washington" }, { MI_FBIRANCH, "FBI Rancher" }, { MI_ENFORCER, "Enforcer" },
	{ MI_RHINO, "Rhino (tank)" }, { MI_BARRACKS, "Barracks OL" }, { MI_AMBULAN, "Ambulance" },
	{ MI_FIRETRUCK, "Firetruck" }, { MI_SECURICA, "Securicar" }, { MI_ROMERO, "Romero's Hearse" },
	{ MI_LOVEFIST, "Love Fist" }, { MI_MRWHOOP, "Mr Whoopee" }, { MI_PONY, "Pony" },
	{ MI_RUMPO, "Rumpo" }, { MI_BURRITO, "Burrito" }, { MI_GANGBUR, "Gang Burrito" },
	{ MI_SPAND, "Spand Express" }, { MI_TOPFUN, "Top Fun" }, { MI_BOXVILLE, "Boxville" },
	{ MI_BENSON, "Benson" }, { MI_MULE, "Mule" }, { MI_YANKEE, "Yankee" },
	{ MI_FLATBED, "Flatbed" }, { MI_PACKER, "Packer" }, { MI_LINERUN, "Linerunner" },
	{ MI_TRASH, "Trashmaster" }, { MI_BUS, "Bus" }, { MI_COACH, "Coach" },
	{ MI_CADDY, "Caddy" }, { MI_BAGGAGE, "Baggage Handler" },
	{ MI_PCJ600, "PCJ-600 (bike)" }, { MI_FREEWAY, "Freeway (bike)" }, { MI_SANCHEZ, "Sanchez (bike)" },
	{ MI_ANGEL, "Angel (bike)" }, { MI_FAGGIO, "Faggio (bike)" }, { MI_PIZZABOY, "Pizza Boy (bike)" },
	{ MI_MAVERICK, "Maverick (heli)" }, { MI_POLMAV, "Police Maverick (heli)" }, { MI_VCNMAV, "VCN Maverick (heli)" },
	{ MI_SPARROW, "Sparrow (heli)" }, { MI_SEASPAR, "Sea Sparrow (heli)" }, { MI_HUNTER, "Hunter (heli)" },
};
#define NUM_CARS (int)(sizeof(cars)/sizeof(cars[0]))

static void
showMessage(const char *msg)
{
	strncpy(message, msg, sizeof(message)-1);
	messageTime = CTimer::GetTimeInMillisecondsPauseMode();
}

static void
spawnCar(int model)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);
	if(CStreaming::ms_aInfoForModel[model].m_loadState != STREAMSTATE_LOADED){
		showMessage("Not enough memory for that car here");
		return;
	}
	CStreaming::SetModelIsDeletable(model);
	CStreaming::SetModelTxdIsDeletable(model);

	// in front of the player (or of the player's vehicle), same heading
	CEntity *ref = player->InVehicle() && player->m_pMyVehicle ? (CEntity*)player->m_pMyVehicle : (CEntity*)player;
	CVector fwd = ref->GetForward();
	fwd.z = 0.0f;
	fwd.Normalise();
	float ahead = player->InVehicle() ? 8.0f : 5.0f;
	CVector pos = ref->GetPosition() + fwd*ahead;
	bool found;
	float z = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 3.0f, &found);
	pos.z = (found ? z : pos.z) + 1.5f;

	CVehicleModelInfo *mi = (CVehicleModelInfo*)CModelInfo::GetModelInfo(model);
	CVehicle *veh;
	// a random vehicle (as the game's own car cheats with FIX_BUGS): once it's
	// left behind, out of sight, the game removes it like any other car
	if(mi->m_vehicleType == VEHICLE_TYPE_BIKE)
		veh = new CBike(model, RANDOM_VEHICLE);
	else
		veh = new CAutomobile(model, RANDOM_VEHICLE);
	if(veh == nil){
		showMessage("Too many cars around");
		return;
	}
	veh->SetPosition(pos);
	veh->SetHeading(CGeneral::GetATanOfXY(fwd.x, fwd.y) - HALFPI);
	veh->SetStatus(STATUS_ABANDONED);
	veh->m_nDoorLock = CARLOCK_UNLOCKED;
	CWorld::Add(veh);
	showMessage("Car delivered");
}

// testing aid (autostart.txt "drive"): put the player in a car that
// drives itself around town, the camera follows it
void
pspTestDrive(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || player->InVehicle())
		return;
	extern char pspTestDriveModelName[24];
	int model = MI_INFERNUS;
	if(pspTestDriveModelName[0]){
		int32 id;
		if(CModelInfo::GetModelInfo(pspTestDriveModelName, &id))
			model = id;
	}
	spawnCar(model);
	CVehicle *veh = nil;
	float best = 1e9f;
	int n = CPools::GetVehiclePool()->GetSize();
	for(int i = 0; i < n; i++){
		CVehicle *v = CPools::GetVehiclePool()->GetSlot(i);
		if(v == nil || v->GetModelIndex() != model) continue;
		float d = (v->GetPosition() - player->GetPosition()).MagnitudeSqr();
		if(d < best){ best = d; veh = v; }
	}
	if(veh == nil)
		return;
	int node = ThePaths.FindNodeClosestToCoors(player->GetPosition(), PATH_CAR, 200.0f);
	if(node >= 0){
		CVector np = ThePaths.m_pathNodes[node].GetPosition();
		bool found;
		float z = CWorld::FindGroundZFor3DCoord(np.x, np.y, np.z + 3.0f, &found);
		veh->Teleport(CVector(np.x, np.y, (found ? z : np.z) + 1.0f));
	}
	veh->bExplosionProof = veh->bBulletProof = veh->bFireProof = veh->bCollisionProof = veh->bMeleeProof = true;
	player->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, veh);
	player->WarpPedIntoCar(veh);
	veh->SetStatus(STATUS_PHYSICS);
	veh->bEngineOn = true;
	veh->AutoPilot.m_nCarMission = MISSION_CRUISE;
	extern int pspTestDriveSpeed;
	veh->AutoPilot.m_nCruiseSpeed = pspTestDriveSpeed;
	if(pspTestDriveSpeed == 0)
		veh->AutoPilot.m_nCarMission = MISSION_NONE;
	extern int pspTestPlayerDrives;
	if(pspTestPlayerDrives)
		veh->SetStatus(STATUS_PLAYER);	// "holdx": the pad drives
	veh->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	CCarCtrl::JoinCarWithRoadSystem(veh);
	debug("DRIVE started\n");
}

static void
apply(void)
{
	switch(sel){
	case CM_HEALTH:
		HealthCheat();
		ArmourCheat();
		showMessage("Health and armour full");
		break;
	case CM_WANTED:
		WantedLevelDownCheat();
		showMessage("Wanted level cleared");
		break;
	case CM_WEAPONS:
		if(weaponSet == 0) WeaponCheat1();
		else if(weaponSet == 1) WeaponCheat2();
		else WeaponCheat3();
		showMessage("Weapons given");
		break;
	case CM_CAR:
		spawnCar(cars[carSel].id);
		break;
	case CM_CRASHTEST: {
		// checks the crash catcher: a red screen and reVC_crash.txt
		extern void pspLogFlush(void);
		extern void pspLog(const char *);
		pspLog("CRASH TEST from the cheat menu\n");
		pspLogFlush();
		*(volatile int*)4 = 1;
		break;
	}
	case CM_MONEY:
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 200000;
		showMessage("$200,000 added");
		break;
	case CM_SHOT: {
		// a second later, without the menu on screen
		extern int pspShotCountdown;
		pspShotCountdown = 30;
		menuOpen = false;
		break;
	}
	case CM_CLOSE:
		menuOpen = false;
		break;
	}
}

// Called with the raw PSP buttons every frame. Returns true when the menu
// takes the input (the game then gets no buttons this frame).
bool
pspCheatMenuInput(uint32 b)
{
	uint32 pressed = b & ~oldButtons;
	oldButtons = b;

	bool allowed = !FrontEndMenuManager.m_bMenuActive && !CCutsceneMgr::IsRunning() && FindPlayerPed() != nil;
	if(!allowed){
		menuOpen = false;
		return false;
	}
	bool combo = (b & PSP_CTRL_LTRIGGER) && (b & PSP_CTRL_RTRIGGER) && (pressed & PSP_CTRL_SELECT);
	if(combo){
		menuOpen = !menuOpen;
		return true;
	}
	if(!menuOpen)
		return false;

	if(pressed & PSP_CTRL_UP) sel = (sel + CM_NUM_ITEMS - 1) % CM_NUM_ITEMS;
	if(pressed & PSP_CTRL_DOWN) sel = (sel + 1) % CM_NUM_ITEMS;
	int dir = (pressed & PSP_CTRL_RIGHT) ? 1 : (pressed & PSP_CTRL_LEFT) ? -1 : 0;
	if(dir){
		if(sel == CM_WEAPONS) weaponSet = (weaponSet + 3 + dir) % 3;
		if(sel == CM_CAR) carSel = (carSel + NUM_CARS + dir) % NUM_CARS;
		if(sel == CM_FXTEST){
			int m = (rw::psp::fxTest + NUM_FXTEST + dir) % NUM_FXTEST;
			rw::psp::fxTest = m;
			debug("FXTEST mode %s\n", fxTestNames[m]);
		}
	}
	// L / R: 10 cars back or forward
	int jump = (pressed & PSP_CTRL_RTRIGGER) ? 10 : (pressed & PSP_CTRL_LTRIGGER) ? -10 : 0;
	if(jump && sel == CM_CAR)
		carSel = ((carSel + jump) % NUM_CARS + NUM_CARS) % NUM_CARS;
	if(pressed & PSP_CTRL_CROSS) apply();
	if(pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) menuOpen = false;
	return true;
}

void
pspCheatMessage(const char *msg)
{
	showMessage(msg);
}

bool
pspCheatMenuOpen(void)
{
	return menuOpen;
}

static void
printLine(float x, float y, const char *s, const CRGBA &col)
{
	wchar buf[96];
	AsciiToUnicode(s, buf);
	CFont::SetColor(col);
	CFont::PrintString(x, y, buf);
}

// squares hunt: the smoke picture drawn a few different ways, side by side
extern RwRaster *gpSmoke2Raster;
static void
quad(float x, float y, float s, RwRaster *ras, uint8 a, bool add)
{
	RwIm2DVertex v[4];
	float z = RwIm2DGetNearScreenZ(), rz = 1.0f/RwCameraGetNearClipPlane(Scene.camera);
	float xs[4] = { x, x, x+s, x+s }, ys[4] = { y, y+s, y+s, y }, us[4] = { 0, 0, 1, 1 }, vs[4] = { 0, 1, 1, 0 };
	for(int i = 0; i < 4; i++){
		RwIm2DVertexSetScreenX(&v[i], xs[i]);
		RwIm2DVertexSetScreenY(&v[i], ys[i]);
		RwIm2DVertexSetScreenZ(&v[i], z);
		RwIm2DVertexSetRecipCameraZ(&v[i], rz);
		RwIm2DVertexSetIntRGBA(&v[i], 255, 255, 255, a);
		RwIm2DVertexSetU(&v[i], us[i], rz);
		RwIm2DVertexSetV(&v[i], vs[i], rz);
	}
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)(add ? rwBLENDONE : rwBLENDSRCALPHA));
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)(add ? rwBLENDONE : rwBLENDINVSRCALPHA));
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, ras);
	RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, v, 4);
}
static void
drawTestPictures(void)
{
	// mid grey background so the soft edges show
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
	CSprite2d::DrawRect(CRect(SCREEN_SCALE_X(5.0f), SCREEN_SCALE_Y(30.0f), SCREEN_SCALE_X(400.0f), SCREEN_SCALE_Y(110.0f)), CRGBA(60, 60, 140, 255));
	float s = SCREEN_SCALE_X(64.0f), y = SCREEN_SCALE_Y(38.0f);
	quad(SCREEN_SCALE_X(10.0f), y, s, gpSmoke2Raster, 255, false);
	quad(SCREEN_SCALE_X(80.0f), y, s, gpSmoke2Raster, 100, false);
	rw::psp::im2DThrough = 1;
	quad(SCREEN_SCALE_X(150.0f), y, s, gpSmoke2Raster, 255, false);
	rw::psp::im2DThrough = 0;
	quad(SCREEN_SCALE_X(220.0f), y, s, RwTextureGetRaster(gpCoronaTexture[0]), 255, true);
	// the game's own sprite function
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, gpSmoke2Raster);
	CSprite::RenderOneXLUSprite(SCREEN_SCALE_X(322.0f), y + s*0.5f, RwIm2DGetNearScreenZ(), s*0.5f, s*0.5f,
		255, 255, 255, 255, 1.0f/RwCameraGetNearClipPlane(Scene.camera), 255);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
}

void
pspCheatMenuDraw(void)
{
	if(rw::psp::fxTest == 5)
		drawTestPictures();
	bool showMsg = message[0] && CTimer::GetTimeInMillisecondsPauseMode() - messageTime < 2500;
	if(!menuOpen && !showMsg && rw::psp::fxTest == 0)
		return;

	CFont::SetBackgroundOff();
	CFont::SetPropOn();
	CFont::SetJustifyOff();
	CFont::SetRightJustifyOff();
	CFont::SetCentreOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetFontStyle(FONT_STANDARD);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
	CFont::SetScale(SCREEN_SCALE_X(0.45f), SCREEN_SCALE_Y(0.9f));

	if(menuOpen){
		float x = SCREEN_SCALE_X(30.0f), y = SCREEN_SCALE_Y(60.0f);
		float lh = SCREEN_SCALE_Y(18.0f);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
		CSprite2d::DrawRect(CRect(x - SCREEN_SCALE_X(10.0f), y - SCREEN_SCALE_Y(10.0f),
			x + SCREEN_SCALE_X(300.0f), y + lh*7.2f), CRGBA(0, 0, 0, 170));
		printLine(x, y, "CHEATS", CRGBA(230, 20, 20, 255));
		y += lh*1.3f;
		char line[96];
		for(int i = 0; i < CM_NUM_ITEMS; i++){
			switch(i){
			case CM_HEALTH: strcpy(line, "Health + armour"); break;
			case CM_WANTED: strcpy(line, "Remove wanted level"); break;
			// (the game font has no < >, they show as symbols)
			case CM_WEAPONS: sprintf(line, "Weapons: %s", weaponSetNames[weaponSet]); break;
			case CM_CAR: sprintf(line, "Spawn car %d/%d: %s", carSel + 1, NUM_CARS, cars[carSel].name); break;
			case CM_MONEY: strcpy(line, "Money + $200,000"); break;
			case CM_FXTEST: sprintf(line, "Squares test: %s", fxTestNames[rw::psp::fxTest]); break;
			case CM_SHOT: strcpy(line, "Take screenshot"); break;
			case CM_CLOSE: strcpy(line, "Close"); break;
			}
			CRGBA col = i == sel ? CRGBA(255, 210, 60, 255) : CRGBA(220, 220, 220, 255);
			printLine(x + SCREEN_SCALE_X(4.0f), y, line, col);
			y += lh;
		}
		CFont::SetScale(SCREEN_SCALE_X(0.35f), SCREEN_SCALE_Y(0.7f));
		printLine(x, y + lh*0.3f, "Up/Down choose - Left/Right change - X apply - O close", CRGBA(160, 160, 160, 255));
	}
	if(rw::psp::fxTest && !menuOpen){
		// stays on screen so a video shows which test is on
		CFont::SetScale(SCREEN_SCALE_X(0.4f), SCREEN_SCALE_Y(0.8f));
		char t[64];
		sprintf(t, "TEST %s", fxTestNames[rw::psp::fxTest]);
		printLine(SCREEN_SCALE_X(150.0f), SCREEN_SCALE_Y(8.0f), t, CRGBA(255, 255, 0, 255));
	}
	if(showMsg){
		CFont::SetScale(SCREEN_SCALE_X(0.45f), SCREEN_SCALE_Y(0.9f));
		printLine(SCREEN_SCALE_X(30.0f), SCREEN_SCALE_FROM_BOTTOM(60.0f), message, CRGBA(120, 230, 120, 255));
	}
	CFont::DrawFonts();
	CFont::SetDropShadowPosition(0);
}
#endif
