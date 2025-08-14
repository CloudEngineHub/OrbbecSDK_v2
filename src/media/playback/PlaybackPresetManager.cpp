// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#include "PlaybackPresetManager.hpp"
#include "property/InternalProperty.hpp"
#include "InternalTypes.hpp"
#include "exception/ObException.hpp"
#include "utils/Utils.hpp"
#include "PlaybackDepthWorkModeManager.hpp"
#include "DevicePids.hpp"
#include <json/json.h>

namespace libobsensor {

PlaybackPresetManager::PlaybackPresetManager(IDevice *owner) : DeviceComponentBase(owner) {
    auto devInfo = owner->getInfo();
    if(!devInfo || std::find(G330DevPids.begin(), G330DevPids.end(), devInfo->pid_) == G330DevPids.end()) {
        throw invalid_value_exception(utils::string::to_string() << "Unsupported device, pid: " << devInfo->pid_);
    }

    if(std::find(DaBaiADevPids.begin(), DaBaiADevPids.end(), devInfo->pid_) != DaBaiADevPids.end()) {
        isDaBaiADevice_ = true;
    }

    auto depthWorkModeManager = owner->getComponentT<PlaybackDepthWorkModeManager>(OB_DEV_COMPONENT_DEPTH_WORK_MODE_MANAGER);
    auto depthWorkModeList    = depthWorkModeManager->getDepthWorkModeList();

    for(auto &mode: depthWorkModeList) {
        availablePresets_.emplace_back(mode.name);
    }

    if (!availablePresets_.empty()) {
        currentPreset_ = availablePresets_[0];
    }
}

void PlaybackPresetManager::loadPreset(const std::string &presetName) {
    LOG_DEBUG("Playback Device: unsupported loadPreset() called with name: {}", presetName);
}

const std::string &PlaybackPresetManager::getCurrentPresetName() const {
    return currentPreset_;
}

const std::vector<std::string> &PlaybackPresetManager::getAvailablePresetList() const {
    return availablePresets_;
}

void PlaybackPresetManager::loadPresetFromJsonData(const std::string &presetName, const std::vector<uint8_t> &jsonData) {
    (void)jsonData;
    LOG_DEBUG("Playback Device: unsupported loadPresetFromJsonData() called with name: {}", presetName);
}

void PlaybackPresetManager::loadPresetFromJsonFile(const std::string &filePath) {
    LOG_DEBUG("Playback Device: unsupported loadPresetFromJsonFile() called with file: {}", filePath);
}

Json::Value PlaybackPresetManager::exportSettingsAsPresetJsonValueDaBaiA() {
    auto        owner = getOwner();
    std::string depthWorkMode;
    Json::Value root;

    // Depth work mode
    {
        auto depthWorkModeManager = owner->getComponentT<PlaybackDepthWorkModeManager>(OB_DEV_COMPONENT_DEPTH_WORK_MODE_MANAGER);
        depthWorkMode             = depthWorkModeManager->getCurrentDepthWorkMode().name;
    }

    auto propServer = owner->getPropertyServer();

    root["depth_alg_mode"]               = depthWorkMode;
    root["laser_state"]                  = propServer->getPropertyValueT<int>(OB_PROP_LASER_CONTROL_INT);
    root["laser_power_level"]            = propServer->getPropertyValueT<int>(OB_PROP_LASER_POWER_LEVEL_CONTROL_INT);
    root["depth_auto_exposure"]          = propServer->getPropertyValueT<bool>(OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL);
    root["depth_exposure_time"]          = propServer->getPropertyValueT<int>(OB_PROP_IR_EXPOSURE_INT);
    root["depth_gain"]                   = propServer->getPropertyValueT<int>(OB_PROP_IR_GAIN_INT);
    root["target_mean_intensity"]        = propServer->getPropertyValueT<int>(OB_PROP_IR_BRIGHTNESS_INT);
    root["color_auto_exposure"]          = propServer->getPropertyValueT<bool>(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL);
    root["color_exposure_time"]          = propServer->getPropertyValueT<int>(OB_PROP_COLOR_EXPOSURE_INT);
    root["color_gain"]                   = propServer->getPropertyValueT<int>(OB_PROP_COLOR_GAIN_INT);
    root["color_contrast"]               = propServer->getPropertyValueT<int>(OB_PROP_COLOR_CONTRAST_INT);
    root["color_saturation"]             = propServer->getPropertyValueT<int>(OB_PROP_COLOR_SATURATION_INT);
    root["color_sharpness"]              = propServer->getPropertyValueT<int>(OB_PROP_COLOR_SHARPNESS_INT);
    root["color_brightness"]             = propServer->getPropertyValueT<int>(OB_PROP_COLOR_BRIGHTNESS_INT);
    root["color_hue"]                    = propServer->getPropertyValueT<int>(OB_PROP_COLOR_HUE_INT);
    root["color_backlight_compensation"] = propServer->getPropertyValueT<bool>(OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT);
    root["color_power_line_frequency"]   = propServer->getPropertyValueT<int>(OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT);

    return root;
}

Json::Value PlaybackPresetManager::exportSettingsAsPresetJsonValueG300() {
    auto        owner = getOwner();
    std::string depthWorkMode;
    Json::Value root;

    // Depth work mode
    {
        auto depthWorkModeManager = owner->getComponentT<PlaybackDepthWorkModeManager>(OB_DEV_COMPONENT_DEPTH_WORK_MODE_MANAGER);
        depthWorkMode             = depthWorkModeManager->getCurrentDepthWorkMode().name;
    }

    auto propServer = owner->getPropertyServer();

    root["depth_alg_mode"]               = depthWorkMode;
    root["laser_state"]                  = propServer->getPropertyValueT<int>(OB_PROP_LASER_CONTROL_INT);
    root["laser_power_level"]            = propServer->getPropertyValueT<int>(OB_PROP_LASER_POWER_LEVEL_CONTROL_INT);
    root["depth_auto_exposure"]          = propServer->getPropertyValueT<bool>(OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL);
    root["depth_exposure_time"]          = propServer->getPropertyValueT<int>(OB_PROP_IR_EXPOSURE_INT);
    root["depth_gain"]                   = propServer->getPropertyValueT<int>(OB_PROP_IR_GAIN_INT);
    root["target_mean_intensity"]        = propServer->getPropertyValueT<int>(OB_PROP_IR_BRIGHTNESS_INT);
    root["color_auto_exposure"]          = propServer->getPropertyValueT<bool>(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL);
    root["color_exposure_time"]          = propServer->getPropertyValueT<int>(OB_PROP_COLOR_EXPOSURE_INT);
    root["color_auto_white_balance"]     = propServer->getPropertyValueT<bool>(OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL);
    root["color_white_balance"]          = propServer->getPropertyValueT<int>(OB_PROP_COLOR_WHITE_BALANCE_INT);
    root["color_gain"]                   = propServer->getPropertyValueT<int>(OB_PROP_COLOR_GAIN_INT);
    root["color_contrast"]               = propServer->getPropertyValueT<int>(OB_PROP_COLOR_CONTRAST_INT);
    root["color_saturation"]             = propServer->getPropertyValueT<int>(OB_PROP_COLOR_SATURATION_INT);
    root["color_sharpness"]              = propServer->getPropertyValueT<int>(OB_PROP_COLOR_SHARPNESS_INT);
    root["color_brightness"]             = propServer->getPropertyValueT<int>(OB_PROP_COLOR_BRIGHTNESS_INT);
    root["color_hue"]                    = propServer->getPropertyValueT<int>(OB_PROP_COLOR_HUE_INT);
    root["color_gamma"]                  = propServer->getPropertyValueT<int>(OB_PROP_COLOR_GAMMA_INT);
    root["color_backlight_compensation"] = propServer->getPropertyValueT<bool>(OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT);
    root["color_power_line_frequency"]   = propServer->getPropertyValueT<int>(OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT);

    return root;
}

Json::Value PlaybackPresetManager::exportSettingsAsPresetJsonValue() {
    if(isDaBaiADevice_) {
        return exportSettingsAsPresetJsonValueDaBaiA();
    }
    else {
        return exportSettingsAsPresetJsonValueG300();
    }
}

const std::vector<uint8_t> &PlaybackPresetManager::exportSettingsAsPresetJsonData(const std::string &presetName) {
    if(std::find(availablePresets_.begin(), availablePresets_.end(), presetName) == availablePresets_.end()) {
        throw invalid_value_exception(utils::string::to_string() << "Invalid preset name: " << presetName);
    }

    auto                      root = exportSettingsAsPresetJsonValue();
    Json::StreamWriterBuilder builder;
    builder.settings_["enableYAMLCompatibility"] = true;
    builder.settings_["dropNullPlaceholders"]    = true;
    std::ostringstream oss;
    builder.newStreamWriter()->write(root, &oss);
    tmpJsonData_.clear();
    auto str = oss.str();
    std::copy(str.begin(), str.end(), std::back_inserter(tmpJsonData_));
    return tmpJsonData_;
}

void PlaybackPresetManager::exportSettingsAsPresetJsonFile(const std::string &filePath) {
    auto root = exportSettingsAsPresetJsonValue();

    std::ofstream             ofs(filePath);
    Json::StreamWriterBuilder builder;
    // builder.settings_["indentation"]             = "    ";
    builder.settings_["enableYAMLCompatibility"] = true;
    builder.settings_["dropNullPlaceholders"]    = true;
    auto writer                                  = builder.newStreamWriter();
    writer->write(root, &ofs);
}

void PlaybackPresetManager::fetchPreset() {
    LOG_DEBUG("Playback Device: unsupported fetchPreset()");
}

}  // namespace libobsensor
