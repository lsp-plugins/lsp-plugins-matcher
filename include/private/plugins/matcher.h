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
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/MultiSpectralProcessor.h>
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

                enum ref_source_t
                {
                    REF_CAPTURE,
                    REF_FILE,
                    REF_EQUALIZER,
                    REF_SIDECHAIN,
                    REF_LINK
                };

                enum cap_source_t
                {
                    CAP_NONE,           // No capture
                    CAP_INPUT,          // Take spectral data from input channel
                    CAP_SIDECHAIN,      // Take spectral data from sidechain channel
                    CAP_LINK,           // Take spectral data from shared memory link
                    CAP_REFERENCE       // Take spectral data from reference channel
                };

                enum sig_meters_t
                {
                    SM_IN,
                    SM_REFERENCE,
                    SM_CAPTURE,
                    SM_OUT,

                    SM_TOTAL
                };

                enum profile_type_t
                {
                    PROF_INPUT,         // Profile for the input audio
                    PROF_REFERENCE,     // Profile for the reference audio
                    PROF_CAPTURE,       // Profile for the captured audio
                    PROF_FILE,          // Profile for the file
                    PROF_EQUALIZER,     // Profile for the equalizer

                    PROF_TOTAL
                };

                enum profile_data_flags_t
                {
                    PFLAGS_NONE             = 0,
                    PFLAGS_DEFAULT          = 1 << 0,           // Default (empty) profile
                    PFLAGS_DIRTY            = 1 << 1,           // Profile is dirty and has not been saved
                };

                typedef struct channel_t
                {
                    // DSP processing modules
                    dspu::Bypass            sBypass;            // Bypass

                    float                  *vIn;                // Input buffer
                    float                  *vOut;               // Output buffer
                    float                  *vSc;                // Sidechain buffer
                    float                  *vShmIn;             // Shared memory link
                    float                  *vFft[SM_TOTAL];     // FFT data

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
                    float                   fMaxAmp;            // Maximum amplification
                    float                   fMaxRed;            // Maximum reduction
                    float                   fReact;             // Reactivity

                    plug::IPort            *pMaxAmp;            // Maximum amplification
                    plug::IPort            *pMaxRed;            // Maximum reduction
                    plug::IPort            *pReact;             // Reactvity
                } match_band_t;

                typedef struct profile_data_t
                {
                    uint32_t                nOriginRate;        // Original sample rate of the profile
                    uint32_t                nActualRate;        // Actual sample rate of the profile
                    uint32_t                nOriginRank;        // Original FFT rank of the profile
                    uint32_t                nActualRank;        // Actual FFT rank of the profile
                    uint32_t                nFlags;             // Profile data flags
                    float                 **vOriginData;        // Original data (without resampling)
                    float                 **vActualData;        // Resampled data (matching processing)
                } profile_data_t;

            protected:
                uint32_t            nChannels;          // Number of channels
                channel_t          *vChannels;          // Delay channels
                uint32_t            nRefSource;         // Reference source
                uint32_t            nCapSource;         // Capture source
                uint32_t            nRank;              // FFT rank
                float               fFftTau;            // FFT time constant
                float               fFftShift;          // FFT shift
                bool                bSidechain;         // Sidechain flag

                dspu::MultiSpectralProcessor    sProcessor; // Multi-channel spectral processor
                match_band_t        vMatchBands[meta::matcher::MATCH_BANDS];    // Match bands
                profile_data_t     *pEqProfile;         // Actual equalization profile
                lltl::state<profile_data_t> vProfileData[PROF_TOTAL];           // Profile data

                uint16_t           *vIndices;           // FFT indices
                float              *vFreqs;             // FFT frequencies
                float              *vEnvelope;          // FFT envelope
                float              *vBuffer;            // Temporary buffer

                plug::IPort        *pBypass;            // Bypass
                plug::IPort        *pGainIn;            // Input gain
                plug::IPort        *pGainOut;           // Output gain
                plug::IPort        *pFftSize;           // FFT size
                plug::IPort        *pResetIn;           // Reset input signal profile
                plug::IPort        *pResetRef;          // Reset reference signal profile
                plug::IPort        *pResetCap;          // Reset captured signal profile
                plug::IPort        *pInReactivity;      // Input profile reactivity
                plug::IPort        *pRefReactivity;     // Reference profile reactivity
                plug::IPort        *pRefSource;         // Reference source
                plug::IPort        *pCapSource;         // Capture source
                plug::IPort        *pProfile;           // Start profiling
                plug::IPort        *pCapture;           // Enable capturing
                plug::IPort        *pListen;            // Listen capture
                plug::IPort        *pStereoLink;        // Stereo link

                plug::IPort        *pMatchMode;         // Operating mode
                plug::IPort        *pMatchReset;        // Reset match curves
                plug::IPort        *pMatchImmediate;    // Perform immediate match
                plug::IPort        *pMatchMesh;         // Match mesh

                plug::IPort        *pFftReact;          // FFT reactivity for analysis
                plug::IPort        *pFftShift;          // FFT shift
                plug::IPort        *pFftMesh;           // Mesh for FFT analysis

                uint8_t            *pData;              // Allocated data

            protected:
                static void         process_block(void *object, void *subject, float * const * spectrum, size_t rank);
                profile_data_t     *allocate_profile_data();
                profile_data_t     *create_default_profile();
                static void         free_profile_data(profile_data_t *profile);

            protected:
                void                do_destroy();
                void                bind_buffers();
                void                process_signal(size_t to_do);
                void                update_frequency_mapping();
                void                output_fft_mesh_data();
                void                process_block(float * const * spectrum, size_t rank);
                void                analyze_spectrum(channel_t *c, sig_meters_t meter, const float *fft);
                uint32_t            decode_reference_source(size_t ref) const;
                uint32_t            decode_capture_source(size_t cap, bool capture, size_t ref) const;

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
                virtual void        dump(dspu::IStateDumper *v) const override;
        };

    } /* namespace plugins */
} /* namespace lsp */


#endif /* PRIVATE_PLUGINS_MATCHER_H_ */

