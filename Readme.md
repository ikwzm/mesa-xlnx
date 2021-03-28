mesa-xlnx
====================

Overview
--------------------

### Introduction

This Repository provides debian package for the xlnx mesa dri driver.

For Ubuntu 20.04
--------------------

### Download

```console
shell$ git clone -b mesa-xlnx-20.2.6-0ubuntu0.20.04.1 https://github.com/ikwzm/mesa-xlnx
```

### Install

#### Install Debian Package

```console
shell$ sudo dpkg -i ./mesa-xlnx/libgl1-mesa-xlnx-dri_20.2.6-0ubuntu0.20.04.1_arm64.deb
```

#### Disable Rendering with Lima

**Note: The current Lima DRI Driver does not support 3D textures. As a result, some gnome applications do not work. Therefore, avoid using Lima rendering when launching gnome.**

```console
shell$ cat <<EOT > /etc/systemd/user/gnome-shell-x11.service.d/override.conf
[Service]
Environment="LIBGL_ALWAYS_SOFTWARE=1"
EOT
```

