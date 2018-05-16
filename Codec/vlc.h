/*! @file vlc.h

*  @brief
*
*  @version 1.0.0
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
*
*/

#ifndef _VLC_H
#define _VLC_H

#include "bitstream.h"

#ifndef _COMPANDING
#define _COMPANDING		1
#define _COMPANDING_MORE	(54) // zero is off, 54 is a good value
#endif

#if _COMPANDING_MORE
#define VALUE_TABLE_SIZE	11  //11 is need for CUBIC table that has range +/- 1024
#else
#define VALUE_TABLE_SIZE	8
#endif
#define VALUE_TABLE_LENGTH	(1 << VALUE_TABLE_SIZE)

#define	_OLD_FAST_LOOKUP 1		// Use the old fast lookup table algorithms

/*
	The codebook is organized as a vector of variable length code entries
	indexed by the absolute value of the quantity to be encoded.  The header
	for the codebook contains the number of entries in the codebook.  May
	extend the codebook header later to include escape codes for handling
	the case where the codebook does not contain an entry for that value.
	The current strategy is to saturate the value to the maximum entry in
	the codebook.
*/

typedef struct codebook
{
    int length;			// Number of entries in the codebook
} VLCBOOK;				// Array of codebook entries follows the header

typedef struct vlc
{
    int size;			// Size of code word in bits
    uint32_t  bits;		// Code word bits right justified
} VLC;

typedef struct vle
{
    uint32_t entry;		// Packed codebook entry
} VLE;

#define VLE_CODESIZE_MASK	0x1F
#define VLE_CODESIZE_SHIFT	27
#define VLE_CODEWORD_MASK	0x7FFFFFF
#define VLE_CODEWORD_SHIFT	0

// Should the original unpacked variable length code entry be used?
#define USE_UNPACKED_VLC	0
#define USE_UNPACKED_RLC	0

// Codewords for the sign bits that follow the magnitude of a non-zero value
#define VLC_POSITIVE_CODE 0x0
#define VLC_POSITIVE_SIZE 1
#define VLC_NEGATIVE_CODE 0x1
#define VLC_NEGATIVE_SIZE 1

// Error codes for the return value from codebook lookup
#define VLC_ERROR_OKAY		0
#define VLC_ERROR_NOTFOUND -1

/*
	There is a second set of routines and data structures provided for run
	lengths and values, since it may be useful to code the run (length, value)
	pairs as a single symbol if there are correlations between run length and
	value.

	The table of run length codes could be indexed by the magnitude of the value
	and each entry could be a codebook for encoding the (length, value) pair.

	Currently use two other codebooks for separately coding run lengths and values
	and only zero values are run length coded.
*/

#if USE_UNPACKED_RLC

typedef struct rlc  	// Codebook entries for runs of zeros
{
    int size;			// Size of code word in bits
    uint32_t  bits;		// Code word bits right justified
    int count;			// Run length
} RLC;

#else

typedef struct rlc  	// Codebook entries for runs of zeros
{
    short size;			// Size of code word in bits
    short count;		// Run length
    uint32_t  bits;		// Code word bits right justified
} RLC;

#endif

// Run length code table entry generated by the Huffman program
typedef struct rle  	// Codebook entries for runs of zeros
{
    int size;			// Size of code word in bits
    uint32_t  bits;		// Code word bits right justified
    int count;			// Run length
} RLE;

typedef struct rlcbook
{
    int length;
} RLCBOOK;

typedef struct rlv  	// Codebook entries for arbitrary runs
{
    int size;			// Size of code word in bits
    uint32_t  bits;		// Code word bits right justified
    int count;			// Run length
    int32_t value;			// Run value
} RLV;

typedef struct rlvbook
{
    int length;			// Number of entries in the code book
} RLVBOOK;				// Array of codebook entries follows the header

// Codebook data structure that combines the run length and magnitude
typedef struct rmcbook
{
    RLCBOOK *runbook;	// Codebook for the run length
    VLCBOOK *magbook;	// Codebook for the run magnitude
} RMCBOOK;

// Structure returned by the run length decoding routines
typedef struct run
{
    int count;			// Run length count
    int32_t value;			// Run length value
} RUN;


// The old fast decoding algorithm returned the run length and value
// of the first codeword found in the bitstream.  The new algorithm
// returns the column from the start of scan that includes a nonzero
// value and the count field is used to store the relative column.
// The new algorithm assumes that only zeros are run length encoded.

#if 0
// Entry in the lookup table for fast decoding
typedef struct lookup
{
    int count;			// Run length or column (zero if no entry)
    int32_t value;			// Run value (signed if using new algorithm)
    int shift;			// Number of bits to skip in bitstream
    //int step;			// Number of columns to skip in the decoded row
    //int skip;			// Number of additional columns to skip
} FLC;
#else
// Smaller version so more of the table can fit in the cache
typedef struct lookup
{
    unsigned short count;	// Run length or column (zero if no entry)
    char value;					// Run value (signed if using new algorithm)
    unsigned char shift;		// Number of bits to skip in bitstream
    //unsigned char step;		// Number of columns to skip in the decoded row
    //unsigned char skip;		// Number of additional columns to skip
} FLC;
#endif

// NOTE: The step and skip fields are currently not used but one of them
// will be used to improve fast decoding by recording the number of columns
// of zeros that follow the value decoded from the bitstream.

// Fast decoding lookup table
typedef struct
{
    int size;
    int length;
} FLCBOOK;

// Indexable table for signed values
typedef struct
{
    int size;
    int length;
} VALBOOK;

// Some run length decoding routines require more state information
typedef struct
{
    int32_t value;		// Value of the last run read from the bit stream
    int column;		// Current column position within the row
    int width;		// Number of columns in the current row
} RUNSTATE;

#ifdef _MSC_VER
#pragma warning(disable : 4200)
#endif

// These are the formats of the code tables generated by the Huffman routines
// declared constant since the values should not be changed by the codec
typedef const struct
{
    VLCBOOK header;
    VLC entries[];
} VLCTABLE;

typedef const struct
{
    RLCBOOK header;
    RLE entries[];
} RLCTABLE;

typedef const struct
{
    RLVBOOK header;
    RLV entries[];
} RLVTABLE;


// The finite state machine is represented by an array of table entries organized into
// groups of table entries that are indexed by the bits read from the bitstream.  The
// finite state machine is an array of structs where each struct corresponds to an internal
// node in the Huffman tree and that struct contains an array which is indexed by the
// bits read from the bitstream.

// The number of table entries n in the finite state machine is n = 2^k * m, where k is the number
// of bits read from the bitstream at a time and m is the number of internal nodes in the Huffman tree.

// The next_state field points to an array of next state (table entries) indexed by
// a byte in the next state field of the table entry corresponding to the current state.

#define	_INDIVIDUAL_LUT		1		// Use seperate table for each state
#define INDEX_LENGTH		4		// Number of bits to decode as a chunk

// Names for the index size and mask that are more easily recognized as part of the FSM
#define FSM_INDEX_SIZE		(INDEX_LENGTH)
#define FSM_INDEX_MASK		((1 << INDEX_LENGTH) - 1)
#define FSM_INDEX_ENTRIES	(1 << FSM_INDEX_SIZE)
//#define FSM_NUM_STATES	136
#define FSM_NUM_STATES_MAX	518
/*
typedef struct table {
	short values[2];				// At most two non-zero magnitude values can be decoded from 4 bits
	unsigned short pre_skip;		// Number of zeros before any non-zero magnitude is decoded
	unsigned char post_skip;		// Number of zeros after all non-zero magnitudes are decoded
	unsigned char next_state;		// the next state
} FSMENTRY;
*/

typedef struct table_unpacked
{
    short values[2];				// At most two non-zero magnitude values can be decoded from 4 bits
    unsigned short pre_skip;		// Number of zeros before any non-zero magnitude is decoded
    unsigned short post_skip;		// Number of zeros after all non-zero magnitudes are decoded
    unsigned short next_state;		// the next state
} FSMENTRY_UNPACKED;

typedef struct table_packed
{
    short value0;					// At most two non-zero magnitude values can be decoded from 4 bits
    short value1;					// At most two non-zero magnitude values can be decoded from 4 bits
    unsigned short pre_post_skip;		// Number of zeros before any non-zero magnitude is decoded
    unsigned short next_state;		// the next state
} FSMENTRY;

typedef struct table_packed_fast
{
    int32_t values;
    unsigned short pre_post_skip;		// Number of zeros before any non-zero magnitude is decoded
    unsigned short next_state;		// the next state
} FSMENTRYFAST;


#if _INDIVIDUAL_LUT

// Array of finite state machine entries generated by the Huffman program
typedef struct  			// Data structure for the finite state machine decoder
{
    int num_states;			// Number of states in the finite state machine
    FSMENTRY_UNPACKED entries[];		// Array of finite state machine entries (all of the state tables)
} FSMARRAY;

typedef struct  			// Data structure for the finite state machine decoder
{
    int num_states;			// Number of states in the finite state machine
    FSMENTRY entries[];		// Array of finite state machine entries (all of the state tables)
} FSMARRAY_PACKED;

// Finite state machine table flag bits
#define FSMTABLE_FLAGS_COMPANDING_DONE			0x0001		// Indicates if companding has been applied
#define FSMTABLE_FLAGS_COMPANDING_NOT_NEEDED	0x0002		// Indicates that companding is not used from this band
#define FSMTABLE_FLAGS_COMPANDING_CUBIC			0x0004		// Indicates that companding is cubic x + (x^3/(255^3))*768 (0 to 255 becomes 0 to 1023)
#define FSMTABLE_FLAGS_INITIALIZED				0x8000		// Use the sign bit to indicate initialization

#define _INDIVIDUAL_ENTRY 0
#define _SINGLE_FSM_TABLE 0 //needs _INDIVIDUAL_ENTRY = 1
#define _FSM_NO_POINTERS  1

// Collection of the individual lookup tables (one for each state)
typedef struct  			// Data structure for the finite state machine decoder
{
    short flags;			// Flag bits that indicate the table status
    short num_states;		// Number of states in the finite state machine
    // Array of pointers to lookup tables (one for each state in the finite state machine)

#if _FSM_NO_POINTERS
    FSMENTRY entries[FSM_NUM_STATES_MAX][FSM_INDEX_ENTRIES];
#else
    FSMENTRY *entries[FSM_NUM_STATES_MAX];
#endif
#if _INDIVIDUAL_ENTRY
    FSMENTRY *entries_ind[FSM_NUM_STATES_MAX << FSM_INDEX_SIZE];
    FSMENTRY *firstentry;
#endif
} FSMTABLE;



#if !_INDIVIDUAL_ENTRY
#define FSMTABLE_INITIALIZER	{0, 0, NULL}
#else
#define FSMTABLE_INITIALIZER	{0, 0, NULL, NULL}
#endif

// Runtime finite state machine data structure
typedef struct
{
    FSMENTRY *next_state;	// Pointer to the loopup table for the current state
#if _INDIVIDUAL_ENTRY
    int next_state_index;
#endif
    FSMTABLE table;		// Pointer to the table of finite state machine lookup tables
    int InitizedRestore;
    int LastQuant;

    short restoreFSM[FSM_NUM_STATES_MAX * (1 << FSM_INDEX_SIZE) * 2];
} FSM;

#else

// Use the following new FSM structure which allows the LUT memory to be aligned

typedef struct  			// Data structure for the finite state machine decoder
{
    int num_states;			// Number of states in the finite state machine
    FSMENTRY *next_state;	// Pointer to the lookup table for the next state
    FSMENTRY *entries;		// Lookup tables (one at each state) for the FSM
} FSM;

typedef struct  			// Data structure for the finite state machine decoder
{
    int num_states;			// Number of states in the finite state machine
    FSMENTRY *next_state;	// Pointer to the lookup table for the next state
    FSMENTRY entries[];		// Array of n finite state machine entries (all of the lookup tables)
} FSMARRAY;

#endif

#if 0
// The following code shows how to include the codebooks in the source code.
// Define the _CODEBOOKS symbol to be zero to prevent duplicate inclusion

#ifndef _CODEBOOKS
#define _CODEBOOKS 1
#endif

#if _CODEBOOKS

// Insert the codebooks here and in the verification routine below
#include "table3x.inc"
#include "table3z.inc"

// Dual codebook for runlength and magnitude coding
RMCBOOK rmctable3z = {(RLCBOOK *) &table3z, (VLCBOOK *) &table3x};

// Default codebook for encoding highpass coefficient magnitudes
CODEBOOK *highbook = (CODEBOOK *) &table3x;

// Default codebook for encoding runs of highpass coefficients
RMCBOOK *runsbook = &rmctable3z;

#else

// The codebooks have been included elsewhere
extern CODEBOOK *highbook;
extern RMCBOOK *runsbook;

#endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

bool IsValidCodebook(VLCBOOK *codebook);

int32_t PutVlc(BITSTREAM *stream, int32_t value, VLCBOOK *codebook);
int32_t PutVlcSigned(BITSTREAM *stream, int32_t value, VLCBOOK *codebook);
int32_t GetVlc(BITSTREAM *stream, VLCBOOK *codebook);
int32_t GetVlcSigned(BITSTREAM *stream, VLCBOOK *codebook);

int32_t PutRun(BITSTREAM *stream, int count, RLCBOOK *codebook, int *remainder);
void PutZeroRun(BITSTREAM *stream, int count, RLCBOOK *codebook);
void PutFastRun(BITSTREAM *stream, int count, RLCBOOK *codebook);
int32_t PutRlc(BITSTREAM *stream, int count, int32_t value, RMCBOOK *codebook);
int32_t PutRlcSigned(BITSTREAM *stream, int count, int32_t value, RMCBOOK *codebook);
int GetRlc(BITSTREAM *stream, RUN *run, RLVBOOK *codebook);
int GetRlcSigned(BITSTREAM *stream, RUN *run, RLVBOOK *codebook);

// Fast codebook search using a lookup table
int GetRlcIndexed(BITSTREAM *stream, RUN *run, RLVBOOK *codebook, int index);
int LookupRlc(BITSTREAM *stream, RUN *run, FLCBOOK *fastbook, RLVBOOK *codebook);
int LookupRlcSigned(BITSTREAM *stream, RUN *run, FLCBOOK *fastbook, RLVBOOK *codebook);

// Skip runs of zeros and return the first non-zero value in the current row
// See comments on how the run length argument is interpreted by this routine
int ScanRlcValue(BITSTREAM *stream, RUNSTATE *state, FLCBOOK *fastbook, RLVBOOK *codebook);

// Output the code for a signed eight bit coefficient
void PutVlcByte(BITSTREAM *stream, int value, VALBOOK *codebook);

#if (0 && _DEBUG)

// Return true if the value would be saturated by the codebook
bool IsVlcByteSaturated(VALBOOK *codebook, int value);

#endif

#ifdef __cplusplus
}
#endif

#endif
