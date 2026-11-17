## Copyright (C) 2001-2019 Peter Selinger.
## This file is part of Potrace. It is free software and it is covered
## by the GNU General Public License. See the file COPYING for details.

## Process this file with automake to produce Makefile.in

AM_CFLAGS = $(CADD)
AM_CPPFLAGS = -I$(top_srcdir)/src

check_PROGRAMS = pgmdiff checkbin batch-check batch-bench
check_SCRIPTS = input-check.sh backend-check.sh potrace-check.sh postscript-check.sh pdf-check.sh mkbitmap-check.sh mkbitmap-input-check.sh binary-check.sh

TESTS = $(check_SCRIPTS) batch-check

INPUTS = data1.pbm data1.pgm data1.ppm data1.pbm.plain data1.pgm.plain data1.ppm.plain data1.bmp1 data1.bmp4 data1.bmp8 data1.bmp24 data1.bmp24td data1.bmp24v5 data1.bmp32 data1.bmp32bf data1.bmp4r data1.bmp8r data1-out.pgm data1.pbm.gs data1.pbm.rot data1.pgm.rot data1.pgm.inv data2.ppm data2.bmp32bf data2-out1.pbm data2-out2.pbm data3.pgm

EXTRA_DIST = $(check_SCRIPTS) missing.sh $(INPUTS:%=data/%)

pgmdiff_SOURCES = pgmdiff.c
pgmdiff_LDADD = ../src/greymap.o -lm
pgmdiff_DEPENDENCIES = ../src/greymap.o

checkbin_SOURCES = checkbin.c

batch_check_SOURCES = batch-check.c
batch_check_LDADD = ../src/libpotrace.la -lm $(PTHREAD_LIBS)
batch_check_DEPENDENCIES = ../src/libpotrace.la

batch_bench_SOURCES = batch-bench.c
batch_bench_LDADD = ../src/libpotrace.la -lm $(PTHREAD_LIBS)
batch_bench_DEPENDENCIES = ../src/libpotrace.la

../src/greymap.o:
	(cd ../src && $(MAKE) $(AM_MAKEFLAGS) greymap.o)
