#!/usr/bin/env bash

WD=$(pwd)
DIR=$(cd $(dirname "$0") && pwd)

if [ "$WD" != "$DIR" ]; then
	echo 'must execute in then same directory as run.sh'
	exit 1
fi

./../../build/weenet -c weenet.conf
