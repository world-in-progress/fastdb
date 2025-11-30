FROM ubuntu:24.04

ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY

ENV http_proxy=${HTTP_PROXY} \
    https_proxy=${HTTPS_PROXY} \
    no_proxy=${NO_PROXY} \
    HTTP_PROXY=${HTTP_PROXY} \
    HTTPS_PROXY=${HTTPS_PROXY} \
    NO_PROXY=${NO_PROXY}

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    swig \
    curl \
    ca-certificates \
    git \
    && apt-get clean -y && rm -rf /var/lib/apt/lists/*

RUN git config --global http.proxy ${HTTP_PROXY} && \
    git config --global https.proxy ${HTTPS_PROXY}

# Install uv (Universal Version Manager) and add to PATH
RUN curl -LsSf https://astral.sh/uv/install.sh | sh
ENV PATH="/root/.local/bin:$PATH"

# Create a uv python environment with Python 3.13
RUN uv python install 3.13 && \
    uv python pin 3.13
