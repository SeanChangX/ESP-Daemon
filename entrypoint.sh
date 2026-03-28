#!/usr/bin/env bash
set -euo pipefail

ROS_DISTRO="${ROS_DISTRO:-humble}"
WS_DIR="${WS_DIR:-$HOME/esp_daemon}"
SERIAL_DEV="${MICRO_ROS_SERIAL_DEV:-/dev/ttyACM0}"
AUTO_BOOTSTRAP_MICRO_ROS="${AUTO_BOOTSTRAP_MICRO_ROS:-true}"
MICRO_ROS_INSTALL_ARGS="${MICRO_ROS_INSTALL_ARGS:-}"
AGENT_EXEC="${WS_DIR}/install/micro_ros_agent/lib/micro_ros_agent/micro_ros_agent"
INSTALLER_SCRIPT="${WS_DIR}/micro-ROS_install.sh"

is_true() {
  case "${1,,}" in
    1|true|yes|on)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

source_ros_env() {
  set +u
  source "/opt/ros/${ROS_DISTRO}/setup.bash"
  if [[ -f "${WS_DIR}/install/setup.bash" ]]; then
    source "${WS_DIR}/install/setup.bash"
  fi
  set -u
}

bootstrap_micro_ros_if_needed() {
  if [[ -x "${AGENT_EXEC}" ]] && ros2 pkg executables micro_ros_agent | grep -q "micro_ros_agent"; then
    return 0
  fi

  if ! is_true "${AUTO_BOOTSTRAP_MICRO_ROS}"; then
    cat >&2 <<EOF
micro_ros_agent is missing and AUTO_BOOTSTRAP_MICRO_ROS is disabled.
Run bootstrap manually:
  cd ${WS_DIR}
  ./micro-ROS_install.sh
EOF
    exit 1
  fi

  if [[ ! -f "${INSTALLER_SCRIPT}" ]]; then
    echo "Bootstrap script not found: ${INSTALLER_SCRIPT}" >&2
    exit 1
  fi

  chmod +x "${INSTALLER_SCRIPT}"
  echo "micro_ros_agent is missing, running bootstrap ..."

  if [[ -n "${MICRO_ROS_INSTALL_ARGS}" ]]; then
    local -a install_args
    read -r -a install_args <<< "${MICRO_ROS_INSTALL_ARGS}"
    "${INSTALLER_SCRIPT}" "${install_args[@]}"
  else
    "${INSTALLER_SCRIPT}"
  fi

  source_ros_env

  if [[ ! -x "${AGENT_EXEC}" ]] || ! ros2 pkg executables micro_ros_agent | grep -q "micro_ros_agent"; then
    echo "Bootstrap finished, but micro_ros_agent is still unavailable." >&2
    exit 1
  fi
}

source_ros_env

if (($# > 0)); then
  exec "$@"
fi

bootstrap_micro_ros_if_needed

if [[ -e "${SERIAL_DEV}" ]]; then
  sudo chmod a+rw "${SERIAL_DEV}" || true
else
  echo "Warning: ${SERIAL_DEV} not found; micro_ros_agent may fail to connect." >&2
fi

exec ros2 run micro_ros_agent micro_ros_agent serial --dev "${SERIAL_DEV}"
