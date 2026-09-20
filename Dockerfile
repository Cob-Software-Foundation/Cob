# =============================================================================
# Dockerfile -- Cob Toolchain stress-test image
# =============================================================================
# Builds cob_interp_full (SQLite + _cobwindow) in a clean, reproducible
# Ubuntu environment and runs tools/stress/run_stress.sh against it. This
# exists to catch regressions that only show up under sustained load
# (interpreter loop overhead, SQLite under many statements, repeated window
# open/close cycles) in an environment that doesn't depend on whatever's
# already installed on a developer's machine or a GitHub-hosted runner's
# image -- everything this needs is installed explicitly, below.
#
# Usage:
#   docker build -t cob-stress .
#   docker run --rm cob-stress                          # one pass
#   docker run --rm cob-stress --iterations 20           # repeat 20x
#
# This does NOT test cob_interp_db (Tcl/Tk) -- that's a separate, much
# heavier build (see Release.txt for the autoconf/busybox-ash fragility
# that comes with it) and isn't part of the default `make` this image runs.
# =============================================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build tools + everything _cobwindow's raylib backend needs (X11 + OpenGL
# dev headers for GLFW's X11 backend) + Xvfb so a real (virtual) display is
# available for window_open() to actually open a window against, same as
# .github/workflows/build.yml's test_extensions job does.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        make \
        libx11-dev \
        libxrandr-dev \
        libxinerama-dev \
        libxcursor-dev \
        libxi-dev \
        libgl1-mesa-dev \
        xvfb \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /cob
COPY . /cob

# Build once at image-build time so `docker run` starts fast and a broken
# build fails the image build itself, not silently at container-run time.
RUN make

RUN chmod +x tools/stress/run_stress.sh

ENTRYPOINT ["sh", "tools/stress/run_stress.sh"]
CMD []
