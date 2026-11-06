// pspconv: converts GTA Vice City PC data for the reVC PSP port.
//
// Run it on a PC (Windows/Linux) against your own copy of the game:
//   pspconv <GTA VC folder> <output folder>
//
// It writes PSP versions of:
//   models/gta3.img + gta3.dir  (all textures converted, everything else copied)
//   every loose .txd (models/, models/generic/, txd/)
// Textures are shrunk, palettized (4/8 bit) and swizzled for the PSP GE,
// so the PSP never has to decode or resize textures while you play.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <set>
#include <sys/stat.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#define mkdir_p(p) mkdir(p, 0755)
#endif

#include <rw.h>

using namespace rw;

static int gWorldMax = 128;
static bool gLite;	// PSP-1000: everything smaller, world textures 16 colours
static bool gLow;	// PSP-2000/3000 "low": world textures keep their size but get 16 colours
static std::set<std::string> gKeepColours;	// peds, vehicles, weapons: stay 256 colours in "low"
static int gNumConverted, gNumFailed;

static std::string
lower(std::string s)
{
	for(auto &c : s) c = tolower(c);
	return s;
}

static bool
endsWith(const std::string &s, const char *suf)
{
	size_t n = strlen(suf);
	return s.size() >= n && lower(s.substr(s.size()-n)) == suf;
}

// Maximum texture size by TXD name
static bool
isUiTxd(const std::string &txdName)
{
	std::string n = lower(txdName);
	return n.find("loadsc") == 0 || n.find("intro") == 0 || n.find("splash") == 0 ||
		n == "news.txd" || n == "outro.txd" || n.find("font") == 0 ||
		n.find("fronten") == 0 || n.find("frontend") == 0 || n.find("btns") != std::string::npos ||
		n == "hud.txd" || n == "misc.txd";
}

// cutscene characters (csplay.txd, cskent.txd...): close-ups of faces,
// they keep 256 colours and get bigger textures
static bool
isCutsceneCharTxd(const std::string &txdName)
{
	std::string n = lower(txdName);
	return n.size() > 2 && n.compare(0, 2, "cs") == 0 && n[2] != '_';
}

static int
maxSizeFor(const std::string &txdName)
{
	std::string n = lower(txdName);
	if(isCutsceneCharTxd(n))
		return gLite ? 128 : 256;
	if(gLite){
		if(n.find("font") == 0)
			return 256;	// text has to stay readable
		if(n.find("loadsc") == 0 || n.find("intro") == 0 || n.find("splash") == 0 ||
		   n == "news.txd" || n == "outro.txd")
			return 128;
		if(n.find("fronten") == 0 || n.find("frontend") == 0 || n.find("btns") != std::string::npos)
			return 128;
		if(n == "hud.txd" || n == "misc.txd")
			return 64;
		if(n == "particle.txd" || n.find("lod") != std::string::npos)
			return 32;
		return gWorldMax;
	}
	if(n.find("loadsc") == 0 || n.find("intro") == 0 || n.find("splash") == 0 ||
	   n == "news.txd" || n == "outro.txd")
		return 256;	// loading screens
	if(n.find("font") == 0)
		return 256;
	if(n.find("fronten") == 0 || n.find("frontend") == 0 || n.find("btns") != std::string::npos)
		return 256;
	if(n == "hud.txd" || n == "misc.txd")
		return 128;
	if(n == "particle.txd")
		return 64;
	if(n.find("lod") != std::string::npos)
		return 64;
	return gWorldMax;
}

static int
palettizeFor(const std::string &txdName)
{
	if(isUiTxd(txdName) || isCutsceneCharTxd(txdName))
		return 1;
	if(gLite)
		return 2;
	if(gLow){
		std::string n = lower(txdName);
		size_t dot = n.rfind('.');
		if(dot != std::string::npos)
			n = n.substr(0, dot);
		return gKeepColours.count(n) ? 1 : 2;
	}
	return 1;
}

// txd names of peds, vehicles and weapons from data/default.ide
static void readKeepColoursFrom(const std::string &path);
static void
readKeepColours(const std::string &srcDir)
{
	readKeepColoursFrom(srcDir + "/data/default.ide");
	readKeepColoursFrom(srcDir + "/data/peds.ide");	// people (if the game has this file)
	gKeepColours.insert("player");
	// Tommy's other outfits (player2..player9, play10..play12)
	for(int i = 2; i <= 9; i++)
		gKeepColours.insert("player" + std::to_string(i));
	for(int i = 10; i <= 12; i++)
		gKeepColours.insert("play" + std::to_string(i));
	printf("%d ped/vehicle/weapon texture sets keep 256 colours\n", (int)gKeepColours.size());
}

static void
readKeepColoursFrom(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil){
		if(path.find("default.ide") != std::string::npos)
			fprintf(stderr, "can't read %s, world textures get 16 colours\n", path.c_str());
		return;
	}
	char line[512];
	bool keep = false;
	while(fgets(line, sizeof(line), f)){
		std::string l = lower(line);
		size_t a = l.find_first_not_of(" \t\r\n");
		if(a == std::string::npos || l[a] == '#')
			continue;
		l = l.substr(a);
		if(l.compare(0, 3, "end") == 0 && (l.size() == 3 || !isalnum((unsigned char)l[3]))){
			keep = false;
			continue;
		}
		if(l.compare(0, 4, "peds") == 0 || l.compare(0, 4, "cars") == 0 || l.compare(0, 4, "weap") == 0){
			keep = true;
			continue;
		}
		if(!keep || !isdigit((unsigned char)l[0]))
			continue;
		// id, model, txd, ...
		size_t c1 = l.find(','), c2 = c1 == std::string::npos ? c1 : l.find(',', c1+1);
		if(c2 == std::string::npos)
			continue;
		std::string txd = l.substr(c1+1, c2-c1-1);
		size_t b = txd.find_first_not_of(" \t"), e = txd.find_last_not_of(" \t");
		if(b != std::string::npos)
			gKeepColours.insert(txd.substr(b, e-b+1));
	}
	fclose(f);
}

// Convert all textures of a TXD in memory, return new TXD bytes
static bool
convertTxd(const uint8 *data, uint32 size, const std::string &name, std::vector<uint8> &out)
{
	StreamMemory in;
	in.open((uint8*)data, size);
	if(!findChunk(&in, ID_TEXDICTIONARY, nil, nil)){
		in.close();
		return false;
	}
	TexDictionary *txd = TexDictionary::streamRead(&in);
	in.close();
	if(txd == nil)
		return false;

	int maxSize = maxSizeFor(name);
	FORLIST(lnk, txd->textures){
		Texture *tex = Texture::fromDict(lnk);
		Raster *ras = tex->raster;
		int pal = palettizeFor(name);
		// already converted textures are only redone for the lite/low data
		if(ras == nil)
			continue;
		if(ras->platform == PLATFORM_PSP){
			if(gLow){
				if(pal != 2 || ras->depth != 8)
					continue;
			}else if(!gLite)
				continue;
		}
		Image *img = ras->toImage();
		if(img == nil){
			gNumFailed++;
			continue;
		}
		Raster *n = psp::rasterFromImageEx(img, maxSize, pal, 1);
		img->destroy();
		if(n == nil){
			gNumFailed++;
			continue;
		}
		ras->destroy();
		tex->raster = n;
		gNumConverted++;
	}

	uint32 sz = txd->streamGetSize() + 12;
	out.resize(sz + 64);
	StreamMemory os;
	os.open(out.data(), 0, out.size());
	txd->streamWrite(&os);
	out.resize(os.getLength());
	os.close();
	txd->destroy();
	return true;
}

static bool
readFile(const std::string &path, std::vector<uint8> &buf)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf.resize(n);
	bool ok = fread(buf.data(), 1, n, f) == (size_t)n;
	fclose(f);
	return ok;
}

static bool
writeFile(const std::string &path, const std::vector<uint8> &buf)
{
	FILE *f = fopen(path.c_str(), "wb");
	if(f == nil)
		return false;
	fwrite(buf.data(), 1, buf.size(), f);
	fclose(f);
	return true;
}

struct DirEntry
{
	uint32 offset;	// sectors
	uint32 size;	// sectors
	char name[24];
};

static bool
convertImg(const std::string &srcDir, const std::string &dstDir)
{
	std::string dirPath = srcDir + "/models/gta3.dir";
	std::string imgPath = srcDir + "/models/gta3.img";
	std::vector<uint8> dirData;
	if(!readFile(dirPath, dirData)){
		fprintf(stderr, "can't read %s\n", dirPath.c_str());
		return false;
	}
	FILE *img = fopen(imgPath.c_str(), "rb");
	if(img == nil){
		fprintf(stderr, "can't read %s\n", imgPath.c_str());
		return false;
	}
	int n = dirData.size()/sizeof(DirEntry);
	DirEntry *entries = (DirEntry*)dirData.data();

	std::string outImg = dstDir + "/models/gta3.img";
	FILE *out = fopen(outImg.c_str(), "wb");
	if(out == nil){
		fprintf(stderr, "can't write %s\n", outImg.c_str());
		return false;
	}
	std::vector<DirEntry> newEntries(n);
	uint32 sector = 0;
	std::vector<uint8> buf, conv;
	uint64_t inBytes = 0, outBytes = 0;
	for(int i = 0; i < n; i++){
		DirEntry &e = entries[i];
		buf.resize(e.size*2048);
		fseek(img, (long)e.offset*2048, SEEK_SET);
		if(fread(buf.data(), 1, buf.size(), img) != buf.size())
			fprintf(stderr, "short read for %s\n", e.name);
		std::string name(e.name, strnlen(e.name, 24));
		const std::vector<uint8> *data = &buf;
		if(endsWith(name, ".txd")){
			if(convertTxd(buf.data(), buf.size(), name, conv))
				data = &conv;
			else
				fprintf(stderr, "couldn't convert %s, copying\n", name.c_str());
		}
		uint32 sectors = (data->size() + 2047)/2048;
		std::vector<uint8> padded(*data);
		padded.resize(sectors*2048, 0);
		fwrite(padded.data(), 1, padded.size(), out);
		newEntries[i] = e;
		newEntries[i].offset = sector;
		newEntries[i].size = sectors;
		sector += sectors;
		inBytes += buf.size();
		outBytes += padded.size();
		if((i % 500) == 0)
			printf("  %d/%d %s\n", i, n, name.c_str());
	}
	fclose(out);
	fclose(img);
	std::vector<uint8> newDir((uint8*)newEntries.data(), (uint8*)newEntries.data() + n*sizeof(DirEntry));
	writeFile(dstDir + "/models/gta3.dir", newDir);
	printf("gta3.img: %d entries, %.1f MB -> %.1f MB\n", n, inBytes/1048576.0, outBytes/1048576.0);
	return true;
}

static void
convertLooseTxd(const std::string &srcDir, const std::string &dstDir, const char *rel)
{
	std::vector<uint8> buf, conv;
	std::string src = srcDir + "/" + rel;
	if(!readFile(src, buf))
		return;	// not present in this copy
	std::string name = rel;
	size_t slash = name.find_last_of("/\\");
	if(slash != std::string::npos)
		name = name.substr(slash+1);
	if(convertTxd(buf.data(), buf.size(), name, conv)){
		writeFile(dstDir + "/" + rel, conv);
		printf("%s: %u -> %u bytes\n", rel, (unsigned)buf.size(), (unsigned)conv.size());
	}else
		fprintf(stderr, "couldn't convert %s\n", rel);
}

int
main(int argc, char *argv[])
{
	if(argc < 3){
		printf("usage: %s <GTA Vice City folder> <output folder> [max texture size, default 128 | low | lite]\n"
		       "  low: world textures with 16 colours (less memory, works on already converted data)\n"
		       "  lite: for the PSP-1000 (32 MB): 64 px world textures with 16 colours\n", argv[0]);
		return 1;
	}
	std::string src = argv[1], dst = argv[2];
	if(argc > 3){
		if(lower(argv[3]) == "lite"){
			gLite = true;
			gWorldMax = 64;
			printf("PSP-1000 lite data: world textures 64 px, 16 colours\n");
		}else if(lower(argv[3]) == "low"){
			gLow = true;
			printf("low data: world textures 128 px with 16 colours, people and vehicles unchanged\n");
		}else
			gWorldMax = atoi(argv[3]);
	}
	if(gLow)
		readKeepColours(src);

	Engine::init();
	registerMeshPlugin();
	registerNativeDataPlugin();
	registerAnisotropyPlugin();
	Engine::open(nil);
	Engine::start();
	d3d::isP8supported = 1;	// keep palettes as palettes

	mkdir_p(dst.c_str());
	mkdir_p((dst + "/models").c_str());
	mkdir_p((dst + "/models/generic").c_str());
	mkdir_p((dst + "/txd").c_str());

	static const char *loose[] = {
		"models/fonts.txd", "models/fonts_r.txd", "models/fonts_p.txd", "models/fonts_j.txd",
		"models/fronten1.txd", "models/fronten2.txd",
		"models/frontend_ds2.txd", "models/frontend_ds3.txd", "models/frontend_ds4.txd",
		"models/frontend_nsw.txd", "models/frontend_x360.txd", "models/frontend_xone.txd",
		"models/generic.txd", "models/hud.txd", "models/INTRO.TXD", "models/MISC.TXD",
		"models/particle.txd", "models/ps3btns.txd", "models/x360btns.txd", "models/nswbtns.txd",
		"models/generic/wheels.TXD",
		"txd/intro1.txd", "txd/intro2.txd", "txd/INTRO3.TXD", "txd/intro4.txd",
		"txd/LOADSC0.TXD", "txd/LOADSC1.TXD", "txd/LOADSC2.TXD", "txd/LOADSC3.TXD",
		"txd/LOADSC4.TXD", "txd/LOADSC5.TXD", "txd/LOADSC6.TXD", "txd/LOADSC7.TXD",
		"txd/LOADSC8.TXD", "txd/LOADSC9.TXD", "txd/LOADSC10.TXD", "txd/LOADSC11.TXD",
		"txd/LOADSC12.TXD", "txd/LOADSC13.TXD", "txd/NEWS.TXD", "txd/outro.txd",
		"txd/SPLASH1.TXD", "txd/SPLASH2.TXD", "txd/SPLASH3.TXD",
		nil
	};
	for(int i = 0; loose[i]; i++)
		convertLooseTxd(src, dst, loose[i]);

	if(!convertImg(src, dst))
		return 1;

	printf("done: %d textures converted, %d failed\n", gNumConverted, gNumFailed);
	return 0;
}
