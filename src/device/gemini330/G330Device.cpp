// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#include "G330Device.hpp"

#include "DevicePids.hpp"
#include "InternalTypes.hpp"

#include "utils/Utils.hpp"
#include "environment/EnvConfig.hpp"
#include "usb/uvc/UvcDevicePort.hpp"
#include "stream/StreamProfileFactory.hpp"
#include "sensor/video/VideoSensor.hpp"
#include "sensor/video/DisparityBasedSensor.hpp"
#include "sensor/imu/ImuStreamer.hpp"
#include "sensor/imu/AccelSensor.hpp"
#include "sensor/imu/GyroSensor.hpp"

#include "FilterFactory.hpp"
#include "publicfilters/FormatConverterProcess.hpp"

#include "metadata/FrameMetadataParserContainer.hpp"
#include "timestamp/GlobalTimestampFitter.hpp"
#include "timestamp/FrameTimestampCalculator.hpp"
#include "timestamp/DeviceClockSynchronizer.hpp"
#include "property/VendorPropertyAccessor.hpp"
#include "property/UvcPropertyAccessor.hpp"
#include "property/PropertyServer.hpp"
#include "property/CommonPropertyAccessors.hpp"
#include "property/FilterPropertyAccessors.hpp"
#include "property/PrivateFilterPropertyAccessors.hpp"
#include "monitor/DeviceMonitor.hpp"
#include "syncconfig/DeviceSyncConfigurator.hpp"
#include "firmwareupdater/FirmwareUpdater.hpp"
#include "firmwareupdater/firmwareupdateguard/FirmwareUpdateGuards.hpp"
#include "frameprocessor/FrameProcessor.hpp"

#include "G330MetadataParser.hpp"
#include "G330MetadataTypes.hpp"
#include "G330AlgParamManager.hpp"
#include "G330PresetManager.hpp"
#include "G330DepthWorkModeManager.hpp"
#include "G330SensorStreamStrategy.hpp"
#include "G330PropertyAccessors.hpp"
#include "G330FrameMetadataParserContainer.hpp"
#include "G330NetDisparitySensor.hpp"
#include "G330NetVideoSensor.hpp"
#include "G330NetAccelSensor.hpp"
#include "G330NetGyroSensor.hpp"
#include "utils/BufferParser.hpp"
#include "G330FrameInterleaveManager.hpp"
#include "G330NetStreamProfileFilter.hpp"
#include "G330DeviceInfo.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

namespace libobsensor {

constexpr uint8_t  INTERFACE_COLOR        = 4;
constexpr uint8_t  INTERFACE_DEPTH        = 0;
constexpr uint16_t GMSL_MAX_CMD_DATA_SIZE = 232;

G330Device::G330Device(const std::shared_ptr<const IDeviceEnumInfo> &info) : DeviceBase(info), isGmslDevice_(info->getConnectionType() == "GMSL2") {
    init();

    // check and start heartbeat after initialization is complete
    checkAndStartHeartbeat();
}

G330Device::~G330Device() noexcept {}

void G330Device::init() {
    if(isGmslDevice_) {
        LOG_DEBUG("G330Device::init() for GMSL2 device");
        initSensorListGMSL();
    }
    else {
        initSensorList();
    }
    initProperties();
    fetchDeviceInfo();
    fetchExtensionInfo();

    videoFrameTimestampCalculatorCreator_ = [this]() {
        auto metadataType = OB_FRAME_METADATA_TYPE_TIMESTAMP;
        auto iter         = std::find(G330LDevPids.begin(), G330LDevPids.end(), deviceInfo_->pid_);
        if(iter == G330LDevPids.end()) {
            metadataType = OB_FRAME_METADATA_TYPE_SENSOR_TIMESTAMP;
        }
        return std::make_shared<FrameTimestampCalculatorOverMetadata>(this, metadataType, frameTimeFreq_);
    };

    auto globalTimestampFilter = std::make_shared<GlobalTimestampFitter>(this);
    registerComponent(OB_DEV_COMPONENT_GLOBAL_TIMESTAMP_FILTER, globalTimestampFilter);

    auto algParamManager = std::make_shared<G330AlgParamManager>(this);
    registerComponent(OB_DEV_COMPONENT_ALG_PARAM_MANAGER, algParamManager);

    auto depthWorkModeManager = std::make_shared<G330DepthWorkModeManager>(this);
    registerComponent(OB_DEV_COMPONENT_DEPTH_WORK_MODE_MANAGER, depthWorkModeManager);

    if(getFirmwareVersionInt() >= 10441) {
        // support custom presets upgrade
        auto propertyServer = getPropertyServer();
        propertyServer->registerAccessCallback(
            {
                OB_STRUCT_CURRENT_DEPTH_ALG_MODE,
            },
            [&](uint32_t propertyId, const uint8_t *, size_t, PropertyOperationType operationType) {
                if(operationType == PROP_OP_WRITE && propertyId == OB_STRUCT_CURRENT_DEPTH_ALG_MODE) {
                    // fetch preset version info via fetchExtensionInfo
                    fetchExtensionInfo();
                }
            });
    }

    auto presetManager = std::make_shared<G330PresetManager>(this);
    registerComponent(OB_DEV_COMPONENT_PRESET_MANAGER, presetManager);

    auto fwVersion = getFirmwareVersionInt();
    if(fwVersion > 10370) {
        auto propertyServer         = getPropertyServer();
        auto vendorPropertyAccessor = getComponentT<VendorPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
        propertyServer->registerProperty(OB_PROP_FRAME_INTERLEAVE_CONFIG_INDEX_INT, "rw", "rw", vendorPropertyAccessor.get());
        propertyServer->registerProperty(OB_PROP_FRAME_INTERLEAVE_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor.get());
        propertyServer->registerProperty(OB_PROP_FRAME_INTERLEAVE_LASER_PATTERN_SYNC_DELAY_INT, "rw", "rw", vendorPropertyAccessor.get());

        auto frameInterleaveManager = std::make_shared<G330FrameInterleaveManager>(this);
        registerComponent(OB_DEV_COMPONENT_FRAME_INTERLEAVE_MANAGER, frameInterleaveManager);
    }

    if(fwVersion >= 10401) {
        auto propertyServer                = getPropertyServer();
        auto hwNoiseRemovePropertyAccessor = std::make_shared<G330HWNoiseRemovePropertyAccessor>(this);
        propertyServer->registerProperty(OB_PROP_HW_NOISE_REMOVE_FILTER_ENABLE_BOOL, "rw", "rw", hwNoiseRemovePropertyAccessor);
        propertyServer->registerProperty(OB_PROP_HW_NOISE_REMOVE_FILTER_THRESHOLD_FLOAT, "rw", "rw", hwNoiseRemovePropertyAccessor);
    }

    if(fwVersion >= 10510) {
        auto propertyServer         = getPropertyServer();
        auto vendorPropertyAccessor = getComponentT<VendorPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
        propertyServer->registerProperty(OB_DEVICE_AUTO_CAPTURE_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor.get());
        propertyServer->registerProperty(OB_DEVICE_AUTO_CAPTURE_INTERVAL_TIME_INT, "rw", "rw", vendorPropertyAccessor.get());
    }

    if(fwVersion >= 10540) {
        auto propertyServer         = getPropertyServer();
        auto vendorPropertyAccessor = getComponentT<VendorPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
        propertyServer->registerProperty(OB_STRUCT_DEVICE_ERROR_STATE, "", "r", vendorPropertyAccessor.get());
    }

    auto sensorStreamStrategy = std::make_shared<G330SensorStreamStrategy>(this);
    registerComponent(OB_DEV_COMPONENT_SENSOR_STREAM_STRATEGY, sensorStreamStrategy);

    static const std::vector<OBMultiDeviceSyncMode> supportedSyncModes = {
        OB_MULTI_DEVICE_SYNC_MODE_FREE_RUN,         OB_MULTI_DEVICE_SYNC_MODE_STANDALONE,          OB_MULTI_DEVICE_SYNC_MODE_PRIMARY,
        OB_MULTI_DEVICE_SYNC_MODE_SECONDARY_SYNCED, OB_MULTI_DEVICE_SYNC_MODE_SOFTWARE_TRIGGERING, OB_MULTI_DEVICE_SYNC_MODE_HARDWARE_TRIGGERING
    };
    auto deviceSyncConfigurator = std::make_shared<DeviceSyncConfigurator>(this, supportedSyncModes);
    registerComponent(OB_DEV_COMPONENT_DEVICE_SYNC_CONFIGURATOR, deviceSyncConfigurator);

    auto deviceClockSynchronizer = std::make_shared<DeviceClockSynchronizer>(this);
    registerComponent(OB_DEV_COMPONENT_DEVICE_CLOCK_SYNCHRONIZER, deviceClockSynchronizer);

    registerComponent(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY, [this]() {
        std::shared_ptr<FrameProcessorFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FrameProcessorFactory>(this); })
        return factory;
    });

    registerComponent(OB_DEV_COMPONENT_FIRMWARE_UPDATER, [this]() {
        std::shared_ptr<FirmwareUpdater> firmwareUpdater;
        TRY_EXECUTE({ firmwareUpdater = std::make_shared<FirmwareUpdater>(this); })
        return firmwareUpdater;
    });

    registerComponent(OB_DEV_COMPONENT_FIRMWARE_UPDATE_GUARD_FACTORY, [this]() {
        std::shared_ptr<FirmwareUpdateGuardFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FirmwareUpdateGuardFactory>(this); })
        return factory;
    });

    registerComponent(OB_DEV_COMPONENT_COLOR_FRAME_METADATA_CONTAINER, [this]() {
        std::shared_ptr<FrameMetadataParserContainer> container;
#ifdef __linux__
        auto sensorPortInfo = getSensorPortInfo(OB_SENSOR_COLOR);
        if(sensorPortInfo->portType == SOURCE_PORT_USB_UVC && !isGmslDevice_) {
            auto port    = getSourcePort(sensorPortInfo);
            auto uvcPort = std::dynamic_pointer_cast<UvcDevicePort>(port);
            auto backend = uvcPort->getBackendType();
            if(backend == OB_UVC_BACKEND_TYPE_V4L2) {
                container = std::make_shared<G330ColorFrameMetadataParserContainerByScr>(this, deviceTimeFreq_, frameTimeFreq_);
                return container;
            }
        }
#endif
        container = std::make_shared<G330ColorFrameMetadataParserContainer>(this);
        return container;
    });

    registerComponent(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER, [this]() {
        std::shared_ptr<FrameMetadataParserContainer> container;
#ifdef __linux__
        auto sensorPortInfo = getSensorPortInfo(OB_SENSOR_DEPTH);
        if(sensorPortInfo->portType == SOURCE_PORT_USB_UVC && !isGmslDevice_) {
            auto port    = getSourcePort(sensorPortInfo);
            auto uvcPort = std::dynamic_pointer_cast<UvcDevicePort>(port);
            auto backend = uvcPort->getBackendType();
            if(backend == OB_UVC_BACKEND_TYPE_V4L2) {
                container = std::make_shared<G330DepthFrameMetadataParserContainerByScr>(this, deviceTimeFreq_, frameTimeFreq_);
                return container;
            }
        }
#endif
        container = std::make_shared<G330DepthFrameMetadataParserContainer>(this);
        return container;
    });

    fetchDeviceErrorState();
}

std::shared_ptr<const StreamProfile> G330Device::loadDefaultStreamProfile(OBSensorType sensorType) {
    std::shared_ptr<const StreamProfile> defaultStreamProfile = nullptr;
    LOG_DEBUG("loadDefaultStreamProfile: deviceConnectionType:={}", deviceInfo_->connectionType_);

    OBStreamType defStreamType = OB_STREAM_UNKNOWN;
    int          defFps        = 10;
    int          defWidth      = 848;
    int          defHeight     = 480;
    OBFormat     defFormat     = OB_FORMAT_Y16;

    // USB2.0 default resolution config
    if(deviceInfo_->connectionType_ == "USB2.1") {
        LOG_DEBUG("loadDefaultStreamProfile set USB2.1 device default stream profile.");
        switch(sensorType) {
        case OB_SENSOR_DEPTH:
            defStreamType = OB_STREAM_DEPTH;
            break;
        case OB_SENSOR_IR_LEFT:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR_LEFT;
            break;
        case OB_SENSOR_IR_RIGHT:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR_RIGHT;
            break;
        case OB_SENSOR_IR:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR;
            break;
        case OB_SENSOR_COLOR: {
            defFormat     = OB_FORMAT_MJPG;
            defStreamType = OB_STREAM_COLOR;
            defWidth      = 1280;
            defHeight     = 720;

        } break;
        default:
            break;
        }
    }

    // GMSL2 default resolution config
    if(deviceInfo_->connectionType_ == "GMSL2") {
        LOG_DEBUG("loadDefaultStreamProfile set GMSL2 device default stream profile.");
        defFps = 30;
        switch(sensorType) {
        case OB_SENSOR_DEPTH:
            defStreamType = OB_STREAM_DEPTH;
            break;
        case OB_SENSOR_IR_LEFT:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR_LEFT;
            break;
        case OB_SENSOR_IR_RIGHT:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR_RIGHT;
            break;
        case OB_SENSOR_IR:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR;
            break;
        case OB_SENSOR_COLOR: {
            defFormat     = OB_FORMAT_YUYV;
            defStreamType = OB_STREAM_COLOR;
            defWidth      = 1280;
            defHeight     = 720;

        } break;
        default:
            break;
        }
    }

    if(defStreamType != OB_STREAM_UNKNOWN) {
        defaultStreamProfile = StreamProfileFactory::createVideoStreamProfile(defStreamType, defFormat, defWidth, defHeight, defFps);
        LOG_DEBUG("default profile StreamType:{}, Format:{}, Width:{}, Height:{}, Fps:{}", defStreamType, defFormat, defWidth, defHeight, defFps);
    }

    if(!defaultStreamProfile) {
        // load default stream profile from env config
        defaultStreamProfile = StreamProfileFactory::getDefaultStreamProfileFromEnvConfig(deviceInfo_->name_, sensorType);
    }

    return defaultStreamProfile;
}

void G330Device::initSensorStreamProfile(std::shared_ptr<ISensor> sensor) {
    auto defaultStreamProfile = loadDefaultStreamProfile(sensor->getSensorType());
    if(defaultStreamProfile != nullptr) {
        sensor->updateDefaultStreamProfile(defaultStreamProfile);
    }

    // bind params: extrinsics, intrinsics, etc.
    auto profiles = sensor->getStreamProfileList();
    {
        auto algParamManager = getComponentT<G330AlgParamManager>(OB_DEV_COMPONENT_ALG_PARAM_MANAGER);
        algParamManager->bindStreamProfileParams(profiles);
    }

    auto sensorType = sensor->getSensorType();
    LOG_INFO("Sensor {} created! Found {} stream profiles.", sensorType, profiles.size());
    for(auto &profile: profiles) {
        LOG_INFO(" - {}", profile);
    }
}

void G330Device::initSensorList() {
    registerComponent(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY, [this]() {
        std::shared_ptr<FrameProcessorFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FrameProcessorFactory>(this); })
        return factory;
    });

    const auto &sourcePortInfoList = enumInfo_->getSourcePortInfoList();
    auto depthPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_UVC && std::dynamic_pointer_cast<const USBSourcePortInfo>(portInfo)->infIndex == INTERFACE_DEPTH;
    });

    if(depthPortInfoIter != sourcePortInfoList.end()) {
        auto depthPortInfo = *depthPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_DEPTH_SENSOR,
            [this, depthPortInfo]() {
                auto port   = getSourcePort(depthPortInfo);
                auto sensor = std::make_shared<DisparityBasedSensor>(this, OB_SENSOR_DEPTH, port);

                sensor->updateFormatFilterConfig({ { FormatFilterPolicy::REMOVE, OB_FORMAT_Y8, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BGR, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BGRA, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_YV12, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_UYVY, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REPLACE, OB_FORMAT_Z16, OB_FORMAT_Y16, nullptr } });

                auto depthMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                auto propServer = getPropertyServer();
                auto depthUnit  = propServer->getPropertyValueT<float>(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT);
                sensor->setDepthUnit(depthUnit);

                auto hwD2D = propServer->getPropertyValueT<bool>(OB_PROP_DISPARITY_TO_DEPTH_BOOL);
                sensor->markOutputDisparityFrame(!hwD2D);

                initSensorStreamProfile(sensor);

                sensor->registerStreamStateChangedCallback([&](OBStreamState state, const std::shared_ptr<const StreamProfile> &sp) {
                    if(state == STREAM_STATE_STREAMING) {
                        auto algParamManager = getComponentT<G330AlgParamManager>(OB_DEV_COMPONENT_ALG_PARAM_MANAGER);
                        algParamManager->reFetchDisparityParams();
                        algParamManager->bindDisparityParam({ sp });
                    }
                });

                loadDefaultDepthPostProcessingConfig();
                return sensor;
            },
            true);

        registerSensorPortInfo(OB_SENSOR_DEPTH, depthPortInfo);

        registerComponent(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_DEPTH);
            return frameProcessor;
        });

        registerComponent(
            OB_DEV_COMPONENT_LEFT_IR_SENSOR,
            [this, depthPortInfo]() {
                auto port   = getSourcePort(depthPortInfo);
                auto sensor = std::make_shared<VideoSensor>(this, OB_SENSOR_IR_LEFT, port);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },  //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_YV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_NV12, OB_FORMAT_Y12, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BGR, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BGRA, OB_FORMAT_ANY, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FrameUnpacker", OB_SENSOR_IR_LEFT, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::REPLACE, OB_FORMAT_NV12, OB_FORMAT_Y16, formatConverter });
                }

                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto depthMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_IR_LEFT, depthPortInfo);

        registerComponent(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_IR_LEFT);
            return frameProcessor;
        });

        registerComponent(
            OB_DEV_COMPONENT_RIGHT_IR_SENSOR,
            [this, depthPortInfo]() {
                auto port   = getSourcePort(depthPortInfo);
                auto sensor = std::make_shared<VideoSensor>(this, OB_SENSOR_IR_RIGHT, port);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },   //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Y8, OB_FORMAT_ANY, nullptr },    //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },  //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BGR, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BGRA, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_UYVY, OB_FORMAT_ANY, nullptr },  //
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_BA81, OB_FORMAT_Y8, nullptr },  //
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_YV12, OB_FORMAT_Y12, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FrameUnpacker", OB_SENSOR_IR_RIGHT, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::REPLACE, OB_FORMAT_YV12, OB_FORMAT_Y16, formatConverter });
                }

                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto depthMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_IR_RIGHT, depthPortInfo);

        registerComponent(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_IR_RIGHT);
            return frameProcessor;
        });

        // the main property accessor is using the depth port(uvc xu)
        registerComponent(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR, [this, depthPortInfo]() {
            auto port          = getSourcePort(depthPortInfo);
            auto uvcDevicePort = std::dynamic_pointer_cast<UvcDevicePort>(port);
            uvcDevicePort->updateXuUnit(OB_G330_XU_UNIT);  // update xu unit to g330 xu unit
            auto accessor = std::make_shared<VendorPropertyAccessor>(this, port);
            return accessor;
        });

        // The device monitor is using the depth port(uvc xu)
        registerComponent(OB_DEV_COMPONENT_DEVICE_MONITOR, [this, depthPortInfo]() {
            auto port          = getSourcePort(depthPortInfo);
            auto uvcDevicePort = std::dynamic_pointer_cast<UvcDevicePort>(port);
            uvcDevicePort->updateXuUnit(OB_G330_XU_UNIT);  // update xu unit to g330 xu unit
            auto devMonitor = std::make_shared<DeviceMonitor>(this, port);
            return devMonitor;
        });
    }

    auto colorPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_UVC && std::dynamic_pointer_cast<const USBSourcePortInfo>(portInfo)->infIndex == INTERFACE_COLOR;
    });

    if(colorPortInfoIter != sourcePortInfoList.end()) {
        auto colorPortInfo = *colorPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_COLOR_SENSOR,
            [this, colorPortInfo]() {
                auto port   = getSourcePort(colorPortInfo);
                auto sensor = std::make_shared<VideoSensor>(this, OB_SENSOR_COLOR, port);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_BYR2, OB_FORMAT_RW16, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FormatConverter", OB_SENSOR_COLOR, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_RGB, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_RGBA, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_BGR, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_BGRA, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_Y16, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_Y8, formatConverter });
                }

                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto colorMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_COLOR_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(colorMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_COLOR, colorPortInfo);

        registerComponent(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_COLOR);
            return frameProcessor;
        });
    }

    auto imuPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_HID;  //
    });

    if(imuPortInfoIter != sourcePortInfoList.end()) {
        auto imuPortInfo = *imuPortInfoIter;

        registerComponent(OB_DEV_COMPONENT_IMU_STREAMER, [this, imuPortInfo]() {
            // the gyro and accel are both on the same port and share the same filter

            auto port               = getSourcePort(imuPortInfo);
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", OB_SENSOR_ACCEL, true);

            auto dataStreamPort = std::dynamic_pointer_cast<IDataStreamPort>(port);
            auto imuStreamer    = std::make_shared<ImuStreamer>(this, dataStreamPort, imuCorrectorFilter);
            return imuStreamer;
        });

        registerComponent(
            OB_DEV_COMPONENT_ACCEL_SENSOR,
            [this, imuPortInfo]() {
                auto port                 = getSourcePort(imuPortInfo);
                auto imuStreamer          = getComponentT<ImuStreamer>(OB_DEV_COMPONENT_IMU_STREAMER);
                auto imuStreamerSharedPtr = imuStreamer.get();
                auto sensor               = std::make_shared<AccelSensor>(this, port, imuStreamerSharedPtr);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_ACCEL, imuPortInfo);

        registerComponent(
            OB_DEV_COMPONENT_GYRO_SENSOR,
            [this, imuPortInfo]() {
                auto port                 = getSourcePort(imuPortInfo);
                auto imuStreamer          = getComponentT<ImuStreamer>(OB_DEV_COMPONENT_IMU_STREAMER);
                auto imuStreamerSharedPtr = imuStreamer.get();
                auto sensor               = std::make_shared<GyroSensor>(this, port, imuStreamerSharedPtr);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_GYRO, imuPortInfo);
    }
}

static const uint8_t GMSL_INTERFACE_DEPTH    = 0;
static const uint8_t GMSL_INTERFACE_IR       = 2;
static const uint8_t GMSL_INTERFACE_IR_LEFT  = 2;
static const uint8_t GMSL_INTERFACE_IR_RIGHT = 3;
static const uint8_t GMSL_INTERFACE_COLOR    = 4;
void                 G330Device::initSensorListGMSL() {
    registerComponent(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY, [this]() {
        std::shared_ptr<FrameProcessorFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FrameProcessorFactory>(this); })
        return factory;
    });

    const auto &sourcePortInfoList = enumInfo_->getSourcePortInfoList();
    auto depthPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_UVC && std::dynamic_pointer_cast<const USBSourcePortInfo>(portInfo)->infIndex == GMSL_INTERFACE_DEPTH;
    });
    if(depthPortInfoIter != sourcePortInfoList.end()) {
        auto depthPortInfo = *depthPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_DEPTH_SENSOR,
            [this, depthPortInfo]() {
                auto port   = getSourcePort(depthPortInfo);
                auto sensor = std::make_shared<DisparityBasedSensor>(this, OB_SENSOR_DEPTH, port);

                sensor->updateFormatFilterConfig({ { FormatFilterPolicy::REMOVE, OB_FORMAT_Y8, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_MJPG, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_Y10, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_Y14, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_YV12, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_UYVY, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_YUYV, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REPLACE, OB_FORMAT_Z16, OB_FORMAT_Y16, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BGR, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BGRA, OB_FORMAT_ANY, nullptr } });

                auto depthMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                auto propServer = getPropertyServer();
                auto depthUnit  = propServer->getPropertyValueT<float>(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT);
                sensor->setDepthUnit(depthUnit);

                auto hwD2D = propServer->getPropertyValueT<bool>(OB_PROP_DISPARITY_TO_DEPTH_BOOL);
                sensor->markOutputDisparityFrame(!hwD2D);

                initSensorStreamProfile(sensor);

                sensor->registerStreamStateChangedCallback([&](OBStreamState state, const std::shared_ptr<const StreamProfile> &sp) {
                    if(state == STREAM_STATE_STREAMING) {
                        auto algParamManager = getComponentT<G330AlgParamManager>(OB_DEV_COMPONENT_ALG_PARAM_MANAGER);
                        algParamManager->reFetchDisparityParams();
                        algParamManager->bindDisparityParam({ sp });
                    }
                });
                loadDefaultDepthPostProcessingConfig();
                return sensor;
            },
            true);

        registerSensorPortInfo(OB_SENSOR_DEPTH, depthPortInfo);

        registerComponent(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_DEPTH);
            return frameProcessor;
        });

        // the main property accessor is using the depth port(uvc xu)
        registerComponent(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR, [this, depthPortInfo]() {
            auto port          = getSourcePort(depthPortInfo);
            auto uvcDevicePort = std::dynamic_pointer_cast<UvcDevicePort>(port);
            uvcDevicePort->updateXuUnit(OB_G330_XU_UNIT);  // update xu unit to g330 xu unit
            auto accessor = std::make_shared<VendorPropertyAccessor>(this, port);
            accessor->setRawdataTransferPacketSize(GMSL_MAX_CMD_DATA_SIZE);
            accessor->setStructListDataTransferPacketSize(GMSL_MAX_CMD_DATA_SIZE);
            return accessor;
        });

        // The device monitor is using the depth port(uvc xu)
        registerComponent(OB_DEV_COMPONENT_DEVICE_MONITOR, [this, depthPortInfo]() {
            auto port          = getSourcePort(depthPortInfo);
            auto uvcDevicePort = std::dynamic_pointer_cast<UvcDevicePort>(port);
            uvcDevicePort->updateXuUnit(OB_G330_XU_UNIT);  // update xu unit to g330 xu unit
            auto devMonitor = std::make_shared<DeviceMonitor>(this, port);
            return devMonitor;
        });
    }

    auto leftIrPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_UVC && std::dynamic_pointer_cast<const USBSourcePortInfo>(portInfo)->infIndex == GMSL_INTERFACE_IR_LEFT;
    });
    if(leftIrPortInfoIter != sourcePortInfoList.end()) {
        auto leftIrPortInfo = *leftIrPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_LEFT_IR_SENSOR,
            [this, leftIrPortInfo]() {
                auto port   = getSourcePort(leftIrPortInfo);
                auto sensor = std::make_shared<VideoSensor>(this, OB_SENSOR_IR_LEFT, port);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },   //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_MJPG, OB_FORMAT_ANY, nullptr },  //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Y10, OB_FORMAT_ANY, nullptr },   //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Y14, OB_FORMAT_ANY, nullptr },   //
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_UYVY, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_YUYV, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BGR, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BGRA, OB_FORMAT_ANY, nullptr }
                    // { FormatFilterPolicy::REPLACE, OB_FORMAT_NV12, OB_FORMAT_Y12, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FrameUnpacker", OB_SENSOR_IR_LEFT, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::REPLACE, OB_FORMAT_YUYV, OB_FORMAT_Y16, formatConverter });
                }

                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto depthMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_IR_LEFT, leftIrPortInfo);

        registerComponent(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_IR_LEFT);
            return frameProcessor;
        });
    }

    auto rightIrPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_UVC && std::dynamic_pointer_cast<const USBSourcePortInfo>(portInfo)->infIndex == GMSL_INTERFACE_IR_RIGHT;
    });
    if(rightIrPortInfoIter != sourcePortInfoList.end()) {
        auto rightIrPortInfo = *rightIrPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_RIGHT_IR_SENSOR,
            [this, rightIrPortInfo]() {
                auto port   = getSourcePort(rightIrPortInfo);
                auto sensor = std::make_shared<VideoSensor>(this, OB_SENSOR_IR_RIGHT, port);

                std::vector<FormatFilterConfig> formatFilterConfigs = { { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },  //
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_MJPG, OB_FORMAT_ANY, nullptr },  //
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_Y10, OB_FORMAT_ANY, nullptr },  //
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_Y14, OB_FORMAT_ANY, nullptr },  //
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_UYVY, OB_FORMAT_ANY, nullptr },
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_YUYV, OB_FORMAT_ANY, nullptr },
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_BGR, OB_FORMAT_ANY, nullptr },
                                                                        { FormatFilterPolicy::REMOVE, OB_FORMAT_BGRA, OB_FORMAT_ANY, nullptr } };

                auto formatConverter = getSensorFrameFilter("FrameUnpacker", OB_SENSOR_IR_RIGHT, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::REPLACE, OB_FORMAT_Y12, OB_FORMAT_Y16, formatConverter });
                }

                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto depthMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_IR_RIGHT, rightIrPortInfo);

        registerComponent(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_IR_RIGHT);
            return frameProcessor;
        });
    }

    auto colorPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_UVC && std::dynamic_pointer_cast<const USBSourcePortInfo>(portInfo)->infIndex == INTERFACE_COLOR;
    });

    if(colorPortInfoIter != sourcePortInfoList.end()) {
        auto colorPortInfo = *colorPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_COLOR_SENSOR,
            [this, colorPortInfo]() {
                auto port   = getSourcePort(colorPortInfo);
                auto sensor = std::make_shared<VideoSensor>(this, OB_SENSOR_COLOR, port);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Y14, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_MJPG, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Y10, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Y8, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_BYR2, OB_FORMAT_RW16, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FormatConverter", OB_SENSOR_COLOR, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_RGB, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_RGBA, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_BGR, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_BGRA, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_Y16, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_Y8, formatConverter });
                }

                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto colorMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_COLOR_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(colorMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_COLOR, colorPortInfo);

        registerComponent(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_COLOR);
            return frameProcessor;
        });
    }

    auto imuPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_USB_HID;  //
    });

    if(imuPortInfoIter != sourcePortInfoList.end()) {
        auto imuPortInfo = *imuPortInfoIter;

        registerComponent(OB_DEV_COMPONENT_IMU_STREAMER, [this, imuPortInfo]() {
            // the gyro and accel are both on the same port and share the same filter

            auto port               = getSourcePort(imuPortInfo);
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", OB_SENSOR_ACCEL, true);

            auto dataStreamPort = std::dynamic_pointer_cast<IDataStreamPort>(port);
            auto imuStreamer    = std::make_shared<ImuStreamer>(this, dataStreamPort, imuCorrectorFilter);
            return imuStreamer;
        });

        registerComponent(
            OB_DEV_COMPONENT_ACCEL_SENSOR,
            [this, imuPortInfo]() {
                auto port                 = getSourcePort(imuPortInfo);
                auto imuStreamer          = getComponentT<ImuStreamer>(OB_DEV_COMPONENT_IMU_STREAMER);
                auto imuStreamerSharedPtr = imuStreamer.get();
                auto sensor               = std::make_shared<AccelSensor>(this, port, imuStreamerSharedPtr);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);
                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_ACCEL, imuPortInfo);

        registerComponent(
            OB_DEV_COMPONENT_GYRO_SENSOR,
            [this, imuPortInfo]() {
                auto port                 = getSourcePort(imuPortInfo);
                auto imuStreamer          = getComponentT<ImuStreamer>(OB_DEV_COMPONENT_IMU_STREAMER);
                auto imuStreamerSharedPtr = imuStreamer.get();
                auto sensor               = std::make_shared<GyroSensor>(this, port, imuStreamerSharedPtr);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_GYRO, imuPortInfo);
    }
}

void G330Device::initProperties() {
    auto propertyServer = std::make_shared<PropertyServer>(this);

    auto d2dPropertyAccessor = std::make_shared<G330Disp2DepthPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DISPARITY_TO_DEPTH_BOOL, "rw", "rw", d2dPropertyAccessor);      // hw
    propertyServer->registerProperty(OB_PROP_SDK_DISPARITY_TO_DEPTH_BOOL, "rw", "rw", d2dPropertyAccessor);  // sw
    propertyServer->registerProperty(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT, "rw", "rw", d2dPropertyAccessor);

    auto privatePropertyAccessor = std::make_shared<PrivateFilterPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DEPTH_SOFT_FILTER_BOOL, "rw", "rw", privatePropertyAccessor);
    propertyServer->registerProperty(OB_PROP_DEPTH_MAX_DIFF_INT, "rw", "rw", privatePropertyAccessor);
    propertyServer->registerProperty(OB_PROP_DEPTH_MAX_SPECKLE_SIZE_INT, "rw", "rw", privatePropertyAccessor);

    auto frameTransformPropertyAccessor = std::make_shared<StereoFrameTransformPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DEPTH_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // depth
    propertyServer->registerProperty(OB_PROP_DEPTH_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_DEPTH_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_COLOR_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // color
    propertyServer->registerProperty(OB_PROP_COLOR_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_COLOR_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // left ir
    propertyServer->registerProperty(OB_PROP_IR_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_RIGHT_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // right ir
    propertyServer->registerProperty(OB_PROP_IR_RIGHT_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_RIGHT_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);

    auto sensors = getSensorTypeList();
    for(auto &sensor: sensors) {
        auto &sourcePortInfo = getSensorPortInfo(sensor);
        if(sensor == OB_SENSOR_COLOR) {
            auto uvcPropertyAccessor = std::make_shared<LazyPropertyAccessor>([this, &sourcePortInfo]() {
                auto port     = getSourcePort(sourcePortInfo);
                auto accessor = std::make_shared<UvcPropertyAccessor>(port);
                return accessor;
            });

            propertyServer->registerProperty(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_GAIN_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_SATURATION_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_WHITE_BALANCE_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_BRIGHTNESS_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_SHARPNESS_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_CONTRAST_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_HUE_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_GAMMA_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT, "rw", "rw", uvcPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT, "rw", "rw", uvcPropertyAccessor);
        }
        else if(sensor == OB_SENSOR_DEPTH) {
            auto uvcPropertyAccessor = std::make_shared<LazyPropertyAccessor>([this, &sourcePortInfo]() {
                auto port     = getSourcePort(sourcePortInfo);
                auto accessor = std::make_shared<UvcPropertyAccessor>(port);
                return accessor;
            });
            propertyServer->registerProperty(OB_PROP_DEPTH_GAIN_INT, "rw", "rw", uvcPropertyAccessor);

            auto vendorPropertyAccessor = std::make_shared<LazySuperPropertyAccessor>([this, &sourcePortInfo]() {
                auto accessor = getComponentT<IPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
                return accessor.get();
            });

            propertyServer->registerProperty(OB_PROP_DISP_SEARCH_OFFSET_INT, "rw", "rw", d2dPropertyAccessor);  // using d2d property accessor
            propertyServer->registerProperty(OB_STRUCT_DISP_OFFSET_CONFIG, "rw", "rw", vendorPropertyAccessor);

            propertyServer->registerProperty(OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_AUTO_EXPOSURE_PRIORITY_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);  // using vendor property accessor
            propertyServer->registerProperty(OB_PROP_LDP_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_CONTROL_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_ALWAYS_ON_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_ON_OFF_PATTERN_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TEMPERATURE_COMPENSATION_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LDP_STATUS_BOOL, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_ALIGN_HARDWARE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_POWER_LEVEL_CONTROL_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LDP_MEASURE_DISTANCE_INT, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TIMER_RESET_SIGNAL_BOOL, "w", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TIMER_RESET_TRIGGER_OUT_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TIMER_RESET_DELAY_US_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_SYNC_SIGNAL_TRIGGER_OUT_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_CAPTURE_IMAGE_SIGNAL_BOOL, "w", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_CAPTURE_IMAGE_FRAME_NUMBER_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_VERSION, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_TEMPERATURE, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_TIME, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_CURRENT_DEPTH_ALG_MODE, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_SERIAL_NUMBER, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_ASIC_SERIAL_NUMBER, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_MULTI_DEVICE_SYNC_CONFIG, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_DEPTH_CALIB_PARAM, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_ALIGN_CALIB_PARAM, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_D2C_ALIGN_SUPPORT_PROFILE_LIST, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEPTH_HDR_CONFIG, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_COLOR_AE_ROI, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEPTH_AE_ROI, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_IMU_CALIB_PARAM, "", "rw", vendorPropertyAccessor);

            // todo: add these properties to the frame processor
            // propertyServer->registerProperty(OB_PROP_SDK_DEPTH_FRAME_UNPACK_BOOL, "rw", "rw", vendorPropertyAccessor);

            propertyServer->registerProperty(OB_PROP_EXTERNAL_SIGNAL_RESET_BOOL, "rw", "rw", vendorPropertyAccessor);
            // propertyServer->registerProperty(OB_PROP_GPM_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_POWER_ACTUAL_LEVEL_INT, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_TIME, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_GYRO_ODR_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ACCEL_ODR_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ACCEL_SWITCH_BOOL, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_GYRO_SWITCH_BOOL, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_GYRO_FULL_SCALE_INT, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ACCEL_FULL_SCALE_INT, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_ACCEL_PRESETS_ODR_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_ACCEL_PRESETS_FULL_SCALE_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_GYRO_PRESETS_ODR_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_GYRO_PRESETS_FULL_SCALE_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_IR_BRIGHTNESS_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_DEVICE_EXTENSION_INFORMATION, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_IR_AE_MAX_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_AE_MAX_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DISP_SEARCH_RANGE_MODE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_SLAVE_DEVICE_SYNC_STATUS_BOOL, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEVICE_RESET_BOOL, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_DEPTH_ALG_MODE_LIST, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_CURRENT_DEPTH_ALG_MODE, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_STOP_IR_STREAM_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_STOP_COLOR_STREAM_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_STOP_DEPTH_STREAM_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ON_CHIP_CALIBRATION_HEALTH_CHECK_FLOAT, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ON_CHIP_CALIBRATION_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor);

            if(isGmslDevice_) {
                propertyServer->registerProperty(OB_PROP_DEVICE_REPOWER_BOOL, "w", "w", vendorPropertyAccessor);
            }
            else {
                propertyServer->registerProperty(OB_PROP_DEVICE_USB2_REPEAT_IDENTIFY_BOOL, "rw", "rw", vendorPropertyAccessor);
            }
        }
        else if(sensor == OB_SENSOR_ACCEL) {
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", sensor);
            if(imuCorrectorFilter) {
                auto filterStateProperty = std::make_shared<FilterStatePropertyAccessor>(imuCorrectorFilter);
                propertyServer->registerProperty(OB_PROP_SDK_ACCEL_FRAME_TRANSFORMED_BOOL, "rw", "rw", filterStateProperty);
            }
        }
        else if(sensor == OB_SENSOR_GYRO) {
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", sensor);
            if(imuCorrectorFilter) {
                auto filterStateProperty = std::make_shared<FilterStatePropertyAccessor>(imuCorrectorFilter);
                propertyServer->registerProperty(OB_PROP_SDK_GYRO_FRAME_TRANSFORMED_BOOL, "rw", "rw", filterStateProperty);
            }
        }
    }

    propertyServer->aliasProperty(OB_PROP_IR_AUTO_EXPOSURE_BOOL, OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL);
    propertyServer->aliasProperty(OB_PROP_IR_EXPOSURE_INT, OB_PROP_DEPTH_EXPOSURE_INT);
    propertyServer->aliasProperty(OB_PROP_IR_GAIN_INT, OB_PROP_DEPTH_GAIN_INT);

    auto heartbeatPropertyAccessor = std::make_shared<HeartbeatPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_HEARTBEAT_BOOL, "rw", "rw", heartbeatPropertyAccessor);

    auto baseLinePropertyAccessor = std::make_shared<BaselinePropertyAccessor>(this);
    propertyServer->registerProperty(OB_STRUCT_BASELINE_CALIBRATION_PARAM, "r", "r", baseLinePropertyAccessor);

    registerComponent(OB_DEV_COMPONENT_PROPERTY_SERVER, propertyServer, true);
}

std::vector<std::shared_ptr<IFilter>> G330Device::createRecommendedPostProcessingFilters(OBSensorType type) {
    auto filterFactory = FilterFactory::getInstance();
    if(type == OB_SENSOR_DEPTH) {
        // activate depth frame processor library
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, false);

        std::vector<std::shared_ptr<IFilter>> depthFilterList;

        if(filterFactory->isFilterCreatorExists("DecimationFilter")) {
            auto decimationFilter = filterFactory->createFilter("DecimationFilter");
            depthFilterList.push_back(decimationFilter);
        }

        if(filterFactory->isFilterCreatorExists("HDRMerge")) {
            auto hdrMergeFilter = filterFactory->createFilter("HDRMerge");
            depthFilterList.push_back(hdrMergeFilter);
        }

        if(filterFactory->isFilterCreatorExists("SequenceIdFilter")) {
            auto sequenceIdFilter = filterFactory->createFilter("SequenceIdFilter");
            depthFilterList.push_back(sequenceIdFilter);
        }

        if(filterFactory->isFilterCreatorExists("SpatialAdvancedFilter")) {
            auto spatFilter = filterFactory->createFilter("SpatialAdvancedFilter");
            // magnitude, alpha, disp_diff, radius
            std::vector<std::string> params = { "1", "0.5", "160", "1" };
            spatFilter->updateConfig(params);
            depthFilterList.push_back(spatFilter);
        }

        if(filterFactory->isFilterCreatorExists("TemporalFilter")) {
            auto tempFilter = filterFactory->createFilter("TemporalFilter");
            // diff_scale, weight
            std::vector<std::string> params = { "0.1", "0.4" };
            tempFilter->updateConfig(params);
            depthFilterList.push_back(tempFilter);
        }

        if(filterFactory->isFilterCreatorExists("HoleFillingFilter")) {
            auto                     hfFilter = filterFactory->createFilter("HoleFillingFilter");
            std::vector<std::string> params   = { "2" };
            hfFilter->updateConfig(params);
            depthFilterList.push_back(hfFilter);
        }

        if(filterFactory->isFilterCreatorExists("DisparityTransform")) {
            auto dtFilter = filterFactory->createFilter("DisparityTransform");
            depthFilterList.push_back(dtFilter);
        }

        if(filterFactory->isFilterCreatorExists("ThresholdFilter")) {
            auto ThresholdFilter = filterFactory->createFilter("ThresholdFilter");
            depthFilterList.push_back(ThresholdFilter);
        }

        for(size_t i = 0; i < depthFilterList.size(); i++) {
            auto filter = depthFilterList[i];
            if(filter->getName() != "DisparityTransform") {
                filter->enable(false);
            }
        }
        return depthFilterList;
    }
    else if(type == OB_SENSOR_COLOR) {
        // activate color frame processor library
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, false);

        std::vector<std::shared_ptr<IFilter>> colorFilterList;
        if(filterFactory->isFilterCreatorExists("DecimationFilter")) {
            auto decimationFilter = filterFactory->createFilter("DecimationFilter");
            decimationFilter->enable(false);
            colorFilterList.push_back(decimationFilter);
        }
        return colorFilterList;
    }
    else if(type == OB_SENSOR_IR_LEFT) {
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, false);
        std::vector<std::shared_ptr<IFilter>> leftIRFilterList;
        if(filterFactory->isFilterCreatorExists("SequenceIdFilter")) {
            auto sequenceIdFilter = filterFactory->createFilter("SequenceIdFilter");
            sequenceIdFilter->enable(false);
            leftIRFilterList.push_back(sequenceIdFilter);
            return leftIRFilterList;
        }
    }
    else if(type == OB_SENSOR_IR_RIGHT) {
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, false);
        std::vector<std::shared_ptr<IFilter>> rightIRFilterList;
        if(filterFactory->isFilterCreatorExists("SequenceIdFilter")) {
            auto sequenceIdFilter = filterFactory->createFilter("SequenceIdFilter");
            sequenceIdFilter->enable(false);
            rightIRFilterList.push_back(sequenceIdFilter);
            return rightIRFilterList;
        }
    }

    return {};
}

void G330Device::loadDefaultDepthPostProcessingConfig() {
    auto envConfig = EnvConfig::getInstance();

    try {
        std::string deviceName = utils::string::removeSpace(deviceInfo_->name_);
        std::string nodeName   = std::string("Device.") + deviceName + std::string(".DepthPostProcessing");
        if(envConfig->isNodeContained(nodeName)) {
            bool hwNoiseRmEnable = true;
            bool swNoiseRmEnable = true;

            auto propertyServer = getPropertyServer();
            if(propertyServer->isPropertySupported(OB_PROP_HW_NOISE_REMOVE_FILTER_ENABLE_BOOL, PROP_OP_READ_WRITE, PROP_ACCESS_USER)) {
                if(envConfig->getBooleanValue(nodeName + std::string(".HardwareNoiseRemoveFilter"), hwNoiseRmEnable)
                   && envConfig->getBooleanValue(nodeName + std::string(".SoftwareNoiseRemoveFilter"), swNoiseRmEnable)) {
                    propertyServer->setPropertyValueT(OB_PROP_HW_NOISE_REMOVE_FILTER_ENABLE_BOOL, hwNoiseRmEnable, PROP_ACCESS_USER);
                    propertyServer->setPropertyValueT(OB_PROP_DEPTH_SOFT_FILTER_BOOL, swNoiseRmEnable, PROP_ACCESS_USER);
                }
                else {
                    LOG_DEBUG("Getting depth post processing XML node failed");
                }
            }
        }
        else {
            LOG_DEBUG("No depth post processing config found for device");
        }
    }
    catch(libobsensor_exception &e) {
        std::string errorMsg = "Failed to load default depth post processing config: " + std::string(e.what());
        LOG_WARN(errorMsg);
    }
}

//====================================================================================================================================
//=========================================================G330NetDevice==============================================================

G330NetDevice::G330NetDevice(const std::shared_ptr<const IDeviceEnumInfo> &info) : DeviceBase(info) {
    LOG_INFO("Create {} net device.", info->getName());
    init();

    // check and start heartbeat after initialization is complete
    checkAndStartHeartbeat();
}

G330NetDevice::~G330NetDevice() noexcept {}

void G330NetDevice::init() {
    initSensorList();
    initProperties();
    fetchDeviceInfo();
    fetchExtensionInfo();
    fetchAllProfileList();

    videoFrameTimestampCalculatorCreator_ = [this]() {
        auto metadataType = OB_FRAME_METADATA_TYPE_TIMESTAMP;
        auto iter         = std::find(G330LDevPids.begin(), G330LDevPids.end(), deviceInfo_->pid_);
        if(iter == G330LDevPids.end()) {
            metadataType = OB_FRAME_METADATA_TYPE_SENSOR_TIMESTAMP;
        }
        return std::make_shared<FrameTimestampCalculatorOverMetadata>(this, metadataType, frameTimeFreq_);
    };

    auto globalTimestampFilter = std::make_shared<GlobalTimestampFitter>(this);
    registerComponent(OB_DEV_COMPONENT_GLOBAL_TIMESTAMP_FILTER, globalTimestampFilter);

    auto algParamManager = std::make_shared<G330AlgParamManager>(this);
    registerComponent(OB_DEV_COMPONENT_ALG_PARAM_MANAGER, algParamManager);

    auto depthWorkModeManager = std::make_shared<G330DepthWorkModeManager>(this);
    registerComponent(OB_DEV_COMPONENT_DEPTH_WORK_MODE_MANAGER, depthWorkModeManager);

    if(getFirmwareVersionInt() >= 10500) {
        // support custom presets upgrade
        auto propertyServer = getPropertyServer();
        propertyServer->registerAccessCallback(
            {
                OB_STRUCT_CURRENT_DEPTH_ALG_MODE,
            },
            [&](uint32_t propertyId, const uint8_t *, size_t, PropertyOperationType operationType) {
                if(operationType == PROP_OP_WRITE && propertyId == OB_STRUCT_CURRENT_DEPTH_ALG_MODE) {
                    // fetch preset version info via fetchExtensionInfo
                    fetchExtensionInfo();
                }
            });
    }

    auto presetManager = std::make_shared<G330PresetManager>(this);
    registerComponent(OB_DEV_COMPONENT_PRESET_MANAGER, presetManager);

    auto sensorStreamStrategy = std::make_shared<G330SensorStreamStrategy>(this);
    registerComponent(OB_DEV_COMPONENT_SENSOR_STREAM_STRATEGY, sensorStreamStrategy);

    static const std::vector<OBMultiDeviceSyncMode> supportedSyncModes = {
        OB_MULTI_DEVICE_SYNC_MODE_FREE_RUN,         OB_MULTI_DEVICE_SYNC_MODE_STANDALONE,          OB_MULTI_DEVICE_SYNC_MODE_PRIMARY,
        OB_MULTI_DEVICE_SYNC_MODE_SECONDARY_SYNCED, OB_MULTI_DEVICE_SYNC_MODE_SOFTWARE_TRIGGERING, OB_MULTI_DEVICE_SYNC_MODE_HARDWARE_TRIGGERING
    };
    auto deviceSyncConfigurator = std::make_shared<DeviceSyncConfigurator>(this, supportedSyncModes);
    registerComponent(OB_DEV_COMPONENT_DEVICE_SYNC_CONFIGURATOR, deviceSyncConfigurator);

    auto deviceClockSynchronizer = std::make_shared<DeviceClockSynchronizer>(this);
    registerComponent(OB_DEV_COMPONENT_DEVICE_CLOCK_SYNCHRONIZER, deviceClockSynchronizer);

    registerComponent(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY, [this]() {
        std::shared_ptr<FrameProcessorFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FrameProcessorFactory>(this); })
        return factory;
    });

    registerComponent(OB_DEV_COMPONENT_FIRMWARE_UPDATER, [this]() {
        std::shared_ptr<FirmwareUpdater> firmwareUpdater;
        TRY_EXECUTE({ firmwareUpdater = std::make_shared<FirmwareUpdater>(this); })
        return firmwareUpdater;
    });

    registerComponent(OB_DEV_COMPONENT_COLOR_FRAME_METADATA_CONTAINER, [this]() {
        std::shared_ptr<FrameMetadataParserContainer> container;
        TRY_EXECUTE({ container = std::make_shared<G330ColorFrameMetadataParserContainer>(this); })
        return container;
    });

    registerComponent(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER, [this]() {
        std::shared_ptr<FrameMetadataParserContainer> container;
        TRY_EXECUTE({ container = std::make_shared<G330DepthFrameMetadataParserContainer>(this); })
        return container;
    });

    registerComponent(OB_DEV_COMPONENT_FIRMWARE_UPDATE_GUARD_FACTORY, [this]() {
        std::shared_ptr<FirmwareUpdateGuardFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FirmwareUpdateGuardFactory>(this); })
        return factory;
    });

    auto propertyServer = getPropertyServer();
    auto fwVersion      = getFirmwareVersionInt();
    if(fwVersion >= 373) {
        auto hwNoiseRemovePropertyAccessor = std::make_shared<G330HWNoiseRemovePropertyAccessor>(this);
        propertyServer->registerProperty(OB_PROP_HW_NOISE_REMOVE_FILTER_ENABLE_BOOL, "rw", "rw", hwNoiseRemovePropertyAccessor);
        propertyServer->registerProperty(OB_PROP_HW_NOISE_REMOVE_FILTER_THRESHOLD_FLOAT, "rw", "rw", hwNoiseRemovePropertyAccessor);
    }

    if(fwVersion >= 10510) {
        auto vendorPropertyAccessor = getComponentT<VendorPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
        propertyServer->registerProperty(OB_DEVICE_AUTO_CAPTURE_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor.get());
        propertyServer->registerProperty(OB_DEVICE_AUTO_CAPTURE_INTERVAL_TIME_INT, "rw", "rw", vendorPropertyAccessor.get());
    }

    if(fwVersion >= 10533) {
        auto vendorPropertyAccessor = getComponentT<VendorPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
        propertyServer->registerProperty(OB_PROP_FRAME_INTERLEAVE_CONFIG_INDEX_INT, "rw", "rw", vendorPropertyAccessor.get());
        propertyServer->registerProperty(OB_PROP_FRAME_INTERLEAVE_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor.get());
        propertyServer->registerProperty(OB_PROP_FRAME_INTERLEAVE_LASER_PATTERN_SYNC_DELAY_INT, "rw", "rw", vendorPropertyAccessor.get());
        auto frameInterleaveManager = std::make_shared<G330FrameInterleaveManager>(this);
        registerComponent(OB_DEV_COMPONENT_FRAME_INTERLEAVE_MANAGER, frameInterleaveManager);
    }

    if(fwVersion >= 10540) {
        auto vendorPropertyAccessor = getComponentT<VendorPropertyAccessor>(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR);
        propertyServer->registerProperty(OB_STRUCT_DEVICE_ERROR_STATE, "", "r", vendorPropertyAccessor.get());
    }

#if defined(__linux__) || defined(__aarch64__)
    if(getFirmwareVersionInt() >= 10533) {
        auto ptpClockSyncPropertyAccessor = std::make_shared<G330NetPTPClockSyncPropertyAccessor>(this);
        propertyServer->registerProperty(OB_DEVICE_PTP_CLOCK_SYNC_ENABLE_BOOL, "rw", "rw", ptpClockSyncPropertyAccessor);
    }
#endif

    fetchDeviceErrorState();
}

void G330NetDevice::fetchDeviceInfo() {
    auto propServer                   = getPropertyServer();
    auto version                      = propServer->getStructureDataT<OBVersionInfo>(OB_STRUCT_VERSION);
    auto deviceInfo                   = std::make_shared<NetDeviceInfo>();
    auto portInfo                     = enumInfo_->getSourcePortInfoList().front();
    auto netPortInfo                  = std::dynamic_pointer_cast<const NetSourcePortInfo>(portInfo);
    deviceInfo->ipAddress_            = netPortInfo->address;
    deviceInfo_                       = deviceInfo;
    deviceInfo_->name_                = enumInfo_->getName();
    deviceInfo_->fullName_            = "Orbbec " + deviceInfo_->name_;
    deviceInfo_->fwVersion_           = version.firmwareVersion;
    deviceInfo_->deviceSn_            = version.serialNumber;
    deviceInfo_->asicName_            = version.depthChip;
    deviceInfo_->hwVersion_           = version.hardwareVersion;
    deviceInfo_->type_                = static_cast<uint16_t>(version.deviceType);
    deviceInfo_->supportedSdkVersion_ = version.sdkVersion;
    deviceInfo_->pid_                 = enumInfo_->getPid();
    deviceInfo_->vid_                 = enumInfo_->getVid();
    deviceInfo_->uid_                 = enumInfo_->getUid();
    deviceInfo_->connectionType_      = enumInfo_->getConnectionType();

    netBandwidth_ = G335LE_1000M_NET_BAND_WIDTH;
    netBandwidth_ = propServer->getPropertyValueT<int>(OB_PROP_NETWORK_BANDWIDTH_TYPE_INT);
    LOG_DEBUG("The network bandwidth read from device is {}.", netBandwidth_);

    linkSpeed_ = netBandwidth_;
#if (!defined(WIN32) && !defined(_WIN32) && !defined(WINCE))
    std::string   path = "/sys/class/net/" + netPortInfo->netInterfaceName + "/speed";
    std::ifstream file(path);
    if(file.is_open()) {
        file >> linkSpeed_;
        if(linkSpeed_ <= G335LE_10M_NET_BAND_WIDTH) {
            LOG_WARN("Link speed is {}Mb/s, Please check the ethernet connection and reconnect the device!", linkSpeed_);
        }
        else {
            LOG_DEBUG("Link speed is {}Mb/s.", linkSpeed_);
        }
    }
#endif
}

void libobsensor::G330NetDevice::fetchAllProfileList() {
    auto                 propServer = getPropertyServer();
    std::vector<uint8_t> data;
    BEGIN_TRY_EXECUTE({
        propServer->getRawData(
            OB_RAW_DATA_STREAM_PROFILE_LIST,
            [&](OBDataTranState state, OBDataChunk *dataChunk) {
                if(state == DATA_TRAN_STAT_TRANSFERRING) {
                    data.insert(data.end(), dataChunk->data, dataChunk->data + dataChunk->size);
                }
            },
            PROP_ACCESS_INTERNAL);
    })
    CATCH_EXCEPTION_AND_EXECUTE({
        LOG_ERROR("Get profile list params failed!");
        data.clear();
    })

    if(!data.empty()) {
        std::vector<OBInternalStreamProfile> outputProfiles;
        uint16_t                             dataSize = static_cast<uint16_t>(data.size());
        outputProfiles                                = parseBuffer<OBInternalStreamProfile>(data.data(), dataSize);
        allNetProfileList_.clear();
        for(const auto &item: outputProfiles) {
            OBStreamType streamType = utils::mapSensorTypeToStreamType((OBSensorType)item.sensorType);
            OBFormat     format     = utils::uvcFourccToOBFormat(item.profile.video.formatFourcc);
            allNetProfileList_.push_back(StreamProfileFactory::createVideoStreamProfile(streamType, format, item.profile.video.width, item.profile.video.height,
                                                                                        item.profile.video.fps));
        }
    }
    else {
        LOG_WARN("Get stream profile list failed!");
    }
}

void G330NetDevice::initSensorList() {
    registerComponent(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY, [this]() {
        std::shared_ptr<FrameProcessorFactory> factory;
        TRY_EXECUTE({ factory = std::make_shared<FrameProcessorFactory>(this); })
        return factory;
    });

    auto netStreamProfileFilter = std::make_shared<G330NetStreamProfileFilter>(this);
    registerComponent(OB_DEV_COMPONENT_STREAM_PROFILE_FILTER, netStreamProfileFilter);

    const auto &sourcePortInfoList = enumInfo_->getSourcePortInfoList();

    auto vendorPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(),
                                           [](const std::shared_ptr<const SourcePortInfo> &portInfo) { return portInfo->portType == SOURCE_PORT_NET_VENDOR; });
    if(vendorPortInfoIter != sourcePortInfoList.end()) {
        vendorPortInfo_ = *vendorPortInfoIter;
    }

    auto depthPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_NET_RTP && std::dynamic_pointer_cast<const RTPStreamPortInfo>(portInfo)->streamType == OB_STREAM_DEPTH;
    });
    if(depthPortInfoIter != sourcePortInfoList.end()) {
        auto depthPortInfo = *depthPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_DEPTH_SENSOR,
            [this, depthPortInfo]() {
                auto port   = getSourcePort(depthPortInfo);
                auto sensor = std::make_shared<G330NetDisparitySensor>(this, OB_SENSOR_DEPTH, port, linkSpeed_);

                sensor->enableTimestampAnomalyDetection(false);

                initSensorStreamProfileList(sensor);
                sensor->updateFormatFilterConfig({ { FormatFilterPolicy::REMOVE, OB_FORMAT_Y8, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_YV12, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REMOVE, OB_FORMAT_UYVY, OB_FORMAT_ANY, nullptr },
                                                   { FormatFilterPolicy::REPLACE, OB_FORMAT_Z16, OB_FORMAT_Y16, nullptr } });

                auto depthMdParserContainer_ = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer_.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                auto propServer = getPropertyServer();
                auto depthUnit  = propServer->getPropertyValueT<float>(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT);
                sensor->setDepthUnit(depthUnit);

                auto hwD2D = propServer->getPropertyValueT<bool>(OB_PROP_DISPARITY_TO_DEPTH_BOOL);
                sensor->markOutputDisparityFrame(!hwD2D);

                initSensorStreamProfile(sensor);
                initStreamProfileFilter(sensor);
                sensor->registerStreamStateChangedCallback([&](OBStreamState state, const std::shared_ptr<const StreamProfile> &sp) {
                    if(state == STREAM_STATE_STREAMING) {
                        auto algParamManager = getComponentT<G330AlgParamManager>(OB_DEV_COMPONENT_ALG_PARAM_MANAGER);
                        algParamManager->reFetchDisparityParams();
                        algParamManager->bindDisparityParam({ sp });
                    }
                });

                loadDefaultDepthPostProcessingConfig();
                return sensor;
            },
            true);

        registerSensorPortInfo(OB_SENSOR_DEPTH, depthPortInfo);

        registerComponent(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_DEPTH);
            return frameProcessor;
        });
    }

    auto irLeftPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_NET_RTP && std::dynamic_pointer_cast<const RTPStreamPortInfo>(portInfo)->streamType == OB_STREAM_IR_LEFT;
    });
    if(irLeftPortInfoIter != sourcePortInfoList.end()) {
        auto irLeftPortInfo = *irLeftPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_LEFT_IR_SENSOR,
            [this, irLeftPortInfo]() {
                auto port   = getSourcePort(irLeftPortInfo);
                auto sensor = std::make_shared<G330NetVideoSensor>(this, OB_SENSOR_IR_LEFT, port, linkSpeed_);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_YV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_NV12, OB_FORMAT_Y12, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FrameUnpacker", OB_SENSOR_IR_LEFT, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::REPLACE, OB_FORMAT_NV12, OB_FORMAT_Y16, formatConverter });
                }

                sensor->enableTimestampAnomalyDetection(false);

                initSensorStreamProfileList(sensor);
                sensor->updateFormatFilterConfig(formatFilterConfigs);
                auto depthMdParserContainer_ = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer_.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);
                initStreamProfileFilter(sensor);
                return sensor;
            },
            true);

        registerSensorPortInfo(OB_SENSOR_IR_LEFT, irLeftPortInfo);

        registerComponent(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_IR_LEFT);
            return frameProcessor;
        });
    }

    auto irRightPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_NET_RTP && std::dynamic_pointer_cast<const RTPStreamPortInfo>(portInfo)->streamType == OB_STREAM_IR_RIGHT;
    });
    if(irRightPortInfoIter != sourcePortInfoList.end()) {
        auto irRightPortInfo = *irRightPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_RIGHT_IR_SENSOR,
            [this, irRightPortInfo]() {
                auto port   = getSourcePort(irRightPortInfo);
                auto sensor = std::make_shared<G330NetVideoSensor>(this, OB_SENSOR_IR_RIGHT, port, linkSpeed_);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_Z16, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_BA81, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_YV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_NV12, OB_FORMAT_Y12, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FrameUnpacker", OB_SENSOR_IR_RIGHT, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::REPLACE, OB_FORMAT_YV12, OB_FORMAT_Y16, formatConverter });
                }

                sensor->enableTimestampAnomalyDetection(false);

                initSensorStreamProfileList(sensor);
                sensor->updateFormatFilterConfig(formatFilterConfigs);

                auto depthMdParserContainer_ = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_DEPTH_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(depthMdParserContainer_.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);
                initStreamProfileFilter(sensor);
                return sensor;
            },
            true);

        registerSensorPortInfo(OB_SENSOR_IR_RIGHT, irRightPortInfo);

        registerComponent(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_IR_RIGHT);
            return frameProcessor;
        });

        std::shared_ptr<const SourcePortInfo> vendorPortInfo = vendorPortInfo_;
        registerComponent(OB_DEV_COMPONENT_MAIN_PROPERTY_ACCESSOR, [this, vendorPortInfo]() {
            auto port     = getSourcePort(vendorPortInfo);
            auto accessor = std::make_shared<VendorPropertyAccessor>(this, port);
            return accessor;
        });

        registerComponent(OB_DEV_COMPONENT_DEVICE_MONITOR, [this, vendorPortInfo]() {
            auto port       = getSourcePort(vendorPortInfo);
            auto devMonitor = std::make_shared<DeviceMonitor>(this, port);
            return devMonitor;
        });
    }

    auto colorPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_NET_RTP && std::dynamic_pointer_cast<const RTPStreamPortInfo>(portInfo)->streamType == OB_STREAM_COLOR;
    });
    if(colorPortInfoIter != sourcePortInfoList.end()) {
        auto colorPortInfo = *colorPortInfoIter;
        registerComponent(
            OB_DEV_COMPONENT_COLOR_SENSOR,
            [this, colorPortInfo]() {
                auto port   = getSourcePort(colorPortInfo);
                auto sensor = std::make_shared<G330NetVideoSensor>(this, OB_SENSOR_COLOR, port, linkSpeed_);

                std::vector<FormatFilterConfig> formatFilterConfigs = {
                    { FormatFilterPolicy::REMOVE, OB_FORMAT_NV12, OB_FORMAT_ANY, nullptr },
                    { FormatFilterPolicy::REPLACE, OB_FORMAT_BYR2, OB_FORMAT_RW16, nullptr },
                };

                auto formatConverter = getSensorFrameFilter("FormatConverter", OB_SENSOR_COLOR, false);
                if(formatConverter) {
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_RGB, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_RGBA, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_BGR, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_BGRA, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_Y16, formatConverter });
                    formatFilterConfigs.push_back({ FormatFilterPolicy::ADD, OB_FORMAT_YUYV, OB_FORMAT_Y8, formatConverter });
                }

                sensor->enableTimestampAnomalyDetection(false);

                initSensorStreamProfileList(sensor);
                sensor->updateFormatFilterConfig(formatFilterConfigs);

                auto colorMdParserContainer = getComponentT<IFrameMetadataParserContainer>(OB_DEV_COMPONENT_COLOR_FRAME_METADATA_CONTAINER);
                sensor->setFrameMetadataParserContainer(colorMdParserContainer.get());

                auto frameTimestampCalculator = videoFrameTimestampCalculatorCreator_();
                sensor->setFrameTimestampCalculator(frameTimestampCalculator);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                auto frameProcessor = getComponentT<FrameProcessor>(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, false);
                if(frameProcessor) {
                    sensor->setFrameProcessor(frameProcessor.get());
                }

                initSensorStreamProfile(sensor);
                initStreamProfileFilter(sensor);
                return sensor;
            },
            true);

        registerSensorPortInfo(OB_SENSOR_COLOR, colorPortInfo);

        registerComponent(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, [this]() {
            auto factory        = getComponentT<FrameProcessorFactory>(OB_DEV_COMPONENT_FRAME_PROCESSOR_FACTORY);
            auto frameProcessor = factory->createFrameProcessor(OB_SENSOR_COLOR);
            return frameProcessor;
        });
    }

    auto imuPortInfoIter = std::find_if(sourcePortInfoList.begin(), sourcePortInfoList.end(), [](const std::shared_ptr<const SourcePortInfo> &portInfo) {
        return portInfo->portType == SOURCE_PORT_NET_RTP && std::dynamic_pointer_cast<const RTPStreamPortInfo>(portInfo)->streamType == OB_STREAM_ACCEL;
    });

    if(imuPortInfoIter != sourcePortInfoList.end()) {
        auto imuPortInfo = *imuPortInfoIter;

        registerComponent(OB_DEV_COMPONENT_IMU_STREAMER, [this, imuPortInfo]() {
            // the gyro and accel are both on the same port and share the same filter
            auto port               = getSourcePort(imuPortInfo);
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", OB_SENSOR_ACCEL, true);
            auto dataStreamPort     = std::dynamic_pointer_cast<IDataStreamPort>(port);
            auto imuStreamer        = std::make_shared<ImuStreamer>(this, dataStreamPort, imuCorrectorFilter);
            return imuStreamer;
        });

        registerComponent(
            OB_DEV_COMPONENT_ACCEL_SENSOR,
            [this, imuPortInfo]() {
                auto port                 = getSourcePort(imuPortInfo);
                auto imuStreamer          = getComponentT<ImuStreamer>(OB_DEV_COMPONENT_IMU_STREAMER);
                auto imuStreamerSharedPtr = imuStreamer.get();
                auto sensor               = std::make_shared<G330NetAccelSensor>(this, port, imuStreamerSharedPtr);
                sensor->enableTimestampAnomalyDetection(false);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_ACCEL, imuPortInfo);

        registerComponent(
            OB_DEV_COMPONENT_GYRO_SENSOR,
            [this, imuPortInfo]() {
                auto port                 = getSourcePort(imuPortInfo);
                auto imuStreamer          = getComponentT<ImuStreamer>(OB_DEV_COMPONENT_IMU_STREAMER);
                auto imuStreamerSharedPtr = imuStreamer.get();
                auto sensor               = std::make_shared<G330NetGyroSensor>(this, port, imuStreamerSharedPtr);
                sensor->enableTimestampAnomalyDetection(false);

                auto globalFrameTimestampCalculator = std::make_shared<GlobalTimestampCalculator>(this, deviceTimeFreq_, frameTimeFreq_);
                sensor->setGlobalTimestampCalculator(globalFrameTimestampCalculator);

                initSensorStreamProfile(sensor);

                return sensor;
            },
            true);
        registerSensorPortInfo(OB_SENSOR_GYRO, imuPortInfo);
    }
}

void G330NetDevice::initProperties() {
    auto propertyServer = std::make_shared<PropertyServer>(this);

    auto d2dPropertyAccessor = std::make_shared<G330Disp2DepthPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DISPARITY_TO_DEPTH_BOOL, "rw", "rw", d2dPropertyAccessor);      // hw
    propertyServer->registerProperty(OB_PROP_SDK_DISPARITY_TO_DEPTH_BOOL, "rw", "rw", d2dPropertyAccessor);  // sw
    propertyServer->registerProperty(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT, "rw", "rw", d2dPropertyAccessor);

    auto netPerformanceModePropertyAccessor = std::make_shared<G330NetPerformanceModePropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DEVICE_PERFORMANCE_MODE_INT, "rw", "rw", netPerformanceModePropertyAccessor);

    auto privatePropertyAccessor = std::make_shared<PrivateFilterPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DEPTH_SOFT_FILTER_BOOL, "rw", "rw", privatePropertyAccessor);
    propertyServer->registerProperty(OB_PROP_DEPTH_MAX_DIFF_INT, "rw", "rw", privatePropertyAccessor);
    propertyServer->registerProperty(OB_PROP_DEPTH_MAX_SPECKLE_SIZE_INT, "rw", "rw", privatePropertyAccessor);

    auto frameTransformPropertyAccessor = std::make_shared<StereoFrameTransformPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_DEPTH_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // depth
    propertyServer->registerProperty(OB_PROP_DEPTH_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_DEPTH_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_COLOR_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // color
    propertyServer->registerProperty(OB_PROP_COLOR_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_COLOR_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // left ir
    propertyServer->registerProperty(OB_PROP_IR_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_RIGHT_MIRROR_BOOL, "rw", "rw", frameTransformPropertyAccessor); // right ir
    propertyServer->registerProperty(OB_PROP_IR_RIGHT_FLIP_BOOL, "rw", "rw", frameTransformPropertyAccessor);
    propertyServer->registerProperty(OB_PROP_IR_RIGHT_ROTATE_INT, "rw", "rw", frameTransformPropertyAccessor);

    auto sensors = getSensorTypeList();
    for(auto &sensor: sensors) {
        auto &sourcePortInfo = vendorPortInfo_;
        if(sensor == OB_SENSOR_COLOR) {
            auto vendorPropertyAccessor = std::make_shared<LazySuperPropertyAccessor>([this, &sourcePortInfo]() {
                auto port     = getSourcePort(sourcePortInfo);
                auto accessor = std::make_shared<VendorPropertyAccessor>(this, port);
                return accessor;
            });

            propertyServer->registerProperty(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);  // replace by vendor property accessor
            propertyServer->registerProperty(OB_PROP_COLOR_GAIN_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_SATURATION_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_WHITE_BALANCE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_BRIGHTNESS_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_SHARPNESS_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_CONTRAST_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_HUE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_GAMMA_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_STREAM_PROFILE_LIST, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_START_COLOR_STREAM_BOOL, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_COLOR_STREAM_PROFILE, "", "w", vendorPropertyAccessor);
        }
        else if(sensor == OB_SENSOR_DEPTH) {
            auto vendorPropertyAccessor = std::make_shared<LazySuperPropertyAccessor>([this, &sourcePortInfo]() {
                auto port     = getSourcePort(sourcePortInfo);
                auto accessor = std::make_shared<VendorPropertyAccessor>(this, port);
                return accessor;
            });

            propertyServer->registerProperty(OB_PROP_DISP_SEARCH_OFFSET_INT, "rw", "rw", d2dPropertyAccessor);  // using d2d property accessor
            propertyServer->registerProperty(OB_STRUCT_DISP_OFFSET_CONFIG, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_GAIN_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_AUTO_EXPOSURE_PRIORITY_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LDP_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_CONTROL_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_ALWAYS_ON_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_ON_OFF_PATTERN_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TEMPERATURE_COMPENSATION_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LDP_STATUS_BOOL, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEPTH_ALIGN_HARDWARE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_POWER_LEVEL_CONTROL_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LDP_MEASURE_DISTANCE_INT, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TIMER_RESET_SIGNAL_BOOL, "w", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TIMER_RESET_TRIGGER_OUT_ENABLE_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_TIMER_RESET_DELAY_US_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_SYNC_SIGNAL_TRIGGER_OUT_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_CAPTURE_IMAGE_SIGNAL_BOOL, "w", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_CAPTURE_IMAGE_FRAME_NUMBER_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_VERSION, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_TEMPERATURE, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_TIME, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_CURRENT_DEPTH_ALG_MODE, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_SERIAL_NUMBER, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_ASIC_SERIAL_NUMBER, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_MULTI_DEVICE_SYNC_CONFIG, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_DEPTH_CALIB_PARAM, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_ALIGN_CALIB_PARAM, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_D2C_ALIGN_SUPPORT_PROFILE_LIST, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEPTH_HDR_CONFIG, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_COLOR_AE_ROI, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEPTH_AE_ROI, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_IMU_CALIB_PARAM, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_START_DEPTH_STREAM_BOOL, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEPTH_STREAM_PROFILE, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_START_IR_STREAM_BOOL, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_IR_STREAM_PROFILE, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_START_IR_RIGHT_STREAM_BOOL, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_IR_RIGHT_STREAM_PROFILE, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_IP_ADDR_CONFIG, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_NETWORK_BANDWIDTH_TYPE_INT, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_EXTERNAL_SIGNAL_RESET_BOOL, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_LASER_POWER_ACTUAL_LEVEL_INT, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_DEVICE_TIME, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_GYRO_ODR_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ACCEL_ODR_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ACCEL_SWITCH_BOOL, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_IMU_STREAM_PORT_INT, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_GYRO_SWITCH_BOOL, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_GYRO_FULL_SCALE_INT, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_ACCEL_FULL_SCALE_INT, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_ACCEL_PRESETS_ODR_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_ACCEL_PRESETS_FULL_SCALE_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_GYRO_PRESETS_ODR_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_GET_GYRO_PRESETS_FULL_SCALE_LIST, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_IR_BRIGHTNESS_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_DEVICE_EXTENSION_INFORMATION, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_IR_AE_MAX_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_COLOR_AE_MAX_EXPOSURE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DISP_SEARCH_RANGE_MODE_INT, "rw", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_SLAVE_DEVICE_SYNC_STATUS_BOOL, "r", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_PROP_DEVICE_RESET_BOOL, "", "w", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_DEPTH_ALG_MODE_LIST, "", "r", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_STRUCT_CURRENT_DEPTH_ALG_MODE, "", "rw", vendorPropertyAccessor);
            propertyServer->registerProperty(OB_RAW_DATA_STREAM_PROFILE_LIST, "", "r", vendorPropertyAccessor);
        }
        else if(sensor == OB_SENSOR_ACCEL) {
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", sensor);
            if(imuCorrectorFilter) {
                auto filterStateProperty = std::make_shared<FilterStatePropertyAccessor>(imuCorrectorFilter);
                propertyServer->registerProperty(OB_PROP_SDK_ACCEL_FRAME_TRANSFORMED_BOOL, "rw", "rw", filterStateProperty);
            }
        }
        else if(sensor == OB_SENSOR_GYRO) {
            auto imuCorrectorFilter = getSensorFrameFilter("IMUCorrector", sensor);
            if(imuCorrectorFilter) {
                auto filterStateProperty = std::make_shared<FilterStatePropertyAccessor>(imuCorrectorFilter);
                propertyServer->registerProperty(OB_PROP_SDK_GYRO_FRAME_TRANSFORMED_BOOL, "rw", "rw", filterStateProperty);
            }
        }
    }

    propertyServer->aliasProperty(OB_PROP_IR_AUTO_EXPOSURE_BOOL, OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL);
    propertyServer->aliasProperty(OB_PROP_IR_EXPOSURE_INT, OB_PROP_DEPTH_EXPOSURE_INT);
    propertyServer->aliasProperty(OB_PROP_IR_GAIN_INT, OB_PROP_DEPTH_GAIN_INT);

    auto heartbeatPropertyAccessor = std::make_shared<HeartbeatPropertyAccessor>(this);
    propertyServer->registerProperty(OB_PROP_HEARTBEAT_BOOL, "rw", "rw", heartbeatPropertyAccessor);

    auto baseLinePropertyAccessor = std::make_shared<BaselinePropertyAccessor>(this);
    propertyServer->registerProperty(OB_STRUCT_BASELINE_CALIBRATION_PARAM, "r", "r", baseLinePropertyAccessor);

    registerComponent(OB_DEV_COMPONENT_PROPERTY_SERVER, propertyServer, true);
}

std::vector<std::shared_ptr<IFilter>> G330NetDevice::createRecommendedPostProcessingFilters(OBSensorType type) {
    auto filterFactory = FilterFactory::getInstance();
    if(type == OB_SENSOR_DEPTH) {
        // activate depth frame processor library
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_DEPTH_FRAME_PROCESSOR, false);

        std::vector<std::shared_ptr<IFilter>> depthFilterList;

        if(filterFactory->isFilterCreatorExists("DecimationFilter")) {
            auto decimationFilter = filterFactory->createFilter("DecimationFilter");
            depthFilterList.push_back(decimationFilter);
        }

        if(filterFactory->isFilterCreatorExists("HDRMerge")) {
            auto hdrMergeFilter = filterFactory->createFilter("HDRMerge");
            depthFilterList.push_back(hdrMergeFilter);
        }

        if(filterFactory->isFilterCreatorExists("SequenceIdFilter")) {
            auto sequenceIdFilter = filterFactory->createFilter("SequenceIdFilter");
            depthFilterList.push_back(sequenceIdFilter);
        }

        if(filterFactory->isFilterCreatorExists("SpatialAdvancedFilter")) {
            auto spatFilter = filterFactory->createFilter("SpatialAdvancedFilter");
            // magnitude, alpha, disp_diff, radius
            std::vector<std::string> params = { "1", "0.5", "160", "1" };
            spatFilter->updateConfig(params);
            depthFilterList.push_back(spatFilter);
        }

        if(filterFactory->isFilterCreatorExists("TemporalFilter")) {
            auto tempFilter = filterFactory->createFilter("TemporalFilter");
            // diff_scale, weight
            std::vector<std::string> params = { "0.1", "0.4" };
            tempFilter->updateConfig(params);
            depthFilterList.push_back(tempFilter);
        }

        if(filterFactory->isFilterCreatorExists("HoleFillingFilter")) {
            auto                     hfFilter = filterFactory->createFilter("HoleFillingFilter");
            std::vector<std::string> params   = { "2" };
            hfFilter->updateConfig(params);
            depthFilterList.push_back(hfFilter);
        }

        if(filterFactory->isFilterCreatorExists("DisparityTransform")) {
            auto dtFilter = filterFactory->createFilter("DisparityTransform");
            depthFilterList.push_back(dtFilter);
        }

        if(filterFactory->isFilterCreatorExists("ThresholdFilter")) {
            auto ThresholdFilter = filterFactory->createFilter("ThresholdFilter");
            depthFilterList.push_back(ThresholdFilter);
        }

        for(size_t i = 0; i < depthFilterList.size(); i++) {
            auto filter = depthFilterList[i];
            if(filter->getName() != "DisparityTransform") {
                filter->enable(false);
            }
        }
        return depthFilterList;
    }
    else if(type == OB_SENSOR_COLOR) {
        // activate color frame processor library
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_COLOR_FRAME_PROCESSOR, false);

        std::vector<std::shared_ptr<IFilter>> colorFilterList;
        if(filterFactory->isFilterCreatorExists("DecimationFilter")) {
            auto decimationFilter = filterFactory->createFilter("DecimationFilter");
            decimationFilter->enable(false);
            colorFilterList.push_back(decimationFilter);
        }
        return colorFilterList;
    }
    else if(type == OB_SENSOR_IR_LEFT) {
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_LEFT_IR_FRAME_PROCESSOR, false);
        std::vector<std::shared_ptr<IFilter>> leftIRFilterList;
        if(filterFactory->isFilterCreatorExists("SequenceIdFilter")) {
            auto sequenceIdFilter = filterFactory->createFilter("SequenceIdFilter");
            sequenceIdFilter->enable(false);
            leftIRFilterList.push_back(sequenceIdFilter);
            return leftIRFilterList;
        }
    }
    else if(type == OB_SENSOR_IR_RIGHT) {
        getComponentT<FrameProcessor>(OB_DEV_COMPONENT_RIGHT_IR_FRAME_PROCESSOR, false);
        std::vector<std::shared_ptr<IFilter>> rightIRFilterList;
        if(filterFactory->isFilterCreatorExists("SequenceIdFilter")) {
            auto sequenceIdFilter = filterFactory->createFilter("SequenceIdFilter");
            sequenceIdFilter->enable(false);
            rightIRFilterList.push_back(sequenceIdFilter);
            return rightIRFilterList;
        }
    }

    return {};
}

void libobsensor::G330NetDevice::initSensorStreamProfileList(std::shared_ptr<ISensor> sensor) {
    auto              sensorType = sensor->getSensorType();
    OBStreamType      streamType = utils::mapSensorTypeToStreamType(sensorType);
    StreamProfileList profileList;
    for(const auto &profile: allNetProfileList_) {
        if(streamType == profile->getType()) {
            profileList.push_back(profile);
        }
    }

    if(profileList.size() != 0) {
        sensor->setStreamProfileList(profileList);
    }
}

std::shared_ptr<const StreamProfile> G330NetDevice::loadDefaultStreamProfile(OBSensorType sensorType) {
    std::shared_ptr<const StreamProfile> defaultStreamProfile = nullptr;

    OBStreamType defStreamType = OB_STREAM_UNKNOWN;
    int          defFps        = 10;
    int          defWidth      = 640;
    int          defHeight     = 400;
    OBFormat     defFormat     = OB_FORMAT_Y16;

    // USB2.0 default resolution config
    if(netBandwidth_ == 100) {
        LOG_DEBUG("loadDefaultStreamProfile set USB2.1 device default stream profile.");
        switch(sensorType) {
        case OB_SENSOR_DEPTH:
            defFormat     = OB_FORMAT_Y16;
            defStreamType = OB_STREAM_DEPTH;
            break;
        case OB_SENSOR_IR_LEFT:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR_LEFT;
            break;
        case OB_SENSOR_IR_RIGHT:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR_RIGHT;
            break;
        case OB_SENSOR_IR:
            defFormat     = OB_FORMAT_Y8;
            defStreamType = OB_STREAM_IR;
            break;
        case OB_SENSOR_COLOR: {
            defFormat     = OB_FORMAT_MJPG;
            defStreamType = OB_STREAM_COLOR;

        } break;
        default:
            break;
        }
    }

    if(defStreamType != OB_STREAM_UNKNOWN) {
        defaultStreamProfile = StreamProfileFactory::createVideoStreamProfile(defStreamType, defFormat, defWidth, defHeight, defFps);
        LOG_DEBUG("default profile StreamType:{}, Format:{}, Width:{}, Height:{}, Fps:{}", defStreamType, defFormat, defWidth, defHeight, defFps);
    }

    if(!defaultStreamProfile) {
        // load default stream profile from env config
        defaultStreamProfile = StreamProfileFactory::getDefaultStreamProfileFromEnvConfig(deviceInfo_->name_, sensorType);
    }

    return defaultStreamProfile;
}

void G330NetDevice::loadDefaultDepthPostProcessingConfig() {
    auto envConfig = EnvConfig::getInstance();

    try {
        std::string deviceName = utils::string::removeSpace(deviceInfo_->name_);
        std::string nodeName   = std::string("Device.") + deviceName + std::string(".DepthPostProcessing");
        if(envConfig->isNodeContained(nodeName)) {
            bool hwNoiseRmEnable = true;
            bool swNoiseRmEnable = true;

            auto propertyServer = getPropertyServer();
            if(propertyServer->isPropertySupported(OB_PROP_HW_NOISE_REMOVE_FILTER_ENABLE_BOOL, PROP_OP_READ_WRITE, PROP_ACCESS_USER)) {
                if(envConfig->getBooleanValue(nodeName + std::string(".HardwareNoiseRemoveFilter"), hwNoiseRmEnable)
                   && envConfig->getBooleanValue(nodeName + std::string(".SoftwareNoiseRemoveFilter"), swNoiseRmEnable)) {
                    propertyServer->setPropertyValueT(OB_PROP_HW_NOISE_REMOVE_FILTER_ENABLE_BOOL, hwNoiseRmEnable, PROP_ACCESS_USER);
                    propertyServer->setPropertyValueT(OB_PROP_DEPTH_SOFT_FILTER_BOOL, swNoiseRmEnable, PROP_ACCESS_USER);
                }
                else {
                    LOG_DEBUG("Getting depth post processing XML node failed");
                }
            }
        }
        else {
            LOG_DEBUG("No depth post processing config found for device");
        }
    }
    catch(libobsensor_exception &e) {
        std::string errorMsg = "Failed to load default depth post processing config: " + std::string(e.what());
        LOG_WARN(errorMsg);
    }
}

void G330NetDevice::initSensorStreamProfile(std::shared_ptr<ISensor> sensor) {
    auto streamProfile = loadDefaultStreamProfile(sensor->getSensorType());
    if(streamProfile) {
        sensor->updateDefaultStreamProfile(streamProfile);
    }

    // bind params: extrinsics, intrinsics, etc.
    auto profiles = sensor->getStreamProfileList();
    {
        auto algParamManager = getComponentT<G330AlgParamManager>(OB_DEV_COMPONENT_ALG_PARAM_MANAGER);
        algParamManager->bindStreamProfileParams(profiles);
    }

    auto sensorType = sensor->getSensorType();
    LOG_INFO("Sensor {} created! Found {} stream profiles.", sensorType, profiles.size());
    for(auto &profile: profiles) {
        LOG_INFO(" - {}", profile);
    }
}

void G330NetDevice::initStreamProfileFilter(std::shared_ptr<ISensor> sensor) {
    auto                    propServer      = getPropertyServer();
    OBCameraPerformanceMode performanceMode = ADAPTIVE_PERFORMANCE_MODE;
    BEGIN_TRY_EXECUTE({
        auto mode       = propServer->getPropertyValueT<int>(OB_PROP_DEVICE_PERFORMANCE_MODE_INT);
        performanceMode = (OBCameraPerformanceMode)mode;
    })
    CATCH_EXCEPTION_AND_EXECUTE({
        LOG_ERROR("Get camera performance mode failed!");
        performanceMode = ADAPTIVE_PERFORMANCE_MODE;
    })
    auto streamProfileFilter = getComponentT<G330NetStreamProfileFilter>(OB_DEV_COMPONENT_STREAM_PROFILE_FILTER);
    streamProfileFilter->switchFilterMode((OBCameraPerformanceMode)performanceMode);
    sensor->setStreamProfileFilter(streamProfileFilter.get());
}

}  // namespace libobsensor
