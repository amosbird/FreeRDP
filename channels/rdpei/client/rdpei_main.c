/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Input Virtual Channel Extension
 *
 * Copyright 2013 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
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

#include <freerdp/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <winpr/crt.h>
#include <winpr/cast.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>
#include <winpr/sysinfo.h>
#include <winpr/cmdline.h>
#include <winpr/collections.h>

#include <freerdp/addin.h>
#include <freerdp/freerdp.h>
#include <freerdp/client/channels.h>

#include "rdpei_common.h"

#include "rdpei_main.h"

#define RDPEI_TAG CHANNELS_TAG("rdpei.client")

/**
 * Touch Input
 * http://msdn.microsoft.com/en-us/library/windows/desktop/dd562197/
 *
 * Windows Touch Input
 * http://msdn.microsoft.com/en-us/library/windows/desktop/dd317321/
 *
 * Input: Touch injection sample
 * http://code.msdn.microsoft.com/windowsdesktop/Touch-Injection-Sample-444d9bf7
 *
 * Pointer Input Message Reference
 * http://msdn.microsoft.com/en-us/library/hh454916/
 *
 * POINTER_INFO Structure
 * http://msdn.microsoft.com/en-us/library/hh454907/
 *
 * POINTER_TOUCH_INFO Structure
 * http://msdn.microsoft.com/en-us/library/hh454910/
 */

#define MAX_CONTACTS 64
#define MAX_PEN_CONTACTS 4

typedef struct
{
	GENERIC_DYNVC_PLUGIN base;

	RdpeiClientContext* context;

	UINT32 version;
	UINT32 features; /* SC_READY_MULTIPEN_INJECTION_SUPPORTED */
	UINT16 maxTouchContacts;
	UINT64 currentFrameTime;
	UINT64 previousFrameTime;
	RDPINPUT_CONTACT_POINT contactPoints[MAX_CONTACTS];

	UINT64 currentPenFrameTime;
	UINT64 previousPenFrameTime;
	UINT16 maxPenContacts;
	RDPINPUT_PEN_CONTACT_POINT penContactPoints[MAX_PEN_CONTACTS];

	CRITICAL_SECTION lock;
	rdpContext* rdpcontext;

	HANDLE thread;

	HANDLE event;
	UINT64 lastPollEventTime;
	BOOL running;
	BOOL async;
} RDPEI_PLUGIN;

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_send_frame(RdpeiClientContext* context, RDPINPUT_TOUCH_FRAME* frame);

#ifdef WITH_DEBUG_RDPEI
static const char* rdpei_eventid_string(UINT16 event)
{
	switch (event)
	{
		case EVENTID_SC_READY:
			return "EVENTID_SC_READY";
		case EVENTID_CS_READY:
			return "EVENTID_CS_READY";
		case EVENTID_TOUCH:
			return "EVENTID_TOUCH";
		case EVENTID_SUSPEND_TOUCH:
			return "EVENTID_SUSPEND_TOUCH";
		case EVENTID_RESUME_TOUCH:
			return "EVENTID_RESUME_TOUCH";
		case EVENTID_DISMISS_HOVERING_CONTACT:
			return "EVENTID_DISMISS_HOVERING_CONTACT";
		case EVENTID_PEN:
			return "EVENTID_PEN";
		default:
			return "EVENTID_UNKNOWN";
	}
}
#endif

static RDPINPUT_CONTACT_POINT* rdpei_contact(RDPEI_PLUGIN* rdpei, INT32 externalId, BOOL active)
{
	for (UINT16 i = 0; i < rdpei->maxTouchContacts; i++)
	{
		RDPINPUT_CONTACT_POINT* contactPoint = &rdpei->contactPoints[i];

		if (!contactPoint->active && active)
			continue;
		else if (!contactPoint->active && !active)
		{
			contactPoint->contactId = i;
			contactPoint->externalId = externalId;
			contactPoint->active = TRUE;
			return contactPoint;
		}
		else if (contactPoint->externalId == externalId)
		{
			return contactPoint;
		}
	}
	return NULL;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_add_frame(RdpeiClientContext* context)
{
	RDPEI_PLUGIN* rdpei = NULL;
	RDPINPUT_TOUCH_FRAME frame = { 0 };
	RDPINPUT_CONTACT_DATA contacts[MAX_CONTACTS] = { 0 };

	if (!context || !context->handle)
		return ERROR_INTERNAL_ERROR;

	rdpei = (RDPEI_PLUGIN*)context->handle;
	frame.contacts = contacts;

	for (UINT16 i = 0; i < rdpei->maxTouchContacts; i++)
	{
		RDPINPUT_CONTACT_POINT* contactPoint = &rdpei->contactPoints[i];
		RDPINPUT_CONTACT_DATA* contact = &contactPoint->data;

		if (contactPoint->dirty)
		{
			contacts[frame.contactCount] = *contact;
			rdpei->contactPoints[i].dirty = FALSE;
			frame.contactCount++;
		}
		else if (contactPoint->active)
		{
			if (contact->contactFlags & RDPINPUT_CONTACT_FLAG_DOWN)
			{
				contact->contactFlags = RDPINPUT_CONTACT_FLAG_UPDATE;
				contact->contactFlags |= RDPINPUT_CONTACT_FLAG_INRANGE;
				contact->contactFlags |= RDPINPUT_CONTACT_FLAG_INCONTACT;
			}

			contacts[frame.contactCount] = *contact;
			frame.contactCount++;
		}
		if (contact->contactFlags & RDPINPUT_CONTACT_FLAG_UP)
		{
			contactPoint->active = FALSE;
			contactPoint->externalId = 0;
			contactPoint->contactId = 0;
		}
	}

	if (frame.contactCount > 0)
	{
		UINT error = rdpei_send_frame(context, &frame);
		if (error != CHANNEL_RC_OK)
		{
			WLog_Print(rdpei->base.log, WLOG_ERROR,
			           "rdpei_send_frame failed with error %" PRIu32 "!", error);
			return error;
		}
	}
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_send_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s, UINT16 eventId,
                           size_t pduLength)
{
	UINT status = 0;

	if (!callback || !s || !callback->channel || !callback->channel->Write)
		return ERROR_INTERNAL_ERROR;

	if (pduLength > UINT32_MAX)
		return ERROR_INVALID_PARAMETER;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
	if (!rdpei)
		return ERROR_INTERNAL_ERROR;

	Stream_SetPosition(s, 0);
	Stream_Write_UINT16(s, eventId);   /* eventId (2 bytes) */
	Stream_Write_UINT32(s, (UINT32)pduLength); /* pduLength (4 bytes) */
	Stream_SetPosition(s, Stream_Length(s));
	status = callback->channel->Write(callback->channel, (UINT32)Stream_Length(s), Stream_Buffer(s),
	                                  NULL);
#ifdef WITH_DEBUG_RDPEI
	WLog_Print(rdpei->base.log, WLOG_DEBUG,
	           "rdpei_send_pdu: eventId: %" PRIu16 " (%s) length: %" PRIu32 " status: %" PRIu32 "",
	           eventId, rdpei_eventid_string(eventId), pduLength, status);
#endif
	return status;
}

static UINT rdpei_write_pen_frame(wStream* s, const RDPINPUT_PEN_FRAME* frame)
{
	if (!s || !frame)
		return ERROR_INTERNAL_ERROR;

	if (!rdpei_write_2byte_unsigned(s, frame->contactCount))
		return ERROR_OUTOFMEMORY;
	if (!rdpei_write_8byte_unsigned(s, frame->frameOffset))
		return ERROR_OUTOFMEMORY;
	for (UINT16 x = 0; x < frame->contactCount; x++)
	{
		const RDPINPUT_PEN_CONTACT* contact = &frame->contacts[x];

		if (!Stream_EnsureRemainingCapacity(s, 1))
			return ERROR_OUTOFMEMORY;
		Stream_Write_UINT8(s, contact->deviceId);
		if (!rdpei_write_2byte_unsigned(s, contact->fieldsPresent))
			return ERROR_OUTOFMEMORY;
		if (!rdpei_write_4byte_signed(s, contact->x))
			return ERROR_OUTOFMEMORY;
		if (!rdpei_write_4byte_signed(s, contact->y))
			return ERROR_OUTOFMEMORY;
		if (!rdpei_write_4byte_unsigned(s, contact->contactFlags))
			return ERROR_OUTOFMEMORY;
		if (contact->fieldsPresent & RDPINPUT_PEN_CONTACT_PENFLAGS_PRESENT)
		{
			if (!rdpei_write_4byte_unsigned(s, contact->penFlags))
				return ERROR_OUTOFMEMORY;
		}
		if (contact->fieldsPresent & RDPINPUT_PEN_CONTACT_PRESSURE_PRESENT)
		{
			if (!rdpei_write_4byte_unsigned(s, contact->pressure))
				return ERROR_OUTOFMEMORY;
		}
		if (contact->fieldsPresent & RDPINPUT_PEN_CONTACT_ROTATION_PRESENT)
		{
			if (!rdpei_write_2byte_unsigned(s, contact->rotation))
				return ERROR_OUTOFMEMORY;
		}
		if (contact->fieldsPresent & RDPINPUT_PEN_CONTACT_TILTX_PRESENT)
		{
			if (!rdpei_write_2byte_signed(s, contact->tiltX))
				return ERROR_OUTOFMEMORY;
		}
		if (contact->fieldsPresent & RDPINPUT_PEN_CONTACT_TILTY_PRESENT)
		{
			if (!rdpei_write_2byte_signed(s, contact->tiltY))
				return ERROR_OUTOFMEMORY;
		}
	}
	return CHANNEL_RC_OK;
}

static UINT rdpei_send_pen_event_pdu(GENERIC_CHANNEL_CALLBACK* callback, size_t frameOffset,
                                     const RDPINPUT_PEN_FRAME* frames, size_t count)
{
	UINT status = 0;
	wStream* s = NULL;

	WINPR_ASSERT(callback);

	if (frameOffset > UINT32_MAX)
		return ERROR_INVALID_PARAMETER;
	if (count > UINT16_MAX)
		return ERROR_INVALID_PARAMETER;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
	if (!rdpei)
		return ERROR_INTERNAL_ERROR;

	if (!frames || (count == 0))
		return ERROR_INTERNAL_ERROR;

	s = Stream_New(NULL, 64);

	if (!s)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Seek(s, RDPINPUT_HEADER_LENGTH);
	/**
	 * the time that has elapsed (in milliseconds) from when the oldest touch frame
	 * was generated to when it was encoded for transmission by the client.
	 */
	rdpei_write_4byte_unsigned(s,
	                           (UINT32)frameOffset); /* encodeTime (FOUR_BYTE_UNSIGNED_INTEGER) */
	rdpei_write_2byte_unsigned(s, (UINT16)count);    /* (frameCount) TWO_BYTE_UNSIGNED_INTEGER */

	for (size_t x = 0; x < count; x++)
	{
		if ((status = rdpei_write_pen_frame(s, &frames[x])))
		{
			WLog_Print(rdpei->base.log, WLOG_ERROR,
			           "rdpei_write_pen_frame failed with error %" PRIu32 "!", status);
			Stream_Free(s, TRUE);
			return status;
		}
	}
	Stream_SealLength(s);

	status = rdpei_send_pdu(callback, s, EVENTID_PEN, Stream_Length(s));
	Stream_Free(s, TRUE);
	return status;
}

static UINT rdpei_send_pen_frame(RdpeiClientContext* context, RDPINPUT_PEN_FRAME* frame)
{
	const UINT64 currentTime = GetTickCount64();
	RDPEI_PLUGIN* rdpei = NULL;
	GENERIC_CHANNEL_CALLBACK* callback = NULL;
	UINT error = 0;

	if (!context)
		return ERROR_INTERNAL_ERROR;
	rdpei = (RDPEI_PLUGIN*)context->handle;
	if (!rdpei || !rdpei->base.listener_callback)
		return ERROR_INTERNAL_ERROR;
	if (!rdpei || !rdpei->rdpcontext)
		return ERROR_INTERNAL_ERROR;
	if (freerdp_settings_get_bool(rdpei->rdpcontext->settings, FreeRDP_SuspendInput))
		return CHANNEL_RC_OK;

	callback = rdpei->base.listener_callback->channel_callback;
	/* Just ignore the event if the channel is not connected */
	if (!callback)
		return CHANNEL_RC_OK;

	if (!rdpei->previousPenFrameTime && !rdpei->currentPenFrameTime)
	{
		rdpei->currentPenFrameTime = currentTime;
		frame->frameOffset = 0;
	}
	else
	{
		rdpei->currentPenFrameTime = currentTime;
		frame->frameOffset = rdpei->currentPenFrameTime - rdpei->previousPenFrameTime;
	}

	const size_t off = WINPR_ASSERTING_INT_CAST(size_t, frame->frameOffset);
	error = rdpei_send_pen_event_pdu(callback, off, frame, 1);
	if (error)
		return error;

	rdpei->previousPenFrameTime = rdpei->currentPenFrameTime;
	return error;
}

static UINT rdpei_add_pen_frame(RdpeiClientContext* context)
{
	RDPEI_PLUGIN* rdpei = NULL;
	RDPINPUT_PEN_FRAME penFrame = { 0 };
	RDPINPUT_PEN_CONTACT penContacts[MAX_PEN_CONTACTS] = { 0 };

	if (!context || !context->handle)
		return ERROR_INTERNAL_ERROR;

	rdpei = (RDPEI_PLUGIN*)context->handle;

	penFrame.contacts = penContacts;

	for (UINT16 i = 0; i < rdpei->maxPenContacts; i++)
	{
		RDPINPUT_PEN_CONTACT_POINT* contact = &(rdpei->penContactPoints[i]);

		if (contact->dirty)
		{
			penContacts[penFrame.contactCount++] = contact->data;
			contact->dirty = FALSE;
		}
		else if (contact->active)
		{
			if (contact->data.contactFlags & RDPINPUT_CONTACT_FLAG_DOWN)
			{
				contact->data.contactFlags = RDPINPUT_CONTACT_FLAG_UPDATE;
				contact->data.contactFlags |= RDPINPUT_CONTACT_FLAG_INRANGE;
				contact->data.contactFlags |= RDPINPUT_CONTACT_FLAG_INCONTACT;
			}

			penContacts[penFrame.contactCount++] = contact->data;
		}
		if (contact->data.contactFlags & RDPINPUT_CONTACT_FLAG_CANCELED)
		{
			contact->externalId = 0;
			contact->active = FALSE;
		}
	}

	if (penFrame.contactCount > 0)
		return rdpei_send_pen_frame(context, &penFrame);
	return CHANNEL_RC_OK;
}

static UINT rdpei_update(wLog* log, RdpeiClientContext* context)
{
	UINT error = rdpei_add_frame(context);
	if (error != CHANNEL_RC_OK)
	{
		WLog_Print(log, WLOG_ERROR, "rdpei_add_frame failed with error %" PRIu32 "!", error);
		return error;
	}

	return rdpei_add_pen_frame(context);
}

static BOOL rdpei_poll_run_unlocked(rdpContext* context, void* userdata)
{
	RDPEI_PLUGIN* rdpei = userdata;
	WINPR_ASSERT(rdpei);
	WINPR_ASSERT(context);

	const UINT64 now = GetTickCount64();

	/* Send an event every ~20ms */
	if ((now < rdpei->lastPollEventTime) || (now - rdpei->lastPollEventTime < 20ULL))
		return TRUE;

	rdpei->lastPollEventTime = now;

	const UINT error = rdpei_update(rdpei->base.log, rdpei->context);

	(void)ResetEvent(rdpei->event);

	if (error != CHANNEL_RC_OK)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR, "rdpei_add_frame failed with error %" PRIu32 "!",
		           error);
		setChannelError(context, error, "rdpei_add_frame reported an error");
		return FALSE;
	}

	return TRUE;
}

static BOOL rdpei_poll_run(rdpContext* context, void* userdata)
{
	RDPEI_PLUGIN* rdpei = userdata;
	WINPR_ASSERT(rdpei);

	EnterCriticalSection(&rdpei->lock);
	BOOL rc = rdpei_poll_run_unlocked(context, userdata);
	LeaveCriticalSection(&rdpei->lock);
	return rc;
}

static DWORD WINAPI rdpei_periodic_update(LPVOID arg)
{
	DWORD status = 0;
	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)arg;
	UINT error = CHANNEL_RC_OK;
	RdpeiClientContext* context = NULL;

	if (!rdpei)
	{
		error = ERROR_INVALID_PARAMETER;
		goto out;
	}

	context = rdpei->context;

	if (!context)
	{
		error = ERROR_INVALID_PARAMETER;
		goto out;
	}

	while (rdpei->running)
	{
		status = WaitForSingleObject(rdpei->event, 20);

		if (status == WAIT_FAILED)
		{
			error = GetLastError();
			WLog_Print(rdpei->base.log, WLOG_ERROR,
			           "WaitForMultipleObjects failed with error %" PRIu32 "!", error);
			break;
		}

		if (!rdpei_poll_run(rdpei->rdpcontext, rdpei))
			error = ERROR_INTERNAL_ERROR;
	}

out:

	if (error && rdpei && rdpei->rdpcontext)
		setChannelError(rdpei->rdpcontext, error, "rdpei_schedule_thread reported an error");

	if (rdpei)
		rdpei->running = FALSE;

	ExitThread(error);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_send_cs_ready_pdu(GENERIC_CHANNEL_CALLBACK* callback)
{
	UINT status = 0;
	wStream* s = NULL;
	UINT32 flags = 0;
	UINT32 pduLength = 0;
	RDPEI_PLUGIN* rdpei = NULL;

	if (!callback || !callback->plugin)
		return ERROR_INTERNAL_ERROR;
	rdpei = (RDPEI_PLUGIN*)callback->plugin;

	flags |= CS_READY_FLAGS_SHOW_TOUCH_VISUALS & rdpei->context->clientFeaturesMask;
	if (rdpei->version > RDPINPUT_PROTOCOL_V10)
		flags |= CS_READY_FLAGS_DISABLE_TIMESTAMP_INJECTION & rdpei->context->clientFeaturesMask;
	if (rdpei->features & SC_READY_MULTIPEN_INJECTION_SUPPORTED)
		flags |= CS_READY_FLAGS_ENABLE_MULTIPEN_INJECTION & rdpei->context->clientFeaturesMask;

	pduLength = RDPINPUT_HEADER_LENGTH + 10;
	s = Stream_New(NULL, pduLength);

	if (!s)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Seek(s, RDPINPUT_HEADER_LENGTH);
	Stream_Write_UINT32(s, flags);                   /* flags (4 bytes) */
	Stream_Write_UINT32(s, rdpei->version);          /* protocolVersion (4 bytes) */
	Stream_Write_UINT16(s, rdpei->maxTouchContacts); /* maxTouchContacts (2 bytes) */
	Stream_SealLength(s);
	status = rdpei_send_pdu(callback, s, EVENTID_CS_READY, pduLength);
	Stream_Free(s, TRUE);
	return status;
}

#if defined(WITH_DEBUG_RDPEI)
static void rdpei_print_contact_flags(wLog* log, UINT32 contactFlags)
{
	if (contactFlags & RDPINPUT_CONTACT_FLAG_DOWN)
		WLog_Print(log, WLOG_DEBUG, " RDPINPUT_CONTACT_FLAG_DOWN");

	if (contactFlags & RDPINPUT_CONTACT_FLAG_UPDATE)
		WLog_Print(log, WLOG_DEBUG, " RDPINPUT_CONTACT_FLAG_UPDATE");

	if (contactFlags & RDPINPUT_CONTACT_FLAG_UP)
		WLog_Print(log, WLOG_DEBUG, " RDPINPUT_CONTACT_FLAG_UP");

	if (contactFlags & RDPINPUT_CONTACT_FLAG_INRANGE)
		WLog_Print(log, WLOG_DEBUG, " RDPINPUT_CONTACT_FLAG_INRANGE");

	if (contactFlags & RDPINPUT_CONTACT_FLAG_INCONTACT)
		WLog_Print(log, WLOG_DEBUG, " RDPINPUT_CONTACT_FLAG_INCONTACT");

	if (contactFlags & RDPINPUT_CONTACT_FLAG_CANCELED)
		WLog_Print(log, WLOG_DEBUG, " RDPINPUT_CONTACT_FLAG_CANCELED");
}
#endif

static INT16 bounded(INT32 val)
{
	if (val < INT16_MIN)
		return INT16_MIN;
	if (val > INT16_MAX)
		return INT16_MAX;
	return (INT16)val;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_write_touch_frame(wLog* log, wStream* s, RDPINPUT_TOUCH_FRAME* frame)
{
	int rectSize = 2;
	RDPINPUT_CONTACT_DATA* contact = NULL;
	if (!s || !frame)
		return ERROR_INTERNAL_ERROR;
#ifdef WITH_DEBUG_RDPEI
	WLog_Print(log, WLOG_DEBUG, "contactCount: %" PRIu32 "", frame->contactCount);
	WLog_Print(log, WLOG_DEBUG, "frameOffset: 0x%016" PRIX64 "", frame->frameOffset);
#endif
	rdpei_write_2byte_unsigned(s,
	                           frame->contactCount); /* contactCount (TWO_BYTE_UNSIGNED_INTEGER) */
	/**
	 * the time offset from the previous frame (in microseconds).
	 * If this is the first frame being transmitted then this field MUST be set to zero.
	 */
	rdpei_write_8byte_unsigned(s, frame->frameOffset *
	                                  1000); /* frameOffset (EIGHT_BYTE_UNSIGNED_INTEGER) */

	if (!Stream_EnsureRemainingCapacity(s, (size_t)frame->contactCount * 64))
	{
		WLog_Print(log, WLOG_ERROR, "Stream_EnsureRemainingCapacity failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	for (UINT32 index = 0; index < frame->contactCount; index++)
	{
		contact = &frame->contacts[index];
		contact->fieldsPresent |= CONTACT_DATA_CONTACTRECT_PRESENT;
		contact->contactRectLeft = bounded(contact->x - rectSize);
		contact->contactRectTop = bounded(contact->y - rectSize);
		contact->contactRectRight = bounded(contact->x + rectSize);
		contact->contactRectBottom = bounded(contact->y + rectSize);
#ifdef WITH_DEBUG_RDPEI
		WLog_Print(log, WLOG_DEBUG, "contact[%" PRIu32 "].contactId: %" PRIu32 "", index,
		           contact->contactId);
		WLog_Print(log, WLOG_DEBUG, "contact[%" PRIu32 "].fieldsPresent: %" PRIu32 "", index,
		           contact->fieldsPresent);
		WLog_Print(log, WLOG_DEBUG, "contact[%" PRIu32 "].x: %" PRId32 "", index, contact->x);
		WLog_Print(log, WLOG_DEBUG, "contact[%" PRIu32 "].y: %" PRId32 "", index, contact->y);
		WLog_Print(log, WLOG_DEBUG, "contact[%" PRIu32 "].contactFlags: 0x%08" PRIX32 "", index,
		           contact->contactFlags);
		rdpei_print_contact_flags(log, contact->contactFlags);
#endif
		Stream_Write_UINT8(
		    s, WINPR_ASSERTING_INT_CAST(uint8_t, contact->contactId)); /* contactId (1 byte) */
		/* fieldsPresent (TWO_BYTE_UNSIGNED_INTEGER) */
		rdpei_write_2byte_unsigned(s, contact->fieldsPresent);
		rdpei_write_4byte_signed(s, contact->x); /* x (FOUR_BYTE_SIGNED_INTEGER) */
		rdpei_write_4byte_signed(s, contact->y); /* y (FOUR_BYTE_SIGNED_INTEGER) */
		/* contactFlags (FOUR_BYTE_UNSIGNED_INTEGER) */
		rdpei_write_4byte_unsigned(s, contact->contactFlags);

		if (contact->fieldsPresent & CONTACT_DATA_CONTACTRECT_PRESENT)
		{
			/* contactRectLeft (TWO_BYTE_SIGNED_INTEGER) */
			rdpei_write_2byte_signed(s, contact->contactRectLeft);
			/* contactRectTop (TWO_BYTE_SIGNED_INTEGER) */
			rdpei_write_2byte_signed(s, contact->contactRectTop);
			/* contactRectRight (TWO_BYTE_SIGNED_INTEGER) */
			rdpei_write_2byte_signed(s, contact->contactRectRight);
			/* contactRectBottom (TWO_BYTE_SIGNED_INTEGER) */
			rdpei_write_2byte_signed(s, contact->contactRectBottom);
		}

		if (contact->fieldsPresent & CONTACT_DATA_ORIENTATION_PRESENT)
		{
			/* orientation (FOUR_BYTE_UNSIGNED_INTEGER) */
			rdpei_write_4byte_unsigned(s, contact->orientation);
		}

		if (contact->fieldsPresent & CONTACT_DATA_PRESSURE_PRESENT)
		{
			/* pressure (FOUR_BYTE_UNSIGNED_INTEGER) */
			rdpei_write_4byte_unsigned(s, contact->pressure);
		}
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_send_touch_event_pdu(GENERIC_CHANNEL_CALLBACK* callback,
                                       RDPINPUT_TOUCH_FRAME* frame)
{
	UINT status = 0;

	WINPR_ASSERT(callback);

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
	if (!rdpei || !rdpei->rdpcontext)
		return ERROR_INTERNAL_ERROR;
	if (freerdp_settings_get_bool(rdpei->rdpcontext->settings, FreeRDP_SuspendInput))
		return CHANNEL_RC_OK;

	if (!frame)
		return ERROR_INTERNAL_ERROR;

	size_t pduLength = 64ULL + (64ULL * frame->contactCount);
	wStream* s = Stream_New(NULL, pduLength);

	if (!s)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Seek(s, RDPINPUT_HEADER_LENGTH);
	/**
	 * the time that has elapsed (in milliseconds) from when the oldest touch frame
	 * was generated to when it was encoded for transmission by the client.
	 */
	rdpei_write_4byte_unsigned(
	    s, (UINT32)frame->frameOffset); /* encodeTime (FOUR_BYTE_UNSIGNED_INTEGER) */
	rdpei_write_2byte_unsigned(s, 1);   /* (frameCount) TWO_BYTE_UNSIGNED_INTEGER */

	status = rdpei_write_touch_frame(rdpei->base.log, s, frame);
	if (status)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR,
		           "rdpei_write_touch_frame failed with error %" PRIu32 "!", status);
		Stream_Free(s, TRUE);
		return status;
	}

	Stream_SealLength(s);
	status = rdpei_send_pdu(callback, s, EVENTID_TOUCH, Stream_Length(s));
	Stream_Free(s, TRUE);
	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_recv_sc_ready_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	UINT32 features = 0;
	UINT32 protocolVersion = 0;

	if (!callback || !callback->plugin)
		return ERROR_INTERNAL_ERROR;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;

	if (!Stream_CheckAndLogRequiredLengthWLog(rdpei->base.log, s, 4))
		return ERROR_INVALID_DATA;
	Stream_Read_UINT32(s, protocolVersion); /* protocolVersion (4 bytes) */

	if (protocolVersion >= RDPINPUT_PROTOCOL_V300)
	{
		if (!Stream_CheckAndLogRequiredLengthWLog(rdpei->base.log, s, 4))
			return ERROR_INVALID_DATA;
	}

	if (Stream_GetRemainingLength(s) >= 4)
		Stream_Read_UINT32(s, features);

	if (rdpei->version > protocolVersion)
		rdpei->version = protocolVersion;
	rdpei->features = features;

	if (protocolVersion > RDPINPUT_PROTOCOL_V300)
	{
		WLog_Print(rdpei->base.log, WLOG_WARN,
		           "Unknown [MS-RDPEI] protocolVersion: 0x%08" PRIX32 "", protocolVersion);
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_recv_suspend_touch_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	UINT error = CHANNEL_RC_OK;

	WINPR_UNUSED(s);

	if (!callback || !callback->plugin)
		return ERROR_INTERNAL_ERROR;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
	RdpeiClientContext* context = rdpei->context;
	if (!rdpei)
		return ERROR_INTERNAL_ERROR;

	IFCALLRET(context->SuspendTouch, error, context);

	if (error)
		WLog_Print(rdpei->base.log, WLOG_ERROR,
		           "rdpei->SuspendTouch failed with error %" PRIu32 "!", error);

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_recv_resume_touch_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	UINT error = CHANNEL_RC_OK;
	if (!s || !callback)
		return ERROR_INTERNAL_ERROR;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
	if (!rdpei)
		return ERROR_INTERNAL_ERROR;

	RdpeiClientContext* context = (RdpeiClientContext*)callback->plugin->pInterface;
	if (!context)
		return ERROR_INTERNAL_ERROR;

	IFCALLRET(context->ResumeTouch, error, context);

	if (error)
		WLog_Print(rdpei->base.log, WLOG_ERROR, "rdpei->ResumeTouch failed with error %" PRIu32 "!",
		           error);

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_recv_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	UINT16 eventId = 0;
	UINT32 pduLength = 0;
	UINT error = 0;

	if (!callback || !s)
		return ERROR_INTERNAL_ERROR;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
	if (!rdpei)
		return ERROR_INTERNAL_ERROR;

	if (!Stream_CheckAndLogRequiredLengthWLog(rdpei->base.log, s, 6))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT16(s, eventId);   /* eventId (2 bytes) */
	Stream_Read_UINT32(s, pduLength); /* pduLength (4 bytes) */
#ifdef WITH_DEBUG_RDPEI
	WLog_Print(rdpei->base.log, WLOG_DEBUG,
	           "rdpei_recv_pdu: eventId: %" PRIu16 " (%s) length: %" PRIu32 "", eventId,
	           rdpei_eventid_string(eventId), pduLength);
#endif

	if ((pduLength < 6) || !Stream_CheckAndLogRequiredLengthWLog(rdpei->base.log, s, pduLength - 6))
		return ERROR_INVALID_DATA;

	switch (eventId)
	{
		case EVENTID_SC_READY:
			if ((error = rdpei_recv_sc_ready_pdu(callback, s)))
			{
				WLog_Print(rdpei->base.log, WLOG_ERROR,
				           "rdpei_recv_sc_ready_pdu failed with error %" PRIu32 "!", error);
				return error;
			}

			if ((error = rdpei_send_cs_ready_pdu(callback)))
			{
				WLog_Print(rdpei->base.log, WLOG_ERROR,
				           "rdpei_send_cs_ready_pdu failed with error %" PRIu32 "!", error);
				return error;
			}

			break;

		case EVENTID_SUSPEND_TOUCH:
			if ((error = rdpei_recv_suspend_touch_pdu(callback, s)))
			{
				WLog_Print(rdpei->base.log, WLOG_ERROR,
				           "rdpei_recv_suspend_touch_pdu failed with error %" PRIu32 "!", error);
				return error;
			}

			break;

		case EVENTID_RESUME_TOUCH:
			if ((error = rdpei_recv_resume_touch_pdu(callback, s)))
			{
				WLog_Print(rdpei->base.log, WLOG_ERROR,
				           "rdpei_recv_resume_touch_pdu failed with error %" PRIu32 "!", error);
				return error;
			}

			break;

		default:
			break;
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	return rdpei_recv_pdu(callback, data);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_on_close(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	if (callback)
	{
		RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)callback->plugin;
		if (rdpei && rdpei->base.listener_callback)
		{
			if (rdpei->base.listener_callback->channel_callback == callback)
				rdpei->base.listener_callback->channel_callback = NULL;
		}
	}
	free(callback);
	return CHANNEL_RC_OK;
}

/**
 * Channel Client Interface
 */

static UINT32 rdpei_get_version(RdpeiClientContext* context)
{
	RDPEI_PLUGIN* rdpei = NULL;
	if (!context || !context->handle)
		return 0;
	rdpei = (RDPEI_PLUGIN*)context->handle;
	return rdpei->version;
}

static UINT32 rdpei_get_features(RdpeiClientContext* context)
{
	RDPEI_PLUGIN* rdpei = NULL;
	if (!context || !context->handle)
		return 0;
	rdpei = (RDPEI_PLUGIN*)context->handle;
	return rdpei->features;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
UINT rdpei_send_frame(RdpeiClientContext* context, RDPINPUT_TOUCH_FRAME* frame)
{
	UINT64 currentTime = GetTickCount64();
	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)context->handle;
	GENERIC_CHANNEL_CALLBACK* callback = NULL;
	UINT error = 0;

	callback = rdpei->base.listener_callback->channel_callback;

	/* Just ignore the event if the channel is not connected */
	if (!callback)
		return CHANNEL_RC_OK;

	if (!rdpei->previousFrameTime && !rdpei->currentFrameTime)
	{
		rdpei->currentFrameTime = currentTime;
		frame->frameOffset = 0;
	}
	else
	{
		rdpei->currentFrameTime = currentTime;
		frame->frameOffset = rdpei->currentFrameTime - rdpei->previousFrameTime;
	}

	if ((error = rdpei_send_touch_event_pdu(callback, frame)))
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR,
		           "rdpei_send_touch_event_pdu failed with error %" PRIu32 "!", error);
		return error;
	}

	rdpei->previousFrameTime = rdpei->currentFrameTime;
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_add_contact(RdpeiClientContext* context, const RDPINPUT_CONTACT_DATA* contact)
{
	RDPINPUT_CONTACT_POINT* contactPoint = NULL;
	RDPEI_PLUGIN* rdpei = NULL;
	if (!context || !contact || !context->handle)
		return ERROR_INTERNAL_ERROR;

	rdpei = (RDPEI_PLUGIN*)context->handle;

	EnterCriticalSection(&rdpei->lock);
	contactPoint = &rdpei->contactPoints[contact->contactId];
	contactPoint->data = *contact;
	contactPoint->dirty = TRUE;
	(void)SetEvent(rdpei->event);
	LeaveCriticalSection(&rdpei->lock);

	return CHANNEL_RC_OK;
}

static UINT rdpei_touch_process(RdpeiClientContext* context, INT32 externalId, UINT32 contactFlags,
                                INT32 x, INT32 y, INT32* contactId, UINT32 fieldFlags, va_list ap)
{
	INT64 contactIdlocal = -1;
	RDPINPUT_CONTACT_POINT* contactPoint = NULL;
	UINT error = CHANNEL_RC_OK;

	if (!context || !contactId || !context->handle)
		return ERROR_INTERNAL_ERROR;

	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)context->handle;
	/* Create a new contact point in an empty slot */
	EnterCriticalSection(&rdpei->lock);
	const BOOL begin = (contactFlags & RDPINPUT_CONTACT_FLAG_DOWN) != 0;
	contactPoint = rdpei_contact(rdpei, externalId, !begin);
	if (contactPoint)
		contactIdlocal = contactPoint->contactId;
	LeaveCriticalSection(&rdpei->lock);

	if (contactIdlocal > UINT32_MAX)
		return ERROR_INVALID_PARAMETER;

	if (contactIdlocal >= 0)
	{
		RDPINPUT_CONTACT_DATA contact = { 0 };
		contact.x = x;
		contact.y = y;
		contact.contactId = (UINT32)contactIdlocal;
		contact.contactFlags = contactFlags;
		contact.fieldsPresent = WINPR_ASSERTING_INT_CAST(UINT16, fieldFlags);

		if (fieldFlags & CONTACT_DATA_CONTACTRECT_PRESENT)
		{
			INT32 val = va_arg(ap, INT32);
			contact.contactRectLeft = WINPR_ASSERTING_INT_CAST(INT16, val);

			val = va_arg(ap, INT32);
			contact.contactRectTop = WINPR_ASSERTING_INT_CAST(INT16, val);

			val = va_arg(ap, INT32);
			contact.contactRectRight = WINPR_ASSERTING_INT_CAST(INT16, val);

			val = va_arg(ap, INT32);
			contact.contactRectBottom = WINPR_ASSERTING_INT_CAST(INT16, val);
		}
		if (fieldFlags & CONTACT_DATA_ORIENTATION_PRESENT)
		{
			UINT32 p = va_arg(ap, UINT32);
			if (p >= 360)
			{
				WLog_Print(rdpei->base.log, WLOG_WARN,
				           "TouchContact %" PRId64 ": Invalid orientation value %" PRIu32
				           "degree, clamping to 359 degree",
				           contactIdlocal, p);
				p = 359;
			}
			contact.orientation = p;
		}
		if (fieldFlags & CONTACT_DATA_PRESSURE_PRESENT)
		{
			UINT32 p = va_arg(ap, UINT32);
			if (p > 1024)
			{
				WLog_Print(rdpei->base.log, WLOG_WARN,
				           "TouchContact %" PRId64 ": Invalid pressure value %" PRIu32
				           ", clamping to 1024",
				           contactIdlocal, p);
				p = 1024;
			}
			contact.pressure = p;
		}

		error = context->AddContact(context, &contact);
	}

	if (contactId)
		*contactId = (INT32)contactIdlocal;
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_touch_begin(RdpeiClientContext* context, INT32 externalId, INT32 x, INT32 y,
                              INT32* contactId)
{
	UINT rc = 0;
	va_list ap = { 0 };
	rc = rdpei_touch_process(context, externalId,
	                         RDPINPUT_CONTACT_FLAG_DOWN | RDPINPUT_CONTACT_FLAG_INRANGE |
	                             RDPINPUT_CONTACT_FLAG_INCONTACT,
	                         x, y, contactId, 0, ap);
	return rc;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_touch_update(RdpeiClientContext* context, INT32 externalId, INT32 x, INT32 y,
                               INT32* contactId)
{
	UINT rc = 0;
	va_list ap = { 0 };
	rc = rdpei_touch_process(context, externalId,
	                         RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE |
	                             RDPINPUT_CONTACT_FLAG_INCONTACT,
	                         x, y, contactId, 0, ap);
	return rc;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_touch_end(RdpeiClientContext* context, INT32 externalId, INT32 x, INT32 y,
                            INT32* contactId)
{
	UINT error = 0;
	va_list ap = { 0 };
	error = rdpei_touch_process(context, externalId,
	                            RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE |
	                                RDPINPUT_CONTACT_FLAG_INCONTACT,
	                            x, y, contactId, 0, ap);
	if (error != CHANNEL_RC_OK)
		return error;
	error =
	    rdpei_touch_process(context, externalId, RDPINPUT_CONTACT_FLAG_UP, x, y, contactId, 0, ap);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_touch_cancel(RdpeiClientContext* context, INT32 externalId, INT32 x, INT32 y,
                               INT32* contactId)
{
	UINT rc = 0;
	va_list ap = { 0 };
	rc = rdpei_touch_process(context, externalId,
	                         RDPINPUT_CONTACT_FLAG_UP | RDPINPUT_CONTACT_FLAG_CANCELED, x, y,
	                         contactId, 0, ap);
	return rc;
}

static UINT rdpei_touch_raw_event(RdpeiClientContext* context, INT32 externalId, INT32 x, INT32 y,
                                  INT32* contactId, UINT32 flags, UINT32 fieldFlags, ...)
{
	UINT rc = 0;
	va_list ap;
	va_start(ap, fieldFlags);
	rc = rdpei_touch_process(context, externalId, flags, x, y, contactId, fieldFlags, ap);
	va_end(ap);
	return rc;
}

static UINT rdpei_touch_raw_event_va(RdpeiClientContext* context, INT32 externalId, INT32 x,
                                     INT32 y, INT32* contactId, UINT32 flags, UINT32 fieldFlags,
                                     va_list args)
{
	return rdpei_touch_process(context, externalId, flags, x, y, contactId, fieldFlags, args);
}

static RDPINPUT_PEN_CONTACT_POINT* rdpei_pen_contact(RDPEI_PLUGIN* rdpei, INT32 externalId,
                                                     BOOL active)
{
	if (!rdpei)
		return NULL;

	for (UINT32 x = 0; x < rdpei->maxPenContacts; x++)
	{
		RDPINPUT_PEN_CONTACT_POINT* contact = &rdpei->penContactPoints[x];
		if (active)
		{
			if (contact->active)
			{
				if (contact->externalId == externalId)
					return contact;
			}
		}
		else
		{
			if (!contact->active)
			{
				contact->externalId = externalId;
				contact->active = TRUE;
				return contact;
			}
		}
	}
	return NULL;
}

static UINT rdpei_add_pen(RdpeiClientContext* context, INT32 externalId,
                          const RDPINPUT_PEN_CONTACT* contact)
{
	RDPEI_PLUGIN* rdpei = NULL;
	RDPINPUT_PEN_CONTACT_POINT* contactPoint = NULL;

	if (!context || !contact || !context->handle)
		return ERROR_INTERNAL_ERROR;

	rdpei = (RDPEI_PLUGIN*)context->handle;

	EnterCriticalSection(&rdpei->lock);
	contactPoint = rdpei_pen_contact(rdpei, externalId, TRUE);
	if (contactPoint)
	{
		contactPoint->data = *contact;
		contactPoint->dirty = TRUE;
		(void)SetEvent(rdpei->event);
	}
	LeaveCriticalSection(&rdpei->lock);

	return CHANNEL_RC_OK;
}

static UINT rdpei_pen_process(RdpeiClientContext* context, INT32 externalId, UINT32 contactFlags,
                              UINT32 fieldFlags, INT32 x, INT32 y, va_list ap)
{
	RDPINPUT_PEN_CONTACT_POINT* contactPoint = NULL;
	RDPEI_PLUGIN* rdpei = NULL;
	UINT error = CHANNEL_RC_OK;

	if (!context || !context->handle)
		return ERROR_INTERNAL_ERROR;

	rdpei = (RDPEI_PLUGIN*)context->handle;

	EnterCriticalSection(&rdpei->lock);
	// Start a new contact only when it is not active.
	contactPoint = rdpei_pen_contact(rdpei, externalId, TRUE);
	if (!contactPoint)
	{
		const UINT32 mask = RDPINPUT_CONTACT_FLAG_INRANGE;
		if ((contactFlags & mask) == mask)
		{
			contactPoint = rdpei_pen_contact(rdpei, externalId, FALSE);
		}
	}
	LeaveCriticalSection(&rdpei->lock);
	if (contactPoint != NULL)
	{
		RDPINPUT_PEN_CONTACT contact = { 0 };

		contact.x = x;
		contact.y = y;
		contact.fieldsPresent = WINPR_ASSERTING_INT_CAST(UINT16, fieldFlags);

		contact.contactFlags = contactFlags;
		if (fieldFlags & RDPINPUT_PEN_CONTACT_PENFLAGS_PRESENT)
		{
			const UINT32 val = va_arg(ap, UINT32);
			contact.penFlags = WINPR_ASSERTING_INT_CAST(UINT16, val);
		}
		if (fieldFlags & RDPINPUT_PEN_CONTACT_PRESSURE_PRESENT)
		{
			const UINT32 val = va_arg(ap, UINT32);
			contact.pressure = WINPR_ASSERTING_INT_CAST(UINT16, val);
		}
		if (fieldFlags & RDPINPUT_PEN_CONTACT_ROTATION_PRESENT)
		{
			const UINT32 val = va_arg(ap, UINT32);
			contact.rotation = WINPR_ASSERTING_INT_CAST(UINT16, val);
		}
		if (fieldFlags & RDPINPUT_PEN_CONTACT_TILTX_PRESENT)
		{
			const INT32 val = va_arg(ap, INT32);
			contact.tiltX = WINPR_ASSERTING_INT_CAST(INT16, val);
		}
		if (fieldFlags & RDPINPUT_PEN_CONTACT_TILTY_PRESENT)
		{
			const INT32 val = va_arg(ap, INT32);
			WINPR_ASSERT((val >= INT16_MIN) && (val <= INT16_MAX));
			contact.tiltY = WINPR_ASSERTING_INT_CAST(INT16, val);
		}

		error = context->AddPen(context, externalId, &contact);
	}

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_pen_begin(RdpeiClientContext* context, INT32 externalId, UINT32 fieldFlags,
                            INT32 x, INT32 y, ...)
{
	UINT error = 0;
	va_list ap;

	va_start(ap, y);
	error = rdpei_pen_process(context, externalId,
	                          RDPINPUT_CONTACT_FLAG_DOWN | RDPINPUT_CONTACT_FLAG_INRANGE |
	                              RDPINPUT_CONTACT_FLAG_INCONTACT,
	                          fieldFlags, x, y, ap);
	va_end(ap);

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_pen_update(RdpeiClientContext* context, INT32 externalId, UINT32 fieldFlags,
                             INT32 x, INT32 y, ...)
{
	UINT error = 0;
	va_list ap;

	va_start(ap, y);
	error = rdpei_pen_process(context, externalId,
	                          RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE |
	                              RDPINPUT_CONTACT_FLAG_INCONTACT,
	                          fieldFlags, x, y, ap);
	va_end(ap);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_pen_end(RdpeiClientContext* context, INT32 externalId, UINT32 fieldFlags, INT32 x,
                          INT32 y, ...)
{
	UINT error = 0;
	va_list ap;
	va_start(ap, y);
	error = rdpei_pen_process(context, externalId,
	                          RDPINPUT_CONTACT_FLAG_UP | RDPINPUT_CONTACT_FLAG_INRANGE, fieldFlags,
	                          x, y, ap);
	va_end(ap);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_pen_hover_begin(RdpeiClientContext* context, INT32 externalId, UINT32 fieldFlags,
                                  INT32 x, INT32 y, ...)
{
	UINT error = 0;
	va_list ap;

	va_start(ap, y);
	error = rdpei_pen_process(context, externalId,
	                          RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE,
	                          fieldFlags, x, y, ap);
	va_end(ap);

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_pen_hover_update(RdpeiClientContext* context, INT32 externalId, UINT32 fieldFlags,
                                   INT32 x, INT32 y, ...)
{
	UINT error = 0;
	va_list ap;

	va_start(ap, y);
	error = rdpei_pen_process(context, externalId,
	                          RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_INRANGE,
	                          fieldFlags, x, y, ap);
	va_end(ap);

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpei_pen_hover_cancel(RdpeiClientContext* context, INT32 externalId, UINT32 fieldFlags,
                                   INT32 x, INT32 y, ...)
{
	UINT error = 0;
	va_list ap;

	va_start(ap, y);
	error = rdpei_pen_process(context, externalId,
	                          RDPINPUT_CONTACT_FLAG_UPDATE | RDPINPUT_CONTACT_FLAG_CANCELED,
	                          fieldFlags, x, y, ap);
	va_end(ap);

	return error;
}

static UINT rdpei_pen_raw_event(RdpeiClientContext* context, INT32 externalId, UINT32 contactFlags,
                                UINT32 fieldFlags, INT32 x, INT32 y, ...)
{
	UINT error = 0;
	va_list ap;

	va_start(ap, y);
	error = rdpei_pen_process(context, externalId, contactFlags, fieldFlags, x, y, ap);
	va_end(ap);
	return error;
}

static UINT rdpei_pen_raw_event_va(RdpeiClientContext* context, INT32 externalId,
                                   UINT32 contactFlags, UINT32 fieldFlags, INT32 x, INT32 y,
                                   va_list args)
{
	return rdpei_pen_process(context, externalId, contactFlags, fieldFlags, x, y, args);
}

static UINT init_plugin_cb(GENERIC_DYNVC_PLUGIN* base, rdpContext* rcontext, rdpSettings* settings)
{
	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)base;

	WINPR_ASSERT(base);
	WINPR_UNUSED(settings);

	rdpei->version = RDPINPUT_PROTOCOL_V300;
	rdpei->currentFrameTime = 0;
	rdpei->previousFrameTime = 0;
	rdpei->maxTouchContacts = MAX_CONTACTS;
	rdpei->maxPenContacts = MAX_PEN_CONTACTS;
	rdpei->rdpcontext = rcontext;

	WINPR_ASSERT(rdpei->base.log);

	InitializeCriticalSection(&rdpei->lock);
	rdpei->event = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!rdpei->event)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR, "calloc failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	RdpeiClientContext* context = (RdpeiClientContext*)calloc(1, sizeof(RdpeiClientContext));
	if (!context)
	{
		WLog_Print(rdpei->base.log, WLOG_ERROR, "calloc failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	context->clientFeaturesMask = UINT32_MAX;
	context->handle = (void*)rdpei;
	context->GetVersion = rdpei_get_version;
	context->GetFeatures = rdpei_get_features;
	context->AddContact = rdpei_add_contact;
	context->TouchBegin = rdpei_touch_begin;
	context->TouchUpdate = rdpei_touch_update;
	context->TouchEnd = rdpei_touch_end;
	context->TouchCancel = rdpei_touch_cancel;
	context->TouchRawEvent = rdpei_touch_raw_event;
	context->TouchRawEventVA = rdpei_touch_raw_event_va;
	context->AddPen = rdpei_add_pen;
	context->PenBegin = rdpei_pen_begin;
	context->PenUpdate = rdpei_pen_update;
	context->PenEnd = rdpei_pen_end;
	context->PenHoverBegin = rdpei_pen_hover_begin;
	context->PenHoverUpdate = rdpei_pen_hover_update;
	context->PenHoverCancel = rdpei_pen_hover_cancel;
	context->PenRawEvent = rdpei_pen_raw_event;
	context->PenRawEventVA = rdpei_pen_raw_event_va;

	rdpei->context = context;
	rdpei->base.iface.pInterface = (void*)context;

	rdpei->async =
	    !freerdp_settings_get_bool(rdpei->rdpcontext->settings, FreeRDP_SynchronousDynamicChannels);
	if (rdpei->async)
	{
		rdpei->running = TRUE;

		rdpei->thread = CreateThread(NULL, 0, rdpei_periodic_update, rdpei, 0, NULL);
		if (!rdpei->thread)
		{
			WLog_Print(rdpei->base.log, WLOG_ERROR, "calloc failed!");
			return CHANNEL_RC_NO_MEMORY;
		}
	}
	else
	{
		if (!freerdp_client_channel_register(rdpei->rdpcontext->channels, rdpei->event,
		                                     rdpei_poll_run, rdpei))
			return ERROR_INTERNAL_ERROR;
	}

	return CHANNEL_RC_OK;
}

static void terminate_plugin_cb(GENERIC_DYNVC_PLUGIN* base)
{
	RDPEI_PLUGIN* rdpei = (RDPEI_PLUGIN*)base;
	WINPR_ASSERT(rdpei);

	rdpei->running = FALSE;
	if (rdpei->event)
		(void)SetEvent(rdpei->event);

	if (rdpei->thread)
	{
		(void)WaitForSingleObject(rdpei->thread, INFINITE);
		(void)CloseHandle(rdpei->thread);
	}

	if (rdpei->event && !rdpei->async)
		(void)freerdp_client_channel_unregister(rdpei->rdpcontext->channels, rdpei->event);

	if (rdpei->event)
		(void)CloseHandle(rdpei->event);

	DeleteCriticalSection(&rdpei->lock);
	free(rdpei->context);
}

static const IWTSVirtualChannelCallback geometry_callbacks = { rdpei_on_data_received,
	                                                           NULL, /* Open */
	                                                           rdpei_on_close, NULL };

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
FREERDP_ENTRY_POINT(UINT VCAPITYPE rdpei_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))
{
	return freerdp_generic_DVCPluginEntry(pEntryPoints, RDPEI_TAG, RDPEI_DVC_CHANNEL_NAME,
	                                      sizeof(RDPEI_PLUGIN), sizeof(GENERIC_CHANNEL_CALLBACK),
	                                      &geometry_callbacks, init_plugin_cb, terminate_plugin_cb);
}
