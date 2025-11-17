#!/bin/bash

g++ -O3 -std=c++17 -march=native -mavx512f -pthread -o read read.cpp

g++ -O3 -std=c++17 -march=native -mavx512f -pthread -o read_once read_once.cpp



