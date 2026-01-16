
# 1. 베이스 이미지: Python 3.11이 기본인 Ubuntu 23.10 또는
# 안정적인 설치를 위해 python:3.11-slim 이미지를 베이스로 사용하는 것이 가장 좋습니다.
FROM python:3.11-slim

# 2. 환경 변수 설정
ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONUNBUFFERED=1

# 3. GStreamer 및 시스템 필수 라이브러리 설치 (slim 이미지라 최소한으로 설치)
RUN apt-get update && apt-get install -y --no-install-recommends \
    vim nano curl wget net-tools iputils-ping git \
    build-essential \
    pkg-config \
    libgl1 \
    libglx0 \
    libglib2.0-0 \
    libssl-dev \
    # GStreamer 풀 세트
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    && rm -rf /var/lib/apt/lists/*

# 4. 작업 디렉토리 설정
WORKDIR /root

# 5. FastRTC 및 핵심 라이브러리 설치
# python:3.11-slim 베이스이므로 --break-system-packages 없이 깔끔하게 설치됩니다.
RUN pip install --no-cache-dir --upgrade pip && \
    pip install --no-cache-dir \
    "fastrtc[vad]" \
    opencv-python \
    fastapi \
    uvicorn \
    python-multipart \
    aiortc

# 6. 컨테이너 시작 시 바로 Bash 쉘 실행
CMD ["/bin/bash"]
