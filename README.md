[![icon](/data/pixmaps/idbutton.png?raw=true)](https://www.darktable.org/) darktable [![build status](https://travis-ci.org/darktable-org/darktable.svg?branch=master)](https://travis-ci.org/darktable-org/darktable) [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/470/badge)](https://bestpractices.coreinfrastructure.org/projects/470)
=========

darktable is an open source photography workflow application and raw developer. A virtual lighttable and darkroom for photographers. It manages your digital negatives in a database, lets you view them through a zoomable lighttable and enables you to develop raw images and enhance them.

[https://www.darktable.org/](https://www.darktable.org/ "darktable homepage")

Contributing
------------

* Write a blog about darktable
* Create a tutorial for darktable
* Help expand the [user wiki](https://github.com/darktable-org/darktable/wiki)
* Answer questions on the [user mailing list](https://www.mail-archive.com/darktable-user@lists.darktable.org/)
* Share your ideas on the [developer mailing list](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)
* Test [releases](https://www.darktable.org/install/)
* Review [pull requests](https://github.com/darktable-org/darktable/pulls)
* Start [hacking on darktable](https://www.darktable.org/redmine/projects/darktable/wiki/Contributing_code) and see [developer's guide](https://github.com/darktable-org/darktable/wiki/Developer's-guide)

Building
--------

### Requirements

Supported platforms:
* Linux,
* Windows 8, 10,
* MacOS.

Compatible compilers:
* Clang: 8, 9, 10
* GCC: 8, 9, 10
* Mingw64: 6, 7

Hardware (minimal / **recommended**):
* RAM: 4 GB / **8 GB**
* CPU: Intel Pentium 4 (64 bits, SIMD) / **Intel Core i5 4×2.4 GHz (64 bits, SIMD)** 
* GPU: none / **1024 cores, 4 GB, OpenCL 1.2 compatible**
* free disk space: 250 MB / **1 GB**

### Dependencies

Required dependencies minimal version:
* CMake 3.10
* Gtk 3.22
* Glib 2.40
* Sqlite 3.15 (but 3.24 strongly recommended)

Optional dependencies minimal version:
* OpenMP 4.5
* LLVM 3.9
* OpenCL 1.2
* Lua 5.3
* libavif 0.6.0
* WebP 0.3.0

Optional dependencies with no version requirement:
* Gphoto2
* Lensfun
* OpenEXR
* OpenJPEG
* Colord
* G'Mic


### Get the source

#### Master branch (unstable)

The master branch contains the latest version of the source code and is intended:
* as a working base for developers,
* for beta-testers to chase bugs, 
* for users willing to sacrifice stability for new features without waiting for next release.

The master branch comes with no garanty of stability, might corrupt your database and XMP files, 
might result in loss of data and edits history, and temporarily break compatibility with previous versions and commits.

After backing up your `~/.config/darktable` directory as well as the sidecar .XMP files of the pictures you will open
with the master branch, you may get the source:
```bash
git clone https://github.com/darktable-org/darktable.git
cd darktable
```

#### Latest release : 3.0.2 (stable)

darktable project releases one major version every year, for Christmas, tagged with even numbers, (like 2.2, 2.4, 2.6, 3.0). 
Minor revisions are tagged with a third digit (like 3.0.1, 3.0.2) and mostly provide bug fixes and minor new features.
You may want to compile these stable releases yourself in order to get better performance for your particular computer:

```bash
git clone https://github.com/darktable-org/darktable.git
cd darktable
git fetch --tags
git checkout tags/release-3.0.2
```

### Get submodules

Note that [rawspeed](https://github.com/darktable-org/rawspeed) is tracked via a git submodule, as well as OpenCL and LibXCF modules, so after checking-out the darktable, you need to update/checkout them,

```bash
git submodule init
git submodule update
```

### Compile

#### Easy way

darktable provides a shell script that automaticaly takes care of the building for classic cases in a single command. 


```bash
./build.sh --prefix /opt/darktable --build-type Release --install --sudo
```

This builds the software for your own architecture only, with:

* `-O3` optimization level, 
* SSE/AVX support if detected, 
* OpenMP support (multi-threading and vectorization) if detected,
* OpenCL support (GPU offloading) if detected,
* Lua scripting support if detected.

#### Manual way

You can alternatively use the manual building to pass on custom arguments. 


```bash
mkdir build/
cd build/
cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable/ ..
make
sudo make install
```

### Further reading

There is a comprehensive list of [build instructions for Ubuntu/Debian related Linux distributions](https://github.com/darktable-org/darktable/wiki/Build-instructions-for-Ubuntu-18.04-to-20.04) or for [Fedora and related ones distributions](https://github.com/darktable-org/darktable/wiki/Build-Instructions-for-Fedora). These build instructions could easily be adapted to all others distributions


**Tip:** Check that you have the latest [gphoto2 library](http://www.gphoto.org/ "gphoto2 homepage") installed in order to support the newest cameras.

Wiki
----

* [GitHub wiki](https://github.com/darktable-org/darktable/wiki "github wiki")
* [User wiki](https://www.darktable.org/redmine/projects/users/wiki "darktable user wiki")
* [Developer wiki](https://github.com/darktable-org/darktable/wiki/Developer's-guide "darktable developer wiki")


Mailing lists
-------------

* Users [[subscribe](mailto:darktable-user+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-user@lists.darktable.org/)]
* Developer [[subscribe](mailto:darktable-dev+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)]
* CI (read-only, high traffic!) [[subscribe](mailto:darktable-ci+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-ci@lists.darktable.org/)]
