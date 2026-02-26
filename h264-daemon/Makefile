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

# Output binary
TARGET = mp4_play

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -o $(TARGET) main.cpp $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)
