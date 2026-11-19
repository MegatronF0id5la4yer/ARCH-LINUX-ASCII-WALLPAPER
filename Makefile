CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-deprecated-declarations -Isrc
PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gtk-layer-shell-0 pangocairo gdk-pixbuf-2.0)
PKG_LIBS := $(shell pkg-config --libs gtk+-3.0 gtk-layer-shell-0 pangocairo gdk-pixbuf-2.0)

TARGET := arch-linux-ascii-wallpaper

.PHONY: all test clean

all: $(TARGET)

$(TARGET): src/ascii_wallpaper.cpp src/audio_reactive.hpp
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) src/ascii_wallpaper.cpp -o $(TARGET) $(PKG_LIBS)

test: $(TARGET)
	./$(TARGET) --self-test

clean:
	rm -f $(TARGET)

