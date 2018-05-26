/*! @file CFHDEncoder.cpp

*  @brief This module implements the C functions for the original encoder API.
*
*  The original encoder API was not threaded.  For applications that perform encoding
*  using multiple threads, the asynchronous encoder API is recommended.  The original
*  encoder API used functions that take an encoder reference as the first argument.
*  The routines in the new asynchronous encoder API use an encoder pool reference.
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


#include "StdAfx.h"
#include "Interface.h"

// Include files from the codec library
#include "encoder.h"
#include "thread.h"
#include "metadata.h"

//TODO: Eliminate references to the codec library

// Include files from the encoder DLL
#include "Allocator.h"
#include "CFHDEncoder.h"

#include "VideoBuffers.h"
#include "SampleEncoder.h"

/*!	@function CFHD_OpenEncoder

	@brief Open an instance of the CineForm HD encoder and return a reference
	to the encoder through the pointer provided as the first argument.

	@param encoderRefOut
	Pointer to the variable that will receive the encoder reference.

	@param allocator
	CFHD_ALLOCATOR structure, for those was wishing to control memory allocations. Pass NULL if not used.

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_OpenEncoder(CFHD_EncoderRef *encoderRefOut,
                 CFHD_ALLOCATOR *allocator)
{
    // Check the input arguments
    if (encoderRefOut == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    // Allocate a new encoder data structure
    CSampleEncoder *encoderRef = new CSampleEncoder;
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_OUTOFMEMORY;
    }

    encoderRef->SetAllocator(allocator);

    // Return the encoder data structure
    *encoderRefOut = (CFHD_EncoderRef)encoderRef;

    return CFHD_ERROR_OKAY;
}

/*!	@function CFHD_GetInputFormats

	@brief Return a list of pixel formats that can be used for
	the input frames passed to the encoder.

	@param encoderRef
	Reference to an encoder created by a call to @ref CFHD_OpenEncoder.

	@param inputFormatArray
	CFHD pixel format array that will receive the list of pixel formats.

	@param inputFormatArrayLength
	Maximum number of pixel formats in the input format array.

	@param actualInputFormatCountOut
	Return count of the actual number of pixel formats copied into the
	input format array.

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_GetInputFormats(CFHD_EncoderRef encoderRef,
                     CFHD_PixelFormat *inputFormatArray,
                     int inputFormatArrayLength,
                     int *actualInputFormatCountOut)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleEncoder *encoder = (CSampleEncoder *)encoderRef;

    errorCode = encoder->GetInputFormats(inputFormatArray,
                                         inputFormatArrayLength,
                                         actualInputFormatCountOut);

    return errorCode;
}

/*!	@function CFHD_PrepareToEncode

	@brief Initialize an encoder instance for encoding.

	@param encoderRef
	Reference to an encoder created by a call to @ref CFHD_OpenEncoder.

	@param inputWidth
	Width of each input frame in pixels.

	@param inputHeight
	Number of lines in each input frame.

	@param inputFormat
	Format of the pixels in the input frames.

	@param encodedFormat
	Encoding format used internally by the codec.
	Video can be encoded as three channels of RGB with 4:4:4 sampling,
	three channels of YUV with 4:2:2 sampling, or other formats.
	See the formats listed in CFHD_EncodedFormat.

	@param encodingFlags
	Flags that provide further information about the video format.
	See the flags defined in CFHD_EncodingFlags.

	@param encodingQuality
	Quality to use for encoding.  Corresponds to the setting in the export
	dialog boxes.	0=Fixed, 1=Low, 2=Medium, 3=High, 4=FilmScan1, 5=FilmScan2

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_PrepareToEncode(CFHD_EncoderRef encoderRef,
                     int inputWidth,
                     int inputHeight,
                     CFHD_PixelFormat inputFormat,
                     CFHD_EncodedFormat encodedFormat,
                     CFHD_EncodingFlags encodingFlags,
                     CFHD_EncodingQuality encodingQuality)
{
    CFHD_Error error = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleEncoder *encoder = (CSampleEncoder *)encoderRef;

    error = encoder->PrepareToEncode(inputWidth,
                                     inputHeight,
                                     inputFormat,
                                     encodedFormat,
                                     encodingFlags,
                                     &encodingQuality);

    return error;
}

/*!	@function CFHD_EncodeSample

	@brief Encode one frame of video.

	@param encoderRef
	Reference to an encoder created by a call to @ref CFHD_OpenEncoder.
	The encoder must have been initialized by a call to @ref CFHD_PrepareToEncode
	before attempting to encode frames.

	@param frameBuffer
	Pointer to the frame to encode.
	The width and height of the frame and the pixel format must be the
	same as declared in the call to @ref CFHD_PrepareToEncode.

	@param framePitch
	Number of bytes between rows in the frame.

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_EncodeSample(CFHD_EncoderRef encoderRef,
                  void *frameBuffer,
                  int framePitch)
{
    CFHD_Error error = CFHD_ERROR_OKAY;
    CFHD_Error errorFree = CFHD_ERROR_OKAY;		// 20090610 CMD - so we can remember the encode error.

    // Check the input arguments
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleEncoder *encoder = (CSampleEncoder *)encoderRef;
    assert(encoder != NULL);
    if (! (encoder != NULL))
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    error = encoder->HandleMetadata();
    error = encoder->EncodeSample(frameBuffer, framePitch);
    errorFree = encoder->FreeLocalMetadata();	// 20090610 CMD - Do not clear encode error result
    if (error == CFHD_ERROR_OKAY)
    {
        return errorFree;						// 20090610 CMD - Get this as an error if encode was OK but free failed.
    }
    return error;								// 20090610 CMD - The saved encode error.
}

/*!	@function CFHD_GetSampleData

	@brief Get the most recent video sample encoded by a call to
	@ref CFHD_EncodeSample.

	@param encoderRef
	Reference to an encoder created by a call to @ref CFHD_OpenEncoder
	and initialized by a call to @ref CFHD_PrepareToEncode.

	@param sampleDataOut
	Pointer to a variable to receive the address of the encoded sample.

	@param sampleSizeOut
	Pointer to a variable to receive the size of the encoded sample in bytes.

	@discussion Separating the operation of obtaining the encoded sample
	from the operation of creating the encoded sample allows the encoder
	to manage memory more efficiently.  For example, it can reallocate the
	sample buffer if the size of the encoded sample is larger than expected.

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_GetSampleData(CFHD_EncoderRef encoderRef,
                   void **sampleDataOut,
                   size_t *sampleSizeOut)
{
    CFHD_Error error = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleEncoder *encoder = (CSampleEncoder *)encoderRef;
    assert(encoder != NULL);
    if (! (encoder != NULL))
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    error = encoder->GetSampleData(sampleDataOut, sampleSizeOut);

    return error;
}

/*!	@function CFHD_CloseEncoder

	@brief Release any resources allocated to the encoder.

	@param encoderRef
	Reference to an encoder created by a call to @ref CFHD_OpenEncoder
	and initialized by a call to @ref CFHD_PrepareToEncode.

	@discussion Do not attempt to use an encoder reference after the
	encoded has been closed by a call to this function.

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_CloseEncoder(CFHD_EncoderRef encoderRef)
{
    // Check the input arguments
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleEncoder *encoder = (CSampleEncoder *)encoderRef;

    delete encoder;

    return CFHD_ERROR_OKAY;
}

/*!	@function CFHD_GetEncodeThumbnail

	@brief The generate a thumbnail

	@description Extract the base wavelet into a using image thumbnail without
	decompressing the sample

	@param encoderRef
	Reference to an encoder engine created by a call to @ref CFHD_MetadataOpen that
	the current metadata should be attached.

	@param samplePtr
	Pointer to a sample containing one frame of encoded video in the
	CineForm HD format.

	@param sampleSize
	Size of the encoded sample.

	@param outputBuffer
	Buffer that will receive the thumbnail of size 1/8 x 1/8 the original frame.

	@param outputBufferSize
	Size must be at least ((w+7)/8) * ((h+7)/8) * 4 for 10-bit RGB format.

	@param flags
	future usage

	@param retWidth
	If successful contains thumbnail width.

	@param retHeight
	If successful contains thumbnail Height.

	@param retSize
	If successful contains thumbnail size in bytes.

	@return Returns a CFHD error code.
*/
CFHDENCODER_API CFHD_Error
CFHD_GetEncodeThumbnail(CFHD_EncoderRef encoderRef,
                        void *samplePtr,
                        size_t sampleSize,
                        void *outputBuffer,
                        size_t outputBufferSize,
                        uint32_t flags,
                        size_t *retWidth,
                        size_t *retHeight,
                        size_t *retSize)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (encoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }
    if (samplePtr == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }
    if (outputBuffer == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleEncoder *encoder = (CSampleEncoder *)encoderRef;

    if (flags == 0)
    {
        flags = 1;
    }

    errorCode = encoder->GetThumbnail(
                    samplePtr,
                    sampleSize,
                    outputBuffer,
                    outputBufferSize,
                    flags,
                    retWidth,
                    retHeight,
                    retSize);

    return errorCode;
}
