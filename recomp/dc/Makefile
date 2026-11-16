# Dreamcart 64 (needs the KOS environment). Any cart, scanned and translated on the console: make -C dc n64-live.
ROOT    := $(abspath ..)
ifeq ($(KOS_BASE),)
$(error KOS_BASE is not set: source KOS's environ.sh first)
endif
.DEFAULT_GOAL := n64-live
include $(ROOT)/common.mk
DCDIR    := $(ROOT)/../dc
CC       := sh-elf-gcc
# The harness's objects take plain SH-4 flags; KOS's own take the KOS flags
HCC      := $(CC) -ml -m4-single
# KOS init's maple scan wait goes through dc_exit.c, which arms the exit thread and bounds the wait
export KOS_LDFLAGS += -Wl,--wrap=maple_wait_scan
# dc_exit.c turns the MMU off on every abort and exit, so no dcload restarts into a TLB miss after the program
export KOS_LDFLAGS += -Wl,--wrap=arch_abort -Wl,--wrap=arch_exit
MUPEN_HLE := $(ROOT)/mupen64plus-rsp-hle/src
HLE_OBJS  := hle_alist.o hle_alist_audio.o hle_alist_nead.o hle_alist_naudio.o hle_mp3.o hle_audio.o hle_memory.o abi1_fast.o
# harness/abi1_fast.c replaces these three; mupen's copies stay as weak definitions
HLE_WEAK  := --weaken-symbol=_alist_envmix_exp --weaken-symbol=_alist_resample --weaken-symbol=_alist_adpcm \
             --weaken-symbol=_alist_clear --weaken-symbol=_alist_envmix_nead

COMMON   := -O2 -g -ffunction-sections -fdata-sections -fno-common -fno-math-errno -fno-strict-aliasing -w $(PREFIX_MAP)
GFXDIR   := $(ROOT)/harness/gfx
GFX_SRC  := $(wildcard $(GFXDIR)/*.c)
LIVE_DEADLINE_S ?= 1800

# One ELF for any cart: the inserted cart is scanned, translated and linked on the console (harness/cartboot.c).
# make -C dc n64-live | n64-live-quiet: build/dc-any/n64-live{,-quiet}.elf
AB       := $(ROOT)/build/dc-any
AB_INC  := -I$(ROOT)/harness -I$(GFXDIR) -I$(ROOT)/dc -I$(ROOT)/xlate -I$(ROOT)/cartscan -I$(RAB)/include
AB_CF    := $(COMMON) $(AB_INC)
AB_KCF   := $(KOS_CFLAGS) $(KOS_INC_PATHS) -I$(DCDIR) $(AB_INC) -w $(PREFIX_MAP)
AB_HARN  := call tgeneric hooks os hostmmu rcp pi_ledger pif flashram cartboot tload ovl rgn
AB_CS    := $(CS_SRCS)
# Audio: the game's voices on AICA channels (harness/alist_aica.c)
AB_AUD   := $(AB)/h/rsp_audio.o $(AB)/h/alist_decode.o $(AB)/h/audio_aica.o $(AB)/h/alist_aica.o $(AB)/h/aica_voice.o \
            $(AB)/h/aica_cache.o $(AB)/k/aica_hw.o
AB_STATS := $(AB)/k/play_stats_aica.o
AB_OBJS  := $(addprefix $(AB)/h/,$(addsuffix .o,$(AB_HARN)) fiber.o $(HLE_OBJS)) $(AB_AUD) \
            $(patsubst $(GFXDIR)/%.c,$(AB)/h/gfx_%.o,$(GFX_SRC)) $(addprefix $(AB)/cs/,$(addsuffix .o,$(AB_CS))) \
            $(AB)/cs/sigdb.o $(AB)/cs/hooktab.o $(addprefix $(AB)/x/,$(addsuffix .o,$(XL_PARTS))) $(AB)/x/xlpack.o \
            $(AB)/x/xlz.o $(RAB_KOS) \
            $(AB_STATS) $(addprefix $(AB)/k/,play_stats.o cartsrc_reader.o cartboot_kos.o loading.o romcache.o mapleraw.o \
            dcsim.o cartlink.o dcmmu.o pvr.o vmupak.o cartflash.o vmulcd.o tmu1.o)
# The live builds open on the Dreamcart 64 menu (dc64_menu.c), and their exit combo restarts into it: dc64_entry.S
# snapshots .data as the ELF entry, and dc64_restart.c takes arch_abort's last call
DC64_LD      := -Wl,-e,_dc64_entry '-Wl,--defsym=_dc64_data_lo=ADDR(.data)' -Wl,--wrap=arch_real_exit
DC64_RESTART := $(AB)/k/dc64_entry.o $(AB)/k/dc64_restart.o
DC64_OBJS    := $(DC64_RESTART) $(AB)/k/dc64_menu.o $(AB)/k/dc64_cart.o $(AB)/k/dc64_sfx.o
LIVE_OBJS    := $(AB_OBJS) $(AB)/k/play_cartboot.o $(DC64_OBJS) $(AB)/k/dc_exit-menu.o $(AB)/h/drivers.o
$(AB)/k/dc64_entry.o: $(ROOT)/dc/dc64_entry.S
	@mkdir -p $(@D)
	$(HCC) -c $< -o $@
$(AB)/h/%.o: $(ROOT)/harness/%.c $(wildcard $(ROOT)/harness/*.h) $(wildcard $(ROOT)/cartscan/*.h)
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -I$(MUPEN_HLE) -c $< -o $@
$(AB)/h/call.o: $(ROOT)/harness/call_tmpl.c $(ROOT)/harness/tmpl.h
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -c $< -o $@
$(AB)/h/fiber.o: $(ROOT)/harness/fiber.S
	@mkdir -p $(@D)
	$(HCC) -c $< -o $@
$(AB)/h/hle_%.o: $(MUPEN_HLE)/%.c
	@mkdir -p $(@D)
	$(HCC) $(COMMON) -DNDEBUG -I$(MUPEN_HLE) -c $< -o $@
	sh-elf-objcopy $(HLE_WEAK) $@
$(AB)/h/abi1_fast.o: $(ROOT)/harness/abi1_fast.c
	@mkdir -p $(@D)
	$(HCC) $(COMMON) -DNDEBUG -I$(MUPEN_HLE) -c $< -o $@
$(AB)/h/alist_aica.o: $(ROOT)/harness/alist_aica.c $(wildcard $(ROOT)/harness/*.h)
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -DNDEBUG -I$(MUPEN_HLE) -c $< -o $@
$(AB)/h/gfx_%.o: $(GFXDIR)/%.c $(wildcard $(GFXDIR)/*.h)
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -c $< -o $@
$(AB)/cs/%.o: $(ROOT)/cartscan/%.c $(wildcard $(ROOT)/cartscan/*.h)
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -c $< -o $@
$(AB)/cs/%.o: $(SIGDB)/%.c $(wildcard $(ROOT)/cartscan/*.h) $(ROOT)/harness/tload.h
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -c $< -o $@
$(addprefix $(AB)/x/,$(addsuffix .o,$(XL_PARTS))): $(AB)/x/%.o: $(ROOT)/xlate/%.c $(XL_SRCS)
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -std=gnu11 -c $< -o $@
$(AB)/x/xlpack.o: $(ROOT)/xlate/xlpack.c $(XL_SRCS) $(ROOT)/xlate/xlpack.h
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -std=gnu11 -DXL_VER=$(XL_VER)u -c $< -o $@
$(AB)/x/xlz.o: $(ROOT)/xlate/xlz.c $(ROOT)/xlate/xlz.h
	@mkdir -p $(@D)
	$(HCC) $(AB_CF) -std=gnu11 -c $< -o $@
# The menu's play core, and the same without console output after startup for a slow link to the host (run.sh -q)
$(AB)/k/play-menu.o: $(ROOT)/dc/play.c $(wildcard $(ROOT)/dc/*.h)
	@mkdir -p $(@D)
	$(CC) $(AB_KCF) -DDC64_MENU -c $< -o $@
$(AB)/k/play-quiet.o: $(ROOT)/dc/play.c $(wildcard $(ROOT)/dc/*.h)
	@mkdir -p $(@D)
	$(CC) $(AB_KCF) -DDC64_MENU -DPLAY_QUIET -c $< -o $@
$(AB)/k/%.o: $(ROOT)/dc/%.c $(wildcard $(ROOT)/dc/*.h)
	@mkdir -p $(@D)
	$(CC) $(AB_KCF) -c $< -o $@
$(addprefix $(AB)/k/,play_cartboot.o cartsrc_reader.o): $(ROOT)/harness/cartboot.h $(wildcard $(ROOT)/xlate/*.h)
$(addprefix $(AB)/k/,aica_hw.o play_stats_aica.o): $(ROOT)/harness/aica_voice.h $(ROOT)/harness/alist_aica.h \
    $(ROOT)/harness/aica_cache.h
$(AB)/k/%.o: $(DCDIR)/%.c $(wildcard $(DCDIR)/*.h)
	@mkdir -p $(@D)
	$(CC) $(AB_KCF) -c $< -o $@
$(AB)/k/pvr.o: $(ROOT)/dc/gfx_pvr.c $(ROOT)/dc/gfx_pvr.h
	@mkdir -p $(@D)
	$(CC) $(AB_KCF) -c $< -o $@
# The ELFs that restart (dc64_restart.c): a restart's combo counts only once let go
$(AB)/k/dc_exit-menu.o: $(ROOT)/dc/dc_exit.c $(ROOT)/dc/dc_exit.h $(ROOT)/dc/dc64_restart.h
	@mkdir -p $(@D)
	$(CC) $(AB_KCF) -DDC_DEADLINE_S=$(LIVE_DEADLINE_S) -DDC_EXIT_RESTARTS -c $< -o $@
$(AB)/n64-live.elf: $(LIVE_OBJS) $(AB)/k/play-menu.o
	kos-cc -Wl,--gc-sections -Wl,--sort-section=name $(DC64_LD) -o $@ $^
$(AB)/n64-live-quiet.elf: $(LIVE_OBJS) $(AB)/k/play-quiet.o
	kos-cc -Wl,--gc-sections -Wl,--sort-section=name $(DC64_LD) -o $@ $^
.PHONY: n64-live n64-live-quiet
n64-live: $(AB)/n64-live.elf
n64-live-quiet: $(AB)/n64-live-quiet.elf
