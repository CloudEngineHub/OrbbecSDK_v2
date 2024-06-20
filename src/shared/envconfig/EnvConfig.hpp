#pragma once
#include "xml/XmlReader.hpp"

namespace libobsensor {

class EnvConfig {

    explicit EnvConfig(const std::string &configFilePath);

    static std::mutex               instanceMutex_;
    static std::weak_ptr<EnvConfig> instanceWeakPtr_;

public:
    static std::shared_ptr<EnvConfig> getInstance(const std::string &configFilePath = "");

    ~EnvConfig() noexcept = default;

    bool getIntValue(const std::string &nodePathName, int &t);
    bool getBooleanValue(const std::string &nodePathName, bool &t);
    bool getFloatValue(const std::string &nodePathName, float &t);
    bool getDoubleValue(const std::string &nodePathName, double &t);
    bool getStringValue(const std::string &nodePathName, std::string &t);

private:
    std::vector<std::shared_ptr<XmlReader>> xmlReaders_;
};

}  // namespace libobsensor