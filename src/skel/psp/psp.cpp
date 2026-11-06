// Sony PSP platform layer for reVC
// Based on the glfw skeleton: window/keyboard/mouse code removed,
// input comes from the PSP controller, rendering is librw's PSP (GE) backend.

#if defined PSP && defined RW_PSP

#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspdisplay.h>
#include <pspfpu.h>
#ifndef N3DS
#include <pspintrman.h>
#endif

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"
#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "crossplatform.h"

#include "main.h"
#include "FileMgr.h"
#include "Text.h"
#include "Pad.h"
#include "Timer.h"
#include "DMAudio.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Game.h"
#include "PCSave.h"
#include "MemoryCard.h"
#include "Sprite2d.h"
#include "AnimViewer.h"
#include "Font.h"
#include "MemoryMgr.h"

PSP_MODULE_INFO("reVC", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#ifdef PSP_LITE
PSP_MAIN_THREAD_STACK_SIZE_KB(256);	// peak use is a few KB
#else
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
#endif
// leave some memory outside the heap for thread stacks and the kernel
#ifdef PSP_LITE
PSP_HEAP_THRESHOLD_SIZE_KB(1024);
#else
PSP_HEAP_THRESHOLD_SIZE_KB(1536);
#endif

long _dwOperatingSystemVersion;

rw::EngineOpenParams openParams;

static RwBool ForegroundApp = TRUE;
static RwBool RwInitialised = FALSE;
static psGlobalType PsGlobal;

size_t _dwMemAvailPhys;
RwUInt32 gGameState;

static volatile bool exitRequested;
#ifdef N3DS
int pspLogDirect = 0;	// write every log line at once (debug_tev.txt: testing)
#endif

// Log file on the memory stick (reVC_log.txt), only while testing
#ifdef N3DS
#define LOG_FILE "log.txt"
#else
#define LOG_FILE "reVC_log.txt"
#endif
static SceUID logFd = -1;
static SceUID pspLogSemaId = -1;
static char *pspLogBufPtr;
static int *pspLogLenPtr;
static int pspLogBufSize;
static void pspLogLocked(const char *str, char *logBuf, int logSize, int &logLen);

// the log is only written while testing (an autostart.txt next to the EBOOT)
static bool
pspLogOn(void)
{
	static int on = -1;
	if(on < 0){
		struct stat st;
		on = stat("autostart.txt", &st) == 0;
	}
	return on;
}

void
pspLog(const char *str)
{
	if(!pspLogOn())
		return;
	// buffered: every write to the Memory Stick blocks the game (and the
	// model streaming, which reads from the same stick) for a while.
	// Flushed every 15 frames, when the buffer fills and on errors/exit.
	static char logBuf[16384];
	static int logLen;
	static SceUID logSema = -1;	// audio threads log too
	if(logSema < 0)
		logSema = sceKernelCreateSema("log", 0, 1, 1, nil);	// first call is from main() before any thread
	pspLogSemaId = logSema;
	pspLogBufPtr = logBuf;
	pspLogLenPtr = &logLen;
	pspLogBufSize = sizeof(logBuf);
	sceKernelWaitSema(logSema, 1, nil);
	pspLogLocked(str, logBuf, sizeof(logBuf), logLen);
#ifdef N3DS
	if(pspLogDirect)
		pspLogLocked(nil, logBuf, sizeof(logBuf), logLen);
#endif
	sceKernelSignalSema(logSema, 1);
}

static void
pspLogLocked(const char *str, char *logBuf, int logSize, int &logLen)
{
	if(str == nil){
		if(logLen > 0){
			if(logFd < 0)
				logFd = sceIoOpen(LOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
			if(logFd >= 0)
				sceIoWrite(logFd, logBuf, logLen);
			logLen = 0;
		}
		return;
	}
	int n = strlen(str);
	if(logLen + n > logSize)
		pspLogLocked(nil, logBuf, logSize, logLen);
	if(n > logSize){
		if(logFd < 0)
			logFd = sceIoOpen(LOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if(logFd >= 0)
			sceIoWrite(logFd, str, n);
		return;
	}
	memcpy(logBuf + logLen, str, n);
	logLen += n;
}

void
pspLogFlush(void)
{
	pspLog(nil);
}

#ifdef N3DS
// writes what the game logged every half second, from its own low priority
// thread (the game thread only copies the lines into the buffer)
static int
logWriterMain(SceSize, void*)
{
	static char out[16384];
	for(;;){
		sceKernelDelayThread(500000);
		if(pspLogSemaId < 0 || pspLogBufPtr == nil)
			continue;
		sceKernelWaitSema(pspLogSemaId, 1, nil);
		int n = *pspLogLenPtr;
		memcpy(out, pspLogBufPtr, n);
		*pspLogLenPtr = 0;
		if(n > 0 && logFd < 0)
			logFd = sceIoOpen(LOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		int fd = logFd;
		sceKernelSignalSema(pspLogSemaId, 1);
		// (outside the lock: the game keeps logging into the buffer meanwhile;
		// only a full buffer writes from the game thread)
		if(n > 0 && fd >= 0)
			sceIoWrite(fd, out, n);
	}
	return 0;
}

void
n3dsLogWriterStart(void)
{
	static bool started;
	if(started || !pspLogOn())
		return;
	started = true;
	SceUID th = sceKernelCreateThread("log_writer", logWriterMain, 0x2F, 0x4000, PSP_THREAD_ATTR_USER, nil);
	if(th >= 0)
		sceKernelStartThread(th, 0, nil);
}
#endif

// for the freeze watchdog: the main thread may be stuck holding the log's
// lock (or the allocator's), so never wait for it for long
static void
pspLogTryFlush(void)
{
	if(pspLogSemaId < 0 || pspLogBufPtr == nil)
		return;
	SceUInt timeout = 200000;
	if(sceKernelWaitSema(pspLogSemaId, 1, &timeout) < 0)
		return;
	pspLogLocked(nil, pspLogBufPtr, pspLogBufSize, *pspLogLenPtr);
	sceKernelSignalSema(pspLogSemaId, 1);
}
static void
pspLogWriteNow(const char *str)
{
	if(!pspLogOn())
		return;
	if(logFd < 0)
		logFd = sceIoOpen(LOG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(logFd >= 0)
		sceIoWrite(logFd, str, strlen(str));
}

static int pspHeapLimitKB;
static int pspVectorUnit = 1;
static int autoLoadSlot;
int pspRadioStep;	// d-pad radio: +1 next, -1 previous station (read by the music manager)
int pspAudioLog;	// testing aid: autostart.txt "audiolog"
static int pspTestHoldR;
static int pspTestHoldRight;
static int pspTestHoldX;
int pspTestPlayerDrives;
static int pspTestOrbit;
static int pspTestResume;
static int pspTestCpuMhz;	// testing aid: autostart.txt "load N"	// testing aid: vector_unit=0 in psp_settings.txt
// Performance settings, see psptune.h
#include "psptune.h"
#ifdef N3DS
#define SETTINGS_FILE "nintendo_settings.txt"
#else
#define SETTINGS_FILE "psp_settings.txt"
#endif
#if defined(N3DS)
// 3DS: the user's own settings (his PSP build's), chosen as the defaults
static const PspTune pspTuneDefaults = {
	800.0f,	// drawDistance (the furthest; auto quality decides)
	0.6f,	// lodScale
	1.0f,	// pedDensity
	0.9f,	// carDensity
	0,	// shadows
	0,	// particles
	0,	// detailWater
	1,	// vsync
	1,	// showFps
	0,	// carReflections
	0.0f,	// groundFix
	1,	// audio
	1,	// music (decoded on the second CPU core)
	3072,	// sfxCacheKB
	1.25f,	// nearClip
	6,	// roadDepthBias
	3,	// decalDepthBias
	16000,	// audioRate
	1,	// autoQuality
	400.0f,	// minDrawDistance
	30,	// fpsTarget
	0,	// halfRateHud: a PSP GE feature
	1,	// fpsCounter
	1,	// colourFilter
	0,	// motionBlur: off on the 3DS (its copy of the picture came out shifted)
};
#elif defined(PSP_LITE)
// PSP-1000 (32 MB): very short draw distance, few people and cars, no music
static const PspTune pspTuneDefaults = {
	120.0f,	// drawDistance
	0.45f,	// lodScale
	0.25f,	// pedDensity
	0.25f,	// carDensity
	0,	// shadows
	0,	// particles
	0,	// detailWater
	1,	// vsync
	1,	// showFps
	0,	// carReflections
	0.0f,	// groundFix: the split polygons cost memory the PSP-1000 doesn't have
	1,	// audio
	0,	// music
	512,	// sfxCacheKB
	1.25f,	// nearClip
	6,	// roadDepthBias
	3,	// decalDepthBias
	16000,	// audioRate
	1,	// autoQuality
	90.0f,	// minDrawDistance
	30,	// fpsTarget
	1,	// halfRateHud
	1,	// fpsCounter
	2,	// colourFilter
	1,	// motionBlur
};
#else
static const PspTune pspTuneDefaults = {
	450.0f,	// drawDistance (the furthest; auto quality decides)
	0.6f,	// lodScale
	1.0f,	// pedDensity (a busy city like VCS: auto quality takes detail
	0.9f,	// carDensity  and distance away before people and cars)
	1,	// shadows
	0,	// particles
	0,	// detailWater
	1,	// vsync
	1,	// showFps
	0,	// carReflections
	0.0f,	// groundFix: not needed, the CPU clipping fixes the holes
	1,	// audio
	1,	// music
	3072,	// sfxCacheKB
	1.25f,	// nearClip
	6,	// roadDepthBias
	3,	// decalDepthBias
	16000,	// audioRate
	1,	// autoQuality
	400.0f,	// minDrawDistance (auto quality never pulls the far distance in closer)
	30,	// fpsTarget
	1,	// halfRateHud
	1,	// fpsCounter
	2,	// colourFilter
	1,	// motionBlur
};
#endif
PspTune pspTune = pspTuneDefaults;
float gPspDefaultNear = 1.25f;
float pspDynDrawDistance = 200.0f;
float pspDynLodScale = 0.6f;
float pspDynDensity = 1.0f;
float pspPedsWanted;	// what the population wants around the player (log)
float pspDynDetail = 1.0f;
int pspFpsShown;

// streamed models get what's left after audio
static int pspHeapTotalKB;
int
pspStreamingBudget(void)
{
#ifdef PSP_LITE
	// what's left after the fixed game data (about 7.2 MB in game), the free
	// memory the memory guard keeps, its reserve and the sound effects cache
	int budget = (pspHeapTotalKB - 7200 - 1536 - 512) * 1024;
	if(pspTune.audio)
		budget -= pspTune.sfxCacheKB * 1024;
#else
	int budget = PSP_STREAMING_MEMORY;
	if(pspTune.audio)
		budget -= (pspTune.sfxCacheKB + 1536) * 1024;
	// the split ground polygons live outside the streaming budget
	if(pspTune.groundFix > 0.0f)
		budget -= 2*1024*1024;
#endif
#ifdef PSP_LITE
	if(budget < 4*1024*1024)
		budget = 4*1024*1024;
#else
	if(budget < 12*1024*1024)
		budget = 12*1024*1024;
#endif
	return budget;
}

static const char *tuneHelp =
#ifdef N3DS
	"# GTA Vice City 3DS performance and graphics settings. Delete this file to get the defaults back.\n"
#else
	"# GTA Vice City PSP performance settings. Delete this file to get the defaults back.\n"
#endif
	"# Lower values = faster. Restart the game after editing.\n"
	"# draw_distance: the furthest the world is drawn, in metres (100 - 800). With\n"
	"#   auto_quality=1 the game goes out to it whenever the frame time allows (25%\n"
	"#   further in vehicles) and comes back in when it doesn't\n"
	"# lod_scale: model detail distance (0.4 - 1.2)\n"
	"# ped_density / car_density: amount of people / traffic (0.0 - 2.0, 1.0 = like the PC game)\n"
	"# shadows: 0 off, 1 player and cars, 2 all\n"
	"# particles: 0 reduced, 1 full\n"
	"# detail_water: 0 simple water, 1 wavy water near the camera\n"
	"# show_fps: 1 writes fps and timings to the log (while testing)\n"
	"# car_reflections: 1 shiny environment reflections on cars, 0 off\n"
	"# ground_fix: 0 (best). The holes in the ground are fixed by clipping on the CPU;\n"
	"#   a size in metres also splits big ground polygons (costs memory, not needed)\n"
	"# audio: 1 sound on, 0 off (needs the AUDIO folder of the game)\n"
	"# music: 1 radio, cutscene and mission speech, 0 sound effects only\n"
	"# sfx_cache_kb: memory for sound effects (1024 - 8192)\n"
	"# near_clip: camera near plane in metres (0.9 - 2.0), higher = less flickering\n"
	"#   of distant roads and buildings, but walls close to the camera get cut\n"
	"# road_bias / decal_bias: pull roads and ground decals (markings, shadows) in front of\n"
	"#   the ground below them so they don't flicker in the distance (0 = off, 0 - 32)\n"
	"# audio_rate: sound output rate: 11025, 16000 (default, like LCS/VCS), 22050 or 44100\n"
	"#   lower = less CPU for the sound mixer\n"
	"# auto_quality: 1 = measures how long each frame takes to build and keeps 30 fps:\n"
	"#   too slow = less model detail first, then a shorter distance (never under\n"
	"#   min_draw_distance); people and cars stay; time to spare = the other way round; 0 = fixed\n"
	"# fps_target: 30 or 20 (steady 20 with more draw distance when 30 can't be held)\n"
	"# min_draw_distance: the shortest far distance auto_quality goes down to\n"
	"# fast_hud: 1 = the HUD is redrawn every other frame (saves 2-3 ms per frame), 0 = every frame\n"
	"# colour_filter: the time-of-day colour grading: 2 = VCS-like (bright, warm days,\n"
	"#   default), 3 = exactly the PS2 version's filter (stronger tint), 1 = PC, 0 = off\n"
	"# motion_blur: 1 = trails when driving fast, like VCS (default), 2 = stronger, 0 = off\n";

unsigned int
pspTimeUs(void)
{
	return sceKernelGetSystemTimeLow();
}

void
pspLoadTune(void)
{
	const char *readName = SETTINGS_FILE;
	bool oldName = false;
#ifdef N3DS
	{
		// the old name: its settings go into nintendo_settings.txt once
		struct stat st;
		if(stat(SETTINGS_FILE, &st) != 0 && stat("psp_settings.txt", &st) == 0){
			readName = "psp_settings.txt";
			oldName = true;
		}
	}
#endif
	FILE *f = fopen(readName, "r");
	int found = 0;	// number of known keys in the file
	int version = 0;
	if(f){
		char line[128], key[64];
		float val;
		while(fgets(line, sizeof(line), f)){
			if(line[0] == '#' || sscanf(line, " %63[^= ] = %f", key, &val) != 2)
				continue;
			found++;
			if(strcmp(key, "draw_distance") == 0) pspTune.drawDistance = Clamp(val, 50.0f, 2000.0f);
			else if(strcmp(key, "lod_scale") == 0) pspTune.lodScale = Clamp(val, 0.2f, 2.0f);
			else if(strcmp(key, "ped_density") == 0) pspTune.pedDensity = Clamp(val, 0.0f, 2.0f);
			else if(strcmp(key, "car_density") == 0) pspTune.carDensity = Clamp(val, 0.0f, 2.0f);
			else if(strcmp(key, "shadows") == 0) pspTune.shadows = (int)val;
			else if(strcmp(key, "particles") == 0) pspTune.particles = (int)val;
			else if(strcmp(key, "detail_water") == 0) pspTune.detailWater = (int)val;
			else if(strcmp(key, "show_fps") == 0) pspTune.showFps = (int)val;
			else if(strcmp(key, "heap_limit_kb") == 0){ pspHeapLimitKB = (int)val; found--; }	// testing aid
			else if(strcmp(key, "vector_unit") == 0){ pspVectorUnit = (int)val; found--; }	// testing aid
			else if(strcmp(key, "cpu_mhz") == 0){ pspTestCpuMhz = (int)val; found--; }	// testing aid (slower PSP)
#ifndef N3DS
			else if(strcmp(key, "cpu_clip") == 0){ rw::psp::clipEnabled = (int)val; found--; }	// testing aid
			else if(strcmp(key, "bake") == 0){ rw::psp::bakeEnabled = (int)val; found--; }	// testing aid: recorded building commands
#endif
			else if(strcmp(key, "car_reflections") == 0) pspTune.carReflections = (int)val;
			else if(strcmp(key, "audio") == 0) pspTune.audio = (int)val;
			else if(strcmp(key, "near_clip") == 0) pspTune.nearClip = Clamp(val, 0.5f, 3.0f);
			else if(strcmp(key, "music") == 0) pspTune.music = (int)val;
			else if(strcmp(key, "sfx_cache_kb") == 0) pspTune.sfxCacheKB = Clamp((int)val, 512, 16384);
			else if(strcmp(key, "ground_fix") == 0) pspTune.groundFix = val <= 0.0f ? 0.0f : Clamp(val, 1.0f, 50.0f);
			else if(strcmp(key, "road_bias") == 0) pspTune.roadDepthBias = Clamp((int)val, 0, 32);
			else if(strcmp(key, "decal_bias") == 0) pspTune.decalDepthBias = Clamp((int)val, 0, 32);
			else if(strcmp(key, "audio_rate") == 0){
				int r = (int)val;
				pspTune.audioRate = r >= 44100 ? 44100 : r >= 22050 ? 22050 : r >= 16000 ? 16000 : 11025;
			}
			else if(strcmp(key, "auto_quality") == 0) pspTune.autoQuality = (int)val;
			else if(strcmp(key, "fps_target") == 0) pspTune.fpsTarget = (int)val == 20 ? 20 : 30;
			else if(strcmp(key, "min_draw_distance") == 0) pspTune.minDrawDistance = Clamp(val, 50.0f, 2000.0f);
			else if(strcmp(key, "settings_version") == 0) version = (int)val;
			else if(strcmp(key, "fast_hud") == 0) pspTune.halfRateHud = (int)val;
			else if(strcmp(key, "fps_counter") == 0){ found--; }	// (gone: the counter was removed)
			else if(strcmp(key, "colour_filter") == 0) pspTune.colourFilter = Clamp((int)val, 0, 3);
			else if(strcmp(key, "motion_blur") == 0) pspTune.motionBlur = Clamp((int)val, 0, 2);
			else found--;
		}
		fclose(f);
	}
	// version 2: min_draw_distance is now the lowest far distance (the low
	// detail skyline) instead of the detail distance, the old value would
	// bring back the blank world in the distance
	if(f && version < 2){
		pspTune.minDrawDistance = pspTuneDefaults.minDrawDistance;
	}
	// version 3: 16000 Hz sound by default (less CPU, LCS/VCS-like)
	if(f && version < 3 && pspTune.audioRate == 22050)
		pspTune.audioRate = pspTuneDefaults.audioRate;
	// version 4: draw_distance is the furthest auto quality may go (it now
	// measures the real frame time and has room to spare at 200 m): old
	// files get the new, longer distances and at least the default detail
	if(f && version < 4){
		pspTune.drawDistance = Max(pspTune.drawDistance, pspTuneDefaults.drawDistance);
		pspTune.minDrawDistance = Max(pspTune.minDrawDistance, pspTuneDefaults.minDrawDistance);
		pspTune.lodScale = Max(pspTune.lodScale, pspTuneDefaults.lodScale);
		pspTune.autoQuality = 1;
	}
	// version 7: the holes in the ground are fixed by clipping on the CPU, the
	// split ground (which cost a lot of memory) isn't needed any more; a busy
	// city: more people and cars, auto quality lowers detail and distance first
	if(f && version < 7){
		pspTune.groundFix = 0.0f;
		pspTune.pedDensity = Max(pspTune.pedDensity, pspTuneDefaults.pedDensity);
		pspTune.carDensity = Max(pspTune.carDensity, pspTuneDefaults.carDensity);
		pspTune.minDrawDistance = Min(pspTune.minDrawDistance, pspTuneDefaults.minDrawDistance);
	}
#if !defined(PSP_LITE) && !defined(N3DS)
	// version 8: faster drawing (recorded building commands): auto quality
	// keeps the far distance at 400 m or more and never thins out people and cars
	if(f && version < 8)
		pspTune.minDrawDistance = Max(pspTune.minDrawDistance, pspTuneDefaults.minDrawDistance);
#endif
#ifdef N3DS
	// 3DS version 9: the PSP build's graphics (it had short distances, few
	// people and cars, no shadows, colour filter or motion blur before)
	if(f && version < 9){
		pspTune.drawDistance = pspTuneDefaults.drawDistance;
		pspTune.minDrawDistance = pspTuneDefaults.minDrawDistance;
		pspTune.lodScale = pspTuneDefaults.lodScale;
		pspTune.pedDensity = pspTuneDefaults.pedDensity;
		pspTune.carDensity = pspTuneDefaults.carDensity;
		pspTune.shadows = pspTuneDefaults.shadows;
		pspTune.colourFilter = pspTuneDefaults.colourFilter;
		pspTune.motionBlur = pspTuneDefaults.motionBlur;
		pspTune.autoQuality = 1;
	}
	// version 10: motion blur off (shifted double picture and a crash on the
	// real 3DS), 300 m at most
	if(f && version < 10){
		pspTune.motionBlur = 0;
		pspTune.drawDistance = Min(pspTune.drawDistance, pspTuneDefaults.drawDistance);
	}
	// version 11: less traffic and people, shorter distance and detail (the
	// real Old 3DS ran 24-25 fps in traffic with version 9's values)
	if(f && version < 11){
		pspTune.drawDistance = Min(pspTune.drawDistance, pspTuneDefaults.drawDistance);
		pspTune.minDrawDistance = Min(pspTune.minDrawDistance, pspTuneDefaults.minDrawDistance);
		pspTune.lodScale = Min(pspTune.lodScale, pspTuneDefaults.lodScale);
		pspTune.pedDensity = Min(pspTune.pedDensity, pspTuneDefaults.pedDensity);
		pspTune.carDensity = Min(pspTune.carDensity, pspTuneDefaults.carDensity);
	}
#undef SETTINGS_VERSION
#define SETTINGS_VERSION 11
#else
#define SETTINGS_VERSION 8
#endif
	bool limited = false;
#ifdef PSP_LITE
	// The PSP-1000 has 14 MB for the game: a settings file from the PSP-2000/3000
	// version (longer distance, more people, ground fix, music) runs it out of
	// memory in the first cutscene. Keep everything within what fits.
	#define LITE_MAX(field, max) if(pspTune.field > (max)){ pspTune.field = (max); limited = true; }
	LITE_MAX(drawDistance, 150.0f);
	LITE_MAX(minDrawDistance, 100.0f);
	LITE_MAX(lodScale, 0.5f);
	LITE_MAX(pedDensity, 0.35f);
	LITE_MAX(carDensity, 0.35f);
	LITE_MAX(groundFix, 0.0f);
	LITE_MAX(sfxCacheKB, 768);
	LITE_MAX(music, 0);
	LITE_MAX(shadows, 1);
	LITE_MAX(particles, 0);
	LITE_MAX(detailWater, 0);
	#undef LITE_MAX
	if(limited)
		pspLog("settings: limited to what fits in the PSP-1000's memory\n");
#endif
	// new file, or settings added by a newer version missing: (re)write it
	// with the current values so every setting is listed
	if(found < 24 || version < SETTINGS_VERSION || limited || oldName){	// (limited: the file shows what's used)
		f = fopen(SETTINGS_FILE, "w");
		if(f){
			fputs(tuneHelp, f);
			fprintf(f, "draw_distance=%d\nlod_scale=%.2f\nped_density=%.2f\ncar_density=%.2f\n"
				"shadows=%d\nparticles=%d\ndetail_water=%d\nshow_fps=%d\ncar_reflections=%d\nground_fix=%.1f\n"
				"audio=%d\nmusic=%d\nsfx_cache_kb=%d\nnear_clip=%.2f\nroad_bias=%d\ndecal_bias=%d\naudio_rate=%d\n"
				"auto_quality=%d\nmin_draw_distance=%d\nfps_target=%d\nfast_hud=%d\ncolour_filter=%d\nmotion_blur=%d\nsettings_version=%d\n",
				(int)pspTune.drawDistance, pspTune.lodScale, pspTune.pedDensity, pspTune.carDensity,
				pspTune.shadows, pspTune.particles, pspTune.detailWater, pspTune.showFps,
				pspTune.carReflections, pspTune.groundFix,
				pspTune.audio, pspTune.music, pspTune.sfxCacheKB, pspTune.nearClip,
				pspTune.roadDepthBias, pspTune.decalDepthBias, pspTune.audioRate,
				pspTune.autoQuality, (int)pspTune.minDrawDistance, pspTune.fpsTarget, pspTune.halfRateHud,
				pspTune.colourFilter, pspTune.motionBlur, SETTINGS_VERSION);
			if(pspHeapLimitKB)
				fprintf(f, "heap_limit_kb=%d\n", pspHeapLimitKB);
			if(!pspVectorUnit)
				fprintf(f, "vector_unit=0\n");
			fclose(f);
		}
	}
	char b[256];
	snprintf(b, sizeof(b), "settings: draw_distance %d lod_scale %.2f peds %.2f cars %.2f shadows %d particles %d water %d reflections %d ground_fix %.1f audio %d music %d near %.2f bias %d/%d\n",
		(int)pspTune.drawDistance, pspTune.lodScale, pspTune.pedDensity, pspTune.carDensity,
		pspTune.shadows, pspTune.particles, pspTune.detailWater, pspTune.carReflections, pspTune.groundFix,
		pspTune.audio, pspTune.music, pspTune.nearClip, pspTune.roadDepthBias, pspTune.decalDepthBias);
	pspLog(b);
	extern int pspApplyHeapLimit(int);
	pspApplyHeapLimit(pspHeapLimitKB);
	gPspDefaultNear = pspTune.nearClip;
	rw::psp::envMapEnabled = pspTune.carReflections;
	if(pspVectorUnit){
		int bad = rw::matrixVfpuSelfTest();
		char b2[96];
		snprintf(b2, sizeof(b2), "matrix multiply: %s\n", bad == 0 ? "vector unit" : "FPU (vector unit test failed)");
		pspLog(b2);
	}else
		pspLog("matrix multiply: FPU (vector_unit=0)\n");
	rw::psp::tessMaxEdge = pspTune.groundFix;
	rw::psp::decalDepthBias = pspTune.decalDepthBias;
	if(pspTestCpuMhz >= 20 && pspTestCpuMhz < 333){
		scePowerSetClockFrequency(pspTestCpuMhz, pspTestCpuMhz, pspTestCpuMhz/2);
		char b3[64];
		snprintf(b3, sizeof(b3), "testing: CPU at %d MHz\n", pspTestCpuMhz);
		pspLog(b3);
	}
	if(pspTune.minDrawDistance > pspTune.drawDistance)
		pspTune.minDrawDistance = pspTune.drawDistance;
	// start half way, auto quality takes it from there
	pspDynDrawDistance = pspTune.autoQuality ? (pspTune.drawDistance + pspTune.minDrawDistance)*0.5f : pspTune.drawDistance;
	pspDynLodScale = pspTune.lodScale;
}

// Stack watermark: how much of the main thread stack was ever used
static uint8 *stackLow, *stackTop;
static void
initStackWatermark(void)
{
	SceKernelThreadInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if(sceKernelReferThreadStatus(sceKernelGetThreadId(), &info) < 0)
		return;
	uint8 *sp;
#ifdef N3DS
	asm volatile("mov %0, sp" : "=r"(sp));
#else
	asm volatile("move %0, $sp" : "=r"(sp));
#endif
	stackLow = (uint8*)info.stack + 256;
	stackTop = (uint8*)info.stack + info.stackSize;
	uint8 *end = sp - 1024;
	char b[160];
	snprintf(b, sizeof(b), "stack info: base %p size %d sp %p\n", info.stack, info.stackSize, sp);
	pspLog(b);
	if(end > stackLow)
		memset(stackLow, 0xCD, end - stackLow);
}
int
pspStackUsedKB(void)
{
	if(stackLow == nil)
		return -1;
	// scanning 512 KB costs about 2% CPU: only every 30th time (30 s)
	static int calls, last = -1;
	if(last >= 0 && (++calls % 30) != 0)
		return last;
	const uint32 *p = (const uint32*)(((uintptr)stackLow + 3) & ~3);
	while((const uint8*)p < stackTop && *p == 0xCDCDCDCD)
		p++;
	last = (stackTop - (const uint8*)p)/1024;
	return last;
}

/*
 *****************************************************************************
 */
static int
exitCallback(int arg1, int arg2, void *common)
{
	exitRequested = true;
	RsGlobal.quit = TRUE;
	pspLogFlush();
	sceKernelExitGame();
	return 0;
}

static int
callbackThread(SceSize args, void *argp)
{
	pspFpuSetEnable(0);
	int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, nil);
	sceKernelRegisterExitCallback(cbid);
	sceKernelSleepThreadCB();
	return 0;
}

static void
setupCallbacks(void)
{
	int thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
	if(thid >= 0)
		sceKernelStartThread(thid, 0, 0);
}

/*
 *****************************************************************************
 */
void _psCreateFolder(const char *path)
{
	struct stat info;
	if(stat(path, &info) != 0)
		mkdir(path, 0777);
}

const char *_psGetUserFilesFolder()
{
	static char szUserFiles[256];
	strcpy(szUserFiles, "userfiles");
	_psCreateFolder(szUserFiles);
	return szUserFiles;
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	if(!RwCameraBeginUpdate(Scene.camera)){
		ForegroundApp = FALSE;
		RsEventHandler(rsACTIVATE, (void *)FALSE);
		return FALSE;
	}
	return TRUE;
}

#include <malloc.h>
#include "Streaming.h"
#include "WeaponInfo.h"
#include "Wanted.h"
#include "CutsceneMgr.h"
#include "World.h"
#include "Camera.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "Clock.h"
#include "CutsceneObject.h"
#include "RpAnimBlend.h"
#include "AnimBlendClumpData.h"
#include "Script.h"
#include "Remote.h"
#include "Radar.h"
#include "Weather.h"
#include "Pools.h"
#include "Renderer.h"
#include "ModelInfo.h"
#include "GenericGameStorage.h"
#include "Population.h"
#include "Skidmarks.h"
#include "Particle.h"
#include "CarCtrl.h"
#include "Hud.h"
#include "Restart.h"
#include "Stats.h"

//
// Memory guard. All mallocs go through these wrappers (linker --wrap).
// A 1 MB emergency reserve is released when an allocation fails, and at the
// end of every frame the game unloads least used models when free memory gets
// low, so big one-off loads (cutscenes) don't run the heap dry.
//
extern "C" {
void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
void *__real_memalign(size_t align, size_t size);
}

static void *emergencyReserve;
static volatile int memFailures;
static int batches;	// model batches unloaded for failed allocations this frame
#ifdef PSP_LITE
#define RESERVE_SIZE (512*1024)
#define MIN_FREE_KB (1536)

#else
#define RESERVE_SIZE (1024*1024)
#define MIN_FREE_KB (3*1024)
#endif

static void
memLog(const char *what, size_t size)
{
	char b[96];
	snprintf(b, sizeof(b), "%s: allocation of %u bytes failed\n", what, (unsigned)size);
	pspLog(b);
	pspLogFlush();
}

// called when an allocation failed; returns whether it's worth retrying
static SceUID pspMainThread = -1;
static bool
memFailed(size_t size, int attempt)
{
#ifndef N3DS
	// another thread: the main thread may just be holding a big block for a
	// moment (pspLargestFreeKB), wait a little before touching the reserve
	if(pspMainThread >= 0 && sceKernelGetThreadId() != pspMainThread && attempt < 4){
		sceKernelDelayThread(2000);
		return true;
	}
#endif
#ifndef N3DS
	// memory waiting for the GE to finish a frame: get it back first
	if(pspMainThread >= 0 && sceKernelGetThreadId() == pspMainThread && rw::psp::geFlushFreesNow())
		return true;
#endif
	memFailures++;
	// Free memory counts every little gap, so "6 MB free" can still fail a
	// 120 KB block (the yacht cutscene in The Party crashed that way). Unload
	// unused models, a batch at a time, and try again: on the main thread
	// only, the streaming lists belong to it.
	static bool inRelease;
	if(pspMainThread >= 0 && sceKernelGetThreadId() == pspMainThread && !inRelease && batches < 12){
		inRelease = true;
		int n = 0;
		while(n < 24 && CStreaming::RemoveLeastUsedModel(0))
			n++;
		inRelease = false;
		if(n > 0){
			batches++;
			char b[96];
			snprintf(b, sizeof(b), "memory: %u bytes failed, unloaded %d models, trying again\n", (unsigned)size, n);
			pspLog(b);
			return true;
		}
	}
	batches = 0;
	if(emergencyReserve){
		free(emergencyReserve);
		emergencyReserve = nil;
		memLog("LOW MEMORY, using reserve", size);
		return true;
	}
	memLog("OUT OF MEMORY", size);
	return false;
}

volatile int pspAllocDepth;	// > 0: some thread is inside the allocator (freeze watchdog)
extern "C" void*
__wrap_malloc(size_t size)
{
	pspAllocDepth++;
	void *p = __real_malloc(size);
	pspAllocDepth--;
	for(int k = 0; p == nil && size && k < 16 && memFailed(size, k); k++)
		p = __real_malloc(size);
	return p;
}

extern "C" void*
__wrap_calloc(size_t n, size_t size)
{
	void *p = __real_calloc(n, size);
	for(int k = 0; p == nil && n*size && k < 16 && memFailed(n*size, k); k++)
		p = __real_calloc(n, size);
	return p;
}

extern "C" void*
__wrap_realloc(void *ptr, size_t size)
{
	void *p = __real_realloc(ptr, size);
	for(int k = 0; p == nil && size && k < 16 && memFailed(size, k); k++)
		p = __real_realloc(ptr, size);
	return p;
}

extern "C" void*
__wrap_memalign(size_t align, size_t size)
{
	void *p = __real_memalign(align, size);
	for(int k = 0; p == nil && size && k < 16 && memFailed(size, k); k++)
		p = __real_memalign(align, size);
	return p;
}

int
pspApplyHeapLimit(int kb)
{
	if(kb > 0 && (pspHeapTotalKB == 0 || kb < pspHeapTotalKB))
		pspHeapTotalKB = kb;
	return pspHeapTotalKB;
}

void
pspMemMark(const char *what)
{
	static int last;
	struct mallinfo mi = mallinfo();
	int kb = (int)(mi.uordblks/1024);
	char b[128];
	snprintf(b, sizeof(b), "memmark %-20s heap %5d KB (+%d), streaming %d KB\n", what, kb, kb - last,
		(int)(CStreaming::ms_memoryUsed/1024));
	pspLog(b);
	last = kb;
}

int
pspFreeHeapKB(void)
{
	struct mallinfo mi = mallinfo();
	return pspHeapTotalKB - (int)(mi.uordblks/1024);
}


// Freeze watchdog: when no frame has been finished for 3 seconds (the sound
// keeps playing, the picture stands still), say in the log where the game
// is stuck and write the log out at once; a GE that has been busy for
// seconds is cancelled so the game can go on.
volatile int pspLastLap = -1;
volatile uint32 pspFramesDone;
#ifndef N3DS
static int
watchdogMain(SceSize, void*)
{
	uint32 lastFrames = 0, stuckSince = 0, lastReport = 0;
	for(;;){
		sceKernelDelayThread(500000);
		uint32 now = sceKernelGetSystemTimeLow();
		if(pspFramesDone != lastFrames || gGameState < GS_INIT_FRONTEND){
			lastFrames = pspFramesDone;
			stuckSince = now;
			lastReport = 0;
			continue;
		}
		uint32 stuck = now - stuckSince;
		if(stuck < 3000000 || (lastReport && now - lastReport < 10000000))
			continue;
		lastReport = now;
		SceKernelThreadInfo ti;
		memset(&ti, 0, sizeof(ti));
		ti.size = sizeof(ti);
		sceKernelReferThreadStatus(pspMainThread, &ti);
		uint32 ge = rw::psp::geSyncStart;
		// (nothing here may wait for a lock the main thread could hold:
		// no malloc/mallinfo, no log lock for more than a moment)
		pspLogTryFlush();
		char b[300];
		snprintf(b, sizeof(b), "FREEZE: no frame for %u s (game state %d), last part %d, main thread status %x wait type %x id %x, "
			"waiting for the GE %u ms, inside the allocator %d, models requested %d, allocation failures %d\n",
			(unsigned)(stuck/1000000), (int)gGameState, pspLastLap, ti.status, ti.waitType, (unsigned)ti.waitId,
			ge ? (unsigned)((now - ge)/1000) : 0, (int)pspAllocDepth, (int)CStreaming::ms_numModelsRequested, (int)memFailures);
		pspLogWriteNow(b);
		if(ge && now - ge > 4000000){
			pspLogWriteNow("FREEZE: the GE hangs, cancelling its display list\n");
			rw::psp::geRecover();
		}
	}
	return 0;
}
#endif

volatile const char *pspStep = "";
#ifdef N3DS
namespace rw { namespace psp { uint32 c3dRasterChecksum(Raster *raster); int32 c3dCanaryCheck(void); } }
extern unsigned int n3dsMp3Frames;
#endif	// the last marked step (menu, loading a save)
volatile int pspRemovingModel = -1;	// the model being removed (3DS freeze report)
#ifdef N3DS
// Called by the 3DS watchdog thread (n3ds.cpp) when no frame was finished
// for a while: what the game was doing, written straight to the log. Like
// the PSP's, it waits for no lock the stuck thread could hold.
void
n3dsFreezeReport(unsigned stuckSec)
{
	pspLogTryFlush();
	char b[400];
	snprintf(b, sizeof(b), "FREEZE: no frame for %u s, step '%s', last part %d, game state %d, menu %d page %d, "
		"start up %d shut down %d, want to load %d restart %d, inside the allocator %d, models requested %d, removing model %d\n",
		stuckSec, (const char*)pspStep, pspLastLap, (int)gGameState,
		(int)FrontEndMenuManager.m_bMenuActive, (int)FrontEndMenuManager.m_nCurrScreen,
		(int)FrontEndMenuManager.m_bStartUpFrontEndRequested, (int)FrontEndMenuManager.m_bShutDownFrontEndRequested,
		(int)FrontEndMenuManager.m_bWantToLoad, (int)FrontEndMenuManager.m_bWantToRestart,
		(int)pspAllocDepth, (int)CStreaming::ms_numModelsRequested, (int)pspRemovingModel);
	pspLogWriteNow(b);
}
// the 3DS crash handler (n3ds.cpp): the buffered lines, then its report
void
n3dsCrashReport(const char *what)
{
	pspLogTryFlush();
	char b[160];
	snprintf(b, sizeof(b), "CRASH at step '%s', last part %d, game state %d\n", (const char*)pspStep, pspLastLap, (int)gGameState);
	pspLogWriteNow(b);
	pspLogWriteNow(what);
}
#endif

// The crash catcher (crashprx/): a kernel module that writes the registers
// and the stack to reVC_crash.txt when the game crashes (the PSP just froze
// before, nothing in the log said where). Written next to EBOOT.PBP and
// loaded at start; on a PSP that doesn't allow it the game runs without.
#ifndef N3DS
#include "revccrash_prx.h"
// where a crashed thread is sent (it can't go back to what crashed); the
// crash catcher's own thread writes the report meanwhile
static void __attribute__((noinline))
pspCrashPark(void)
{
	for(;;)
		__asm__ volatile("nop");
}

static void
startCrashCatcher(void)
{
	char cwd[160], path[200], crash[200];
	if(getcwd(cwd, sizeof(cwd)) == nil)
		return;
	snprintf(path, sizeof(path), "%s/revccrash.prx", cwd);
	snprintf(crash, sizeof(crash), "%s/reVC_crash.txt", cwd);
	{
		// (written every time: an older version of it may be there)
		SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if(fd >= 0){
			sceIoWrite(fd, pspCrashPrx, sizeof(pspCrashPrx));
			sceIoClose(fd);
		}
	}
	SceUID mod = sceKernelLoadModule(path, 0, nil);
	int r = mod, status = -1;
	if(mod >= 0){
		// the report's path, then the parking loop's address
		char args[208];
		int len = strlen(crash) + 1;
		memcpy(args, crash, len);
		uint32 park = (uint32)(uintptr)pspCrashPark;
		memcpy(args + len, &park, 4);
		r = sceKernelStartModule(mod, len + 4, args, &status, nil);
	}
	char b[256];
	snprintf(b, sizeof(b), "crash catcher: %s (module %x, handler %x)\n", r >= 0 && status >= 0 ? "on" : "not working on this PSP", (unsigned)r, (unsigned)status);
	pspLog(b);
}
#endif

static void
startWatchdog(void)
{
#ifndef N3DS
	SceUID th = sceKernelCreateThread("watchdog", watchdogMain, 0x12, 0x2000, PSP_THREAD_ATTR_USER, NULL);
	if(th >= 0)
		sceKernelStartThread(th, 0, NULL);
#else
	extern void n3dsWatchdogStart(void);
	if(pspLogOn())	// testing only, like the log
		n3dsWatchdogStart();
#endif
}

// unload least used models until at least minKB are free
void
pspEnsureFreeMemory(int minKB)
{
	if(pspHeapTotalKB == 0)
		return;
	int n = 0;
	while(pspFreeHeapKB() < minKB && n++ < 400)
		if(!CStreaming::RemoveLeastUsedModel(0))
			break;
	if(n > 0){
		char b[96];
		snprintf(b, sizeof(b), "memory: unloaded %d models, %d KB free\n", n, pspFreeHeapKB());
		pspLog(b);
	}
}

// size of the biggest block malloc can hand out now (up to capKB): "free
// memory" counts every little gap between models, a cutscene needs big blocks
int
pspLargestFreeKB(int capKB)
{
	int lo = 0, hi = capKB;
	while(lo < hi){
		int mid = (lo + hi + 1)/2;
		void *p = __real_malloc(mid*1024);
		if(p){
			free(p);
			lo = mid;
		}else
			hi = mid - 1;
	}
	return lo;
}

// Before a cutscene: the models of the streets around the player are in use
// (their buildings have RwObjects), so unloading "unused" models can't free
// enough and what it frees is scattered in small gaps; the cutscene's
// animations and characters then fail to load and the game hangs on a black
// screen (Kaufman Cabs on a real PSP: 6.7 MB free, a 50 KB block failed).
// The screen is black anyway: let go of the buildings' RwObjects, unload what
// isn't used any more, and the free memory joins up into big blocks again.
// The streets stream back in during the cutscene.
//
// Autosave (like VCS): a mission passed is saved into slot 8 ("Autosave")
// once the mission has ended and Tommy is free to move, and the help box
// (top left) says so
//
bool pspAutosavePending;	// set when a mission is registered as passed
bool pspAutosaving;		// the save skips the safehouse's 6 hours
CVector pspAutosaveSpawn;	// where the save puts Tommy
int pspAutosaveTest;	// testing aid: autostart.txt "autosave" saves 20 s in
#ifndef N3DS
extern bool pspSaveMemBegin(void);
extern void pspSaveMemDiscard(void);
extern bool pspSavedataStoreAsync(int slot);
extern int pspSavedataPump(void);
#endif

// Where an autosave puts Tommy when it's loaded: where he stands; in a car
// or on a bike next to it (the game's saves have no player in a vehicle:
// loading such a save crashed); in a boat, helicopter or plane at the
// nearest hospital (land, the main map)
static bool
autosaveSpawnPoint(CPlayerPed *pl, CVector &out)
{
	if(!pl->InVehicle() || pl->m_pMyVehicle == nil){
		out = pl->GetPosition();
		return true;
	}
	CVehicle *veh = pl->m_pMyVehicle;
	if(veh->IsCar() || veh->IsBike()){
		if(veh->GetVehicleAppearance() == VEHICLE_APPEARANCE_CAR || veh->GetVehicleAppearance() == VEHICLE_APPEARANCE_BIKE){
			// the side of the car with ground under it
			for(int side = 0; side < 2; side++){
				CVector p = veh->GetPosition() + veh->GetRight()*(side ? 2.2f : -2.2f);
				bool found;
				float z = CWorld::FindGroundZFor3DCoord(p.x, p.y, p.z + 2.0f, &found);
				if(found && Abs(z - veh->GetPosition().z) < 3.0f){
					out = CVector(p.x, p.y, z + 1.0f);
					return true;
				}
			}
			out = veh->GetPosition() + CVector(0.0f, 0.0f, 1.5f);
			return true;
		}
	}
	if(CGame::currArea != AREA_MAIN_MAP)
		return false;
	float heading;
	CRestart::FindClosestHospitalRestartPoint(veh->GetPosition(), &out, &heading);
	return true;
}

void
pspAutosaveTick(void)
{
#ifndef N3DS
	static uint32 calmSince;
	static int testFrames;
	// an autosave being written by the PSP: tell the player when it's done
	int done = pspSavedataPump();
	if(done == 1)
		CHud::SetHelpMessage(TheText.Get("PSP_ASV"), false);
	if(pspAutosaveTest && ++testFrames == 600)
		pspAutosavePending = true;
	// autosave taken out (the user's choice): missions don't ask for it any more
	if(!pspAutosaveTest)
		pspAutosavePending = false;
	if(!pspAutosavePending || gGameState != GS_PLAYING_GAME){
		calmSince = 0;
		return;
	}
	CPlayerInfo &pi = CWorld::Players[CWorld::PlayerInFocus];
	CPlayerPed *pl = pi.m_pPed;
	bool calm = pl && pi.m_WBState == WBSTATE_PLAYING && !CTheScripts::IsPlayerOnAMission() &&
		!CCutsceneMgr::IsRunning() && !CCutsceneMgr::IsCutsceneProcessing() &&
		TheCamera.GetScreenFadeStatus() == FADE_0 && !TheCamera.m_WideScreenOn &&
		!CPad::GetPad(0)->ArePlayerControlsDisabled() && pl->GetPedState() != PED_DEAD &&
		pi.m_pRemoteVehicle == nil;
	uint32 now = CTimer::GetTimeInMilliseconds();
	if(!calm){
		static uint32 lastLog;
		if(now - lastLog > 10000){
			lastLog = now;
			debug("AUTOSAVE waiting: mission %d cutscene %d fade %d wide %d controls %d remote %d\n",
				CTheScripts::IsPlayerOnAMission(), CCutsceneMgr::IsRunning(), TheCamera.GetScreenFadeStatus(),
				TheCamera.m_WideScreenOn, CPad::GetPad(0)->ArePlayerControlsDisabled(), pi.m_pRemoteVehicle != nil);
		}
		calmSince = 0;
		return;
	}
	if(calmSince == 0){
		calmSince = now;	// the "mission passed" message first, then save
		return;
	}
	if(now - calmSince < 4000)
		return;
	calmSince = 0;
	{
		// Only the player is kept in a save: a script still holding one of
		// its own characters (the ice cream business, a job...) would find
		// it gone after loading, and the game crashed when it looked at it
		// (Mr. Whoopee at the Cherry Popper). Wait until there's none.
		int scriptPeds = 0;
		for(int i = 0; i < CPools::GetPedPool()->GetSize(); i++){
			CPed *p = CPools::GetPedPool()->GetSlot(i);
			if(p && !p->IsPlayer() && p->CharCreatedBy == MISSION_CHAR)
				scriptPeds++;
		}
		static uint32 waitingSince;
		if(scriptPeds > 0){
			if(waitingSince == 0)
				waitingSince = now;
			static uint32 lastLog;
			if(now - lastLog > 15000){
				lastLog = now;
				debug("AUTOSAVE waiting: %d characters of the scripts around\n", scriptPeds);
			}
			// (a business with its own people for good: no autosave this time)
			if(now - waitingSince > 5*60*1000){
				pspAutosavePending = false;
				waitingSince = 0;
				pspLog("AUTOSAVE skipped: the scripts keep characters of their own\n");
			}
			return;
		}
		waitingSince = 0;
	}
	if(!autosaveSpawnPoint(pl, pspAutosaveSpawn))
		return;	// (in a boat in an interior?) try again later
	if(!pspSaveMemBegin())
		return;	// the last one is still being written
	pspAutosavePending = false;
	const int slot = SLOT_COUNT - 1;	// slot 8
	uint32 t0 = sceKernelGetSystemTimeLow();
	pspAutosaving = true;
	bool ok = GenericSave(-1);
	pspAutosaving = false;
	uint32 t1 = sceKernelGetSystemTimeLow();
	if(ok)
		ok = pspSavedataStoreAsync(slot);
	else
		pspSaveMemDiscard();
	char b[160];
	snprintf(b, sizeof(b), "AUTOSAVE slot %d: %s (%s), made in %u ms, Tommy at %.0f %.0f %.0f%s\n", slot+1,
		ok ? "being written" : "failed", CStats::LastMissionPassedName, (unsigned)((t1 - t0)/1000),
		pspAutosaveSpawn.x, pspAutosaveSpawn.y, pspAutosaveSpawn.z, pl->InVehicle() ? " (out of the vehicle)" : "");
	pspLog(b);
#endif
}

int pspCutsceneForceFree;	// testing aid: "cutscene NAME force"
void
pspMakeRoomForCutscene(int needKB)
{
	if(pspHeapTotalKB == 0)
		return;
	pspEnsureFreeMemory(needKB);
	int largest = pspLargestFreeKB(8192);
	int freeKB = pspFreeHeapKB();
	char b[160];
	if(freeKB < needKB || largest < 4096 || pspCutsceneForceFree){
		int x, y;
		for(x = 0; x < NUMSECTORS_X; x++)
			for(y = 0; y < NUMSECTORS_Y; y++){
				CSector *sect = CWorld::GetSector(x, y);
				CStreaming::DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
				CStreaming::DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
				CStreaming::DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				CStreaming::DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES_OVERLAP]);
			}
		pspEnsureFreeMemory(needKB + 6*1024);
		snprintf(b, sizeof(b), "memory for the cutscene: %d KB free, largest block %d KB; the streets let go: %d KB free, largest block %d KB\n",
			freeKB, largest, pspFreeHeapKB(), pspLargestFreeKB(8192));
	}else
		snprintf(b, sizeof(b), "memory for the cutscene: %d KB free, largest block %d KB\n", freeKB, largest);
	pspLog(b);
	pspLogFlush();
}

#ifdef N3DS
extern "C" unsigned long linearSpaceFree(void);	// libctru
#endif
static void
memoryMaintenance(void)
{
#ifdef N3DS
	{
		// textures and models live in the GPU's memory (28 MB): when it runs
		// low, stream less (the far distance comes in a little, auto quality)
		static int lf;
		if((++lf % 30) == 0){
			unsigned long freeKB = linearSpaceFree()/1024;
			if(freeKB < 3*1024 && CStreaming::ms_memoryAvailable > 16*1024*1024){
				CStreaming::ms_memoryAvailable -= 2*1024*1024;
				int n = 0;
				while(n < 64 && CStreaming::ms_memoryUsed > CStreaming::ms_memoryAvailable &&
				      CStreaming::RemoveLeastUsedModel(0))
					n++;
				char b[128];
				snprintf(b, sizeof(b), "memory: GPU memory low (%lu KB free): streaming budget %d KB, %d models unloaded\n",
					freeKB, (int)(CStreaming::ms_memoryAvailable/1024), n);
				pspLog(b);
			}
		}
	}
#endif
	static int frame;
	static int lastFailures;
	static int calmFrames;
	static int fullBudget;
	if(fullBudget == 0)
		fullBudget = pspStreamingBudget();
	batches = 0;
	if(memFailures != lastFailures){
		// we ran dry: stream less for a while
		lastFailures = memFailures;
		calmFrames = 0;
		if(CStreaming::ms_memoryAvailable > 12*1024*1024)
			CStreaming::ms_memoryAvailable -= 2*1024*1024;
		char b[96];
		snprintf(b, sizeof(b), "memory: streaming budget lowered to %d KB\n", (int)(CStreaming::ms_memoryAvailable/1024));
		pspLog(b);
		pspEnsureFreeMemory(MIN_FREE_KB + RESERVE_SIZE/1024);
	}else{
		if((++frame % 10) == 0)
			pspEnsureFreeMemory(MIN_FREE_KB);
		if((frame % 30) == 15 && !CCutsceneMgr::IsRunning()){
			// the free memory got cut up into small gaps: unload a few
			// unused models so they join up again (big loads need 512 KB+)
			int largest = pspLargestFreeKB(1024);
			if(largest < 768){
				int n = 0;
				while(n < 32 && largest < 1024 && CStreaming::RemoveLeastUsedModel(0)){
					n++;
					if((n & 7) == 0) largest = pspLargestFreeKB(1024);
				}
				static int logged;
				if(logged++ < 40){
					char b[128];
					snprintf(b, sizeof(b), "memory: free memory in small pieces, unloaded %d models, largest block now %d KB\n", n, pspLargestFreeKB(1024));
					pspLog(b);
				}
			}
		}
		// 90 s without trouble: give 1 MB back, up to the normal budget
		if(++calmFrames >= 2700 && CStreaming::ms_memoryAvailable < fullBudget){
			calmFrames = 0;
#ifdef N3DS
			if(pspFreeHeapKB() > MIN_FREE_KB + 4096 && linearSpaceFree() > 6*1024*1024){
#else
			if(pspFreeHeapKB() > MIN_FREE_KB + 4096){
#endif
				CStreaming::ms_memoryAvailable += 1024*1024;
				if(CStreaming::ms_memoryAvailable > fullBudget)
					CStreaming::ms_memoryAvailable = fullBudget;
				char b[96];
				snprintf(b, sizeof(b), "memory: streaming budget raised to %d KB\n", (int)(CStreaming::ms_memoryAvailable/1024));
				pspLog(b);
			}
		}
	}
	if(emergencyReserve == nil)
		emergencyReserve = __real_malloc(RESERVE_SIZE);
}
void
psPrintMemory(const char *where)
{
	struct mallinfo mi = mallinfo();
	debug("MEM %s: heap used %d KB, free in heap %d KB, streaming %d KB / %d KB, ground fix: %u models %u -> %u verts (%u skipped), display list peak %u KB%s\n", where,
		mi.uordblks/1024, mi.fordblks/1024,
		(int)(CStreaming::ms_memoryUsed/1024), (int)(CStreaming::ms_memoryAvailable/1024),
		(unsigned)rw::psp::tessStats[0], (unsigned)rw::psp::tessStats[1], (unsigned)rw::psp::tessStats[2], (unsigned)rw::psp::tessStats[3],
		(unsigned)(rw::psp::listPeakBytes/1024), rw::psp::listDroppedDraws ? " (FULL, draws dropped)" : "");
	rw::psp::listPeakBytes = 0;
	rw::psp::listDroppedDraws = 0;
#ifndef N3DS
	debug("CLIP models checked %u, clipped %u, triangles clipped %u, dropped %u, vertices %u, areas %u (%u partly in), triangles checked %u whole models %u by area (per 60 frames), %u triangles listed by area\n",
		(unsigned)rw::psp::clipStats[0], (unsigned)rw::psp::clipStats[1], (unsigned)rw::psp::clipStats[2], (unsigned)rw::psp::clipStats[3],
		(unsigned)rw::psp::clipStats[4], (unsigned)rw::psp::clipStats[5], (unsigned)rw::psp::clipStats[6],
		(unsigned)rw::psp::clipStats[7], (unsigned)rw::psp::clipStats[8], (unsigned)rw::psp::clipStats[9]);
	memset(rw::psp::clipStats, 0, 9*sizeof(rw::psp::clipStats[0]));	// ([9] counts up)
	debug("BAKE models recorded %u, replayed %u, refused %u, pool emptied %u times, building draws %u (%u clipped) (per 60 frames)%s\n",
		(unsigned)rw::psp::bakeStats[0], (unsigned)rw::psp::bakeStats[1], (unsigned)rw::psp::bakeStats[2],
		(unsigned)rw::psp::bakeStats[3], (unsigned)rw::psp::bakeStats[4], (unsigned)rw::psp::bakeStats[5],
		rw::psp::bakeEnabled ? "" : ", off");
	memset(rw::psp::bakeStats, 0, sizeof(rw::psp::bakeStats));
#endif
#ifdef N3DS
	extern void n3dsPrintMemory(void);
	n3dsPrintMemory();
#endif
}

// Auto quality: a steady 30 fps with as much draw distance as the frame
// time allows. It measures how long each frame really takes to build (CPU
// work, including any wait for the GE, without the frame limiter's sleep),
// so it knows how much time is left over, not just whether 30 fps was held.
//  - too slow (average over 26 ms, or frames over the 33 ms budget):
//    less model detail first, then the far distance (never below
//    min_draw_distance); people and cars are never thinned out (PSP-2000/3000)
//  - time to spare (average under 21 ms, longest under 30 ms): people and
//    cars first, then the far distance (up to draw_distance, 25% further in
//    vehicles), then the detail
// With fps_target=20 the budget is 50 ms instead of 33.
static uint32 aqBusySum, aqBusyMax, aqFrames, aqOver;
static void
autoQualityFrame(uint32 busy)
{
	aqBusySum += busy;
	if(busy > aqBusyMax) aqBusyMax = busy;
	if(busy > (pspTune.fpsTarget == 20 ? 50000u : 33300u)) aqOver++;
	aqFrames++;
}

static void
autoQuality(uint32 now)
{
	static int calm, settle;
	static float detail = 1.0f;
	(void)now;
	if(!pspTune.autoQuality || FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::IsRunning() ||
	   gGameState != GS_PLAYING_GAME){
		aqBusySum = aqBusyMax = aqFrames = aqOver = 0;
		settle = 30;
		return;
	}
	if(aqFrames < 30)
		return;
	float scale = pspTune.fpsTarget == 20 ? 33.333f/50.0f : 1.0f;	// thresholds below are for 30 fps
	float avg = aqBusySum / 1000.0f / aqFrames * scale;
	float mx = aqBusyMax / 1000.0f * scale;
	int over = aqOver;
	aqBusySum = aqBusyMax = aqFrames = aqOver = 0;
	// after a load (teleport, restart) the first frames stream: ignore them
	if(settle > 0){ settle -= 30; return; }

	CPlayerPed *pl = FindPlayerPed();
	bool driving = pl && pl->InVehicle();
	float farTarget = pspTune.drawDistance * (driving ? 1.25f : 1.0f);
	float minD = Min(pspTune.minDrawDistance * (driving ? 1.15f : 1.0f), farTarget);
	float d = pspDynDrawDistance;
	// the streamer can't keep up (a long queue of models waiting, they
	// wouldn't fit or arrive in time): no further out for now
	static int queueHigh;
	queueHigh = CStreaming::ms_numModelsRequested > 30 ? queueHigh + 1 : 0;
	bool memTight = queueHigh >= 3;

	// The city should stay busy (VCS): when too slow the model detail goes
	// first, then the far distance (down to min_draw_distance), the people
	// and cars only last and never below 80%. With time to spare they come
	// back first, then the distance, then the detail.
#ifdef N3DS
	// 3DS: more room kept free, every frame past 33 ms is a visible hitch
	int slow = avg > 28.0f || over >= 3 ? 2 : avg > 24.0f || over >= 1 ? 1 : 0;
	const float calmAvg = 19.0f, calmMax = 28.0f;
#else
	int slow = avg > 30.0f || over >= 4 ? 2 : avg > 26.0f || over >= 2 ? 1 : 0;
	const float calmAvg = 21.0f, calmMax = 30.0f;
#endif
	if(slow){
		calm = 0;
#ifdef N3DS
		const float minDetail = 0.5f, minDensity = 0.6f;
#else
		const float minDetail = 0.6f, minDensity = 0.8f;
#endif
		if(detail > minDetail)
			detail = Max(detail - 0.06f*slow, minDetail);
		else if(d > minD + 1.0f)
			d *= slow == 2 ? 0.88f : 0.95f;
#if defined(PSP_LITE) || defined(N3DS)
		else if(pspDynDensity > minDensity)
			pspDynDensity = Max(pspDynDensity - 0.05f*slow, minDensity);
#endif
	}else if(avg < calmAvg && mx < calmMax && ++calm >= 2){
		calm = 1;	// then every second
		if(pspDynDensity < 1.0f)
			pspDynDensity = Min(pspDynDensity + 0.05f, 1.0f);
		else if(d < farTarget && !memTight)
			d += avg < 15.0f ? 30.0f : 15.0f;
		else if(detail < 1.0f)
			detail = Min(detail + 0.04f, 1.0f);
	}
	if(queueHigh >= 8 && d > minD)	// still waiting after 8 s: pull in a little
		d -= 10.0f;
	if(d < minD) d = minD;
	if(d > farTarget) d = farTarget;
	pspDynDrawDistance = d;
	pspDynLodScale = pspTune.lodScale * detail;
	pspDynDetail = detail;
}

// testing aid: autostart.txt "keys 900S 960d 1020x 1100P ..." presses a
// button on that frame (S start, e select, x cross, o circle, q square,
// t triangle, L, R, u d l r d-pad, P saves keyshotN.ppm); frames count from
// when the game is up
static struct { int f; char k; } keyScript[64];
static int keyScriptN = -1, keyFrame, keyShotPending, keyShotNum;
static void
loadKeyScript(void)
{
	keyScriptN = 0;
	FILE *f = fopen("autostart.txt", "r");
	if(f == nil) return;
	char t[512] = { 0 };
	fread(t, 1, 511, f);
	fclose(f);
	const char *k = strstr(t, "keys");
	if(k == nil) return;
	k += 4;
	int fr, n; char c;
	while(keyScriptN < 64 && sscanf(k, " %d%c%n", &fr, &c, &n) == 2){
		keyScript[keyScriptN].f = fr;
		keyScript[keyScriptN].k = c;
		keyScriptN++;
		k += n;
	}
}
static uint32
keyScriptButtons(void)
{
	if(keyScriptN < 0) loadKeyScript();
	if(keyScriptN == 0 || gGameState != GS_PLAYING_GAME) return 0;
	keyFrame++;
	uint32 b = 0;
	for(int i = 0; i < keyScriptN; i++){
		int d = keyFrame - keyScript[i].f;
		if(d < 0 || d > 3) continue;	// held for 4 frames
		switch(keyScript[i].k){
		case 'S': b |= PSP_CTRL_START; break;
		case 'e': b |= PSP_CTRL_SELECT; break;
		case 'x': b |= PSP_CTRL_CROSS; break;
		case 'o': b |= PSP_CTRL_CIRCLE; break;
		case 'q': b |= PSP_CTRL_SQUARE; break;
		case 't': b |= PSP_CTRL_TRIANGLE; break;
		case 'L': b |= PSP_CTRL_LTRIGGER; break;
		case 'R': b |= PSP_CTRL_RTRIGGER; break;
		case 'u': b |= PSP_CTRL_UP; break;
		case 'd': b |= PSP_CTRL_DOWN; break;
		case 'l': b |= PSP_CTRL_LEFT; break;
		case 'r': b |= PSP_CTRL_RIGHT; break;
		case 'P': if(d == 0) keyShotPending = 1; break;
		case 'O': if(d == 0) pspTestOrbit = 40; break;	// L + stick right for 40 frames
		}
	}
	return b;
}

// Memory watch: the particle textures (smoke, dust) turned into dark squares
// on a PSP; a checksum of their pixels and palettes is taken when they load
// and compared every two seconds, so the log says when something overwrote them
#ifndef N3DS
static rw::TexDictionary *watchDict;
static uint32 watchSum;
static uint32
particleTexSum(void)
{
	uint32 sum = 0;
	FORLIST(lnk, watchDict->textures){
		rw::Texture *t = rw::Texture::fromDict(lnk);
		if(t->raster == nil) continue;
		rw::psp::PspRaster *n = GETPSPRASTEREXT(t->raster);
		const uint32 *d = (const uint32*)n->data;
		for(uint32 i = 0; d && i < n->dataSize/4; i++)
			sum = sum*31 + d[i];
		if(n->palette)
			for(int i = 0; i < (n->psm == rw::psp::PSM_T4 ? 16 : 256); i++)
				sum = sum*31 + n->palette[i];
	}
	return sum;
}
#endif
void
pspWatchParticleTextures(void *texDict)
{
#ifndef N3DS
	watchDict = (rw::TexDictionary*)texDict;
	watchSum = particleTexSum();
#endif
}
static void
pspCheckParticleTextures(void)
{
#ifndef N3DS
	static bool told;
	if(watchDict && !told && particleTexSum() != watchSum){
		told = true;
		debug("MEMORY: the particle textures were overwritten (smoke would show as squares)\n");
	}
#endif
}

int pspShotCountdown;	// cheat menu "Take screenshot": frames until the picture is saved
static void
pspTestShots(void)
	{
	if(pspShotCountdown > 0 && --pspShotCountdown == 0){
		keyShotPending = 1;
		extern void pspCheatMessage(const char *msg);
		pspCheatMessage("Screenshot saved");
	}
	if(keyShotPending){
		keyShotPending = 0;
		rw::Image *img = rw::psp::grabFrameBuffer();
		char name[32];
		sprintf(name, "keyshot%d.ppm", keyShotNum++);
		FILE *f = fopen(name, "wb");
		if(f){
			fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
			for(int y = 0; y < img->height; y++)
				for(int x = 0; x < img->width; x++)
					fwrite(img->pixels + y*img->stride + x*4, 1, 3, f);
			fclose(f);
		}
		img->destroy();
		debug("KEYSHOT %s frame %d\n", name, keyFrame);
	}
	// testing aid: autostart.txt "cutshots N" saves frames N..N+5 of the
	// first cutscene as shotK.ppm (flicker hunting)
	static int shotStart = -2, cutFrames;
	if(shotStart == -2){
		shotStart = -1;
		FILE *f = fopen("autostart.txt", "r");
		if(f){
			char t[64] = { 0 };
			fread(t, 1, 63, f);
			fclose(f);
			const char *c = strstr(t, "cutshots");
			if(c) shotStart = atoi(c+8);
			debug("SHOTS from %d\n", shotStart);
		}
	}
	if(shotStart >= 0 && CCutsceneMgr::IsRunning()){
		cutFrames++;
		if(cutFrames >= shotStart && cutFrames < shotStart + 12){
			rw::Image *img = rw::psp::grabFrameBuffer();
			char name[32];
			sprintf(name, "shot%d.ppm", cutFrames - shotStart);
			FILE *f = fopen(name, "wb");
			for(int oi = 0; oi < 3; oi++){
				CCutsceneObject *o = CCutsceneMgr::GetCutsceneObject(oi);
				if(o == nil || o->m_rwObject == nil || RwObjectGetType(o->m_rwObject) != rpCLUMP) continue;
				RpHAnimHierarchy *h = GetAnimHierarchyFromSkinClump(o->GetClump());
				CVector p = o->GetPosition();
				if(h && h->numNodes > 2)
					debug("OBJ %d model %d pos %d %d %d bone0 %d %d %d bone2 %d %d %d (mm)\n", oi, o->GetModelIndex(),
						(int)(p.x*1000), (int)(p.y*1000), (int)(p.z*1000),
						(int)(h->matrices[0].pos.x*1000), (int)(h->matrices[0].pos.y*1000), (int)(h->matrices[0].pos.z*1000),
						(int)(h->matrices[2].pos.x*1000), (int)(h->matrices[2].pos.y*1000), (int)(h->matrices[2].pos.z*1000));
			}
			{ debug("SHOT %s step %d/1000 nc %d/1000 time %u cut %d\n", name, (int)(CTimer::GetTimeStep()*1000.0f), (int)(CTimer::GetTimeStepNonClipped()*1000.0f), CTimer::GetTimeInMilliseconds(), CCutsceneMgr::GetCutsceneTimeInMilleseconds()); }
			if(f){
				fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
				for(int y = 0; y < img->height; y++)
					for(int x = 0; x < img->width; x++)
						fwrite(img->pixels + y*img->stride + x*4, 1, 3, f);
				fclose(f);
			}
			img->destroy();
		}
	}
}

// VCS-like motion blur: trails that grow with the speed of the player's
// vehicle (none on foot, in menus or cutscenes). Called before the HUD.
int pspBlurShown;	// for the log
void
pspMotionBlurFrame(void)
{
#ifdef N3DS
	// the 3DS backend's blur showed the picture shifted (and the game crashed
	// driving fast with it): off until it can be checked on a real 3DS
	return;
#endif
	{
	static float amount;
	float target = 0.0f;
	CVehicle *veh = FindPlayerVehicle();
	if(pspTune.motionBlur && veh && !CCutsceneMgr::IsRunning() && !TheCamera.m_WideScreenOn &&
	   !FrontEndMenuManager.m_bMenuActive){
		// 1 unit per frame is about 180 km/h: from ~70 km/h to full at ~200 km/h
		float speed = veh->GetMoveSpeed().Magnitude();
		float t = (speed - 0.4f) / 0.7f;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		target = t * (pspTune.motionBlur == 2 ? 165.0f : 110.0f);
	}
	{
		// testing aid: autostart.txt "blurtest" keeps the blur on
		static int blurTest = -1;
		if(blurTest < 0){
			blurTest = 0;
			FILE *f = fopen("autostart.txt", "r");
			if(f){
				char t[128] = { 0 };
				fread(t, 1, 127, f);
				fclose(f);
				blurTest = strstr(t, "blurtest") != nil;
			}
		}
		if(blurTest)
			target = 200.0f;
	}
	amount += (target - amount) * 0.12f;	// fades in and out over about half a second
	if(amount < 4.0f && target == 0.0f)
		amount = 0.0f;
	rw::psp::motionBlur((int)amount);
	pspBlurShown = (int)amount;
	}
}

static uint32 loopMHz(uint32 n);
static uint32 setFullClock(const char *when);
extern uint32 pspCpuMHz;
uint32 pspFrameStartUs;	// when the main loop started this frame
uint32 pspFrameStartRun;	// CPU time of the main thread then
// CPU time the main thread has used, microseconds (the rest of a frame's
// time it was waiting: file reads, the GE, other threads)
uint32
pspMainRunUs(void)
{
#ifdef N3DS
	return 0;
#else
	SceKernelThreadRunStatus st;
	st.size = sizeof(st);
	if(sceKernelReferThreadRunStatus(0, &st) < 0) return 0;
	return st.runClocks.low;
#endif
}
#ifdef N3DS
#define PSP_GEWAIT 0u
#else
#define PSP_GEWAIT rw::psp::geWaitUs
#endif
static uint32 profPrev[PSPPROF_SLOTS], geWaitPrev;	// profiler at the end of the last frame (spike log)
// per-frame work (CPU, including any wait for the GE) of the last 60 frames
static uint32 busySum, busyMax, busyOver, gapMax, gapOver;
uint32 pspBusyAvgUs = 0, pspBusyMaxUs = 0;	// last window, for auto quality

void
psCameraShowRaster(RwCamera *camera)
{
	pspFramesDone++;
	static int frames;
	static uint32 lastTime;
	uint32 now = sceKernelGetSystemTimeLow();
	memoryMaintenance();
	autoQuality(now);
	{
		// on-screen counter: frames shown in the last half second
		static uint32 fpsStart;
		static int fpsFrames;
		fpsFrames++;
		uint32 dt = now - fpsStart;
		if(dt >= 500000){
			pspFpsShown = (int)((fpsFrames * 1000000u + dt/2) / dt);
			fpsFrames = 0;
			fpsStart = now;
		}
	}

	// often enough that a crash loses at most about a second of the log
#ifdef N3DS
	// the 3DS: a write to the SD card took 100-300 ms now and then (hitches
	// in the log's own frame times): a thread of its own writes it
	{
		extern void n3dsLogWriterStart(void);
		n3dsLogWriterStart();
	}
#else
	if((frames % 15) == 14)
		pspLogFlush();
#endif
#ifdef N3DS
	// testing: when the font's texture or a block of linear memory nothing
	// uses changes, say so once (a corruption seen only on the real 3DS)
	if(pspLogOn() && (frames % 10) == 0){
		static uint32 fontSum, told;
		static int32 fontFrames;
		RwTexture *ft = CFont::Sprite[0].m_pTexture;
		uint32 sum = ft ? rw::psp::c3dRasterChecksum(ft->raster) : 0;
		if(sum != fontSum){
			if(fontSum && fontFrames > 30 && told < 3){
				told++;
				char b[200];
				snprintf(b, sizeof(b), "CHECK: the font texture changed, frame %u, step '%s', mp3 frames %u, linear free %u KB\n",
					(unsigned)frames, (const char*)pspStep, (unsigned)n3dsMp3Frames, (unsigned)(linearSpaceFree()/1024));
				pspLog(b);
			}
			fontSum = sum;
			fontFrames = 0;
		}else
			fontFrames++;
		static bool canaryTold;
		int32 bad = rw::psp::c3dCanaryCheck();
		if(bad >= 0 && !canaryTold){
			canaryTold = true;
			char b[200];
			snprintf(b, sizeof(b), "CHECK: linear memory nothing uses was written (word %d), frame %u, mp3 frames %u\n",
				(int)bad, (unsigned)frames, (unsigned)n3dsMp3Frames);
			pspLog(b);
		}
	}
#endif
	if((++frames % 60) == 0 && pspTune.showFps){
		// no floating point printing here: newlib formats floats with
		// software doubles, which cost several % of the CPU
		psPrintMemory("frame");
		{
			uint8 *csp;
#ifdef N3DS
			asm volatile("mov %0, sp" : "=r"(csp));
#else
			asm volatile("move %0, $sp" : "=r"(csp));
#endif
			debug("STACK main thread peak %d KB of 512 KB (now %d KB)\n", pspStackUsedKB(), (int)((stackTop - csp)/1024));
		}
		uint32 dt = now - lastTime;
		if(dt == 0) dt = 1;
		uint32 frame10 = dt/6000;	// tenths of ms per frame
		uint32 fps10 = 600000000u/dt;
		debug("PERF avg frame %u.%u ms (%u.%u fps), draw calls %u\n", frame10/10, frame10%10, fps10/10, fps10%10, (unsigned)rw::psp::drawCalls);
		{
			pspBusyAvgUs = busySum/60;
			pspBusyMaxUs = busyMax;
#ifndef N3DS
			uint32 gw = rw::psp::geWaitUs/60;
			rw::psp::geWaitUs = 0;
			geWaitPrev = 0;
			{
				// best of two short runs (an interrupt can land in one)
				uint32 a = loopMHz(40000), c = loopMHz(40000);
				pspCpuMHz = a > c ? a : c;
				// slow: ask for the full clock again (something may have lowered it), every 10 s at most
				static int sinceSet = 100;
				if(pspCpuMHz < 260 && ++sinceSet >= 5){
					sinceSet = 0;
					pspCpuMHz = setFullClock("slow, again");
				}
			}
			debug("PERF work avg %u.%u ms max %u.%u ms, %u frames over 33 ms, GE wait avg %u.%u ms, longest gap %u.%u ms (%u hitches), verts %u, conversions put off %u, cpu %u MHz, blur %d\n",
				pspBusyAvgUs/1000, pspBusyAvgUs/100%10, busyMax/1000, busyMax/100%10, busyOver, gw/1000, gw/100%10,
				gapMax/1000, gapMax/100%10, gapOver, (unsigned)rw::psp::lastFrameVerts, (unsigned)rw::psp::instanceDeferred, (unsigned)pspCpuMHz, pspBlurShown);
			rw::psp::instanceDeferred = 0;
#endif
			busySum = busyMax = busyOver = gapMax = gapOver = 0;
		}
		{
			// where the player is, to find the place of a crash in the log
			CPlayerPed *pl = FindPlayerPed();
			if(pl){
				CVector p = pl->GetPosition();
				CVehicle *v = FindPlayerVehicle();
				debug("WHERE player %d %d %d vehicle %d cutscene %d, people %d of %d wanted (%d models), cars %d of %d\n", (int)p.x, (int)p.y, (int)p.z,
					v ? v->GetModelIndex() : -1, CCutsceneMgr::IsRunning() ? 1 : 0,
					(int)CPopulation::ms_nTotalPeds, (int)pspPedsWanted, (int)CStreaming::ms_numPedsLoaded,
					(int)CCarCtrl::NumRandomCars, (int)(CWorld::Players[0].m_nTrafficMultiplier * CCarCtrl::CarDensityMultiplier * pspTune.carDensity * pspDynDensity));
				// the traffic's car models: how many are loaded, how many kinds drive around
				int kinds = 0, seen[16];
				int n = CPools::GetVehiclePool()->GetSize();
				for(int i = 0; i < n && kinds < 16; i++){
					CVehicle *c = CPools::GetVehiclePool()->GetSlot(i);
					if(c == nil || c == v) continue;
					int m = c->GetModelIndex(), k;
					for(k = 0; k < kinds && seen[k] != m; k++);
					if(k == kinds) seen[kinds++] = m;
				}
				debug("TRAFFIC car models loaded %d, kinds of car around %d\n", (int)CStreaming::ms_numVehiclesLoaded, kinds);
			}
		}
		// (pspCheckParticleTextures: debugging, it summed every particle texture each frame)
		{
			namespace rp = rw::psp;
			static uint32 lastT4 = ~0u;
			if(rp::t4Expanded != lastT4){
				lastT4 = rp::t4Expanded;
				debug("TEX 4 bit textures made 8 bit: %d, kept (opaque or mipmapped): %d\n", (int)rp::t4Expanded, (int)rp::t4Kept);
			}
		}
		{
			// screen held black outside a cutscene: log what the mission script is waiting for
			extern int pspScriptTrace;
			static int fadedSecs;
			if(!CCutsceneMgr::IsRunning() && !FrontEndMenuManager.m_bMenuActive &&
			   TheCamera.GetScreenFadeStatus() == FADE_2){
				if(++fadedSecs >= 2 && fadedSecs < 20){
					pspScriptTrace = 24;
					char rq[300]; char *q = rq; int n = 0;
					for(CStreamingInfo *si = CStreaming::ms_startRequestedList.m_next;
					    si && si != &CStreaming::ms_endRequestedList && n < 24; si = si->m_next, n++)
						q += sprintf(q, " %d:%d", (int)(si - CStreaming::ms_aInfoForModel), si->m_loadState);
					*q = 0;
					debug("FADED %d s: %d models requested:%s\n", fadedSecs*2, (int)CStreaming::ms_numModelsRequested, rq);
				}
				// (counted per 60 frames, ~2 s) a black screen for half a minute is never on purpose
				if(fadedSecs == 15){
					debug("FADED: screen black for too long, fading back in\n");
					TheCamera.Fade(1.0f, FADE_IN);
				}
			}else
				fadedSecs = 0;
		}
		lastTime = now;
		char line[400];
		char *lp;
#define MS(label, n) do { uint32 t_ = pspProf[n]/6000; lp += sprintf(lp, label " %u.%u", t_/10, t_%10); } while(0)
		lp = line;
		MS("PROF ms/frame: process", 0); MS(" renderlist", 1); MS(" horizon", 7); MS(" scene", 2);
		MS(" effects", 3); MS(" 2d", 4); MS(" menus/fade", 5); MS(" endframe(GE wait)", 6);
		debug("%s\n", line);
		lp = line;
		MS("PROF scene: roads", 8); MS(" world", 9); MS(" water", 10); MS(" fading/boats", 11);
		MS(" | effects: shadows", 12); MS(" coronas", 13); MS(" particles", 14); MS(" other", 15);
		debug("%s\n", line);
		int nMoving = 0;
		for(CPtrNode *node = CWorld::GetMovingEntityList().first; node; node = node->next)
			nMoving++;
		lp = line;
		MS("PROF process: streaming", 16); MS(" scripts", 17); MS(" population", 18); MS(" misc", 21);
		MS(" world", 19); MS(" (control", 22); MS(" collision", 23);
		lp += sprintf(lp, ", %d moving, %d peds %d vehicles)", nMoving,
			CPools::GetPedPool() ? CPools::GetPedPool()->GetNoOfUsedSpaces() : 0,
			CPools::GetVehiclePool() ? CPools::GetVehiclePool()->GetNoOfUsedSpaces() : 0);
		MS(" camera/rest", 20);
		debug("%s\n", line);
		lp = line;
		MS("PROF 2d: colour filter", 24); MS(" hud", 25); MS(" (radar", 26); MS(") messages", 27); MS(" fonts", 28);
		{
			extern int pspAlphaInserts, pspVisibleEntities;
			lp += sprintf(lp, " | entities visible %d, sorted alpha %d, draw distance %d, detail %d%%, density %d%%",
				pspVisibleEntities, pspAlphaInserts/60, (int)pspDynDrawDistance, (int)(pspDynDetail*100.0f + 0.5f),
				(int)(pspDynDensity*100.0f + 0.5f));
			lp += sprintf(lp, ", hud copy %u KB", (unsigned)(rw::psp::overlayBytes/1024));
			pspAlphaInserts = 0;
		}
		debug("%s\n", line);
#undef MS
		extern int gColPlaneRecalcs, gColPlaneTris;
		debug("PROF collision cache: %d models / %d triangles recalculated per 60 frames\n", gColPlaneRecalcs, gColPlaneTris);
		gColPlaneRecalcs = gColPlaneTris = 0;
		memset(pspProf, 0, sizeof(pspProf));
		memset(profPrev, 0, sizeof(profPrev));
	}
#ifdef LEGACY_MENU_OPTIONS
	if(FrontEndMenuManager.m_PrefsVsync || FrontEndMenuManager.m_bMenuActive)
#else
	if(pspTune.vsync && (FrontEndMenuManager.m_PrefsFrameLimiter || FrontEndMenuManager.m_bMenuActive))
#endif
		RwCameraShowRaster(camera, nil, rwRASTERFLIPWAITVSYNC);
	else
		RwCameraShowRaster(camera, nil, rwRASTERFLIPDONTWAIT);
	pspTestShots();	// after the frame's display list is closed
	extern void pspTourShot(void);
	pspTourShot();
	{
		// how long this frame really took to build (the frame limiter's
		// sleep not included), and gaps between shown frames (hitches)
		static uint32 lastShown;
		uint32 t = sceKernelGetSystemTimeLow();
		if(pspFrameStartUs){
			uint32 busy = t - pspFrameStartUs;
			busySum += busy;
			if(busy > busyMax) busyMax = busy;
			autoQualityFrame(busy);
			// a frame much slower than usual: which part of it (log only)
			static uint32 avgBusy = 10000;
			if(pspTune.showFps && busy > 2*avgBusy + 4000 && busy > 12000 && gGameState == GS_PLAYING_GAME){
				char line[256], *lp = line;
				uint32 ran = pspMainRunUs() - pspFrameStartRun;
				uint32 gw = PSP_GEWAIT - geWaitPrev;
				lp += sprintf(lp, "SPIKE %u.%u ms (usual %u.%u, CPU %u.%u, GE wait %u.%u):", busy/1000, busy/100%10, avgBusy/1000, avgBusy/100%10,
					ran/1000, ran/100%10, gw/1000, gw/100%10);
				for(int i = 0; i < PSPPROF_SLOTS; i++){
					uint32 d = pspProf[i] - profPrev[i];
					if(d > 1500 && lp - line < 220)
						lp += sprintf(lp, " %d:%u.%u", i, d/1000, d/100%10);
				}
				extern unsigned int pspSlowestEntityUs; extern int pspSlowestEntity;
				if(pspSlowestEntityUs > 2000 && pspSlowestEntity >= 0)
					lp += sprintf(lp, " | slowest %s %u.%u ms", CModelInfo::GetModelInfo(pspSlowestEntity)->GetModelName(),
						pspSlowestEntityUs/1000, pspSlowestEntityUs/100%10);
				debug("%s\n", line);
			}
			{ extern unsigned int pspSlowestEntityUs; pspSlowestEntityUs = 0; }
			avgBusy = (avgBusy*15 + busy)/16;
			geWaitPrev = PSP_GEWAIT;
			memcpy(profPrev, pspProf, sizeof(profPrev));
			if(busy > 33300) busyOver++;
		}
		if(lastShown){
			uint32 gap = t - lastShown;
			if(gap > gapMax) gapMax = gap;
			if(gap > 40000) gapOver++;
		}
		lastShown = t;
	}
}

RwImage *
psGrabScreen(RwCamera *pCamera)
{
	rw::Image *image = RwCameraGetRaster(pCamera)->toImage();
	if(image){
		image->removeMask();
		return image;
	}
	return nil;
}

RwUInt32
psTimerMS(void)
{
	return (RwUInt32)(sceKernelGetSystemTimeWide() / 1000);
}

double
psTimer(void)
{
	// microseconds since boot
	return (double)sceKernelGetSystemTimeWide() / 1000.0;
}

void
psMouseSetPos(RwV2d *pos)
{
	FrontEndMenuManager.m_nMouseTempPosX = pos->x;
	FrontEndMenuManager.m_nMouseTempPosY = pos->y;
	FrontEndMenuManager.m_nMousePosX = pos->x;
	FrontEndMenuManager.m_nMousePosY = pos->y;
	FrontEndMenuManager.m_nMouseOldPosX = pos->x;
	FrontEndMenuManager.m_nMouseOldPosY = pos->y;
	PSGLOBAL(lastMousePos.x) = (RwInt32)pos->x;
	PSGLOBAL(lastMousePos.y) = (RwInt32)pos->y;
}

RwMemoryFunctions*
psGetMemoryFunctions(void)
{
	return nil;
}

RwBool
psInstallFileSystem(void)
{
	return TRUE;
}

RwBool
psNativeTextureSupport(void)
{
	return TRUE;
}

/*
 *****************************************************************************
 */
RwBool
psInitialize(void)
{
	PsGlobal.lastMousePos.x = PsGlobal.lastMousePos.y = 0.0f;
	RsGlobal.ps = &PsGlobal;
	PsGlobal.fullScreen = TRUE;
	PsGlobal.joy1id = 0;
	PsGlobal.joy2id = -1;

	CFileMgr::Initialise();

	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());

	InitialiseLanguage();

	gGameState = GS_START_UP;
	TRACE("gGameState = GS_START_UP");
	_dwOperatingSystemVersion = OS_WINXP;

	FrontEndMenuManager.LoadSettings();

	_dwMemAvailPhys = sceKernelTotalFreeMemSize();
	debug("Free memory %u, largest block %u\n", (unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize());

	TheText.Unload();

	return TRUE;
}

void
psTerminate(void)
{
}

static RwChar **_VMList;

RwInt32 _psGetNumVideModes()
{
	return RwEngineGetNumVideoModes();
}

RwBool _psFreeVideoModeList()
{
	RwInt32 numModes = _psGetNumVideModes();
	if(_VMList == nil)
		return TRUE;
	for(RwInt32 i = 0; i < numModes; i++)
		RwFree(_VMList[i]);
	RwFree(_VMList);
	_VMList = nil;
	return TRUE;
}

RwChar **_psGetVideoModeList()
{
	if(_VMList != nil)
		return _VMList;
	RwInt32 numModes = RwEngineGetNumVideoModes();
	_VMList = (RwChar **)RwCalloc(numModes, sizeof(RwChar*));
	for(RwInt32 i = 0; i < numModes; i++){
		RwVideoMode vm;
		RwEngineGetVideoModeInfo(&vm, i);
		_VMList[i] = (RwChar*)RwCalloc(100, sizeof(RwChar));
		rwsprintf(_VMList[i], "%d X %d X %d", vm.width, vm.height, vm.depth);
	}
	return _VMList;
}

void _psSelectScreenVM(RwInt32 videoMode)
{
	// only one mode on the PSP
}

RwBool IsForegroundApp()
{
	return !!ForegroundApp;
}

RwBool
psSelectDevice()
{
	RwVideoMode vm;
	if(!RwEngineSetSubSystem(0))
		return FALSE;
	if(!RwEngineSetVideoMode(0))
		return FALSE;
	RwEngineGetVideoModeInfo(&vm, 0);
	RsGlobal.maximumWidth = vm.width;
	RsGlobal.maximumHeight = vm.height;
	RsGlobal.width = vm.width;
	RsGlobal.height = vm.height;
	PSGLOBAL(fullScreen) = TRUE;
#ifdef IMPROVED_VIDEOMODE
	FrontEndMenuManager.m_nPrefsWidth = vm.width;
	FrontEndMenuManager.m_nPrefsHeight = vm.height;
	FrontEndMenuManager.m_nPrefsDepth = vm.depth;
	FrontEndMenuManager.m_nPrefsWindowed = 0;
	FrontEndMenuManager.m_nPrefsSubsystem = 0;
#endif
	FrontEndMenuManager.m_nPrefsVideoMode = 0;
	FrontEndMenuManager.m_nDisplayVideoMode = 0;
	FrontEndMenuManager.m_nCurrOption = 0;
	return TRUE;
}

void _InputTranslateShiftKeyUpDown(RsKeyCodes *rs) {}
long _InputInitialiseMouse(bool exclusive) { return 0; }
void _InputShutdownMouse() {}
bool _InputMouseNeedsExclusive() { return false; }
void _InputInitialiseJoys()
{
	PSGLOBAL(joy1id) = 0;
	ControlsManager.InitDefaultControlConfigJoyPad(16);
}

void psPostRWinit(void)
{
	_InputInitialiseJoys();
	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);
}

RwBool _psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	return TRUE;
}

void InitialiseLanguage()
{
	// The PSP system language could be used here, English for now
	CGame::nastyGame = true;
	FrontEndMenuManager.m_PrefsAllowNastyGame = true;
	CGame::noProstitutes = false;
	FrontEndMenuManager.OS_Language = LANG_ENGLISH;
	FrontEndMenuManager.m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;

	TheText.Unload();
	TheText.Load();
}

void HandleExit()
{
	if(exitRequested)
		RsGlobal.quit = TRUE;
}

/*
 *****************************************************************************
 * Input
 *
 * The PSP buttons are fed to the game like a PS2 pad (GLFW gamepad layout),
 * arranged like Vice City Stories on the PSP:
 *   Cross/Circle/Square/Triangle -> same buttons
 *   R                            -> R1 (target / handbrake)
 *   L                            -> L1 on foot (camera behind the player)
 *   On foot: d-pad left / right weapon (L2 / R2), down crouch (L3),
 *     L + R look behind (R3), L + stick turns the camera (right stick)
 *   Vehicle: L + stick left / right / down looks left / right / behind,
 *     d-pad left / right radio, down horn (L3), up sub-mission (R3);
 *     helicopters turn with L / R
 *   Select / Start               -> Select (camera) / Start
 * In the menus the d-pad stays a d-pad.
 */

RwV2d leftStickPos;
RwV2d rightStickPos;
static float pspTurretX;
int pspAimDpadX, pspAimDpadY;	// arrows while aiming (Cam.cpp)
int pspFreeAimReq;	// R + down pressed this frame (PlayerPed.cpp)
static eWeaponType pspTestHudGun = WEAPONTYPE_M4;	// testing aid "hud" ("pistol": a pistol)
bool pspOrbitCam;	// L + stick on foot: the third person camera turned around Tommy (Cam.cpp)
static int pspStickOverride;	// 1: the stick looks around in a vehicle, 2: it turns the camera on foot
char pspTestDriveModelName[24];
int pspTestDriveSpeed = 30;	// testing aid: "drive0" = a car standing still, engine on

void CapturePad(RwInt32 padID)
{
	if(padID != 0)
		return;

	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);

	if(ControlsManager.m_bFirstCapture == false)
		memcpy(&ControlsManager.m_OldState, &ControlsManager.m_NewState, sizeof(ControlsManager.m_NewState));

	JoyState &st = ControlsManager.m_NewState;
	memset(st.buttons, 0, sizeof(st.buttons));
	memset(st.mappedButtons, 0, sizeof(st.mappedButtons));
	st.numButtons = 17;
	st.id = 0;
	st.isGamepad = true;

	uint32 b = pad.Buttons | keyScriptButtons();
	bool menu = FrontEndMenuManager.m_bMenuActive;
	{
		// testing aid: autostart.txt "cheattest" drives the cheat menu
		static int cheatTest = -1;
		if(cheatTest < 0){
			cheatTest = 0;
			FILE *f = fopen("autostart.txt", "r");
			if(f){
				char t[64] = { 0 };
				fread(t, 1, 63, f);
				fclose(f);
				if(strstr(t, "cheattest")) cheatTest = 1;
			}
		}
		if(cheatTest && !menu && FindPlayerPed()){
			static int ct;
			static const struct { int f; uint32 b; } seq[] = {
				{ 600, PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER|PSP_CTRL_SELECT },
				{ 630, PSP_CTRL_CROSS },		// health + armour
				{ 640, PSP_CTRL_DOWN }, { 650, PSP_CTRL_CROSS },	// wanted level
				{ 660, PSP_CTRL_DOWN }, { 690, PSP_CTRL_RIGHT }, { 720, PSP_CTRL_CROSS },	// heavy weapons
				{ 750, PSP_CTRL_DOWN }, { 780, PSP_CTRL_RIGHT }, { 810, PSP_CTRL_CROSS },	// cheetah
				{ 900, PSP_CTRL_RIGHT }, { 930, PSP_CTRL_RIGHT }, { 960, PSP_CTRL_RIGHT }, { 990, PSP_CTRL_RIGHT },
				{ 1020, PSP_CTRL_RIGHT }, { 1050, PSP_CTRL_RIGHT }, { 1080, PSP_CTRL_CROSS },	// pcj-600 (bike)
				{ 0, 0 } };
			ct++;
			for(int i = 0; seq[i].f; i++)
				if(ct == seq[i].f) b |= seq[i].b;
		}
	}
	{
		// testing aid: autostart.txt "menutest" opens the map, sets a waypoint
		// and toggles the legend
		static int menuTest = -1;
		if(menuTest < 0){
			menuTest = 0;
			FILE *f = fopen("autostart.txt", "r");
			if(f){
				char t[64] = { 0 };
				fread(t, 1, 63, f);
				fclose(f);
				if(strstr(t, "menutest")) menuTest = 1;
				if(strstr(t, "quittest")) menuTest = 2;	// quit after a while
			}
		}
		static int qt;
		if(menuTest == 2 && FindPlayerPed() && gGameState == GS_PLAYING_GAME && ++qt == 600)
			RsGlobal.quit = TRUE;
		static int mt;
		if(menuTest == 1 && (menu || FindPlayerPed()) && gGameState == GS_PLAYING_GAME){
			static const struct { int f; uint32 b; } seq[] = {
				{ 900, PSP_CTRL_START }, { 960, PSP_CTRL_DOWN }, { 990, PSP_CTRL_DOWN },
				{ 1020, PSP_CTRL_CROSS }, { 1100, PSP_CTRL_CROSS }, { 1160, PSP_CTRL_SQUARE }, { 0, 0 } };
			mt++;
			for(int i = 0; seq[i].f; i++)
				if(mt == seq[i].f) b |= seq[i].b;
			if(mt == 1101 || mt == 1161)
				debug("MENUTEST frame %d menu %d screen %d legends %d waypoint %d\n", mt, FrontEndMenuManager.m_bMenuActive,
					FrontEndMenuManager.m_nCurrScreen, FrontEndMenuManager.m_PrefsShowLegends, CRadar::TargetMarkerId);
		}
	}
	// cheat menu (L + R + Select): while it's open the game gets no input
	extern bool pspCheatMenuInput(uint32 buttons);
	if(pspCheatMenuInput(b)){
		b = 0;
		pad.Lx = pad.Ly = 128;
	}
	if(pspTestHoldR) b |= PSP_CTRL_RTRIGGER;	// testing aid "sniper"
	if(pspTestHoldRight) b |= PSP_CTRL_RIGHT;
	if(pspTestHoldX && gGameState == GS_PLAYING_GAME && FindPlayerVehicle()) b |= PSP_CTRL_CROSS;	// testing aid "holdx": accelerate
	if(pspTestOrbit > 0){ pspTestOrbit--; b |= PSP_CTRL_LTRIGGER; pad.Lx = 255; }	// testing aid key 'O'
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_A] = !!(b & PSP_CTRL_CROSS);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_B] = !!(b & PSP_CTRL_CIRCLE);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_X] = !!(b & PSP_CTRL_SQUARE);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_Y] = !!(b & PSP_CTRL_TRIANGLE);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = !!(b & PSP_CTRL_LTRIGGER);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = !!(b & PSP_CTRL_RTRIGGER);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_BACK] = !!(b & PSP_CTRL_SELECT);
	st.mappedButtons[GLFW_GAMEPAD_BUTTON_START] = !!(b & PSP_CTRL_START);
	if(menu){
		st.mappedButtons[GLFW_GAMEPAD_BUTTON_DPAD_UP] = !!(b & PSP_CTRL_UP);
		st.mappedButtons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] = !!(b & PSP_CTRL_DOWN);
		st.mappedButtons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = !!(b & PSP_CTRL_LEFT);
		st.mappedButtons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = !!(b & PSP_CTRL_RIGHT);
	}else{
		static uint32 prevPad;
		uint32 arrows = b & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
		uint32 arrowsPressed = arrows & ~prevPad;
		CPlayerPed *player = FindPlayerPed();
		// the vehicle being driven: the player's own, or a remote controlled
		// toy (RC helicopter, plane, car) the player isn't sitting in
		CVehicle *ctrlVeh = CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle;
		if(ctrlVeh == nil && player && player->InVehicle())
			ctrlVeh = player->m_pMyVehicle;
		bool inVehicle = ctrlVeh != nil;
		float sx = ((int)pad.Lx - 128) / 128.0f, sy = ((int)pad.Ly - 128) / 128.0f;
		bool lHeld = !!(b & PSP_CTRL_LTRIGGER), rHeld = !!(b & PSP_CTRL_RTRIGGER);
		if(inVehicle)
			pspOrbitCam = false;
		if(inVehicle){
			// Vice City Stories layout. L only works together with the
			// stick: L + left / right looks left / right, L + down looks
			// behind (the game's L2 / R2), and the stick doesn't steer then
			st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = 0;
			CVehicle *veh = ctrlVeh;
			bool heli = veh->GetVehicleAppearance() == VEHICLE_APPEARANCE_HELI ||
			   veh->GetModelIndex() == MI_RCGOBLIN || veh->GetModelIndex() == MI_RCRAIDER;
			if(heli){
				// helicopters: L turns left, R turns right (the game turns
				// them with L2 / R2; R's handbrake does nothing in the air)
				// Circle fires the gun (Sea Sparrow, Sparrow, Hunter; the game's handbrake)
				st.mappedButtons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = !!(b & PSP_CTRL_CIRCLE);
				st.mappedButtons[15] = lHeld;
				st.mappedButtons[16] = rHeld;
			}else if(lHeld){
				if(sy > 0.5f){
					st.mappedButtons[15] = st.mappedButtons[16] = 1;	// look behind
					pspStickOverride = 1;
				}else if(sx < -0.5f){
					st.mappedButtons[15] = 1;	// look left
					pspStickOverride = 1;
				}else if(sx > 0.5f){
					st.mappedButtons[16] = 1;	// look right
					pspStickOverride = 1;
				}
			}
			int mi = veh->GetModelIndex();
			if(mi == MI_RHINO || mi == MI_FIRETRUCK){
				// tank / fire truck: d-pad left / right turns the turret
				// (the game's right stick)
				if(b & PSP_CTRL_LEFT) pspTurretX = -1.0f;
				if(b & PSP_CTRL_RIGHT) pspTurretX = 1.0f;
			}else{
				// d-pad left / right: previous / next radio station
				if(arrowsPressed & PSP_CTRL_RIGHT) pspRadioStep = 1;
				if(arrowsPressed & PSP_CTRL_LEFT) pspRadioStep = -1;
			}
			st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] = !!(b & PSP_CTRL_DOWN);	// horn / siren
			st.mappedButtons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] = !!(b & PSP_CTRL_UP);	// sub-mission
		}else{
			// on foot (Vice City Stories layout): Select changes the camera
			int camMode = TheCamera.Cams[TheCamera.ActiveCam].Mode;
			bool aiming = camMode == CCam::MODE_SNIPER || camMode == CCam::MODE_M16_1STPERSON ||
				camMode == CCam::MODE_ROCKETLAUNCHER || camMode == CCam::MODE_CAMERA ||
				camMode == CCam::MODE_SNIPER_RUNABOUT || camMode == CCam::MODE_ROCKETLAUNCHER_RUNABOUT ||
				camMode == CCam::MODE_M16_1STPERSON_RUNABOUT || camMode == CCam::MODE_HELICANNON_1STPERSON;
			if(aiming){
				// sniper rifle, rifles, rocket launcher, camera: the arrows move
				// the aim slowly for a precise shot (Cam.cpp)
				pspAimDpadX = (b & PSP_CTRL_RIGHT) ? 1 : (b & PSP_CTRL_LEFT) ? -1 : 0;
				pspAimDpadY = (b & PSP_CTRL_UP) ? 1 : (b & PSP_CTRL_DOWN) ? -1 : 0;
			}else{
				pspAimDpadX = pspAimDpadY = 0;
				st.mappedButtons[15] = !!(b & PSP_CTRL_LEFT);	// L2: previous weapon / target
				st.mappedButtons[16] = !!(b & PSP_CTRL_RIGHT);	// R2: next weapon / target
				// R + down: free aim (VCS), first person with pistols and SMGs (PlayerPed.cpp)
				pspFreeAimReq = rHeld && (arrowsPressed & PSP_CTRL_DOWN);
				if(!rHeld)
					st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] = !!(b & PSP_CTRL_DOWN);	// crouch
			}
			if(lHeld && rHeld){
				// L + R: look behind
				st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = 0;
				st.mappedButtons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = 0;
				st.mappedButtons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] = 1;
			}else if(lHeld && (Abs(sx) > 0.3f || Abs(sy) > 0.3f)){
				// L + stick: turn the camera around Tommy (the player stands
				// still), third person like VCS; the camera stays where it was
				// put until Tommy walks off
				st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = 0;
				pspStickOverride = 2;
				pspOrbitCam = true;
			}
			// the turned camera stays (also walking) until L is pressed on
			// its own: a press and release without the stick
			static bool lDown, lUsedStick;
			if(lHeld){
				if(!lDown){
					lDown = true;
					lUsedStick = false;
				}
				if(Abs(sx) > 0.3f || Abs(sy) > 0.3f)
					lUsedStick = true;
				// L still held after turning: the game's L (camera behind
				// Tommy) must not bring the camera back
				if(lUsedStick)
					st.mappedButtons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = 0;
			}else if(lDown){
				lDown = false;
				if(!lUsedStick)
					pspOrbitCam = false;
			}
		}
		prevPad = b;
	}

	if(ControlsManager.m_bFirstCapture == true){
		memcpy(&ControlsManager.m_OldState, &ControlsManager.m_NewState, sizeof(ControlsManager.m_NewState));
		ControlsManager.m_bFirstCapture = false;
	}

	RsPadButtonStatus bs;
	bs.padID = 0;
	RsPadEventHandler(rsPADBUTTONUP, (void *)&bs);

	leftStickPos.x = ((int)pad.Lx - 128) / 128.0f;
	leftStickPos.y = ((int)pad.Ly - 128) / 128.0f;
	rightStickPos.x = 0.0f;
	rightStickPos.y = 0.0f;
	if(pspStickOverride == 2){
		rightStickPos = leftStickPos;
		leftStickPos.x = leftStickPos.y = 0.0f;
	}else if(pspStickOverride == 1)
		leftStickPos.x = leftStickPos.y = 0.0f;
	if(pspTurretX != 0.0f)
		rightStickPos.x = pspTurretX;
	pspStickOverride = 0;
	pspTurretX = 0.0f;

	if(CPad::m_bMapPadOneToPadTwo)
		bs.padID = 1;
	RsPadEventHandler(rsPADBUTTONUP, (void *)&bs);
	RsPadEventHandler(rsPADBUTTONDOWN, (void *)&bs);

	// testing aid: autostart.txt containing "walk" makes the player run around
	static int testWalk = -1;
	static float testView[4];
	static int testViewWalk, testViewDrive, testArea, testHud, testHour = -1, testSkid, testSniper;
	static float testGps[2]; static int testGpsColour = 5;
	static int testGpsMission, testGpsDone;
	if(testWalk < 0){
		testWalk = 0;
		FILE *f = fopen("autostart.txt", "r");
		if(f){
			char buf[64] = { 0 };
			fread(buf, 1, 63, f);
			fclose(f);
			if(strstr(buf, "walk")) testWalk = 1;
			if(strncmp(buf, "savetest", 8) == 0) testWalk = 2;
			if(strstr(buf, "savemenu")) testWalk = 4;	// the safehouse save screen
#ifndef N3DS
			if(strstr(buf, "saveauto")){ extern int pspSaveDialogAuto; pspSaveDialogAuto = 1; }
#endif
			if(strstr(buf, "autosave")){ extern int pspAutosaveTest; pspAutosaveTest = 1; }
			if(strstr(buf, "audiolog")) pspAudioLog = 1;
			if(strstr(buf, "helptest")) testWalk = 5;	// a help text with every kind of button
			if(strstr(buf, "holdx")){ pspTestHoldX = 1; pspTestPlayerDrives = 1; }
			const char *v = strstr(buf, "view");
			if(v){
				int n = sscanf(v+4, "%f %f %f %f", &testView[0], &testView[1], &testView[2], &testView[3]);
				if(n >= 3) testWalk = 3;
				if(strstr(buf, "walk")) testViewWalk = 1;
				if(strstr(buf, "drive")) testViewDrive = 1;
				if(strstr(buf, "drive0")) pspTestDriveSpeed = 0;
				const char *vm = strstr(buf, "veh=");
				if(vm) sscanf(vm + 4, "%23s", pspTestDriveModelName);	// "veh=mrwhoop": that vehicle
				if(strstr(buf, "hud")) testHud = 1;	// a gun, armour and two stars
				if(strstr(buf, "pistol")) pspTestHudGun = WEAPONTYPE_COLT45;
				const char *gp = strstr(buf, "gps");
				if(gp){
					sscanf(gp+3, "%f %f %d", &testGps[0], &testGps[1], &testGpsColour);
					testGpsMission = gp > buf && gp[-1] == 'm';	// "mgps x y"
				}
				if(strstr(buf, "skid")) testSkid = 1;
				if(strstr(buf, "smoke")) testSkid = 2;
				if(strstr(buf, "sniper")) testSniper = 1;	// aim with the sniper rifle, push the stick
				const char *fx = strstr(buf, "fx");
				if(fx) rw::psp::fxTest = atoi(fx+2);
				const char *hr = strstr(buf, "hour");
				if(hr) testHour = atoi(hr+4);
				const char *ar = strstr(buf, "area");
				if(ar) testArea = atoi(ar+4);
				if(n < 4) testView[3] = -1000.0f;
			}
		}
	}
	{
		extern void pspTourTick(void);
		if(!menu) pspTourTick();
		extern void pspAutosaveTick(void);
		if(!menu) pspAutosaveTick();
	}
	{
		// testing aid: autostart.txt "mission N" starts mission N (the
		// debug menu's list: 3 = The Party) ten seconds into the game
		static int missionNum = -2, frames, missionAt = 300;
		if(missionNum == -2){
			missionNum = -1;
			FILE *f = fopen("autostart.txt", "r");
			if(f){
				char b[64] = { 0 };
				fread(b, 1, 63, f);
				fclose(f);
				const char *m = strstr(b, "mission");
				if(m){
					int at = 300;
					sscanf(m + 7, "%d %d", &missionNum, &at);
					missionAt = at;
				}
			}
		}
		if(missionNum >= 0 && !menu && ++frames == missionAt){
			debug("TEST starting mission %d\n", missionNum);
			CTheScripts::SwitchToMission(missionNum);
			missionNum = -1;
		}
	}
	if(testWalk == 3 && !menu){
		// testing aid: "view x y heading" puts the player there once the intro is over
		static int done, calm;
		CPlayerPed *pl = FindPlayerPed();
		if(!done && pl && !CCutsceneMgr::IsRunning() && ++calm > 300){
			bool found;
			CVector tp(testView[0], testView[1], testView[3] > -999.0f ? testView[3] : 20.0f);
			// a save made in a safehouse starts in an interior; "area N" picks one
			CGame::currArea = testArea;
			pl->m_area = testArea;
			CStreaming::RemoveBuildingsNotInArea(testArea);
			CStreaming::LoadScene(tp);	// world and collision around the new place first
			float z = testView[3] > -999.0f ? testView[3] :
				CWorld::FindGroundZFor3DCoord(testView[0], testView[1], 100.0f, &found);
			pl->Teleport(CVector(testView[0], testView[1], z + 1.0f));
			pl->m_fRotationCur = pl->m_fRotationDest = DEGTORAD(testView[2]);
			pl->SetHeading(DEGTORAD(testView[2]));
			TheCamera.SetCameraDirectlyBehindForFollowPed_CamOnAString();
			CClock::SetGameClock(testHour >= 0 ? testHour : 13, 0);
			debug("VIEW teleported to %.0f %.0f %.1f\n", testView[0], testView[1], z);
			if(testHud){
				CStreaming::RequestModel(CWeaponInfo::GetWeaponInfo(pspTestHudGun)->m_nModelId, STREAMFLAGS_DONT_REMOVE);
				CStreaming::LoadAllRequestedModels(false);
				pl->GiveWeapon(pspTestHudGun, 300);
				pl->SetCurrentWeapon(pspTestHudGun);
				pl->m_fArmour = 100.0f;
				pl->m_pWanted->SetWantedLevel(2);
			}
			done = 1;
		}
		// keep it sunny midday for comparable screenshots and timings
		if(done && (CTimer::GetFrameCounter() % 300) == 0){
			CClock::SetGameClock(testHour >= 0 ? testHour : 12, 0);
			CWeather::ForceWeatherNow(WEATHER_SUNNY);
		}
	}
	if(testWalk == 5 && !menu && FindPlayerPed()){
		static uint32 tht;
		if(++tht == 700){
			static wchar w[200];
			AsciiToUnicode("Press ~k~~VEHICLE_ENTER_EXIT~ to get in. ~k~~PED_FIREWEAPON~ fires, ~k~~PED_SPRINT~ runs, "
				"~k~~PED_JUMPING~ jumps, ~k~~PED_LOCK_TARGET~ aims, ~k~~PED_CYCLE_WEAPON_RIGHT~ next weapon, "
				"~k~~PED_DUCK~ crouch, ~k~~GO_FORWARD~ walk.", w);
			CHud::SetHelpMessage(w, false);
		}
	}
	if(testWalk == 4 && !menu){
		// testing aid: open the save screen like a safehouse does
		static uint32 tsm;
		if(++tsm == 300){
			debug("SAVEMENU open\n");
			FrontEndMenuManager.m_bActivateSaveMenu = true;
		}
		if(tsm == 600 || tsm == 900)
			debug("SAVEMENU back in the game, menu active %d\n", FrontEndMenuManager.m_bMenuActive);
	}
	if(testWalk == 2 && !menu){
		// testing aid: save into slot 8 after a while, exercises the save code
		static uint32 ts;
		if(++ts == 300){
			debug("SAVETEST start\n");
			int r = PcSaveHelper.SaveSlot(7);
			debug("SAVETEST done %d\n", r);
		}
	}
	if(testWalk == 3 && !menu){
		// testing aid: state of a few models around the view point
		static uint32 tc;
		if(tc == 1200){
			// load and instance the car park models directly (crash hunt)
			static const char *load[] = { "washbuild013", "washbuild114", "washbuild039", "washbuild043",
				"washbuild210", "washbuild041", "wshcrprtstuf1", "wshcrprtstuf2", "washcrprkdirt1",
				"wshcarprksign", "wshprkbarrier2", "ticketbooth2", "wshbuildws23", "wshbuildws02", nil };
			for(int i = 0; load[i]; i++){
				int id;
				if(CModelInfo::GetModelInfo(load[i], &id))
					CStreaming::RequestModel(id, STREAMFLAGS_DONT_REMOVE);
			}
			CStreaming::LoadAllRequestedModels(false);
			for(int i = 0; load[i]; i++){
				int id;
				CBaseModelInfo *mi = CModelInfo::GetModelInfo(load[i], &id);
				if(mi == nil || mi->GetModelType() != MITYPE_SIMPLE) continue;
				CSimpleModelInfo *smi = (CSimpleModelInfo*)mi;
				for(int j = 0; j < smi->m_numAtomics; j++){
					RpAtomic *a = smi->m_atomics[j];
					if(a == nil) continue;
					debug("INSTANCE %s atomic %d verts %d tris %d\n", load[i], j, a->geometry->numVertices, a->geometry->numTriangles);
					pspLogFlush();
					a->getPipeline()->instance(a);
					debug("INSTANCE %s done inst %p\n", load[i], a->geometry->instData);
				}
			}
		}
		if(tc == 1400 || tc == 2400){
			int n = CPools::GetPedPool()->GetSize();
			for(int i = 0; i < n; i++){
				CPed *pd = CPools::GetPedPool()->GetSlot(i);
				if(pd == nil) continue;
				debug("PED %d model %d by %d state %d wait %d area %d pos %d %d %d\n", i, pd->GetModelIndex(), pd->CharCreatedBy,
					pd->m_nPedState, pd->m_nWaitState, pd->m_area, (int)pd->GetPosition().x, (int)pd->GetPosition().y, (int)pd->GetPosition().z);
			}
		}
		if(++tc == 1500 || tc == 2500){
			static const char *names[] = { "washbuild013", "washbuild114", "washbuild039", "washbuild043", "washbuild210", "washbuild041", nil };
			for(int i = 0; names[i]; i++){
				int id;
				CBaseModelInfo *mi = CModelInfo::GetModelInfo(names[i], &id);
				if(mi == nil){ debug("MODEL %s not found\n", names[i]); continue; }
				RpAtomic *a = mi->GetModelType() == MITYPE_SIMPLE ? ((CSimpleModelInfo*)mi)->GetAtomicFromDistance(0.0f) : nil;
				rw::Geometry *g = a ? a->geometry : nil;
				debug("MODEL %s id %d state %d refs %d atomic %p geo verts %d tris %d inst %p lod dist %d radius %d\n", names[i], id,
					CStreaming::ms_aInfoForModel[id].m_loadState, mi->GetNumRefs(), a,
					g ? g->numVertices : -1, g ? g->numTriangles : -1, g ? g->instData : nil,
					mi->GetModelType() == MITYPE_SIMPLE ? (int)((CSimpleModelInfo*)mi)->GetLargestLodDistance() : -1,
					mi->GetColModel() ? (int)mi->GetColModel()->boundingSphere.radius : -1);
			}
		}
	}
	if(testWalk == 3 && testSkid == 2 && !menu){
		// testing aid: steam and smoke in front of the player
		CPlayerPed *pl = FindPlayerPed();
		if(pl && (pl->GetPosition() - CVector(testView[0], testView[1], pl->GetPosition().z)).Magnitude() < 30.0f){
			CEntity *e = pl->InVehicle() && pl->m_pMyVehicle ? (CEntity*)pl->m_pMyVehicle : (CEntity*)pl;
			CVector cf = TheCamera.GetForward(); cf.z = 0.0f; cf.Normalise();
			CVector p = TheCamera.GetPosition() + cf*9.0f;
			p.z = e->GetPosition().z;
			(void)e;
			CParticle *pp = CParticle::AddParticle(PARTICLE_STEAM_NY, p, CVector(0.0f, 0.0f, 0.02f), nil, 0.6f);
			static int logged;
			if(logged++ % 300 == 0) debug("SMOKETEST particle %p at %.0f %.0f %.0f\n", pp, p.x, p.y, p.z);
			CParticle::AddParticle(PARTICLE_EXHAUST_FUMES, p + CVector(cf.y, -cf.x, 0.0f)*2.0f, CVector(0.0f, 0.0f, 0.01f), nil, 0.3f);
			CParticle::AddParticle(PARTICLE_ENGINE_SMOKE, p - CVector(cf.y, -cf.x, 0.0f)*2.0f, CVector(0.0f, 0.0f, 0.02f), nil, 0.5f);
		}
	}
	if(testWalk == 3 && testSkid == 1 && !menu){
		// testing aid: skid marks next to the player (two wheel tracks)
		static uint32 ts;
		CPlayerPed *pl = FindPlayerPed();
		if(pl && (pl->GetPosition() - CVector(testView[0], testView[1], pl->GetPosition().z)).Magnitude() < 30.0f && ++ts > 100){
			CVector base = pl->GetPosition() + pl->GetForward()*4.0f;
			bool found;
			float gz = CWorld::FindGroundZFor3DCoord(base.x, base.y, base.z + 2.0f, &found);
			for(int w = 0; w < 2; w++){
				CVector pos = base + pl->GetRight()*(w ? 0.8f : -0.8f) + pl->GetForward()*(((ts-100)%100)*0.04f);
				pos.z = gz + 0.05f;
				bool bloody = false;
				CSkidmarks::RegisterOne(0x5000 + w + ((ts-100)/100)*2, pos, pl->GetForward().x, pl->GetForward().y, SKIDMARK_NORMAL, &bloody);
			}
		}
	}
	if(testWalk == 3 && testViewDrive && !menu){
		// after the teleport: a self-driving car with the player in it
		static uint32 td;
		extern void pspTestDrive(void);
		if(++td == 500)
			pspTestDrive();
		if(td == 520 && testGps[0] != 0.0f && !testGpsDone){
			testGpsDone = 1;
			if(testGpsMission){
				// a pretend mission with a destination blip
				if(CTheScripts::OnAMissionFlag)
					*(int32*)&CTheScripts::ScriptSpace[CTheScripts::OnAMissionFlag] = 1;
				CRadar::ChangeBlipColour(CRadar::SetCoordBlip(BLIP_COORD, CVector(testGps[0], testGps[1], 10.0f), testGpsColour, BLIP_DISPLAY_BOTH), testGpsColour);
				debug("MGPS mission flag %d onmission %d colour %d\n", CTheScripts::OnAMissionFlag, CTheScripts::IsPlayerOnAMission(), testGpsColour);
			}else
				CRadar::ToggleTargetMarker(testGps[0], testGps[1]);
		}
	}
	if(testWalk == 3 && !testViewDrive && testGpsMission && !testGpsDone && !menu){
		// "mgps x y" without "drive": just the mission blip and its marker
		static uint32 tg;
		if(++tg == 520){
			testGpsDone = 1;
			if(CTheScripts::OnAMissionFlag)
				*(int32*)&CTheScripts::ScriptSpace[CTheScripts::OnAMissionFlag] = 1;
			CRadar::ChangeBlipColour(CRadar::SetCoordBlip(BLIP_COORD, CVector(testGps[0], testGps[1], 10.0f), testGpsColour, BLIP_DISPLAY_BOTH), testGpsColour);
		}
	}
	if(testWalk == 3 && testViewWalk && !menu){
		// after the teleport: run straight ahead
		static uint32 tv;
		if(++tv > 400){
			leftStickPos.y = -1.0f;
			leftStickPos.x = 0.0f;
			st.mappedButtons[GLFW_GAMEPAD_BUTTON_A] = 1;
		}
	}
	if(testWalk == 3 && testSniper && !menu){
		// testing aid: sniper rifle, aim with R, then half / full stick right
		static uint32 ts;
		CPlayerPed *pl = FindPlayerPed();
		ts++;
		if(pl && ts == 300){
			CStreaming::RequestModel(MI_SNIPERRIFLE, STREAMFLAGS_DONT_REMOVE);
			CStreaming::LoadAllRequestedModels(false);
			pl->GiveWeapon(WEAPONTYPE_SNIPERRIFLE, 25);
			pl->SetCurrentWeapon(WEAPONTYPE_SNIPERRIFLE);
		}
		if(pl && ts > 380){
			pspTestHoldR = ts < 760;
			pspTestHoldRight = ts > 640 && ts <= 700;	// the arrow: slow aim
			if(ts > 420 && ts <= 480) leftStickPos.x = 0.5f;
			else if(ts > 520 && ts <= 580) leftStickPos.x = 1.0f;
			if(ts % 10 == 0 && ts < 760)
				debug("SNIPERTEST w %d tgt %d %u mode %d heading %.1f fov %.1f stick %.2f aim %d\n", pl->GetWeapon()->m_eWeaponType, CPad::GetPad(0)->GetTarget(), ts, TheCamera.Cams[TheCamera.ActiveCam].Mode,
					RADTODEG(TheCamera.Cams[TheCamera.ActiveCam].Beta), TheCamera.Cams[TheCamera.ActiveCam].FOV, leftStickPos.x, CPad::pspSniperAim);
		}
	}
	{
		// testing aid: autostart.txt "cutscene NAME" (with "view"): plays that
		// cutscene (camera and animations, the player model loaded) twice
		static int state = -1, runs;
		static uint32 ct;
		static char cutName[16];
		if(state == -1){
			state = 0;
			FILE *f = fopen("autostart.txt", "r");
			if(f){
				char b[128] = { 0 };
				fread(b, 1, 127, f);
				fclose(f);
				const char *c = strstr(b, "cutscene ");
				if(c && sscanf(c + 9, "%15s", cutName) == 1)
					state = 1;
				if(strstr(b, "force")){ extern int pspCutsceneForceFree; pspCutsceneForceFree = 1; }
			}
		}
		if(state > 0 && !menu && testWalk == 3){
			ct++;
			if(state == 1 && ct == 1200){
				debug("CUTTEST loading %s, %d KB free\n", cutName, pspFreeHeapKB());
				CStreaming::RequestSpecialChar(0, "csplay", STREAMFLAGS_DEPENDENCY | STREAMFLAGS_SCRIPTOWNED);
				CStreaming::LoadAllRequestedModels(false);
				CCutsceneMgr::LoadCutsceneData(cutName);
				CCutsceneMgr::ms_cutsceneLoadStatus = 1;
				state = 2;
			}else if(state == 2 && CCutsceneMgr::IsRunning() && CCutsceneMgr::HasCutsceneFinished()){
				CCutsceneMgr::DeleteCutsceneData();
				TheCamera.RestoreWithJumpCut();
				debug("CUTTEST %s finished, %d KB free\n", cutName, pspFreeHeapKB());
				state = ++runs < 2 ? 1 : 3;
				ct = 600;
			}
		}
	}
	{
		// testing aid: autostart.txt "entercar=NAME": Tommy walks to the
		// nearest such vehicle and gets in (like the player doing it)
		static int state = -1;
		static char name[24];
		static uint32 t;
		if(state == -1){
			state = 0;
			FILE *f = fopen("autostart.txt", "r");
			if(f){
				char b[128] = { 0 };
				fread(b, 1, 127, f);
				fclose(f);
				const char *c = strstr(b, "entercar=");
				if(c && sscanf(c + 9, "%23s", name) == 1)
					state = 1;
			}
		}
		if(state == 1 && !menu && ++t == 450){
			int32 id;
			CPlayerPed *pl = FindPlayerPed();
			if(pl && CModelInfo::GetModelInfo(name, &id)){
				CVehicle *best = nil;
				float bd = 1e9f;
				for(int i = 0; i < CPools::GetVehiclePool()->GetSize(); i++){
					CVehicle *v = CPools::GetVehiclePool()->GetSlot(i);
					if(v && v->GetModelIndex() == id){
						float d = (v->GetPosition() - pl->GetPosition()).Magnitude();
						if(d < bd){ bd = d; best = v; }
					}
				}
				debug("TEST entercar %s: %s %.0f m away\n", name, best ? "found" : "none", bd);
				if(best)
					pl->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, best);
			}
			state = 2;
		}
		if(state == 2 && !menu && (++t % 30) == 0 && t < 1500){
			CPlayerPed *pl = FindPlayerPed();
			debug("TEST entercar: player %p ref %x in vehicle %d state %d, handle 1 is %p\n", pl, pl ? CPools::GetPedRef(pl) : -1,
				pl ? pl->InVehicle() : -1, pl ? pl->GetPedState() : -1, CPools::GetPedPool()->GetAt(1));
		}
	}
	if(testWalk == 1 && !menu){
		static uint32 t;
		t++;
		leftStickPos.y = -1.0f;
		leftStickPos.x = ((t/300) & 1) ? 0.4f : -0.2f;
		st.mappedButtons[GLFW_GAMEPAD_BUTTON_A] = 1;	// sprint
	}

	CPad *cpad = CPad::GetPad(bs.padID);
	// the PSP nub isn't very precise, use a dead zone
	if(Abs(leftStickPos.x) > 0.25f)
		cpad->PCTempJoyState.LeftStickX = (int32)(leftStickPos.x * 128.0f);
	if(Abs(leftStickPos.y) > 0.25f)
		cpad->PCTempJoyState.LeftStickY = (int32)(leftStickPos.y * 128.0f);
	if(Abs(rightStickPos.x) > 0.25f)
		cpad->PCTempJoyState.RightStickX = (int32)(rightStickPos.x * 128.0f);
	if(Abs(rightStickPos.y) > 0.25f)
		cpad->PCTempJoyState.RightStickY = (int32)(rightStickPos.y * 128.0f);
}

void joysChangeCB(int jid, int event)
{
}

/*
 *****************************************************************************
 */

//
// Missing game files: show a system message instead of crashing
//
#ifdef N3DS
#include <3ds.h>
static void
showErrorAndExit(const char *msg)
{
	pspLog(msg);
	pspLog("\n");
	pspLogFlush();
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	printf("\x1b[2J\n  GTA Vice City\n\n");
	for(const char *p = msg; *p; p++)
		putchar(*p);
	printf("\n\n  Press START to exit.\n");
	while(aptMainLoop()){
		hidScanInput();
		if(hidKeysDown() & KEY_START)
			break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	gfxExit();
	exit(0);
}
#else
#include <psputility.h>
#include <pspgu.h>
#include <pspdisplay.h>

static unsigned int __attribute__((aligned(64))) errList[4096];

static void
showErrorAndExit(const char *msg)
{
	pspLog(msg);
	pspLog("\n");
	pspLogFlush();
	sceGuInit();
	sceGuStart(GU_DIRECT, errList);
	sceGuDrawBuffer(GU_PSM_5650, (void*)0, 512);
	sceGuDispBuffer(480, 272, (void*)(512*272*2), 512);
	sceGuDepthBuffer((void*)(512*272*4), 512);
	sceGuOffset(2048 - 240, 2048 - 136);
	sceGuViewport(2048, 2048, 480, 272);
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);

	pspUtilityMsgDialogParams params;
	memset(&params, 0, sizeof(params));
	params.base.size = sizeof(params);
	sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
	sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &params.base.buttonSwap);
	params.base.graphicsThread = 0x11;
	params.base.accessThread = 0x13;
	params.base.fontThread = 0x12;
	params.base.soundThread = 0x10;
	params.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
	params.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT;
	strncpy(params.message, msg, sizeof(params.message)-1);
	if(sceUtilityMsgDialogInitStart(&params) >= 0){
		for(;;){
			sceGuStart(GU_DIRECT, errList);
			sceGuClearColor(0xFF402010);
			sceGuClear(GU_COLOR_BUFFER_BIT);
			sceGuFinish();
			sceGuSync(0, 0);
			int st = sceUtilityMsgDialogGetStatus();
			if(st == PSP_UTILITY_DIALOG_VISIBLE)
				sceUtilityMsgDialogUpdate(1);
			else if(st == PSP_UTILITY_DIALOG_QUIT)
				sceUtilityMsgDialogShutdownStart();
			else if(st == PSP_UTILITY_DIALOG_NONE || st < 0)
				break;
			sceDisplayWaitVblankStart();
			sceGuSwapBuffers();
		}
	}
	sceKernelExitGame();
}

#endif

// How fast does the CPU really run? A loop of known length (3 cycles per
// round). Lower than 333 means something else takes the CPU: a plugin, a
// launcher that stays loaded, or the clock was refused.
static uint32
loopMHz(uint32 n)
{
	uint32 t0 = sceKernelGetSystemTimeLow();
#ifdef N3DS
	__asm__ volatile("1: subs %0, %0, #1\n bne 1b\n" : "+r"(n) :: "cc");
	(void)t0;
	return 333;	// (not measured on the 3DS)
#else
	uint32 n0 = n;
	__asm__ volatile(".set noreorder\n1: addiu %0, %0, -1\n bnez %0, 1b\n nop\n.set reorder\n" : "+r"(n));
	uint32 us = sceKernelGetSystemTimeLow() - t0;
	return us ? (uint32)((uint64)n0*3/us) : 0;
#endif
}
uint32 pspCpuMHz = 333;	// measured, every second

// ask for 333/166 every way the firmware offers; returns the measured speed
static uint32
setFullClock(const char *when)
{
#ifdef N3DS
	return 333;
#else
	int r1 = scePowerSetClockFrequency(333, 333, 166);
	uint32 mhz = loopMHz(2000000);
	int r2 = 0, r3 = 0;
	if(mhz < 280){
		r2 = scePowerSetCpuClockFrequency(333);
		r3 = scePowerSetBusClockFrequency(166);
		sceKernelDelayThread(1000);
		mhz = loopMHz(2000000);
	}
	char b[200];
	snprintf(b, sizeof(b), "clock (%s): set %x/%x/%x, reported cpu %d bus %d, measured %u MHz, battery %d%%%s%s\n", when,
		r1, r2, r3, scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), (unsigned)mhz,
		scePowerGetBatteryLifePercent(), scePowerIsLowBattery() == 1 ? " LOW" : "", scePowerIsPowerOnline() == 1 ? ", charger" : "");
	pspLog(b);
	return mhz;
#endif
}

// the other threads and modules that share the PSP with the game (log)
static void
logSystemLoad(void)
{
#ifndef N3DS
	static SceUID ids[64];
	int n = 0;
	if(sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &n) >= 0){
		char b[160];
		for(int i = 0; i < n && i < 64; i++){
			SceKernelThreadInfo ti;
			memset(&ti, 0, sizeof(ti));
			ti.size = sizeof(ti);
			if(sceKernelReferThreadStatus(ids[i], &ti) < 0) continue;
			snprintf(b, sizeof(b), "  thread %s priority %x status %x ran %u ms\n", ti.name, ti.currentPriority, ti.status,
				(unsigned)(ti.runClocks.low/1000));
			pspLog(b);
		}
	}
	n = 0;
	if(sceKernelGetModuleIdList(ids, sizeof(ids), &n) >= 0){
		char b[160];
		for(int i = 0; i < n && i < 64; i++){
			SceKernelModuleInfo mi;
			memset(&mi, 0, sizeof(mi));
			mi.size = sizeof(mi);
			if(sceKernelQueryModuleInfo(ids[i], &mi) < 0) continue;
			snprintf(b, sizeof(b), "  module %s\n", mi.name);
			pspLog(b);
		}
	}
#endif
}

// the files the game can't start without
static void
checkGameFiles(void)
{
	static const char *required[] = {
		"TEXT/AMERICAN.GXT", "DATA/GTA_VC.DAT", "DATA/DEFAULT.DAT", "ANIM/PED.IFP",
		"MODELS/GTA3.IMG", "MODELS/GTA3.DIR", "MODELS/FONTS.TXD", "MODELS/GENERIC.TXD",
		"MODELS/HUD.TXD", "MODELS/PARTICLE.TXD", "MODELS/COLL/PEDS.COL",
		nil
	};
	char missing[400] = "";
	int n = 0;
	for(int i = 0; required[i]; i++){
		SceUID fd = sceIoOpen(required[i], PSP_O_RDONLY, 0);
		if(fd >= 0){
			sceIoClose(fd);
			continue;
		}
		if(n < 6){
			strcat(missing, "\n  ");
			strcat(missing, required[i]);
		}
		n++;
	}
	if(n == 0)
		return;
	char msg[512];
	snprintf(msg, sizeof(msg),
		"Game files are missing from this folder:%s%s\n\n"
		"Copy the data, anim, TEXT and models folders of GTA Vice City "
		"(and the pspconv output) next to EBOOT.PBP.",
		missing, n > 6 ? "\n  ..." : "");
	showErrorAndExit(msg);
}

#ifndef N3DS
// Where is the CPU fetching our code from? 0x08.. is the normal (cached)
// address of the PSP's RAM, 0x48.. the same RAM without the instruction
// cache: code runs there at about half speed (every instruction waits for
// the memory bus). Some loaders start a program's main thread there; all
// the calls after that stay in the same window.
static uint32 __attribute__((noinline))
currentPC(void)
{
	uint32 pc;
	__asm__ volatile(".set noreorder\n bal 1f\n nop\n1: move %0, $ra\n.set reorder\n" : "=r"(pc) :: "ra");
	return pc;
}
static bool uncachedPC(uint32 pc) { return (pc & 0xF0000000) == 0x40000000 || (pc & 0xF0000000) == 0xA0000000; }
static uint32 cachedAddr(uint32 a) { return (a & 0xF0000000) == 0x40000000 ? a & ~0x40000000u : (a & 0xF0000000) == 0xA0000000 ? a & ~0x20000000u : a; }

// threads the game creates start at the cached address too
extern "C" SceUID __real_sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority, int stackSize, SceUInt attr, SceKernelThreadOptParam *option);
extern "C" SceUID
__wrap_sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority, int stackSize, SceUInt attr, SceKernelThreadOptParam *option)
{
	return __real_sceKernelCreateThread(name, (SceKernelThreadEntry)cachedAddr((uint32)entry), initPriority, stackSize, attr, option);
}

static int realMain(int argc, char *argv[]);
static int gMainArgc;
static char **gMainArgv;
static uint32 gStartPC;
static int
cachedMainThread(SceSize, void*)
{
	int r = realMain(gMainArgc, gMainArgv);
	sceKernelExitGame();
	return r;
}

int
main(int argc, char *argv[])
{
	gStartPC = currentPC();
	if(uncachedPC(gStartPC)){
		// start over in a thread at the cached address
		gMainArgc = argc;
		gMainArgv = argv;
		SceUID th = __real_sceKernelCreateThread("user_main_cached", (SceKernelThreadEntry)cachedAddr((uint32)cachedMainThread),
			0x20, 512*1024, THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
		if(th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0){
			sceKernelWaitThreadEnd(th, NULL);
			sceKernelExitGame();
			return 0;
		}
	}
	return realMain(argc, argv);
}

static int
realMain(int argc, char *argv[])
#else
int
main(int argc, char *argv[])
#endif
{
	RwV2d pos;
	RwInt32 i;
	pspMainThread = sceKernelGetThreadId();

#ifdef N3DS
	extern void n3dsStartup(void);
	n3dsStartup();
#endif
	{
		// The Allegrex FPU can trap on invalid operations (0/0, sqrt(-1)...),
		// which the PC (and PPSSPP) silently ignore. Turn the traps off and
		// flush denormals like the PS2 did.
		char b[96];
		snprintf(b, sizeof(b), "FCR31 at start %08x\n", (unsigned)pspFpuGetFCR31());
		pspLog(b);
		pspFpuSetEnable(0);
		pspFpuSetFCR31(pspFpuGetFCR31() | PSP_FPU_FS_MASK);
	}
	initStackWatermark();
	{
		// how big can the heap get on this PSP? (probe, then free)
		int kb = 0;
		for(int sz = 64*1024; sz >= 1024; sz -= 256){
			void *p = __real_malloc(sz*1024);	// not the guarded malloc
			if(p){ free(p); kb = sz; break; }
		}
		pspHeapTotalKB = kb;
		if(pspHeapLimitKB > 0 && pspHeapLimitKB < kb)
			pspHeapTotalKB = pspHeapLimitKB;
		emergencyReserve = __real_malloc(RESERVE_SIZE);
		char b[128];
		snprintf(b, sizeof(b), "reVC PSP start: largest heap block %d KB, kernel free %d KB\n",
			kb, (int)(sceKernelTotalFreeMemSize()/1024));
		pspLog(b);
	}
	pspLoadTune();
	setupCallbacks();
	startWatchdog();
#ifndef N3DS
	if(pspLogOn())	// testing only, like the log
		startCrashCatcher();
#endif
	checkGameFiles();
#if !defined(N3DS) && !defined(PSP_LITE)
	// The PSP-2000/3000 version needs the slim's extra 32 MB; without it the
	// loading runs out of memory and the PSP crashes. Say so instead.
	if(pspHeapTotalKB > 0 && pspHeapTotalKB < 30*1024){
		char msg[400];
		snprintf(msg, sizeof(msg),
			"Only %d MB of memory is free for the game (it needs about 40 MB).\n\n"
			"The PSP's extra memory is turned off: in your CFW settings turn on "
			"\"Extra RAM\" (or \"Force extra memory\") for homebrew, restart the PSP "
			"and start the game again.\n\n"
			"A PSP-1000 needs the PSP-1000 version of the game.", pspHeapTotalKB/1024);
		showErrorAndExit(msg);
	}
#endif
	{
		extern void pspSavedataSync(void);
		pspSavedataSync();
	}
	int clockRes;
	if(pspTestCpuMhz >= 20 && pspTestCpuMhz < 333)
		clockRes = scePowerSetClockFrequency(pspTestCpuMhz, pspTestCpuMhz, pspTestCpuMhz/2);
	else
		clockRes = scePowerSetClockFrequency(333, 333, 166);
	{
		// is the CPU really at 333 MHz? (a plugin or the firmware can refuse)
		uint32 mhz = loopMHz(20000000);
		char b[200];
		snprintf(b, sizeof(b), "clock: set %08x, cpu %d MHz, bus %d MHz, loop benchmark %u MHz\n", clockRes,
			scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), (unsigned)mhz);
		pspLog(b);
#ifndef N3DS
		{
			uint32 pc = currentPC();
			snprintf(b, sizeof(b), "code: started at %08x, now running at %08x, functions at %08x%s\n", (unsigned)gStartPC, (unsigned)pc,
				(unsigned)(uint32)&loopMHz, uncachedPC(gStartPC) ? " (moved off the uncached window)" : "");
			pspLog(b);
		}
		if(mhz < 280){
			// The same loop with interrupts off (a few ms): if it's fast then,
			// the missing time goes to interrupt handlers (kernel plugins hook
			// the vblank, timer and other interrupts), not to the clock.
			unsigned int st = sceKernelCpuSuspendIntr();
			uint32 offMhz = loopMHz(2000000);
			sceKernelCpuResumeIntr(st);
			// and the fastest of many very short runs (between interrupts)
			uint32 best = 0;
			for(int k = 0; k < 400; k++){
				uint32 m = loopMHz(3000);
				if(m > best) best = m;
			}
			snprintf(b, sizeof(b), "clock: interrupts off %u MHz, best short run %u MHz -> %s\n", (unsigned)offMhz, (unsigned)best,
				offMhz > 280 ? "interrupt handlers take the missing time (a plugin?)" : "the CPU itself runs slow");
			pspLog(b);
			// which clock request does the firmware really follow?
			static const int tries[][3] = { { 222, 222, 111 }, { 333, 333, 166 }, { 333, 333, 111 }, { 300, 300, 150 }, { 333, 333, 166 } };
			for(int t = 0; t < 5; t++){
				int r = scePowerSetClockFrequency(tries[t][0], tries[t][1], tries[t][2]);
				sceKernelDelayThread(20000);
				uint32 m = loopMHz(4000000);
				snprintf(b, sizeof(b), "clock: asked %d/%d/%d -> %x, reported %d/%d, measured %u MHz\n", tries[t][0], tries[t][1], tries[t][2],
					r, scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), (unsigned)m);
				pspLog(b);
			}
			mhz = loopMHz(20000000);
		}
		if(mhz < 280){
			pspLog("clock: the CPU is much slower than it should be, something else is using it:\n");
			logSystemLoad();
			sceKernelDelayThread(200000);
			mhz = setFullClock("start, again");
		}
#endif
		pspCpuMHz = mhz;
	}
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR)
		return FALSE;

	for(i = 1; i < argc; i++)
		RsEventHandler(rsPREINITCOMMANDLINE, argv[i]);

	openParams.width = RsGlobal.maximumWidth;
	openParams.height = RsGlobal.maximumHeight;
	openParams.windowtitle = RsGlobal.appName;

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	if(rsEVENTERROR == RsEventHandler(rsRWINITIALIZE, &openParams)){
		pspLog("RenderWare initialisation failed\n");
		RsEventHandler(rsTERMINATE, nil);
		sceKernelExitGame();
		return 0;
	}

	psPostRWinit();
	pspLog("RW initialised\n");

	ControlsManager.InitDefaultControlConfigMouse(MousePointerStateHelper.GetMouseSetUp());

	for(i = 1; i < argc; i++)
		RsEventHandler(rsCOMMANDLINE, argv[i]);

	{
		RwRect r;
		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;
		RsEventHandler(rsCAMERASIZE, &r);
	}

	{
		CFileMgr::SetDirMyDocuments();
#ifdef LOAD_INI_SETTINGS
		int connectedPadButtons = ControlsManager.ms_padButtonsInited;
#endif
		int32 gta3set = CFileMgr::OpenFile("gta_vc.set", "r");
		if(gta3set){
			ControlsManager.LoadSettings(gta3set);
			CFileMgr::CloseFile(gta3set);
		}
		CFileMgr::SetDir("");
#ifdef LOAD_INI_SETTINGS
		LoadINIControllerSettings();
		if(connectedPadButtons != 0)
			ControlsManager.InitDefaultControlConfigJoyPad(connectedPadButtons);
		SaveINISettings();
		SaveINIControllerSettings();
#endif
	}

	while(TRUE){
		RwInitialised = TRUE;

		pos.x = RsGlobal.maximumWidth * 0.5f;
		pos.y = RsGlobal.maximumHeight * 0.5f;
		RsMouseSetPos(&pos);

		while(!RsGlobal.quit && !FrontEndMenuManager.m_bWantToRestart){
			HandleExit();
#ifdef N3DS
			// HOME menu / power button / closing the game from the system
			extern bool n3dsMainLoop(void);
			if(!n3dsMainLoop()){
				pspLog("3ds: system asked us to quit\n");
				RsGlobal.quit = TRUE;
				break;
			}
#endif
			if(ForegroundApp){
				switch(gGameState){
				case GS_START_UP:
					gGameState = GS_INIT_ONCE;
					TRACE("gGameState = GS_INIT_ONCE");
					break;

				case GS_INIT_LOGO_MPEG:
				case GS_LOGO_MPEG:
				case GS_INIT_INTRO_MPEG:
				case GS_INTRO_MPEG:
					gGameState = GS_INIT_ONCE;
					break;

				case GS_INIT_ONCE:
					LoadingScreen(nil, nil, "loadsc0");
					if(!CGame::InitialiseOnceAfterRW())
						RsGlobal.quit = TRUE;
					gGameState = GS_INIT_FRONTEND;
					TRACE("gGameState = GS_INIT_FRONTEND;");
					break;

				case GS_INIT_FRONTEND:
				{
					// testing aid: skip the menu when autostart.txt exists
					FILE *f = fopen("autostart.txt", "r");
					if(f){
						// "load N" (optionally followed by "walk") loads save slot N
						char buf[32] = { 0 };
						fread(buf, 1, 31, f);
						fclose(f);
						int slot;
						if(strncmp(buf, "boot", 4) == 0)
							goto bootLastSave;	// testing aid: start like without autostart.txt
						if(strncmp(buf, "resume", 6) == 0){
							// testing aid: the main menu's "Resume game"
							pspTestResume = 1;
							goto normalFrontend;
						}
						if(sscanf(buf, "load %d", &slot) == 1 && slot >= 1 && slot <= 8){
							FrontEndMenuManager.m_nCurrSaveSlot = slot - 1;
							autoLoadSlot = slot;
						}
						debug("AUTOSTART '%s' load slot %d\n", buf, autoLoadSlot);
						gGameState = GS_INIT_PLAYING_GAME;
						break;
					}
				bootLastSave:
					// Like Vice City Stories: straight into the last save made
					// or loaded, no menu. Holding L while the game starts shows
					// the main menu instead.
					if(!pspTestResume){
						SceCtrlData cp;
						memset(&cp, 0, sizeof(cp));
						sceCtrlSetSamplingCycle(0);
						sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
						sceCtrlPeekBufferPositive(&cp, 1);
						PcSaveHelper.PopulateSlotInfo();
						extern int pspGetLastSlot(void);
						extern int pspNewestSaveSlot(void);
						int as = pspGetLastSlot();
						if(as < 0 || Slots[as] != SLOT_OK)
							as = pspNewestSaveSlot();
						bool menuWanted = !!(cp.Buttons & PSP_CTRL_LTRIGGER);
						debug("BOOT last save: slot %d, L %s\n", as + 1, menuWanted ? "held: menu" : "not held");
						if(as >= 0 && Slots[as] == SLOT_OK && !menuWanted){
							FrontEndMenuManager.m_nCurrSaveSlot = as;
							autoLoadSlot = as + 1;
							gGameState = GS_INIT_PLAYING_GAME;
							TRACE("gGameState = GS_INIT_PLAYING_GAME;");
							break;
						}
					}
				}
				normalFrontend:
					LoadingScreen(nil, nil, "loadsc0");
					FrontEndMenuManager.m_bGameNotLoaded = true;
					FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
					gGameState = GS_FRONTEND;
					TRACE("gGameState = GS_FRONTEND;");
					break;

				case GS_FRONTEND:
					if(pspTestResume && ++pspTestResume == 200 && FrontEndMenuManager.m_nCurrScreen == MENUPAGE_START_MENU){
						debug("TEST resume game\n");
						FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_LOADING_IN_PROGRESS);
						FrontEndMenuManager.ProcessFileActions();	// in the same frame, like the menu does
					}
					RsEventHandler(rsFRONTENDIDLE, nil);
					if(!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad){
						gGameState = GS_INIT_PLAYING_GAME;
						TRACE("gGameState = GS_INIT_PLAYING_GAME;");
					}
					if(FrontEndMenuManager.m_bWantToLoad){
						InitialiseGame();
						FrontEndMenuManager.m_bGameNotLoaded = false;
						gGameState = GS_PLAYING_GAME;
						TRACE("gGameState = GS_PLAYING_GAME;");
						pspCpuMHz = setFullClock("game start");
					}
					break;

				case GS_INIT_PLAYING_GAME:
					InitialiseGame();
					FrontEndMenuManager.m_bGameNotLoaded = false;
					if(autoLoadSlot){
						// testing aid: load the save like the menu does (restart + load)
						FrontEndMenuManager.m_nCurrSaveSlot = autoLoadSlot - 1;
						CheckSlotDataValid(autoLoadSlot - 1);	// sets the file name to load
						FrontEndMenuManager.m_bWantToLoad = true;
						FrontEndMenuManager.m_bWantToRestart = true;
						autoLoadSlot = 0;
						debug("AUTOSTART restarting to load\n");
					}
					gGameState = GS_PLAYING_GAME;
					TRACE("gGameState = GS_PLAYING_GAME;");
					pspCpuMHz = setFullClock("game start");
					break;

				case GS_PLAYING_GAME:
				{
					if(RwInitialised){
						// 30 fps on the vblank grid: a new frame every second
						// vblank (60 Hz screen), so frames are evenly spaced
						static unsigned int lastVcount;
						unsigned int v = sceDisplayGetVcount();
						if(v - lastVcount >= (pspTune.fpsTarget == 20 ? 3u : 2u)){
							lastVcount = v;
							pspFrameStartUs = sceKernelGetSystemTimeLow();
							pspFrameStartRun = pspMainRunUs();
							RsEventHandler(rsIDLE, (void *)TRUE);
						}else
							sceDisplayWaitVblankStart();	// sleep until the next vblank
					}
					break;
				}
				}
			}else{
				if(RwCameraBeginUpdate(Scene.camera)){
					RwCameraEndUpdate(Scene.camera);
					ForegroundApp = TRUE;
					RsEventHandler(rsACTIVATE, (void *)TRUE);
				}
			}
		}

		RwInitialised = FALSE;
#ifdef N3DS
		if(RsGlobal.quit){
			extern void n3dsQuit(void);
			n3dsQuit();
		}
#endif
		FrontEndMenuManager.UnloadTextures();
		if(!FrontEndMenuManager.m_bWantToRestart)
			break;

		CPad::ResetCheats();
		CPad::StopPadsShaking();
		pspStep = "restart: music off";
		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
		CTimer::Stop();

		if(FrontEndMenuManager.m_bWantToLoad){
			// (each step is written to the log at once: a save that doesn't
			// load shows where it stopped)
#ifdef N3DS
			int directBefore = pspLogDirect;
			pspLogDirect = 1;	// every line straight to the SD card while it loads
#endif
			pspStep = "restart: loading a save, shutting the game down"; pspLog("restart: loading a save, shutting the game down\n"); pspLogFlush();
			CGame::ShutDownForRestart();
			pspStep = "restart: shut down, starting again"; pspLog("restart: shut down, starting again\n"); pspLogFlush();
			CGame::InitialiseWhenRestarting();
			pspStep = "restart: started, music back on"; pspLog("restart: started, music back on\n"); pspLogFlush();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			pspStep = "restart: done"; pspLog("restart: done\n"); pspLogFlush();
#ifdef N3DS
			pspLogDirect = directBefore;
#endif
			FrontEndMenuManager.m_bWantToLoad = false;
		}else{
			if(gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();
			CTimer::Stop();
			if(FrontEndMenuManager.m_bFirstTime == true){
				gGameState = GS_INIT_FRONTEND;
				TRACE("gGameState = GS_INIT_FRONTEND;");
			}else{
				gGameState = GS_INIT_PLAYING_GAME;
				TRACE("gGameState = GS_INIT_PLAYING_GAME;");
			}
		}

		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
	}

	if(gGameState == GS_PLAYING_GAME)
		CGame::ShutDown();

	DMAudio.Terminate();
	_psFreeVideoModeList();
	RsEventHandler(rsRWTERMINATE, nil);
	RsEventHandler(rsTERMINATE, nil);

	sceKernelExitGame();
	return 0;
}

#endif

// sinf/cosf with big arguments (game timers in ms turned into angles) go
// through newlib's slow exact range reduction; reduce them here first,
// the lost precision doesn't matter for those
extern "C" float __real_sinf(float);
extern "C" float __real_cosf(float);
static inline float
pspReduceAngle(float x)
{
	if(x > 100.0f || x < -100.0f){
		const float twoPi = 6.28318530718f;
		x -= twoPi * (float)(int)(x * (1.0f/twoPi));
	}
	return x;
}
extern "C" float __wrap_sinf(float x) { return __real_sinf(pspReduceAngle(x)); }
extern "C" float __wrap_cosf(float x) { return __real_cosf(pspReduceAngle(x)); }




// the save's description in the PSP's save list, like Vice City Stories:
// island, safehouse, day and money, how far the game is
#include "Stats.h"
#include "Zones.h"
void
pspSaveDetail(char *out, int size)
{
	char place[48] = "";
	CPlayerPed *player = FindPlayerPed();
	if(player){
		CVector pos = player->GetPosition();
		CZone *z = CTheZones::FindSmallestNavigationZoneForPosition(&pos, false, true);
		if(z == nil)
			z = CTheZones::FindSmallestNavigationZoneForPosition(&pos, true, false);
		wchar *name = z ? z->GetTranslatedName() : nil;
		int n = 0;
		for(; name && name[n] && n < (int)sizeof(place)-1; n++){
			if(name[n] >= 128){	// the save list only shows plain letters
				n = 0;
				break;
			}
			place[n] = (char)name[n];
		}
		place[n] = '\0';
		// zone names are in capitals: "OCEAN BEACH" -> "Ocean Beach"
		for(int i = 1; i < n; i++)
			if(place[i-1] != ' ' && place[i] >= 'A' && place[i] <= 'Z')
				place[i] += 'a' - 'A';
	}
	snprintf(out, size, "%s.\n%s%s.\nDay %d, $%d.\n%.1f%% complete.",
		CGame::currLevel == LEVEL_MAINLAND ? "Mainland" : "Vice City Beach",
		place, place[0] ? " safehouse" : "Safehouse", (int)CStats::DaysPassed + 1,
		(int)CWorld::Players[CWorld::PlayerInFocus].m_nMoney, CStats::GetPercentageProgress());
}
