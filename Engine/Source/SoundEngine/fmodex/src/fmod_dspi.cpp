#include "fmod_settings.h"

#include "fmod_dspi.h"
#include "fmod_dsp_filter.h"
#include "fmod_dsp_resampler.h"
#include "fmod_dsp_soundcard.h"
#include "fmod_localcriticalsection.h"
#include "fmod_memory.h"
#include "fmod_soundi.h"
#include "fmod_string.h"
#include "fmod_3d.h"

#if defined(PLATFORM_PS3) || defined(PLATFORM_WINDOWS_PS3MODE) 
#include "../ps3/src/fmod_common_spu.h"
#include "fmod_dsp_sfxreverb.h"
#endif

#ifdef PLATFORM_PS3_SPU
    #include <cell/dma.h>
    #include "fmod_systemi_spu.h"
    #include "fmod_spu_printf.h"
#else
    #include "fmod_systemi.h"
#endif

namespace FMOD
{


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 47224ec8 auto
// OFFSET: 0x008DC390
FMOD_RESULT DSPI::doesUnitExist(DSPI *target)
{
    FMOD_RESULT result;
    int count;
    int numinputs;

    if (this == target)
    {
        return FMOD_OK;
    }

    result = getNumInputs(&numinputs);
    if (result != FMOD_OK)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    for (count=0; count < numinputs; count++)
    {
        DSPConnectionI *connection;

        result = getInput(count, &connection, 0);
        if (result != FMOD_OK)
        {
            return result;
        }

        result = connection->mInputUnit->doesUnitExist(target);
        if (result == FMOD_OK)
        {
            return result;
        }
    }

    return FMOD_ERR_INVALID_PARAM;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 38c9e444 auto
// OFFSET: 0x008DA410
FMOD_RESULT DSPI::alloc(FMOD_DSP_DESCRIPTION_EX *description)
{
    FMOD_RESULT result;
    int outputchannels,rate;

    if (!description)
    {
        return FMOD_ERR_INVALID_PARAM;
    }
    if (description->channels < 0)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    result = mSystem->getSoftwareFormat(&rate, 0, &outputchannels, 0, 0, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    /*
        Do various error checking based on the type of unit being created.
    */
    switch (description->mCategory)
    {
        case FMOD_DSP_CATEGORY_FILTER:
        {
            if (description->channels > outputchannels)
            {
                return FMOD_ERR_TOOMANYCHANNELS;
            }
            break;
        }
        case FMOD_DSP_CATEGORY_DSPCODECMPEG:
        case FMOD_DSP_CATEGORY_DSPCODECRAW:
        case FMOD_DSP_CATEGORY_DSPCODECXMA:
        case FMOD_DSP_CATEGORY_DSPCODECADPCM:
        {
            if (!description->channels)
            {
                return FMOD_ERR_INVALID_PARAM;
            }
            break;
        }
        case FMOD_DSP_CATEGORY_RESAMPLER:
        {
            if (description->channels)
            {
                return FMOD_ERR_INVALID_PARAM;
            }
            break;
        }
        case FMOD_DSP_CATEGORY_WAVETABLE:
        {
            break;
        }
        case FMOD_DSP_CATEGORY_SOUNDCARD:
        {
            break;
        }
        default:
        {
            return FMOD_ERR_INVALID_PARAM;
        }
    }

    FMOD_memcpy(&mDescription, description, sizeof(FMOD_DSP_DESCRIPTION_EX));

    mFlags &= ~FMOD_DSP_FLAG_ACTIVE;
    mFlags &= ~FMOD_DSP_FLAG_USEDADDDSP;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 423a3035 auto
// OFFSET: 0x008DBEF0
FMOD_RESULT DSPI::getInput(int index, DSPConnectionI **input, DSPI **inputdsp)
{
    LocalCriticalSection crit(mSystem->mDSPConnectionCrit);
    int count;
    LinkedListNode *current;
    DSPConnectionI *connection;

    mSystem->flushDSPConnectionRequests();

    crit.enter();

    if (index >= mNumInputs)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    current = mInputHead.getNext();
    if (current == &mInputHead)
    {
        return FMOD_ERR_INTERNAL;
    }

    count = 0;
    while (count < index)
    {
        current = current->getNext();
        count++;
    }
    connection = (DSPConnectionI *)current->getData();

    if (input)
    {
        *input = connection;
    }
    if (inputdsp)
    {
        *inputdsp = connection->mInputUnit;
    }

    crit.leave();

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 831d5a23 auto
// OFFSET: 0x008DC350
FMOD_RESULT DSPI::getInput(int index, DSPI **inputdsp)
{
    return getInput(index, 0, inputdsp);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 7d0964e2 auto
// OFFSET: 0x008DBF90
FMOD_RESULT DSPI::getOutput(int index, DSPConnectionI **output, DSPI **outputdsp)
{
    LocalCriticalSection crit(mSystem->mDSPConnectionCrit);
    int count;
    LinkedListNode *current;
    DSPConnectionI *connection;

    mSystem->flushDSPConnectionRequests();

    crit.enter();

    if (index >= mNumOutputs)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    current = mOutputHead.getNext();
    if (current == &mOutputHead)
    {
        return FMOD_ERR_INTERNAL;
    }

    count = 0;
    while (count < index)
    {
        current = current->getNext();
        count++;
    }
    connection = (DSPConnectionI *)current->getData();

    if (output)
    {
        *output = connection;
    }
    if (outputdsp)
    {
        *outputdsp = connection->mOutputUnit;
    }

    crit.leave();

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: d6e6e8a0 auto
// OFFSET: 0x008DC370
FMOD_RESULT DSPI::getOutput(int index, DSPI **outputdsp)
{
    return getOutput(index, 0, outputdsp);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 379314dc auto
// OFFSET: 0x008DA4A0
FMOD_RESULT DSPI::execute(float *inbuffer, float **outbuffer, unsigned int *length, int inchannels, int *outchannels, FMOD_SPEAKERMODE speakermode)
{
    return FMOD_ERR_INTERNAL;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 33aa7b7d manual
// OFFSET: 0x008DA4B0
FMOD_RESULT DSPI::execute(void *inbuffer, void **outbuffer, unsigned int *length, int inchannels, int *outchannels, FMOD_SPEAKERMODE speakermode)
{
    return execute((float *)inbuffer, (float **)outbuffer, length, inchannels, outchannels, speakermode);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: c8b00d3a auto
// OFFSET: 0x008DA4C0
FMOD_RESULT DSPI::resetVisited()
{
    LinkedListNode *current;

    current = mInputHead.getNext();
    while (current != &mInputHead)
    {
        DSPConnectionI *connection = (DSPConnectionI *)current->getData();

        if (!connection)
        {
            break;
        }

        connection->mInputUnit->resetVisited();

        current = current->getNext();
    }

    mFlags &= ~FMOD_DSP_FLAG_VISITED;

    if (*mWantsToFinish)
    {
        mFlags |= FMOD_DSP_FLAG_FINISHED;
        *mWantsToFinish = 0;
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 978b605a auto
// OFFSET: 0x008DBE20
FMOD_RESULT DSPI::setFinished(bool finished, bool force)
{
    if (!finished)
    {
        *mWantsToFinish = 0;
        mFlags &= ~FMOD_DSP_FLAG_FINISHED;
    }

    if (force)
    {
        FMOD_OS_CriticalSection_Enter(mSystem->mDSPCrit);

        *mWantsToFinish = 0;

        if (finished)
        {
            mFlags |= FMOD_DSP_FLAG_FINISHED;
        }
        else
        {
            mFlags &= ~FMOD_DSP_FLAG_FINISHED;
        }

        FMOD_OS_CriticalSection_Leave(mSystem->mDSPCrit);
    }
    else
    {
        *mWantsToFinish = finished;
    }

    return FMOD_OK;
}


#ifndef FMOD_SUPPORT_MIXER_NONRECURSIVE

#endif

#ifdef FMOD_SUPPORT_MIXER_NONRECURSIVE

#ifdef PLATFORM_WINDOWS_PS3MODE

#endif


#endif


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

	[SEE_ALSO]
]
*/
// VERIFY: 7449cc04 auto
// OFFSET: 0x008DA520
FMOD_RESULT DSPI::validate(DSP *dsp, DSPI **dspi)
{
    if (!dspi)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (!dsp)
    {
        return FMOD_ERR_INVALID_HANDLE;
    }

    *dspi = (DSPI *)dsp;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: f19a7781 auto
// OFFSET: 0x008DC420
FMOD_RESULT DSPI::setPosition(unsigned int position, bool processinputs)
{
    if (processinputs)
    {
        FMOD_RESULT result;
        int numinputs, count;

        numinputs = 0;

        result = getNumInputs(&numinputs);
        if (result != FMOD_OK)
        {
            return result;
        }

        for (count = 0; count < numinputs; count++)
        {
            DSPI *input = 0;

            getInput(count, 0, &input);

            input->setPosition(position, true);
        }
    }

    if (mDescription.setposition)
    {
        instance = (FMOD_DSP *)this;

        return mDescription.setposition((FMOD_DSP_STATE *)this, position);
    }

    return FMOD_OK;
}

 
/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]
    
	[REMARKS]

    [PLATFORMS]
    
	[SEE_ALSO]
]
*/
// VERIFY: daa9ee82 manual
// OFFSET: 0x008DA550
FMOD_RESULT DSPI::calculateSpeakerLevels(float frontleft, float frontright, float center, float lfe, float backleft, float backright, float sideleft, float sideright, FMOD_SPEAKERMODE speakermode, int channels, FMOD_SPEAKERMAPTYPE speakermap, float *outlevels, int *numinputlevels)
{
    *numinputlevels = 0;

    switch (speakermode)
    {
        case FMOD_SPEAKERMODE_RAW:
        {
            switch (channels)
            {
                case 1:
                {
                    break;
                }
                case 2:
                {
                    break;
                }
                case 4:
                {
                    break;
                }
                case 6:
                {
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_MONO:
        {
            switch (channels)
            {
                case 1:
                {
                    float levels[1][1] = 
                    {
                        { FMOD_SQRT(frontleft*frontleft + center*center*0.5f + backleft*backleft * 0.25f)    },
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[1][2] = 
                    {   
                        { 
                            FMOD_SQRT(frontleft*frontleft + center*center*0.5f + backleft*backleft * 0.25f), 0,  
                        },
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 4:
                {
                    float levels[1][4] = 
                    {    
                        { frontleft,       0,                backleft*0.5f,    0                },    // FL
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[1][6] = 
                    {    
                        { frontleft,   frontright,  center,    lfe,   backleft,    backright }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 6;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_STEREO:
        {
            if (speakermap == FMOD_SPEAKERMAPTYPE_ALLMONO)
            {
                int count;

                FMOD_memset(outlevels, 0, 2 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT)  + count] = center * .707f;
                    outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = center * .707f;
                }

                *numinputlevels = channels;
                break;
            }
            else if (speakermap == FMOD_SPEAKERMAPTYPE_ALLSTEREO || channels > 8)
            {
                int count;

                FMOD_memset(outlevels, 0, 2 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    if (count & 1)
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = frontright;
                    }
                    else
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT) + count] = frontleft;
                    }
                }

                *numinputlevels = channels;
                break;
            }

            switch (channels)
            {
                case 1:
                {
                    float levels[2][1] = 
                    {
                        { FMOD_SQRT(frontleft*frontleft + center*center*0.5f + backleft*backleft * 0.25f)    },
                        { FMOD_SQRT(frontright*frontright + center*center*0.5f + backright*backright * 0.25f)  }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[2][2] = 
                    {   
                        { 
                            FMOD_SQRT(frontleft*frontleft + center*center*0.5f + backleft*backleft * 0.25f), 
                            0  
                        },
                        { 
                            0, 
                            FMOD_SQRT(frontright*frontright + center*center*0.5f + backright*backright * 0.25f)
                        }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 2;
                    break;
                }
                case 4:
                {
                    float levels[2][4] = 
                    {    
                        { frontleft,       0,                backleft*0.5f,    0                },    // FL
                        { 0,               frontright,       0,                backright*0.5f   },    // FR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[2][6] = 
                    {    /*  FL      FR          C              LFE         BL             BR             */
                        { frontleft, 0,          center * 0.5f, lfe * 0.5f, backleft*0.5f, 0              },    // FL
                        { 0,         frontright, center * 0.5f, lfe * 0.5f, 0,             backright*0.5f },    // FR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                
                    *numinputlevels = 6;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_QUAD:
        {
            if (speakermap == FMOD_SPEAKERMAPTYPE_ALLMONO)
            {
                int count;

                FMOD_memset(outlevels, 0, 4 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT)  + count] = center * .707f;
                    outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = center * .707f;
                }

                *numinputlevels = channels;
                break;
            }
            else if (speakermap == FMOD_SPEAKERMAPTYPE_ALLSTEREO || channels > 8)
            {
                int count;

                FMOD_memset(outlevels, 0, 4 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    if (count & 1)
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = frontright;
                    }
                    else
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT) + count] = frontleft;
                    }
                }

                *numinputlevels = channels;
                break;
            }

            switch (channels)
            {
                case 1:
                {
                    float levels[4][1] = 
                    {
                        { frontleft  + center },    /* FIX THIS - CLIPPING */
                        { frontright + center },    /* FIX THIS - CLIPPING */
                        { backleft            },
                        { backright           }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[4][2] = 
                    {    /*  left        right  */
                        { frontleft,     0          },
                        { 0,             frontright }, 
                        { backleft,      0          },
                        { 0,             backright  }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 2;
                    break;
                }
                case 4:
                {
                    float levels[4][4] = 
                    {    
                        { frontleft,       0,                0,               0               },    // FL
                        { 0,               frontright,       0,               0               },    // FR
                        { 0,               0,                backleft*0.5f,   0               },    // RL
                        { 0,               0,                0,               backright*0.5f  }     // RR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[4][6] = 
                    {    /*  FL            FR                C                 LFE               BL                BR       */
                        { frontleft,       0,                center * 0.5f,    lfe * 0.25f,      0,                0        },    // FL
                        { 0,               frontright,       center * 0.5f,    lfe * 0.25f,      0,                0        },    // FR
                        { 0,               0,                0,                lfe * 0.25f,      backleft,         0        },    // RL
                        { 0,               0,                0,                lfe * 0.25f,      0,                backright},    // RR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                
                    *numinputlevels = 6;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_SURROUND:
        {
            if (speakermap == FMOD_SPEAKERMAPTYPE_ALLMONO)
            {
                int count;

                FMOD_memset(outlevels, 0, 5 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    outlevels[(channels * FMOD_SPEAKER_FRONT_CENTER) + count] = center;
                }

                *numinputlevels = channels;
                break;
            }
            else if (speakermap == FMOD_SPEAKERMAPTYPE_ALLSTEREO || channels > 8)
            {
                int count;

                FMOD_memset(outlevels, 0, 5 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    if (count & 1)
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = frontright;
                    }
                    else
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT) + count] = frontleft;
                    }
                }

                *numinputlevels = channels;
                break;
            }

            switch (channels)
            {
                case 1:
                {
                    float levels[5][1] = 
                    {
                        { frontleft },
                        { frontright },
                        { center },
                        { backleft },
                        { backright }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[5][2] = 
                    {    /*  left          right  */
                        { frontleft,     0 },
                        { 0,             frontright }, 
                        { center * 0.5f, center * 0.5f },  /* This is a mono channel with both left and right going into it, so keep the volume consistent by multiplying by 0.5f. */
                        { backleft,      0 },
                        { 0,             backright },
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 2;
                    break;
                }
                case 4:
                {
                    float levels[5][4] = 
                    {   //   L             R                 C                 S                      
                        { frontleft,       0,                0,                0                },    // FL
                        { 0,               frontright,       0,                0                },    // FR
                        { center * 0.5f,   center * 0.5f,    0,                0                },    // C
                        { 0,               0,                backleft,         0                },    // RL
                        { 0,               0,                0,                backright        }     // RR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[5][6] = 
                    {    /*  FL            FR                C                 LFE               BL                BR               */
                        { frontleft,       0,                0,                0,                0,                0          },    // FL
                        { 0,               frontright,       0,                0,                0,                0          },    // FR
                        { 0,               0,                center,           0,                0,                0          },    // C
                        { 0,               0,                0,                0,                backleft,         0          },    // RL
                        { 0,               0,                0,                0,                0,                backright  }     // RR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                
                    *numinputlevels = 6;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_5POINT1:
        {
            if (speakermap == FMOD_SPEAKERMAPTYPE_ALLMONO)
            {
                int count;

                FMOD_memset(outlevels, 0, 6 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    outlevels[(channels * FMOD_SPEAKER_FRONT_CENTER) + count] = center;
                }

                *numinputlevels = channels;
                break;
            }
            else if (speakermap == FMOD_SPEAKERMAPTYPE_ALLSTEREO || channels > 8)
            {
                int count;

                FMOD_memset(outlevels, 0, 6 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    if (count & 1)
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = frontright;
                    }
                    else
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT) + count] = frontleft;
                    }
                }

                *numinputlevels = channels;
                break;
            }

            switch (channels)
            {
                case 1:
                {
                    float levels[6][1] = 
                    {
                        { frontleft },
                        { frontright },
                        { center },
                        { lfe },
                        { backleft },
                        { backright }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[6][2] = 
                    {    /*  left        right  */
                        { frontleft,     0 },
                        { 0,             frontright }, 
                        { center * 0.5f, center * 0.5f },  /* This is a mono channel with both left and right going into it, so keep the volume consistent by multiplying by 0.5f. */
                        { lfe    * 0.5f, lfe    * 0.5f },  /* This is a mono channel with both left and right going into it, so keep the volume consistent by multiplying by 0.5f. */
                        { backleft,      0 },
                        { 0,             backright },
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 2;
                    break;
                }
                case 4:
                {
                    float levels[6][4] = 
                    {    
                        { frontleft,       0,                0,                0                },    // FL
                        { 0,               frontright,       0,                0                },    // FR
                        { center * 0.5f,   center * 0.5f,    0,                0                },    // C
                        { lfe    * 0.5f,   lfe    * 0.5f,    0,                0                },    // LFE
                        { 0,               0,                backleft,         0                },    // RL
                        { 0,               0,                0,                backright        }     // RR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[6][6] = 
                    {    /*  FL            FR                C                 LFE               BL                BR         */
                        { frontleft,       0,                0,                0,                0,                0          },    // FL
                        { 0,               frontright,       0,                0,                0,                0          },    // FR
                        { 0,               0,                center,           0,                0,                0          },    // C
                        { 0,               0,                0,                lfe,              0,                0          },    // LFE
                        { 0,               0,                0,                0,                backleft,         0          },    // RL
                        { 0,               0,                0,                0,                0,                backright  }     // RR
                    };
                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                
                    *numinputlevels = 6;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_7POINT1:
        {
            if (speakermap == FMOD_SPEAKERMAPTYPE_ALLMONO)
            {
                int count;

                FMOD_memset(outlevels, 0, 8 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    outlevels[(channels * FMOD_SPEAKER_FRONT_CENTER) + count] = center;
                }

                *numinputlevels = channels;
                break;
            }
            else if (speakermap == FMOD_SPEAKERMAPTYPE_ALLSTEREO || channels > 8)
            {
                int count;

                FMOD_memset(outlevels, 0, 8 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    if (count & 1)
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = frontright;
                    }
                    else
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT) + count] = frontleft;
                    }
                }

                *numinputlevels = channels;
                break;
            }

            switch (channels)
            {
                case 1:
                {
                    float levels[8][1] = 
                    {
                        { frontleft },
                        { frontright },
                        { center },
                        { lfe },
                        { backleft },
                        { backright },
                        { sideleft },
                        { sideright }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[8][2] = 
                    {    /*  left        right  */
                        { frontleft,     0 },
                        { 0,             frontright }, 
                        { center * 0.5f, center * 0.5f },  /* This is a mono channel with both left and right going into it, so keep the volume consistent by multiplying by 0.5f. */
                        { lfe    * 0.5f, lfe    * 0.5f },  /* This is a mono channel with both left and right going into it, so keep the volume consistent by multiplying by 0.5f. */
                        { backleft,      0 },
                        { 0,             backright },
                        { sideleft,      0 },
                        { 0,             sideright }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 2;
                    break;
                }
                case 4:
                {
                    float levels[8][4] = 
                    {   //  FL            FR                RL                RR              
                        { frontleft,       0,                0,                0                },    // FL
                        { 0,               frontright,       0,                0                },    // FR
                        { center * 0.5f,   center * 0.5f,    0,                0                },    // C
                        { lfe    * 0.5f,   lfe    * 0.5f,    0,                0                },    // LFE
                        { 0,               0,                backleft*0.5f,    0                },    // RL
                        { 0,               0,                0,                backright*0.5f   },    // RR
                        { 0,               0,                sideleft*0.5f,    0                },    // SL
                        { 0,               0,                0,                sideright*0.5f   },    // SR
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[8][6] = 
                    {    /*  FL            FR                C                 LFE               BL                BR               */
                        { frontleft,       0,                0,                0,                0,                0                },    // FL
                        { 0,               frontright,       0,                0,                0,                0                },    // FR
                        { 0,               0,                center,           0,                0,                0                },    // C
                        { 0,               0,                0,                lfe,              0,                0                },    // LFE
                        { 0,               0,                0,                0,                backleft,         0                },    // RL
                        { 0,               0,                0,                0,                0,                backright        },    // RR
                        { sideleft * 0.5f, 0,                0,                0,                sideleft * 0.5f,  0                },    // SL
                        { 0,               sideright * 0.5f, 0,                0,                0,                sideleft * 0.5f  },    // SR
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                
                    *numinputlevels = 6;
                    break;
                }
                case 8:
                {
                    float levels[8][8] = 
                    {    /*  FL            FR                C                 LFE               BL                BR               SL              SR          */
                        { frontleft,       0,                0,                0,                0,                0,               0,              0           },    // FL
                        { 0,               frontright,       0,                0,                0,                0,               0,              0           },    // FR
                        { 0,               0,                center,           0,                0,                0,               0,              0           },    // C
                        { 0,               0,                0,                lfe,              0,                0,               0,              0           },    // LFE
                        { 0,               0,                0,                0,                backleft,         0,               0,              0           },    // RL
                        { 0,               0,                0,                0,                0,                backright,       0,              0           },    // RR
                        { 0,               0,                0,                0,                0,                0,               sideleft,       0           },    // SL
                        { 0,               0,                0,                0,                0,                0,               0,              sideright   },    // SR
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 8;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        case FMOD_SPEAKERMODE_PROLOGIC:
        {
            if (speakermap == FMOD_SPEAKERMAPTYPE_ALLMONO)
            {
                int count;

                FMOD_memset(outlevels, 0, 2 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT)  + count] = center * .707f;
                    outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = center * .707f;
                }

                *numinputlevels = channels;
                break;
            }
            else if (speakermap == FMOD_SPEAKERMAPTYPE_ALLSTEREO || channels > 8)
            {
                int count;

                FMOD_memset(outlevels, 0, 2 * channels * sizeof(float));

                for (count = 0; count < channels; count++)
                {
                    if (count & 1)
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_RIGHT) + count] = frontright;
                    }
                    else
                    {
                        outlevels[(channels * FMOD_SPEAKER_FRONT_LEFT) + count] = frontleft;
                    }
                }

                *numinputlevels = channels;
                break;
            }

            /*
                Square the input levels to conform with Pro Logic linear gain  
            */
            float fl2 = frontleft * frontleft;
            float fr2 = frontright * frontright;
            float c2 = center * center;
            float bl2 = backleft * backleft;
            float br2 = backright * backright;

            switch (channels)
            {
                case 1:
                {              
                    float levels[2] = 
                    {
                        fl2 + (c2 *0.707f) + (-0.872f * bl2) + (-0.49f * br2),
                        fr2 + (c2 *0.707f) + (0.49f   * bl2) + (0.872f * br2)
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 1;
                    break;
                }
                case 2:
                {
                    float levels[2][2] = 
                    {
                        { fl2 + (c2 *0.707f) + (-0.872f * bl2) + (-0.49f * br2), fl2 + (c2 *0.707f) + (-0.872f * bl2) + (-0.49f * br2) },
                        { fr2 + (c2 *0.707f) + (0.49f   * bl2) + (0.872f * br2), fl2 + (c2 *0.707f) + (0.49f   * bl2) + (0.872f * br2) }
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 2;
                    break;
                }
                case 4:
                {
                    float levels[2][4] = 
                    {    
                        { fl2,             0,         0.5f * bl2,           0          },    // FL
                        { 0,               fr2,       0              ,      0.5f * br2 }     // FR
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                    *numinputlevels = 4;
                    break;
                }
                case 6:
                {
                    float levels[2][6] = 
                    {    /*  FL            FR           C                 LFE               BL                BR       */
                        { fl2,             0,           c2 * 0.5f,        lfe * 0.5f,       bl2,              0},      // FL
                        { 0,               fr2,         c2 * 0.5f,        lfe * 0.5f,       0,                br2},    // FR
                    };

                    FMOD_memcpy(outlevels, levels, sizeof(levels));
                
                    *numinputlevels = 6;
                    break;
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        default:
        {
            break;
        }
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: fd77a6ac manual
// OFFSET: 0x008DC4A0
DSPI::DSPI()
{
    mBuffer             = 0;
    mBufferMemory       = 0;
    mFlags              = 0;
    mWantsToFinish      = &mWantsToFinishMem;
    *mWantsToFinish     = 0;

    mNumInputs          = 0;
    mNumOutputs         = 0;
    mTreeLevel          = -1;

    mDefaultVolume      = 1.0f;
    mDefaultFrequency   = DEFAULT_FREQUENCY;
    mDefaultPan         = 0.0f;
    mDefaultPriority    = FMOD_CHANNEL_DEFAULTPRIORITY;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 1abf99a8 manual
// OFFSET: 0x008DC540
FMOD_RESULT DSPI::release(bool freethis)
{
    FMOD_RESULT result;

    if (mSystem)
    {
        result = mSystem->stopDSP(this);
        if (result != FMOD_OK)
        {
            return result;
        }
    }

    result = remove();
    if (result != FMOD_OK)
    {
        result = disconnectFromQueued(0);
        if (result != FMOD_OK)
        {
            return result;
        }
    }

    mSystem->flushDSPConnectionRequests();  /* If we're about to free DSP memory, dont leave any in queue! */

    if (mBufferMemory)
    {
        FMOD_Memory_Free(mBufferMemory);
        mBufferMemory = 0;
    }

    removeNode();

    if (mDescription.release)
    {
        instance = (FMOD_DSP *)this;
        mDescription.release((FMOD_DSP_STATE *)this);
    }

    if (freethis)
    {
        FMOD_Memory_Free(this);
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::getSystemObject(System **system)
{
    if (!system)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *system = (System *)mSystem;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 9539ce6d manual
// OFFSET: 0x008DB720
FMOD_RESULT DSPI::updateTreeLevel(int level)
{
    LinkedListNode *current;

    if (mNumOutputs > 1 && level < mTreeLevel)  /* Don't make it move backwards if it is already connected to something else that is deeper. */
    {
        return FMOD_OK;
    }

    mTreeLevel = level;

    if ((!mBufferMemory || (FMOD_UINT_NATIVE)mBuffer != FMOD_ALIGNPOINTER(mBufferMemory, 16)) && level > 0)
    {
        if (!mSystem->mDSPMixBuff[level - 1])
        {
            mSystem->mDSPMixBuff[level - 1] = (float *)FMOD_Memory_Calloc((mSystem->mDSPBlockSize * (mSystem->mMaxOutputChannels < mSystem->mMaxInputChannels ? mSystem->mMaxInputChannels : mSystem->mMaxOutputChannels) * sizeof(float)) + 16);
            if (!mSystem->mDSPMixBuff[level - 1])
            {
                return FMOD_ERR_MEMORY;
            }
        }
    }

    current = mInputHead.getNext();
    while (current != &mInputHead)
    {
        DSPConnectionI *connection = (DSPConnectionI *)current->getData();

        connection->mInputUnit->updateTreeLevel(mTreeLevel + 1);

        current = current->getNext();
    }

    if (!mBufferMemory || (FMOD_UINT_NATIVE)mBuffer != FMOD_ALIGNPOINTER(mBufferMemory, 16))
    {
        mBuffer = (float *)FMOD_ALIGNPOINTER(mSystem->mDSPMixBuff[mTreeLevel], 16);
    }

    return FMOD_OK;
}



#ifndef PLATFORM_PS3_SPU
#endif



/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/

// VERIFY: f87f4dc3 manual
// OFFSET: 0x008DC600
FMOD_RESULT DSPI::addInputInternal(DSPI *target, bool checkcircular, DSPConnectionI *connection, DSPConnectionI **connection_out)
{
    FMOD_RESULT result;
    LocalCriticalSection crit(mSystem->mDSPCrit);
    LocalCriticalSection crit2(mSystem->mDSPConnectionCrit);
    bool allocated = false;

    if (!target)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if ((mDescription.mCategory == FMOD_DSP_CATEGORY_RESAMPLER && target->mNumOutputs) || target->mDescription.mCategory == FMOD_DSP_CATEGORY_SOUNDCARD)
    {
        return FMOD_ERR_DSP_CONNECTION;
    }

    if (checkcircular)
    {
        if (target->doesUnitExist(this) == FMOD_OK)
        {
            return FMOD_ERR_DSP_CONNECTION;
        }
    }

    crit.enter();
    crit2.enter();

    if (!connection)
    {
        result = mSystem->mDSPConnectionPool.alloc(&connection);
        if (result != FMOD_OK)
        {
            return result;
        }

        allocated = true;
    }

    connection->mInputNode.addBefore(&mInputHead);
    mNumInputs++;

    connection->mOutputNode.addBefore(&target->mOutputHead);
    target->mNumOutputs++;

    connection->mInputUnit  = target;
    connection->mOutputUnit = this;

    if (allocated)
    {
        connection->reset();
    }

    if (mTreeLevel >= 0)
    {
        target->updateTreeLevel(mTreeLevel + 1);
    }

    if (!mBufferMemory)
    {
        mBuffer = (float *)FMOD_ALIGNPOINTER(mSystem->mDSPMixBuff[mTreeLevel], 16);
    }

    if (target->mNumOutputs > 1)
    {
        if (!target->mBufferMemory)
        {
            target->mBufferMemory = (float *)FMOD_Memory_Calloc((mSystem->mDSPBlockSize * (mSystem->mMaxOutputChannels < mSystem->mMaxInputChannels ? mSystem->mMaxInputChannels : mSystem->mMaxOutputChannels) * sizeof(float)) + 15);
            if (!target->mBufferMemory)
            {
                return FMOD_ERR_MEMORY;
            }
        }
        target->mBuffer = (float *)FMOD_ALIGNPOINTER(target->mBufferMemory, 16);
    }

    crit.leave();
    crit2.leave();

    if (connection_out)
    {
        *connection_out = connection;
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 137a7e5e manual
// OFFSET: 0x008DCC10
FMOD_RESULT DSPI::addInput(DSPI *target)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;
    DSPConnectionRequest *request;

    if (!mSystem)
    {
        return FMOD_ERR_UNINITIALIZED;
    }

    if (!target)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if ((mDescription.mCategory == FMOD_DSP_CATEGORY_RESAMPLER && target->mNumOutputs) || target->mDescription.mCategory == FMOD_DSP_CATEGORY_SOUNDCARD)
    {
        return FMOD_ERR_DSP_CONNECTION;
    }

    if (target->doesUnitExist(this) == FMOD_OK)
    {
        return FMOD_ERR_DSP_CONNECTION;
    }

    result = mSystem->mDSPConnectionPool.alloc(&connection);
    if (result != FMOD_OK)
    {
        return result;
    }

    connection->reset();

    FMOD_OS_CriticalSection_Enter(mSystem->mDSPRequestCrit);
    {
        if (mSystem->mConnectionRequestFreeHead.isEmpty())
        {
            mSystem->flushDSPConnectionRequests();
        }

        request = (DSPConnectionRequest *)mSystem->mConnectionRequestFreeHead.getNext();
        request->removeNode();
        request->addBefore(&mSystem->mConnectionRequestUsedHead);

        request->mThis       = this;
        request->mTarget     = target;
        request->mConnection = connection;
        request->mRequest    = DSPCONNECTION_REQUEST_ADDINPUT;
    }
    FMOD_OS_CriticalSection_Leave(mSystem->mDSPRequestCrit);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: b21922eb manual
// OFFSET: 0x008DC820
FMOD_RESULT DSPI::addInputQueued(DSPI *target, bool checkcircular, DSPConnectionI *connection_old, DSPConnectionI **connection_out)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;
    DSPConnectionRequest *request;

    if (!target)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if ((mDescription.mCategory == FMOD_DSP_CATEGORY_RESAMPLER && target->mNumOutputs) || target->mDescription.mCategory == FMOD_DSP_CATEGORY_SOUNDCARD)
    {
        return FMOD_ERR_DSP_CONNECTION;
    }

    if (checkcircular)
    {
        if (target->doesUnitExist(this) == FMOD_OK)
        {
            return FMOD_ERR_DSP_CONNECTION;
        }
    }

    result = mSystem->mDSPConnectionPool.alloc(&connection);
    if (result != FMOD_OK)
    {
        return result;
    }

    if (connection_old)
    {
        connection->copy(connection_old);
    }
    else
    {
        connection->reset();
    }

    FMOD_OS_CriticalSection_Enter(mSystem->mDSPRequestCrit);
    {
        if (mSystem->mConnectionRequestFreeHead.isEmpty())
        {
            mSystem->flushDSPConnectionRequests();
        }

        request = (DSPConnectionRequest *)mSystem->mConnectionRequestFreeHead.getNext();
        request->removeNode();
        request->addBefore(&mSystem->mConnectionRequestUsedHead);

        request->mThis       = this;
        request->mTarget     = target;
        request->mConnection = connection;
        request->mRequest    = DSPCONNECTION_REQUEST_ADDINPUT;
    }
    FMOD_OS_CriticalSection_Leave(mSystem->mDSPRequestCrit);

    if (connection_out)
    {
        *connection_out = connection;
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 6c3ec849 manual
// OFFSET: 0x008DC030
FMOD_RESULT DSPI::disconnectFromInternal(DSPI *target)
{
    FMOD_RESULT result;
    FMOD_OS_CRITICALSECTION *dspcrit = mSystem->mDSPCrit;
    FMOD_OS_CRITICALSECTION *dspconnectioncrit = mSystem->mDSPConnectionCrit;

    if (!target)
    {
        int numinputs = 0, numoutputs = 0;

        getNumInputs(&numinputs);
        while (numinputs)
        {
            DSPI *input;

            result = getInput(0, 0, &input);
            if (result != FMOD_OK)
            {
                return result;
            }

            disconnectFromInternal(input);

            getNumInputs(&numinputs);
        }

        getNumOutputs(&numoutputs);
        while (numoutputs)
        {
            DSPI *output;

            result = getOutput(0, 0, &output);
            if (result != FMOD_OK)
            {
                return result;
            }

            output->disconnectFromInternal(this);

            getNumOutputs(&numoutputs);
        }

        return FMOD_OK;
    }
    else
    {
        int count;
        DSPConnectionI *connection = 0;

        // Not stock: DSP crit first, then connection crit, the order addInputInternal and the mixer use; stock took
        // them the other way round (lock-order inversion, deadlock between the mixer and a disconnect).
        FMOD_OS_CriticalSection_Enter(dspcrit);
        FMOD_OS_CriticalSection_Enter(dspconnectioncrit);

        for (count = 0; count < mNumInputs; count++)
        {
            result = getInput(count, &connection, 0);
            if (result != FMOD_OK)
            {
                FMOD_OS_CriticalSection_Leave(dspconnectioncrit);
                FMOD_OS_CriticalSection_Leave(dspcrit);
                return result;
            }

            if (connection->mInputUnit == target)
            {
                break;
            }
        }

        if (count == mNumInputs)
        {
            FMOD_OS_CriticalSection_Leave(dspcrit);
            FMOD_OS_CriticalSection_Leave(dspconnectioncrit);
            return FMOD_ERR_DSP_NOTFOUND;
        }

        connection->mInputNode.removeNode();
        mNumInputs--;

        if (mBufferMemory && mNumOutputs <= 1)
        {
            FMOD_Memory_Free(mBufferMemory);
            mBufferMemory = 0;
        }

        connection->mOutputNode.removeNode();
        target->mNumOutputs--;

        if (*mWantsToFinish)
        {
            mFlags |= FMOD_DSP_FLAG_FINISHED;
            *mWantsToFinish = 0;
        }
        if (*target->mWantsToFinish)
        {
            target->mFlags |= FMOD_DSP_FLAG_FINISHED;
            *target->mWantsToFinish = 0;
        }

        result = mSystem->mDSPConnectionPool.free(connection);
        if (result != FMOD_OK)
        {
            FMOD_OS_CriticalSection_Leave(dspconnectioncrit);
            FMOD_OS_CriticalSection_Leave(dspcrit);
            return result;
        }

        FMOD_OS_CriticalSection_Leave(dspconnectioncrit);
        FMOD_OS_CriticalSection_Leave(dspcrit);
    }

    return FMOD_OK;
}



/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 61ccedfa manual
// OFFSET: 0x008DB830
FMOD_RESULT DSPI::disconnectFromQueued(DSPI *target)
{
    FMOD_OS_CriticalSection_Enter(mSystem->mDSPRequestCrit);
    {
        DSPConnectionRequest *request;

        if (mSystem->mConnectionRequestFreeHead.isEmpty())
        {
            mSystem->flushDSPConnectionRequests();
        }

        request = (DSPConnectionRequest *)mSystem->mConnectionRequestFreeHead.getNext();
        request->removeNode();
        request->addBefore(&mSystem->mConnectionRequestUsedHead);

        request->mThis    = this;
        request->mTarget  = target;
        request->mRequest = DSPCONNECTION_REQUEST_DISCONNECTFROM;
    }
    FMOD_OS_CriticalSection_Leave(mSystem->mDSPRequestCrit);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: b9d22179 auto
// OFFSET: 0x008DC330
FMOD_RESULT DSPI::disconnectFrom(DSPI *target)
{
    return disconnectFromQueued(target);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 0f27bba7 auto
// OFFSET: 0x008DC950
FMOD_RESULT DSPI::disconnectAllInternal(bool inputs, bool outputs)
{
    FMOD_RESULT result;
    int count;

    if (inputs)
    {
        int numinputs;

        result = getNumInputs(&numinputs);
        if (result != FMOD_OK)
        {
            return result;
        }

        for (count = 0; count < numinputs; count++)
        {
            DSPI *input;

            result = getInput(0, 0, &input);
            if (result != FMOD_OK)
            {
                return result;
            }

            result = disconnectFromInternal(input);
            if (result != FMOD_OK)
            {
                return result;
            }
        }
    }

    if (outputs)
    {
        int numoutputs;

        result = getNumOutputs(&numoutputs);
        if (result != FMOD_OK)
        {
            return result;
        }

        for (count = 0; count < numoutputs; count++)
        {
            DSPI *output;

            result = getOutput(0, 0, &output);
            if (result != FMOD_OK)
            {
                return result;
            }

            result = output->disconnectFromInternal(this);
            if (result != FMOD_OK)
            {
                return result;
            }
        }
    }

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 2917f03c manual
// OFFSET: 0x008DB8D0
FMOD_RESULT DSPI::disconnectAllQueued(bool inputs, bool outputs)
{
    if (!inputs && !outputs)
    {
        return FMOD_OK;
    }

    FMOD_OS_CriticalSection_Enter(mSystem->mDSPRequestCrit);
    {
        DSPConnectionRequest *request;

        if (mSystem->mConnectionRequestFreeHead.isEmpty())
        {
            mSystem->flushDSPConnectionRequests();
        }

        request = (DSPConnectionRequest *)mSystem->mConnectionRequestFreeHead.getNext();
        request->removeNode();
        request->addBefore(&mSystem->mConnectionRequestUsedHead);

        request->mThis    = this;
        request->mTarget  = 0;

        if (inputs && outputs)
        {
            request->mRequest = DSPCONNECTION_REQUEST_DISCONNECTALL;
        }
        else if (inputs)
        {
            request->mRequest = DSPCONNECTION_REQUEST_DISCONNECTALLINPUTS;
        }
        else
        {
            request->mRequest = DSPCONNECTION_REQUEST_DISCONNECTALLOUTPUTS;
        }
    }
    FMOD_OS_CriticalSection_Leave(mSystem->mDSPRequestCrit);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]

	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 28c096c9 auto
// OFFSET: 0x008DC340
FMOD_RESULT DSPI::disconnectAll(bool inputs, bool outputs)
{
    return disconnectAllQueued(inputs, outputs);
}





/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: a50253d1 auto
// OFFSET: 0x008DCAC0
FMOD_RESULT DSPI::remove()
{
    FMOD_RESULT result;
    int numinputs, numoutputs;

    getNumInputs(&numinputs);

    result = getNumOutputs(&numoutputs);
    if (result != FMOD_OK)
    {
        return result;
    }

    if (numinputs == 1 && numoutputs == 1)
    {
        DSPI *input, *output;

        mFlags &= ~FMOD_DSP_FLAG_ACTIVE;

        result = getInput(0, 0, &input);
        if (result != FMOD_OK)
        {
            return result;
        }

        result = getOutput(0, 0, &output);
        if (result != FMOD_OK)
        {
            return result;
        }

        result = disconnectFromQueued(0);
        if (result != FMOD_OK)
        {
            return result;
        }

        result = output->addInput(input);
        if (result != FMOD_OK)
        {
            return result;
        }
    }
    else
    {
        mFlags &= ~FMOD_DSP_FLAG_USEDADDDSP;

        return disconnectFromQueued(0);
    }

    mFlags &= ~FMOD_DSP_FLAG_USEDADDDSP;

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: c26fc1e3 auto
// OFFSET: 0x008DB990
FMOD_RESULT DSPI::getNumInputs(int *numinputs)
{
    if (!numinputs)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    mSystem->flushDSPConnectionRequests();

    FMOD_OS_CriticalSection_Enter(mSystem->mDSPConnectionCrit);
    *numinputs = mNumInputs;
    FMOD_OS_CriticalSection_Leave(mSystem->mDSPConnectionCrit);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/

// VERIFY: 3b712902 auto
// OFFSET: 0x008DB9E0
FMOD_RESULT DSPI::getNumOutputs(int *numoutputs)
{
    if (!numoutputs)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    mSystem->flushDSPConnectionRequests();

    FMOD_OS_CriticalSection_Enter(mSystem->mDSPConnectionCrit);
    *numoutputs = mNumOutputs;
    FMOD_OS_CriticalSection_Leave(mSystem->mDSPConnectionCrit);

    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: f6ff7c02 auto
// OFFSET: 0x008DBA30
FMOD_RESULT DSPI::reset()
{
#ifdef FMOD_SUPPORT_SOFTWARE
#ifndef PLATFORM_PS3_SPU
    if (!mDescription.reset)
    {
        return FMOD_ERR_UNSUPPORTED;
    }

    instance = (FMOD_DSP *)this;

    return mDescription.reset((FMOD_DSP_STATE *)this);
#else
    return FMOD_ERR_INTERNAL;
#endif  
#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 62f9d92b auto
// OFFSET: 0x008DBA50
FMOD_RESULT DSPI::setParameter(int index, float value)
{
#ifdef FMOD_SUPPORT_SOFTWARE
#ifndef PLATFORM_PS3_SPU
    FMOD_RESULT result;

    if (!mDescription.setparameter)
    {
        return FMOD_ERR_UNSUPPORTED;
    }

    if (index < 0 || index > mDescription.numparameters)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    #ifdef FMOD_DEBUG
    
    result = FMOD_CHECKFLOAT(value);
    if (result != FMOD_OK)
    {
        return result;
    }
    
    #endif

    if (value < mDescription.paramdesc[index].min)
    {
        value = mDescription.paramdesc[index].min;
    }
    if (value > mDescription.paramdesc[index].max)
    {
        value = mDescription.paramdesc[index].max;
    }

    instance = (FMOD_DSP *)this;

    result = mDescription.setparameter((FMOD_DSP_STATE *)this, index, value);
    if (result != FMOD_OK)
    {
        return result;
    }
#endif
    return FMOD_OK;
#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: e332c590 auto
// OFFSET: 0x008DBAD0
FMOD_RESULT DSPI::getParameter(int index, float *value, char *valuestr, int valuestrlen)
{
#ifdef FMOD_SUPPORT_SOFTWARE
#ifndef PLATFORM_PS3_SPU
    FMOD_RESULT result;
    float   v;
    char    s[32];

    if (!mDescription.getparameter)
    {
        return FMOD_ERR_UNSUPPORTED;
    }

    if (index < 0 || index > mDescription.numparameters)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    instance = (FMOD_DSP *)this;

    result = mDescription.getparameter((FMOD_DSP_STATE *)this, index, &v, s);
    if (result != FMOD_OK)
    {
        return result;
    }

    if (value)
    {
        *value = v;
    }

    if (valuestr)
    {
        FMOD_strncpy(valuestr, s, valuestrlen > 16 ? 16 : valuestrlen);
    }
#endif
    return FMOD_OK;
#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::getNumParameters(int *numparams)
{
#ifndef PLATFORM_PS3_SPU
    if (!numparams)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *numparams = mDescription.numparameters;
#endif
    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 6d6faa71 auto
// OFFSET: 0x008DBB60
FMOD_RESULT DSPI::getParameterInfo(int index, char *name, char *label, char *description, int descriptionlen, float *min, float *max)
{
#ifdef FMOD_SUPPORT_SOFTWARE
#ifndef PLATFORM_PS3_SPU
    if (index < 0 || index >= mDescription.numparameters)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (name)
    {
        FMOD_strcpy(name, mDescription.paramdesc[index].name);
    }
    if (description && descriptionlen)
    {
        if (mDescription.paramdesc[index].description)
        {
            FMOD_strncpy(description, mDescription.paramdesc[index].description, descriptionlen);
        }
        else
        {
            *description = 0;
        }
    }
    if (label)
    {
        FMOD_strcpy(label, mDescription.paramdesc[index].label);
    }
    if (min)
    {
        *min = mDescription.paramdesc[index].min;
    }
    if (max)
    {
        *max = mDescription.paramdesc[index].max;
    }
#endif
    return FMOD_OK;
#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}

/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: a520ca60 auto
// OFFSET: 0x008DBC40
FMOD_RESULT DSPI::showConfigDialog(void *hwnd, bool show)
{
#ifdef FMOD_SUPPORT_SOFTWARE

    #ifndef PLATFROM_PS3_SPU

    if (!mDescription.config)
    {
        return FMOD_ERR_UNSUPPORTED;
    }

    instance = (FMOD_DSP *)this;

    return mDescription.config((FMOD_DSP_STATE *)this, hwnd, show);
    
    #else
    
    return FMOD_ERR_INTERNAL;
    
    #endif
#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: c6fb155b auto
// OFFSET: 0x008DBC70
FMOD_RESULT DSPI::getInfo(char *name, unsigned int *version, int *channels, int *configwidth, int *configheight)
{
#ifdef FMOD_SUPPORT_SOFTWARE
    
    #ifndef PLATFORM_PS3_SPU
    
    if (name)
    {
        FMOD_strncpy(name, mDescription.name, 32);
    }

    if (version)
    {
        *version = mDescription.version;
    }

    if (channels)
    {
        *channels = mDescription.channels;
    }

    if (configwidth)
    {
        *configwidth = mDescription.configwidth;
    }

    if (configheight)
    {
        *configheight = mDescription.configheight;
    }
    
    #endif
    
    return FMOD_OK;

#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}

/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: a6efdf8a auto
// OFFSET: 0x008DBCD0
FMOD_RESULT DSPI::getType(FMOD_DSP_TYPE *type)
{
    if (type)
    {
        *type = mDescription.mType;
    }

    return FMOD_OK;
}

/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 8ac4d762 manual
// OFFSET: 0x008DBCF0
FMOD_RESULT DSPI::setDefaults(float frequency, float volume, float pan, int priority)
{
#ifdef FMOD_SUPPORT_SOFTWARE
    
    #ifndef PLATFORM_PS3_SPU
    
    if (volume > 1)
    {
        volume = 1;
    }
    if (volume < 0)
    {
        volume = 0;
    }
    if (pan < -1)
    {
        pan = -1;
    }
    if (pan > 1)
    {
        pan = 1;
    }
    if (priority < 0)
    {
        priority = 0;
    }
    if (priority > 256)
    {
        priority = 256;
    }

    mDefaultFrequency = frequency;
    mDefaultVolume    = volume;
    mDefaultPan       = pan;
    mDefaultPriority  = priority;
    
    #endif
    
    return FMOD_OK;

#else

    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 0bf2c3e3 auto
// OFFSET: 0x008DBD80
FMOD_RESULT DSPI::getDefaults(float *frequency, float *volume, float *pan, int *priority)
{
#ifdef FMOD_SUPPORT_SOFTWARE
    
    #ifndef PLATFORM_PS3_SPU
    
    if (frequency)
    {
        *frequency = mDefaultFrequency;
    }
    if (volume)
    {
        *volume = mDefaultVolume;
    }
    if (pan)
    {
        *pan = mDefaultPan;
    }
    if (priority)
    {
        *priority = mDefaultPriority;
    }
    
    #endif
    
    return FMOD_OK;

#else
    return FMOD_ERR_NEEDSSOFTWARE;
#endif
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 37835d2d auto
// OFFSET: 0x008DBDD0
FMOD_RESULT DSPI::setUserData(void *userdata)
{
#ifndef PLATFORM_PS3_SPU
    mDescription.userdata = userdata;
#endif    
    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: d9add7fa auto
// OFFSET: 0x008DBDF0
FMOD_RESULT DSPI::getUserData(void **userdata)
{
#ifndef PLATFORM_PS3_SPU
    if (!userdata)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *userdata = mDescription.userdata;
#endif
    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 33a02789 auto
// OFFSET: 0x008DBEA0
FMOD_RESULT DSPI::setTargetFrequency(int frequency)
{
#ifndef PLATFORM_PS3_SPU
    mTargetFrequency = frequency;
#endif
    return FMOD_OK;
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
// VERIFY: 65b83dd4 auto
// OFFSET: 0x008DBEB0
FMOD_RESULT DSPI::getTargetFrequency(int *frequency)
{
#ifndef PLATFORM_PS3_SPU
    if (!frequency)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *frequency = mTargetFrequency;
#endif
    return FMOD_OK;
}





/*

*/
/*
[API]
[
	[DESCRIPTION]
    This callback is called once when a user creates a DSP unit of this type.  It is used to allocate memory, initialize variables and the like.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    System::createDSP<br>
    System::createDSPByType<br>
    System::createDSPByPlugin<br>
    Sometimes a user will re-use a DSP unit instead of releasing it and creating a new one, so it may be useful to implement FMOD_DSP_RESETCALLBACK to reset any variables or buffers when the user calls it.<br>
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    System::createDSP
    System::createDSPByType
    System::createDSPByPlugin
    FMOD_DSP_RESETCALLBACK
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_CREATECALLBACK(FMOD_DSP_STATE *dsp_state);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    This callback is called when the user releases the DSP unit.  It is used to free any resources allocated during the course of the lifetime of the DSP or perform any shut down code needed to clean up the DSP unit.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    DSP::release<br>
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    DSP::release
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_RELEASECALLBACK(FMOD_DSP_STATE *dsp_state);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    This callback function is called by DSP::reset to allow the effect to reset itself to a default state.<br>
    This is useful if an effect is for example still holding audio data for a sound that has stopped, and the unit wants to be relocated to a new sound.  Resetting the unit would clear any buffers, put the effect back to its initial state, and get it ready for new sound data.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    DSP::reset<br>
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    DSP::reset
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_RESETCALLBACK(FMOD_DSP_STATE *dsp_state);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    This callback is called back regularly when the unit has been created, inserted to the DSP network, and set to active by the user.<br>
    This callback requires the user to fill the output pointer with data.  Incoming data is provided and may be filtered on its way to the output pointer.<br>

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.
    'inbuffer'      Pointer to incoming floating point -1.0 to +1.0 ranged data.
    'outbuffer'     Pointer to outgoing floating point -1.0 to +1.0 ranged data.  The dsp writer must write to this pointer else there will be silence.
    'length'        The length of the incoming and outgoing buffer in samples.  To get the length of the buffer in bytes, the user must multiply this number by the number of channels coming in (and out, they may be different) and then multiply by 4 for 1 float = 4 bytes.
    'inchannels'    The number of channels of interleaved PCM data in the inbuffer parameter.  A mono signal coming in would be 1.  A stereo signal coming in would be 2.
    'outchannels'   The number of channels of interleaved PCM data in the outbuffer parameter.  A mono signal going out would be 1.  A stereo signal going out would be 2.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    <i>None</i>.<br>
    This callback is called automatically and periodically when the DSP engine updates.<br>
    For a read update to be called it would have to be enabled, and this is done with DSP::setActive.
    <br>
    The range of -1 to 1 is a soft limit. In the case of the inbuffer it is not guaranteed to be in that range, and in the case of the outbuffer FMOD will accept values outside that range. However all values will be clamped to the range of -1 to 1 in the final mix.<br>
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    DSP::setActive
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_READCALLBACK(FMOD_DSP_STATE *dsp_state, float *inbuffer, float *outbuffer, unsigned int length, int inchannels, int outchannels);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    Callback that is called when the user sets the position of a channel with Channel::setPosition.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.
    'position'      Position in channel stream to set to.  Units are PCM samples (ie FMOD_TIMEUNIT_PCM).

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    Channel::setPosition.<br>
    If a DSP unit is attached to a channel and the user calls Channel::setPosition then this funciton will be called.
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    Channel::setPosition
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_SETPOSITIONCALLBACK(FMOD_DSP_STATE *dsp_state, unsigned int position);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    This callback is called when the user wants the plugin to display a configuration dialog box.  This is not always nescessary, so this can be left blank if wanted.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.
    'hwnd'          This is the target hwnd to display the dialog in.  It must not pop up on this hwnd, it must actually be drawn within it.
    'show'          1 = show the dialog, 0 = hide/remove the dialog.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    DSP::showConfigDialog.<br>
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    DSP::showConfigDialog
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_DIALOGCALLBACK(FMOD_DSP_STATE *dsp_state, void *hwnd, int show);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    This callback is called when the user wants to set a parameter for a DSP unit.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.
    'index'         The index into the parameter list for the parameter the user wants to set.
    'value'         The value passed in by the user to set for the selected parameter.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    DSP::setParameter.<br>
    <br>
    Range checking is not needed.  FMOD will clamp the incoming value to the specified min/max.
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    DSP::setParameter
    FMOD_DSP_GETPARAMCALLBACK
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_SETPARAMCALLBACK(FMOD_DSP_STATE *dsp_state, int index, float value);
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/


/*
[API]
[
	[DESCRIPTION]
    This callback is called when the user wants to get an indexed parameter from a DSP unit.

	[PARAMETERS]
    'dsp_state'     Pointer to the plugin state.  The user can use this variable to access runtime plugin specific variables and plugin writer user data.  Do not cast this to FMOD_DSP!  The handle to the user created DSP handle is stored within the FMOD_DSP_STATE structure.
    'index'         The index into the parameter list for the parameter the user wants to get.
    'value'         Pointer to a floating point variable to receive the selected parameter value.
    'valuestr'      A pointer to a string to receive the value of the selected parameter, but in text form.  This might be useful to display words instead of numbers.  For example "ON" or "OFF" instead of 1.0 and 0.0.  The length of the buffer being passed in is always 16 bytes, so do not exceed this.

	[RETURN_VALUE]

	[REMARKS]
    Functions that the user would have to call for this callback to be called.<br>
    DSP::getParameter.<br>
    FMOD_DSP_GETPARAMCALLBACK.<br>
    <br>
    Remember to return FMOD_OK at the bottom of the function, or an appropriate error code from FMOD_RESULT.<br>

    [PLATFORMS]
    Win32, Win64, Linux, Linux64, Macintosh, Xbox360, PlayStation 2, PlayStation Portable, PlayStation 3, Wii, Solaris, iPhone

	[SEE_ALSO]
    FMOD_DSP_STATE
    DSP::getParameter
    FMOD_DSP_SETPARAMCALLBACK
]
*/
/*
FMOD_RESULT F_CALLBACK FMOD_DSP_GETPARAMCALLBACK(FMOD_DSP_STATE *dsp_state, int index, float *value, char *valuestr)
{
#if 0
	 // here purely for documentation purposes.
#endif
}
*/



/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]
    Win32, Win64, Linux, Macintosh, XBox, PlayStation 2, GameCube

	[SEE_ALSO]
]
*/


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::setInputMix(int index, float volume)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getInput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->setMix(volume);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::getInputMix(int index, float *volume)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getInput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->getMix(volume);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::setInputLevels(int index, FMOD_SPEAKER speaker, float *levels, int numlevels)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getInput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->setLevels(speaker, levels, numlevels);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::getInputLevels(int index, FMOD_SPEAKER speaker, float *levels, int numlevels)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getInput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->getLevels(speaker, levels, numlevels);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::setOutputMix(int index, float volume)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getOutput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->setMix(volume);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::getOutputMix(int index, float *volume)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getOutput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->getMix(volume);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::setOutputLevels(int index, FMOD_SPEAKER speaker, float *levels, int numlevels)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getOutput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->setLevels(speaker, levels, numlevels);
}


/*
[
	[DESCRIPTION]

	[PARAMETERS]
 
	[RETURN_VALUE]

	[REMARKS]

    [PLATFORMS]

	[SEE_ALSO]
]
*/
FMOD_RESULT DSPI::getOutputLevels(int index, FMOD_SPEAKER speaker, float *levels, int numlevels)
{
    FMOD_RESULT result;
    DSPConnectionI *connection;

    result = getOutput(index, &connection, 0);
    if (result != FMOD_OK)
    {
        return result;
    }

    return connection->getLevels(speaker, levels, numlevels);
}


}
