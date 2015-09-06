/*
    octane.hpp
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

#pragma once

#include <memory>

#ifndef OCTANE_DISABLE

#define OCTANE_ALIGNMENT 16

#if !defined(OCTANE_POOL_SIZE) || (OCTANE_POOL_SIZE < 4096)
#define OCTANE_POOL_SIZE 4092*OCTANE_ALIGNMENT
#endif

#if !defined(OCTANE_TRACKED_POOL_COUNT) || (OCTANE_TRACKED_POOL_COUNT < 64)
#define OCTANE_TRACKED_POOL_COUNT 256
#endif

#if !defined(OCTANE_RECYLCE_THRESHOLD) || (OCTANE_RECYLCE_THRESHOLD < 128)
#define OCTANE_RECYLCE_THRESHOLD 128
#endif

#define OCTANE_ALLOC( size ) aligned_alloc(OCTANE_ALIGNMENT, size)
#define OCTANE_FREE( ptr ) free(ptr)

//#define OCTANE_DEBUG_METRICS 1
#if OCTANE_DEBUG_METRICS
#define DEBUG_METRIC_ADD(metric, val) (metric += val)
#define DEBUG_METRIC(metric) atomic_int metric
#else
#define DEBUG_METRIC_ADD(metric, val) 
#define DEBUG_METRIC(metric) 
#endif

void *octane_alloc( size_t size, size_t align = 1 );
void *octane_realloc( void *ptr, size_t newsize, size_t align = 1 );
void octane_free( void *ptr );

template<typename T, typename ...Targs> T *new_aligned(Targs ...Args) {
    void *ptr = octane_alloc(sizeof(T), alignof(T));
    return new (ptr) T(Args...);
}

#endif
