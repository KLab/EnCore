#ifndef LX_MULTITASKING_PLATFORM_H
#define LX_MULTITASKING_PLATFORM_H

#include <windows.h>

//
// 0. Assert depends on platform.
//
#include <stdio.h>
#define EC_ASSERT_MSG(a,b)	if (!(a)) { printf("%s",b);  while (true) { } }

//
// 1. May define a faster function than standard memcpy for pointer aligned pointer array copy
//    note that arguments are the same as memcpy (size is in byte)
//    We route here to default standard memcpy
//
#define	memcpyPtrSize		memcpy

//
// 2. A Route a Win32 thread handle to our engine type.
//    B Route a Win32 Critical section as our engine lock type
//    C Route a Win32 handle used for event based locking.
//
// See LX_MultiTaskingPlatform.cpp
//
typedef HANDLE				LXTHREAD_HANDLE;
typedef CRITICAL_SECTION	LXLOCK_HANDLE;
typedef HANDLE				LXEVENT_LOCK_HANDLE;

//
// 3. _ThreadProc function is the signature of the function used by theading system, so it is platform dependant.
//
// See LX_MultiTaskingPlatform.cpp
//
class OSWorkThread {
	friend class Scheduler;
private:
	static DWORD WINAPI _ThreadProc( void* pThreadContext );
	LXTHREAD_HANDLE	m_osThread;
};

#endif
