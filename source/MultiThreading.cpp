#include "EC_MultiTasking.h"
#include <stdio.h>

class TestTask : public Task {
public:
	TestTask();
	void Execute(u8 worker);
	u32 m_time;
};

TestTask::TestTask() 
{
	m_time = 0;
	TSK_SetRunFunction((execFunc)&TestTask::Execute);
}

void TestTask::Execute(u8 worker) {
	WRK_PushExecute(worker,this);
	m_time++;
}

int main(int argc, char* argv[])
{
	// Setup library
	if (WRKLIB_Init()) {
		TestTask* pTaskT1	= new TestTask();
		TestTask* pTaskT2	= new TestTask();

		WRK_PushExecute(0, pTaskT1);
		WRK_PushExecute(1, pTaskT2);

		// WRK_AllowIdle(false);

		//
		// Start Cores
		//
		WRK_CreateWorkers(2, false, 16384, NULL);

		int sec = 0;
		Sleep(1000);
		while (1) {
			u32 sum = pTaskT1->m_time + pTaskT2->m_time;
			printf("[%i][%i] %i\n", pTaskT1->m_time, pTaskT2->m_time, ++sec);
			pTaskT1->m_time = 0;
			pTaskT2->m_time = 0;
			Sleep(1000);
		}

		delete pTaskT1;
		delete pTaskT2;

		// Never reach here in our test but let's be clean anyway.
		WRKLIB_Release();
	}

	return 0;
}
