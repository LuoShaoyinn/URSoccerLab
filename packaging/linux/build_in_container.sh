#!/bin/bash
set -euo pipefail
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=x86_64
cmake -S /work -B /tmp/out/build -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF
cmake --build /tmp/out/build -j 4
mkdir -p /tmp/out/AppDir/usr/bin /tmp/out/AppDir/usr/share/licenses/urs-viewer
cp /tmp/out/build/urs-viewer /tmp/out/AppDir/usr/bin/
strip /tmp/out/AppDir/usr/bin/urs-viewer
cp /work/LICENSE /work/NOTICE /work/THIRD_PARTY_NOTICES.md /tmp/out/AppDir/usr/share/licenses/urs-viewer/
cp /usr/share/doc/libwxgtk3.2-1/copyright /tmp/out/AppDir/usr/share/licenses/urs-viewer/wxwidgets-copyright
cp /usr/share/doc/libavcodec59/copyright /tmp/out/AppDir/usr/share/licenses/urs-viewer/ffmpeg-copyright
/usr/bin/linuxdeploy --appdir /tmp/out/AppDir --desktop-file /work/packaging/linux/viewer.desktop --icon-file /work/packaging/linux/urs-viewer.png --executable /tmp/out/AppDir/usr/bin/urs-viewer
cd /tmp/out
/usr/bin/appimagetool --comp zstd AppDir /tmp/out/URSoccerLab-Viewer-0.1.2-linux-x86_64.AppImage
