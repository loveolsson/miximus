// dvp_test.cpp — minimal DVP init diagnostic
// Build: g++ dvp_test.cpp -o dvp_test -lGL -lX11 -lXext \
//        /path/to/libdvp.a -L/usr/lib/x86_64-linux-gnu
// Run:   DISPLAY=:0 ./dvp_test

#include "dvp170_linux/x64/DVPAPI.h"
#include "dvp170_linux/x64/dvpapi_gl.h"

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

static int x_error_handler(Display*, XErrorEvent* e)
{
    char buf[256];
    XGetErrorText(e->display, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "[X Error] code=%d (%s) request=%d minor=%d\n", e->error_code, buf, e->request_code, e->minor_code);
    return 0;
}

int main()
{
    XSetErrorHandler(x_error_handler);

    // --- 1. Open X display ---
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "FAIL: XOpenDisplay\n");
        return 1;
    }
    printf("OK: XOpenDisplay %s\n", XDisplayString(dpy));

    // --- 2. Create minimal GLX window ---
    int          screen    = DefaultScreen(dpy);
    int          attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, None};
    XVisualInfo* vi        = glXChooseVisual(dpy, screen, attribs);
    if (!vi) {
        fprintf(stderr, "FAIL: glXChooseVisual\n");
        return 1;
    }
    printf("OK: glXChooseVisual visual=%ld\n", vi->visualid);

    Window               root = RootWindow(dpy, vi->screen);
    XSetWindowAttributes swa{};
    swa.colormap   = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.event_mask = ExposureMask;
    Window win =
        XCreateWindow(dpy, root, 0, 0, 64, 64, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);

    GLXContext ctx = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
    if (!ctx) {
        fprintf(stderr, "FAIL: glXCreateContext\n");
        return 1;
    }
    printf("OK: glXCreateContext\n");

    if (!glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "FAIL: glXMakeCurrent\n");
        return 1;
    }
    printf("OK: glXMakeCurrent\n");

    printf("Renderer: %s\n", (const char*)glGetString(GL_RENDERER));
    printf("GLX version: %s\n", (const char*)glGetString(GL_VERSION));

    // --- 3. Check for NVIDIA DVP extension ---
    const char* exts = glXQueryExtensionsString(dpy, screen);
    printf("Has NV_gpu_affinity: %s\n", strstr(exts, "NV_gpu_affinity") ? "YES" : "NO");
    printf("Has NV_copy_image:   %s\n", strstr(exts, "NV_copy_image") ? "YES" : "NO");
    printf("Has NV_present_video:%s\n", strstr(exts, "NV_present_video") ? "YES" : "NO");

    // --- 4. Call dvpGetRequiredConstantsGLCtx BEFORE init (to probe) ---
    uint32_t  ba, bga, sa, ss, spo, sps;
    DVPStatus s = dvpGetRequiredConstantsGLCtx(&ba, &bga, &sa, &ss, &spo, &sps);
    printf("dvpGetRequiredConstantsGLCtx status=%d (0=OK)\n", (int)s);

    // --- 5. Call dvpInitGLContext with SHARE flag ---
    s = dvpInitGLContext(DVP_DEVICE_FLAGS_SHARE_APP_CONTEXT);
    printf("dvpInitGLContext(SHARE) status=%d (0=OK)\n", (int)s);
    if (s != DVP_STATUS_OK)
        dvpCloseGLContext();

    // --- 6. Try with NO flags ---
    s = dvpInitGLContext(0);
    printf("dvpInitGLContext(0)    status=%d (0=OK)\n", (int)s);
    if (s == DVP_STATUS_OK) {
        dvpGetRequiredConstantsGLCtx(&ba, &bga, &sa, &ss, &spo, &sps);
        printf("  buf_align=%u  sem_align=%u  sem_alloc=%u\n", ba, sa, ss);
        dvpCloseGLContext();
    }

    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}
