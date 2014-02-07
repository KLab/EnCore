#include "EC_MultiTaskingInternal.h"

#define EC_ASSERT(a)		EC_ASSERT_MSG(a,"Error")
#define EC_ASSERTALWAYS		EC_ASSERT(false)

#define MAX_TASK_COUNT_INTERNAL		(MAX_TASK_COUNT+1)
#define NULL_IDX					(0xFFFF)

typedef u16	TaskSlot;

struct TaskEntry {
	Task*		pTask;
	TaskParam	param[3];
};

struct DependancyEntry {
	TaskSlot	m_prev;
	TaskSlot	m_next;

	s16			m_lprev;
	s16			m_lnext;

	s16			m_sprev;
	s16			m_snext;

	TaskSlot	m_self;
	TaskEntry	m_pTask;
	TaskEntry	m_pToTask;
};

__declspec(align(64)) class WorkThread {
	friend class Task;
public:
	bool	Init(errorFunc error);
	void	Release();

	enum STATE {
		TH_DEAD		= 0,
		TH_ACTIVE	= 1,
		TH_ASLEEP	= 2
	};

	inline
	STATE	GetState		() { return m_state; };

	bool	UnpopAndSwap	(TaskEntry* task);
	bool	PushFast		(TaskEntry* task);
	bool	PushExecute		(TaskEntry* task);

	/** 
		User Context associated with a thread.
		One possible usage is a heap allocator specific to each thread,
		or anything the user wants to manager on a "per-thread" basis.
	 */
	inline
	void*	GetUserContext	() { return m_userContext; }

	inline
	u8		GetThreadIndex	() { return m_threadIndex; }

	void	RemoveFromGroup	(u8 worker);
	void	AddToGroup		(u8 worker);
	void	AllowIdleReset	(u8 neverIdle);
public:
	WorkThread();
public:
	inline
	TaskEntry*	PopExecute		(TaskEntry* info) {
		m_taskArrayLock.Lock();
		// Need to simplify copy when do stealing...
		if (m_taskExecuteStart == m_taskExecuteEnd) {
			m_taskExecuteStart = m_taskExecuteEnd = m_taskExecuteArray;
			info = NULL;
		} else {
			*info = *m_taskExecuteStart;
			m_taskExecuteStart++;
			if (m_taskExecuteStart >= &m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL]) {
				m_taskExecuteStart = m_taskExecuteArray;
			}
		}
		m_taskArrayLock.Unlock();
		return info;
	}

	void	Run				();
	bool	StealTasks		();
	bool	GiveUpSomeWork	(WorkThread* pThread);
	bool	Start			(u32 schedulerIndex, bool isMainThread);

	// Ring Buffer of executable threads.
	errorFunc	m_error;
	TaskEntry*	m_taskExecuteStart;
	TaskEntry*	m_taskExecuteEnd;
	TaskEntry	m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL];
	void*		m_userContext;
	TLock		m_taskArrayLock;
	TLock		m_modifierLock;
	TEventLock*	m_SleepNotification;
	u8			m_group[MAX_THREAD_COUNT];
	bool		m_bKeepAlive;
	bool		m_bIsMainThread;
	volatile
	STATE		m_state;
	u8			m_threadIndex;
	u8			m_threadTick;
	u8			m_groupID;
	u8			m_groupCount;
	u8			m_gotWork;
	u8			m_bNeverIdle;
};

void RunThread(void* p) {
	((WorkThread*)p)->Run();
}

// ============================================================================
//   Global Variable and context.
// ============================================================================
handleWorkerUserContext g_userContextFunc	= NULL;
u32						g_userStackSize		= 0;
u32						g_ThreadCount		= 0;
spyWorkerFunc			g_spyFunc			= NULL;
errorFunc				g_errFunc			= NULL;
void*					g_spyCtx			= NULL;
u8						g_bNeverIdle		= 1;
WorkThread				g_threads[MAX_THREAD_COUNT];
TLock					g_lock;
TEventLock				g_SleepNotification;
DependancyEntry			g_dependanceEntries	[MAX_DEPENDANCE_ENTRIES];
TLock					g_dependantLockFree;

// Link list of free entries.
static const TaskSlot	IDX_FREE_START	= 0;
static const TaskSlot	IDX_FREE_END	= 1;

TaskSlot AllocDependant();
void FreeDependant(TaskSlot	slot);

// ============================================================================
//   Worker management implementation
// ============================================================================

bool	WRK_CreateWorkers		(u32 count, bool includeMain, u32 stackSize, handleWorkerUserContext userFunc) {
	EC_ASSERT(count <= MAX_THREAD_COUNT);
	u32 created = 0;
	bool res = true;

	g_userContextFunc = userFunc;
	g_userStackSize   = stackSize;

	// TODO : need here if task posted BEFORE.
	// BUT always do AFTER CREATE if we want to post... PARADOX because else, thread will all start to do stealing.
	g_ThreadCount = count;

	// All workers by default assigned to group 0.
	for (u32 n=0; n < count; n++) {
		g_threads[n].m_groupID = 0xFF;
		WRK_SetGroup(n, 0);
	}

	for (u32 n=0; n < count; n++) {
		if ((n!=0) || ((n == 0) && (!includeMain))) {
			void* userContext = NULL;
			if (userFunc) {
				userContext = userFunc(n,NULL);
			}
			g_threads[n].m_userContext = userContext;
			res &= g_threads[n].Start(n,false);
		}
	}
	if (includeMain) {
		void* userContext = NULL;
		if (userFunc) {
			userContext = userFunc(0,NULL);
		}
		g_threads[0].m_userContext = userContext;
		res &= g_threads[0].Start(0,true);
	}

	return res;
}

void	WRK_ShutDown			(u8 worker_or_all, bool async) {
	if (worker_or_all != 0xFF) {
		EC_ASSERT(worker_or_all < MAX_THREAD_COUNT);
		g_threads[worker_or_all].m_bKeepAlive = false;		
	} else {
		for (u32 i=0; i < MAX_THREAD_COUNT; i++) {
			WRK_ShutDown(i, true);
		}
	}

	if (!async) {
		u32 count;
		do {
			count = 0;
			for (u32 i=0; i < MAX_THREAD_COUNT; i++) {
				if (g_threads[i].m_state == WorkThread::TH_ACTIVE) {
					count++;
				}
			}
			// printf("Count %i\n", count);
		} while (count != 0);
	}
}

void	WRK_SetSpy				(spyWorkerFunc cbSpyFunc, void* spyContext) {
	g_spyFunc	= cbSpyFunc;	
	g_spyCtx	= spyContext;
}

void	WRK_AllowIdle		(bool idling, u8 worker) {
	u8 tmpV = g_bNeverIdle = (!idling) ? 1 : 0;
	if (worker == EC_ALL) {
		WorkThread* pWorker = g_threads;
		WorkThread* pEnd    = &g_threads[g_ThreadCount];
		while (pWorker < pEnd) {
			pWorker->AllowIdleReset(tmpV);
			pWorker++;
		}
	} else {
		g_threads[worker].AllowIdleReset(tmpV);
	}
}

bool	WRKLIB_Init				(errorFunc errFunc) {
	g_userContextFunc	= NULL;
	g_userStackSize		= 0;
	g_ThreadCount		= 0;
	g_spyFunc			= NULL;
	g_errFunc			= errFunc;
	g_spyCtx			= NULL;
	g_bNeverIdle		= true;

	bool res = true;
	for (int n=0; n < MAX_THREAD_COUNT; n++) {
		res &= g_threads[n].Init(errFunc);
	}

	res &= g_lock.init();
	res &= g_SleepNotification.init();
	res &= g_dependantLockFree.init();

	return res;
}

void	WRKLIB_Release			() {
	// Force all threads to shutdown
	// and release
	WRK_ShutDown(EC_ALL,false);	

	for (int n=0; n < MAX_THREAD_COUNT; n++) {
		g_threads[n].Release();
	}

	g_lock.release();
	g_SleepNotification.release();
	g_dependantLockFree.release();
}

u8		WRK_GetSleepingCount	() {
	//
	// Concurrency ok about thread count and state of cores.
	// -> We do not use any locks.
	//

	u32 count = 0;
	u32 threadCount = g_ThreadCount;
	for (u32 n=0; n < threadCount; n++) {
		count += (g_threads[n].m_state == WorkThread::TH_ASLEEP) ? 1 : 0;
	}
	return (u8)count;
}

// ============================================================================
//   Worker Instance Implementation
// ============================================================================

//
// Default
// Group 0 for all thread.
//
void		WRK_SetGroup	(u8 worker, u8 groupID) {
	WorkThread* pThread = &g_threads[worker];
	if (pThread->m_groupID != groupID) {
		pThread->m_groupID = groupID;

		// Remove from all group
		for (int n=0; n < pThread->m_groupCount; n++) {
			u8 workerTarget = pThread->m_group[n];
			g_threads[workerTarget].RemoveFromGroup(worker);
		}
	
		// Find all worker having the same ID
		for (int n=0; n < MAX_THREAD_COUNT; n++) {
			if ((n != worker) && (g_threads[n].m_groupID)) {
				g_threads[n].AddToGroup(worker);
			}
		}
	}
}

u8			WRK_GetState	(u8 worker) {
	return g_threads[worker].GetState();
}

bool WRK_PushExecute(u8 worker, Task* task, TaskParam* pParam) {
	TaskEntry entry;
	entry.pTask = task;
	if (pParam) {
		entry.param[0] = pParam[0];
		entry.param[1] = pParam[1];
		entry.param[2] = pParam[2];
	}
	bool res = g_threads[worker].PushExecute(&entry);
	return res;
}

void*		WRK_GetUserContext	(u8 worker) {
	return g_threads[worker].GetUserContext();
}

void WRK_TaskYield(u8 worker, Task* task, TaskParam* arrayParam, u8 mode) {
	TaskEntry taske;
	switch (mode) {
	case 0:
		// Do nothing;
		task->TSK_UnsetYield();
	case 1:
		taske.pTask = task;
		if (arrayParam) {
			taske.param[0] = arrayParam[0];
			taske.param[1] = arrayParam[1];
			taske.param[2] = arrayParam[2];
		}
		g_threads[worker].UnpopAndSwap(&taske);	break;
		task->TSK_SetYield();
		break;
	case 2:
		taske.pTask = task;
		if (arrayParam) {
			taske.param[0] = arrayParam[0];
			taske.param[1] = arrayParam[1];
			taske.param[2] = arrayParam[2];
		}
		g_threads[worker].PushExecute(&taske);	break;
		task->TSK_SetYield();
		break;
	case 3:
		taske.pTask = task;
		if (arrayParam) {
			taske.param[0] = arrayParam[0];
			taske.param[1] = arrayParam[1];
			taske.param[2] = arrayParam[2];
		}
		g_threads[worker].PushFast(&taske);	break;
		task->TSK_SetYield();
		break;
	}
}

//______________________________________________________________________________________________________________
//
//      WORK THREAD Implementation
//______________________________________________________________________________________________________________

void WorkThread::RemoveFromGroup(u8 worker) {
	// Find phase
	int n;
	for (n=0; n < m_groupCount; n++) {
		if (m_group[n] == worker) {			
			break;
		}
	}

	// Copy phase
	if (n != m_groupCount) {
		m_groupCount--; // Remove one item
		
		// Probably faster than a memcpy given the size.
		for (int m=n; m<m_groupCount;n++) {	
			m_group[m] = m_group[m+1];
		}
	}
}

void WorkThread::AddToGroup(u8 worker) {
	m_group[m_groupCount++] = worker;
}

WorkThread::WorkThread()
:m_state	(TH_DEAD)
,m_groupID	(0xFF)
,m_gotWork	(0)
{
	// Reset Task List.
	m_taskExecuteStart	= m_taskExecuteArray;
	m_taskExecuteEnd	= m_taskExecuteArray;
}

bool WorkThread::Init(errorFunc error) {
	m_bNeverIdle= g_bNeverIdle;
	m_state		= TH_DEAD;
	m_groupID	= 0xFF;
	m_taskExecuteStart	= m_taskExecuteArray;
	m_taskExecuteEnd	= m_taskExecuteArray;
	m_SleepNotification = &g_SleepNotification;
	m_error				= error;

	bool res = true;
	res &= m_taskArrayLock.init();
	return res;
}

void WorkThread::Release() {
	m_taskArrayLock.release();
	m_groupCount = 0;
}

bool WorkThread::GiveUpSomeWork( WorkThread* pThread ) {
	if (this->m_state != TH_DEAD) {
		bool res		= true;
		bool lockOther	= false;

		// anything to share ?
		s32 Grab;
		m_taskArrayLock.Lock();
		Grab = m_taskExecuteEnd - m_taskExecuteStart;
		if ( Grab == 0 ) { res = false; goto exitLbl; }

		lockOther = true;
		pThread->m_taskArrayLock.Lock();

		if ( pThread->m_taskExecuteStart != pThread->m_taskExecuteEnd ) {
			// can happen if we're trying to steal work while taskpool has gone idle and started again
			res = false; goto exitLbl;
		}

		// Better give even if we become empty... We may still busy now and this one want work anyway.
		TaskEntry* target = pThread->m_taskExecuteStart;
		if (Grab > 0) {
			//      S        E
			// -----xxxxxxxxx|----

			// Have 1 -> Give 1
			// Have 2 -> Give 1
			// Have 3 -> Give 2
			// ...
			Grab = (Grab + 1) >> 1;
		} else {
			//     E <-Grab- S
			// xxxx|---------xxxxx
			// 1 Phase or 2 Phase
			Grab = (MAX_TASK_COUNT_INTERNAL + Grab + 1) >> 1;	// Grab is negative.
			s32 phase1 = &m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL] - m_taskExecuteStart;
			if (phase1 <= Grab) {
				// Phase 1
				memcpyPtrSize(pThread->m_taskExecuteStart, m_taskExecuteStart, phase1 * sizeof(TaskEntry));
				pThread->m_taskExecuteEnd	+= phase1;
				m_taskExecuteStart			= m_taskExecuteArray;

				// Phase 2
				Grab -= phase1;
				target = &pThread->m_taskExecuteStart[phase1];
			}
		}

		memcpyPtrSize(target, m_taskExecuteStart, Grab * sizeof(TaskEntry));
		m_taskExecuteStart			+= Grab;
		pThread->m_taskExecuteEnd	+= Grab;

	exitLbl:
		if (lockOther) {
			pThread->m_taskArrayLock.Unlock();
		}

		m_taskArrayLock.Unlock();
		return res;
	} else {
		return false;
	}
}

bool WorkThread::StealTasks() {
	// avoid always starting with same other thread. This aims at avoiding a potential
	// for problematic patterns. Note: the necessity of doing is largely speculative

	// Steal only from same group.

	u32 threadCount		= m_groupCount;
	u32 Offset			= m_threadIndex + m_threadTick++;

	if (m_threadTick > threadCount) { m_threadTick = 0;		 }
	if (Offset       > threadCount) { Offset -= threadCount; }

	u32 endOffset		= Offset + threadCount;

	while (Offset < endOffset) {
		// (Offset - ((Offset >= threadCount) * threadCount))
		// Equiv to branchless : (Offset >= threadCount) ? (Offset - threadCount) : Offset
		WorkThread* pThread = &g_threads[ m_group[(Offset - ((Offset >= threadCount) * threadCount))]];
		Offset++;
		if( pThread == this ) 							{ continue; 	}
		if( pThread->GiveUpSomeWork( this ) )			{ 
			m_gotWork++;
			return true;
		}
		// Necessary ? If GiveUp return false... my own list will stay empty and if true, it is already changed.
		// if( m_taskExecuteStart != m_taskExecuteEnd )	{ return true;	}
	}

	return false;
}

bool WorkThread::UnpopAndSwap(TaskEntry* task) {
	bool res = true;
	EC_ASSERT(task->pTask->m_executeRemainCount==0);
	m_taskArrayLock.Lock();
	TaskEntry* last = m_taskExecuteStart;
	if (m_taskExecuteStart <= m_taskExecuteEnd) {
		m_taskExecuteStart--;
		if (m_taskExecuteStart < m_taskExecuteArray) {
			m_taskExecuteStart = &m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL-1];
			if (m_taskExecuteEnd == m_taskExecuteStart) {
				// Revert and de
				m_taskExecuteStart++;
				res = false;
			}
		}
	} else {
		res = ((m_taskExecuteEnd) != (m_taskExecuteStart-1));
		if (res) {
			m_taskExecuteStart--;
		}
	}

	if (res) {
		// Swap last and 
		*m_taskExecuteStart	= *last;
		// TODO : roll back all task until attribute is not "yield" waiting for exec.
		*last				= *task;
	}
	m_taskArrayLock.Unlock();
	return res;
}

bool WorkThread::PushFast(TaskEntry* task) {
	bool res = true;
	EC_ASSERT(task->pTask->m_executeRemainCount==0);
	m_taskArrayLock.Lock();

	if (m_taskExecuteStart <= m_taskExecuteEnd) {
		m_taskExecuteStart--;
		if (m_taskExecuteStart < m_taskExecuteArray) {
			m_taskExecuteStart = &m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL-1];
			if (m_taskExecuteEnd == m_taskExecuteStart) {
				// Revert and de
				m_taskExecuteStart++;
				res = false;
			}
		}
	} else {
		res = ((m_taskExecuteEnd) != (m_taskExecuteStart-1));
		if (res) {
			m_taskExecuteStart--;
		}
	}

	if (res) {
		*m_taskExecuteStart	= *task;
	}
	m_taskArrayLock.Unlock();
	return res;
}
/*
bool WorkThread::PushExecute(TaskEntry** pTaskArray, u32 count) {
	// TODO OPTIMIZE : avoid each lock and process the batch
	bool res = true;
	for (u32 n=0; n < count; n++) {
		if (res) {
			res &= PushExecute(pTaskArray[n]);
		} else {
			break;
		}
	}
	return res;
}
*/

bool WorkThread::PushExecute(TaskEntry* task) {
	bool res = true;
	EC_ASSERT(task->pTask->m_executeRemainCount==0);
	m_taskArrayLock.Lock();
	if (m_taskExecuteStart <= m_taskExecuteEnd) {
		*m_taskExecuteEnd++ = *task;
		if (m_taskExecuteEnd == &m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL]) {
			m_taskExecuteEnd = m_taskExecuteArray;
			if (m_taskExecuteEnd == m_taskExecuteStart) {
				// Revert
				m_taskExecuteEnd = &m_taskExecuteArray[MAX_TASK_COUNT_INTERNAL-1];
				res = false;
			}
		}
	} else {
		res = ((m_taskExecuteEnd + 1) != m_taskExecuteStart);
		if (res) {
			*m_taskExecuteEnd++ = *task;
		}
	}
	m_taskArrayLock.Unlock();

//  TODO : for now signaling is dividing the perf by x10 for task push cost.
//	m_SleepNotification->SendEvent();
	return res;
}

void WorkThread::Run() {
	m_state		= TH_ACTIVE;
	u32 lastRunCount = -1;

	u8 worker = this->GetThreadIndex();
	// Enter

	// IMPORTANT : copy to local space to avoid false sharing !
	spyWorkerFunc	spyFunc = g_spyFunc;
	void*			spyCtx	= g_spyCtx;

	if (spyFunc) { spyFunc(spyCtx, worker, NULL, NULL); }
	while (1) {
		TaskEntry task;
		do
		{
			while (m_bKeepAlive && (PopExecute(&task))) {
				if (spyFunc) {
					spyFunc(spyCtx, worker, task.pTask, &task.param[0]);
				}

				//
				// Splittable.
				//
				if ((task.pTask->m_attribute & (Task::ATTRB_SPLITTABLE | Task::ATTRB_SPLITTED)) == Task::ATTRB_SPLITTABLE) {
					task.pTask->m_attribute |= Task::ATTRB_SPLITTED;

					//
					// Split maximum amount of task around all the same group thread.
					//
					u32 splitCount	= g_ThreadCount * 3;
					u32 amount		= task.pTask->m_streamAmount;
					u32 splitSize	= (amount / splitCount) + 1; // Round up to be sure
					if (splitSize < task.pTask->m_streamGranularity) {
						splitSize	= task.pTask->m_streamGranularity;
						splitCount = (amount + (splitSize-1)) / (splitSize);
					}
					
					task.pTask->m_splitCount = splitCount;
					task.pTask->m_splitSize  = splitSize;

					// Push back other task from 1..n to workers

					u32 splitter	= 1;
					u32 workerIdx	= 0;

					//
					// Split around in fast mode for all in the same group.
					//
					WorkThread* pWorkers = g_threads; // Avoid global var access in loop.
					while (splitter < splitCount) {
						task.param[0].u16[0] = splitter++;
						if (workerIdx >= m_groupCount) {
							workerIdx = 0;
							this->PushFast(&task);
						} else {
							pWorkers[m_group[workerIdx++]].PushFast(&task);
						}
					}
				}

				//
				// Execution
				//
				// pTask->m_runFunc(pTask,worker);
				(*((CppCall*)task.pTask).*task.pTask->m_runFunc)(worker, &task.param[0]);

				// Execution complete
				// We ensure that multiple thread will not split at the same time.
				// TODO OPTIMIZE, replace later with Atomic Counter
				if (task.pTask->m_splitCount == 0) {
					if (task.pTask->m_dependantToCount) { // TODO : for now variable only modified when registering task outside of execution kernel, use Task::ATTRB_PUSHED attribute to garantee.
						DependancyEntry* slot = (DependancyEntry*)task.pTask->m_dependancySlotTo;
						while (slot) {
							slot->m_pToTask.pTask->FinishedExecute(&slot->m_pToTask, this);
							if (slot->m_snext) {
								slot = &slot[slot->m_snext]; // Trick with relative adress
							} else {
								slot = NULL;
							}
						}
					}
				}
			}
		} while( StealTasks() && m_bKeepAlive);

		// Could not steal task, we are in "do nothing mode"
		if (spyFunc) {
			if (lastRunCount != m_gotWork) {
				lastRunCount = m_gotWork;
				spyFunc(spyCtx, worker, (Task*)(-1), 0);
			}
		}

		//
		// Put this logic AFTER the task processing, so when we start the thread
		// we do NOT go to sleep straight.
		//
		if( !m_bNeverIdle ) {
			m_state = TH_ASLEEP;
			if (!m_bIsMainThread) {
				g_SleepNotification.WaitEvent();
			} else {
				// TODO need for signaling : Semaphore !
				// Should be checking semaphore : reach 0, everybody asleep -> Quit
				m_bKeepAlive = false;
				break;
			}
		} else {
			if (m_bIsMainThread) {
				// Again check against global semaphore counter.
			}
		}

		if (!m_bKeepAlive) {
			break;
		}
	}

	// Release the user context when thread is dying
	if ((g_userContextFunc) && (m_userContext != NULL)) {
		g_userContextFunc(m_threadIndex, m_userContext);
	}

	// Avoid user stack reset when restarting the thread.
	m_taskExecuteEnd = m_taskExecuteStart = m_taskExecuteArray;

	m_state = TH_DEAD;
}

void WorkThread::AllowIdleReset(u8 neverIdle) {
	// May not be optimal but rarely used.
	m_modifierLock.Lock();
	if (neverIdle != m_bNeverIdle) {
		m_bNeverIdle = neverIdle;
		// Wake up.
		if (!m_bIsMainThread) {
			if (m_state == TH_ASLEEP) {
				g_SleepNotification.SendEvent();
			}
		}
	} /* else { } No changes*/
	m_modifierLock.Unlock();
}

bool WorkThread::Start(u32 schedulerIndex, bool isMainThread) {
	m_taskArrayLock.Lock();
	m_threadIndex		= schedulerIndex;
	m_threadTick		= 1;
	m_bKeepAlive		= true;
	m_bIsMainThread		= isMainThread;
	m_gotWork			= 0;
	m_taskArrayLock.Unlock();
	if (!isMainThread) {
		return CreateThread(schedulerIndex, g_userStackSize, this);
	} else {
		Run();
		return true;
	}
}

//______________________________________________________________________________________________________________
//
//      TASK Implementation
//______________________________________________________________________________________________________________

Task::Task()
:m_dependantFromCount	(0)
,m_dependantToCount		(0)
,m_scheduleSlot			(NULL_IDX)
,m_runFunc				(NULL)
,m_dependancySlotFrom	(NULL)
,m_dependancySlotTo		(NULL)
,m_attribute			(0)
,m_executeRemainCount	(0)
,m_splitCount			(0)
,m_currSplit			(0)
,m_streamAmount			(0)
,m_streamGranularity	(0)
,m_splitSize			(1)
{
}

Task::~Task() {
	// Remove dependancy from other tasks.
	/*
	DependancyEntry* slot = m_dependancySlotTo;
	while (slot) {
		TSK_UnWaitingForMe(slot->m_pTask.pTask);
		if (slot->m_lnext) {
			slot = &slot[slot->m_lnext]; // Trick with relative adress
		} else {
			slot = NULL;
		}
	}

	slot = m_dependancySlotFrom;
	while (slot) {
		TSK_UnWaitingForTask(slot->m_pTask.pTask);
		if (slot->m_lnext) {
			slot = &slot[slot->m_lnext]; // Trick with relative adress
		} else {
			slot = NULL;
		}
	}
	*/
}

void Task::TSK_ResetStreamAmount() {
	m_dependancyLock.Lock();
	m_streamAmount = 0;
	m_dependancyLock.Unlock();
}

u32 Task::TSK_GetSplit(u32* start, u32* end) {
	// TODO assert if start null and end is not, and opposite.

	m_dependancyLock.Lock();
	u32 split = m_currSplit++;
	m_splitCount--;
	m_dependancyLock.Unlock();

	if (start && end) {
		u32 s = split * m_splitSize;
		u32 e = s + m_splitSize;
		if (e > m_streamAmount) {
			e = m_streamAmount;
		}
		*start = s;
		*end   = e;
	}

	return split;
}

void Task::TSK_IncrementStreamAmount(u32 add) {
	m_dependancyLock.Lock();
	m_streamAmount += add;
	m_dependancyLock.Unlock();
}

void Task::TSK_SetGranularity(u32 granularity) {
	m_dependancyLock.Lock();
	m_streamGranularity = granularity;
	m_dependancyLock.Unlock();
}

void Task::TSK_SetSplittable() {
	m_attribute = ATTRB_SPLITTABLE;
}

bool Task::TSK_WaitingForTask(Task* pTask, TaskParam* pParam) {
	TaskSlot slot = AllocDependant();

	if (slot != NULL_IDX) {
		//
		// Do the link in 'this'
		//
		DependancyEntry* pEntry = &g_dependanceEntries[slot];
		pEntry->m_lprev = 0;
		pEntry->m_sprev = 0;
		if (m_dependancySlotFrom) {
			pEntry->m_lnext = ((DependancyEntry*)m_dependancySlotFrom)->m_self - pEntry->m_self; // End
		} else {
			pEntry->m_lnext = 0;
		}
		m_dependancySlotFrom = pEntry;
		pEntry->m_pTask.pTask = pTask;
		if (pParam) {
			pEntry->m_pTask.param[0] = pParam[0];
			pEntry->m_pTask.param[1] = pParam[1];
			pEntry->m_pTask.param[2] = pParam[2];
		}
		m_dependantFromCount++;
		m_executeRemainCount = m_dependantFromCount;

		//
		// Do the link in 'pTask'
		//
		if (pTask->m_dependancySlotTo) {
			pEntry->m_snext = ((DependancyEntry*)(pTask->m_dependancySlotTo))->m_self - pEntry->m_self; // End
		} else {
			pEntry->m_snext = 0;
		}
		pTask->m_dependancySlotTo = pEntry;
		pEntry->m_pToTask.pTask = this;
		pTask->m_dependantToCount++;

		return true;
	} else {
		return false;
	}
}

/*
void Task::TSK_UnWaitingForTask	(Task* pTask) {
	// TODO IMPLEMENT
	EC_ASSERT_MSG(false, "TODO IMPLEMENT");
}
*/
void Task::FinishedExecute(void* pTask, WorkThread* pWorker) {
	m_dependancyLock.Lock();
	if ((--m_executeRemainCount) == 0) {
		// Push as 0 dependancy in the thread of the ended task.
		pWorker->PushExecute((TaskEntry*)pTask);
		// Reset counter after push.
		m_executeRemainCount = this->m_dependantFromCount;
	}
	m_dependancyLock.Unlock();
}

void InitDependancies() {
	for(TaskSlot n = 0; n < MAX_DEPENDANCE_ENTRIES; n++) {
		g_dependanceEntries[n].m_prev		= n-1;
		g_dependanceEntries[n].m_next		= n+1;
		g_dependanceEntries[n].m_self		= n;
	}

	g_dependanceEntries[IDX_FREE_START].m_prev				= NULL_IDX;
	g_dependanceEntries[IDX_FREE_START].m_next				= 2;

	g_dependanceEntries[IDX_FREE_END  ].m_prev				= MAX_DEPENDANCE_ENTRIES - 1;
	g_dependanceEntries[IDX_FREE_END  ].m_next				= NULL_IDX;

	g_dependanceEntries[2].m_prev							= IDX_FREE_START;
	g_dependanceEntries[MAX_DEPENDANCE_ENTRIES-1].m_next	= IDX_FREE_END;
}

TaskSlot AllocDependant() {
	g_dependantLockFree.Lock();
	TaskSlot free = g_dependanceEntries[0].m_next;
	if (free != IDX_FREE_END) {
		// Take from free pool.
		TaskSlot n = g_dependanceEntries[free].m_next;
		g_dependanceEntries[0].m_next		= n;
		g_dependanceEntries[n].m_prev		= 0;
	} else {
		free = NULL_IDX;
	}
	g_dependantLockFree.Unlock();
	return free;
}

void FreeDependant(TaskSlot	slot) {
	g_dependantLockFree.Lock();
	TaskSlot next = g_dependanceEntries[IDX_FREE_START].m_next;
	g_dependanceEntries[next].m_prev	= slot;
	g_dependanceEntries[0].m_next		= slot;

	g_dependanceEntries[slot].m_next		= next;
	g_dependanceEntries[slot].m_prev		= IDX_FREE_START;
	g_dependantLockFree.Unlock();
}
