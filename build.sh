#!/bin/bash

g++ -O3 -std=c++11 -g -fPIC -shared -o bumpmalloc.so bumpmalloc.cc
