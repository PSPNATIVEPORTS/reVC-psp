// Sony PSP sample manager.
//
//  - Sound effects come from AUDIO/SFX.RAW (+ SFX.SDT). The PC game keeps the
//    whole effects bank in RAM; that doesn't fit on the PSP, so samples are
//    loaded on demand into a small LRU cache (pspTune.sfxCacheKB).
//  - Radio, cutscene and mission speech are MP3 streams (.ADF = MP3 xor 0x22),
//    decoded on the Media Engine with sceAudiocodec by a decoder thread.
//  - A mixer thread mixes all channels and streams into one 44.1 kHz stereo
//    hardware channel.
//  - 3D positioning is done by the game's own audio manager
//    (no EXTERNAL_3D_SOUND), we only get volume and pan per channel.

#include "common.h"

#ifdef AUDIO_PSP

#include <pspkernel.h>
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <psputility.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "Timer.h"
#include "FileMgr.h"
#include "../skel/psp/psptune.h"

extern void pspLog(const char *str);

cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;
uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s;

#define MAX_MIX_SAMPLES	1024
// 44100 Hz (1024 samples per 23 ms block) or, to save CPU, 22050 Hz
// through the hardware sample rate converter (audio_rate in psp_settings.txt)
static int OUT_RATE = 44100;
static int MIX_SAMPLES = 1024;
static bool useSRC;

static void
logf(const char *fmt, int a = 0, int b = 0, int c = 0)
{
	char buf[160];
	snprintf(buf, sizeof(buf), fmt, a, b, c);
	pspLog(buf);
}

static void
fixPath(char *dst, const char *src, int size)
{
	// Absolute: the files are opened by the decoder thread too, and on the
	// PSP the current directory belongs to the thread (only PPSSPP shares it).
	int i = 0;
	const char *root = CFileMgr::GetRootDirName();
	if(root[0] && strchr(src, ':') == nil)
		for(; root[i] && i < size-1; i++)
			dst[i] = root[i] == '\\' ? '/' : root[i];
	if(i > 0 && dst[i-1] != '/' && i < size-1)
		dst[i++] = '/';
	for(int j = 0; src[j] && i < size-1; j++, i++)
		dst[i] = src[j] == '\\' ? '/' : src[j];
	dst[i] = '\0';
}

// Mixer lock: held by the mixer while it mixes a block, and by the game
// thread whenever it changes sample data that the mixer may be reading.
static SceUID mixLock = -1;
static inline void lockMix(void) { if(mixLock >= 0) sceKernelWaitSema(mixLock, 1, NULL); }
static inline void unlockMix(void) { if(mixLock >= 0) sceKernelSignalSema(mixLock, 1); }

//
// Sample data
//

static SceUID sfxFile = -1;
static int32 sfxFileSize;

struct CachedSample
{
	int16 * volatile data;
	uint32 lastUse;
	volatile uint32 lastUseMs;	// game time it was last asked for (never evicted soon after)
	volatile uint8 loading;	// queued for the audio file thread
};
static CachedSample *sampleCache;	// TOTAL_AUDIO_SAMPLES entries
static uint32 cacheUsed, cacheBudget;
static uint32 cacheClock;

static uint8 *pedSlotData;		// MAX_PEDSFX * PED_BLOCKSIZE
static int32 nPedSlotSfx[MAX_PEDSFX];
static uint8 nCurrentPedSlot;
static uint32 gPlayerTalkSfx = UINT32_MAX;
static uint8 *gPlayerTalkData;
static uint32 gPlayerTalkDataSize;

static bool
readSfx(uint32 offset, void *dst, uint32 size)
{
	if(sfxFile < 0)
		return false;
	if(sceIoLseek32(sfxFile, offset, PSP_SEEK_SET) == (int)offset &&
	   sceIoRead(sfxFile, dst, size) == (int)size)
		return true;
	// after sleep mode the PSP's open files are dead: open it again
	sceIoClose(sfxFile);
	sfxFile = sceIoOpen("AUDIO/SFX.RAW", PSP_O_RDONLY, 0);
	if(sfxFile < 0)
		return false;
	return sceIoLseek32(sfxFile, offset, PSP_SEEK_SET) == (int)offset &&
		sceIoRead(sfxFile, dst, size) == (int)size;
}

//
// Channels
//

struct MixChannel
{
	const int16 *data;	// mono 16 bit
	uint32 length;		// samples
	uint32 pos;		// integer sample position
	uint32 frac;		// 16.16 fraction
	uint32 step;		// 16.16 source samples per output sample
	uint32 freq;
	int32 loopStart, loopEnd;	// samples, loopEnd -1 = end of sample
	uint32 loopCount;	// 0 = forever, 1 = once
	int32 volL, volR;	// 0..256
	uint32 volume, pan;	// as set by the game
	int32 sfx;		// sample id
	volatile bool playing;
};
static MixChannel channels[NUM_CHANNELS];

static void
updateChannelVolume(cSampleManager *sm, MixChannel *c, uint8 effectsVol, uint8 fadeVol)
{
	uint32 v = (fadeVol * c->volume * effectsVol) >> 14;	// 0..127
	int32 pan = c->pan;	// 0 left .. 63 centre .. 127 right
	int32 l = pan <= 63 ? 256 : (127 - pan) * 256 / 64;
	int32 r = pan >= 64 ? 256 : pan * 256 / 63;
	c->volL = v * l / 127;
	c->volR = v * r / 127;
}

//
// Streams
//

#define STREAM_RING	(64*1024)	// stereo frames: 1.5 s of 44.1 kHz, room for a slow memory stick read
#define STREAM_READBUF	(16*1024)
#define MP3_MAX_FRAME	2881

enum { STREAM_MP3, STREAM_WAV_PCM, STREAM_WAV_IMA };

struct MixStream
{
	SceUID fd;
	uint32 track;	// to open the file again after sleep mode
	int type;
	bool xorAdf;
	uint32 fileSize;
	uint32 dataStart;	// MP3: after ID3 tag, WAV: start of the data chunk
	uint32 dataEnd;
	uint32 filePos;		// next byte to read
	uint32 sampleRate;
	uint32 bitrate;		// MP3: bits per second (first frame)
	uint32 lengthMS;
	// WAV
	int channels;
	uint32 blockAlign;	// IMA: bytes per block
	uint32 samplesPerBlock;	// IMA
	bool opened, decoding, eof, loop;
	volatile bool playing, paused;
	uint32 startMS;		// position of the first decoded sample
	uint64 framesPlayed;	// output position in source sample frames
	// codec
	unsigned long codec[65] __attribute__((aligned(64)));
	bool codecReady;
	uint8 *inBuf;		// compressed data window
	uint32 inLen, inOff;
	int16 *pcmFrame;	// one decoded frame, stereo
	// decoded ring (stereo frames at sampleRate)
	int16 *ring;
	volatile uint32 ringWrite, ringRead;	// in frames, wrap at STREAM_RING
	uint32 frac;
	int32 volL, volR;
	uint32 volume, pan;
	bool effectFlag;
	// requests from the game thread, carried out by the decoder thread
	// (the game never waits for the memory stick)
	volatile uint32 reqSerial;	// bumped for every request
	uint32 doneSerial;	// decoder: last request carried out
	volatile int reqOp;	// REQ_OPEN or REQ_CLOSE
	volatile uint32 reqTrack, reqPos;
	volatile bool reqPlay, reqLoop;
	volatile bool pending;	// a request is waiting (game view: "opening")
};
enum { REQ_OPEN = 1, REQ_CLOSE = 2 };
static MixStream streams[MAX_STREAMS];
static uint32 nStreamLength[TOTAL_STREAMED_SOUNDS];
static bool8 nStreamLoopedFlag[MAX_STREAMS];
static bool codecModuleLoaded;

// Stream lock: the decoder thread holds it while it works on a stream, the
// game thread while it opens, closes or seeks one. Without it the decoder
// could work on buffers that are being reset (random crashes).
static SceUID streamLock = -1;
static inline void lockStreams(void) { if(streamLock >= 0) sceKernelWaitSema(streamLock, 1, NULL); }
static inline void unlockStreams(void) { if(streamLock >= 0) sceKernelSignalSema(streamLock, 1); }

// Request lock: only ever held for copying a few request fields, never
// around file access
static SceUID reqLock = -1;
static inline void lockReq(void) { if(reqLock >= 0) sceKernelWaitSema(reqLock, 1, NULL); }
static inline void unlockReq(void) { if(reqLock >= 0) sceKernelSignalSema(reqLock, 1); }

// Effect samples, ped comments and player speech the game asked for, read
// by the decoder thread from SFX.RAW (game thread puts, decoder takes)
enum { IOQ_SFX = 1, IOQ_PEDCOMMENT, IOQ_PLAYERTALK };
struct IoReq { int type; uint32 sfx; int32 slot; };
#define IOQ_SIZE 64
static IoReq ioQueue[IOQ_SIZE];
static volatile uint32 ioHead, ioTail;
static bool
ioPut(int type, uint32 sfx, int32 slot)
{
	uint32 h = ioHead;
	if(((h + 1) & (IOQ_SIZE-1)) == ioTail)
		return false;	// full: the game asks again later
	ioQueue[h].type = type;
	ioQueue[h].sfx = sfx;
	ioQueue[h].slot = slot;
	__sync_synchronize();
	ioHead = (h + 1) & (IOQ_SIZE-1);
	return true;
}
static volatile int32 pendingPedComment = -1, pendingPlayerTalk = -1;
static tSample *gSamples;	// SampleManager.m_aSamples, for the decoder thread

static const int mp3Bitrates[2][16] = {
	{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },	// MPEG1 L3
	{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },	// MPEG2/2.5 L3
};
static const int mp3Rates[3][3] = {
	{ 44100, 48000, 32000 },	// MPEG1
	{ 22050, 24000, 16000 },	// MPEG2
	{ 11025, 12000, 8000 },		// MPEG2.5
};

struct Mp3Header { int size, rate, samples, bitrate; };

static bool
parseHeader(const uint8 *h, Mp3Header *out)
{
	if(h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
		return false;
	int ver = (h[1] >> 3) & 3;	// 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
	int layer = (h[1] >> 1) & 3;	// 1 = layer 3
	if(ver == 1 || layer != 1)
		return false;
	int bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
	if(bri == 0 || bri == 15 || sri == 3)
		return false;
	int v = ver == 3 ? 0 : ver == 2 ? 1 : 2;
	int br = mp3Bitrates[v == 0 ? 0 : 1][bri] * 1000;
	int rate = mp3Rates[v][sri];
	out->samples = v == 0 ? 1152 : 576;
	out->size = (out->samples / 8) * br / rate + pad;
	out->rate = rate;
	out->bitrate = br;
	return out->size > 4;
}

static int
streamRead(MixStream *s, uint8 *dst, int size)
{
	if(s->dataEnd && s->filePos + size > s->dataEnd)
		size = s->dataEnd > s->filePos ? s->dataEnd - s->filePos : 0;
	if(size <= 0)
		return 0;
	int n = sceIoRead(s->fd, dst, size);
	if(n < 0){
		// after sleep mode the PSP's open files are dead: open it again
		char path[192];
		fixPath(path, StreamedNameTable[s->track], sizeof(path));
		sceIoClose(s->fd);
		s->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
		if(s->fd < 0)
			return 0;
		sceIoLseek32(s->fd, s->filePos, PSP_SEEK_SET);
		n = sceIoRead(s->fd, dst, size);
	}
	if(n <= 0)
		return 0;
	if(s->xorAdf)
		for(int i = 0; i < n; i++)
			dst[i] ^= 0x22;
	s->filePos += n;
	return n;
}

// fill the compressed window so it holds at least one whole frame
static void
refill(MixStream *s)
{
	if(s->inOff > s->inLen)
		s->inOff = s->inLen;
	if(s->inOff > 0){
		memmove(s->inBuf, s->inBuf + s->inOff, s->inLen - s->inOff);
		s->inLen -= s->inOff;
		s->inOff = 0;
	}
	if(s->inLen < STREAM_READBUF)
		s->inLen += streamRead(s, s->inBuf + s->inLen, STREAM_READBUF - s->inLen);
}

static void
seekBytes(MixStream *s, uint32 pos)
{
	if(pos < s->dataStart) pos = s->dataStart;
	uint32 end = s->dataEnd ? s->dataEnd : s->fileSize;
	if(pos > end) pos = end;
	sceIoLseek32(s->fd, pos, PSP_SEEK_SET);
	s->filePos = pos;
	s->inLen = s->inOff = 0;
	s->eof = false;
}

static uint32
id3Size(SceUID fd)
{
	uint8 h[10];
	sceIoLseek32(fd, 0, PSP_SEEK_SET);
	if(sceIoRead(fd, h, 10) != 10)
		return 0;
	if(h[0] == 'I' && h[1] == 'D' && h[2] == '3')
		return 10 + ((h[6]&0x7F)<<21 | (h[7]&0x7F)<<14 | (h[8]&0x7F)<<7 | (h[9]&0x7F));
	return 0;
}

// find the first frame at or after the current read position
static bool
syncFrame(MixStream *s, Mp3Header *hdr)
{
	for(int tries = 0; tries < 64; tries++){
		refill(s);
		while(s->inOff + 4 <= s->inLen){
			uint8 *p = s->inBuf + s->inOff;
			Mp3Header h;
			if(parseHeader(p, &h)){
				// confirm with the following header when it's in the window
				if(s->inOff + h.size + 4 <= s->inLen){
					Mp3Header h2;
					if(!parseHeader(p + h.size, &h2)){
						s->inOff++;
						continue;
					}
				}
				*hdr = h;
				return true;
			}
			s->inOff++;
		}
		if(s->filePos >= s->fileSize)
			return false;
	}
	return false;
}

static inline uint32 rd16(const uint8 *p) { return p[0] | p[1]<<8; }
static inline uint32 rd32(const uint8 *p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32)p[3]<<24; }

// RIFF/WAVE header: PCM 16 bit or IMA ADPCM, mono or stereo
static bool
parseWav(MixStream *s)
{
	uint8 h[12];
	sceIoLseek32(s->fd, 0, PSP_SEEK_SET);
	if(sceIoRead(s->fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h+8, "WAVE", 4))
		return false;
	uint32 pos = 12;
	int format = 0;
	bool haveFmt = false;
	for(int n = 0; n < 32; n++){
		uint8 ch[8];
		sceIoLseek32(s->fd, pos, PSP_SEEK_SET);
		if(sceIoRead(s->fd, ch, 8) != 8)
			return false;
		uint32 size = rd32(ch+4);
		if(memcmp(ch, "fmt ", 4) == 0){
			uint8 f[20];
			memset(f, 0, sizeof(f));
			sceIoRead(s->fd, f, size < 20 ? size : 20);
			format = rd16(f);
			s->channels = rd16(f+2);
			s->sampleRate = rd32(f+4);
			s->blockAlign = rd16(f+12);
			int bits = rd16(f+14);
			if(format == 1 && bits == 16)
				s->type = STREAM_WAV_PCM;
			else if(format == 0x11 && bits == 4)
				s->type = STREAM_WAV_IMA;
			else{
				logf("audio: unsupported wav format %d (%d bit)\n", format, bits);
				return false;
			}
			haveFmt = true;
		}else if(memcmp(ch, "data", 4) == 0){
			if(!haveFmt || s->channels < 1 || s->channels > 2 || s->sampleRate == 0)
				return false;
			s->dataStart = pos + 8;
			s->dataEnd = s->dataStart + size;
			if(s->dataEnd > s->fileSize)
				s->dataEnd = s->fileSize;
			uint32 frames;
			if(s->type == STREAM_WAV_IMA){
				if(s->blockAlign <= 4u*s->channels)
					return false;
				s->samplesPerBlock = (s->blockAlign - 4*s->channels) * 2 / s->channels + 1;
				frames = (s->dataEnd - s->dataStart) / s->blockAlign * s->samplesPerBlock;
			}else
				frames = (s->dataEnd - s->dataStart) / (2*s->channels);
			s->lengthMS = (uint32)((uint64)frames * 1000 / s->sampleRate);
			return true;
		}
		pos += 8 + size + (size & 1);
		if(pos >= s->fileSize)
			return false;
	}
	return false;
}

static void
closeStreamLocked(MixStream *s)
{
	lockMix();
	s->playing = false;
	s->opened = false;
	unlockMix();
	if(s->fd >= 0){
		sceIoClose(s->fd);
		s->fd = -1;
	}
}

static void
closeStream(MixStream *s)
{
	lockStreams();
	closeStreamLocked(s);
	unlockStreams();
}

#define lockStreams()	// open/seek run on the decoder thread, which holds the lock
#define unlockStreams()
static bool
openStream(MixStream *s, uint32 track)
{
	lockStreams();
	closeStreamLocked(s);
	if(s->ring == nil){
		unlockStreams();
		return false;
	}
	char path[192];
	fixPath(path, StreamedNameTable[track], sizeof(path));
	s->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	s->track = track;
	static bool shown;
	extern int pspAudioLog;
	if(!shown || pspAudioLog){
		shown = true;
		char b[240];
		snprintf(b, sizeof(b), "audio: first stream %s (%x)\n", path, (unsigned)s->fd);
		pspLog(b);
	}
	if(s->fd < 0){
		unlockStreams();
		// (once per track, not every frame)
		static uint8 told[(TOTAL_STREAMED_SOUNDS+7)/8];
		if(track < TOTAL_STREAMED_SOUNDS && !(told[track/8] & (1<<(track%8)))){
			told[track/8] |= 1<<(track%8);
			char b[240];
			snprintf(b, sizeof(b), "audio: can't open stream %d %s (%x)\n", (int)track, path, (unsigned)s->fd);
			pspLog(b);
		}
		return false;
	}
	int len = strlen(path);
	uint8 magic[4] = { 0, 0, 0, 0 };
	sceIoRead(s->fd, magic, 4);
	bool isWav = memcmp(magic, "RIFF", 4) == 0;	// by content, not by name
	s->xorAdf = len > 4 && (path[len-3] == 'A' || path[len-3] == 'a') && (path[len-2] == 'D' || path[len-2] == 'd');
	s->fileSize = sceIoLseek32(s->fd, 0, PSP_SEEK_END);
	s->dataEnd = 0;
	bool ok;
	if(isWav)
		ok = parseWav(s);
	else if(!s->codecReady)
		ok = false;
	else{
		s->type = STREAM_MP3;
		s->dataStart = s->xorAdf ? 0 : id3Size(s->fd);
		seekBytes(s, s->dataStart);
		Mp3Header h;
		ok = syncFrame(s, &h);
		if(ok){
			s->sampleRate = h.rate;
			s->bitrate = h.bitrate;
			s->lengthMS = (uint32)((uint64)(s->fileSize - s->dataStart) * 8000 / h.bitrate);
			// the codec keeps state between frames (bit reservoir), start clean
			sceAudiocodecInit(s->codec, PSP_CODEC_MP3);
		}
	}
	if(!ok){
		sceIoClose(s->fd);
		s->fd = -1;
		unlockStreams();
		logf("audio: can't play stream %d\n", track);
		return false;
	}
	seekBytes(s, s->dataStart);
	nStreamLength[track] = s->lengthMS;
	s->startMS = 0;
	s->framesPlayed = 0;
	s->ringRead = s->ringWrite = 0;
	s->frac = 0;
	s->eof = false;
	s->paused = false;
	s->decoding = true;
	s->opened = true;
	unlockStreams();
	return true;
}

static void
seekStreamMS(MixStream *s, uint32 ms)
{
	lockStreams();
	if(!s->opened){
		unlockStreams();
		return;
	}
	if(s->lengthMS && ms >= s->lengthMS)
		ms = s->loop ? ms % s->lengthMS : s->lengthMS;
	uint32 pos;
	if(s->type == STREAM_MP3)
		pos = s->dataStart + (uint32)((uint64)ms * s->bitrate / 8000);
	else if(s->type == STREAM_WAV_IMA){
		uint32 block = (uint32)((uint64)ms * s->sampleRate / 1000) / s->samplesPerBlock;
		pos = s->dataStart + block * s->blockAlign;
		ms = (uint32)((uint64)block * s->samplesPerBlock * 1000 / s->sampleRate);
	}else
		pos = s->dataStart + (uint32)((uint64)ms * s->sampleRate / 1000) * 2 * s->channels;
	lockMix();
	seekBytes(s, pos);
	s->ringRead = s->ringWrite = 0;
	s->frac = 0;
	s->startMS = ms;
	s->framesPlayed = 0;
	unlockMix();
	if(s->type == STREAM_MP3)
		sceAudiocodecInit(s->codec, PSP_CODEC_MP3);
	unlockStreams();
}
#undef lockStreams
#undef unlockStreams

// decoder thread, stream lock held: carry out the game's latest request
static void
serviceStreamRequest(MixStream *s)
{
	if(s->reqSerial == s->doneSerial)
		return;
	lockReq();
	uint32 serial = s->reqSerial;
	int op = s->reqOp;
	uint32 track = s->reqTrack, pos = s->reqPos;
	bool loop = s->reqLoop;
	unlockReq();
	if(op == REQ_CLOSE){
		closeStreamLocked(s);
	}else if(op == REQ_OPEN){
		s->loop = loop;
		bool ok = openStream(s, track);
		if(ok && pos != 0)
			seekStreamMS(s, pos);
		lockReq();
		// the game may have asked to start it while it was opening
		if(ok && s->reqSerial == serial && s->reqPlay)
			s->playing = true;
		unlockReq();
	}
	s->doneSerial = serial;
	lockReq();
	if(s->reqSerial == serial)
		s->pending = false;
	unlockReq();
}

// game thread: a new request replaces any older one still waiting
static void
postStreamRequest(MixStream *s, int op, uint32 track, uint32 pos, bool play, bool loop)
{
	lockReq();
	s->reqOp = op;
	s->reqTrack = track;
	s->reqPos = pos;
	s->reqPlay = play;
	s->reqLoop = loop;
	s->pending = true;
	s->reqSerial++;
	unlockReq();
	// stop the old sound right away (the mixer lock is only held while mixing)
	lockMix();
	s->playing = false;
	s->opened = false;
	unlockMix();
}

static inline uint32
ringFill(MixStream *s)
{
	return (s->ringWrite - s->ringRead) & (STREAM_RING - 1);
}

static inline void
ringPut(MixStream *s, uint32 &w, int16 l, int16 r)
{
	s->ring[w*2] = l;
	s->ring[w*2+1] = r;
	w = (w + 1) & (STREAM_RING - 1);
}

static const int imaSteps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int imaIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int16
imaNibble(int nib, int &pred, int &idx)
{
	int step = imaSteps[idx];
	int diff = step >> 3;
	if(nib & 1) diff += step >> 2;
	if(nib & 2) diff += step >> 1;
	if(nib & 4) diff += step;
	if(nib & 8) pred -= diff; else pred += diff;
	if(pred > 32767) pred = 32767;
	if(pred < -32768) pred = -32768;
	idx += imaIndex[nib];
	if(idx < 0) idx = 0;
	if(idx > 88) idx = 88;
	return (int16)pred;
}

// one IMA ADPCM block into the ring (needs samplesPerBlock free frames)
static void
decodeImaBlock(MixStream *s, const uint8 *blk)
{
	uint32 w = s->ringWrite;
	int ch = s->channels;
	int pred[2], idx[2];
	for(int c = 0; c < ch; c++){
		pred[c] = (int16)rd16(blk + c*4);
		idx[c] = blk[c*4 + 2];
		if(idx[c] > 88) idx[c] = 88;
	}
	ringPut(s, w, pred[0], pred[ch-1]);
	const uint8 *d = blk + 4*ch;
	uint32 left = s->samplesPerBlock - 1;
	if(ch == 1){
		for(uint32 i = 0; i < left; i += 2){
			int16 a = imaNibble(*d & 0xF, pred[0], idx[0]);
			ringPut(s, w, a, a);
			if(i + 1 < left){
				int16 b = imaNibble(*d >> 4, pred[0], idx[0]);
				ringPut(s, w, b, b);
			}
			d++;
		}
	}else{
		// 8 samples per channel in groups of 4 bytes, left then right
		int16 tmp[2][8];
		for(uint32 i = 0; i < left; i += 8){
			for(int c = 0; c < 2; c++)
				for(int k = 0; k < 4; k++){
					tmp[c][k*2] = imaNibble(d[c*4+k] & 0xF, pred[c], idx[c]);
					tmp[c][k*2+1] = imaNibble(d[c*4+k] >> 4, pred[c], idx[c]);
				}
			d += 8;
			for(int k = 0; k < 8 && i + k < left; k++)
				ringPut(s, w, tmp[0][k], tmp[1][k]);
		}
	}
	s->ringWrite = w;
}

static bool
rewindForLoop(MixStream *s)
{
	if(!s->loop)
		return false;
	seekBytes(s, s->dataStart);
	if(s->type == STREAM_MP3)
		sceAudiocodecInit(s->codec, PSP_CODEC_MP3);
	return true;
}

static void
decodeWav(MixStream *s)
{
	if(s->type == STREAM_WAV_IMA){
		while(STREAM_RING - 1 - ringFill(s) >= s->samplesPerBlock){
			if(s->inLen - s->inOff < s->blockAlign)
				refill(s);
			if(s->inLen - s->inOff < s->blockAlign){
				if(rewindForLoop(s))
					continue;
				s->eof = true;
				return;
			}
			decodeImaBlock(s, s->inBuf + s->inOff);
			s->inOff += s->blockAlign;
		}
	}else{
		uint32 frameBytes = 2*s->channels;
		while(STREAM_RING - 1 - ringFill(s) >= 1024){
			if(s->inLen - s->inOff < frameBytes)
				refill(s);
			uint32 avail = (s->inLen - s->inOff) / frameBytes;
			if(avail == 0){
				if(rewindForLoop(s))
					continue;
				s->eof = true;
				return;
			}
			if(avail > 1024) avail = 1024;
			uint32 w = s->ringWrite;
			const uint8 *d = s->inBuf + s->inOff;
			for(uint32 i = 0; i < avail; i++){
				int16 l = (int16)rd16(d);
				int16 r = s->channels == 2 ? (int16)rd16(d+2) : l;
				ringPut(s, w, l, r);
				d += frameBytes;
			}
			s->ringWrite = w;
			s->inOff += avail*frameBytes;
		}
	}
}

// decode until the ring is (nearly) full; decoder thread only, stream lock held
static void
decodeStream(MixStream *s)
{
	if(!s->opened || !s->decoding || s->eof)
		return;
	if(s->type != STREAM_MP3){
		decodeWav(s);
		return;
	}
	while(STREAM_RING - 1 - ringFill(s) >= 1152){
		Mp3Header h;
		if(!syncFrame(s, &h)){
			if(rewindForLoop(s))
				continue;
			s->eof = true;
			return;
		}
		if(s->inOff + h.size > s->inLen){
			s->eof = true;
			return;
		}
		uint8 *frame = s->inBuf + s->inOff;
		s->codec[6] = (unsigned long)frame;
		s->codec[8] = (unsigned long)s->pcmFrame;
		s->codec[7] = s->codec[10] = h.size;
		s->codec[9] = 1152*4;
		int r = sceAudiocodecDecode(s->codec, PSP_CODEC_MP3);
		s->inOff += h.size;
		if(r < 0)
			continue;	// skip a bad frame
		// output is always stereo
		uint32 w = s->ringWrite;
		for(int i = 0; i < h.samples; i++)
			ringPut(s, w, s->pcmFrame[i*2], s->pcmFrame[i*2+1]);
		s->ringWrite = w;
	}
}

static SceUID decodeThread = -1, mixThread = -1;
static volatile bool audioRunning;

static void serviceIoQueue(void);
static void loadSampleIo(tSample *samples, uint32 nSfx);
static void serviceStreamRequest(MixStream *s);
static void backgroundLengths(void);

static int
decoderMain(SceSize, void*)
{
	{
		// this thread's own current directory: the game's folder (paths are absolute anyway)
		char root[192];
		fixPath(root, "", sizeof(root));
		int n = strlen(root);
		if(n > 1 && root[n-1] == '/' && root[n-2] != ':') root[n-1] = '\0';
#ifndef N3DS
		sceIoChdir(root);
#endif
	}
	while(audioRunning){
		serviceIoQueue();
		for(int i = 0; i < MAX_STREAMS; i++){
			lockStreams();
			serviceStreamRequest(&streams[i]);
			decodeStream(&streams[i]);
			unlockStreams();
		}
		backgroundLengths();
		if(ioTail != ioHead)
			sceKernelDelayThread(500);	// more to load: just let the others run a moment
		else
			sceKernelDelayThread(10000);
	}
	return 0;
}

//
// Mixer
//

static int32 mixBuf[MAX_MIX_SAMPLES*2];
static int16 outBuf[2][MAX_MIX_SAMPLES*2] __attribute__((aligned(64)));
static int hwChannel = -1;

static void
mixChannel(MixChannel *c)
{
	const int16 *data = c->data;
	uint32 pos = c->pos, frac = c->frac, step = c->step;
	int32 vl = c->volL, vr = c->volR;
	uint32 end = c->loopEnd >= 0 && (uint32)c->loopEnd <= c->length ? c->loopEnd : c->length;
	bool looping = c->loopCount != 1;
	if(step < 0x10000){
		// A sample stored at a lower rate than the output (the club music
		// heard outside the Malibu is ~2000 Hz): each sample used to be
		// repeated 8 times, a staircase that sounds like static. In between
		// two samples the value is now blended (linear interpolation).
		uint32 loopTo = c->loopStart < (int32)end ? c->loopStart : 0;
		for(int i = 0; i < MIX_SAMPLES; i++){
			if(pos >= end){
				if(looping){
					pos = loopTo;
				}else{
					c->playing = false;
					break;
				}
			}
			uint32 next = pos + 1;
			if(next >= end)
				next = looping ? loopTo : pos;
			int32 a = data[pos], b = data[next];
			int32 s = a + (((b - a) * (int32)(frac >> 1)) >> 15);
			mixBuf[i*2] += s * vl;
			mixBuf[i*2+1] += s * vr;
			frac += step;
			pos += frac >> 16;
			frac &= 0xFFFF;
		}
		c->pos = pos;
		c->frac = frac;
		return;
	}
	for(int i = 0; i < MIX_SAMPLES; i++){
		if(pos >= end){
			if(looping){
				pos = c->loopStart < (int32)end ? c->loopStart : 0;
			}else{
				c->playing = false;
				break;
			}
		}
		int32 s = data[pos];
		mixBuf[i*2] += s * vl;
		mixBuf[i*2+1] += s * vr;
		frac += step;
		pos += frac >> 16;
		frac &= 0xFFFF;
	}
	c->pos = pos;
	c->frac = frac;
}

static void
mixStream(MixStream *s)
{
	uint32 step = (s->sampleRate << 16) / OUT_RATE;
	uint32 frac = s->frac;
	uint32 r = s->ringRead;
	uint32 avail = ringFill(s);
	int32 vl = s->volL, vr = s->volR;
	uint32 used = 0;
	for(int i = 0; i < MIX_SAMPLES; i++){
		if(used >= avail)
			break;	// underrun: decoder will catch up
		mixBuf[i*2] += s->ring[r*2] * vl;
		mixBuf[i*2+1] += s->ring[r*2+1] * vr;
		frac += step;
		uint32 adv = frac >> 16;
		frac &= 0xFFFF;
		// Never step past what's decoded: a 44.1 kHz stream advances 2-3
		// frames per output sample, and overshooting the write position by
		// even one frame made the ring look full: the mixer then played the
		// whole ring again, the last 0.7 s once more (a line said twice in a
		// cutscene, whenever the decoder fell behind for a moment).
		if(used + adv > avail)
			adv = avail - used;
		r = (r + adv) & (STREAM_RING - 1);
		used += adv;
	}
	s->ringRead = r;
	s->frac = frac;
	s->framesPlayed += used;
}

static int statInit, statInitOk, statStart;
static SceUID dumpFd = -1;
static int dumpBlocks;

static int
mixerMain(SceSize, void*)
{
	int cur = 0;
	while(audioRunning){
		memset(mixBuf, 0, MIX_SAMPLES*2*sizeof(int32));
		sceKernelWaitSema(mixLock, 1, NULL);
		for(int i = 0; i < NUM_CHANNELS; i++){
			MixChannel *c = &channels[i];
			if(c->playing && c->data && (c->volL || c->volR))
				mixChannel(c);
			else if(c->playing && c->data){
				// silent: just advance
				uint64 p = ((uint64)c->pos << 16 | c->frac) + (uint64)c->step * MIX_SAMPLES;
				uint32 end = c->loopEnd >= 0 && (uint32)c->loopEnd <= c->length ? c->loopEnd : c->length;
				if((p >> 16) >= end){
					if(c->loopCount != 1){
						uint32 ls = c->loopStart < (int32)end ? c->loopStart : 0;
						uint32 len = end - ls;
						p = (uint64)(ls + (((p >> 16) - ls) % (len ? len : 1))) << 16;
					}else
						c->playing = false;
				}
				c->pos = p >> 16;
				c->frac = p & 0xFFFF;
			}
		}
		for(int i = 0; i < MAX_STREAMS; i++){
			MixStream *s = &streams[i];
			if(s->opened && s->playing && !s->paused)
				mixStream(s);
		}
		sceKernelSignalSema(mixLock, 1);
		int16 *out = outBuf[cur];
		for(int i = 0; i < MIX_SAMPLES*2; i++){
			int32 v = mixBuf[i] >> 8;
			out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
		}
		if(dumpFd >= 0){
			// testing aid (autostart.txt "audiodump"): record 2 minutes, from 2 minutes in
			if(++dumpBlocks > 120*OUT_RATE/MIX_SAMPLES)
				sceIoWrite(dumpFd, out, MIX_SAMPLES*2*sizeof(int16));
			if(dumpBlocks > 240*OUT_RATE/MIX_SAMPLES){
				sceIoClose(dumpFd);
				dumpFd = -1;
			}
		}
		if(useSRC)
			sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
		else
			sceAudioOutputBlocking(hwChannel, PSP_AUDIO_VOLUME_MAX, out);
		cur ^= 1;
	}
	return 0;
}

//
// cSampleManager
//

cSampleManager::cSampleManager(void) { }
cSampleManager::~cSampleManager(void) { }

bool8 cSampleManager::IsMP3RadioChannelAvailable(void) { return FALSE; }
void cSampleManager::ReleaseDigitalHandle(void) { }
void cSampleManager::ReacquireDigitalHandle(void) { }
bool8 cSampleManager::CheckForAnAudioFileOnCD(void) { return TRUE; }
char cSampleManager::GetCDAudioDriveLetter(void) { return '\0'; }

bool8
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised)
		return TRUE;
	if(!pspTune.audio){
		pspLog("audio: off in psp_settings.txt\n");
		return FALSE;
	}

	for(int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++){
		m_aSamples[i].nOffset = 0;
		m_aSamples[i].nSize = 0;
		m_aSamples[i].nFrequency = 22050;
		m_aSamples[i].nLoopStart = 0;
		m_aSamples[i].nLoopEnd = -1;
	}
	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = 0;
	memset(channels, 0, sizeof(channels));
	for(int32 i = 0; i < MAX_PEDSFX; i++)
		nPedSlotSfx[i] = NO_SAMPLE;
	nCurrentPedSlot = 0;

	if(!InitialiseSampleBanks()){
		pspLog("audio: AUDIO/SFX.SDT or SFX.RAW missing, no audio\n");
		return FALSE;
	}

	gSamples = m_aSamples;
	sampleCache = (CachedSample*)calloc(TOTAL_AUDIO_SAMPLES, sizeof(CachedSample));
	cacheBudget = pspTune.sfxCacheKB * 1024;
	cacheUsed = 0;
	pedSlotData = (uint8*)memalign(64, PED_BLOCKSIZE * MAX_PEDSFX);
	gPlayerTalkDataSize = 0;
	for(uint32 i = PLAYER_COMMENTS_START; i <= PLAYER_COMMENTS_END; i++)
		gPlayerTalkDataSize = Max(gPlayerTalkDataSize, m_aSamples[i].nSize);
	gPlayerTalkData = (uint8*)memalign(64, gPlayerTalkDataSize + 64);
	if(sampleCache == nil || pedSlotData == nil || gPlayerTalkData == nil){
		pspLog("audio: out of memory\n");
		return FALSE;
	}

	// MP3 decoding on the Media Engine
	if(pspTune.music){
		int r = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
		codecModuleLoaded = r >= 0 || r == (int)0x80111102;	// already loaded
		if(!codecModuleLoaded)
			logf("audio: can't load avcodec module (%08x), no music\n", r);
	}
	for(int i = 0; i < MAX_STREAMS; i++){
		MixStream *s = &streams[i];
		memset(s, 0, sizeof(*s));
		s->fd = -1;
		s->volume = 100;
		s->pan = 63;
		if(!pspTune.music)
			continue;
		// buffers for every stream (WAV speech needs no Media Engine)
		s->inBuf = (uint8*)memalign(64, STREAM_READBUF + MP3_MAX_FRAME);
		s->pcmFrame = (int16*)memalign(64, 1152*4);
		s->ring = (int16*)memalign(64, STREAM_RING*4);
		if(s->inBuf == nil || s->pcmFrame == nil || s->ring == nil){
			s->ring = nil;
			continue;
		}
		if(!codecModuleLoaded)
			continue;
		if(sceAudiocodecCheckNeedMem(s->codec, PSP_CODEC_MP3) < 0 ||
		   sceAudiocodecGetEDRAM(s->codec, PSP_CODEC_MP3) < 0){
			logf("audio: no ME decoder for stream %d\n", i);
			continue;
		}
		if(sceAudiocodecInit(s->codec, PSP_CODEC_MP3) < 0){
			sceAudiocodecReleaseEDRAM(s->codec);
			continue;
		}
		s->codecReady = true;
	}
	for(int i = 0; i < TOTAL_STREAMED_SOUNDS; i++)
		nStreamLength[i] = 0;

	{
		FILE *f = fopen("autostart.txt", "r");
		if(f){
			char b[96] = { 0 };
			fread(b, 1, 95, f);
			fclose(f);
			if(strstr(b, "audiodump"))
				dumpFd = sceIoOpen("audio_dump.raw", PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC, 0777);
		}
	}
	if(pspTune.audioRate != 44100){
		// lower output rates (LCS/VCS-like sound) mix fewer samples:
		// blocks of about 23 ms, a multiple of 64 samples
		OUT_RATE = pspTune.audioRate;
		MIX_SAMPLES = OUT_RATE == 22050 ? 512 : OUT_RATE == 16000 ? 384 : 256;
		int r = sceAudioSRCChReserve(MIX_SAMPLES, OUT_RATE, 2);
		if(r >= 0){
			useSRC = true;
			hwChannel = 0;
		}else{
			logf("audio: no low rate channel (%08x), using 44100 Hz\n", r);
			OUT_RATE = 44100;
			MIX_SAMPLES = 1024;
		}
	}
	if(!useSRC)
		hwChannel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, MIX_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
	if(hwChannel < 0){
		logf("audio: no hardware channel (%08x)\n", hwChannel);
		return FALSE;
	}
	mixLock = sceKernelCreateSema("mixlock", 0, 1, 1, NULL);
	streamLock = sceKernelCreateSema("streamlock", 0, 1, 1, NULL);
	reqLock = sceKernelCreateSema("streamreq", 0, 1, 1, NULL);
	audioRunning = true;
	mixThread = sceKernelCreateThread("audio_mix", mixerMain, 0x12, 0x4000, PSP_THREAD_ATTR_USER, NULL);
	decodeThread = sceKernelCreateThread("audio_decode", decoderMain, 0x16, 0x4000, PSP_THREAD_ATTR_USER, NULL);
	if(mixThread >= 0) sceKernelStartThread(mixThread, 0, NULL);
	if(decodeThread >= 0) sceKernelStartThread(decodeThread, 0, NULL);

	LoadSampleBank(SFX_BANK_0);
	_bSampmanInitialised = TRUE;
	logf("audio: ready, %d Hz, sfx cache %d KB, music %d\n", OUT_RATE, cacheBudget/1024, codecModuleLoaded);
	return TRUE;
}

void
cSampleManager::Terminate(void)
{
	if(!_bSampmanInitialised)
		return;
	for(int i = 0; i < MAX_STREAMS; i++)
		closeStream(&streams[i]);
	audioRunning = false;
	if(mixThread >= 0){ sceKernelWaitThreadEnd(mixThread, NULL); sceKernelDeleteThread(mixThread); }
	if(decodeThread >= 0){ sceKernelWaitThreadEnd(decodeThread, NULL); sceKernelDeleteThread(decodeThread); }
	if(useSRC) sceAudioSRCChRelease();
	else if(hwChannel >= 0) sceAudioChRelease(hwChannel);
	for(int i = 0; i < MAX_STREAMS; i++)
		if(streams[i].codecReady)
			sceAudiocodecReleaseEDRAM(streams[i].codec);
	if(sfxFile >= 0) sceIoClose(sfxFile);
	_bSampmanInitialised = FALSE;
}

void
cSampleManager::UpdateEffectsVolume(void)
{
	for(int32 i = 0; i < NUM_CHANNELS; i++)
		updateChannelVolume(this, &channels[i], m_nEffectsVolume, m_nEffectsFadeVolume);
}

void cSampleManager::SetEffectsMasterVolume(uint8 nVolume) { m_nEffectsVolume = nVolume; UpdateEffectsVolume(); }
void cSampleManager::SetMusicMasterVolume(uint8 nVolume) { m_nMusicVolume = nVolume; }
void cSampleManager::SetMP3BoostVolume(uint8 nVolume) { m_nMP3BoostVolume = nVolume; }
void cSampleManager::SetEffectsFadeVolume(uint8 nVolume) { m_nEffectsFadeVolume = nVolume; UpdateEffectsVolume(); }
void cSampleManager::SetMusicFadeVolume(uint8 nVolume) { m_nMusicFadeVolume = nVolume; }
void cSampleManager::SetMonoMode(bool8 nMode) { m_nMonoMode = nMode; }

// The effects bank isn't loaded as a whole; samples come in on demand.
bool8 cSampleManager::LoadSampleBank(uint8 nBank) { return TRUE; }
void cSampleManager::UnloadSampleBank(uint8 nBank) { }
int8 cSampleManager::IsSampleBankLoaded(uint8 nBank) { return _bSampmanInitialised ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED; }

uint8
cSampleManager::IsMissionAudioLoaded(uint8 nSlot, uint32 nSample)
{
	return nSample == gPlayerTalkSfx ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
}

bool8
cSampleManager::LoadMissionAudio(uint8 nSlot, uint32 nSample)
{
	if(!_bSampmanInitialised || nSample >= TOTAL_AUDIO_SAMPLES || m_aSamples[nSample].nSize > gPlayerTalkDataSize)
		return FALSE;
	// read by the decoder thread; IsMissionAudioLoaded says when it's there
	if(pendingPlayerTalk == (int32)nSample || gPlayerTalkSfx == nSample)
		return TRUE;
	if(pendingPlayerTalk >= 0)
		return FALSE;	// one at a time
	pendingPlayerTalk = nSample;
	if(!ioPut(IOQ_PLAYERTALK, nSample, 0)){
		pendingPlayerTalk = -1;
		return FALSE;
	}
	return TRUE;
}

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	return _GetPedCommentSlot(nComment) >= 0 ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
}

int32
cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	for(int32 i = 0; i < 3; i++){
		int32 slot = (int32)nCurrentPedSlot - i - 1;
		if(slot < 0)
			slot += MAX_PEDSFX;
		if((int32)nComment == nPedSlotSfx[slot])
			return slot;
	}
	return -1;
}

bool8
cSampleManager::LoadPedComment(uint32 nComment)
{
	if(!_bSampmanInitialised || CTimer::GetIsCodePaused())
		return FALSE;
	if(MusicManager.IsInitialised() && MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE)
		return FALSE;
	uint32 size = m_aSamples[nComment].nSize;
	if(size > PED_BLOCKSIZE)
		return FALSE;
	// read by the decoder thread into the next slot; IsPedCommentLoaded
	// says when it's there
	if(pendingPedComment == (int32)nComment)
		return TRUE;
	if(pendingPedComment >= 0)
		return FALSE;	// one at a time
	int32 slot = nCurrentPedSlot;
	nPedSlotSfx[slot] = NO_SAMPLE;
	pendingPedComment = nComment;
	if(!ioPut(IOQ_PEDCOMMENT, nComment, slot)){
		pendingPedComment = -1;
		return FALSE;
	}
	if(++nCurrentPedSlot >= MAX_PEDSFX)
		nCurrentPedSlot = 0;
	return TRUE;
}

// decoder thread: everything in the file queue
static void
serviceIoQueue(void)
{
	// at most ~15 ms of loading per round: the streams are decoded in
	// between (a burst of sound effect loads used to let the music run dry)
	uint32 t0 = sceKernelGetSystemTimeLow();
	while(ioTail != ioHead && sceKernelGetSystemTimeLow() - t0 < 15000){
		IoReq r = ioQueue[ioTail];
		__sync_synchronize();
		ioTail = (ioTail + 1) & (IOQ_SIZE-1);
		tSample *samples = gSamples;
		switch(r.type){
		case IOQ_SFX:
			loadSampleIo(samples, r.sfx);
			break;
		case IOQ_PEDCOMMENT:{
			uint8 *dst = pedSlotData + PED_BLOCKSIZE*r.slot;
			lockMix();
			for(int i = 0; i < NUM_CHANNELS; i++)
				if(channels[i].data == (int16*)dst)
					channels[i].playing = false;
			unlockMix();
			if(readSfx(samples[r.sfx].nOffset, dst, samples[r.sfx].nSize)){
				__sync_synchronize();
				nPedSlotSfx[r.slot] = r.sfx;
			}
			pendingPedComment = -1;
			break;
		}
		case IOQ_PLAYERTALK:
			lockMix();
			for(int i = 0; i < NUM_CHANNELS; i++)
				if(channels[i].data == (int16*)gPlayerTalkData)
					channels[i].playing = false;
			gPlayerTalkSfx = UINT32_MAX;
			unlockMix();
			if(readSfx(samples[r.sfx].nOffset, gPlayerTalkData, samples[r.sfx].nSize)){
				__sync_synchronize();
				gPlayerTalkSfx = r.sfx;
			}
			pendingPlayerTalk = -1;
			break;
		}
	}
}

int32
cSampleManager::GetBankContainingSound(uint32 offset)
{
	if(offset >= SAMPLEBANK_PED_START)
		return SFX_BANK_PED_COMMENTS;
	return SFX_BANK_0;
}

uint32 cSampleManager::GetSampleBaseFrequency(uint32 nSample) { return m_aSamples[nSample].nFrequency; }
uint32 cSampleManager::GetSampleLoopStartOffset(uint32 nSample) { return m_aSamples[nSample].nLoopStart; }
int32 cSampleManager::GetSampleLoopEndOffset(uint32 nSample) { return m_aSamples[nSample].nLoopEnd; }
uint32 cSampleManager::GetSampleLength(uint32 nSample) { return m_aSamples[nSample].nSize / sizeof(uint16); }

bool8 cSampleManager::UpdateReverb(void) { return FALSE; }
void cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag) { }

static bool
sampleInUse(const int16 *data)
{
	for(int i = 0; i < NUM_CHANNELS; i++)
		if(channels[i].playing && channels[i].data == data)
			return true;
	return false;
}

// get an effects sample (game thread): from the cache, or queue it for
// the decoder thread and play nothing this time (it's there a few frames later)
static const int16*
getSample(cSampleManager *sm, tSample *samples, uint32 nSfx)
{
	CachedSample *cs = &sampleCache[nSfx];
	cs->lastUse = ++cacheClock;
	cs->lastUseMs = CTimer::GetTimeInMillisecondsPauseMode();
	if(cs->data)
		return cs->data;
	uint32 size = samples[nSfx].nSize;
	if(size == 0 || size > cacheBudget)
		return nil;
	if(!cs->loading){
		cs->loading = 1;
		if(!ioPut(IOQ_SFX, nSfx, 0))
			cs->loading = 0;
	}
	return nil;
}

// decoder thread: load one effects sample into the cache
static void
loadSampleIo(tSample *samples, uint32 nSfx)
{
	CachedSample *cs = &sampleCache[nSfx];
	uint32 size = samples[nSfx].nSize;
	uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	// make room: drop the least recently used samples nothing plays and
	// the game hasn't asked for in the last 2 seconds
	while(cacheUsed + size > cacheBudget){
		int32 oldest = -1;
		uint32 oldestUse = 0xFFFFFFFF;
		for(int32 i = 0; i < SAMPLEBANK_MAX; i++)
			if(sampleCache[i].data && sampleCache[i].lastUse < oldestUse && now - sampleCache[i].lastUseMs > 2000 &&
			   !sampleInUse(sampleCache[i].data)){
				oldest = i;
				oldestUse = sampleCache[i].lastUse;
			}
		if(oldest < 0){
			// Everything in the cache was asked for in the last 2 seconds
			// (driving through traffic): the oldest one that isn't playing
			// goes anyway. Before, the new sample was then never loaded: the
			// engine of a fast car or a flying helicopter went silent until
			// the throttle was let go (another sample).
			for(int32 i = 0; i < SAMPLEBANK_MAX; i++)
				if(sampleCache[i].data && sampleCache[i].lastUse < oldestUse && now - sampleCache[i].lastUseMs > 200 &&
				   !sampleInUse(sampleCache[i].data)){
					oldest = i;
					oldestUse = sampleCache[i].lastUse;
				}
		}
		if(oldest < 0){
			static uint32 lastLog;
			if(now - lastLog > 5000){
				lastLog = now;
				logf("audio: no room in the sound cache for sample %d (%d KB)\n", nSfx, size/1024);
			}
			cs->loading = 0;
			return;
		}
		lockMix();
		// channels that were set up with it but aren't playing forget it
		for(int i = 0; i < NUM_CHANNELS; i++)
			if(channels[i].data == sampleCache[oldest].data){
				channels[i].playing = false;
				channels[i].data = nil;
			}
		int16 *old = sampleCache[oldest].data;
		sampleCache[oldest].data = nil;
		unlockMix();
		free(old);
		cacheUsed -= samples[oldest].nSize;
	}
	int16 *data = (int16*)malloc(size);
	if(data == nil){
		cs->loading = 0;
		return;
	}
	if(!readSfx(samples[nSfx].nOffset, data, size)){
		free(data);
		cs->loading = 0;
		return;
	}
	cacheUsed += size;
	__sync_synchronize();
	cs->data = data;
	cs->loading = 0;
}

bool8
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	statInit++;
	if(!_bSampmanInitialised || nChannel >= NUM_CHANNELS || nSfx >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	const int16 *data;
	if(nSfx < SAMPLEBANK_MAX)
		data = getSample(this, m_aSamples, nSfx);
	else if(nSfx >= PLAYER_COMMENTS_START && nSfx <= PLAYER_COMMENTS_END){
		if(!IsMissionAudioLoaded(MISSION_AUDIO_PLAYER_COMMENT, nSfx))
			return FALSE;
		data = (int16*)gPlayerTalkData;
	}else{
		int32 slot = _GetPedCommentSlot(nSfx);
		if(slot < 0)
			return FALSE;
		data = (int16*)(pedSlotData + PED_BLOCKSIZE*slot);
	}
	if(data == nil)
		return FALSE;
	MixChannel *c = &channels[nChannel];
	lockMix();
	c->playing = false;
	c->data = data;
	c->length = m_aSamples[nSfx].nSize / 2;
	c->pos = 0;
	c->frac = 0;
	c->freq = m_aSamples[nSfx].nFrequency;
	c->step = ((uint64)c->freq << 16) / OUT_RATE;
	c->loopStart = 0;
	c->loopEnd = -1;
	c->loopCount = 1;
	c->sfx = nSfx;
	unlockMix();
	statInitOk++;
	return TRUE;
}

void
cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	if(nChannel >= NUM_CHANNELS) return;
	MixChannel *c = &channels[nChannel];
	c->volume = Min(nVolume, (uint32)MAX_VOLUME);
	if(MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE)
		c->volume >>= 2;
	updateChannelVolume(this, c, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	if(nChannel >= NUM_CHANNELS) return;
	MixChannel *c = &channels[nChannel];
	c->pan = Min(nPan, (uint32)127);
	if(m_nMonoMode)
		c->pan = 63;
	updateChannelVolume(this, c, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void
cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	if(nChannel >= NUM_CHANNELS) return;
	channels[nChannel].freq = nFreq;
	channels[nChannel].step = ((uint64)nFreq << 16) / OUT_RATE;
}

void
cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	if(nChannel >= NUM_CHANNELS) return;
	channels[nChannel].loopStart = nLoopStart / (DIGITALBITS / 8);
	channels[nChannel].loopEnd = nLoopEnd < 0 ? -1 : nLoopEnd / (DIGITALBITS / 8);
}

void
cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	if(nChannel >= NUM_CHANNELS) return;
	channels[nChannel].loopCount = nLoopCount;
}

bool8
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	return nChannel < NUM_CHANNELS && channels[nChannel].playing;
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	if(nChannel >= NUM_CHANNELS || channels[nChannel].data == nil) return;
	statStart++;
	channels[nChannel].pos = 0;
	channels[nChannel].frac = 0;
	channels[nChannel].playing = true;
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	if(nChannel >= NUM_CHANNELS) return;
	channels[nChannel].playing = false;
}

//
// Streams
//

static void
updateStreamVolume(MixStream *s, uint32 vol)
{
	if(vol > MAX_VOLUME) vol = MAX_VOLUME;
	int32 pan = s->pan;
	int32 l = pan <= 63 ? 256 : (127 - pan) * 256 / 64;
	int32 r = pan >= 64 ? 256 : pan * 256 / 63;
	s->volL = vol * l / 127;
	s->volR = vol * r / 127;
}

void
cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	if(!_bSampmanInitialised || nStream >= MAX_STREAMS || nFile >= TOTAL_STREAMED_SOUNDS)
		return;
	postStreamRequest(&streams[nStream], REQ_OPEN, nFile, 0, false, false);
}

void
cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return;
	streams[nStream].paused = nPauseFlag != FALSE;
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return;
	MixStream *s = &streams[nStream];
	lockReq();
	if(s->pending && s->reqOp == REQ_OPEN){
		s->reqPlay = true;	// starts as soon as it's open
		s->paused = false;
	}else if(s->opened){
		s->paused = false;
		s->playing = true;
	}
	unlockReq();
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	if(!_bSampmanInitialised || nStream >= MAX_STREAMS)
		return FALSE;
	if(nFile >= TOTAL_STREAMED_SOUNDS || nFile == STREAMED_SOUND_RADIO_MP3_PLAYER)
		nFile = 0;
	MixStream *s = &streams[nStream];
	bool loop = nStreamLoopedFlag[nStream];
	nStreamLoopedFlag[nStream] = TRUE;
	if(s->ring == nil)
		return FALSE;
	s->paused = false;
	postStreamRequest(s, REQ_OPEN, nFile, nPos, true, loop);
	return TRUE;
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return;
	postStreamRequest(&streams[nStream], REQ_CLOSE, 0, 0, false, false);
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return 0;
	MixStream *s = &streams[nStream];
	if(s->pending)
		return s->reqOp == REQ_OPEN ? s->reqPos : 0;
	if(!s->opened || s->sampleRate == 0)
		return 0;
	uint32 ms = s->startMS + (uint32)(s->framesPlayed * 1000 / s->sampleRate);
	if(s->loop && s->lengthMS)
		ms %= s->lengthMS;
	return ms;
}

void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return;
	if(nVolume > MAX_VOLUME) nVolume = MAX_VOLUME;
	if(nPan > MAX_VOLUME) nPan = MAX_VOLUME;
	MixStream *s = &streams[nStream];
	s->pan = m_nMonoMode ? 63 : nPan;
	uint32 vol;
	if(nEffectFlag){
		if(nStream == 1 || nStream == 2)
			vol = 128*nVolume*m_nEffectsVolume >> 14;
		else
			vol = m_nEffectsFadeVolume*nVolume*m_nEffectsVolume >> 14;
	}else
		vol = m_nMusicFadeVolume*nVolume*m_nMusicVolume >> 14;
	updateStreamVolume(s, vol);
}

static void computeStreamLength(uint32 track);

// decoder thread: work out every track's length in the background, one file
// every 150 ms, so the game rarely has to (radio stations first)
static void
backgroundLengths(void)
{
	static uint32 next, lastMs;
	if(next >= TOTAL_STREAMED_SOUNDS || !_bSampmanInitialised)
		return;
	uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	if(now - lastMs < 150)
		return;
	lastMs = now;
	if(nStreamLength[next] == 0)
		computeStreamLength(next);
	next++;
}

// length of a track in ms (works out the length once per track)
int32
cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	uint32 track = nStream;
	if(track >= TOTAL_STREAMED_SOUNDS)
		return 1;
	if(nStreamLength[track] == 0 && _bSampmanInitialised)
		computeStreamLength(track);
	return nStreamLength[track] ? nStreamLength[track] : 1;
}

static void
computeStreamLength(uint32 track)
{
	{
		char path[192];
		fixPath(path, StreamedNameTable[track], sizeof(path));
		SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
		if(fd >= 0){
			int len = strlen(path);
			bool adf = len > 4 && (path[len-3] == 'A' || path[len-3] == 'a');
			bool wav = len > 4 && (path[len-3] == 'W' || path[len-3] == 'w');
			uint32 size = sceIoLseek32(fd, 0, PSP_SEEK_END);
			if(wav){
				MixStream tmp;	// (both threads may be here)
				memset(&tmp, 0, sizeof(tmp));
				tmp.fd = fd;
				tmp.fileSize = size;
				if(parseWav(&tmp))
					nStreamLength[track] = tmp.lengthMS;
				sceIoClose(fd);
				if(nStreamLength[track] == 0)
					nStreamLength[track] = 1;
				return;
			}
			uint32 start = adf ? 0 : id3Size(fd);
			uint8 buf[4096];
			sceIoLseek32(fd, start, PSP_SEEK_SET);
			int n = sceIoRead(fd, buf, sizeof(buf));
			for(int i = 0; i + 4 <= n; i++){
				uint8 h[4];
				for(int k = 0; k < 4; k++) h[k] = adf ? buf[i+k]^0x22 : buf[i+k];
				Mp3Header hdr;
				if(parseHeader(h, &hdr)){
					nStreamLength[track] = (uint32)((uint64)(size - start) * 8000 / hdr.bitrate);
					break;
				}
			}
			sceIoClose(fd);
		}
		if(nStreamLength[track] == 0)
			nStreamLength[track] = 1;
	}
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	if(nStream >= MAX_STREAMS) return FALSE;
	MixStream *s = &streams[nStream];
	if(s->pending)
		return s->reqOp == REQ_OPEN && s->reqPlay;	// starting
	if(!s->opened || !s->playing)
		return FALSE;
	// finished when the file is done and everything decoded was played
	if(s->eof && ringFill(s) < 64)
		return FALSE;
	return TRUE;
}

void
cSampleManager::Service(void)
{
	static int frames;
	extern int pspAudioLog;
	if(pspAudioLog && frames % 15 == 0){
		// testing aid: which sounds are playing
		char b[400], *p = b;
		p += sprintf(p, "AUDIOLOG");
		for(int i = 0; i < NUM_CHANNELS && p - b < 340; i++)
			if(channels[i].playing || channels[i].data)
				p += sprintf(p, " %d:%c%d/%u/%u", i, channels[i].playing ? 'P' : '-', channels[i].sfx, (unsigned)channels[i].freq, (unsigned)channels[i].volume);
		strcpy(p, "\n");
		pspLog(b);
	}
	if(pspTune.showFps && ++frames % 600 == 0){
		int playing = 0;
		for(int i = 0; i < NUM_CHANNELS; i++)
			if(channels[i].playing) playing++;
		char b[160];
		snprintf(b, sizeof(b), "audio: channels playing %d, inits %d ok %d starts %d, cache %d KB, streams %d%d%d\n",
			playing, statInit, statInitOk, statStart, (int)(cacheUsed/1024),
			streams[0].playing, streams[1].playing, streams[2].playing);
		pspLog(b);
		statInit = statInitOk = statStart = 0;
	}
}

bool8
cSampleManager::InitialiseSampleBanks(void)
{
	SceUID fd = sceIoOpen("AUDIO/SFX.SDT", PSP_O_RDONLY, 0);
	if(fd < 0)
		return FALSE;
	sceIoRead(fd, m_aSamples, sizeof(tSample) * TOTAL_AUDIO_SAMPLES);
	sceIoClose(fd);
	sfxFile = sceIoOpen("AUDIO/SFX.RAW", PSP_O_RDONLY, 0);
	if(sfxFile < 0)
		return FALSE;
	sfxFileSize = sceIoLseek32(sfxFile, 0, PSP_SEEK_END);
	return TRUE;
}

void
cSampleManager::SetStreamedFileLoopFlag(bool8 nLoopFlag, uint8 nStream)
{
	if(nStream < MAX_STREAMS)
		nStreamLoopedFlag[nStream] = nLoopFlag;
}

#endif
