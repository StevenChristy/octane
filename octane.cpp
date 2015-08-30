#include <atomic>
#include <memory>
#include <x86intrin.h>

using namespace std;

//#define OCTANE_DISABLE 1
#ifndef OCTANE_DISABLE

#if !defined(OCTANE_POOL_SIZE) || (OCTANE_POOL_SIZE < 4096)
#define OCTANE_POOL_SIZE 65536
#endif

#if !defined(OCTANE_TRACKED_POOL_COUNT) || (OCTANE_TRACKED_POOL_COUNT < 64)
#define OCTANE_TRACKED_POOL_COUNT 64
#endif

#if !defined(OCTANE_RECYLCE_THRESHOLD) || (OCTANE_RECYLCE_THRESHOLD < 128)
#define OCTANE_RECYLCE_THRESHOLD 128
#endif

#define OCTANE_DEBUG_METRICS 1
#if OCTANE_DEBUG_METRICS
#include <iostream>
#define DEBUG_METRIC_ADD(metric, val) (metric += val)
#define DEBUG_METRIC(metric) atomic_int metric
#else
#define DEBUG_METRIC_ADD(metric, val) 
#define DEBUG_METRIC(metric) 
#endif

DEBUG_METRIC(dbgAllocatorCount);
DEBUG_METRIC(dbgPoolCount);
DEBUG_METRIC(dbgRootCount);

struct AllocatorPool;
struct AllocatorBlock;
struct AllocatorRoot
{
    atomic_int     RefCount;
    AllocatorPool *Pools[OCTANE_TRACKED_POOL_COUNT];
    int            FreePools;
    
    void release() {
        if ( --RefCount == 0 ) {
            free(this);
            DEBUG_METRIC_ADD(dbgRootCount, -1);
        }
    }
};

struct AllocatorPool
{
    AllocatorRoot *Root;
    int            PoolSize;
    atomic_int     PoolFree;
    atomic_int     PoolReturned;
    atomic_int     RefCount;    
       
    void release() {
        if ( --RefCount == 0 ) {
            if ( Root ) {
                int Free = PoolFree;
                int Returned = PoolReturned;
                if ( Returned != 0 && (Free+Returned) == PoolSize ) {
                    RefCount++;
                    if ( PoolFree.compare_exchange_strong(Free, 0) ) {
                        if ( RefCount == 1 ) {
                            PoolReturned = 0;
                            PoolFree = PoolSize;
                        }
                    }
                    release();
                }
            } else {
                free(this);
                DEBUG_METRIC_ADD(dbgPoolCount, -1);
            }
        }
    }
    
    void detach() {
        ++RefCount;
        AllocatorRoot *R = Root;
        Root=nullptr;
        R->release();        
        release();
    }
};

struct AllocatorBlock
{
    AllocatorPool  *Pool;
    int             Length;
    
    void release() {
        Pool->PoolReturned += Length;
        Pool->release();
    }
};



class ThreadLocalAllocator
{
private:
    int    PoolSize;
    int    PoolEff;
    AllocatorRoot *Root;   

    
    void *newPool( int pool_max, int returnSize, bool addRoot ) {
        DEBUG_METRIC_ADD(dbgPoolCount, 1);
        AllocatorPool * Pool = reinterpret_cast<AllocatorPool *>(malloc(pool_max+sizeof(AllocatorPool)));
        Pool->PoolSize = pool_max;
        Pool->PoolFree = pool_max - returnSize;
        Pool->PoolReturned = 0;
        Pool->RefCount = 1;
        if ( addRoot && Root->FreePools > 0 ) {
            AllocatorPool **P = &Root->Pools[0];
            for ( int x = 0; x < OCTANE_TRACKED_POOL_COUNT; x++, P++) {
                if ( !(*P) ) {
                    Pool->Root = Root;
                    ++Root->RefCount;
                    --Root->FreePools;
                    *P = Pool;
                    break;
                }
            }
        } else {
            Pool->Root = nullptr;
        }        
        AllocatorBlock *ptr = reinterpret_cast<AllocatorBlock*>(reinterpret_cast<unsigned char*>(Pool) + sizeof(AllocatorPool));
        ptr->Pool = Pool;
        ptr->Length = returnSize;
        ptr++;        
        return reinterpret_cast<void*>(ptr);
    }
    
public:
    ThreadLocalAllocator(int _PoolSize) : PoolSize(_PoolSize), PoolEff(static_cast<size_t>(_PoolSize)-sizeof(AllocatorPool)), Root(nullptr) {
        DEBUG_METRIC_ADD(dbgRootCount, 1);
        DEBUG_METRIC_ADD(dbgAllocatorCount, 1);
        Root = reinterpret_cast<AllocatorRoot*>(malloc(sizeof(AllocatorRoot)));
        Root->RefCount = 1;
        Root->FreePools = OCTANE_TRACKED_POOL_COUNT;
        AllocatorPool **P = &Root->Pools[0];
        for ( int x = 0; x < OCTANE_TRACKED_POOL_COUNT; x++, P++) 
            *P = nullptr;
    }
    
    ~ThreadLocalAllocator() {        
        if ( Root ) {
            AllocatorRoot *R = Root;
            Root = nullptr;
            AllocatorPool **P = &R->Pools[0];
            for ( int x = 0; x < OCTANE_TRACKED_POOL_COUNT; x++, P++) {
                if ( (*P) ) {
                    (*P)->detach();
                    *P = nullptr;
                }
                
            }
            R->release();
#ifdef OCTANE_DEBUG_METRICS
            if ( DEBUG_METRIC_ADD(dbgAllocatorCount, -1) == 0 ) {
                cout << "Pool Count : " << dbgPoolCount << "  Root Count : " << dbgRootCount << endl;
            }
#endif
        }
    }
    
    void *alloc(int n) {
        // Adjust n to be more effective.
        int k = n / sizeof(int);
        if ( k & 1 ) k++;
        else k+=2;
        n = k*sizeof(int) + sizeof(AllocatorBlock);
        
        // Do allocation
        if ( n > PoolEff ) {
            return newPool(n, n, false);
        }
        
        int trimThreshold = OCTANE_RECYLCE_THRESHOLD;
        if ( Root->FreePools == 0 ) {
            trimThreshold = PoolEff / 2;
        }

        AllocatorPool **P = &Root->Pools[0];
        for ( int x = 0; x < OCTANE_TRACKED_POOL_COUNT; x++, P++) {
            if ( *P )
            {
                int Free = (*P)->PoolFree;
                if ( Free >= n) {
                    if ( (*P)->PoolFree.compare_exchange_strong(Free, Free-n) ) {
                        (*P)->RefCount++;
                        AllocatorBlock *ptr = reinterpret_cast<AllocatorBlock*>(reinterpret_cast<unsigned char*>(*P) + sizeof(AllocatorPool) + ((*P)->PoolSize-Free));
                        ptr->Pool = *P;
                        ptr->Length = n;
                        ptr++;
                        return reinterpret_cast<void*>(ptr);
                    }
                } else if (Free < trimThreshold ) {
                    AllocatorPool *Pool = *P;
                    *P = nullptr;
                    Pool->detach();
                }                
            }
        }
        
        if ( Root->FreePools == 0 ) {
            return newPool(n, n, false);
        } else {
            return newPool(PoolEff, n, true);
        }
    }
};

thread_local ThreadLocalAllocator tlsallocator(OCTANE_POOL_SIZE);
void *operator new(size_t n) {
    return tlsallocator.alloc(n);
}

void *operator new[](size_t n) {
    return tlsallocator.alloc(n);
}

void operator delete( void *ptr ) {
    AllocatorBlock *Block = reinterpret_cast<AllocatorBlock*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(AllocatorBlock));
    Block->release();
}

void operator delete[]( void *ptr ) {
    AllocatorBlock *Block = reinterpret_cast<AllocatorBlock*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(AllocatorBlock));
    Block->release();
}
#endif