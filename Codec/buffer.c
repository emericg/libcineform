/*!
 * @file buffer.c
 *
 *  (C) Copyright 2017 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stddef.h>
#include <assert.h>

#include "buffer.h"
#include "image.h"
#include "config.h"

#include <emmintrin.h>
#include <memory.h>

// Initialize a scratch buffer
void InitScratchBuffer(SCRATCH *scratch, char *base, size_t size)
{
    scratch->base_ptr = base;
    scratch->free_ptr = base;
    scratch->free_size = size;
    scratch->next_ptr = NULL;
}

// Initialize a local section within the scratch buffer
void PushScratchBuffer(SCRATCH *section, const SCRATCH *scratch)
{
    section->base_ptr = scratch->free_ptr;
    section->free_ptr = scratch->free_ptr;
    section->free_size = scratch->free_size;
    section->next_ptr = NULL;
}

// Utility routine for subdividing scratch space into buffers
char *AllocScratchBuffer(SCRATCH *scratch, size_t request)
{
    char *buffer = NULL;	// Allocated portion of scratch space

    // Check that scratch space has been allocated
    assert(scratch->base_ptr != NULL);

    // Is there enough scratch space for the new allocation?
    if (request <= scratch->free_size)
    {
        buffer = scratch->free_ptr;
        scratch->free_ptr += request;
        scratch->free_size -= request;
    }

    // Check that the requested amount of scratch space was allocated
    assert(buffer != NULL);

    // Return the allocated portion of scratch space
    return buffer;
}

// Aligned allocation of a scratch buffer
char *AllocAlignedBuffer(SCRATCH *scratch, size_t request, int alignment)
{
    char *buffer;

    // Compute the prefix required for the specified alignment
    int prefix = alignment - ((uintptr_t)scratch->free_ptr % alignment);

    // Allocate a block large enough for the requested allocation with alignment
    request += prefix;
    buffer = AllocScratchBuffer(scratch, request);

    // Check that the requested amount of scratch space was allocated
    assert(buffer != NULL);

    if (buffer != NULL)
    {
        // Force the required alignment
        buffer += prefix;

        // Check that the pointer into the buffer is properly aligned
        assert(ISALIGNED(buffer, alignment));
    }

    // Return the aligned pointer into the buffer
    return buffer;
}
