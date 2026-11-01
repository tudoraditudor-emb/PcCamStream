# PcCamStream — build system (GNU Make + pkg-config)
PKG_CONFIG ?= pkg-config
PKGS       := gstreamer-1.0 gstreamer-app-1.0

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
