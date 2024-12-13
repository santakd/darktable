#!/bin/bash
#
# Script to build darktable with default configuration
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/
scriptDir=$(pwd)

# Set variables
buildDir="${scriptDir}/../../build"
installDir="${buildDir}/macosx"

# Options
options=" \
    -DUSE_GRAPHICSMAGICK=OFF \
    -DUSE_IMAGEMAGICK=ON \
    -DBUILD_CURVE_TOOLS=ON \
    -DBUILD_NOISE_TOOLS=ON \
"

# Check for previous attempt and clean
if [[ -d "$buildDir" ]]; then
    echo "Deleting directory $buildDir ... "
    rm -rf "$buildDir"
fi

# Clean build here
../../build.sh --install --build-generator Ninja --build-type Release --prefix "$installDir" -- $options
