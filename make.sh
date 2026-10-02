#!/bin/sh
# Run make in the m68k cross-compiler container: build it once with
#   sudo docker build -t se30-build -f docker/Dockerfile.se30-build docker
#   sudo docker build -t se30-xbuild -f docker/Dockerfile.se30-xbuild docker
cd "$(dirname "$0")" && exec sudo docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src -w /src se30-xbuild make "$@"
