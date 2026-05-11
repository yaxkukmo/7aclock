/* 7aclock - analog X11 clock with optional date display inside the face
 * inspired by xclock and urxvclock
 *
 * Rendering uses a back-buffer Pixmap to avoid flicker:
 *   draw() → off-screen Pixmap via Cairo
 *   present() → single atomic XCopyArea to window
 *
 * Transparency: when -alpha < 1.0 we look for a 32-bit ARGB visual and
 * use CAIRO_OPERATOR_SOURCE so the alpha channel is written correctly to
 * the pixmap.  A compositing manager (picom, kwin, …) then blends the
 * window against the desktop.  Without a compositor the clock still runs,
 * just without real transparency.
 *
 * Sleep timing: without the seconds hand we sync to the minute boundary
 * via clock_gettime(); with it we sync to the second boundary.  Either
 * way we sleep precisely until the next hand movement is visible, so CPU
 * usage stays near zero between frames.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>

#define PI       M_PI
#define PROG     "7aclock"
#define DEF_SIZE 150
#define DEF_PAD  4

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

/* ── types ──────────────────────────────────────────────────────────── */

typedef struct { double r, g, b, a; } Color;

typedef struct {
    int    w, h;
    int    show_date;
    char  *date_fmt;
    Color  bg, fg, hands, sec_hand, date_fg, date_bg;
    int    padding;
    int    noseconds;
    int    noring;
    int    update_ms;   /* 0 = auto-sync to second/minute boundary */
    char  *title;
    char  *name;       /* WM_CLASS instance (res_name) */
    char  *wm_class;   /* WM_CLASS class   (res_class) */
    char  *geometry;
} Cfg;

/* Off-screen buffer for double-buffering.
 * Stores depth + visual so it can create its own Pixmap on resize.      */
typedef struct {
    Display        *dpy;
    int             depth;
    Visual         *visual;
    Pixmap          pix;
    GC              gc;
    cairo_surface_t *surf;
    cairo_t         *cr;
    int              w, h;
} Buf;

/* ── color ──────────────────────────────────────────────────────────── */

static int parse_color(Display *dpy, const char *spec, Color *c)
{
    XColor xc;
    if (!XParseColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), spec, &xc)) {
        fprintf(stderr, PROG ": bad color '%s'\n", spec);
        return 0;
    }
    c->r = xc.red   / 65535.0;
    c->g = xc.green / 65535.0;
    c->b = xc.blue  / 65535.0;
    /* alpha preserved from previous value */
    return 1;
}

/* ── back-buffer ────────────────────────────────────────────────────── */

static void buf_free(Buf *b)
{
    if (b->cr)   { cairo_destroy(b->cr);           b->cr   = NULL; }
    if (b->surf) { cairo_surface_destroy(b->surf); b->surf = NULL; }
    if (b->pix)  { XFreePixmap(b->dpy, b->pix);   b->pix  = 0;    }
}

static void buf_create(Buf *b, Window win, int w, int h)
{
    buf_free(b);
    b->w    = w;
    b->h    = h;
    b->pix  = XCreatePixmap(b->dpy, win, w, h, b->depth);
    b->surf = cairo_xlib_surface_create(b->dpy, b->pix, b->visual, w, h);
    b->cr   = cairo_create(b->surf);
    cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_BEST);
}

static void buf_present(Buf *b, Window win)
{
    cairo_surface_flush(b->surf);
    XCopyArea(b->dpy, b->pix, win, b->gc, 0, 0, b->w, b->h, 0, 0);
    XFlush(b->dpy);
}

/* ── sleep timing ───────────────────────────────────────────────────── */

/* Return a timeval that expires exactly at the next second (noseconds=0)
 * or minute (noseconds=1) boundary.  This keeps the hands accurate while
 * sleeping as long as possible between frames.                           */
static struct timeval next_tick(int noseconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    long usec  = ts.tv_nsec / 1000;    /* microseconds past this second */
    long sleep_us;

    if (noseconds) {
        time_t next_min = (ts.tv_sec / 60 + 1) * 60;
        sleep_us = (long)(next_min - ts.tv_sec) * 1000000L - usec;
    } else {
        sleep_us = 1000000L - usec;
    }

    /* Guard against rounding to zero or negative (land on exact boundary) */
    if (sleep_us <= 0)
        sleep_us += noseconds ? 60000000L : 1000000L;

    struct timeval tv = {
        .tv_sec  = (time_t)(sleep_us / 1000000),
        .tv_usec = (suseconds_t)(sleep_us % 1000000),
    };
    return tv;
}

/* ── title ──────────────────────────────────────────────────────────── */

static void update_title(Display *dpy, Window win, const char *fmt)
{
    time_t now = time(NULL);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, localtime(&now));

    /* Legacy WM_NAME / WM_ICON_NAME */
    XStoreName(dpy, win, buf);
    XSetIconName(dpy, win, buf);

    /* EWMH _NET_WM_NAME — modern WMs show this instead of WM_NAME */
    static Atom net_wm_name      = None;
    static Atom net_wm_icon_name = None;
    static Atom utf8_string      = None;
    if (net_wm_name == None) {
        net_wm_name      = XInternAtom(dpy, "_NET_WM_NAME",      False);
        net_wm_icon_name = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
        utf8_string      = XInternAtom(dpy, "UTF8_STRING",       False);
    }
    XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
        (const unsigned char *)buf, (int)strlen(buf));
    XChangeProperty(dpy, win, net_wm_icon_name, utf8_string, 8, PropModeReplace,
        (const unsigned char *)buf, (int)strlen(buf));
}

/* ── drawing ────────────────────────────────────────────────────────── */

/* Filled lance-shaped hand: pointed at both ends, widest at wp*R from the
 * pivot toward the tip.  front/back/wp/hw are all fractions of R.         */
static void hand_polygon(cairo_t *cr, double cx, double cy, double R,
                         double angle, double front, double back,
                         double wp, double hw)
{
    double ca = cos(angle), sa = sin(angle);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - back*R*ca,          cy - back*R*sa);
    cairo_line_to(cr, cx + wp*R*ca - hw*R*sa,  cy + wp*R*sa + hw*R*ca);
    cairo_line_to(cr, cx + front*R*ca,          cy + front*R*sa);
    cairo_line_to(cr, cx + wp*R*ca + hw*R*sa,  cy + wp*R*sa - hw*R*ca);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw(cairo_t *cr, int w, int h, const Cfg *cfg)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);

    double cx = w * 0.5;
    double cy = h * 0.5;
    double R  = ((w < h) ? w : h) * 0.5 - cfg->padding;
    if (R < 1.0) R = 1.0;

    /* Background — CAIRO_OPERATOR_SOURCE writes alpha too, which is what
     * the compositing manager reads to blend our window against the desktop.
     * CAIRO_OPERATOR_OVER would accumulate opacity over multiple frames.  */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, cfg->bg.r, cfg->bg.g, cfg->bg.b, cfg->bg.a);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* face circle */
    if (!cfg->noring) {
        cairo_set_source_rgba(cr, cfg->fg.r, cfg->fg.g, cfg->fg.b, cfg->fg.a);
        cairo_set_line_width(cr, R * 0.03);
        cairo_arc(cr, cx, cy, R, 0, 2 * PI);
        cairo_stroke(cr);
    }

    /* hour ticks */
    cairo_set_source_rgba(cr, cfg->fg.r, cfg->fg.g, cfg->fg.b, cfg->fg.a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    for (int i = 0; i < 12; i++) {
        double a = i * (PI / 6.0) - PI * 0.5;
        cairo_set_line_width(cr, R * 0.045);
        cairo_move_to(cr, cx + R * 0.80 * cos(a), cy + R * 0.80 * sin(a));
        cairo_line_to(cr, cx + R * 0.92 * cos(a), cy + R * 0.92 * sin(a));
        cairo_stroke(cr);
    }

    /* minute ticks */
    cairo_set_line_width(cr, R * 0.015);
    for (int i = 0; i < 60; i++) {
        if (i % 5 == 0) continue;
        double a = i * (PI / 30.0) - PI * 0.5;
        cairo_move_to(cr, cx + R * 0.87 * cos(a), cy + R * 0.87 * sin(a));
        cairo_line_to(cr, cx + R * 0.92 * cos(a), cy + R * 0.92 * sin(a));
        cairo_stroke(cr);
    }

    /* date window near 6 o'clock */
    if (cfg->show_date) {
        char buf[128];
        strftime(buf, sizeof(buf), cfg->date_fmt, tm);

        cairo_select_font_face(cr, "sans-serif",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, R * 0.185);

        cairo_text_extents_t te;
        cairo_text_extents(cr, buf, &te);

        double pad = R * 0.05;
        double bw  = te.width  + pad * 2.0;
        double bh  = te.height + pad * 2.0;
        double bx  = cx - bw * 0.5;
        double by  = cy + R * 0.42 - bh * 0.5;

        cairo_set_source_rgba(cr,
            cfg->date_bg.r, cfg->date_bg.g, cfg->date_bg.b, cfg->date_bg.a);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);

        cairo_set_source_rgba(cr,
            cfg->fg.r, cfg->fg.g, cfg->fg.b, cfg->fg.a * 0.6);
        cairo_set_line_width(cr, R * 0.012);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr,
            cfg->date_fg.r, cfg->date_fg.g, cfg->date_fg.b, cfg->date_fg.a);
        cairo_move_to(cr,
            bx + pad - te.x_bearing,
            by + pad - te.y_bearing);
        cairo_show_text(cr, buf);
    }

    /* minute hand — lance: wide at 0.08R ahead of pivot, tapers to both tips */
    double min_a = ((tm->tm_min + tm->tm_sec / 60.0) / 60.0) * 2.0 * PI - PI * 0.5;
    cairo_set_source_rgba(cr, cfg->hands.r, cfg->hands.g, cfg->hands.b, cfg->hands.a);
    hand_polygon(cr, cx, cy, R, min_a, 0.76, 0.12, 0.08, 0.028);

    /* hour hand — wider and stubbier lance */
    double hr_a = ((tm->tm_hour % 12 + tm->tm_min / 60.0) / 12.0) * 2.0 * PI - PI * 0.5;
    hand_polygon(cr, cx, cy, R, hr_a, 0.55, 0.12, 0.06, 0.040);

    /* seconds hand — thin needle lance, symmetric (widest at pivot) */
    if (!cfg->noseconds) {
        double sec_a = (tm->tm_sec / 60.0) * 2.0 * PI - PI * 0.5;
        cairo_set_source_rgba(cr,
            cfg->sec_hand.r, cfg->sec_hand.g, cfg->sec_hand.b, cfg->sec_hand.a);
        hand_polygon(cr, cx, cy, R, sec_a, 0.87, 0.20, 0.0, 0.011);
    }

    /* center cap */
    cairo_set_source_rgba(cr, cfg->hands.r, cfg->hands.g, cfg->hands.b, cfg->hands.a);
    cairo_arc(cr, cx, cy, R * 0.045, 0, 2 * PI);
    cairo_fill(cr);
}

/* ── argument helpers ───────────────────────────────────────────────── */

static void usage(void)
{
    fputs(
        "Usage: " PROG " [options]\n"
        "  -date              show date inside clock face\n"
        "  -dateformat FMT    strftime format (default: \"%d %b\")\n"
        "  -noseconds         hide seconds hand (wakes once/minute)\n"
        "  -noring            hide outer ring of the clock face\n"
        "  -alpha N           background opacity 0.0–1.0 (default: 1.0)\n"
        "                     requires a compositing window manager\n"
        "  -bg COLOR          background color         (default: #1a1a2e)\n"
        "  -fg COLOR          face/ticks/border color  (default: #e0e0e0)\n"
        "  -hd COLOR          hour+minute hands color  (default: #e0e0e0)\n"
        "  -sd COLOR          seconds hand color       (default: #e05050)\n"
        "  -dc COLOR          date text color          (default: same as -fg)\n"
        "  -db COLOR          date box background      (default: same as -bg)\n"
        "  -padding N         padding pixels           (default: 4)\n"
        "  -title FMT         window title; strftime formats allowed\n"
        "                     e.g. \"%H:%M\" shows current time as title\n"
        "  -name NAME         WM_CLASS instance name (default: 7aclock)\n"
        "  -class CLASS       WM_CLASS class name    (default: 7aclock)\n"
        "  -geometry WxH+X+Y  window geometry\n"
        "  -update MS         fixed redraw interval ms (overrides auto-sync)\n"
        "  -help              this message\n"
        "  q / Escape         quit\n",
        stderr);
}

static int safe_atoi(const char *s, int lo, int hi, const char *name)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi) {
        fprintf(stderr, PROG ": invalid %s '%s' (range %d..%d)\n",
                name, s, lo, hi);
        exit(1);
    }
    return (int)v;
}

static double safe_atod(const char *s, double lo, double hi, const char *name)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || *end || v < lo || v > hi) {
        fprintf(stderr, PROG ": invalid %s '%s' (range %.1f..%.1f)\n",
                name, s, lo, hi);
        exit(1);
    }
    return v;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int geom_flags = 0, geom_x = 0, geom_y = 0;

    Cfg cfg = {
        .w          = DEF_SIZE,
        .h          = DEF_SIZE,
        .show_date  = 0,
        .date_fmt   = "%d %b",
        .bg         = {0.102, 0.102, 0.180, 1.0},
        .fg         = {0.878, 0.878, 0.878, 1.0},
        .hands      = {0.878, 0.878, 0.878, 1.0},
        .sec_hand   = {0.878, 0.314, 0.314, 1.0},
        .date_fg    = {0.878, 0.878, 0.878, 1.0},
        .date_bg    = {0.102, 0.102, 0.180, 1.0},
        .padding    = DEF_PAD,
        .noseconds  = 0,
        .update_ms  = 0,    /* 0 = auto-sync */
        .title      = PROG,
        .name       = PROG,
        .wm_class   = PROG,
        .geometry   = NULL,
    };

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs(PROG ": cannot open display\n", stderr); return 1; }

    for (int i = 1; i < argc; i++) {
#define NEED(opt) \
        if (i + 1 >= argc) { \
            fprintf(stderr, PROG ": %s requires an argument\n", opt); \
            XCloseDisplay(dpy); return 1; \
        } ++i
        if      (!strcmp(argv[i], "-date"))      cfg.show_date = 1;
        else if (!strcmp(argv[i], "-noseconds")) cfg.noseconds = 1;
        else if (!strcmp(argv[i], "-noring"))    cfg.noring    = 1;
        else if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help")) {
            usage(); XCloseDisplay(dpy); return 0;
        }
        else if (!strcmp(argv[i], "-alpha")) {
            NEED("-alpha");
            double a = safe_atod(argv[i], 0.0, 1.0, "-alpha");
            cfg.bg.a = a;
            cfg.date_bg.a = a < 1.0 ? a * 0.85 : 1.0;
        }
        else if (!strcmp(argv[i], "-dateformat")) { NEED("-dateformat"); cfg.date_fmt = argv[i]; }
        else if (!strcmp(argv[i], "-title"))   { NEED("-title");   cfg.title    = argv[i]; }
        else if (!strcmp(argv[i], "-name"))    { NEED("-name");    cfg.name     = argv[i]; }
        else if (!strcmp(argv[i], "-class"))   { NEED("-class");   cfg.wm_class = argv[i]; }
        else if (!strcmp(argv[i], "-geometry")) {
            NEED("-geometry");
            cfg.geometry = argv[i];
            unsigned int uw = 0, uh = 0;
            geom_flags = XParseGeometry(argv[i], &geom_x, &geom_y, &uw, &uh);
            if ((geom_flags & WidthValue)  && uw > 0) cfg.w = (int)uw;
            if ((geom_flags & HeightValue) && uh > 0) cfg.h = (int)uh;
        }
        else if (!strcmp(argv[i], "-update")) {
            NEED("-update");
            cfg.update_ms = safe_atoi(argv[i], 100, 60000, "-update");
        }
        else if (!strcmp(argv[i], "-padding")) {
            NEED("-padding");
            cfg.padding = safe_atoi(argv[i], 0, 500, "-padding");
        }
        else if (!strcmp(argv[i], "-bg")) {
            NEED("-bg");
            double saved_a = cfg.bg.a;
            if (parse_color(dpy, argv[i], &cfg.bg)) {
                cfg.bg.a     = saved_a;
                cfg.date_bg  = cfg.bg;
                cfg.date_fg  = cfg.fg;
            }
        }
        else if (!strcmp(argv[i], "-fg")) {
            NEED("-fg");
            if (parse_color(dpy, argv[i], &cfg.fg)) cfg.date_fg = cfg.fg;
        }
        else if (!strcmp(argv[i], "-hd")) { NEED("-hd"); parse_color(dpy, argv[i], &cfg.hands);    }
        else if (!strcmp(argv[i], "-sd")) { NEED("-sd"); parse_color(dpy, argv[i], &cfg.sec_hand); }
        else if (!strcmp(argv[i], "-dc")) { NEED("-dc"); parse_color(dpy, argv[i], &cfg.date_fg);  }
        else if (!strcmp(argv[i], "-db")) { NEED("-db"); parse_color(dpy, argv[i], &cfg.date_bg);  }
        else {
            fprintf(stderr, PROG ": unknown option '%s'\n", argv[i]);
            usage(); XCloseDisplay(dpy); return 1;
        }
#undef NEED
    }

    int    screen = DefaultScreen(dpy);
    Window root   = RootWindow(dpy, screen);

    /* Resolve geometry position — negative offsets are from right/bottom edge */
    int win_x = 0, win_y = 0;
    if (geom_flags & XValue)
        win_x = (geom_flags & XNegative)
            ? DisplayWidth(dpy, screen)  + geom_x - cfg.w
            : geom_x;
    if (geom_flags & YValue)
        win_y = (geom_flags & YNegative)
            ? DisplayHeight(dpy, screen) + geom_y - cfg.h
            : geom_y;

    /* ── visual selection ──────────────────────────────────────────── */
    /* For transparency we need a 32-bit ARGB visual.  If we can't find
     * one, fall back to the default visual and clamp alpha to 1.0.     */
    Visual  *use_visual = DefaultVisual(dpy, screen);
    int      use_depth  = DefaultDepth(dpy, screen);
    Colormap use_cmap   = DefaultColormap(dpy, screen);
    int      own_cmap   = 0;

    if (cfg.bg.a < 1.0) {
        XVisualInfo tmpl = {
            .screen = screen, .depth = 32, .class = TrueColor
        };
        int n = 0;
        XVisualInfo *vi = XGetVisualInfo(dpy,
            VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
        if (vi && n > 0) {
            use_visual = vi[0].visual;
            use_depth  = 32;
            use_cmap   = XCreateColormap(dpy, root, use_visual, AllocNone);
            own_cmap   = 1;
        } else {
            fprintf(stderr, PROG ": no 32-bit ARGB visual available, "
                            "transparency disabled\n");
            cfg.bg.a      = 1.0;
            cfg.date_bg.a = 1.0;
        }
        if (vi) XFree(vi);
    }

    /* ── window ────────────────────────────────────────────────────── */
    /* border_pixel must be set explicitly when using a non-default visual */
    XSetWindowAttributes attr = {
        .background_pixmap = None,
        .border_pixel      = 0,
        .colormap          = use_cmap,
        .event_mask        = ExposureMask | StructureNotifyMask
                           | KeyPressMask | PropertyChangeMask,
    };
    unsigned long attr_mask = CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask;

    Window win = XCreateWindow(dpy, root,
        win_x, win_y, cfg.w, cfg.h, 0,
        use_depth, InputOutput, use_visual,
        attr_mask, &attr);

    XClassHint ch = { .res_name = cfg.name, .res_class = cfg.wm_class };
    XSetClassHint(dpy, win, &ch);

    Atom wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del, 1);

    /* All geometry positions must be corrected for WM frame decorations so
     * that the OUTER frame edge lands exactly at the geometry pixel.
     * USPosition in XSizeHints specifies the CLIENT window position (ICCCM),
     * so we offset by frame extents in both directions:
     *   +offset → add the adjacent extent  (e.g. win_y = geom_y + top_ext)
     *   −offset → subtract the far extent  (e.g. win_x = SW + geom_x − right_ext − w)
     * We ask the WM for _NET_FRAME_EXTENTS before mapping (wait ≤ 200 ms);
     * if it responds we set XSizeHints only once with the corrected values.
     * If the WM responds only after XMapWindow, the PropertyNotify fallback
     * corrects the position via XMoveWindow (also client coords, same math).
     * XSetNormalHints is intentionally called only once — a second call with
     * different coords causes some WMs to misinterpret the position.        */
    Atom a_frame_extents = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    Atom a_req_frame     = XInternAtom(dpy, "_NET_REQUEST_FRAME_EXTENTS", True);
    int  frame_adj_done  = 0;

    if (a_req_frame != None && (geom_flags & (XValue | YValue))) {
        XEvent fev = {0};
        fev.xclient.type         = ClientMessage;
        fev.xclient.window       = win;
        fev.xclient.message_type = a_req_frame;
        fev.xclient.format       = 32;
        XSendEvent(dpy, root, False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &fev);
        XFlush(dpy);

        int pre_fd = ConnectionNumber(dpy);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!frame_adj_done) {
            struct timespec tn;
            clock_gettime(CLOCK_MONOTONIC, &tn);
            long ms = (tn.tv_sec - t0.tv_sec) * 1000L
                    + (tn.tv_nsec - t0.tv_nsec) / 1000000L;
            if (ms >= 200) break;
            fd_set fds; FD_ZERO(&fds); FD_SET(pre_fd, &fds);
            long rem = 200 - ms;
            struct timeval tv = { rem / 1000, (rem % 1000) * 1000 };
            if (select(pre_fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
            while (XPending(dpy)) {
                XEvent pev;
                XNextEvent(dpy, &pev);
                if (pev.type != PropertyNotify
                    || pev.xproperty.atom != a_frame_extents) continue;
                Atom at; int fmt; unsigned long n, ba;
                unsigned char *data = NULL;
                if (XGetWindowProperty(dpy, win, a_frame_extents, 0, 4,
                        False, XA_CARDINAL, &at, &fmt, &n, &ba, &data)
                    == Success && data && n >= 4) {
                    long *ex = (long *)data;
                    /* ex = [left, right, top, bottom] frame extents.
                     * USPosition specifies the CLIENT window position; the
                     * WM places decorations around it.  So to land the outer
                     * frame edge at the requested geometry pixel we add the
                     * adjacent extent for positive offsets and subtract the
                     * opposite extent for negative (right/bottom) offsets.  */
                    if ((geom_flags & XValue) && (geom_flags & XNegative))
                        win_x = DisplayWidth(dpy, screen) + geom_x
                                - cfg.w - (int)ex[1];
                    else if (geom_flags & XValue)
                        win_x = geom_x + (int)ex[0];
                    if ((geom_flags & YValue) && (geom_flags & YNegative))
                        win_y = DisplayHeight(dpy, screen) + geom_y
                                - cfg.h - (int)ex[3];
                    else if (geom_flags & YValue)
                        win_y = geom_y + (int)ex[2];
                    frame_adj_done = 1;
                }
                if (data) XFree(data);
            }
        }
    }

    XSizeHints sh = {
        .flags     = PMinSize,
        .min_width = 50, .min_height = 50,
    };
    if (geom_flags & (XValue | YValue)) {
        sh.flags |= USPosition;
        sh.x = win_x;
        sh.y = win_y;
    }
    XSetNormalHints(dpy, win, &sh);

    XMapWindow(dpy, win);
    XFlush(dpy);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    Buf buf = { .dpy = dpy, .depth = use_depth, .visual = use_visual, .gc = gc };
    buf_create(&buf, win, cfg.w, cfg.h);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    update_title(dpy, win, cfg.title);
    draw(buf.cr, buf.w, buf.h, &cfg);
    buf_present(&buf, win);

    int xfd = ConnectionNumber(dpy);

    while (g_running) {
        struct timeval tv;
        if (cfg.update_ms > 0) {
            tv.tv_sec  = cfg.update_ms / 1000;
            tv.tv_usec = (cfg.update_ms % 1000) * 1000;
        } else {
            tv = next_tick(cfg.noseconds);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int sel = select(xfd + 1, &fds, NULL, NULL, &tv);
        if (sel < 0 && errno == EINTR) continue;

        /* Redraw only when the timer fires, not on every X event.
         * Expose and ConfigureNotify set this flag explicitly below.
         * Without this guard, update_title() → XChangeProperty() generates
         * PropertyNotify back to us (PropertyChangeMask), causing select()
         * to return immediately on every iteration — a 15 % CPU busy-loop. */
        int do_redraw = (sel == 0);

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0)
                    buf_present(&buf, win);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width  != buf.w ||
                    ev.xconfigure.height != buf.h) {
                    cfg.w = ev.xconfigure.width;
                    cfg.h = ev.xconfigure.height;
                    buf_create(&buf, win, cfg.w, cfg.h);
                    draw(buf.cr, buf.w, buf.h, &cfg);
                    buf_present(&buf, win);
                }
                break;
            case PropertyNotify:
                /* Fallback: correct position post-map via XMoveWindow.
                 * XMoveWindow takes CLIENT window coords directly, so the
                 * outer frame edge ends up at the geometry-specified pixel. */
                if (!frame_adj_done
                    && ev.xproperty.atom == a_frame_extents
                    && (geom_flags & (XValue | YValue))) {
                    Atom at; int fmt; unsigned long n, ba;
                    unsigned char *data = NULL;
                    if (XGetWindowProperty(dpy, win, a_frame_extents,
                                           0, 4, False, XA_CARDINAL,
                                           &at, &fmt, &n, &ba, &data)
                        == Success && data && n >= 4) {
                        long *ex = (long *)data;
                        if ((geom_flags & XValue) && (geom_flags & XNegative))
                            win_x = DisplayWidth(dpy, screen) + geom_x
                                    - cfg.w - (int)ex[1];
                        else if (geom_flags & XValue)
                            win_x = geom_x + (int)ex[0];
                        if ((geom_flags & YValue) && (geom_flags & YNegative))
                            win_y = DisplayHeight(dpy, screen) + geom_y
                                    - cfg.h - (int)ex[3];
                        else if (geom_flags & YValue)
                            win_y = geom_y + (int)ex[2];
                        XMoveWindow(dpy, win, win_x, win_y);
                        XFlush(dpy);
                    }
                    if (data) XFree(data);
                    frame_adj_done = 1;
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_del)
                    g_running = 0;
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_q || ks == XK_Escape)
                    g_running = 0;
                break;
            }
            }
        }

        if (do_redraw) {
            update_title(dpy, win, cfg.title);
            draw(buf.cr, buf.w, buf.h, &cfg);
            buf_present(&buf, win);
        }
    }

    buf_free(&buf);
    XFreeGC(dpy, gc);
    if (own_cmap) XFreeColormap(dpy, use_cmap);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
