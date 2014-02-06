#include "EC_MultiTasking.h"
#include <stdio.h>

class TestTask : public Task {
public:
	TestTask();
	void Execute(u8 worker, TaskParam* param);
	u32 m_time;
};

TestTask::TestTask() 
{
	m_time = 0;
	TSK_SetRunFunction((execFunc)&TestTask::Execute);
}

void TestTask::Execute(u8 worker, TaskParam* param)	{
	WRK_PushExecute(worker,this, NULL);
	m_time++;
}

int main(int argc, char* argv[])
{
	// Setup library
	if (WRKLIB_Init()) {
		TestTask* pTaskT1	= new TestTask();
		TestTask* pTaskT2	= new TestTask();
		TestTask* pTaskT3	= new TestTask();
		TestTask* pTaskT4	= new TestTask();
		TestTask* pTaskT5	= new TestTask();
		TestTask* pTaskT6	= new TestTask();
		TestTask* pTaskT7	= new TestTask();
		TestTask* pTaskT8	= new TestTask();

		WRK_PushExecute(0, pTaskT1, NULL);
		WRK_PushExecute(1, pTaskT2, NULL);
		WRK_PushExecute(2, pTaskT3, NULL);
		WRK_PushExecute(3, pTaskT4, NULL);
		WRK_PushExecute(4, pTaskT5, NULL);
		WRK_PushExecute(5, pTaskT6, NULL);
		WRK_PushExecute(6, pTaskT7, NULL);
		WRK_PushExecute(7, pTaskT8, NULL);

		// WRK_AllowIdle(false);

		//
		// Start Cores
		//
		WRK_CreateWorkers(7, false, 16384, NULL);

		int sec = 0;
		Sleep(1000);
		while (1) {
			u32 sum = pTaskT1->m_time + pTaskT2->m_time + pTaskT3->m_time + pTaskT4->m_time;
			sum += pTaskT5->m_time + pTaskT6->m_time + pTaskT7->m_time + pTaskT8->m_time;
			printf("%i/%i/%i/%i/%i/%i/%i/%i Total %i %i\n", 
				pTaskT1->m_time, pTaskT2->m_time,pTaskT3->m_time,pTaskT4->m_time, 
				pTaskT5->m_time, pTaskT6->m_time,pTaskT7->m_time,pTaskT8->m_time, 
				sum, ++sec);
			pTaskT1->m_time = 0;
			pTaskT2->m_time = 0;
			pTaskT3->m_time = 0;
			pTaskT4->m_time = 0;
			pTaskT5->m_time = 0;
			pTaskT6->m_time = 0;
			pTaskT7->m_time = 0;
			pTaskT8->m_time = 0;
			Sleep(1000);
		}

		delete pTaskT1;
		delete pTaskT2;

		// Never reach here in our test but let's be clean anyway.
		WRKLIB_Release();
	}

	return 0;
}
