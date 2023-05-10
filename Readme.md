mesa-xlnx
====================

Overview
--------------------

### Introduction

This Repository provides debian package for the xlnx mesa dri driver.

For Ubuntu 22.04.2
--------------------

### Install Debian Package

#### Download

```console
shell$ git clone -b mesa-xlnx-dri_22.2.5-0ubuntu0.1-22.04.1 https://github.com/ikwzm/mesa-xlnx
```

#### Install

```console
shell$ sudo apt-get install ./mesa-xlnx/libgl1-mesa-xlnx-dri_22.2.5-0ubuntu0.1~22.04.1_arm64.deb
```

#### Disable Rendering with Lima(if necessary)

The Lima DRI Driver is currently under development, so the application may not work.
If you want to run an application that doesn't work, set the environment variable LIBGL_ALWAYS_SOFTWARE to 1 when you start gnome-shell to disable rendering with Lima as follows.

```console
shell$ cat <<EOT > /etc/systemd/user/gnome-shell-x11.service.d/override.conf
[Service]
Environment="LIBGL_ALWAYS_SOFTWARE=1"
EOT
```

### Build Debian Package

#### Install Tools for build Mesa

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx$ sudo apt-get build-dep mesa=22.2.5-0ubuntu0.1~22.04.1
fpga@ubuntu-fpga:~/work/mesa-xlnx$ sudo apt-get install cmake valgrind libunwind-dev libconfig-dev
```

#### Download Mesa Source Code

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx$ apt-get source mesa=22.2.5-0ubuntu0.1~22.04.1
fpga@ubuntu-fpga:~/work/mesa-xlnx$ cd mesa-22.2.5
```

#### Patch for xlnx

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-22.2.5$ patch -p1 < ../files/mesa-xlnx-22.2.5.diff
```

#### Build 

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-22.2.5$ sudo debian/rules binary
```

```console
fpga@ubuntu-fpga:~/work/mesa-xlnx/mesa-22.2.5$ dpkg --info ../libgl1-mesa-xlnx-dri_22.2.5-0ubuntu0.1~22.04.1_arm64.deb 
 new Debian package, version 2.0.
 size 7266128 bytes: control archive=939 bytes.
    1109 bytes,    21 lines      control              
     397 bytes,     5 lines      md5sums              
 Package: libgl1-mesa-xlnx-dri
 Source: mesa
 Version: 22.2.5-0ubuntu0.1~22.04.1
 Architecture: arm64
 Maintainer: Debian X Strike Force <debian-x@lists.debian.org>
 Installed-Size: 22677
 Depends: libc6 (>= 2.34), libdrm-amdgpu1 (>= 2.4.105), libdrm-nouveau2 (>= 2.4.66), libdrm-radeon1 (>= 2.4.31), libdrm2 (>= 2.4.89), libelf1 (>= 0.142), libexpat1 (>= 2.0.1), libgcc-s1 (>= 3.0), libglapi-mesa (= 22.2.5-0ubuntu0.1~22.04.1), libllvm15, libsensors5 (>= 1:3.5.0), libstdc++6 (>= 11), libxcb-dri3-0 (>= 1.13), libzstd1 (>= 1.4.0), zlib1g (>= 1:1.1.4)
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

