TARGET = revccrash
OBJS = main.o exception_asm.o
BUILD_PRX = 1
PRX_EXPORTS =
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
INCDIR =
CFLAGS = -O2 -G0 -Wall
LIBDIR =
LDFLAGS = -nostartfiles
LIBS = -lpspdebug -lpspdisplay_driver -lpspge_driver -lpspkernel
PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
