/*
 * Copyright © 2014-2015 David FORT <contact@hardening-consulting.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include "uwac-priv.h"
#include "uwac-utils.h"

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>

#include <winpr/cast.h>

#include "uwac-os.h"
#include "wayland-cursor.h"
#include "wayland-client-protocol.h"

static struct wl_buffer* create_pointer_buffer(UwacSeat* seat, const void* src, size_t size)
{
	struct wl_buffer* buffer = NULL;
	struct wl_shm_pool* pool = NULL;

	assert(seat);

	const int fd = uwac_create_anonymous_file(WINPR_ASSERTING_INT_CAST(off_t, size));

	if (fd < 0)
		return buffer;

	void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (data == MAP_FAILED)
	{
		goto error_mmap;
	}
	memcpy(data, src, size);

	pool = wl_shm_create_pool(seat->display->shm, fd, WINPR_ASSERTING_INT_CAST(int32_t, size));

	if (!pool)
	{
		munmap(data, size);
		goto error_mmap;
	}

	buffer = wl_shm_pool_create_buffer(
	    pool, 0, WINPR_ASSERTING_INT_CAST(int32_t, seat->pointer_image->width),
	    WINPR_ASSERTING_INT_CAST(int32_t, seat->pointer_image->height),
	    WINPR_ASSERTING_INT_CAST(int32_t, seat->pointer_image->width * 4), WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);

	if (munmap(data, size) < 0)
	{
		char buffer[256] = { 0 };
		(void)fprintf(stderr, "%s: munmap(%p, %zu) failed with [%d] %s\n", __func__, data, size,
		              errno, uwac_strerror(errno, buffer, sizeof(buffer)));
	}

error_mmap:
	close(fd);
	return buffer;
}

static void on_buffer_release(void* data, struct wl_buffer* wl_buffer)
{
	(void)data;
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener buffer_release_listener = { on_buffer_release };

static UwacReturnCode set_cursor_image(UwacSeat* seat, uint32_t serial)
{
	struct wl_buffer* buffer = NULL;
	struct wl_cursor* cursor = NULL;
	struct wl_cursor_image* image = NULL;
	struct wl_surface* surface = NULL;
	int32_t x = 0;
	int32_t y = 0;

	if (!seat || !seat->display || !seat->default_cursor || !seat->default_cursor->images)
		return UWAC_ERROR_INTERNAL;

	int scale = 1;
	if (seat->pointer_focus)
		scale = seat->pointer_focus->display->actual_scale;

	switch (seat->pointer_type)
	{
		case 2: /* Custom pointer */
			image = seat->pointer_image;
			buffer = create_pointer_buffer(seat, seat->pointer_data, seat->pointer_size);
			if (!buffer)
				return UWAC_ERROR_INTERNAL;
			if (wl_buffer_add_listener(buffer, &buffer_release_listener, seat) < 0)
				return UWAC_ERROR_INTERNAL;

			surface = seat->pointer_surface;
			x = WINPR_ASSERTING_INT_CAST(int32_t, image->hotspot_x / scale);
			y = WINPR_ASSERTING_INT_CAST(int32_t, image->hotspot_y / scale);
			break;
		case 1: /* NULL pointer */
			break;
		default: /* Default system pointer */
			cursor = seat->default_cursor;
			if (!cursor)
				return UWAC_ERROR_INTERNAL;
			image = cursor->images[0];
			if (!image)
				return UWAC_ERROR_INTERNAL;
			x = WINPR_ASSERTING_INT_CAST(int32_t, image->hotspot_x);
			y = WINPR_ASSERTING_INT_CAST(int32_t, image->hotspot_y);
			buffer = wl_cursor_image_get_buffer(image);
			if (!buffer)
				return UWAC_ERROR_INTERNAL;
			surface = seat->pointer_surface;
			break;
	}

	if (surface && buffer)
	{
		wl_surface_set_buffer_scale(surface, scale);
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage(surface, 0, 0, WINPR_ASSERTING_INT_CAST(int32_t, image->width),
		                  WINPR_ASSERTING_INT_CAST(int32_t, image->height));
		wl_surface_commit(surface);
	}

	wl_pointer_set_cursor(seat->pointer, serial, surface, x, y);

	return UWAC_SUCCESS;
}

static void keyboard_repeat_func(UwacTask* task, uint32_t events)
{
	UwacSeat* input = container_of(task, UwacSeat, repeat_task);
	assert(input);
	UwacWindow* window = input->keyboard_focus;
	uint64_t exp = 0;

	if (read(input->repeat_timer_fd, &exp, sizeof exp) != sizeof exp)
		/* If we change the timer between the fd becoming
		 * readable and getting here, there'll be nothing to
		 * read and we get EAGAIN. */
		return;

	if (window)
	{
		UwacKeyEvent* key = NULL;

		key = (UwacKeyEvent*)UwacDisplayNewEvent(input->display, UWAC_EVENT_KEY);
		if (!key)
			return;

		key->window = window;
		key->sym = input->repeat_sym;
		key->raw_key = input->repeat_key;
		key->pressed = true;
		key->repeated = true;
	}
}

static void keyboard_handle_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format,
                                   int fd, uint32_t size)
{
	UwacSeat* input = data;
	struct xkb_keymap* keymap = NULL;
	struct xkb_state* state = NULL;
	char* map_str = NULL;
	int mapFlags = MAP_SHARED;

	if (!data)
	{
		close(fd);
		return;
	}

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
	{
		close(fd);
		return;
	}

	if (input->seat_version >= 7)
		mapFlags = MAP_PRIVATE;

	map_str = mmap(NULL, size, PROT_READ, mapFlags, fd, 0);
	if (map_str == MAP_FAILED)
	{
		close(fd);
		return;
	}

	keymap = xkb_keymap_new_from_string(input->xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	munmap(map_str, size);
	close(fd);

	if (!keymap)
	{
		assert(uwacErrorHandler(input->display, UWAC_ERROR_INTERNAL, "failed to compile keymap\n"));
		return;
	}

	state = xkb_state_new(keymap);
	if (!state)
	{
		assert(
		    uwacErrorHandler(input->display, UWAC_ERROR_NOMEMORY, "failed to create XKB state\n"));
		xkb_keymap_unref(keymap);
		return;
	}

	xkb_keymap_unref(input->xkb.keymap);
	xkb_state_unref(input->xkb.state);
	input->xkb.keymap = keymap;
	input->xkb.state = state;

	input->xkb.control_mask = 1 << xkb_keymap_mod_get_index(input->xkb.keymap, "Control");
	input->xkb.alt_mask = 1 << xkb_keymap_mod_get_index(input->xkb.keymap, "Mod1");
	input->xkb.shift_mask = 1 << xkb_keymap_mod_get_index(input->xkb.keymap, "Shift");
	input->xkb.caps_mask = 1 << xkb_keymap_mod_get_index(input->xkb.keymap, "Lock");
	input->xkb.num_mask = 1 << xkb_keymap_mod_get_index(input->xkb.keymap, "Mod2");
}

static void keyboard_handle_key(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                                uint32_t time, uint32_t key, uint32_t state_w);

static void keyboard_handle_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                                  struct wl_surface* surface, struct wl_array* keys)
{
	UwacSeat* input = (UwacSeat*)data;
	assert(input);

	UwacKeyboardEnterLeaveEvent* event = (UwacKeyboardEnterLeaveEvent*)UwacDisplayNewEvent(
	    input->display, UWAC_EVENT_KEYBOARD_ENTER);
	if (!event)
		return;

	event->window = input->keyboard_focus = (UwacWindow*)wl_surface_get_user_data(surface);
	event->seat = input;

	/* we may have the keys in the `keys` array, but as this function is called only
	 * when the window gets focus, so there may be keys from other unrelated windows, eg.
	 * this was leading to problems like passing CTRL+D to freerdp from closing terminal window
	 * if it was closing very fast and the keys was still pressed by the user while the freerdp
	 * gets focus
	 *
	 * currently just ignore this, as further key presses will be handled correctly anyway
	 */
}

static void keyboard_handle_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                                  struct wl_surface* surface)
{
	struct itimerspec its = { 0 };
	uint32_t* pressedKey = NULL;
	size_t i = 0;

	UwacSeat* input = (UwacSeat*)data;
	assert(input);

	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = 0;
	its.it_value.tv_nsec = 0;
	(void)timerfd_settime(input->repeat_timer_fd, 0, &its, NULL);

	UwacPointerEnterLeaveEvent* event =
	    (UwacPointerEnterLeaveEvent*)UwacDisplayNewEvent(input->display, UWAC_EVENT_POINTER_LEAVE);
	if (!event)
		return;

	event->window = input->keyboard_focus;

	/* we are currently losing input focus of the main window:
	 * check if we currently have some keys pressed and release them as if we enter the window again
	 * it will be still "virtually" pressed in remote even if in reality the key has been released
	 */
	for (pressedKey = input->pressed_keys.data, i = 0; i < input->pressed_keys.size;
	     i += sizeof(uint32_t))
	{
		keyboard_handle_key(data, keyboard, serial, 0, *pressedKey, WL_KEYBOARD_KEY_STATE_RELEASED);
		pressedKey++;
	}
}

static int update_key_pressed(UwacSeat* seat, uint32_t key)
{
	uint32_t* keyPtr = NULL;
	assert(seat);

	/* check if the key is not already pressed */
	wl_array_for_each(keyPtr, &seat->pressed_keys)
	{
		if (*keyPtr == key)
			return 1;
	}

	keyPtr = wl_array_add(&seat->pressed_keys, sizeof(uint32_t));
	if (!keyPtr)
		return -1;

	*keyPtr = key;
	return 0;
}

static int update_key_released(UwacSeat* seat, uint32_t key)
{
	size_t toMove = 0;
	bool found = false;

	assert(seat);

	size_t i = 0;
	uint32_t* keyPtr = seat->pressed_keys.data;
	for (; i < seat->pressed_keys.size; i++, keyPtr++)
	{
		if (*keyPtr == key)
		{
			found = true;
			break;
		}
	}

	if (found)
	{
		toMove = seat->pressed_keys.size - ((i + 1) * sizeof(uint32_t));
		if (toMove)
			memmove(keyPtr, keyPtr + 1, toMove);

		seat->pressed_keys.size -= sizeof(uint32_t);
	}
	return 1;
}

static void keyboard_handle_key(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                                uint32_t time, uint32_t key, uint32_t state_w)
{
	UwacSeat* input = (UwacSeat*)data;
	assert(input);

	UwacWindow* window = input->keyboard_focus;
	UwacKeyEvent* keyEvent = NULL;

	uint32_t code = 0;
	uint32_t num_syms = 0;
	enum wl_keyboard_key_state state = state_w;
	const xkb_keysym_t* syms = NULL;
	xkb_keysym_t sym = 0;
	struct itimerspec its;

	if (state_w == WL_KEYBOARD_KEY_STATE_PRESSED)
		update_key_pressed(input, key);
	else
		update_key_released(input, key);

	input->display->serial = serial;
	code = key + 8;
	if (!window || !input->xkb.state)
		return;

	/* We only use input grabs for pointer events for now, so just
	 * ignore key presses if a grab is active.  We expand the key
	 * event delivery mechanism to route events to widgets to
	 * properly handle key grabs.  In the meantime, this prevents
	 * key event delivery while a grab is active. */
	/*if (input->grab && input->grab_button == 0)
	    return;*/

	num_syms = xkb_state_key_get_syms(input->xkb.state, code, &syms);

	sym = XKB_KEY_NoSymbol;
	if (num_syms == 1)
		sym = syms[0];

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED && key == input->repeat_key)
	{
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 0;
		its.it_value.tv_sec = 0;
		its.it_value.tv_nsec = 0;
		(void)timerfd_settime(input->repeat_timer_fd, 0, &its, NULL);
	}
	else if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
	         xkb_keymap_key_repeats(input->xkb.keymap, code))
	{
		input->repeat_sym = sym;
		input->repeat_key = key;
		input->repeat_time = time;
		its.it_interval.tv_sec = input->repeat_rate_sec;
		its.it_interval.tv_nsec = input->repeat_rate_nsec;
		its.it_value.tv_sec = input->repeat_delay_sec;
		its.it_value.tv_nsec = input->repeat_delay_nsec;
		(void)timerfd_settime(input->repeat_timer_fd, 0, &its, NULL);
	}

	keyEvent = (UwacKeyEvent*)UwacDisplayNewEvent(input->display, UWAC_EVENT_KEY);
	if (!keyEvent)
		return;

	keyEvent->window = window;
	keyEvent->sym = sym;
	keyEvent->raw_key = key;
	keyEvent->pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
	keyEvent->repeated = false;
}

static void keyboard_handle_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                                      uint32_t mods_depressed, uint32_t mods_latched,
                                      uint32_t mods_locked, uint32_t group)
{
	UwacSeat* input = data;
	assert(input);

	UwacKeyboardModifiersEvent* event = NULL;
	xkb_mod_mask_t mask = 0;

	/* If we're not using a keymap, then we don't handle PC-style modifiers */
	if (!input->xkb.keymap)
		return;

	xkb_state_update_mask(input->xkb.state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
	mask = xkb_state_serialize_mods(input->xkb.state, XKB_STATE_MODS_DEPRESSED |
	                                                      XKB_STATE_MODS_LATCHED |
	                                                      XKB_STATE_MODS_LOCKED);
	input->modifiers = 0;
	if (mask & input->xkb.control_mask)
		input->modifiers |= UWAC_MOD_CONTROL_MASK;
	if (mask & input->xkb.alt_mask)
		input->modifiers |= UWAC_MOD_ALT_MASK;
	if (mask & input->xkb.shift_mask)
		input->modifiers |= UWAC_MOD_SHIFT_MASK;
	if (mask & input->xkb.caps_mask)
		input->modifiers |= UWAC_MOD_CAPS_MASK;
	if (mask & input->xkb.num_mask)
		input->modifiers |= UWAC_MOD_NUM_MASK;

	event = (UwacKeyboardModifiersEvent*)UwacDisplayNewEvent(input->display,
	                                                         UWAC_EVENT_KEYBOARD_MODIFIERS);
	if (!event)
		return;

	event->modifiers = input->modifiers;
}

static void set_repeat_info(UwacSeat* input, int32_t rate, int32_t delay)
{
	assert(input);

	input->repeat_rate_sec = input->repeat_rate_nsec = 0;
	input->repeat_delay_sec = input->repeat_delay_nsec = 0;

	/* a rate of zero disables any repeating, regardless of the delay's
	 * value */
	if (rate == 0)
		return;

	if (rate == 1)
		input->repeat_rate_sec = 1;
	else
		input->repeat_rate_nsec = 1000000000 / rate;

	input->repeat_delay_sec = delay / 1000;
	delay -= (input->repeat_delay_sec * 1000);
	input->repeat_delay_nsec = delay * 1000 * 1000;
}

static void keyboard_handle_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate,
                                        int32_t delay)
{
	UwacSeat* input = data;
	assert(input);

	set_repeat_info(input, rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap, keyboard_handle_enter,     keyboard_handle_leave,
	keyboard_handle_key,    keyboard_handle_modifiers, keyboard_handle_repeat_info
};

static bool touch_send_start_frame(UwacSeat* seat)
{
	assert(seat);

	UwacTouchFrameBegin* ev =
	    (UwacTouchFrameBegin*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_TOUCH_FRAME_BEGIN);
	if (!ev)
		return false;

	seat->touch_frame_started = true;
	return true;
}

static void touch_handle_down(void* data, struct wl_touch* wl_touch, uint32_t serial, uint32_t time,
                              struct wl_surface* surface, int32_t id, wl_fixed_t x_w,
                              wl_fixed_t y_w)
{
	UwacSeat* seat = data;
	UwacTouchDown* tdata = NULL;

	assert(seat);
	assert(seat->display);

	seat->display->serial = serial;
	if (!seat->touch_frame_started && !touch_send_start_frame(seat))
		return;

	tdata = (UwacTouchDown*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_TOUCH_DOWN);
	if (!tdata)
		return;

	tdata->seat = seat;
	tdata->id = id;

	double sx = wl_fixed_to_double(x_w);
	double sy = wl_fixed_to_double(y_w);

	tdata->x = (wl_fixed_t)lround(sx);
	tdata->y = (wl_fixed_t)lround(sy);

#if 0
	struct widget *widget;
	float sx = wl_fixed_to_double(x);
	float sy = wl_fixed_to_double(y);


	input->touch_focus = wl_surface_get_user_data(surface);
	if (!input->touch_focus) {
		DBG("Failed to find to touch focus for surface %p\n", (void*) surface);
		return;
	}

	if (surface != input->touch_focus->main_surface->surface) {
		DBG("Ignoring input event from subsurface %p\n", (void*) surface);
		input->touch_focus = NULL;
		return;
	}

	if (input->grab)
		widget = input->grab;
	else
		widget = window_find_widget(input->touch_focus,
					    wl_fixed_to_double(x),
					    wl_fixed_to_double(y));
	if (widget) {
		struct touch_point *tp = xmalloc(sizeof *tp);
		if (tp) {
			tp->id = id;
			tp->widget = widget;
			tp->x = sx;
			tp->y = sy;
			wl_list_insert(&input->touch_point_list, &tp->link);

			if (widget->touch_down_handler)
				(*widget->touch_down_handler)(widget, input,
							      serial, time, id,
							      sx, sy,
							      widget->user_data);
		}
	}
#endif
}

static void touch_handle_up(void* data, struct wl_touch* wl_touch, uint32_t serial, uint32_t time,
                            int32_t id)
{
	UwacSeat* seat = data;
	UwacTouchUp* tdata = NULL;

	assert(seat);

	if (!seat->touch_frame_started && !touch_send_start_frame(seat))
		return;

	tdata = (UwacTouchUp*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_TOUCH_UP);
	if (!tdata)
		return;

	tdata->seat = seat;
	tdata->id = id;

#if 0
	struct touch_point *tp, *tmp;

	if (!input->touch_focus) {
		DBG("No touch focus found for touch up event!\n");
		return;
	}

	wl_list_for_each_safe(tp, tmp, &input->touch_point_list, link) {
		if (tp->id != id)
			continue;

		if (tp->widget->touch_up_handler)
			(*tp->widget->touch_up_handler)(tp->widget, input, serial,
							time, id,
							tp->widget->user_data);

		wl_list_remove(&tp->link);
		free(tp);

		return;
	}
#endif
}

static void touch_handle_motion(void* data, struct wl_touch* wl_touch, uint32_t time, int32_t id,
                                wl_fixed_t x_w, wl_fixed_t y_w)
{
	UwacSeat* seat = data;
	assert(seat);

	UwacTouchMotion* tdata = NULL;

	if (!seat->touch_frame_started && !touch_send_start_frame(seat))
		return;

	tdata = (UwacTouchMotion*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_TOUCH_MOTION);
	if (!tdata)
		return;

	tdata->seat = seat;
	tdata->id = id;

	double sx = wl_fixed_to_double(x_w);
	double sy = wl_fixed_to_double(y_w);

	tdata->x = (wl_fixed_t)lround(sx);
	tdata->y = (wl_fixed_t)lround(sy);

#if 0
	struct touch_point *tp;
	float sx = wl_fixed_to_double(x);
	float sy = wl_fixed_to_double(y);

	DBG("touch_handle_motion: %i %i\n", id, wl_list_length(&seat->touch_point_list));

	if (!seat->touch_focus) {
		DBG("No touch focus found for touch motion event!\n");
		return;
	}

	wl_list_for_each(tp, &seat->touch_point_list, link) {
		if (tp->id != id)
			continue;

		tp->x = sx;
		tp->y = sy;
		if (tp->widget->touch_motion_handler)
			(*tp->widget->touch_motion_handler)(tp->widget, seat, time,
							    id, sx, sy,
							    tp->widget->user_data);
		return;
	}
#endif
}

static void touch_handle_frame(void* data, struct wl_touch* wl_touch)
{
	UwacSeat* seat = data;
	assert(seat);

	UwacTouchFrameEnd* ev =
	    (UwacTouchFrameEnd*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_TOUCH_FRAME_END);
	if (!ev)
		return;

	ev->seat = seat;
	seat->touch_frame_started = false;
}

static void touch_handle_cancel(void* data, struct wl_touch* wl_touch)
{
	UwacSeat* seat = data;
	assert(seat);

	UwacTouchCancel* ev =
	    (UwacTouchCancel*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_TOUCH_CANCEL);
	if (!ev)
		return;

	ev->seat = seat;
	seat->touch_frame_started = false;

#if 0
	struct touch_point *tp, *tmp;

	DBG("touch_handle_cancel\n");

	if (!input->touch_focus) {
		DBG("No touch focus found for touch cancel event!\n");
		return;
	}

	wl_list_for_each_safe(tp, tmp, &input->touch_point_list, link) {
		if (tp->widget->touch_cancel_handler)
			(*tp->widget->touch_cancel_handler)(tp->widget, input,
							    tp->widget->user_data);

		wl_list_remove(&tp->link);
		free(tp);
	}
#endif
}

static void touch_handle_shape(void* data, struct wl_touch* wl_touch, int32_t id, wl_fixed_t major,
                               wl_fixed_t minor)
{
	UwacSeat* seat = data;
	assert(seat);

	// TODO
}

static void touch_handle_orientation(void* data, struct wl_touch* wl_touch, int32_t id,
                                     wl_fixed_t orientation)
{
	UwacSeat* seat = data;
	assert(seat);

	// TODO
}

static const struct wl_touch_listener touch_listener = {
	touch_handle_down,   touch_handle_up,    touch_handle_motion,     touch_handle_frame,
	touch_handle_cancel, touch_handle_shape, touch_handle_orientation
};

static void pointer_handle_enter(void* data, struct wl_pointer* pointer, uint32_t serial,
                                 struct wl_surface* surface, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
	UwacSeat* input = data;
	UwacWindow* window = NULL;
	UwacPointerEnterLeaveEvent* event = NULL;

	assert(input);

	double sx = wl_fixed_to_double(sx_w);
	double sy = wl_fixed_to_double(sy_w);

	if (!surface)
	{
		/* enter event for a window we've just destroyed */
		return;
	}

	input->display->serial = serial;
	input->display->pointer_focus_serial = serial;
	window = wl_surface_get_user_data(surface);
	if (window)
		window->pointer_enter_serial = serial;
	input->pointer_focus = window;
	input->sx = sx;
	input->sy = sy;

	event =
	    (UwacPointerEnterLeaveEvent*)UwacDisplayNewEvent(input->display, UWAC_EVENT_POINTER_ENTER);
	if (!event)
		return;

	event->seat = input;
	event->window = window;
	event->x = (uint32_t)lround(sx);
	event->y = (uint32_t)lround(sy);

	/* Apply cursor theme */
	set_cursor_image(input, serial);
}

static void pointer_handle_leave(void* data, struct wl_pointer* pointer, uint32_t serial,
                                 struct wl_surface* surface)
{
	UwacPointerEnterLeaveEvent* event = NULL;
	UwacWindow* window = NULL;
	UwacSeat* input = data;
	assert(input);

	input->display->serial = serial;

	event =
	    (UwacPointerEnterLeaveEvent*)UwacDisplayNewEvent(input->display, UWAC_EVENT_POINTER_LEAVE);
	if (!event)
		return;

	window = wl_surface_get_user_data(surface);

	event->seat = input;
	event->window = window;
}

static void pointer_handle_motion(void* data, struct wl_pointer* pointer, uint32_t time,
                                  wl_fixed_t sx_w, wl_fixed_t sy_w)
{
	UwacPointerMotionEvent* motion_event = NULL;
	UwacSeat* input = data;
	assert(input);

	UwacWindow* window = input->pointer_focus;
	if (!window || !window->display)
		return;

	int scale = window->display->actual_scale;
	int sx_i = wl_fixed_to_int(sx_w) * scale;
	int sy_i = wl_fixed_to_int(sy_w) * scale;
	double sx_d = wl_fixed_to_double(sx_w) * scale;
	double sy_d = wl_fixed_to_double(sy_w) * scale;

	if ((sx_i < 0) || (sy_i < 0))
		return;

	input->sx = sx_d;
	input->sy = sy_d;

	motion_event =
	    (UwacPointerMotionEvent*)UwacDisplayNewEvent(input->display, UWAC_EVENT_POINTER_MOTION);
	if (!motion_event)
		return;

	motion_event->seat = input;
	motion_event->window = window;
	motion_event->x = sx_i;
	motion_event->y = sy_i;
}

static void pointer_handle_button(void* data, struct wl_pointer* pointer, uint32_t serial,
                                  uint32_t time, uint32_t button, uint32_t state_w)
{
	UwacPointerButtonEvent* event = NULL;
	UwacSeat* seat = data;
	assert(seat);

	UwacWindow* window = seat->pointer_focus;

	seat->display->serial = serial;

	event = (UwacPointerButtonEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_POINTER_BUTTONS);
	if (!event)
		return;

	event->seat = seat;
	event->window = window;
	event->x = (uint32_t)lround(seat->sx);
	event->y = (uint32_t)lround(seat->sy);
	event->button = button;
	event->state = (enum wl_pointer_button_state)state_w;
}

static void pointer_handle_axis(void* data, struct wl_pointer* pointer, uint32_t time,
                                uint32_t axis, wl_fixed_t value)
{
	UwacPointerAxisEvent* event = NULL;
	UwacSeat* seat = data;
	assert(seat);

	UwacWindow* window = seat->pointer_focus;

	if (!window)
		return;

	event = (UwacPointerAxisEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_POINTER_AXIS);
	if (!event)
		return;

	event->seat = seat;
	event->window = window;
	event->x = (uint32_t)lround(seat->sx);
	event->y = (uint32_t)lround(seat->sy);
	event->axis = axis;
	event->value = value;
}

static void pointer_frame(void* data, struct wl_pointer* wl_pointer)
{
	UwacPointerFrameEvent* event = NULL;
	UwacSeat* seat = data;
	assert(seat);

	UwacWindow* window = seat->pointer_focus;

	if (!window)
		return;

	event = (UwacPointerFrameEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_POINTER_FRAME);
	if (!event)
		return;

	event->seat = seat;
	event->window = window;
}

static void pointer_axis_source(void* data, struct wl_pointer* wl_pointer, uint32_t axis_source)
{
	UwacPointerSourceEvent* event = NULL;
	UwacSeat* seat = data;
	assert(seat);

	UwacWindow* window = seat->pointer_focus;

	if (!window)
		return;

	event = (UwacPointerSourceEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_POINTER_SOURCE);
	if (!event)
		return;

	event->seat = seat;
	event->window = window;
	event->axis_source = axis_source;
}

static void pointer_axis_stop(void* data, struct wl_pointer* wl_pointer, uint32_t time,
                              uint32_t axis)
{
	UwacSeat* seat = data;
	assert(seat);
}

static void pointer_axis_discrete(void* data, struct wl_pointer* wl_pointer, uint32_t axis,
                                  int32_t discrete)
{
	/*UwacSeat *seat = data;*/
	UwacPointerAxisEvent* event = NULL;
	UwacSeat* seat = data;
	assert(seat);

	UwacWindow* window = seat->pointer_focus;

	if (!window)
		return;

	event =
	    (UwacPointerAxisEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_POINTER_AXIS_DISCRETE);
	if (!event)
		return;

	event->seat = seat;
	event->window = window;
	event->x = (uint32_t)lround(seat->sx);
	event->y = (uint32_t)lround(seat->sy);
	event->axis = axis;
	event->value = discrete;
}

static void pointer_axis_value120(void* data, struct wl_pointer* wl_pointer, uint32_t axis,
                                  int32_t value120)
{
	/*UwacSeat *seat = data;*/
	UwacPointerAxisEvent* event = NULL;
	UwacSeat* seat = data;
	assert(seat);

	UwacWindow* window = seat->pointer_focus;

	if (!window)
		return;

	event =
	    (UwacPointerAxisEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_POINTER_AXIS_DISCRETE);
	if (!event)
		return;

	event->seat = seat;
	event->window = window;
	event->x = (uint32_t)lround(seat->sx);
	event->y = (uint32_t)lround(seat->sy);
	event->axis = axis;
	event->value = value120 / 120;
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,  pointer_handle_leave, pointer_handle_motion, pointer_handle_button,
	pointer_handle_axis,   pointer_frame,        pointer_axis_source,   pointer_axis_stop,
	pointer_axis_discrete, pointer_axis_value120
};

static void seat_handle_capabilities(void* data, struct wl_seat* seat, uint32_t caps)
{
	UwacSeat* input = data;
	assert(input);

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer)
	{
		input->pointer = wl_seat_get_pointer(seat);
		wl_pointer_set_user_data(input->pointer, input);
		wl_pointer_add_listener(input->pointer, &pointer_listener, input);

		input->cursor_theme = wl_cursor_theme_load(NULL, 32, input->display->shm);
		if (!input->cursor_theme)
		{
			assert(uwacErrorHandler(input->display, UWAC_ERROR_NOMEMORY,
			                        "unable to get wayland cursor theme\n"));
			return;
		}

		input->default_cursor = wl_cursor_theme_get_cursor(input->cursor_theme, "left_ptr");
		if (!input->default_cursor)
		{
			assert(uwacErrorHandler(input->display, UWAC_ERROR_NOMEMORY,
			                        "unable to get wayland cursor left_ptr\n"));
			return;
		}
	}
	else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer)
	{
#ifdef WL_POINTER_RELEASE_SINCE_VERSION
		if (input->seat_version >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(input->pointer);
		else
#endif
			wl_pointer_destroy(input->pointer);
		if (input->cursor_theme)
			wl_cursor_theme_destroy(input->cursor_theme);

		input->default_cursor = NULL;
		input->cursor_theme = NULL;
		input->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard)
	{
		input->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_set_user_data(input->keyboard, input);
		wl_keyboard_add_listener(input->keyboard, &keyboard_listener, input);
	}
	else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard)
	{
#ifdef WL_KEYBOARD_RELEASE_SINCE_VERSION
		if (input->seat_version >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(input->keyboard);
		else
#endif
			wl_keyboard_destroy(input->keyboard);
		input->keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch)
	{
		input->touch = wl_seat_get_touch(seat);
		wl_touch_set_user_data(input->touch, input);
		wl_touch_add_listener(input->touch, &touch_listener, input);
	}
	else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch)
	{
#ifdef WL_TOUCH_RELEASE_SINCE_VERSION
		if (input->seat_version >= WL_TOUCH_RELEASE_SINCE_VERSION)
			wl_touch_release(input->touch);
		else
#endif
			wl_touch_destroy(input->touch);
		input->touch = NULL;
	}
}

static void seat_handle_name(void* data, struct wl_seat* seat, const char* name)
{
	UwacSeat* input = data;
	assert(input);

	if (input->name)
		free(input->name);

	input->name = strdup(name);
	if (!input->name)
		assert(uwacErrorHandler(input->display, UWAC_ERROR_NOMEMORY,
		                        "unable to strdup seat's name\n"));
}

static const struct wl_seat_listener seat_listener = { seat_handle_capabilities, seat_handle_name };

UwacSeat* UwacSeatNew(UwacDisplay* d, uint32_t id, uint32_t version)
{
	UwacSeat* ret = xzalloc(sizeof(UwacSeat));
	if (!ret)
		return NULL;

	ret->display = d;
	ret->seat_id = id;
	ret->seat_version = version;

	wl_array_init(&ret->pressed_keys);
	ret->xkb_context = xkb_context_new(0);
	if (!ret->xkb_context)
	{
		(void)fprintf(stderr, "%s: unable to allocate a xkb_context\n", __func__);
		goto fail;
	}

	ret->seat = wl_registry_bind(d->registry, id, &wl_seat_interface, version);
	wl_seat_add_listener(ret->seat, &seat_listener, ret);
	wl_seat_set_user_data(ret->seat, ret);

	ret->repeat_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (ret->repeat_timer_fd < 0)
	{
		(void)fprintf(stderr, "%s: error creating repeat timer\n", __func__);
		goto fail;
	}
	ret->repeat_task.run = keyboard_repeat_func;
	if (UwacDisplayWatchFd(d, ret->repeat_timer_fd, EPOLLIN, &ret->repeat_task) < 0)
	{
		(void)fprintf(stderr, "%s: error polling repeat timer\n", __func__);
		goto fail;
	}

	wl_list_insert(d->seats.prev, &ret->link);
	return ret;

fail:
	UwacSeatDestroy(ret);
	return NULL;
}

void UwacSeatDestroy(UwacSeat* s)
{
	if (!s)
		return;

	UwacSeatInhibitShortcuts(s, false);
	if (s->seat)
	{
#ifdef WL_SEAT_RELEASE_SINCE_VERSION
		if (s->seat_version >= WL_SEAT_RELEASE_SINCE_VERSION)
			wl_seat_release(s->seat);
		else
#endif
			wl_seat_destroy(s->seat);
	}
	s->seat = NULL;

	free(s->name);
	wl_array_release(&s->pressed_keys);

	xkb_state_unref(s->xkb.state);
	xkb_context_unref(s->xkb_context);

	if (s->pointer)
	{
#ifdef WL_POINTER_RELEASE_SINCE_VERSION
		if (s->seat_version >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(s->pointer);
		else
#endif
			wl_pointer_destroy(s->pointer);
	}

	if (s->touch)
	{
#ifdef WL_TOUCH_RELEASE_SINCE_VERSION
		if (s->seat_version >= WL_TOUCH_RELEASE_SINCE_VERSION)
			wl_touch_release(s->touch);
		else
#endif
			wl_touch_destroy(s->touch);
	}

	if (s->keyboard)
	{
#ifdef WL_KEYBOARD_RELEASE_SINCE_VERSION
		if (s->seat_version >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(s->keyboard);
		else
#endif
			wl_keyboard_destroy(s->keyboard);
	}

	if (s->data_device)
		wl_data_device_destroy(s->data_device);

	if (s->data_source)
		wl_data_source_destroy(s->data_source);

	if (s->pointer_surface)
		wl_surface_destroy(s->pointer_surface);

	free(s->pointer_image);
	free(s->pointer_data);

	wl_list_remove(&s->link);
	free(s);
}

const char* UwacSeatGetName(const UwacSeat* seat)
{
	assert(seat);
	return seat->name;
}

UwacSeatId UwacSeatGetId(const UwacSeat* seat)
{
	assert(seat);
	return seat->seat_id;
}

UwacReturnCode UwacSeatInhibitShortcuts(UwacSeat* s, bool inhibit)
{
	if (!s)
		return UWAC_ERROR_CLOSED;

	if (s->keyboard_inhibitor)
	{
		zwp_keyboard_shortcuts_inhibitor_v1_destroy(s->keyboard_inhibitor);
		s->keyboard_inhibitor = NULL;
	}
	if (inhibit && s->display && s->display->keyboard_inhibit_manager)
		s->keyboard_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
		    s->display->keyboard_inhibit_manager, s->keyboard_focus->surface, s->seat);

	if (inhibit && !s->keyboard_inhibitor)
		return UWAC_ERROR_INTERNAL;
	return UWAC_SUCCESS;
}

UwacReturnCode UwacSeatSetMouseCursor(UwacSeat* seat, const void* data, size_t length, size_t width,
                                      size_t height, size_t hot_x, size_t hot_y)
{
	if (!seat)
		return UWAC_ERROR_CLOSED;

	free(seat->pointer_image);
	seat->pointer_image = NULL;

	free(seat->pointer_data);
	seat->pointer_data = NULL;
	seat->pointer_size = 0;

	/* There is a cursor provided */
	if ((data != NULL) && (length != 0))
	{
		seat->pointer_image = xzalloc(sizeof(struct wl_cursor_image));
		if (!seat->pointer_image)
			return UWAC_ERROR_NOMEMORY;
		seat->pointer_image->width = width;
		seat->pointer_image->height = height;
		seat->pointer_image->hotspot_x = hot_x;
		seat->pointer_image->hotspot_y = hot_y;

		free(seat->pointer_data);
		seat->pointer_data = xmalloc(length);
		memcpy(seat->pointer_data, data, length);
		seat->pointer_size = length;

		seat->pointer_type = 2;
	}
	/* We want to use the system cursor */
	else if (length != 0)
	{
		seat->pointer_type = 0;
	}
	/* Hide the cursor */
	else
	{
		seat->pointer_type = 1;
	}
	if (seat && !seat->default_cursor)
		return UWAC_SUCCESS;
	return set_cursor_image(seat, seat->display->pointer_focus_serial);
}
