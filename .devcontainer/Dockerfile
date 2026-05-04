FROM ubuntu:latest

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
        clang-format-14 \
        curl \
        python3-pip \
        pre-commit \
        vim \
        google-perftools \
        libgoogle-perftools-dev

WORKDIR /app
COPY .github/install_dependencies /app/
RUN /app/install_dependencies

RUN curl -LsSf https://astral.sh/uv/install.sh | sh && \
    ln -s /root/.local/bin/uv /usr/local/bin/uv && \
    ln -s /root/.local/bin/uvx /usr/local/bin/uvx

RUN apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*
