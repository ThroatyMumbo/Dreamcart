# Included by dc/Makefile; ROOT is recomp/

# The libultra signature DB the cart scan matches against, and the hook table the console links with
SIGDB    := $(ROOT)/build/sigdb
SIGDB_IN := $(ROOT)/tools/gen_sigdb.py $(ROOT)/hooks/patch_symbols.inl \
            $(ROOT)/hooks/libultra_extra.inl $(ROOT)/hooks/libultra_rom.inl $(ROOT)/hooks/libgultra.inl \
            $(ROOT)/hooks/libultra_L.inl $(ROOT)/hooks/policy.json
$(SIGDB)/sigdb.c $(SIGDB)/hooktab.c &: $(SIGDB_IN)
	@mkdir -p $(SIGDB)
	python3 -I $(ROOT)/tools/gen_sigdb.py $(SIGDB)/sigdb.c $(SIGDB)/hooktab.c

# Source paths relative to the checkout's parent, so assert strings carry no host path and every checkout links alike
PREFIX_MAP := -ffile-prefix-map=$(abspath $(ROOT)/..)/=

# The cart scanner's sources (cartscan/), for every build that links it
CS_SRCS  := scan sigs bootimg meta metacache

# The SD cache's key: any change to the translator's sources is a new key
# The translator's objects, for every build that links it
XL_PARTS := xlate xl_abi xl_crc
XL_SRCS  := $(addprefix $(ROOT)/xlate/,$(addsuffix .c,$(XL_PARTS))) $(ROOT)/xlate/xlate.h $(ROOT)/xlate/xl_int.h \
            $(ROOT)/xlate/sh4.h $(ROOT)/xlate/xlpack.c $(ROOT)/xlate/xlpack.h $(ROOT)/xlate/xlz.c $(ROOT)/xlate/xlz.h \
            $(ROOT)/cartscan/ovkey.h
XL_VER   := $(firstword $(shell cat $(XL_SRCS) | cksum))

# rabbitizer, built for KOS
RAB      := $(ROOT)/rabbitizer
RAB_SRC  := $(shell find $(RAB)/src -name '*.c')
RAB_KOS  := $(patsubst $(RAB)/src/%.c,$(ROOT)/build/rab-kos/%.o,$(RAB_SRC))
$(ROOT)/build/rab-kos/%.o: $(RAB)/src/%.c
	@mkdir -p $(@D)
	sh-elf-gcc $(KOS_CFLAGS) $(KOS_INC_PATHS) $(PREFIX_MAP) -O2 -std=gnu11 -w -I$(RAB)/include -I$(RAB)/tables -c $< -o $@
