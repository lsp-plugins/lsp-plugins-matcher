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

                typedef struct channel_t
                {
                    // DSP processing modules
                    dspu::Bypass            sBypass;            // Bypass

                    float                  *vIn;                // Input buffer
                    float                  *vOut;               // Output buffer
                    float                  *vSc;                // Sidechain buffer
                    float                  *vShmIn;             // Shared memory link

                    plug::IPort            *pIn;                // Input buffer
                    plug::IPort            *pOut;               // Output buffer
                    plug::IPort            *pSc;                // Sidechain buffer
                    plug::IPort            *pShmIn;             // Shared memory link
                } channel_t;

            protected:
                uint32_t            nChannels;          // Number of channels
                channel_t          *vChannels;          // Delay channels
                uint32_t            nRefSource;         // Reference source
                uint32_t            nCapSource;         // Capture source
                bool                bSidechain;         // Sidechain flag

                dspu::MultiSpectralProcessor    sProcessor; // Multi-channel spectral processor

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

                uint8_t            *pData;              // Allocated data

            protected:
                static void         process_block(void *object, void *subject, float * const * spectrum, size_t rank);

            protected:
                void                do_destroy();
                void                bind_buffers();
                void                process_signal(size_t to_do);
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

