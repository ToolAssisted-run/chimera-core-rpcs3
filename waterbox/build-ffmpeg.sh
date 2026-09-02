#!/bin/sh
# FFmpeg from source for one flavor, identically configured for both so the
# decoded media is flavor-identical: no assembly (asm paths are not bit-exact
# with the C paths for every codec), no programs, no network, only the
# decoders the PS3's HLE modules ask for. Artifacts install into
# build/ffmpeg-<flavor> (include/ + lib/).
#   sh build-ffmpeg.sh native|guest
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
flavor="${1:?native|guest}"
src="$root/extern/ffmpeg"
out="$root/build/ffmpeg-$flavor"
bld="$root/build/ffmpeg-$flavor-obj"
mkdir -p "$bld"
SR="${MINIBOX_SYSROOT:-$HOME/chimera/extern/tools/chimera-common-minibox/build/meson-cpp/guest-sysroot}"
if [ "$flavor" = guest ]; then
	CC="gcc -specs=$SR/lib/musl-gcc.specs"
	CFLAGS="-fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -O2"
	cross="--enable-cross-compile --target-os=linux --arch=x86_64 --pkg-config=false"
else
	CC="gcc"
	CFLAGS="-O2"
	cross=""
fi
cd "$bld"
"$src/configure" --prefix="$out" --cc="$CC" --extra-cflags="$CFLAGS" $cross \
	--disable-asm --disable-x86asm --disable-inline-asm --disable-programs --disable-doc \
	--disable-network --disable-shared --enable-static --disable-autodetect --disable-debug \
	--disable-everything --disable-avdevice --disable-avfilter \
	--enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
	--enable-decoder=h264,mpeg2video,mpeg4,mpeg1video,aac,aac_latm,mp3,mp2,ac3,atrac1,atrac3,atrac3p,atrac9,pcm_s16le,pcm_s16be,pcm_f32le,adpcm_ima_wav \
	--enable-parser=h264,mpegvideo,mpeg4video,aac,aac_latm,mpegaudio,ac3 \
	--enable-demuxer=mov,mp3,aac,wav,ac3,mpegps,mpegts,m4v,h264,oma,pcm_s16le,pcm_s16be \
	--enable-protocol=file \
	--enable-encoder=pcm_s16le,pcm_s16be --enable-muxer=wav,mp4,adts \
	--disable-pthreads --disable-w32threads --disable-os2threads \
	--disable-vaapi --disable-vdpau --disable-cuda --disable-cuvid --disable-nvdec --disable-nvenc --disable-libdrm --disable-vulkan --disable-opencl --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2 --disable-xlib \
	--disable-runtime-cpudetect > "$bld/configure.log" 2>&1 || { tail -20 "$bld/configure.log"; exit 1; }
make -j"$(nproc)" > "$bld/make.log" 2>&1 || { tail -20 "$bld/make.log"; exit 1; }
make install > "$bld/install.log" 2>&1
echo "ffmpeg $flavor: $(ls "$out/lib" | tr '\n' ' ')"
