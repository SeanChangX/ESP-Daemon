######################################################################
# - Base stage
#   - This stage serves as the base image for the following stages.
######################################################################

ARG ROS_DISTRO=humble
FROM ubuntu:22.04 AS base

LABEL org.opencontainers.image.title="ESP Daemon"
LABEL org.opencontainers.image.authors="scx@gapp.nthu.edu.tw"
LABEL org.opencontainers.image.licenses="MIT"

# Set timezone to avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Taipei

# Set up ROS repository
RUN apt-get update && apt-get install -y \
    curl \
    gnupg2 \
    lsb-release \
    ca-certificates \
    && curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o /usr/share/keyrings/ros-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu $(lsb_release -cs) main" | tee /etc/apt/sources.list.d/ros2.list > /dev/null

# Install ROS 2 Humble
RUN apt-get update && apt-get install -y \
    ros-humble-desktop \
    python3-argcomplete \
    python3-colcon-common-extensions \
    python3-rosdep \
    && rm -rf /var/lib/apt/lists/*

ARG USERNAME=ros
# Use the same UID and GID as the host user
ARG USER_UID=1000
ARG USER_GID=1000

######################################################################
# - User setup stage
#   - Create a non-root user with default bash shell.
######################################################################

FROM base AS user-setup

RUN groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME \
    && apt-get update \
    && apt-get install -y sudo \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME \
    && rm -rf /var/lib/apt/lists/*

######################################################################
# - Tools Installation stage
#   - Install common tools for development.
######################################################################

FROM user-setup AS tools

RUN apt-get update && apt-get install -y \
    tree \
    git \
    vim \
    wget \
    unzip \
    python3-pip \
    python3-vcstool \
    bash-completion \
    ros-humble-rmw-cyclonedds-cpp \
    ros-humble-foxglove-bridge

RUN apt-get update && apt-get dist-upgrade -y \
    && apt-get autoremove -y \
    && apt-get autoclean -y \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/*

######################################################################
# - Final stage
#   - Install the main packages and set the entrypoint.
######################################################################

FROM tools AS final

# Set up the user environment
ENV TZ=Asia/Taipei
ENV SHELL=/bin/bash
ENV TERM=xterm-256color
USER $USERNAME
WORKDIR /esp_daemon

# Set up bashrc
COPY .bashrc /home/$USERNAME/.bashrc.conf
RUN cat /home/$USERNAME/.bashrc.conf >> /home/$USERNAME/.bashrc

# Set up python environment
# RUN pip install --upgrade pip
# RUN pip install --no-cache-dir

CMD ["/bin/bash"]