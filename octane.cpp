#include <atomic>
#include <memory>

using namespace std;

#if !defined(OCTANE_POOL_SIZE) || (OCTANE_POOL_SIZE < 4096)
#define OCTANE_POOL_SIZE 32768
#endif

#if !defined(OCTANE_KEEP_POOLS) || (OCTANE_KEEP_POOLS < 1) 
#define OCTANE_KEEP_POOLS 2
#endif

#if OCTANE_DEBUG_METRICS
#include <iostream>
#define DEBUG_METRIC_ADD(metric, val) (metric += val)
#define DEBUG_METRIC(metric) atomic_int metric
#else
#define DEBUG_METRIC_ADD(metric, val) 
#define DEBUG_METRIC(metric) 
#endif

//#define OCTANE_DISABLE 1
#ifndef OCTANE_DISABLE
DEBUG_METRIC(dbgAllocatorCount);
DEBUG_METRIC(dbgPoolCount);
DEBUG_METRIC(dbgRootCount);

struct AllocatorPool;
struct AllocatorHead;
typedef void (*free_handler)(AllocatorPool *);
struct AllocatorRoot
{
    atomic<AllocatorPool *> Pools;
    atomic<int>             FreePools;
    free_handler            FreePool;
    int                     Reserved;
};

struct AllocatorPool
{
    AllocatorRoot          *Root;
    int                     PoolSize;
    atomic_int              PoolFree;
    atomic_int              PoolReturned;
    atomic<AllocatorHead *> FreeHead;
    atomic<AllocatorPool *> Next;
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
        atomic<AllocatorPool *> *Prev = &Root->Pools;
        atomic<AllocatorPool *> *NextPrev = nullptr;
        AllocatorPool *Next = nullptr;
        
        if ( MaxFree == -1 ) {
            Root->FreePool = &ThreadLocalAllocator::Dead;
            
            if ( Root->Pools.compare_exchange_strong(Next, Next) ) {
                free(Root);
                DEBUG_METRIC_ADD(dbgRootCount, -1);
                return true;
            }
        }
        
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
                break;
            }
            
            if ( !Prev->compare_exchange_strong(P, Next) ) {
                P->PoolFree+=PoolFree;
                break;
            }
            
            Root->FreePools -= 1;
            NextPrev = Prev;
            free(P);            
            
            DEBUG_METRIC_ADD(dbgPoolCount, -1);
            if ( MaxFree <= 0 && Next == nullptr) {
                if ( Root->Pools.compare_exchange_strong(Next, Next) )
                {
                    free(Root);
                    DEBUG_METRIC_ADD(dbgRootCount, -1);
                    return true;
                }
            }
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
                if ( numFreePools > OCTANE_KEEP_POOLS )
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

        AllocatorPool *RootPool;
        do 
        {
            RootPool = Root->Pools;
            Pool->Next.store(RootPool);
        }
        while ( !Root->Pools.compare_exchange_weak(RootPool, Pool) );
        
        return reinterpret_cast<void*>(ptr);
    }
    
public:
    ThreadLocalAllocator(int _PoolSize) : PoolSize(_PoolSize), PoolEff(static_cast<size_t>(_PoolSize)-sizeof(AllocatorPool)), Root(nullptr) {
        DEBUG_METRIC_ADD(dbgRootCount, 1);
        DEBUG_METRIC_ADD(dbgAllocatorCount, 1);
        Root = reinterpret_cast<AllocatorRoot*>(malloc(sizeof(AllocatorRoot)));
        Root->Pools = nullptr;
        Root->FreePool = &ThreadLocalAllocator::Alive;
        Root->FreePools = 0;
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
                return reinterpret_cast<void*>(ptr);
            }
        }
        
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
    int Length = Head->Length;
    do 
    {
        Head->Next = Pool->FreeHead;
    }
    while ( !Pool->FreeHead.compare_exchange_weak(Head->Next, Head) );
    Pool->PoolReturned += Length;
    Pool->Root->FreePool(Pool);
}

void operator delete[]( void *ptr ) {
    AllocatorHead *Head = reinterpret_cast<AllocatorHead*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(AllocatorHead));
    AllocatorPool *Pool = Head->Pool;
    int Length = Head->Length;
    do 
    {
        Head->Next = Pool->FreeHead;
    }
    while ( !Pool->FreeHead.compare_exchange_weak(Head->Next, Head) );
    Pool->PoolReturned += Length;
    Pool->Root->FreePool(Pool);
}
#endif
