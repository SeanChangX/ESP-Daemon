#!/usr/bin/env python3

from launch import LaunchDescription
from launch.actions import ExecuteProcess
from ament_index_python.packages import get_package_share_directory
import os


def generate_launch_description():
    package_share_directory = get_package_share_directory('battery_converter')
    executable_path = os.path.join(package_share_directory, '..', '..', 'bin', 'battery_converter_node')
    
    return LaunchDescription([
        ExecuteProcess(
            cmd=[executable_path],
            name='battery_converter_node',
            output='screen'
        ),
    ])
