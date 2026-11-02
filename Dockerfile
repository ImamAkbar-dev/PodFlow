# ============================================================================ #
# PodFlow — Concurrent Chunked Audio/Media Streaming Server                    #
# Environment: Ubuntu 22.04 LTS, GCC (C11), POSIX Threads, Valgrind, Python 3  #
# ============================================================================ #

FROM ubuntu:22.04

# Prevent interactive timezone prompts during apt-get install
ENV DEBIAN_FRONTEND=noninteractive

# Install C11 build toolchain, Valgrind, Python3 (for test scripts), and net tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    make \
     libc6-dev \
    valgrind \
    python3 \
    python3-pip \
    iproute2 \
    netcat-openbsd \
    tcpdump \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set working directory inside container
WORKDIR /app

# Copy project files into container
COPY . /app

# Generate reference WAV files if media/generate_wav.sh is implemented,
# then compile all available targets and run Member 2 protocol unit tests
RUN if [ -s media/generate_wav.sh ]; then bash media/generate_wav.sh; fi && \
    if [ -s src/server/server.c ] && [ -s src/client/client.c ]; then \
        make all && make test; \
    else \
        make m2 && make test; \
    fi

# Expose TCP port 8888 per shared contract
EXPOSE 8888

# Default command: run streaming server on port 8888 (or fallback to test suite)
CMD ["sh", "-c", "if [ -x ./server ]; then ./server 8888; else ./test_protocol; fi"]