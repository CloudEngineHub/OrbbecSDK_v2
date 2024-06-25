#pragma once
#include "IDevice.hpp"
#include "IPresetManager.hpp"
#include "InternalTypes.hpp"
#include "G330DepthAlgModeManager.hpp"

#include <map>
#include <string>
#include <vector>

namespace Json {
class Value;  // forward declaration
}  // namespace Json

namespace libobsensor {

struct G330Preset {
    // device/global
    std::string depthAlgMode;
    int         laserState;
    int         laserPowerLevel;

    // depth and infrared sensor
    int depthAutoExposure;
    int depthExposureTime;
    int depthGain;
    int depthBrightness;
    //    // stream profile
    //    int depthStreamFormat;
    //    int depthStreamFrameRate;
    //    int depthStreamWidth;
    //    int depthStreamHeight;
    //    int irStreamFormat;  // infrared stream frame rate and resolution are same as depth stream

    // color sensor
    int colorAutoExposure;
    int colorExposureTime;
    int colorGain;
    int colorAutoWhiteBalance;
    int colorWhiteBalance;
    int colorHue;
    int colorSaturation;
    int colorContrast;
    int colorBrightness;
    int colorSharpness;
    int colorGamma;
    int colorBacklightCompensation;
    int colorPowerLineFrequency;
    //    // stream profile
    //    int colorStreamFormat;
    //    int colorStreamFrameRate;
    //    int colorStreamWidth;
    //    int colorStreamHeight;
};

class G330PresetManager : public IPresetManager {
public:
    G330PresetManager(DeviceResourceGetter<IPropertyAccessor> &propertyAccessorGetter, std::shared_ptr<G330DepthAlgModeManager> depthAlgModeManager);
    ~G330PresetManager() override = default;

    void                            loadPreset(const std::string &presetName) override;
    const std::string              &getCurrentPresetName() const override;
    const std::vector<std::string> &getAvailablePresetList() const override;
    void                            loadPresetFromJsonData(const std::string &presetName, const std::vector<uint8_t> &jsonData) override;
    void                            loadPresetFromJsonFile(const std::string &filePath) override;
    const std::vector<uint8_t>     &exportSettingsAsPresetJsonData(const std::string &presetName) override;
    void                            exportSettingsAsPresetJsonFile(const std::string &filePath) override;

private:
    void        loadCustomPreset(const std::string &presetName, const G330Preset &preset);
    void        storeCustomPreset(std::string presetName);
    void        restoreCustomPreset(std::string presetName);
    void        loadPresetFromJsonValue(const std::string &presetName, const Json::Value &root);
    Json::Value exportSettingsAsPresetJsonValue(const std::string &presetName);

private:
    DeviceResourceGetter<IPropertyAccessor>  propertyAccessorGetter_;
    std::shared_ptr<G330DepthAlgModeManager> depthAlgModeManager_;

    std::vector<std::string> availablePresets_;
    std::string              currentPreset_;
    std::vector<uint8_t>     tmpJsonData_;

    std::map<std::string, G330Preset> customPresets_;
};

}  // namespace libobsensor