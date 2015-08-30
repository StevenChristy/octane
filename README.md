# octane

C++11 memory accelerator. Simply add octane.cpp to your projects to increase performance of new/delete (array versions too). Standard memory manager considerations apply when using shared libraries. If the shared library deletes memory allocated by a call to new then it will also need octane.cpp otherwise octane will not work for you.

	Status: Stable.
	Version: 0.5
	License: GPL v3

## Features

* MT-Safe
* Optimized for in environments where threads fighting each other for access to the memory manager.
* Can outperform the default memory manager by as much as 200% in testing. (Real world performance needs more testing.)
* Tunable for your applications needs. Its already tuned well, but maybe you need a little more.
* Lock-free

## Contraindications

* Embedding octane in your dlls is difficult - missing source code, licensing conflicts, etc.
* You app uses lots of threads - but not much heap allocation.
* Single-threaded app.
* Doesn't change malloc - just the C++ allocators.

## Tuning

The octane memory manager should be tuned to meet the needs of your application. This can be done by passing defines when compiling octane.cpp. The following defines are supported:

### OCTANE_POOL_SIZE

* Default pool size is 65400
* Larger pool sizes cause threads to hold more memory longer. If you find its holding too much memory consider lowering to 32700. It should not significantly impact performance. 
	
### OCTANE_TRACKED_POOL_COUNT

* Maximum number of pools each thread will track.
* Default pool count is 256. Lower this value at your own risk. 

### OCTANE_RECYLCE_THRESHOLD
 
* Number of bytes expressed as an integer
* Defaults to 128
* Acts to cull pools from being tracked by.

### OCTANE_DEBUG_METRICS
	
* Tracks a few basic stats to make sure there are no leaks.
	
### OCTANE_DISABLE
	
* To compile your application without octane.
	
## Compiling the test program

With Octane (my results are around 5000ms):

	g++ -O3 test.cpp --std=c++11 -pthread -o test
	


Without Octane (my results are around 20000ms):

	g++ -O3 test.cpp --std=c++11 -pthread -o test -DOCTANE_DISABLE=1

YMMV! Real world improvements will depend on thread contention in the memory manager.
