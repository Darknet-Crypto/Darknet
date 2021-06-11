#!/usr/bin/env bash

export LC_ALL=C

INFILE="$1"
OUTFILE="$2"

hexdump -v -e '8/1 "0x%02x, "' -e '"\n"' $INFILE | sed -e 's/0x  ,//g' >> $OUTFILE