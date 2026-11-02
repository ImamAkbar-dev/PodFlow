# ============================================================================ #
# PodFlow — Concurrent Chunked Audio/Media Streaming Server                    #
# Shared Team Makefile (Session 1 & Session 2 Ready)                           #
# ============================================================================ #

CC          := gcc
CFLAGS      := -std=c11 -Wall -Wextra -Werror -pthread -g -Iinclude
LDFLAGS     := -pthread
ASAN_FLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer

# Directory paths
INC_DIR     := include
COMMON_DIR  := src/common
SERVER_DIR  := src/server
CLIENT_DIR  := src/client
TEST_DIR    := tests
MEDIA_DIR   := media

# Shared headers & sources (Member 1 & Member 2)
HEADERS     := $(wildcard $(INC_DIR)/*.h)
COMMON_SRCS := $(COMMON_DIR)/io_utils.c $(COMMON_DIR)/protocol.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

# Server sources: always compile server.c; automatically include pacing.c and
# ring_buffer.c only if they exist and are non-empty (prevents Session 1 build
# failures when Session 2 files are still empty placeholders).
SERVER_EXTRA_SRCS := $(shell for f in $(SERVER_DIR)/pacing.c $(SERVER_DIR)/ring_buffer.c; do \
                        if [ -s "$$f" ]; then echo "$$f"; fi; \
                     done)
SERVER_SRCS := $(SERVER_DIR)/server.c $(SERVER_EXTRA_SRCS)
SERVER_OBJS := $(SERVER_SRCS:.c=.o)

# Client sources (Member 3)
CLIENT_SRCS := $(CLIENT_DIR)/client.c
CLIENT_OBJS := $(CLIENT_SRCS:.c=.o)

# Output binaries (placed in repo root per CLI contract: ./server & ./client)
SERVER_BIN  := server
CLIENT_BIN  := client
TEST_PROTO  := test_protocol
TEST_ASAN   := test_protocol_asan
PACING_BIN  := pacing_experiment

.PHONY: all m2 server-bin client-bin test test-asan test-valgrind test-all \
        integration-test media clean help

# Default target: builds server, client, and protocol unit test binary
all: $(SERVER_BIN) $(CLIENT_BIN) $(TEST_PROTO)

# ============================================================================ #
# MEMBER 2 STANDALONE TARGETS (Works even if server.c / client.c are empty)    #
# ============================================================================ #

# Build only common object files and protocol unit tests
m2: $(COMMON_OBJS) $(TEST_PROTO)

$(TEST_PROTO): $(TEST_DIR)/test_protocol.c $(COMMON_OBJS) $(HEADERS)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_protocol.c $(COMMON_OBJS) -o $@ $(LDFLAGS)

test: $(TEST_PROTO)
	./$(TEST_PROTO)

test-asan: $(TEST_DIR)/test_protocol.c $(COMMON_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $(TEST_DIR)/test_protocol.c $(COMMON_SRCS) -o $(TEST_ASAN) $(LDFLAGS)
	./$(TEST_ASAN)

test-valgrind: $(TEST_PROTO)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 ./$(TEST_PROTO)

test-all: test test-asan test-valgrind

# ============================================================================ #
# CORE APPLICATION BINARIES (Server & Client)                                  #
# ============================================================================ #

$(SERVER_BIN): $(SERVER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(SERVER_OBJS) $(COMMON_OBJS) -o $@ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(CLIENT_OBJS) $(COMMON_OBJS) -o $@ $(LDFLAGS)

# Optional Session 2 pacing experiment binary
$(PACING_BIN): $(TEST_DIR)/pacing_experiment.c $(COMMON_OBJS) $(HEADERS)
	$(CC) $(CFLAGS) $(TEST_DIR)/pacing_experiment.c $(SERVER_EXTRA_SRCS) $(COMMON_OBJS) -o $@ $(LDFLAGS)

# Pattern rule for compiling .c files into .o object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# ============================================================================ #
# MEDIA GENERATION & END-TO-END QA SCRIPTS                                     #
# ============================================================================ #

media:
	bash $(MEDIA_DIR)/generate_wav.sh

integration-test: all
	@if [ -s $(TEST_DIR)/test_framing.sh ]; then bash $(TEST_DIR)/test_framing.sh; fi
	@if [ -s $(TEST_DIR)/test_md5.sh ]; then bash $(TEST_DIR)/test_md5.sh; fi

# ============================================================================ #
# CLEANUP & HELP                                                               #
# ============================================================================ #

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) $(TEST_PROTO) $(TEST_ASAN) $(PACING_BIN)
	rm -f *.o $(COMMON_DIR)/*.o $(SERVER_DIR)/*.o $(CLIENT_DIR)/*.o $(TEST_DIR)/*.o

help:
	@echo "PodFlow Build Targets:"
	@echo "  make all            - Build ./server, ./client, and ./test_protocol"
	@echo "  make m2             - Build only Member 2 files (io_utils.o, protocol.o, test_protocol)"
	@echo "  make test           - Compile and run ./test_protocol"
	@echo "  make test-asan      - Run protocol tests with AddressSanitizer + UBSan"
	@echo "  make test-valgrind  - Run protocol tests under Valgrind Memcheck"
	@echo "  make test-all       - Run standard, ASan/UBSan, and Valgrind tests sequentially"
	@echo "  make media          - Generate reference WAV files via media/generate_wav.sh"
	@echo "  make clean          - Remove all compiled binaries and .o object files"