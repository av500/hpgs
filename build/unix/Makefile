srcdir=../../src
prefix=/usr/local

PROGRAM=hpgs
LIBRARY=libhpgs.a
OBJ=o
CC=gcc
STRIP=strip
LNS=ln -s

COPT=-g -O2
#COPT=-g
#COPT=-O3 -march=pentium4 -msse2 -mfpmath=sse

CFLAGS=$(COPT) -Wall -I$(srcdir)
LDFLAGS=
LIBS=-lpng12 -lz -lm -lpthread -ldl

AR=ar
ARFLAGS=cq

include $(srcdir)/include.mak
