#!/usr/bin/env bash

WD=$(pwd)
DIR=$(cd $(dirname "$0") && pwd)

if [ "$WD" != "$DIR" ]; then
    echo 'must execute in then same directory as run.sh'
    exit 1
fi

BIN='./../../build/weenet'
if [ -f $BIN ] && [ -x $BIN ]; then
    LUA_PATH=../../src/lualib/lua/?.lua LUA_CPATH=../../src/lualib/c/?.so $BIN -c weenet.conf
else
    echo 'build weenet first'
fi
