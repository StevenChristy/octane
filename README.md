# octane

C++11 memory accelerator. Simply add octane.cpp to your projects to increase performance of new/delete (array versions too). Standard memory manager considerations apply when using shared libraries. If the shared library deletes memory allocated by a call to new then it will also need octane.cpp otherwise octane will not work for you.

	Status: Stable.
	Version: 0.6
	License: GPL v3

## Features

* MT-Safe
* Optimized for environments where threads are fighting for access to the memory manager.
* Can outperform the default memory manager by as much as 75% in testing.
* Tunable for your applications needs.
* Lock-free
* 16-byte aligned internally.

## Contraindications

* Embedding octane in your dlls is difficult - missing source code, licensing conflicts, etc.
* You app uses lots of threads - but not much heap allocation.
* Your app is single threaded - it probably won't benefit much.
* If you need a drop-in replacement for malloc this is not it.

## Alignments greater than 16-bytes

With octane it is easy to construct classes on the heap with alignments of 32, 64 or even 128 bytes. First declare your class like:

	class alignas(32) My32bytealignedClass : public ...
	{
	...
	};

Then construct these classes aligned using new_aligned<T>(Args...) defined in octane.hpp. Like:
	
	My32bytealignedClass *ptr = new_aligned<My32bytealignedClass>(...);

## Tuning

The octane memory manager should be tuned to meet the needs of your application. This can be done by passing defines when compiling octane.cpp or by editing octane.hpp manually. The following defines are supported:

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
	
## Compiling and Running the test program

With Octane:

	g++ -O3 test.cpp --std=c++11 -pthread -o test
	./test

My results were between 5300 ms and 6500 ms over several successive tests.	

Without Octane:

	g++ -O3 test.cpp --std=c++11 -pthread -o test -DOCTANE_DISABLE=1
	./test

My results without octane were between 23700 ms and 35000 ms over several successive tests. The time spend resolving contentions varied widely but in no case did the result come close to what could be achieved using octane.

YMMV! Real world improvements will depend on how much time is being spent resolving thread contention in the memory manager.

## Roadmap

* TODO: Add an API to allow threads to customize the memory manager attributes on a per thread basis.

