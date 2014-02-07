#include "EC_MultiTasking.h"
#include <stdio.h>

typedef long long int s64;
typedef long long unsigned int u64;

// ===============================================================================
// Execution time
u64 nanotime()
{
	LARGE_INTEGER counter;
	LARGE_INTEGER freq;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	s64 val = counter.QuadPart * 1000000000LL / freq.QuadPart;
	return (u64)val;
}

class BenchTask: public Task {
public:
	u64 startTime;
	u64 endTime;
	u8  worker;
};

// ===============================================================================
// Task computing the min/max and sum of a greyscale image.
//
class WorkSumMinMax : public BenchTask {
public:
  WorkSumMinMax(u8* source, u32 width, u32 height);
  void DoSumMinMax(u8 worker, TaskParam* pParam);
  
  u8*    m_greyScaleImage;
  u32    m_width;
  u32    m_height;
  u32    m_sum;
  u32    m_min;
  u32    m_max;
};

WorkSumMinMax::WorkSumMinMax(u8* source, u32 width, u32 height) {
    m_width = width;
    m_height= height;
    m_greyScaleImage = source;
	TSK_SetRunFunction((execFunc)&WorkSumMinMax::DoSumMinMax);
}

void WorkSumMinMax::DoSumMinMax(u8 worker, TaskParam* pParam) {
	startTime = nanotime();
	this->worker = worker;

	// 初期化
    u32 size = m_width * m_height;
    u8* parse;
    u8* parseEnd= &m_greyScaleImage[size];
    
    u32 max = 0;
    u32 min = 255;
    u32 sum = 0;
    
    // 最大、最小、総和を計算
	parse     = m_greyScaleImage;
	while (parse < parseEnd) {
		u8 val = *parse++;
		sum += val;
		if (val > max)    { max = val; }
		if (val < min)    { min = val; }
	}

    m_max = max; m_min = min; m_sum = sum;

	endTime = nanotime();
}

// ===============================================================================
// Task computing the histogram of a greyscale image.
//

class WorkHistogram : public BenchTask {
public:
  WorkHistogram(u8* source, u32 width, u32 height);
  void DoComputeHistogram(u8 worker, TaskParam* pParam);
  u8*    m_greyScaleImage;
  u32    m_width;
  u32    m_height;
  
  u32    m_histogram[256];
};

WorkHistogram::WorkHistogram(u8* source, u32 width, u32 height) {
    m_width = width;
    m_height= height;
    m_greyScaleImage = source;
	TSK_SetRunFunction((execFunc)&WorkHistogram::DoComputeHistogram);
}


void WorkHistogram::DoComputeHistogram(u8 worker, TaskParam* pParam) {
    startTime = nanotime();
	this->worker = worker;
	
	u32 size = m_width * m_height;
    u8* parse;
    u8* parseEnd= &m_greyScaleImage[size];
    
    u32* histogram = m_histogram;
    
    for (int n=0; n<256; n++) {
        histogram[n] = 0;
    }
    
	parse     = m_greyScaleImage;
	while (parse < parseEnd) {
		u8 val = *parse++;
		histogram[val]++;
	}

	endTime = nanotime();
}

// ===============================================================================
// This task use the result of the 2 first tasks to create 2 histograms seperated by the average pixel value
//
class WorkSeperate : public BenchTask {
public:
  WorkSeperate(WorkHistogram* taskA, WorkSumMinMax* taskB)
  : m_histo (taskA)
  , m_sum   (taskB)
  , m_complete(false)
  {
	  TSK_SetRunFunction((execFunc)&WorkSeperate::SeperateHistogram);
  }
  
  void SeperateHistogram(u8 worker, TaskParam* pParam);
  
  WorkHistogram* m_histo;
  WorkSumMinMax* m_sum;
  u32 m_histogramLow [256];
  u32 m_histogramHigh[256];
  bool m_complete;
};

void WorkSeperate::SeperateHistogram(u8 worker, TaskParam* pParam) {
	startTime = nanotime();
	this->worker = worker;

	// 画像内の平均値を計算
	u32 average = (m_sum->m_sum) / (m_sum->m_width * m_sum->m_height);
	
	// 初期化
	for (int n=0; n<256; n++) {
		m_histogramLow[n] = 0; m_histogramHigh[n] = 0;
	}
	
	u32* histoData = m_histo->m_histogram;
    for (u32 n=0; n < 256; n++) {
		if (n < average) { m_histogramLow [n] = histoData[n]; }
		            else { m_histogramHigh[n] = histoData[n]; }
    }
	
	endTime = nanotime();
	m_complete = true;
}

int main(int argc, char* argv[])
{
  // Setup library
  if (WRKLIB_Init()) {
    // Create Greyscale image. (no error handling),
	// Default random value, memory not init.
	
	#define SIZE	(3000)

    u8* image = new u8[SIZE * SIZE];
	// Fill with samples.
	for (int n=0; n < SIZE*SIZE; n++) {
		image[n] = n;
	}
    
    // Create 3 tasks.
    WorkHistogram* histo = new WorkHistogram(image,SIZE,SIZE);
    WorkSumMinMax* sum   = new WorkSumMinMax(image,SIZE,SIZE);
    WorkSeperate*  histo2= new WorkSeperate (histo, sum);
	
	// Task WorkSeperate execute when Histogram AND SumMinMax complete.
	// WorkSeperate PUSHED AUTOMATICALLY on completion by dependance.
	histo2->TSK_WaitingForTask(histo, NULL);
	histo2->TSK_WaitingForTask(sum, NULL);

	// Start Cores : 2 threads.
	WRK_CreateWorkers(2, false, 16384, NULL);
    
	while (1) {
		// Push the histogram and sum/minmax task.
		WRK_PushExecute(0, histo, NULL);
		WRK_PushExecute(0, sum, NULL);

		// Check Performances
		u32 sec = 0;
		while (!histo2->m_complete) {
		  // Wait...
		  Sleep(5); // Avoid having the main thread using the CPU
		}
	
		printf("====\nHisto Time uSec:%llu @ %i\n", (histo->endTime - histo->startTime) / 1000,histo->worker);
              printf("Sum   Time uSec:%llu @ %i\n", (  sum->endTime -   sum->startTime) / 1000,  sum->worker);

		// Buggy for counter overflow but correct most of the time.
		u64 min = min(histo->startTime, sum->startTime) / 1000;
		u64 max = max(histo->endTime, sum->endTime)     / 1000;
		printf("Min:%llu Max:%llu -> Delta : %llu\n", min, max, max-min);

		/* Dump histogram for debug.
		for (int n=0; n<256; n++) {
			printf("%i:L=_%i H= %i\n",n,histo2->m_histogramLow[n], histo2->m_histogramHigh[n]);
		}*/

		histo2->m_complete = false;
	}
	delete histo; delete sum; delete histo2;
	delete image;
    
	// Never reach here in our test but let's be clean anyway.
	WRKLIB_Release();
  }
}
