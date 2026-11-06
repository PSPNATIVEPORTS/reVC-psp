#pragma once

// Performance settings for the PSP port.
// Defaults are tuned for a stable 30 fps on a PSP-2000/3000; they can be
// changed without rebuilding in psp_settings.txt in the game folder.
struct PspTune
{
	float drawDistance;	// max far clip in metres (PC is ~1000+)
	float lodScale;		// multiplies model draw distances (PC default 1.2)
	float pedDensity;	// multiplies pedestrian density
	float carDensity;	// multiplies traffic density
	int shadows;		// 0 none, 1 player + cars only, 2 all
	int particles;		// 0 reduced, 1 full
	int detailWater;	// 0 flat water only, 1 PC wavy water near camera
	int vsync;		// wait for vblank when presenting
	int showFps;		// debug line in the log every 60 frames
	int carReflections;	// environment map shine on cars
	float groundFix;	// split world triangles bigger than this (metres), 0 = off
	int audio;		// sound on/off
	int music;		// radio, cutscene and mission speech (mp3 streams)
	int sfxCacheKB;		// memory for sound effects
	float nearClip;		// camera near plane (PC 0.9), further = less depth flicker
	int roadDepthBias;	// depth units roads are pulled towards the camera
	int decalDepthBias;	// same for alpha blended geometry (markings, shadows)
	int audioRate;		// 44100 or 22050 (less CPU)
	int autoQuality;	// lower the draw distance when the game can't hold 30 fps
	float minDrawDistance;	// lowest it goes
	int fpsTarget;		// 30 or 20
	int halfRateHud;	// 1: HUD built every other frame (replayed in between)
	int fpsCounter;		// 1: "FPS : 30" at the bottom right of the screen
	int colourFilter;	// 0 off, 1 PC (flat tint), 2 VCS (PS2 brightness, half its tint), 3 PS2 (exact console filter)
	int motionBlur;		// 0 off, 1 VCS-like trails when driving fast, 2 stronger
};

extern PspTune pspTune;
extern float pspDynDrawDistance;	// draw distance in use (auto quality)
extern float pspDynLodScale;	// lod scale in use
extern float pspDynDensity;	// people/traffic multiplier in use (auto quality, 0.6 - 1)
extern float pspDynDetail;
extern int pspFpsShown;	// frames per second for the on-screen counter	// share of lod_scale in use (auto quality, 0.6 - 1)

void pspLoadTune(void);
int pspStreamingBudget(void);	// bytes for streamed models
void pspMemMark(const char *what);	// logs heap use

// Frame profiler (ms per frame in reVC_log.txt), slots:
//  0 process 1 renderlist 2 scene 3 effects 4 2d 5 menus 6 endframe 7 horizon
//  8 roads 9 world 10 water 11 fading/boats 12 shadows 13 coronas 14 particles 15 other fx
// 16 streaming 17 scripts 18 population 19 world process 20 camera/rest 21 misc updates
// 24 motion blur/colour filter 25 hud 26 radar 27 messages/other 2d 28 fonts
// 29 alpha list insertion
#define PSPPROF_SLOTS 32
extern unsigned int pspProf[PSPPROF_SLOTS];
unsigned int pspTimeUs(void);
#define PSPPROF_DECL unsigned int pspT_ = pspTimeUs()
extern volatile int pspLastLap;	// the frame part that finished last (freeze watchdog)
#define PSPPROF_LAP(n) do { unsigned int t_ = pspTimeUs(); pspProf[n] += t_ - pspT_; pspT_ = t_; pspLastLap = (n); } while(0)
