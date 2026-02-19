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
    python3 \
    ffmpeg \
    pre-commit \
    vim

WORKDIR /app
COPY .github/install_dependencies /app/
RUN /app/install_dependencies

RUN apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*
