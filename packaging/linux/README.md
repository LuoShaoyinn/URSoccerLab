# Linux viewer AppImage

Build the Debian 12 image with Docker or the Podman Docker shim, then mount the
source, a fresh output directory, and installed `linuxdeploy` and `appimagetool`
executables into the container. CMake fetches the pinned cJSON archive.

```sh
docker build -t urs-viewer-bookworm packaging/linux
mkdir -p artifacts/viewer-appimage
docker run --rm -v "$PWD:/work:ro" \
  -v "$PWD/artifacts/viewer-appimage:/tmp/out:rw" \
  -v /usr/bin/linuxdeploy:/usr/bin/linuxdeploy:ro \
  -v /usr/bin/appimagetool:/usr/bin/appimagetool:ro \
  urs-viewer-bookworm /work/packaging/linux/build_in_container.sh
```

The result bundles distribution-provided wxWidgets and FFmpeg dependencies.
Debian 12 is the build environment, not a guarantee of Ubuntu compatibility.
Verify the output on target distributions and retain dependency license notices.
