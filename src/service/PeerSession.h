//
// Created by ican on 10/10/26.
//

#ifndef WEBRTC_APP_PEERSESSION_H
#define WEBRTC_APP_PEERSESSION_H

#include "Room.h"
#include "ISignalingSink.h"
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <gst/gst.h>

using namespace std;

class PeerSession : public enable_shared_from_this<PeerSession> {
public:
    string id;
    shared_ptr<Room> room;
    GstElement *webRTCBin;
    const int tileSlot;
    ISignalingSink *signaling;

    // Guards elements/requestPads, which are filled from GStreamer streaming threads
    mutex elementsMutex;
    vector<GstElement*> elements;   // per-peer elements/bins added to the room pipeline
    vector<GstPad*> requestPads;    // tee src pads and compositor/mixer sink pads

    atomic<bool> offerGenerated{false};
    atomic<int> inboundCounter{0};
    atomic<bool> closed{false};


    // Constructor
    PeerSession(string id, shared_ptr<Room> room, GstElement *webRTCBin, int tileSlot, ISignalingSink *signaling)
        : id{std::move(id)},
    room{std::move(room)},
    webRTCBin{webRTCBin ? GST_ELEMENT(gst_object_ref(webRTCBin)) : nullptr},
    tileSlot{tileSlot},
    signaling{signaling} {}

    //Destructor
    ~PeerSession() {
        cleanup();
    }

    // Delete copy/move operations (GStreamer objects shouldn't be shallow-copied,
    // and the session is always owned through a shared_ptr)
    PeerSession(const PeerSession &) = delete;
    PeerSession &operator=(const PeerSession &) = delete;

private:

    // Helper method to safely unref all GStreamer resources
    void cleanup() {
        if (webRTCBin) {
            gst_object_unref(webRTCBin);
            webRTCBin = nullptr;
        }
        for (auto* elem : elements) {
            if (elem) gst_object_unref(elem);
        }
        elements.clear();

        for (auto* pad : requestPads) {
            if (pad) gst_object_unref(pad);
        }
        requestPads.clear();
    }
};

#endif //WEBRTC_APP_PEERSESSION_H
