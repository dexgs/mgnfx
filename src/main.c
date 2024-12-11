#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/composite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include <libinput.h>

#include <linux/input-event-codes.h>

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/shm.h>


#ifndef DEFAULT_WIDTH
#define DEFAULT_WIDTH 300
#endif

#ifndef DEFAULT_HEIGHT
#define DEFAULT_HEIGHT 300
#endif

#ifndef WINDOW_TITLE
#define WINDOW_TITLE "Magnifier"
#endif
static const unsigned char WINDOW_TITLE_BYTES[] = WINDOW_TITLE;

static const int ATOM_SIZE = 32;


static int xerror_handler(Display *d, XErrorEvent *e) {
    (void) d;
    (void) e;
    return 0;
}

// libinput helper functions
static int _li_open(const char *path, int flags, void *user_data) {
    (void) user_data;
    return open(path, flags);
} 
static void _li_close(int fd, void *user_data) {
    (void) user_data;
    close(fd);
}
static const struct libinput_interface li_interface = {
    .open_restricted = _li_open,
    .close_restricted = _li_close,
};

// Print an error message to stderr, then exit with status 1
void exit_error(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

// Try to get an atom with the given name with XInternAtom and exit the program
// if it doesn't exist (if XInternAtom returns `None`)
Atom get_atom_not_none(Display *d, char *name) {
    Atom atom = XInternAtom(d, name, true);
    if (atom == None) {
        fprintf(stderr, "XInternAtom returned `None` for \"%s\"\n", name);
        exit(1);
    }
    return atom;
}

Pixmap get_root_pixmap(Display *d, Window root) {
    Atom root_pixmap = get_atom_not_none(d, "_XROOTPMAP_ID");

    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned long *prop;

    int status = XGetWindowProperty(
            d, root, root_pixmap, 0, 1, false, XA_PIXMAP, &actual_type,
            &actual_format, &nitems, &bytes_after, (unsigned char **) &prop);

    if (status != Success) exit_error("Getting root pixmap failed");

    return *prop;
}

static Bool wait_for_event_predicate(Display *d, XEvent *x_ev, XPointer arg) {
    (void) d;

    int event_type = *((int *) arg);
    return x_ev->type == event_type;
}

static int wait_for_event(Display *d, XEvent *x_ev, int event_type) {
    return XIfEvent(d, x_ev, wait_for_event_predicate, (XPointer) &event_type);
}

static int int_min(int i1, int i2) {
    return i1 < i2 ? i1 : i2;
}

static int int_max(int i1, int i2) {
    return i1 < i2 ? i2 : i1;
}

static bool get_intersection(
        int dest_window_x, int dest_window_y,
        int dest_window_width, int dest_window_height,
        int src_window_x, int src_window_y,
        int src_window_width, int src_window_height,
        int *src_x, int *src_y, int *dest_x, int *dest_y,
        int *width, int *height)
{
    int top_left_x = int_max(dest_window_x, src_window_x);
    int top_left_y = int_max(dest_window_y, src_window_y);
    int bottom_right_x = int_min(dest_window_x + dest_window_width, src_window_x + src_window_width);
    int bottom_right_y = int_min(dest_window_y + dest_window_height, src_window_y + src_window_height);

    *src_x = top_left_x - src_window_x;
    *src_y = top_left_y - src_window_y;
    
    *dest_x = top_left_x - dest_window_x;
    *dest_y = top_left_y - dest_window_y;

    *width = bottom_right_x - top_left_x;
    *height = bottom_right_y - top_left_y;

    bool intersection_is_valid = *width > 1 && *height > 1;
    return intersection_is_valid;
}

static bool get_cursor_position(
        Display *d, Window w, int *cursor_x, int *cursor_y)
{
    int dummy_int;
    unsigned int dummy_uint;
    Window dummy_window;
    bool pointer_is_on_screen = XQueryPointer(
            d, w, &dummy_window, &dummy_window,
            cursor_x, cursor_y, &dummy_int, &dummy_int, &dummy_uint);
    return pointer_is_on_screen;
}

/*
static void move_window_to_cursor(Display *d, Window w, int width, int height) {
    int cursor_x;
    int cursor_y;
    if (get_cursor_position(d, w, &cursor_x, &cursor_y)) {
        XMoveWindow(d, w, cursor_x - width / 2, cursor_y - height / 2);
    }
}
*/

static bool has_extension(Display *d, const char *name) {
    int dummy_int;
    bool has_extension = XQueryExtension(d, name, &dummy_int, &dummy_int, &dummy_int);
    if (!has_extension) {
        fprintf(stderr, "The \"%s\" extension is not available\n", name);
    }
    return has_extension;
}

static void init_extension(Display *d, const char *name) {
    XExtCodes *codes = XInitExtension(d, name);
    if (codes == NULL) {
        fprintf(stderr, "Initializing the \"%s\" extension failed\n", name);
        exit(1);
    }
}

static XImage *allocate_shm_image(
        Display *d, XShmSegmentInfo *info, Visual *v, int depth, int width, int height)
{
    XImage *img = XShmCreateImage(
            d, v, depth, XShmPixmapFormat(d), NULL, info,
            width, height);
    info->shmid = shmget(IPC_PRIVATE,
            img->bytes_per_line * img->height,
            IPC_CREAT|0777);
    info->shmaddr = img->data = shmat(info->shmid, 0, 0);
    info->readOnly = false;
    XShmAttach(d, info);
    return img;
}

int main() {
    // An int to pass as a fishing pointer to functions which will fail if
    // we pass NULL in cases where we don't care about the returned value
    int dummy_int;

    // Xlib setup
    Display *d = XOpenDisplay(NULL);
    if (d == NULL) {
        exit_error("Failed to open X display");
    }
    Window root = XDefaultRootWindow(d);
    XWindowAttributes root_attr;
    XGetWindowAttributes(d, root, &root_attr);

    // Set error handler to avoid exiting the program for non-fatal X errors
    XSetErrorHandler(xerror_handler);

    // Assert that reading the first `ATOM_SIZE` bits of an Atom will
    // not read any uninitialized memory
    assert((long unsigned int) ATOM_SIZE <= sizeof(Atom) * 8);

    // Ensure all required X extensions are available
    bool has_all_extensions = true;
    const char *required_extensions[] = {
        DAMAGE_NAME,
        SHAPENAME,
        XFIXES_NAME,
        SHMNAME,
        SHAPENAME,
        COMPOSITE_NAME,
        RENDER_NAME
    };
    int num_extensions =
        sizeof(required_extensions) / sizeof(required_extensions[0]);
    for (int i = 0; i < num_extensions; i++) {
        has_all_extensions &= has_extension(d, required_extensions[i]);
    }
    if (!has_all_extensions) {
        exit_error("A required X extension is unavailable");
    }
    // Initialize all the required extensions
    for (int i = 0; i < num_extensions; i++) {
        init_extension(d, required_extensions[i]);
    }

    // Create the window
    int attr_mask = CWOverrideRedirect | CWBackPixel;
    XSetWindowAttributes WindowAttributes = {
        .override_redirect = true,
        .background_pixel = 0x000000
    };
    int width = root_attr.width;
    int height = root_attr.height;
    Window w = XCreateWindow(
            d, root,  0, 0, width, height,
            0, CopyFromParent, CopyFromParent, CopyFromParent,
            attr_mask, &WindowAttributes);
    //move_window_to_cursor(d, w, width, height);

    Atom atom = get_atom_not_none(d, "ATOM");
    Atom string = get_atom_not_none(d, "STRING");

    // Set the 'window name' property
    Atom name = get_atom_not_none(d, "WM_NAME");
    XChangeProperty(
            d, w, name, string, 8, PropModeReplace,
            WINDOW_TITLE_BYTES, sizeof(WINDOW_TITLE_BYTES) - 1);

    // Set the EWMH 'window name' property
    Atom ewmh_name = XInternAtom(d, "_NET_WM_NAME", true);
    Atom utf8_string = XInternAtom(d, "UTF8_STRING", true);
    if (ewmh_name != None && utf8_string != None) {
        XChangeProperty(
                d, w, ewmh_name, utf8_string, 8, PropModeReplace,
                WINDOW_TITLE_BYTES, sizeof(WINDOW_TITLE_BYTES));
    }

    // Set the EWMH 'window type' property
    Atom window_type = XInternAtom(d, "_NET_WM_WINDOW_TYPE", true);
    Atom window_type_utility = XInternAtom(
            d, "_NET_WM_WINDOW_TYPE_UTILITY", true);
    if (window_type != None && window_type_utility != None) {
        XChangeProperty(
                d, w, window_type, atom, ATOM_SIZE, PropModeReplace,
                (unsigned char *) &window_type_utility, 1);
    }

    // Set EWMH 'window state' properties
    Atom ewmh_state = XInternAtom(d, "_NET_WM_STATE", true);
    if (ewmh_state != None) {
        char *state_names[] = {
            "_NET_WM_STATE_ABOVE",
            "_NET_WM_STATE_STAYS_ON_TOP",
            "_NET_WM_STATE_SKIP_TASKBAR",
            "_NET_WM_STATE_SKIP_PAGER",
            "_NET_WM_STATE_STICKY"
        };
        int num_state_names = sizeof(state_names) / sizeof(state_names[0]);

        XChangeProperty(
                d, w, ewmh_state, atom, ATOM_SIZE, PropModeReplace,
                NULL, 0);
        for (int i = 0; i < num_state_names; i++) {
            Atom state_atom = XInternAtom(d, state_names[i], true);
            if (state_atom != None) {
                XChangeProperty(
                        d, w, ewmh_state, atom, ATOM_SIZE, PropModeAppend,
                        (unsigned char *) &state_atom, 1);
            }
        }
    }

    // Make it so the magnifier window doesn't consume any input events, and
    // they get processed as if the window didn't exist.
    XRectangle rect;
    XserverRegion region = XFixesCreateRegion(d, &rect, 1);
    XFixesSetWindowShapeRegion(d, w, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(d, region);

    // Setup getting events from Xlib
    int d_fd = ConnectionNumber(d);
    // We want to know about substructure events because these tell us when new
    // windows are created, raised, fullscreened, etc. and let us keep our
    // magnifier window on top when this happens.
    XSelectInput(d, root, SubstructureNotifyMask);
    int damage_event_base;
    XDamageQueryExtension(d, &damage_event_base, &dummy_int);
    int damage_notify_event = damage_event_base + XDamageNotify;
    Damage damage = XDamageCreate(d, root, XDamageReportRawRectangles);

    // Setup getting events from libinput
    struct udev *udev = udev_new();
    struct libinput *li = libinput_udev_create_context(
            &li_interface, NULL, udev);
    libinput_udev_assign_seat(li, "seat0");
    libinput_dispatch(li);
    int li_fd = libinput_get_fd(li);

    struct pollfd pollfds[] = {
        { .fd = d_fd, .events = POLLIN },
        { .fd = li_fd, .events = POLLIN }
    };
    int num_fds = sizeof(pollfds) / sizeof(pollfds[0]);

    struct pollfd *x_pollfd = &pollfds[0];
    struct pollfd *li_pollfd = &pollfds[1];

    // Setup getting events from MIT-SHM
    int shm_event_base = XShmGetEventBase(d);
    int shm_completion_event = shm_event_base + ShmCompletion;

    // Setup SHM images
    Visual *v = DefaultVisual(d, 0);
    GC gc = DefaultGC(d, 0);

    // Image which gets copied to the main window
    XShmSegmentInfo info;
    XImage *img = allocate_shm_image(d, &info, v, DefaultDepth(d, 0), root_attr.width, root_attr.height);

    // Image into which we copy windows, needs to be 32 bit in order to do
    // alpha blending
    XShmSegmentInfo back_info;
    XImage *back_img = allocate_shm_image(d, &back_info, v, 32, root_attr.width, root_attr.height);

    // XRender setup

    // Format for windows with 32-bit colour (alpha + RGB)
    XRenderPictFormat *format_32 = XRenderFindStandardFormat(d, PictStandardARGB32);
    if (format_32 == NULL) exit_error("Finding XRender format failed for PictStandardARGB32");

    // Format for windows with 24-bit colour (RGB only)
    XRenderPictFormat *format_24 = XRenderFindStandardFormat(d, PictStandardRGB24);
    if (format_24 == NULL) exit_error("Finding XRender format failed for PictStandardRGB24");

    // Format for 1-bit colour (alpha masks)
    XRenderPictFormat *format_1 = XRenderFindStandardFormat(d, PictStandardA1);
    if (format_1 == NULL) exit_error("Finding XRender format failed for PictStandardA1");

    Pixmap dest_pixmap = XCreatePixmap(d, root, root_attr.width, root_attr.height, root_attr.depth);
    Picture dest_pic = XRenderCreatePicture(d, dest_pixmap, format_24, 0, NULL);
    if (dest_pic == NULL) exit_error("Creating destination XRender picture failed");

    Pixmap root_pixmap = get_root_pixmap(d, root);

    // Show the window
    XMapWindow(d, w);
    XFlush(d);

    bool keep_running = true;
    while (keep_running) {
        poll(pollfds, num_fds, -1);

        XRaiseWindow(d, w);
        //XFlush(d);
        //XFlush(d);
        //XSync(d, false);

        // If there are new events from libinput
        if (li_pollfd->revents & POLLIN) {
            libinput_dispatch(li);
            struct libinput_event *li_ev;
            while ((li_ev = libinput_get_event(li)) != NULL) {
                switch (libinput_event_get_type(li_ev)) {
                    case LIBINPUT_EVENT_KEYBOARD_KEY:
                        struct libinput_event_keyboard *li_ev_key =
                            libinput_event_get_keyboard_event(li_ev);
                        uint32_t keycode =
                            libinput_event_keyboard_get_key(li_ev_key);
                        if (keycode == KEY_ESC) {
                            keep_running = false;
                        }
                        break;
                    case LIBINPUT_EVENT_POINTER_AXIS:
                        struct libinput_event_pointer *li_ev_axis =
                            libinput_event_get_pointer_event(li_ev);
                        break;
                    default:
                }
                libinput_event_destroy(li_ev);
            }
        }

        bool no_damage = true;
        // TODO: manage these better
        int top_left_x = width;
        int top_left_y = height;
        int bottom_right_x = 0;
        int bottom_right_y = 0;
        // If there are new events from Xlib
        if (x_pollfd->revents & POLLIN) {
            while (XPending(d) > 0) {
                XRaiseWindow(d, w);
                XEvent x_ev;
                XNextEvent(d, &x_ev);
                if (x_ev.type == damage_notify_event) {
                    no_damage = false;
                    XDamageNotifyEvent *d_ev = (XDamageNotifyEvent *) &x_ev;
                    // area, geometry
                    top_left_x = int_min(top_left_x, d_ev->area.x);
                    top_left_y = int_min(top_left_y, d_ev->area.y);
                    bottom_right_x = int_max(bottom_right_x, d_ev->area.x + d_ev->area.width);
                    bottom_right_y = int_max(bottom_right_y, d_ev->area.y + d_ev->area.height);
                    XDamageSubtract(d, damage, None, None);
                }
            }
        }
        //XDamageSubtract(d, damage, None, None);
        //XSync(d, false);
        //XDamageSubtract(d, damage, None, None);
        XFlush(d);

        // Redraw the window contents
        if (!no_damage) {
            /*
            while (XPending(d) > 0) {
                XEvent x_ev;
                XNextEvent(d, &x_ev);
            }
            */

        XWindowAttributes dest_attr;
        XGetWindowAttributes(d, w, &dest_attr);

        // Clear img
        /*
        for (int y = top_left_y; y < bottom_right_y; y++) {
            for (int x = top_left_x; x < bottom_right_x; x++) {
                XPutPixel(img, x, y, 0);
            }
        }
        */
        // Copy wallpaper
        if (root_pixmap != None) {
            XCopyArea(d, root_pixmap, dest_pixmap, gc, 0, 0, root_attr.width, root_attr.height, 0, 0);
        }

        Window dummy_window;
        unsigned int num_windows;
        Window *windows;
        XQueryTree(d, root, &dummy_window, &dummy_window, &windows, &num_windows);
        for (unsigned int i = 0; i < num_windows; i++) {
            Window src_w = windows[i];
            if (src_w == w) continue;

            XWindowAttributes src_attr;
            Status err = XGetWindowAttributes(d, src_w, &src_attr);
            if (err == 0 || src_attr.map_state != IsViewable) continue;

            int src_x;
            int src_y;
            int dest_x;
            int dest_y;
            int intersection_width;
            int intersection_height;
            bool intersection_is_valid = get_intersection(
                    dest_attr.x, dest_attr.y, dest_attr.width, dest_attr.height,
                    src_attr.x, src_attr.y, src_attr.width, src_attr.height,
                    &src_x, &src_y, &dest_x, &dest_y,
                    &intersection_width, &intersection_height);
            if (intersection_is_valid) {
                Picture src_pic = XRenderCreatePicture(d, src_w, src_attr.depth == 24 ? format_24 : format_32, 0, NULL);
                if (src_pic == NULL) exit_error("Creating source XRender picture failed");

                Pixmap mask = XCreatePixmap(d, root, src_attr.width, src_attr.height, 1);
                GC mask_gc = XCreateGC(d, mask, 0, NULL);
                Picture mask_pic = XRenderCreatePicture(d, mask, format_1, 0, NULL);
                if (mask_pic == NULL) exit_error("Creating mask XRender picture failed");
                XSetForeground(d, mask_gc, BlackPixel(d, DefaultScreen(d)));
                XFillRectangle(d, mask, mask_gc, 0, 0, src_attr.width, src_attr.height);
                XSetForeground(d, mask_gc, WhitePixel(d, DefaultScreen(d)));
                int num_holes;
                XRectangle *holes = XShapeGetRectangles(d, src_w, ShapeBounding, &num_holes, &dummy_int);
                for (int i = 0; i < num_holes; i++) {
                    XRectangle hole = holes[i];
                    XFillRectangle(d, mask, mask_gc, hole.x, hole.y, hole.width, hole.height);
                }
                XFree(holes);
                XFreeGC(d, mask_gc);
                /*
                // Fudging image dimensions because shm images can't be
                // (nicely) resized
                //back_img->width = src_attr.width;
                back_img->width = intersection_width;
                //back_img->height = src_attr.height;
                back_img->height = intersection_height;
                unsigned char bytes_per_pixel = (back_img->bits_per_pixel + 7) / 8;
                back_img->bytes_per_line = bytes_per_pixel * back_img->width;
                XShmGetImage(d, src_w, back_img, src_x, src_y, AllPlanes);
                for (int y = 0; y < intersection_height; y++) {
                    for (int x = 0; x < intersection_width; x++) {
                        unsigned long pixel = XGetPixel(back_img, x, y);
                        unsigned char alpha = (pixel >> (bytes_per_pixel - 1) * 8) & 0xff;
                        if (alpha > 100) {
                            XPutPixel(img, dest_x + x, dest_y + y, pixel);
                            //XPutPixel(img, dest_x + x, dest_y + y, src_w / 10 + pixel);
                        }
                    }
                }
                */
                int op = src_attr.depth == 32 ? PictOpOver : PictOpSrc;
                XRenderComposite(d, op, src_pic, mask_pic, dest_pic, src_x, src_y, src_x, src_y, dest_x, dest_y, intersection_width, intersection_height);

                XRenderFreePicture(d, src_pic);
                XRenderFreePicture(d, mask_pic);
                XFreePixmap(d, mask);
            }
        }
        XFree(windows);
        //XRenderComposite(d, PictOpSrc, dest_pic, None, dest_pic, 200, 200, 0, 0, 0, 0, 200, 200);

        // paint the damaged region
        /*
        for (int y = top_left_y; y < bottom_right_y; y++) {
            for (int x = top_left_x; x < bottom_right_x; x++) {
                XPutPixel(img, x, y, XGetPixel(img, x, y) / 2);
            }
        }
        */

        // TODO: replace with proper compositing with zoom effect
        XCopyArea(d, dest_pixmap, w, gc, 0, 0, root_attr.width, root_attr.height, 0, 0);

        //XSync(d, true);
        //TODO: consider tracking prev damage and updating that as well as
        //current damage!!!
        //XShmPutImage(d, w, gc, img, 0, 0, 0, 0, width, height, true);
        //XShmPutImage(d, w, gc, img, 0, 0, 0, 0, width, height, false);
        //XFlush(d);
        //XShmPutImage(d, w, gc, img, top_left_x, top_left_y, top_left_x, top_left_y, bottom_right_x - top_left_x, bottom_right_y - top_left_y, true);
        //XShmPutImage(d, w, gc, img, top_left_x, top_left_y, top_left_x, top_left_y, bottom_right_x - top_left_x, bottom_right_y - top_left_y, false);
        //XSync(d, false);
        //XFlush(d);
        // Wait until the image is set before continuing
        // Wait until damage event
        /*
        for (;;) {
            XEvent x_ev;
            XNextEvent(d, &x_ev);
            if (x_ev.type == damage_notify_event) break;
        }
        XDamageSubtract(d, damage, None, None);
        */
        // TODO: use XIFEVENT instead!!!!
        /*
        for (;;) {
            XEvent x_ev;
            XNextEvent(d, &x_ev);
            if (x_ev.type == shm_completion_event) {
                break;
            } else {
                XPutBackEvent(d, &x_ev);
            }
        }
        */
        XEvent x_ev;
        //wait_for_event(d, &x_ev, shm_completion_event);
        // Consume one damage event to prevent infinite loop
        wait_for_event(d, &x_ev, damage_notify_event);
        }
    }

    XDamageDestroy(d, damage);
    XCloseDisplay(d);

    udev_unref(udev);
    libinput_unref(li);

    return 0;
}
