/*!
 * @file CFHDDecoder.h
 * @brief Interface to the CineForm HD decoder.  The decoder API uses an opaque
 * data type to represent an instance of an decoder.  The decoder reference
 * is returned by the call to @ref CFHD_OpenDecoder.
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

#ifndef CFHD_DECODER_H
#define CFHD_DECODER_H

#include "CFHDError.h"
#include "CFHDTypes.h"
#include "CFHDMetadata.h"
#ifdef __cplusplus
#include "CFHDSampleHeader.h"
#endif

#ifdef _WIN32
#ifndef DYNAMICLIB
#define CFHDDECODER_API
#else
#ifdef DECODERDLL_EXPORTS
// Export the entry points for the decoder
#define CFHDDECODER_API __declspec(dllexport)
#else
// Declare the entry points to the decoder
#define CFHDDECODER_API __declspec(dllimport)
#endif
#endif
#else
#ifdef DECODERDLL_EXPORTS
#define CFHDDECODER_API __attribute__((visibility("default")))
#else
#define CFHDDECODER_API
#endif
#endif

//! Opaque datatype for the CineForm HD decoder
typedef void *CFHD_DecoderRef;

// Interface to the codec library for use with either C or C++
#ifdef __cplusplus
extern "C" {
#endif


/*!
 * \brief Open an instance of the CineForm HD decoder.
 * \param decoderRefOut: An opaque reference to a decoder returned by but this function.
 * \param allocator: Optional CFHD_ALLOCATOR structure, for those was wishing to control memory allocations.
 * \return Returns a CFHD error code.
 *
 * Open an instance of the CineForm HD decoder and return a reference to the
 * decoder through the pointer provided as the first argument.
 */
CFHDDECODER_API CFHD_Error
CFHD_OpenDecoder(CFHD_DecoderRef *decoderRefOut,
                 CFHD_ALLOCATOR *allocator);

/*!
 * \brief Returns a list of output formats (in decreasing order of preference)
 *  that are appropriate for the encoded sample that is provided as an argument.
 * \param decoderRef: An opaque reference to a decoder created by a call to @ref CFHD_OpenDecoder.
 * \param samplePtr: The memory address of a CineForm compressed sample
 * \param sampleSize: The size of a CineForm compressed sample
 * \param outputFormatArray: Pointer to a preallocated array of type CFHD_PixelFormat.
 * \param outputFormatArrayLength: Number elements in the preallocated array of type CFHD_PixelFormat.
 * \param actualOutputFormatCountOut: Location to return the number of recommended formats.
 * \return Returns a CFHD error code.
 *
 * The CineForm HD codec encodes source video in a variety of internal formats
 * depending on the product in which the codec is delivered, the video source
 * format, and options provided to the encoder.
 * This routine examines the tags that are embedded in the encoded sample and
 * selects the output formats that are best for the encoded format, in decreasing
 * order of preference.
 * Output formats that are not appropriate to the encoded format are omitted.
 * For example, raw Bayer output formats are not provided if the encoded samples
 * are not raw Bayer data.  The list of output formats is ordered to avoid color
 * conversion and deeper pixel formats are listed first.
 */
CFHDDECODER_API CFHD_Error
CFHD_GetOutputFormats(CFHD_DecoderRef decoderRef,
                      void *samplePtr,
                      size_t sampleSize,
                      CFHD_PixelFormat *outputFormatArray,
                      int outputFormatArrayLength,
                      int *actualOutputFormatCountOut);

/*!
 * \brief Returns requested information about the current sample.
 * \param decoderRef: An opaque reference to a decoder created by a call to @ref CFHD_OpenDecoder.
 * \param samplePtr: The memory address of a CineForm compressed sample
 * \param sampleSize: The size of a CineForm compressed sample
 * \param tag: The request the desired data.
 * \param value: pointer to an buffer that holds the return value.
 * \param buffer_size: size of the buffer for the return value.
 * \return Returns a CFHD error code.
 *
 * Requesting miscellaneous information from a CineForm sample, by Tag-Value pair.
 */
CFHDDECODER_API CFHD_Error
CFHD_GetSampleInfo(CFHD_DecoderRef decoderRef,
                   void *samplePtr,
                   size_t sampleSize,
                   CFHD_SampleInfoTag tag,
                   void *value,
                   size_t buffer_size);

/*!
 * \brief Initializes an instance of the CineForm HD decoder that was created by a call to @ref CFHD_OpenDecoder.
 *
 * \param decoderRef: An opaque reference to a decoder created by a call to @ref CFHD_OpenDecoder.
 *
 * \param outputWidth:
 * The desired width of the decoded frame.  Pass zero to allow this routine
 * to choose the best output width.
 *
 * \param outputHeight:
 * The desired width of the decoded frame.  Pass zero to allow this routine
 * to choose the best output height.
 *
 * \param outputFormat:
 * The desired output format passed as a four character code.  The requested
 * output format will be used if it is one of the formats that would be returned
 * by a call to @ref CFHD_GetOutputFormats.  See the pixel formats defined in the
 * enumeration CFHD_PixelFormat.  The decoder will always output frames in the
 * specified pixel format if possible; otherwise, the call to this routine will
 * return an error code.
 *
 * \param decodedResolution:
 * The desired resolution for decoding relative to the encoded frame size.
 * See the possible resolutions defined in the enumeration CFHD_DecodedResolution.
 * If this argument is non-zero, it must specify a valid decoded resolution such
 * as full or half resolution.  The decoder will divide the encoded dimensions
 * by the divisor implied by this parameter to determine the actual output dimensions.
 *
 * \param decodingFlags:
 * Flags that specify options for initializing the decoder.  See the flags defined in
 * the enumeration for CFHD_DecodingFlags.  The decoding flags are not currently used.
 * Pass zero for this argument.
 *
 * \param samplePtr:
 * Pointer to an encoded sample that is representative of the samples that
 * will be passed to the decoder.  The sample is parsed to obtain information
 * about how the video was encoded.  This information guides this routine in
 * initializing the decoder.
 *
 * \param sampleSize:
 * Normally this size of the sample in bytes, if you intend to go on to decode the frame.
 * However, if you was only initializing a decode, and wish to reduce disk overhead,
 * you can set the size to a little as 512, as that is sufficient to pass all the need
 * information from the sample header.
 *
 * \param actualWidthOut:
 * Pointer to a variable that will receive the actual width of the decoded
 * frames.  The caller can pass NULL, but it is recommended that the caller
 * always use the actual dimensions and output format to allocate buffers
 * for the decoded frames.
 *
 * \param actualHeightOut:
 * Pointer to a variable that will receive the actual height of the decoded
 * frames.  The caller can pass NULL, but it is recommended that the caller
 * always use the actual dimensions and output format to allocate buffers
 * for the decoded frames.
 *
 * \param actualFormatOut:
 * Pointer to a variable that will receive the actual pixel format of the
 * decoded frames.  The caller can pass NULL, but should use the output pixel
 * format to determine the size of the output pixels for allocating the buffers
 * that will receive the decoded frames.
 *
 * \return Returns a CFHD error code.
 *
 * The caller can specify the exact dimensions of the decoded frame or pass zero
 * for either the output width or output height arguments to allow this routine
 * to choose the best output dimensions.  Typically, the output dimensions will
 * be the same as the encoded dimensions, with a reduction as specified by the
 * decoded resolution argument.
 * Likewise, the caller can specify an output pixel format	or allow the routine
 * to select the best format.
 * The function @ref CFHD_GetOutputFormats provides a list of output formats in
 * decreasing order of preference and this function will use the first output
 * format from that list if an output format is not specified.  The actual
 * output dimensions and pixel format are returned.
 *
 * If the output width or height are zero, the decoder will compute
 * the output width and height by using the encoded width and height obtained
 * from the video sample passed as an argument and reducing the width and height
 * as specified by the decoded resolution argument.  This makes it very easy to
 * initialize the decoder so that it provides frames at close to the desired
 * size needed by the application as efficiently as possible.  It is anticipated
 * that in this scenario the application will provide it own scaling routines if
 * necessary.
 */
CFHDDECODER_API CFHD_Error
CFHD_PrepareToDecode(CFHD_DecoderRef decoderRef,
                     int outputWidth,
                     int outputHeight,
                     CFHD_PixelFormat outputFormat,
                     CFHD_DecodedResolution decodedResolution,
                     CFHD_DecodingFlags decodingFlags,
                     void *samplePtr,
                     size_t sampleSize,
                     int *actualWidthOut,
                     int *actualHeightOut,
                     CFHD_PixelFormat *actualFormatOut);

#ifdef __cplusplus
/*!
 * \brief Parse the header in the encoded video sample. OBSOLETED by CFHD_GetSampleInfo()
 * \param samplePtr: The memory address of a CineForm compressed sample.
 * \param sampleSize: The size of a CineForm compressed sample.
 * \param sampleHeader: The address of a pre-allocated structure of type CFHD_SampleHeader.
 * \return Returns a CFHD error code.
 *
 * The sample header is parsed to obtain information about the video sample without decoding the video sample.
 */
CFHDDECODER_API CFHD_Error
CFHD_ParseSampleHeader(void *samplePtr,
                       size_t sampleSize,
                       CFHD_SampleHeader *sampleHeaderOut);
#endif

/*!
 * \brief Return the size of the specified pixel format (in bytes).
 * \param pixelFormat: CFHD_PixelFormat of the decoding pixel type.
 * \param pixelSizeOut: pointer to return the pixel size.
 * \return Returns a CFHD error code.
 *
 * Return the size of a pixel in byte is it uniquely addressable.
 * Note that the pixel size is not defined for some image formats such as v210.
 * This routine returns zero for pixel formats that do not have a size that is
 * an integer number of bytes. When the pixel size is not well-defined, it cannot
 * be used to compute the pitch of the image rows.  See @ref CFHD_GetImagePitch.
 */
CFHDDECODER_API CFHD_Error
CFHD_GetPixelSize(CFHD_PixelFormat pixelFormat, uint32_t *pixelSizeOut);

/*!
 * \brief Return the allocated length of each image row (in bytes).
 * \param imageWidth: Width of the image.
 * \param pixelFormat: CFHD_PixelFormat of the decoding pixel type.
 * \param imagePitchOut: pointer to return the rowsize/pitch in bytes.
 * \return Returns a CFHD error code.
 *
 * This routine must be used to determine the pitch for pixel formats such
 * as v210 where the pixel size is not defined.
 */
CFHDDECODER_API CFHD_Error
CFHD_GetImagePitch(uint32_t imageWidth, CFHD_PixelFormat pixelFormat, int32_t *imagePitchOut);

/*!
 * \brief Return the size of an image (in bytes).
 * \param imageWidth: Width of the image.
 * \param imageHeight: Height of the image. In the case of a 3D image, this is the height of a single eye.
 * \param pixelFormat: CFHD_PixelFormat of the decoding pixel type.
 * \param videoselect: CFHD_VideoSelect type to specifty if you are decoding left/right or both eyes.
 * \param stereotype: CFHD_Stereo3DType type to specifty 3D format if decoding both eyes.
 * \param imageSizeOut: pointer to return the image size in bytes.
 * \return Returns a CFHD error code.
 *
 * This image size returned by this routine can be used to allocate a  buffer
 * for a decoded 2D or 3D image.
 */
CFHDDECODER_API CFHD_Error
CFHD_GetImageSize(uint32_t imageWidth, uint32_t imageHeight, CFHD_PixelFormat pixelFormat,
                  CFHD_VideoSelect videoselect,	CFHD_Stereo3DType stereotype, uint32_t *imageSizeOut);

/*!
 * \brief Decode one frame of CineForm HD encoded video.
 * \param decoderRef: A reference to a decoder that was initialized by a call to CFHD_PrepareToDecode.
 * \param samplePtr: Pointer to a sample containing one frame of encoded video in the CineForm HD format.
 * \param sampleSize: Size of the encoded sample.
 * \param outputBuffer: Buffer that will receive the decoded frame.  The buffer must start on an address that is aligned to 16 bytes.
 * \param outputPitch: Pitch of the output buffer in bytes.  The pitch must be at least as large as the
 *  size of one row of decoded pixels.  Since each output row must start on an address
 *  that is aligned to 16 bytes, the pitch must be a multiple of 16 bytes.
 * \return Returns a CFHD error code.
 *
 * The decoder must have been initialized by a call to CFHD_PrepareToDecode.
 * The decoded frame will have the dimensions and format returned by the call to
 * CFHD_PrepareToDecode.
 */
CFHDDECODER_API CFHD_Error
CFHD_DecodeSample(CFHD_DecoderRef decoderRef,
                  void *samplePtr,
                  size_t sampleSize,
                  void *outputBuffer,
                  int32_t outputPitch);

/*!
 * \brief Set the metadata rules for the decoder.
 * \param decoderRef: An opaque reference to a decoder created by a call to @ref CFHD_OpenDecoder.
 * \param metadataRef: Reference to a metadata interface returned by a call to @ref CFHD_OpenMetadata.
 * \param tag: The FOURCC of the Tag you wish to add for active decoder control.
 * \param type: The data type of active metadata.
 * \param data: Pointer to the data.
 * \param size: The number of bytes of data.
 * \return Returns a CFHD error code.
 *
 * Decoder will use the active metadata store in the sample, or in the color
 * database or overrided by the Tags added by this function.  If you want the
 * decoder to use the original camera data with a few change, initialize
 * the metadata engine with @ref CFHD_InitSampleMetadata with the track set to
 * METADATATYPE_ORIGINAL. Then call CFHD_SetActiveMetadata we the tag you want
 * it to act upon (new whilebalanc, Look etc.)
 */
CFHDDECODER_API CFHD_Error
CFHD_SetActiveMetadata(CFHD_DecoderRef decoderRef,
                       CFHD_MetadataRef metadataRef,
                       unsigned int tag,
                       CFHD_MetadataType type,
                       void *data,
                       unsigned int size);

/*!
 * \brief The generate a thumbnail
 * \param decoderRef: An opaque reference to a decoder created by a call to @ref CFHD_OpenDecoder.
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
 * Extract the base wavelet into a using image thumbnail without decompressing the sample.
 */
CFHDDECODER_API CFHD_Error
CFHD_GetThumbnail(CFHD_DecoderRef decoderRef,
                  void *samplePtr,
                  size_t sampleSize,
                  void *outputBuffer,
                  size_t outputBufferSize,
                  uint32_t flags,
                  size_t *retWidth,
                  size_t *retHeight,
                  size_t *retSize);

//! Clear the metadata rules for the decoder
CFHDDECODER_API CFHD_Error
CFHD_ClearActiveMetadata(CFHD_DecoderRef decoderRef,
                         CFHD_MetadataRef metadataRef);

/*!
 * \brief Close an instance of the CineForm HD decoder and release all resources.
 * \param decoderRef: A reference to a decoder that was initialized by a call to CFHD_PrepareToDecode.
 * \return Returns a CFHD error code.
 *
 * Do not attempt to use the decoder after it has been closed by a call to this routine.
 */
CFHDDECODER_API CFHD_Error
CFHD_CloseDecoder(CFHD_DecoderRef decoderRef);


#ifdef __cplusplus
}
#endif

#endif // CFHD_DECODER_H
