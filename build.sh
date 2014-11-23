#!/bin/sh
gcc -Wall fusexmp.c `pkg-config fuse --cflags --libs` -o fusexmp