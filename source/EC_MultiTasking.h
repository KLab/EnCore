#ifndef EC_MULTITASKING_H
#define EC_MULTITASKING_H

#include "EC_BaseType.h"

// -------------------------------------------------------
//    Compilation constant (Application Specific)
// -------------------------------------------------------

// Maximum number of tasks per thread.
#define	MAX_TASK_COUNT			(256)

// Maximum number of core.		(internal limit is 32 due to API implementation internal)
#define MAX_THREAD_COUNT		(4)

// Maximum number of tasks that stay alive between frames.
#define MAX_TASK_PERSISTANT		(2048)

// TODO
#define MAX_DEPENDANCE_ENTRIES	(3500)


// -------------------------------------------------------
//    User callback customization function.
// -------------------------------------------------------
class Task;

/**
	Optionnal User function associated when creating/destroying a worker thread.

	Parameters :
	- Index is the worker ID.
	- NULL or old context pointer.

	Function has two behaviors :
	1 - Allocate a user context when input is NULL, return the newly allocated context.
	2 - Release  a user context when input is a valid context, return value is ignored.

	Example of implementation :
	
	if (oldContext) {
		free(oldContext);
		return NULL;
	} else {
		return malloc(sizeof(MyContext));
	}
 */
typedef	void*	(*handleWorkerUserContext)	(u32 index, void* oldContext);

/**
	Optionnal User function associated with workers to spy on workers behavior
	
	Parameters :
	- Context given for spying when registering the spying function.
	- Currently executing worker index.
	- Task to be executed.

	Warning : this function is going to be executed in a multithreaded context, the spy context is the same
	for ALL workers and multiple call could occurs at the same time.

	So, storing a slot inside the spy context for each worker would be one way of dealing with different workers at the same time.

	Also, if you care about performance in the spy function, you should avoid using lock or any ressources that is going to be
	shared between multiple thread in a locked fashion. It will seriously impact your performance as the spy function is called
	for EACH TASK executed in EACH WORKER.
 */
typedef void	(*spyWorkerFunc)			(void* spyCtx, u32 workerIndex, Task* pTask);


/** C++ type Execution function for a task */
class CppCall {
	void execFunc		(u8 worker);
};
typedef bool	(CppCall::*execFunc)		(u8 worker);

/** Error function */
// TODO : future feature, api to use it.
// typedef void	(*errorFunc)				(u32 worker, u32 errCode);

// ============================================================================
// APIs
// ============================================================================

static const int EC_ALL	= 255;	// Used for ALL group or ALL workers when possible.

// ============================================================================
// Task execution dependancy & control
// ============================================================================

// ============================================================================
// Workers Control
// ============================================================================

extern "C" {

/** Assign a a group to a worker */
void	WRK_SetGroup			(u8 worker, u8 groupID);
/** Get the state of a worker */
u8		WRK_GetState			(u8 worker);
/** Push a task to a worker */
bool	WRK_PushExecute			(u8 worker, Task* task);
/** Get user context associated with a worker (See WRK_CreateWorkers function)) */
void*	WRK_GetUserContext		(u8 worker);
/** When a task is not complete and is waiting for an asynchronous signal/response, instead of doing a while loop that will lock the worker,
    we call WRK_TaskYield and return as soon as possible.

	fastMode insert back our current task just AFTER the next task in this queue.

	So the execution looks like :
	[A]
		WRK_TaskYield
		return
	[B]
		...
	[A]
		Check

	When not in fast mode, the current task is pushed back at the END of the queue,
	thus, response time will be delayed further.
 */
void	WRK_TaskYield			(u8 worker, u8 fastMode);
/** Return the number of workers currently sleeping */
u8		WRK_GetSleepingCount	();

// ============================================================================
// Worker Creation 
// ============================================================================

bool	WRK_CreateWorkers		(u32 count, bool includeThisThread, u32 stackSize, handleWorkerUserContext userFunc);
void	WRK_ShutDown			(u8 worker_or_all, bool async); // 255 = ALL
void	WRK_SetSpy				(spyWorkerFunc cbSpyFunc, void* spyContext);
u8		WRK_GetHWWorkerCount	();
void	WRK_AllowIdle			(bool idling);

// Init & Release library.
bool	WRKLIB_Init				();
void	WRKLIB_Release			();

}

//==============================================================================
// Private Header
//==============================================================================
#include "EC_MultiTaskingPlatform.h"

#define EC_ASSERT(a)		EC_ASSERT_MSG(a,"Error")
#define EC_ASSERTALWAYS		EC_ASSERT(false)

#define MAX_TASK_COUNT_INTERNAL		(MAX_TASK_COUNT+1)

#define NULL_IDX			(0xFFFF)

bool CreateThread	(u8 workerIndex, u32 stackSize, void* context);
void RunThread		(void* context);

// Avoid visibility from user & into documentation.
class TLock {
public:
	TLock		() { }
	~TLock		() { }
//inline
//void TLock::Lock()				{	EnterCriticalSection(&m_handle);									}
//inline
//void TLock::Unlock()			{	LeaveCriticalSection(&m_handle);									}
	void Lock	();
	void Unlock	();

	bool init();
	void release();
	LXLOCK_HANDLE	m_handle;
};

typedef u16	TaskSlot;

struct DependancyEntry {
	TaskSlot	m_prev;
	TaskSlot	m_next;

	s16			m_lprev;
	s16			m_lnext;

	s16			m_sprev;
	s16			m_snext;

	TaskSlot	m_self;
	Task*		m_pTask;
	Task*		m_pToTask;
};

/**
	Base class for Task.
	User need to use this class to create his own tasks.

	1. Derive the class implement the following functions
	- [MUST  ] Run function.
	- [OPTION] Split function.

	2. Call SetAttribute in user class constructor.
 */
__declspec(align(64)) class Task {
	friend class WorkThread;
public:
	Task();
	~Task();
private:
	static const u32	ATTRB_NONSPLITTABLE	= 0x00;
	static const u32	ATTRB_SPLITTABLE	= 0x01;
	static const u32	ATTRB_SUICIDE		= 0x02; /* Ask for the task to be destroyed when execution is complete : execute once model */
	static const u32	ATTRB_YIELD			= 0x04;
	// 4,8,10,20,40 are free for now.
	static const u32	ATTRB_SPLITTED		= 0x80;
public:
	inline void	 TSK_SetTaskID			(u32 userid)	{ m_taskID = userid;					}
	inline u32	 TSK_GetTaskID			()				{ return m_taskID;						}
	inline void	 TSK_SetRunFunction		(execFunc func)	{ m_runFunc = func;						}

	/* Note : used by WRK_TaskYield */
	inline void  TSK_SetYield			()				{ m_attribute |= Task::ATTRB_YIELD;		}
	inline void  TSK_UnsetYield			()				{ m_attribute &=~Task::ATTRB_YIELD;		}
			
	/** This task is waiting for the task pTask to end to perform its execution.
		Note: it is possble for a task to wait for multiple tasks							*/
			bool TSK_WaitingForTask		(Task* pTask);

	/** pTask is waiting for this task to end to perform its execution.
		Note: it is possble for a task to wait for multiple tasks							*/
	inline	bool TSK_WaitingForMe		(Task* pTask)	{ return pTask->TSK_WaitingForTask(this);}

	/** Remove the dependancy between this task and the wait for the end of pTask.			*/
			void TSK_UnWaitingForTask	(Task* pTask);

	/** Remove the dependancy between pTask and the wait for the end of this task.			*/
	inline	void TSK_UnWaitingForMe		(Task* pTask)	{ pTask->TSK_UnWaitingForTask(this);	}

	/** Remove all dependancies in both directions											*/
			void TSK_ClearDependencies	();
	/** Task is destroyed after execution													*/
	inline	void TSK_Suicide			()				{ m_attribute |= ATTRB_SUICIDE;			}

protected:
	/**
			Must be called inside the constructor to define if the task is splittable or not.
	 */
			void TSK_SetSplittable		();
			void TSK_ResetStreamAmount	();
			void TSK_IncrementStreamAmount	(u32 add);
			void TSK_SetGranularity		(u32 granularity);
	inline  u32* TSK_GetStreamInfo		() { return &m_streamAmount; }
	inline	u32	 TSK_GetSplitSize		() { return m_splitSize;     }
			u32  TSK_GetSplit			(u32* start, u32* end);

private:
			void FinishedExecute		(Task* pTask, WorkThread* pWorker);

	u32			m_streamAmount;
	u32			m_streamGranularity;
	u32			m_splitSize;
	u32			m_taskID;

	TLock		
				m_dependancyLock;
	TaskSlot	m_scheduleSlot;
	DependancyEntry*	
				m_dependancySlotFrom;
	DependancyEntry*	
				m_dependancySlotTo;
	execFunc	m_runFunc;

	u16			m_dependantFromCount;
	u16			m_dependantToCount;
	u16			m_executeRemainCount;
	u16			m_splitCount;
	u16			m_currSplit;
	u8			m_attribute;
};

#endif
