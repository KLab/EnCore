#include "src/LX_MultiTaskingInternal.h"

//______________________________________________________________________________________________________________
//
// ====== OS RELATED ======
//
// Job to do for platform porting :
//
//	1. 	Rewrite all the Lock/Unlock/Init/Release for TLock
//		Prefer to use mutex that are locking between threads but not between process.
//		In our case, we use CRITICAL_SECTION for Win32.
//
//	2.	Rewrite TEventLock release/init/WaitEvent/SendEvent
//		It is basically used to sleep a thread (call to WaitEvent()), and wake them all up using SendEvent().
//
//	3.	_ThreadProc is the OS specific thread function signature.
//______________________________________________________________________________________________________________


// ---- Thread Creation ----
DWORD WINAPI _ThreadProc( void* p ) {
	((WorkThread*)p)->Run();
	return 0;
}

/* static */
bool CreateThread(u32 stackSize, void* context) {
	DWORD ThreadID; // Ignored on our system, could be added for debug.

	// Create and launch thread.
	HANDLE threadHandle = CreateThread( NULL, stackSize, _ThreadProc, context, 0, &ThreadID);
	return threadHandle != 0;
}

// ---- System Information ----
/* static */
u32 Scheduler::GetProcessorCount()
{
	SYSTEM_INFO si;
	GetSystemInfo( &si );
	return si.dwNumberOfProcessors;
}
