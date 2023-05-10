arch=i686-pc-mingw32
srcdir=../../src
prefix=/usr/local/$(arch)

NSIS_ARCH=i586
NSIS_PROGRAMFILES=PROGRAMFILES
NSIS_REGVIEW=32

PROGRAM=hpgs.exe
SLIB=hpgs$(VERSION).dll
IMPLIB=libhpgs.dll.a
SLIBALIAS1=hpgs$(MVERSION).dll
SLIBALIAS2=hpgs.dll
slibsfx=bin
OBJ=o
CC=$(arch)-gcc
STRIP=$(arch)-strip
LNS=ln -sf

COPT=-g -O2
#COPT=-g
#COPT=-O3 -march=pentium4 -msse2 -mfpmath=sse

CFLAGS=$(COPT) -Wall -DHPGS_SHARED -I$(srcdir)
LIBCFLAGS=-DHPGS_BUILD_SLIB -DHPGS_HAVE_GETTEXT
LDFLAGS=
LIBS=-lpng -lz -lm -liconv -lintl

SLD=$(arch)-gcc
SLDFLAGS=-shared -Wl,-dll -Wl,--out-implib=$(IMPLIB)

include $(srcdir)/include.mak
