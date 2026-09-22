# =============================================================================
# Reproducible Linux build and measurement environment (ESD.md sections 2, 3, 36).
#
# The project is developed on Windows but several requirements can only be met
# on Linux:
#
#   * ASan/UBSan. The MinGW toolchain does not ship the sanitizer runtimes, so
#     the Debug configuration of ESD.md section 34 is a no-op on Windows.
#   * clang-format and clang-tidy (ESD.md sections 2 and 35).
#   * A second compiler. Building the same sources with GCC and Clang catches
#     undefined behaviour that a single compiler's codegen happens to hide.
#   * A pinned environment, so a reviewer can reproduce the build exactly.
#
# Debian is pinned by digest-bearing tag rather than :latest so that rebuilding
# this image a year from now yields the same compiler versions. A benchmark
# whose toolchain silently changed is not reproducible.
# =============================================================================
FROM debian:bookworm-20250630-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# -----------------------------------------------------------------------------
# Toolchain.
#
# linux-perf is installed, but note that hardware counters are unavailable when
# this image runs under WSL2: the WSL2 kernel does not expose a `cpu` PMU, so
# perf can report only software events. See docs/experiments/profiling.md.
# The package is present so the same image works on a native Linux host, where
# the counters PRD.md section 20 asks for are genuinely collectable.
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install --no-install-recommends -y \
        build-essential \
        g++-12 \
        clang-16 \
        clang-format-16 \
        clang-tidy-16 \
        # Clang's sanitizer runtimes live in a separate Debian package. Without
        # it, -fsanitize=address compiles but fails at link with a missing
        # libclang_rt.asan-x86_64.a, so the Debug profile silently stops
        # providing the coverage ESD.md section 34 asks for.
        libclang-rt-16-dev \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        curl \
        python3 \
        python3-pip \
        python3-venv \
        linux-perf \
        pkg-config \
        libomp-16-dev \
    && rm -rf /var/lib/apt/lists/*

# Unversioned aliases so CMake presets and scripts do not hard-code a version.
RUN update-alternatives --install /usr/bin/clang        clang        /usr/bin/clang-16 100 && \
    update-alternatives --install /usr/bin/clang++      clang++      /usr/bin/clang++-16 100 && \
    update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-16 100 && \
    update-alternatives --install /usr/bin/clang-tidy   clang-tidy   /usr/bin/clang-tidy-16 100 && \
    update-alternatives --install /usr/bin/gcc          gcc          /usr/bin/gcc-12 100 && \
    update-alternatives --install /usr/bin/g++          g++          /usr/bin/g++-12 100

# -----------------------------------------------------------------------------
# Python tooling for training, statistics and plotting (ESD.md section 2).
#
# Installed into a virtualenv rather than system-wide: Debian marks its system
# Python as externally managed, and a venv keeps the pinned analysis stack from
# colliding with distribution packages.
# -----------------------------------------------------------------------------
ENV VIRTUAL_ENV=/opt/lpq-venv
RUN python3 -m venv "$VIRTUAL_ENV"
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# A login shell (`bash -lc`, which is what `docker run ... bash -lc` and most
# CI steps use) sources /etc/profile and rebuilds PATH from scratch, discarding
# the ENV above. Without this the venv is invisible to exactly the invocation
# style the scripts use, and `import numpy` fails inside the image that just
# installed numpy.
RUN echo 'export VIRTUAL_ENV=/opt/lpq-venv'        >  /etc/profile.d/10-lpq-venv.sh && \
    echo 'export PATH="$VIRTUAL_ENV/bin:$PATH"'    >> /etc/profile.d/10-lpq-venv.sh && \
    chmod 0644 /etc/profile.d/10-lpq-venv.sh

COPY python/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir --upgrade pip && \
    pip install --no-cache-dir -r /tmp/requirements.txt && \
    rm /tmp/requirements.txt

# -----------------------------------------------------------------------------
# Build the project inside the image so that `docker build` itself is a smoke
# test of the Linux build. Sources are mounted over this at run time for
# development; the baked copy proves the image can build from a clean tree.
# -----------------------------------------------------------------------------
WORKDIR /workspace

# Benchmarks executed in a container are wall-clock comparable only when the
# container is not CPU-throttled. scripts/benchmark records cgroup limits into
# the result metadata so a throttled run cannot be mistaken for a slow
# algorithm.
CMD ["/bin/bash"]
