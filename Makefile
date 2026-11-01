# PcCamStream — build system (GNU Make + pkg-config)
PKG_CONFIG ?= pkg-config
PKGS       := gstreamer-1.0 gstreamer-app-1.0

# Mesaj clar dacă lipsesc dependențele de build (vezi README, secțiunea „Cerințe”)
ifeq ($(shell command -v $(PKG_CONFIG) 2>/dev/null),)
$(error pkg-config nu este instalat. Instalează dependențele: sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-tools)
endif
ifeq ($(strip $(shell $(PKG_CONFIG) --exists $(PKGS) 2>/dev/null && echo yes)),)
$(error pkg-config nu găsește '$(PKGS)' — lipsesc headerele GStreamer. Instalează: sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev)
endif

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS))

BIN := pc-cam-stream
SRC := src/main.c

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDLIBS)

# run against the real camera, displayed on screen
run: $(BIN)
	./$(BIN)

# headless smoke test: synthetic source, no display needed, stops after 120 frames
test: $(BIN)
	./$(BIN) --test --sink=fakesink --frames=120

clean:
	rm -f $(BIN)

.PHONY: all run test clean
