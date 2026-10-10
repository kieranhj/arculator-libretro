# libretro core for Arculator. Expects the Arculator tree at ./arculator (git
# submodule, mcp-headless branch). Cross-build example (see scripts/build-aarch64.sh):
#   make CC="zig cc -target aarch64-linux-gnu.2.31" CFLAGS="$(OPT) -mcpu=cortex_a53" (see the script)
ARC ?= ./arculator
SRC := $(ARC)/src
ZLIB ?= ./zlib
NSPARK ?= ./nspark
CC ?= cc
TARGET := arculator_libretro.so

# zig cc leaves UBSan traps and the stack protector on even at -O3; LTO is
# worth ~7% on an A53. See scripts/build-aarch64.sh for the cross-build flags.
CFLAGS ?= -O3 -flto -fno-sanitize=undefined -fno-stack-protector -fomit-frame-pointer
# -std=gnu17: the tree uses K&R-style empty-paren declarations that C23 rejects.
# -fno-strict-aliasing: the memory access macros type-pun through uint8_t*.
# RELEASE_BUILD compiles out the internal consistency fatal() checks in arm.c.
# -UNDEBUG: zig cc defines NDEBUG at -O3, and hostfs.c has a broken NDEBUG path.
# platform/ supplies a stand-in SDL.h so keyboard_sdl.h scancodes resolve.
override CFLAGS += -std=gnu17 -fPIC -fno-strict-aliasing -DRELEASE_BUILD -UNDEBUG \
          -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
          -Wno-unknown-warning-option -Wno-misleading-indentation -Wno-parentheses \
          -Wno-pointer-sign -Wno-sign-compare -Wno-unused-value -Wno-format \
          -Wno-deprecated-non-prototype -Wno-strict-prototypes \
          -Iplatform -I$(SRC) -I$(ZLIB) -I.
# -Bsymbolic: bind the emulator's own global calls inside the .so instead of
# through the PLT.
LDFLAGS += -shared -Wl,--no-undefined -Wl,-Bsymbolic -Wl,-version-script=link.T -lm -ldl

# Mirrors arculator_headless_SOURCES in src/Makefile.am, minus the headless
# front-end, video and sound backends (replaced by core.c, libretro_video.c and
# libretro_sound.c). headless_input.c is reused as-is for key[] and the mouse.
ARC_C := \
  82c711.c 82c711_fdc.c arm.c bmu.c cmos.c colourcard.c config.c cp15.c \
  ddnoise.c debugger.c debugger_swis.c disc.c disc_adf.c disc_apd.c disc_fdi.c disc_hfe.c disc_jfd.c \
  disc_mfm_common.c disc_scp.c ds2401.c eterna.c fdi2raw.c fpa.c g16.c g332.c hostfs.c ide.c ide_a3in.c \
  ide_config.c ide_hccs_a3k.c ide_idea.c ide_riscdev.c ide_zidefs.c ide_zidefs_a3k.c ioc.c ioeb.c \
  joystick.c keyboard.c lc.c main.c mem.c memc.c podules.c printer.c riscdev_hdfc.c romload.c sound.c \
  st506.c st506_akd52.c timer.c vidc.c wd1770.c \
  headless_input.c hostfs-unix.c

LOCAL_C := core.c libretro_video.c libretro_sound.c

# disc_apd.c and disc_jfd.c read zlib-compressed images. zlib is bundled as a
# submodule and compiled in, so the core needs no zlib on the target.
ZLIB_C := adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c

OBJDIR := obj
# nspark unpacks Spark/ArcFS archives (,ddc) into HostFS, as archi.medes.live
# does. Its main() becomes nspark_main() and its generic names are prefixed so
# they cannot collide with Arculator's.
NSPARK_C := arcfs.c compress.c crc.c date.c error.c garble.c io.c misc.c pack.c store.c unix.c arc.c main.c unarc.c
NSPARK_FLAGS := -DPOSIX -I$(NSPARK) -Dmain=nspark_main -Derror=nspark_error -Dwarning=nspark_warning   -Dbasename=nspark_basename -Dfilesize=nspark_filesize -Dread_header=nspark_read_header -Ddebug=nspark_debug -Duncompress=nspark_uncompress -Dcompress=nspark_compress -Dmsg=nspark_msg -Dexit=nspark_exit

OBJS := $(addprefix $(OBJDIR)/arc/, $(ARC_C:.c=.o)) $(addprefix $(OBJDIR)/zlib/, $(ZLIB_C:.c=.o)) $(addprefix $(OBJDIR)/nspark/, $(NSPARK_C:.c=.o)) $(addprefix $(OBJDIR)/, $(LOCAL_C:.c=.o))

PYTHON ?= python3

all: $(TARGET)

data_embedded.h: gen_data.py $(ARC)/cmos/riscos3_old/cmos.bin $(ARC)/roms/arcrom_ext
	$(PYTHON) gen_data.py $(ARC) $@

$(OBJDIR)/core.o: data_embedded.h

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(OBJDIR)/arc/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/nspark/%.o: $(NSPARK)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NSPARK_FLAGS) -Wno-implicit-fallthrough -c -o $@ $<

$(OBJDIR)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DHAVE_UNISTD_H -Wno-implicit-fallthrough -c -o $@ $<

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET) data_embedded.h

.PHONY: all clean
