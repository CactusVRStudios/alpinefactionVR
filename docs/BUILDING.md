Building Alpine Faction
=======================

Windows
-------

On Windows use CMake GUI to generate project files for your favorite IDE.
Supports only Win32 platform. If you are using Visual Studio 2019+ on x86_64 Windows host
CMake by default selects Win64 target platform. Please change it manually to Win32.

### OpenXR support

OpenXR is disabled by default. A normal build has no OpenXR dependency and preserves Alpine Faction's flat renderer:

```
-DAF_ENABLE_OPENXR=OFF
```

To build the AlpineFaction VR path, configure a Win32 build with:

```
-DAF_ENABLE_OPENXR=ON
```

The enabled configuration downloads the official Khronos OpenXR SDK `release-1.1.60` source at its pinned commit and
builds the 32-bit loader statically. `AlpineFaction.dll` therefore does not require a separately packaged
`openxr_loader.dll`. Runtime initialization remains opt-in through `-vr`, requires D3D11, and supports the
singleplayer campaign only.

Linux
-----

Building on Ubuntu 24.04 based distribution is recommended.
Building on other Linux distributions should be possible but may require different/additional steps that are not
covered by this instruction.

Make sure you have all the needed tools:

* mingw-w64
* cmake
* make

To install them run:

```
sudo apt-get install mingw-w64 cmake make
```

You can use Ninja instead of Make to speed up build.

Checkout source code repository:

```
git clone https://github.com/GooberRF/alpinefaction.git
```

Create `build` directory and generate makefiles (you can use a different directory):

```
cd alpinefaction
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-ubuntu.cmake -DCMAKE_BUILD_TYPE=Release
```

Start build:

```
make -j$(nproc)
```

After build is finished you will find binaries in `build/Release/bin` subdirectory.

To update your custom builds run:

```
cd alpinefaction/build
git pull
make -j$(nproc)
```
