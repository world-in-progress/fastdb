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

# Install uv (Universal Version Manager) and add to PATH
RUN curl -LsSf https://astral.sh/uv/install.sh | sh
ENV PATH="/root/.local/bin:$PATH"


# Install Miniforge (ARM64 compatible miniconda alternative)
# ENV CONDA_DIR=/opt/conda
# RUN wget "https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-$(uname -m).sh" -O /tmp/miniforge.sh \
#     && bash /tmp/miniforge.sh -b -p ${CONDA_DIR} \
#     && rm /tmp/miniforge.sh \
#     && ${CONDA_DIR}/bin/conda clean -afy

# ENV PATH="${CONDA_DIR}/bin:${PATH}"

# Create a uv python environment with Python 3.13
RUN uv python install 3.13 && \
    uv python pin 3.13

# Create a conda environment named fastdb4py with Python 3.13
# Install GDAL and Numpy in the environment
# RUN conda create -n fastdb4py python=3.13 -y && \
#     conda run -n fastdb4py conda install -c conda-forge gdal numpy -y