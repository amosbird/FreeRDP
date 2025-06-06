/**
 * WinPR: Windows Portable Runtime
 * Serial Communication API
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2014 Hewlett-Packard Development Company, L.P.
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

#ifndef WINPR_COMM_PRIVATE_H
#define WINPR_COMM_PRIVATE_H

#if defined(__linux__)
#define WINPR_HAVE_COMM_COUNTERS
#include <linux/serial.h>
#endif

#include <winpr/comm.h>

#include "../handle/handle.h"
#include <winpr/config.h>

#if defined(WINPR_HAVE_SYS_EVENTFD_H)
#include <sys/eventfd.h>
#endif

struct winpr_comm
{
	WINPR_HANDLE common;

	int fd;

	int fd_read;
	int fd_read_event; /* as of today, only used by _purge() */
	CRITICAL_SECTION ReadLock;

	int fd_write;
	int fd_write_event; /* as of today, only used by _purge() */
	CRITICAL_SECTION WriteLock;

	/* permissive mode on errors. If TRUE (default is FALSE)
	 * CommDeviceIoControl always return TRUE.
	 *
	 * Not all features are supported yet and an error is then returned when
	 * an application turns them on (e.g: i/o buffers > 4096). It appeared
	 * though that devices and applications can be still functional on such
	 * errors.
	 *
	 * see also: comm_ioctl.c
	 *
	 * FIXME: getting rid of this flag once all features supported.
	 */
	BOOL permissive;

	SERIAL_DRIVER_ID serverSerialDriverId;

	COMMTIMEOUTS timeouts;

	CRITICAL_SECTION
	EventsLock; /* protects counters, WaitEventMask and PendingEvents */
#if defined(WINPR_HAVE_COMM_COUNTERS)
	struct serial_icounter_struct counters;
#endif
	ULONG WaitEventMask;
	ULONG PendingEvents;

	BYTE eventChar;
	/* NB: CloseHandle() has to free resources */
	ULONG XOnLimit;
	ULONG XOffLimit;

#if defined(WINPR_HAVE_COMM_COUNTERS)
	BOOL TIOCGICOUNTSupported;
#endif
};

typedef struct winpr_comm WINPR_COMM;

#define SERIAL_EV_RXCHAR 0x0001
#define SERIAL_EV_RXFLAG 0x0002
#define SERIAL_EV_TXEMPTY 0x0004
#define SERIAL_EV_CTS 0x0008
#define SERIAL_EV_DSR 0x0010
#define SERIAL_EV_RLSD 0x0020
#define SERIAL_EV_BREAK 0x0040
#define SERIAL_EV_ERR 0x0080
#define SERIAL_EV_RING 0x0100
#define SERIAL_EV_PERR 0x0200
#define SERIAL_EV_RX80FULL 0x0400
#define SERIAL_EV_EVENT1 0x0800
#define SERIAL_EV_EVENT2 0x1000
#define SERIAL_EV_WINPR_WAITING 0x4000 /* bit today unused by other SERIAL_EV_* */
#define SERIAL_EV_WINPR_STOP 0x8000    /* bit today unused by other SERIAL_EV_* */

#define WINPR_PURGE_TXABORT 0x00000001 /* abort pending transmission */
#define WINPR_PURGE_RXABORT 0x00000002 /* abort pending reception */

#define CommLog_Print(level, ...) CommLog_PrintEx(level, __FILE__, __LINE__, __func__, __VA_ARGS__)
void CommLog_PrintEx(DWORD wlog_level, const char* file, size_t line, const char* fkt, ...);

BOOL CommIsHandled(HANDLE handle);
BOOL CommIsHandleValid(HANDLE handle);
BOOL CommCloseHandle(HANDLE handle);
const HANDLE_CREATOR* GetCommHandleCreator(void);

#define CommIoCtl(pComm, ctl, data) \
	CommIoCtl_int((pComm), (ctl), (data), __FILE__, __func__, __LINE__)
BOOL CommIoCtl_int(WINPR_COMM* pComm, unsigned long int ctl, void* data, const char* file,
                   const char* fkt, size_t line);
BOOL CommUpdateIOCount(HANDLE handle, BOOL checkSupportStatus);

const char* CommSerialEvString(ULONG status, char* buffer, size_t size);

#if defined(WINPR_HAVE_SYS_EVENTFD_H)
#ifndef WITH_EVENTFD_READ_WRITE
int eventfd_read(int fd, eventfd_t* value);
int eventfd_write(int fd, eventfd_t value);
#endif
#endif

#endif /* WINPR_COMM_PRIVATE_H */
