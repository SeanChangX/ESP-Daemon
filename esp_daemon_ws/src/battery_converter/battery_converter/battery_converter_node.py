#!/usr/bin/env python3

import rclpy
from rclpy.node import Node
from sensor_msgs.msg import BatteryState
from std_msgs.msg import Float32


class BatteryConverterNode(Node):
    """
    Convert MAVROS battery data to separate voltage and current topics
    """

    def __init__(self):
        super().__init__('battery_converter_node')
        
        # Create publishers for voltage and current
        self.voltage_publisher = self.create_publisher(
            Float32,
            '/battery_voltage',
            10
        )
        
        self.current_publisher = self.create_publisher(
            Float32,
            '/battery_current',
            10
        )
        
        # Create subscriber for MAVROS battery data
        self.battery_subscriber = self.create_subscription(
            BatteryState,
            '/mavros/battery',
            self.battery_callback,
            10
        )
        
        self.get_logger().info('Battery converter node started')
        self.get_logger().info('Subscribing to /mavros/battery')
        self.get_logger().info('Publishing to /battery_voltage and /battery_current')

    def battery_callback(self, msg):
        """
        Callback function to process battery data and publish voltage/current separately
        """
        try:
            # Extract voltage (in volts)
            voltage = msg.voltage
            if voltage > 0:  # Only publish if voltage is valid
                voltage_msg = Float32()
                voltage_msg.data = voltage
                self.voltage_publisher.publish(voltage_msg)
                self.get_logger().debug(f'Published voltage: {voltage:.2f}V')
            
            # Extract current (in amperes)
            current = msg.current
            if current is not None:  # Only publish if current is valid
                current_msg = Float32()
                current_msg.data = current
                self.current_publisher.publish(current_msg)
                self.get_logger().debug(f'Published current: {current:.2f}A')
            
        except Exception as e:
            self.get_logger().error(f'Error processing battery data: {str(e)}')


def main(args=None):
    rclpy.init(args=args)
    
    node = BatteryConverterNode()
    
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
