#include "publicFilterLoader.hpp"
#include "FramePixelValueProcess.hpp"
#include "HdrMergeProcess.hpp"
#include "SequenceIdProcess.hpp"
#include "DecimationProcess.hpp"
#include "IMUFrameReversion.hpp"
#include "FormatConverterProcess.hpp"
#include "FrameGeometricTransform.hpp"
#include "PointCloudProcess.hpp"
#include "DU08mmTo1mmConverter.hpp"
#include "FrameIMUCorrectProcess.hpp"

namespace libobsensor {
publicFilterCreator::publicFilterCreator(std::function<std::shared_ptr<IFilter>()> creatorFunc) : creatorFunc_(creatorFunc) {}

std::shared_ptr<IFilter> publicFilterCreator::create() {
    return creatorFunc_();
}


namespace PublicFilterCreatorLoader {

#define ADD_FILTER_CREATOR(name)                                                                     \
    {                                                                                                \
        #name, std::make_shared<publicFilterCreator>([]() { return std::make_shared<name>(#name); }) \
    }

std::map<std::string, std::shared_ptr<IFilterCreator>> getCreators() {
    static std::map<std::string, std::shared_ptr<IFilterCreator>> filterCreators = {
        ADD_FILTER_CREATOR(PixelValueScaler),     ADD_FILTER_CREATOR(PixelValueCutOff),
        ADD_FILTER_CREATOR(PixelValueOffset),     ADD_FILTER_CREATOR(HdrMerge),
        ADD_FILTER_CREATOR(SequenceIdFilter),     ADD_FILTER_CREATOR(DecimationFilter),
        ADD_FILTER_CREATOR(IMUFrameReversion),    ADD_FILTER_CREATOR(FormatConverter),
        ADD_FILTER_CREATOR(FrameMirror),          ADD_FILTER_CREATOR(FrameFlip),
        ADD_FILTER_CREATOR(FrameRotate),          ADD_FILTER_CREATOR(PointCloudFilter),
        ADD_FILTER_CREATOR(DU08mmTo1mmConverter), ADD_FILTER_CREATOR(IMUCorrecter),
    };

    return filterCreators;
}

}  // namespace publicFilterLoader
}  // namespace libobsensor