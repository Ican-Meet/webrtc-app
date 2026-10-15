#include "WebRTC.h"
#include "WebRTCSignalingHandler.h"

void WebRTC::start() {
    std::thread ws_thread([]() {
        uWS::App app;

        // Instantiate signaling handler routes
        WebRTCSignalingHandler signalingHandler;
        signalingHandler.initRoutes(app);

        app.listen(9001, [](auto *listenSocket) {
            if (listenSocket) {
                spdlog::info("WebRTC signaling server listening on port 9001...");
            } else {
                spdlog::error("Failed to setup listener on port 9001!");
            }
        }).run();
    });

    ws_thread.detach();
}