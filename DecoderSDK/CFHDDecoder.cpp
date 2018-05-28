/*! @file CFHDDecoder.cpp

*  @brief This module implements the C functions for the decoder API
*
*  Interface to the CineForm HD decoder.  The decoder API uses an opaque
*  data type to represent an instance of a decoder.  The decoder reference
*  is returned by the call to @ref CFHD_OpenDecoder.
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
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if _WIN32

// Export the interface to the decoder
#define DECODERDLL_EXPORTS	1

#elif __APPLE__

#define DECODERDLL_EXPORTS	1

#ifdef DECODERDLL_API
#undef DECODERDLL_API
#endif

#define DECODERDLL_API __attribute__((visibility("default")))
#include <CoreFoundation/CoreFoundation.h>

#else

// Code required by GCC on Linux to define the entry points

#ifdef DECODERDLL_API
#undef DECODERDLL_API
#endif

#define DECODERDLL_API __attribute__((visibility("default")))

#endif

// Include declarations from the codec library
#include "decoder.h"
#include "swap.h"
#include "thumbnail.h"

// Include declarations for the decoder component
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "IAllocator.h"
#include "ISampleDecoder.h"
#include "SampleDecoder.h"
#include "SampleMetadata.h"


#if _WIN32

#ifdef DYNAMICLIB
BOOL APIENTRY DllMain(HANDLE hModule,
                      DWORD ulReasonForCall,
                      LPVOID lpReserved)
{
    switch (ulReasonForCall)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
#endif

#else

void _splitpath( const char *fullPath, char *drive, char *dir, char *fname, char *ext)
{
    int pathLen = 0;
    char *namePtr;
    char *extPtr;
    char *originalNamePtr;

    drive[0] = '\0';
    dir[0] = '\0';
    fname[0] = '\0';
    ext[0] = '\0';
    originalNamePtr = namePtr = (char *)malloc( strlen( fullPath ) + 1 );
    if (namePtr)
    {
        strcpy( namePtr, fullPath );
        while ( namePtr[0] && strchr( namePtr, '/' ) )
        {
            pathLen++;
            namePtr++;
        }
        strncpy( dir, fullPath, pathLen );
        dir[pathLen] = '\0';
        extPtr = strrchr( namePtr, '.');
        if (extPtr)
        {
            strcpy( ext, extPtr );
            namePtr = strtok(namePtr, extPtr );
        }
        strcpy( fname, namePtr );
        free(originalNamePtr);
    }
}

void _makepath(char *filename,  char *drive, char *dir, char *fname, char *ext)
{
    filename = strcat( fname, ext );
}

#endif

CFHDDECODER_API CFHD_Error
CFHD_OpenDecoder(CFHD_DecoderRef *decoderRefOut,
                 CFHD_ALLOCATOR *allocator)
{
    // Check the input arguments
    if (decoderRefOut == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    // Allocate a new decoder data structure
    CSampleDecoder *decoderRef = new CSampleDecoder;
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_OUTOFMEMORY;
    }

    decoderRef->SetAllocator(allocator);

    // Return the decoder data structure
    *decoderRefOut = (CFHD_DecoderRef)decoderRef;

    return CFHD_ERROR_OKAY;
}

CFHDDECODER_API CFHD_Error
CFHD_GetOutputFormats(CFHD_DecoderRef decoderRef,
                      void *samplePtr,
                      size_t sampleSize,
                      CFHD_PixelFormat *outputFormatArray,
                      int outputFormatArrayLength,
                      int *actualOutputFormatCountOut)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    errorCode = decoder->GetOutputFormats(samplePtr,
                                          sampleSize,
                                          outputFormatArray,
                                          outputFormatArrayLength,
                                          actualOutputFormatCountOut);

    return errorCode;
}

CFHDDECODER_API CFHD_Error
CFHD_GetSampleInfo(CFHD_DecoderRef decoderRef,
                   void *samplePtr,
                   size_t sampleSize,
                   CFHD_SampleInfoTag tag,
                   void *value,
                   size_t buffer_size)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    errorCode = decoder->GetSampleInfo(samplePtr,
                                       sampleSize,
                                       tag,
                                       value,
                                       buffer_size);

    return errorCode;
}

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
                     CFHD_PixelFormat *actualFormatOut)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    errorCode = decoder->PrepareDecoder(outputWidth,
                                        outputHeight,
                                        outputFormat,
                                        decodedResolution,
                                        decodingFlags,
                                        samplePtr,
                                        sampleSize,
                                        actualWidthOut,
                                        actualHeightOut,
                                        actualFormatOut);
    if (errorCode != CFHD_ERROR_OKAY)
    {
        return errorCode;
    }

    return errorCode;
}

CFHDDECODER_API CFHD_Error
CFHD_ParseSampleHeader(void *samplePtr,
                       size_t sampleSize,
                       CFHD_SampleHeader *sampleHeader)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Catch any errors in the decoder
    try
    {
        CFHD_EncodedFormat encodedFormat = CFHD_ENCODED_FORMAT_YUV_422;
        CFHD_FieldType fieldType = CFHD_FIELD_TYPE_UNKNOWN;

        // Initialize a bitstream to the sample data
        BITSTREAM bitstream;
        InitBitstreamBuffer(&bitstream, (uint8_t *)samplePtr, sampleSize, BITSTREAM_ACCESS_READ);

        // Clear the fields in the sample header
        SAMPLE_HEADER header;
        memset(&header, 0, sizeof(SAMPLE_HEADER));

        // Decode the sample header
        bool result = ::ParseSampleHeader(&bitstream, &header);
        if (!result)
        {
            // The frame dimensions must be obtained from the encoded sample
            if (header.width == 0 || header.height == 0)
            {
                assert(0);
                errorCode = CFHD_ERROR_BADSAMPLE;
                goto finish;
            }

            // Try to fill in missing information with default values
            if (header.encoded_format == ENCODED_FORMAT_UNKNOWN)
            {
                // The encoded format is probably YUV 4:2:2
                header.encoded_format = ENCODED_FORMAT_YUV_422;
            }

            // It is okay if the original input format is not known
        }

        // Copy the sample header information to the output

        encodedFormat = CSampleDecoder::EncodedFormat(header.encoded_format);
        sampleHeader->SetEncodedFormat(encodedFormat);

        fieldType = CSampleDecoder::FieldType(&header);
        sampleHeader->SetFieldType(fieldType);

        sampleHeader->SetFrameSize(header.width, header.height);
    }
    catch (...)
    {
#if _WIN32
        char message[256];
        sprintf_s(message, sizeof(message), "CSampleDecoder::PrepareDecoder caught internal codec error\n");
        OutputDebugString(message);
#endif
        return CFHD_ERROR_INTERNAL;
    }

finish:
    return errorCode;
}

CFHDDECODER_API CFHD_Error
CFHD_GetPixelSize(CFHD_PixelFormat pixelFormat, uint32_t *pixelSizeOut)
{
    CFHD_Error ret = CFHD_ERROR_OKAY;
    if (pixelSizeOut == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    // Catch any errors in the decoder
    try
    {
        *pixelSizeOut = (uint32_t)GetPixelSize(pixelFormat);
    }
    catch (...)
    {
        *pixelSizeOut = 0;
        ret = CFHD_ERROR_BADFORMAT;
    }

    return ret;
/*
    uint32_t pixelSize = 0;

    switch (pixelFormat)
    {
    case CFHD_PIXEL_FORMAT_YUY2:
    case CFHD_PIXEL_FORMAT_2VUY:
    case CFHD_PIXEL_FORMAT_BYR2:
    case CFHD_PIXEL_FORMAT_BYR4:
    case CFHD_PIXEL_FORMAT_CT_10BIT_2_8:		// Avid format with two planes of 2-bit and 8-bit pixels
        pixelSize = 2;
        break;

    case CFHD_PIXEL_FORMAT_V210:
        pixelSize = 0;				// 3 is close, but no true pixel size can be returned.
        break;

    case CFHD_PIXEL_FORMAT_BGRA:
    case CFHD_PIXEL_FORMAT_BGRa:
    case CFHD_PIXEL_FORMAT_R408:
    case CFHD_PIXEL_FORMAT_V408:
    case CFHD_PIXEL_FORMAT_R210:
    case CFHD_PIXEL_FORMAT_DPX0:
    case CFHD_PIXEL_FORMAT_RG30:
    case CFHD_PIXEL_FORMAT_AB10:
    case CFHD_PIXEL_FORMAT_AR10:
    case CFHD_PIXEL_FORMAT_YU64:
    case CFHD_PIXEL_FORMAT_CT_SHORT_2_14:		// Avid fixed point 2.14 pixel format
    case CFHD_PIXEL_FORMAT_CT_USHORT_10_6:		// Avid fixed point 10.6 pixel format
    case CFHD_PIXEL_FORMAT_CT_SHORT:			// Avid 16-bit signed pixels
        pixelSize = 4;
        break;

    case CFHD_PIXEL_FORMAT_RG48:
    case CFHD_PIXEL_FORMAT_WP13:
        pixelSize = 6;
        break;

    case CFHD_PIXEL_FORMAT_B64A:
    case CFHD_PIXEL_FORMAT_W13A:
        pixelSize = 8;
        break;

    default:
        //TODO: Add more pixel formats
        assert(0);
        return CFHD_ERROR_INVALID_ARGUMENT;
        break;
    }

    *pixelSizeOut = pixelSize;
*/
    return CFHD_ERROR_OKAY;
}

CFHDDECODER_API CFHD_Error
CFHD_GetImagePitch(uint32_t imageWidth, CFHD_PixelFormat pixelFormat, int32_t *imagePitchOut)
{
    int32_t imagePitch = GetFramePitch(imageWidth, pixelFormat);
    if (imagePitchOut)
    {
        // Return the image pitch (in bytes)
        *imagePitchOut = imagePitch;
        return CFHD_ERROR_OKAY;
    }

    return CFHD_ERROR_INVALID_ARGUMENT;
}

CFHDDECODER_API CFHD_Error
CFHD_GetImageSize(uint32_t imageWidth, uint32_t imageHeight, CFHD_PixelFormat pixelFormat,
                  CFHD_VideoSelect videoselect,	CFHD_Stereo3DType stereotype, uint32_t *imageSizeOut)
{
    uint32_t imagePitch = GetFramePitch(imageWidth, pixelFormat);
    uint32_t imageSize = imagePitch * imageHeight;

    if (stereotype == STEREO3D_TYPE_DEFAULT && videoselect == VIDEO_SELECT_BOTH_EYES)
        imageSize *= 2;

    if (imageSizeOut)
    {
        // Return the size of the image (in bytes)
        *imageSizeOut = imageSize;
        return CFHD_ERROR_OKAY;
    }
    return CFHD_ERROR_INVALID_ARGUMENT;
}

CFHDDECODER_API CFHD_Error
CFHD_DecodeSample(CFHD_DecoderRef decoderRef,
                  void *samplePtr,
                  size_t sampleSize,
                  void *outputBuffer,
                  int outputPitch)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    // Test the memory buffer provided for the required size
    try
    {
        uint32_t length = 0;
        uint8_t *test_mem = (uint8_t *)outputBuffer;

        decoder->GetRequiredBufferSize(length);

        test_mem[0] = 0;
        if (length > 0)
        {
            int len = length;
            if (outputPitch > 0)
                test_mem[len - 1] = 0;
            if (outputPitch < 0)
                test_mem[-(len + outputPitch)] = 0;
        }
    }
    catch (...)
    {
#ifdef _WIN32
        OutputDebugString("Target memory buffer is an invalid size");
#endif
        return CFHD_ERROR_DECODE_BUFFER_SIZE;
    }

    errorCode = decoder->DecodeSample(samplePtr, sampleSize, outputBuffer, outputPitch);
    if (errorCode != CFHD_ERROR_OKAY)
    {
        return errorCode;
    }

    return CFHD_ERROR_OKAY;
}

CFHDDECODER_API CFHD_Error
CFHD_CloseDecoder(CFHD_DecoderRef decoderRef)
{
    //CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    delete decoder;

    return CFHD_ERROR_OKAY;
}


#include "CFHDMetadata.h"
#include "SampleMetadata.h"
#include "../Codec/metadata.h"

//! Table of CRCs of all 8-bit messages.
uint32_t crc_table[256];

//! Flag: has the table been computed? Initially false.
int crc_table_computed = 0;

//! Make the table for a fast CRC.
void make_crc_table(void)
{
    uint32_t c;
    int n, k;

    for (n = 0; n < 256; n++)
    {
        c = (uint32_t) n;
        for (k = 0; k < 8; k++)
        {
            if (c & 1)
                c = 0xedb88320L ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[n] = c;
    }
    crc_table_computed = 1;
}

/*!
 * Update a running CRC with the bytes buf[0..len-1]--the CRC should be initialized
 * to all 1's, and the transmitted value is the 1's complement of the final running
 * CRC (see the crc() routine below)).
 */
uint32_t update_crc(uint32_t crc, unsigned char *buf, int len)
{
    uint32_t c = crc;

    if (!crc_table_computed)
        make_crc_table();
    for (int n = 0; n < len; n++)
    {
        c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
    }

    return c;
}

//! Return the CRC of the bytes buf[0..len-1].
uint32_t calccrc(unsigned char *buf, int len)
{
    return update_crc(0xffffffffL, buf, len) ^ 0xffffffffL;
}

CFHDDECODER_API CFHD_Error
CFHD_SetActiveMetadata(	CFHD_DecoderRef decoderRef,
                        CFHD_MetadataRef metadataRef,
                        unsigned int tag,
                        CFHD_MetadataType type,
                        void *data,
                        unsigned int size)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (metadataRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    if ((tag == 0 && type != METADATATYPE_CINEFORM) || data == NULL || size == 0)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleMetadata *metadata = (CSampleMetadata *)metadataRef;
    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    //Hack, pass the decoders custom allocator on to CSampleMetadata
    metadata->SetAllocator(decoder->GetAllocator());

    //if(metadata->m_overrideSize == 0)
    {
        if (metadata->m_metadataTrack & METADATAFLAG_MODIFIED)
        {
            int data = 1;
            int typesizebytes = ('H' << 24) | 4;

            metadata->AddMetaData(TAG_FORCE_DATABASE, typesizebytes, (void *)&data);

            data = 0;
            metadata->AddMetaData(TAG_IGNORE_DATABASE, typesizebytes, (void *)&data);
        }
        else
        {
            int data = 1;
            int typesizebytes = ('H' << 24) | 4;

            metadata->AddMetaData(TAG_IGNORE_DATABASE, typesizebytes, (void *)&data);

            data = 0;
            metadata->AddMetaData(TAG_FORCE_DATABASE, typesizebytes, (void *)&data);
        }
    }

    unsigned int typesizebytes = 0;

    switch (type)
    {
        case METADATATYPE_STRING:
            typesizebytes = 'c' << 24;
            break;
        case METADATATYPE_UINT32:
            typesizebytes = 'L' << 24;
            break;
        case METADATATYPE_UINT16:
            typesizebytes = 'S' << 24;
            break;
        case METADATATYPE_UINT8:
            typesizebytes = 'B' << 24;
            break;
        case METADATATYPE_FLOAT:
            typesizebytes = 'f' << 24;
            break;
        case METADATATYPE_DOUBLE:
            typesizebytes = 'd' << 24;
            break;
        case METADATATYPE_GUID:
            typesizebytes = 'G' << 24;
            break;
        case METADATATYPE_XML:
            typesizebytes = 'x' << 24;
            break;
        case METADATATYPE_LONG_HEX:
            typesizebytes = 'H' << 24;
            break;
        case METADATATYPE_HIDDEN:
            typesizebytes = 'h' << 24;
            break;
        case METADATATYPE_TAG:
            typesizebytes = 'T' << 24;
            break;
        case METADATATYPE_UNKNOWN:
        default:
            break;
    }

    typesizebytes |= size;

    if (tag == TAG_CHANNELS_ACTIVE)
    {
        decoder->SetChannelsActive(*((uint32_t *)data));
    }
    if (tag == TAG_CHANNELS_MIX)
    {
        decoder->SetChannelMix(*((uint32_t *)data));
    }

    if (tag == TAG_LOOK_FILE)
    {
        uint32_t crc = 0;
        static char lastpath[260] = "";
        static char lastLUTfilename[40] = "";
        static uint32_t lastLUTcrc = 0;

        if (lastLUTcrc && 0 == strcmp(lastpath, (char *)data))
        {
            typesizebytes = ('c' << 24) | 39;
            metadata->AddMetaData(TAG_LOOK_FILE, typesizebytes, (void *)&lastLUTfilename[0]);
            typesizebytes = ('H' << 24) | 4;
            metadata->AddMetaData(TAG_LOOK_CRC, typesizebytes, (void *)&lastLUTcrc);
        }
        else
        {
            char drive[260];
            char dir[260];
            char fname[260];
            char ext[64];
            char filename[260];

#ifdef _WIN32
            strcpy_s(lastpath, sizeof(lastpath), (char *)data);
            _splitpath_s((char *)data, drive, sizeof(drive), dir, sizeof(dir), fname, sizeof(fname), ext, sizeof(ext));
            _makepath_s(filename, sizeof(filename), NULL, NULL, fname, ext);
#else
            strcpy(lastpath, (char *)data);
            _splitpath((char *)data, drive, dir, fname, ext);
            _makepath(filename, NULL, NULL, fname, ext);
#endif

            if (strlen(filename) < 40)
            {
                typesizebytes = ('c' << 24) | 39;
                metadata->AddMetaData(TAG_LOOK_FILE, typesizebytes, (void *)&filename[0]);

#ifdef _WIN32
                strcpy_s(lastLUTfilename, sizeof(lastLUTfilename), filename);
#else
                strcpy(lastLUTfilename, filename);
#endif

                if (crc)
                {
                    typesizebytes = ('H' << 24) | 4;
                    metadata->AddMetaData(TAG_LOOK_CRC, typesizebytes, (void *)&crc);
                    lastLUTcrc = crc;
                }
            }
        }
    }
    else if (type == METADATATYPE_CINEFORM)
    {
        uint32_t *ptr = (uint32_t *)data;
        while (size >= 12 && size < 4096)
        {

            uint32_t tag = *ptr++;
            size -= 4;
            uint32_t typesizebytes = *ptr++;
            size -= 4;
            uint32_t *newdata = ptr;
            uint32_t tagsize = typesizebytes & 0xffffff;

            metadata->AddMetaData(tag, typesizebytes, newdata);

            tagsize += 3;
            tagsize &= ~3;
            size -= tagsize;
            ptr += tagsize / 4;
        }
    }
    else if (tag == TAG_UNIQUE_FRAMENUM)
    {
        metadata->m_currentUFRM = *(uint32_t *)data;
    }
    else
    {
        if (metadata->m_metadataTrack & METADATAFLAG_LEFT_EYE)
        {
            metadata->AddMetaDataChannel(tag, typesizebytes, data, 1);
        }
        else if (metadata->m_metadataTrack & METADATAFLAG_RIGHT_EYE)
        {
            metadata->AddMetaDataChannel(tag, typesizebytes, data, 2);
        }
        else
        {
            metadata->AddMetaData(tag, typesizebytes, data);
        }
    }

    return errorCode;
}

CFHDDECODER_API CFHD_Error
CFHD_ClearActiveMetadata(CFHD_DecoderRef decoderRef,
                         CFHD_MetadataRef metadataRef)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (metadataRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }
    if (decoderRef == NULL)
    {
        return CFHD_ERROR_INVALID_ARGUMENT;
    }

    CSampleMetadata *metadata = (CSampleMetadata *)metadataRef;
    CSampleDecoder *decoder = (CSampleDecoder *)decoderRef;

    metadata->FreeDatabase();

    return errorCode;
}

CFHDDECODER_API CFHD_Error
CFHD_GetThumbnail(CFHD_DecoderRef decoderRef,
                  void *samplePtr,
                  size_t sampleSize,
                  void *outputBuffer,
                  size_t outputBufferSize,
                  uint32_t flags,
                  size_t *retWidth = NULL,
                  size_t *retHeight = NULL,
                  size_t *retSize = NULL)
{
    CFHD_Error errorCode = CFHD_ERROR_OKAY;

    // Check the input arguments
    if (decoderRef == NULL)
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

    CSampleDecoder *decoder = reinterpret_cast<CSampleDecoder *>(decoderRef);

    // Have the thumbnail flags been set?
    if (flags == THUMBNAIL_FLAGS_NONE)
    {
        // Use the default thumbnail flags
        flags = THUMBNAIL_FLAGS_DEFAULT;
    }

    errorCode = decoder->GetThumbnail(samplePtr,
                                      sampleSize,
                                      outputBuffer,
                                      outputBufferSize,
                                      flags,
                                      retWidth,
                                      retHeight,
                                      retSize);

    return errorCode;
}

#ifdef __cplusplus
}
#endif
