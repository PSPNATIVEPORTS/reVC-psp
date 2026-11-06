// reVC crash catcher: a small kernel module the game loads at start. When
// the game crashes (an exception the PSP would freeze on), it writes the
// registers and the stack to reVC_crash.txt next to EBOOT.PBP and shows
// them on screen, so the place in the code can be found.
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <string.h>

PSP_MODULE_INFO("reVCcrash", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

static char crashPath[200];
static char buf[4096];

// tiny formatter (the kernel's libc has no printf): %08x and text only
static int
append(int n, const char *fmt, unsigned a, unsigned b, unsigned c, unsigned d)
{
	unsigned v[4] = { a, b, c, d };
	int k = 0;
	for(const char *p = fmt; *p && n < (int)sizeof(buf) - 12; p++){
		if(p[0] == '%' && p[1] == '0' && p[2] == '8' && p[3] == 'x'){
			unsigned x = k < 4 ? v[k++] : 0;
			for(int i = 7; i >= 0; i--)
				buf[n++] = "0123456789abcdef"[(x >> (i*4)) & 15];
			p += 3;
		}else
			buf[n++] = *p;
	}
	buf[n] = 0;
	return n;
}

PspDebugRegBlock crashRegs;
volatile unsigned int crashFlag;
unsigned int crashParkAddr;
void crashExceptionHandler(void);
int sceKernelRegisterDefaultExceptionHandler(void *func);

static void
report(PspDebugRegBlock *regs)
{
	int n = 0;
	n = append(n, "reVC CRASH: exception cause %08x epc %08x badvaddr %08x ra %08x\n",
		regs->cause, regs->epc, regs->badvaddr, regs->r[31]);
	n = append(n, "sp %08x status %08x %08x %08x\n", regs->r[29], regs->status, 0, 0);
	for(int i = 0; i < 32; i += 4)
		n = append(n, "%08x %08x %08x %08x\n", regs->r[i], regs->r[i+1], regs->r[i+2], regs->r[i+3]);
	// return addresses on the stack (the code addresses among them)
	unsigned sp = regs->r[29];
	if(sp >= 0x08800000 && sp < 0x0A000000 - 1024){
		n = append(n, "stack:\n", 0, 0, 0, 0);
		unsigned *s = (unsigned*)sp;
		for(int i = 0; i < 256; i += 4)
			n = append(n, "%08x %08x %08x %08x\n", s[i], s[i+1], s[i+2], s[i+3]);
	}
	if(crashPath[0]){
		SceUID fd = sceIoOpen(crashPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if(fd >= 0){
			sceIoWrite(fd, buf, n);
			sceIoClose(fd);
		}
	}
	pspDebugScreenInit();
	pspDebugScreenSetBackColor(0x00400000);
	pspDebugScreenSetTextColor(0xFFFFFFFF);
	pspDebugScreenClear();
	pspDebugScreenPuts("GTA Vice City crashed. Saved to reVC_crash.txt\n");
	pspDebugScreenPuts("Please send this file. Hold the power switch to turn off.\n\n");
	buf[1500] = 0;
	pspDebugScreenPuts(buf);
}

// waits for the exception handler's flag, then writes the report
static int
watcher(SceSize args, void *argp)
{
	for(;;){
		if(crashFlag){
			report(&crashRegs);
			for(;;)
				sceKernelDelayThread(1000000);
		}
		sceKernelDelayThread(50000);
	}
	return 0;
}

// args: the path of reVC_crash.txt, a 0, then the address (4 bytes) of
// the game's parking loop for the crashed thread
int
module_start(SceSize args, void *argp)
{
	const char *a = (const char*)argp;
	if(args < 6 || argp == 0)
		return -1;
	int len = 0;
	while(len < (int)args && a[len] && len < (int)sizeof(crashPath) - 1){
		crashPath[len] = a[len];
		len++;
	}
	crashPath[len] = 0;
	if(len + 1 + 4 > (int)args)
		return -2;
	memcpy(&crashParkAddr, a + len + 1, 4);
	SceUID th = sceKernelCreateThread("reVCcrashwatch", watcher, 0x10, 0x2000, 0, 0);
	if(th >= 0)
		sceKernelStartThread(th, 0, 0);
	return sceKernelRegisterDefaultExceptionHandler((void*)((unsigned)crashExceptionHandler | 0x80000000));
}

int
module_stop(SceSize args, void *argp)
{
	return 0;
}

// pspdebug's printf wants this; it isn't used for anything that matters
#include <stdarg.h>
int
vsnprintf(char *out, unsigned int size, const char *fmt, va_list ap)
{
	unsigned int i;
	for(i = 0; fmt[i] && i + 1 < size; i++)
		out[i] = fmt[i];
	if(size) out[i] = 0;
	return i;
}
