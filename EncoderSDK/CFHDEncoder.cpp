/*!
 * @file CFHDEncoder.cpp
 * @brief This module implements the C functions for the original encoder API.
 *
 * The original encoder API was not threaded.  For applications that perform encoding
 * using multiple threads, the asynchronous encoder API is recommended.  The original
 * encoder API used functions that take an encoder reference as the first argument.
 * The routines in the new asynchronous encoder API use an encoder pool reference.
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
        // 20090610 CMD - Get this as an error if encode was OK but free failed.
        return errorFree;
    }

    return error;
}

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

    errorCode = encoder->GetThumbnail(samplePtr,
                                      sampleSize,
                                      outputBuffer,
                                      outputBufferSize,
                                      flags,
                                      retWidth,
                                      retHeight,
                                      retSize);

    return errorCode;
}
