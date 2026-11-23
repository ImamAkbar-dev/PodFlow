# ======================================================================
# PodFlow - Makefile (owner: Member 1)
#
# Contract flags: C11, -D_GNU_SOURCE (ppoll), -Wall -Wextra -Werror,
#                 -pthread, -g.  Sources are found with wildcards: a new
#                 .c file in src/{common,server,client} is picked up
#                 automatically; empty skeleton files are ignored.
#
#   make              build ./server and ./client
#   make check        ONE COMMAND: run every test suite (unit, shell, python,
#                     small load test) and print one summary (about 30 s)
#   make check-quick  unit tests + framing + jitter self-test (about 10 s)
#   make bench        FULL load benchmark (1..200 clients + back-pressure) and
#                     plots; writes into $(RESULTS_DIR)/ (default: results/)
#   make test         build + run unit tests (protocol, ring buffer, session)
#   make test-shell   shell tests (framing, md5, control)
#   make asan         build with AddressSanitizer+UBSan into build/asan and
#                     run the unit tests with it
#   make tsan         same with ThreadSanitizer into build/tsan
#   make valgrind     unit tests under Valgrind memcheck
#   make helgrind     unit tests under Valgrind Helgrind
#   make valgrind-server / helgrind-server   run ./server under Valgrind
#                     (interactive: stop it with Ctrl+C)
#   make clean
# ======================================================================

CC      := gcc
WERROR  ?= -Werror
PORT    ?= 8888
BUILD   := build
RESULTS_DIR   ?= results
BENCH_COUNTS  ?= 1,10,50,100,200

# ---- build variant: plain (default) | asan | tsan ---------------------
VARIANT ?=
ifeq ($(VARIANT),asan)
  SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -O1
else ifeq ($(VARIANT),tsan)
  SAN := -fsanitize=thread -fno-omit-frame-pointer -O1
else ifneq ($(VARIANT),)
  $(error unknown VARIANT '$(VARIANT)' (use asan or tsan))
endif

OBJROOT := $(BUILD)$(if $(VARIANT),/$(VARIANT))
BIN     := $(if $(VARIANT),$(OBJROOT)/bin,.)

CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra $(WERROR) -pthread -g $(SAN) \
           -Iinclude -MMD -MP
LDFLAGS := -pthread $(SAN)

# ---- sources (wildcards; empty files are skipped) ---------------------
nonempty = $(foreach f,$(1),$(if $(shell test -s $(f) && echo y),$(f)))
obj      = $(patsubst %.c,$(OBJROOT)/%.o,$(1))

SRC_COMMON := $(call nonempty,$(wildcard src/common/*.c))
SRC_SERVER := $(call nonempty,$(wildcard src/server/*.c))
SRC_CLIENT := $(call nonempty,$(wildcard src/client/*.c))

OBJ_COMMON := $(call obj,$(SRC_COMMON))
OBJ_SERVER := $(call obj,$(SRC_SERVER))
OBJ_CLIENT := $(call obj,$(SRC_CLIENT))

OBJ_RB      := $(OBJROOT)/src/server/ring_buffer.o
OBJ_SESSION := $(OBJROOT)/src/server/stream_session.o
OBJ_PACING  := $(OBJROOT)/src/server/pacing.o

SERVER_HAS_SESSION := $(filter %/stream_session.c,$(SRC_SERVER))

UNIT_TESTS := test_protocol test_ring_buffer test_session
UNIT_BINS  := $(addprefix $(BIN)/,$(UNIT_TESTS))

.DEFAULT_GOAL := all
.PHONY: all test test-shell test-all check check-quick bench asan tsan sanitize \
        valgrind helgrind valgrind-server helgrind-server clean help

all: $(BIN)/server $(BIN)/client

# In a sanitizer variant the binaries live in build/<variant>/bin, so give
# the short names back as phony aliases (in a plain build `server` IS the file).
ifneq ($(VARIANT),)
.PHONY: server client
server: $(BIN)/server
client: $(BIN)/client
endif

# ---- programs ---------------------------------------------------------
$(BIN)/server: $(OBJ_COMMON) $(OBJ_SERVER)
	@test -n "$(SERVER_HAS_SESSION)" || { \
	  echo "error: src/server/stream_session.c is missing (Member 2): cannot link the server"; \
	  exit 1; }
	@mkdir -p $(BIN)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BIN)/client: $(OBJ_COMMON) $(OBJ_CLIENT)
	@test -n "$(SRC_CLIENT)" || { \
	  echo "error: no non-empty source in src/client (Member 3)"; exit 1; }
	@mkdir -p $(BIN)
	$(CC) $^ -o $@ $(LDFLAGS)

# ---- unit tests and tools --------------------------------------------
$(BIN)/test_protocol: $(OBJROOT)/tests/test_protocol.o $(OBJ_COMMON)
	@mkdir -p $(BIN)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BIN)/test_ring_buffer: $(OBJROOT)/tests/test_ring_buffer.o $(OBJ_RB)
	@mkdir -p $(BIN)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BIN)/test_session: $(OBJROOT)/tests/test_session.o $(OBJ_SESSION) $(OBJ_RB)
	@mkdir -p $(BIN)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BIN)/pacing_experiment: $(OBJROOT)/tests/pacing_experiment.o $(OBJ_PACING)
	@mkdir -p $(BIN)
	$(CC) $^ -o $@ $(LDFLAGS)

# ---- generic compile rule (with automatic header dependencies) --------
$(OBJROOT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- tests ------------------------------------------------------------
# Unit tests run from the repository root (test_session makes ./test_media_tmp).
test: $(UNIT_BINS)
	@set -e; for t in $(UNIT_BINS); do echo "== $$t"; $$t; done
	@echo "== all unit tests passed"

test-shell: all
	@set -e; for s in tests/test_framing.sh tests/test_md5.sh tests/test_control.sh; do \
	  if [ -s $$s ]; then echo "== $$s"; bash $$s; \
	  else echo "[skip] $$s missing or empty"; fi; \
	done

# One command for everything (see tests/run_all.sh). test-all is an alias.
check test-all:
	@bash tests/run_all.sh

check-quick:
	@bash tests/run_all.sh --quick

# Full benchmark: overwrites the files in $(RESULTS_DIR)/ (committed by Member 3).
bench: all
	python3 tests/load_test.py --counts $(BENCH_COUNTS) --results-dir $(RESULTS_DIR)
	python3 tests/plot_results.py --results-dir $(RESULTS_DIR)

# ---- sanitizers: a separate build tree, so plain objects are untouched -
asan:
	@$(MAKE) --no-print-directory VARIANT=asan sanitize
tsan:
	@$(MAKE) --no-print-directory VARIANT=tsan sanitize

sanitize: all $(UNIT_BINS)
	@set -e; for t in $(UNIT_BINS); do echo "== $(VARIANT): $$t"; \
	  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
	  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 $$t; done
	@echo "== $(VARIANT): unit tests clean; binaries in $(BIN)/"

# ---- Valgrind ---------------------------------------------------------
VG_CHECK = command -v valgrind >/dev/null 2>&1 || { \
             echo "valgrind is not installed: sudo apt install valgrind"; exit 1; }
VG_MEM   = valgrind --leak-check=full --show-leak-kinds=all \
             --errors-for-leak-kinds=definite,indirect --error-exitcode=1
VG_HELG  = valgrind --tool=helgrind --error-exitcode=1

valgrind: $(UNIT_BINS)
	@$(VG_CHECK)
	@set -e; for t in $(UNIT_BINS); do echo "== memcheck: $$t"; $(VG_MEM) $$t; done

helgrind: $(UNIT_BINS)
	@$(VG_CHECK)
	@set -e; for t in $(UNIT_BINS); do echo "== helgrind: $$t"; $(VG_HELG) $$t; done

valgrind-server: $(BIN)/server
	@$(VG_CHECK)
	$(VG_MEM) ./server $(PORT)

helgrind-server: $(BIN)/server
	@$(VG_CHECK)
	$(VG_HELG) ./server $(PORT)

# ---- housekeeping -----------------------------------------------------
clean:
	rm -rf $(BUILD) server client test_protocol test_ring_buffer test_session \
	       pacing_experiment test_media_tmp

help:
	@sed -n '3,26p' Makefile | sed 's/^# \{0,1\}//'

# header dependency files generated by -MMD
-include $(wildcard $(OBJROOT)/src/*/*.d $(OBJROOT)/tests/*.d)