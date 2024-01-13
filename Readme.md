mesa-xlnx
====================

Overview
--------------------

### Introduction

This Repository provides debian package for the xlnx mesa dri driver.


Release
--------------------

| Ubuntu Version | Debian Package                                           | Git Branch     |
|:---------------|:---------------------------------------------------------|:---------------|
| 22.04.3        | libgl1-mesa-xlnx-dri_23.0.4-0ubuntu1~22.04.1_arm64.deb   | [mesa-xlnx-dri_23.0.4-0ubuntu1-22.04.1](https://github.com/ikwzm/mesa-xlnx/tree/mesa-xlnx-dri_23.0.4-0ubuntu1-22.04.1)
| 22.04.2        | libgl1-mesa-xlnx-dri_22.2.5-0ubuntu0.1~22.04.3_arm64.deb | [mesa-xlnx-dri_22.2.5-0ubuntu0.1-22.04.3](https://github.com/ikwzm/mesa-xlnx/tree/mesa-xlnx-dri_22.2.5-0ubuntu0.1-22.04.3)
| 22.04.1        | libgl1-mesa-xlnx-dri_22.0.5-0ubuntu0.1_arm64.deb         | [mesa-xlnx-dri_22.0.5-0ubuntu0.1](https://github.com/ikwzm/mesa-xlnx/tree/mesa-xlnx-dri_22.0.5-0ubuntu0.1)
| 20.04          | libgl1-mesa-xlnx-dri_20.2.6-0ubuntu0~20.04.1_arm64.deb   | [mesa-xlnx-20.2.6-0ubuntu0.20.04.1](https://github.com/ikwzm/mesa-xlnx/tree/mesa-xlnx-20.2.6-0ubuntu0.20.04.1)

Install
--------------------

### For Ubuntu 22.04.3 LTS

#### Download

```console
shell$ git clone -b mesa-xlnx-dri_23.0.4-0ubuntu1-22.04.1 https://github.com/ikwzm/mesa-xlnx
```
#### Install

```console
shell$ sudo apt-get install ./mesa-xlnx/libgl1-mesa-xlnx-dri_23.0.4-0ubuntu1~22.04.1_arm64.deb
```

### For Ubuntu 22.04.2 LTS

#### Download

```console
shell$ git clone -b mesa-xlnx-dri_22.2.5-0ubuntu0.1-22.04.3 https://github.com/ikwzm/mesa-xlnx
```
#### Install

```console
shell$ sudo apt-get install ./mesa-xlnx/libgl1-mesa-xlnx-dri_22.2.5-0ubuntu0.1~22.04.3_arm64.deb
```

### For Ubuntu 22.04.1 LTS

#### Download

```console
shell$ git clone -b mesa-xlnx-dri_22.0.5-0ubuntu0.1 https://github.com/ikwzm/mesa-xlnx
```
#### Install

```console
shell$ sudo apt-get install ./mesa-xlnx/libgl1-mesa-xlnx-dri_22.0.5-0ubuntu0.1_arm64.deb
```

### For Ubuntu 20.04.1

#### Download

```console
shell$ git clone -b mesa-xlnx-20.2.6-0ubuntu0.20.04.1 https://github.com/ikwzm/mesa-xlnx
```

#### Install

```console
shell$ sudo apt-get install ./mesa-xlnx/libgl1-mesa-xlnx-dri_20.2.6-0ubuntu0~20.04.1_arm64.deb
```

### Disable Rendering with Lima(if necessary)

The Lima DRI Driver is currently under development, so the application may not work.
If you want to run an application that doesn't work, set the environment variable LIBGL_ALWAYS_SOFTWARE to 1 when you start gnome-shell to disable rendering with Lima as follows.

```console
shell$ cat <<EOT > /etc/systemd/user/gnome-shell-x11.service.d/override.conf
[Service]
Environment="LIBGL_ALWAYS_SOFTWARE=1"
EOT
```

