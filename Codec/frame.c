/*! @file frame.c

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

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <emmintrin.h>		// SSE2 intrinsics
#include "config.h"
#include "frame.h"
#include "wavelet.h"
#include "color.h"
#include "timing.h"
#include "convert.h"

#include "decoder.h"
#include "swap.h"
#include "RGB2YUV.h"

#include <stdlib.h>
#include <stdio.h>

#define DEBUG  (1 && _DEBUG)
#define TIMING (1 && _TIMING)
#define XMMOPT (1 && _XMMOPT)

// Performance measurements
#if TIMING
extern TIMER tk_convert;				// Time for image format conversion
extern COUNTER alloc_frame_count;		// Number of frames allocated
#endif

#ifndef DEBUG
#define DEBUG  (1 && _DEBUG)
#endif

#if _ENABLE_GAMMA_CORRECTION		// Color conversion macros
#include "gamma_table.inc"
#endif

#define COLOR_CONVERSION_16BITS		1		// Use 16-bit fixed point for color conversion
#define	INTERPOLATE_CHROMA			0		// This caused shear in multi-generation tests

#define YU16_MAX	65535					// Maximum for 16 bit pixels
#define YU10_MAX	 1023					// maximum for 10 bit pixels


#define SATURATE_10U(x) _saturate10u(x)
#define SATURATE_12U(x) _saturate12u(x)


typedef union
{
    unsigned long long u64[2];
    long long s64[2];
    unsigned int       u32[4];
    int       s32[4];
    unsigned short     u16[8];
    short     s16[8];
    unsigned char      u8[16];
    char      s8[16];
    __m128i            m128;
} m128i;


inline static int _saturate10u(int x)
{
    const int upper_limit = 1023;

    if (x < 0) x = 0;
    else if (x > upper_limit) x = upper_limit;

    return x;
}

inline static int _saturate12u(int x)
{
    const int upper_limit = 4095;

    if (x < 0) x = 0;
    else if (x > upper_limit) x = upper_limit;

    return x;
}

#if _ALLOCATOR
FRAME *CreateFrame(ALLOCATOR *allocator, int width, int height, int display_height, int format)
#else
FRAME *CreateFrame(int width, int height, int display_height, int format)
#endif
{
    int chroma_width, chroma_height;

#if _ALLOCATOR
    FRAME *frame = (FRAME *)Alloc(allocator, sizeof(FRAME));
#else
    FRAME *frame = (FRAME *)MEMORY_ALLOC(sizeof(FRAME));
#endif
    if (frame == NULL)
    {
#if (DEBUG && _WIN32)
        OutputDebugString("sizeof(FRAME)");
#endif
        return NULL;
    }

    // Clear all fields in the frame
    memset(frame, 0, sizeof(FRAME));

    if (format == FRAME_FORMAT_GRAY)
    {
        frame->num_channels = 1;
#if _ALLOCATOR
        frame->channel[0] = CreateImage(allocator, width, height);
#else
        frame->channel[0] = CreateImage(width, height);
#endif
    }
    else if (format == FRAME_FORMAT_YUV)
    {
        // Currently only handle color frames in YUV format
        assert(format == FRAME_FORMAT_YUV);

        frame->num_channels = 3;
#if _ALLOCATOR
        frame->channel[0] = CreateImage(allocator, width, height);
#else
        frame->channel[0] = CreateImage(width, height);
#endif
#if _YUV422
        chroma_width = width / 2;
        chroma_height = height;
#if _ALLOCATOR
        frame->channel[1] = CreateImage(allocator, chroma_width, chroma_height);
        frame->channel[2] = CreateImage(allocator, chroma_width, chroma_height);
#else
        frame->channel[1] = CreateImage(chroma_width, chroma_height);
        frame->channel[2] = CreateImage(chroma_width, chroma_height);
#endif
#else
#if _ALLOCATOR
        frame->channel[1] = CreateImage(allocator, width, height);
        frame->channel[2] = CreateImage(allocator, width, height);
#else
        frame->channel[1] = CreateImage(width, height);
        frame->channel[2] = CreateImage(width, height);
#endif
#endif
    }
    else if (format == FRAME_FORMAT_RGBA)
    {
        frame->num_channels = 4;
#if _ALLOCATOR
        frame->channel[0] = CreateImage(allocator, width, height);
        frame->channel[1] = CreateImage(allocator, width, height);
        frame->channel[2] = CreateImage(allocator, width, height);
        frame->channel[3] = CreateImage(allocator, width, height);
#else
        frame->channel[0] = CreateImage(width, height);
        frame->channel[1] = CreateImage(width, height);
        frame->channel[2] = CreateImage(width, height);
        frame->channel[3] = CreateImage(width, height);
#endif
    }
    else if (format == FRAME_FORMAT_RGB)
    {
        frame->num_channels = 3;
#if _ALLOCATOR
        frame->channel[0] = CreateImage(allocator, width, height);
        frame->channel[1] = CreateImage(allocator, width, height);
        frame->channel[2] = CreateImage(allocator, width, height);
#else
        frame->channel[0] = CreateImage(width, height);
        frame->channel[1] = CreateImage(width, height);
        frame->channel[2] = CreateImage(width, height);
#endif
    }

    // Save the frame dimensions and format
    frame->width = width;
    frame->height = height;
    frame->display_height = display_height;
    frame->format = format;

    // Assume that this is not a key frame
    frame->iskey = false;

#if TIMING
    alloc_frame_count++;
#endif

    return frame;
}

#if _ALLOCATOR
FRAME *ReallocFrame(ALLOCATOR *allocator, FRAME *frame, int width, int height, int display_height, int format)
#else
FRAME *ReallocFrame(FRAME *frame, int width, int height, int display_height, int format)
#endif
{
    if (frame != NULL)
    {
        if (frame->width == width &&
                frame->height == height &&
                frame->format == format &&
                frame->display_height == display_height)
        {
            return frame;
        }
#if _ALLOCATOR
        DeleteFrame(allocator, frame);
#else
        DeleteFrame(frame);
#endif
    }

#if _ALLOCATOR
    return CreateFrame(allocator, width, height, display_height, format);
#else
    return CreateFrame(width, height, display_height, format);
#endif
}

// Set the frame dimensions without allocating memory for the planes
void SetFrameDimensions(FRAME *frame, int width, int height, int display_height, int format)
{
    //int chroma_width;
    //int chroma_height;

    // Clear all fields in the frame
    memset(frame, 0, sizeof(FRAME));

    switch (format)
    {
        case FRAME_FORMAT_GRAY:
            frame->num_channels = 1;
            break;

        case FRAME_FORMAT_YUV:
            frame->num_channels = 3;
            break;

        case FRAME_FORMAT_RGBA:
            frame->num_channels = 4;
            break;

        case FRAME_FORMAT_RGB:
            frame->num_channels = 3;
            break;
    }

    // Save the frame dimensions and format
    frame->width = width;
    frame->height = height;
    frame->display_height = display_height;
    frame->format = format;

    // Assume that this is not a key frame
    frame->iskey = false;
}

// Create a frame with the same dimensions and format as another frame
#if _ALLOCATOR
FRAME *CreateFrameFromFrame(ALLOCATOR *allocator, FRAME *frame)
#else
FRAME *CreateFrameFromFrame(FRAME *frame)
#endif
{
    IMAGE *image = frame->channel[0];
    int width = image->width;
    int height = image->height;
    int display_height = frame->display_height;

    // Note: This code should be extended to duplicate the bands

#if _ALLOCATOR
    FRAME *new_frame = CreateFrame(allocator, width, height, display_height, frame->format);
#else
    FRAME *new_frame = CreateFrame(width, height, display_height, frame->format);
#endif

    return new_frame;
}

#if 0
// Create an image data structure from planar video frame data
FRAME *CreateFrameFromPlanes(ALLOCATOR *allocator, LPBYTE data, int width, int height, int pitch, int format)
{
    // To be written
    assert(0);

    return NULL;
}
#endif

void ConvertPackedToFrame(uint8_t *data, int width, int height, int pitch, FRAME *frame)
{
    IMAGE *image = frame->channel[0];
    uint8_t *rowptr = data;
    PIXEL *outptr = image->band[0];
    int data_pitch = pitch;
    int image_pitch = image->pitch / sizeof(PIXEL);
    int row, column;

    for (row = 0; row < height; row++)
    {
        for (column = 0; column < width; column++)
        {
            PIXEL value = rowptr[2 * column];
            outptr[column] = SATURATE(value);
        }
        rowptr += data_pitch;
        outptr += image_pitch;
    }
}


// Faster version of ConvertRGBToFrame8uNoIPP using MMX intrinsics
void ConvertRGB32to10bitYUVFrame(uint8_t *rgb, int pitch, FRAME *frame,  uint8_t *scratch, int scratchsize, int color_space,
                                 int precision, int srcHasAlpha, int rgbaswap)
{
    ROI roi;

    int display_height, height, width;

    int shift = 6; // using 10-bit math

    assert(MIN_DECODED_COLOR_SPACE <= color_space && color_space <= MAX_DECODED_COLOR_SPACE);

    {
        PIXEL8U *RGB_row;
        unsigned short *color_plane[3];
        int color_pitch[3];
        PIXEL8U *Y_row, *U_row, *V_row;
        PIXEL *Y_row16, *U_row16, *V_row16;
        int Y_pitch, U_pitch, V_pitch;
        int row;
        int i;
        //int precisionshift = 10 - precision;
        unsigned short *scanline, *scanline2;

        // The frame format should be three channels of YUV (4:2:2 format)
        assert(frame->num_channels == 3);
        assert(frame->format == FRAME_FORMAT_YUV);
        display_height = frame->display_height;
        height = frame->height;
        width = frame->width;

        assert(scratch);
        assert(scratchsize > width * 12);

        scanline = (unsigned short *)scratch;
        scanline2 = scanline + width * 3;

        // Get pointers to the image planes and set the pitch for each plane
        for (i = 0; i < 3; i++)
        {
            IMAGE *image = frame->channel[i];

            // Set the pointer to the individual planes and pitch for each channel
            color_plane[i] = (PIXEL16U *)image->band[0];
            color_pitch[i] = image->pitch;

            // The first channel establishes the processing dimensions
            if (i == 0)
            {
                roi.width = image->width;
                roi.height = image->height;
            }
        }

        // Input RGB image is upside down so reverse it
        // by starting from the end of the image and going back
        RGB_row = &rgb[0];
        RGB_row += (display_height - 1) * pitch;
        pitch = -pitch;

        //U and V are swapped
        {
            PIXEL16U *t = color_plane[1];
            color_plane[1] = color_plane[2];
            color_plane[2] = t;
        }


        Y_row = (PIXEL8U *)color_plane[0];
        Y_pitch = color_pitch[0];
        U_row = (PIXEL8U *)color_plane[1];
        U_pitch = color_pitch[1];
        V_row = (PIXEL8U *)color_plane[2];
        V_pitch = color_pitch[2];

        for (row = 0; row < display_height; row++)
        {
            //int column = 0;

            if (srcHasAlpha)
            {
                if (rgbaswap)
                    ChunkyARGB8toPlanarRGB16((unsigned char *)RGB_row, scanline, width);
                else
                    ChunkyBGRA8toPlanarRGB16((unsigned char *)RGB_row, scanline, width);
            }
            else
                ChunkyBGR8toPlanarRGB16((unsigned char *)RGB_row, scanline, width);
            PlanarRGB16toPlanarYUV16(scanline, scanline2, width, color_space);
            PlanarYUV16toChannelYUYV16(scanline2, (unsigned short **)color_plane, width, color_space, shift);

            // Advance the RGB pointers
            RGB_row += pitch;

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;

            color_plane[0] = (PIXEL16U *)Y_row;
            color_plane[1] = (PIXEL16U *)U_row;
            color_plane[2] = (PIXEL16U *)V_row;
        }

        for (; row < height; row++)
        {
            int column = 0;

#if (1 && XMMOPT)
            int column_step = 16;
            int post_column = roi.width - (roi.width % column_step);

            __m128i *Y_ptr = (__m128i *)Y_row;
            __m128i *U_ptr = (__m128i *)U_row;
            __m128i *V_ptr = (__m128i *)V_row;
            __m128i Y = _mm_set1_epi16(64);
            __m128i UV = _mm_set1_epi16(512);
            // Convert to YUYV in sets of 2 pixels

            for (; column < post_column; column += column_step)
            {
                *Y_ptr++ = Y;
                *Y_ptr++ = Y;
                *U_ptr++ = UV;
                *V_ptr++ = UV;
            }

#endif
            // Process the rest of the column

            Y_row16 = (PIXEL *)Y_row;
            U_row16 = (PIXEL *)U_row;
            V_row16 = (PIXEL *)V_row;
            for (; column < roi.width; column += 2)
            {
                int Y = 64, UV = 512;

                Y_row16[column] = Y;

                U_row16[column / 2] = UV;
                V_row16[column / 2] = UV;
                Y_row16[column + 1] = Y;
            }

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;
        }


        // Set the image parameters for each channel
        for (i = 0; i < 3; i++)
        {
            IMAGE *image = frame->channel[i];
            int band;

            // Set the image scale
            for (band = 0; band < IMAGE_NUM_BANDS; band++)
                image->scale[band] = 1;

            // Set the pixel type
            image->pixel_type[0] = PIXEL_TYPE_16S;
        }

#if _MONOCHROME
        // Continue with the gray channel only (useful for debugging)
        frame->num_channels = 1;
        frame->format = FRAME_FORMAT_GRAY;
#endif
    }
}


// Faster version of ConvertRGBToFrame8uNoIPP using MMX intrinsics
void ConvertNV12to10bitYUVFrame(uint8_t *nv12, int pitch, FRAME *frame,  uint8_t *scratch, int scratchsize,
                                int color_space, int precision, int progressive)
{
    ROI roi;

    int display_height, height, width;

    //int shift = 6; // using 10-bit math

    assert(MIN_DECODED_COLOR_SPACE <= color_space && color_space <= MAX_DECODED_COLOR_SPACE);

    {
        unsigned short *color_plane[3];
        int color_pitch[3];
        PIXEL8U *Y_row, *U_row, *V_row;
        PIXEL *Y_row16, *U_row16, *V_row16;
        int Y_pitch, U_pitch, V_pitch;
        int row;
        int i;
        //int precisionshift = 10 - precision;
        unsigned short *scanline, *scanline2;
        uint8_t *nv12Yline;
        uint8_t *nv12UVline, *nv12UVnext;

        // The frame format should be three channels of YUV (4:2:2 format)
        assert(frame->num_channels == 3);
        assert(frame->format == FRAME_FORMAT_YUV);
        display_height = frame->display_height;
        height = frame->height;
        width = frame->width;

        assert(scratch);
        assert(scratchsize > width * 12);

        scanline = (unsigned short *)scratch;
        scanline2 = scanline + width * 3;

        // Get pointers to the image planes and set the pitch for each plane
        for (i = 0; i < 3; i++)
        {
            IMAGE *image = frame->channel[i];

            // Set the pointer to the individual planes and pitch for each channel
            color_plane[i] = (PIXEL16U *)image->band[0];
            color_pitch[i] = image->pitch;

            // The first channel establishes the processing dimensions
            if (i == 0)
            {
                roi.width = image->width;
                roi.height = image->height;
            }
        }

        Y_row = (PIXEL8U *)color_plane[0];
        Y_pitch = color_pitch[0];
        U_row = (PIXEL8U *)color_plane[1];
        U_pitch = color_pitch[1];
        V_row = (PIXEL8U *)color_plane[2];
        V_pitch = color_pitch[2];


        if (progressive)
        {
            nv12Yline   = nv12;
            nv12UVline  = nv12Yline + width * display_height;
            nv12UVnext  = nv12UVline + width;

            for (row = 0; row < display_height; row++)
            {
                int column = 0;

                Y_row16 = (PIXEL *)Y_row;
                U_row16 = (PIXEL *)U_row;
                V_row16 = (PIXEL *)V_row;

                if (row == 0 || row >= display_height - 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = nv12UVline[column] << 2;
                        U_row16[column / 2] = nv12UVline[column + 1] << 2;
                    }
                    nv12Yline += width;
                }
                else if (row & 1)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = (nv12UVline[column] * 3 + nv12UVnext[column]);
                        U_row16[column / 2] = (nv12UVline[column + 1] * 3 + nv12UVnext[column + 1]);
                    }
                    nv12Yline += width;
                }
                else
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = (nv12UVline[column] + nv12UVnext[column] * 3);
                        U_row16[column / 2] = (nv12UVline[column + 1] + nv12UVnext[column + 1] * 3);
                    }
                    nv12Yline += width;
                    nv12UVline = nv12UVnext;
                    nv12UVnext = nv12UVline + width;
                }

                // Advance the YUV pointers
                Y_row += Y_pitch;
                U_row += U_pitch;
                V_row += V_pitch;
            }
        }
        else
        {
            uint8_t *nv12UVline2, *nv12UVnext2;

            nv12Yline   = nv12;
            nv12UVline  = nv12Yline + width * display_height;
            nv12UVnext  = nv12UVline + width * 2;

            nv12UVline2 = nv12UVline + width;
            nv12UVnext2 = nv12UVline2 + width * 2;

            //Top field
            for (row = 0; row < display_height; row += 2)
            {
                int column = 0;

                Y_row16 = (PIXEL *)Y_row;
                U_row16 = (PIXEL *)U_row;
                V_row16 = (PIXEL *)V_row;

                //Top field
                if (row == 0 || row >= display_height - 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = nv12UVline[column] << 2;
                        U_row16[column / 2] = nv12UVline[column + 1] << 2;
                    }
                    nv12Yline += width;
                }
                else if (row & 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = (nv12UVline[column] * 5 + nv12UVnext[column] * 3) >> 1;
                        U_row16[column / 2] = (nv12UVline[column + 1] * 5 + nv12UVnext[column + 1] * 3) >> 1;
                    }
                    nv12Yline += width;
                }
                else
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = (nv12UVline[column] + nv12UVnext[column] * 7) >> 1;
                        U_row16[column / 2] = (nv12UVline[column + 1] + nv12UVnext[column + 1] * 7) >> 1;
                    }
                    nv12Yline += width;
                    nv12UVline = nv12UVnext;
                    nv12UVnext = nv12UVline + width * 2;
                }

                // Advance the YUV pointers
                Y_row += Y_pitch;
                U_row += U_pitch;
                V_row += V_pitch;
                Y_row16 = (PIXEL *)Y_row;
                U_row16 = (PIXEL *)U_row;
                V_row16 = (PIXEL *)V_row;



                //Bottom field
                if (row <= 2 || row >= display_height - 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = nv12UVline2[column] << 2;
                        U_row16[column / 2] = nv12UVline2[column + 1] << 2;
                    }
                    nv12Yline += width;
                }
                else if (row & 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = (nv12UVline2[column] + nv12UVnext2[column] * 7) >> 1;
                        U_row16[column / 2] = (nv12UVline2[column + 1] + nv12UVnext2[column + 1] * 7) >> 1;
                    }
                    nv12Yline += width;
                    nv12UVline2 = nv12UVnext2;
                    nv12UVnext2 = nv12UVline2 + width * 2;
                }
                else
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        V_row16[column / 2] = (nv12UVline2[column] * 3 + nv12UVnext2[column] * 5) >> 1;
                        U_row16[column / 2] = (nv12UVline2[column + 1] * 3 + nv12UVnext2[column + 1] * 5) >> 1;
                    }
                    nv12Yline += width;
                }

                // Advance the YUV pointers
                Y_row += Y_pitch;
                U_row += U_pitch;
                V_row += V_pitch;
            }
        }

        for (; row < height; row++)
        {
            int column = 0;

            Y_row16 = (PIXEL *)Y_row;
            U_row16 = (PIXEL *)U_row;
            V_row16 = (PIXEL *)V_row;
            for (; column < roi.width; column += 2)
            {
                int Y = 64, UV = 512;

                Y_row16[column] = Y;

                U_row16[column / 2] = UV;
                V_row16[column / 2] = UV;
                Y_row16[column + 1] = Y;
            }

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;
        }


        // Set the image parameters for each channel
        for (i = 0; i < 3; i++)
        {
            IMAGE *image = frame->channel[i];
            int band;

            // Set the image scale
            for (band = 0; band < IMAGE_NUM_BANDS; band++)
                image->scale[band] = 1;

            // Set the pixel type
            image->pixel_type[0] = PIXEL_TYPE_16S;
        }

#if _MONOCHROME
        // Continue with the gray channel only (useful for debugging)
        frame->num_channels = 1;
        frame->format = FRAME_FORMAT_GRAY;
#endif
    }
}


void ConvertYV12to10bitYUVFrame(uint8_t *nv12, int pitch, FRAME *frame,  uint8_t *scratch, int scratchsize,
                                int color_space, int precision, int progressive)
{
    ROI roi;

    int display_height, height, width;

    //int shift = 6; // using 10-bit math

    assert(MIN_DECODED_COLOR_SPACE <= color_space && color_space <= MAX_DECODED_COLOR_SPACE);

    {
        unsigned short *color_plane[3];
        int color_pitch[3];
        PIXEL8U *Y_row, *U_row, *V_row;
        PIXEL *Y_row16, *U_row16, *V_row16;
        int Y_pitch, U_pitch, V_pitch;
        int row;
        int i;
        //int precisionshift = 10 - precision;
        unsigned short *scanline, *scanline2;
        uint8_t *nv12Yline;
        uint8_t *nv12Uline, *nv12Unext;
        uint8_t *nv12Vline, *nv12Vnext;

        // The frame format should be three channels of YUV (4:2:2 format)
        assert(frame->num_channels == 3);
        assert(frame->format == FRAME_FORMAT_YUV);
        display_height = frame->display_height;
        height = frame->height;
        width = frame->width;

        assert(scratch);
        assert(scratchsize > width * 12);

        scanline = (unsigned short *)scratch;
        scanline2 = scanline + width * 3;

        // Get pointers to the image planes and set the pitch for each plane
        for (i = 0; i < 3; i++)
        {
            IMAGE *image = frame->channel[i];

            // Set the pointer to the individual planes and pitch for each channel
            color_plane[i] = (PIXEL16U *)image->band[0];
            color_pitch[i] = image->pitch;

            // The first channel establishes the processing dimensions
            if (i == 0)
            {
                roi.width = image->width;
                roi.height = image->height;
            }
        }

        Y_row = (PIXEL8U *)color_plane[0];
        Y_pitch = color_pitch[0];
        U_row = (PIXEL8U *)color_plane[1];
        U_pitch = color_pitch[1];
        V_row = (PIXEL8U *)color_plane[2];
        V_pitch = color_pitch[2];


        if (progressive)
        {
            nv12Yline   = nv12;
            nv12Uline  =  nv12Yline + width * display_height;
            nv12Vline  =  nv12Uline + (width / 2) * (display_height / 2);
            nv12Unext  = nv12Uline + width / 2;
            nv12Vnext  = nv12Vline + width / 2;

            for (row = 0; row < display_height; row++)
            {
                int column = 0;

                Y_row16 = (PIXEL *)Y_row;
                U_row16 = (PIXEL *)U_row;
                V_row16 = (PIXEL *)V_row;

                if (row == 0 || row == display_height - 1)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = nv12Uline[column / 2] << 2;
                        V_row16[column / 2] = nv12Vline[column / 2] << 2;
                    }
                    nv12Yline += width;
                }
                else if (row & 1)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = (nv12Uline[column / 2] * 3 + nv12Unext[column / 2]);
                        V_row16[column / 2] = (nv12Vline[column / 2] * 3 + nv12Vnext[column / 2]);
                    }
                    nv12Yline += width;
                }
                else
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = (nv12Uline[column / 2] + nv12Unext[column / 2] * 3);
                        V_row16[column / 2] = (nv12Vline[column / 2] + nv12Vnext[column / 2] * 3);
                    }
                    nv12Yline += width;
                    nv12Uline = nv12Unext;
                    nv12Vline = nv12Vnext;
                    nv12Unext = nv12Uline + width / 2;
                    nv12Vnext = nv12Vline + width / 2;
                }

                // Advance the YUV pointers
                Y_row += Y_pitch;
                U_row += U_pitch;
                V_row += V_pitch;
            }
        }
        else
        {
            uint8_t *nv12Uline2, *nv12Unext2;
            uint8_t *nv12Vline2, *nv12Vnext2;

            nv12Yline  = nv12;
            nv12Uline  = nv12Yline + width * display_height;
            nv12Vline  =  nv12Uline + (width / 2) * (display_height / 2);
            nv12Unext  = nv12Uline + width;
            nv12Vnext  = nv12Vline + width;

            nv12Uline2 = nv12Uline + width / 2;
            nv12Unext2 = nv12Uline2 + width;
            nv12Vline2 = nv12Vline + width / 2;
            nv12Vnext2 = nv12Vline2 + width;

            //Top field
            for (row = 0; row < display_height; row += 2)
            {
                int column = 0;

                Y_row16 = (PIXEL *)Y_row;
                U_row16 = (PIXEL *)U_row;
                V_row16 = (PIXEL *)V_row;

                //Top field
                if (row == 0)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = nv12Uline[column / 2] << 2;
                        V_row16[column / 2] = nv12Vline[column / 2] << 2;
                    }
                    nv12Yline += width;
                }
                else if (row & 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = (nv12Uline[column / 2] * 5 + nv12Unext[column / 2] * 3) >> 1;
                        V_row16[column / 2] = (nv12Vline[column / 2] * 5 + nv12Vnext[column / 2] * 3) >> 1;
                    }
                    nv12Yline += width;
                }
                else
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = (nv12Uline[column / 2] + nv12Unext[column / 2] * 7) >> 1;
                        V_row16[column / 2] = (nv12Vline[column / 2] + nv12Vnext[column / 2] * 7) >> 1;
                    }
                    nv12Yline += width;
                    nv12Uline = nv12Unext;
                    nv12Vline = nv12Vnext;
                    nv12Unext = nv12Uline + width;
                    nv12Vnext = nv12Vline + width;
                }

                // Advance the YUV pointers
                Y_row += Y_pitch;
                U_row += U_pitch;
                V_row += V_pitch;
                Y_row16 = (PIXEL *)Y_row;
                U_row16 = (PIXEL *)U_row;
                V_row16 = (PIXEL *)V_row;



                //Bottom field
                if (row <= 2 || row >= display_height - 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = nv12Uline2[column / 2] << 2;
                        V_row16[column / 2] = nv12Vline2[column / 2] << 2;
                    }
                    nv12Yline += width;
                }
                else if (row & 2)
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = (nv12Uline2[column / 2] + nv12Unext2[column / 2] * 7) >> 1;
                        V_row16[column / 2] = (nv12Vline2[column / 2] + nv12Vnext2[column / 2] * 7) >> 1;
                    }
                    nv12Yline += width;
                    nv12Uline2 = nv12Unext2;
                    nv12Vline2 = nv12Vnext2;
                    nv12Unext2 = nv12Uline2 + width;
                    nv12Vnext2 = nv12Vline2 + width;
                }
                else
                {
                    for (column = 0; column < roi.width; column += 2)
                    {
                        Y_row16[column] = nv12Yline[column] << 2;
                        Y_row16[column + 1] = nv12Yline[column + 1] << 2;
                        U_row16[column / 2] = (nv12Uline2[column / 2] * 3 + nv12Unext2[column / 2] * 5) >> 1;
                        V_row16[column / 2] = (nv12Vline2[column / 2] * 3 + nv12Vnext2[column / 2] * 5) >> 1;
                    }
                    nv12Yline += width;
                }

                // Advance the YUV pointers
                Y_row += Y_pitch;
                U_row += U_pitch;
                V_row += V_pitch;
            }
        }

        for (; row < height; row++)
        {
            int column = 0;

            Y_row16 = (PIXEL *)Y_row;
            U_row16 = (PIXEL *)U_row;
            V_row16 = (PIXEL *)V_row;
            for (; column < roi.width; column += 2)
            {
                int Y = 64, UV = 512;

                Y_row16[column] = Y;

                U_row16[column / 2] = UV;
                V_row16[column / 2] = UV;
                Y_row16[column + 1] = Y;
            }

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;
        }


        // Set the image parameters for each channel
        for (i = 0; i < 3; i++)
        {
            IMAGE *image = frame->channel[i];
            int band;

            // Set the image scale
            for (band = 0; band < IMAGE_NUM_BANDS; band++)
                image->scale[band] = 1;

            // Set the pixel type
            image->pixel_type[0] = PIXEL_TYPE_16S;
        }

#if _MONOCHROME
        // Continue with the gray channel only (useful for debugging)
        frame->num_channels = 1;
        frame->format = FRAME_FORMAT_GRAY;
#endif
    }
}


void ConvertYUYVToFrame16s(uint8_t *yuv, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *y_image;
    IMAGE *u_image;
    IMAGE *v_image;
    PIXEL8U *yuyv_row_ptr;
    PIXEL16S *y_row_ptr;
    PIXEL16S *u_row_ptr;
    PIXEL16S *v_row_ptr;
    int yuyv_pitch;
    int y_pitch;
    int u_pitch;
    int v_pitch;
    int width;
    int height;
    int row;
    int column;
    int i;
    int display_height;

    // Process sixteen luma values per loop iteration
    const int column_step = 16;

    // Column at which post processing must begin
    int post_column;

    if (frame == NULL) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);

    y_image = frame->channel[0];					// Get the individual planes
    u_image = frame->channel[1];
    v_image = frame->channel[2];

    yuyv_row_ptr = yuv;								// Pointers to the rows
    y_row_ptr = (PIXEL16S *)(y_image->band[0]);
    u_row_ptr = (PIXEL16S *)(u_image->band[0]);
    v_row_ptr = (PIXEL16S *)(v_image->band[0]);

    yuyv_pitch = pitch / sizeof(PIXEL8U);				// Convert pitch from bytes to pixels
    y_pitch = y_image->pitch / sizeof(PIXEL16S);
    u_pitch = u_image->pitch / sizeof(PIXEL16S);
    v_pitch = v_image->pitch / sizeof(PIXEL16S);

    width = y_image->width;							// Dimensions of the luma image
    height = y_image->height;
    display_height = frame->display_height;

    post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(yuyv_pitch > 0);

    for (row = 0; row < display_height; row++)
    {
        // Begin processing at the leftmost column
        column = 0;

        // Process the rest of the column
        for (; column < width; column += 2)
        {
            int index = 2 * column;
            int c0 = column;
            int c1 = column + 1;
            int c2 = column / 2;

            // Unpack two luminance values and two chroma (which are reversed)
            PIXEL8U y1 = yuyv_row_ptr[index++];
            PIXEL8U v  = yuyv_row_ptr[index++];
            PIXEL8U y2 = yuyv_row_ptr[index++];
            PIXEL8U u  = yuyv_row_ptr[index++];

            // Output the luminance and chrominance values to separate planes
            y_row_ptr[c0] = y1;
            y_row_ptr[c1] = y2;
            u_row_ptr[c2] = u;
            v_row_ptr[c2] = v;
        }

        // Should have exited the loop just after the last column
        assert(column == width);

        // Advance to the next rows in the input and output images
        yuyv_row_ptr += yuyv_pitch;
        y_row_ptr += y_pitch;
        u_row_ptr += u_pitch;
        v_row_ptr += v_pitch;
    }

    // Set the image parameters for each channel
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}


//#if BUILD_PROSPECT
// Convert the packed 10-bit YUV 4:2:2 to planes of 8-bit YUV
#if DANREMOVE
void ConvertV210ToFrame8u(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *y_image;
    IMAGE *u_image;
    IMAGE *v_image;
    uint32_t *v210_row_ptr;
    PIXEL8U *y_row_ptr;
    PIXEL8U *u_row_ptr;
    PIXEL8U *v_row_ptr;
    int v210_pitch;
    int y_pitch;
    int u_pitch;
    int v_pitch;
    int width;
    int height;
    int row;
    int i;
    int display_height;

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);

    y_image = frame->channel[0];					// Get the individual planes
    u_image = frame->channel[1];
    v_image = frame->channel[2];

    v210_row_ptr = (uint32_t *)data;					// Pointers to the rows
    y_row_ptr = (PIXEL8U *)(y_image->band[0]);
    u_row_ptr = (PIXEL8U *)(u_image->band[0]);
    v_row_ptr = (PIXEL8U *)(v_image->band[0]);

    v210_pitch = pitch / sizeof(uint32_t);				// Convert pitch from bytes to pixels
    y_pitch = y_image->pitch / sizeof(PIXEL8U);
    u_pitch = u_image->pitch / sizeof(PIXEL8U);
    v_pitch = v_image->pitch / sizeof(PIXEL8U);

    width = y_image->width;							// Dimensions of the luma image
    height = y_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(v210_pitch > 0);

    for (row = 0; row < display_height; row++)
    {
#if 0
        // Start processing the row at the first column
        int column = 0;

#if (0 && XMMOPT)

        /***** Add code for the fast loop here *****/


        assert(column == post_column);
#endif

        // Process the rest of the row
        for (; column < width; column += 2)
        {
        }

        // Should have exited the loop just after the last column
        assert(column == width);
#else
        // Unpack the row of 10-bit pixels
        ConvertV210RowToYUV((uint8_t *)v210_row_ptr, (PIXEL *)buffer, width);

        // Convert the unpacked pixels to 8-bit planes
        ConvertYUVPacked16sRowToPlanar8u((PIXEL *)buffer, width, y_row_ptr, u_row_ptr, v_row_ptr);
#endif
        // Advance to the next rows in the input and output images
        v210_row_ptr += v210_pitch;
        y_row_ptr += y_pitch;
        u_row_ptr += u_pitch;
        v_row_ptr += v_pitch;
    }

    //_mm_empty();		// Clear the mmx register state

    // Set the image parameters for each channel
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_8U;
    }
}
#endif
//#endif


//#if BUILD_PROSPECT
// Convert the packed 10-bit YUV 4:2:2 to planes of 16-bit YUV
void ConvertV210ToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *y_image;
    IMAGE *u_image;
    IMAGE *v_image;
    uint32_t *v210_row_ptr;
    PIXEL *y_row_ptr;
    PIXEL *u_row_ptr;
    PIXEL *v_row_ptr;
    int v210_pitch;
    int y_pitch;
    int u_pitch;
    int v_pitch;
    int width;
    int height;
    int row;
    int i;
    int display_height;

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);

    y_image = frame->channel[0];					// Get the individual planes
    u_image = frame->channel[1];
    v_image = frame->channel[2];

    v210_row_ptr = (uint32_t *)data;					// Pointers to the rows
    y_row_ptr = y_image->band[0];
    u_row_ptr = u_image->band[0];
    v_row_ptr = v_image->band[0];

    v210_pitch = pitch / sizeof(uint32_t);				// Convert pitch from bytes to pixels
    y_pitch = y_image->pitch / sizeof(PIXEL16S);
    u_pitch = u_image->pitch / sizeof(PIXEL16S);
    v_pitch = v_image->pitch / sizeof(PIXEL16S);

    width = y_image->width;							// Dimensions of the luma image
    height = y_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(v210_pitch > 0);

    for (row = 0; row < display_height; row++)
    {
#if 0
        // Start processing the row at the first column
        int column = 0;

#if (0 && XMMOPT)

        /***** Add code for the fast loop here *****/


        assert(column == post_column);
#endif

        // Process the rest of the row
        for (; column < width; column += 2)
        {
        }

        // Should have exited the loop just after the last column
        assert(column == width);
#elif 0
        // Unpack the row of 10-bit pixels
        ConvertV210RowToYUV((uint8_t *)v210_row_ptr, (PIXEL *)buffer, width);

        // Convert the unpacked pixels to 16-bit planes
        ConvertYUVPacked16sRowToPlanar16s((PIXEL *)buffer, width, y_row_ptr, u_row_ptr, v_row_ptr);
#else
        // Does the input row have the required alignment for fast unpacking?
        if (ISALIGNED16(v210_row_ptr))
        {
            // Unpack the row of 10-bit pixels to 16-bit planes
            ConvertV210RowToPlanar16s((uint8_t *)v210_row_ptr, width, y_row_ptr, u_row_ptr, v_row_ptr);
        }
        else
        {
            // Check that the buffer is properly aligned
            assert(ISALIGNED16(buffer));

            // Copy the row into aligned memory
            memcpy(buffer, v210_row_ptr, pitch);

            // Unpack the row of 10-bit pixels to 16-bit planes
            ConvertV210RowToPlanar16s(buffer, width, y_row_ptr, u_row_ptr, v_row_ptr);
        }
#endif
        // Advance to the next rows in the input and output images
        v210_row_ptr += v210_pitch;
        y_row_ptr += y_pitch;
        u_row_ptr += u_pitch;
        v_row_ptr += v_pitch;
    }

    // Set the image parameters for each channel
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}
//#endif

//#if BUILD_PROSPECT
// Convert the unpacked 16-bit YUV 4:2:2 to planes of 16-bit YUV
void ConvertYU64ToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *y_image;
    IMAGE *u_image;
    IMAGE *v_image;
    PIXEL *y_row_ptr;
    PIXEL *u_row_ptr;
    PIXEL *v_row_ptr;
    int yu64_pitch;
    int y_pitch;
    int u_pitch;
    int v_pitch;
    int width;
    int height;
    int rowp;
    int i;
    int display_height;

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);

    y_image = frame->channel[0];					// Get the individual planes
    u_image = frame->channel[1];
    v_image = frame->channel[2];

    y_row_ptr = y_image->band[0];
    u_row_ptr = u_image->band[0];
    v_row_ptr = v_image->band[0];

    yu64_pitch = pitch / sizeof(uint32_t);				// Convert pitch from bytes to pixels
    y_pitch = y_image->pitch / sizeof(PIXEL16S);
    u_pitch = u_image->pitch / sizeof(PIXEL16S);
    v_pitch = v_image->pitch / sizeof(PIXEL16S);

    width = y_image->width;							// Dimensions of the luma image
    height = y_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(yu64_pitch > 0);

    for (rowp = 0; rowp < height/*display_height*/; rowp++) //DAN20090215 File the frame with edge to prevent ringing artifacts.
    {
        int row = rowp < display_height ? rowp : display_height - 1;
        uint32_t *yu64_row_ptr = (uint32_t *)data;	// Pointers to the rows

        yu64_row_ptr += yu64_pitch * row;

        // Unpack the row of 10-bit pixels
        ConvertYU64RowToYUV10bit((uint8_t *)yu64_row_ptr, (PIXEL *)buffer, width);

        // Convert the unpacked pixels to 16-bit planes
        ConvertYUVPacked16sRowToPlanar16s((PIXEL *)buffer, width, y_row_ptr, u_row_ptr, v_row_ptr);

        // Advance to the next rows in the input and output images
        y_row_ptr += y_pitch;
        u_row_ptr += u_pitch;
        v_row_ptr += v_pitch;
    }

    // Set the image parameters for each channel
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}
//#endif


// Convert the packed 8-bit BAYER RGB to planes of 16-bit RGBA
void ConvertBYR1ToFrame16s(int bayer_format, uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *g_image;
    IMAGE *rg_diff_image;
    IMAGE *bg_diff_image;
    IMAGE *gdiff_image;
    uint8_t *byr1_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *gdiff_row_ptr;
    PIXEL *rg_row_ptr;
    PIXEL *bg_row_ptr;
    int byr1_pitch;
    int width;
    int height;
    int row;
    int i, x;
    int display_height;

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be four channels of RGBA
    assert(frame->num_channels == 4);
    assert(frame->format == FRAME_FORMAT_RGBA);

    g_image = frame->channel[0];					// Get the individual planes
    rg_diff_image = frame->channel[1];
    bg_diff_image = frame->channel[2];
    gdiff_image = frame->channel[3];

    byr1_row_ptr = (uint8_t *)data;					// Pointers to the rows
    g_row_ptr = g_image->band[0];
    rg_row_ptr = rg_diff_image->band[0];
    bg_row_ptr = bg_diff_image->band[0];
    gdiff_row_ptr = gdiff_image->band[0];

    byr1_pitch = g_image->pitch / sizeof(PIXEL16S);

    width = g_image->width;							// Dimensions of the luma image
    height = g_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(byr1_pitch > 0);

    for (row = 0; row < display_height; row++)
    {
        uint8_t *line1 = &byr1_row_ptr[row * pitch];
        uint8_t *line2 = line1 + (pitch >> 1);

        __m128i *line1ptr_epi16 = (__m128i *)line1;
        __m128i *line2ptr_epi16 = (__m128i *)line2;
        __m128i *gptr_epi16 = (__m128i *)g_row_ptr;
        __m128i *gdiffptr_epi16 = (__m128i *)gdiff_row_ptr;
        __m128i *rgptr_epi16 = (__m128i *)rg_row_ptr;
        __m128i *bgptr_epi16 = (__m128i *)bg_row_ptr;


        __m128i row_epi16;
        __m128i row1a_epi16;
        __m128i row2a_epi16;
        __m128i row1b_epi16;
        __m128i row2b_epi16;

        __m128i g1_epi16;
        __m128i g2_epi16;
        __m128i r_epi16;
        __m128i b_epi16;

        __m128i temp_epi16;
        __m128i g_epi16;
        __m128i gdiff_epi16;
        __m128i rg_epi16;
        __m128i bg_epi16;

        const __m128i rounding_epi16 = _mm_set1_epi16(512);
        const __m128i rounding256_epi16 = _mm_set1_epi16(256);
        const __m128i zero_epi16 = _mm_set1_epi16(0);
        const __m128i one_epi16 = _mm_set1_epi16(1);

        switch (bayer_format)
        {
            case BAYER_FORMAT_RED_GRN:
                for (x = 0; x < width; x += 8)
                {
                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line1ptr_epi16++);
                    row1a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    // Read the first group of 8 16-bit packed 12-bit pixels
                    //row1a_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  r0 g0 r1 g1 r2 g2 r3 g3
                    row1a_epi16 = _mm_shufflehi_epi16(row1a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g0 g1 r0 r1 r2 g2 r3 g3  _mm_shufflehi_epi16
                    row1a_epi16 = _mm_shufflelo_epi16(row1a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g0 g1 r0 r1 g2 g3 r2 r3  _mm_shufflelo_epi16
                    row1a_epi16 = _mm_shuffle_epi32(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 r0r1 r2r3  _mm_shuffle_epi32

                    row1b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row1b_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  r4 g4 r5 g5 r6 g6 r7 g7
                    row1b_epi16 = _mm_shufflehi_epi16(row1b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g4 g5 r4 r5 r6 g6 r7 g7  _mm_shufflehi_epi16
                    row1b_epi16 = _mm_shufflelo_epi16(row1b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g4 g5 r4 r5 g6 g7 r6 r7  _mm_shufflelo_epi16
                    row1b_epi16 = _mm_shuffle_epi32(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4g5 g6g7 r4r5 r6r7  _mm_shuffle_epi32


                    r_epi16 = 	_mm_unpackhi_epi32(row1a_epi16, row1b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpackhi_epi32
                    r_epi16 = 	_mm_shuffle_epi32(r_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32

                    g1_epi16 = 	_mm_unpacklo_epi32(row1a_epi16, row1b_epi16);
                    //r0r1 r4r5 r2r3 r6r7  _mm_unpacklo_epi32
                    g1_epi16 = 	_mm_shuffle_epi32(g1_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //r0r1 r2r3 r4r5 r6r7  _mm_shuffle_epi32




                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line2ptr_epi16++);
                    row2a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    //row2a_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  g b g b g b g b
                    row2a_epi16 = _mm_shufflehi_epi16(row2a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2a_epi16 = _mm_shufflelo_epi16(row2a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2a_epi16 = _mm_shuffle_epi32(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 g0g1 g2g3  _mm_shuffle_epi32

                    row2b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row2b_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  g b g b g b g b
                    row2b_epi16 = _mm_shufflehi_epi16(row2b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2b_epi16 = _mm_shufflelo_epi16(row2b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2b_epi16 = _mm_shuffle_epi32(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b4b5 b6b7 g4g5 g6g7  _mm_shuffle_epi32


                    g2_epi16 = 	_mm_unpackhi_epi32(row2a_epi16, row2b_epi16);
                    //b0b1 b4b5 b2b3 b6b7  _mm_unpackhi_epi32
                    g2_epi16 = 	_mm_shuffle_epi32(g2_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 b4b5 b6b7  _mm_shuffle_epi32


                    b_epi16 = 	_mm_unpacklo_epi32(row2a_epi16, row2b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpacklo_epi32
                    b_epi16 = 	_mm_shuffle_epi32(b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32


                    //*g_row_ptr++ = (g<<1)+1;
                    g_epi16 = _mm_adds_epi16(g1_epi16, g2_epi16);
                    temp_epi16 = _mm_slli_epi16(g_epi16, 1);
                    temp_epi16 = _mm_adds_epi16(temp_epi16, one_epi16);
                    _mm_store_si128(gptr_epi16++, temp_epi16);

                    //*rg_row_ptr++ = (r<<1)-g+512;
                    rg_epi16 = _mm_slli_epi16(r_epi16, 1);
                    rg_epi16 = _mm_subs_epi16(rg_epi16, g_epi16);
                    rg_epi16 = _mm_adds_epi16(rg_epi16, rounding_epi16);
                    _mm_store_si128(rgptr_epi16++, rg_epi16);

                    //*bg_row_ptr++ = (b<<1)-g+512;
                    bg_epi16 = _mm_slli_epi16(b_epi16, 1);
                    bg_epi16 = _mm_subs_epi16(bg_epi16, g_epi16);
                    bg_epi16 = _mm_adds_epi16(bg_epi16, rounding_epi16);
                    _mm_store_si128(bgptr_epi16++, bg_epi16);

                    //*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    gdiff_epi16 = _mm_subs_epi16(g1_epi16, g2_epi16);
                    gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding256_epi16);
                    gdiff_epi16 = _mm_slli_epi16(gdiff_epi16, 1);
                    _mm_store_si128(gdiffptr_epi16++, gdiff_epi16);



                    /*	r = *line1++;
                    	g1 = *line1++;
                    	g2 = *line2++;
                    	b = *line2++;

                    	// 10 bit
                    	g = (g1+g2);
                    	*g_row_ptr++ = (g<<1)+1;
                    	*rg_row_ptr++ = (r<<1)-g+512;
                    	*bg_row_ptr++ = (b<<1)-g+512;
                    	*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    	*/
                }
                break;

            case BAYER_FORMAT_GRN_RED:
                for (x = 0; x < width; x += 8)
                {
                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line1ptr_epi16++);
                    row1a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    // Read the first group of 8 16-bit packed 12-bit pixels
                    //row1a_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  g0 r0 g1 r1 g2 r2 g3 r3
                    row1a_epi16 = _mm_shufflehi_epi16(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0 g1 r0 r1 g2 r2 g3 r3  _mm_shufflehi_epi16
                    row1a_epi16 = _mm_shufflelo_epi16(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0 g1 r0 r1 g2 g3 r2 r3  _mm_shufflelo_epi16
                    row1a_epi16 = _mm_shuffle_epi32(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 r0r1 r2r3  _mm_shuffle_epi32

                    row1b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row1b_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  g4 r4 g5 r5 g6 r6 g7 r7
                    row1b_epi16 = _mm_shufflehi_epi16(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4 g5 r4 r5 g6 r6 g7 r7  _mm_shufflehi_epi16
                    row1b_epi16 = _mm_shufflelo_epi16(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4 g5 r4 r5 g6 g7 r6 r7  _mm_shufflelo_epi16
                    row1b_epi16 = _mm_shuffle_epi32(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4g5 g6g7 r4r5 r6r7  _mm_shuffle_epi32


                    r_epi16 = 	_mm_unpackhi_epi32(row1a_epi16, row1b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpackhi_epi32
                    r_epi16 = 	_mm_shuffle_epi32(r_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32

                    g1_epi16 = 	_mm_unpacklo_epi32(row1a_epi16, row1b_epi16);
                    //r0r1 r4r5 r2r3 r6r7  _mm_unpacklo_epi32
                    g1_epi16 = 	_mm_shuffle_epi32(g1_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //r0r1 r2r3 r4r5 r6r7  _mm_shuffle_epi32




                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line2ptr_epi16++);
                    row2a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    //row2a_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  b g b g b g b g
                    row2a_epi16 = _mm_shufflehi_epi16(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2a_epi16 = _mm_shufflelo_epi16(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2a_epi16 = _mm_shuffle_epi32(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 g0g1 g2g3  _mm_shuffle_epi32

                    row2b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row2b_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  b g b g b g b g
                    row2b_epi16 = _mm_shufflehi_epi16(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2b_epi16 = _mm_shufflelo_epi16(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2b_epi16 = _mm_shuffle_epi32(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b4b5 b6b7 g4g5 g6g7  _mm_shuffle_epi32


                    g2_epi16 = 	_mm_unpackhi_epi32(row2a_epi16, row2b_epi16);
                    //b0b1 b4b5 b2b3 b6b7  _mm_unpackhi_epi32
                    g2_epi16 = 	_mm_shuffle_epi32(g2_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 b4b5 b6b7  _mm_shuffle_epi32


                    b_epi16 = 	_mm_unpacklo_epi32(row2a_epi16, row2b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpacklo_epi32
                    b_epi16 = 	_mm_shuffle_epi32(b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32


                    //*g_row_ptr++ = (g<<1)+1;
                    g_epi16 = _mm_adds_epi16(g1_epi16, g2_epi16);
                    temp_epi16 = _mm_slli_epi16(g_epi16, 1);
                    temp_epi16 = _mm_adds_epi16(temp_epi16, one_epi16);
                    _mm_store_si128(gptr_epi16++, temp_epi16);

                    //*rg_row_ptr++ = (r<<1)-g+512;
                    rg_epi16 = _mm_slli_epi16(r_epi16, 1);
                    rg_epi16 = _mm_subs_epi16(rg_epi16, g_epi16);
                    rg_epi16 = _mm_adds_epi16(rg_epi16, rounding_epi16);
                    _mm_store_si128(rgptr_epi16++, rg_epi16);

                    //*bg_row_ptr++ = (b<<1)-g+512;
                    bg_epi16 = _mm_slli_epi16(b_epi16, 1);
                    bg_epi16 = _mm_subs_epi16(bg_epi16, g_epi16);
                    bg_epi16 = _mm_adds_epi16(bg_epi16, rounding_epi16);
                    _mm_store_si128(bgptr_epi16++, bg_epi16);

                    //*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    gdiff_epi16 = _mm_subs_epi16(g1_epi16, g2_epi16);
                    gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding256_epi16);
                    gdiff_epi16 = _mm_slli_epi16(gdiff_epi16, 1);
                    _mm_store_si128(gdiffptr_epi16++, gdiff_epi16);



                    /*	g1 = *line1++;
                    	r = *line1++;
                    	b = *line2++;
                    	g2 = *line2++;

                    	// 10 bit
                    	g = (g1+g2);
                    	*g_row_ptr++ = (g<<1)+1;
                    	*rg_row_ptr++ = (r<<1)-g+512;
                    	*bg_row_ptr++ = (b<<1)-g+512;
                    	*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    	*/
                }
                break;

            case BAYER_FORMAT_BLU_GRN:
                for (x = 0; x < width; x += 8)
                {
                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line1ptr_epi16++);
                    row1a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    // Read the first group of 8 16-bit packed 12-bit pixels
                    //row1a_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  r0 g0 r1 g1 r2 g2 r3 g3
                    row1a_epi16 = _mm_shufflehi_epi16(row1a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g0 g1 r0 r1 r2 g2 r3 g3  _mm_shufflehi_epi16
                    row1a_epi16 = _mm_shufflelo_epi16(row1a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g0 g1 r0 r1 g2 g3 r2 r3  _mm_shufflelo_epi16
                    row1a_epi16 = _mm_shuffle_epi32(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 r0r1 r2r3  _mm_shuffle_epi32

                    row1b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row1b_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  r4 g4 r5 g5 r6 g6 r7 g7
                    row1b_epi16 = _mm_shufflehi_epi16(row1b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g4 g5 r4 r5 r6 g6 r7 g7  _mm_shufflehi_epi16
                    row1b_epi16 = _mm_shufflelo_epi16(row1b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    //g4 g5 r4 r5 g6 g7 r6 r7  _mm_shufflelo_epi16
                    row1b_epi16 = _mm_shuffle_epi32(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4g5 g6g7 r4r5 r6r7  _mm_shuffle_epi32


                    b_epi16 = 	_mm_unpackhi_epi32(row1a_epi16, row1b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpackhi_epi32
                    b_epi16 = 	_mm_shuffle_epi32(b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32

                    g1_epi16 = 	_mm_unpacklo_epi32(row1a_epi16, row1b_epi16);
                    //r0r1 r4r5 r2r3 r6r7  _mm_unpacklo_epi32
                    g1_epi16 = 	_mm_shuffle_epi32(g1_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //r0r1 r2r3 r4r5 r6r7  _mm_shuffle_epi32




                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line2ptr_epi16++);
                    row2a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    //row2a_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  g b g b g b g b
                    row2a_epi16 = _mm_shufflehi_epi16(row2a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2a_epi16 = _mm_shufflelo_epi16(row2a_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2a_epi16 = _mm_shuffle_epi32(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 g0g1 g2g3  _mm_shuffle_epi32

                    row2b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row2b_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  g b g b g b g b
                    row2b_epi16 = _mm_shufflehi_epi16(row2b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2b_epi16 = _mm_shufflelo_epi16(row2b_epi16, _MM_SHUFFLE(2, 0, 3, 1));
                    row2b_epi16 = _mm_shuffle_epi32(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b4b5 b6b7 g4g5 g6g7  _mm_shuffle_epi32


                    g2_epi16 = 	_mm_unpackhi_epi32(row2a_epi16, row2b_epi16);
                    //b0b1 b4b5 b2b3 b6b7  _mm_unpackhi_epi32
                    g2_epi16 = 	_mm_shuffle_epi32(g2_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 b4b5 b6b7  _mm_shuffle_epi32


                    r_epi16 = 	_mm_unpacklo_epi32(row2a_epi16, row2b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpacklo_epi32
                    r_epi16 = 	_mm_shuffle_epi32(r_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32


                    //*g_row_ptr++ = (g<<1)+1;
                    g_epi16 = _mm_adds_epi16(g1_epi16, g2_epi16);
                    temp_epi16 = _mm_slli_epi16(g_epi16, 1);
                    temp_epi16 = _mm_adds_epi16(temp_epi16, one_epi16);
                    _mm_store_si128(gptr_epi16++, temp_epi16);

                    //*rg_row_ptr++ = (r<<1)-g+512;
                    rg_epi16 = _mm_slli_epi16(r_epi16, 1);
                    rg_epi16 = _mm_subs_epi16(rg_epi16, g_epi16);
                    rg_epi16 = _mm_adds_epi16(rg_epi16, rounding_epi16);
                    _mm_store_si128(rgptr_epi16++, rg_epi16);

                    //*bg_row_ptr++ = (b<<1)-g+512;
                    bg_epi16 = _mm_slli_epi16(b_epi16, 1);
                    bg_epi16 = _mm_subs_epi16(bg_epi16, g_epi16);
                    bg_epi16 = _mm_adds_epi16(bg_epi16, rounding_epi16);
                    _mm_store_si128(bgptr_epi16++, bg_epi16);

                    //*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    gdiff_epi16 = _mm_subs_epi16(g1_epi16, g2_epi16);
                    gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding256_epi16);
                    gdiff_epi16 = _mm_slli_epi16(gdiff_epi16, 1);
                    _mm_store_si128(gdiffptr_epi16++, gdiff_epi16);


                    /*	b = *line1++;
                    	g1 = *line1++;
                    	g2 = *line2++;
                    	r = *line2++;

                    	// 10 bit
                    	g = (g1+g2);
                    	*g_row_ptr++ = (g<<1)+1;
                    	*rg_row_ptr++ = (r<<1)-g+512;
                    	*bg_row_ptr++ = (b<<1)-g+512;
                    	*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    	*/
                }
                break;

            case BAYER_FORMAT_GRN_BLU:
                for (x = 0; x < width; x += 8)
                {
                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line1ptr_epi16++);
                    row1a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    // Read the first group of 8 16-bit packed 12-bit pixels
                    //row1a_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  g0 r0 g1 r1 g2 r2 g3 r3
                    row1a_epi16 = _mm_shufflehi_epi16(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0 g1 r0 r1 g2 r2 g3 r3  _mm_shufflehi_epi16
                    row1a_epi16 = _mm_shufflelo_epi16(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0 g1 r0 r1 g2 g3 r2 r3  _mm_shufflelo_epi16
                    row1a_epi16 = _mm_shuffle_epi32(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 r0r1 r2r3  _mm_shuffle_epi32

                    row1b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row1b_epi16 = _mm_load_si128(line1ptr_epi16++);
                    //g1 and r  g4 r4 g5 r5 g6 r6 g7 r7
                    row1b_epi16 = _mm_shufflehi_epi16(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4 g5 r4 r5 g6 r6 g7 r7  _mm_shufflehi_epi16
                    row1b_epi16 = _mm_shufflelo_epi16(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4 g5 r4 r5 g6 g7 r6 r7  _mm_shufflelo_epi16
                    row1b_epi16 = _mm_shuffle_epi32(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g4g5 g6g7 r4r5 r6r7  _mm_shuffle_epi32


                    b_epi16 = 	_mm_unpackhi_epi32(row1a_epi16, row1b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpackhi_epi32
                    b_epi16 = 	_mm_shuffle_epi32(b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32

                    g1_epi16 = 	_mm_unpacklo_epi32(row1a_epi16, row1b_epi16);
                    //r0r1 r4r5 r2r3 r6r7  _mm_unpacklo_epi32
                    g1_epi16 = 	_mm_shuffle_epi32(g1_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //r0r1 r2r3 r4r5 r6r7  _mm_shuffle_epi32




                    // Read the first group of 16 8-bit packed pixels
                    row_epi16 = _mm_load_si128(line2ptr_epi16++);
                    row2a_epi16 = _mm_unpacklo_epi8(row_epi16, zero_epi16);

                    //row2a_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  b g b g b g b g
                    row2a_epi16 = _mm_shufflehi_epi16(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2a_epi16 = _mm_shufflelo_epi16(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2a_epi16 = _mm_shuffle_epi32(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 g0g1 g2g3  _mm_shuffle_epi32

                    row2b_epi16 = _mm_unpackhi_epi8(row_epi16, zero_epi16);
                    //row2b_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  b g b g b g b g
                    row2b_epi16 = _mm_shufflehi_epi16(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2b_epi16 = _mm_shufflelo_epi16(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    row2b_epi16 = _mm_shuffle_epi32(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b4b5 b6b7 g4g5 g6g7  _mm_shuffle_epi32


                    g2_epi16 = 	_mm_unpackhi_epi32(row2a_epi16, row2b_epi16);
                    //b0b1 b4b5 b2b3 b6b7  _mm_unpackhi_epi32
                    g2_epi16 = 	_mm_shuffle_epi32(g2_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //b0b1 b2b3 b4b5 b6b7  _mm_shuffle_epi32


                    r_epi16 = 	_mm_unpacklo_epi32(row2a_epi16, row2b_epi16);
                    //g0g1 g4g5 g2g3 g6g7  _mm_unpacklo_epi32
                    r_epi16 = 	_mm_shuffle_epi32(r_epi16, _MM_SHUFFLE(3, 1, 2, 0));
                    //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32


                    //*g_row_ptr++ = (g<<1)+1;
                    g_epi16 = _mm_adds_epi16(g1_epi16, g2_epi16);
                    temp_epi16 = _mm_slli_epi16(g_epi16, 1);
                    temp_epi16 = _mm_adds_epi16(temp_epi16, one_epi16);
                    _mm_store_si128(gptr_epi16++, temp_epi16);

                    //*rg_row_ptr++ = (r<<1)-g+512;
                    rg_epi16 = _mm_slli_epi16(r_epi16, 1);
                    rg_epi16 = _mm_subs_epi16(rg_epi16, g_epi16);
                    rg_epi16 = _mm_adds_epi16(rg_epi16, rounding_epi16);
                    _mm_store_si128(rgptr_epi16++, rg_epi16);

                    //*bg_row_ptr++ = (b<<1)-g+512;
                    bg_epi16 = _mm_slli_epi16(b_epi16, 1);
                    bg_epi16 = _mm_subs_epi16(bg_epi16, g_epi16);
                    bg_epi16 = _mm_adds_epi16(bg_epi16, rounding_epi16);
                    _mm_store_si128(bgptr_epi16++, bg_epi16);

                    //*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    gdiff_epi16 = _mm_subs_epi16(g1_epi16, g2_epi16);
                    gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding256_epi16);
                    gdiff_epi16 = _mm_slli_epi16(gdiff_epi16, 1);
                    _mm_store_si128(gdiffptr_epi16++, gdiff_epi16);


                    /*	g1 = *line1++;
                    	b = *line1++;
                    	r = *line2++;
                    	g2 = *line2++;

                    	// 10 bit
                    	g = (g1+g2);
                    	*g_row_ptr++ = (g<<1)+1;
                    	*rg_row_ptr++ = (r<<1)-g+512;
                    	*bg_row_ptr++ = (b<<1)-g+512;
                    	*gdiff_row_ptr++ = (g1-g2+256)<<1;
                    	*/
                }
                break;

        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch;// - width;
        rg_row_ptr += byr1_pitch;// - width;
        bg_row_ptr += byr1_pitch;// - width;
        gdiff_row_ptr += byr1_pitch;// - width;
    }

    // Set the image parameters for each channel
    for (i = 0; i < 4; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}


#include <math.h>


#define BYR2_USE_GAMMA_TABLE  0
#define BYR2_HORIZONTAL_BAYER_SHIFT 1
#define BYR2_SWAP_R_B	0

// Convert the packed 16-bit BAYER RGB to planes of 16-bit RGBA
void ConvertBYR2ToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *g_image;
    IMAGE *rg_diff_image;
    IMAGE *bg_diff_image;
    IMAGE *gdiff_image;
    PIXEL *byr2_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *gdiff_row_ptr;
    PIXEL *rg_row_ptr;
    PIXEL *bg_row_ptr;
    int byr1_pitch;
    int width;
    int height;
    int display_height;
    int row;
    int i, x;
#if BYR2_USE_GAMMA_TABLE
    unsigned short gamma12bit[4096];
#endif

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be four channels of RGBA
    assert(frame->num_channels == 4);
    assert(frame->format == FRAME_FORMAT_RGBA);

    g_image = frame->channel[0];					// Get the individual planes
#if BYR2_SWAP_R_B
    rg_diff_image = frame->channel[2];
    bg_diff_image = frame->channel[1];
#else
    rg_diff_image = frame->channel[1];
    bg_diff_image = frame->channel[2];
#endif
    gdiff_image = frame->channel[3];

    byr2_row_ptr = (PIXEL *)data;					// Pointers to the rows
    g_row_ptr = g_image->band[0];
    rg_row_ptr = rg_diff_image->band[0];
    bg_row_ptr = bg_diff_image->band[0];
    gdiff_row_ptr = gdiff_image->band[0];

    byr1_pitch = g_image->pitch / sizeof(PIXEL16S);

    width = g_image->width;							// Dimensions of the luma image
    height = g_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(byr1_pitch > 0);

    // for the SEQ speed test on my 2.5 P4 I get 56fps this the C code.
#if BYR2_USE_GAMMA_TABLE

#define BYR2_GAMMATABLE(x,y)  (  (int)(pow( (double)(x)/4095.0, (y) )*1023.0)  )

    // #define BYR2_GAMMA2(x)  ((x)>>2)
#define BYR2_GAMMA2(x)  ( gamma12bit[(x)] )
    //#define BYR2_GAMMA2(x)  (  (int)(pow( double(x)/4096.0, 1.0/2.0 )*256.0)  )
    //inline int BYR2_GAMMA2(int x)  {  int v = 4095-(int)(x);  return ((4095 - ((v*v)>>12))>>4);  }

    {
        int blacklevel = 0;//100;
        float fgamma = 2.2;

        for (i = 0; i < 4096; i++)
        {
            int j = (i - blacklevel) * 4096 / (4096 - blacklevel);
            if (j < 0) j = 0;
            gamma12bit[i] = BYR2_GAMMATABLE(j, 1.0 / fgamma);
        }
    }

    {
#define LINMAX 40
        float linearmax = (float)gamma12bit[LINMAX];
        float linearstep = linearmax / (float)LINMAX;
        float accum = 0.0;

        for (i = 0; i < 40; i++)
        {
            gamma12bit[i] = accum;
            accum += linearstep;
        }
    }

    for (row = 0; row < display_height; row++)
    {
        PIXEL g, g1, g2, r, b;
        PIXEL *line1, *line2;

        line1 = &byr2_row_ptr[row * pitch / 2];
        line2 = line1 + (pitch >> 2);

        for (x = 0; x < width; x++)
        {
            /*	g1 = *line1++ >> 2;
            	r = *line1++ >> 2;
            	b = *line2++ >> 2;
            	g2 = *line2++ >> 2;*/
#if BYR2_HORIZONTAL_BAYER_SHIFT
            r = BYR2_GAMMA2(*line1++);
            g1 = BYR2_GAMMA2(*line1++);
            g2 = BYR2_GAMMA2(*line2++);
            b = BYR2_GAMMA2(*line2++);
#else
            g1 = BYR2_GAMMA2(*line1++);
            r = BYR2_GAMMA2(*line1++);
            b = BYR2_GAMMA2(*line2++);
            g2 = BYR2_GAMMA2(*line2++);
#endif

            /* 10 bit */
            g = (g1 + g2) >> 1;
            *g_row_ptr++ = g;
            *rg_row_ptr++ = ((r - g) >> 1) + 512;
            *bg_row_ptr++ = ((b - g) >> 1) + 512;
            *gdiff_row_ptr++ = (g1 - g2 + 1024) >> 1;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }
    for (; row < height; row++)
    {
        PIXEL g, g1, g2, r, b;
        PIXEL *line1, *line2;

        for (x = 0; x < width; x++)
        {
            *g_row_ptr++ = 0;
            *rg_row_ptr++ = 0;
            *bg_row_ptr++ = 0;
            *gdiff_row_ptr++ = 0;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }


#else

    // for the SEQ speed test on my 2.5 P4 I get 65fps in the SSE2 code.
    for (row = 0; row < display_height; row++)
    {

        PIXEL *line1 = &byr2_row_ptr[row * pitch / 2];
        PIXEL *line2 = line1 + (pitch >> 2);

        __m128i *line1ptr_epi16 = (__m128i *)line1;
        __m128i *line2ptr_epi16 = (__m128i *)line2;
        __m128i *gptr_epi16 = (__m128i *)g_row_ptr;
        __m128i *gdiffptr_epi16 = (__m128i *)gdiff_row_ptr;
        __m128i *rgptr_epi16 = (__m128i *)rg_row_ptr;
        __m128i *bgptr_epi16 = (__m128i *)bg_row_ptr;

        __m128i row1a_epi16;
        __m128i row2a_epi16;
        __m128i row1b_epi16;
        __m128i row2b_epi16;

        __m128i g1_epi16;
        __m128i g2_epi16;
        __m128i r_epi16;
        __m128i b_epi16;

        __m128i g_epi16;
        __m128i gdiff_epi16;
        __m128i rg_epi16;
        __m128i bg_epi16;

        const __m128i rounding_epi16 = _mm_set1_epi16(512);


        for (x = 0; x < width; x += 8)
        {
            // Read the first group of 8 16-bit packed 12-bit pixels
            row1a_epi16 = _mm_load_si128(line1ptr_epi16++);
            //g1 and r  g0 r0 g1 r1 g2 r2 g3 r3
            row1a_epi16 = _mm_shufflehi_epi16(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0 g1 r0 r1 g2 r2 g3 r3  _mm_shufflehi_epi16
            row1a_epi16 = _mm_shufflelo_epi16(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0 g1 r0 r1 g2 g3 r2 r3  _mm_shufflelo_epi16
            row1a_epi16 = _mm_shuffle_epi32(row1a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0g1 g2g3 r0r1 r2r3  _mm_shuffle_epi32

            row1b_epi16 = _mm_load_si128(line1ptr_epi16++);
            //g1 and r  g4 r4 g5 r5 g6 r6 g7 r7
            row1b_epi16 = _mm_shufflehi_epi16(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g4 g5 r4 r5 g6 r6 g7 r7  _mm_shufflehi_epi16
            row1b_epi16 = _mm_shufflelo_epi16(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g4 g5 r4 r5 g6 g7 r6 r7  _mm_shufflelo_epi16
            row1b_epi16 = _mm_shuffle_epi32(row1b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g4g5 g6g7 r4r5 r6r7  _mm_shuffle_epi32

#if	BYR2_HORIZONTAL_BAYER_SHIFT
            g1_epi16 = 	_mm_unpackhi_epi32(row1a_epi16, row1b_epi16);
            //g0g1 g4g5 g2g3 g6g7  _mm_unpackhi_epi32
            g1_epi16 = 	_mm_shuffle_epi32(g1_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32

            r_epi16 = 	_mm_unpacklo_epi32(row1a_epi16, row1b_epi16);
            //r0r1 r4r5 r2r3 r6r7  _mm_unpacklo_epi32
            r_epi16 = 	_mm_shuffle_epi32(r_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //r0r1 r2r3 r4r5 r6r7  _mm_shuffle_epi32
#else
            r_epi16 = 	_mm_unpackhi_epi32(row1a_epi16, row1b_epi16);
            //g0g1 g4g5 g2g3 g6g7  _mm_unpackhi_epi32
            r_epi16 = 	_mm_shuffle_epi32(r_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32

            g1_epi16 = 	_mm_unpacklo_epi32(row1a_epi16, row1b_epi16);
            //r0r1 r4r5 r2r3 r6r7  _mm_unpacklo_epi32
            g1_epi16 = 	_mm_shuffle_epi32(g1_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //r0r1 r2r3 r4r5 r6r7  _mm_shuffle_epi32
#endif



            // Read the first group of 8 16-bit packed 12-bit pixels
            row2a_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  b g b g b g b g
            row2a_epi16 = _mm_shufflehi_epi16(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            row2a_epi16 = _mm_shufflelo_epi16(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            row2a_epi16 = _mm_shuffle_epi32(row2a_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //b0b1 b2b3 g0g1 g2g3  _mm_shuffle_epi32

            row2b_epi16 = _mm_load_si128(line2ptr_epi16++);  //b and g2  b g b g b g b g
            row2b_epi16 = _mm_shufflehi_epi16(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            row2b_epi16 = _mm_shufflelo_epi16(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            row2b_epi16 = _mm_shuffle_epi32(row2b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //b4b5 b6b7 g4g5 g6g7  _mm_shuffle_epi32


#if	BYR2_HORIZONTAL_BAYER_SHIFT
            b_epi16 = 	_mm_unpackhi_epi32(row2a_epi16, row2b_epi16);
            //b0b1 b4b5 b2b3 b6b7  _mm_unpackhi_epi32
            b_epi16 = 	_mm_shuffle_epi32(b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //b0b1 b2b3 b4b5 b6b7  _mm_shuffle_epi32

            g2_epi16 = 	_mm_unpacklo_epi32(row2a_epi16, row2b_epi16);
            //g0g1 g4g5 g2g3 g6g7  _mm_unpacklo_epi32
            g2_epi16 = 	_mm_shuffle_epi32(g2_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32
#else
            g2_epi16 = 	_mm_unpackhi_epi32(row2a_epi16, row2b_epi16);
            //b0b1 b4b5 b2b3 b6b7  _mm_unpackhi_epi32
            g2_epi16 = 	_mm_shuffle_epi32(g2_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //b0b1 b2b3 b4b5 b6b7  _mm_shuffle_epi32

            b_epi16 = 	_mm_unpacklo_epi32(row2a_epi16, row2b_epi16);
            //g0g1 g4g5 g2g3 g6g7  _mm_unpacklo_epi32
            b_epi16 = 	_mm_shuffle_epi32(b_epi16, _MM_SHUFFLE(3, 1, 2, 0));
            //g0g1 g2g3 g4g5 g6g7  _mm_shuffle_epi32
#endif

            g1_epi16 = _mm_srai_epi16(g1_epi16, 2);
            g2_epi16 = _mm_srai_epi16(g2_epi16, 2);
            r_epi16 = _mm_srai_epi16(r_epi16, 2);
            b_epi16 = _mm_srai_epi16(b_epi16, 2);

            g_epi16 = _mm_adds_epi16(g1_epi16, g2_epi16);
            g_epi16 = _mm_srai_epi16(g_epi16, 1);
            _mm_store_si128(gptr_epi16++, g_epi16);

            rg_epi16 = _mm_subs_epi16(r_epi16, g_epi16);
            rg_epi16 = _mm_srai_epi16(rg_epi16, 1);
            rg_epi16 = _mm_adds_epi16(rg_epi16, rounding_epi16);
            _mm_store_si128(rgptr_epi16++, rg_epi16);

            bg_epi16 = _mm_subs_epi16(b_epi16, g_epi16);
            bg_epi16 = _mm_srai_epi16(bg_epi16, 1);
            bg_epi16 = _mm_adds_epi16(bg_epi16, rounding_epi16);
            _mm_store_si128(bgptr_epi16++, bg_epi16);

            gdiff_epi16 = _mm_subs_epi16(g1_epi16, g2_epi16);
            gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding_epi16);
            gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding_epi16);
            gdiff_epi16 = _mm_srai_epi16(gdiff_epi16, 1);
            _mm_store_si128(gdiffptr_epi16++, gdiff_epi16);



            /*	g1 = *line1++ >> 2;
            	r = *line1++ >> 2;
            	b = *line2++ >> 2;
            	g2 = *line2++ >> 2;

            	// 10 bit
            	g = (g1+g2)>>1;
            	*g_row_ptr++ = g;
            	*rg_row_ptr++ = ((r-g)>>1)+512;
            	*bg_row_ptr++ = ((b-g)>>1)+512;
            	*gdiff_row_ptr++ = (g1-g2+1024)>>1;
            	*/
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch;// - width;
        rg_row_ptr += byr1_pitch;// - width;
        bg_row_ptr += byr1_pitch;// - width;
        gdiff_row_ptr += byr1_pitch;// - width;
    }
#endif


    // Set the image parameters for each channel
    for (i = 0; i < 4; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}





#define BYR3_USE_GAMMA_TABLE  0
#define BYR3_HORIZONTAL_BAYER_SHIFT 0
#define BYR3_SWAP_R_B	1


int ConvertPackedToRawBayer16(int width, int height, uint32_t *uncompressed_chunk,
                              uint32_t uncompressed_size, PIXEL16U *RawBayer16, PIXEL16U *scratch,
                              int resolution)
{
    int row;
    int x;
    int srcwidth;
    int linestep = 1;

    if (uncompressed_size < ((uint32_t)width * height * 4 * 3 / 2))
    {
        // Not the correct data format
        return 0;
    }

    srcwidth = width;
    if (resolution == DECODED_RESOLUTION_QUARTER_NODEBAYER_SCALED)
    {
        srcwidth = width * 2;
        linestep = 2;
    }

    for (row = 0; row < height; row++)
    {
        PIXEL16U *tptr, *dptr;
        uint8_t *outB, *outN;

        tptr = scratch;
        dptr = RawBayer16;
        dptr += row * (width * 4);

        outB = (uint8_t *)uncompressed_chunk;
        outB += row * linestep * srcwidth * 4 * 3 / 2; //12-bit
        outN = outB;
        outN += srcwidth * 4;

        {
            __m128i g1_epi16;
            __m128i g2_epi16;
            __m128i g3_epi16;
            __m128i g4_epi16;
            __m128i B1_epi16;
            __m128i N1_epi16;
            __m128i B2_epi16;
            __m128i N2_epi16;
            __m128i B3_epi16;
            __m128i N3_epi16;
            __m128i B4_epi16;
            __m128i N4_epi16;
            __m128i zero = _mm_set1_epi16(0);
            __m128i MaskUp = _mm_set1_epi16(0xf0f0);
            __m128i MaskDn = _mm_set1_epi16(0x0f0f);

            __m128i *tmp_epi16 = (__m128i *)tptr;
            __m128i *outB_epi16 = (__m128i *)outB;
            __m128i *outN_epi16 = (__m128i *)outN;

            for (x = 0; x < srcwidth * 4; x += 32)
            {
                B1_epi16 = _mm_loadu_si128(outB_epi16++);
                B2_epi16 = _mm_loadu_si128(outB_epi16++);
                N1_epi16 = _mm_loadu_si128(outN_epi16++);

                N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                g4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                g3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                g2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                g1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                g1_epi16 = _mm_or_si128(g1_epi16, B1_epi16);
                g2_epi16 = _mm_or_si128(g2_epi16, B2_epi16);
                g3_epi16 = _mm_or_si128(g3_epi16, B3_epi16);
                g4_epi16 = _mm_or_si128(g4_epi16, B4_epi16);

                _mm_store_si128(tmp_epi16++, g1_epi16);
                _mm_store_si128(tmp_epi16++, g2_epi16);
                _mm_store_si128(tmp_epi16++, g3_epi16);
                _mm_store_si128(tmp_epi16++, g4_epi16);
            }

            if (linestep == 1)
            {
                __m128i *rp_epi16 = (__m128i *)tptr;
                __m128i *g1p_epi16 = (__m128i *)&tptr[width];
                __m128i *g2p_epi16 = (__m128i *)&tptr[width * 2];
                __m128i *bp_epi16 = (__m128i *)&tptr[width * 3];
                __m128i *dgg_epi16 = (__m128i *)dptr;
                __m128i *drg_epi16 = (__m128i *)&dptr[width];
                __m128i *dbg_epi16 = (__m128i *)&dptr[width * 2];
                __m128i *ddg_epi16 = (__m128i *)&dptr[width * 3];
                __m128i mid11bit = _mm_set1_epi16(1 << (13 - 1));

                for (x = 0; x < srcwidth; x += 8)
                {
                    __m128i r_epi16 = _mm_load_si128(rp_epi16++);
                    __m128i g1_epi16 = _mm_load_si128(g1p_epi16++);
                    __m128i g2_epi16 = _mm_load_si128(g2p_epi16++);
                    __m128i b_epi16 = _mm_load_si128(bp_epi16++);

                    __m128i gg = _mm_adds_epu16(g1_epi16, g2_epi16); //13-bit
                    __m128i rg = _mm_adds_epu16(r_epi16, r_epi16); //13-bit
                    __m128i bg = _mm_adds_epu16(b_epi16, b_epi16); //13-bit
                    __m128i dg = _mm_subs_epi16(g1_epi16, g2_epi16); //signed 12-bit

                    rg = _mm_subs_epi16(rg, gg); //13-bit
                    bg = _mm_subs_epi16(bg, gg); //13-bit
                    rg = _mm_srai_epi16(rg, 1); //12-bit signed
                    bg = _mm_srai_epi16(bg, 1); //12-bit signed
                    rg = _mm_adds_epi16(rg, mid11bit); //13-bit unsigned
                    bg = _mm_adds_epi16(bg, mid11bit); //13-bit unsigned
                    dg = _mm_adds_epi16(dg, mid11bit); //13-bit unsigned
                    gg = _mm_slli_epi16(gg, 3); //16-bit unsigned
                    rg = _mm_slli_epi16(rg, 3); //16-bit unsigned
                    bg = _mm_slli_epi16(bg, 3); //16-bit unsigned
                    dg = _mm_slli_epi16(dg, 3); //16-bit unsigned

                    _mm_store_si128(dgg_epi16++, gg);
                    _mm_store_si128(drg_epi16++, rg);
                    _mm_store_si128(dbg_epi16++, bg);
                    _mm_store_si128(ddg_epi16++, dg);
                }

                for (; x < srcwidth; x++)
                {
                    int G = (scratch[x + width] + scratch[x + width * 2]) << 2;
                    int RG = (scratch[x] << 3) - G + 32768;
                    int BG = (scratch[x + width * 3] << 3) - G + 32768;
                    int DG = ((scratch[x + width] - scratch[x + width * 2]) << 3) + 32768;
                    dptr[x] = G << 1;
                    dptr[x + width] = RG; //scratch[x+width];
                    dptr[x + width * 2] = BG; //scratch[x+width*2];
                    dptr[x + width * 3] = DG; //scratch[x+width*3];
                }
            }
            else
            {
                for (x = 0; x < width; x++)
                {
                    int G = (scratch[x * 2 + srcwidth] + scratch[x * 2 + srcwidth * 2]) << 2;
                    int RG = (scratch[x * 2] << 3) - G + 32768;
                    int BG = (scratch[x * 2 + srcwidth * 3] << 3) - G + 32768;
                    int DG = ((scratch[x * 2 + srcwidth] - scratch[x * 2 + srcwidth * 2]) << 3) + 32768;
                    dptr[x] = G << 1;
                    dptr[x + width] = RG; //scratch[x+width];
                    dptr[x + width * 2] = BG; //scratch[x+width*2];
                    dptr[x + width * 3] = DG; //scratch[x+width*3];
                }
            }
        }
    }

    return 0;
}



int ConvertPackedToBYR2(int width, int height, uint32_t *uncompressed_chunk, uint32_t uncompressed_size, uint8_t *output_buffer, int output_pitch, unsigned short *curve)
{
    int row, x;

    if (uncompressed_size < ((uint32_t)width * height * 4 * 3 / 2))
    {
        // Not the correct data format
        return 0;
    }

    for (row = 0; row < height; row++)
    {
        PIXEL16U *dptrRG, *dptrGB;
        uint8_t *outB, *outN;

        dptrRG = (PIXEL16U *)output_buffer;
        dptrRG += row * (width * 4);
        dptrGB = dptrRG;
        dptrGB += (width * 2);

        outB = (uint8_t *)uncompressed_chunk;
        outB += row * width * 4 * 3 / 2; //12-bit
        outN = outB;
        outN += width * 4;

        {
            __m128i gA1_epi16;
            __m128i gA2_epi16;
            __m128i gA3_epi16;
            __m128i gA4_epi16;

            __m128i gB1_epi16;
            __m128i gB2_epi16;
            __m128i gB3_epi16;
            __m128i gB4_epi16;

            __m128i r1_epi16;
            __m128i r2_epi16;
            __m128i r3_epi16;
            __m128i r4_epi16;

            __m128i b1_epi16;
            __m128i b2_epi16;
            __m128i b3_epi16;
            __m128i b4_epi16;

            __m128i B1_epi16;
            __m128i N1_epi16;
            __m128i B2_epi16;
            __m128i N2_epi16;
            __m128i B3_epi16;
            __m128i N3_epi16;
            __m128i B4_epi16;
            __m128i N4_epi16;
            __m128i zero = _mm_set1_epi16(0);
            __m128i MaskUp = _mm_set1_epi16(0xf0f0);
            __m128i MaskDn = _mm_set1_epi16(0x0f0f);

            __m128i *dstRG_epi16 = (__m128i *)dptrRG;
            __m128i *dstGB_epi16 = (__m128i *)dptrGB;
            __m128i *outBr_epi16 = (__m128i *)outB;
            __m128i *outNr_epi16 = (__m128i *)outN;
            __m128i *outBgA_epi16;
            __m128i *outNgA_epi16;
            __m128i *outBgB_epi16;
            __m128i *outNgB_epi16;
            __m128i *outBb_epi16;
            __m128i *outNb_epi16;

            outB += width;
            outN += width >> 1;
            outBgA_epi16 = (__m128i *)outB;
            outNgA_epi16 = (__m128i *)outN;

            outB += width;
            outN += width >> 1;
            outBgB_epi16 = (__m128i *)outB;
            outNgB_epi16 = (__m128i *)outN;

            outB += width;
            outN += width >> 1;
            outBb_epi16 = (__m128i *)outB;
            outNb_epi16 = (__m128i *)outN;


            for (x = 0; x < width; x += 32)
            {
                B1_epi16 = _mm_loadu_si128(outBr_epi16++);
                B2_epi16 = _mm_loadu_si128(outBr_epi16++);
                N1_epi16 = _mm_loadu_si128(outNr_epi16++);

                N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                r4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                r3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                r2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                r1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                r1_epi16 = _mm_or_si128(r1_epi16, B1_epi16);
                r2_epi16 = _mm_or_si128(r2_epi16, B2_epi16);
                r3_epi16 = _mm_or_si128(r3_epi16, B3_epi16);
                r4_epi16 = _mm_or_si128(r4_epi16, B4_epi16);

                r1_epi16 = _mm_slli_epi16(r1_epi16, 4);
                r2_epi16 = _mm_slli_epi16(r2_epi16, 4);
                r3_epi16 = _mm_slli_epi16(r3_epi16, 4);
                r4_epi16 = _mm_slli_epi16(r4_epi16, 4);




                B1_epi16 = _mm_loadu_si128(outBgA_epi16++);
                B2_epi16 = _mm_loadu_si128(outBgA_epi16++);
                N1_epi16 = _mm_loadu_si128(outNgA_epi16++);

                N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                gA4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                gA3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                gA2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                gA1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                gA1_epi16 = _mm_or_si128(gA1_epi16, B1_epi16);
                gA2_epi16 = _mm_or_si128(gA2_epi16, B2_epi16);
                gA3_epi16 = _mm_or_si128(gA3_epi16, B3_epi16);
                gA4_epi16 = _mm_or_si128(gA4_epi16, B4_epi16);

                gA1_epi16 = _mm_slli_epi16(gA1_epi16, 4);
                gA2_epi16 = _mm_slli_epi16(gA2_epi16, 4);
                gA3_epi16 = _mm_slli_epi16(gA3_epi16, 4);
                gA4_epi16 = _mm_slli_epi16(gA4_epi16, 4);


                _mm_store_si128(dstRG_epi16++, _mm_unpacklo_epi16(r1_epi16, gA1_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpackhi_epi16(r1_epi16, gA1_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpacklo_epi16(r2_epi16, gA2_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpackhi_epi16(r2_epi16, gA2_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpacklo_epi16(r3_epi16, gA3_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpackhi_epi16(r3_epi16, gA3_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpacklo_epi16(r4_epi16, gA4_epi16));
                _mm_store_si128(dstRG_epi16++, _mm_unpackhi_epi16(r4_epi16, gA4_epi16));


                B1_epi16 = _mm_loadu_si128(outBgB_epi16++);
                B2_epi16 = _mm_loadu_si128(outBgB_epi16++);
                N1_epi16 = _mm_loadu_si128(outNgB_epi16++);

                N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                gB4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                gB3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                gB2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                gB1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                gB1_epi16 = _mm_or_si128(gB1_epi16, B1_epi16);
                gB2_epi16 = _mm_or_si128(gB2_epi16, B2_epi16);
                gB3_epi16 = _mm_or_si128(gB3_epi16, B3_epi16);
                gB4_epi16 = _mm_or_si128(gB4_epi16, B4_epi16);

                gB1_epi16 = _mm_slli_epi16(gB1_epi16, 4);
                gB2_epi16 = _mm_slli_epi16(gB2_epi16, 4);
                gB3_epi16 = _mm_slli_epi16(gB3_epi16, 4);
                gB4_epi16 = _mm_slli_epi16(gB4_epi16, 4);




                B1_epi16 = _mm_loadu_si128(outBb_epi16++);
                B2_epi16 = _mm_loadu_si128(outBb_epi16++);
                N1_epi16 = _mm_loadu_si128(outNb_epi16++);

                N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                b4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                b3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                b2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                b1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                b1_epi16 = _mm_or_si128(b1_epi16, B1_epi16);
                b2_epi16 = _mm_or_si128(b2_epi16, B2_epi16);
                b3_epi16 = _mm_or_si128(b3_epi16, B3_epi16);
                b4_epi16 = _mm_or_si128(b4_epi16, B4_epi16);

                b1_epi16 = _mm_slli_epi16(b1_epi16, 4);
                b2_epi16 = _mm_slli_epi16(b2_epi16, 4);
                b3_epi16 = _mm_slli_epi16(b3_epi16, 4);
                b4_epi16 = _mm_slli_epi16(b4_epi16, 4);



                _mm_store_si128(dstGB_epi16++, _mm_unpacklo_epi16(gB1_epi16, b1_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpackhi_epi16(gB1_epi16, b1_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpacklo_epi16(gB2_epi16, b2_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpackhi_epi16(gB2_epi16, b2_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpacklo_epi16(gB3_epi16, b3_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpackhi_epi16(gB3_epi16, b3_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpacklo_epi16(gB4_epi16, b4_epi16));
                _mm_store_si128(dstGB_epi16++, _mm_unpackhi_epi16(gB4_epi16, b4_epi16));
            }
        }

        if (curve)
        {
            for (x = 0; x < width * 2; x++)
            {
                dptrRG[x] = curve[dptrRG[x] >> 2];
                dptrGB[x] = curve[dptrGB[x] >> 2];
            }
        }
    }

    return 0;
}

int ConvertPackedToBYR3(int width, int height, uint32_t *uncompressed_chunk, uint32_t uncompressed_size, uint8_t *output_buffer, int output_pitch)
{
    int row, x;

    if (uncompressed_size < ((uint32_t)width * height * 4 * 3 / 2))
    {
        // Not the correct data format
        return 0;
    }

    for (row = 0; row < height; row++)
    {
        PIXEL16U *dptr;
        uint8_t *outB, *outN;

        dptr = (PIXEL16U *)output_buffer;
        dptr += row * (width * 4);

        outB = (uint8_t *)uncompressed_chunk;
        outB += row * width * 4 * 3 / 2; //12-bit
        outN = outB;
        outN += width * 4;

        {
            __m128i g1_epi16;
            __m128i g2_epi16;
            __m128i g3_epi16;
            __m128i g4_epi16;
            __m128i B1_epi16;
            __m128i N1_epi16;
            __m128i B2_epi16;
            __m128i N2_epi16;
            __m128i B3_epi16;
            __m128i N3_epi16;
            __m128i B4_epi16;
            __m128i N4_epi16;
            __m128i zero = _mm_set1_epi16(0);
            __m128i MaskUp = _mm_set1_epi16(0xf0f0);
            __m128i MaskDn = _mm_set1_epi16(0x0f0f);

            __m128i *dst_epi16 = (__m128i *)dptr;
            __m128i *outB_epi16 = (__m128i *)outB;
            __m128i *outN_epi16 = (__m128i *)outN;


            for (x = 0; x < width * 4; x += 32)
            {
                B1_epi16 = _mm_loadu_si128(outB_epi16++);
                B2_epi16 = _mm_loadu_si128(outB_epi16++);
                N1_epi16 = _mm_loadu_si128(outN_epi16++);

                N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                g4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                g3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                g2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                g1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                g1_epi16 = _mm_or_si128(g1_epi16, B1_epi16);
                g2_epi16 = _mm_or_si128(g2_epi16, B2_epi16);
                g3_epi16 = _mm_or_si128(g3_epi16, B3_epi16);
                g4_epi16 = _mm_or_si128(g4_epi16, B4_epi16);

                g1_epi16 = _mm_srli_epi16(g1_epi16, 2); //12-bit down 10-bit.
                g2_epi16 = _mm_srli_epi16(g2_epi16, 2);
                g3_epi16 = _mm_srli_epi16(g3_epi16, 2);
                g4_epi16 = _mm_srli_epi16(g4_epi16, 2);

                _mm_store_si128(dst_epi16++, g1_epi16);
                _mm_store_si128(dst_epi16++, g2_epi16);
                _mm_store_si128(dst_epi16++, g3_epi16);
                _mm_store_si128(dst_epi16++, g4_epi16);
            }
        }
    }

    return 0;
}

int ConvertBYR3ToPacked(uint8_t *data, int pitch, int width, int height, uint8_t *buffer)
{
    int row, x;


    for (row = 0; row < height; row++)
    {
        PIXEL16U *sptr;
        uint8_t *outB, *outN;

        sptr = (PIXEL16U *)data;
        sptr += row * (pitch >> 1);

        outB = (uint8_t *)buffer;
        outB += row * width * 4 * 3 / 2; //12-bit
        outN = outB;
        outN += width * 4;

#if 1
        {
            __m128i g1_epi16;
            __m128i g2_epi16;
            __m128i g3_epi16;
            __m128i g4_epi16;
            __m128i B1_epi16;
            __m128i N1_epi16;
            __m128i B2_epi16;
            __m128i N2_epi16;
            __m128i B3_epi16;
            __m128i N3_epi16;
            __m128i B4_epi16;
            __m128i N4_epi16;
            __m128i MaskHi = _mm_set1_epi16(0x00f0);

            __m128i *src_epi16 = (__m128i *)sptr;
            __m128i *outB_epi16 = (__m128i *)outB;
            __m128i *outN_epi16 = (__m128i *)outN;


            for (x = 0; x < width * 4; x += 32)
            {
                // Read the first group of 8 16-bit packed 12-bit pixels
                g1_epi16 = _mm_load_si128(src_epi16++);
                g2_epi16 = _mm_load_si128(src_epi16++);
                g3_epi16 = _mm_load_si128(src_epi16++);
                g4_epi16 = _mm_load_si128(src_epi16++);

                // boost to 12-bit first
                g1_epi16 = _mm_slli_epi16(g1_epi16, 2);
                g2_epi16 = _mm_slli_epi16(g2_epi16, 2);
                g3_epi16 = _mm_slli_epi16(g3_epi16, 2);
                g4_epi16 = _mm_slli_epi16(g4_epi16, 2);

                B1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                N1_epi16 = _mm_slli_epi16(g1_epi16, 4);
                B2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                N2_epi16 = _mm_slli_epi16(g2_epi16, 4);
                B3_epi16 = _mm_srli_epi16(g3_epi16, 4);
                N3_epi16 = _mm_slli_epi16(g3_epi16, 4);
                B4_epi16 = _mm_srli_epi16(g4_epi16, 4);
                N4_epi16 = _mm_slli_epi16(g4_epi16, 4);

                N1_epi16 = _mm_and_si128(N1_epi16, MaskHi);
                N2_epi16 = _mm_and_si128(N2_epi16, MaskHi);
                N3_epi16 = _mm_and_si128(N3_epi16, MaskHi);
                N4_epi16 = _mm_and_si128(N4_epi16, MaskHi);

                B1_epi16 = _mm_packus_epi16(B1_epi16, B2_epi16);
                N1_epi16 = _mm_packus_epi16(N1_epi16, N2_epi16);

                B2_epi16 = _mm_packus_epi16(B3_epi16, B4_epi16);
                N2_epi16 = _mm_packus_epi16(N3_epi16, N4_epi16);

                N2_epi16 = _mm_srli_epi16(N2_epi16, 4);
                N1_epi16 = _mm_or_si128(N1_epi16, N2_epi16);

                _mm_store_si128(outB_epi16++, B1_epi16);
                _mm_store_si128(outB_epi16++, B2_epi16);
                _mm_store_si128(outN_epi16++, N1_epi16);
            }
        }
#else
        for (x = 0; x < width * 4; x += 32) //R, G1,G2, B
        {
            int xx;
            for (xx = 0; xx < 32; xx++)
            {
                g1[xx] = *sptr++ << 2;
            }

            for (xx = 0; xx < 32; xx++)
            {
                *outB++ = g1[xx] >> 4;
            }
            for (xx = 0; xx < 32; xx += 2)
            {
                *outN++ = ((g1[xx] << 4) & 0xf0) | (g1[xx + 1] & 0xf);
            }
        }
#endif
    }

    return 3 * width * 4 * height / 2;
}



int ConvertRGB10ToDPX0(uint8_t *data, int pitch, int width, int height, int unc_format)
{
    int row, x;

    for (row = 0; row < height; row++)
    {
        uint32_t val, *sptr;
        int r, g, b;

        sptr = (uint32_t *)data;
        sptr += row * (pitch >> 2);

        switch (unc_format)
        {
            case COLOR_FORMAT_RG30://rg30 A2B10G10R10
            case COLOR_FORMAT_AB10:
                for (x = 0; x < width; x++)
                {
                    val = *sptr;
                    r = val & 0x3ff;
                    val >>= 10;
                    g = val & 0x3ff;
                    val >>= 10;
                    b = val & 0x3ff;

                    r <<= 22;
                    g <<= 12;
                    b <<= 2;
                    val = r;
                    val |= g;
                    val |= b;
                    *sptr++ = SwapInt32(val);
                }
                break;
            case COLOR_FORMAT_R210:
                for (x = 0; x < width; x++)
                {
                    val = SwapInt32(*sptr);
                    b = val & 0x3ff;
                    val >>= 10;
                    g = val & 0x3ff;
                    val >>= 10;
                    r = val & 0x3ff;

                    r <<= 22;
                    g <<= 12;
                    b <<= 2;
                    val = r;
                    val |= g;
                    val |= b;
                    *sptr++ = SwapInt32(val);
                }
                break;
            case COLOR_FORMAT_AR10:
                for (x = 0; x < width; x++)
                {
                    val = *sptr;
                    b = val & 0x3ff;
                    val >>= 10;
                    g = val & 0x3ff;
                    val >>= 10;
                    r = val & 0x3ff;

                    r <<= 22;
                    g <<= 12;
                    b <<= 2;
                    val = r;
                    val |= g;
                    val |= b;
                    *sptr++ = SwapInt32(val);
                }
                break;
        }
    }

    return width * 4 * height;
}


int ConvertDPX0ToRGB10(uint8_t *data, int pitch, int width, int height, int unc_format)
{
    int row, x;

    for (row = 0; row < height; row++)
    {
        uint32_t val, *sptr;
        int r, g, b;

        sptr = (uint32_t *)data;
        sptr += row * (pitch >> 2);

        switch (unc_format)
        {
            case COLOR_FORMAT_RG30://rg30 A2B10G10R10
            case COLOR_FORMAT_AB10:
                for (x = 0; x < width; x++)
                {
                    val = SwapInt32(*sptr);
                    val >>= 2;
                    b = val & 0x3ff;
                    val >>= 10;
                    g = val & 0x3ff;
                    val >>= 10;
                    r = val & 0x3ff;

                    r <<= 0;
                    g <<= 10;
                    b <<= 20;
                    val = r;
                    val |= g;
                    val |= b;
                    *sptr++ = val;
                }
                break;
            case COLOR_FORMAT_R210:
                for (x = 0; x < width; x++)
                {
                    val = SwapInt32(*sptr);
                    val >>= 2;
                    b = val & 0x3ff;
                    val >>= 10;
                    g = val & 0x3ff;
                    val >>= 10;
                    r = val & 0x3ff;

                    r <<= 20;
                    g <<= 10;
                    b <<= 0;
                    val = r;
                    val |= g;
                    val |= b;
                    *sptr++ = SwapInt32(val);
                }
                break;
            case COLOR_FORMAT_AR10:
                for (x = 0; x < width; x++)
                {
                    val = SwapInt32(*sptr);
                    val >>= 2;
                    b = val & 0x3ff;
                    val >>= 10;
                    g = val & 0x3ff;
                    val >>= 10;
                    r = val & 0x3ff;

                    r <<= 20;
                    g <<= 10;
                    b <<= 0;
                    val = r;
                    val |= g;
                    val |= b;
                    *sptr++ = val;
                }
                break;
        }
    }

    return width * 4 * height;
}


int ConvertBYR4ToPacked(uint8_t *data, int pitch, int width, int height, uint8_t *buffer, int bayer_format)
{
    int row, x;


    for (row = 0; row < height; row++)
    {
        PIXEL16U *sptr1, *sptr2;
        uint8_t *outB, *outN;
        uint8_t *outBR, *outNR;
        uint8_t *outBG1, *outNG1;
        uint8_t *outBG2, *outNG2;
        uint8_t *outBB, *outNB;

        sptr1 = (PIXEL16U *)data;
        sptr1 += row * (pitch >> 1);
        sptr2 = sptr1 + (pitch >> 2);

        outB = (uint8_t *)buffer;
        outB += row * width * 4 * 3 / 2; //12-bit
        outN = outB;
        outN += width * 4;

        outBR = outB;
        outBG1 = outBR + width;
        outBG2 = outBG1 + width;
        outBB = outBG2 + width;
        outNR = outBB + width;
        outNG1 = outNR + (width >> 1);
        outNG2 = outNG1 + (width >> 1);
        outNB = outNG2 + (width >> 1);

#if 1
        {
            __m128i rg_epi16;
            __m128i gb_epi16;
            __m128i gr_epi16;
            __m128i bg_epi16;
            __m128i t_epi16;
            __m128i r_epi16;
            __m128i g1_epi16;
            __m128i g2_epi16;
            __m128i b_epi16;
            __m128i Br_epi16;
            __m128i Bg1_epi16;
            __m128i Bg2_epi16;
            __m128i Bb_epi16;
            __m128i Nr_epi16;
            __m128i Ng1_epi16;
            __m128i Ng2_epi16;
            __m128i Nb_epi16;
            __m128i Brb_epi16;
            __m128i Bg1b_epi16;
            __m128i Bg2b_epi16;
            __m128i Bbb_epi16;
            __m128i Nra_epi16;
            __m128i Ng1a_epi16;
            __m128i Ng2a_epi16;
            __m128i Nba_epi16;
            __m128i Nrb_epi16;
            __m128i Ng1b_epi16;
            __m128i Ng2b_epi16;
            __m128i Nbb_epi16;
            __m128i Nrc_epi16;
            __m128i Ng1c_epi16;
            __m128i Ng2c_epi16;
            __m128i Nbc_epi16;
            __m128i Nrd_epi16;
            __m128i Ng1d_epi16;
            __m128i Ng2d_epi16;
            __m128i Nbd_epi16;
            __m128i ZeroHi = _mm_set1_epi32(0x0000ffff);
            __m128i MaskHi = _mm_set1_epi16(0x00f0);

            __m128i *src1_epi16 = (__m128i *)sptr1;
            __m128i *src2_epi16 = (__m128i *)sptr2;
            __m128i *outBR_epi16 = (__m128i *)outBR;
            __m128i *outBG1_epi16 = (__m128i *)outBG1;
            __m128i *outBG2_epi16 = (__m128i *)outBG2;
            __m128i *outBB_epi16 = (__m128i *)outBB;
            __m128i *outNR_epi16 = (__m128i *)outNR;
            __m128i *outNG1_epi16 = (__m128i *)outNG1;
            __m128i *outNG2_epi16 = (__m128i *)outNG2;
            __m128i *outNB_epi16 = (__m128i *)outNB;


            switch (bayer_format)
            {
                case BAYER_FORMAT_RED_GRN: //Red-grn phase
                    for (x = 0; x < width; x += 32) //R,G1,R,G1... G2,B,G2,B...
                    {
                        /*	int xx;
                        	sptr1 = (PIXEL16U *)src1_epi16;
                        	sptr2 = (PIXEL16U *)src2_epi16;
                        	for(xx=0;xx<32;xx++)
                        	{
                        		sptr1[xx*2] = (x+xx)<<4;
                        		sptr1[xx*2+1] = (x+xx)<<4;
                        		sptr2[xx*2] = (x+xx)<<4;
                        		sptr2[xx*2+1] = (x+xx)<<4;
                        	}*/

                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        r_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);


                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        g2_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);


                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        r_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);


                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        g2_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrc_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1c_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2c_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbc_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        r_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);


                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        g2_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);

                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        r_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        rg_epi16 = _mm_load_si128(src1_epi16++);
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);


                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        g2_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        gb_epi16 = _mm_load_si128(src2_epi16++);
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrd_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1d_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2d_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbd_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        Nra_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00600040002000000
                        Nrc_epi16 = _mm_srli_epi16(Nrc_epi16, 8);	  //00706050403020100
                        Nrc_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00700050003000100
                        Nrb_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00e000c000a000800
                        Nrd_epi16 = _mm_srli_epi16(Nrd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nrd_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00f000d000b000900
                        Nra_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Nra_epi16 = _mm_srli_epi16(Nra_epi16, 4);
                        Nrb_epi16 = _mm_packus_epi16(Nrc_epi16, Nrd_epi16);
                        Nr_epi16 = _mm_or_si128(Nra_epi16, Nrb_epi16);

                        Ng1a_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00600040002000000
                        Ng1c_epi16 = _mm_srli_epi16(Ng1c_epi16, 8);	  //00706050403020100
                        Ng1c_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00700050003000100
                        Ng1b_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00e000c000a000800
                        Ng1d_epi16 = _mm_srli_epi16(Ng1d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng1d_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00f000d000b000900
                        Ng1a_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng1a_epi16 = _mm_srli_epi16(Ng1a_epi16, 4);
                        Ng1b_epi16 = _mm_packus_epi16(Ng1c_epi16, Ng1d_epi16);
                        Ng1_epi16 = _mm_or_si128(Ng1a_epi16, Ng1b_epi16);

                        Ng2a_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00600040002000000
                        Ng2c_epi16 = _mm_srli_epi16(Ng2c_epi16, 8);	  //00706050403020100
                        Ng2c_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00700050003000100
                        Ng2b_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00e000c000a000800
                        Ng2d_epi16 = _mm_srli_epi16(Ng2d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng2d_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00f000d000b000900
                        Ng2a_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Ng2a_epi16 = _mm_srli_epi16(Ng2a_epi16, 4);
                        Ng2b_epi16 = _mm_packus_epi16(Ng2c_epi16, Ng2d_epi16);
                        Ng2_epi16 = _mm_or_si128(Ng2a_epi16, Ng2b_epi16);

                        Nba_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00600040002000000
                        Nbc_epi16 = _mm_srli_epi16(Nbc_epi16, 8);	  //00706050403020100
                        Nbc_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00700050003000100
                        Nbb_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00e000c000a000800
                        Nbd_epi16 = _mm_srli_epi16(Nbd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nbd_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00f000d000b000900
                        Nba_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);
                        Nba_epi16 = _mm_srli_epi16(Nba_epi16, 4);
                        Nbb_epi16 = _mm_packus_epi16(Nbc_epi16, Nbd_epi16);
                        Nb_epi16 = _mm_or_si128(Nba_epi16, Nbb_epi16);


                        _mm_store_si128(outNR_epi16++, Nr_epi16);
                        _mm_store_si128(outNG1_epi16++, Ng1_epi16);
                        _mm_store_si128(outNG2_epi16++, Ng2_epi16);
                        _mm_store_si128(outNB_epi16++, Nb_epi16);

                    }
                    break;
                case BAYER_FORMAT_GRN_RED:// grn-red
                    for (x = 0; x < width; x += 32) //R,G1,R,G1... G2,B,G2,B...
                    {
                        /*	int xx;
                        	sptr1 = (PIXEL16U *)src1_epi16;
                        	sptr2 = (PIXEL16U *)src2_epi16;
                        	for(xx=0;xx<32;xx++)
                        	{
                        		sptr1[xx*2] = (x+xx)<<4;
                        		sptr1[xx*2+1] = (x+xx)<<4;
                        		sptr2[xx*2] = (x+xx)<<4;
                        		sptr2[xx*2+1] = (x+xx)<<4;
                        	}*/

                        rg_epi16 = _mm_load_si128(src1_epi16++);		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        g1_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        rg_epi16 = _mm_load_si128(src1_epi16++);  		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gb_epi16 = _mm_load_si128(src2_epi16++);		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gb_epi16, 16);		//0,g,0,g,0,g,0,g
                        b_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        gb_epi16 = _mm_load_si128(src2_epi16++);  		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b


                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        rg_epi16 = _mm_load_si128(src1_epi16++);		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        g1_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        rg_epi16 = _mm_load_si128(src1_epi16++);  		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gb_epi16 = _mm_load_si128(src2_epi16++);		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gb_epi16, 16);		//0,g,0,g,0,g,0,g
                        b_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        gb_epi16 = _mm_load_si128(src2_epi16++);  		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrc_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1c_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2c_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbc_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        rg_epi16 = _mm_load_si128(src1_epi16++);		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        g1_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        rg_epi16 = _mm_load_si128(src1_epi16++);  		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gb_epi16 = _mm_load_si128(src2_epi16++);		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gb_epi16, 16);		//0,g,0,g,0,g,0,g
                        b_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        gb_epi16 = _mm_load_si128(src2_epi16++);  		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b

                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        rg_epi16 = _mm_load_si128(src1_epi16++);		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        g1_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        rg_epi16 = _mm_load_si128(src1_epi16++);  		//r,g,r,g,r,g,r,g
                        rg_epi16 = _mm_srli_epi16(rg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(rg_epi16, 16);			//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r
                        t_epi16 = _mm_and_si128(rg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gb_epi16 = _mm_load_si128(src2_epi16++);		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gb_epi16, 16);		//0,g,0,g,0,g,0,g
                        b_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        gb_epi16 = _mm_load_si128(src2_epi16++);  		//g,b,g,b,g,b,g,b
                        gb_epi16 = _mm_srli_epi16(gb_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gb_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gb_epi16, ZeroHi);		//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrd_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1d_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2d_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbd_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        Nra_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00600040002000000
                        Nrc_epi16 = _mm_srli_epi16(Nrc_epi16, 8);	  //00706050403020100
                        Nrc_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00700050003000100
                        Nrb_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00e000c000a000800
                        Nrd_epi16 = _mm_srli_epi16(Nrd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nrd_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00f000d000b000900
                        Nra_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Nra_epi16 = _mm_srli_epi16(Nra_epi16, 4);
                        Nrb_epi16 = _mm_packus_epi16(Nrc_epi16, Nrd_epi16);
                        Nr_epi16 = _mm_or_si128(Nra_epi16, Nrb_epi16);

                        Ng1a_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00600040002000000
                        Ng1c_epi16 = _mm_srli_epi16(Ng1c_epi16, 8);	  //00706050403020100
                        Ng1c_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00700050003000100
                        Ng1b_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00e000c000a000800
                        Ng1d_epi16 = _mm_srli_epi16(Ng1d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng1d_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00f000d000b000900
                        Ng1a_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng1a_epi16 = _mm_srli_epi16(Ng1a_epi16, 4);
                        Ng1b_epi16 = _mm_packus_epi16(Ng1c_epi16, Ng1d_epi16);
                        Ng1_epi16 = _mm_or_si128(Ng1a_epi16, Ng1b_epi16);

                        Ng2a_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00600040002000000
                        Ng2c_epi16 = _mm_srli_epi16(Ng2c_epi16, 8);	  //00706050403020100
                        Ng2c_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00700050003000100
                        Ng2b_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00e000c000a000800
                        Ng2d_epi16 = _mm_srli_epi16(Ng2d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng2d_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00f000d000b000900
                        Ng2a_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Ng2a_epi16 = _mm_srli_epi16(Ng2a_epi16, 4);
                        Ng2b_epi16 = _mm_packus_epi16(Ng2c_epi16, Ng2d_epi16);
                        Ng2_epi16 = _mm_or_si128(Ng2a_epi16, Ng2b_epi16);

                        Nba_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00600040002000000
                        Nbc_epi16 = _mm_srli_epi16(Nbc_epi16, 8);	  //00706050403020100
                        Nbc_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00700050003000100
                        Nbb_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00e000c000a000800
                        Nbd_epi16 = _mm_srli_epi16(Nbd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nbd_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00f000d000b000900
                        Nba_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);
                        Nba_epi16 = _mm_srli_epi16(Nba_epi16, 4);
                        Nbb_epi16 = _mm_packus_epi16(Nbc_epi16, Nbd_epi16);
                        Nb_epi16 = _mm_or_si128(Nba_epi16, Nbb_epi16);


                        _mm_store_si128(outNR_epi16++, Nr_epi16);
                        _mm_store_si128(outNG1_epi16++, Ng1_epi16);
                        _mm_store_si128(outNG2_epi16++, Ng2_epi16);
                        _mm_store_si128(outNB_epi16++, Nb_epi16);

                    }
                    break;
                case BAYER_FORMAT_GRN_BLU:
                    for (x = 0; x < width; x += 32) //G1,B,G1,B... R,G2,R,G2...
                    {
                        /*	int xx;
                        	sptr1 = (PIXEL16U *)src1_epi16;
                        	sptr2 = (PIXEL16U *)src2_epi16;
                        	for(xx=0;xx<32;xx++)
                        	{
                        		sptr1[xx*2] = (x+xx)<<4;
                        		sptr1[xx*2+1] = (x+xx)<<4;
                        		sptr2[xx*2] = (x+xx)<<4;
                        		sptr2[xx*2+1] = (x+xx)<<4;
                        	}*/

                        bg_epi16 = _mm_load_si128(src1_epi16++);		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        g1_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        bg_epi16 = _mm_load_si128(src1_epi16++);  		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gr_epi16 = _mm_load_si128(src2_epi16++);		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gr_epi16, 16);		//0,g,0,g,0,g,0,g
                        r_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        gr_epi16 = _mm_load_si128(src2_epi16++);  		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r


                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        bg_epi16 = _mm_load_si128(src1_epi16++);		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        g1_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        bg_epi16 = _mm_load_si128(src1_epi16++);  		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gr_epi16 = _mm_load_si128(src2_epi16++);		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gr_epi16, 16);		//0,g,0,g,0,g,0,g
                        r_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        gr_epi16 = _mm_load_si128(src2_epi16++);  		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r


                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrc_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1c_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2c_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbc_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        bg_epi16 = _mm_load_si128(src1_epi16++);		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        g1_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        bg_epi16 = _mm_load_si128(src1_epi16++);  		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gr_epi16 = _mm_load_si128(src2_epi16++);		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gr_epi16, 16);		//0,g,0,g,0,g,0,g
                        r_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        gr_epi16 = _mm_load_si128(src2_epi16++);  		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r

                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        bg_epi16 = _mm_load_si128(src1_epi16++);		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        b_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        g1_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        bg_epi16 = _mm_load_si128(src1_epi16++);  		//b,g,b,g,b,g,b,g
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);			//0,b,0,b,0,b,0,b
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);	//b,b,b,b,b,b,b,b
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);		//0,g,0,g,0,g,0,g
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);	//g,g,g,g,g,g,g,g


                        gr_epi16 = _mm_load_si128(src2_epi16++);		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        g2_epi16 = _mm_srli_epi32(gr_epi16, 16);		//0,g,0,g,0,g,0,g
                        r_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        gr_epi16 = _mm_load_si128(src2_epi16++);  		//g,r,g,r,g,r,g,r
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);			//shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);			//0,g,0,g,0,g,0,g
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);	//g,g,g,g,g,g,g,g
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);		//0,r,0,r,0,r,0,r
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);	//r,r,r,r,r,r,r,r

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrd_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1d_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2d_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbd_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        Nra_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00600040002000000
                        Nrc_epi16 = _mm_srli_epi16(Nrc_epi16, 8);	  //00706050403020100
                        Nrc_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00700050003000100
                        Nrb_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00e000c000a000800
                        Nrd_epi16 = _mm_srli_epi16(Nrd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nrd_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00f000d000b000900
                        Nra_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Nra_epi16 = _mm_srli_epi16(Nra_epi16, 4);
                        Nrb_epi16 = _mm_packus_epi16(Nrc_epi16, Nrd_epi16);
                        Nr_epi16 = _mm_or_si128(Nra_epi16, Nrb_epi16);

                        Ng1a_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00600040002000000
                        Ng1c_epi16 = _mm_srli_epi16(Ng1c_epi16, 8);	  //00706050403020100
                        Ng1c_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00700050003000100
                        Ng1b_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00e000c000a000800
                        Ng1d_epi16 = _mm_srli_epi16(Ng1d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng1d_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00f000d000b000900
                        Ng1a_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng1a_epi16 = _mm_srli_epi16(Ng1a_epi16, 4);
                        Ng1b_epi16 = _mm_packus_epi16(Ng1c_epi16, Ng1d_epi16);
                        Ng1_epi16 = _mm_or_si128(Ng1a_epi16, Ng1b_epi16);

                        Ng2a_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00600040002000000
                        Ng2c_epi16 = _mm_srli_epi16(Ng2c_epi16, 8);	  //00706050403020100
                        Ng2c_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00700050003000100
                        Ng2b_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00e000c000a000800
                        Ng2d_epi16 = _mm_srli_epi16(Ng2d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng2d_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00f000d000b000900
                        Ng2a_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Ng2a_epi16 = _mm_srli_epi16(Ng2a_epi16, 4);
                        Ng2b_epi16 = _mm_packus_epi16(Ng2c_epi16, Ng2d_epi16);
                        Ng2_epi16 = _mm_or_si128(Ng2a_epi16, Ng2b_epi16);

                        Nba_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00600040002000000
                        Nbc_epi16 = _mm_srli_epi16(Nbc_epi16, 8);	  //00706050403020100
                        Nbc_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00700050003000100
                        Nbb_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00e000c000a000800
                        Nbd_epi16 = _mm_srli_epi16(Nbd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nbd_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00f000d000b000900
                        Nba_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);
                        Nba_epi16 = _mm_srli_epi16(Nba_epi16, 4);
                        Nbb_epi16 = _mm_packus_epi16(Nbc_epi16, Nbd_epi16);
                        Nb_epi16 = _mm_or_si128(Nba_epi16, Nbb_epi16);


                        _mm_store_si128(outNR_epi16++, Nr_epi16);
                        _mm_store_si128(outNG1_epi16++, Ng1_epi16);
                        _mm_store_si128(outNG2_epi16++, Ng2_epi16);
                        _mm_store_si128(outNB_epi16++, Nb_epi16);
                    }
                    break;

                case BAYER_FORMAT_BLU_GRN:
                    for (x = 0; x < width; x += 32) //B,G1,B,G1... G2,R,G2,R...
                    {
                        /*	int xx;
                        	sptr1 = (PIXEL16U *)src1_epi16;
                        	sptr2 = (PIXEL16U *)src2_epi16;
                        	for(xx=0;xx<32;xx++)
                        	{
                        		sptr1[xx*2] = (x+xx)<<4;
                        		sptr1[xx*2+1] = (x+xx)<<4;
                        		sptr2[xx*2] = (x+xx)<<4;
                        		sptr2[xx*2+1] = (x+xx)<<4;
                        	}*/

                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        b_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);


                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        g2_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);


                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        b_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);


                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        g2_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrc_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1c_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2c_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbc_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        b_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);


                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        g2_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);

                        Br_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nra_epi16 = _mm_and_si128(Nra_epi16, MaskHi); //0x00f0
                        Bg1_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1a_epi16 = _mm_and_si128(Ng1a_epi16, MaskHi); //0x00f0
                        Bg2_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2a_epi16 = _mm_and_si128(Ng2a_epi16, MaskHi); //0x00f0
                        Bb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nba_epi16 = _mm_and_si128(Nba_epi16, MaskHi); //0x00f0



                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        g1_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        b_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        bg_epi16 = _mm_load_si128(src1_epi16++);
                        bg_epi16 = _mm_srli_epi16(bg_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(bg_epi16, 16);
                        g1_epi16 = _mm_packs_epi32(g1_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(bg_epi16, ZeroHi);
                        b_epi16 = _mm_packs_epi32(b_epi16, t_epi16);


                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        r_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        g2_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        gr_epi16 = _mm_load_si128(src2_epi16++);
                        gr_epi16 = _mm_srli_epi16(gr_epi16, 4);	 //shift 16-bit to 12-bit
                        t_epi16 = _mm_srli_epi32(gr_epi16, 16);
                        r_epi16 = _mm_packs_epi32(r_epi16, t_epi16);
                        t_epi16 = _mm_and_si128(gr_epi16, ZeroHi);
                        g2_epi16 = _mm_packs_epi32(g2_epi16, t_epi16);

                        Brb_epi16 = _mm_srli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_slli_epi16(r_epi16, 4);
                        Nrb_epi16 = _mm_and_si128(Nrb_epi16, MaskHi); //0x00f0
                        Bg1b_epi16 = _mm_srli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_slli_epi16(g1_epi16, 4);
                        Ng1b_epi16 = _mm_and_si128(Ng1b_epi16, MaskHi); //0x00f0
                        Bg2b_epi16 = _mm_srli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_slli_epi16(g2_epi16, 4);
                        Ng2b_epi16 = _mm_and_si128(Ng2b_epi16, MaskHi); //0x00f0
                        Bbb_epi16 = _mm_srli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_slli_epi16(b_epi16, 4);
                        Nbb_epi16 = _mm_and_si128(Nbb_epi16, MaskHi); //0x00f0


                        Br_epi16 = _mm_packus_epi16(Br_epi16, Brb_epi16);
                        Bg1_epi16 = _mm_packus_epi16(Bg1_epi16, Bg1b_epi16);
                        Bg2_epi16 = _mm_packus_epi16(Bg2_epi16, Bg2b_epi16);
                        Bb_epi16 = _mm_packus_epi16(Bb_epi16, Bbb_epi16);

                        _mm_store_si128(outBR_epi16++, Br_epi16);
                        _mm_store_si128(outBG1_epi16++, Bg1_epi16);
                        _mm_store_si128(outBG2_epi16++, Bg2_epi16);
                        _mm_store_si128(outBB_epi16++, Bb_epi16);

                        Nrd_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Ng1d_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng2d_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Nbd_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);


                        Nra_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00600040002000000
                        Nrc_epi16 = _mm_srli_epi16(Nrc_epi16, 8);	  //00706050403020100
                        Nrc_epi16 = _mm_and_si128(Nrc_epi16, MaskHi); //00700050003000100
                        Nrb_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00e000c000a000800
                        Nrd_epi16 = _mm_srli_epi16(Nrd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nrd_epi16 = _mm_and_si128(Nrd_epi16, MaskHi); //00f000d000b000900
                        Nra_epi16 = _mm_packus_epi16(Nra_epi16, Nrb_epi16);
                        Nra_epi16 = _mm_srli_epi16(Nra_epi16, 4);
                        Nrb_epi16 = _mm_packus_epi16(Nrc_epi16, Nrd_epi16);
                        Nr_epi16 = _mm_or_si128(Nra_epi16, Nrb_epi16);

                        Ng1a_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00600040002000000
                        Ng1c_epi16 = _mm_srli_epi16(Ng1c_epi16, 8);	  //00706050403020100
                        Ng1c_epi16 = _mm_and_si128(Ng1c_epi16, MaskHi); //00700050003000100
                        Ng1b_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00e000c000a000800
                        Ng1d_epi16 = _mm_srli_epi16(Ng1d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng1d_epi16 = _mm_and_si128(Ng1d_epi16, MaskHi); //00f000d000b000900
                        Ng1a_epi16 = _mm_packus_epi16(Ng1a_epi16, Ng1b_epi16);
                        Ng1a_epi16 = _mm_srli_epi16(Ng1a_epi16, 4);
                        Ng1b_epi16 = _mm_packus_epi16(Ng1c_epi16, Ng1d_epi16);
                        Ng1_epi16 = _mm_or_si128(Ng1a_epi16, Ng1b_epi16);

                        Ng2a_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00600040002000000
                        Ng2c_epi16 = _mm_srli_epi16(Ng2c_epi16, 8);	  //00706050403020100
                        Ng2c_epi16 = _mm_and_si128(Ng2c_epi16, MaskHi); //00700050003000100
                        Ng2b_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00e000c000a000800
                        Ng2d_epi16 = _mm_srli_epi16(Ng2d_epi16, 8);	  //00f0e0d0c0b0a0908
                        Ng2d_epi16 = _mm_and_si128(Ng2d_epi16, MaskHi); //00f000d000b000900
                        Ng2a_epi16 = _mm_packus_epi16(Ng2a_epi16, Ng2b_epi16);
                        Ng2a_epi16 = _mm_srli_epi16(Ng2a_epi16, 4);
                        Ng2b_epi16 = _mm_packus_epi16(Ng2c_epi16, Ng2d_epi16);
                        Ng2_epi16 = _mm_or_si128(Ng2a_epi16, Ng2b_epi16);

                        Nba_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00600040002000000
                        Nbc_epi16 = _mm_srli_epi16(Nbc_epi16, 8);	  //00706050403020100
                        Nbc_epi16 = _mm_and_si128(Nbc_epi16, MaskHi); //00700050003000100
                        Nbb_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00e000c000a000800
                        Nbd_epi16 = _mm_srli_epi16(Nbd_epi16, 8);	  //00f0e0d0c0b0a0908
                        Nbd_epi16 = _mm_and_si128(Nbd_epi16, MaskHi); //00f000d000b000900
                        Nba_epi16 = _mm_packus_epi16(Nba_epi16, Nbb_epi16);
                        Nba_epi16 = _mm_srli_epi16(Nba_epi16, 4);
                        Nbb_epi16 = _mm_packus_epi16(Nbc_epi16, Nbd_epi16);
                        Nb_epi16 = _mm_or_si128(Nba_epi16, Nbb_epi16);


                        _mm_store_si128(outNR_epi16++, Nr_epi16);
                        _mm_store_si128(outNG1_epi16++, Ng1_epi16);
                        _mm_store_si128(outNG2_epi16++, Ng2_epi16);
                        _mm_store_si128(outNB_epi16++, Nb_epi16);
                    }
                    break;
            }
        }
#else
        for (x = 0; x < width; x += 8) //R,G1,R,G1... G2,B,G2,B...
        {
            int xx;
            /*for(xx=0;xx<8;xx++)
            {
            	sptr1[xx*2] = (x+xx)<<4;
            	sptr1[xx*2+1] = (x+xx)<<4;
            	sptr2[xx*2] = (x+xx)<<4;
            	sptr2[xx*2+1] = (x+xx)<<4;
            }*/
            switch (bayer_format)
            {
                case BAYER_FORMAT_RED_GRN: //Red-grn phase
                    for (xx = 0; xx < 8; xx++)
                    {
                        r[xx] = *sptr1++ >> 4;
                        g1[xx] = *sptr1++ >> 4;
                        g2[xx] = *sptr2++ >> 4;
                        b[xx] = *sptr2++ >> 4;
                    }
                    break;
                case BAYER_FORMAT_GRN_RED:// grn-red
                    for (xx = 0; xx < 8; xx++)
                    {
                        g1[xx] = *sptr1++ >> 4;
                        r[xx] = *sptr1++ >> 4;
                        b[xx] = *sptr2++ >> 4;
                        g2[xx] = *sptr2++ >> 4;
                    }
                    break;
                case BAYER_FORMAT_GRN_BLU:
                    for (xx = 0; xx < 8; xx++)
                    {
                        g1[xx] = *sptr1++ >> 4;
                        b[xx] = *sptr1++ >> 4;
                        r[xx] = *sptr2++ >> 4;
                        g2[xx] = *sptr2++ >> 4;
                    }
                    break;
                case BAYER_FORMAT_BLU_GRN:
                    for (xx = 0; xx < 8; xx++)
                    {
                        b[xx] = *sptr1++ >> 4;
                        g1[xx] = *sptr1++ >> 4;
                        g2[xx] = *sptr2++ >> 4;
                        r[xx] = *sptr2++ >> 4;
                    }
                    break;
            }

            for (xx = 0; xx < 8; xx++)
            {
                *outBR++ = r[xx] >> 4; //  top 8-bits
                *outBG1++ = g1[xx] >> 4;
                *outBG2++ = g2[xx] >> 4;
                *outBB++ = b[xx] >> 4;
            }
            for (xx = 0; xx < 8; xx += 2)
            {
                *outNR++ = ((r[xx + 1] << 4) & 0xf0) | (r[xx] & 0xf);
                *outNG1++ = ((g1[xx + 1] << 4) & 0xf0) | (g1[xx] & 0xf);
                *outNG2++ = ((g2[xx + 1] << 4) & 0xf0) | (g2[xx] & 0xf);
                *outNB++ = ((b[xx + 1] << 4) & 0xf0) | (b[xx] & 0xf);
            }
        }
#endif
    }

    return 3 * width * 4 * height / 2;
}


// Convert the packed 16-bit BAYER RGB to planes of 16-bit RGBA
void ConvertBYR3ToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    IMAGE *g_image;
    IMAGE *rg_diff_image;
    IMAGE *bg_diff_image;
    IMAGE *gdiff_image;
    PIXEL *byr2_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *gdiff_row_ptr;
    PIXEL *rg_row_ptr;
    PIXEL *bg_row_ptr;
    int byr1_pitch;
    int width;
    int height;
    int display_height;
    int row;
    int i, x;
#if USE_GAMMA_TABLE
    unsigned short gamma12bit[4096];
#endif

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be four channels of RGBA
    assert(frame->num_channels == 4);
    assert(frame->format == FRAME_FORMAT_RGBA);

    g_image = frame->channel[0];					// Get the individual planes
    rg_diff_image = frame->channel[1];
    bg_diff_image = frame->channel[2];
    gdiff_image = frame->channel[3];

    byr2_row_ptr = (PIXEL *)data;					// Pointers to the rows
    g_row_ptr = g_image->band[0];
    rg_row_ptr = rg_diff_image->band[0];
    bg_row_ptr = bg_diff_image->band[0];
    gdiff_row_ptr = gdiff_image->band[0];

    byr1_pitch = g_image->pitch / sizeof(PIXEL16S);

    width = g_image->width;							// Dimensions of the luma image
    height = g_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    // The output pitch should be a positive number (no image inversion)
    assert(byr1_pitch > 0);

    // for the SEQ speed test on my 2.5 P4 I get 56fps this the C code.
#if BYR3_USE_GAMMA_TABLE

#define BYR3_GAMMATABLE(x,y)  (  (int)(pow( (double)(x)/4095.0, (y) )*1023.0)  )

#define BYR3_GAMMA2(x)  ((x)>>2)
    //#define GAMMA2(x)  ( gamma12bit[(x)] )
    //#define GAMMA2(x)  (  (int)(pow( double(x)/4096.0, 1.0/2.0 )*256.0)  )
    //inline int GAMMA2(int x)  {  int v = 4095-(int)(x);  return ((4095 - ((v*v)>>12))>>4);  }

    {
        int blacklevel = 0;//100;
        float fgamma = 1.0;//2.2;

        for (i = 0; i < 4096; i++)
        {
            int j = (i - blacklevel) * 4096 / (4096 - blacklevel);
            if (j < 0) j = 0;
            gamma12bit[i] = BYR3_GAMMATABLE(j, 1.0 / fgamma);
        }
    }

    {
#define LINMAX 40
        float linearmax = (float)gamma12bit[LINMAX];
        float linearstep = linearmax / (float)LINMAX;
        float accum = 0.0;

        for (i = 0; i < 40; i++)
        {
            gamma12bit[i] = accum;
            accum += linearstep;
        }
    }

    for (row = 0; row < display_height; row++)
    {
        PIXEL g, g1, g2, r, b;
        PIXEL *line1, *line2;

        line1 = &byr2_row_ptr[row * pitch / 2];
        line2 = line1 + (pitch >> 2);

        for (x = 0; x < width; x++)
        {
            /*	g1 = *line1++ >> 2;
            	r = *line1++ >> 2;
            	b = *line2++ >> 2;
            	g2 = *line2++ >> 2;*/
#if BYR3_HORIZONTAL_BAYER_SHIFT
            r = BYR3_GAMMA2(*line1++);
            g1 = BYR3_GAMMA2(*line1++);
            g2 = BYR3_GAMMA2(*line2++);
            b = BYR3_GAMMA2(*line2++);
#else
            g1 = BYR3_GAMMA2(*line1++);
            r = BYR3_GAMMA2(*line1++);
            b = BYR3_GAMMA2(*line2++);
            g2 = BYR3_GAMMA2(*line2++);
#endif

            /* 10 bit */
            g = (g1 + g2) >> 1;
            *g_row_ptr++ = g;
            *rg_row_ptr++ = ((r - g) >> 1) + 512;
            *bg_row_ptr++ = ((b - g) >> 1) + 512;
            *gdiff_row_ptr++ = (g1 - g2 + 1024) >> 1;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }
    for (; row < height; row++)
    {
        PIXEL g, g1, g2, r, b;
        PIXEL *line1, *line2;

        for (x = 0; x < width; x++)
        {
            *g_row_ptr++ = 0;
            *rg_row_ptr++ = 0;
            *bg_row_ptr++ = 0;
            *gdiff_row_ptr++ = 0;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }


#else

#if 0 // non-MMX   approx 32fps Medium

    for (row = 0; row < display_height; row++)
    {
        PIXEL g, g1, g2, r, b;
        PIXEL *line1a, *line2a;
        PIXEL *line1b, *line2b;

        line1a = &byr2_row_ptr[row * pitch / 2];
        line2a = line1a + (pitch >> 2);
        line1b = line1a + (pitch >> 3);
        line2b = line2a + (pitch >> 3);

        for (x = 0; x < width; x++)
        {
            r = (*line1a++);
            g1 = (*line1b++);
            g2 = (*line2a++);
            b = (*line2b++);

            /* 10 bit */
            g = (g1 + g2) >> 1;
            *g_row_ptr++ = g;
            *rg_row_ptr++ = ((r - g) >> 1) + 512;
            *bg_row_ptr++ = ((b - g) >> 1) + 512;
            *gdiff_row_ptr++ = (g1 - g2 + 1024) >> 1;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }
    for (; row < height; row++)
    {
        PIXEL g, g1, g2, r, b;
        PIXEL *line1, *line2;

        for (x = 0; x < width; x++)
        {
            *g_row_ptr++ = 0;
            *rg_row_ptr++ = 0;
            *bg_row_ptr++ = 0;
            *gdiff_row_ptr++ = 0;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }

#else  // 38fps Medium

    for (row = 0; row < display_height; row++)
    {
        PIXEL *line1a, *line2a;
        PIXEL *line1b, *line2b;

        line1a = &byr2_row_ptr[row * pitch / 2];
        line2a = line1a + (pitch >> 2);
        line1b = line1a + (pitch >> 3);
        line2b = line2a + (pitch >> 3);

        {

            __m128i *line1aptr_epi16 = (__m128i *)line1a;
            __m128i *line2aptr_epi16 = (__m128i *)line2a;
            __m128i *line1bptr_epi16 = (__m128i *)line1b;
            __m128i *line2bptr_epi16 = (__m128i *)line2b;
            __m128i *gptr_epi16 = (__m128i *)g_row_ptr;
            __m128i *gdiffptr_epi16 = (__m128i *)gdiff_row_ptr;
            __m128i *rgptr_epi16 = (__m128i *)rg_row_ptr;
            __m128i *bgptr_epi16 = (__m128i *)bg_row_ptr;

            __m128i g1_epi16;
            __m128i g2_epi16;
            __m128i r_epi16;
            __m128i b_epi16;

            __m128i g_epi16;
            __m128i gdiff_epi16;
            __m128i rg_epi16;
            __m128i bg_epi16;

            const __m128i rounding_epi16 = _mm_set1_epi16(512);


            for (x = 0; x < width; x += 8)
            {
                // Read the first group of 8 16-bit packed 12-bit pixels
                r_epi16 = _mm_load_si128(line1aptr_epi16++);
                g1_epi16 = _mm_load_si128(line1bptr_epi16++);
                g2_epi16 = _mm_load_si128(line2aptr_epi16++);
                b_epi16 = _mm_load_si128(line2bptr_epi16++);

                g_epi16 = _mm_adds_epi16(g1_epi16, g2_epi16);
                g_epi16 = _mm_srai_epi16(g_epi16, 1);
                _mm_store_si128(gptr_epi16++, g_epi16);

                rg_epi16 = _mm_subs_epi16(r_epi16, g_epi16);
                rg_epi16 = _mm_srai_epi16(rg_epi16, 1);
                rg_epi16 = _mm_adds_epi16(rg_epi16, rounding_epi16);
                _mm_store_si128(rgptr_epi16++, rg_epi16);

                bg_epi16 = _mm_subs_epi16(b_epi16, g_epi16);
                bg_epi16 = _mm_srai_epi16(bg_epi16, 1);
                bg_epi16 = _mm_adds_epi16(bg_epi16, rounding_epi16);
                _mm_store_si128(bgptr_epi16++, bg_epi16);

                gdiff_epi16 = _mm_subs_epi16(g1_epi16, g2_epi16);
                gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding_epi16);
                gdiff_epi16 = _mm_adds_epi16(gdiff_epi16, rounding_epi16);
                gdiff_epi16 = _mm_srai_epi16(gdiff_epi16, 1);
                _mm_store_si128(gdiffptr_epi16++, gdiff_epi16);

            }
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch;// - width;
        rg_row_ptr += byr1_pitch;// - width;
        bg_row_ptr += byr1_pitch;// - width;
        gdiff_row_ptr += byr1_pitch;// - width;
    }
    for (; row < height; row++)
    {
        for (x = 0; x < width; x++)
        {
            *g_row_ptr++ = 0;
            *rg_row_ptr++ = 0;
            *bg_row_ptr++ = 0;
            *gdiff_row_ptr++ = 0;
        }

        // Advance to the next rows in the input and output images
        g_row_ptr += byr1_pitch - width;
        rg_row_ptr += byr1_pitch - width;
        bg_row_ptr += byr1_pitch - width;
        gdiff_row_ptr += byr1_pitch - width;
    }
#endif
#endif


    // Set the image parameters for each channel
    for (i = 0; i < 4; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}

/*!
	@brief Maximum precision for the encoding curve lookpu table

	The maximum is 14 bits as 12 for SI2K/ArriD20, 14 for Dalsa
*/
#define MAX_INPUT_PRECISION	14

void AddCurveToUncompressedBYR4(uint32_t encode_curve, uint32_t encode_curve_preset,
                                uint8_t *data, int pitch, FRAME *frame)
{
    unsigned short curve[1 << MAX_INPUT_PRECISION];
    int precision = 16;


    if (encode_curve_preset == 0)
    {
        int i, row, max_value = 1 << MAX_INPUT_PRECISION;

        //int greylevels =  (1<<precision); // 10-bit = 1024;
        //int midpoint =  greylevels/2; // 10-bit = 512;
        int width = frame->width * 2;
        int height = frame->display_height * 2;
        int encode_curve_type = (encode_curve >> 16);
        //int encode_curve_neg = encode_curve_type & CURVE_TYPE_NEGATIVE;

#define LOGBASE	90
#define BYR4_LOGTABLE(x)  (  (int)( CURVE_LIN2LOG(x,LOGBASE) * (float)((1<<precision)-1))  )
#define BYR4_CURVE(x)  ( curve[(x)] )

        for (i = 0; i < max_value; i++)
        {
            if (encode_curve == 0 || encode_curve == CURVE_LOG_90)
            {
                if (i)
                    BYR4_CURVE(i) = BYR4_LOGTABLE((float)i / (float)max_value);
                else
                    BYR4_CURVE(0) = 0;
            }
            else if ((encode_curve_type & CURVE_TYPE_MASK) == CURVE_TYPE_LOG)
            {
                float logbase;

                if (encode_curve_type & CURVE_TYPE_EXTENDED)
                {
                    logbase = (float)(encode_curve & 0xffff);
                }
                else
                {
                    float num, den;
                    num = (float)((encode_curve >> 8) & 0xff);
                    den = (float)(encode_curve & 0xff);
                    logbase = num / den;
                }

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2LOG((float)i / (float)max_value, logbase) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_CINEON)
            {
                float num, den, logbase;

                num = (float)((encode_curve >> 8) & 0xff);
                den = (float)(encode_curve & 0xff);
                logbase = num / den;

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2CINEON((float)i / (float)max_value, logbase) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_CINE985)
            {
                float num, den, logbase;

                num = (float)((encode_curve >> 8) & 0xff);
                den = (float)(encode_curve & 0xff);
                logbase = num / den;

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2CINE985((float)i / (float)max_value, logbase) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_PARA)
            {
                int gain, power;

                gain = (int)((encode_curve >> 8) & 0xff);
                power = (int)(encode_curve & 0xff);

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2PARA((float)i / (float)max_value, gain, power) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_GAMMA)
            {
                double num, den, gamma;

                num = (double)((encode_curve >> 8) & 0xff);
                den = (double)(encode_curve & 0xff);
                gamma = num / den;

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2GAM((double)((float)i / (float)max_value), gamma) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_CSTYLE)
            {
                int num;
                num = ((encode_curve >> 8) & 0xff);

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2CSTYLE((float)i, num) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_SLOG)
            {
                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2SLOG((float)i) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_LOGC)
            {
                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2LOGC((float)i) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else //if(encode_curve == CURVE_LINEAR) // or for pre-curved sources.
            {
                BYR4_CURVE(i) = (int)(((float)i / (float)max_value) * (float)((1 << precision) - 1));
            }

        }

        for (row = 0; row < height; row++)
        {
            int x;
            uint16_t *line = (uint16_t *)(data + (pitch >> 1) * row);
            for (x = 0; x < width; x++)
            {
                //line[x] = 0x1010;
                line[x] = BYR4_CURVE(line[x] >> (16 - MAX_INPUT_PRECISION));
            }
        }
    }
}

// Convert the packed 16-bit BAYER RGB to planes of 16-bit RGBA
void ConvertBYR4ToFrame16s(int bayer_format, uint32_t encode_curve, uint32_t encode_curve_preset,
                           uint8_t *data, int pitch, FRAME *frame, int precision)
{
    IMAGE *g_image;
    IMAGE *rg_diff_image;
    IMAGE *bg_diff_image;
    IMAGE *gdiff_image;
    PIXEL *byr4_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *gdiff_row_ptr;
    PIXEL *rg_row_ptr;
    PIXEL *bg_row_ptr;
    int byr1_pitch;
    int width;
    int height;
    int display_height;
    int row;
    int i, x;
    int max_value = 1 << MAX_INPUT_PRECISION;

    int greylevels =  (1 << precision); // 10-bit = 1024;
    int midpoint =  greylevels / 2; // 10-bit = 512;

    // Process 16 bytes each of luma and chroma per loop iteration
    //const int column_step = 2 * sizeof(__m64);

    // Column at which post processing must begin
    //int post_column;

    if (frame == NULL) return;

    // The frame format should be four channels of RGBA
    assert(frame->num_channels == 4);
    assert(frame->format == FRAME_FORMAT_RGBA);

    g_image = frame->channel[0];					// Get the individual planes
    rg_diff_image = frame->channel[1];
    bg_diff_image = frame->channel[2];
    gdiff_image = frame->channel[3];

    byr4_row_ptr = (PIXEL *)data;					// Pointers to the rows
    g_row_ptr = g_image->band[0];
    rg_row_ptr = rg_diff_image->band[0];
    bg_row_ptr = bg_diff_image->band[0];
    gdiff_row_ptr = gdiff_image->band[0];

    pitch /= sizeof(PIXEL16S);
    byr1_pitch = g_image->pitch / sizeof(PIXEL16S);

    width = g_image->width;							// Dimensions of the luma image
    height = g_image->height;
    display_height = frame->display_height;

    //post_column = width - (width % column_step);

    if (encode_curve_preset)
    {
        int mid11bit = (1 << (13 - 1));

        for (row = 0; row < height; row++)
        {
            PIXEL16U g1, g2, r, b;
            int gg, rg, bg, dg;
            PIXEL16U *line1, *line2;
            int srcrow = row;

            if (row >= display_height)
                srcrow = display_height - 1;

            line1 = (PIXEL16U *)&byr4_row_ptr[srcrow * pitch];
            line2 = line1 + (pitch >> 1);



            switch (bayer_format)
            {
                case BAYER_FORMAT_RED_GRN:
                    for (x = 0; x < width; x++)
                    {
                        r = (*line1++ >> (16 - precision));
                        g1 = (*line1++ >> (16 - precision));
                        g2 = (*line2++ >> (16 - precision));
                        b = (*line2++ >> (16 - precision));

                        /*	g = (g1+g2)>>1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r-g)>>1)+midpoint;
                        *bg_row_ptr++ = ((b-g)>>1)+midpoint;
                        *gdiff_row_ptr++ = (g1-g2+greylevels)>>1;*/

                        gg = (g1 + g2) >> 1;
                        dg = g1 - g2;

                        rg = r - gg;
                        bg = b - gg;
                        rg += mid11bit;
                        bg += mid11bit;
                        dg += mid11bit;
                        rg >>= 1;
                        bg >>= 1;
                        dg >>= 1;

                        *g_row_ptr++ = gg;
                        *rg_row_ptr++ = rg;
                        *bg_row_ptr++ = bg;
                        *gdiff_row_ptr++ = dg;
                    }
                    break;
                case BAYER_FORMAT_GRN_RED:
                    for (x = 0; x < width; x++)
                    {
                        g1 = (*line1++ >> (16 - precision));
                        r = (*line1++ >> (16 - precision));
                        b = (*line2++ >> (16 - precision));
                        g2 = (*line2++ >> (16 - precision));

                        /*	g = (g1+g2)>>1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r-g)>>1)+midpoint;
                        *bg_row_ptr++ = ((b-g)>>1)+midpoint;
                        *gdiff_row_ptr++ = (g1-g2+greylevels)>>1;*/

                        gg = (g1 + g2) >> 1;
                        dg = g1 - g2;

                        rg = r - gg;
                        bg = b - gg;
                        rg += mid11bit;
                        bg += mid11bit;
                        dg += mid11bit;
                        rg >>= 1;
                        bg >>= 1;
                        dg >>= 1;

                        *g_row_ptr++ = gg;
                        *rg_row_ptr++ = rg;
                        *bg_row_ptr++ = bg;
                        *gdiff_row_ptr++ = dg;
                    }
                    break;
                case BAYER_FORMAT_BLU_GRN:
                    for (x = 0; x < width; x++)
                    {
                        b = (*line1++ >> (16 - precision));
                        g1 = (*line1++ >> (16 - precision));
                        g2 = (*line2++ >> (16 - precision));
                        r = (*line2++ >> (16 - precision));

                        /*	g = (g1+g2)>>1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r-g)>>1)+midpoint;
                        *bg_row_ptr++ = ((b-g)>>1)+midpoint;
                        *gdiff_row_ptr++ = (g1-g2+greylevels)>>1;*/

                        gg = (g1 + g2) >> 1;
                        dg = g1 - g2;

                        rg = r - gg;
                        bg = b - gg;
                        rg += mid11bit;
                        bg += mid11bit;
                        dg += mid11bit;
                        rg >>= 1;
                        bg >>= 1;
                        dg >>= 1;

                        *g_row_ptr++ = gg;
                        *rg_row_ptr++ = rg;
                        *bg_row_ptr++ = bg;
                        *gdiff_row_ptr++ = dg;
                    }
                    break;
                case BAYER_FORMAT_GRN_BLU:
                    for (x = 0; x < width; x++)
                    {
                        g1 = (*line1++ >> (16 - precision));
                        b = (*line1++ >> (16 - precision));
                        r = (*line2++ >> (16 - precision));
                        g2 = (*line2++ >> (16 - precision));

                        /*	g = (g1+g2)>>1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r-g)>>1)+midpoint;
                        *bg_row_ptr++ = ((b-g)>>1)+midpoint;
                        *gdiff_row_ptr++ = (g1-g2+greylevels)>>1;*/

                        gg = (g1 + g2) >> 1;
                        dg = g1 - g2;

                        rg = r - gg;
                        bg = b - gg;
                        rg += mid11bit;
                        bg += mid11bit;
                        dg += mid11bit;
                        rg >>= 1;
                        bg >>= 1;
                        dg >>= 1;

                        *g_row_ptr++ = gg;
                        *rg_row_ptr++ = rg;
                        *bg_row_ptr++ = bg;
                        *gdiff_row_ptr++ = dg;
                    }
                    break;
            }



            // Advance to the next rows in the input and output images
            g_row_ptr += byr1_pitch - width;
            rg_row_ptr += byr1_pitch - width;
            bg_row_ptr += byr1_pitch - width;
            gdiff_row_ptr += byr1_pitch - width;
        }
    }
    else
    {
        unsigned short curve[1 << MAX_INPUT_PRECISION];
        int encode_curve_type = (encode_curve >> 16);
        //int encode_curve_neg = encode_curve_type & CURVE_TYPE_NEGATIVE;

#define LOGBASE	90
        //#define BYR4_LOGTABLE(x)  (  (int)((log((((double)(x))/((double)((1<<MAX_INPUT_PRECISION)-1))) * (double)(LOG-1) + 1.0)/log(LOG))*(double)((1<<precision)-1))  )
#define BYR4_LOGTABLE(x)  (  (int)( CURVE_LIN2LOG(x,LOGBASE) * (float)((1<<precision)-1))  )
#define BYR4_CURVE(x)  ( curve[(x)] )

        for (i = 0; i < max_value; i++)
        {

            if (encode_curve == 0 || encode_curve == CURVE_LOG_90)
            {
                if (i)
                    BYR4_CURVE(i) = BYR4_LOGTABLE((float)i / (float)max_value);
                else
                    BYR4_CURVE(0) = 0;

            }
            else if ((encode_curve_type & CURVE_TYPE_MASK) == CURVE_TYPE_LOG)
            {
                float logbase;

                if (encode_curve_type & CURVE_TYPE_EXTENDED)
                {
                    logbase = (float)(encode_curve & 0xffff);
                }
                else
                {
                    float num, den;
                    num = (float)((encode_curve >> 8) & 0xff);
                    den = (float)(encode_curve & 0xff);
                    logbase = num / den;
                }

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2LOG((float)i / (float)max_value, logbase) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_CINEON)
            {
                float num, den, logbase;

                num = (float)((encode_curve >> 8) & 0xff);
                den = (float)(encode_curve & 0xff);
                logbase = num / den;

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2CINEON((float)i / (float)max_value, logbase) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_CINE985)
            {
                float num, den, logbase;

                num = (float)((encode_curve >> 8) & 0xff);
                den = (float)(encode_curve & 0xff);
                logbase = num / den;

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2CINE985((float)i / (float)max_value, logbase) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_PARA)
            {
                int gain, power;

                gain = (int)((encode_curve >> 8) & 0xff);
                power = (int)(encode_curve & 0xff);

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2PARA((float)i / (float)max_value, gain, power) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_GAMMA)
            {
                double num, den, gamma;

                num = (double)((encode_curve >> 8) & 0xff);
                den = (double)(encode_curve & 0xff);
                gamma = num / den;

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2GAM((double)((float)i / (float)max_value), gamma) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_CSTYLE)
            {
                int num;
                num = ((encode_curve >> 8) & 0xff);

                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2CSTYLE((float)i, num) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_SLOG)
            {
                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2SLOG((float)i) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else if (encode_curve_type == CURVE_TYPE_LOGC)
            {
                if (i)
                    BYR4_CURVE(i) = (int)(CURVE_LIN2LOGC((float)i) * (float)((1 << precision) - 1));
                else
                    BYR4_CURVE(0) = 0;
            }
            else //if(encode_curve == CURVE_LINEAR) // or for pre-curved sources.
            {
                BYR4_CURVE(i) = (int)(((float)i / (float)max_value) * (float)((1 << precision) - 1));
            }

        }

        /*	if(*bayer_format == 0) // unset, therefore scan for pixel order -- really we shouldn't be guessing - WIP
        	{
        		// Red in the first line
        		int tl=0,tr=0,bl=0,br=0,total = 0;
        		for (row = 20; row < display_height-20; row++)
        		{
        			PIXEL16U *line1,*line2;

        			line1 = &byr4_row_ptr[row * pitch/2];
        			line2 = line1 + (pitch>>2);

        			for(x=0; x<width; x++)
        			{
        				tl += (*line1++);
        				tr += (*line1++);
        				bl += (*line2++);
        				br += (*line2++);
        				total++;
        			}

        			if(total > 10000)
        				break;
        		}

        		if(abs(tl-br) > abs(tr-bl))
        		{
        			*bayer_format = BAYER_FORMAT_RED_GRN+4; // +4 to flag as set as RED_GRN is 0
        		}
        		else
        		{
        			*bayer_format = BAYER_FORMAT_GRN_RED;
        		}
        	}*/


        for (row = 0; row < height; row++)
        {
            PIXEL16U g, g1, g2, r, b;
            PIXEL16U *line1, *line2;
            int srcrow = row;

            if (row >= display_height)
                srcrow = display_height - 1;

            line1 = (PIXEL16U *)&byr4_row_ptr[srcrow * width * 4];
            line2 = line1 + (width * 2);

            switch (bayer_format)
            {
                case BAYER_FORMAT_RED_GRN:
                    for (x = 0; x < width; x++)
                    {
                        r = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        g1 = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        g2 = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));
                        b = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));

                        /* 10 bit */
                        g = (g1 + g2) >> 1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r - g) >> 1) + midpoint;
                        *bg_row_ptr++ = ((b - g) >> 1) + midpoint;
                        *gdiff_row_ptr++ = (g1 - g2 + greylevels) >> 1;
                    }
                    break;
                case BAYER_FORMAT_GRN_RED:
                    for (x = 0; x < width; x++)
                    {
                        g1 = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        r = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        b = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));
                        g2 = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));

                        /* 10 bit */
                        g = (g1 + g2) >> 1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r - g) >> 1) + midpoint;
                        *bg_row_ptr++ = ((b - g) >> 1) + midpoint;
                        *gdiff_row_ptr++ = (g1 - g2 + greylevels) >> 1;
                    }
                    break;
                case BAYER_FORMAT_BLU_GRN:
                    for (x = 0; x < width; x++)
                    {
                        b = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        g1 = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        g2 = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));
                        r = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));

                        /* 10 bit */
                        g = (g1 + g2) >> 1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r - g) >> 1) + midpoint;
                        *bg_row_ptr++ = ((b - g) >> 1) + midpoint;
                        *gdiff_row_ptr++ = (g1 - g2 + greylevels) >> 1;
                    }
                    break;
                case BAYER_FORMAT_GRN_BLU:
                    for (x = 0; x < width; x++)
                    {
                        g1 = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        b = BYR4_CURVE(*line1++ >> (16 - MAX_INPUT_PRECISION));
                        r = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));
                        g2 = BYR4_CURVE(*line2++ >> (16 - MAX_INPUT_PRECISION));

                        /* 10 bit */
                        g = (g1 + g2) >> 1;
                        *g_row_ptr++ = g;
                        *rg_row_ptr++ = ((r - g) >> 1) + midpoint;
                        *bg_row_ptr++ = ((b - g) >> 1) + midpoint;
                        *gdiff_row_ptr++ = (g1 - g2 + greylevels) >> 1;
                    }
                    break;
            }

            // Advance to the next rows in the input and output images
            g_row_ptr += byr1_pitch - width;
            rg_row_ptr += byr1_pitch - width;
            bg_row_ptr += byr1_pitch - width;
            gdiff_row_ptr += byr1_pitch - width;
        }
    }

    // Set the image parameters for each channel
    for (i = 0; i < 4; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}



void ConvertBYR5ToFrame16s(int bayer_format, uint8_t *uncompressed_chunk, int pitch, FRAME *frame, uint8_t *scratch)
{
    IMAGE *g_image;
    IMAGE *rg_diff_image;
    IMAGE *bg_diff_image;
    IMAGE *gdiff_image;
    PIXEL *g_row_ptr;
    PIXEL *gdiff_row_ptr;
    PIXEL *rg_row_ptr;
    PIXEL *bg_row_ptr;
    int byr1_pitch;
    int width;
    int height;
    int display_height;
    int i;
    //int max_value = 1<<MAX_INPUT_PRECISION;

    //int greylevels =  (1<<12);
    //int midpoint =  greylevels/2;

    if (frame == NULL) return;

    // The frame format should be four channels of RGBA
    assert(frame->num_channels == 4);
    assert(frame->format == FRAME_FORMAT_RGBA);

    g_image = frame->channel[0];					// Get the individual planes
    rg_diff_image = frame->channel[1];
    bg_diff_image = frame->channel[2];
    gdiff_image = frame->channel[3];

    g_row_ptr = g_image->band[0];
    rg_row_ptr = rg_diff_image->band[0];
    bg_row_ptr = bg_diff_image->band[0];
    gdiff_row_ptr = gdiff_image->band[0];

    pitch /= sizeof(PIXEL16S);
    byr1_pitch = g_image->pitch / sizeof(PIXEL16S);

    width = g_image->width;							// Dimensions of the luma image
    height = g_image->height;
    display_height = frame->display_height;


    {
        int row, x, srcwidth;

        srcwidth = width;

        for (row = 0; row < height; row++)
        {
            PIXEL16U *tptr;
            uint8_t *outB, *outN;
            int srcrow = row;

            tptr = (PIXEL16U *)scratch;
            if (row >= display_height)
                srcrow = display_height - 1;

            outB = (uint8_t *)uncompressed_chunk;
            outB += srcrow * srcwidth * 4 * 3 / 2; //12-bit
            outN = outB;
            outN += srcwidth * 4;

            {
                __m128i g1_epi16;
                __m128i g2_epi16;
                __m128i g3_epi16;
                __m128i g4_epi16;
                __m128i B1_epi16;
                __m128i N1_epi16;
                __m128i B2_epi16;
                __m128i N2_epi16;
                __m128i B3_epi16;
                __m128i N3_epi16;
                __m128i B4_epi16;
                __m128i N4_epi16;
                __m128i zero = _mm_set1_epi16(0);
                __m128i MaskUp = _mm_set1_epi16(0xf0f0);
                __m128i MaskDn = _mm_set1_epi16(0x0f0f);

                __m128i *tmp_epi16 = (__m128i *)tptr;
                __m128i *outB_epi16 = (__m128i *)outB;
                __m128i *outN_epi16 = (__m128i *)outN;


                for (x = 0; x < srcwidth * 4; x += 32)
                {
                    B1_epi16 = _mm_loadu_si128(outB_epi16++);
                    B2_epi16 = _mm_loadu_si128(outB_epi16++);
                    N1_epi16 = _mm_loadu_si128(outN_epi16++);

                    N2_epi16 = _mm_and_si128(N1_epi16, MaskDn);
                    N1_epi16 = _mm_and_si128(N1_epi16, MaskUp);
                    N1_epi16 = _mm_srli_epi16(N1_epi16, 4);

                    N3_epi16 = _mm_unpacklo_epi8(N2_epi16, N1_epi16);
                    N4_epi16 = _mm_unpackhi_epi8(N2_epi16, N1_epi16);

                    g4_epi16 = _mm_unpackhi_epi8 (N4_epi16, zero);
                    g3_epi16 = _mm_unpacklo_epi8 (N4_epi16, zero);
                    g2_epi16 = _mm_unpackhi_epi8 (N3_epi16, zero);
                    g1_epi16 = _mm_unpacklo_epi8 (N3_epi16, zero);

                    B4_epi16 = _mm_unpackhi_epi8 (B2_epi16, zero);
                    B3_epi16 = _mm_unpacklo_epi8 (B2_epi16, zero);
                    B2_epi16 = _mm_unpackhi_epi8 (B1_epi16, zero);
                    B1_epi16 = _mm_unpacklo_epi8 (B1_epi16, zero);

                    B4_epi16 = _mm_slli_epi16(B4_epi16, 4);
                    B3_epi16 = _mm_slli_epi16(B3_epi16, 4);
                    B2_epi16 = _mm_slli_epi16(B2_epi16, 4);
                    B1_epi16 = _mm_slli_epi16(B1_epi16, 4);

                    g1_epi16 = _mm_or_si128(g1_epi16, B1_epi16);
                    g2_epi16 = _mm_or_si128(g2_epi16, B2_epi16);
                    g3_epi16 = _mm_or_si128(g3_epi16, B3_epi16);
                    g4_epi16 = _mm_or_si128(g4_epi16, B4_epi16);

                    _mm_store_si128(tmp_epi16++, g1_epi16);
                    _mm_store_si128(tmp_epi16++, g2_epi16);
                    _mm_store_si128(tmp_epi16++, g3_epi16);
                    _mm_store_si128(tmp_epi16++, g4_epi16);
                }

                {
                    __m128i *rp_epi16;
                    __m128i *g1p_epi16;
                    __m128i *g2p_epi16;
                    __m128i *bp_epi16;
                    __m128i *dgg_epi16;
                    __m128i *drg_epi16;
                    __m128i *dbg_epi16;
                    __m128i *ddg_epi16;
                    __m128i mid11bit = _mm_set1_epi16(1 << (13 - 1));

                    dgg_epi16 = (__m128i *)g_row_ptr;
                    drg_epi16 = (__m128i *)rg_row_ptr;
                    dbg_epi16 = (__m128i *)bg_row_ptr;
                    ddg_epi16 = (__m128i *)gdiff_row_ptr;

                    switch (bayer_format)
                    {
                        case BAYER_FORMAT_RED_GRN:
                            rp_epi16 = (__m128i *)tptr;
                            g1p_epi16 = (__m128i *)&tptr[width];
                            g2p_epi16 = (__m128i *)&tptr[width * 2];
                            bp_epi16 = (__m128i *)&tptr[width * 3];
                            break;
                        case BAYER_FORMAT_GRN_RED:
                            g1p_epi16 = (__m128i *)tptr;
                            rp_epi16 = (__m128i *)&tptr[width];
                            bp_epi16 = (__m128i *)&tptr[width * 2];
                            g2p_epi16 = (__m128i *)&tptr[width * 3];
                            break;
                        case BAYER_FORMAT_GRN_BLU:
                            g1p_epi16 = (__m128i *)tptr;
                            bp_epi16 = (__m128i *)&tptr[width];
                            rp_epi16 = (__m128i *)&tptr[width * 2];
                            g2p_epi16 = (__m128i *)&tptr[width * 3];
                            break;
                        case BAYER_FORMAT_BLU_GRN:
                            bp_epi16 = (__m128i *)tptr;
                            g1p_epi16 = (__m128i *)&tptr[width];
                            g2p_epi16 = (__m128i *)&tptr[width * 2];
                            rp_epi16 = (__m128i *)&tptr[width * 3];
                            break;
                    }


                    for (x = 0; x < srcwidth; x += 8)
                    {
                        __m128i r_epi16 = _mm_load_si128(rp_epi16++);
                        __m128i g1_epi16 = _mm_load_si128(g1p_epi16++);
                        __m128i g2_epi16 = _mm_load_si128(g2p_epi16++);
                        __m128i b_epi16 = _mm_load_si128(bp_epi16++);
                        __m128i gg = _mm_adds_epu16(g1_epi16, g2_epi16); //13-bit
                        __m128i dg = _mm_subs_epi16(g1_epi16, g2_epi16); //signed 12-bit
                        __m128i rg;
                        __m128i bg;

                        gg = _mm_srai_epi16(gg, 1); //12-bit unsigned
                        rg = _mm_subs_epi16(r_epi16, gg); //13-bit
                        bg = _mm_subs_epi16(b_epi16, gg); //13-bit
                        rg = _mm_adds_epi16(rg, mid11bit); //13-bit unsigned
                        bg = _mm_adds_epi16(bg, mid11bit); //13-bit unsigned
                        dg = _mm_adds_epi16(dg, mid11bit); //13-bit unsigned
                        rg = _mm_srai_epi16(rg, 1); //12-bit unsigned
                        bg = _mm_srai_epi16(bg, 1); //12-bit unsigned
                        dg = _mm_srai_epi16(dg, 1); //12-bit unsigned

                        _mm_store_si128(dgg_epi16++, gg);
                        _mm_store_si128(drg_epi16++, rg);
                        _mm_store_si128(dbg_epi16++, bg);
                        _mm_store_si128(ddg_epi16++, dg);
                    }

                    for (; x < srcwidth; x++)
                    {
                        int G;
                        int RG;
                        int BG;
                        int DG;

                        switch (bayer_format)
                        {
                            case BAYER_FORMAT_RED_GRN:
                                G = (scratch[x + width] + scratch[x + width * 2]);
                                RG = (scratch[x] << 3) - G + 32768;
                                BG = (scratch[x + width * 3] << 3) - G + 32768;
                                DG = ((scratch[x + width] - scratch[x + width * 2]) << 3) + 32768;
                                break;
                            case BAYER_FORMAT_GRN_RED:
                                G = (scratch[x] + scratch[x + width * 3]);
                                RG = (scratch[x + width] << 3) - G + 32768;
                                BG = (scratch[x + width * 2] << 3) - G + 32768;
                                DG = ((scratch[x] - scratch[x + width * 3]) << 3) + 32768;
                                break;
                            case BAYER_FORMAT_GRN_BLU:
                                G = (scratch[x] + scratch[x + width * 3]);
                                RG = (scratch[x + width * 2] << 3) - G + 32768;
                                BG = (scratch[x + width] << 3) - G + 32768;
                                DG = ((scratch[x] - scratch[x + width * 3]) << 3) + 32768;
                                break;
                            case BAYER_FORMAT_BLU_GRN:
                                G = (scratch[x + width] + scratch[x + width * 2]);
                                RG = (scratch[x + width * 3] << 3) - G + 32768;
                                BG = (scratch[x] << 3) - G + 32768;
                                DG = ((scratch[x + width] - scratch[x + width * 2]) << 3) + 32768;
                                break;
                        }

                        g_row_ptr[x] =  G >> 1;
                        rg_row_ptr[x] = RG >> 4;
                        bg_row_ptr[x] = BG >> 4;
                        gdiff_row_ptr[x] = DG >> 4;
                    }
                }
            }

            // Advance to the next rows in the input and output images
            g_row_ptr += byr1_pitch;
            rg_row_ptr += byr1_pitch;
            bg_row_ptr += byr1_pitch;
            gdiff_row_ptr += byr1_pitch;
        }
    }

    // Set the image parameters for each channel
    for (i = 0; i < 4; i++)
    {
        IMAGE *image = frame->channel[i];
        int band;

        // Set the image scale
        for (band = 0; band < IMAGE_NUM_BANDS; band++)
            image->scale[band] = 1;

        // Set the pixel type
        image->pixel_type[0] = PIXEL_TYPE_16S;
    }
}


void ConvertRGBA64ToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int precision, int origformat, int alpha)
{
    const int num_channels = alpha ? 4 : 3;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[4];
    int color_pitch[4];
    int frame_width = 0;
    int frame_height = 0;
    int display_height;
    int rowp;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;
    uint8_t *a_row_ptr = NULL;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    int a_row_pitch = 0;

    //int shift = 20;			// Shift down to form a 10 bit pixel

    //const int max_rgb = USHRT_MAX;

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // Swap the chroma values
    r_row_ptr = (uint8_t *)color_plane[0];
    r_row_pitch = color_pitch[0];
    g_row_ptr = (uint8_t *)color_plane[1];
    g_row_pitch = color_pitch[1];
    b_row_ptr = (uint8_t *)color_plane[2];
    b_row_pitch = color_pitch[2];
    if (alpha)
    {
        a_row_ptr = (uint8_t *)color_plane[3];
        a_row_pitch = color_pitch[3];
    }

    for (rowp = 0; rowp < frame_height/*display_height*/; rowp++) //DAN20090215 File the frame with edge to prevent ringing artifacts.
    {
        // Start at the leftmost column
        int column = 0;
        int row = rowp < display_height ? rowp : display_height - 1;

        // Start at the leftmost column

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        PIXEL16U *rgb_ptr = (PIXEL16U *)rgb_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;
        PIXEL16U *a_ptr = (PIXEL16U *)a_row_ptr;

        //int channeldepth = pitch * 8 / frame_width;

        rgb_ptr += (rgb_row_pitch / 2) * row;

        if (origformat == COLOR_FORMAT_RG30 || origformat == COLOR_FORMAT_AB10 ) // RG30
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = precision - 10;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = *(rgb_Lptr++) << shift;
                r = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                b = val & 0xffc;

                *(r_ptr++) = g;
                *(g_ptr++) = r;
                *(b_ptr++) = b;
            }
        }
        else if (origformat == COLOR_FORMAT_AR10 ) // AR10
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = precision - 10;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = *(rgb_Lptr++) << shift;
                b = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                r = val & 0xffc;

                *(r_ptr++) = g;
                *(g_ptr++) = r;
                *(b_ptr++) = b;
            }
        }
        else if (origformat == COLOR_FORMAT_R210 ) // R210
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = 12 - precision;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = _bswap(*(rgb_Lptr++));
                b = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                r = val & 0xffc;

                *(r_ptr++) = g >> shift;
                *(g_ptr++) = r >> shift;
                *(b_ptr++) = b >> shift;
            }
        }
        else if (origformat == COLOR_FORMAT_DPX0 ) // DPX0
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = 12 - precision;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = _bswap(*(rgb_Lptr++));
                r = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                b = val & 0xffc;

                *(r_ptr++) = g >> shift;
                *(g_ptr++) = r >> shift;
                *(b_ptr++) = b >> shift;
            }
        }
        else
        {
            int shift = 16 - precision;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int r, g, b, a;

                // Load the first set of ARGB values (skip the alpha value)
                r = *(rgb_ptr++);
                g = *(rgb_ptr++);
                b = *(rgb_ptr++);
                a = *(rgb_ptr++);

                // Clamp the values
                /*	if (r < 0) r = 0;
                	if (r > YU10_MAX) r = YU10_MAX;

                	if (g < 0) g = 0;
                	if (g > YU10_MAX) g = YU10_MAX;

                	if (b < 0) b = 0;
                	if (b > YU10_MAX) b = YU10_MAX;*/

                // Store
#if 1
                *(r_ptr++) = g >> shift;
                *(g_ptr++) = r >> shift;
                *(b_ptr++) = b >> shift;
#elif 0
                *(r_ptr++) = (g) >> shift;
                *(g_ptr++) = (((r - g) >> 1) + 32768) >> shift; //r
                *(b_ptr++) = (((b - g) >> 1) + 32768) >> shift; //b;
#endif
                if (alpha)
                {
                    a >>= shift;
                    // This help preserve the encoding of alpha channel extremes 0 and 1.  Alpha encoding curve
                    if (a > 0 && a < (255 << 4))
                    {
                        // step function 0 = 0, 0.0001 = 16/255, 0.9999 = 239/256, 1 = 255/256
                        a *= 223;
                        a += 128;
                        a >>= 8;
                        a += 16 << 4;
                    }
                    //	if (a < 0) a = 0;
                    //	if (a > YU10_MAX) a = YU10_MAX;
                    *(a_ptr++) = a;
                }
            }
        }

        // Advance the row pointers
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
        a_row_ptr += a_row_pitch;
    }
}


void ConvertRGB48ToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int precision, int origformat)
{
    const int num_channels = 3;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[3];
    int color_pitch[3];
    int frame_width = 0;
    int frame_height = 0;
    int display_height;
    int rowp;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;

    //int shift = 20;			// Shift down to form a 10 bit pixel

    //const int max_rgb = USHRT_MAX;

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // Swap the chroma values
    r_row_ptr = (uint8_t *)color_plane[0];
    r_row_pitch = color_pitch[0];
    g_row_ptr = (uint8_t *)color_plane[1];
    g_row_pitch = color_pitch[1];
    b_row_ptr = (uint8_t *)color_plane[2];
    b_row_pitch = color_pitch[2];

    for (rowp = 0; rowp < frame_height/*display_height*/; rowp++) //DAN20090215 File the frame with edge to prevent ringing artifacts.
    {
        // Start at the leftmost column
        int column = 0;
        int row = rowp < display_height ? rowp : display_height - 1;

        // Start at the leftmost column

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        PIXEL16U *rgb_ptr = (PIXEL16U *)rgb_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;

        //int channeldepth = pitch * 8 / frame_width;

        rgb_ptr += (rgb_row_pitch / 2) * row;

        if (origformat == COLOR_FORMAT_RG30 || origformat == COLOR_FORMAT_AB10 ) // RG30
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = precision - 10;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = *(rgb_Lptr++) << shift;
                r = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                b = val & 0xffc;

                *(r_ptr++) = g;
                *(g_ptr++) = r;
                *(b_ptr++) = b;
            }
        }
        else if (origformat == COLOR_FORMAT_AR10 ) // AR10
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = precision - 10;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = *(rgb_Lptr++) << shift;
                b = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                r = val & 0xffc;

                *(r_ptr++) = g;
                *(g_ptr++) = r;
                *(b_ptr++) = b;
            }
        }
        else if (origformat == COLOR_FORMAT_R210 ) // R210
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = 12 - precision;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = _bswap(*(rgb_Lptr++));
                b = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                r = val & 0xffc;

                *(r_ptr++) = g >> shift;
                *(g_ptr++) = r >> shift;
                *(b_ptr++) = b >> shift;
            }
        }
        else if (origformat == COLOR_FORMAT_DPX0 ) // DPX0
        {
            unsigned int *rgb_Lptr = (unsigned int *)rgb_row_ptr;
            int shift = 12 - precision;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int val;
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                val = _bswap(*(rgb_Lptr++));
                r = val & 0xffc;
                val >>= 10;
                g = val & 0xffc;
                val >>= 10;
                b = val & 0xffc;

                *(r_ptr++) = g >> shift;
                *(g_ptr++) = r >> shift;
                *(b_ptr++) = b >> shift;
            }
        }
        else
        {
            int shift = 16 - precision;
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                r = *(rgb_ptr++);
                g = *(rgb_ptr++);
                b = *(rgb_ptr++);

                // Clamp the values
                /*	if (r < 0) r = 0;
                	if (r > YU10_MAX) r = YU10_MAX;

                	if (g < 0) g = 0;
                	if (g > YU10_MAX) g = YU10_MAX;

                	if (b < 0) b = 0;
                	if (b > YU10_MAX) b = YU10_MAX;*/

                // Store
#if 1
                *(r_ptr++) = g >> shift;
                *(g_ptr++) = r >> shift;
                *(b_ptr++) = b >> shift;
#elif 0
                *(r_ptr++) = (g) >> shift;
                *(g_ptr++) = (((r - g) >> 1) + 32768) >> shift; //r
                *(b_ptr++) = (((b - g) >> 1) + 32768) >> shift; //b;
#endif
            }
        }

        // Advance the row pointers
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
    }
}

void ConvertRGBtoRGB48(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int precision)
{
    const int num_channels = 3;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[4];
    int color_pitch[4];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;

    //int shift = 20;			// Shift down to form a 10 bit pixel

    //const int max_rgb = USHRT_MAX;

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // Swap the chroma values
    r_row_ptr = (uint8_t *)color_plane[0];
    r_row_pitch = color_pitch[0];
    g_row_ptr = (uint8_t *)color_plane[1];
    g_row_pitch = color_pitch[1];
    b_row_ptr = (uint8_t *)color_plane[2];
    b_row_pitch = color_pitch[2];
    //	if(alpha)
    //	{
    //		a_row_ptr = (uint8_t *)color_plane[3];		a_row_pitch = color_pitch[3];
    //	}


    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        uint8_t *rgb_ptr = (uint8_t *)rgb_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;


        rgb_ptr += (display_height - 1 - row) * rgb_row_pitch;
        {
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                b = *(rgb_ptr++);
                g = *(rgb_ptr++);
                r = *(rgb_ptr++);

                // Store
                *(r_ptr++) = g << 4; // 8bit to 12bit
                *(g_ptr++) = r << 4;
                *(b_ptr++) = b << 4;
                //	if(alpha)
                //	{
                //		a = *(rgb_ptr++)>>shift;
                //		*(a_ptr++) = a;
                //	}
            }
        }

        // Advance the row pointers
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
    }
}






void ConvertRGBAtoRGB48(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int precision, int rgbaswap)
{
    const int num_channels = 3;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[4];
    int color_pitch[4];
    int frame_width;
    int frame_height;
    int display_height;
    int rowp;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;

    //int shift = 20;			// Shift down to form a 10 bit pixel

    //const int max_rgb = USHRT_MAX;

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // Swap the chroma values
    r_row_ptr = (uint8_t *)color_plane[0];
    r_row_pitch = color_pitch[0];
    g_row_ptr = (uint8_t *)color_plane[1];
    g_row_pitch = color_pitch[1];
    b_row_ptr = (uint8_t *)color_plane[2];
    b_row_pitch = color_pitch[2];
    //	if(alpha)
    //	{
    //		a_row_ptr = (uint8_t *)color_plane[3];		a_row_pitch = color_pitch[3];
    //	}


    for (rowp = 0; rowp < frame_height/*display_height*/; rowp++) //DAN20090215 File the frame with edge to prevent ringing artifacts.
    {
        // Start at the leftmost column
        int column = 0;
        int row = rowp < display_height ? rowp : display_height - 1;

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        uint8_t *rgb_ptr = (uint8_t *)rgb_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;


        rgb_ptr += (display_height - 1 - row) * rgb_row_pitch;

        if (rgbaswap) //ARGB
        {
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                rgb_ptr++;
                r = *(rgb_ptr++);
                g = *(rgb_ptr++);
                b = *(rgb_ptr++);

                // Store
                *(r_ptr++) = g << 4; // 8bit to 12bit
                *(g_ptr++) = r << 4;
                *(b_ptr++) = b << 4;
            }
        }
        else //BGRA
        {
            // Process the rest of the column
            for (; column < frame_width; column ++)
            {
                int r, g, b;

                // Load the first set of ARGB values (skip the alpha value)
                b = *(rgb_ptr++);
                g = *(rgb_ptr++);
                r = *(rgb_ptr++);
                rgb_ptr++;

                // Store
                *(r_ptr++) = g << 4; // 8bit to 12bit
                *(g_ptr++) = r << 4;
                *(b_ptr++) = b << 4;
            }
        }

        // Advance the row pointers
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
    }
}




void ConvertRGBAtoRGBA64(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int precision, int rgbaswap)
{
    const int num_channels = 4;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[4];
    int color_pitch[4];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;
    uint8_t *a_row_ptr;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    int a_row_pitch;

    //int shift = 20;			// Shift down to form a 10 bit pixel

    //const int max_rgb = USHRT_MAX;

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // Swap the chroma values
    r_row_ptr = (uint8_t *)color_plane[0];
    r_row_pitch = color_pitch[0];
    g_row_ptr = (uint8_t *)color_plane[1];
    g_row_pitch = color_pitch[1];
    b_row_ptr = (uint8_t *)color_plane[2];
    b_row_pitch = color_pitch[2];
    a_row_ptr = (uint8_t *)color_plane[3];
    a_row_pitch = color_pitch[3];


    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        uint8_t *rgb_ptr = (uint8_t *)rgb_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;
        PIXEL16U *a_ptr = (PIXEL16U *)a_row_ptr;


        rgb_ptr += (display_height - 1 - row) * rgb_row_pitch;

        if (rgbaswap) //ARGB
        {
            for (; column < frame_width; column ++)
            {
                int r, g, b, a;

                // Load the first set of ARGB values (skip the alpha value)
                a = *(rgb_ptr++) << 4; // 8bit to 12bit
                r = *(rgb_ptr++) << 4;
                g = *(rgb_ptr++) << 4;
                b = *(rgb_ptr++) << 4;

                // This help preserve the encoding of alpha channel extremes 0 and 1. Alpha encoding curve
                if (a > 0 && a < (255 << 4))
                {
                    // step function 0 = 0, 0.0001 = 16/255, 0.9999 = 239/256, 1 = 255/256
                    a *= 223;
                    a += 128;
                    a >>= 8;
                    a += 16 << 4;
                }

                // Store
                *(r_ptr++) = g;
                *(g_ptr++) = r;
                *(b_ptr++) = b;
                *(a_ptr++) = a;
            }
        }
        else //BGRA
        {
            // Process the rest of the column

            for (; column < frame_width; column ++)
            {
                int r, g, b, a;

                // Load the first set of ARGB values (skip the alpha value)
                b = *(rgb_ptr++) << 4; // 8bit to 12bit
                g = *(rgb_ptr++) << 4;
                r = *(rgb_ptr++) << 4;
                a = *(rgb_ptr++) << 4;

                // This help preserve the encoding of alpha channel extremes 0 and 1. Alpha encoding curve
                if (a > 0 && a < (255 << 4))
                {
                    // step function 0 = 0, 0.0001 = 16/255, 0.9999 = 239/256, 1 = 255/256
                    a *= 223;
                    a += 128;
                    a >>= 8;
                    a += 16 << 4;
                }

                // Store
                *(r_ptr++) = g;
                *(g_ptr++) = r;
                *(b_ptr++) = b;
                *(a_ptr++) = a;
            }
        }

        // Advance the row pointers
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
        a_row_ptr += a_row_pitch;
    }
}


/*
	Convert QuickTime format b64a to a frame of planar RGBA.
	This routine was adapted from ConvertBGRA64ToFrame16s.
	The alpha channel is currently ignored.
*/
CODEC_ERROR ConvertBGRA64ToFrame_4444_16s(uint8_t *data, int pitch, FRAME *frame,
        uint8_t *buffer, int precision)
{
    //#pragma unused(buffer);

    //TODO: Add code to write the alpha channel into the fourth plane
    int num_channels;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[FRAME_MAX_CHANNELS];
    int color_pitch[FRAME_MAX_CHANNELS];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;
    uint8_t *a_row_ptr = NULL;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    int a_row_pitch = 0;

    //int shift = 20;			// Shift down to form a 10-bit pixel
    int shift = 16 - precision;
    int channel_depth;

    bool alpha_flag;

    //const int max_rgb = USHRT_MAX;

    //TODO: Need to return error codes
    assert(frame != NULL);
    if (! (frame != NULL))
    {
        return CODEC_ERROR_INVALID_ARGUMENT;
    }

    assert(frame->format == FRAME_FORMAT_RGB || frame->format == FRAME_FORMAT_RGBA);
    if (! (frame->format == FRAME_FORMAT_RGB || frame->format == FRAME_FORMAT_RGBA))
    {
        return CODEC_ERROR_BAD_FRAME;
    }

    alpha_flag = (frame->format == FRAME_FORMAT_RGBA);
    num_channels = (alpha_flag ? 4 : 3);

    // Check that the frame was allocated with enough channels
    assert(frame->num_channels >= num_channels);

    //TODO: Set the alpha flag and number of channels using the values in the frame data structure

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the frame dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // This routine does not handle the RG30 format
    channel_depth = pitch * 8 / frame_width;
    assert(channel_depth != 32);
    if (! (channel_depth != 32))
    {
        return CODEC_ERROR_BADFORMAT;
    }

    // Set the row pointers for each channel to the correct plane
    r_row_ptr = (uint8_t *)color_plane[1];
    r_row_pitch = color_pitch[1];
    g_row_ptr = (uint8_t *)color_plane[0];
    g_row_pitch = color_pitch[0];
    b_row_ptr = (uint8_t *)color_plane[2];
    b_row_pitch = color_pitch[2];
    if (alpha_flag)
    {
        a_row_ptr = (uint8_t *)color_plane[3];
        a_row_pitch = color_pitch[3];
    }

    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

        //TODO: Process each row by calling an optimized subroutine
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        PIXEL16U *rgb_ptr = (PIXEL16U *)rgb_row_ptr;

        // Pointers into the output rows for each plane
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;
        PIXEL16U *a_ptr = (PIXEL16U *)a_row_ptr;

        // Process the rest of the column
        for (; column < frame_width; column ++)
        {
            int r, g, b, a;

            // Load the first set of ARGB values
            a = *(rgb_ptr++);
            r = *(rgb_ptr++);
            g = *(rgb_ptr++);
            b = *(rgb_ptr++);

            // Shift the 16-bit pixels to the encoded precision
            *(r_ptr++) = r >> shift;
            *(g_ptr++) = g >> shift;
            *(b_ptr++) = b >> shift;

            if (alpha_flag)
            {
                //*(a_ptr++) = a >> shift;
                a >>= shift;
                // This help preserve the encoding of alpha channel extremes 0 and 1.  Alpha encoding curve
                if (a > 0 && a < (255 << 4))
                {
                    // step function 0 = 0, 0.0001 = 16/255, 0.9999 = 239/256, 1 = 255/256
                    a *= 223;
                    a += 128;
                    a >>= 8;
                    a += 16 << 4;
                }
                //	if (a < 0) a = 0;
                //	if (a > YU10_MAX) a = YU10_MAX;
                *(a_ptr++) = a;
            }
        }

        // Advance the input row pointer
        rgb_row_ptr += rgb_row_pitch;

        // Advance the output row pointers
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
        a_row_ptr += a_row_pitch;
    }

    // Successful conversion
    return CODEC_ERROR_OKAY;
}


/*
	Convert QuickTime format b64a to a frame of planar YUV.
*/
void ConvertAnyDeep444to422(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int color_space, int origformat)
{
    //#pragma unused(buffer);

    const int num_channels = 3;

    uint8_t *rgb_row_ptr = data;
    int rgb_row_pitch = pitch;

    PIXEL *color_plane[3];
    int color_pitch[3];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *y_row_ptr;
    uint8_t *u_row_ptr;
    uint8_t *v_row_ptr;

    int y_row_pitch;
    int u_row_pitch;
    int v_row_pitch;

    //int shift = 14;		// Shift down to form a 16 bit pixel
    int shift = 20;			// Shift down to form a 10 bit pixel

    int y_rmult;
    int y_gmult;
    int y_bmult;
    int y_offset;

    int u_rmult;
    int u_gmult;
    int u_bmult;
    int u_offset;

    int v_rmult;
    int v_gmult;
    int v_bmult;
    int v_offset;

    //const int max_rgb = USHRT_MAX;

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == num_channels);
    assert(frame->format == FRAME_FORMAT_YUV);

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

#if 0
    // Invert the input image
    rgb_row_ptr += (display_height - 1) * rgb_row_pitch;
    rgb_row_pitch = NEG(rgb_row_pitch);
#endif

    // Swap the chroma values
    y_row_ptr = (uint8_t *)color_plane[0];
    y_row_pitch = color_pitch[0];
    u_row_ptr = (uint8_t *)color_plane[2];
    u_row_pitch = color_pitch[2];
    v_row_ptr = (uint8_t *)color_plane[1];
    v_row_pitch = color_pitch[1];


    // Select the coefficients corresponding to the color space
    switch (color_space & COLORSPACE_MASK)
    {
        case COLOR_SPACE_CG_601:		// Computer systems 601

            // sRGB + 601
            // Floating point arithmetic is
            //
            // Y  = 0.257R + 0.504G + 0.098B + 16.5;
            // Cb =-0.148R - 0.291G + 0.439B + 128.5;
            // Cr = 0.439R - 0.368G - 0.071B + 128.5;
            //
            // Fixed point approximation (8-bit) is
            //
            // Y  = ( 66R + 129G +  25B +  4224) >> 8;
            // Cb = (-38R -  74G + 112B + 32896) >> 8;
            // Cr = (112R -  94G -  18B + 32896) >> 8;

            //
            // Fixed point approximation (14-bit) is
            //
            // Y  = ( 4211.R + 8258.G + 1606.B) >> 14) + y_offset;
            // Cb = (-2425.R - 4768.G + 7193.B) >> 14) + u_offset;
            // Cr = ( 7193.R - 6029.G - 1163.B) >> 14) + v_offset;

            y_rmult = 4211;
            y_gmult = 8258;
            y_bmult = 1606;
            y_offset = 64;

            u_rmult = 2425;
            u_gmult = 4768;
            u_bmult = 7193;
            u_offset = 512;

            v_rmult = 7193;
            v_gmult = 6029;
            v_bmult = 1163;
            v_offset = 512;

            break;

        case COLOR_SPACE_VS_601:		// Video systems 601

            // video systems RGB + 601
            // Floating point arithmetic is
            // Y = 0.299R + 0.587G + 0.114B
            // Cb = -0.172R - 0.339G + 0.511B + 128
            // Cr = 0.511R - 0.428G - 0.083B + 128
            //
            // Fixed point approximation (8-bit) is
            //
            // Y  = ( 77R + 150G +  29B + 128) >> 8;
            // Cb = (-44R -  87G + 131B + 32896) >> 8;
            // Cr = (131R - 110G -  21B + 32896) >> 8;
            //
            // Fixed point approximation (14-bit) is
            //
            // Y  = ( 4899.R + 9617.G + 1868.B) >> 14) + y_offset;
            // Cb = (-2818.R - 5554.G + 8372.B) >> 14) + u_offset;
            // Cr = ( 8372.R - 7012.G - 1360.B) >> 14) + v_offset;

            y_rmult = 4899;
            y_gmult = 9617;
            y_bmult = 1868;
            y_offset = 0;

            u_rmult = 2818;
            u_gmult = 5554;
            u_bmult = 8372;
            u_offset = 512;

            v_rmult = 8372;
            v_gmult = 7012;
            v_bmult = 1360;
            v_offset = 512;

            break;

        default:
            assert(0);
        case COLOR_SPACE_CG_709:		// Computer systems 709

            // sRGB + 709
            // Y = 0.183R + 0.614G + 0.062B + 16
            // Cb = -0.101R - 0.338G + 0.439B + 128
            // Cr = 0.439R - 0.399G - 0.040B + 128
            //
            // Fixed point approximation (8-bit) is
            //
            // Y  = ( 47R + 157G +  16B +  4224) >> 8;
            // Cb = (-26R -  87G + 112B + 32896) >> 8;
            // Cr = (112R - 102G -  10B + 32896) >> 8;
            //
            // Fixed point approximation (14-bit) is
            //
            // Y  = ( 2998.R + 10060.G + 1016.B) >> 14) + y_offset;
            // Cb = (-1655.R -  5538.G + 7193.B) >> 14) + u_offset;
            // Cr = ( 7193.R -  6537.G -  655.B) >> 14) + v_offset;

            y_rmult = 2998;
            y_gmult = 10060;
            y_bmult = 1016;
            y_offset = 64;

            u_rmult = 1655;
            u_gmult = 5538;
            u_bmult = 7193;
            u_offset = 512;

            v_rmult = 7193;
            v_gmult = 6537;
            v_bmult = 655;
            v_offset = 512;

            break;

        case COLOR_SPACE_VS_709:		// Video systems 709

            // video systems RGB + 709
            // Floating point arithmetic is
            // Y = 0.213R + 0.715G + 0.072B
            // Cb = -0.117R - 0.394G + 0.511B + 128
            // Cr = 0.511R - 0.464G - 0.047B + 128
            //
            // Fixed point approximation (8-bit) is
            //
            // Y  = ( 55R + 183G +  18B +  128) >> 4;
            // Cb = (-30R - 101G + 131B + 32896) >> 4;
            // Cr = (131R - 119G -  12B + 32896) >> 4;
            //
            // Fixed point approximation (14-bit) is
            //
            // Y  = ( 3490.R + 11715.G + 1180.B) >> 14) + y_offset;
            // Cb = (-1917.R -  6455.G + 8372.B) >> 14) + u_offset;
            // Cr = ( 8372.R -  7602.G -  770.B) >> 14) + v_offset;

            y_rmult = 3490;
            y_gmult = 11715;
            y_bmult = 1180;
            y_offset = 0;

            u_rmult = 1917;
            u_gmult = 6455;
            u_bmult = 8372;
            u_offset = 512;

            v_rmult = 8372;
            v_gmult = 7602;
            v_bmult = 770;
            v_offset = 512;
            break;
    }

    for (row = 0; row < frame->height; row++) //DAN20170725 Fix of odd heights
    {
        // Start at the leftmost column
        int column = 0;

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the RGB input row
        PIXEL16U *rgb_ptr = (PIXEL16U *)rgb_row_ptr;
        uint32_t *rgb10_ptr = (uint32_t *)rgb_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *y_ptr = (PIXEL16U *)y_row_ptr;
        PIXEL16U *u_ptr = (PIXEL16U *)u_row_ptr;
        PIXEL16U *v_ptr = (PIXEL16U *)v_row_ptr;

        // Process the rest of the column
        for (; column < frame_width; column += 2)
        {
            int r, g, b;
            int y, u, v;
            uint32_t val;

            // Load the first set of ARGB values (skip the alpha value)
            switch (origformat)
            {
                case COLOR_FORMAT_R210:
                    //*outA32++ = _bswap((r<<20)|(g<<10)|(b));
                    val = _bswap(*(rgb10_ptr++));
                    r = (val >> 14) & 0xffc0;
                    g = (val >> 4) & 0xffc0;
                    b = (val << 6) & 0xffc0;
                    break;
                case COLOR_FORMAT_DPX0:
                    //*outA32++ = _bswap((r<<22)|(g<<12)|(b<<2));
                    val = _bswap(*(rgb10_ptr++));
                    r = (val >> 16) & 0xffc0;
                    g = (val >> 6) & 0xffc0;
                    b = (val << 4) & 0xffc0;
                    break;
                case COLOR_FORMAT_RG30:
                case COLOR_FORMAT_AB10:
                    //*outA32++ = r|(g<<10)|(b<<20);
                    val = *(rgb10_ptr++);
                    b = (val >> 14) & 0xffc0;
                    g = (val >> 4) & 0xffc0;
                    r = (val << 6) & 0xffc0;
                    break;
                case COLOR_FORMAT_AR10:
                    //*outA32++ = (r<<20)|(g<<10)|(b);
                    val = *(rgb10_ptr++);
                    r = (val >> 14) & 0xffc0;
                    g = (val >> 4) & 0xffc0;
                    b = (val << 6) & 0xffc0;
                    break;
                case COLOR_FORMAT_RG48:
                    r = *(rgb_ptr++);
                    g = *(rgb_ptr++);
                    b = *(rgb_ptr++);
                    break;
                case COLOR_FORMAT_RG64:
                    r = *(rgb_ptr++);
                    g = *(rgb_ptr++);
                    b = *(rgb_ptr++);
                    rgb_ptr++;
                    break;
                case COLOR_FORMAT_B64A:
                    rgb_ptr++;
                    r = *(rgb_ptr++);
                    g = *(rgb_ptr++);
                    b = *(rgb_ptr++);
                    break;

                default:
                    // Eliminate compiler warning about using uninitialized variable
                    r = g = b = 0;
                    break;
            }

            // Convert RGB to YCbCr
            y = (( y_rmult * r + y_gmult * g + y_bmult * b) >> shift) + y_offset;
            u = ((-u_rmult * r - u_gmult * g + u_bmult * b) >> shift);
            v = (( v_rmult * r - v_gmult * g - v_bmult * b) >> shift);

            // Clamp and store the first luma value
            if (y < 0) y = 0;
            if (y > YU10_MAX) y = YU10_MAX;
            *(y_ptr++) = y;

            // Load the second set of ARGB values (skip the alpha value)
            switch (origformat)
            {
                case COLOR_FORMAT_R210:
                    //*outA32++ = _bswap((r<<20)|(g<<10)|(b));
                    val = _bswap(*(rgb10_ptr++));
                    r = (val >> 14) & 0xffc0;
                    g = (val >> 4) & 0xffc0;
                    b = (val << 6) & 0xffc0;
                    break;
                case COLOR_FORMAT_DPX0:
                    //*outA32++ = _bswap((r<<22)|(g<<12)|(b<<2));
                    val = _bswap(*(rgb10_ptr++));
                    r = (val >> 16) & 0xffc0;
                    g = (val >> 6) & 0xffc0;
                    b = (val << 4) & 0xffc0;
                    break;
                case COLOR_FORMAT_RG30:
                case COLOR_FORMAT_AB10:
                    //*outA32++ = r|(g<<10)|(b<<20);
                    val = *(rgb10_ptr++);
                    b = (val >> 14) & 0xffc0;
                    g = (val >> 4) & 0xffc0;
                    r = (val << 6) & 0xffc0;
                    break;
                case COLOR_FORMAT_AR10:
                    //*outA32++ = (r<<20)|(g<<10)|(b);
                    val = *(rgb10_ptr++);
                    r = (val >> 14) & 0xffc0;
                    g = (val >> 4) & 0xffc0;
                    b = (val << 6) & 0xffc0;
                    break;
                case COLOR_FORMAT_RG48:
                    r = *(rgb_ptr++);
                    g = *(rgb_ptr++);
                    b = *(rgb_ptr++);
                    break;
                case COLOR_FORMAT_RG64:
                    r = *(rgb_ptr++);
                    g = *(rgb_ptr++);
                    b = *(rgb_ptr++);
                    rgb_ptr++;
                    break;
                case COLOR_FORMAT_B64A:
                    rgb_ptr++;
                    r = *(rgb_ptr++);
                    g = *(rgb_ptr++);
                    b = *(rgb_ptr++);
                    break;
            }

            // Convert RGB to YCbCr
            y = (( y_rmult * r + y_gmult * g + y_bmult * b) >> shift) + y_offset;

#if !INTERPOLATE_CHROMA
            // The OneRiver Media filter test requires this for correct rendering of the red vertical stripes
            u += ((-u_rmult * r - u_gmult * g + u_bmult * b) >> shift);
            v += (( v_rmult * r - v_gmult * g - v_bmult * b) >> shift);
            u >>= 1;
            v >>= 1;
#endif
            u += u_offset;
            v += v_offset;

            // Clamp the luma and chroma values
            if (y < 0) y = 0;
            if (y > YU10_MAX) y = YU10_MAX;

            if (u < 0) u = 0;
            if (u > YU10_MAX) u = YU10_MAX;

            if (v < 0) v = 0;
            if (v > YU10_MAX) v = YU10_MAX;

            // Store the second luma value
            *(y_ptr++) = y;

            // Store the chroma values
            *(u_ptr++) = u;
            *(v_ptr++) = v;
        }

        // Advance the row pointers
        if (row < display_height - 1)	//DAN20170725 - Fix for odd vertical heights
            rgb_row_ptr += rgb_row_pitch;
        y_row_ptr += y_row_pitch;
        u_row_ptr += u_row_pitch;
        v_row_ptr += v_row_pitch;
    }
}

// Pack the lowpass band of RGB 4:4:4 into the specified RGB format
void ConvertLowpassRGB444ToRGB(IMAGE *image_array[], uint8_t *output_buffer,
                               int output_width, int output_height,
                               int32_t output_pitch, int format,
                               bool inverted, int shift, int num_channels)
{
    PIXEL *plane_array[TRANSFORM_MAX_CHANNELS] = {0};
    int pitch_array[TRANSFORM_MAX_CHANNELS] = {0};
    ROI roi = {0, 0};
    int channel;
    //int saturate = 1;

    // Only 24 and 32 bit true color RGB formats are supported
    //assert(format == COLOR_FORMAT_RGB24 || format == COLOR_FORMAT_RGB32);

    // Convert from pixel to byte data
    for (channel = 0; channel < num_channels; channel++)
    {
        IMAGE *image = image_array[channel];

        plane_array[channel] = image->band[0];
        pitch_array[channel] = image->pitch;

        // The overall frame dimensions are determined by the first channel
        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = output_height;// image->height;  //DAN20170725 Fix of odd heights
        }
    }

    switch (format & 0x7ffffff)
    {
        case COLOR_FORMAT_RGB24:
            ConvertLowpassRGB444ToRGB24(plane_array, pitch_array,
                                        output_buffer, output_pitch,
                                        roi, inverted, shift);
            break;

        case COLOR_FORMAT_RGB32:
        case COLOR_FORMAT_RGB32_INVERTED:
            ConvertLowpassRGB444ToRGB32(plane_array, pitch_array,
                                        output_buffer, output_pitch,
                                        roi, inverted, shift, num_channels);
            break;

        case COLOR_FORMAT_RG48:
            ConvertLowpassRGB444ToRGB48(plane_array, pitch_array,
                                        output_buffer, output_pitch,
                                        roi, inverted, shift);
            break;
        case COLOR_FORMAT_RG64:
            ConvertLowpassRGB444ToRGBA64(plane_array, pitch_array,
                                         output_buffer, output_pitch,
                                         roi, inverted, shift);
            break;
        case COLOR_FORMAT_B64A:
            ConvertLowpassRGB444ToB64A(plane_array, pitch_array,
                                       output_buffer, output_pitch,
                                       roi, inverted, shift, num_channels);
            break;
        case COLOR_FORMAT_RG30:
        case COLOR_FORMAT_AR10:
        case COLOR_FORMAT_AB10:
        case COLOR_FORMAT_R210:
        case COLOR_FORMAT_DPX0:
            ConvertLowpassRGB444ToRGB30(plane_array, pitch_array,
                                        output_buffer, output_pitch,
                                        roi, inverted, shift, format);
            break;

        default:
            assert(0);		// Unsupported pixel format
            break;
    }
}

void ConvertLowpassRGB444ToRGB24(PIXEL *plane_array[], int pitch_array[],
                                 uint8_t *output_buffer, int output_pitch,
                                 ROI roi, bool inverted, int shift)
{
    if (inverted && output_pitch > 0)
    {
        output_buffer += output_pitch * (roi.height - 1);
        output_pitch = -output_pitch;
    }

    ConvertPlanarRGB16uToPackedRGB24(plane_array, pitch_array, roi,
                                     output_buffer, output_pitch, roi.width, 6);
}


void ConvertLowpassRGB444ToRGB32(PIXEL *plane_array[], int pitch_array[],
                                 uint8_t *output_buffer, int output_pitch,
                                 ROI roi, bool inverted, int shift, int num_channels)
{
    if (inverted && output_pitch > 0)
    {
        output_buffer += output_pitch * (roi.height - 1);
        output_pitch = -output_pitch;
    }

    ConvertPlanarRGB16uToPackedRGB32(plane_array, pitch_array, roi,
                                     output_buffer, output_pitch, roi.width, 6, num_channels);

}

void ConvertLowpassRGB444ToRGB48(PIXEL *plane_array[], int pitch_array[],
                                 uint8_t *output_buffer, int output_pitch,
                                 ROI roi, bool inverted, int shift)
{
    PIXEL *r_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *b_row_ptr;
    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    //int r_prescale;
    //int g_prescale;
    //int b_prescale;
    PIXEL16U *argb_row_ptr;

    int output_height = roi.height;

    //size_t output_row_size = output_pitch;

    int row;
    int column;

    const bool saturate = true;
    const int rgb_max = USHRT_MAX;
    //const int alpha = USHRT_MAX;

    // Get pointers to the rows in the lowpass band of each channel
    r_row_ptr = plane_array[1];
    r_row_pitch = pitch_array[1] / sizeof(PIXEL);
    g_row_ptr = plane_array[0];
    g_row_pitch = pitch_array[0] / sizeof(PIXEL);
    b_row_ptr = plane_array[2];
    b_row_pitch = pitch_array[2] / sizeof(PIXEL);

    // Convert the output pitch to units of pixels
    output_pitch /= sizeof(PIXEL);

    argb_row_ptr = (PIXEL16U *)output_buffer;
    if (inverted)
    {
        argb_row_ptr += (output_height - 1) * output_pitch;
        output_pitch = NEG(output_pitch);
    }

    for (row = 0; row < output_height; row++)
    {
        PIXEL16U *argb_ptr = argb_row_ptr;
#if (0 && XMMOPT)
        int column_step = 16;
        int post_column = roi.width - (roi.width % column_step);
        __m64 *r_ptr = (__m64 *)r_row_ptr;
        __m64 *g_ptr = (__m64 *)g_row_ptr;
        __m64 *b_ptr = (__m64 *)b_row_ptr;
        __m64 *argb_ptr = (__m64 *)argb_row_ptr;
#endif
        // Start at the leftmost column
        column = 0;

        // Clear the output row (for debugging)
        //memset(argb_row_ptr, 0, output_row_size);

#if (0 && XMMOPT)

        // Should have exited the loop at the post processing column
        assert(column == post_column);
#endif
        // Process the rest of the row
        for (; column < roi.width; column++)
        {
            int r;
            int g;
            int b;

            // Load the tuple of RGB values
            r = r_row_ptr[column];
            g = g_row_ptr[column];
            b = b_row_ptr[column];

            //r >>= r_prescale;
            //g >>= g_prescale;
            //b >>= b_prescale;

            // Scale the lowpass values to 16 bits
            r <<= shift;
            g <<= shift;
            b <<= shift;

            if (saturate)
            {
                if (r < 0) r = 0;
                else if (r > rgb_max) r = rgb_max;
                if (g < 0) g = 0;
                else if (g > rgb_max) g = rgb_max;
                if (b < 0) b = 0;
                else if (b > rgb_max) b = rgb_max;
            }

            *(argb_ptr++) = r;
            *(argb_ptr++) = g;
            *(argb_ptr++) = b;
        }

        // Advance the row pointers into each lowpass band
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;

        // Advance the row pointer into the output buffer
        argb_row_ptr += output_pitch;
    }
}

void ConvertLowpassRGB444ToRGBA64(PIXEL *plane_array[], int pitch_array[],
                                  uint8_t *output_buffer, int output_pitch,
                                  ROI roi, bool inverted, int shift)
{
    PIXEL *r_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *b_row_ptr;
    PIXEL *a_row_ptr;
    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    int a_row_pitch;
    //int r_prescale;
    //int g_prescale;
    //int b_prescale;
    PIXEL16U *argb_row_ptr;

    int output_height = roi.height;

    //size_t output_row_size = output_pitch;

    int row;
    int column;

    const bool saturate = true;
    const int rgb_max = USHRT_MAX;
    //const int alpha = USHRT_MAX;

    //alpha_Companded likely doesn't set as the is not does to go through Convert4444LinesToOutput

    // Get pointers to the rows in the lowpass band of each channel
    r_row_ptr = plane_array[1];
    r_row_pitch = pitch_array[1] / sizeof(PIXEL);
    g_row_ptr = plane_array[0];
    g_row_pitch = pitch_array[0] / sizeof(PIXEL);
    b_row_ptr = plane_array[2];
    b_row_pitch = pitch_array[2] / sizeof(PIXEL);
    a_row_ptr = plane_array[3];
    a_row_pitch = pitch_array[3] / sizeof(PIXEL);

    // Convert the output pitch to units of pixels
    output_pitch /= sizeof(PIXEL);

    argb_row_ptr = (PIXEL16U *)output_buffer;
    if (inverted)
    {
        argb_row_ptr += (output_height - 1) * output_pitch;
        output_pitch = NEG(output_pitch);
    }

    for (row = 0; row < output_height; row++)
    {
        PIXEL16U *argb_ptr = argb_row_ptr;
#if (0 && XMMOPT)
        int column_step = 16;
        int post_column = roi.width - (roi.width % column_step);
        __m64 *r_ptr = (__m64 *)r_row_ptr;
        __m64 *g_ptr = (__m64 *)g_row_ptr;
        __m64 *b_ptr = (__m64 *)b_row_ptr;
        __m64 *argb_ptr = (__m64 *)argb_row_ptr;
#endif
        // Start at the leftmost column
        column = 0;

        // Clear the output row (for debugging)
        //memset(argb_row_ptr, 0, output_row_size);

#if (0 && XMMOPT)

        // Should have exited the loop at the post processing column
        assert(column == post_column);
#endif
        // Process the rest of the row
        for (; column < roi.width; column++)
        {
            int r;
            int g;
            int b;
            int a;

            // Load the tuple of RGB values
            r = r_row_ptr[column];
            g = g_row_ptr[column];
            b = b_row_ptr[column];
            a = a_row_ptr[column];

            //r >>= r_prescale;
            //g >>= g_prescale;
            //b >>= b_prescale;

            // Scale the lowpass values to 16 bits
            r <<= shift;
            g <<= shift;
            b <<= shift;
            a <<= shift;

            // Remove the alpha encoding curve.
            //a -= 16<<8;
            //a <<= 8;
            //a += 111;
            //a /= 223;
            //12-bit SSE calibrated code
            //a2_output_epi16 = _mm_subs_epu16(a2_output_epi16, _mm_set1_epi16(alphacompandDCoffset)); //12-bit  -16
            //a2_output_epi16 = _mm_slli_epi16(a2_output_epi16, 3);  //15-bit
            //a2_output_epi16 = _mm_mulhi_epi16(a2_output_epi16, _mm_set1_epi16(alphacompandGain));

            a >>= 4; //12-bit
            a -= alphacompandDCoffset;
            a <<= 3; //15-bit
            a *= alphacompandGain;
            a >>= 16; //12-bit
            a <<= 4; // 16-bit;

            if (saturate)
            {
                if (r < 0) r = 0;
                else if (r > rgb_max) r = rgb_max;
                if (g < 0) g = 0;
                else if (g > rgb_max) g = rgb_max;
                if (b < 0) b = 0;
                else if (b > rgb_max) b = rgb_max;
                if (a < 0) a = 0;
                else if (a > rgb_max) a = rgb_max;
            }

            *(argb_ptr++) = r;
            *(argb_ptr++) = g;
            *(argb_ptr++) = b;
            *(argb_ptr++) = a;
        }

        // Advance the row pointers into each lowpass band
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
        a_row_ptr += a_row_pitch;

        // Advance the row pointer into the output buffer
        argb_row_ptr += output_pitch;
    }
}

void ConvertLowpassRGB444ToB64A(PIXEL *plane_array[], int pitch_array[],
                                uint8_t *output_buffer, int output_pitch,
                                ROI roi, bool inverted, int shift, int num_channels)
{
    PIXEL *r_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *b_row_ptr;
    PIXEL *a_row_ptr = NULL;
    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    int a_row_pitch;
    //int r_prescale;
    //int g_prescale;
    //int b_prescale;
    PIXEL16U *argb_row_ptr;

    int output_height = roi.height;

    //size_t output_row_size = output_pitch;

    int row;
    int column;

    const bool saturate = 1;
    const int rgb_max = USHRT_MAX;
    const int alpha = USHRT_MAX;

    // Get pointers to the rows in the lowpass band of each channel
    r_row_ptr = (PIXEL *)plane_array[1];
    r_row_pitch = pitch_array[1] / sizeof(PIXEL);
    g_row_ptr = (PIXEL *)plane_array[0];
    g_row_pitch = pitch_array[0] / sizeof(PIXEL);
    b_row_ptr = (PIXEL *)plane_array[2];
    b_row_pitch = pitch_array[2] / sizeof(PIXEL);
    if (num_channels == 4)
    {
        a_row_ptr = (PIXEL *)plane_array[3];
        a_row_pitch = pitch_array[3] / sizeof(PIXEL);
    }

    // Convert the output pitch to units of pixels
    output_pitch /= sizeof(PIXEL);

    argb_row_ptr = (PIXEL16U *)output_buffer;
    if (inverted)
    {
        argb_row_ptr += (output_height - 1) * output_pitch;
        output_pitch = NEG(output_pitch);
    }

    for (row = 0; row < output_height; row++)
    {
#if (0 && XMMOPT)
        int column_step = 16;
        int post_column = roi.width - (roi.width % column_step);
        __m64 *r_ptr = (__m64 *)r_row_ptr;
        __m64 *g_ptr = (__m64 *)g_row_ptr;
        __m64 *b_ptr = (__m64 *)b_row_ptr;
        __m64 *a_ptr = (__m64 *)a_row_ptr;
        __m64 *argb_ptr = (__m64 *)argb_row_ptr;
#endif
        // Start at the leftmost column
        column = 0;

        // Clear the output row (for debugging)
        //memset(argb_row_ptr, 0, output_row_size);

#if (0 && XMMOPT)

        // Should have exited the loop at the post processing column
        assert(column == post_column);
#endif
        // Process the rest of the row
        if (num_channels == 4)
        {
            for (; column < roi.width; column++)
            {
                PIXEL16U *argb_ptr = &argb_row_ptr[column * 4];
                int r, g, b, a;

                // Load the tuple of RGB values
                r = r_row_ptr[column];
                g = g_row_ptr[column];
                b = b_row_ptr[column];
                a = a_row_ptr[column];

                // Scale the lowpass values to 16 bits
                r <<= shift;
                g <<= shift;
                b <<= shift;
                a <<= shift;

                // Remove the alpha encoding curve.
                //a -= 16<<8;
                //a <<= 8;
                //a += 111;
                //a /= 223;
                //12-bit SSE calibrated code
                //a2_output_epi16 = _mm_subs_epu16(a2_output_epi16, _mm_set1_epi16(alphacompandDCoffset)); //12-bit  -16
                //a2_output_epi16 = _mm_slli_epi16(a2_output_epi16, 3);  //15-bit
                //a2_output_epi16 = _mm_mulhi_epi16(a2_output_epi16, _mm_set1_epi16(alphacompandGain));

                a >>= 4; //12-bit
                a -= alphacompandDCoffset;
                a <<= 3; //15-bit
                a *= alphacompandGain;
                a >>= 16; //12-bit
                a <<= 4; // 16-bit;


                if (saturate)
                {
                    if (r < 0) r = 0;
                    else if (r > rgb_max) r = rgb_max;
                    if (g < 0) g = 0;
                    else if (g > rgb_max) g = rgb_max;
                    if (b < 0) b = 0;
                    else if (b > rgb_max) b = rgb_max;
                    if (a < 0) a = 0;
                    else if (a > rgb_max) a = rgb_max;
                }

                *(argb_ptr++) = a;
                *(argb_ptr++) = r;
                *(argb_ptr++) = g;
                *(argb_ptr++) = b;
            }
        }
        else
        {
            for (; column < roi.width; column++)
            {
                PIXEL16U *argb_ptr = &argb_row_ptr[column * 4];
                int r;
                int g;
                int b;

                // Load the tuple of RGB values
                r = r_row_ptr[column];
                g = g_row_ptr[column];
                b = b_row_ptr[column];

                //r >>= r_prescale;
                //g >>= g_prescale;
                //b >>= b_prescale;

                // Scale the lowpass values to 16 bits
                r <<= shift;
                g <<= shift;
                b <<= shift;

                if (saturate)
                {
                    if (r < 0) r = 0;
                    else if (r > rgb_max) r = rgb_max;
                    if (g < 0) g = 0;
                    else if (g > rgb_max) g = rgb_max;
                    if (b < 0) b = 0;
                    else if (b > rgb_max) b = rgb_max;
                }

                *(argb_ptr++) = alpha;
                *(argb_ptr++) = r;
                *(argb_ptr++) = g;
                *(argb_ptr++) = b;
            }
        }

        // Advance the row pointers into each lowpass band
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
        if (num_channels == 4)
            a_row_ptr += a_row_pitch;

        // Advance the row pointer into the output buffer
        argb_row_ptr += output_pitch;
    }
}

void ConvertLowpassRGB444ToRGB30(PIXEL *plane_array[], int pitch_array[],
                                 uint8_t *output_buffer, int output_pitch,
                                 ROI roi, bool inverted, int shift, int format)
{
    PIXEL *r_row_ptr;
    PIXEL *g_row_ptr;
    PIXEL *b_row_ptr;
    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    //int r_prescale;
    //int g_prescale;
    //int b_prescale;
    unsigned int *rgb_row_ptr;

    int output_height = roi.height;

    //size_t output_row_size = output_pitch;

    int row;
    int column;

    const bool saturate = 1;
    const int rgb_max = USHRT_MAX;

    // Get pointers to the rows in the lowpass band of each channel
    r_row_ptr = plane_array[1];
    r_row_pitch = pitch_array[1] / sizeof(PIXEL);
    g_row_ptr = plane_array[0];
    g_row_pitch = pitch_array[0] / sizeof(PIXEL);
    b_row_ptr = plane_array[2];
    b_row_pitch = pitch_array[2] / sizeof(PIXEL);

    // Convert the output pitch to units of pixels
    output_pitch /= sizeof(int);

    rgb_row_ptr = (unsigned int *)output_buffer;
    if (inverted)
    {
        rgb_row_ptr += (output_height - 1) * output_pitch;
        output_pitch = NEG(output_pitch);
    }

    for (row = 0; row < output_height; row++)
    {
        unsigned int *rgb_ptr = &rgb_row_ptr[0];
        // Start at the leftmost column
        column = 0;


        // Process the rest of the row
        for (; column < roi.width; column++)
        {
            int r;
            int g;
            int b;
            int rgb = 0;

            // Load the tuple of RGB values
            r = r_row_ptr[column];
            g = g_row_ptr[column];
            b = b_row_ptr[column];

            // Scale the lowpass values to 16 bits
            r <<= shift;
            g <<= shift;
            b <<= shift;

            if (saturate)
            {
                if (r < 0) r = 0;
                else if (r > rgb_max) r = rgb_max;
                if (g < 0) g = 0;
                else if (g > rgb_max) g = rgb_max;
                if (b < 0) b = 0;
                else if (b > rgb_max) b = rgb_max;
            }

            r >>= 6; // 10-bit
            g >>= 6; // 10-bit
            b >>= 6; // 10-bit

            switch (format)
            {
                case DECODED_FORMAT_RG30:
                case DECODED_FORMAT_AB10:
                    g <<= 10;
                    b <<= 20;
                    rgb |= r;
                    rgb |= g;
                    rgb |= b;

                    *rgb_ptr++ = rgb;
                    break;

                case DECODED_FORMAT_AR10:
                    g <<= 10;
                    r <<= 20;
                    rgb |= r;
                    rgb |= g;
                    rgb |= b;

                    *rgb_ptr++ = rgb;
                    break;

                case DECODED_FORMAT_R210:
                    //b <<= 0;
                    g <<= 10;
                    r <<= 20;
                    rgb |= r;
                    rgb |= g;
                    rgb |= b;

                    *rgb_ptr++ = _bswap(rgb);
                    break;
                case DECODED_FORMAT_DPX0:
                    r <<= 22;
                    g <<= 12;
                    b <<= 2;
                    rgb |= r;
                    rgb |= g;
                    rgb |= b;

                    *rgb_ptr++ = _bswap(rgb);
                    break;
            }
        }

        // Advance the row pointers into each lowpass band
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;

        // Advance the row pointer into the output buffer
        rgb_row_ptr += output_pitch;
    }
}


// Convert QuickTime format r408, v408 to a frame of planar YUV 4:2:2
void ConvertYUVAToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer, int format)
{
    //#pragma unused(buffer);

    const int num_channels = 3;

    uint8_t *yuva_row_ptr = data;
    int yuva_row_pitch = pitch;

    PIXEL *color_plane[3];
    int color_pitch[3];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *y_row_ptr;
    uint8_t *u_row_ptr;
    uint8_t *v_row_ptr;

    int y_row_pitch;
    int u_row_pitch;
    int v_row_pitch;

    //const int max_yuv = 1023;			// Maximum pixel value at 10 bit precision

    // CCIR black and white for 10-bit pixels
    //const int yuv_black = (16 << 2);
    //const int yuv_white = (235 << 2);
    //const int yuv_scale = (yuv_white - yuv_black);

    // Neutral chroma value for 10-bit pixels
    //const int yuv_neutral = (128 << 2);

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == num_channels);
    assert(frame->format == FRAME_FORMAT_YUV);

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }


    // Swap the chroma values
    y_row_ptr = (uint8_t *)color_plane[0];
    y_row_pitch = color_pitch[0];
    u_row_ptr = (uint8_t *)color_plane[2];
    u_row_pitch = color_pitch[2];
    v_row_ptr = (uint8_t *)color_plane[1];
    v_row_pitch = color_pitch[1];

    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

        // Pointer into the YUVA input row
        uint8_t *yuva_ptr = (uint8_t *)yuva_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *y_ptr = (PIXEL16U *)y_row_ptr;
        PIXEL16U *u_ptr = (PIXEL16U *)u_row_ptr;
        PIXEL16U *v_ptr = (PIXEL16U *)v_row_ptr;

        // Process the rest of the column

        switch (format)
        {
            case COLOR_FORMAT_V408: //UYVA
                for (; column < frame_width; column += 2)
                {
                    PIXEL16U y1, y2;
                    PIXEL16U u;
                    PIXEL16U v;

                    // Load the first set of UYVA values
                    u = *(yuva_ptr++) << 1; // bump to 10-bit (u1+u2) = 10-bit
                    y1 = *(yuva_ptr++) << 2;
                    v = *(yuva_ptr++) << 1;
                    yuva_ptr++; // alpha

                    // Load the second set of YUVA values (skip the alpha value)
                    u += *(yuva_ptr++) << 1; // bump to 10-bit
                    y2 = *(yuva_ptr++) << 2;
                    v += *(yuva_ptr++) << 1;
                    yuva_ptr++; // alpha

                    // Output the first pixel
                    *(y_ptr++) = y1;
                    *(u_ptr++) = u;
                    // Output the second pixel
                    *(y_ptr++) = y2;
                    *(v_ptr++) = v;
                }
                break;

            case COLOR_FORMAT_R408: //AYUV
                for (; column < frame_width; column += 2)
                {
                    PIXEL16U y1, y2;
                    PIXEL16U u;
                    PIXEL16U v;

                    // Load the first set of UYVA values
                    yuva_ptr++; // alpha
                    y1 = *(yuva_ptr++) << 2;
                    u = *(yuva_ptr++) << 1; // bump to 10-bit (u1+u2) = 10-bit
                    v = *(yuva_ptr++) << 1;

                    // Load the second set of YUVA values (skip the alpha value)
                    yuva_ptr++; // alpha
                    y2 = *(yuva_ptr++) << 2;
                    u += *(yuva_ptr++) << 1; // bump to 10-bit
                    v += *(yuva_ptr++) << 1;

                    // Output the first pixel
                    *(y_ptr++) = y1 + 64; // convert 0-219 range to 16-235 before encoding
                    *(u_ptr++) = u;
                    // Output the second pixel
                    *(y_ptr++) = y2 + 64;
                    *(v_ptr++) = v;
                }
                break;
        }

        // Advance the row pointers
        yuva_row_ptr += yuva_row_pitch;
        y_row_ptr += y_row_pitch;
        u_row_ptr += u_row_pitch;
        v_row_ptr += v_row_pitch;
    }
}



// Convert QuickTime format r4fl to a frame of planar YUV 4:2:2
void ConvertYUVAFloatToFrame16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    //#pragma unused(buffer);

    const int num_channels = 3;

    uint8_t *yuva_row_ptr = data;
    int yuva_row_pitch = pitch;

    PIXEL *color_plane[3];
    int color_pitch[3];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *y_row_ptr;
    uint8_t *u_row_ptr;
    uint8_t *v_row_ptr;

    int y_row_pitch;
    int u_row_pitch;
    int v_row_pitch;

    const int max_yuv = 1023;			// Maximum pixel value at 10 bit precision

    const float r4fl_white = 0.859f;		// CCIR white in the r4fl pixel format
    const float r4fl_neutral = 0.502f;	// Neutral chroma in the r4fl pixel format

#if 1
    // CCIR black and white for 10-bit pixels
    const int yuv_black = (16 << 2);
    const int yuv_white = (235 << 2);
    const int yuv_scale = (yuv_white - yuv_black);
#else
    // CCIR black and white for 10-bit pixels
    const int yuv_black = 0;
    const int yuv_white = (235 << 2);
    const int yuv_scale = (yuv_white - yuv_black);
#endif

    // Neutral chroma value for 10-bit pixels
    const int yuv_neutral = (128 << 2);

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == num_channels);
    assert(frame->format == FRAME_FORMAT_YUV);

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

#if 0
    // Invert the input image
    rgb_row_ptr += (display_height - 1) * rgb_row_pitch;
    rgb_row_pitch = NEG(rgb_row_pitch);
#endif

    // Swap the chroma values
    y_row_ptr = (uint8_t *)color_plane[0];
    y_row_pitch = color_pitch[0];
    u_row_ptr = (uint8_t *)color_plane[2];
    u_row_pitch = color_pitch[2];
    v_row_ptr = (uint8_t *)color_plane[1];
    v_row_pitch = color_pitch[1];

    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

        //TODO: Add optimized code
#if (1 && XMMOPT)

#endif

        // Pointer into the YUVA input row
        float *yuva_ptr = (float *)yuva_row_ptr;

        // Pointers into the YUV output rows
        PIXEL16U *y_ptr = (PIXEL16U *)y_row_ptr;
        PIXEL16U *u_ptr = (PIXEL16U *)u_row_ptr;
        PIXEL16U *v_ptr = (PIXEL16U *)v_row_ptr;

        // Process the rest of the column
        for (; column < frame_width; column += 2)
        {
            float y;
            float uA, uB;
            float vA, vB;

            int y1;
            int y2;
            int u1;
            int v1;

            // Load the first set of YUVA values (skip the alpha value)
            yuva_ptr++;
            y = *(yuva_ptr++);
            uA = *(yuva_ptr++);
            vA = *(yuva_ptr++);

            // Clamp to black (this removes superblack)
            if (y < 0.0) y = 0.0;

            // Convert floating-point to 10-bit integer
            y1 = (int)((y / r4fl_white) * yuv_scale + yuv_black);
            if (y1 < 0) y1 = 0;
            else if (y1 > max_yuv) y1 = max_yuv;

            // Load the second set of YUVA values (skip the alpha value)
            yuva_ptr++;
            y = *(yuva_ptr++);
            uB = *(yuva_ptr++);
            vB = *(yuva_ptr++);

            // Clamp to black (this removes superblack)
            if (y < 0.0) y = 0.0;

            // Convert floating-point to 10-bit integer
            // Clamp the luma and chroma values
            y2 = (int)((y / r4fl_white) * yuv_scale + yuv_black);
            if (y2 < 0) y2 = 0;
            else if (y2 > max_yuv) y2 = max_yuv;


            // Clamp the luma and chroma values to 10 bits
            u1 = (int)(((uA + uB) / r4fl_neutral) * yuv_neutral * 0.5f);
            if (u1 < 0) u1 = 0;
            else if (u1 > max_yuv) u1 = max_yuv;
            v1 = (int)(((vA + vB) / r4fl_neutral) * yuv_neutral * 0.5f);
            if (v1 < 0) v1 = 0;
            else if (v1 > max_yuv) v1 = max_yuv;

            // Output the first pixel
            *(y_ptr++) = y1;
            *(u_ptr++) = u1;

            // Output the second pixel
            *(y_ptr++) = y2;
            *(v_ptr++) = v1;
        }

        // Advance the row pointers
        yuva_row_ptr += yuva_row_pitch;
        y_row_ptr += y_row_pitch;
        u_row_ptr += u_row_pitch;
        v_row_ptr += v_row_pitch;
    }
}

// Convert QuickTime format r4fl to a frame of planar RGB 4:4:4
void ConvertYUVAFloatToFrame_RGB444_16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    //#pragma unused(buffer);

    const int num_channels = 3;

    // Assume computer systems 709 color space for r4fl
    //DAN20080716 -- not sure if r4fl is always CG 709
    COLOR_SPACE color_space = (COLOR_SPACE)COLOR_SPACE_BT_709;

    uint8_t *yuva_row_ptr = data;
    int yuva_row_pitch = pitch;

    PIXEL *color_plane[3];
    int color_pitch[3];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;

    int luma_offset;

    float ymult;
    float r_vmult;
    float g_vmult;
    float g_umult;
    float b_umult;

    const int max_rgb = 4095;			// Maximum pixel value at 12 bit precision

    //const float r4fl_white = 0.859f;		// CCIR white in the r4fl pixel format
    const float r4fl_neutral = 0.502f;	// Neutral chroma in the r4fl pixel format

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    // The frame format should be three channels of RGB (4:4:4 format)
    assert(frame->num_channels == num_channels);
    assert(frame->format == FRAME_FORMAT_RGB);

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // RGB planes are stored in the order G, R, B
    r_row_ptr = (uint8_t *)color_plane[1];
    g_row_ptr = (uint8_t *)color_plane[0];
    b_row_ptr = (uint8_t *)color_plane[2];

    r_row_pitch = color_pitch[1];
    g_row_pitch = color_pitch[0];
    b_row_pitch = color_pitch[2];

    // Initialize the color conversion constants (floating-point version)
    switch (color_space & COLORSPACE_MASK)
    {
        case COLOR_SPACE_CG_601:	// Computer systems 601
            luma_offset = 16;
            ymult =   1.164f;
            r_vmult = 1.596f;
            g_vmult = 0.813f;
            g_umult = 0.391f;
            b_umult = 2.018f;
            break;

        case COLOR_SPACE_VS_601:	// Video systems 601
            luma_offset = 0;
            ymult =     1.0f;
            r_vmult = 1.371f;
            g_vmult = 0.698f;
            g_umult = 0.336f;
            b_umult = 1.732f;
            break;

        default:
            assert(0);
        case COLOR_SPACE_CG_709:	// Computer systems 709
            luma_offset = 16;
            ymult =   1.164f;
            r_vmult = 1.793f;
            g_vmult = 0.534f;
            g_umult = 0.213f;
            b_umult = 2.115f;
            break;

        case COLOR_SPACE_VS_709:	// Video Systems 709
            luma_offset = 0;
            ymult =     1.0f;
            r_vmult = 1.540f;
            g_vmult = 0.459f;
            g_umult = 0.183f;
            b_umult = 1.816f;
            break;
    }

    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

#if (1 && XMMOPT)
        //TODO: Add optimized code
#endif
        // Pointer into the YUVA input row
        float *yuva_ptr = (float *)yuva_row_ptr;

        // Pointers into the RGB output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;

        // Process the rest of the column
        for (; column < frame_width; column++)
        {
            // Get the next pixel
            //float a1 = *(yuva_ptr++);
            float y1 = *(yuva_ptr++);
            float v1 = *(yuva_ptr++);	// Cb
            float u1 = *(yuva_ptr++);	// Cr

            float r1, g1, b1, t1, t2;

            int r1_out, g1_out, b1_out;

            // Subtract the chroma offsets
            u1 -= r4fl_neutral;
            v1 -= r4fl_neutral;

            r1 = ymult * y1;
            t1 = r_vmult * u1;
            r1 += t1;

            g1 = ymult * y1;
            t1 = g_vmult * u1;
            g1 -= t1;
            t2 = g_umult * v1;
            g1 -= t2;

            b1 = ymult * y1;
            t1 = b_umult * v1;
            b1 += t1;

            // Convert to integer values
            r1_out = (int)(r1 * (float)max_rgb);
            g1_out = (int)(g1 * (float)max_rgb);
            b1_out = (int)(b1 * (float)max_rgb);

            // Force the RGB values into valid range
            if (r1_out < 0) r1_out = 0;
            if (g1_out < 0) g1_out = 0;
            if (b1_out < 0) b1_out = 0;

            if (r1_out > max_rgb) r1_out = max_rgb;
            if (g1_out > max_rgb) g1_out = max_rgb;
            if (b1_out > max_rgb) b1_out = max_rgb;

            *(r_ptr++) = r1_out;
            *(g_ptr++) = g1_out;
            *(b_ptr++) = b1_out;
        }

        // Advance the row pointers
        yuva_row_ptr += yuva_row_pitch;
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
    }
}

// Convert QuickTime format r4fl to a frame of planar RGBA 4:4:4:4
void ConvertYUVAFloatToFrame_RGBA4444_16s(uint8_t *data, int pitch, FRAME *frame, uint8_t *buffer)
{
    const int num_channels = FRAME_MAX_CHANNELS;

    // Assume computer systems 709 color space for r4fl
    //DAN20080716 -- not sure if r4fl is always CG 709
    COLOR_SPACE color_space = (COLOR_SPACE)COLOR_SPACE_BT_709;

    uint8_t *yuva_row_ptr = data;
    int yuva_row_pitch = pitch;

    PIXEL *color_plane[FRAME_MAX_CHANNELS];
    int color_pitch[FRAME_MAX_CHANNELS];
    int frame_width;
    int frame_height;
    int display_height;
    int row;
    int i;

    uint8_t *r_row_ptr;
    uint8_t *g_row_ptr;
    uint8_t *b_row_ptr;
    uint8_t *a_row_ptr = NULL;

    int r_row_pitch;
    int g_row_pitch;
    int b_row_pitch;
    int a_row_pitch = 0;

    int luma_offset;

    float ymult;
    float r_vmult;
    float g_vmult;
    float g_umult;
    float b_umult;

    const int max_rgb = 4095;			// Maximum pixel value at 12 bit precision

    //const float r4fl_white = 0.859f;		// CCIR white in the r4fl pixel format
    const float r4fl_neutral = 0.502f;	// Neutral chroma in the r4fl pixel format

    assert(frame != NULL);
    if (! (frame != NULL)) return;

    // The frame format should be four channels of RGBA (4:4:4:4 format)
    assert(frame->num_channels == num_channels);
    assert(frame->format == FRAME_FORMAT_RGBA);

    display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < num_channels; i++)
    {
        IMAGE *image = frame->channel[i];

        assert(frame->channel[i] != NULL);

        // Set the pointer to the individual planes and pitch for each channel
        color_plane[i] = image->band[0];
        color_pitch[i] = image->pitch;

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            frame_width = image->width;
            frame_height = image->height;
        }
    }

    // RGB planes are stored in the order G, R, B
    r_row_ptr = (uint8_t *)color_plane[1];
    g_row_ptr = (uint8_t *)color_plane[0];
    b_row_ptr = (uint8_t *)color_plane[2];
    a_row_ptr = (uint8_t *)color_plane[3];

    r_row_pitch = color_pitch[1];
    g_row_pitch = color_pitch[0];
    b_row_pitch = color_pitch[2];
    a_row_pitch = color_pitch[3];

    // Initialize the color conversion constants (floating-point version)
    switch (color_space & COLORSPACE_MASK)
    {
        case COLOR_SPACE_CG_601:	// Computer systems 601
            luma_offset = 16;
            ymult =   1.164f;
            r_vmult = 1.596f;
            g_vmult = 0.813f;
            g_umult = 0.391f;
            b_umult = 2.018f;
            break;

        case COLOR_SPACE_VS_601:	// Video systems 601
            luma_offset = 0;
            ymult =     1.0f;
            r_vmult = 1.371f;
            g_vmult = 0.698f;
            g_umult = 0.336f;
            b_umult = 1.732f;
            break;

        default:
            assert(0);
        case COLOR_SPACE_CG_709:	// Computer systems 709
            luma_offset = 16;
            ymult =   1.164f;
            r_vmult = 1.793f;
            g_vmult = 0.534f;
            g_umult = 0.213f;
            b_umult = 2.115f;
            break;

        case COLOR_SPACE_VS_709:	// Video Systems 709
            luma_offset = 0;
            ymult =     1.0f;
            r_vmult = 1.540f;
            g_vmult = 0.459f;
            g_umult = 0.183f;
            b_umult = 1.816f;
            break;
    }

    for (row = 0; row < display_height; row++)
    {
        // Start at the leftmost column
        int column = 0;

#if (1 && XMMOPT)
        //TODO: Add optimized code
#endif
        // Pointer into the YUVA input row
        float *yuva_ptr = (float *)yuva_row_ptr;

        // Pointers into the RGB output rows
        PIXEL16U *r_ptr = (PIXEL16U *)r_row_ptr;
        PIXEL16U *g_ptr = (PIXEL16U *)g_row_ptr;
        PIXEL16U *b_ptr = (PIXEL16U *)b_row_ptr;
        PIXEL16U *a_ptr = (PIXEL16U *)a_row_ptr;

        // Process the rest of the column
        for (; column < frame_width; column++)
        {
            // Get the next pixel
            float a1 = *(yuva_ptr++);
            float y1 = *(yuva_ptr++);
            float v1 = *(yuva_ptr++);	// Cb
            float u1 = *(yuva_ptr++);	// Cr

            float r1, g1, b1, t1, t2;

            int r1_out, g1_out, b1_out, a1_out;

            // Subtract the chroma offsets
            u1 -= r4fl_neutral;
            v1 -= r4fl_neutral;

            r1 = ymult * y1;
            t1 = r_vmult * u1;
            r1 += t1;

            g1 = ymult * y1;
            t1 = g_vmult * u1;
            g1 -= t1;
            t2 = g_umult * v1;
            g1 -= t2;

            b1 = ymult * y1;
            t1 = b_umult * v1;
            b1 += t1;

            // Convert to integer values
            r1_out = (int)(r1 * (float)max_rgb);
            g1_out = (int)(g1 * (float)max_rgb);
            b1_out = (int)(b1 * (float)max_rgb);
            a1_out = (int)(a1 * (float)max_rgb);

            // Force the RGB values into valid range
            if (r1_out < 0) r1_out = 0;
            if (g1_out < 0) g1_out = 0;
            if (b1_out < 0) b1_out = 0;
            if (a1_out < 0) a1_out = 0;

            if (r1_out > max_rgb) r1_out = max_rgb;
            if (g1_out > max_rgb) g1_out = max_rgb;
            if (b1_out > max_rgb) b1_out = max_rgb;
            if (a1_out > max_rgb) b1_out = max_rgb;

            *(r_ptr++) = r1_out;
            *(g_ptr++) = g1_out;
            *(b_ptr++) = b1_out;
            *(a_ptr++) = a1_out;
        }

        // Advance the row pointers
        yuva_row_ptr += yuva_row_pitch;
        r_row_ptr += r_row_pitch;
        g_row_ptr += g_row_pitch;
        b_row_ptr += b_row_pitch;
        a_row_ptr += a_row_pitch;
    }
}


// Convert the input frame from YUV422 to RGB
void ConvertLowpass16sToRGBNoIPPFast(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch, int format, int colorspace, bool inverted, int descale)
{
    PIXEL *plane[3];
    int pitch[3];
    ROI roi;
    int channel;

    //CG_601
    int y_offset = 16; // not VIDEO_RGB & not YUV709
    int ymult = 128 * 149;	//7bit 1.164
    int r_vmult = 204;		//7bit 1.596
    int g_vmult = 208;		//8bit 0.813
    int g_umult = 100;		//8bit 0.391
    int b_umult = 129;		//6bit 2.018
    int saturate = 1;

    //if(colorspace & COLOR_SPACE_422_TO_444)
    //{
    //	upconvert422to444 = 1;
    //}

    switch (colorspace & COLORSPACE_MASK)
    {
        case COLOR_SPACE_CG_601:
            y_offset = 16;		// not VIDEO_RGB & not YUV709
            ymult = 128 * 149;	//7bit 1.164
            r_vmult = 204;		//7bit 1.596
            g_vmult = 208;		//8bit 0.813
            g_umult = 100;		//8bit 0.391
            b_umult = 129;		//6bit 2.018
            saturate = 1;
            break;

        default:
            assert(0);
        case COLOR_SPACE_CG_709:
            y_offset = 16;
            ymult = 128 * 149;	//7bit 1.164
            r_vmult = 230;		//7bit 1.793
            g_vmult = 137;		//8bit 0.534
            g_umult = 55;		//8bit 0.213
            b_umult = 135;		//6bit 2.115
            saturate = 1;
            break;

        case COLOR_SPACE_VS_601:
            y_offset = 0;
            ymult = 128 * 128;	//7bit 1.0
            r_vmult = 175;		//7bit 1.371
            g_vmult = 179;		//8bit 0.698
            g_umult = 86;		//8bit 0.336
            b_umult = 111;		//6bit 1.732
            saturate = 0;
            break;

        case COLOR_SPACE_VS_709:
            y_offset = 0;
            ymult = 128 * 128;	//7bit 1.0
            r_vmult = 197;		//7bit 1.540
            g_vmult = 118;		//8bit 0.459
            g_umult = 47;		//8bit 0.183
            b_umult = 116;		//6bit 1.816
            saturate = 0;
            break;
    }



#if (_USE_YCBCR == 0) || (_ENABLE_GAMMA_CORRECTION == 1)
    // Check that the correct compiler time switches are set correctly
    assert(0);
#endif

    // We use 16-bit fixed-point arithmetic to approximate the color conversion coefficients
    //
    // Floating point arithmetic is
    //
    // R = 1.164 * (Y - 16) + 1.596 * (V - 128);
    // G = 1.164 * (Y - 16) - 0.813 * (V - 128) - 0.392 * (U - 128);
    // B = 1.164 * (Y - 16) + 2.017 * (U - 128);
    //
    // Fixed point approximation (8-bit) is
    //
    // Y = (Y << 1) -  32;
    // U = (U << 1) - 256;
    // V = (V << 1) - 256;
    // R = (149 * Y + 204 * V) >> 8;
    // G = (149 * Y - 104 * V - 50 * U) >> 8;
    // B = (149 * Y + 258 * U) >> 8;
    //
    // Fixed point approximation (7-bit) is
    //
    // Y = (Y << 1) -  16;
    // U = (U << 1) - 256;
    // V = (V << 1) - 256;
    // R = (74 * Y + 102 * V) >> 7;
    // G = (74 * Y -  52 * V - 25 * U) >> 7;
    // B = (74 * Y + 129 * U) >> 7;
    //
    // We use 7-bit arithmetic

    // New 7 bit version to fix rounding errors 2/26/03
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = (149 * Y           + 204 * V) >> 7;
    // G = (149 * Y -  50 * U - 104 * V) >> 7;
    // B = (149 * Y + 258 * U) >> 7;
    //
    // New 6 bit version to fix rounding errors
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = ((149 * Y>>1)           + 102 * V) >> 6;
    // G = ((149 * Y>>1) -  25 * U - 52 * V) >> 6;
    // B = ((149 * Y>>1) + 129 * U) >> 6;



    // Bt.709
    // R = 1.164 * (Y - 16)                     + 1.793 * (V - 128);
    // G = 1.164 * (Y - 16) - 0.213 * (U - 128) - 0.534 * (V - 128);
    // B = 1.164 * (Y - 16) + 2.115 * (U - 128);
    //
    //
    // We use 7-bit arithmetic
    // Y = Y - 16;
    // U = U - 128;
    // V = V - 128;
    // R = (149 * Y           + 229 * V) >> 7;     // 229.5
    // G = (149 * Y -  27 * U - 68 * V) >> 7;		 //27.264  68.35
    // B = (149 * Y + 271 * U) >> 7;				 // 270.72
    //
    // New 6 bit version to fix rounding errors
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = ((149 * Y>>1)           + 115 * V - (V>>2)) >> 6;		//114.752
    // G = ((149 * Y>>1) -  ((109*U)>>3) - ((137*V)>>2) + (V>>2)) >> 6;			//13.632 approx 8 * 13.632 = 109,  137-0.25
    // B = ((149 * Y>>1) + 135 * U + (U>>2)) >> 6;
    //
    // New 6 bit version crude
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = ((149 * Y>>1)           + 115 * V ) >> 6;		//114.752
    // G = ((149 * Y>>1) -  14 * U -  34 * V ) >> 6;		//13.632 approx 8 * 13.632 = 109,  137-0.25
    // B = ((149 * Y>>1) + 135 * U           ) >> 6;


    // Only 24 and 32 bit true color RGB formats are supported
    assert(format == COLOR_FORMAT_RGB24 || format == COLOR_FORMAT_RGB32);

    // Convert from pixel to byte data
    for (channel = 0; channel < 3; channel++)
    {
        IMAGE *image = images[channel];

        plane[channel] = (PIXEL *)(image->band[0]);
        pitch[channel] = image->pitch / sizeof(PIXEL);

        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    // Output to RGB24 format?
    if (format == COLOR_FORMAT_RGB24)
    {
        PIXEL *Y_row, *U_row, *V_row;
        int Y_pitch, U_pitch, V_pitch;
        int Y_prescale, U_prescale, V_prescale;
        uint8_t *RGB_row;
        int row, column;

        Y_row = plane[0];
        Y_pitch = pitch[0];
        Y_prescale = descale + PRESCALE_LUMA;
        U_row = plane[1];
        U_pitch = pitch[1];
        U_prescale = descale + PRESCALE_CHROMA;
        V_row = plane[2];
        V_pitch = pitch[2];
        V_prescale = descale + PRESCALE_CHROMA;

        RGB_row = &output_buffer[0];
        if (inverted)
        {
            RGB_row += (output_height - 1) * output_pitch;
            output_pitch = -output_pitch;
        }

        for (row = 0; row < output_height; row++)
        {
            //int column_step = 16;
            //int post_column = roi.width - (roi.width % column_step);
            //uint8_t *RGB_ptr = RGB_row;
            //int *RGB_int_ptr = (int *)RGB_ptr;

            column = 0;

            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column += 2)
            {
                int R, G, B;
                int Y, U, V;
                uint8_t *RGB_ptr = &RGB_row[column * 3];

                // Convert the first set of YCbCr values
                if (saturate)
                {
                    Y = SATURATE_Y(Y_row[column]    >> Y_prescale);
                    V = SATURATE_Cr(U_row[column / 2] >> V_prescale);
                    U = SATURATE_Cb(V_row[column / 2] >> U_prescale);
                }
                else
                {
                    Y = (Y_row[column]   >> Y_prescale);
                    V = (U_row[column / 2] >> V_prescale);
                    U = (V_row[column / 2] >> U_prescale);
                }

                Y = Y - y_offset;
                U = U - 128;
                V = V - 128;

                Y = Y * ymult >> 7;

                R = (Y           + r_vmult * V) >> 7;
                G = (Y * 2 -  g_umult * U - g_vmult * V) >> 8;
                B = (Y + 2 * b_umult * U) >> 7;

                RGB_ptr[0] = SATURATE_8U(B);
                RGB_ptr[1] = SATURATE_8U(G);
                RGB_ptr[2] = SATURATE_8U(R);

                // Convert the second set of YCbCr values
                if (saturate)
                    Y = SATURATE_Y(Y_row[column + 1] >> Y_prescale);
                else
                    Y = (Y_row[column + 1] >> Y_prescale);

                Y = Y - y_offset;
                Y = Y * ymult >> 7;

                R = (Y           + r_vmult * V) >> 7;
                G = (Y * 2 -  g_umult * U - g_vmult * V) >> 8;
                B = (Y + 2 * b_umult * U) >> 7;

                RGB_ptr[3] = SATURATE_8U(B);
                RGB_ptr[4] = SATURATE_8U(G);
                RGB_ptr[5] = SATURATE_8U(R);
            }

            // Fill the rest of the output row with black
            for (; column < output_width; column++)
            {
                uint8_t *RGB_ptr = &RGB_row[column * 3];

                RGB_ptr[0] = 0;
                RGB_ptr[1] = 0;
                RGB_ptr[2] = 0;
            }

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;

            // Advance the RGB pointers
            RGB_row += output_pitch;
        }
    }

    else	// Output format is RGB32 so set the alpha channel to the default
    {
        PIXEL *Y_row, *U_row, *V_row;
        int Y_pitch, U_pitch, V_pitch;
        int Y_prescale, U_prescale, V_prescale;
        uint8_t *RGBA_row;
        int row, column;
        //int column_step = 2;

        Y_row = plane[0];
        Y_pitch = pitch[0];
        Y_prescale = descale + PRESCALE_LUMA;
        U_row = plane[1];
        U_pitch = pitch[1];
        U_prescale = descale + PRESCALE_CHROMA;
        V_row = plane[2];
        V_pitch = pitch[2];
        V_prescale = descale + PRESCALE_CHROMA;

        RGBA_row = &output_buffer[0];
        if (inverted)
        {
            RGBA_row += (output_height - 1) * output_pitch;
            output_pitch = -output_pitch;
        }

        for (row = 0; row < output_height; row++)
        {
            int column_step = 16;
            int post_column = roi.width - (roi.width % column_step);
            __m128i *Y_ptr = (__m128i *)Y_row;
            __m128i *U_ptr = (__m128i *)U_row;
            __m128i *V_ptr = (__m128i *)V_row;
            __m128i *RGBA_ptr = (__m128i *)RGBA_row;

            column = 0;
#if 1
            // Convert the YUV422 frame back into RGB in sets of 2 pixels
            for (; column < post_column; column += column_step)
            {
                __m128i R1, G1, B1;
                __m128i R2, G2, B2;
                __m128i R_pi8, G_pi8, B_pi8;
                __m128i Y, U, V;
                __m128i Y_pi8, U_pi8, V_pi8;
                __m128i temp, temp2;
                __m128i RGBA;

                /***** Load sixteen YCbCr values and eight each U, V value *****/

                // Convert 16-bit signed lowpass pixels into 8-bit unsigned pixels,
                // packing into three 128-bit SSE vectors (one per channel).
                // Otto: Yes, I see that the U comes from the V pointer, and the V
                // comes from the U pointer. I don't know why.
                temp  = *Y_ptr++;
                temp2 = *Y_ptr++;
                temp  = _mm_srai_epi16(temp,  Y_prescale);
                temp2 = _mm_srai_epi16(temp2, Y_prescale);
                Y_pi8 = _mm_packus_epi16(temp, temp2);

                temp  = *U_ptr++;
                temp  = _mm_srai_epi16(temp, V_prescale);
                V_pi8 = _mm_packus_epi16(temp, _mm_setzero_si128());

                temp  = *V_ptr++;
                temp  = _mm_srai_epi16(temp, U_prescale);
                U_pi8 = _mm_packus_epi16(temp, _mm_setzero_si128());

#if STRICT_SATURATE
                // Perform strict saturation on YUV if required
                if (saturate)
                {
                    Y_pi8 = _mm_subs_epu8(Y_pi8, _mm_set1_pi8(16));
                    Y_pi8 = _mm_adds_epu8(Y_pi8, _mm_set1_pi8(36));	// 36 = 16 + 20 = 16 + (255-235)
                    Y_pi8 = _mm_subs_epu8(Y_pi8, _mm_set1_pi8(20));

                    U_pi8 = _mm_subs_epu8(U_pi8, _mm_set1_pi8(16));
                    U_pi8 = _mm_adds_epu8(U_pi8, _mm_set1_pi8(31));	// 31 = 16 + 15 = 16 + (255-240)
                    U_pi8 = _mm_subs_epu8(U_pi8, _mm_set1_pi8(15));

                    V_pi8 = _mm_subs_epu8(V_pi8, _mm_set1_pi8(16));
                    V_pi8 = _mm_adds_epu8(V_pi8, _mm_set1_pi8(31));	// 31 = 16 + 15 = 16 + (255-240)
                    V_pi8 = _mm_subs_epu8(V_pi8, _mm_set1_pi8(15));
                }
#endif

                /***** Calculate the first eight RGB values *****/

                // Unpack the first eight Y values
                Y = _mm_unpacklo_epi8(Y_pi8, _mm_setzero_si128());

                // Set the first eight U,V values (duplicating the first four)
                U = _mm_unpacklo_epi8(U_pi8, _mm_setzero_si128());
                V = _mm_unpacklo_epi8(V_pi8, _mm_setzero_si128());
                {
                    m128i lo, hi;
                    m128i mask;
                    mask.u64[0] = ~0ULL;
                    mask.u64[1] = 0;
                    lo.m128 = _mm_shufflelo_epi16(U, _MM_SHUFFLE(1, 1, 0, 0));
                    lo.m128 = _mm_and_si128(lo.m128, mask.m128);
                    hi.m128 = _mm_shufflelo_epi16(U, _MM_SHUFFLE(3, 3, 2, 2));
                    hi.m128 = _mm_slli_si128(hi.m128, 8);
                    U = _mm_or_si128(lo.m128, hi.m128);

                    lo.m128 = _mm_shufflelo_epi16(V, _MM_SHUFFLE(1, 1, 0, 0));
                    lo.m128 = _mm_and_si128(lo.m128, mask.m128);
                    hi.m128 = _mm_shufflelo_epi16(V, _MM_SHUFFLE(3, 3, 2, 2));
                    hi.m128 = _mm_slli_si128(hi.m128, 8);
                    V = _mm_or_si128(lo.m128, hi.m128);
                }


                // Convert YUV to RGB

                //				Y = _mm_slli_pi16(Y, 1);
                //				U = _mm_slli_pi16(U, 1);
                //				V = _mm_slli_pi16(V, 1);

                temp = _mm_set1_epi16( y_offset);
                Y = _mm_subs_epi16(Y, temp);
                temp = _mm_set1_epi16(128);
                U = _mm_subs_epi16(U, temp);
                V = _mm_subs_epi16(V, temp);

                Y = _mm_slli_epi16(Y, 7);			// This code fix an overflow case where very bright
                temp = _mm_set1_epi16(ymult);		//pixel with some color produced interim values over 32768
                Y = _mm_mulhi_epi16(Y, temp);
                Y = _mm_slli_epi16(Y, 1);

                // Calculate R
                temp = _mm_set1_epi16(r_vmult);
                temp = _mm_mullo_epi16(V, temp);
                temp = _mm_srai_epi16(temp, 1); //7bit to 6
                R1 = _mm_adds_epi16(Y, temp);
                R1 = _mm_srai_epi16(R1, 6);

                // Calculate G
                temp = _mm_set1_epi16(g_vmult);
                temp = _mm_mullo_epi16(V, temp);
                temp = _mm_srai_epi16(temp, 2); //8bit to 6
                G1 = _mm_subs_epi16(Y, temp);
                temp = _mm_set1_epi16(g_umult);
                temp = _mm_mullo_epi16(U, temp);
                temp = _mm_srai_epi16(temp, 2); //8bit to 6
                G1 = _mm_subs_epi16(G1, temp);
                G1 = _mm_srai_epi16(G1, 6);

                // Calculate B
                temp = _mm_set1_epi16(b_umult);
                temp = _mm_mullo_epi16(U, temp);
                B1 = _mm_adds_epi16(Y, temp);
                B1 = _mm_srai_epi16(B1, 6);


                /***** Calculate the second eight RGB values *****/

                // Unpack the second eight Y values
                Y = _mm_unpackhi_epi8(Y_pi8, _mm_setzero_si128());

                // Unpack the second eight U,V values (duplicating the
                // second four)
                U = _mm_unpacklo_epi8(U_pi8, _mm_setzero_si128());
                V = _mm_unpacklo_epi8(V_pi8, _mm_setzero_si128());
                {
                    m128i lo, hi;
                    m128i mask;
                    mask.u64[0] = 0;
                    mask.u64[1] = ~0ULL;

                    lo.m128 = _mm_shufflehi_epi16(U, _MM_SHUFFLE(1, 1, 0, 0));
                    lo.m128 = _mm_srli_si128(lo.m128, 8);
                    hi.m128 = _mm_shufflehi_epi16(U, _MM_SHUFFLE(3, 3, 2, 2));
                    hi.m128 = _mm_and_si128(hi.m128, mask.m128);
                    U = _mm_or_si128(lo.m128, hi.m128);

                    lo.m128 = _mm_shufflehi_epi16(V, _MM_SHUFFLE(1, 1, 0, 0));
                    lo.m128 = _mm_srli_si128(lo.m128, 8);
                    hi.m128 = _mm_shufflehi_epi16(V, _MM_SHUFFLE(3, 3, 2, 2));
                    hi.m128 = _mm_and_si128(hi.m128, mask.m128);
                    V = _mm_or_si128(lo.m128, hi.m128);
                }

                // Convert YUV to RGB

                //				Y = _mm_slli_pi16(Y, 1);
                //				U = _mm_slli_pi16(U, 1);
                //				V = _mm_slli_pi16(V, 1);

                temp = _mm_set1_epi16( y_offset);
                Y = _mm_subs_epi16(Y, temp);
                temp = _mm_set1_epi16(128);
                U = _mm_subs_epi16(U, temp);
                V = _mm_subs_epi16(V, temp);

                Y = _mm_slli_epi16(Y, 7);			// This code fix an overflow case where very bright
                temp = _mm_set1_epi16(ymult);		//pixel with some color produced interim values over 32768
                Y = _mm_mulhi_epi16(Y, temp);
                Y = _mm_slli_epi16(Y, 1);

                // Calculate R
                temp = _mm_set1_epi16(r_vmult);
                temp = _mm_mullo_epi16(V, temp);
                temp = _mm_srai_epi16(temp, 1); //7bit to 6
                R2 = _mm_adds_epi16(Y, temp);
                R2 = _mm_srai_epi16(R2, 6);

                // Calculate G
                temp = _mm_set1_epi16(g_vmult);
                temp = _mm_mullo_epi16(V, temp);
                temp = _mm_srai_epi16(temp, 2); //8bit to 6
                G2 = _mm_subs_epi16(Y, temp);
                temp = _mm_set1_epi16(g_umult);
                temp = _mm_mullo_epi16(U, temp);
                temp = _mm_srai_epi16(temp, 2); //8bit to 6
                G2 = _mm_subs_epi16(G2, temp);
                G2 = _mm_srai_epi16(G2, 6);

                // Calculate B
                temp = _mm_set1_epi16(b_umult);
                temp = _mm_mullo_epi16(U, temp);
                B2 = _mm_adds_epi16(Y, temp);
                B2 = _mm_srai_epi16(B2, 6);


                // Prepare to store the sixteen RGB values
                B_pi8 = _mm_packus_epi16(R1, R2);
                G_pi8 = _mm_packus_epi16(G1, G2);
                R_pi8 = _mm_packus_epi16(B1, B2);

                temp  = _mm_unpacklo_epi8(R_pi8, G_pi8);
                temp2 = _mm_unpacklo_epi8(B_pi8, _mm_set1_epi8(RGBA_DEFAULT_ALPHA));

                // Store the first four RGB values
                RGBA = _mm_unpacklo_epi16(temp, temp2);
                *RGBA_ptr++ = RGBA;

                // Store the second four RGB values
                RGBA = _mm_unpackhi_epi16(temp, temp2);
                *RGBA_ptr++ = RGBA;

                temp = _mm_unpackhi_epi8(R_pi8, G_pi8);
                temp2 = _mm_unpackhi_epi8(B_pi8, _mm_set1_epi8(RGBA_DEFAULT_ALPHA));

                // Store the third four RGB values
                RGBA = _mm_unpacklo_epi16(temp, temp2);
                *RGBA_ptr++ = RGBA;

                // Store the fourth four RGB values
                RGBA = _mm_unpackhi_epi16(temp, temp2);
                *RGBA_ptr++ = RGBA;

            }

            // Check that the loop ends at the right position
            assert(column == post_column);
#endif

            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B;
                int Y, U, V;
                uint8_t *RGBA_ptr = &RGBA_row[column * 4];

                // Convert the first set of YCbCr values
                if (saturate)
                {
                    Y = SATURATE_Y(Y_row[column]    >> Y_prescale);
                    V = SATURATE_Cr(U_row[column / 2] >> V_prescale);
                    U = SATURATE_Cb(V_row[column / 2] >> U_prescale);
                }
                else
                {
                    Y = (Y_row[column]   >> Y_prescale);
                    V = (U_row[column / 2] >> V_prescale);
                    U = (V_row[column / 2] >> U_prescale);
                }

                Y = Y - y_offset;
                U = U - 128;
                V = V - 128;

                Y = Y * ymult >> 7;

                R = (Y     + r_vmult * V) >> 7;
                G = (Y * 2  -  g_umult * U - g_vmult * V) >> 8;
                B = (Y + 2 * b_umult * U) >> 7;

                RGBA_ptr[0] = SATURATE_8U(B);
                RGBA_ptr[1] = SATURATE_8U(G);
                RGBA_ptr[2] = SATURATE_8U(R);
                RGBA_ptr[3] = RGBA_DEFAULT_ALPHA;

                // Convert the second set of YCbCr values
                if (saturate)
                    Y = SATURATE_Y(Y_row[column + 1] >> Y_prescale);
                else
                    Y = (Y_row[column + 1] >> Y_prescale);


                Y = Y - y_offset;
                Y = Y * ymult >> 7;

                R = (Y           + r_vmult * V) >> 7;
                G = (Y * 2 -  g_umult * U - g_vmult * V) >> 8;
                B = (Y + 2 * b_umult * U) >> 7;

                RGBA_ptr[4] = SATURATE_8U(B);
                RGBA_ptr[5] = SATURATE_8U(G);
                RGBA_ptr[6] = SATURATE_8U(R);
                RGBA_ptr[7] = RGBA_DEFAULT_ALPHA;
            }

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;

            // Advance the RGB pointers
            RGBA_row += output_pitch;
        }
    }
}

// Convert the input frame from YUV422 to RGB
void ConvertLowpass16sYUVtoRGB48(IMAGE *images[], uint8_t *output_buffer, int output_width,
                                 int output_height, int32_t output_pitch, int colorspace,
                                 bool inverted, int descale, int format, int whitebitdepth)
{
    PIXEL *plane[3];
    int pitch[3];
    ROI roi;
    int channel;

    int y_offset = 16; // not VIDEO_RGB & not YUV709
    int ymult = 128 * 149;	//7bit 1.164
    int r_vmult = 204;		//7bit 1.596
    int g_vmult = 208;		//8bit 0.813
    int g_umult = 100;		//8bit 0.391
    int b_umult = 129;		//6bit 2.018
    int saturate = 1;
    int mmx_y_offset = (y_offset << 7);
    int upconvert422to444 = 0;
    int dnshift = 0;

    if (whitebitdepth)
        dnshift = 16 - whitebitdepth;

    //colorspace |= COLOR_SPACE_422_TO_444; //DAN20090601
    output_pitch /= sizeof(PIXEL16U);

    if (colorspace & COLOR_SPACE_422_TO_444)
    {
        upconvert422to444 = 1;
    }

    switch (colorspace & COLORSPACE_MASK)
    {
        case COLOR_SPACE_CG_601:
            y_offset = 16;		// not VIDEO_RGB & not YUV709
            ymult = 128 * 149;	//7bit 1.164
            r_vmult = 204;		//7bit 1.596
            g_vmult = 208;		//8bit 0.813
            g_umult = 100;		//8bit 0.391
            b_umult = 129;		//6bit 2.018
            saturate = 1;
            break;

        default:
            assert(0);
        case COLOR_SPACE_CG_709:
            y_offset = 16;
            ymult = 128 * 149;	//7bit 1.164
            r_vmult = 230;		//7bit 1.793
            g_vmult = 137;		//8bit 0.534
            g_umult = 55;		//8bit 0.213
            b_umult = 135;		//6bit 2.115
            saturate = 1;
            break;

        case COLOR_SPACE_VS_601:
            y_offset = 0;
            ymult = 128 * 128;	//7bit 1.0
            r_vmult = 175;		//7bit 1.371
            g_vmult = 179;		//8bit 0.698
            g_umult = 86;		//8bit 0.336
            b_umult = 111;		//6bit 1.732
            saturate = 0;
            break;

        case COLOR_SPACE_VS_709:
            y_offset = 0;
            ymult = 128 * 128;	//7bit 1.0
            r_vmult = 197;		//7bit 1.540
            g_vmult = 118;		//8bit 0.459
            g_umult = 47;		//8bit 0.183
            b_umult = 116;		//6bit 1.816
            saturate = 0;
            break;
    }


    mmx_y_offset = (y_offset << 7);


#if (_USE_YCBCR == 0) || (_ENABLE_GAMMA_CORRECTION == 1)
    // Check that the correct compiler time switches are set correctly
    assert(0);
#endif

    // We use 16-bit fixed-point arithmetic to approximate the color conversion coefficients
    //
    // Floating point arithmetic is
    //
    // R = 1.164 * (Y - 16) + 1.596 * (V - 128);
    // G = 1.164 * (Y - 16) - 0.813 * (V - 128) - 0.392 * (U - 128);
    // B = 1.164 * (Y - 16) + 2.017 * (U - 128);
    //
    // Fixed point approximation (8-bit) is
    //
    // Y = (Y << 1) -  32;
    // U = (U << 1) - 256;
    // V = (V << 1) - 256;
    // R = (149 * Y + 204 * V) >> 8;
    // G = (149 * Y - 104 * V - 50 * U) >> 8;
    // B = (149 * Y + 258 * U) >> 8;
    //
    // Fixed point approximation (7-bit) is
    //
    // Y = (Y << 1) -  16;
    // U = (U << 1) - 256;
    // V = (V << 1) - 256;
    // R = (74 * Y + 102 * V) >> 7;
    // G = (74 * Y -  52 * V - 25 * U) >> 7;
    // B = (74 * Y + 129 * U) >> 7;
    //
    // We use 7-bit arithmetic

    // New 7 bit version to fix rounding errors 2/26/03
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = (149 * Y           + 204 * V) >> 7;
    // G = (149 * Y -  50 * U - 104 * V) >> 7;
    // B = (149 * Y + 258 * U) >> 7;
    //
    // New 6 bit version to fix rounding errors
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = ((149 * Y>>1)           + 102 * V) >> 6;
    // G = ((149 * Y>>1) -  25 * U - 52 * V) >> 6;
    // B = ((149 * Y>>1) + 129 * U) >> 6;



    // Bt.709
    // R = 1.164 * (Y - 16)                     + 1.793 * (V - 128);
    // G = 1.164 * (Y - 16) - 0.213 * (U - 128) - 0.534 * (V - 128);
    // B = 1.164 * (Y - 16) + 2.115 * (U - 128);
    //
    //
    // We use 7-bit arithmetic
    // Y = Y - 16;
    // U = U - 128;
    // V = V - 128;
    // R = (149 * Y           + 229 * V) >> 7;     // 229.5
    // G = (149 * Y -  27 * U - 68 * V) >> 7;		 //27.264  68.35
    // B = (149 * Y + 271 * U) >> 7;				 // 270.72
    //
    // New 6 bit version to fix rounding errors
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = ((149 * Y>>1)           + 115 * V - (V>>2)) >> 6;		//114.752
    // G = ((149 * Y>>1) -  ((109*U)>>3) - ((137*V)>>2) + (V>>2)) >> 6;			//13.632 approx 8 * 13.632 = 109,  137-0.25
    // B = ((149 * Y>>1) + 135 * U + (U>>2)) >> 6;
    //
    // New 6 bit version crude
    // Y = Y - 16 ;
    // U = U - 128;
    // V = V - 128;
    // R = ((149 * Y>>1)           + 115 * V ) >> 6;		//114.752
    // G = ((149 * Y>>1) -  14 * U -  34 * V ) >> 6;		//13.632 approx 8 * 13.632 = 109,  137-0.25
    // B = ((149 * Y>>1) + 135 * U           ) >> 6;


    // Convert from pixel to byte data
    for (channel = 0; channel < 3; channel++)
    {
        IMAGE *image = images[channel];

        plane[channel] = (PIXEL *)(image->band[0]);
        pitch[channel] = image->pitch / sizeof(PIXEL);

        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    {
        PIXEL16U *Y_row, *U_row, *V_row;
        int Y_pitch, U_pitch, V_pitch;
        int Y_prescale, U_prescale, V_prescale;
        PIXEL16U *RGBA_row;
        int row, column;
        //int column_step = 2;

        Y_row = (PIXEL16U *)plane[0];
        Y_pitch = pitch[0];
        Y_prescale = descale + PRESCALE_LUMA;
        U_row = (PIXEL16U *)plane[1];
        U_pitch = pitch[1];
        U_prescale = descale + PRESCALE_CHROMA;
        V_row = (PIXEL16U *)plane[2];
        V_pitch = pitch[2];
        V_prescale = descale + PRESCALE_CHROMA;

        RGBA_row = (PIXEL16U *)&output_buffer[0];
        if (inverted)
        {
            RGBA_row += (output_height - 1) * output_pitch;
            output_pitch = -output_pitch;
        }

        //TODO SSE2

        for (row = 0; row < output_height; row++)
        {
            unsigned short *RGB_ptr = &RGBA_row[0];
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (column = 0; column < roi.width; column += 2)
            {
                int R, G, B;
                int Y, U, V;

                // Convert the first set of YCbCr values
                if (saturate)
                {
                    Y = SATURATE_Y(Y_row[column]    << (8 - Y_prescale));
                    V = SATURATE_Cr(U_row[column / 2] << (8 - V_prescale));
                    U = SATURATE_Cb(V_row[column / 2] << (8 - U_prescale));
                }
                else
                {
                    Y = Y_row[column]   << (8 - Y_prescale);
                    V = U_row[column / 2] << (8 - V_prescale);
                    U = V_row[column / 2] << (8 - U_prescale);
                }

                Y = Y - (y_offset << 8);
                U = U - 32768;
                V = V - 32768;

                Y = Y * ymult >> 7;

                R = (Y     + r_vmult * V) >> 7;
                G = (Y * 2  -  g_umult * U - g_vmult * V) >> 8;
                B = (Y + 2 * b_umult * U) >> 7;

                if (dnshift)
                {
                    R >>= dnshift;
                    G >>= dnshift;
                    B >>= dnshift;
                }
                else
                {
                    R = SATURATE_16U(R);
                    G = SATURATE_16U(G);
                    B = SATURATE_16U(B);
                }

                switch (format)
                {
                    case DECODED_FORMAT_B64A: //b64a
                        *RGB_ptr++ = 0xffff;
                        *RGB_ptr++ = R;
                        *RGB_ptr++ = G;
                        *RGB_ptr++ = B;
                        break;
                    case DECODED_FORMAT_R210: //r210 byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            //B <<= 0;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_DPX0: //r210 byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 22;
                            G <<= 12;
                            B <<= 2;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_RG30: //rg30 A2B10G10R10
                    case DECODED_FORMAT_AB10:
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            B <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_AR10:
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_RG64: //RGBA64
                        *RGB_ptr++ = R;
                        *RGB_ptr++ = G;
                        *RGB_ptr++ = B;
                        *RGB_ptr++ = 0xffff;
                        break;
                    case DECODED_FORMAT_RG48: //RGB48
                        *RGB_ptr++ = R;
                        *RGB_ptr++ = G;
                        *RGB_ptr++ = B;
                        break;
                }

                // Convert the second set of YCbCr values
                if (saturate)
                    Y = SATURATE_Y(Y_row[column + 1] << (8 - U_prescale));
                else
                    Y = (Y_row[column + 1] << (8 - U_prescale));


                Y = Y - (y_offset << 8);
                Y = Y * ymult >> 7;

                R = (Y           + r_vmult * V) >> 7;
                G = (Y * 2 -  g_umult * U - g_vmult * V) >> 8;
                B = (Y + 2 * b_umult * U) >> 7;


                if (dnshift)
                {
                    R >>= dnshift;
                    G >>= dnshift;
                    B >>= dnshift;
                }
                else
                {
                    R = SATURATE_16U(R);
                    G = SATURATE_16U(G);
                    B = SATURATE_16U(B);
                }


                switch (format)
                {
                    case DECODED_FORMAT_B64A: //b64a
                        *RGB_ptr++ = 0xffff;
                        *RGB_ptr++ = R;
                        *RGB_ptr++ = G;
                        *RGB_ptr++ = B;
                        break;
                    case DECODED_FORMAT_R210: //r210 byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            //B <<= 0;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_DPX0: //r210 byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 22;
                            G <<= 12;
                            B <<= 2;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_RG30: //rg30 A2B10G10R10
                    case DECODED_FORMAT_AB10:
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            B <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_AR10:
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                            RGB_ptr += 2;
                        }
                        break;
                    case DECODED_FORMAT_RG64: //RGBA64
                        *RGB_ptr++ = R;
                        *RGB_ptr++ = G;
                        *RGB_ptr++ = B;
                        *RGB_ptr++ = 0xffff;
                        break;
                    case DECODED_FORMAT_RG48: //RGB48
                        *RGB_ptr++ = R;
                        *RGB_ptr++ = G;
                        *RGB_ptr++ = B;
                        break;
                }

            }

            // Advance the YUV pointers
            Y_row += Y_pitch;
            U_row += U_pitch;
            V_row += V_pitch;

            // Advance the RGB pointer
            RGBA_row += output_pitch;
        }
    }
}




// Convert the input frame from YUV422 to RGB
void ConvertLowpass16sRGB48ToRGB(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch, int format, int colorspace, bool inverted, int descale, int num_channels)
{
    PIXEL *plane[CODEC_MAX_CHANNELS];
    int pitch[CODEC_MAX_CHANNELS];
    ROI roi;
    int channel;
    int saturate = 1;

    // Only 24 and 32 bit true color RGB formats are supported
    assert(format == COLOR_FORMAT_RGB24 || format == COLOR_FORMAT_RGB32);


    //alpha_Companded likely doesn't set as the is not does to go through Convert4444LinesToOutput

    // Convert from pixel to byte data
    for (channel = 0; channel < num_channels; channel++)
    {
        IMAGE *image = images[channel];

        plane[channel] = (PIXEL *)(image->band[0]);
        pitch[channel] = image->pitch / sizeof(PIXEL);

        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    // Output to RGB24 format?
    if (format == COLOR_FORMAT_RGB24)
    {
        PIXEL *R_row, *G_row, *B_row;
        int R_pitch, G_pitch, B_pitch;
        int R_prescale, G_prescale, B_prescale;
        uint8_t *RGB_row;
        int row, column;

        G_row = plane[0];
        G_pitch = pitch[0];
        G_prescale = descale + PRESCALE_LUMA;
        R_row = plane[1];
        R_pitch = pitch[1];
        R_prescale = descale + PRESCALE_LUMA;
        B_row = plane[2];
        B_pitch = pitch[2];
        B_prescale = descale + PRESCALE_LUMA;

        RGB_row = &output_buffer[0];
        if (inverted && output_pitch > 0)
        {
            RGB_row += (output_height - 1) * output_pitch;
            output_pitch = -output_pitch;
        }

        for (row = 0; row < output_height; row++)
        {
            //int column_step = 16;
            //int post_column = roi.width - (roi.width % column_step);
            //__m64 *R_ptr = (__m64 *)R_row;
            //__m64 *G_ptr = (__m64 *)G_row;
            //__m64 *B_ptr = (__m64 *)B_row;
            //uint8_t *RGB_ptr = RGB_row;
            //int *RGB_int_ptr = (int *)RGB_ptr;
            //__m64 *output_ptr = (__m64 *)RGB_ptr;

            column = 0;
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B;
                uint8_t *RGB_ptr = &RGB_row[column * 3];

                // Convert the first set of YCbCr values
                R = (R_row[column] >> R_prescale);
                G = (G_row[column] >> G_prescale);
                B = (B_row[column] >> B_prescale);
                if (saturate)
                {
                    if (R < 0) R = 0;
                    if (R > 255) R = 255;
                    if (G < 0) G = 0;
                    if (G > 255) G = 255;
                    if (B < 0) B = 0;
                    if (B > 255) B = 255;
                }

                RGB_ptr[0] = B;
                RGB_ptr[1] = G;
                RGB_ptr[2] = R;
            }

            // Fill the rest of the output row with black
            for (; column < output_width; column++)
            {
                uint8_t *RGB_ptr = &RGB_row[column * 3];

                RGB_ptr[0] = 0;
                RGB_ptr[1] = 0;
                RGB_ptr[2] = 0;
            }

            // Advance the YUV pointers
            R_row += R_pitch;
            G_row += G_pitch;
            B_row += B_pitch;

            // Advance the RGB pointers
            RGB_row += output_pitch;
        }
    }

    else	// Output format is RGB32 so set the alpha channel to the default
    {
        PIXEL *R_row, *G_row, *B_row, *A_row;
        int R_pitch, G_pitch, B_pitch, A_pitch;
        int R_prescale, G_prescale, B_prescale, A_prescale;
        uint8_t *RGBA_row;
        int row, column;

        G_row = plane[0];
        G_pitch = pitch[0];
        G_prescale = descale + PRESCALE_LUMA;
        R_row = plane[1];
        R_pitch = pitch[1];
        R_prescale = descale + PRESCALE_LUMA;
        B_row = plane[2];
        B_pitch = pitch[2];
        B_prescale = descale + PRESCALE_LUMA;

        if (num_channels == 4)
        {
            A_row = plane[3];
            A_pitch = pitch[3];
            A_prescale = descale + PRESCALE_LUMA;
        }


        RGBA_row = &output_buffer[0];
        if (inverted)
        {
            RGBA_row += (output_height - 1) * output_pitch;
            output_pitch = -output_pitch;
        }

        for (row = 0; row < output_height; row++)
        {
            //int column_step = 16;
            //int post_column = roi.width - (roi.width % column_step);
            /*
            __m64 *R_ptr = (__m64 *)R_row;
            __m64 *G_ptr = (__m64 *)G_row;
            __m64 *B_ptr = (__m64 *)B_row;
            __m64 *A_ptr = (__m64 *)A_row;
            __m64 *RGBA_ptr = (__m64 *)RGBA_row;*/

            column = 0;
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B;
                uint8_t *RGBA_ptr = &RGBA_row[column * 4];

                // Convert the first set of YCbCr values
                R = (R_row[column] >> R_prescale);
                G = (G_row[column] >> G_prescale);
                B = (B_row[column] >> B_prescale);
                if (saturate)
                {
                    if (R < 0) R = 0;
                    if (R > 255) R = 255;
                    if (G < 0) G = 0;
                    if (G > 255) G = 255;
                    if (B < 0) B = 0;
                    if (B > 255) B = 255;
                }

                RGBA_ptr[0] = B;
                RGBA_ptr[1] = G;
                RGBA_ptr[2] = R;

                if (num_channels == 4)
                {
                    int A = A_row[column];

                    // Remove the alpha encoding curve.
                    //A -= 16<<A_prescale;
                    //A <<= 8;
                    //A += 111;
                    //A /= 223;
                    //12-bit SSE calibrated code
                    //a2_output_epi16 = _mm_subs_epu16(a2_output_epi16, _mm_set1_epi16(alphacompandDCoffset)); //12-bit  -16
                    //a2_output_epi16 = _mm_slli_epi16(a2_output_epi16, 3);  //15-bit
                    //a2_output_epi16 = _mm_mulhi_epi16(a2_output_epi16, _mm_set1_epi16(alphacompandGain));

                    A >>= A_prescale; // 8-bit
                    A <<= 4;// 12-bit
                    A -= alphacompandDCoffset;
                    A <<= 3; //15-bit
                    A *= alphacompandGain;
                    A >>= 16; //12-bit

                    A >>= A_prescale;

                    if (saturate)
                    {
                        if (A < 0) A = 0;
                        if (A > 255) A = 255;
                    }
                    RGBA_ptr[3] = A;
                }
                else
                    RGBA_ptr[3] = RGBA_DEFAULT_ALPHA;
            }

            // Advance the YUV pointers
            R_row += R_pitch;
            G_row += G_pitch;
            B_row += B_pitch;
            A_row += A_pitch;

            // Advance the RGB pointers
            //			R_row += output_pitch;
            //			G_row += output_pitch;
            //			B_row += output_pitch;
            RGBA_row += output_pitch;
        }
    }
}



// Convert the input frame from YUV422 to RGB
void ConvertLowpass16sRGB48ToRGB48(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch, int descale, int num_channels)
{
    PIXEL *plane[CODEC_MAX_CHANNELS];
    int pitch[CODEC_MAX_CHANNELS];
    ROI roi;
    int channel;
    //int saturate = 1;


    // Convert from pixel to byte data
    for (channel = 0; channel < num_channels; channel++)
    {
        IMAGE *image = images[channel];

        plane[channel] = (PIXEL *)(image->band[0]);
        pitch[channel] = image->pitch / sizeof(PIXEL);

        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    // Output to RGB48 format?
    {
        PIXEL *R_row, *G_row, *B_row;
        int R_pitch, G_pitch, B_pitch;
        int R_prescale, G_prescale, B_prescale;
        unsigned short *RGB_row;
        int row, column;

        G_row = plane[0];
        G_pitch = pitch[0];
        G_prescale = descale + PRESCALE_LUMA;
        R_row = plane[1];
        R_pitch = pitch[1];
        R_prescale = descale + PRESCALE_LUMA;
        B_row = plane[2];
        B_pitch = pitch[2];
        B_prescale = descale + PRESCALE_LUMA;

        RGB_row = (uint16_t *)&output_buffer[0];

        for (row = 0; row < output_height; row++)
        {
            column = 0;
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B;
                unsigned short *RGB_ptr = &RGB_row[column * 3];

                // Convert the first set of YCbCr values
                R = R_row[column];
                G = G_row[column];
                B = B_row[column];

                /*	R >>= R_prescale;
                	G >>= G_prescale;
                	B >>= B_prescale;
                	if(saturate)
                	{
                		if(R < 0) R=0; if(R > 255) R=255;
                		if(G < 0) G=0; if(G > 255) G=255;
                		if(B < 0) B=0; if(B > 255) B=255;
                	}*/

                RGB_ptr[0] = R << descale;
                RGB_ptr[1] = G << descale;
                RGB_ptr[2] = B << descale;
            }

            // Fill the rest of the output row with black
            for (; column < output_width; column++)
            {
                uint8_t *RGB_ptr = (uint8_t *)&RGB_row[column * 3];

                RGB_ptr[0] = 0;
                RGB_ptr[1] = 0;
                RGB_ptr[2] = 0;
            }

            // Advance the YUV pointers
            R_row += R_pitch;
            G_row += G_pitch;
            B_row += B_pitch;

            // Advance the RGB pointers
            RGB_row += output_pitch >> 1;
        }
    }
}


void ConvertLowpass16sBayerToRGB48(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch, int descale, int num_channels)
{
    PIXEL *plane[CODEC_MAX_CHANNELS];
    int pitch[CODEC_MAX_CHANNELS];
    ROI roi;
    int channel;
    //int saturate = 1;


    // Convert from pixel to byte data
    for (channel = 0; channel < num_channels; channel++)
    {
        IMAGE *image = images[channel];

        plane[channel] = (PIXEL *)(image->band[0]);
        pitch[channel] = image->pitch / sizeof(PIXEL);

        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    // Output to RGB48 format?
    {
        PIXEL *R_row, *G_row, *B_row;
        int R_pitch, G_pitch, B_pitch;
        int R_prescale, G_prescale, B_prescale;
        unsigned short *RGB_row;
        int row, column;

        G_row = plane[0];
        G_pitch = pitch[0];
        G_prescale = descale + PRESCALE_LUMA;
        R_row = plane[1];
        R_pitch = pitch[1];
        R_prescale = descale + PRESCALE_LUMA;
        B_row = plane[2];
        B_pitch = pitch[2];
        B_prescale = descale + PRESCALE_LUMA;

        RGB_row = (uint16_t *)&output_buffer[0];

        for (row = 0; row < output_height; row++)
        {
            unsigned short *RGB_ptr = &RGB_row[0];
            column = 0;
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B;

                // Convert the first set of YCbCr values
                R = R_row[column] << descale;
                G = G_row[column] << descale;
                B = B_row[column] << descale;

                R = G + (R * 2 - 65535); //*2); //DAN200080816 -- fixed grn bayer thumbnails
                B = G + (B * 2 - 65535); //*2);

                /*	R >>= R_prescale;
                	G >>= G_prescale;
                	B >>= B_prescale;
                	if(saturate)
                	{
                		if(R < 0) R=0; if(R > 255) R=255;
                		if(G < 0) G=0; if(G > 255) G=255;
                		if(B < 0) B=0; if(B > 255) B=255;
                	}*/


                if (R < 0) R = 0;
                if (R > 65535) R = 65535;
                if (G < 0) G = 0;
                if (G > 65535) G = 65535;
                if (B < 0) B = 0;
                if (B > 65535) B = 65535;

                *RGB_ptr++  = R;
                *RGB_ptr++  = G;
                *RGB_ptr++  = B;
                *RGB_ptr++  = R;
                *RGB_ptr++  = G;
                *RGB_ptr++  = B;
            }

            // Fill the rest of the output row with black
            for (; column < output_width; column++)
            {
                *RGB_ptr++ = 0;
                *RGB_ptr++ = 0;
                *RGB_ptr++ = 0;
                *RGB_ptr++ = 0;
                *RGB_ptr++ = 0;
                *RGB_ptr++ = 0;
            }

            // Advance the YUV pointers
            if (row & 1)
            {
                R_row += R_pitch;
                G_row += G_pitch;
                B_row += B_pitch;
            }

            // Advance the RGB pointers
            RGB_row += output_pitch >> 1;
        }
    }
}

// Convert the input frame from YUV422 to RGB
void ConvertLowpass16sRGBA64ToRGBA64(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch, int descale, int num_channels, int format)
{
    PIXEL *plane[CODEC_MAX_CHANNELS];
    int pitch[CODEC_MAX_CHANNELS];
    ROI roi;
    int channel;
    //int saturate = 1;

    // Convert from pixel to byte data
    for (channel = 0; channel < num_channels; channel++)
    {
        IMAGE *image = images[channel];

        plane[channel] = (PIXEL *)(image->band[0]);
        pitch[channel] = image->pitch / sizeof(PIXEL);

        if (channel == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    // Output to RGB48 format?
    if (num_channels == 3)
    {
        PIXEL *R_row, *G_row, *B_row;
        int R_pitch, G_pitch, B_pitch;
        int R_prescale, G_prescale, B_prescale;
        unsigned short *RGB_row;
        int row, column;

        G_row = plane[0];
        G_pitch = pitch[0];
        G_prescale = descale + PRESCALE_LUMA;
        R_row = plane[1];
        R_pitch = pitch[1];
        R_prescale = descale + PRESCALE_LUMA;
        B_row = plane[2];
        B_pitch = pitch[2];
        B_prescale = descale + PRESCALE_LUMA;

        RGB_row = (unsigned short *)&output_buffer[0];

        for (row = 0; row < output_height; row++)
        {
            //int column_step = 16;
            //int post_column = roi.width - (roi.width % column_step);
            //__m64 *R_ptr = (__m64 *)R_row;
            //__m64 *G_ptr = (__m64 *)G_row;
            //__m64 *B_ptr = (__m64 *)B_row;
            //uint8_t *RGB_ptr = (uint8_t *)RGB_row;
            //int *RGB_int_ptr = (int *)RGB_ptr;
            //__m64 *output_ptr = (__m64 *)RGB_ptr;

            column = 0;
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B;
                unsigned short *RGB_ptr = &RGB_row[column * 4];

                // Convert the first set of YCbCr values
                R = R_row[column] << R_prescale;
                G = G_row[column] << G_prescale;
                B = B_row[column] << B_prescale;

                switch (format)
                {
                    case DECODED_FORMAT_B64A:
                        RGB_ptr[0] = 0xffff;
                        RGB_ptr[1] = B;
                        RGB_ptr[2] = G;
                        RGB_ptr[3] = R;
                        break;

                    case DECODED_FORMAT_R210: //byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            //B <<= 0;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                        }
                        break;

                    case DECODED_FORMAT_DPX0: //byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 22;
                            G <<= 12;
                            B <<= 2;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                        }
                        break;
                    case DECODED_FORMAT_RG30: //rg30 A2B10G10R10
                    case DECODED_FORMAT_AB10:
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            B <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                        }
                        break;
                    case DECODED_FORMAT_AR10: //A2R10G10B10
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                        }
                        break;
                    case DECODED_FORMAT_RG64:
                        RGB_ptr[0] = R;
                        RGB_ptr[1] = G;
                        RGB_ptr[2] = B;
                        RGB_ptr[3] = 0xffff;
                        break;
                }
            }
            // Fill the rest of the output row with black
            for (; column < output_width; column++)
            {
                uint8_t *RGB_ptr = (uint8_t *)&RGB_row[column * 4];

                RGB_ptr[0] = 0;
                RGB_ptr[1] = 0;
                RGB_ptr[2] = 0;
                RGB_ptr[3] = 0;
            }

            // Advance the YUV pointers
            R_row += R_pitch;
            G_row += G_pitch;
            B_row += B_pitch;

            // Advance the RGB pointers
            RGB_row += output_pitch >> 1;
        }
    }
    else	// Output format is RGB32 so set the alpha channel to the default
    {
        PIXEL *R_row, *G_row, *B_row, *A_row;
        int R_pitch, G_pitch, B_pitch, A_pitch;
        int R_prescale, G_prescale, B_prescale, A_prescale;
        unsigned short *RGBA_row;
        int row, column;

        G_row = plane[0];
        G_pitch = pitch[0];
        G_prescale = descale + PRESCALE_LUMA;
        R_row = plane[1];
        R_pitch = pitch[1];
        R_prescale = descale + PRESCALE_LUMA;
        B_row = plane[2];
        B_pitch = pitch[2];
        B_prescale = descale + PRESCALE_LUMA;
        A_row = plane[3];
        A_pitch = pitch[3];
        A_prescale = descale + PRESCALE_LUMA;

        RGBA_row = (uint16_t *)&output_buffer[0];

        for (row = 0; row < output_height; row++)
        {
            //int column_step = 16;
            //int post_column = roi.width - (roi.width % column_step);
            /*
            __m64 *R_ptr = (__m64 *)R_row;
            __m64 *G_ptr = (__m64 *)G_row;
            __m64 *B_ptr = (__m64 *)B_row;
            __m64 *A_ptr = (__m64 *)A_row;
            __m64 *RGBA_ptr = (__m64 *)RGBA_row;*/

            column = 0;
            // Process the rest of the row with 7-bit fixed point arithmetic
            for (; column < roi.width; column ++)
            {
                int R, G, B, A;
                unsigned short *RGB_ptr = &RGBA_row[column * 4];

                // Convert the first set of YCbCr values
                R = R_row[column];
                G = G_row[column];
                B = B_row[column];
                A = A_row[column];

                {
                    int A = A_row[column];
                    A <<= 1;

                    // Remove the alpha encoding curve.
                    //A -= 16<<8;
                    //A <<= 8;
                    //A += 111;
                    //A /= 223;
                    //12-bit SSE calibrated code
                    //a2_output_epi16 = _mm_subs_epu16(a2_output_epi16, _mm_set1_epi16(alphacompandDCoffset)); //12-bit  -16
                    //a2_output_epi16 = _mm_slli_epi16(a2_output_epi16, 3);  //15-bit
                    //a2_output_epi16 = _mm_mulhi_epi16(a2_output_epi16, _mm_set1_epi16(alphacompandGain));

                    A >>= 4; //12-bit
                    A -= alphacompandDCoffset;
                    A <<= 3; //15-bit
                    A *= alphacompandGain;
                    A >>= 16; //12-bit
                    A <<= 4; //16-bit

                    if (A < 0) A = 0;
                    if (A > 0xffff) A = 0xffff;
                }

                switch (format)
                {
                    case DECODED_FORMAT_B64A:
                        RGB_ptr[0] = A;
                        RGB_ptr[1] = B;
                        RGB_ptr[2] = G;
                        RGB_ptr[3] = R;
                        break;
                    case DECODED_FORMAT_R210: //byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            //B <<= 0;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                        }
                        break;
                    case DECODED_FORMAT_DPX0: //byteswap(R10G10B10A2)
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 22;
                            G <<= 12;
                            B <<= 2;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = _bswap(rgb);
                        }
                        break;
                    case DECODED_FORMAT_RG30: //rg30 A2B10G10R10
                    case DECODED_FORMAT_AB10:
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            B <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                        }
                        break;
                    case DECODED_FORMAT_AR10: //A2R10G10B10
                        R >>= 6; // 10-bit
                        G >>= 6; // 10-bit
                        B >>= 6; // 10-bit
                        {
                            int rgb;
                            unsigned int *RGB = (unsigned int *)RGB_ptr;

                            R <<= 20;
                            G <<= 10;
                            rgb = R;
                            rgb |= G;
                            rgb |= B;

                            *RGB = rgb;
                        }
                        break;
                    case DECODED_FORMAT_RG64: //RGBA64
                        RGB_ptr[0] = R;
                        RGB_ptr[1] = G;
                        RGB_ptr[2] = B;
                        RGB_ptr[3] = A;
                        break;
                }
            }

            // Advance the YUV pointers
            R_row += R_pitch;
            G_row += G_pitch;
            B_row += B_pitch;
            A_row += A_pitch;

            // Advance the RGB pointers
            //			R_row += output_pitch;
            //			G_row += output_pitch;
            //			B_row += output_pitch;
            RGBA_row += output_pitch >> 1;
        }
    }
}


void ConvertLowpass16sToYUV(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch,
                            int format, bool inverted)
{
    IMAGE *y_image = images[0];
    IMAGE *u_image = images[1];
    IMAGE *v_image = images[2];
    int width = y_image->width;

    PIXEL *y_row_ptr = y_image->band[0];
    PIXEL *u_row_ptr = u_image->band[0];
    PIXEL *v_row_ptr = v_image->band[0];
    int y_pitch = y_image->pitch / sizeof(PIXEL);
    int u_pitch = u_image->pitch / sizeof(PIXEL);
    int v_pitch = v_image->pitch / sizeof(PIXEL);
    //int y_prescale = PRESCALE_LUMA;
    //int u_prescale = PRESCALE_CHROMA;
    //int v_prescale = PRESCALE_CHROMA;

    //size_t output_size = height * output_pitch;
    //size_t output_width = output_pitch / 2;
    uint8_t *outrow = output_buffer;
    uint8_t *outptr;
    int row, column;

    // The output pitch should be a positive number before inversion
    assert(output_pitch > 0);

    // Should the image be inverted?
    if (inverted)
    {
        outrow += (output_height - 1) * output_pitch;		// Start at the bottom row
        output_pitch = (- output_pitch);			// Negate the pitch to go up
    }

    if ((format & 0xffff) == COLOR_FORMAT_YUYV)
    {
        for (row = 0; row < output_height; row++)
        {
            column = 0;
            outptr = (uint8_t *)outrow;

            // Process the rest of the row
            for (; column < width; column++)
            {
                PIXEL value;

                // Copy the luminance byte to the output
                value = y_row_ptr[column] >> PRESCALE_LUMA;
                *(outptr++) = SATURATE_8U(value);

                // Copy the chroma to the output
                value = v_row_ptr[column / 2] >> PRESCALE_CHROMA;
                *(outptr++) = SATURATE_8U(value);

                // Copy the luminance to the output
                value = y_row_ptr[++column] >> PRESCALE_LUMA;
                *(outptr++) = SATURATE_8U(value);

                // Copy the chroma to the output
                value = u_row_ptr[column / 2] >> PRESCALE_CHROMA;
                *(outptr++) = SATURATE_8U(value);
            }

            // Should have exited the loop just after the last column
            assert(column == width);

#if 1
            // Check that the output width is valid
            assert(output_width >= width);

            // Fill the rest of the output row
            for (; column < output_width; column++)
            {
                // Set the luminance byte to black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;

                // Set the luminance to the black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;
            }
#endif
            // Advance to the next rows in the input and output images
            y_row_ptr += y_pitch;
            u_row_ptr += u_pitch;
            v_row_ptr += v_pitch;
            outrow += output_pitch;
        }
    }
    else if ((format & 0xffff) == COLOR_FORMAT_UYVY)
    {
        for (row = 0; row < output_height; row++)
        {
            column = 0;
            outptr = (uint8_t *)outrow;

            // Process the rest of the row
            for (; column < width; column++)
            {
                PIXEL value;

                // Copy the chroma to the output
                value = v_row_ptr[column / 2] >> PRESCALE_CHROMA;
                *(outptr++) = SATURATE_8U(value);

                // Copy the luminance byte to the output
                value = y_row_ptr[column] >> PRESCALE_LUMA;
                *(outptr++) = SATURATE_8U(value);

                // Copy the chroma to the output
                value = u_row_ptr[column / 2] >> PRESCALE_CHROMA;
                *(outptr++) = SATURATE_8U(value);

                // Copy the luminance to the output
                value = y_row_ptr[++column] >> PRESCALE_LUMA;
                *(outptr++) = SATURATE_8U(value);
            }

            // Should have exited the loop just after the last column
            assert(column == width);

            // Check that the output width is valid
            assert(output_width >= width);

            // Fill the rest of the output row
            for (; column < output_width; column++)
            {
                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;

                // Set the luminance byte to black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;

                // Set the luminance to the black
                *(outptr++) = COLOR_LUMA_BLACK;
            }

            // Advance to the next rows in the input and output images
            y_row_ptr += y_pitch;
            u_row_ptr += u_pitch;
            v_row_ptr += v_pitch;
            outrow += output_pitch;
        }
    }
    else assert(0);		// Only support YUYV and UYVY formats
}


//TODO DAN04262004 make the routine XMM
void ConvertLowpass16sToYUV64(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch,
                              int format, bool inverted, int precision)
{
    IMAGE *y_image = images[0];
    IMAGE *u_image = images[1];
    IMAGE *v_image = images[2];
    int width = y_image->width;
    int height = output_height;

    PIXEL *y_row_ptr = y_image->band[0];
    PIXEL *u_row_ptr = u_image->band[0];
    PIXEL *v_row_ptr = v_image->band[0];
    int y_pitch = y_image->pitch / sizeof(PIXEL);
    int u_pitch = u_image->pitch / sizeof(PIXEL);
    int v_pitch = v_image->pitch / sizeof(PIXEL);
    //int y_prescale = PRESCALE_LUMA;
    //int u_prescale = PRESCALE_CHROMA;
    //int v_prescale = PRESCALE_CHROMA;

    //size_t output_size = height * output_pitch;
    //size_t output_width = output_pitch / 2;
    PIXEL *outrow = (PIXEL *)output_buffer;
    PIXEL *outptr;
    int row, column;

    // Definitions for optimization
    //const int column_step = 2 * sizeof(__m64);
    __m64 *yuvptr;

    // Column at which post processing must begin
    //int post_column = width - (width % column_step);

    // The output pitch should be a positive number before inversion
    assert(output_pitch > 0);

    // Should the image be inverted?
    if (inverted)
    {
        outrow += (height - 1) * output_pitch;		// Start at the bottom row
        output_pitch = (- output_pitch);			// Negate the pitch to go up
    }

    if (format == COLOR_FORMAT_YU64)
    {
        for (row = 0; row < height; row++)
        {
            yuvptr = (__m64 *)outrow;
            column = 0;

#if (0 && XMMOPT)

            for (; column < post_column; column += column_step)
            {
                __m64 first_pi16;	// First four signed shorts of color components
                __m64 second_pi16;	// Second four signed shorts of color components

                __m64 yyyy;		// Eight unsigned bytes of color components
                __m64 uuuu;
                __m64 vvvv;
                __m64 uvuv;
                __m64 yuyv;		// Interleaved bytes of luma and chroma

                __m64 mask;		// Mask for zero chroma values

                // Adjust the column for YUV 4:2:2 frame format
                int chroma_column = column / 2;

                // Load eight signed shorts of luma
                first_pi16  = *((__m64 *)&y_row_ptr[column]);
                second_pi16 = *((__m64 *)&y_row_ptr[column + 4]);

                // Convert to eight unsigned bytes of luma
                first_pi16  = _mm_srai_pi16(first_pi16,  PRESCALE_LUMA);
                second_pi16 = _mm_srai_pi16(second_pi16, PRESCALE_LUMA);
                yyyy = _mm_packs_pu16(first_pi16, second_pi16);

#if (0 && STRICT_SATURATE)
                // Perform strict saturation on luma if required
                yyyy = _mm_subs_pu8(yyyy, _mm_set1_pi8(16));
                yyyy = _mm_adds_pu8(yyyy, _mm_set1_pi8(36));	// 36 = 16 + 20 = 16 + (255-235)
                yyyy = _mm_subs_pu8(yyyy, _mm_set1_pi8(20));
#endif
                // Load eight signed shorts of chroma
                first_pi16  = *((__m64 *)&u_row_ptr[chroma_column]);
                second_pi16 = *((__m64 *)&u_row_ptr[chroma_column + 4]);

                // Convert to eight unsigned bytes of chroma
                first_pi16  = _mm_srai_pi16(first_pi16,  PRESCALE_CHROMA);
                second_pi16 = _mm_srai_pi16(second_pi16, PRESCALE_CHROMA);
                uuuu = _mm_packs_pu16(first_pi16, second_pi16);

#if (0 && STRICT_SATURATE)
                // Perform strict saturation on chroma if required
                uuuu = _mm_subs_pu8(uuuu, _mm_set1_pi8(16));
                uuuu = _mm_adds_pu8(uuuu, _mm_set1_pi8(31));	// 31 = 16 + 15 = 16 + (255-240)
                uuuu = _mm_subs_pu8(uuuu, _mm_set1_pi8(15));
#endif
                // Load eight signed shorts of luma
                first_pi16  = *((__m64 *)&v_row_ptr[chroma_column]);
                second_pi16 = *((__m64 *)&v_row_ptr[chroma_column + 4]);

                // Convert to eight unsigned bytes of chroma
                first_pi16  = _mm_srai_pi16(first_pi16,  PRESCALE_CHROMA);
                second_pi16 = _mm_srai_pi16(second_pi16, PRESCALE_CHROMA);
                vvvv = _mm_packs_pu16(first_pi16, second_pi16);

#if (0 && STRICT_SATURATE)
                // Perform strict saturation on chroma if required
                vvvv = _mm_subs_pu8(vvvv, _mm_set1_pi8(16));
                vvvv = _mm_adds_pu8(vvvv, _mm_set1_pi8(31));	// 31 = 16 + 15 = 16 + (255-240)
                vvvv = _mm_subs_pu8(vvvv, _mm_set1_pi8(15));
#endif
                // Pack eight bytes of luma with alternating bytes of chroma

                uvuv = _mm_unpacklo_pi8(vvvv, uuuu);	// Interleave first four chroma
                yuyv = _mm_unpacklo_pi8(yyyy, uvuv);	// Interleave the luma and chroma
                _mm_stream_pi(yuvptr++, yuyv);			// Store the first four yuyv pairs

                yuyv = _mm_unpackhi_pi8(yyyy, uvuv);	// Interleave the luma and chroma
                _mm_stream_pi(yuvptr++, yuyv);			// Store the second four yuyv pairs

                // Interleave eight more luma values with the remaining chroma

                // Load the next eight signed shorts of luma
                first_pi16  = *((__m64 *)&y_row_ptr[column + 8]);
                second_pi16 = *((__m64 *)&y_row_ptr[column + 12]);

                // Convert to eight unsigned bytes of luma
                first_pi16  = _mm_srai_pi16(first_pi16,  PRESCALE_LUMA);
                second_pi16 = _mm_srai_pi16(second_pi16, PRESCALE_LUMA);
                yyyy = _mm_packs_pu16(first_pi16, second_pi16);

#if (0 && STRICT_SATURATE)
                // Perform strict saturation on luma if required
                yyyy = _mm_subs_pu8(yyyy, _mm_set1_pi8(16));
                yyyy = _mm_adds_pu8(yyyy, _mm_set1_pi8(36));	// 36 = 16 + 20 = 16 + (255-235)
                yyyy = _mm_subs_pu8(yyyy, _mm_set1_pi8(20));
#endif
                uvuv = _mm_unpackhi_pi8(vvvv, uuuu);	// Interleave second four chroma
                yuyv = _mm_unpacklo_pi8(yyyy, uvuv);	// Interleave the luma and chroma
                _mm_stream_pi(yuvptr++, yuyv);			// Store the third four yuyv pairs

                yuyv = _mm_unpackhi_pi8(yyyy, uvuv);	// Interleave the luma and chroma
                _mm_stream_pi(yuvptr++, yuyv);			// Store the fourth four yuyv pairs

                // Done interleaving eight bytes of each chroma channel with sixteen luma
            }

            //_mm_empty();

            // Should have exited the loop at the post processing column
            assert(column == post_column);
#endif
#if 1
            // Get the byte pointer to the rest of the row
            outptr = (PIXEL *)yuvptr;


            if (precision == 13) // weird mode
            {
                //int maxval = 32767;

                // Process the rest of the row
                for (; column < width; column++)
                {
                    PIXEL value;

                    // Copy the luminance byte to the output
                    value = y_row_ptr[column];
                    //	if(value < 0) value = 0;
                    //	if(value > maxval) value = maxval;
                    //	value <<= 16-precision;
                    *(outptr++) = value << 1; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = u_row_ptr[column / 2];
                    //	if(value < 0) value = 0;
                    //	if(value > maxval) value = maxval;
                    //	value <<= 16-precision;
                    *(outptr++) = value << 1; //SATURATE_Cr(value);	//SATURATE_8U(value);

                    // Copy the luminance to the output
                    value = y_row_ptr[++column];
                    //	if(value < 0) value = 0;
                    //	if(value > maxval) value = maxval;
                    //	value <<= 16-precision;
                    *(outptr++) = value << 1; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = v_row_ptr[column / 2];
                    //	if(value < 0) value = 0;
                    //	if(value > maxval) value = maxval;
                    //	value <<= 16-precision;
                    *(outptr++) = value << 1; //SATURATE_Cb(value);	//SATURATE_8U(value);
                }
            }
            else if (precision == CODEC_PRECISION_12BIT)
            {
                // Process the rest of the row
                for (; column < width; column++)
                {
                    PIXEL value;

                    // Copy the luminance byte to the output
                    value = y_row_ptr[column];
                    if (value < 0) value = 0;
                    if (value > 16383) value = 16383;
                    value <<= 2;
                    *(outptr++) = value; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = u_row_ptr[column / 2];
                    if (value < 0) value = 0;
                    if (value > 16383) value = 16383;
                    value <<= 2;
                    *(outptr++) = value; //SATURATE_Cr(value);	//SATURATE_8U(value);

                    // Copy the luminance to the output
                    value = y_row_ptr[++column];
                    if (value < 0) value = 0;
                    if (value > 16383) value = 16383;
                    value <<= 2;
                    *(outptr++) = value; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = v_row_ptr[column / 2];
                    if (value < 0) value = 0;
                    if (value > 16383) value = 16383;
                    value <<= 2;
                    *(outptr++) = value; //SATURATE_Cb(value);	//SATURATE_8U(value);
                }
            }
            else if (precision == CODEC_PRECISION_10BIT)
            {
                // Process the rest of the row
                for (; column < width; column++)
                {
                    PIXEL value;

                    // Copy the luminance byte to the output
                    value = y_row_ptr[column];
                    if (value < 0) value = 0;
                    if (value > 4095) value = 4095;
                    value <<= 4;
                    *(outptr++) = value; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = u_row_ptr[column / 2];
                    if (value < 0) value = 0;
                    if (value > 4095) value = 4095;
                    value <<= 4;
                    *(outptr++) = value; //SATURATE_Cr(value);	//SATURATE_8U(value);

                    // Copy the luminance to the output
                    value = y_row_ptr[++column];
                    if (value < 0) value = 0;
                    if (value > 4095) value = 4095;
                    value <<= 4;
                    *(outptr++) = value; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = v_row_ptr[column / 2];
                    if (value < 0) value = 0;
                    if (value > 4095) value = 4095;
                    value <<= 4;
                    *(outptr++) = value; //SATURATE_Cb(value);	//SATURATE_8U(value);
                }
            }
            else
            {
                // Process the rest of the row
                for (; column < width; column++)
                {
                    PIXEL value;

                    // Copy the luminance byte to the output
                    value = y_row_ptr[column];
                    if (value < 0) value = 0;
                    if (value > 1023) value = 1023;
                    value <<= 6;
                    *(outptr++) = value; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = u_row_ptr[column / 2];
                    if (value < 0) value = 0;
                    if (value > 1023) value = 1023;
                    value <<= 6;
                    *(outptr++) = value; //SATURATE_Cr(value);	//SATURATE_8U(value);

                    // Copy the luminance to the output
                    value = y_row_ptr[++column];
                    if (value < 0) value = 0;
                    if (value > 1023) value = 1023;
                    value <<= 6;
                    *(outptr++) = value; //SATURATE_Y(value);	//SATURATE_8U(value);

                    // Copy the chroma to the output
                    value = v_row_ptr[column / 2];
                    if (value < 0) value = 0;
                    if (value > 1023) value = 1023;
                    value <<= 6;
                    *(outptr++) = value; //SATURATE_Cb(value);	//SATURATE_8U(value);
                }
            }


            // Should have exited the loop just after the last column
            assert(column == width);
#endif
#if 1
            // Check that the output width is valid
            assert(output_width >= width);

            // Fill the rest of the output row
            for (; column < output_width; column++)
            {
                // Set the luminance byte to black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO << 8;

                // Set the luminance to the black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO << 8;
            }
#endif
            // Advance to the next rows in the input and output images
            y_row_ptr += y_pitch;
            u_row_ptr += u_pitch;
            v_row_ptr += v_pitch;
            outrow += output_pitch / 2;
        }
    }
    else assert(0);		// Only support YUYV and UYVY formats
}


//#if BUILD_PROSPECT
// Convert the lowpass band to rows of unpacked 16-bit YUV
void ConvertLowpass16sToYR16(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height, int32_t output_pitch,
                             int format, bool inverted, int precision)
{
    IMAGE *y_image = images[0];
    IMAGE *u_image = images[1];
    IMAGE *v_image = images[2];

    int width = y_image->width;
    int height = output_height;

    PIXEL *y_input_ptr = y_image->band[0];
    PIXEL *u_input_ptr = u_image->band[0];
    PIXEL *v_input_ptr = v_image->band[0];
    int y_pitch = y_image->pitch / sizeof(PIXEL);
    int u_pitch = u_image->pitch / sizeof(PIXEL);
    int v_pitch = v_image->pitch / sizeof(PIXEL);
    //int y_prescale = PRESCALE_LUMA;
    //int u_prescale = PRESCALE_CHROMA;
    //int v_prescale = PRESCALE_CHROMA;

    //size_t output_size = height * output_pitch;
    //size_t output_width = output_pitch / 2;
    //PIXEL *outrow = (PIXEL *)output_buffer;
    //PIXEL *outptr;

    // Each output row starts with a row of luma
    uint8_t *output_row_ptr = output_buffer;

    int row;

    // Process eight columns of luma per iteration of the fast loop
    //const int column_step = 8;

    // Column at which post processing must begin
    //int post_column = width - (width % column_step);

    // The output pitch should be a positive number before inversion
    assert(output_pitch > 0);

    // Should the image be inverted?
    if (inverted)
    {
        output_row_ptr += (height - 1) * output_pitch;	// Start at the bottom row
        output_pitch = NEG(output_pitch);				// Negate the pitch to go up
    }

    if (format == COLOR_FORMAT_YR16)
    {
        for (row = 0; row < height; row++)
        {
            PIXEL *y_output_ptr = (PIXEL *)output_row_ptr;
            PIXEL *u_output_ptr = y_output_ptr + output_width;
            PIXEL *v_output_ptr = u_output_ptr + output_width / 2;

            int column = 0;

            if (precision == CODEC_PRECISION_10BIT)
            {
                // Process the rest of the row
                for (; column < width; column += 2)
                {
                    PIXEL value;

                    // Copy the scaled luma to the output
                    value = y_input_ptr[column];
                    value = SATURATE_12U(value);
                    value <<= 4;
                    *(y_output_ptr++) = value;

                    // Copy the scaled u chroma to the output
                    value = u_input_ptr[column / 2];
                    value = SATURATE_12U(value);
                    value <<= 4;
                    *(u_output_ptr++) = value;

                    // Copy the scaled luma to the output
                    value = y_input_ptr[column + 1];
                    value = SATURATE_12U(value);
                    value <<= 4;
                    *(y_output_ptr++) = value;

                    // Copy the scaled v chroma to the output
                    value = v_input_ptr[column / 2];
                    value = SATURATE_12U(value);
                    value <<= 4;
                    *(v_output_ptr++) = value;
                }
            }
            else
            {
                assert(precision == CODEC_PRECISION_8BIT);

                // Process the rest of the row
                for (; column < width; column += 2)
                {
                    PIXEL value;

                    // Copy the scaled luma to the output
                    value = y_input_ptr[column];
                    value = SATURATE_10U(value);
                    value <<= 6;
                    *(y_output_ptr++) = value;

                    // Copy the scaled u chroma to the output
                    value = u_input_ptr[column / 2];
                    value = SATURATE_10U(value);
                    value <<= 6;
                    *(u_output_ptr++) = value;

                    // Copy the scaled luma to the output
                    value = y_input_ptr[column + 1];
                    value = SATURATE_10U(value);
                    value <<= 6;
                    *(y_output_ptr++) = value;

                    // Copy the scaled v chroma to the output
                    value = v_input_ptr[column / 2];
                    value = SATURATE_10U(value);
                    value <<= 6;
                    *(v_output_ptr++) = value;
                }
            }


            // Should have exited the loop just after the last column
            assert(column == width);

            // Check that the output width is valid
            assert(output_width >= width);

            // Fill the rest of the output row
            for (; column < output_width; column++)
            {
                const int luma_value = COLOR_LUMA_BLACK;
                const int chroma_value = (COLOR_CHROMA_ZERO << 8);

                // Set the luminance byte to black
                *(y_output_ptr++) = luma_value;

                // Zero the chroma byte
                *(u_output_ptr++) = chroma_value;

                // Set the luminance to the black
                *(y_output_ptr++) = luma_value;

                // Zero the chroma byte
                *(v_output_ptr++) = chroma_value;
            }

            // Advance to the next rows in the input and output images
            y_input_ptr += y_pitch;
            u_input_ptr += u_pitch;
            v_input_ptr += v_pitch;

            output_row_ptr += output_pitch;
        }
    }
    else
    {
        assert(0);		// Only support YR16 format
    }
}
//#endif


#if 0
#ifdef PRESCALE_LUMA10
#undef PRESCALE_LUMA10
#endif

#define PRESCALE_LUMA10		4

#ifdef PRESCALE_CHROMA10
#undef PRESCALE_CHROMA10
#endif

#define PRESCALE_CHROMA10	5
#endif

void ConvertLowpass16s10bitToYUV(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height,
                                 int32_t output_pitch, int format, bool inverted, int lineskip)
{
    IMAGE *y_image = images[0];
    IMAGE *u_image = images[1];
    IMAGE *v_image = images[2];
    int width = y_image->width;
    int height = output_height;

    PIXEL *y_row_ptr = y_image->band[0];
    PIXEL *u_row_ptr = u_image->band[0];
    PIXEL *v_row_ptr = v_image->band[0];
    int y_pitch = y_image->pitch / sizeof(PIXEL);
    int u_pitch = u_image->pitch / sizeof(PIXEL);
    int v_pitch = v_image->pitch / sizeof(PIXEL);
    //int y_prescale = PRESCALE_LUMA;
    //int u_prescale = PRESCALE_CHROMA;
    //int v_prescale = PRESCALE_CHROMA;

    //size_t output_size = height * output_pitch;
    //size_t output_width = output_pitch / 2;
    uint8_t *outrow = output_buffer;
    uint8_t *outptr;
    int row, column;

    // Column at which post processing must begin
    //int post_column = width - (width % column_step);

    // The output pitch should be a positive number before inversion
    assert(output_pitch > 0);

    // Should the image be inverted?
    if (inverted)
    {
        outrow += (height - 1) * output_pitch;		// Start at the bottom row
        output_pitch = NEG(output_pitch);			// Negate the pitch to go up
    }

    if ((format & 0xffff) == COLOR_FORMAT_YUYV)
    {
        for (row = 0; row < height; row += lineskip)
        {
            column = 0;
            outptr = outrow;

            // Process the rest of the row
            for (; column < width; column++)
            {
                PIXEL value;

                // Copy the luminance byte to the output
                value = y_row_ptr[column] >> PRESCALE_LUMA10;
                *(outptr++) = SATURATE_8U(value);

                // Copy the chroma to the output
                value = v_row_ptr[column / 2] >> PRESCALE_CHROMA10;
                *(outptr++) = SATURATE_8U(value);

                // Copy the luminance to the output
                value = y_row_ptr[++column] >> PRESCALE_LUMA10;
                *(outptr++) = SATURATE_8U(value);

                // Copy the chroma to the output
                value = u_row_ptr[column / 2] >> PRESCALE_CHROMA10;
                *(outptr++) = SATURATE_8U(value);
            }

            // Should have exited the loop just after the last column
            assert(column == width);

            // Check that the output width is valid
            assert(output_width >= width);

            // Fill the rest of the output row
            for (; column < output_width; column++)
            {
                // Set the luminance byte to black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;

                // Set the luminance to the black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;
            }

            // Advance to the next rows in the input and output images
            y_row_ptr += y_pitch * lineskip; // 3D Work
            u_row_ptr += u_pitch * lineskip;
            v_row_ptr += v_pitch * lineskip;

            outrow += output_pitch;
        }
    }
    else if ((format & 0xffff) == COLOR_FORMAT_UYVY)
    {
        for (row = 0; row < height; row += lineskip)
        {
            column = 0;
            outptr = outrow;

            // Process the rest of the row
            for (; column < width; column++)
            {
                PIXEL value;

                // Copy the chroma to the output
                value = v_row_ptr[column / 2] >> PRESCALE_CHROMA10;
                *(outptr++) = SATURATE_8U(value);

                // Copy the luminance byte to the output
                value = y_row_ptr[column] >> PRESCALE_LUMA10;
                *(outptr++) = SATURATE_8U(value);

                // Copy the chroma to the output
                value = u_row_ptr[column / 2] >> PRESCALE_CHROMA10;
                *(outptr++) = SATURATE_8U(value);

                // Copy the luminance to the output
                value = y_row_ptr[++column] >> PRESCALE_LUMA10;
                *(outptr++) = SATURATE_8U(value);
            }

            // Should have exited the loop just after the last column
            assert(column == width);

            // Check that the output width is valid
            assert(output_width >= width);

            // Fill the rest of the output row
            for (; column < output_width; column++)
            {
                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;

                // Set the luminance byte to black
                *(outptr++) = COLOR_LUMA_BLACK;

                // Zero the chroma byte
                *(outptr++) = COLOR_CHROMA_ZERO;

                // Set the luminance to the black
                *(outptr++) = COLOR_LUMA_BLACK;
            }

            // Advance to the next rows in the input and output images
            y_row_ptr += y_pitch * lineskip; // 3D Work
            u_row_ptr += u_pitch * lineskip;
            v_row_ptr += v_pitch * lineskip;
            outrow += output_pitch;
        }
    }
    else assert(0);		// Only support YUYV and UYVY formats
}


void ConvertLowpass16s10bitToV210(IMAGE *images[], uint8_t *output_buffer, int output_width, int output_height,
                                  int32_t output_pitch, int format, bool inverted)
{
    // Note: This routine swaps the chroma values
    IMAGE *y_image = images[0];
    IMAGE *u_image = images[2];
    IMAGE *v_image = images[1];

    int width = y_image->width;
    int height = output_height;

    PIXEL *y_row_ptr = y_image->band[0];
    PIXEL *u_row_ptr = u_image->band[0];
    PIXEL *v_row_ptr = v_image->band[0];
    int y_pitch = y_image->pitch / sizeof(PIXEL);
    int u_pitch = u_image->pitch / sizeof(PIXEL);
    int v_pitch = v_image->pitch / sizeof(PIXEL);

    uint32_t *outrow = (uint32_t *)output_buffer;

    const int v210_column_step = 6;

#if (0 && XMMOPT)
    // Process four bytes each of luma and chroma per loop iteration
    const int column_step = 16;

    // Column at which post processing must begin
    int post_column = width - (width % column_step);
#endif

    int row;

    // The output pitch should be a positive number before inversion
    assert(output_pitch > 0);
    output_pitch /= sizeof(uint32_t);

#if 1
    // This routine does not handle inversion
    assert(!inverted);
#else
    // Should the image be inverted?
    if (inverted)
    {
        outrow += (height - 1) * output_pitch;		// Start at the bottom row
        output_pitch = (- output_pitch);			// Negate the pitch to go up
    }
#endif

    // Adjust the width to a multiple of the number of pixels packed into four words
    width -= (width % v210_column_step);

    if (format == COLOR_FORMAT_V210)
    {
        for (row = 0; row < height; row++)
        {
            int column = 0;
            int output_column = 0;

            // Process the rest of the row
            for (; column < width; column += v210_column_step)
            {
                int y1, y2;
                int u;
                int v;
                uint32_t yuv;

                // Get the first u chroma value
                u = (u_row_ptr[column / 2] >> PRESCALE_CHROMA);
                if (u < 0) u = 0;
                if (u > 1023) u = 1023;

                // Get the first luma value
                y1 = (y_row_ptr[column] >> PRESCALE_LUMA);
                if (y1 < 0) y1 = 0;
                if (y1 > 1023) y1 = 1023;

                // Get the first v chroma value
                v = (v_row_ptr[column / 2] >> PRESCALE_CHROMA);
                if (v < 0) v = 0;
                if (v > 1023) v = 1023;

                // Assemble and store the first packed word
                yuv = (v << V210_VALUE3_SHIFT) | (y1 << V210_VALUE2_SHIFT) | (u << V210_VALUE1_SHIFT);
                outrow[output_column++] = yuv;


                // Get the second luma value
                y1 = (y_row_ptr[column + 1] >> PRESCALE_LUMA);
                if (y1 < 0) y1 = 0;
                if (y1 > 1023) y1 = 1023;

                // Get the second u chroma value
                u = (u_row_ptr[column / 2 + 1] >> PRESCALE_CHROMA);
                if (u < 0) u = 0;
                if (u > 1023) u = 1023;

                // Get the third luma value
                y2 = (y_row_ptr[column + 2] >> PRESCALE_LUMA);
                if (y2 < 0) y2 = 0;
                if (y2 > 1023) y2 = 1023;

                // Assemble and store the second packed word
                yuv = (y2 << V210_VALUE3_SHIFT) | (u << V210_VALUE2_SHIFT) | (y1 << V210_VALUE1_SHIFT);
                outrow[output_column++] = yuv;


                // Get the second v chroma value
                v = (v_row_ptr[column / 2 + 1] >> PRESCALE_CHROMA);
                if (v < 0) v = 0;
                if (v > 1023) v = 1023;

                // Get the fourth luma value
                y1 = (y_row_ptr[column + 3] >> PRESCALE_LUMA);
                if (y1 < 0) y1 = 0;
                if (y1 > 1023) y1 = 1023;

                // Get the third u chroma value
                u = (u_row_ptr[column / 2 + 2] >> PRESCALE_CHROMA);
                if (u < 0) u = 0;
                if (u > 1023) u = 1023;

                // Assemble and store the third packed word
                yuv = (u << V210_VALUE3_SHIFT) | (y1 << V210_VALUE2_SHIFT) | (v << V210_VALUE1_SHIFT);
                outrow[output_column++] = yuv;


                // Get the fifth luma value
                y1 = (y_row_ptr[column + 4] >> PRESCALE_LUMA);
                if (y1 < 0) y1 = 0;
                if (y1 > 1023) y1 = 1023;
                // Get the third v chroma value
                v = (v_row_ptr[column / 2 + 2] >> PRESCALE_CHROMA);
                if (v < 0) v = 0;
                if (v > 1023) v = 1023;

                // Get the sixth luma value
                y2 = (y_row_ptr[column + 5] >> PRESCALE_LUMA);
                if (y2 < 0) y2 = 0;
                if (y2 > 1023) y2 = 1023;

                // Assemble and store the fourth packed word
                yuv = (y2 << V210_VALUE3_SHIFT) | (v << V210_VALUE2_SHIFT) | (y1 << V210_VALUE1_SHIFT);
                outrow[output_column++] = yuv;
            }

            // Should have exited the loop just after the last column
            assert(column == width);

            // Advance to the next rows in the input and output images
            y_row_ptr += y_pitch;
            u_row_ptr += u_pitch;
            v_row_ptr += v_pitch;
            outrow += output_pitch;
        }

        ////_mm_empty();		// Clear the mmx register state
    }
    else assert(0);		// Only support V210 format
}

/*!
	@brief Convert the Avid 2.8 packed format to planes of 10-bit unsigned pixels
*/
void ConvertCbYCrY_10bit_2_8ToFrame16s(void *data, int pitch, FRAME *frame, void *buffer, int precision, int alpha)
{
    uint8_t *upper_plane;
    uint8_t *lower_plane;
    uint8_t *upper_row_ptr;
    uint8_t *lower_row_ptr;
    int upper_row_pitch;
    int lower_row_pitch;
    PIXEL16U *plane_array[3];
    int plane_pitch[3];
    ROI roi;
    int row, column;
    int i;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);
    //display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        plane_array[i] = (PIXEL16U *)image->band[0];
        plane_pitch[i] = image->pitch / sizeof(PIXEL16U);

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    upper_plane = data;
    lower_plane = upper_plane + roi.width * roi.height / 2;

    upper_row_ptr = upper_plane;
    lower_row_ptr = lower_plane;

    upper_row_pitch = roi.width / 2;
    lower_row_pitch = roi.width * 2;

    for (row = 0; row < roi.height; row++)
    {
        // Process two pixels per iteration
        for (column = 0; column < roi.width; column += 2)
        {
            PIXEL16U Y1_upper, Cr_upper, Y2_upper, Cb_upper;
            PIXEL16U Y1_lower, Cr_lower, Y2_lower, Cb_lower;
            PIXEL16U Y1, Cr, Y2, Cb;
            PIXEL16U upper;

            upper = upper_row_ptr[column / 2];

            Cb_upper = (upper >> 6) & 0x03;
            Y1_upper = (upper >> 4) & 0x03;
            Cr_upper = (upper >> 2) & 0x03;
            Y2_upper = (upper >> 0) & 0x03;

            Cb_lower = lower_row_ptr[2 * column + 0];
            Y1_lower = lower_row_ptr[2 * column + 1];
            Cr_lower = lower_row_ptr[2 * column + 2];
            Y2_lower = lower_row_ptr[2 * column + 3];

            Y1 = (Y1_lower << 2) | Y1_upper;
            Y2 = (Y2_lower << 2) | Y2_upper;
            Cr = (Cr_lower << 2) | Cr_upper;
            Cb = (Cb_lower << 2) | Cb_upper;

            plane_array[0][column + 0] = Y1;
            plane_array[0][column + 1] = Y2;
            plane_array[1][column / 2] = Cr;
            plane_array[2][column / 2] = Cb;
        }

        upper_row_ptr += upper_row_pitch;
        lower_row_ptr += lower_row_pitch;

        for (i = 0; i < 3; i++)
        {
            plane_array[i] += plane_pitch[i];
        }
    }
}

/*!
	@brief Convert the Avid 2.14 packed format to planes of 10-bit unsigned pixels
*/
void ConvertCbYCrY_16bit_2_14ToFrame16s(void *data, int pitch, FRAME *frame, void *buffer, int precision, int alpha)
{
    PIXEL16S *input_row_ptr = (PIXEL16S *)data;
    int input_row_pitch = pitch / sizeof(PIXEL16S);
    PIXEL16U *plane_array[3];
    int plane_pitch[3];
    ROI roi;
    int row, column;
    int i;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);
    //display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        plane_array[i] = (PIXEL16U *)image->band[0];
        plane_pitch[i] = image->pitch / sizeof(PIXEL16U);

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    for (row = 0; row < roi.height; row++)
    {
        // Process two pixels per iteration
        for (column = 0; column < roi.width; column += 2)
        {
            int32_t Y1_signed, Cr_signed, Y2_signed, Cb_signed;
            int32_t Y1_unsigned, Cr_unsigned, Y2_unsigned, Cb_unsigned;

            // Convert Avid signed 2.14 format to 16-bit unsigned chroma
            Cb_signed = input_row_ptr[2 * column + 0];
            Cb_unsigned = (((224 * (Cb_signed + 8192)) / 16384 + 16) << 2);

            // Convert Avid signed 2.14 format to 16-bit unsigned luma
            Y1_signed = input_row_ptr[2 * column + 1];
            Y1_unsigned = (((219 * Y1_signed) / 16384 + 16) << 2);

            // Convert Avid signed 2.14 format to 16-bit unsigned chroma
            Cr_signed = input_row_ptr[2 * column + 2];
            Cr_unsigned = (((224 * (Cr_signed + 8192)) / 16384 + 16) << 2);

            // Convert Avid signed 2.14 format to 16-bit unsigned luma
            Y2_signed = input_row_ptr[2 * column + 3];
            Y2_unsigned = (((219 * Y2_signed) / 16384 + 16) << 2);

            Cb_unsigned = SATURATE_10U(Cb_unsigned);
            Y1_unsigned = SATURATE_10U(Y1_unsigned);
            Cr_unsigned = SATURATE_10U(Cr_unsigned);
            Y2_unsigned = SATURATE_10U(Y2_unsigned);

            // Output the unsigned 10-bit components for the next two pixels
            plane_array[0][column + 0] = Y1_unsigned;
            plane_array[0][column + 1] = Y2_unsigned;
            plane_array[1][column / 2] = Cr_unsigned;
            plane_array[2][column / 2] = Cb_unsigned;
        }

        input_row_ptr += input_row_pitch;

        for (i = 0; i < 3; i++)
        {
            plane_array[i] += plane_pitch[i];
        }
    }
}

/*!
	@brief Convert the Avid 10.6 packed format to planes of 10-bit unsigned pixels

	Note that the Avid 10.6 format is exactly the same as the CineForm 16-bit unsigned format
	except that the color channels are ordered Cb, Y1, Cr, Y2.  Within each color channel, the
	values are in big endian order according to the Avid Buffer Formats document.

	@todo Need to check big versus little endian.
*/
void ConvertCbYCrY_16bit_10_6ToFrame16s(void *data, int pitch, FRAME *frame, void *buffer, int precision, int alpha)
{
    PIXEL16U *input_row_ptr = (PIXEL16U *)data;
    int input_row_pitch = pitch / sizeof(PIXEL16U);
    PIXEL16U *output_plane_array[3];
    int output_plane_pitch[3];
    ROI roi;
    int row, column;
    int i;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);
    //display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        output_plane_array[i] = (PIXEL16U *)image->band[0];
        output_plane_pitch[i] = image->pitch / sizeof(PIXEL16U);

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    for (row = 0; row < roi.height; row++)
    {
        // Process two pixels per iteration
        for (column = 0; column < roi.width; column += 2)
        {
            unsigned short Y1_unsigned, Cr_unsigned, Y2_unsigned, Cb_unsigned;

            // Extract the color components and shift off the fractional part
            Cb_unsigned = input_row_ptr[2 * column + 0];
            Y1_unsigned = input_row_ptr[2 * column + 1];
            Cr_unsigned = input_row_ptr[2 * column + 2];
            Y2_unsigned = input_row_ptr[2 * column + 3];

            Cb_unsigned >>= 6;
            Y1_unsigned >>= 6;
            Cr_unsigned >>= 6;
            Y2_unsigned >>= 6;

            output_plane_array[0][column + 0] = Y1_unsigned;
            output_plane_array[0][column + 1] = Y2_unsigned;
            output_plane_array[1][column / 2] = Cr_unsigned;
            output_plane_array[2][column / 2] = Cb_unsigned;
        }

        input_row_ptr += input_row_pitch;

        for (i = 0; i < 3; i++)
        {
            output_plane_array[i] += output_plane_pitch[i];
        }
    }
}

/*!
	@brief Convert Avid unsigned char format to planes of 10-bit unsigned pixels
*/
void ConvertCbYCrY_8bitToFrame16s(void *data, int pitch, FRAME *frame, void *buffer, int precision, int alpha)
{
    PIXEL8U *input_row_ptr = (PIXEL8U *)data;
    int input_row_pitch = pitch / sizeof(PIXEL8U);
    PIXEL16U *output_plane_array[3];
    int output_plane_pitch[3];
    ROI roi;
    int row, column;
    int i;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);
    //display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        output_plane_array[i] = (PIXEL16U *)image->band[0];
        output_plane_pitch[i] = image->pitch / sizeof(PIXEL16U);

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    for (row = 0; row < roi.height; row++)
    {
        // Process two pixels per iteration
        for (column = 0; column < roi.width; column += 2)
        {
            unsigned short Y1_unsigned, Cr_unsigned, Y2_unsigned, Cb_unsigned;

            // Extract the color components and shift to 10 bits
            Cb_unsigned = input_row_ptr[2 * column + 0];
            Y1_unsigned = input_row_ptr[2 * column + 1];
            Cr_unsigned = input_row_ptr[2 * column + 2];
            Y2_unsigned = input_row_ptr[2 * column + 3];

            Cb_unsigned <<= 2;
            Y1_unsigned <<= 2;
            Cr_unsigned <<= 2;
            Y2_unsigned <<= 2;

            output_plane_array[0][column + 0] = Y1_unsigned;
            output_plane_array[0][column + 1] = Y2_unsigned;
            output_plane_array[1][column / 2] = Cr_unsigned;
            output_plane_array[2][column / 2] = Cb_unsigned;
        }

        input_row_ptr += input_row_pitch;

        for (i = 0; i < 3; i++)
        {
            output_plane_array[i] += output_plane_pitch[i];
        }
    }
}

/*!
	@brief Convert Avid short format to planes of 10-bit unsigned pixels
*/
void ConvertCbYCrY_16bitToFrame16s(void *data, int pitch, FRAME *frame, void *buffer, int precision, int alpha)
{
    PIXEL16U *input_row_ptr = (PIXEL16U *)data;
    int input_row_pitch = pitch / sizeof(PIXEL16U);
    PIXEL16U *output_plane_array[3];
    int output_plane_pitch[3];
    ROI roi;
    int row, column;
    int i;

    // The frame format should be three channels of YUV (4:2:2 format)
    assert(frame->num_channels == 3);
    assert(frame->format == FRAME_FORMAT_YUV);
    //display_height = frame->display_height;

    // Get pointers to the image planes and set the pitch for each plane
    for (i = 0; i < 3; i++)
    {
        IMAGE *image = frame->channel[i];

        // Set the pointer to the individual planes and pitch for each channel
        output_plane_array[i] = (PIXEL16U *)image->band[0];
        output_plane_pitch[i] = image->pitch / sizeof(PIXEL16U);

        // The first channel establishes the processing dimensions
        if (i == 0)
        {
            roi.width = image->width;
            roi.height = image->height;
        }
    }

    for (row = 0; row < roi.height; row++)
    {
        // Process two pixels per iteration
        for (column = 0; column < roi.width; column += 2)
        {
            unsigned short Y1_unsigned, Cr_unsigned, Y2_unsigned, Cb_unsigned;

            // Extract the color components and shift to 10 bits
            Cb_unsigned = input_row_ptr[2 * column + 0];
            Y1_unsigned = input_row_ptr[2 * column + 1];
            Cr_unsigned = input_row_ptr[2 * column + 2];
            Y2_unsigned = input_row_ptr[2 * column + 3];

            Cb_unsigned >>= 6;
            Y1_unsigned >>= 6;
            Cr_unsigned >>= 6;
            Y2_unsigned >>= 6;

            output_plane_array[0][column + 0] = Y1_unsigned;
            output_plane_array[0][column + 1] = Y2_unsigned;
            output_plane_array[1][column / 2] = Cr_unsigned;
            output_plane_array[2][column / 2] = Cb_unsigned;
        }

        input_row_ptr += input_row_pitch;

        for (i = 0; i < 3; i++)
        {
            output_plane_array[i] += output_plane_pitch[i];
        }
    }
}


#if _ALLOCATOR
void DeleteFrame(ALLOCATOR *allocator, FRAME *frame)
#else
void DeleteFrame(FRAME *frame)
#endif
{
    int i;

    if (frame == NULL) return;

    for (i = 0; i < frame->num_channels; i++)
    {
        IMAGE *image = frame->channel[i];
        if (image != NULL)
        {
#if _ALLOCATOR
            DeleteImage(allocator, image);
#else
            DeleteImage(image);
#endif
        }
    }

#if _ALLOCATOR
    Free(allocator, frame);
#else
    MEMORY_FREE(frame);
#endif
}

#if 0
//void Generate10bitThumbnail(BITSTREAM *output, int type)
int GenerateThumbnail(void *samplePtr,
                      size_t sampleSize,
                      void *outputBuffer,
                      size_t outputSize,
                      size_t flags,
                      size_t *retWidth,
                      size_t *retHeight,
                      size_t *retSize)
{
    BITSTREAM input;
    SAMPLE_HEADER header;
    BYTE *ptr = samplePtr;
    uint32_t  *optr = (uint32_t *)outputBuffer;

    InitBitstreamBuffer(&input, (uint8_t *)samplePtr, (DWORD)sampleSize, BITSTREAM_ACCESS_READ);


    memset(&header, 0, sizeof(SAMPLE_HEADER));
    header.find_lowpass_bands = 1;

    if (ParseSampleHeader(&input, &header))
    {
        uint32_t *yptr;
        uint32_t *uptr;
        uint32_t *vptr;
        uint32_t *gptr;
        uint32_t *gptr2;
        uint32_t *rptr;
        uint32_t *rptr2;
        uint32_t *bptr;
        uint32_t *bptr2;
        int x, y, width, height, watermark = flags >> 8;

        width = (header.width + 7) / 8;
        height = (header.height + 7) / 8;

        if (flags & 3)
        {
            if (outputSize < width * height * 4)
                return 0; // failed

            switch (header.encoded_format)
            {
                case ENCODED_FORMAT_UNKNOWN:
                case ENCODED_FORMAT_YUV_422:
                    yptr = (uint32_t *)(ptr + header.thumbnail_channel_offsets[0]);
                    uptr = (uint32_t *)(ptr + header.thumbnail_channel_offsets[1]);
                    vptr = (uint32_t *)(ptr + header.thumbnail_channel_offsets[2]);
                    for (y = 0; y < height; y++)
                    {
                        for (x = 0; x < width; x += 4)
                        {
                            int y1, u1, v1, y2, u2, v2, r, g, b, rgb, pp;

                            pp = _bswap(*yptr++);
                            y1 = ((pp >> 20) & 0x3ff) - 64;
                            y2 = ((pp >> 4) & 0x3ff) - 64;
                            pp = _bswap(*uptr++);
                            u1 = ((pp >> 20) & 0x3ff) - 0x200;
                            u2 = ((pp >> 4) & 0x3ff) - 0x200;
                            pp = _bswap(*vptr++);
                            v1 = ((pp >> 20) & 0x3ff) - 0x200;
                            v2 = ((pp >> 4) & 0x3ff) - 0x200;

                            r = (1192 * y1 + 1836 * u1) >> 10;
                            g = (1192 * y1 - 547 * u1 - 218 * v1) >> 10;
                            b = (1192 * y1 + 2166 * v1) >> 10;
                            if (r < 0) r = 0;
                            if (r > 0x3ff) r = 0x3ff;
                            if (g < 0) g = 0;
                            if (g > 0x3ff) g = 0x3ff;
                            if (b < 0) b = 0;
                            if (b > 0x3ff) b = 0x3ff;
                            rgb = ((r << 22) | (g << 12) | (b << 2));
                            *(optr++) = _bswap(rgb);

                            r = (1192 * y2 + 1836 * u1) >> 10;
                            g = (1192 * y2 - 547 * u1 - 218 * v1) >> 10;
                            b = (1192 * y2 + 2166 * v1) >> 10;
                            if (r < 0) r = 0;
                            if (r > 0x3ff) r = 0x3ff;
                            if (g < 0) g = 0;
                            if (g > 0x3ff) g = 0x3ff;
                            if (b < 0) b = 0;
                            if (b > 0x3ff) b = 0x3ff;
                            rgb = ((r << 22) | (g << 12) | (b << 2));
                            *(optr++) = _bswap(rgb);


                            pp = _bswap(*yptr++);
                            y1 = ((pp >> 20) & 0x3ff) - 64;
                            y2 = ((pp >> 4) & 0x3ff) - 64;

                            r = (1192 * y1 + 1836 * u2) >> 10;
                            g = (1192 * y1 - 547 * u2 - 218 * v2) >> 10;
                            b = (1192 * y1 + 2166 * v2) >> 10;
                            if (r < 0) r = 0;
                            if (r > 0x3ff) r = 0x3ff;
                            if (g < 0) g = 0;
                            if (g > 0x3ff) g = 0x3ff;
                            if (b < 0) b = 0;
                            if (b > 0x3ff) b = 0x3ff;
                            rgb = ((r << 22) | (g << 12) | (b << 2));
                            *(optr++) = _bswap(rgb);

                            r = (1192 * y2 + 1836 * u2) >> 10;
                            g = (1192 * y2 - 547 * u2 - 218 * v2) >> 10;
                            b = (1192 * y2 + 2166 * v2) >> 10;
                            if (r < 0) r = 0;
                            if (r > 0x3ff) r = 0x3ff;
                            if (g < 0) g = 0;
                            if (g > 0x3ff) g = 0x3ff;
                            if (b < 0) b = 0;
                            if (b > 0x3ff) b = 0x3ff;
                            rgb = ((r << 22) | (g << 12) | (b << 2));
                            *(optr++) = _bswap(rgb);
                        }
                    }
                    break;
                case ENCODED_FORMAT_BAYER:
                    for (y = 0; y < height; y++)
                    {
                        int y1 = (y) / 2;
                        int y2 = (y + 1) / 2;
                        if (y2 == height / 2)
                            y2--;
                        if (y1 == height / 2)
                            y1--;
                        gptr = gptr2 = (uint32_t *)(ptr + header.thumbnail_channel_offsets[0]);
                        gptr += (y1) * (width / 4);
                        rptr = rptr2 = (uint32_t *)(ptr + header.thumbnail_channel_offsets[1]);
                        rptr += (y1) * (width / 4);
                        bptr = bptr2 = (uint32_t *)(ptr + header.thumbnail_channel_offsets[2]);
                        bptr += (y1) * (width / 4);

                        if (y1 != y2)
                        {
                            gptr2 += (y2) * (width / 4);
                            rptr2 += (y2) * (width / 4);
                            bptr2 += (y2) * (width / 4);

                            for (x = 0; x < width; x += 4)
                            {
                                int r, g, b, r1, g1, b1, r2, g2, b2, r3, g3, b3, r4, g4, b4, rgb, pp;

                                pp = _bswap(*gptr++);
                                g1 = (pp >> 20) & 0x3ff;
                                g2 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*gptr2++);
                                g3 = (pp >> 20) & 0x3ff;
                                g4 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*rptr++);
                                r1 = (pp >> 20) & 0x3ff;
                                r2 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*rptr2++);
                                r3 = (pp >> 20) & 0x3ff;
                                r4 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*bptr++);
                                b1 = (pp >> 20) & 0x3ff;
                                b2 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*bptr2++);
                                b3 = (pp >> 20) & 0x3ff;
                                b4 = (pp >> 4) & 0x3ff;

                                r1 = (r1 - 0x200) * 2 + g1;
                                if (r1 < 0) r1 = 0;
                                if (r1 > 0x3ff) r1 = 0x3ff;
                                b1 = (b1 - 0x200) * 2 + g1;
                                if (b1 < 0) b1 = 0;
                                if (b1 > 0x3ff) b1 = 0x3ff;

                                r2 = (r2 - 0x200) * 2 + g2;
                                if (r2 < 0) r2 = 0;
                                if (r2 > 0x3ff) r2 = 0x3ff;
                                b2 = (b2 - 0x200) * 2 + g2;
                                if (b2 < 0) b2 = 0;
                                if (b2 > 0x3ff) b2 = 0x3ff;

                                r3 = (r3 - 0x200) * 2 + g3;
                                if (r3 < 0) r3 = 0;
                                if (r3 > 0x3ff) r3 = 0x3ff;
                                b3 = (b3 - 0x200) * 2 + g3;
                                if (b3 < 0) b3 = 0;
                                if (b3 > 0x3ff) b3 = 0x3ff;

                                r4 = (r4 - 0x200) * 2 + g4;
                                if (r4 < 0) r4 = 0;
                                if (r4 > 0x3ff) r4 = 0x3ff;
                                b4 = (b2 - 0x200) * 2 + g4;
                                if (b4 < 0) b4 = 0;
                                if (b4 > 0x3ff) b4 = 0x3ff;

                                r = (r1 + r3) >> 1;
                                g = (g1 + g3) >> 1;
                                b = (b1 + b3) >> 1;
                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *(optr++) = _bswap(rgb);

                                r = (r1 + r2 + r3 + r4) >> 2;
                                g = (g1 + g2 + g3 + g4) >> 2;
                                b = (b1 + b2 + b3 + b4) >> 2;
                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *(optr++) = _bswap(rgb);

                                r = (r2 + r4) >> 1;
                                g = (g2 + g4) >> 1;
                                b = (b2 + b4) >> 1;
                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *(optr++) = _bswap(rgb);


                                pp = _bswap(*gptr);
                                g1 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*gptr2);
                                g3 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*rptr);
                                r1 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*rptr2);
                                r3 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*bptr);
                                b1 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*bptr2);
                                b3 = (pp >> 20) & 0x3ff;


                                r1 = (r1 - 0x200) * 2 + g1;
                                if (r1 < 0) r1 = 0;
                                if (r1 > 0x3ff) r1 = 0x3ff;
                                b1 = (b1 - 0x200) * 2 + g1;
                                if (b1 < 0) b1 = 0;
                                if (b1 > 0x3ff) b1 = 0x3ff;

                                r3 = (r3 - 0x200) * 2 + g3;
                                if (r3 < 0) r3 = 0;
                                if (r3 > 0x3ff) r3 = 0x3ff;
                                b3 = (b3 - 0x200) * 2 + g3;
                                if (b3 < 0) b3 = 0;
                                if (b3 > 0x3ff) b3 = 0x3ff;

                                r = (r1 + r2 + r3 + r4) >> 2;
                                g = (g1 + g2 + g3 + g4) >> 2;
                                b = (b1 + b2 + b3 + b4) >> 2;
                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *(optr++) = _bswap(rgb);
                            }
                        }
                        else
                        {
                            for (x = 0; x < width; x += 4)
                            {
                                int r, g, b, r1, g1, b1, r2, g2, b2, rgb, pp;

                                pp = _bswap(*gptr++);
                                g1 = (pp >> 20) & 0x3ff;
                                g2 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*rptr++);
                                r1 = (pp >> 20) & 0x3ff;
                                r2 = (pp >> 4) & 0x3ff;
                                pp = _bswap(*bptr++);
                                b1 = (pp >> 20) & 0x3ff;
                                b2 = (pp >> 4) & 0x3ff;

                                r1 = (r1 - 0x200) * 2 + g1;
                                if (r1 < 0) r1 = 0;
                                if (r1 > 0x3ff) r1 = 0x3ff;
                                b1 = (b1 - 0x200) * 2 + g1;
                                if (b1 < 0) b1 = 0;
                                if (b1 > 0x3ff) b1 = 0x3ff;

                                r2 = (r2 - 0x200) * 2 + g2;
                                if (r2 < 0) r2 = 0;
                                if (r2 > 0x3ff) r2 = 0x3ff;
                                b2 = (b2 - 0x200) * 2 + g2;
                                if (b2 < 0) b2 = 0;
                                if (b2 > 0x3ff) b2 = 0x3ff;

                                rgb = ((r1 << 22) | (g1 << 12) | (b1 << 2));
                                *(optr++) = _bswap(rgb);

                                r = (r1 + r2) >> 1;
                                g = (g1 + g2) >> 1;
                                b = (b1 + b2) >> 1;
                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *(optr++) = _bswap(rgb);

                                rgb = ((r2 << 22) | (g2 << 12) | (b2 << 2));
                                *(optr++) = _bswap(rgb);

                                pp = _bswap(*gptr);
                                g1 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*rptr);
                                r1 = (pp >> 20) & 0x3ff;
                                pp = _bswap(*bptr);
                                b1 = (pp >> 20) & 0x3ff;

                                r1 = (r1 - 0x200) * 2 + g1;
                                if (r1 < 0) r1 = 0;
                                if (r1 > 0x3ff) r1 = 0x3ff;
                                b1 = (b1 - 0x200) * 2 + g1;
                                if (b1 < 0) b1 = 0;
                                if (b1 > 0x3ff) b1 = 0x3ff;

                                r = (r1 + r2) >> 1;
                                g = (g1 + g2) >> 1;
                                b = (b1 + b2) >> 1;
                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *(optr++) = _bswap(rgb);
                            }
                        }
                    }
                    break;

                case ENCODED_FORMAT_RGB_444:
                case ENCODED_FORMAT_RGBA_4444:
                    gptr = (uint32_t *)(ptr + header.thumbnail_channel_offsets[0]);
                    rptr = (uint32_t *)(ptr + header.thumbnail_channel_offsets[1]);
                    bptr = (uint32_t *)(ptr + header.thumbnail_channel_offsets[2]);
                    for (y = 0; y < height; y++)
                    {
                        for (x = 0; x < width; x += 2)
                        {
                            int r1, g1, b1, r2, g2, b2, rgb, pp;

                            pp = _bswap(*gptr++);
                            g1 = (pp >> 20) & 0x3ff;
                            g2 = (pp >> 4) & 0x3ff;
                            pp = _bswap(*rptr++);
                            r1 = (pp >> 20) & 0x3ff;
                            r2 = (pp >> 4) & 0x3ff;
                            pp = _bswap(*bptr++);
                            b1 = (pp >> 20) & 0x3ff;
                            b2 = (pp >> 4) & 0x3ff;
                            rgb = ((r1 << 22) | (g1 << 12) | (b1 << 2));
                            *(optr++) = _bswap(rgb);

                            rgb = ((r2 << 22) | (g2 << 12) | (b2 << 2));
                            *(optr++) = _bswap(rgb);
                        }
                    }
                    break;
            }

            if (flags & 2) // DXP-c watermark
            {
                uint32_t *lptr = (uint32_t *)outputBuffer;
                int yy = 0;
                for (y = height - dpxc_image.height; y < height; y++)
                {
                    lptr = (uint32_t *)outputBuffer;
                    lptr += y * width;
                    if (y >= 0)
                    {
                        for (x = 0; x < dpxc_image.width; x++)
                        {
                            if (x < width)
                            {
                                int r, g, b, rgb;
                                int alpha = 128;

                                rgb = _bswap(*lptr);
                                r = (rgb >> 22) & 0x3ff;
                                g = (rgb >> 12) & 0x3ff;
                                b = (rgb >> 2) & 0x3ff;

                                r = (r * alpha + dpxc_image.pixel_data[x * 3 + yy * dpxc_image.width * 3 + 0] * 4 * (256 - alpha)) >> 8;
                                g = (g * alpha + dpxc_image.pixel_data[x * 3 + yy * dpxc_image.width * 3 + 1] * 4 * (256 - alpha)) >> 8;
                                b = (b * alpha + dpxc_image.pixel_data[x * 3 + yy * dpxc_image.width * 3 + 2] * 4 * (256 - alpha)) >> 8;

                                rgb = ((r << 22) | (g << 12) | (b << 2));
                                *lptr++ = _bswap(rgb);
                            }
                        }
                        yy++;
                    }
                }
            }
        }
    }

    if (retWidth)
        *retWidth = (header.width + 7) / 8;
    if (retHeight)
        *retHeight = (header.height + 7) / 8;
    if (retSize)
        *retSize = (*retWidth * *retHeight * 4);

    return 1;
}
#endif
