#include <libobsensor/ObSensor.hpp>

#include "utils_opencv.hpp"

int main(void) try {

    // Create a pipeline with default device.
    ob::Pipeline pipe;

    // By creating config to configure which streams to enable or disable for the pipeline, here the depth stream will be enabled.
    std::shared_ptr<ob::Config> config = std::make_shared<ob::Config>();

    // Enable depth stream.
    config->enableVideoStream(OB_STREAM_DEPTH);

    // Start the pipeline with config.
    pipe.start(config);

    // Create a window for rendering, and set the resolution of the window.
    Window app("DepthViewer", 1280, 720);

    while(app) {
        // Wait for up to 100ms for a frameset in blocking mode.
        auto frameSet = pipe.waitForFrameset(100);
        if(frameSet == nullptr) {
            continue;
        }

        // Get the depth frame from frameset.
        auto frame = frameSet->getFrame(OB_FRAME_DEPTH);
        if(!frame) {
            continue;
        }

        auto depthFrame = frame->as<ob::DepthFrame>();
        // for Y16 format depth frame, print the distance of the center pixel every 30 frames.
        if(depthFrame->getIndex() % 30 == 0 && depthFrame->getFormat() == OB_FORMAT_Y16) {
            uint32_t        width  = depthFrame->getWidth();
            uint32_t        height = depthFrame->getHeight();
            float           scale  = depthFrame->getValueScale();
            const uint16_t *data   = reinterpret_cast<const uint16_t *>(depthFrame->getData());

            // pixel value multiplied by scale is the actual distance value in millimeters.
            float centerDistance = data[width * height / 2 + width / 2] * scale;

            // attention: if the distance is 0, it means that the depth camera cannot detect the object (may be out of detection range).
            std::cout << "Facing an object " << centerDistance << " mm away. " << std::endl;
        }

        // Render frame in the window
        app.addToRender(depthFrame);
    }

    // Stop the pipeline
    pipe.stop();

    return 0;
}
catch(ob::Error &e) {
    std::cerr << "function:" << e.getFunction() << "\nargs:" << e.getArgs() << "\nmessage:" << e.what() << "\ntype:" << e.getExceptionType() << std::endl;
    exit(EXIT_FAILURE);
}