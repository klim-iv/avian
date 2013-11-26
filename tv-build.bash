#!/bin/bash

make platform=linux arch=arm MAKEFLAGS= \
  use-lto=false \
  cxx="${CXX}" \
  cc="${CC}" \
  ar="${AR}" \
  ranlib="${RANLIB}" \
  strip="${STRIP}" \
  extra-cflags=" \
    -I${OPENJDK_BIN}/include \
    -I${OPENJDK_BIN}/include/linux" \
  openjdk=${OPENJDK_BIN} \
  openjdk-src=${OPENJDK_SRC}/jdk/src
