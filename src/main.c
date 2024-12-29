#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/composite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h> 
#include <X11/extensions/shape.h>

#include <libinput.h>
#include <libevdev-1.0/libevdev/libevdev.h>

#include <linux/input-event-codes.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/file.h>

#define XSTR(s) #s
#define STR(s) XSTR(s)

#ifndef DEFAULT_WIDTH
#define DEFAULT_WIDTH 400
#endif

#ifndef DEFAULT_HEIGHT
#define DEFAULT_HEIGHT 400
#endif

#ifndef DEFAULT_WIDTH_STEP
#define DEFAULT_WIDTH_STEP 50
#endif

#ifndef DEFAULT_HEIGHT_STEP
#define DEFAULT_HEIGHT_STEP 50
#endif

#ifndef DEFAULT_ZOOM
#define DEFAULT_ZOOM 2.0
#endif

#ifndef DEFAULT_ZOOM_SCALE
#define DEFAULT_ZOOM_SCALE 0.05
#endif

#ifndef DEFAULT_ZOOM_STEP
#define DEFAULT_ZOOM_STEP 0.5
#endif

#ifndef DEFAULT_RATE
#define DEFAULT_RATE 60
#endif

#ifndef DEFAULT_QUIT_KEY
#define DEFAULT_QUIT_KEY "KEY_ESC"
#endif

#ifndef DEFAULT_GROW_WIDTH_KEY
#define DEFAULT_GROW_WIDTH_KEY "KEY_RIGHT"
#endif

#ifndef DEFAULT_SHRINK_WIDTH_KEY
#define DEFAULT_SHRINK_WIDTH_KEY "KEY_LEFT"
#endif

#ifndef DEFAULT_GROW_HEIGHT_KEY
#define DEFAULT_GROW_HEIGHT_KEY "KEY_DOWN"
#endif

#ifndef DEFAULT_SHRINK_HEIGHT_KEY
#define DEFAULT_SHRINK_HEIGHT_KEY "KEY_UP"
#endif

#ifndef DEFAULT_ZOOM_IN_KEY
#define DEFAULT_ZOOM_IN_KEY "KEY_EQUAL"
#endif

#ifndef DEFAULT_ZOOM_OUT_KEY
#define DEFAULT_ZOOM_OUT_KEY "KEY_MINUS"
#endif

#ifndef DEFAULT_MODIFIER_KEYS
#define DEFAULT_MODIFIER_KEYS "KEY_LEFTMETA", "KEY_LEFTCTRL"
#define NUM_DEFAULT_MODIFIER_KEYS 2
#endif

#ifndef MAX_SCALE
#define MAX_SCALE 10.0
#endif

#ifndef MIN_SCALE
#define MIN_SCALE 1.0
#endif

#ifndef WINDOW_TITLE
#define WINDOW_TITLE "Magnifier"
#endif
static const unsigned char WINDOW_TITLE_BYTES[] = WINDOW_TITLE;

#ifndef PIDFILE_NAME
#define PIDFILE_NAME "mgnfx.pid"
#endif
static char pidfile_name[256] = PIDFILE_NAME;

static const int ATOM_SIZE = 32;


// Print an error message to stderr, then exit with status 1
void exit_error(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

void exit_errno(const char *msg) {
    fprintf(stderr, "%s: %s.\n", msg, strerror(errno));
    exit(1);
}

void exit_error_if(bool cond, const char *msg) {
    if (cond) exit_error(msg);
}

void exit_errno_if(int cond, const char *msg) {
    if (cond == -1) exit_errno(msg);
}


struct opts {
    unsigned int width;
    unsigned int height;
    unsigned int width_step;
    unsigned int height_step;
    double zoom;
    double zoom_scale;
    double zoom_step;
    unsigned int rate;

    uint32_t quit_key;
    uint32_t grow_width_key;
    uint32_t shrink_width_key;
    uint32_t grow_height_key;
    uint32_t shrink_height_key;
    uint32_t zoom_in_key;
    uint32_t zoom_out_key;
    uint32_t modifier_keys[10];
    unsigned int num_modifier_keys;
};

static uint32_t get_key_by_name(const char *name) {
    int key = libevdev_event_code_from_name(EV_KEY, name);
    if (key < 0) {
        fprintf(stderr, "`%s` is not a valid key name\n", name);
        exit(1);
    } else {
        return key;
    }
}

static void get_opts(int argc, char **argv, struct opts *opts) {
    *opts = (struct opts) {
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT,
        .width_step = DEFAULT_WIDTH_STEP,
        .height_step = DEFAULT_HEIGHT_STEP,
        .zoom = DEFAULT_ZOOM,
        .zoom_scale = DEFAULT_ZOOM_SCALE,
        .zoom_step = DEFAULT_ZOOM_STEP,
        .rate = DEFAULT_RATE,
        .quit_key = get_key_by_name(DEFAULT_QUIT_KEY),
        .grow_width_key = get_key_by_name(DEFAULT_GROW_WIDTH_KEY),
        .shrink_width_key = get_key_by_name(DEFAULT_SHRINK_WIDTH_KEY),
        .grow_height_key = get_key_by_name(DEFAULT_GROW_HEIGHT_KEY),
        .shrink_height_key = get_key_by_name(DEFAULT_SHRINK_HEIGHT_KEY),
        .zoom_in_key = get_key_by_name(DEFAULT_ZOOM_IN_KEY),
        .zoom_out_key = get_key_by_name(DEFAULT_ZOOM_OUT_KEY)
    };

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            puts(
                    "Options:\n"
                    "--help        prints this message and exits\n"
                    "-w PIXELS     magnifier width in pixels (default " STR(DEFAULT_WIDTH) ")\n"
                    "-h PIXELS     magnifier height in pixels (default " STR(DEFAULT_HEIGHT) ")\n"
                    "-W PIXELS     width resize increment in pixels (default " STR(DEFAULT_WIDTH_STEP) ")\n"
                    "-H PIXELS     height resize increment in pixels (default " STR(DEFAULT_HEIGHT_STEP) ")\n"
                    "-s DECIMAL    zoom scale (default " STR(DEFAULT_ZOOM) ")\n"
                    "-z DECIMAL    zoom scale coefficient (default " STR(DEFAULT_ZOOM_SCALE) ")\n"
                    "-Z DECIMAL    zoom scale increment (default " STR(DEFAULT_ZOOM_STEP) ")\n"
                    "-r NUMBER     max redraws per second (default " STR(DEFAULT_RATE) ")\n"
                    "-q KEY_NAME   key binding to exit the program (default " DEFAULT_QUIT_KEY ")\n"
                    "-i KEY_NAME   key binding to increase magnifier width (default " DEFAULT_GROW_WIDTH_KEY ")\n"
                    "-I KEY_NAME   key binding to decrease magnifier width (default " DEFAULT_SHRINK_WIDTH_KEY ")\n"
                    "-e KEY_NAME   key binding to increase magnifier height (default " DEFAULT_GROW_HEIGHT_KEY ")\n"
                    "-E KEY_NAME   key binding to decrease magnifier height (default " DEFAULT_SHRINK_HEIGHT_KEY ")\n"
                    "-n KEY_NAME   key binding to zoom in (default " DEFAULT_ZOOM_IN_KEY ")\n"
                    "-o KEY_NAME   key binding to zoom out (default " DEFAULT_ZOOM_OUT_KEY ")\n"
                    "-m KEY_NAME   specify a single modifier key\n"
                    "The default modifier keys are " STR((DEFAULT_MODIFIER_KEYS)) "\n\n"
                    "Usage:\n"
"Press the quit key at any time to exit the program. While the program is\n"
"running, the region around the mouse cursor is magnified according to the\n"
"current zoom scale.\n\n"
"When all the modifier keys are held, the following actions are available:\n"
"- Resize the magnified region by clicking and dragging with the mouse\n"
"- Resize the magnified region according to the resize increments using the resize keys\n"
"- Change the zoom level by scrolling with the mouse (scaled by zoom scale coefficient)\n"
"- Change the zoom level according to the zoom scale increment using the zoom in/out keys");
            exit(1);
        }
    }

    int optchar;
    while ((optchar = getopt(argc, argv, "w:h:W:H:s:z:Z:r:q:i:I:e:E:n:o:m:")) != -1) {
        switch (optchar) {
            case 'w':
                opts->width = atoi(optarg);
                break;
            case 'h':
                opts->height = atoi(optarg);
                break;
            case 'W':
                opts->width_step = atoi(optarg);
                break;
            case 'H':
                opts->height_step = atoi(optarg);
                break;
            case 's':
                opts->zoom = strtod(optarg, NULL);
                break;
            case 'z':
                opts->zoom_scale = strtod(optarg, NULL);
                break;
            case 'Z':
                opts->zoom_step = strtod(optarg, NULL);
                break;
            case 'r':
                opts->rate = atoi(optarg);
                break;
            case 'q':
                opts->quit_key = get_key_by_name(optarg);
                break;
            case 'i':
                opts->grow_width_key = get_key_by_name(optarg);
                break;
            case 'I':
                opts->shrink_width_key = get_key_by_name(optarg);
                break;
            case 'e':
                opts->grow_height_key = get_key_by_name(optarg);
                break;
            case 'E':
                opts->shrink_height_key = get_key_by_name(optarg);
                break;
            case 'n':
                opts->zoom_in_key = get_key_by_name(optarg);
                break;
            case 'o':
                opts->zoom_out_key = get_key_by_name(optarg);
                break;
            case 'm':
                const unsigned int max_modifier_keys = sizeof(opts->modifier_keys) / sizeof(opts->modifier_keys[0]);
                if (opts->num_modifier_keys >= max_modifier_keys) {
                    exit_error("Too many modifier keys");
                } else {
                    opts->modifier_keys[opts->num_modifier_keys] = get_key_by_name(optarg);
                    opts->num_modifier_keys++;
                }
                break;
        }
    }
    if (opts->num_modifier_keys == 0) {
        opts->num_modifier_keys = NUM_DEFAULT_MODIFIER_KEYS;
        char *default_modifier_keys[] = { DEFAULT_MODIFIER_KEYS };
        for (unsigned int i = 0; i < opts->num_modifier_keys; i++) {
            opts->modifier_keys[i] = get_key_by_name(default_modifier_keys[i]);
        }
    }
}


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

Pixmap get_root_background_pixmap(Display *d, Window root) {
    Atom root_pixmap = get_atom_not_none(d, "_XROOTPMAP_ID");

    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned long *prop;

    int status = XGetWindowProperty(
            d, root, root_pixmap, 0, 1, false, XA_PIXMAP, &actual_type,
            &actual_format, &nitems, &bytes_after, (unsigned char **) &prop);

    if (status == Success && prop != NULL) {
        Pixmap root_background_pixmap = *prop;
        XFree(prop);
        return root_background_pixmap;
    } else {
        return None;
    }
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

    bool intersection_is_valid = *width > 0 && *height > 0;
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

void draw(
        int width, int height, double scale, int cursor_x, int cursor_y,
        Pixmap dest_pixmap, Pixmap final_pixmap,
        Picture dest_pic, Picture final_pic,
        XWindowAttributes root_attr, XWindowAttributes dest_attr,
        Window root, Window w, Display *d, GC gc,
        XRenderPictFormat *format_32, XRenderPictFormat *format_24, XRenderPictFormat *format_1)
{
    int dummy_int;

    // Copy wallpaper
    Pixmap root_background_pixmap = get_root_background_pixmap(d, root);
    if (root_background_pixmap != None) {
        XCopyArea(d, root_background_pixmap, dest_pixmap, gc, 0, 0, root_attr.width, root_attr.height, 0, 0);
    } else {
        XSetForeground(d, gc, BlackPixel(d, DefaultScreen(d)));
        XFillRectangle(d, dest_pixmap, gc, 0, 0, root_attr.width, root_attr.height);
    }

    Window dummy_window;

    unsigned int num_windows = 0;
    Window *windows = NULL;
    XQueryTree(d, root, &dummy_window, &dummy_window, &windows, &num_windows);

    for (unsigned int i = 0; i < num_windows; i++) {
        Window src_w = windows[i];
        if (src_w == w) continue;

        Status status;

        XWindowAttributes src_attr;
        status = XGetWindowAttributes(d, src_w, &src_attr);
        if (status == 0 || src_attr.map_state != IsViewable) continue;

        // Don't draw windows which aren't direct children of the root window
        Window parent_w = None;
        unsigned int num_child_windows;
        Window *child_windows = NULL;
        status = XQueryTree(d, src_w, &dummy_window, &parent_w, &child_windows, &num_child_windows);
        if (status == 0) continue;
        if (child_windows != NULL) XFree(child_windows);
        if (parent_w != root) continue;

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
        if (!intersection_is_valid) continue;

        Picture src_pic = XRenderCreatePicture(d, src_w, src_attr.depth == 24 ? format_24 : format_32, 0, NULL);
        if (src_pic != None) {
            Pixmap mask = None;
            Picture mask_pic = None;
            int num_rects = 0;
            XRectangle *rects = XShapeGetRectangles(d, src_w, ShapeBounding, &num_rects, &dummy_int);
            if (num_rects > 1) {
                mask = XCreatePixmap(d, root, src_attr.width, src_attr.height, 1);
                mask_pic = XRenderCreatePicture(d, mask, format_1, 0, NULL);
                GC mask_gc = XCreateGC(d, mask, 0, NULL);
                XSetForeground(d, mask_gc, BlackPixel(d, DefaultScreen(d)));
                XFillRectangle(d, mask, mask_gc, 0, 0, src_attr.width, src_attr.height);
                XSetForeground(d, mask_gc, WhitePixel(d, DefaultScreen(d)));
                for (int i = 0; i < num_rects; i++) {
                    XRectangle rect = rects[i];
                    XFillRectangle(d, mask, mask_gc, rect.x, rect.y, rect.width, rect.height);
                }
                if (rects != NULL) XFree(rects);
                XFreeGC(d, mask_gc);
            }

            int op = src_attr.depth == 32 ? PictOpOver : PictOpSrc;
            XRenderComposite(d, op, src_pic, mask_pic, dest_pic, src_x, src_y, src_x, src_y, dest_x, dest_y, intersection_width, intersection_height);

            XRenderFreePicture(d, src_pic);
            XRenderFreePicture(d, mask_pic);
            XFreePixmap(d, mask);
        }
    }
    if (windows != NULL) XFree(windows);

    XCopyArea(d, dest_pixmap, final_pixmap, gc, 0, 0, root_attr.width, root_attr.height, 0, 0);

    XFixed scale_f = XDoubleToFixed(1.0 / scale);
    XFixed one_f = XDoubleToFixed(1.0);
    XFixed zero_f = XDoubleToFixed(0.0);

    XTransform scale_transform = {{
        {scale_f, zero_f, zero_f},
            {zero_f, scale_f, zero_f},
            {zero_f, zero_f, one_f}
    }};

    XRenderSetPictureTransform(d, dest_pic, &scale_transform);

    int scaled_cursor_x = cursor_x * scale;
    int scaled_cursor_y = cursor_y * scale;
    int half_width = width / 2;
    int half_height = height / 2;

    XSetForeground(d, gc, BlackPixel(d, DefaultScreen(d)));
    XFillRectangle(d, final_pixmap, gc, cursor_x - half_width - 2, cursor_y - half_height - 2, width + 4, height + 4);

    XRenderComposite(d, PictOpSrc, dest_pic, None, final_pic, scaled_cursor_x - half_width, scaled_cursor_y - half_height, 0, 0, cursor_x - half_width, cursor_y - half_height, width, height);

    XCopyArea(d, final_pixmap, w, gc, 0, 0, root_attr.width, root_attr.height, 0, 0);
}

int main(int argc, char **argv) {
    struct opts opts;
    get_opts(argc, argv, &opts);

    char *display = getenv("DISPLAY");
    if (display == NULL) exit_error("Reading the `DISPLAY` environment variable failed");

    // Exit if another instance is already running
    strcat(pidfile_name, display);
    char *xdg_runtime_dir_path = getenv("XDG_RUNTIME_DIR");
    int xdg_runtime_dir = -1;
    if (xdg_runtime_dir_path != NULL) {
        pid_t pid = getpid();

        xdg_runtime_dir = open(xdg_runtime_dir_path, O_PATH);
        exit_errno_if(xdg_runtime_dir, "Opening XDG runtime directory failed");
        int pidfile = openat(xdg_runtime_dir, pidfile_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        exit_errno_if(pidfile, "Opening pidfile failed");

        exit_errno_if(flock(pidfile, LOCK_EX), "Locking pidfile failed");

        pid_t oldpid;
        size_t bytes_read = 0;
        ssize_t r = 1;
        while (r > 0) {
            r = read(pidfile, ((char *) &oldpid) + bytes_read, sizeof(oldpid) - bytes_read);
            exit_errno_if(r, "Reading pidfile failed");
            bytes_read += r;
        }

        if (bytes_read == sizeof(oldpid) && pid != oldpid) {
            char *exe = realpath("/proc/self/exe", NULL);
            char old_exe_path[256];
            snprintf(old_exe_path, sizeof(old_exe_path), "/proc/%u/exe", oldpid);
            old_exe_path[sizeof(old_exe_path) - 1] = 0;
            char *old_exe = realpath(old_exe_path, NULL);

            if (exe != NULL && old_exe != NULL && strcmp(exe, old_exe) == 0) {
                exit_error("Another instance is already running.");
            }
            free(exe);
            free(old_exe);
        }

        exit_errno_if(ftruncate(pidfile, 0), "Truncating pidfile failed");
        exit_errno_if(lseek(pidfile, 0, SEEK_SET), "Seeking to start of pidfile failed");
        size_t bytes_written = 0;
        ssize_t w = 1;
        while (w > 0) {
            w = write(pidfile, ((char *) &pid) + bytes_written, sizeof(pid) - bytes_written);
            exit_errno_if(w, "Writing pidfile failed");
            bytes_written += w;
        }

        exit_errno_if(flock(pidfile, LOCK_UN), "Unlocking pidfile failed");
        exit_errno_if(close(pidfile), "Closing pidfile failed");
    }

    // Setup getting events from libinput
    char *seat = getenv("XDG_SEAT");
    if (seat == NULL) exit_error("Reading `XDG_SEAT` environment variable failed");
    struct udev *udev = udev_new();
    struct libinput *li = libinput_udev_create_context(
            &li_interface, NULL, udev);
    libinput_udev_assign_seat(li, seat);
    libinput_dispatch(li);
    int li_fd = libinput_get_fd(li);

    // An int to pass as a fishing pointer to functions which will fail if
    // we pass NULL in cases where we don't care about the returned value
    int dummy_int;

    // Xlib setup
    Display *d = XOpenDisplay(display);
    if (d == NULL) {
        exit_error("Failed to open X display");
    }
    Window root = DefaultRootWindow(d);
    XWindowAttributes root_attr;
    XGetWindowAttributes(d, root, &root_attr);
    int screen = DefaultScreen(d);

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
        SHAPENAME,
        COMPOSITE_NAME,
        RENDER_NAME,
        RANDR_NAME
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

    GC gc = DefaultGC(d, screen);

    // Create the window
    int attr_mask = CWOverrideRedirect | CWBackPixel;
    XSetWindowAttributes WindowAttributes = {
        .override_redirect = true,
    };

    Window w = XCreateWindow(
            d, root,  0, 0, root_attr.width, root_attr.height,
            0, CopyFromParent, CopyFromParent, CopyFromParent,
            attr_mask, &WindowAttributes);

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
    //XSelectInput(d, root, SubstructureNotifyMask | StructureNotifyMask | ResizeRedirectMask);
    int damage_event_base;
    XDamageQueryExtension(d, &damage_event_base, &dummy_int);
    int damage_notify_event = damage_event_base + XDamageNotify;
    Damage damage = XDamageCreate(d, root, XDamageReportRawRectangles);
    int rr_event_base;
    XRRQueryExtension(d, &rr_event_base, &dummy_int);
    int screen_change_notify_event = rr_event_base + RRScreenChangeNotify;
    XRRSelectInput(d, w, RRScreenChangeNotifyMask);

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

    // `dest_pixmap` will hold the copy of the screen contents
    Pixmap dest_pixmap = XCreatePixmap(d, root, root_attr.width, root_attr.height, root_attr.depth);
    Picture dest_pic = XRenderCreatePicture(d, dest_pixmap, format_24, 0, NULL);
    if (dest_pic == None) exit_error("Creating destination XRender picture failed");

    // `final_pixmap` will hold the final image shown to the user
    Pixmap final_pixmap = XCreatePixmap(d, root, root_attr.width, root_attr.height, root_attr.depth);
    Picture final_pic = XRenderCreatePicture(d, final_pixmap, format_24, 0, NULL);
    if (final_pic == None) exit_error("Creating final XRender picture failed");

    // Setup polling
    struct pollfd pollfds[] = {
        { .fd = d_fd, .events = POLLIN },
        { .fd = li_fd, .events = POLLIN }
    };
    int num_fds = sizeof(pollfds) / sizeof(pollfds[0]);

    struct pollfd *x_pollfd = &pollfds[0];
    struct pollfd *li_pollfd = &pollfds[1];

    // Show the window
    XMapWindow(d, w);
    // Put the screen contents on the window initially
    XCopyArea(d, root, w, gc, 0, 0, root_attr.width, root_attr.height, 0, 0);

    XWindowAttributes dest_attr;
    XGetWindowAttributes(d, w, &dest_attr);

    int cursor_x = 0;
    int cursor_y = 0;
    get_cursor_position(d, root, &cursor_x, &cursor_y);
    int width = opts.width;
    int height = opts.height;
    double scale = opts.zoom;
    int rate = opts.rate;

    unsigned int modifiers_held = 0;

    draw(
            width, height, scale, cursor_x, cursor_y,
            dest_pixmap, final_pixmap,
            dest_pic, final_pic, root_attr, dest_attr, root, w, d, gc,
            format_32, format_24, format_1);
    XFlush(d);

    bool keep_running = true;
    bool input_grabbed = false;
    bool mouse_held = false;
    int click_x;
    int click_y;
    while (keep_running) {
        poll(pollfds, num_fds, -1);

        struct timespec prev_time;
        struct timespec time;

        clock_gettime(CLOCK_MONOTONIC_RAW, &prev_time);

        bool has_damage = false;
        bool has_input = false;

        bool got_cursor_position = get_cursor_position(d, root, &cursor_x, &cursor_y);

        // If there are new events from libinput
        if (li_pollfd->revents & POLLIN) {
            has_input = true;
            libinput_dispatch(li);
            struct libinput_event *li_ev;
            while ((li_ev = libinput_get_event(li)) != NULL) {
                switch (libinput_event_get_type(li_ev)) {
                    case LIBINPUT_EVENT_POINTER_MOTION:
                        if (!input_grabbed) break;
                        if (mouse_held && got_cursor_position) {
                            width = abs(cursor_x - click_x) * 2;
                            height = abs(cursor_y - click_y) * 2;
                        }
                        break;
                    case LIBINPUT_EVENT_POINTER_BUTTON:
                        if (!input_grabbed) break;
                        struct libinput_event_pointer *li_ev_pointer =
                            libinput_event_get_pointer_event(li_ev);
                        uint32_t button = libinput_event_pointer_get_button(li_ev_pointer);
                        enum libinput_button_state button_state = libinput_event_pointer_get_button_state(li_ev_pointer);
                        if (button == BTN_LEFT) {
                            switch (button_state) {
                                case LIBINPUT_BUTTON_STATE_PRESSED:
                                    if (got_cursor_position) {
                                        mouse_held = true;
                                        click_x = cursor_x;
                                        click_y = cursor_y;
                                    }
                                    break;
                                case LIBINPUT_BUTTON_STATE_RELEASED:
                                    mouse_held = false;
                                    break;
                            }
                        }
                        break;
                    case LIBINPUT_EVENT_KEYBOARD_KEY:
                        struct libinput_event_keyboard *li_ev_key =
                            libinput_event_get_keyboard_event(li_ev);
                        uint32_t keycode =
                            libinput_event_keyboard_get_key(li_ev_key);
                        enum libinput_key_state state =
                            libinput_event_keyboard_get_key_state(li_ev_key);
                        int modifier = -1;
                        for (unsigned int i = 0; i < opts.num_modifier_keys; i++) {
                            if (keycode == opts.modifier_keys[i]) {
                                modifier = i;
                                break;
                            }
                        }
                        switch (state) {
                            case LIBINPUT_KEY_STATE_PRESSED:
                                if (modifier != -1) {
                                    modifiers_held++;
                                    bool all_modifiers_held = modifiers_held == opts.num_modifier_keys;
                                    if (all_modifiers_held) {
                                        input_grabbed =
                                            XGrabPointer(d, w, true, NoEventMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess
                                            && XGrabKeyboard(d, w, true, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
                                        if (!input_grabbed) {
                                            XUngrabPointer(d, CurrentTime);
                                            XUngrabKeyboard(d, CurrentTime);
                                        }
                                    }
                                }
                                break;
                            case LIBINPUT_KEY_STATE_RELEASED:
                                if (modifier != -1) {
                                    modifiers_held = 0;
                                    XUngrabPointer(d, CurrentTime);
                                    XUngrabKeyboard(d, CurrentTime);
                                    input_grabbed = false;
                                } else if (keycode == opts.quit_key) {
                                    keep_running = false;
                                }
                                if (input_grabbed) {
                                    if (keycode == opts.grow_width_key) {
                                        width = int_min(width + opts.width_step, root_attr.width);
                                    } else if (keycode == opts.shrink_width_key) {
                                        width = int_max(width - opts.width_step, 1);
                                    } else if (keycode == opts.grow_height_key) {
                                        height = int_min(height + opts.height_step, root_attr.height);
                                    } else if (keycode == opts.shrink_height_key) {
                                        height = int_max(height - opts.height_step, 1);
                                    } else if (keycode == opts.zoom_in_key) {
                                        scale += opts.zoom_step;
                                        if (scale > MAX_SCALE) scale = MAX_SCALE;
                                    } else if (keycode == opts.zoom_out_key) {
                                        scale -= opts.zoom_step;
                                        if (scale < MIN_SCALE) scale = MIN_SCALE;
                                    }
                                }
                                break;
                        }
                        break;
                    case LIBINPUT_EVENT_POINTER_AXIS:
                        if (input_grabbed) {
                            struct libinput_event_pointer *li_ev_axis =
                                libinput_event_get_pointer_event(li_ev);
                            double scroll = libinput_event_pointer_get_axis_value(li_ev_axis, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                            scale -= scroll * opts.zoom_scale;
                            if (scale < MIN_SCALE) {
                                scale = MIN_SCALE;
                            } else if (scale > MAX_SCALE) {
                                scale = MAX_SCALE;
                            }
                        }
                        break;
                    default:
                }
                libinput_event_destroy(li_ev);
            }
        }

        // If there are new events from Xlib
        if (x_pollfd->revents & POLLIN) {
            //bool more = false;
            while (XPending(d) > 0) {
                XEvent x_ev;
                XNextEvent(d, &x_ev);
                //XRRUpdateConfiguration(&x_ev);
                if (x_ev.type == damage_notify_event) {
                    has_damage = true;
                    //more = ((XDamageNotifyEvent *) &x_ev)->more;
                    //if (!more) break;
                } else if (x_ev.type == screen_change_notify_event) {
                    puts("rr");
                }
                //printf("event: %i\n", x_ev.type);
            }
            XDamageSubtract(d, damage, None, None);
        }

        if (has_input || has_damage) {
            // Redraw the window contents
            draw(
                    width, height, scale, cursor_x, cursor_y,
                    dest_pixmap, final_pixmap,
                    dest_pic, final_pic, root_attr, dest_attr, root, w, d, gc,
                    format_32, format_24, format_1);
            XSync(d, false);
            //XFlush(d);

            // Wait for completiona
            XEvent x_ev;
            wait_for_event(d, &x_ev, damage_notify_event);
            wait_for_event(d, &x_ev, NoExpose);

            // Sleep to prevent re-drawing faster than update rate
            clock_gettime(CLOCK_MONOTONIC_RAW, &time);
            const long one_second = 1000000000;
            const long target_nsec = one_second / rate;
            long delta_nsec =
                (time.tv_sec - prev_time.tv_sec) * one_second
                + (time.tv_nsec - prev_time.tv_nsec);
            if (delta_nsec < target_nsec) {
                struct timespec duration = {
                    .tv_sec = 0,
                    .tv_nsec = target_nsec - delta_nsec
                };
                struct timespec remaining = {
                    .tv_nsec = 1
                };
                int success = -1;
                while (success != 0) {
                    success = nanosleep(&duration, &remaining);
                    duration = remaining;
                }
            }
        }

        //XSync(d, true);
        XRaiseWindow(d, w);
    }

    // Clean up X objects
    /*
    XFreePixmap(d, dest_pixmap);
    XRenderFreePicture(d, dest_pic);
    XFreePixmap(d, final_pixmap);
    XRenderFreePicture(d, final_pic);
    XDamageDestroy(d, damage);
    XDestroyWindow(d, w);
    */
    XCloseDisplay(d);

    // Clean up libinput
    libinput_unref(li);
    udev_unref(udev);

    // Remove pidfile, if it was created
    if (xdg_runtime_dir != -1) {
        exit_errno_if(unlinkat(xdg_runtime_dir, pidfile_name, 0), "Removing pidfile failed");
        exit_errno_if(close(xdg_runtime_dir), "Closing XDG runtime dir failed");
    }

    return 0;
}
