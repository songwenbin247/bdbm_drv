#!/bin/bash

sudo fio --randrepeat=1 \
	--ioengine=libaio \
	--name=fio\
	--filename=/media/blueDBM/fio \
	--bs=4k \
	--iodepth=128 \
	--size=1000M \
	--readwrite=randwrite \
	--rwmixwrite=100 \
	--overwrite=1 \
	--numjobs=1 \
	--direct=0 \
	--buffer=0
