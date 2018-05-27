/*!
 * @file lutpath.c
 * @brief Active MetadataTools
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

#include "stdafx.h"
#include "config.h"
#include "encoder.h"

#ifdef _WIN32
#include <tchar.h>
#else
#include <errno.h>
#endif

#include "codec.h"
#include "lutpath.h"

// Define the maximum length of a keyword including the terminating nul
#define KEYWORD_MAX 64

#ifndef DEBUG
#define DEBUG (1 && _DEBUG)
#endif

void InitializeCFHDDataToDefaults(CFHDDATA *cfhddata, uint32_t colorspace)
{
    float unity[4] = {1.0, 1.0, 1.0, 1.0};
    float cm[12] = {1.0, 0.0, 0.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0
                   };
    int channelNum;

    // Set all these fields to default values
    cfhddata->update_last_used = 1;
    cfhddata->bayer_format = 0;
    cfhddata->encode_curve = 0;
    cfhddata->encode_curve_preset = 0;
    cfhddata->decode_curve = 0;
    //	cfhddata->process_path_flags = 0; //DAN20090417
    cfhddata->user_look_CRC = 0;
    cfhddata->demosaic_type = 0;
    cfhddata->channel_flip = 0;		// CMD20100303 - not initializing
    cfhddata->calibration = 0;
    cfhddata->FramingFlags = 0;     // CMD20130424 - not initializing
    cfhddata->FrameOffsetX = 0;		//DAN20100903
    cfhddata->FrameOffsetY = 0;		//DAN20100903
    cfhddata->FrameOffsetR = 0;
    cfhddata->FrameOffsetF = 0;
    cfhddata->FrameHScale = 1.0;
    cfhddata->FrameHDynamic = 1.0;
    cfhddata->FrameHDynCenter = 0.5;
    cfhddata->FrameHDynWidth = 0.0;
    cfhddata->split_CC_position = 0.0;
    memcpy(cfhddata->orig_colormatrix, cm, 48);
    memcpy(cfhddata->custom_colormatrix, cm, 48);
    cfhddata->version = CFHDDATA_VERSION;
    cfhddata->MSChannel_type_value = 0;
    cfhddata->use_base_matrix = 2; //use user matrix
    cfhddata->ComputeFlags = 0;
    cfhddata->lensGoPro = 1;
    cfhddata->lensSphere = 0;
    cfhddata->lensFill = 0;
    cfhddata->doMesh = 0;


    // Set all these fields to default values for each channel

    //  float user_contrast;		// -1.0 to 3.0+, 0.0 unity   real range 0 to 4
    //	float user_saturation;		// -1.0 to 3.0+, 0.0 unity   real range 0 to 4
    //	float user_highlight_sat;	// -1.0 to 0.0+, 0.0 unity   real range 0 to 1
    //	float user_highlight_point;	// -1.0 to 0.0+, 0.0 unity   real range 0 to 11
    //	float user_vignette_start;	// -1.0 to 0.0+, 0.0 unity   real range 0 to 1
    //	float user_vignette_end;	// -1.0 to 0.0+, 0.0 unity   real range 0 to 2
    //	float user_vignette_gain;	// 0.0 unity   real range 0 to 8
    //	float user_exposure;		// -1.0 to 7.0+, 0.0 unity   real range 0 to 8
    //	float user_rgb_lift[3];		// -1.0 to 1.0, 0.0 unity black offsets
    //	float user_rgb_gamma[3];		// if 0.0  then no gamma tweaks -- not a camera control used in post.
    //	float user_rgb_gain[3];		// -1.0 to 3.0+, 0.0 unity RGB gains (upon the current matrix)   real range 0 to 4
    //	float white_balance[3];
    //	float user_cdl_sat;			// -1.0 to 3.0+, 0.0 unity   real range 0 to 4
    //	float user_blur_sharpen;	// 0.0 to 1.0, 0.0 unity -- 1.0 sharp
    //
    //	float FrameZoom;
    //	float FrameDiffZoom;
    //	float FrameAutoZoom;
    //	float HorizontalOffset;			// 0.0 centre, -1.0 far left, 1.0 far right
    //	float VerticalOffset;			// 0.0 centre, -1.0 far up, 1.0 far down
    //	float RotationOffset;			// 0.0 centre, -0.1 anti-clockwize, 0.1 clockwize
    //	float FrameKeyStone;
    //	float FloatingWindowMaskL;
    //	float FloatingWindowMaskR;
    //	float FrameTilt;

    memset(&cfhddata->channel[0], 0, sizeof(ChannelData) * 3);

    for (channelNum = 0; channelNum < 3; channelNum++)
    {
        // Default is 1.0 memcpy(cfhddata->channel[channelNum].user_rgb_lift,unity,12);
        memcpy(cfhddata->channel[channelNum].user_rgb_gamma, unity, 12);
        memcpy(cfhddata->channel[channelNum].user_rgb_gain, unity, 12);
        memcpy(cfhddata->channel[channelNum].white_balance, unity, 12);
        cfhddata->channel[channelNum].FrameZoom = 1.0;
        cfhddata->channel[channelNum].FrameDiffZoom = 1.0;
        cfhddata->channel[channelNum].FrameAutoZoom = 1.0;
    }

    cfhddata->cpu_limit = 0;		// if non-zero limit to number of cores used to run.
    cfhddata->cpu_affinity = 0;		// if non-zero set the CPU affinity used to run each thread.
    cfhddata->colorspace = colorspace; //DAN20010916 -- fix for IP frames with the 422to444 filter
    cfhddata->ignore_disk_database = false;             // Not initialized anywhere obvious..
    cfhddata->force_metadata_refresh = true;            // first time through
}

void CopyMetadataChunks(DECODER *decoder, DECODER *parentDecoder)
{
    int i;
    for (i = 0; i < decoder->metadatachunks; i++)
    {
#if _ALLOCATOR
        if (decoder->mdc[i])
            Free(decoder->allocator, decoder->mdc[i]);
#else
        if (decoder->mdc[i])
            MEMORY_FREE(decoder->mdc[i]);
#endif
        decoder->mdc_size[i] = 0;
    }
    decoder->metadatachunks = 0;

    if (parentDecoder)
    {
        for (i = 0; i < parentDecoder->metadatachunks; i++)
        {
#if _ALLOCATOR
            decoder->mdc[decoder->metadatachunks] = (unsigned char *)Alloc(decoder->allocator, parentDecoder->mdc_size[i]);
#else
            decoder->mdc[decoder->metadatachunks] = (unsigned char *)MEMORY_ALLOC(parentDecoder->mdc_size[i]);
#endif
            if (decoder->mdc[decoder->metadatachunks] && parentDecoder->mdc[i])
                memcpy(decoder->mdc[decoder->metadatachunks], parentDecoder->mdc[i], parentDecoder->mdc_size[i]);
            decoder->mdc_size[decoder->metadatachunks] = parentDecoder->mdc_size[i];
            decoder->metadatachunks++;
        }
    }
}

void OverrideCFHDDATA(DECODER *decoder, unsigned char *lpCurrentBuffer, int nWordsUsed)
{
    CFHDDATA *cfhddata = &decoder->cfhddata;
    uint32_t *last_set_time = &decoder->last_set_time;
    int process_path_flags_mask = decoder->cfhddata.process_path_flags_mask;
    int checkdiskinfo = 0;
    int checkdiskinfotime = 0;
    decoder->drawmetadataobjects = 0; // fix for metadata display on P frames.

    decoder->codec.PFrame = IsSampleKeyFrame(lpCurrentBuffer, nWordsUsed) ? 0 : (1 - decoder->image_dev_only);
    //if(decoder->codec.PFrame && decoder->codec.unique_framenumber != -1 && (decoder->codec.unique_framenumber & 1) == 0)
    if (decoder->codec.PFrame && decoder->codec.unique_framenumber != UINT32_MAX && (decoder->codec.unique_framenumber & 1) == 0)
    {
        decoder->codec.unique_framenumber++;
    }

    clock_t process_time = clock();         // defined to return in CLOCKS_PER_SEC units
    time_t now;
    uint32_t diff = (uint32_t)process_time - (uint32_t) * last_set_time;
    now = time(NULL);                       // time is defined in POSIX.1 as seconds since 00:00:00 Jan 1, 1970

#define MS_DIFF	(CLOCKS_PER_SEC / 10)

    // Pre-processing
    //  See if the decoder has been initialized.  If not, initialize it and the cfhddata structures
    //  Read the first chunk of metadata, clear out the cfhddata structure and init from the metadata
    //
    // Read the metadata first to see if it is a new file
    {
        //unsigned char *ptr;
        //int len;

        myGUID lastGUID = cfhddata->clip_guid;

        // Read Clip GUID and other file data before checking the database.
        //if(lastGUID.Data1==0 && lastGUID.Data2==0 && lastGUID.Data3==0)
        size_t metadatasize = 0;
        void *metadatastart;
        bool cfhddataInitialized = false;

        //void *data;
        METADATA_TAG tag;
        METADATA_TYPE type;
        METADATA_SIZE size;
        unsigned char *buf = lpCurrentBuffer;
        unsigned int samplesize = nWordsUsed;

        if (decoder->MDPdefault.initialized == 0)
        {
            decoder->MDPdefault.initialized = 1;

            InitializeCFHDDataToDefaults(cfhddata, decoder->frame.colorspace);
            cfhddataInitialized = true;

            decoder->metadatachunks = 0;
            decoder->drawmetadataobjects = 0;
            decoder->preformatted_3D_type = 0;
            decoder->metadatachunks = 0;
            decoder->drawmetadataobjects = 0;

            decoder->ActiveSafe[0] = 0.0375f / 2.0f;
            decoder->ActiveSafe[1] = 0.05f / 2.0f;
            decoder->TitleSafe[0] = 0.075f / 2.0f;
            decoder->TitleSafe[1] = 0.1f / 2.0f;
            decoder->OverlaySafe[0] = 0.075f / 2.0f;
            decoder->OverlaySafe[1] = 0.1f / 2.0f;

#ifdef _WIN32
            strcpy_s(decoder->MDPdefault.font, sizeof(decoder->MDPdefault.font), "Courier New Bold");
#else
            strcpy(decoder->MDPdefault.font, "Courier New Bold");
#endif
            decoder->MDPdefault.fontsize = 0.04f;

            decoder->MDPdefault.bcolor[0] = 0.0;
            decoder->MDPdefault.bcolor[1] = 0.0;
            decoder->MDPdefault.bcolor[2] = 0.0;
            decoder->MDPdefault.bcolor[3] = 1.0f;

            decoder->MDPdefault.scolor[0] = 0.0;
            decoder->MDPdefault.scolor[1] = 0.0;
            decoder->MDPdefault.scolor[2] = 0.0;
            decoder->MDPdefault.scolor[3] = 1.0f;

            decoder->MDPdefault.fcolor[0] = 1.0f;
            decoder->MDPdefault.fcolor[1] = 1.0f;
            decoder->MDPdefault.fcolor[2] = 1.0f;
            decoder->MDPdefault.fcolor[3] = 1.0f;

            {
                int j;
                for (j = 0; j < 16; j++)
                {
                    decoder->MDPdefault.xypos[j][0] = -1;
                    decoder->MDPdefault.xypos[j][1] = -1;
                }
            }

            memcpy(&decoder->MDPcurrent, &decoder->MDPdefault, sizeof(MDParams));

            //DAN20100114 -- do these reset once outside the MetaDataFindFirst() test, just in case the file is old and has no metadata.

            //DAN20080710 -- reset the value before loading them, as some RAW streams didn't have all the
            //value, which causes a database reset to fail (switch color database would not switch for
            //Premiere.)

            //decoder->codec.unique_framenumber = -1;
            decoder->codec.unique_framenumber = UINT32_MAX;
        }
        else
            cfhddataInitialized = true;

        if (decoder->image_dev_only || (metadatastart = MetaDataFindFirst(buf, samplesize,
                                        &metadatasize, &tag, &size, &type)))
        {
            //DAN20080710 -- reset the value before loading them, as some RAW streams didn't have all the
            //value, which causes a database reset to fail (switch color database would not switch for
            //Premiere.)
            if (false == cfhddataInitialized)
            {
                InitializeCFHDDataToDefaults(cfhddata, decoder->frame.colorspace);
                cfhddata->force_metadata_refresh = false;
            }
            decoder->metadatachunks = 0;
            decoder->drawmetadataobjects = 0;
            decoder->ghost_bust_left = 0;
            decoder->ghost_bust_right = 0;
            decoder->preformatted_3D_type = 0;
            decoder->codec.unique_framenumber = UINT32_MAX;
            CopyMetadataChunks(decoder, NULL);

            if (!decoder->image_dev_only)
            {
                do
                {
                    buf = (unsigned char *)metadatastart;
                    buf -= 8; // Point to the tag not the data

                    UpdateCFHDDATA(decoder, buf, (int)metadatasize, 0, METADATA_PRIORITY_FRAME);
                    buf += metadatasize;
                    samplesize -= (unsigned int)metadatasize;
                } while ((metadatastart = MetaDataFindFirst(buf, samplesize, &metadatasize, &tag, &size, &type)));
            }

            if (decoder->image_dev_only || memcmp( &lastGUID, &cfhddata->clip_guid, sizeof(cfhddata->clip_guid) ) != 0)
            {
                if (cfhddata->ignore_disk_database == false)
                {
                    checkdiskinfo = 1;	// CD See if it is a new clip  Need to set if not ignoring disk
                }

                memcpy(&lastGUID, &cfhddata->clip_guid, sizeof(cfhddata->clip_guid));
            }
        }
    }

    // Need to move here since cfhddata may not be initialized yet...

    if (diff > MS_DIFF || *last_set_time == 0 || now != decoder->last_time_t) // only test every 1000ms
    {
        if (cfhddata->ignore_disk_database == false)
            checkdiskinfo = 1;
        checkdiskinfotime = 1;
    }

    if (checkdiskinfo || cfhddata->force_disk_database)	// only test every 1000ms - this is common to both conditions so
        // move it out here so the init is only done once
    {
        //  Lets note we just checked because we have a new clip and/or time expired.

        *last_set_time = (unsigned int)process_time;
        decoder->last_time_t = now;
    }

    {
        if (process_path_flags_mask > 0)
        {
            cfhddata->process_path_flags_mask = process_path_flags_mask | 1;
        }
    }

    if ((uint32_t)decoder->frame.colorspace != cfhddata->colorspace && cfhddata->colorspace)
    {
        if (cfhddata->colorspace & COLORSPACE_MASK)
            decoder->frame.colorspace = cfhddata->colorspace;		// colorspace and 422->444
        else
            decoder->frame.colorspace |= (cfhddata->colorspace & ~COLORSPACE_MASK); // 422->444 only
    }

    if (decoder->thread_cntrl.limit == 0 && cfhddata->cpu_limit)
    {
        decoder->thread_cntrl.limit = cfhddata->cpu_limit;
        decoder->thread_cntrl.set_thread_params = 1;
    }

    if (decoder->thread_cntrl.affinity == 0 && cfhddata->cpu_affinity)
    {
        decoder->thread_cntrl.affinity = cfhddata->cpu_affinity;
        decoder->thread_cntrl.set_thread_params = 1;
    }
}

void OverrideCFHDDATAUsingParent(struct decoder *decoder, struct decoder *parentDecoder, unsigned char *lpCurrentBuffer, int nWordsUsed)
{
    // Copy the databases and buffers from the parent instead of doing a read from disk
    //    fprintf(stderr, "Usepar\n");

    CFHDDATA *cfhddata = &decoder->cfhddata;
    int process_path_flags_mask = decoder->cfhddata.process_path_flags_mask;
    myGUID lastGUID = cfhddata->clip_guid;

    decoder->codec.PFrame = IsSampleKeyFrame(lpCurrentBuffer, nWordsUsed) ? 0 : 1;
    if (decoder->codec.PFrame && decoder->codec.unique_framenumber != UINT32_MAX && (decoder->codec.unique_framenumber & 1) == 0)
    {
        decoder->codec.unique_framenumber++;
    }
    // Pre-processing
    //  See if the decoder has been initialized.  If not, initialize it and the cfhddata structures
    //  Read the first chunk of metadata, clear out the cfhddata structure and init from the metadata
    //
    // Read the metadata first to see if it is a new file
    {
        // Read Clip GUID and other file data before checking the database.
        //if(lastGUID.Data1==0 && lastGUID.Data2==0 && lastGUID.Data3==0)
        size_t metadatasize = 0;
        void *metadatastart;
        bool cfhddataInitialized = false;

        //void *data;
        METADATA_TAG tag;
        METADATA_TYPE type;
        METADATA_SIZE size;
        unsigned char *buf = lpCurrentBuffer;
        unsigned int samplesize = nWordsUsed;

        if (decoder->MDPdefault.initialized == 0)
        {
            decoder->MDPdefault.initialized = 1;

            InitializeCFHDDataToDefaults(cfhddata, decoder->frame.colorspace);
            cfhddataInitialized = true;

            decoder->metadatachunks = 0;
            decoder->drawmetadataobjects = 0;
            decoder->preformatted_3D_type = 0;
            decoder->metadatachunks = 0;
            decoder->drawmetadataobjects = 0;

            decoder->ActiveSafe[0] = 0.0375f / 2.0f;
            decoder->ActiveSafe[1] = 0.05f / 2.0f;
            decoder->TitleSafe[0] = 0.075f / 2.0f;
            decoder->TitleSafe[1] = 0.1f / 2.0f;
            decoder->OverlaySafe[0] = 0.075f / 2.0f;
            decoder->OverlaySafe[1] = 0.1f / 2.0f;

#ifdef _WIN32
            strcpy_s(decoder->MDPdefault.font, sizeof(decoder->MDPdefault.font), "Courier New Bold");
#else
            strcpy(decoder->MDPdefault.font, "Courier New Bold");
#endif
            decoder->MDPdefault.fontsize = 0.04f;

            decoder->MDPdefault.bcolor[0] = 0.0;
            decoder->MDPdefault.bcolor[1] = 0.0;
            decoder->MDPdefault.bcolor[2] = 0.0;
            decoder->MDPdefault.bcolor[3] = 1.0f;

            decoder->MDPdefault.scolor[0] = 0.0;
            decoder->MDPdefault.scolor[1] = 0.0;
            decoder->MDPdefault.scolor[2] = 0.0;
            decoder->MDPdefault.scolor[3] = 1.0f;

            decoder->MDPdefault.fcolor[0] = 1.0f;
            decoder->MDPdefault.fcolor[1] = 1.0f;
            decoder->MDPdefault.fcolor[2] = 1.0f;
            decoder->MDPdefault.fcolor[3] = 1.0f;

            {
                int j;
                for (j = 0; j < 16; j++)
                {
                    decoder->MDPdefault.xypos[j][0] = -1;
                    decoder->MDPdefault.xypos[j][1] = -1;
                }
            }

            memcpy(&decoder->MDPcurrent, &decoder->MDPdefault, sizeof(MDParams));

            //DAN20100114 -- do these reset once outside the MetaDataFindFirst() test, just in case the file is old and has no metadata.

            //DAN20080710 -- reset the value before loading them, as some RAW streams didn't have all the
            //value, which causes a database reset to fail (switch color database would not switch for
            //Premiere.)

            //decoder->codec.unique_framenumber = -1;
            decoder->codec.unique_framenumber = UINT32_MAX;
        }

        //  The FRAME metadata cannot be copied from the parent, rebuild it from the sample.
        InitializeCFHDDataToDefaults(cfhddata, decoder->frame.colorspace);

        if ((metadatastart = MetaDataFindFirst(buf, samplesize,
                                               &metadatasize, &tag, &size, &type)))
        {
            //DAN20080710 -- reset the value before loading them, as some RAW streams didn't have all the
            //value, which causes a database reset to fail (switch color database would not switch for
            //Premiere.)
            if (false == cfhddataInitialized)
            {
                InitializeCFHDDataToDefaults(cfhddata, decoder->frame.colorspace);
                cfhddata->force_metadata_refresh = false;
            }
            decoder->metadatachunks = 0;
            decoder->drawmetadataobjects = 0;
            decoder->ghost_bust_left = 0;
            decoder->ghost_bust_right = 0;
            decoder->preformatted_3D_type = 0;
            decoder->cdl_sat = 0;
            decoder->codec.unique_framenumber = UINT32_MAX;
        }
    }
    // Copy the metadata from the parent.  Process the metadata in the correct priority order
    // NOTE: This is done in 2 passes for a good reason.  First clear out all the metadata databases and copy
    //       from the parent then process the data .  It must be done this way since processing the main
    //       colr database (METADATA_PRIORITY_DATABASE) changes the metadata in the METADATA_PRIORITY_DATABASE_1 and
    //       METADATA_PRIORITY_DATABASE_2 databases if single eye adjustments are made.

    // TODO: Should see if we can just reference it..

    // First clear everything out and copy from parent

    if (process_path_flags_mask > 0)
    {
        cfhddata->process_path_flags_mask = process_path_flags_mask | 1;
    }
    // Set the colorspace, cpulimit and thread affinity
    if ((uint32_t)decoder->frame.colorspace != cfhddata->colorspace && cfhddata->colorspace)
    {
        if (cfhddata->colorspace & COLORSPACE_MASK)
            decoder->frame.colorspace = cfhddata->colorspace;		// colorspace and 422->444
        else
            decoder->frame.colorspace |= (cfhddata->colorspace & ~COLORSPACE_MASK); // 422->444 only
    }

    if (decoder->thread_cntrl.limit == 0 && cfhddata->cpu_limit)
    {
        decoder->thread_cntrl.limit = cfhddata->cpu_limit;
        decoder->thread_cntrl.set_thread_params = 1;
    }

    if (decoder->thread_cntrl.affinity == 0 && cfhddata->cpu_affinity)
    {
        decoder->thread_cntrl.affinity = cfhddata->cpu_affinity;
        decoder->thread_cntrl.set_thread_params = 1;
    }
}
