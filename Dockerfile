ARG ROS_DISTRO=humble
FROM ros:${ROS_DISTRO}-ros-base AS final

LABEL org.opencontainers.image.title="ESP Daemon"
LABEL org.opencontainers.image.authors="scx@gapp.nthu.edu.tw"
LABEL org.opencontainers.image.licenses="MIT"

ARG USERNAME=ros
ARG USER_UID=1000
ARG USER_GID=1000
ARG CONTAINER_TZ=Etc/UTC
ARG DEFAULT_RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && OPTIONAL_ROS_PACKAGES=() \
    && for pkg in "ros-${ROS_DISTRO}-foxglove-bridge" "ros-${ROS_DISTRO}-micro-ros-agent"; do \
         if apt-cache show "${pkg}" >/dev/null 2>&1; then \
           OPTIONAL_ROS_PACKAGES+=("${pkg}"); \
         fi; \
       done \
    && apt-get install -y --no-install-recommends \
        bash-completion \
        ca-certificates \
        curl \
        git \
        locales \
        python3-colcon-common-extensions \
        python3-pip \
        python3-rosdep \
        sudo \
        tree \
        unzip \
        vim \
        wget \
        ros-${ROS_DISTRO}-rmw-cyclonedds-cpp \
        ros-${ROS_DISTRO}-rmw-fastrtps-cpp \
        "${OPTIONAL_ROS_PACKAGES[@]}" \
    && locale-gen en_US en_US.UTF-8 \
    && update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux \
    && if getent group "${USER_GID}" >/dev/null; then \
        USER_GROUP="$(getent group "${USER_GID}" | cut -d: -f1)"; \
      else \
        USER_GROUP="${USERNAME}"; \
        groupadd --gid "${USER_GID}" "${USER_GROUP}"; \
      fi \
    && if id -u "${USERNAME}" >/dev/null 2>&1; then \
        usermod --uid "${USER_UID}" --gid "${USER_GID}" "${USERNAME}"; \
      else \
        useradd --uid "${USER_UID}" --gid "${USER_GID}" -m "${USERNAME}"; \
      fi \
    && usermod -aG dialout "${USERNAME}" \
    && echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > "/etc/sudoers.d/${USERNAME}" \
    && chmod 0440 "/etc/sudoers.d/${USERNAME}"

ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    TZ=${CONTAINER_TZ} \
    SHELL=/bin/bash \
    TERM=xterm-256color \
    ROS_DISTRO=${ROS_DISTRO} \
    RMW_IMPLEMENTATION=${DEFAULT_RMW_IMPLEMENTATION}

WORKDIR /home/${USERNAME}/esp_daemon

COPY .bashrc /tmp/esp-daemon.bashrc
RUN cat /tmp/esp-daemon.bashrc >> /home/${USERNAME}/.bashrc \
    && rm /tmp/esp-daemon.bashrc \
    && chown -R ${USERNAME}:${USER_GID} /home/${USERNAME}

USER ${USERNAME}

CMD ["/bin/bash"]
