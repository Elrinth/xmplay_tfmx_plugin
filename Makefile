# xmp-tfmx — native XMPlay input plugin (Hülsbeck TFMX via libtfmxaudiodecoder)
#
#   /usr/bin/make          # host tests + 32-bit DLL
#   /usr/bin/make dll      # dist/xmp-tfmx.dll
#   /usr/bin/make test     # host render/seek/detect tests
#   /usr/bin/make pack     # /workspace/xmp-tfmx-1.0.5.zip
#
# If `make` is a wrapper, invoke GNU make explicitly.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST := $(ROOT)/dist
SRC  := $(ROOT)/src
INC  := $(ROOT)/include/xmplay
LIB  := $(ROOT)/third_party/libtfmxaudiodecoder/src
OBJ  := $(DIST)/obj
OBJW := $(DIST)/obj-i686

I686_HOST := i686-w64-mingw32
I686_CC   := $(I686_HOST)-gcc
I686_CXX  := $(I686_HOST)-g++

LIB_SRCS = \
	$(LIB)/Decoder.cpp \
	$(LIB)/DecoderProxy.cpp \
	$(LIB)/CRCLight.cpp \
	$(LIB)/Dump.cpp \
	$(LIB)/Filter.cpp \
	$(LIB)/PaulaVoice.cpp \
	$(LIB)/LamePaulaVoice.cpp \
	$(LIB)/LamePaulaMixer.cpp \
	$(LIB)/tfmxaudiodecoder.cpp \
	$(LIB)/Chris/TFMXDecoder.cpp \
	$(LIB)/Chris/Macro.cpp \
	$(LIB)/Chris/Modulation.cpp \
	$(LIB)/Chris/Pattern.cpp \
	$(LIB)/Chris/Sequencer.cpp \
	$(LIB)/Chris/Songs.cpp \
	$(LIB)/Chris/ByChecksum.cpp \
	$(LIB)/Chris/SamplesFile.cpp \
	$(LIB)/Chris/MergedFiles.cpp \
	$(LIB)/Chris/DNS/DNSDecoder.cpp \
	$(LIB)/Jochen/HippelDecoder.cpp \
	$(LIB)/Jochen/Analyze.cpp \
	$(LIB)/Jochen/COSO.cpp \
	$(LIB)/Jochen/Envelope.cpp \
	$(LIB)/Jochen/FC.cpp \
	$(LIB)/Jochen/Instrument.cpp \
	$(LIB)/Jochen/MCMD.cpp \
	$(LIB)/Jochen/ModPack.cpp \
	$(LIB)/Jochen/Portamento.cpp \
	$(LIB)/Jochen/Probe.cpp \
	$(LIB)/Jochen/SMOD.cpp \
	$(LIB)/Jochen/TFMX7V.cpp \
	$(LIB)/Jochen/TFMX.cpp \
	$(LIB)/Jochen/TraitsByChecksum.cpp \
	$(LIB)/Jochen/Vibrato.cpp

INCS = -I$(LIB) -I$(LIB)/Chris -I$(LIB)/Chris/DNS -I$(LIB)/Jochen -I$(SRC)

CFLAGS_COM = -O2 -fno-strict-aliasing -Wall -Wno-unused-function \
	-Wno-unused-parameter -DNDEBUG $(INCS)

CXXFLAGS_COM = $(CFLAGS_COM) -std=c++14 -fno-exceptions -fno-rtti

CFLAGS_L = $(CFLAGS_COM) -fPIC
CXXFLAGS_L = $(CXXFLAGS_COM) -fPIC
CFLAGS_W = $(CFLAGS_COM) -DWIN32 -D_WIN32 -D_USE_MATH_DEFINES
CXXFLAGS_W = $(CXXFLAGS_COM) -DWIN32 -D_WIN32

LINUX_LIB_OBJS = $(patsubst $(LIB)/%.cpp,$(OBJ)/%.o,$(LIB_SRCS))
WIN_LIB_OBJS   = $(patsubst $(LIB)/%.cpp,$(OBJW)/%.o,$(LIB_SRCS))

.PHONY: all dll test pack clean

all: test dll

dll: $(DIST)/xmp-tfmx.dll

test: $(DIST)/test_tfmx_render
	cd $(ROOT) && $(DIST)/test_tfmx_render

$(OBJ)/%.o: $(LIB)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_L) -c -o $@ $<

$(OBJ)/tfmx_player.o: $(SRC)/tfmx_player.c $(SRC)/tfmx_player.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_L) -c -o $@ $<

$(OBJW)/%.o: $(LIB)/%.cpp
	@mkdir -p $(dir $@)
	$(I686_CXX) $(CXXFLAGS_W) -c -o $@ $<

$(OBJW)/tfmx_player.o: $(SRC)/tfmx_player.c $(SRC)/tfmx_player.h
	@mkdir -p $(dir $@)
	$(I686_CC) $(CFLAGS_W) -c -o $@ $<

$(DIST)/test_tfmx_render: $(ROOT)/tests/test_tfmx_render.c $(OBJ)/tfmx_player.o $(LINUX_LIB_OBJS)
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS_L) -o $@ $(ROOT)/tests/test_tfmx_render.c \
	  $(OBJ)/tfmx_player.o $(LINUX_LIB_OBJS) -lm

$(OBJW)/xmp-tfmx.res: $(SRC)/xmp-tfmx.rc
	@mkdir -p $(dir $@)
	$(I686_HOST)-windres -O coff -o $@ $<

$(DIST)/xmp-tfmx.dll: $(SRC)/xmp-tfmx.cpp $(SRC)/xmp-tfmx.def $(OBJW)/tfmx_player.o $(WIN_LIB_OBJS) $(OBJW)/xmp-tfmx.res
	mkdir -p $(DIST)
	$(I686_CXX) -shared -O2 -DNDEBUG -std=c++14 \
	  -static -static-libgcc -static-libstdc++ \
	  -I$(INC) $(INCS) -DWIN32 -D_WIN32 \
	  -o $@ $(SRC)/xmp-tfmx.cpp $(SRC)/xmp-tfmx.def \
	  $(OBJW)/tfmx_player.o $(WIN_LIB_OBJS) $(OBJW)/xmp-tfmx.res \
	  -Wl,--kill-at -Wl,--add-stdcall-alias \
	  -luser32 -lgdi32 -Wl,-s
	$(I686_HOST)-objdump -p $@ | grep -E 'dll name|XMPIN_GetInterface|file format' || true
	file $@

pack: dll
	rm -f /workspace/xmp-tfmx-1.0.5.zip
	mkdir -p $(DIST)/pack
	cp -f $(DIST)/xmp-tfmx.dll $(ROOT)/README.md $(DIST)/pack/
	cd $(DIST)/pack && zip -9 /workspace/xmp-tfmx-1.0.5.zip xmp-tfmx.dll README.md
	rm -rf $(DIST)/pack
	ls -l /workspace/xmp-tfmx-1.0.5.zip

clean:
	rm -rf $(DIST)/xmp-tfmx.dll $(DIST)/test_tfmx_render $(DIST)/obj $(DIST)/obj-i686 $(DIST)/pack
