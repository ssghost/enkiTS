// Copyright (c) 2013 Doug Binks
// 
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
// 
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgement in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

#include <assert.h>

#include "TaskScheduler.h"
#include "LockLessMultiReadPipe.h"



using namespace enki;


static const uint32_t PIPESIZE_LOG2 = 8;
static const uint32_t SPIN_COUNT = 100;

// each software thread gets it's own copy of gtl_threadNum, so this is safe to use as a static variable
static THREAD_LOCAL uint32_t                             gtl_threadNum       = 0;

namespace enki 
{
	struct TaskSetInfo
	{
		ITaskSet*           pTask;
		TaskSetPartition    partition;
	};

	// we derive class TaskPipe rather than typedef to get forward declaration working easily
	class TaskPipe : public LockLessMultiReadPipe<PIPESIZE_LOG2,enki::TaskSetInfo> {};

	struct ThreadArgs
	{
		uint32_t		threadNum;
		TaskScheduler*  pTaskScheduler;
	};
}


THREADFUNC_DECL TaskScheduler::TaskingThreadFunction( void* pArgs )
{
	ThreadArgs args					= *(ThreadArgs*)pArgs;
	uint32_t threadNum				= args.threadNum;
	TaskScheduler*  pTS				= args.pTaskScheduler;
    gtl_threadNum      = threadNum;
    
    uint32_t spinCount = 0;
    while( pTS->m_bRunning )
    {
        if(!pTS->TryRunTask( threadNum ) )
        {
            // no tasks, will spin then wait
            ++spinCount;
            if( spinCount > SPIN_COUNT )
            {
                EventWait( pTS->m_NewTaskEvent, EVENTWAIT_INFINITE );
                spinCount = 0;
            }
        }
    }

    AtomicAdd( &pTS->m_NumThreadsRunning, -1 );
    return 0;
}


void TaskScheduler::StartThreads()
{
    if( m_bHaveThreads )
    {
        return;
    }
    m_bRunning = true;

    m_NewTaskEvent = EventCreate();

    // we create one less thread than m_NumThreads as the main thread counts as one
    m_pThreadNumStore = new ThreadArgs[m_NumThreads];
    m_pThreadIDs      = new threadid_t[m_NumThreads];
    for( uint32_t thread = 1; thread < m_NumThreads; ++thread )
    {
		m_pThreadNumStore[thread].threadNum      = thread;
		m_pThreadNumStore[thread].pTaskScheduler = this;
        ThreadCreate( &m_pThreadIDs[thread], TaskingThreadFunction, &m_pThreadNumStore[thread] );
        ++m_NumThreadsRunning;
    }

    // ensure we have sufficient tasks to equally fill either all threads including main
    // or just the threads we've launched, this is outside the firstinit as we want to be able
    // to runtime change it
    m_NumPartitions = m_NumThreads * (m_NumThreads - 1);


    m_bHaveThreads = true;
}

void TaskScheduler::StopThreads( bool bWait_ )
{
    if( m_bHaveThreads )
    {
        // wait for them threads quit before deleting data
        m_bRunning = false;
        while( bWait_ && m_NumThreadsRunning )
        {
            // keep firing event to ensure all threads pick up state of m_bRunning
            EventSignal( m_NewTaskEvent );
        }

        for( uint32_t thread = 1; thread < m_NumThreads; ++thread )
        {
            ThreadTerminate( m_pThreadIDs[thread] );
        }

		m_NumThreads = 0;
        delete[] m_pThreadNumStore;
        delete[] m_pThreadIDs;
        m_pThreadNumStore = 0;
        m_pThreadIDs = 0;
        EventClose( m_NewTaskEvent );

        m_bHaveThreads = false;
    }
}

bool TaskScheduler::TryRunTask( uint32_t threadNum )
{
    // check for tasks
    TaskSetInfo info;
    bool bHaveTask = m_pPipesPerThread[ threadNum ].WriterTryReadFront( &info );

    if( m_NumThreads )
    {
        uint32_t checkOtherThread = threadNum + 1;
        checkOtherThread %= m_NumThreads;
        while( !bHaveTask && checkOtherThread != threadNum )
        {
            bHaveTask = m_pPipesPerThread[ checkOtherThread ].ReaderTryReadBack( &info );
            ++checkOtherThread;
            checkOtherThread %= m_NumThreads;
        }
    }
        
    if( bHaveTask )
    {
        // the task has already been divided up by AddTaskSetToPipe, so just run it
        info.pTask->ExecuteRange( info.partition, threadNum );
        AtomicAdd( &info.pTask->m_CompletionCount, -1 );
    }

    return bHaveTask;

}


void    TaskScheduler::AddTaskSetToPipe( ITaskSet* pTaskSet )
{
    TaskSetInfo info;
    info.pTask = pTaskSet;
    info.partition.start = 0;
    info.partition.end = pTaskSet->m_SetSize;

    // no one owns the task as yet, so just add to count
    pTaskSet->m_CompletionCount = 0;

    // divide task up and add to pipe
    uint32_t numToRun = info.pTask->m_SetSize / m_NumPartitions;
    if( numToRun == 0 ) { numToRun = 1; }
    uint32_t rangeLeft = info.partition.end - info.partition.start ;
    while( rangeLeft )
    {
        if( numToRun > rangeLeft )
        {
            numToRun = rangeLeft;
        }
        info.partition.start = pTaskSet->m_SetSize - rangeLeft;
        info.partition.end = info.partition.start + numToRun;
        rangeLeft -= numToRun;

        // add the partition to the pipe
        AtomicAdd( &info.pTask->m_CompletionCount, +1 );
        if( !m_pPipesPerThread[ gtl_threadNum ].WriterTryWriteFront( info ) )
        {
            EventSignal( m_NewTaskEvent );
            info.pTask->ExecuteRange( info.partition, gtl_threadNum );
            --pTaskSet->m_CompletionCount;
        }
    }
    EventSignal( m_NewTaskEvent );

}

void    TaskScheduler::WaitforTaskSet( const ITaskSet* pTaskSet )
{
	if( pTaskSet )
	{
		while( pTaskSet->m_CompletionCount )
		{
			TryRunTask( gtl_threadNum );
			// should add a spin then wait for task completion event.
		}
	}
	else
	{
			TryRunTask( gtl_threadNum );
	}
}

void    TaskScheduler::WaitforAll()
{
    bool bHaveTasks = true;
    while( bHaveTasks )
    {
        TryRunTask( gtl_threadNum );
        bHaveTasks = false;
        for( uint32_t thread = 1; thread < m_NumThreads; ++thread )
        {
            if( !m_pPipesPerThread[ thread ].IsPipeEmpty() )
            {
                bHaveTasks = true;
                break;
            }
        }
     }
}

void    TaskScheduler::WaitforAllAndShutdown()
{
    WaitforAll();
    StopThreads(true);
	delete[] m_pPipesPerThread;
    m_pPipesPerThread = 0;
}

uint32_t        TaskScheduler::GetNumTaskThreads() const
{
    return m_NumThreads;
}

TaskScheduler::TaskScheduler()
{
}

TaskScheduler::~TaskScheduler()
{
    StopThreads( true ); // Stops threads, waiting for them.

    delete[] m_pPipesPerThread;
    m_pPipesPerThread = 0;
}

void    TaskScheduler::Initialize( uint32_t numThreads_ )
{
    StopThreads( true ); // Stops threads, waiting for them.
    delete[] m_pPipesPerThread;

	m_NumThreads = numThreads_;

    m_pPipesPerThread = new TaskPipe[ m_NumThreads ];

    StartThreads();
}

void   TaskScheduler::Initialize()
{
	Initialize( GetNumHardwareThreads() );
}