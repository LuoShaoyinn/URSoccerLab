ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE} AS media-build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl xz-utils pkg-config nasm cmake meson ninja-build libva-dev libdrm-dev \
    libvulkan-dev glslang-dev spirv-tools \
    && rm -rf /var/lib/apt/lists/*
# Native encoders connect dynamically to host drivers; no GPU driver is packaged.
RUN curl -fL --retry 3 https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n13.0.19.0.tar.gz -o /tmp/nvcodec.tar.gz \
    && tar xf /tmp/nvcodec.tar.gz -C /tmp \
    && make -C /tmp/nv-codec-headers-n13.0.19.0 install \
    && curl -fL --retry 3 https://github.com/videolan/dav1d/archive/refs/tags/1.5.3.tar.gz -o /tmp/dav1d.tar.gz \
    && tar xf /tmp/dav1d.tar.gz -C /tmp \
    && meson setup /tmp/dav1d-build /tmp/dav1d-1.5.3 --prefix=/opt/urs-ffmpeg --libdir=lib -Denable_tools=false -Denable_tests=false \
    && ninja -C /tmp/dav1d-build install
RUN curl -fL --retry 3 https://github.com/intel/libvpl/archive/refs/tags/v2.16.0.tar.gz -o /tmp/vpl.tar.gz \
    && tar xf /tmp/vpl.tar.gz -C /tmp \
    && cmake -S /tmp/libvpl-2.16.0 -B /tmp/vpl-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/urs-ffmpeg -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DINSTALL_EXAMPLES=OFF \
    && cmake --build /tmp/vpl-build -j8 && cmake --install /tmp/vpl-build
# Portable VA dispatcher supports modern Mesa/iHD drivers without bundling them.
RUN curl -fL --retry 3 https://github.com/intel/libva/archive/refs/tags/2.24.1.tar.gz -o /tmp/libva.tar.gz \
    && tar xf /tmp/libva.tar.gz -C /tmp \
    && meson setup /tmp/libva-build /tmp/libva-2.24.1 --prefix=/opt/urs-ffmpeg --libdir=lib \
       -Dwith_x11=no -Dwith_glx=no -Dwith_wayland=no -Dwith_win32=no \
       -Ddriverdir=/usr/lib/x86_64-linux-gnu/dri:/usr/lib64/dri:/usr/lib/dri \
    && ninja -C /tmp/libva-build install \
    && mkdir -p /opt/urs-ffmpeg/share/licenses \
    && cp /tmp/libva-2.24.1/COPYING /opt/urs-ffmpeg/share/licenses/libva-COPYING
# Include FFmpeg's Vulkan shader support via Ubuntu 22.04 static glslang archives.
# Vulkan video encode definitions and a minimal LGPL shared FFmpeg build.
RUN curl -fL --retry 3 https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.321.tar.gz -o /tmp/vulkan.tar.gz \
    && tar xf /tmp/vulkan.tar.gz -C /tmp \
    && cp -r /tmp/Vulkan-Headers-1.4.321/include/* /usr/include/ \
    && sed -i 's/^Version:.*/Version: 1.4.321/' /usr/lib/x86_64-linux-gnu/pkgconfig/vulkan.pc \
    && curl -fL --retry 3 https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz -o /tmp/ffmpeg.tar.xz \
    && tar xf /tmp/ffmpeg.tar.xz -C /tmp
RUN cd /tmp/ffmpeg-8.1.3 \
    && PKG_CONFIG_PATH=/opt/urs-ffmpeg/lib/pkgconfig ./configure --prefix=/opt/urs-ffmpeg --enable-shared --disable-static \
       --disable-autodetect --disable-everything --disable-programs --disable-doc \
       --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
       --enable-vulkan --enable-libglslang --enable-vaapi --enable-libvpl --enable-nvenc --enable-ffnvcodec \
       --enable-libdav1d --enable-decoder=libdav1d,h264,hevc \
       --enable-encoder=av1_vulkan,h264_vulkan,hevc_vulkan,av1_nvenc,h264_nvenc,hevc_nvenc,av1_vaapi,h264_vaapi,hevc_vaapi,av1_qsv,h264_qsv,hevc_qsv --fatal-warnings \
    && make -j8 && make install \
    && mkdir -p /opt/urs-ffmpeg/share/licenses \
    && cp /tmp/dav1d-1.5.3/COPYING /opt/urs-ffmpeg/share/licenses/dav1d-COPYING \
    && sed -n '1,26p' /tmp/nv-codec-headers-n13.0.19.0/include/ffnvcodec/nvEncodeAPI.h > /opt/urs-ffmpeg/share/licenses/nv-codec-headers-LICENSE \
    && cp /tmp/libvpl-2.16.0/LICENSE /opt/urs-ffmpeg/share/licenses/libvpl-LICENSE \
    && cp COPYING.LGPLv2.1 /opt/urs-ffmpeg/share/licenses/ \
    && cp /usr/share/doc/glslang-dev/copyright /opt/urs-ffmpeg/share/licenses/glslang-copyright \
    && cp /usr/share/doc/spirv-tools/copyright /opt/urs-ffmpeg/share/licenses/spirv-tools-copyright \
    && rm -rf /tmp/ffmpeg* /tmp/Vulkan-Headers* /tmp/vulkan.tar.gz

FROM ${BASE_IMAGE} AS packager
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 git ca-certificates curl jq file xdg-user-dirs \
    libvulkan1 libstdc++6 libgcc-s1 libva2 libva-drm2 \
    libnss3 libnspr4 libxkbcommon0 libgbm1 libdrm2 libcups2 libxshmfence1 \
    libatspi2.0-0 libatk1.0-0 libatk-bridge2.0-0 libpangocairo-1.0-0 libcairo2 \
    libpango-1.0-0 libgl1 libx11-6 libxcursor1 libxrandr2 libxi6 libxinerama1 \
    libxxf86vm1 libxss1 libxtst6 libxrender1 libxext6 libxfixes3 libxdamage1 \
    libxcomposite1 libpulse0 libasound2 libfontconfig1 libnuma1 libdbus-1-3 \
    libcurl4 libharfbuzz0b libssl3 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=media-build /opt/urs-ffmpeg /opt/urs-ffmpeg
RUN curl -fL --retry 3 https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage -o /tmp/appimagetool \
    && chmod +x /tmp/appimagetool && cd /tmp && ./appimagetool --appimage-extract \
    && mv squashfs-root /opt/appimagetool && ln -s /opt/appimagetool/AppRun /usr/local/bin/appimagetool \
    && rm /tmp/appimagetool
# CoACD requires the OpenMP runtime even when no compiler is installed.
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 && rm -rf /var/lib/apt/lists/*
RUN git config --system core.quotepath false && git config --system --add safe.directory /workspace
ENV URS_FFMPEG_ROOT=/opt/urs-ffmpeg LD_LIBRARY_PATH=/opt/urs-ffmpeg/lib DOTNET_ROLL_FORWARD=Major
WORKDIR /workspace
