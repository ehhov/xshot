#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <png.h>

int finish = 0;
const char *cmd;

void
finsignal(int signal)
{
	finish = -1;
}

void
usage(FILE *output)
{
	fprintf(output, "Usage: %s [-br] [-a|-m[a]|-s[tlf]] [-d delay(ms)] [--tty] [-h] [output.png]\n", cmd);
}

void
printximg(XImage *img, FILE *output)
{
	int x, y;
	unsigned long pixel;
	int sr = 0, sg = 0, fr = 0, fg = 0, fb = 0;
	uint8_t *row;
	png_struct *png;
	png_info *info;

	switch (img->bits_per_pixel) {
	case 16: /* only 5-6-5 format supported */
		sr = 11;
		sg = 5;
		fr = fb = 2047;
		fg = 1023;
		break;
	case 24:
	case 32: /* ignore alpha in case of 32-bit */
		sr = 16;
		sg = 8;
		fr = fg = fb = 257;
		break;
	default:
		fprintf(stderr, "Unsupported bpp: %d.\n", img->bits_per_pixel);
		return;
	}

	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(png);
	png_init_io(png, output);
	png_set_IHDR(png, info, img->width, img->height, 8, PNG_COLOR_TYPE_RGB, \
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(png, info);

	row = calloc(sizeof("RGB" - 1) * img->width, sizeof(uint8_t));
	for (y = 0; y < img->height; y++) {
		for (x = 0; x < img->width; x++) {
			pixel = XGetPixel(img, x, y);
			*(row + 3*x + 0) = htons(((pixel & img->red_mask) >> sr) * fr);
			*(row + 3*x + 1) = htons(((pixel & img->green_mask) >> sg) * fg);
			*(row + 3*x + 2) = htons((pixel & img->blue_mask) * fb);
		}
		png_write_row(png, (png_const_bytep)row);
	}
	free(row);

	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
}

Window
recursively(Display *dpy, Window win, Atom prop)
{
	Atom type = None;
	int format;
	unsigned char *data;
	unsigned int i, nchildren;
	unsigned long after, nitems;
	Window child = None, *children, parent, root;

	if (!XQueryTree(dpy, win, &root, &parent, &children, &nchildren))
		return None;
	for (i = 0; (i < nchildren) && (child == None); i++) {
		if ((XGetWindowProperty(dpy, children[i], prop, 0L, 0L, False, \
		                        (Atom) AnyPropertyType, &type, &format, \
		                        &nitems, &after, &data) == Success) \
		   && (type != (Atom) NULL))
			child = children[i];
		if (data)
			XFree(data);
	}
	for (i = 0; (i < nchildren) && (child == None); i++)
		child = recursively(dpy, children[i], prop);
	if (children != None)
		XFree(children);
	return (child);
}

void
excludeborders(Display *dpy, Window *win)
{
	Atom state;
	Atom type = None;
	int format;
	unsigned char *data;
	unsigned long after, items;
	Window stripped;

	state = XInternAtom(dpy, "WM_STATE", True);
	if (state == None)
		return;
	if ((XGetWindowProperty(dpy, *win, state, 0L, 0L, False, (Atom) AnyPropertyType, \
	                        &type, &format, &items, &after, &data) == Success) \
	   && (type != None))
		return;
	stripped = recursively(dpy, *win, state);
	if (!stripped)
		return;
	*win = stripped;
}

void
grabkey(Display *dpy, KeyCode key, unsigned int mod)
{
	static unsigned int numlockmask = 0;

	if (!numlockmask) {
		unsigned int i, j;
		XModifierKeymap *modmap;

		modmap = XGetModifierMapping(dpy);
		for (i = 0; i < 8; i++)
			for (j = 0; j < modmap->max_keypermod; j++)
				if (modmap->modifiermap[i * modmap->max_keypermod + j] \
				    == XKeysymToKeycode(dpy, XK_Num_Lock))
					numlockmask = (1 << i);
		XFreeModifiermap(modmap);
	}

	XGrabKey(dpy, key, mod, DefaultRootWindow(dpy), \
	         False, GrabModeAsync, GrabModeAsync);
	XGrabKey(dpy, key, mod | LockMask, DefaultRootWindow(dpy), \
	         False, GrabModeAsync, GrabModeAsync);
	XGrabKey(dpy, key, mod | numlockmask, DefaultRootWindow(dpy), \
	         False, GrabModeAsync, GrabModeAsync);
	XGrabKey(dpy, key, mod | numlockmask | LockMask, DefaultRootWindow(dpy), \
	         False, GrabModeAsync, GrabModeAsync);
}

int
region(Display *dpy, Window *win, int *x, int *y, int *w, int *h, int twoclick, int lines)
{
	XEvent ev;
	GC gc;
	XGCValues gcval;
	Cursor cursor, cursorangles[4];
	int done = 0, pressed = 0, i = 0, oldi = -1;
	int xstart = 0, ystart = 0, rw = 0, rh = 0;
	struct pollfd fds[1];

	*win = DefaultRootWindow(dpy);

	cursor = XCreateFontCursor(dpy, XC_cross);

	if (XGrabPointer(dpy, *win, False, PointerMotionMask | ButtonPressMask \
	                 | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, \
	                 *win, cursor, CurrentTime) \
	    != GrabSuccess) {
		fputs("Failed to grab pointer.\n", stderr);
		XFreeCursor(dpy, cursor);
		return -1;
	}

	cursorangles[0] = XCreateFontCursor(dpy, XC_lr_angle);
	cursorangles[1] = XCreateFontCursor(dpy, XC_ll_angle);
	cursorangles[2] = XCreateFontCursor(dpy, XC_ur_angle);
	cursorangles[3] = XCreateFontCursor(dpy, XC_ul_angle);

	gcval.function = GXxor;
	gcval.foreground = XWhitePixel(dpy, DefaultScreen(dpy));
	gcval.background = XBlackPixel(dpy, DefaultScreen(dpy));
	gcval.plane_mask = gcval.background ^ gcval.foreground;
	gcval.subwindow_mode = IncludeInferiors;
	gcval.line_width = 1;
	gc = XCreateGC(dpy, *win, GCFunction | GCForeground | GCBackground \
	                          | GCSubwindowMode | GCLineWidth, &gcval);

	grabkey(dpy, XKeysymToKeycode(dpy, XK_q), 0);
	grabkey(dpy, XKeysymToKeycode(dpy, XK_Escape), 0);
	XFlush(dpy); /* needed because the server is probably grabbed */

	if (lines) {
		rw = WidthOfScreen(DefaultScreenOfDisplay(dpy));
		rh = HeightOfScreen(DefaultScreenOfDisplay(dpy));
		XQueryPointer(dpy, *win, &(Window){0}, &(Window){0}, x, y, \
		              &(int){0}, &(int){0}, &(unsigned int){0});
		XDrawLine(dpy, *win, gc, *x, 0, *x, rh);
		XDrawLine(dpy, *win, gc, 0, *y, rw, *y);
		XFlush(dpy);
	}

	fds[0].fd = ConnectionNumber(dpy);
	fds[0].events = POLLIN;
	*w = *h = 0;
	while (!done && !finish) {
		poll(fds, 1, -1);
		while (XPending(dpy) && !finish) {
			XNextEvent(dpy, &ev);
			switch (ev.type) {
			case ButtonPress:
				/* this if() is necessary for twoclick */
				if (!pressed) {
					if (lines) {
						XDrawLine(dpy, *win, gc, *x, 0, *x, rh);
						XDrawLine(dpy, *win, gc, 0, *y, rw, *y);
						XFlush(dpy);
					}
					*x = xstart = ev.xbutton.x_root;
					*y = ystart = ev.xbutton.y_root;
					pressed = 1;
				}
				break;
			case ButtonRelease:
				if (!twoclick) {
					done = 1;
					if (*w == 0 && ev.xbutton.subwindow)
						*win = ev.xbutton.subwindow;
				} else {
					twoclick = 0;
				}
				break;
			case MotionNotify:
				if (pressed) {
					if (*w != 0)
						/* Re-draw last Rectangle to clear it */
						XDrawRectangle(dpy, *win, gc, *x, *y, *w, *h);
					if (i != oldi) {
						XChangeActivePointerGrab(dpy, ButtonPressMask \
						      | PointerMotionMask | ButtonReleaseMask, \
						      cursorangles[i], CurrentTime);
						oldi = i;
					}

					*x = ev.xmotion.x_root;
					*y = ev.xmotion.y_root;

					if (*x > xstart) {
						*w = *x - xstart;
						*x = xstart;
						i &= 2;
					} else {
						*w = xstart - *x;
						i |= 1;
					}
					if (*y > ystart) {
						*h = *y - ystart;
						*y = ystart;
						i &= 1;
					} else {
						*h = ystart - *y;
						i |= 2;
					}

					/* Draw Rectangle */
					XDrawRectangle(dpy, *win, gc, *x, *y, *w, *h);
					XFlush(dpy);
				} else if (lines) {
					XDrawLine(dpy, *win, gc, *x, 0, *x, rh);
					XDrawLine(dpy, *win, gc, 0, *y, rw, *y);

					*x = ev.xmotion.x_root;
					*y = ev.xmotion.y_root;

					XDrawLine(dpy, *win, gc, *x, 0, *x, rh);
					XDrawLine(dpy, *win, gc, 0, *y, rw, *y);
					XFlush(dpy);
				}
				break;
			case KeyPress:
				done = finish = 1;
				break;
			default:
				break;
			}
		}
	}

	/* remove everyting to take the shot */
	if (*w != 0) {
		XDrawRectangle(dpy, *win, gc, *x, *y, *w, *h);
	} else if (!pressed && lines) {
		XDrawLine(dpy, *win, gc, *x, 0, *x, rh);
		XDrawLine(dpy, *win, gc, 0, *y, rw, *y);
	}
	XFlush(dpy);
	
	XUngrabKey(dpy, AnyKey, AnyModifier, DefaultRootWindow(dpy));
	XUngrabPointer(dpy, CurrentTime);
	XFreeCursor(dpy, cursor);
	XFreeCursor(dpy, cursorangles[0]);
	XFreeCursor(dpy, cursorangles[1]);
	XFreeCursor(dpy, cursorangles[2]);
	XFreeCursor(dpy, cursorangles[3]);
	XFreeGC(dpy, gc);
	XSync(dpy, True);

	return *w != 0;
}

int
whichmon(XRRMonitorInfo *m, int n, int x, int y)
{
	int i;
	for (i = 0; i < n; i++)
		if (x >= m[i].x && x < m[i].x + m[i].width \
		    && y >= m[i].y && y < m[i].y + m[i].height)
			return i;
	return -1;
}

void
monitor(Display *dpy, Window *win, int *x, int *y, int *w, int *h, int *mflag)
{
	Window active;
	XWindowAttributes attr;
	XRRMonitorInfo *mons;
	int nmons, i, j;

	mons = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &nmons);
	if (nmons == -1) {
		fputs("XRandr failed to get monitors. Capturing all of them.\n", stderr);
		*win = DefaultRootWindow(dpy);
		*mflag = 0;
		return;
	}

	if (*mflag == 2) {
		XGetInputFocus(dpy, &active, &(int){0});
		XGetWindowAttributes(dpy, active, &attr);
		j = 0;
		do {
			i = whichmon(mons, nmons, attr.x + attr.width * (j & 1), \
			             attr.y + attr.height * ((j/2) & 1));
			j++;
			if (j > 4)
				break;
		} while (i < 0 && !finish);
	} else {
		XQueryPointer(dpy, DefaultRootWindow(dpy), &(Window){0}, \
		              &(Window){0}, x, y, &(int){0}, \
		              &(int){0}, &(unsigned int){0});
		i = whichmon(mons, nmons, *x, *y);
	}
	if (i < 0 || i >= nmons) {
		fputs("Failed to locate monitor. Capturing all of them.\n", stderr);
		*win = DefaultRootWindow(dpy);
		*mflag = 0;
	} else {
		*x = mons[i].x;
		*y = mons[i].y;
		*w = mons[i].width;
		*h = mons[i].height;
	}
	XRRFreeMonitors(mons);
}

int
main(int argc, char *argv[])
{
	XImage *img;
	Display *dpy;
	Window win;
	XWindowAttributes attr;
	int x, y, w, h;
	int rflag = 0, aflag = 0, sflag = 0, freeze = 1, lflag = 0, \
	    twoflag = 0, borders = 1, ttyflag = 0, mflag = 0;
	struct timespec delay = {0, 0};
	char *filename = NULL;
	FILE *file;
	struct sigaction action;

	cmd = argv[0];
	for (argc--, argv++; argv[0]; argc--, argv++) {
		if (argv[0][0] != '-') {
			if (filename) {
				fputs("Only one output file is allowed.\n", stderr);
				usage(stderr);
				return 1;
			}
			filename = argv[0];
			continue;
		}
		if (!strcmp(argv[0], "--tty")) {
			/* don't suppress output if it is a tty */
			ttyflag = 1;
			continue;
		}
		for (char *opt = ++argv[0]; opt[0]; opt++) {
			switch (*opt) {
			case 'm':
				/* capture a monitor instead of the whole root */
				mflag = 1;
				break;
			case 'r':
				/* Use root window to get the image how the
				 * user sees it. This option also enables
				 * borders (otherwise they are detected
				 * incorrectly). */
				rflag = 1;
				break;
			case 'a':
				/* active window */
				aflag = 1;
				break;
			case 's':
				/* select with mouse */
				sflag = 1;
				break;
			case 't':
				/* click two points to select a region */
				twoflag = 1;
				break;
			case 'f':
				/* Do not freeze the screen. This causes
				 * compositors not to pause and hence
				 * breaks the selection rectangle. */
				freeze = 0;
				break;
			case 'b':
				/* don't include window borders and decorations */
				borders = 0;
				break;
			case 'l':
				/* draw lines in selection mode before the first click */
				lflag = 1;
				break;
			case 'd':
				/* delay before the shot in milliseconds */
				if (opt[1]) {
					delay.tv_sec = strtol(++opt, NULL, 0);
					opt += strlen(opt) - 1;
				} else if (argv[1]) {
					argc--; argv++;
					delay.tv_sec = strtol(argv[0], NULL, 0);
				} else {
					fputs("-d flag needs an argument.\n", stderr);
					usage(stderr);
					return 1;
				}
				delay.tv_nsec = 1e6 * (delay.tv_sec % 1000);
				delay.tv_sec = delay.tv_sec / 1000;
				break;
			case 'h':
				/* print the help message and exit */
				usage(stdout);
				return 0;
				break;
			default:
				fprintf(stderr, "Unknown option -%c.\n", *opt);
				usage(stderr);
				return 1;
				break;
			}
		}
	}

	/* special options behavior */
	if (twoflag && !sflag)
		sflag = 1;
	if (aflag && mflag) {
		aflag = 0;
		mflag = 2;
	}

	if (aflag && sflag) {
		fputs("Incompatible options -a and -s (or -t).\n", stderr);
		usage(stderr);
		return 1;
	} else if (mflag && sflag) {
		fputs("Incompatible options -m and -s (or -t).\n", stderr);
		usage(stderr);
		return 1;
	} else if (mflag && !borders) {
		fputs("Note: -b is useless with -m.\n", stderr);
	} else if (lflag && !sflag) {
		fputs("Note: -l is useless without -s (or -t).\n", stderr);
	}

	/* unset useless and dangerous options */
	if (freeze && !sflag)
		freeze = 0;
	if (!sflag && !aflag)
		borders = 1;

	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = finsignal;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		fputs("Failed to open display.\n", stderr);
		return 1;
	}

	if (freeze)
		XGrabServer(dpy);

	/* obtain the window or coordinates */
	if (aflag) {
		XGetInputFocus(dpy, &win, &(int){0});
	} else if (sflag) {
		sflag = region(dpy, &win, &x, &y, &w, &h, twoflag, lflag);
		if (sflag == -1)
			goto ungrab;
	} else if (mflag) {
		monitor(dpy, &win, &x, &y, &w, &h, &mflag);
	} else {
		win = DefaultRootWindow(dpy);
	}

	if (finish)
		goto ungrab;

	if (!borders && win != DefaultRootWindow(dpy))
		excludeborders(dpy, &win);

	if (!freeze)
		XGrabServer(dpy);

	if (delay.tv_sec || delay.tv_nsec) {
		XUngrabServer(dpy);
		XSync(dpy, False);
		nanosleep(&delay, NULL);
		XGrabServer(dpy);
	}

	/* obtain the image */
	if (sflag || mflag) {
		img = XGetImage(dpy, DefaultRootWindow(dpy), x, y, w, h, \
		                AllPlanes, ZPixmap);
	} else {
		XGetWindowAttributes(dpy, win, &attr);
		if (rflag)
			img = XGetImage(dpy, attr.root, attr.x, attr.y, \
			                attr.width, attr.height, AllPlanes, ZPixmap);
		else
			img = XGetImage(dpy, win, 0, 0, attr.width, attr.height, \
			                AllPlanes, ZPixmap);
	}
	XUngrabServer(dpy);
	XCloseDisplay(dpy);

	if (!img) {
		fputs("Failed to get image.\n", stderr);
		return 1;
	}

	if (finish) {
		XDestroyImage(img);
		goto end;
	}

	/* print the image to the standard output or to the specified file */
	if (filename) {
		if (!(file = fopen(filename, "w"))) {
			fprintf(stderr, "Cannot open file %s. Image lost.\n", filename);
			XDestroyImage(img);
			return 1;
		}
		printximg(img, file);
		fclose(file);
	} else {
		if (ttyflag || !isatty(STDOUT_FILENO))
			printximg(img, stdout);
		else
			fputs("Not printing the image to a tty. If you actually" \
			      " need it, run with the --tty option.\n", stderr);
	}

	XDestroyImage(img);

	return 0;

ungrab:
	if (freeze)
		XUngrabServer(dpy);
	XCloseDisplay(dpy);
end:
	if (finish == -1)
		fputs("Signal received, finishing.\n", stderr);
	return 1;
}
