/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-matcher
 * Created on: 02 ноя 2025 г.
 *
 * lsp-plugins-matcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-matcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-matcher. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PRIVATE_PLUGINS_MATCHER_H_
#define PRIVATE_PLUGINS_MATCHER_H_

#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/ctl/Toggle.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/MultiSpectralProcessor.h>
#include <lsp-plug.in/dsp-units/sampling/Sample.h>
#include <lsp-plug.in/dsp-units/sampling/SamplePlayer.h>
#include <lsp-plug.in/plug-fw/plug.h>
#include <lsp-plug.in/lltl/state.h>
#include <private/meta/matcher.h>

namespace lsp
{
    namespace plugins
    {
        /**
         * Base class for the latency compensation delay
         */
        class matcher: public plug::Module
        {
            protected:
                enum processor_channel_t
                {
                    PC_INPUT,
                    PC_REFERENCE,
                    PC_CAPTURE,

                    PC_TOTAL
                };

                enum in_source_t
                {
                    IN_STATIC,              // Static input profile is used for matching
                    IN_DYNAMIC              // Dynamic input profile is used for matching
                };

                enum ref_source_t
                {
                    REF_NONE,
                    REF_CAPTURE,
                    REF_FILE,
                    REF_EQUALIZER,
                    REF_SIDECHAIN,
                    REF_LINK
                };

                enum raw_cap_source_t
                {
                    RAW_CAP_INPUT,          // Take spectral data from input channel
                    RAW_CAP_SIDECHAIN,      // Take spectral data from sidechain channel
                    RAW_CAP_LINK            // Take spectral data from shared memory link
                };

                enum cap_source_t
                {
                    CAP_NONE,               // No capture
                    CAP_INPUT,              // Take spectral data from input channel
                    CAP_SIDECHAIN,          // Take spectral data from sidechain channel
                    CAP_LINK,               // Take spectral data from shared memory link
                    CAP_REFERENCE           // Take spectral data from reference channel
                };

                enum sig_meters_t
                {
                    SM_IN,
                    SM_OUT,
                    SM_CAPTURE,
                    SM_REFERENCE,

                    SM_TOTAL
                };

                enum profile_type_t
                {
                    PROF_MATCH,             // Profile for matching curve, SR -> recompute
                    PROF_INPUT,             // Profile for the dynamic input audio, SR -> reset
                    PROF_REFERENCE,         // Profile for the dynamic reference audio, SR -> reset
                    PROF_STATIC,            // Profile for the static input audio, SR -> resample
                    PROF_CAPTURE,           // Profile for the static captured audio, SR -> resample
                    PROF_FILE,              // Profile for the file, SR -> re-render
                    PROF_ENVELOPE,          // Profile for the envelope, SR -> recompute
                    PROF_MIN_EQUALIZER,     // Profile for the minimum equalizer, SR -> recompute
                    PROF_MAX_EQUALIZER,     // Profile for the maximum equalizer, SR -> recompute

                    PROF_TOTAL
                };

                enum state_profile_type_t
                {
                    SPROF_STATIC,
                    SPROF_CAPTURE,
                    SPROF_FILE,

                    SPROF_TOTAL
                };

                enum kvt_profile_flags_t
                {
                    KVT_PFLAGS_NONE         = 0,
                    KVT_PFLAGS_READY        = 1 << 0,
                    KVT_PFLAGS_DEFAULT      = 1 << 1
                };

                enum profile_data_flags_t
                {
                    PFLAGS_NONE             = 0,
                    PFLAGS_DEFAULT          = 1 << 0,           // Default (empty) profile
                    PFLAGS_READY            = 1 << 1,           // Profile is ready for processing
                    PFLAGS_DIRTY            = 1 << 2,           // Profile is dirty and has not been saved
                    PFLAGS_CHANGED          = 1 << 3,           // Profile has been changed
                    PFLAGS_SYNC             = 1 << 4,           // Profile needs to be synchronized with UI
                    PFLAGS_NORMAL           = 1 << 5,           // Profile is filled with 0 dB amplification
                    PFLAGS_DYNAMIC          = 1 << 6,           // Profile is dynamically changing
                };

                enum eq_param_t
                {
                    EQP_REF_LEVEL,                              // Reference level
                    EQP_MAX_AMPLIFICATION,                      // Maximum amplification
                    EQP_MAX_REDUCTION,                          // Maximum reduction
                    EQP_REACTIVITY,                             // Reactvity

                    EQP_TOTAL
                };

                typedef struct profile_data_t
                {
                    uint32_t                nSampleRate;        // Sample rate of the profile
                    uint32_t                nChannels;          // Number of channels
                    uint32_t                nRank;              // FFT rank of the profile
                    uint32_t                nFlags;             // Profile data flags
                    uint32_t                nFrames;            // Number of frames collected
                    float                   fRMS;               // Profile loudness
                    float                 **vData;              // Sample data
                } profile_data_t;

                typedef struct af_descriptor_t
                {
                    dspu::Toggle        sListen;        // Listen toggle
                    dspu::Toggle        sStop;          // Stop toggle
                    dspu::Sample       *pOriginal;      // Original file sample
                    dspu::Sample       *pProcessed;     // Processed file sample
                    float              *vThumbs[2];     // Thumbnails
                    float               fNorm;          // Norming factor
                    status_t            nStatus;
                    bool                bSync;          // Synchronize file

                    float               fPitch;         // Pitch amount
                    float               fHeadCut;
                    float               fTailCut;
                    float               fDuration;      // Actual audio file duration

                    plug::IPort        *pFile;          // Port that contains file name
                    plug::IPort        *pPitch;         // Pitching amount in semitones
                    plug::IPort        *pHeadCut;
                    plug::IPort        *pTailCut;
                    plug::IPort        *pListen;
                    plug::IPort        *pStop;
                    plug::IPort        *pStatus;        // Status of file loading
                    plug::IPort        *pLength;        // Length of file
                    plug::IPort        *pThumbs;        // Thumbnails of file
                    plug::IPort        *pPlayPosition;  // Output current playback position
                } af_descriptor_t;

                typedef struct channel_t
                {
                    // DSP processing modules
                    dspu::Bypass            sBypass;            // Bypass
                    dspu::SamplePlayer      sPlayer;            // Sample player
                    dspu::Playback          sPlayback;          // Sample playback
                    dspu::Delay             sDryDelay;          // Delay for dry (unprocessed) signal
                    dspu::Delay             sScDelay;           // Delay for sidechain signal

                    float                  *vIn;                // Input buffer
                    float                  *vOut;               // Output buffer
                    float                  *vSc;                // Sidechain buffer
                    float                  *vShmIn;             // Shared memory link
                    float                  *vFft[SM_TOTAL];     // FFT data
                    float                  *vBuffer;            // Temporary buffer

                    bool                    bFft[SM_TOTAL];     // Perform FFT processing

                    plug::IPort            *pIn;                // Input buffer
                    plug::IPort            *pOut;               // Output buffer
                    plug::IPort            *pSc;                // Sidechain buffer
                    plug::IPort            *pShmIn;             // Shared memory link

                    plug::IPort            *pFft[SM_TOTAL];     // Show FFT of signal
                    plug::IPort            *pMeter[SM_TOTAL];   // Level meter of signal
                } channel_t;

                typedef struct match_band_t
                {
                    float                   vParams[EQP_TOTAL]; // Equalizer parameters
                    plug::IPort            *pParams[EQP_TOTAL]; // Reference level
                } match_band_t;

                typedef struct kvt_profile_header_t
                {
                    uint16_t                nVersion;           // Version format of the BLOB
                    uint8_t                 nChannels;          // Number of channels
                    uint8_t                 nRank;              // FFT rank
                    uint32_t                nFlags;             // Flags
                    uint32_t                nSampleRate;        // Sample rate
                    uint32_t                nFrames;            // Frames
                    float                   fRMS;               // RMS
                } kvt_profile_header_t;

                class FileLoader: public ipc::ITask
                {
                    private:
                        matcher                *pCore;

                    public:
                        explicit FileLoader(matcher *core);
                        virtual ~FileLoader() override;

                    public:
                        virtual status_t run() override;

                        void        dump(dspu::IStateDumper *v) const;
                };

                class FileProcessor: public ipc::ITask
                {
                    private:
                        matcher                *pCore;

                    public:
                        explicit FileProcessor(matcher *core);
                        virtual ~FileProcessor() override;

                    public:
                        virtual status_t run() override;
                        void        dump(dspu::IStateDumper *v) const;
                };

                class KVTSync: public ipc::ITask, public core::KVTListener
                {
                    private:
                        matcher                *pCore;
                        profile_data_t         *vProfiles[SPROF_TOTAL];
                        size_t                  nChanges;

                    protected:
                        void        parse_profile(const char *id, const core::kvt_param_t *param, uint32_t type);

                    public:
                        explicit KVTSync(matcher *core);
                        virtual ~KVTSync() override;

                        status_t    init();
                        bool        submit_profile(uint32_t type, profile_data_t *profile);
                        bool        pending() const;
                        void        dump(dspu::IStateDumper *v) const;

                    public: // ipc::ITask
                        virtual status_t run() override;

                    public: // core::KVTListener
                        virtual void created(core::KVTStorage *storage, const char *id, const core::kvt_param_t *param, size_t pending) override;
                        virtual void changed(core::KVTStorage *storage, const char *id, const core::kvt_param_t *oval, const core::kvt_param_t *nval, size_t pending) override;
                        virtual void commit(core::KVTStorage *storage, const char *id, const core::kvt_param_t *param, size_t pending) override;
                };

                class GCTask: public ipc::ITask
                {
                    private:
                        matcher                *pCore;

                    public:
                        explicit GCTask(matcher *base);
                        virtual ~GCTask() override;

                    public:
                        virtual status_t run() override;

                        void        dump(dspu::IStateDumper *v) const;
                };

            protected:
                uint32_t            nChannels;          // Number of channels
                channel_t          *vChannels;          // Delay channels
                uint32_t            nInSource;          // Input source
                uint32_t            nRefSource;         // Reference source
                uint32_t            nRawCapSource;      // Raw capture source
                uint32_t            nCapSource;         // Capture source
                uint32_t            nRank;              // FFT rank
                float               fGainIn;            // Input gain
                float               fGainOut;           // Output gain
                float               fFftTau;            // FFT time constant
                float               fFftShift;          // FFT shift
                float               fInTau;             // Input profile reactivity
                float               fRefTau;            // Reference profile reactivity
                float               fStereoLink;        // Stereo linking
                float               fBlend;             // Blend signal
                float               fHpfFreq;           // HPF frequency
                float               fHpfSlope;          // HPF slope
                float               fLpfFreq;           // LPF frequency
                float               fLpfSlope;          // LPF slope
                float               fClipFreq;          // Brickwall clipping frequency
                uint32_t            nFileProcessReq;    // File processing request
                uint32_t            nFileProcessResp;   // File processing response
                bool                bSidechain;         // Sidechain flag
                bool                bProfile;           // Profile capturing is enabled
                bool                bCapture;           // Capture side signal
                bool                bListen;            // Listen signal
                bool                bSyncRefFFT;        // Synchronize reference FFT
                bool                bSyncFilter;        // Synchronize filter profile
                bool                bUpdateMatch;       // Update matching profile
                bool                bMatchLimit;        // Match curve limiting enabled

                dspu::MultiSpectralProcessor    sProcessor; // Multi-channel spectral processor
                af_descriptor_t     sFile;              // Audio file
                FileLoader          sFileLoader;        // Audio file loader
                FileProcessor       sFileProcessor;     // Audio file processor task
                KVTSync             sKVTSync;           // KVT synchronization task
                GCTask              sGCTask;            // Garbage collection task
                match_band_t        vMatchBands[meta::matcher::MATCH_BANDS];    // Match bands
                ipc::IExecutor     *pExecutor;          // Task executor
                dspu::Sample       *pGCList;            // Garbage collection list
                profile_data_t     *pReactivity;        // Reactivity profile
                profile_data_t     *pTempProfile;       // Temporary profile
                profile_data_t     *pFilterProfile;     // Filter profile
                profile_data_t     *pMatchProfile;      // Actual matching profile
                profile_data_t     *vProfileData[PROF_TOTAL];               // Profile data
                lltl::state<profile_data_t> vProfileState[SPROF_TOTAL];     // Record of the input profile

                uint16_t           *vIndices;           // FFT indices
                float              *vFreqs;             // FFT frequencies
                float              *vFilterCurve;       // Filter curve
                float              *vEnvelope;          // FFT envelope
                float              *vRevEnvelope;       // FFT reverse envelope
                float              *vBuffer;            // Temporary buffer
                float              *vEmptyBuf;          // Empty

                plug::IPort        *pBypass;            // Bypass
                plug::IPort        *pGainIn;            // Input gain
                plug::IPort        *pGainOut;           // Output gain
                plug::IPort        *pFftSize;           // FFT size
                plug::IPort        *pInReactivity;      // Input profile reactivity
                plug::IPort        *pRefReactivity;     // Reference profile reactivity
                plug::IPort        *pInSource;          // Input source
                plug::IPort        *pRefSource;         // Reference source
                plug::IPort        *pCapSource;         // Capture source
                plug::IPort        *pBlend;             // Blend signal
                plug::IPort        *pProfile;           // Start profiling
                plug::IPort        *pCapture;           // Enable capturing
                plug::IPort        *pListen;            // Listen capture
                plug::IPort        *pHpfOn;             // High-pass filter on
                plug::IPort        *pHpfFreq;           // High-pass filter frequency
                plug::IPort        *pHpfSlope;          // High-pass filter slope
                plug::IPort        *pLpfOn;             // Low-pass filter on
                plug::IPort        *pLpfFreq;           // Low-pass filter frequency
                plug::IPort        *pLpfSlope;          // Low-pass filter slope
                plug::IPort        *pClipOn;            // Brickwall clipping enabled
                plug::IPort        *pClipFreq;          // Brickwall clipping frequency
                plug::IPort        *pFilterMesh;        // Filter mesh
                plug::IPort        *pStereoLink;        // Stereo link

                plug::IPort        *pMatchLimit;        // Enable frequency limiting
                plug::IPort        *pMatchMesh;         // Match mesh

                plug::IPort        *pFftReact;          // FFT reactivity for analysis
                plug::IPort        *pFftShift;          // FFT shift
                plug::IPort        *pFftMesh;           // Mesh for FFT analysis

                uint8_t            *pData;              // Allocated data

            protected:
                static void         process_block(void *object, void *subject, float * const * spectrum, size_t rank);
                static void         process_sample_block(void *object, void *subject, float * const * spectrum, size_t rank);
                static void         free_profile_data(profile_data_t *profile);
                static void         destroy_sample(dspu::Sample * &s);
                static void         destroy_samples(dspu::Sample *gc_list);
                static bool         profile_is_relative(size_t profile);

            protected:
                void                do_destroy();
                profile_data_t     *allocate_profile_data(size_t channels = 0);
                profile_data_t     *create_default_profile(size_t channels = 0);
                void                init_buffers();
                void                bind_buffers(size_t samples);
                void                advance_buffers(size_t samples);
                void                update_frequency_mapping();
                void                output_fft_mesh_data();
                void                output_profile_mesh_data();
                void                output_file_mesh_data();
                void                output_filter_mesh_data();
                void                process_block(float * const * spectrum, size_t rank);
                void                analyze_spectrum(channel_t *c, sig_meters_t meter, const float *fft);
                uint32_t            decode_reference_source(size_t ref) const;
                uint32_t            decode_capture_source(size_t cap, size_t ref) const;
                bool                check_need_profile_sync();
                void                record_profile(profile_data_t *profile, float * const * spectrum, size_t channel);
                void                clear_profile_data(profile_data_t *profile);
                status_t            load_audio_file(af_descriptor_t *descr);
                status_t            process_audio_file();
                status_t            preprocess_sample(af_descriptor_t *f);
                status_t            profile_sample(af_descriptor_t *f);
                void                process_file_loading_tasks();
                void                process_file_processing_tasks();
                void                process_kvt_sync_tasks();
                void                process_gc_tasks();
                void                process_listen_events();
                void                perform_gc();
                void                update_profiles();
                void                build_eq_profile(profile_data_t *profile, eq_param_t param, bool envelope);
                void                build_filter_profile();
                void                smooth_eq_curve(float *dst, float x1, float y1, float x2, float y2, size_t count);
                void                sync_profile(profile_data_t *dst, profile_data_t *src);
                inline void         sync_profile_with_state(profile_data_t *profile);
                void                post_process_profiles();
                void                track_profile(profile_data_t *profile, float * const * spectrum, float tau, size_t channel);
                void                build_match_profile(profile_data_t *in, profile_data_t *ref, bool dynamic);
                bool                resample_profile(profile_data_t *profile, size_t srate, size_t rank);
                void                commit_profiles();
                void                process_listen_output(channel_t *c, size_t samples);
                bool                save_profile(core::KVTStorage *kvt, const char *path, profile_data_t *profile);
                profile_data_t     *load_profile(const char *path, const core::kvt_param_t *param);

            public:
                explicit matcher(const meta::plugin_t *meta);
                matcher (const matcher &) = delete;
                matcher (matcher &&) = delete;
                virtual ~matcher() override;

                matcher & operator = (const matcher &) = delete;
                matcher & operator = (matcher &&) = delete;

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports) override;
                virtual void        destroy() override;

            public:
                virtual void        update_sample_rate(long sr) override;
                virtual void        update_settings() override;
                virtual void        process(size_t samples) override;
                virtual void        ui_activated() override;
                virtual void        dump(dspu::IStateDumper *v) const override;
        };

    } /* namespace plugins */
} /* namespace lsp */


#endif /* PRIVATE_PLUGINS_MATCHER_H_ */

