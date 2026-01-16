CXX := g++
PKGS := gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 json-glib-1.0
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 -DGST_USE_UNSTABLE_API $(shell pkg-config --cflags $(PKGS))
LDFLAGS := $(shell pkg-config --libs $(PKGS))

TARGET := client
SRCS := client.cpp

# 기본 타겟: 빌드만 수행
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# [수정] run 타겟: clean -> all -> 실행 순서로 진행
run: clean all
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean run