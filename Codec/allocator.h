/*!
 * @file allocator.h
 * @brief Memory tools
 *
 * (C) Copyright 2017 GoPro Inc (http://gopro.com/).
 *
 * Licensed under either:
 * - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 * - MIT license, http://opensource.org/licenses/MIT
 * at your option.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include "config.h"
#include <emmintrin.h> // Include support for SSE2 intrinsics

// The codec SDK and the codec library use the same memory allocator
#include "../Common/CFHDAllocator.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void *Alloc(ALLOCATOR *allocator, size_t size)
{
#if _ALLOCATOR
    //assert(allocator != NULL);
    if (allocator != NULL)
    {
        return allocator->vtable->unaligned_malloc(allocator, size);
    }
#else
    // The allocator argument is not used
    (void) allocator;
#endif

    return MEMORY_ALLOC(size);
}

static inline void Free(ALLOCATOR *allocator, void *block)
{
#if _ALLOCATOR
    //assert(allocator != NULL);
    if (allocator != NULL)
    {
        allocator->vtable->unaligned_free(allocator, block);
        return;
    }
#else
    // The allocator argument is not used
    (void) allocator;
#endif

    MEMORY_FREE(block);
}


#if (0 && _DEBUG)

typedef struct
{
    void *block;
    size_t size;

} AllocatedBlock;

// Table for recording allocated memory blocks for debugging
static AllocatedBlock m_allocationTable[100];

static int m_allocationCount = 0;
static int m_allocationSize = 0;

#endif


static inline void *AllocAligned(ALLOCATOR *allocator, size_t size, size_t alignment)
{
    void *block = NULL;

#if _ALLOCATOR
    //assert(allocator != NULL);
    if (allocator != NULL)
    {
        return allocator->vtable->aligned_malloc(allocator, size, alignment);
    }
#else
    // The allocator argument is not used
    (void) allocator;
#endif

    block = MEMORY_ALIGNED_ALLOC((int)size, (int)alignment);

#if (0 && _DEBUG)
    if (m_allocationCount < sizeof(m_allocationTable) / sizeof(m_allocationTable[0]))
    {
        m_allocationTable[m_allocationCount].block = block;
        m_allocationTable[m_allocationCount].size = size;
        m_allocationCount++;
    }

    m_allocationSize += size;
#endif

    return block;
}

static inline void FreeAligned(ALLOCATOR *allocator, void *block)
{
#if _ALLOCATOR
    //assert(allocator != NULL);
    if (allocator != NULL)
    {
        allocator->vtable->aligned_free(allocator, block);
        return;
    }
#else
    // The allocator argument is not used
    (void) allocator;
#endif

    MEMORY_ALIGNED_FREE(block);

#if (0 && _DEBUG)
    {
        bool found = false;
        int i, j;

        for (i = 0; i < m_allocationCount; i++)
        {
            // The index should not exceed the allocated size of the table
            assert(i < sizeof(m_allocationTable) / sizeof(m_allocationTable[0]));

            // Found the block in the table?
            if (m_allocationTable[i].block == block)
            {
                //m_allocationTable[i].block = NULL;
                //m_allocationTable[i].size = 0;

                m_allocationSize -= m_allocationTable[i].size;

                // Shift the remaining entries in the table
                for (j = i + 1; j < m_allocationCount; j++)
                {
                    // The index should not exceed the allocated size of the table
                    assert(j < sizeof(m_allocationTable) / sizeof(m_allocationTable[0]));

                    m_allocationTable[i].block = m_allocationTable[j].block;
                    m_allocationTable[i].size = m_allocationTable[j].size;

                    // Advance the index to the next table entry to fill
                    i = j;
                }

                m_allocationTable[i].block = NULL;
                m_allocationTable[i].size = 0;

                // Reduce the number of entries in the table
                m_allocationCount--;

                // Done searching for the allocation table entry
                found = true;
                break;
            }
        }

        assert(found);
    }
#endif
}

#ifdef __cplusplus
}
#endif
