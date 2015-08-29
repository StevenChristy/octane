#include <atomic>
#include <memory>
#include <x86intrin.h>

using namespace std;

//#define OCTANE_DISABLE 1
#ifndef OCTANE_DISABLE

#if !defined(OCTANE_POOL_SIZE) || (OCTANE_POOL_SIZE < 4096)
#define OCTANE_POOL_SIZE 65536
#endif

#if !defined(OCTANE_KEEP_POOLS) || (OCTANE_KEEP_POOLS < 1) 
#define OCTANE_KEEP_POOLS 4
#endif

//#define OCTANE_DEBUG_METRICS 1
#if OCTANE_DEBUG_METRICS
#include <iostream>
#define DEBUG_METRIC_ADD(metric, val) (metric += val)
#define DEBUG_METRIC(metric) atomic_int metric
#else
#define DEBUG_METRIC_ADD(metric, val) 
#define DEBUG_METRIC(metric) 
#endif

#define SPIN_LOCK( lockName )  { int xx_lck = 0; while ( !(xx_lck = (lockName).compare_exchange_strong(xx_lck, 1)) ) { __pause(); } }

DEBUG_METRIC(dbgAllocatorCount);
DEBUG_METRIC(dbgPoolCount);
DEBUG_METRIC(dbgRootCount);

struct AllocatorPool;
struct AllocatorHead;
typedef void (*free_handler)(AllocatorPool *);
struct AllocatorRoot
{
    AllocatorPool          *Pools;
    free_handler            FreeFunct;
    atomic<int>             FreePools;
    atomic<int>             GCCounter;
    atomic<int>             GCLock;
};

struct AllocatorPool
{
    AllocatorRoot          *Root;
    AllocatorPool          *Next;
    int                     PoolSize;
    atomic_int              PoolFree;
    atomic_int              PoolReturned;
    atomic<AllocatorHead *> FreeHead;
    int                     Reserved;
};

struct AllocatorHead
{
    union {
        AllocatorPool      *Pool;
        AllocatorHead      *Next;
    };
    int                     Length;
};

class ThreadLocalAllocator
{
private:
    int    PoolSize;
    int    PoolEff;
    AllocatorRoot *Root;   

    static bool GC( AllocatorRoot *Root, int MaxFree = 0 ) {
        int skipFree = MaxFree;
             
        Root->GCCounter++;
        
        if ( MaxFree == -1 ) {
            Root->FreeFunct = &ThreadLocalAllocator::Dead;
        }

        SPIN_LOCK(Root->GCLock);
        
        AllocatorPool * *Prev = &Root->Pools;
        AllocatorPool * *NextPrev = nullptr;
        AllocatorPool *Next = nullptr;
        
        for ( AllocatorPool *P = Root->Pools; P; P = Next, Prev = NextPrev ) {
            NextPrev = &P->Next;
            Next = P->Next;
            int PoolFree = P->PoolFree;
            if ( PoolFree != P->PoolSize )
                continue;

            if ( skipFree > 0 ) {
                skipFree--;
                continue;
            }
            
            if ( !P->PoolFree.compare_exchange_strong(PoolFree, 0)  ) {
                continue;
            }
            
            P->Next = nullptr;
            *Prev = Next;
            Root->FreePools--;
            NextPrev = Prev;
            free(P);                       
            DEBUG_METRIC_ADD(dbgPoolCount, -1);
        }
        
        Root->GCLock = 0;
        
        if ( ((--Root->GCCounter) == 0) && Root->Pools == nullptr) {
            free(Root);
            DEBUG_METRIC_ADD(dbgRootCount, -1);
            return true;
        }
        
        return false;
    }
    
    static void Alive( AllocatorPool *Pool ) {
        int PoolFree = Pool->PoolFree;
        int PoolReturned = Pool->PoolReturned;
        if ( (PoolReturned + PoolFree) == Pool->PoolSize )
        {
            if ( Pool->PoolFree.compare_exchange_strong(PoolFree, 0)  )
            {
                AllocatorHead *Head;
                do 
                {
                    Head = Pool->FreeHead;
                }
                while ( !Pool->FreeHead.compare_exchange_weak(Head, nullptr) );                
                Pool->PoolFree += Pool->PoolReturned.fetch_sub(PoolReturned) + PoolFree;
                int numFreePools = (Pool->Root->FreePools += 1);
                if ( numFreePools > (OCTANE_KEEP_POOLS+2) )
                {
                    GC(Pool->Root, OCTANE_KEEP_POOLS);
                }
            }
        }
    }
    
    static void Dead( AllocatorPool *Pool ) {
        int PoolFree = Pool->PoolFree;
        int PoolReturned = Pool->PoolReturned;
        if ( (PoolReturned + PoolFree) == Pool->PoolSize )
        {
            if ( Pool->PoolFree.compare_exchange_strong(PoolFree, 0)  )
            {
                AllocatorHead *Head;
                do 
                {
                    Head = Pool->FreeHead;
                }
                while ( !Pool->FreeHead.compare_exchange_weak(Head, nullptr) );
                Pool->PoolFree += Pool->PoolReturned.fetch_sub(PoolReturned) + PoolFree;
                Pool->Root->FreePools += 1;
                GC(Pool->Root);
            }
        }
    }
    
    
    void *newPool( int pool_max, int returnSize ) {
        DEBUG_METRIC_ADD(dbgPoolCount, 1);
        AllocatorPool * Pool = reinterpret_cast<AllocatorPool *>(malloc(pool_max+sizeof(AllocatorPool)));
        Pool->Root = Root;
        Pool->PoolSize = pool_max;
        Pool->PoolFree = pool_max;
        Pool->PoolReturned = 0;
        Pool->Next = nullptr;
        Pool->FreeHead = nullptr;
       
        int freeN = Pool->PoolFree.fetch_sub((int)returnSize);
        AllocatorHead *ptr = reinterpret_cast<AllocatorHead*>(reinterpret_cast<unsigned char*>(Pool) + sizeof(AllocatorPool) + (Pool->PoolSize-freeN));
        ptr->Pool = Pool;
        ptr->Length = returnSize;
        ptr++;

        SPIN_LOCK(Root->GCLock);
        Pool->Next = Root->Pools;
        Root->Pools = Pool;        
        Root->GCLock = 0;
        
        return reinterpret_cast<void*>(ptr);
    }
    
public:
    ThreadLocalAllocator(int _PoolSize) : PoolSize(_PoolSize), PoolEff(static_cast<size_t>(_PoolSize)-sizeof(AllocatorPool)), Root(nullptr) {
        DEBUG_METRIC_ADD(dbgRootCount, 1);
        DEBUG_METRIC_ADD(dbgAllocatorCount, 1);
        Root = reinterpret_cast<AllocatorRoot*>(malloc(sizeof(AllocatorRoot)));
        Root->Pools = nullptr;
        Root->FreeFunct = &ThreadLocalAllocator::Alive;
        Root->FreePools = 0;
        Root->GCCounter = 0;
        Root->GCLock = 0;
    }
    
    ~ThreadLocalAllocator() {        
        if ( Root )
        {
            AllocatorRoot *R = Root;
            Root = nullptr;
            GC(R,-1);
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
        n = k*sizeof(int) + sizeof(AllocatorHead);
        
        // Do allocation
        if ( n > PoolEff ) {
            return newPool(n, n);
        }

        SPIN_LOCK(Root->GCLock);

        for ( AllocatorPool *P = Root->Pools; P; P = P->Next ) {
            if ( P->PoolFree >= n) {
                if ( P->PoolFree == P->PoolSize ) {
                    P->Root->FreePools -= 1;
                }
                int freeN = P->PoolFree.fetch_sub((int)n);
                AllocatorHead *ptr = reinterpret_cast<AllocatorHead*>(reinterpret_cast<unsigned char*>(P) + sizeof(AllocatorPool) + (P->PoolSize-freeN));
                ptr->Pool = P;
                ptr->Length = n;
                ptr++;
                Root->GCLock = 0;
                return reinterpret_cast<void*>(ptr);
            }
        }
        
        Root->GCLock = 0;
        
        return newPool(PoolEff, n);
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
    AllocatorHead *Head = reinterpret_cast<AllocatorHead*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(AllocatorHead));
    AllocatorPool *Pool = Head->Pool;
    AllocatorRoot *Root = Pool->Root;
    int Length = Head->Length;
    do 
    {
        Head->Next = Pool->FreeHead;
    }
    while ( !Pool->FreeHead.compare_exchange_weak(Head->Next, Head) );
    Pool->PoolReturned += Length;
    Root->FreeFunct(Pool);
}

void operator delete[]( void *ptr ) {
    AllocatorHead *Head = reinterpret_cast<AllocatorHead*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(AllocatorHead));
    AllocatorPool *Pool = Head->Pool;
    AllocatorRoot *Root = Pool->Root;
    int Length = Head->Length;
    do 
    {
        Head->Next = Pool->FreeHead;
    }
    while ( !Pool->FreeHead.compare_exchange_weak(Head->Next, Head) );
    Pool->PoolReturned += Length;
    Root->FreeFunct(Pool);
}
#endif
