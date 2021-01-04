mesa-xlnx
====================

Overview
--------------------

### Introduction

This Repository provides an environment for building debian package for the xlnx mesa dri.

Build Debian Package
--------------------

### Requirement

  * ZynqMP-FPGA-Ubuntu20.04-Ultra96

### Download This Repository

```console
fpga@ubuntu-fpga:~/work$ git clone https://github.com/ikwzm/mise-xlnx.git
fpga@ubuntu-fpga:~/work$ cd mesa-xlnx
```

### Install Tools for build Mesa

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx$ sudo apt-get build-dep mesa
fpga@ubuntu-fpga:~/work/mesa-xlnx$ sudo apt-get install cmake valgrind libunwind-dev libconfig-dev
```

### Download Mesa Source Code

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx$ sudo apt-get source mesa
fpga@ubuntu-fpga:~/work/mesa-xlnx$ cd mesa-20.0.8
```

### Patch for xlnx_dri

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-20.0.8$ patch < ../files/mesa_20.0.8-0ubuntu1~20.04.1-xlnx.diff
```

### Build Mesa Debian Package

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-20.0.8$ sudo debian/rules binary
```

### Build libgl1-mesa-xlnx-dri Debian Package

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-20.0.8$ cd ..
fpga@ubuntu-fpga:~/work/mesa-xlnx$ sudo debian/rules binary
rm -rf debian/tmp
install -d debian/tmp/DEBIAN debian/tmp/usr/lib/aarch64-linux-gnu/dri
install --strip mesa-20.0.8/build/src/gallium/targets/dri/libgallium_dri.so debian/tmp/usr/lib/aarch64-linux-gnu/dri/xlnx_dri.so
dpkg-gencontrol -DArchitecture=arm64
cp -a debian/postinst       debian/tmp/DEBIAN
cp -a debian/prerm          debian/tmp/DEBIAN
cp -a debian/postrm         debian/tmp/DEBIAN
chown -R root:root debian/tmp
chmod -u+w,go=rX debian/tmp
dpkg-deb --build debian/tmp ..
dpkg-deb: building package 'libgl1-mesa-xlnx-dri' in '../libgl1-mesa-xlnx-dri_20.0.8-0ubuntu1~20.04.1_arm64.deb'.
```

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx$ dpkg --info ../libgl1-mesa-xlnx-dri_20.0.8-0ubuntu1~20.04.1_arm64.deb
 new Debian package, version 2.0.
 size 4726204 bytes: control archive=524 bytes.
     345 bytes,    11 lines      control
      14 bytes,     2 lines   *  postinst             #!/bin/sh
      13 bytes,     1 lines   *  postrm               #!/bin/sh
      14 bytes,     2 lines   *  prerm                #!/bin/sh
 Package: libgl1-mesa-xlnx-dri
 Source: mesa-xlnx
 Version: 20.0.8-0ubuntu1~20.04.1
 Architecture: arm64
 Maintainer: ikwzm <ichiro_k@ca2.so-net.ne.jp>
 Installed-Size: 16641
 Depends: libgl1-mesa-dri
 Section: devel
 Priority: optional
 Homepage: <https://github.com/ikwzm/mesa-xlnx>
 Description: free implementation of the OpenGL API -- xlnx dri module
```
