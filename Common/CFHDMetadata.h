/*!
 * @file CFHDMetadata.h
 * @brief Metadata parsing functions.
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

#ifndef CFHD_METADATA_H
#define CFHD_METADATA_H

#include "CFHDError.h"
#include "CFHDTypes.h"
#include "CFHDMetadataTags.h"

#ifdef _WIN32
#ifndef DYNAMICLIB
#define CFHDMETADATA_API
#else
#ifdef METADATADLL_EXPORTS
// Export the entry points for the metadata interface
#define CFHDMETADATA_API __declspec(dllexport)
#else
// Declare the entry points to the metadata interface
#define CFHDMETADATA_API __declspec(dllimport)
#endif
#endif
#else
#ifdef METADATADLL_EXPORTS
#define CFHDMETADATA_API __attribute__((visibility("default")))
#else
#define CFHDMETADATA_API
#endif
#endif

//! Opaque datatype for the CineForm HD metadata
typedef void *CFHD_MetadataRef;

// Interface to the codec library for use with either C or C++
#ifdef __cplusplus
extern "C" {
#endif


/*!
 * \brief Creates an interface to CineForm HD metadata.
 * \return Returns a CFHD error code.
 *
 * This function creates a interface that can be used to read CineForm HD metadata.
 * A reference to the metadata interface is returned if the call was successful.
 */
CFHDMETADATA_API CFHD_Error
CFHD_OpenMetadata(CFHD_MetadataRef *metadataRefOut);

/*!
 * \brief Opens an interface to CineForm HD metadata in the specified sample.
 * \param metadataRef: Reference to a metadata interface returned by a call to @ref CFHD_OpenMetadata.
 * \param track: set the type of metadata to be extracted, camera original, user
 *  changes, and/or filtered against active decoding elements.
 * \param sampleData: Pointer to a sample of CineForm HD encoded video.
 * \param sampleSize: Size of the encoded sample in bytes.
 * \return Returns a CFHD error code.
 *
 * This function intializes metadata from a sample of CineForm HD encoded video.
 * This is call on each new sample before retrieve any metadata from the sample.
 */
CFHDMETADATA_API CFHD_Error
CFHD_InitSampleMetadata(CFHD_MetadataRef metadataRef,
                        CFHD_MetadataTrack track,
                        void *sampleData,
                        size_t sampleSize);

/*!
 * \brief Returns the next available metadata entry. Calling recursively will
 *  retrieve all the samples metadata until CFHD_ERROR_METADATA_END is returned.
 * \param metadataRef: Reference to a metadata interface returned by a call
 *  to @ref CFHD_OpenMetadata.
 * \param tag: Pointer to the variable to receive the FOURCC metadata tag.
 * \param type: Pointer to the variable to receive the CFHD_MetadataType.  This
 *  specify the type of data returned, such as METADATATYPE_STRING, METADATATYPE_UINT32 or METADATATYPE_FLOAT.
 * \param data: Pointer to the variable to receive the address of the metadata.
 * \param size: Pointer to the variable to receive the size of the metadata array in bytes.
 * \return Returns the CFHD error code CFHD_ERROR_METADATA_END if no more
 *  metadata was not found in the sample; otherwise, the CFHD error code
 *  CFHD_ERROR_OKAY is returned if the operation succeeded.
 *
 * After a call to @ref CFHD_InitSampleMetadata the next call to
 * this function returns the first metadata tag/size/value group.  The next call
 * returns the next metadata group and so on until all the data is extracted.
 */
CFHDMETADATA_API CFHD_Error
CFHD_ReadMetadata(CFHD_MetadataRef metadataRef,
                  CFHD_MetadataTag *tag,
                  CFHD_MetadataType *type,
                  void **data,
                  CFHD_MetadataSize *size);

/*!
 * \brief Returns the data for a particular metadata entry.
 * \param metadataRef: Reference to a metadata interface returned by a call to @ref CFHD_OpenMetadata.
 * \param tag: is the FOURCC for the requested data.
 * \param type: Pointer to the variable to receive the CFHD_MetadataType.  This
 *  specify the type of data returned, such as METADATATYPE_STRING,
 *  METADATATYPE_UINT32 or METADATATYPE_FLOAT.
 * \param data: Pointer to the variable to receive the address of the metadata.
 * \param size: Pointer to the variable to receive the size of the metadata array in bytes.
 * \return Returns the CFHD error code CFHD_ERROR_METADATA_END if no more
 *  metadata was not found in the sample; otherwise, the CFHD error code
 *  CFHD_ERROR_OKAY is returned if the operation succeeded.
 *
 * After a call to @ref CFHD_InitSampleMetadata the next call to this function
 * returns the data for an particular metadata entry.
 */
CFHDMETADATA_API CFHD_Error
CFHD_FindMetadata(CFHD_MetadataRef metadataRef,
                  CFHD_MetadataTag tag,
                  CFHD_MetadataType *type,
                  void **data,
                  CFHD_MetadataSize *size);

/*!
 * \brief Releases an interface to CineForm HD metadata.
 * \param metadataRef Reference to a metadata interface returned by a call to @ref CFHD_OpenMetadata.
 * \return Returns a CFHD error code.
 *
 * This function releases an interface to CineForm HD metadata created by calls
 * to routines that obtain the metadata from various sources, such as @ref CFHD_ReadMetadata.
 * All resources allocated by the metadata interface are released.  It is a serious
 * error to call any functions in the metadata API after the interface has been released.
 */
CFHDMETADATA_API CFHD_Error
CFHD_CloseMetadata(CFHD_MetadataRef metadataRef);


#ifdef __cplusplus
}
#endif

#endif // CFHD_METADATA_H
