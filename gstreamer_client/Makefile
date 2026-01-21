CXX := g++
PKGS := gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-3.0 json-glib-1.0
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 -DGST_USE_UNSTABLE_API $(shell pkg-config --cflags $(PKGS))
LDFLAGS := $(shell pkg-config --libs $(PKGS))

# 기존 클라이언트 (낮은 수준 API 직접 사용)
TARGET := client
SRCS := client.cpp

# 새로운 간단한 클라이언트 (고수준 API 사용)
SIMPLE_TARGET := simple_client
SIMPLE_SRCS := simple_client.cpp webrtc_client.cpp

# 기본 타겟: 두 클라이언트 모두 빌드
all: $(TARGET) $(SIMPLE_TARGET)

# 기존 클라이언트
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 간단한 클라이언트
$(SIMPLE_TARGET): $(SIMPLE_SRCS) webrtc_client.h
	$(CXX) $(CXXFLAGS) -o $@ $(SIMPLE_SRCS) $(LDFLAGS)

# [수정] run 타겟: clean -> all -> 실행 순서로 진행
run: clean all
	./$(TARGET)

# 간단한 클라이언트 실행
run-simple: clean $(SIMPLE_TARGET)
	./$(SIMPLE_TARGET)

clean:
	rm -f $(TARGET) $(SIMPLE_TARGET)

.PHONY: all clean run run-simple