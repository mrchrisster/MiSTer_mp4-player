CXX      = arm-linux-gnueabihf-g++
CFLAGS   = -Wall -O3 -std=c++11 -D_FILE_OFFSET_BITS=64

# Path to static FFmpeg ARM build.
# Default: /opt/ffmpeg-arm  — override with: make FFMPEG_PREFIX=/your/path
FFMPEG_PREFIX ?= /opt/ffmpeg-arm

# Static-link FFmpeg; dynamic-link libc/libm/pthreads (avoids glibc NSS issues)
INCLUDES = -I$(FFMPEG_PREFIX)/include
LDFLAGS  = -L$(FFMPEG_PREFIX)/lib
LIBS     = -Wl,-Bstatic \
               -lavformat -lavcodec -lavutil -lswresample \
           -Wl,-Bdynamic \
               -lm -lpthread -lz -ldl

# Output binaries
TARGET = mp4_play
TESTS  = test_color test_endian test_rgb_direct

all: $(TARGET)

tests: $(TESTS)

$(TARGET): main.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -o $(TARGET) main.cpp $(LDFLAGS) $(LIBS)

test_color: test_color.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_endian: test_endian.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

test_rgb_direct: test_rgb_direct.cpp
	$(CXX) $(CFLAGS) -o $@ $@.cpp

clean:
	rm -f $(TARGET) $(TESTS)
