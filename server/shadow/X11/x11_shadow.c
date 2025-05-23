/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2011-2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2017 Armin Novak <armin.novak@thincast.com>
 * Copyright 2017 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/signal.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/path.h>
#include <winpr/synch.h>
#include <winpr/image.h>
#include <winpr/sysinfo.h>

#include <freerdp/log.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>

#include "x11_shadow.h"

#define TAG SERVER_TAG("shadow.x11")

// #define USE_SHADOW_BLEND_CURSOR

static UINT32 x11_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors);

#ifdef WITH_PAM

#include <security/pam_appl.h>

typedef struct
{
	const char* user;
	const char* domain;
	const char* password;
} SHADOW_PAM_AUTH_DATA;

typedef struct
{
	char* service_name;
	pam_handle_t* handle;
	struct pam_conv pamc;
	SHADOW_PAM_AUTH_DATA appdata;
} SHADOW_PAM_AUTH_INFO;

static int x11_shadow_pam_conv(int num_msg, const struct pam_message** msg,
                               struct pam_response** resp, void* appdata_ptr)
{
	int pam_status = PAM_CONV_ERR;
	SHADOW_PAM_AUTH_DATA* appdata = NULL;
	struct pam_response* response = NULL;
	WINPR_ASSERT(num_msg >= 0);
	appdata = (SHADOW_PAM_AUTH_DATA*)appdata_ptr;
	WINPR_ASSERT(appdata);

	if (!(response = (struct pam_response*)calloc((size_t)num_msg, sizeof(struct pam_response))))
		return PAM_BUF_ERR;

	for (int index = 0; index < num_msg; index++)
	{
		switch (msg[index]->msg_style)
		{
			case PAM_PROMPT_ECHO_ON:
				response[index].resp = _strdup(appdata->user);

				if (!response[index].resp)
					goto out_fail;

				response[index].resp_retcode = PAM_SUCCESS;
				break;

			case PAM_PROMPT_ECHO_OFF:
				response[index].resp = _strdup(appdata->password);

				if (!response[index].resp)
					goto out_fail;

				response[index].resp_retcode = PAM_SUCCESS;
				break;

			default:
				pam_status = PAM_CONV_ERR;
				goto out_fail;
		}
	}

	*resp = response;
	return PAM_SUCCESS;
out_fail:

	for (int index = 0; index < num_msg; ++index)
	{
		if (response[index].resp)
		{
			memset(response[index].resp, 0, strlen(response[index].resp));
			free(response[index].resp);
		}
	}

	memset(response, 0, sizeof(struct pam_response) * (size_t)num_msg);
	free(response);
	*resp = NULL;
	return pam_status;
}

static BOOL x11_shadow_pam_get_service_name(SHADOW_PAM_AUTH_INFO* info)
{
	const char* base = "/etc/pam.d";
	const char* hints[] = { "lightdm", "gdm", "xdm", "login", "sshd" };

	for (size_t x = 0; x < ARRAYSIZE(hints); x++)
	{
		char path[MAX_PATH] = { 0 };
		const char* hint = hints[x];

		(void)_snprintf(path, sizeof(path), "%s/%s", base, hint);
		if (winpr_PathFileExists(path))
		{

			info->service_name = _strdup(hint);
			return info->service_name != NULL;
		}
	}
	WLog_WARN(TAG, "Could not determine PAM service name");
	return FALSE;
}

static int x11_shadow_pam_authenticate(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                       const char* user, const char* domain, const char* password)
{
	int pam_status = 0;
	SHADOW_PAM_AUTH_INFO info = { 0 };
	WINPR_UNUSED(subsystem);
	WINPR_UNUSED(client);

	if (!x11_shadow_pam_get_service_name(&info))
		return -1;

	info.appdata.user = user;
	info.appdata.domain = domain;
	info.appdata.password = password;
	info.pamc.conv = &x11_shadow_pam_conv;
	info.pamc.appdata_ptr = &info.appdata;
	pam_status = pam_start(info.service_name, 0, &info.pamc, &info.handle);

	if (pam_status != PAM_SUCCESS)
	{
		WLog_ERR(TAG, "pam_start failure: %s", pam_strerror(info.handle, pam_status));
		return -1;
	}

	pam_status = pam_authenticate(info.handle, 0);

	if (pam_status != PAM_SUCCESS)
	{
		WLog_ERR(TAG, "pam_authenticate failure: %s", pam_strerror(info.handle, pam_status));
		return -1;
	}

	pam_status = pam_acct_mgmt(info.handle, 0);

	if (pam_status != PAM_SUCCESS)
	{
		WLog_ERR(TAG, "pam_acct_mgmt failure: %s", pam_strerror(info.handle, pam_status));
		return -1;
	}

	return 1;
}

#endif

static BOOL x11_shadow_input_synchronize_event(WINPR_ATTR_UNUSED rdpShadowSubsystem* subsystem,
                                               WINPR_ATTR_UNUSED rdpShadowClient* client,
                                               WINPR_ATTR_UNUSED UINT32 flags)
{
	/* TODO: Implement */
	WLog_WARN(TAG, "not implemented");
	return TRUE;
}

static BOOL x11_shadow_input_keyboard_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                            UINT16 flags, UINT8 code)
{
#ifdef WITH_XTEST
	x11ShadowSubsystem* x11 = (x11ShadowSubsystem*)subsystem;
	DWORD vkcode = 0;
	DWORD keycode = 0;
	DWORD scancode = 0;
	BOOL extended = FALSE;

	if (!client || !subsystem)
		return FALSE;

	if (flags & KBD_FLAGS_EXTENDED)
		extended = TRUE;

	scancode = code;
	if (extended)
		scancode |= KBDEXT;

	vkcode = GetVirtualKeyCodeFromVirtualScanCode(scancode, WINPR_KBD_TYPE_IBM_ENHANCED);

	if (extended)
		vkcode |= KBDEXT;

	keycode = GetKeycodeFromVirtualKeyCode(vkcode, WINPR_KEYCODE_TYPE_XKB);

	if (keycode != 0)
	{
		XLockDisplay(x11->display);
		XTestGrabControl(x11->display, True);

		if (flags & KBD_FLAGS_RELEASE)
			XTestFakeKeyEvent(x11->display, keycode, False, CurrentTime);
		else
			XTestFakeKeyEvent(x11->display, keycode, True, CurrentTime);

		XTestGrabControl(x11->display, False);
		XFlush(x11->display);
		XUnlockDisplay(x11->display);
	}
#else
	WLog_WARN(TAG, "KeyboardEvent not supported by backend, ignoring");
#endif
	return TRUE;
}

static BOOL x11_shadow_input_unicode_keyboard_event(WINPR_ATTR_UNUSED rdpShadowSubsystem* subsystem,
                                                    WINPR_ATTR_UNUSED rdpShadowClient* client,
                                                    WINPR_ATTR_UNUSED UINT16 flags,
                                                    WINPR_ATTR_UNUSED UINT16 code)
{
	/* TODO: Implement */
	WLog_WARN(TAG, "not implemented");
	return TRUE;
}

static BOOL x11_shadow_input_mouse_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                         UINT16 flags, UINT16 x, UINT16 y)
{
#ifdef WITH_XTEST
	x11ShadowSubsystem* x11 = (x11ShadowSubsystem*)subsystem;
	unsigned int button = 0;
	BOOL down = FALSE;
	rdpShadowServer* server = NULL;
	rdpShadowSurface* surface = NULL;

	if (!subsystem || !client)
		return FALSE;

	server = subsystem->server;

	if (!server)
		return FALSE;

	surface = server->surface;

	if (!surface)
		return FALSE;

	x11->lastMouseClient = client;
	x += surface->x;
	y += surface->y;
	XLockDisplay(x11->display);
	XTestGrabControl(x11->display, True);

	if (flags & PTR_FLAGS_WHEEL)
	{
		BOOL negative = FALSE;

		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			negative = TRUE;

		button = (negative) ? 5 : 4;
		XTestFakeButtonEvent(x11->display, button, True, (unsigned long)CurrentTime);
		XTestFakeButtonEvent(x11->display, button, False, (unsigned long)CurrentTime);
	}
	else if (flags & PTR_FLAGS_HWHEEL)
	{
		BOOL negative = FALSE;

		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			negative = TRUE;

		button = (negative) ? 7 : 6;
		XTestFakeButtonEvent(x11->display, button, True, (unsigned long)CurrentTime);
		XTestFakeButtonEvent(x11->display, button, False, (unsigned long)CurrentTime);
	}
	else
	{
		if (flags & PTR_FLAGS_MOVE)
			XTestFakeMotionEvent(x11->display, 0, x, y, CurrentTime);

		if (flags & PTR_FLAGS_BUTTON1)
			button = 1;
		else if (flags & PTR_FLAGS_BUTTON2)
			button = 3;
		else if (flags & PTR_FLAGS_BUTTON3)
			button = 2;

		if (flags & PTR_FLAGS_DOWN)
			down = TRUE;

		if (button)
			XTestFakeButtonEvent(x11->display, button, down, CurrentTime);
	}

	XTestGrabControl(x11->display, False);
	XFlush(x11->display);
	XUnlockDisplay(x11->display);
#else
	WLog_WARN(TAG, "MouseEvent not supported by backend, ignoring");
#endif
	return TRUE;
}

static BOOL x11_shadow_input_rel_mouse_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                             UINT16 flags, INT16 xDelta, INT16 yDelta)
{
#ifdef WITH_XTEST
	x11ShadowSubsystem* x11 = (x11ShadowSubsystem*)subsystem;
	WINPR_ASSERT(x11);

	unsigned int button = 0;
	BOOL down = FALSE;

	if (!subsystem || !client)
		return FALSE;

	rdpShadowServer* server = subsystem->server;

	if (!server)
		return FALSE;

	rdpShadowSurface* surface = server->surface;

	if (!surface)
		return FALSE;

	x11->lastMouseClient = client;

	XLockDisplay(x11->display);
	XTestGrabControl(x11->display, True);

	if (flags & PTR_FLAGS_MOVE)
		XTestFakeRelativeMotionEvent(x11->display, xDelta, yDelta, 0);

	if (flags & PTR_FLAGS_BUTTON1)
		button = 1;
	else if (flags & PTR_FLAGS_BUTTON2)
		button = 3;
	else if (flags & PTR_FLAGS_BUTTON3)
		button = 2;
	else if (flags & PTR_XFLAGS_BUTTON1)
		button = 4;
	else if (flags & PTR_XFLAGS_BUTTON2)
		button = 5;

	if (flags & PTR_FLAGS_DOWN)
		down = TRUE;

	if (button)
		XTestFakeButtonEvent(x11->display, button, down, CurrentTime);

	XTestGrabControl(x11->display, False);
	XFlush(x11->display);
	XUnlockDisplay(x11->display);
#else
	WLog_WARN(TAG, "RelMouseEvent not supported by backend, ignoring");
#endif
	return TRUE;
}

static BOOL x11_shadow_input_extended_mouse_event(rdpShadowSubsystem* subsystem,
                                                  rdpShadowClient* client, UINT16 flags, UINT16 x,
                                                  UINT16 y)
{
#ifdef WITH_XTEST
	x11ShadowSubsystem* x11 = (x11ShadowSubsystem*)subsystem;
	UINT button = 0;
	BOOL down = FALSE;
	rdpShadowServer* server = NULL;
	rdpShadowSurface* surface = NULL;

	if (!subsystem || !client)
		return FALSE;

	server = subsystem->server;

	if (!server)
		return FALSE;

	surface = server->surface;

	if (!surface)
		return FALSE;

	x11->lastMouseClient = client;
	x += surface->x;
	y += surface->y;
	XLockDisplay(x11->display);
	XTestGrabControl(x11->display, True);
	XTestFakeMotionEvent(x11->display, 0, x, y, CurrentTime);

	if (flags & PTR_XFLAGS_BUTTON1)
		button = 8;
	else if (flags & PTR_XFLAGS_BUTTON2)
		button = 9;

	if (flags & PTR_XFLAGS_DOWN)
		down = TRUE;

	if (button)
		XTestFakeButtonEvent(x11->display, button, down, CurrentTime);

	XTestGrabControl(x11->display, False);
	XFlush(x11->display);
	XUnlockDisplay(x11->display);
#else
	WLog_WARN(TAG, "ExtendedMouseEvent not supported by backend, ignoring");
#endif
	return TRUE;
}

static void x11_shadow_message_free(UINT32 id, SHADOW_MSG_OUT* msg)
{
	switch (id)
	{
		case SHADOW_MSG_OUT_POINTER_POSITION_UPDATE_ID:
			free(msg);
			break;

		case SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE_ID:
			free(((SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE*)msg)->xorMaskData);
			free(((SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE*)msg)->andMaskData);
			free(msg);
			break;

		default:
			WLog_ERR(TAG, "Unknown message id: %" PRIu32 "", id);
			free(msg);
			break;
	}
}

static int x11_shadow_pointer_position_update(x11ShadowSubsystem* subsystem)
{
	UINT32 msgId = SHADOW_MSG_OUT_POINTER_POSITION_UPDATE_ID;
	rdpShadowServer* server = NULL;
	SHADOW_MSG_OUT_POINTER_POSITION_UPDATE templateMsg = { 0 };
	int count = 0;

	if (!subsystem || !subsystem->common.server || !subsystem->common.server->clients)
		return -1;

	templateMsg.xPos = subsystem->common.pointerX;
	templateMsg.yPos = subsystem->common.pointerY;
	templateMsg.common.Free = x11_shadow_message_free;
	server = subsystem->common.server;
	ArrayList_Lock(server->clients);

	for (size_t index = 0; index < ArrayList_Count(server->clients); index++)
	{
		SHADOW_MSG_OUT_POINTER_POSITION_UPDATE* msg = NULL;
		rdpShadowClient* client = (rdpShadowClient*)ArrayList_GetItem(server->clients, index);

		/* Skip the client which send us the latest mouse event */
		if (client == subsystem->lastMouseClient)
			continue;

		msg = malloc(sizeof(templateMsg));

		if (!msg)
		{
			count = -1;
			break;
		}

		memcpy(msg, &templateMsg, sizeof(templateMsg));

		if (shadow_client_post_msg(client, NULL, msgId, (SHADOW_MSG_OUT*)msg, NULL))
			count++;
	}

	ArrayList_Unlock(server->clients);
	return count;
}

static int x11_shadow_pointer_alpha_update(x11ShadowSubsystem* subsystem)
{
	SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE* msg = NULL;
	UINT32 msgId = SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE_ID;
	msg = (SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE*)calloc(1,
	                                                   sizeof(SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE));

	if (!msg)
		return -1;

	msg->xHot = subsystem->cursorHotX;
	msg->yHot = subsystem->cursorHotY;
	msg->width = subsystem->cursorWidth;
	msg->height = subsystem->cursorHeight;

	if (shadow_subsystem_pointer_convert_alpha_pointer_data_to_format(
	        subsystem->cursorPixels, subsystem->format, TRUE, msg->width, msg->height, msg) < 0)
	{
		free(msg);
		return -1;
	}

	msg->common.Free = x11_shadow_message_free;
	return shadow_client_boardcast_msg(subsystem->common.server, NULL, msgId, (SHADOW_MSG_OUT*)msg,
	                                   NULL)
	           ? 1
	           : -1;
}

static int x11_shadow_query_cursor(x11ShadowSubsystem* subsystem, BOOL getImage)
{
	int x = 0;
	int y = 0;
	int n = 0;
	rdpShadowServer* server = NULL;
	rdpShadowSurface* surface = NULL;
	server = subsystem->common.server;
	surface = server->surface;

	if (getImage)
	{
#ifdef WITH_XFIXES
		UINT32* pDstPixel = NULL;
		XFixesCursorImage* ci = NULL;
		XLockDisplay(subsystem->display);
		ci = XFixesGetCursorImage(subsystem->display);
		XUnlockDisplay(subsystem->display);

		if (!ci)
			return -1;

		x = ci->x;
		y = ci->y;

		if (ci->width > subsystem->cursorMaxWidth)
			return -1;

		if (ci->height > subsystem->cursorMaxHeight)
			return -1;

		subsystem->cursorHotX = ci->xhot;
		subsystem->cursorHotY = ci->yhot;
		subsystem->cursorWidth = ci->width;
		subsystem->cursorHeight = ci->height;
		subsystem->cursorId = ci->cursor_serial;
		n = ci->width * ci->height;
		pDstPixel = (UINT32*)subsystem->cursorPixels;

		for (int k = 0; k < n; k++)
		{
			/* XFixesCursorImage.pixels is in *unsigned long*, which may be 8 bytes */
			*pDstPixel++ = (UINT32)ci->pixels[k];
		}

		XFree(ci);
		x11_shadow_pointer_alpha_update(subsystem);
#endif
	}
	else
	{
		UINT32 mask = 0;
		int win_x = 0;
		int win_y = 0;
		int root_x = 0;
		int root_y = 0;
		Window root = 0;
		Window child = 0;
		XLockDisplay(subsystem->display);

		if (!XQueryPointer(subsystem->display, subsystem->root_window, &root, &child, &root_x,
		                   &root_y, &win_x, &win_y, &mask))
		{
			XUnlockDisplay(subsystem->display);
			return -1;
		}

		XUnlockDisplay(subsystem->display);
		x = root_x;
		y = root_y;
	}

	/* Convert to offset based on current surface */
	if (surface)
	{
		x -= surface->x;
		y -= surface->y;
	}

	if ((x != (INT64)subsystem->common.pointerX) || (y != (INT64)subsystem->common.pointerY))
	{
		subsystem->common.pointerX = (UINT32)MAX(0, x);
		subsystem->common.pointerY = (UINT32)MAX(0, y);
		x11_shadow_pointer_position_update(subsystem);
	}

	return 1;
}

static int x11_shadow_handle_xevent(x11ShadowSubsystem* subsystem, XEvent* xevent)
{
	if (xevent->type == MotionNotify)
	{
	}

#ifdef WITH_XFIXES
	else if (xevent->type == subsystem->xfixes_cursor_notify_event)
	{
		x11_shadow_query_cursor(subsystem, TRUE);
	}

#endif
	else
	{
	}

	return 1;
}

#if defined(USE_SHADOW_BLEND_CURSOR)
static int x11_shadow_blend_cursor(x11ShadowSubsystem* subsystem)
{
	if (!subsystem)
		return -1;

	rdpShadowSurface* surface = subsystem->common.server->surface;
	UINT32 nXSrc = 0;
	UINT32 nYSrc = 0;
	UINT32 nWidth = subsystem->cursorWidth;
	UINT32 nHeight = subsystem->cursorHeight;
	INT64 nXDst = subsystem->common.pointerX - subsystem->cursorHotX;
	INT64 nYDst = subsystem->common.pointerY - subsystem->cursorHotY;

	if (nXDst >= surface->width)
		return 1;

	if (nXDst < 0)
	{
		nXDst *= -1;

		if (nXDst >= nWidth)
			return 1;

		nXSrc = (UINT32)nXDst;
		nWidth -= nXDst;
		nXDst = 0;
	}

	if (nYDst >= surface->height)
		return 1;

	if (nYDst < 0)
	{
		nYDst *= -1;

		if (nYDst >= nHeight)
			return 1;

		nYSrc = (UINT32)nYDst;
		nHeight -= nYDst;
		nYDst = 0;
	}

	if ((nXDst + nWidth) > surface->width)
		nWidth = (nXDst > surface->width) ? 0 : (UINT32)(surface->width - nXDst);

	if ((nYDst + nHeight) > surface->height)
		nHeight = (nYDst > surface->height) ? 0 : (UINT32)(surface->height - nYDst);

	const BYTE* pSrcData = subsystem->cursorPixels;
	const UINT32 nSrcStep = subsystem->cursorWidth * 4;
	BYTE* pDstData = surface->data;
	const UINT32 nDstStep = surface->scanline;

	for (size_t y = 0; y < nHeight; y++)
	{
		const BYTE* pSrcPixel = &pSrcData[((nYSrc + y) * nSrcStep) + (4ULL * nXSrc)];
		BYTE* pDstPixel = &pDstData[((WINPR_ASSERTING_INT_CAST(uint32_t, nYDst) + y) * nDstStep) +
		                            (4ULL * WINPR_ASSERTING_INT_CAST(uint32_t, nXDst))];

		for (size_t x = 0; x < nWidth; x++)
		{
			const BYTE B = *pSrcPixel++;
			const BYTE G = *pSrcPixel++;
			const BYTE R = *pSrcPixel++;
			const BYTE A = *pSrcPixel++;

			if (A == 0xFF)
			{
				pDstPixel[0] = B;
				pDstPixel[1] = G;
				pDstPixel[2] = R;
			}
			else
			{
				pDstPixel[0] = B + (pDstPixel[0] * (0xFF - A) + (0xFF / 2)) / 0xFF;
				pDstPixel[1] = G + (pDstPixel[1] * (0xFF - A) + (0xFF / 2)) / 0xFF;
				pDstPixel[2] = R + (pDstPixel[2] * (0xFF - A) + (0xFF / 2)) / 0xFF;
			}

			pDstPixel[3] = 0xFF;
			pDstPixel += 4;
		}
	}

	return 1;
}
#endif

static BOOL x11_shadow_check_resize(x11ShadowSubsystem* subsystem)
{
	XWindowAttributes attr;
	XLockDisplay(subsystem->display);
	XGetWindowAttributes(subsystem->display, subsystem->root_window, &attr);
	XUnlockDisplay(subsystem->display);

	if (attr.width != (INT64)subsystem->width || attr.height != (INT64)subsystem->height)
	{
		MONITOR_DEF* virtualScreen = &(subsystem->common.virtualScreen);

		/* Screen size changed. Refresh monitor definitions and trigger screen resize */
		subsystem->common.numMonitors = x11_shadow_enum_monitors(subsystem->common.monitors, 16);
		if (!shadow_screen_resize(subsystem->common.server->screen))
			return FALSE;

		WINPR_ASSERT(attr.width > 0);
		WINPR_ASSERT(attr.height > 0);

		subsystem->width = (UINT32)attr.width;
		subsystem->height = (UINT32)attr.height;

		virtualScreen->left = 0;
		virtualScreen->top = 0;
		virtualScreen->right = attr.width - 1;
		virtualScreen->bottom = attr.height - 1;
		virtualScreen->flags = 1;
		return TRUE;
	}

	return FALSE;
}

static int x11_shadow_error_handler_for_capture(Display* display, XErrorEvent* event)
{
	char msg[256];
	XGetErrorText(display, event->error_code, (char*)&msg, sizeof(msg));
	WLog_ERR(TAG, "X11 error: %s Error code: %x, request code: %x, minor code: %x", msg,
	         event->error_code, event->request_code, event->minor_code);

	/* Ignore BAD MATCH error during image capture. Abort in other case */
	if (event->error_code != BadMatch)
	{
		abort();
	}

	return 0;
}

static int x11_shadow_screen_grab(x11ShadowSubsystem* subsystem)
{
	int rc = 0;
	size_t count = 0;
	int status = -1;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	XImage* image = NULL;
	rdpShadowServer* server = NULL;
	rdpShadowSurface* surface = NULL;
	RECTANGLE_16 invalidRect;
	RECTANGLE_16 surfaceRect;
	const RECTANGLE_16* extents = NULL;
	server = subsystem->common.server;
	surface = server->surface;
	count = ArrayList_Count(server->clients);

	if (count < 1)
		return 1;

	EnterCriticalSection(&surface->lock);
	surfaceRect.left = 0;
	surfaceRect.top = 0;

	surfaceRect.right = WINPR_ASSERTING_INT_CAST(UINT16, surface->width);
	surfaceRect.bottom = WINPR_ASSERTING_INT_CAST(UINT16, surface->height);
	LeaveCriticalSection(&surface->lock);

	XLockDisplay(subsystem->display);
	/*
	 * Ignore BadMatch error during image capture. The screen size may be
	 * changed outside. We will resize to correct resolution at next frame
	 */
	XSetErrorHandler(x11_shadow_error_handler_for_capture);
#if defined(WITH_XDAMAGE)
	if (subsystem->use_xshm)
	{
		image = subsystem->fb_image;
		XCopyArea(subsystem->display, subsystem->root_window, subsystem->fb_pixmap,
		          subsystem->xshm_gc, 0, 0, subsystem->width, subsystem->height, 0, 0);

		EnterCriticalSection(&surface->lock);
		status = shadow_capture_compare_with_format(
		    surface->data, surface->format, surface->scanline, surface->width, surface->height,
		    (BYTE*)&(image->data[surface->width * 4ull]), subsystem->format,
		    WINPR_ASSERTING_INT_CAST(UINT32, image->bytes_per_line), &invalidRect);
		LeaveCriticalSection(&surface->lock);
	}
	else
#endif
	{
		EnterCriticalSection(&surface->lock);
		image = XGetImage(subsystem->display, subsystem->root_window, surface->x, surface->y,
		                  surface->width, surface->height, AllPlanes, ZPixmap);

		if (image)
		{
			status = shadow_capture_compare_with_format(
			    surface->data, surface->format, surface->scanline, surface->width, surface->height,
			    (BYTE*)image->data, subsystem->format,
			    WINPR_ASSERTING_INT_CAST(UINT32, image->bytes_per_line), &invalidRect);
		}
		LeaveCriticalSection(&surface->lock);
		if (!image)
		{
			/*
			 * BadMatch error happened. The size may have been changed again.
			 * Give up this frame and we will resize again in next frame
			 */
			goto fail_capture;
		}
	}

	/* Restore the default error handler */
	XSetErrorHandler(NULL);
	XSync(subsystem->display, False);
	XUnlockDisplay(subsystem->display);

	if (status)
	{
		BOOL empty = 0;
		EnterCriticalSection(&surface->lock);
		region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion), &invalidRect);
		region16_intersect_rect(&(surface->invalidRegion), &(surface->invalidRegion), &surfaceRect);
		empty = region16_is_empty(&(surface->invalidRegion));
		LeaveCriticalSection(&surface->lock);

		if (!empty)
		{
			BOOL success = 0;
			EnterCriticalSection(&surface->lock);
			extents = region16_extents(&(surface->invalidRegion));
			x = extents->left;
			y = extents->top;
			width = extents->right - extents->left;
			height = extents->bottom - extents->top;
			WINPR_ASSERT(image);
			WINPR_ASSERT(image->bytes_per_line >= 0);
			WINPR_ASSERT(width >= 0);
			WINPR_ASSERT(height >= 0);
			success = freerdp_image_copy_no_overlap(
			    surface->data, surface->format, surface->scanline,
			    WINPR_ASSERTING_INT_CAST(uint32_t, x), WINPR_ASSERTING_INT_CAST(uint32_t, y),
			    WINPR_ASSERTING_INT_CAST(uint32_t, width),
			    WINPR_ASSERTING_INT_CAST(uint32_t, height), (BYTE*)image->data, subsystem->format,
			    WINPR_ASSERTING_INT_CAST(uint32_t, image->bytes_per_line),
			    WINPR_ASSERTING_INT_CAST(UINT32, x), WINPR_ASSERTING_INT_CAST(UINT32, y), NULL,
			    FREERDP_FLIP_NONE);
			LeaveCriticalSection(&surface->lock);
			if (!success)
				goto fail_capture;

#if defined(USE_SHADOW_BLEND_CURSOR)
			if (x11_shadow_blend_cursor(subsystem) < 0)
				goto fail_capture;
#endif
			count = ArrayList_Count(server->clients);
			shadow_subsystem_frame_update(&subsystem->common);

			if (count == 1)
			{
				rdpShadowClient* client = NULL;
				client = (rdpShadowClient*)ArrayList_GetItem(server->clients, 0);

				if (client)
					subsystem->common.captureFrameRate =
					    shadow_encoder_preferred_fps(client->encoder);
			}

			EnterCriticalSection(&surface->lock);
			region16_clear(&(surface->invalidRegion));
			LeaveCriticalSection(&surface->lock);
		}
	}

	rc = 1;
fail_capture:
	if (!subsystem->use_xshm && image)
		XDestroyImage(image);

	if (rc != 1)
	{
		XSetErrorHandler(NULL);
		XSync(subsystem->display, False);
		XUnlockDisplay(subsystem->display);
	}

	return rc;
}

static int x11_shadow_subsystem_process_message(x11ShadowSubsystem* subsystem, wMessage* message)
{
	switch (message->id)
	{
		case SHADOW_MSG_IN_REFRESH_REQUEST_ID:
			shadow_subsystem_frame_update((rdpShadowSubsystem*)subsystem);
			break;

		default:
			WLog_ERR(TAG, "Unknown message id: %" PRIu32 "", message->id);
			break;
	}

	if (message->Free)
		message->Free(message);

	return 1;
}

static DWORD WINAPI x11_shadow_subsystem_thread(LPVOID arg)
{
	x11ShadowSubsystem* subsystem = (x11ShadowSubsystem*)arg;
	XEvent xevent;
	DWORD status = 0;
	DWORD nCount = 0;
	DWORD dwInterval = 0;
	UINT64 frameTime = 0;
	HANDLE events[32];
	wMessage message;
	wMessagePipe* MsgPipe = NULL;
	MsgPipe = subsystem->common.MsgPipe;
	nCount = 0;
	events[nCount++] = subsystem->common.event;
	events[nCount++] = MessageQueue_Event(MsgPipe->In);
	subsystem->common.captureFrameRate = 16;
	dwInterval = 1000 / subsystem->common.captureFrameRate;
	frameTime = GetTickCount64() + dwInterval;

	while (1)
	{
		const UINT64 cTime = GetTickCount64();
		const DWORD dwTimeout =
		    (DWORD)((cTime > frameTime) ? 0 : MIN(UINT32_MAX, frameTime - cTime));
		status = WaitForMultipleObjects(nCount, events, FALSE, dwTimeout);

		if (WaitForSingleObject(MessageQueue_Event(MsgPipe->In), 0) == WAIT_OBJECT_0)
		{
			if (MessageQueue_Peek(MsgPipe->In, &message, TRUE))
			{
				if (message.id == WMQ_QUIT)
					break;

				x11_shadow_subsystem_process_message(subsystem, &message);
			}
		}

		if (WaitForSingleObject(subsystem->common.event, 0) == WAIT_OBJECT_0)
		{
			XLockDisplay(subsystem->display);

			if (XEventsQueued(subsystem->display, QueuedAlready))
			{
				XNextEvent(subsystem->display, &xevent);
				x11_shadow_handle_xevent(subsystem, &xevent);
			}

			XUnlockDisplay(subsystem->display);
		}

		if ((status == WAIT_TIMEOUT) || (GetTickCount64() > frameTime))
		{
			x11_shadow_check_resize(subsystem);
			x11_shadow_screen_grab(subsystem);
			x11_shadow_query_cursor(subsystem, FALSE);
			dwInterval = 1000 / subsystem->common.captureFrameRate;
			frameTime += dwInterval;
		}
	}

	ExitThread(0);
	return 0;
}

static int x11_shadow_subsystem_base_init(x11ShadowSubsystem* subsystem)
{
	if (subsystem->display)
		return 1; /* initialize once */

	// NOLINTNEXTLINE(concurrency-mt-unsafe)
	if (!getenv("DISPLAY"))
	{
		// NOLINTNEXTLINE(concurrency-mt-unsafe)
		setenv("DISPLAY", ":0", 1);
	}

	if (!XInitThreads())
		return -1;

	subsystem->display = XOpenDisplay(NULL);

	if (!subsystem->display)
	{
		WLog_ERR(TAG, "failed to open display: %s", XDisplayName(NULL));
		return -1;
	}

	subsystem->xfds = ConnectionNumber(subsystem->display);
	subsystem->number = DefaultScreen(subsystem->display);
	subsystem->screen = ScreenOfDisplay(subsystem->display, subsystem->number);
	subsystem->depth = WINPR_ASSERTING_INT_CAST(UINT32, DefaultDepthOfScreen(subsystem->screen));
	subsystem->width = WINPR_ASSERTING_INT_CAST(UINT32, WidthOfScreen(subsystem->screen));
	subsystem->height = WINPR_ASSERTING_INT_CAST(UINT32, HeightOfScreen(subsystem->screen));
	subsystem->root_window = RootWindow(subsystem->display, subsystem->number);
	return 1;
}

static int x11_shadow_xfixes_init(x11ShadowSubsystem* subsystem)
{
#ifdef WITH_XFIXES
	int xfixes_event = 0;
	int xfixes_error = 0;
	int major = 0;
	int minor = 0;

	if (!XFixesQueryExtension(subsystem->display, &xfixes_event, &xfixes_error))
		return -1;

	if (!XFixesQueryVersion(subsystem->display, &major, &minor))
		return -1;

	subsystem->xfixes_cursor_notify_event = xfixes_event + XFixesCursorNotify;
	XFixesSelectCursorInput(subsystem->display, subsystem->root_window,
	                        XFixesDisplayCursorNotifyMask);
	return 1;
#else
	return -1;
#endif
}

static int x11_shadow_xinerama_init(x11ShadowSubsystem* subsystem)
{
#ifdef WITH_XINERAMA
	int xinerama_event = 0;
	int xinerama_error = 0;

	const int rc = x11_shadow_subsystem_base_init(subsystem);
	if (rc < 0)
		return rc;

	if (!XineramaQueryExtension(subsystem->display, &xinerama_event, &xinerama_error))
		return -1;

#if defined(WITH_XDAMAGE)
	int major = 0;
	int minor = 0;
	if (!XDamageQueryVersion(subsystem->display, &major, &minor))
		return -1;
#endif

	if (!XineramaIsActive(subsystem->display))
		return -1;

	return 1;
#else
	return -1;
#endif
}

static int x11_shadow_xdamage_init(x11ShadowSubsystem* subsystem)
{
#ifdef WITH_XDAMAGE
	int major = 0;
	int minor = 0;
	int damage_event = 0;
	int damage_error = 0;

	if (!subsystem->use_xfixes)
		return -1;

	if (!XDamageQueryExtension(subsystem->display, &damage_event, &damage_error))
		return -1;

	if (!XDamageQueryVersion(subsystem->display, &major, &minor))
		return -1;

	if (major < 1)
		return -1;

	subsystem->xdamage_notify_event = damage_event + XDamageNotify;
	subsystem->xdamage =
	    XDamageCreate(subsystem->display, subsystem->root_window, XDamageReportDeltaRectangles);

	if (!subsystem->xdamage)
		return -1;

#ifdef WITH_XFIXES
	subsystem->xdamage_region = XFixesCreateRegion(subsystem->display, NULL, 0);

	if (!subsystem->xdamage_region)
		return -1;

#endif
	return 1;
#else
	return -1;
#endif
}

static int x11_shadow_xshm_init(x11ShadowSubsystem* subsystem)
{
	Bool pixmaps = 0;
	int major = 0;
	int minor = 0;
	XGCValues values;

	if (!XShmQueryExtension(subsystem->display))
		return -1;

	if (!XShmQueryVersion(subsystem->display, &major, &minor, &pixmaps))
		return -1;

	if (!pixmaps)
		return -1;

	subsystem->fb_shm_info.shmid = -1;
	subsystem->fb_shm_info.shmaddr = (char*)-1;
	subsystem->fb_shm_info.readOnly = False;
	subsystem->fb_image =
	    XShmCreateImage(subsystem->display, subsystem->visual, subsystem->depth, ZPixmap, NULL,
	                    &(subsystem->fb_shm_info), subsystem->width, subsystem->height);

	if (!subsystem->fb_image)
	{
		WLog_ERR(TAG, "XShmCreateImage failed");
		return -1;
	}

	subsystem->fb_shm_info.shmid =
	    shmget(IPC_PRIVATE,
	           1ull * WINPR_ASSERTING_INT_CAST(uint32_t, subsystem->fb_image->bytes_per_line) *
	               WINPR_ASSERTING_INT_CAST(uint32_t, subsystem->fb_image->height),
	           IPC_CREAT | 0600);

	if (subsystem->fb_shm_info.shmid == -1)
	{
		WLog_ERR(TAG, "shmget failed");
		return -1;
	}

	subsystem->fb_shm_info.shmaddr = shmat(subsystem->fb_shm_info.shmid, 0, 0);
	subsystem->fb_image->data = subsystem->fb_shm_info.shmaddr;

	if (subsystem->fb_shm_info.shmaddr == ((char*)-1))
	{
		WLog_ERR(TAG, "shmat failed");
		return -1;
	}

	if (!XShmAttach(subsystem->display, &(subsystem->fb_shm_info)))
		return -1;

	XSync(subsystem->display, False);
	shmctl(subsystem->fb_shm_info.shmid, IPC_RMID, 0);
	subsystem->fb_pixmap = XShmCreatePixmap(
	    subsystem->display, subsystem->root_window, subsystem->fb_image->data,
	    &(subsystem->fb_shm_info), WINPR_ASSERTING_INT_CAST(uint32_t, subsystem->fb_image->width),
	    WINPR_ASSERTING_INT_CAST(uint32_t, subsystem->fb_image->height),
	    WINPR_ASSERTING_INT_CAST(uint32_t, subsystem->fb_image->depth));
	XSync(subsystem->display, False);

	if (!subsystem->fb_pixmap)
		return -1;

	values.subwindow_mode = IncludeInferiors;
	values.graphics_exposures = False;
#if defined(WITH_XDAMAGE)
	subsystem->xshm_gc = XCreateGC(subsystem->display, subsystem->root_window,
	                               GCSubwindowMode | GCGraphicsExposures, &values);
	XSetFunction(subsystem->display, subsystem->xshm_gc, GXcopy);
#endif
	XSync(subsystem->display, False);
	return 1;
}

UINT32 x11_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors)
{
	Display* display = NULL;
	int displayWidth = 0;
	int displayHeight = 0;
	int numMonitors = 0;

	// NOLINTNEXTLINE(concurrency-mt-unsafe)
	if (!getenv("DISPLAY"))
	{
		// NOLINTNEXTLINE(concurrency-mt-unsafe)
		setenv("DISPLAY", ":0", 1);
	}

	display = XOpenDisplay(NULL);

	if (!display)
	{
		WLog_ERR(TAG, "failed to open display: %s", XDisplayName(NULL));
		return 0;
	}

	displayWidth = WidthOfScreen(DefaultScreenOfDisplay(display));
	displayHeight = HeightOfScreen(DefaultScreenOfDisplay(display));
#ifdef WITH_XINERAMA
	{
#if defined(WITH_XDAMAGE)
		int major = 0;
		int minor = 0;
#endif
		int xinerama_event = 0;
		int xinerama_error = 0;
		XineramaScreenInfo* screens = NULL;

		const Bool xinerama = XineramaQueryExtension(display, &xinerama_event, &xinerama_error);
		const Bool damage =
#if defined(WITH_XDAMAGE)
		    XDamageQueryVersion(display, &major, &minor);
#else
		    False;
#endif

		if (xinerama && damage && XineramaIsActive(display))
		{
			screens = XineramaQueryScreens(display, &numMonitors);

			if (numMonitors > (INT64)maxMonitors)
				numMonitors = (int)maxMonitors;

			if (screens && (numMonitors > 0))
			{
				for (int index = 0; index < numMonitors; index++)
				{
					MONITOR_DEF* monitor = &monitors[index];
					const XineramaScreenInfo* screen = &screens[index];

					monitor->left = screen->x_org;
					monitor->top = screen->y_org;
					monitor->right = monitor->left + screen->width - 1;
					monitor->bottom = monitor->top + screen->height - 1;
					monitor->flags = (index == 0) ? 1 : 0;
				}
			}

			XFree(screens);
		}
	}
#endif
	XCloseDisplay(display);

	if (numMonitors < 1)
	{
		MONITOR_DEF* monitor = &monitors[0];
		numMonitors = 1;

		monitor->left = 0;
		monitor->top = 0;
		monitor->right = displayWidth - 1;
		monitor->bottom = displayHeight - 1;
		monitor->flags = 1;
	}

	errno = 0;
	return WINPR_ASSERTING_INT_CAST(uint32_t, numMonitors);
}

static int x11_shadow_subsystem_init(rdpShadowSubsystem* sub)
{
	int pf_count = 0;
	int vi_count = 0;
	int nextensions = 0;
	char** extensions = NULL;
	XVisualInfo* vi = NULL;
	XVisualInfo* vis = NULL;
	XVisualInfo template = { 0 };
	XPixmapFormatValues* pf = NULL;
	XPixmapFormatValues* pfs = NULL;

	x11ShadowSubsystem* subsystem = (x11ShadowSubsystem*)sub;

	if (!subsystem)
		return -1;

	subsystem->common.numMonitors = x11_shadow_enum_monitors(subsystem->common.monitors, 16);
	const int rc = x11_shadow_subsystem_base_init(subsystem);
	if (rc < 0)
		return rc;

	subsystem->format = (ImageByteOrder(subsystem->display) == LSBFirst) ? PIXEL_FORMAT_BGRA32
	                                                                     : PIXEL_FORMAT_ARGB32;

	if ((subsystem->depth != 24) && (subsystem->depth != 32))
	{
		WLog_ERR(TAG, "unsupported X11 server color depth: %" PRIu32, subsystem->depth);
		return -1;
	}

	extensions = XListExtensions(subsystem->display, &nextensions);

	if (!extensions || (nextensions < 0))
		return -1;

	for (int i = 0; i < nextensions; i++)
	{
		if (strcmp(extensions[i], "Composite") == 0)
			subsystem->composite = TRUE;
	}

	XFreeExtensionList(extensions);

	if (subsystem->composite)
		subsystem->use_xdamage = FALSE;

	pfs = XListPixmapFormats(subsystem->display, &pf_count);

	if (!pfs)
	{
		WLog_ERR(TAG, "XListPixmapFormats failed");
		return -1;
	}

	for (int i = 0; i < pf_count; i++)
	{
		pf = pfs + i;

		if (pf->depth == (INT64)subsystem->depth)
		{
			subsystem->bpp = WINPR_ASSERTING_INT_CAST(uint32_t, pf->bits_per_pixel);
			subsystem->scanline_pad = WINPR_ASSERTING_INT_CAST(uint32_t, pf->scanline_pad);
			break;
		}
	}

	XFree(pfs);
	template.class = TrueColor;
	template.screen = subsystem->number;
	vis = XGetVisualInfo(subsystem->display, VisualClassMask | VisualScreenMask, &template,
	                     &vi_count);

	if (!vis)
	{
		WLog_ERR(TAG, "XGetVisualInfo failed");
		return -1;
	}

	for (int i = 0; i < vi_count; i++)
	{
		vi = vis + i;

		if (vi->depth == (INT64)subsystem->depth)
		{
			subsystem->visual = vi->visual;
			break;
		}
	}

	XFree(vis);
	XSelectInput(subsystem->display, subsystem->root_window, SubstructureNotifyMask);
	subsystem->cursorMaxWidth = 256;
	subsystem->cursorMaxHeight = 256;
	subsystem->cursorPixels =
	    winpr_aligned_malloc(4ULL * subsystem->cursorMaxWidth * subsystem->cursorMaxHeight, 16);

	if (!subsystem->cursorPixels)
		return -1;

	x11_shadow_query_cursor(subsystem, TRUE);

	if (subsystem->use_xfixes)
	{
		if (x11_shadow_xfixes_init(subsystem) < 0)
			subsystem->use_xfixes = FALSE;
	}

	if (subsystem->use_xinerama)
	{
		if (x11_shadow_xinerama_init(subsystem) < 0)
			subsystem->use_xinerama = FALSE;
	}

	if (subsystem->use_xshm)
	{
		if (x11_shadow_xshm_init(subsystem) < 0)
			subsystem->use_xshm = FALSE;
	}

	if (subsystem->use_xdamage)
	{
		if (x11_shadow_xdamage_init(subsystem) < 0)
			subsystem->use_xdamage = FALSE;
	}

	if (!(subsystem->common.event =
	          CreateFileDescriptorEvent(NULL, FALSE, FALSE, subsystem->xfds, WINPR_FD_READ)))
		return -1;

	{
		MONITOR_DEF* virtualScreen = &(subsystem->common.virtualScreen);
		virtualScreen->left = 0;
		virtualScreen->top = 0;
		WINPR_ASSERT(subsystem->width <= INT32_MAX);
		WINPR_ASSERT(subsystem->height <= INT32_MAX);
		virtualScreen->right = (INT32)subsystem->width - 1;
		virtualScreen->bottom = (INT32)subsystem->height - 1;
		virtualScreen->flags = 1;
		WLog_INFO(TAG,
		          "X11 Extensions: XFixes: %" PRId32 " Xinerama: %" PRId32 " XDamage: %" PRId32
		          " XShm: %" PRId32 "",
		          subsystem->use_xfixes, subsystem->use_xinerama, subsystem->use_xdamage,
		          subsystem->use_xshm);
	}

	return 1;
}

static int x11_shadow_subsystem_uninit(rdpShadowSubsystem* sub)
{
	x11ShadowSubsystem* subsystem = (x11ShadowSubsystem*)sub;

	if (!subsystem)
		return -1;

	if (subsystem->display)
	{
		XCloseDisplay(subsystem->display);
		subsystem->display = NULL;
	}

	if (subsystem->common.event)
	{
		(void)CloseHandle(subsystem->common.event);
		subsystem->common.event = NULL;
	}

	if (subsystem->cursorPixels)
	{
		winpr_aligned_free(subsystem->cursorPixels);
		subsystem->cursorPixels = NULL;
	}

	return 1;
}

static int x11_shadow_subsystem_start(rdpShadowSubsystem* sub)
{
	x11ShadowSubsystem* subsystem = (x11ShadowSubsystem*)sub;

	if (!subsystem)
		return -1;

	if (!(subsystem->thread =
	          CreateThread(NULL, 0, x11_shadow_subsystem_thread, (void*)subsystem, 0, NULL)))
	{
		WLog_ERR(TAG, "Failed to create thread");
		return -1;
	}

	return 1;
}

static int x11_shadow_subsystem_stop(rdpShadowSubsystem* sub)
{
	x11ShadowSubsystem* subsystem = (x11ShadowSubsystem*)sub;

	if (!subsystem)
		return -1;

	if (subsystem->thread)
	{
		if (MessageQueue_PostQuit(subsystem->common.MsgPipe->In, 0))
			(void)WaitForSingleObject(subsystem->thread, INFINITE);

		(void)CloseHandle(subsystem->thread);
		subsystem->thread = NULL;
	}

	return 1;
}

static rdpShadowSubsystem* x11_shadow_subsystem_new(void)
{
	x11ShadowSubsystem* subsystem = NULL;
	subsystem = (x11ShadowSubsystem*)calloc(1, sizeof(x11ShadowSubsystem));

	if (!subsystem)
		return NULL;

#ifdef WITH_PAM
	subsystem->common.Authenticate = x11_shadow_pam_authenticate;
#endif
	subsystem->common.SynchronizeEvent = x11_shadow_input_synchronize_event;
	subsystem->common.KeyboardEvent = x11_shadow_input_keyboard_event;
	subsystem->common.UnicodeKeyboardEvent = x11_shadow_input_unicode_keyboard_event;
	subsystem->common.MouseEvent = x11_shadow_input_mouse_event;
	subsystem->common.RelMouseEvent = x11_shadow_input_rel_mouse_event;
	subsystem->common.ExtendedMouseEvent = x11_shadow_input_extended_mouse_event;
	subsystem->composite = FALSE;
	subsystem->use_xshm = FALSE; /* temporarily disabled */
	subsystem->use_xfixes = TRUE;
	subsystem->use_xdamage = FALSE;
	subsystem->use_xinerama = TRUE;
	return (rdpShadowSubsystem*)subsystem;
}

static void x11_shadow_subsystem_free(rdpShadowSubsystem* subsystem)
{
	if (!subsystem)
		return;

	x11_shadow_subsystem_uninit(subsystem);
	free(subsystem);
}

FREERDP_ENTRY_POINT(FREERDP_API const char* ShadowSubsystemName(void))
{
	return "X11";
}

FREERDP_ENTRY_POINT(FREERDP_API int ShadowSubsystemEntry(RDP_SHADOW_ENTRY_POINTS* pEntryPoints))
{
	if (!pEntryPoints)
		return -1;

	pEntryPoints->New = x11_shadow_subsystem_new;
	pEntryPoints->Free = x11_shadow_subsystem_free;
	pEntryPoints->Init = x11_shadow_subsystem_init;
	pEntryPoints->Uninit = x11_shadow_subsystem_uninit;
	pEntryPoints->Start = x11_shadow_subsystem_start;
	pEntryPoints->Stop = x11_shadow_subsystem_stop;
	pEntryPoints->EnumMonitors = x11_shadow_enum_monitors;
	return 1;
}
