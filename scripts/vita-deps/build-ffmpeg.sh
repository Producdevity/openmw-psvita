#!/bin/bash
# Build/install FFmpeg for OpenMW Vita port
# Minimal config: only decoders needed for Morrowind audio/video
set -e
SCRIPT_SIGNATURE="$(cksum < "${BASH_SOURCE[0]}")"

# Error cleanup handler
trap 'rm -f "${WORKDIR}/ffmpeg_done"' ERR

VITASDK="${VITASDK:-/usr/local/vitasdk}"
SYSROOT="${VITASDK}/arm-vita-eabi"
JOBS="${OPENMW_VITA_JOBS:-4}"

echo "=== Installing FFmpeg for Vita ==="

WORKDIR="${1:-$(pwd)/vita-deps-build/ffmpeg}"
FFMPEG_VERSION="6.1"

echo "Building FFmpeg ${FFMPEG_VERSION} from source..."
echo "Work dir: ${WORKDIR}"

mkdir -p "${WORKDIR}"
WORKDIR="$(cd "${WORKDIR}" && pwd)"
cd "${WORKDIR}"

if [ ! -f "ffmpeg-${FFMPEG_VERSION}.tar.bz2" ]; then
    wget -q "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.bz2"
fi
if [ ! -f "ffmpeg-${FFMPEG_VERSION}/configure" ]; then
    tar xf "ffmpeg-${FFMPEG_VERSION}.tar.bz2"
fi
BUILD_SIGNATURE="${FFMPEG_VERSION}|${SYSROOT}|${SCRIPT_SIGNATURE}|$(cksum < "${VITASDK}/bin/arm-vita-eabi-gcc")"
cd "ffmpeg-${FFMPEG_VERSION}"
if [ "$(cat ../ffmpeg_done 2>/dev/null)" != "${BUILD_SIGNATURE}" ] && [ -f ffbuild/config.mak ]; then
    make clean
fi

./configure \
    --prefix="${SYSROOT}" \
    --cross-prefix="${VITASDK}/bin/arm-vita-eabi-" \
    --arch=armv7-a \
    --cpu=cortex-a9 \
    --target-os=none \
    --enable-cross-compile \
    --enable-static \
    --disable-shared \
    --disable-programs \
    --disable-doc \
    --disable-network \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-everything \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-decoder=mp3 \
    --enable-decoder=aac \
    --enable-decoder=pcm_s16le \
    --enable-decoder=pcm_u8 \
    --enable-decoder=vorbis \
    --enable-decoder=bink \
    --enable-decoder=binkaudio_rdft \
    --enable-decoder=binkaudio_dct \
    --enable-decoder=wav \
    --enable-demuxer=mp3 \
    --enable-demuxer=aac \
    --enable-demuxer=ogg \
    --enable-demuxer=wav \
    --enable-demuxer=pcm_s16le \
    --enable-demuxer=bink \
    --enable-parser=mpegaudio \
    --enable-parser=aac \
    --enable-parser=vorbis \
    --enable-protocol=file \
    --enable-pthreads \
    --extra-cflags="-Os -mcpu=cortex-a9 -mfpu=neon -ftree-vectorize -fomit-frame-pointer -Wno-error=incompatible-pointer-types"

make -j"${JOBS}"
make install

# Validate installation
if [ ! -f "${SYSROOT}/lib/libavcodec.a" ] || [ ! -f "${SYSROOT}/lib/libavformat.a" ] || [ ! -f "${SYSROOT}/lib/libavutil.a" ]; then
    echo "ERROR: FFmpeg installation failed - libraries not found in ${SYSROOT}/lib"
    exit 1
fi

cd ..
printf '%s\n' "${BUILD_SIGNATURE}" > ffmpeg_done

echo "=== FFmpeg installation complete ==="
