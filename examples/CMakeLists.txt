# Copyright (c) Orbbec Inc. All Rights Reserved.
# Licensed under the MIT License.

cmake_minimum_required(VERSION 3.5)

file(GLOB subdirectories RELATIVE ${CMAKE_CURRENT_LIST_DIR} "*")

# dependecies
find_package(OpenCV QUIET)
if(NOT ${OpenCV_FOUND})
    message(WARNING "OpenCV not found, some examples may not be built. Please install OpenCV or set OpenCV_DIR to the directory of your OpenCV installation.")
endif()

# utils
add_subdirectory(utils)

# c examples
add_subdirectory(c_examples)

# cpp examples
add_subdirectory(0.basic.enumerate)
add_subdirectory(1.stream.imu)
add_subdirectory(2.device.control)
add_subdirectory(2.device.firmware_update)
add_subdirectory(2.device.multi_devices_firmware_update)
add_subdirectory(2.device.optional_depth_presets_update)
add_subdirectory(3.advanced.point_cloud)
add_subdirectory(3.advanced.preset)
add_subdirectory(4.misc.logger)
add_subdirectory(4.misc.metadata)
if(OB_BUILD_LINUX)
    add_subdirectory(3.advanced.multi_devices_sync_gmsltrigger)
endif()

if (OB_BUILD_PCL_EXAMPLES)
    add_subdirectory(5.wrapper.pcl)
endif()

if (OB_BUILD_OPEN3D_EXAMPLES)
    add_subdirectory(5.wrapper.open3d)
endif()

# cpp examples with OpenCV Requirements
if(${OpenCV_FOUND})
    add_subdirectory(0.basic.quick_start)
    add_subdirectory(1.stream.depth)
    add_subdirectory(1.stream.color)
    add_subdirectory(1.stream.infrared)
    add_subdirectory(1.stream.callback)
    add_subdirectory(1.stream.multi_streams)
    add_subdirectory(2.device.hot_plugin)
    add_subdirectory(2.device.record)
    add_subdirectory(2.device.playback)
    add_subdirectory(3.advanced.common_usages)
    add_subdirectory(3.advanced.sync_align)
    add_subdirectory(3.advanced.hw_d2c_align)
    add_subdirectory(3.advanced.post_processing)
    add_subdirectory(3.advanced.coordinate_transform)
    add_subdirectory(3.advanced.hdr)
    add_subdirectory(3.advanced.laser_interleave)
    add_subdirectory(3.advanced.multi_devices)
    add_subdirectory(3.advanced.multi_devices_sync)
    add_subdirectory(4.misc.save_to_disk)
    add_subdirectory(5.wrapper.opencv)
endif()

# InstallRequiredSystemLibraries
include(InstallRequiredSystemLibraries)
