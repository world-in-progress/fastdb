FROM docker.io/library/golang:1.25-bookworm

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

ARG GIT_USER_NAME
ARG GIT_USER_EMAIL

RUN git config --global http.proxy ${HTTP_PROXY} && \
    git config --global https.proxy ${HTTPS_PROXY} && \
    git config --global user.name "${GIT_USER_NAME}" && \
    git config --global user.email "${GIT_USER_EMAIL}"
