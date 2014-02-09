#include "EC_MultiTaskingInternal.h"
#include <Windows.h>
#include <intrin.h>

// ============================================================================
//   Multithreading
// ============================================================================

// --- Internal ---
DWORD WINAPI _ThreadProc( void* p ) {
	RunThread(p);
	return 0;
}

// [Public]
bool CreateThread(u8 workerIndex, u32 stackSize, void* context) {
	DWORD ThreadID; // Ignored on our system, could be added for debug.

	// Create and launch thread.
	HANDLE threadHandle = CreateThread( NULL, stackSize, _ThreadProc, context, 0, &ThreadID);
	return threadHandle != 0;
}

// ============================================================================
//   Hardware Core Count
// ============================================================================

// [Public]
u8	WRK_GetHWWorkerCount() {
	SYSTEM_INFO si;
	GetSystemInfo( &si );
	return (u8)si.dwNumberOfProcessors;
}

// ============================================================================
//   Mutex and Event Lock
// ============================================================================

// ---- Mutex ----
// [Public]
void TLock::Lock()				{	EnterCriticalSection(&m_handle);									}
void TLock::Unlock()			{	LeaveCriticalSection(&m_handle);									}

bool TLock::init()				{	InitializeCriticalSection(&m_handle);	
									return true;														}
void TLock::release()			{	DeleteCriticalSection(&m_handle);									}

// ---- Thread Sleep and Manager wake up signal ----
// [Public]
void TEventLock::WaitEvent()	{	WaitForSingleObject( m_handle , INFINITE );							}
void TEventLock::SendEvent()	{	SetEvent(m_handle);													}
void TEventLock::release()		{   if (m_handle) { CloseHandle(m_handle); m_handle = 0; }				}
bool TEventLock::init()			{	return (NULL != (m_handle = CreateEvent(NULL,false,false,NULL)));	}

// --- Atomic Decrement Counter ---
u32 atomicDecrement(volatile u32* pValue) {
	return _InterlockedDecrement((volatile long*)pValue);
}
