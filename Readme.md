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
fpga@ubuntu-fpga:~/work/mesa-xlnx$ apt-get source mesa=20.2.6-0ubuntu0.20.04.1
fpga@ubuntu-fpga:~/work/mesa-xlnx$ cd mesa-20.2.6
```

### Patch for xlnx_dri

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-20.2.6$ patch -p1 < ../files/mesa-xlnx-20.2.6.diff
```

### Build Mesa Debian Package

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-20.2.6$ sudo debian/rules binary
```

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-20.2.6$ dpkg --info ../libgl1-mesa-xlnx-dri_20.2.6-0ubuntu0.20.04.1_arm64.deb 
 new Debian package, version 2.0.
 size 5167796 bytes: control archive=1048 bytes.
    1095 bytes,    21 lines      control              
     397 bytes,     5 lines      md5sums              
 Package: libgl1-mesa-xlnx-dri
 Source: mesa
 Version: 20.2.6-0ubuntu0.20.04.1
 Architecture: arm64
 Maintainer: Debian X Strike Force <debian-x@lists.debian.org>
 Installed-Size: 18260
 Depends: libc6 (>= 2.29), libdrm-amdgpu1 (>= 2.4.100), libdrm-nouveau2 (>= 2.4.66), libdrm-radeon1 (>= 2.4.31), libdrm2 (>= 2.4.89), libelf1 (>= 0.142), libexpat1 (>= 2.0.1), libglapi-mesa (= 20.2.6-0ubuntu0.20.04.1), libllvm11 (>= 1:9~svn298832-1~), libsensors5 (>= 1:3.5.0), libstdc++6 (>= 5.2), libunwind8, libzstd1 (>= 1.3.2), zlib1g (>= 1:1.1.4)
 Section: libs
 Priority: optional
 Multi-Arch: same
 Homepage: https://mesa3d.org/
 Description: free implementation of the OpenGL API -- xlnx dri module
  This version of Mesa provides GLX and DRI capabilities: it is capable of
  both direct and indirect rendering.  For direct rendering, it can use DRI
  modules from the libgl1-mesa-dri package to accelerate drawing.
  .
  This package does not include the OpenGL library itself, only the DRI
  modules for accelerating direct rendering.
  .
  For a complete description of Mesa, please look at the
  libglx-mesa0 package.
```
