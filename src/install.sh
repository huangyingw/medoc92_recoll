#!/bin/zsh
SCRIPT=$(realpath "$0")
SCRIPTPATH=$(dirname "$SCRIPT")
cd "$SCRIPTPATH"

# ./install_prerequisite.sh

./autogen.sh
./configure && \
    make && \
    make install
