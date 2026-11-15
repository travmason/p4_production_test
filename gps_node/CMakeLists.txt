# GPS node: an ESP32-S3 that reads a Dragy Pro over Bluetooth (or, later, a
# u-blox module on a UART) and puts speed, position and time on the CAN bus for
# the dashboard. A separate ESP-IDF project from the dashboard in the parent
# directory, sharing only the frame definitions.
#
#     cd gps_node
#     idf.py set-target esp32s3      (first time only)
#     idf.py build flash monitor
#
cmake_minimum_required(VERSION 3.16)

# The CAN frame codec and the UBX parser, shared with the dashboard.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/dash_gps_proto")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gps_node)
