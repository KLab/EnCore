#ifndef EC_MULTITASKING_INTERNAL_H
#define EC_MULTITASKING_INTERNAL_H

#include "EC_MultiTasking.h"
#include "EC_MultiTaskingPlatform.h"

#define EC_ASSERT(a)		EC_ASSERT_MSG(a,"Error")
#define EC_ASSERTALWAYS		EC_ASSERT(false)

#define MAX_TASK_COUNT_INTERNAL		(MAX_TASK_COUNT+1)

#define NULL_IDX			(0xFFFF)

bool CreateThread	(u8 workerIndex, u32 stackSize, void* context);
void RunThread		(void* context);

class TEventLock {
public:
	TEventLock	() { }
	~TEventLock	() { }
	void WaitEvent();
	void SendEvent();

	bool init	();
	void release();
	LXEVENT_LOCK_HANDLE m_handle;
};

#endif
