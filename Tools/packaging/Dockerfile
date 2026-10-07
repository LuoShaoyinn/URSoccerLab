ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE} AS media-build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl xz-utils pkg-config nasm \
    libvulkan-dev glslang-dev spirv-tools \
    && rm -rf /var/lib/apt/lists/*
# Include FFmpeg's Vulkan shader support via Ubuntu 22.04 static glslang archives.
# Vulkan video encode definitions and a minimal LGPL shared FFmpeg build.
RUN curl -fL --retry 3 https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.321.tar.gz -o /tmp/vulkan.tar.gz \
    && tar xf /tmp/vulkan.tar.gz -C /tmp \
    && cp -r /tmp/Vulkan-Headers-1.4.321/include/* /usr/include/ \
    && sed -i 's/^Version:.*/Version: 1.4.321/' /usr/lib/x86_64-linux-gnu/pkgconfig/vulkan.pc \
    && curl -fL --retry 3 https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz -o /tmp/ffmpeg.tar.xz \
    && tar xf /tmp/ffmpeg.tar.xz -C /tmp \
    && cd /tmp/ffmpeg-8.0.1 \
    && ./configure --prefix=/opt/urs-ffmpeg --enable-shared --disable-static \
       --disable-autodetect --disable-everything --disable-programs --disable-doc \
       --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
       --enable-vulkan --enable-libglslang --enable-encoder=av1_vulkan --fatal-warnings \
    && make -j8 && make install \
    && mkdir -p /opt/urs-ffmpeg/share/licenses \
    && cp COPYING.LGPLv2.1 /opt/urs-ffmpeg/share/licenses/ \
    && cp /usr/share/doc/glslang-dev/copyright /opt/urs-ffmpeg/share/licenses/glslang-copyright \
    && cp /usr/share/doc/spirv-tools/copyright /opt/urs-ffmpeg/share/licenses/spirv-tools-copyright \
    && rm -rf /tmp/ffmpeg* /tmp/Vulkan-Headers* /tmp/vulkan.tar.gz

FROM ${BASE_IMAGE} AS packager
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 git ca-certificates curl jq file xdg-user-dirs \
    libvulkan1 libstdc++6 libgcc-s1 \
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
