//
// Created by ican on 10/10/26.
//

#ifndef WEBRTC_APP_ROOM_H
#define WEBRTC_APP_ROOM_H

#include <string>
#include <unordered_set>
#include <set>
#include <utility>
#include <gst/gst.h>

using namespace std;

class Room {
public:
    string id;
    GstPipeline *pipeline;
    GstElement *videoCompositor;
    GstElement *videoTee;
    GstElement *audioMixer;
    GstElement *audioTee;

    unordered_set<string> peers;
    set<int> usedTileSlots;

    // Takes its own reference on every element; the caller keeps (and must release) its own.
    Room(string id, GstPipeline *pipeline, GstElement* videoCompositor, GstElement *videoTee, GstElement* audioMixer, GstElement* audioTee)
        :id{std::move(id)},
    pipeline{pipeline ? GST_PIPELINE(gst_object_ref(pipeline)) : nullptr},
    videoCompositor{videoCompositor ? GST_ELEMENT(gst_object_ref(videoCompositor)) : nullptr},
    videoTee{videoTee ? GST_ELEMENT(gst_object_ref(videoTee)) : nullptr},
    audioMixer{audioMixer ? GST_ELEMENT(gst_object_ref(audioMixer)) : nullptr},
    audioTee{audioTee ? GST_ELEMENT(gst_object_ref(audioTee)) : nullptr} {}

    // Destructor: stops the pipeline and unrefs everything to prevent memory leaks
    ~Room() {
        cleanup();
    }

    // Delete copy/move operations because GStreamer elements shouldn't be shallow-copied
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    // Direct translation of allocateTileSlot() using std::set
    int allocateTileSlot() {
        int slot = 0;
        while (usedTileSlots.count(slot) > 0) {
            slot++;
        }
        usedTileSlots.insert(slot);
        return slot;
    }

    void releaseTileSlot(int slot) {
        usedTileSlots.erase(slot);
    }

private:
    void cleanup() {
        if (pipeline) {
            gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
            GstBus *bus = gst_element_get_bus(GST_ELEMENT(pipeline));
            gst_bus_remove_watch(bus);
            gst_object_unref(bus);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (videoCompositor) {
            gst_object_unref(videoCompositor);
            videoCompositor = nullptr;
        }
        if (videoTee) {
            gst_object_unref(videoTee);
            videoTee = nullptr;
        }
        if (audioMixer) {
            gst_object_unref(audioMixer);
            audioMixer = nullptr;
        }
        if (audioTee) {
            gst_object_unref(audioTee);
            audioTee = nullptr;
        }
        peers.clear();
        usedTileSlots.clear();
    }
};

#endif //WEBRTC_APP_ROOM_H
