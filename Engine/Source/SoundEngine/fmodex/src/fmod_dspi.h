#ifndef _FMOD_DSPI_H
#define _FMOD_DSPI_H

#include "fmod_settings.h"

#include <atomic>

#include "fmod_plugin.h"
#include "fmod.hpp"
#include "fmod_dsp_connectioni.h"
#include "fmod_linkedlist.h"
#include "fmod_os_misc.h"

namespace FMOD
{ 
    class ChannelSoftware;
    class DSPFilter;
    class PluginFactory;
    class MemoryTracker;

    typedef enum
    {
        FMOD_DSP_CATEGORY_FILTER,           /* This is a unit type that processes incoming data. */
        FMOD_DSP_CATEGORY_DSPCODECMPEG,     /* This is a unit type that resamples data from a codec. (MPEG) */
        FMOD_DSP_CATEGORY_DSPCODECADPCM,    /* This is a unit type that resamples data from a codec. (ADPCM) */
        FMOD_DSP_CATEGORY_DSPCODECXMA,      /* This is a unit type that resamples data from a codec. (XMA) */
        FMOD_DSP_CATEGORY_DSPCODECRAW,      /* This is a unit type that resamples data from a codec. (RAW) */
        FMOD_DSP_CATEGORY_SOUNDCARD,        /* This is a unit type that only receives data. (Not used on PS3 non-recursive SPU mixer) */
        FMOD_DSP_CATEGORY_WAVETABLE,        /* This is a unit type that only reads wave data. */
        FMOD_DSP_CATEGORY_RESAMPLER
    } FMOD_DSP_CATEGORY;

    #define FMOD_DSP_MAXTREEDEPTH  128

    const int FMOD_DSP_TYPE_CODECREADER   = 1000;

    struct FMOD_DSP_DESCRIPTION_EX : public FMOD_DSP_DESCRIPTION, public LinkedListNode
    {
        FMOD_SOUND_FORMAT       mFormat;     
        FMOD_DSP_TYPE           mType;
        int                     mSize;
        FMOD_DSP_CATEGORY       mCategory;
#ifdef FMOD_SUPPORT_DLLS
        FMOD_OS_LIBRARY        *mModule;
        void                   *mAEffect;
#endif
        int                     mResamplerBlockLength;
     };

    #define FMOD_DSP_CONNECTION_REQUEST_MAX     1024

    typedef enum
    {
        DSPCONNECTION_REQUEST_ADDINPUT,
        DSPCONNECTION_REQUEST_DISCONNECTFROM,
        DSPCONNECTION_REQUEST_DISCONNECTALLINPUTS,
        DSPCONNECTION_REQUEST_DISCONNECTALLOUTPUTS,
        DSPCONNECTION_REQUEST_DISCONNECTALL
    } FMOD_DSPCONNECTIONREQUEST_CMD;

    #define FMOD_DSP_FLAG_VISITED               0x00000001
    #define FMOD_DSP_FLAG_ACTIVE                0x00000002
    #define FMOD_DSP_FLAG_BYPASS                0x00000004
    #define FMOD_DSP_FLAG_FINISHED              0x00000008
    #define FMOD_DSP_FLAG_IDLE                  0x00000010
    #define FMOD_DSP_FLAG_USEDADDDSP            0x00000020

    class DSPConnectionRequest : public LinkedListNode
    {
      public:

        DSPI                           *mThis;
        DSPI                           *mTarget;
        DSPConnectionI                 *mConnection;
        FMOD_DSPCONNECTIONREQUEST_CMD   mRequest; 
    };
        
    class DSPI : public Plugin, public FMOD_DSP_STATE
    {
        DECLARE_MEMORYTRACKER_NONVIRTUAL_EXPORT

        friend class DSPFilter;
        friend class DSPNet;

      private:

        LinkedListNode          mInputHead;
        LinkedListNode          mOutputHead;
        int                     mNumInputs;
        int                     mNumOutputs;

      protected:

        float                  *mBufferMemory;
        int                     mTargetFrequency;
        int                     mTreeLevel;

        FMOD_RESULT             doesUnitExist(DSPI *target);

      public:

        static  FMOD_RESULT     convert         (void *outbuffer, void *inbuffer, FMOD_SOUND_FORMAT outformat, FMOD_SOUND_FORMAT informat, unsigned int length, int destchannelstep, int srcchannelstep, float wetmix);
        static  FMOD_RESULT     validate        (DSP *dsp, DSPI **dspi);

        virtual FMOD_RESULT     execute         (void *inbuffer, void **outbuffer, unsigned int *length, int inchannels, int *outchannels, FMOD_SPEAKERMODE speakermode);
        virtual FMOD_RESULT     execute         (float *inbuffer, float **outbuffer, unsigned int *length, int inchannels, int *outchannels, FMOD_SPEAKERMODE speakermode);
        virtual FMOD_RESULT     alloc           (FMOD_DSP_DESCRIPTION_EX *description);
        virtual FMOD_RESULT     setPosition     (unsigned int position, bool processinputs);
        static  FMOD_RESULT     calculateSpeakerLevels(float frontleft, float frontright, float center, float lfe, float backleft, float backright, float sideleft, float sideright, FMOD_SPEAKERMODE speakermode, int channels, FMOD_SPEAKERMAPTYPE speakermap, float *outlevels, int *numinputlevels);
                FMOD_RESULT     updateTreeLevel (int level);
                FMOD_RESULT     resetVisited    ();
                FMOD_RESULT     setFinished     (bool finished, bool force);

        FMOD_DSP_DESCRIPTION_EX mDescription;
        float                   mDefaultVolume;
        float                   mDefaultFrequency;
        float                   mDefaultPan;
        int                     mDefaultPriority;
        float                  *mBuffer;
        int                     mBufferChannels;
        // Not stock: atomic, the mixer thread (FINISHED/IDLE) and the API thread (ACTIVE/BYPASS) both read-modify-write
        // it; plain |= / &= lost each other's bits. Same size and alignment as the stock unsigned int.
        std::atomic<unsigned int> mFlags;
        char                   *mWantsToFinish;
        char                    mWantsToFinishMem;

        DSPI();

        virtual FMOD_RESULT release             (bool freethis = true);
        virtual FMOD_RESULT getSystemObject     (System **system);

        // Connection / disconnection / input and output enumeration.
                FMOD_RESULT addInputInternal    (DSPI *target, bool checkcircular, DSPConnectionI *connection = 0, DSPConnectionI **connection_out = 0);
        virtual FMOD_RESULT addInput            (DSPI *target);
                FMOD_RESULT addInputQueued      (DSPI *target, bool checkcircular, DSPConnectionI *connection_old, DSPConnectionI **connection_out);
                FMOD_RESULT disconnectFromInternal(DSPI *target);
                FMOD_RESULT disconnectFromQueued(DSPI *target);
                FMOD_RESULT disconnectFrom      (DSPI *target);
                FMOD_RESULT disconnectAllInternal(bool inputs, bool outputs);
                FMOD_RESULT disconnectAllQueued (bool inputs, bool outputs);
                FMOD_RESULT disconnectAll       (bool inputs, bool outputs);
        virtual FMOD_RESULT remove              ();

        FMOD_RESULT getNumInputs                (int *numinputs);
        FMOD_RESULT getNumOutputs               (int *numoutputs);
        FMOD_RESULT getInput                    (int index, DSPConnectionI **input, DSPI **inputdsp);
        FMOD_RESULT getInput                    (int index, DSPI **inputdsp);
        FMOD_RESULT getOutput                   (int index, DSPConnectionI **output, DSPI **outputdsp);
        FMOD_RESULT getOutput                   (int index, DSPI **outputdsp);
        FMOD_RESULT setInputMix                 (int index, float volume);
        FMOD_RESULT getInputMix                 (int index, float *volume);
        FMOD_RESULT setInputLevels              (int index, FMOD_SPEAKER speaker, float *levels, int numlevels);
        FMOD_RESULT getInputLevels              (int index, FMOD_SPEAKER speaker, float *levels, int numlevels);
        FMOD_RESULT setOutputMix                (int index, float volume);
        FMOD_RESULT getOutputMix                (int index, float *volume);
        FMOD_RESULT setOutputLevels             (int index, FMOD_SPEAKER speaker, float *levels, int numlevels);
        FMOD_RESULT getOutputLevels             (int index, FMOD_SPEAKER speaker, float *levels, int numlevels);

        // DSP unit control
        FMOD_INLINE FMOD_RESULT setActive       (bool active)  { active ? mFlags |= FMOD_DSP_FLAG_ACTIVE : mFlags &= ~FMOD_DSP_FLAG_ACTIVE; return FMOD_OK; }
        FMOD_INLINE FMOD_RESULT getActive       (bool *active) { mFlags & FMOD_DSP_FLAG_ACTIVE ? *active = true : *active = false; return FMOD_OK; }
        FMOD_INLINE FMOD_RESULT getFinished     (bool *finished) { *finished = (!*mWantsToFinish && (mFlags & FMOD_DSP_FLAG_FINISHED)) ? true : false; return FMOD_OK; }
        FMOD_INLINE FMOD_RESULT setBypass       (bool bypass)  { bypass ? mFlags |= FMOD_DSP_FLAG_BYPASS : mFlags &= ~FMOD_DSP_FLAG_BYPASS; return FMOD_OK; }
        FMOD_INLINE FMOD_RESULT getBypass       (bool *bypass) { mFlags & FMOD_DSP_FLAG_BYPASS ? *bypass = true : *bypass = false; return FMOD_OK; }
        virtual FMOD_RESULT reset               ();

        // DSP parameter control
        virtual FMOD_RESULT setParameter        (int index, float value);
        virtual FMOD_RESULT getParameter        (int index, float *value, char *valuestr, int valuestrlen);
        virtual FMOD_RESULT getNumParameters    (int *numparams);
        virtual FMOD_RESULT getParameterInfo    (int index, char *name, char *label, char *description, int descriptionlen, float *min, float *max);
        virtual FMOD_RESULT showConfigDialog    (void *hwnd, bool show);

        // DSP attributes.
        virtual FMOD_RESULT getInfo             (char *name, unsigned int *version, int *channels, int *configwidth, int *configheight);
        virtual FMOD_RESULT getType             (FMOD_DSP_TYPE *type);
        virtual FMOD_RESULT setDefaults         (float frequency, float volume, float pan, int priority);
        virtual FMOD_RESULT getDefaults         (float *frequency, float *volume, float *pan, int *priority);

        // Userdata set/get.
        FMOD_RESULT setUserData                 (void *userdata);
        FMOD_RESULT getUserData                 (void **userdata);

        virtual FMOD_RESULT  setTargetFrequency (int frequency);
        virtual FMOD_RESULT  getTargetFrequency (int *frequency);
    };
}

#endif  


