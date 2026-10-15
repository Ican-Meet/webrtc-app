#include <gstreamer-1.0/gst/gst.h>
#include <glib.h>
#include <cstddef>
#include "service/WebRTC.h"


int main(int argc, char *argv[]) {

    gst_init(&argc, &argv);

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);

    WebRTC webRTC;
    webRTC.start();

    g_main_loop_run(loop);  // blocks; keeps the process (and the WS thread) alive

    g_main_loop_unref(loop);

}
