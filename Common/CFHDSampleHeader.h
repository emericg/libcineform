/*!
 * @file CFHDSampleHeader.h
 * @brief Utilities for parsing the compressed sample header for a CineForm compressed frame.
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

#ifndef CFHD_SAMPLE_HEADER_H
#define CFHD_SAMPLE_HEADER_H

/*!
 * \brief Information obtained by parsing the sample header
 * \todo Add more fields from the sample header to this class as private data
 *       and define public get and set methods to access the data.
 */
class CFHD_SampleHeader
{
    CFHD_EncodedFormat m_encodedFormat = CFHD_ENCODED_FORMAT_YUV_422;
    CFHD_FieldType m_fieldType = CFHD_FIELD_TYPE_UNKNOWN;
    int m_width = 0;
    int m_height = 0;

public:
    CFHD_SampleHeader()
    {
        //
    }

    CFHD_Error SetEncodedFormat(CFHD_EncodedFormat encodedFormat)
    {
        m_encodedFormat = encodedFormat;
        return CFHD_ERROR_OKAY;
    }

    CFHD_Error GetEncodedFormat(CFHD_EncodedFormat *encodedFormatOut)
    {
        *encodedFormatOut = m_encodedFormat;
        return CFHD_ERROR_OKAY;
    }

    CFHD_Error SetFieldType(CFHD_FieldType fieldType)
    {
        m_fieldType = fieldType;
        return CFHD_ERROR_OKAY;
    }

    CFHD_Error GetFieldType(CFHD_FieldType *fieldTypeOut)
    {
        *fieldTypeOut = m_fieldType;
        return CFHD_ERROR_OKAY;
    }

    CFHD_Error SetFrameSize(int width, int height)
    {
        m_width = width;
        m_height = height;
        return CFHD_ERROR_OKAY;
    }

    CFHD_Error GetFrameSize(int *widthOut, int *heightOut)
    {
        *widthOut = m_width;
        *heightOut = m_height;
        return CFHD_ERROR_OKAY;
    }
};

#endif // CFHD_SAMPLE_HEADER_H
