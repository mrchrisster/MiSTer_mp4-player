CXX      = arm-linux-gnueabihf-g++
CFLAGS   = -Wall -O3 -std=c++11 -D_FILE_OFFSET_BITS=64
STRIP    = arm-linux-gnueabihf-strip

# Path to static FFmpeg ARM build.
# Default: /opt/ffmpeg-arm  — override with: make FFMPEG_PREFIX=/your/path
FFMPEG_PREFIX ?= /opt/ffmpeg-arm

# Static-link FFmpeg; dynamic-link libc/libm/pthreads (avoids glibc NSS issues)
INCLUDES = -I$(FFMPEG_PREFIX)/include
LDFLAGS  = -L$(FFMPEG_PREFIX)/lib -L/usr/lib/arm-linux-gnueabihf
LIBS     = -Wl,-Bstatic \
               -lavformat -lavcodec -lavutil -lswresample \
           -Wl,-Bdynamic \
               -lm -lpthread -lz -ldl -lasound

# Output binaries
TARGET = mp4_play
TESTS  = test_color test_endian test_rgb_direct test_stripe test_yuv_fpga test_yuv_dump

all: $(TARGET)

tests: $(TESTS)

$(TARGET): main.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -o $(TARGET) main.cpp $(LDFLAGS) $(LIBS)
	$(STRIP) $(TARGET)

test_color: test_color.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_endian: test_endian.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_rgb_direct: test_rgb_direct.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_stripe: test_stripe.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_yuv_fpga: test_yuv_fpga.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_yuv_dump: test_yuv_dump.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

clean:
	rm -f $(TARGET) $(TESTS)
