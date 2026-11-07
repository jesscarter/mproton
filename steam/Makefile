# Standalone build of Proton's lsteamclient (lsteamclient.dll + lsteamclient.so)
# and steam_helper (steam.exe) against a Wine build tree, without Proton's
# container or top-level Makefile. It issues the same commands Wine's makedep
# generates for an in-tree module (compare a dll's rules in $(WINE_OBJ)/Makefile).
#
#   make WINE_SRC=/path/to/wine WINE_OBJ=/path/to/wine-build
#   make install DESTDIR=/path/to/mproton-dist/files
#
# WINE_SRC  Wine source tree (headers: include/, include/msvcrt/, include/wine/).
# WINE_OBJ  A configured and built Wine tree for the same source: generated
#           headers, tools/winegcc, tools/winebuild, the PE import libraries
#           (dlls/*/<arch>-windows/lib*.a, libs/winecrt0, libs/compiler-rt)
#           and dlls/ntdll/ntdll.so. For athei's arm64-patched tree this is
#           the build directory that mach9 configures with
#           --enable-archs=arm64ec,aarch64,i386.
#
# Outputs (in $(O), installed under $(DESTDIR)/lib/wine/):
#   aarch64-windows/lsteamclient.dll  ARM64X (aarch64 + arm64ec) when both are in PE_ARCHS
#   aarch64-windows/steam.exe         aarch64 (Wine builds programs without arm64ec)
#   i386-windows/{lsteamclient.dll,steam.exe}
#   $(UNIX_ARCH)-unix/lsteamclient.so the unix side (a Mach-O dylib on macOS, as all Wine unixlibs)

WINE_SRC ?=
WINE_OBJ ?=
O        ?= build
DESTDIR  ?= $(O)/dist

PE_ARCHS  ?= aarch64 arm64ec i386
UNIX_OS   ?= $(shell uname -s | tr A-Z a-z)
UNIX_ARCH ?= aarch64

WINEGCC ?= $(WINE_OBJ)/tools/winegcc/winegcc
# Linking the unix side against ntdll.so catches undefined symbols. Set it
# empty to link without it (e.g. a cross build whose Wine tree is another arch).
NTDLL_SO ?= $(WINE_OBJ)/dlls/ntdll/ntdll.so

i386_CC    ?= i686-w64-mingw32-clang
x86_64_CC  ?= x86_64-w64-mingw32-clang
aarch64_CC ?= aarch64-w64-mingw32-clang
arm64ec_CC ?= arm64ec-w64-mingw32-clang

ifeq ($(UNIX_OS),darwin)
UNIX_CC    ?= clang
UNIX_CXX   ?= clang++
UNIX_ARCHFLAGS ?= -arch arm64 -mmacosx-version-min=11.0
UNIX_LDFLAGS_OS = -dynamiclib -install_name @rpath/lsteamclient.so -Wl,-rpath,@loader_path/
else
UNIX_CC    ?= $(UNIX_ARCH)-linux-gnu-gcc
UNIX_CXX   ?= $(UNIX_ARCH)-linux-gnu-g++
UNIX_ARCHFLAGS ?=
UNIX_LDFLAGS_OS = -shared -Wl,-Bsymbolic -Wl,-soname,lsteamclient.so $(if $(NTDLL_SO),-Wl$(comma)-z$(comma)defs) -ldl -lpthread
endif
comma := ,

CFLAGS   ?= -g -O2
CXXFLAGS ?= -g -O2
LDFLAGS  ?=

LSTEAMCLIENT_DEFS = -DSTEAM_API_EXPORTS -Dprivate=public -Dprotected=public

# ---------------------------------------------------------------------------

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(strip $(WINE_SRC)),)
$(error set WINE_SRC to the Wine source tree)
endif
ifeq ($(strip $(WINE_OBJ)),)
$(error set WINE_OBJ to the configured and built Wine tree)
endif
endif

LSC_DIR = lsteamclient
HELPER_DIR = steam_helper

LSC_PE_SRCS   := $(sort $(wildcard $(LSC_DIR)/*.c))
LSC_UNIX_SRCS := $(sort $(wildcard $(LSC_DIR)/*.cpp))

has_arch = $(filter $(1),$(PE_ARCHS))
HYBRID := $(and $(call has_arch,aarch64),$(call has_arch,arm64ec))

# PE target triple and install dir per arch; arm64ec code lives in the
# aarch64-windows dir (ARM64X), as in Wine.
pe_target_i386    = i686-windows
pe_target_x86_64  = x86_64-windows
pe_target_aarch64 = aarch64-windows
pe_target_arm64ec = arm64ec-windows
pe_dir_i386       = i386-windows
pe_dir_x86_64     = x86_64-windows
pe_dir_aarch64    = aarch64-windows
pe_dir_arm64ec    = aarch64-windows

PE_WARN = -Wall -fno-strict-aliasing -Wempty-body -Wignored-qualifiers -Winit-self \
  -Wno-microsoft-enum-forward-reference -Wtype-limits -Wunused-but-set-parameter -Wvla \
  -Wwrite-strings -Wpointer-arith
pe_extra_i386 = -fms-hotpatch -fno-omit-frame-pointer -mlong-double-64 -msse2
pe_ldextra_i386 = -fms-hotpatch

pe_cflags = -D_UCRT -D__WINESRC__ -D__WINE_PE_BUILD -I$(WINE_OBJ)/include -I$(WINE_SRC)/include \
  -I$(WINE_SRC)/include/msvcrt -target $(pe_target_$(1)) -fuse-ld=lld --no-default-config \
  $(PE_WARN) -ffunction-sections -ffp-exception-behavior=maytrap $(pe_extra_$(1)) -gdwarf-4 \
  -fasync-exceptions $(CFLAGS)

# import libraries, in the order makedep passes them
pe_lib = $(WINE_OBJ)/dlls/$(2)/$(pe_dir_$(1))/lib$(2).a
pe_crt = $(WINE_OBJ)/libs/winecrt0/$(pe_dir_$(1))/libwinecrt0.a \
  $(WINE_OBJ)/libs/compiler-rt/$(pe_dir_$(1))/libcompiler-rt.a \
  $(call pe_lib,$(1),ucrtbase) $(call pe_lib,$(1),kernel32) $(call pe_lib,$(1),ntdll)

LSC_IMPORTS    = user32 ws2_32
HELPER_IMPORTS = user32 advapi32 shlwapi shell32 msi ole32

UNIX_CPPFLAGS = -I$(LSC_DIR) -I$(WINE_OBJ)/include -I$(WINE_SRC)/include -D__WINESRC__ -DWINE_UNIX_LIB \
  $(LSTEAMCLIENT_DEFS)
UNIX_CXXFLAGS_ALL = $(UNIX_ARCHFLAGS) -std=c++17 -Wall -pipe -fvisibility=hidden -fno-stack-protector \
  -fno-strict-aliasing -fPIC -fasynchronous-unwind-tables -Wno-unused-variable \
  -Wno-unused-but-set-variable $(CXXFLAGS)

# ---------------------------------------------------------------------------
# PE objects: $(O)/<arch>/{lsteamclient,steam_helper}/*.o

define pe_objs
$(patsubst $(LSC_DIR)/%.c,$(O)/$(1)/lsteamclient/%.o,$(LSC_PE_SRCS))
endef

define pe_arch_rules
$(O)/$(1)/lsteamclient/%.o: $(LSC_DIR)/%.c
	@mkdir -p $$(@D)
	$$($(1)_CC) -c -o $$@ $$< -MMD -MP -I$(LSC_DIR) $(LSTEAMCLIENT_DEFS) $$(call pe_cflags,$(1))

$(O)/$(1)/steam_helper/%.o: $(HELPER_DIR)/%.c
	@mkdir -p $$(@D)
	$$($(1)_CC) -c -o $$@ $$< -MMD -MP -I$(HELPER_DIR) $$(call pe_cflags,$(1))

-include $$(patsubst %.o,%.d,$$(call pe_objs,$(1)) $(O)/$(1)/steam_helper/steam.o)
endef
$(foreach a,$(PE_ARCHS),$(eval $(call pe_arch_rules,$(a))))

# lsteamclient.dll: one link per output dir. aarch64 + arm64ec link together
# into an ARM64X image (makedep: winegcc -b arm64ec-windows -marm64x).
LSC_DLLS :=
HELPER_EXES :=

define pe_dll_rule
# $(1): output arch dir name; $(2): arch whose toolchain links; $(3): extra winegcc flags; $(4): object archs
$(O)/$(pe_dir_$(1))/lsteamclient.dll: $(LSC_DIR)/lsteamclient.spec $(foreach a,$(4),$(call pe_objs,$(a)))
	@mkdir -p $$(@D)
	$(WINEGCC) -o $$@ --wine-objdir $(WINE_OBJ) --cc-cmd="$$($(2)_CC)" -b $(pe_target_$(2)) $(3) \
	  -Wl,--wine-builtin -shared $$^ \
	  $(foreach l,$(LSC_IMPORTS),$(call pe_lib,$(2),$(l))) $(call pe_crt,$(2)) \
	  --no-default-config $(pe_ldextra_$(2)) $(LDFLAGS)
LSC_DLLS += $(O)/$(pe_dir_$(1))/lsteamclient.dll
endef

define pe_exe_rule
$(O)/$(pe_dir_$(1))/steam.exe: $(O)/$(1)/steam_helper/steam.o
	@mkdir -p $$(@D)
	$(WINEGCC) -o $$@ --wine-objdir $(WINE_OBJ) --cc-cmd="$$($(1)_CC)" -b $(pe_target_$(1)) \
	  -Wl,--wine-builtin -mwindows $$^ \
	  $(foreach l,$(HELPER_IMPORTS),$(call pe_lib,$(1),$(l))) $(call pe_crt,$(1)) \
	  --no-default-config $(pe_ldextra_$(1)) $(LDFLAGS)
HELPER_EXES += $(O)/$(pe_dir_$(1))/steam.exe
endef

ifneq ($(HYBRID),)
$(eval $(call pe_dll_rule,aarch64,arm64ec,-marm64x,aarch64 arm64ec))
else
$(foreach a,$(filter aarch64 arm64ec,$(PE_ARCHS)),$(eval $(call pe_dll_rule,$(a),$(a),,$(a))))
endif
$(foreach a,$(filter i386 x86_64,$(PE_ARCHS)),$(eval $(call pe_dll_rule,$(a),$(a),,$(a))))

# steam.exe: Wine builds programs for aarch64 only, never arm64ec
$(foreach a,$(filter-out arm64ec,$(PE_ARCHS)),$(eval $(call pe_exe_rule,$(a))))
ifeq ($(PE_ARCHS),arm64ec)
$(eval $(call pe_exe_rule,arm64ec))
endif

# ---------------------------------------------------------------------------
# unix side

UNIX_OBJS := $(patsubst $(LSC_DIR)/%.cpp,$(O)/$(UNIX_ARCH)-unix/obj/%.o,$(LSC_UNIX_SRCS))
UNIX_LIB  := $(O)/$(UNIX_ARCH)-unix/lsteamclient.so

$(O)/$(UNIX_ARCH)-unix/obj/%.o: $(LSC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(UNIX_CXX) -c -o $@ $< -MMD -MP $(UNIX_CPPFLAGS) $(UNIX_CXXFLAGS_ALL)

$(UNIX_LIB): $(UNIX_OBJS)
	$(UNIX_CXX) -o $@ $(UNIX_ARCHFLAGS) $(UNIX_OBJS) $(NTDLL_SO) $(UNIX_LDFLAGS_OS) $(LDFLAGS)

-include $(UNIX_OBJS:.o=.d)

# ---------------------------------------------------------------------------

all: pe unix
pe: $(LSC_DLLS) $(HELPER_EXES)
unix: $(UNIX_LIB)

install: all
	for f in $(LSC_DLLS) $(HELPER_EXES) $(UNIX_LIB); do \
	  d=$(DESTDIR)/lib/wine/$$(basename $$(dirname $$f)); mkdir -p $$d && cp -f $$f $$d/; \
	done

clean:
	rm -rf $(O)

.PHONY: all pe unix install clean
.DEFAULT_GOAL := all
