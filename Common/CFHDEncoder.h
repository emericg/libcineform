/*!
 * @file CFHDEncoder.h
 * @brief Interface to the CineForm HD encoder.  The encoder API uses an opaque
 * data type to represent an instance of an encoder.  The encoder reference
 * is returned by the call to @ref CFHD_OpenEncoder.
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

#ifndef CFHD_ENCODER_H
#define CFHD_ENCODER_H

#include "CFHDError.h"
#include "CFHDTypes.h"

#ifdef _WIN32
#ifndef DYNAMICLIB
#define CFHDENCODER_API
#else
#ifdef ENCODERDLL_EXPORTS
// Export the entry points for the encoder
#define CFHDENCODER_API __declspec(dllexport)
#else
// Declare the entry points to the encoder
#define CFHDENCODER_API __declspec(dllimport)
#endif
#endif
#else
#ifdef ENCODERDLL_EXPORTS
#define CFHDENCODER_API __attribute__((visibility("default")))
#else
#define CFHDENCODER_API
#endif
#endif

// Convenience macro that defines the entry point and return type
#define CFHDENCODER_API_(type) CFHDENCODER_API type

// Opaque datatypes for the CineForm HD encoder
typedef void *CFHD_EncoderRef;
typedef void *CFHD_MetadataRef;
typedef void *CFHD_EncoderPoolRef;
typedef void *CFHD_SampleBufferRef;

// Interface to the codec library for use with either C or C++
#ifdef __cplusplus
extern "C" {
#endif


/*!
 * \brief Open an instance of the CineForm HD encoder and return a reference.
    to the encoder through the pointer provided as the first argument.
 * \param encoderRefOut: Pointer to the variable that will receive the encoder reference.
 * \param allocator: CFHD_ALLOCATOR structure, for those was wishing to control memory allocations. Pass NULL if not used.
 * \return Returns a CFHD error code.
 */
CFHDENCODER_API CFHD_Error
CFHD_OpenEncoder(CFHD_EncoderRef *encoderRefOut,
                 CFHD_ALLOCATOR *allocator);

/*!
 * \brief Return a list of pixel formats (in decreasing order of preference)
 *  that can be used for the input frames passed to the encoder.
 * \param encoderRef: Reference to an encoder created by a call to @ref CFHD_OpenEncoder.
 * \param inputFormatArray: CFHD pixel format array that will receive the list of pixel formats.
 * \param inputFormatArrayLength: Maximum number of pixel formats in the input format array.
 * \param actualInputFormatCountOut: Return count of the actual number of pixel formats copied into the input format array.
 * \return Returns a CFHD error code.
 */
CFHDENCODER_API CFHD_Error
CFHD_GetInputFormats(CFHD_EncoderRef encoderRef,
                     CFHD_PixelFormat *inputFormatArray,
                     int inputFormatArrayLength,
                     int *actualInputFormatCountOut);

/*!
 * \brief Initialize an encoder instance for encoding.
 * \param encoderRef: Reference to an encoder created by a call to @ref CFHD_OpenEncoder.
 * \param inputWidth: Width of each input frame in pixels.
 * \param inputHeight: Number of lines in each input frame.
 * \param inputFormat: Format of the pixels in the input frames.
 * \param encodedFormat: Encoding format used internally by the codec.
 *  Video can be encoded as three channels of RGB with 4:4:4 sampling,
 *  three channels of YUV with 4:2:2 sampling, or other formats.
 *  See the formats listed in CFHD_EncodedFormat.
 * \param encodingFlags: Flags that provide further information about the video format.
 *  See the flags defined in CFHD_EncodingFlags.
 * \param encodingQuality: Quality to use for encoding.  Corresponds to the setting in the export
 *  dialog boxes. 0=Fixed, 1=Low, 2=Medium, 3=High, 4=FilmScan1, 5=FilmScan2
 * \return Returns a CFHD error code.
 *
 * Initialize for encoding frames with the specified dimensions and format.
 */
CFHDENCODER_API CFHD_Error
CFHD_PrepareToEncode(CFHD_EncoderRef encoderRef,
                     int frameWidth,
                     int frameHeight,
                     CFHD_PixelFormat pixelFormat,
                     CFHD_EncodedFormat encodedFormat,
                     CFHD_EncodingFlags encodingFlags,
                     CFHD_EncodingQuality encodingQuality);

/*!
 * \brief Encode one frame of CineForm HD video.
 * \param encoderRef: Reference to an encoder created by a call to @ref CFHD_OpenEncoder.
 * \param frameBuffer: Pointer to the frame to encode.
 * \param framePitch: Number of bytes between rows in the frame.
 * \return Returns a CFHD error code.
 *
 * The encoder must have been initialized by a call to @ref CFHD_PrepareToEncode
 * before attempting to encode frames.
 * The width and height of the frame and the pixel format must be the same as
 * declared in the call to @ref CFHD_PrepareToEncode.
 */
CFHDENCODER_API CFHD_Error
CFHD_EncodeSample(CFHD_EncoderRef encoderRef,
                  void *frameBuffer,
                  int framePitch);

/*!
 * \brief Get the data and size of the most recent video sample encoded by a call to @ref CFHD_EncodeSample.
 * \param encoderRef: Reference to an encoder created by a call to @ref CFHD_OpenEncoder
 *  and initialized by a call to @ref CFHD_PrepareToEncode.
 * \param sampleDataOut: Pointer to a variable to receive the address of the encoded sample.
 * \param sampleSizeOut: Pointer to a variable to receive the size of the encoded sample in bytes.
 * \return Returns a CFHD error code.
 *
 * Separating the operation of obtaining the encoded sample from the operation
 * of creating the encoded sample allows the encoder to manage memory more efficiently.
 * For example, it can reallocate the sample buffer if the size of the encoded
 * sample is larger than expected.
 */
CFHDENCODER_API CFHD_Error
CFHD_GetSampleData(CFHD_EncoderRef encoderRef,
                   void **sampleDataOut,
                   size_t *sampleSizeOut);

/*!
 * \brief Close an instance of the CineForm HD encoder and release any resources allocated.
 * \param encoderRef: Reference to an encoder created by a call to @ref CFHD_OpenEncoder
 *  and initialized by a call to @ref CFHD_PrepareToEncode.
 * \return Returns a CFHD error code.
 *
 * Do not attempt to use an encoder reference after the encoded has been closed
 * by a call to this function.
 */
CFHDENCODER_API CFHD_Error
CFHD_CloseEncoder(CFHD_EncoderRef encoderRef);

/*!
 * @brief The generate a thumbnail
 * @param encoderRef: Reference to an encoder engine created by a call
 *  to @ref CFHD_MetadataOpen that the current metadata should be attached.
 * \param samplePtr: Pointer to a sample containing one frame of encoded video in the CineForm HD format.
 * \param sampleSize: Size of the encoded sample.
 * \param outputBuffer: Buffer that will receive the thumbnail of size 1/8 x 1/8 the original frame.
 * \param outputBufferSize: Size must be at least ((w+7)/8) * ((h+7)/8) * 4 for 10-bit RGB format.
 * \param flags: future usage.
 * \param retWidth: If successful contains thumbnail width.
 * \param retHeight: If successful contains thumbnail Height.
 * \param retSize: If successful contains thumbnail size in bytes.
 * \return Returns a CFHD error code.
 *
 * Extract the base wavelet into a using image thumbnail without decompressing the sample
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
                        size_t *retSize);

/*!
 * \brief Opens a handle for attaching metadata.
 * \param metadataRefOut: Pointer to the variable that will receive the metadata reference.
 * \return Returns a CFHD error code.
 *
 * Opens a handle for attaching metadata is one of two class: global and local.
 * Global is for metadata that should appear in every frame, and is likely not changing.
 * Local is for metadata that only attached sometimes or is change often.  Something changing
 * every frame can use either class. If an item frames every ten frame, global will store the
 * last value for the non changing frame, whereas local on store data on the frames impacted.
 */
CFHDENCODER_API CFHD_Error
CFHD_MetadataOpen(CFHD_MetadataRef *metadataRefOut);

/*!
 * \brief Adds metadata for later attachment to the encoded bitstream.
 * \param metadataRef: Reference to an metadata engine created by a call to @ref CFHD_MetadataOpen.
 * \param tag: FOURCC code for the tag to add.
 * \param type: CFHD_MetadataType of the data with this tag.
 * \param size: number of byte of data within the tag.
 * \param data:data for the tag.
 * \param local:If the local flag is set, the metadata is will be local and only
 *  placed in the next frame to be encoded.  Otherwise, the metadata will be used for all frames.
 * \return Returns a CFHD error code.
 *
 * \todo Change the metadata size to size_t and the data pointer to void * to eliminate
 * unnecessary compiler warnings.
 *
 * The CineForm metadata can be in two classes, global and local.
 * Global is the most common, adding the same fields to every frame, whether
 * the fields are changing of not.  Local only places the metadata in the current
 * frame that is about to be encoded.  If you want only local metadata, set the
 * local flag.  Examples, director, DP and timecode is global, closed captioning
 * is local. CFHD_MetadataAdd requires a call to @ref CFHD_MetadataAdd to bind the
 * metadata to the encoded frame -- separating these function helps with threading.
 *
 * While CFHD_MetadataAdd is thread safe, it should not be threaded with
 * multiple encoders like CFHD_MetadataAttach can with one metadataRef pointwe.
 * If you wish to control metadata on a per-frame basis, you should have a separate
 * metadataRefs for each thread. Non-frame accurate global data could have it own
 * metadataRef, calling CFHD_MetadataAttach one with each thread, then use the threaded
 * metadataRefs for frame accurate local metadata.
 */
CFHDENCODER_API CFHD_Error
CFHD_MetadataAdd(CFHD_MetadataRef metadataRef,
                 uint32_t tag,
                 CFHD_MetadataType type,
                 size_t size,
                 uint32_t *data,
                 bool temporary);

/*!
 * \brief Attaches metadata to the encoded bitstream.
 * \param encoderRef:Reference to an encoder engine created by a call
 *  to @ref CFHD_MetadataOpen that the current metadata should be attached.
 * \param metadataRef: Reference to an metadata engine created by a call to @ref CFHD_MetadataOpen..
 * \return Returns a CFHD error code.
 *
 * Attaches all data allocated with @ref CFHD_MetadataAdd to the next encoded frame.
 * CFHD_MetadataAttach can be used concurrently by threaded instances of the encoder.
 * Note that @ref CFHD_MetadataAdd is not thread safe.
 */
CFHDENCODER_API CFHD_Error
CFHD_MetadataAttach(CFHD_EncoderRef encoderRef, CFHD_MetadataRef metadataRef);

/*!
 * \brief Release any resources allocated to the CFHD_MetadataOpen.
 * \param metadataRef: Reference to an metadata engine created by a call
 *  to @ref CFHD_MetadataOpen and initialized by a calls to @ref CFHD_MetadataAdd.
 * \return Returns a CFHD error code.
 *
 * Do not attempt to use an metadata reference after being closed by a call to
 * this function.
 */
CFHDENCODER_API CFHD_Error
CFHD_MetadataClose(CFHD_MetadataRef metadataRef);

//! Create an encoder pool for asynchronous encoding
CFHDENCODER_API CFHD_Error
CFHD_CreateEncoderPool(CFHD_EncoderPoolRef *encoderPoolRefOut,
                       int encoderThreadCount,
                       int jobQueueLength,
                       CFHD_ALLOCATOR *allocator);

//! Return a list of input formats in decreasing order of preference
CFHDENCODER_API CFHD_Error
CFHD_GetAsyncInputFormats(CFHD_EncoderPoolRef encoderPoolRef,
                          CFHD_PixelFormat *inputFormatArray,
                          int inputFormatArrayLength,
                          int *actualInputFormatCountOut);

//! Prepare the asynchronous encoders in a pool for encoding
CFHDENCODER_API CFHD_Error
CFHD_PrepareEncoderPool(CFHD_EncoderPoolRef encoderPoolRef,
                        uint_least16_t frameWidth,
                        uint_least16_t frameHeight,
                        CFHD_PixelFormat pixelFormat,
                        CFHD_EncodedFormat encodedFormat,
                        CFHD_EncodingFlags encodingFlags,
                        CFHD_EncodingQuality encodingQuality);

//! Attach metadata to all of the encoders in the pool
CFHDENCODER_API CFHD_Error
CFHD_AttachEncoderPoolMetadata(CFHD_EncoderPoolRef encoderPoolRef,
                               CFHD_MetadataRef metadataRef);

//! Start the asynchronous encoders
CFHDENCODER_API CFHD_Error
CFHD_StartEncoderPool(CFHD_EncoderPoolRef encoderPoolRef);

//! Stop the asynchronous encoders
CFHDENCODER_API CFHD_Error
CFHD_StopEncoderPool(CFHD_EncoderPoolRef encoderPoolRef);

//! Submit a frame for asynchronous encoding
CFHDENCODER_API CFHD_Error
CFHD_EncodeAsyncSample(CFHD_EncoderPoolRef encoderPoolRef,
                       uint32_t frameNumber,
                       void *frameBuffer,
                       intptr_t framePitch,
                       CFHD_MetadataRef metadataRef);

//! Wait until the next encoded sample is ready
CFHDENCODER_API CFHD_Error
CFHD_WaitForSample(CFHD_EncoderPoolRef encoderPoolRef,
                   uint32_t *frameNumberOut,
                   CFHD_SampleBufferRef *sampleBufferRefOut);

//! Test whether the next encoded sample is ready
CFHDENCODER_API CFHD_Error
CFHD_TestForSample(CFHD_EncoderPoolRef encoderPoolRef,
                   uint32_t *frameNumberOut,
                   CFHD_SampleBufferRef *sampleBufferRefOut);

//! Get the size and address of an encoded sample
CFHDENCODER_API CFHD_Error
CFHD_GetEncodedSample(CFHD_SampleBufferRef sampleBufferRef,
                      void **sampleDataOut,
                      size_t *sampleSizeOut);

//! Get the thumbnail image from an encoded sample
CFHDENCODER_API CFHD_Error
CFHD_GetSampleThumbnail(CFHD_SampleBufferRef sampleBufferRef,
                        void *thumbnailBuffer,
                        size_t bufferSize,
                        uint32_t flags,
                        uint_least16_t *actualWidthOut,
                        uint_least16_t *actualHeightOut,
                        CFHD_PixelFormat *pixelFormatOut,
                        size_t *actualSizeOut);

//! Release the sample buffer
CFHDENCODER_API CFHD_Error
CFHD_ReleaseSampleBuffer(CFHD_EncoderPoolRef encoderPoolRef,
                         CFHD_SampleBufferRef sampleBufferRef);

//! Release the encoder pool
CFHDENCODER_API CFHD_Error
CFHD_ReleaseEncoderPool(CFHD_EncoderPoolRef encoderPoolRef);


#ifdef __cplusplus
}
#endif

#endif // CFHD_ENCODER_H
