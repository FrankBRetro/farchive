# FArchive for PC GEM (FreeGEM / OpenGEM on DOS), 32-bit with DJGPP
# Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE)
#
#   source ~/work/vibe/PC-GEM/scripts/djgppenv.sh
#   make -C pcgem              -> pcgem/FARCHIVE.APP
#   make -C pcgem LOG=1        ... that writes FARCHIVE.LOG (clean first)
#   make -C pcgem install      -> C:\FARCHIVE of the PC-GEM test setup
#
# FArchive's menus and dialogs are in ../rsc/farchive.rsc (make rsc), which
# install copies next to FARCHIVE.APP.
#
# FArchive's sources (../src) are compiled with -DPCGEM against GEMLib-style
# headers in pcgem/include (gem.h, vdi.h, osbind.h, mintbind.h ...),
# implemented here on John Elliott's DJGPP GEM bindings - FArchive's own
# copy of the PC GEM layer: pgaes.c and pgvdi.c (trees, strings and bitmaps
# copied to DOS memory for GEM), pgrsc.c (resource helpers) and pgtos.c
# (the TOS calls, on DOS). There is no MiNT on DOS, so the tasks run inside
# FArchive, as on plain TOS (FAWORK.TTP isn't built).
#
# Run it with CWSDPMI.EXE beside it or on the PATH.

DJCC   ?= i586-pc-msdosdjgpp-gcc
GEMKIT ?= $(HOME)/work/vibe/PC-GEM/tools/djgppgem
GK      = $(GEMKIT)/CONTRIB/GEM
GKINC   = obj/kitinc
LOG    ?= 0

CFLAGS   = -O2 -Wall -Wno-pointer-sign -Wno-address-of-packed-member -DPCGEM \
           -DFV_LOG=$(LOG) $(if $(filter 1,$(LOG)),-DEDLOG) -Iinclude -I../src
KITFLAGS = -O2 -Wall -Wno-address-of-packed-member -fgnu89-inline $(if $(filter 1,$(LOG)),-DEDLOG) -I$(GKINC) -I.

CORE = jobcore util arcjob archive arcfmt arcfs arcio codecs \
       deflate inflate lzh lzw lzx
GUI  = farchive job wdlg
LAYER   = pgrsc pgtos pglinear      # the PC GEM layer, GEMLib-style headers
KITPORT = pgaes pgvdi               # the PC GEM layer, the kit's headers

OBJS = $(addprefix obj/,$(addsuffix .o,$(GUI) $(CORE) $(LAYER) $(KITPORT) pclog))
HDRS = $(wildcard ../src/*.h include/*.h include/mint/*.h)

all: FARCHIVE.APP

FARCHIVE.APP: $(OBJS)
	$(DJCC) -o farchive.exe $^ $(GK)/LIB/LIBGEM.A -lm
	cp farchive.exe $@

obj/%.o: ../src/%.c $(HDRS)
	@mkdir -p obj
	$(DJCC) $(CFLAGS) -c -o $@ $<
obj/pgrsc.o obj/pgtos.o obj/pglinear.o: obj/%.o: %.c $(HDRS)
	@mkdir -p obj
	$(DJCC) $(CFLAGS) -c -o $@ $<
obj/pgaes.o obj/pgvdi.o: obj/%.o: %.c pgkit.h $(GKINC)/.fixed
	@mkdir -p obj
	$(DJCC) $(KITFLAGS) -c -o $@ $<
obj/pclog.o: pclog.c
	@mkdir -p obj
	$(DJCC) $(CFLAGS) -c -o $@ $<

# the kit's headers with their struct packing fixed for current gcc
$(GKINC)/.fixed: $(wildcard $(GK)/INCLUDE/*.H)
	@test -f $(GK)/INCLUDE/DJGPPGEM.H || { echo "DJGPP GEM kit not found: set GEMKIT"; exit 1; }
	rm -rf $(GKINC); mkdir -p $(GKINC)
	for f in $(GK)/INCLUDE/*.H; do \
	  lc=$$(basename "$$f" | tr A-Z a-z); \
	  sed -E 's/^\}[[:space:]]*([A-Za-z_0-9]+)[[:space:]]*(__attribute__\(\(packed\)\))?[[:space:]]*GPK/} __attribute__((packed)) \1;/' "$$f" > $(GKINC)/$$lc; \
	done
	touch $@

# into DOSBox-X's C: (C:\FARCHIVE), with CWSDPMI
PCGEMC ?= $(HOME)/work/vibe/PC-GEM/dos/C
install: all
	mkdir -p $(PCGEMC)/FARCHIVE
	cp FARCHIVE.APP $(PCGEMC)/FARCHIVE/
	for f in ../rsc/*.rsc; do cp $$f $(PCGEMC)/FARCHIVE/$$(basename $$f | tr a-z A-Z); done
	test -f $(PCGEMC)/FARCHIVE/CWSDPMI.EXE || cp $(HOME)/work/vibe/PC-GEM/tools/cwsdpmi/bin/CWSDPMI.EXE $(PCGEMC)/FARCHIVE/

clean:
	rm -rf obj farchive.exe FARCHIVE.APP

.PHONY: all install clean
