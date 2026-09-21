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

all: $(FILTERS:%=$(BIN)/%)

$(BIN) $(OBJ) $(BIN)/ref:
	@mkdir -p $@

$(OBJ)/pwf.o: src/pwf.c src/pwf.h | $(OBJ)
	$(CC) $(CFLAGS) $(WARN) $(PW_CFLAGS) -c -o $@ $<

$(BIN)/pw_%: src/pw_%.c src/pwf.h src/dsp.h $(OBJ)/pwf.o | $(BIN)
	$(CC) $(CFLAGS) $(WARN) $(PW_CFLAGS) -o $@ $< $(OBJ)/pwf.o $(PW_LIBS) $(LIBS)

# The reference implementations, built so the tests can compare against them.
$(BIN)/ref/%: reference/%.c | $(BIN)/ref
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

# Offline harnesses: the same DSP core as the filter, driven from stdin so it
# can be diffed against the reference byte for byte.
$(BIN)/t_%: tests/t_%.c src/dsp.h | $(BIN)
	$(CC) $(CFLAGS) $(WARN) -o $@ $< $(LIBS)

refs: $(REFS:%=$(BIN)/ref/%)

# The offline half: DSP cores diffed against the reference implementations.
test: all refs $(REFS:%=$(BIN)/t_%)
	@tests/run.sh

# Everything, including the live PipeWire tests. Needs a running daemon.
check: all refs $(REFS:%=$(BIN)/t_%)
	@tests/all.sh

clean:
	rm -rf $(BIN) $(OBJ)

.PHONY: all clean test check refs
