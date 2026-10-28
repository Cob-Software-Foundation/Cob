# syntax=docker/dockerfile:1
# =============================================================================
# Dockerfile -- Cob in a container
# =============================================================================
# The interpreter + REPL (cob_interp), the native compiler (popcorn_comp), the
# package manager (farmer) and the whole standard library (porch, cornmath,
# kernels, almanac, granary, pantry), ready to run.
#
#   docker build -t cob .
#   docker run -it --rm cob                                  # the REPL
#   docker run --rm -v "$PWD:/work" cob hello.cob            # run a program
#   docker run --rm -v "$PWD:/work" cob popcorn_comp hello.strawberry -o hello
#
# Files you create in a bind-mounted /work belong to whoever the container
# runs as. The image runs as an unprivileged user (uid 10001); to read and
# write your own files as yourself add:  --user "$(id -u):$(id -g)"
#
# A program built by popcorn_comp INSIDE this image is for this image: it
# links the standard library in /opt/cob/lib and the image's glibc, so run it
# here (--entrypoint /work/hello) rather than copying it to another machine.
#
# Not in this image: the barn (windows) and tassel (Tcl/Tk) modules -- a
# container has no display, and they need X11/OpenGL and a long from-source
# build. GitHub Actions builds and publishes this image: see
# .github/workflows/docker.yml (ghcr.io/cob-software-foundation/cob).
# =============================================================================
ARG UBUNTU_VERSION=24.04

# ---- build: compile everything, and run the regression suite -----------------
FROM ubuntu:${UBUNTU_VERSION} AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        make \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN make cob_interp popcorn_comp farmer stdlib

# A broken build must fail the image build, not show up when someone runs it.
# (Skip with --build-arg RUN_TESTS=0 if you are iterating on the Dockerfile.)
ARG RUN_TESTS=1
RUN if [ "$RUN_TESTS" = "1" ]; then make test; fi

# the payload the runtime stage copies: the three tools, and lib/ beside them
RUN mkdir -p /out/lib \
    && cp bin/cob_interp bin/popcorn_comp bin/farmer /out/ \
    && cp bin/lib/* /out/lib/

# ---- runtime: just what is needed to run and compile Cob ---------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="Cob" \
      org.opencontainers.image.description="The Cob language: interpreter and REPL, native compiler, package manager and corn-themed standard library" \
      org.opencontainers.image.url="https://cob.pixel-pulse.dev/" \
      org.opencontainers.image.source="https://github.com/Cob-Software-Foundation/Cob" \
      org.opencontainers.image.licenses="PolyForm-Noncommercial-1.0.0" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}"

ENV DEBIAN_FRONTEND=noninteractive
# gcc + libc6-dev: popcorn_comp transpiles to C and hands it to a real C
# compiler (it finds `cc` on PATH). curl: farmer downloads packages with it.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
        curl \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home --shell /bin/sh cob

# The tools find the standard library at <their own directory>/lib, so keep
# the layout from the build stage. The /usr/local/bin entries are symlinks
# (the tools resolve them, so /opt/cob/lib is still found).
COPY --from=build /out/ /opt/cob/
COPY docker/entrypoint.sh /usr/local/bin/cob-entrypoint
RUN chmod +x /usr/local/bin/cob-entrypoint \
    && ln -s /opt/cob/cob_interp   /usr/local/bin/cob_interp \
    && ln -s /opt/cob/cob_interp   /usr/local/bin/cob \
    && ln -s /opt/cob/popcorn_comp /usr/local/bin/popcorn_comp \
    && ln -s /opt/cob/farmer       /usr/local/bin/farmer

# popcorn_comp spawns "zig cc" unless told otherwise (releases bundle Zig; this
# image does not), so point it at the C compiler installed above.
ENV CobCC=cc

USER cob
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/cob-entrypoint"]
CMD []
