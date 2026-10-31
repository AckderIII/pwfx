# pw_* filters for PipeWire.  make          -- build everything
#                             make test     -- build and run the test suite
#                             make clean

CC       ?= cc
CFLAGS   ?= -O2 -g
WARN      = -Wall -Wextra -Wno-unused-parameter
PW_CFLAGS = $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS   = $(shell pkg-config --libs libpipewire-0.3)
LIBS      = -lm

# Built from whatever is in src/ and tests/, so the tree builds at every
# stage of being filled in rather than only once all six exist.
FILTERS   = $(patsubst src/pw_%.c,pw_%,$(wildcard src/pw_*.c))
REFS      = $(patsubst tests/t_%.c,%,$(wildcard tests/t_*.c))

BIN       = bin
OBJ       = build

# Every header in src/. The DSP cores live in headers shared between a filter
# and its test harness (gate_dsp.h, comp_dsp.h, ...), so a change to one has
# to rebuild both -- depending on dsp.h alone rebuilt the reference side of a
# comparison and not the side it is compared with.
HEADERS   = $(wildcard src/*.h)

# RNNoise, vendored (third_party/rnnoise/README.pwfx), for pw_denoise, its
# reference and its harness. Built as it is, on the portable baseline:
# -Wno-cpp quiets its suggestion to build for a newer CPU, which the filters
# deliberately do not.
RNNOISE_INC = -Ithird_party/rnnoise/include
RNNOISE_OBJ = $(patsubst third_party/rnnoise/src/%.c,$(OBJ)/rnnoise/%.o,\
                $(wildcard third_party/rnnoise/src/*.c))

all: $(FILTERS:%=$(BIN)/%)

$(BIN) $(OBJ) $(BIN)/ref:
	@mkdir -p $@

$(OBJ)/pwf.o: src/pwf.c src/pwf.h | $(OBJ)
	$(CC) $(CFLAGS) $(WARN) $(PW_CFLAGS) -c -o $@ $<

$(BIN)/pw_%: src/pw_%.c $(HEADERS) $(OBJ)/pwf.o | $(BIN)
	$(CC) $(CFLAGS) $(WARN) $(PW_CFLAGS) -o $@ $< $(OBJ)/pwf.o $(PW_LIBS) $(LIBS)

$(OBJ)/rnnoise:
	@mkdir -p $@

$(OBJ)/rnnoise/%.o: third_party/rnnoise/src/%.c | $(OBJ)/rnnoise
	$(CC) $(CFLAGS) -Wno-cpp $(RNNOISE_INC) -Ithird_party/rnnoise/src -c -o $@ $<

# The three that link RNNoise; explicit rules win over the patterns.
$(BIN)/pw_denoise: src/pw_denoise.c $(HEADERS) $(OBJ)/pwf.o $(RNNOISE_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(WARN) $(PW_CFLAGS) $(RNNOISE_INC) -o $@ $< $(OBJ)/pwf.o $(RNNOISE_OBJ) $(PW_LIBS) $(LIBS)

$(BIN)/ref/denoise: reference/denoise.c $(RNNOISE_OBJ) | $(BIN)/ref
	$(CC) $(CFLAGS) $(RNNOISE_INC) -o $@ $< $(RNNOISE_OBJ) $(LIBS)

$(BIN)/t_denoise: tests/t_denoise.c $(HEADERS) $(RNNOISE_OBJ) | $(BIN)
	$(CC) $(CFLAGS) $(WARN) $(RNNOISE_INC) -o $@ $< $(RNNOISE_OBJ) $(LIBS)

# The reference implementations, built so the tests can compare against them.
$(BIN)/ref/%: reference/%.c | $(BIN)/ref
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

# Offline harnesses: the same DSP core as the filter, driven from stdin so it
# can be diffed against the reference byte for byte.
$(BIN)/t_%: tests/t_%.c $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(WARN) -o $@ $< $(LIBS)

refs: $(REFS:%=$(BIN)/ref/%)

# The offline half: DSP cores diffed against the reference implementations.
test: all refs $(REFS:%=$(BIN)/t_%)
	@tests/run.sh

# Everything, including the live PipeWire tests. Needs a running daemon.
check: all refs $(REFS:%=$(BIN)/t_%)
	@tests/all.sh

# The GUI. Separate from `all` because it is a cargo build with its own
# dependency tree; the filters stay buildable with nothing but a C compiler
# and libpipewire.
app: all
	cd app && cargo build --release
	@echo "built app/target/release/pwfx"

app-test:
	cd app && cargo test

clean:
	rm -rf $(BIN) $(OBJ)
	-cd app && cargo clean

# The app as an AppImage, built in a Debian 12 container so it runs on that
# and anything newer. Needs podman; writes dist/. See packaging/appimage.
appimage:
	packaging/appimage/build.sh

.PHONY: all clean test check refs app app-test appimage
