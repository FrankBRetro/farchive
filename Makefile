# FArchive - GEM archiver for TOS / MultiTOS / MiNT
#
#   make                 every build: release and log, for each CPU below
#   make release         release builds only
#   make log             log builds only
#   make VARIANTS=000    just the 68000 builds (any list of the variants)
#   make arctest         host test tool for the archive formats (native gcc)
#   make rsc             rebuild the resource files from rsc/lang/*.txt
#
# Needs the MiNT cross toolchain (m68k-atari-mint-gcc, or the ELF one with
# CROSS=m68k-atari-mintelf-) with mintlib and gemlib. Do not use -mshort.
#
# CPU variants, each with FARCHIVE.PRG and its worker FAWORK.TTP - copy
# both of one build into the same folder:
#   000      68000, runs on every Atari          -> release/, log/
#   030      68030 code, no FPU (TT, Falcon)     -> release/030/, log/030/
#   030fpu   68030 + 68881/68882 FPU code        -> release/030fpu/, log/030fpu/
#   040      68040                               -> release/040/, log/040/
#   cf       ColdFire V4e (FireBee)              -> release/cf/, log/cf/
# Log builds write FARCHIVE.LOG next to the program.

CROSS   ?= m68k-atari-mint-
CC       = $(CROSS)gcc
STRIP    = $(CROSS)strip
CFLAGS  ?= -O2 -fomit-frame-pointer
CFLAGS  += -Wall -Wno-pointer-sign
LDLIBS   = -lgem

VARIANTS ?= 000 030 030fpu 040 cf
FLAGS_000 = -m68000
FLAGS_030 = -m68030 -msoft-float
FLAGS_030fpu = -m68030 -m68881
FLAGS_040 = -m68040
FLAGS_cf  = -mcpu=5475
DIR_000   =
DIR_030   = /030
DIR_030fpu = /030fpu
DIR_040   = /040
DIR_cf    = /cf

# shared by the program and its background worker
CORE = jobcore.c util.c arcjob.c archive.c arcfmt.c arcfs.c arcio.c codecs.c \
       deflate.c inflate.c lzh.c lzw.c lzx.c
GUI  = farchive.c job.c wdlg.c
HDRS = src/fview.h src/gemui.h src/job.h src/doc.h src/arcio.h src/archive.h \
       src/arcint.h src/deflate.h src/wdlg.h src/farsc.h
# the resource files (rsc/mkrsc.py from rsc/lang/*.txt), next to each program
RSCS = $(notdir $(wildcard rsc/*.rsc))

# one build: $(1) variant, $(2) rel|log, $(3) FV_LOG value, $(4) release|log
define BUILD
$(4)$(DIR_$(1))/farchive.prg: $(addprefix build/$(2)$(1)/,$(GUI:.c=.o) $(CORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^ $$(LDLIBS)
	$$(STRIP) $$@

$(4)$(DIR_$(1))/fawork.ttp: $(addprefix build/$(2)$(1)/,worker.o $(CORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^
	$$(STRIP) $$@

build/$(2)$(1)/%.o: src/%.c $$(HDRS)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $(FLAGS_$(1)) -DFV_LOG=$(3) -c $$< -o $$@

$(4)$(DIR_$(1))/%.rsc: rsc/%.rsc
	@mkdir -p $$(@D)
	cp $$< $$@

$(4): $(4)$(DIR_$(1))/farchive.prg $(4)$(DIR_$(1))/fawork.ttp $(addprefix $(4)$(DIR_$(1))/,$(RSCS))
endef

all: release log

$(foreach v,$(VARIANTS),$(eval $(call BUILD,$(v),rel,0,release)))
$(foreach v,$(VARIANTS),$(eval $(call BUILD,$(v),log,1,log)))

# host test of the archive layer (test/arctest.c)
ARCSRC = src/archive.c src/arcfmt.c src/arcfs.c src/arcio.c src/codecs.c src/deflate.c \
         src/inflate.c src/lzh.c src/lzw.c src/lzx.c
arctest: test/arctest.c $(ARCSRC)
	gcc -O1 -g -DARC_TEST -Isrc -o test/arctest test/arctest.c $(ARCSRC)

# the resource files and src/farsc.h, from rsc/lang/*.txt
rsc:
	python3 rsc/mkrsc.py

clean:
	rm -rf build release log test/arctest

.PHONY: all release log clean arctest rsc
