#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#define MAX_STORED 4
#define NAB_TIMEOUT_MS 500

typedef struct {
	Atom type;
	unsigned char *data;
	unsigned long len;
} StoredSelection;

static StoredSelection stored[MAX_STORED];
static int stored_count = 0;

static StoredSelection pending[MAX_STORED];
static int pending_count = 0;

static Atom CLIPBOARD;
static Atom UTF8_STRING;
static Atom STRING;
static Atom TARGETS;
static Atom IMAGE_PNG;
static Atom XCLIPD_PROPERTY;
static Atom INCR;

char * now() {
	time_t ts;
	struct tm * tm;
	char *s, *nl;

	time(&ts);
	tm = localtime(&ts);
	if (!tm) {
		return "(time is unknowable)";
	}

	s = asctime(tm);
	nl = strrchr(s, '\n');
	if (nl) {
		*nl = '\0';
	}
	return s;
}

/* Wait for an X event with timeout. Returns 1 if event available, 0 on timeout. */
int wait_for_event(Display *display, int timeout_ms) {
	if (XPending(display) > 0) return 1;

	int fd = ConnectionNumber(display);
	fd_set fds;
	struct timeval tv;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	return select(fd + 1, &fds, NULL, NULL, &tv) > 0;
}

void clear_stored() {
	for (int i = 0; i < stored_count; i++) {
		if (stored[i].data) {
			XFree(stored[i].data);
			stored[i].data = NULL;
		}
	}
	stored_count = 0;
}

void clear_pending() {
	for (int i = 0; i < pending_count; i++) {
		if (pending[i].data) {
			XFree(pending[i].data);
			pending[i].data = NULL;
		}
	}
	pending_count = 0;
}

StoredSelection *find_stored(Atom type) {
	for (int i = 0; i < stored_count; i++) {
		if (stored[i].type == type) return &stored[i];
	}
	/* Serve STRING requests from UTF8_STRING data */
	if (type == STRING) {
		for (int i = 0; i < stored_count; i++) {
			if (stored[i].type == UTF8_STRING) return &stored[i];
		}
	}
	return NULL;
}

/* Request a single format from the current clipboard owner and stage it in pending */
int nab_type(Display *display, Window w, Atom target) {
	XEvent ev;
	Atom type;
	int di;
	unsigned long size, dul;
	unsigned char *ignored = NULL;

	XConvertSelection(display, CLIPBOARD, target, XCLIPD_PROPERTY, w, CurrentTime);
	for (;;) {
		if (!wait_for_event(display, NAB_TIMEOUT_MS)) {
			fprintf(stderr, "[%s] xclipd: timeout waiting for selection data\n", now());
			return 0;
		}
		XNextEvent(display, &ev);
		if (ev.type != SelectionNotify) continue;

		XSelectionEvent *sev = (XSelectionEvent *)&ev.xselection;
		if (sev->property == None) return 0;

		XGetWindowProperty(display, w, XCLIPD_PROPERTY, 0, 0, False, AnyPropertyType,
		                   &type, &di, &dul, &size, &ignored);
		XFree(ignored);
		if (type == INCR) {
			XDeleteProperty(display, w, XCLIPD_PROPERTY);
			fprintf(stderr, "[%s] xclipd: data too large and INCR mechanism not implemented\n", now());
			return 0;
		}

		unsigned char *data = NULL;
		unsigned long nitems;
		XGetWindowProperty(display, w, XCLIPD_PROPERTY, 0, size, False, AnyPropertyType,
		                   &type, &di, &nitems, &dul, &data);
		XDeleteProperty(display, w, XCLIPD_PROPERTY);

		if (pending_count < MAX_STORED && data && nitems > 0) {
			pending[pending_count].type = target;
			pending[pending_count].data = data;
			pending[pending_count].len = nitems;
			pending_count++;
			return 1;
		}
		if (data) XFree(data);
		return 0;
	}
}

void nab(Display *display, Window w) {
	Window owner;
	XEvent ev;

	owner = XGetSelectionOwner(display, CLIPBOARD);
	if (owner == None) {
		fprintf(stderr, "[%s] xclipd: taking ownership of unowned clipboard.\n", now());
		XSetSelectionOwner(display, CLIPBOARD, w, CurrentTime);
		return;
	}

	pending_count = 0;

	/* Ask the owner what formats it supports */
	XConvertSelection(display, CLIPBOARD, TARGETS, XCLIPD_PROPERTY, w, CurrentTime);

	Atom *targets = NULL;
	unsigned long num_targets = 0;

	for (;;) {
		if (!wait_for_event(display, NAB_TIMEOUT_MS)) {
			fprintf(stderr, "[%s] xclipd: timeout waiting for TARGETS\n", now());
			break;
		}
		XNextEvent(display, &ev);
		if (ev.type != SelectionNotify) continue;

		XSelectionEvent *sev = (XSelectionEvent *)&ev.xselection;
		if (sev->property != None) {
			Atom type;
			int di;
			unsigned long nitems, dul;
			unsigned char *data = NULL;

			XGetWindowProperty(display, w, XCLIPD_PROPERTY, 0, 1024, False, XA_ATOM,
			                   &type, &di, &nitems, &dul, &data);
			XDeleteProperty(display, w, XCLIPD_PROPERTY);

			if (type == XA_ATOM && data) {
				targets = (Atom *)data;
				num_targets = nitems;
			}
		}
		break;
	}

	if (targets && num_targets > 0) {
		/* Grab each format we care about */
		Atom wanted[] = { IMAGE_PNG, UTF8_STRING };
		int n_wanted = 2;

		for (int i = 0; i < n_wanted; i++) {
			for (unsigned long j = 0; j < num_targets; j++) {
				if (targets[j] == wanted[i]) {
					nab_type(display, w, wanted[i]);
					break;
				}
			}
		}
		XFree(targets);
	} else {
		/* Fallback: owner doesn't support TARGETS, try UTF8_STRING directly */
		nab_type(display, w, UTF8_STRING);
	}

	if (pending_count > 0) {
		/* Successfully fetched new data — replace old stored data */
		clear_stored();
		for (int i = 0; i < pending_count; i++)
			stored[i] = pending[i];
		stored_count = pending_count;
		pending_count = 0;
		fprintf(stderr, "[%s] xclipd: taking stewardship of %d format(s)\n", now(), stored_count);
		XSetSelectionOwner(display, CLIPBOARD, w, CurrentTime);
	} else {
		/* Nab failed — keep previous data if we have any */
		clear_pending();
		if (XGetSelectionOwner(display, CLIPBOARD) == None && stored_count > 0) {
			fprintf(stderr, "[%s] xclipd: nab failed, reclaiming with previous data\n", now());
			XSetSelectionOwner(display, CLIPBOARD, w, CurrentTime);
		}
	}
}

void deny(Display *display, XSelectionRequestEvent *sev) {
	XSelectionEvent ssev;

	ssev.type = SelectionNotify;
	ssev.requestor = sev->requestor;
	ssev.selection = sev->selection;
	ssev.target = sev->target;
	ssev.property = None;
	ssev.time = sev->time;

	XSendEvent(display, sev->requestor, True, NoEventMask, (XEvent *)&ssev);
}

void fulfill(Display *display, XSelectionRequestEvent *sev, StoredSelection *s) {
	XSelectionEvent ssev;

	XChangeProperty(display, sev->requestor, sev->property, s->type, 8, PropModeReplace,
	                s->data, s->len);

	ssev.type = SelectionNotify;
	ssev.requestor = sev->requestor;
	ssev.selection = sev->selection;
	ssev.target = sev->target;
	ssev.property = sev->property;
	ssev.time = sev->time;

	XSendEvent(display, sev->requestor, True, NoEventMask, (XEvent *)&ssev);
}

void bailout(int sig, siginfo_t *info, void *ctx) {
	(void)sig; (void)info; (void)ctx;
	exit(0);
}

int main() {
	Display *display;
	Window target_window;
	int screen;
	XEvent ev;
	XSelectionRequestEvent *sev;
	struct sigaction sa;

	sa.sa_sigaction = bailout;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	display = XOpenDisplay(NULL);
	if (!display) {
		fprintf(stderr, "[%s] xclipd: could not open X display\n", now());
		return 1;
	}

	INCR            = XInternAtom(display, "INCR",        False);
	CLIPBOARD       = XInternAtom(display, "CLIPBOARD",   False);
	UTF8_STRING     = XInternAtom(display, "UTF8_STRING", False);
	STRING          = XInternAtom(display, "STRING",      False);
	TARGETS         = XInternAtom(display, "TARGETS",     False);
	IMAGE_PNG       = XInternAtom(display, "image/png",   False);
	XCLIPD_PROPERTY = XInternAtom(display, "XCLIPD",      False);

	screen = DefaultScreen(display);
	target_window = XCreateSimpleWindow(display, RootWindow(display, screen),
	                                    -10, -10, 1, 1, 0, 0, 0);

	int xfixes_event_base, xfixes_error_base;
	if (!XFixesQueryExtension(display, &xfixes_event_base, &xfixes_error_base)) {
		fprintf(stderr, "[%s] xclipd: XFixes extension not available\n", now());
		return 1;
	}
	XFixesSelectSelectionInput(display, target_window, CLIPBOARD,
	                           XFixesSetSelectionOwnerNotifyMask);

	nab(display, target_window);

	for (;;) {
		XNextEvent(display, &ev);

		if (ev.type == xfixes_event_base + XFixesSelectionNotify) {
			XFixesSelectionNotifyEvent *fev = (XFixesSelectionNotifyEvent *)&ev;
			if (fev->owner != target_window && fev->owner != None) {
				fprintf(stderr, "[%s] xclipd: clipboard taken by another process; nabbing...\n", now());
				nab(display, target_window);
			} else if (fev->owner == None && stored_count > 0) {
				fprintf(stderr, "[%s] xclipd: clipboard became unowned; reclaiming...\n", now());
				XSetSelectionOwner(display, CLIPBOARD, target_window, CurrentTime);
			}
			continue;
		}

		switch (ev.type) {
		case SelectionClear:
			/* Handled by XFixes above; just ignore */
			break;

		case SelectionRequest:
			sev = (XSelectionRequestEvent*)&ev.xselectionrequest;

			if (stored_count == 0 || sev->property == None) {
				deny(display, sev);

			} else if (sev->target == TARGETS) {
				Atom tlist[MAX_STORED + 2];
				int n = 0;
				int has_string = 0, has_utf8 = 0;

				tlist[n++] = TARGETS;
				for (int i = 0; i < stored_count; i++) {
					tlist[n++] = stored[i].type;
					if (stored[i].type == STRING) has_string = 1;
					if (stored[i].type == UTF8_STRING) has_utf8 = 1;
				}
				/* If we have UTF8_STRING, also advertise STRING */
				if (has_utf8 && !has_string) tlist[n++] = STRING;

				XChangeProperty(display, sev->requestor, sev->property, XA_ATOM, 32, PropModeReplace,
				                (unsigned char *)tlist, n);

				XSelectionEvent ssev;
				ssev.type = SelectionNotify;
				ssev.requestor = sev->requestor;
				ssev.selection = sev->selection;
				ssev.target = TARGETS;
				ssev.property = sev->property;
				ssev.time = sev->time;
				XSendEvent(display, sev->requestor, True, NoEventMask, (XEvent *)&ssev);

			} else {
				StoredSelection *s = find_stored(sev->target);
				if (s) {
					fulfill(display, sev, s);
				} else {
					deny(display, sev);
				}
			}
			break;
		}
	}
}
