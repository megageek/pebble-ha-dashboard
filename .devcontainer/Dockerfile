FROM python:3-slim

# Define metadata
LABEL maintainer="pebble-ha-dashboard"

# Set arguments for flexibility
ARG PEBBLE_SDK_VERSION=4.17
ARG NODE_VERSION=20
ARG NVM_VERSION=0.35.0

# Set environment variables
ENV PEBBLE_SDK_VERSION=${PEBBLE_SDK_VERSION}
ENV NVM_DIR=/home/pebble/.nvm
ENV PATH="/home/pebble/.local/bin:$PATH"

# Update system and install required dependencies.
# libsdl2-2.0-0/libpixman-1-0/libfdt1/libglib2.0-dev/libfreetype6-dev back the
# QEMU emulator pebble-tool bundles; firefox-esr is handy for opening the Clay
# config webview's data: URI when testing outside a real phone.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        curl \
        libfreetype6-dev \
        bash-completion \
        libsdl2-2.0-0 \
        libfdt1 \
        libpixman-1-0 \
        libglib2.0-dev \
        vim \
        zsh \
        git \
        wget \
        firefox-esr && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# Create a non-root user and disable analytics
RUN adduser --disabled-password --gecos "" --ingroup users pebble && \
    echo "pebble ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers && \
    mkdir -p /home/pebble/.pebble-sdk/ && \
    chown -R pebble:users /home/pebble/.pebble-sdk && \
    touch /home/pebble/.pebble-sdk/NO_TRACKING

# Install Oh My Zsh manually (as the pebble user)
USER pebble
RUN git clone --depth=1 https://github.com/ohmyzsh/ohmyzsh.git ~/.oh-my-zsh && \
    cp ~/.oh-my-zsh/templates/zshrc.zsh-template ~/.zshrc && \
    echo "export PATH=${PATH}" >> ~/.zshrc && \
    echo "export NVM_DIR=${NVM_DIR}" >> ~/.zshrc && \
    echo "[ -s \"\$NVM_DIR/nvm.sh\" ] && \. \"\$NVM_DIR/nvm.sh\"" >> ~/.zshrc && \
    echo "[ -s \"\$NVM_DIR/bash_completion\" ] && \. \"\$NVM_DIR/bash_completion\"" >> ~/.zshrc

# Set default shell to Zsh
USER root
RUN usermod --shell $(which zsh) pebble

# Switch to non-root user for further installations
USER pebble

# Install NVM and Node.js. nvm installs into a full-patch-version directory
# (e.g. v20.19.5) even when only a major version like "20" is requested, so
# symlink the resolved binaries into ~/.local/bin (already on PATH) rather
# than hardcoding a version-specific path that won't match.
RUN mkdir -p $NVM_DIR /home/pebble/.local/bin && \
    curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v${NVM_VERSION}/install.sh | bash && \
    . $NVM_DIR/nvm.sh && \
    nvm install $NODE_VERSION && \
    nvm alias default $NODE_VERSION && \
    NODE_BIN_DIR="$NVM_DIR/versions/node/$(nvm version default)/bin" && \
    ln -s "$NODE_BIN_DIR/node" /home/pebble/.local/bin/node && \
    ln -s "$NODE_BIN_DIR/npm" /home/pebble/.local/bin/npm && \
    ln -s "$NODE_BIN_DIR/npx" /home/pebble/.local/bin/npx

# Install pebble-tool (Python 3) and the pinned Pebble SDK — mirrors
# .github/workflows/test.yml's install steps, which are the known-good recipe
# for this repo's toolchain.
RUN python3 -m pip install --user pebble-tool && \
    ~/.local/bin/pebble sdk install ${PEBBLE_SDK_VERSION} && \
    ~/.local/bin/pebble sdk activate ${PEBBLE_SDK_VERSION}

# Define mountable volume for projects
VOLUME /workspace/

# Set default working directory
WORKDIR /workspace/

# Default command
CMD ["zsh"]
