# pprof viewer for -DPROFILING=TRUE; Ubuntu 26.04 dropped google-perftools, which shipped it.
# Pinned by commit because pprof has no tagged releases.
FROM ubuntu:26.04 AS pprof
RUN apt-get update && \
    apt-get install -y --no-install-recommends golang-go ca-certificates git
RUN GOBIN=/out go install github.com/google/pprof@v0.0.0-20261026192309-dbd0ed30aa88

FROM ubuntu:26.04

RUN sed -i 's/^# \(.*export LS_OPTIONS.*$\)/\1/g' ~/.bashrc && \
    sed -i 's/^# \(.*alias ll.*$\)/\1/g' ~/.bashrc

RUN ln -fs /usr/share/zoneinfo/America/Los_Angeles /etc/localtime
RUN DEBIAN_FRONTEND=noninteractive \
    apt-get update && \
    apt-get install -y \
        tzdata\
        git \
        sudo \
        gdb \
        curl \
        python3-pip \
        pre-commit \
        vim

WORKDIR /app
COPY .github/install_dependencies /app/
RUN /app/install_dependencies

RUN curl -LsSf https://astral.sh/uv/install.sh | sh && \
    ln -s /root/.local/bin/uv /usr/local/bin/uv && \
    ln -s /root/.local/bin/uvx /usr/local/bin/uvx

COPY --from=pprof /out/pprof /usr/local/bin/pprof

RUN apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*
