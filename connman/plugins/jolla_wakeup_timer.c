/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2014 Jolla Ltd. All rights reserved.
 *  Contact: Hannu Mallat <hannu.mallat@jollamobile.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/*
 * Use libiphb to request wakeups from DSME.
 *
 * Act as a wrapper for g_timeout_add_full(); keep track of timeouts
 * which are active and when they are about to expire; schedule
 * wakeups from suspend as necessary.
 *
 * Try not to schedule wakeups too often as it can be expensive; use a
 * large granularity in calculating the next scheduling.
 */

#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glib-unix.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define CONNMAN_API_SUBJECT_TO_CHANGE
#include <connman/plugin.h>
#include <connman/wakeup_timer.h>
#include <connman/log.h>

#include <iphbd/libiphb.h>

#define IPHB_RETRY_DELAY 2
#define GRAIN IPHB_GS_WAIT_30_SEC

struct wakeup_context {
	gboolean initialized;
	iphb_t handle;
	GList *timeouts;
	guint source; /* unixfd source for iphb fd */
	time_t wakeup_time;
};

struct wakeup_timeout {
	struct timespec trigger;
	guint interval;
	GSourceFunc function;
	gpointer user_data;
	GDestroyNotify notify;
	guint source;
	time_t wakeup_time;
};

static struct wakeup_context context = {
	FALSE,
	NULL,
	NULL,
	0,
	0
};

static void wakeup_reschedule(void);

static gboolean wakeup_wakeup(gint fd, GIOCondition cnd, gpointer data);

static void iphb_cleanup(void)
{
	if (context.source) {
		g_source_remove(context.source);
		context.source = 0;
	}

	if (context.handle)
		context.handle = iphb_close(context.handle);
}

static int iphb_setup(void)
{
	int r = 0, fd = -1;

	if (context.handle) {
		/* already set up, no op */
		goto out;
	}

	context.handle = iphb_open(NULL);
	if (context.handle == NULL) {
		connman_warn("Cannot initialize IPHB handle: %s(%d).",
				strerror(errno), errno);
		r = -errno;
		goto error;
	}

	fd = iphb_get_fd(context.handle);
	if (fd < 0) {
		connman_warn("Cannot get IPHB fd: %s(%d).",
				strerror(errno), errno);
		r = -errno;
		goto error;
	}

	context.source = g_unix_fd_add_full(G_PRIORITY_HIGH, fd,
						G_IO_IN | G_IO_HUP |
						G_IO_ERR | G_IO_NVAL,
						wakeup_wakeup, NULL, NULL);
	if (!context.source) {
		connman_warn("Cannot set up IPHB source.");
		r = -EIO;
		goto error;
	}

	DBG("IPHB context set up.");
	goto out;

error:
	iphb_cleanup();

out:
	return r;
}

static gboolean iphb_reestablish(gpointer user_data)
{
	int r;

	r = iphb_setup();
	if (r == 0) {
		/* Ok, force reschedule and that's it */
		context.wakeup_time = 0;
		wakeup_reschedule();
		return FALSE;
	}

	return TRUE; /* DSME still MIA */
}

static void debug_timeouts(void)
{
	struct timespec now;
	GList *l;

	clock_gettime(CLOCK_BOOTTIME, &now);
	DBG("now = %lu", now.tv_sec);
	DBG("context.wakeup_time = %lu", context.wakeup_time);
	DBG("context.timeouts = {");
	for (l = context.timeouts; l; l = l->next) {
		struct wakeup_timeout *timeout = l->data;
		DBG("        timeout %p: wakeup_time = %lu",
			timeout, timeout->wakeup_time);
	}
	DBG("}");
}

static int timespec_cmp(const struct timespec *t1, const struct timespec *t2)
{
	if (t1->tv_sec > t2->tv_sec)
		return 1;
	else if (t1->tv_sec < t2->tv_sec)
		return -1;

	if (t1->tv_nsec > t2->tv_nsec)
		return 1;
	else if (t1->tv_nsec < t2->tv_nsec)
		return -1;

	return 0;
}

static void timespec_add(struct timespec *t1, struct timespec *t2)
{
	t1->tv_nsec += t2->tv_nsec;
	if (t1->tv_nsec > 1000000000) {
		t1->tv_nsec -= 1000000000;
		t1->tv_sec++;
	}
	t1->tv_sec += t2->tv_sec;
}

static gint timeout_compare(gconstpointer a, gconstpointer b)
{
	const struct wakeup_timeout *t1 = a;
	const struct wakeup_timeout *t2 = b;
	return timespec_cmp(&t1->trigger, &t2->trigger);
}

static void timeout_record(struct wakeup_timeout *timeout)
{
	struct timespec now;
	struct timespec interval;

	DBG("Recording timeout %p with %u ms interval.", timeout,
		timeout->interval);

	clock_gettime(CLOCK_BOOTTIME, &now);

	interval.tv_sec = timeout->interval/1000;
	interval.tv_nsec = (timeout->interval % 1000) * 1000000;
	timeout->trigger.tv_sec = now.tv_sec;
	timeout->trigger.tv_nsec = now.tv_nsec;
	timespec_add(&timeout->trigger, &interval);

	timeout->wakeup_time = (timeout->trigger.tv_sec / GRAIN + 1) * GRAIN;

	DBG("Timeout %p wakeup time is at %lu (%lu seconds from now)",
		timeout, timeout->wakeup_time,
		timeout->wakeup_time - now.tv_sec);

	context.timeouts = g_list_insert_sorted(context.timeouts,
						timeout,
						timeout_compare);
	wakeup_reschedule();
	debug_timeouts();
}

static gboolean timeout_function_wrapper(gpointer user_data)
{
	struct wakeup_timeout *timeout = user_data;
	gboolean again;

	DBG("Timeout %p expired", timeout);

	if (g_list_find(context.timeouts, timeout)) {
		context.timeouts = g_list_remove(context.timeouts, timeout);
		wakeup_reschedule();
		debug_timeouts();
	}

	/* If the timeout is to be repeated, put it back to the
	   bookkeeping list in the right position; if not, our
	   GDestroyNotify wrapper will take care of cleanup when glib
	   calls it. */
	again = (timeout->function)(timeout->user_data);
	if (again) {
		DBG("Timeout %p repeating.", timeout);
		timeout_record(timeout);
	} else {
		DBG("Timeout %p not repeating.", timeout);
	}

	return again;
}

static void timeout_notify_wrapper(gpointer user_data)
{
	struct wakeup_timeout *timeout = user_data;

	DBG("Timeout %p cleanup", timeout);

	if (g_list_find(context.timeouts, timeout)) {
		context.timeouts = g_list_remove(context.timeouts, timeout);
		wakeup_reschedule();
		debug_timeouts();
	}

	if (timeout->notify)
		(timeout->notify)(timeout->user_data);
	g_free(timeout);
}

static void wakeup_reschedule(void)
{
	struct timespec now;

	DBG("Checking IPHB wakeup rescheduling need");

	clock_gettime(CLOCK_BOOTTIME, &now);
	DBG("The time is now %lu", now.tv_sec);

	if (context.timeouts) {
		struct wakeup_timeout *timeout = context.timeouts->data;

		DBG("Checking need for wakeup at %lu.", timeout->wakeup_time);

		if (context.wakeup_time != timeout->wakeup_time) {
			time_t delta_min = timeout->wakeup_time - now.tv_sec;
			time_t delta_max = delta_min + GRAIN;

			DBG("Scheduling IPHB wakeup %lu..%lu seconds from now.",
				delta_min, delta_max);

			iphb_I_woke_up(context.handle);
			if (iphb_wait2(context.handle, delta_min, delta_max,
					0, 1) < 0) {
				connman_warn("Cannot schedule IPHB wait.");
			}
			context.wakeup_time = timeout->wakeup_time;

		} else {
			DBG("IPHB wakeup already scheduled at %lu, "
				"no need to reschedule.",
				context.wakeup_time);
		}

	} else {
		DBG("No timeouts left, clearing any pending IPHB wakeup.");
		iphb_I_woke_up(context.handle);
		context.wakeup_time = 0;
	}
}

static gboolean wakeup_wakeup(gint fd, GIOCondition cnd, gpointer data)
{
	/* Glib will execute timeouts now that we are awake; rescheduling
	   will happen through that as needed. */

	DBG("Woke up.");
	iphb_discard_wakeups(context.handle);
	debug_timeouts();

	if ((cnd & G_IO_ERR) || (cnd & G_IO_HUP) || (cnd & G_IO_NVAL)) {
		connman_warn("Lost IPHB connection, trying to re-establish "
			"after a while.");
		iphb_cleanup();
		g_timeout_add_seconds(IPHB_RETRY_DELAY, iphb_reestablish, NULL);

		return FALSE;
	}

	return TRUE;
}

static guint wakeup_timeout(gint priority,
				guint interval,
				GSourceFunc function,
				gpointer user_data,
				GDestroyNotify notify,
				gboolean use_seconds)
{
	struct wakeup_timeout *timeout = NULL;

	timeout = g_new0(struct wakeup_timeout, 1);
	timeout->interval = use_seconds ? 1000*interval : interval;
	timeout->function = function;
	timeout->user_data = user_data;
	timeout->notify = notify;
	timeout->source =
		use_seconds
		? g_timeout_add_seconds_full(priority,
						interval,
						timeout_function_wrapper,
						timeout,
						timeout_notify_wrapper)
		: g_timeout_add_full(priority,
					interval,
					timeout_function_wrapper,
					timeout,
					timeout_notify_wrapper)
		;

	if (timeout->source == 0) {
		connman_warn("Failed to create a timeout source.");
		g_free(timeout);
		return 0;
	}

	DBG("Recording timeout %p", timeout);
	timeout_record(timeout);

	return timeout->source;
}

static guint wakeup_timeout_add(gint priority,
				guint interval,
				GSourceFunc function,
				gpointer user_data,
				GDestroyNotify notify)
{
	DBG("");
	return wakeup_timeout(priority, interval, function, user_data, notify,
				FALSE);
}


static guint wakeup_timeout_add_seconds(gint priority,
					guint interval,
					GSourceFunc function,
					gpointer user_data,
					GDestroyNotify notify)
{
	DBG("");
	return wakeup_timeout(priority, interval, function, user_data, notify,
				TRUE);
}

static struct connman_wakeup_timer timer = {
	"Jolla wakeup timer", wakeup_timeout_add, wakeup_timeout_add_seconds
};

static int jolla_wakeup_timer_init(void)
{
	struct timespec now;
	int r;

	DBG("");

	if (context.initialized) {
		connman_warn("Wakeup timer already initialized.");
		return -EALREADY;
	}

	if (clock_gettime(CLOCK_BOOTTIME, &now) < 0) {
		connman_warn("CLOCK_BOOTTIME not available: %s(%d).",
				strerror(errno), errno);
		r = -errno;
		goto error;
	}

	r = iphb_setup();
	if (r < 0)
		goto error;

	r = connman_wakeup_timer_register(&timer);
	if (r < 0)
		goto error;

	context.initialized = TRUE;
	return r;

error:
	iphb_cleanup();

	return r;
}

static void jolla_wakeup_timer_exit(void)
{
	DBG("");

	connman_wakeup_timer_unregister(&timer);

	while (context.timeouts) {
		/* Clean up back to front to avoid unneeded rescheduling */
		struct wakeup_timeout *timeout =
			g_list_last(context.timeouts)->data;
		g_source_remove(timeout->source);
	}

	iphb_cleanup();

	context.initialized = FALSE;
}

CONNMAN_PLUGIN_DEFINE(jolla_wakeup_timer, "Jolla wakeup timer", VERSION,
			CONNMAN_PLUGIN_PRIORITY_DEFAULT,
			jolla_wakeup_timer_init, jolla_wakeup_timer_exit)
