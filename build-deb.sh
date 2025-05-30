#!/bin/bash

set -e

cd /tmp/build/opensips
git submodule update --init
mkdir /tmp/deb/
make deb-orig-tar
mv ../opensips_3.7.0.orig.tar.gz ../opensips_3.6.0-dev.orig.tar.gz
make deb
cp ../*.deb /tmp/deb
cd /tmp/deb
dpkg-scanpackages . /dev/null > Packages
dpkg-scanpackages . /dev/null | gzip -9c > Packages.gz
