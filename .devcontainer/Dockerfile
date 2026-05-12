FROM harbor.peinser.com/dockerhub/espressif/idf:release-v5.5

ENV DEBIAN_FRONTEND=noninteractive
ENV EDITOR=vim
ENV ESPPORT=/dev/ttyESP32
ENV IDF_CCACHE_ENABLE=1
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8
ENV PATH=/root/.opencode/bin:/workspace/bin:$PATH

WORKDIR /workspace

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash-completion \
    ca-certificates \
    ccache \
    clang-format \
    clangd \
    cppcheck \
    curl \
    git \
    jq \
    less \
    make \
    minicom \
    openssh-client \
    socat \
    usbutils \
    vim \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://opencode.ai/install | bash

RUN printf '%s\n' \
    '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    '. /opt/esp/idf/export.sh >/dev/null 2>&1' \
    'exec python /opt/esp/idf/tools/idf.py "$@"' \
    > /usr/local/bin/idf.py \
    && chmod +x /usr/local/bin/idf.py \
    && printf '%s\n' '. /opt/esp/idf/export.sh >/dev/null 2>&1' >> /root/.bashrc
