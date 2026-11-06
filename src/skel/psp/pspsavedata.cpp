// Save games in the PSP's Saved Data Utility (ms0:/PSP/SAVEDATA/REVC00001SLOTn).
//
// The game itself keeps reading and writing userfiles/GTAVCsfN.b; this file
// mirrors those into system savedata, which shows up in the XMB with an icon
// and title and can be copied/deleted there like any PSP game save:
//  - at startup, each slot is restored from SAVEDATA into userfiles
//    (a slot deleted in the XMB disappears from the game too)
//  - after the game saves a slot it is stored into SAVEDATA
//  - deleting a slot in the game deletes it from SAVEDATA
// Existing userfiles saves are moved into SAVEDATA the first time.

#if defined PSP && defined RW_PSP && !defined N3DS

#include <pspkernel.h>
#include <psputility.h>
#include <pspdisplay.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/stat.h>

#include "pspicon0.h"
#if __has_include("pspsave_art.h")
#include "pspsave_art.h"	// the game's icon and background (res/psp-art, not in the source)
#endif
namespace rw { namespace psp { void utilityDialog(int (*step)(void)); } }

extern void pspLog(const char *str);

#define SAVE_GAMENAME	"REVC00001"
#define SAVE_FILENAME	"DATA.BIN"
#define NUM_SLOTS	8
#define MAX_SAVE_SIZE	(320*1024)
#define MIGRATED_MARK	"userfiles/savedata.txt"

static void
logf(const char *fmt, int a, int b)
{
	char buf[128];
	snprintf(buf, sizeof(buf), fmt, a, b);
	pspLog(buf);
}

static void
localName(char *out, int slot)	// slot 0..7
{
	sprintf(out, "userfiles/GTAVCsf%d.b", slot+1);
}

static void
initParams(SceUtilitySavedataParam *p, int mode, int slot)
{
	memset(p, 0, sizeof(*p));
	p->base.size = sizeof(*p);
	sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &p->base.language);
	sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &p->base.buttonSwap);
	p->base.graphicsThread = 0x11;
	p->base.accessThread = 0x13;
	p->base.fontThread = 0x12;
	p->base.soundThread = 0x10;
	p->mode = (PspUtilitySavedataMode)mode;
	p->overwrite = 1;
	// plain (unencrypted) savedata; versions 0/2/3 need a game key
	p->secureVersion = 1;
	strcpy(p->gameName, SAVE_GAMENAME);
	sprintf(p->saveName, "SLOT%d", slot+1);
	strcpy(p->fileName, SAVE_FILENAME);
}

// runs the (invisible) savedata utility to completion, returns its result
static int
runUtility(SceUtilitySavedataParam *p)
{
	int r = sceUtilitySavedataInitStart(p);
	if(r < 0)
		return r;
	bool shutdown = false;
	for(int i = 0; i < 60*30; i++){	// 30 s timeout
		int st = sceUtilitySavedataGetStatus();
		if(st == PSP_UTILITY_DIALOG_VISIBLE)
			sceUtilitySavedataUpdate(1);
		else if(st == PSP_UTILITY_DIALOG_QUIT){
			if(!shutdown){
				sceUtilitySavedataShutdownStart();
				shutdown = true;
			}
		}else if(st == PSP_UTILITY_DIALOG_NONE && shutdown)
			break;
		else if(st < 0)
			return st;
		sceDisplayWaitVblankStart();
	}
	return p->base.result;
}

static bool
readFile(const char *path, void *buf, int max, int *size)
{
	FILE *f = fopen(path, "rb");
	if(f == NULL)
		return false;
	*size = (int)fread(buf, 1, max, f);
	fclose(f);
	return *size > 0;
}

static bool
fileExists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

void pspSaveDetail(char *out, int size);	// psp.cpp

// save title from the save file: a UTF-16 name at offset 4
static void
saveTitle(char *out, int outSize, const unsigned char *data, int size, int slot)
{
	char name[25];
	int n = 0;
	for(int i = 0; i < 24 && 4 + i*2 + 1 < size; i++){
		unsigned c = data[4 + i*2] | data[4 + i*2 + 1]<<8;
		if(c == 0)
			break;
		name[n++] = (c >= 32 && c < 127) ? (char)c : '?';
	}
	name[n] = '\0';
	if(slot < 0)
		snprintf(out, outSize, "%s", n > 0 ? name : "Vice City");
	else if(n > 0)
		snprintf(out, outSize, "Slot %d - %s", slot+1, name);
	else
		snprintf(out, outSize, "Slot %d", slot+1);
}

static void
prepareStore(SceUtilitySavedataParam &p, int slot, void *data, int size)
{
	initParams(&p, PSP_UTILITY_SAVEDATA_AUTOSAVE, slot);
	p.dataBuf = data;
	p.dataBufSize = size;
	p.dataSize = size;
	strcpy(p.sfoParam.title, "GTA: Vice City");
	{
		extern bool pspAutosaving;
		char name[64];
		saveTitle(name, sizeof(name), (unsigned char*)data, size, -1);
		if(pspAutosaving)
			snprintf(p.sfoParam.savedataTitle, sizeof(p.sfoParam.savedataTitle), "Autosave - %s", name);
		else
			saveTitle(p.sfoParam.savedataTitle, sizeof(p.sfoParam.savedataTitle), (unsigned char*)data, size, slot);
	}
	pspSaveDetail(p.sfoParam.detail, sizeof(p.sfoParam.detail));
	p.sfoParam.parentalLevel = 1;
	p.icon0FileData.buf = (void*)pspSaveIcon0;
	p.icon0FileData.bufSize = sizeof(pspSaveIcon0);
	p.icon0FileData.size = sizeof(pspSaveIcon0);
}

static bool
storeSlot(int slot, void *data, int size)
{
	static SceUtilitySavedataParam p;
	prepareStore(p, slot, data, size);
	int r = runUtility(&p);
	logf("savedata: store slot %d result %08x\n", slot+1, r);
	return r == 0;
}

//
// Autosave without stopping the game: the save is made in memory, and the
// (invisible) savedata utility writes it while the game goes on; it's
// looked after once a frame (pspSavedataPump)
//
static unsigned char *saveMem;
static unsigned int saveMemLen;
static SceUtilitySavedataParam asyncParam;
static int asyncState;	// 0 idle, 1 running, 2 shutting down
static int asyncSlot;

bool
pspSaveMemBegin(void)
{
	if(asyncState != 0 || saveMem)
		return false;
	saveMem = (unsigned char*)memalign(64, MAX_SAVE_SIZE);
	saveMemLen = 0;
	return saveMem != NULL;
}

bool
pspSaveMemWrite(const void *data, unsigned int size)
{
	if(saveMem == NULL || saveMemLen + size > MAX_SAVE_SIZE)
		return false;
	memcpy(saveMem + saveMemLen, data, size);
	saveMemLen += size;
	return true;
}

void
pspSaveMemDiscard(void)
{
	if(asyncState == 0){
		free(saveMem);
		saveMem = NULL;
	}
}

// starts writing the save made in memory into slot (0..7)
bool
pspSavedataStoreAsync(int slot)
{
	if(saveMem == NULL || asyncState != 0)
		return false;
	prepareStore(asyncParam, slot, saveMem, saveMemLen);
	int r = sceUtilitySavedataInitStart(&asyncParam);
	if(r < 0){
		logf("savedata: autosave start failed %08x%.0d\n", r, 0);
		pspSaveMemDiscard();
		return false;
	}
	asyncSlot = slot;
	asyncState = 1;
	return true;
}

// once a frame: 0 nothing / still busy, 1 the autosave was written, -1 it failed
int
pspSavedataPump(void)
{
	if(asyncState == 0)
		return 0;
	int st = sceUtilitySavedataGetStatus();
	if(st == PSP_UTILITY_DIALOG_VISIBLE)
		sceUtilitySavedataUpdate(1);
	else if(st == PSP_UTILITY_DIALOG_QUIT){
		if(asyncState == 1){
			sceUtilitySavedataShutdownStart();
			asyncState = 2;
		}
	}else if((st == PSP_UTILITY_DIALOG_NONE && asyncState == 2) || st < 0){
		int r = st < 0 ? st : asyncParam.base.result;
		logf("savedata: autosave into slot %d result %08x\n", asyncSlot+1, r);
		asyncState = 0;
		free(saveMem);
		saveMem = NULL;
		return r == 0 ? 1 : -1;
	}
	return 0;
}

// the save screens and loading wait for an autosave being written
static void
finishAsync(void)
{
	for(int i = 0; i < 60*30 && asyncState != 0; i++){
		pspSavedataPump();
		sceDisplayWaitVblankStart();
	}
}

//
// The PSP's own save and load screens (the Saved Data Utility with its
// list of slots), shown over the game like in VCS/LCS
//
static char slotNames[NUM_SLOTS+1][20];
static SceUtilitySavedataParam dlgParam;
static PspUtilitySavedataListSaveNewData dlgNewData;
static bool dlgShutdown;

static int
dialogStep(void)
{
	int st = sceUtilitySavedataGetStatus();
	if(st == PSP_UTILITY_DIALOG_VISIBLE)
		sceUtilitySavedataUpdate(1);
	else if(st == PSP_UTILITY_DIALOG_QUIT){
		if(!dlgShutdown){
			sceUtilitySavedataShutdownStart();
			dlgShutdown = true;
		}
	}else if(st == PSP_UTILITY_DIALOG_NONE && dlgShutdown)
		return 0;
	else if(st < 0)
		return 0;
	return 1;
}

static void
setupListDialog(int mode)
{
	initParams(&dlgParam, mode, 0);
	for(int i = 0; i < NUM_SLOTS; i++)
		sprintf(slotNames[i], "SLOT%d", i+1);
	slotNames[NUM_SLOTS][0] = '\0';
	dlgParam.saveNameList = slotNames;
	dlgParam.saveName[0] = '\0';
	dlgParam.focus = mode == PSP_UTILITY_SAVEDATA_LISTSAVE ? PSP_UTILITY_SAVEDATA_FOCUS_FIRSTEMPTY : PSP_UTILITY_SAVEDATA_FOCUS_LATEST;
}

static int
slotFromName(const char *name)
{
	int n;
	if(sscanf(name, "SLOT%d", &n) == 1 && n >= 1 && n <= NUM_SLOTS)
		return n-1;
	return -1;
}

static bool
runDialog(void)
{
	dlgShutdown = false;
	if(sceUtilitySavedataInitStart(&dlgParam) < 0)
		return false;
	rw::psp::utilityDialog(dialogStep);
	return true;
}

// The save screen: the game was saved into tmpPath; the player picks a slot
// (or cancels). Returns the slot 0..7, -1 when cancelled, -2 on an error.
int pspSaveDialogAuto;	// testing aid: save into slot 1 without the screen (no buttons in the emulator)
int
pspSaveDialogSave(const char *tmpPath)
{
	finishAsync();
	if(pspSaveDialogAuto){
		void *b = memalign(64, MAX_SAVE_SIZE);
		int sz;
		bool ok = b && readFile(tmpPath, b, MAX_SAVE_SIZE, &sz) && storeSlot(0, b, sz);
		free(b);
		logf("savedata: test save into slot %d: %d\n", 1, ok);
		return ok ? 0 : -2;
	}
	void *buf = memalign(64, MAX_SAVE_SIZE);
	if(buf == NULL)
		return -2;
	int size;
	if(!readFile(tmpPath, buf, MAX_SAVE_SIZE, &size)){
		free(buf);
		return -2;
	}
	setupListDialog(PSP_UTILITY_SAVEDATA_LISTSAVE);
	dlgParam.dataBuf = buf;
	dlgParam.dataBufSize = size;
	dlgParam.dataSize = size;
	strcpy(dlgParam.sfoParam.title, "GTA: Vice City");
	// the save's name (the last mission or the place), like the game's own list
	char name[64];
	saveTitle(name, sizeof(name), (unsigned char*)buf, size, -1);
	snprintf(dlgParam.sfoParam.savedataTitle, sizeof(dlgParam.sfoParam.savedataTitle), "%s", name);
	pspSaveDetail(dlgParam.sfoParam.detail, sizeof(dlgParam.sfoParam.detail));
	dlgParam.sfoParam.parentalLevel = 1;
#ifdef PSP_SAVE_ART
	dlgParam.icon0FileData.buf = (void*)pspSaveArtIcon0;
	dlgParam.icon0FileData.bufSize = sizeof(pspSaveArtIcon0);
	dlgParam.icon0FileData.size = sizeof(pspSaveArtIcon0);
	dlgParam.pic1FileData.buf = (void*)pspSaveArtPic1;
	dlgParam.pic1FileData.bufSize = sizeof(pspSaveArtPic1);
	dlgParam.pic1FileData.size = sizeof(pspSaveArtPic1);
#else
	dlgParam.icon0FileData.buf = (void*)pspSaveIcon0;
	dlgParam.icon0FileData.bufSize = sizeof(pspSaveIcon0);
	dlgParam.icon0FileData.size = sizeof(pspSaveIcon0);
#endif
	memset(&dlgNewData, 0, sizeof(dlgNewData));
	dlgNewData.title = (char*)"New Save Data";
	dlgParam.newData = &dlgNewData;
	int slot = -2;
	if(runDialog()){
		int r = dlgParam.base.result;
		if(r == 0)
			slot = slotFromName(dlgParam.saveName);
		else if(r == 1)
			slot = -1;	// cancelled
		logf("savedata: save screen result %08x slot %d\n", r, slot+1);
	}
	free(buf);
	return slot < 0 && slot != -1 ? -2 : slot;
}

// The load screen: the chosen save goes into userfiles/GTAVCsfN.b.
// Returns the slot 0..7, -1 when cancelled or nothing was there.
int
pspSaveDialogLoad(void)
{
	finishAsync();
	void *buf = memalign(64, MAX_SAVE_SIZE);
	if(buf == NULL)
		return -1;
	setupListDialog(PSP_UTILITY_SAVEDATA_LISTLOAD);
	dlgParam.dataBuf = buf;
	dlgParam.dataBufSize = MAX_SAVE_SIZE;
	int slot = -1;
	if(runDialog() && dlgParam.base.result == 0 && dlgParam.dataSize > 0){
		slot = slotFromName(dlgParam.saveName);
		if(slot >= 0){
			char path[64];
			localName(path, slot);
			FILE *f = fopen(path, "wb");
			if(f){
				fwrite(buf, 1, dlgParam.dataSize, f);
				fclose(f);
			}else
				slot = -1;
		}
	}
	logf("savedata: load screen result %08x slot %d\n", dlgParam.base.result, slot+1);
	free(buf);
	return slot;
}

// called after the game wrote userfiles/GTAVCsfN.b (slot 0..7)
void
pspSavedataStore(int slot)
{
	finishAsync();
	if(slot < 0 || slot >= NUM_SLOTS)
		return;
	char path[64];
	localName(path, slot);
	void *buf = memalign(64, MAX_SAVE_SIZE);
	if(buf == NULL){
		pspLog("savedata: no memory to store\n");
		return;
	}
	int size;
	if(readFile(path, buf, MAX_SAVE_SIZE, &size))
		storeSlot(slot, buf, size);
	free(buf);
}

// called after the game deleted a slot
void
pspSavedataDelete(int slot)
{
	if(slot < 0 || slot >= NUM_SLOTS)
		return;
	static SceUtilitySavedataParam p;
	initParams(&p, SCE_UTILITY_SAVEDATA_AUTODELETE, slot);
	int r = runUtility(&p);
	logf("savedata: delete slot %d result %08x\n", slot+1, r);
}

// startup: SAVEDATA -> userfiles
void
pspSavedataSync(void)
{
	mkdir("userfiles", 0777);
	bool migrated = fileExists(MIGRATED_MARK);
	void *buf = memalign(64, MAX_SAVE_SIZE);
	if(buf == NULL)
		return;
	// PC saves: GTAVCsfN.b copied from the PC into the "pc_saves" folder
	// next to EBOOT.PBP go into slot N (replacing what's there), then the
	// file is renamed to GTAVCsfN.b.imported so it's done only once
	for(int slot = 0; slot < NUM_SLOTS; slot++){
		char in[64], done[80];
		sprintf(in, "pc_saves/GTAVCsf%d.b", slot+1);
		int size;
		if(!readFile(in, buf, MAX_SAVE_SIZE, &size))
			continue;
		if(size < 1024 || size >= MAX_SAVE_SIZE){
			logf("savedata: pc_saves slot %d has a strange size (%d bytes), skipped\n", slot+1, size);
			continue;
		}
		if(storeSlot(slot, buf, size)){
			logf("savedata: PC save imported into slot %d (%d bytes)\n", slot+1, size);
			sprintf(done, "%s.imported", in);
			remove(done);
			rename(in, done);
		}
	}
	for(int slot = 0; slot < NUM_SLOTS; slot++){
		char path[64];
		localName(path, slot);
		static SceUtilitySavedataParam p;
		initParams(&p, PSP_UTILITY_SAVEDATA_AUTOLOAD, slot);
		p.dataBuf = buf;
		p.dataBufSize = MAX_SAVE_SIZE;
		int r = runUtility(&p);
		if(r == 0 && p.dataSize > 0){
			FILE *f = fopen(path, "wb");
			if(f){
				fwrite(buf, 1, p.dataSize, f);
				fclose(f);
			}
			logf("savedata: slot %d loaded (%d bytes)\n", slot+1, (int)p.dataSize);
		}else if(fileExists(path)){
			if(migrated){
				// deleted in the XMB
				remove(path);
				logf("savedata: slot %d gone, removed local copy (%08x)\n", slot+1, r);
			}else{
				// first run with savedata support: move the old save over
				int size;
				if(readFile(path, buf, MAX_SAVE_SIZE, &size))
					storeSlot(slot, buf, size);
			}
		}
	}
	free(buf);
	FILE *f = fopen(MIGRATED_MARK, "w");
	if(f){
		fputs("saves are kept in PSP/SAVEDATA/" SAVE_GAMENAME "SLOTn\n", f);
		fclose(f);
	}
}

#endif

#ifdef N3DS
// 3DS: saves stay plain files in userfiles/ (on the SD card), nothing to copy.
// PC saves (GTAVCsfN.b) put into the "pc_saves" folder go into slot N once.
#include <stdio.h>
#include <sys/stat.h>
extern void pspLog(const char *str);
void pspSetLastSlot(int slot);
void pspSavedataStore(int slot) { pspSetLastSlot(slot); }	// the game starts with it next time
void pspSavedataDelete(int slot) {}
bool pspSaveMemWrite(const void *data, unsigned int size) { return false; }
void
pspSavedataSync(void)
{
	mkdir("userfiles", 0777);
	for(int slot = 0; slot < 8; slot++){
		char in[64], out[64], done[80];
		sprintf(in, "pc_saves/GTAVCsf%d.b", slot+1);
		sprintf(out, "userfiles/GTAVCsf%d.b", slot+1);
		FILE *f = fopen(in, "rb");
		if(f == NULL)
			continue;
		FILE *o = fopen(out, "wb");
		if(o){
			char buf[4096];
			size_t n;
			while((n = fread(buf, 1, sizeof(buf), f)) > 0)
				fwrite(buf, 1, n, o);
			fclose(o);
		}
		fclose(f);
		if(o){
			sprintf(done, "%s.imported", in);
			remove(done);
			rename(in, done);
			char b[80];
			snprintf(b, sizeof(b), "savedata: PC save imported into slot %d\n", slot+1);
			pspLog(b);
		}
	}
}
#endif

// the save to start with (both PSP and 3DS)
#if defined PSP
#ifdef N3DS
#define NUM_SLOTS	8
static void
logf(const char *fmt, int a, int b)
{
	char buf[128];
	snprintf(buf, sizeof(buf), fmt, a, b);
	pspLog(buf);
}
static void
localName(char *out, int slot)	// slot 0..7
{
	sprintf(out, "userfiles/GTAVCsf%d.b", slot+1);
}
#endif
// "Resume game": the slot with the newest save (the date the game wrote into
// it: the SYSTEMTIME after the name), -1 when there's none
int
pspNewestSaveSlot(void)
{
	int best = -1;
	unsigned long long bestKey = 0;
	for(int slot = 0; slot < NUM_SLOTS - 1; slot++){	// (slot 8 was the autosave)
		char path[64];
		localName(path, slot);
		FILE *f = fopen(path, "rb");
		if(f == NULL)
			continue;
		unsigned char h[68];
		int n = (int)fread(h, 1, sizeof(h), f);
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fclose(f);
		if(n < 68 || size < 1024)
			continue;
		#define W(o) ((unsigned)(h[o] | h[(o)+1]<<8))
		// year month (dayofweek) day hour minute second ms
		unsigned long long key = ((unsigned long long)W(52) << 40) | ((unsigned long long)W(54) << 32) |
			((unsigned long long)W(58) << 24) | (W(60) << 16) | (W(62) << 8) | W(64);
		#undef W
		if(best < 0 || key >= bestKey){
			best = slot;
			bestKey = key;
		}
	}
	logf("savedata: newest save is slot %d%.0d\n", best+1, 0);
	return best;
}

// the slot last saved into or loaded from (the game starts with it)
#define LAST_SLOT_FILE "userfiles/lastslot.txt"
void
pspSetLastSlot(int slot)
{
	FILE *f = fopen(LAST_SLOT_FILE, "w");
	if(f){
		fprintf(f, "%d\n", slot + 1);
		fclose(f);
	}
}

int
pspGetLastSlot(void)
{
	int n = 0;
	FILE *f = fopen(LAST_SLOT_FILE, "r");
	if(f){
		if(fscanf(f, "%d", &n) != 1)
			n = 0;
		fclose(f);
	}
	return n >= 1 && n <= NUM_SLOTS - 1 ? n - 1 : -1;	// (slot 8 was the autosave)
}
#endif
