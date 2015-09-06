/*
    octane.cpp
    Copyright (C) 2015  Steven Christy

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "octane.hpp"
#include <atomic>
#include <cstring>

using namespace std;

#ifdef OCTANE_DEBUG_METRICS
#include <iostream>
#endif


namespace octane {

#ifndef OCTANE_DISABLE    

DEBUG_METRIC(dbgAllocatorCount);
DEBUG_METRIC(dbgPoolCount);
DEBUG_METRIC(dbgRootCount);

struct AllocatorPool;
struct AllocatorBlock;
struct AllocatorRoot
{
    atomic<int>     RefCount;
    AllocatorPool  *Pools[OCTANE_TRACKED_POOL_COUNT];
    int             FreePools;
    
    void release() {
        if ( --RefCount == 0 ) {
            OCTANE_FREE(this);
            DEBUG_METRIC_ADD(dbgRootCount, -1);
        }
    }
};

struct alignas(OCTANE_ALIGNMENT) AllocatorPool
{
    AllocatorRoot  *Root;
    atomic<int>     RefCount;
    atomic<int>     PoolFree;
    atomic<int>     PoolReturned;
    atomic<int>     LastBlock;
    int             PoolSize;
    
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
                OCTANE_FREE(this);
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

struct alignas(OCTANE_ALIGNMENT) AllocatorBlock
{
    int             Offset;
    int             Length;
    int             LastBlock;
    int             Freed;
    
    void release() {
        Freed = 1;
        AllocatorPool *Pool = reinterpret_cast<AllocatorPool*>(reinterpret_cast<unsigned char*>(this) + Offset);
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

    
    void *newPool( int pool_max, int returnSize, bool addRoot, int a ) {
        DEBUG_METRIC_ADD(dbgPoolCount, 1);
        AllocatorPool *Pool = reinterpret_cast<AllocatorPool *>(malloc(pool_max+sizeof(AllocatorPool)));
        Pool->PoolSize = pool_max;
        Pool->PoolFree = pool_max - returnSize;
        Pool->PoolReturned = 0;
        Pool->RefCount = 1;
        Pool->LastBlock = sizeof(AllocatorPool);
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
        
        unsigned char *start = reinterpret_cast<unsigned char*>(reinterpret_cast<unsigned char*>(Pool) + sizeof(AllocatorPool));
        if ( a ) {
            unsigned wasted = 0;
            while ( (intptr_t)(start + sizeof(AllocatorBlock)) % a ) {
                start += OCTANE_ALIGNMENT;
                wasted += OCTANE_ALIGNMENT;
            }
            Pool->PoolReturned += wasted;
        }                        
        
        AllocatorBlock *ptr = reinterpret_cast<AllocatorBlock*>(start);
        ptr->Offset = (reinterpret_cast<unsigned char *>(Pool) - reinterpret_cast<unsigned char *>(ptr));
        ptr->Length = returnSize;
        ptr->LastBlock = 0;
        ptr->Freed = 0;
        ptr++;        
        return reinterpret_cast<void*>(ptr);
    }
    
public:
    ThreadLocalAllocator(int _PoolSize) : PoolSize(_PoolSize), PoolEff(static_cast<size_t>(_PoolSize)-sizeof(AllocatorPool)), Root(nullptr) {
        DEBUG_METRIC_ADD(dbgRootCount, 1);
        DEBUG_METRIC_ADD(dbgAllocatorCount, 1);
        Root = reinterpret_cast<AllocatorRoot*>(OCTANE_ALLOC(sizeof(AllocatorRoot)));
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
    
    void *alloc(int n, int a) {
        n = (n % OCTANE_ALIGNMENT) ? (1+(n/OCTANE_ALIGNMENT))*OCTANE_ALIGNMENT : (n/OCTANE_ALIGNMENT)*OCTANE_ALIGNMENT;
        n += sizeof(AllocatorBlock);
        int na = n;
        
        if ( a > OCTANE_ALIGNMENT ) {
            // a must be multiples of OCTANE_ALIGNMENT
            a = (a % OCTANE_ALIGNMENT) ? (1+(a/OCTANE_ALIGNMENT))*OCTANE_ALIGNMENT : (a/OCTANE_ALIGNMENT)*OCTANE_ALIGNMENT;
            na += a;
        } else {
            a = 0; // ignored.
        }
        
        
        if ( na > PoolEff ) {
            return newPool(na, n, false, a);
        }
        
        int trimThreshold = OCTANE_RECYLCE_THRESHOLD;
        if ( Root->FreePools == 0 ) {
            trimThreshold = PoolEff / 2;
        }

        AllocatorPool **P = &Root->Pools[0];
        for ( int x = 0; x < OCTANE_TRACKED_POOL_COUNT; x++, P++) {
            if ( *P ) {
                int Free = (*P)->PoolFree;
                if ( Free >= na ) {
                    if ( (*P)->PoolFree.compare_exchange_strong(Free, Free-n) ) {
                        (*P)->RefCount++;
                        
                        unsigned char *start = reinterpret_cast<unsigned char*>(*P) + sizeof(AllocatorPool) + ((*P)->PoolSize-Free);
                        if ( a ) {
                            unsigned wasted = 0;
                            while ( (intptr_t)(start + sizeof(AllocatorBlock)) % a ) {
                                start += OCTANE_ALIGNMENT;
                                wasted += OCTANE_ALIGNMENT;
                            }
                            (*P)->PoolReturned += wasted;
                        }                        
                        
                        AllocatorBlock *ptr = reinterpret_cast<AllocatorBlock*>(start);
                        ptr->Offset = (reinterpret_cast<unsigned char *>(*P) - reinterpret_cast<unsigned char *>(ptr));
                        ptr->Length = n;
                        ptr->Freed = 0;
                        ptr->LastBlock = (*P)->LastBlock;
                        (*P)->LastBlock = 0 - ptr->Offset;
                        ptr++;
                        return reinterpret_cast<void*>(ptr);
                    }
                } else if ( Free < trimThreshold ) {
                    AllocatorPool *Pool = *P;
                    *P = nullptr;
                    Pool->detach();
                }                
            }
        }
        
        if ( Root->FreePools == 0 ) {
            return newPool(na, n, false, a);
        } else {
            return newPool(PoolEff, n, true, a);
        }
    }
};


#endif

} // namespace octane

thread_local octane::ThreadLocalAllocator tlsallocator(OCTANE_POOL_SIZE);

void *operator new(size_t n) {
    return tlsallocator.alloc(n, 1);
}

void *operator new[](size_t n) {
    return tlsallocator.alloc(n, 1);
}

void operator delete( void *ptr ) {
    octane::AllocatorBlock *Block = reinterpret_cast<octane::AllocatorBlock*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(octane::AllocatorBlock));
    Block->release();
}

void operator delete[]( void *ptr ) {
    octane::AllocatorBlock *Block = reinterpret_cast<octane::AllocatorBlock*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(octane::AllocatorBlock));
    Block->release();
}

void *octane_alloc( size_t size, size_t align ) {
    return tlsallocator.alloc(size, align);
}

void octane_free( void *ptr ) {
    octane::AllocatorBlock *Block = reinterpret_cast<octane::AllocatorBlock*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(octane::AllocatorBlock));
    Block->release();
}

void *octane_realloc(void *ptr, size_t newsize, size_t align ) {
    int n = static_cast<int>(newsize);
    octane::AllocatorBlock *Block = reinterpret_cast<octane::AllocatorBlock*>(reinterpret_cast<unsigned char *>(ptr) - sizeof(octane::AllocatorBlock));
    void *mem = tlsallocator.alloc(n, static_cast<int>(align));
    if ( mem ) {
        memcpy(mem, ptr, n < Block->Length ? n : Block->Length);
        free(ptr);
    }
    return mem;
}

   