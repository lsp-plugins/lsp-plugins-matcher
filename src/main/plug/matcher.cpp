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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/plug-fw/core/AudioBuffer.h>
#include <lsp-plug.in/plug-fw/meta/func.h>
#include <lsp-plug.in/shared/debug.h>

#include <private/plugins/matcher.h>

namespace lsp
{
    namespace plugins
    {
        /* The size of temporary buffer for audio processing */
        static constexpr size_t BUFFER_SIZE             = 0x200;

        //---------------------------------------------------------------------
        // Plugin factory
        static const meta::plugin_t *plugins[] =
        {
            &meta::matcher_mono,
            &meta::matcher_stereo,
            &meta::sc_matcher_mono,
            &meta::sc_matcher_stereo
        };

        static plug::Module *plugin_factory(const meta::plugin_t *meta)
        {
            return new matcher(meta);
        }

        static plug::Factory factory(plugin_factory, plugins, 4);

        //---------------------------------------------------------------------
        // Implementation
        matcher::matcher(const meta::plugin_t *meta):
            Module(meta)
        {
            // Compute the number of audio channels by the number of inputs
            nChannels       = 0;
            for (const meta::port_t *p = meta->ports; p->id != NULL; ++p)
                if (meta::is_audio_out_port(p))
                    ++nChannels;

            bSidechain      =
                (!strcmp(meta->uid, meta::sc_matcher_mono.uid)) ||
                (!strcmp(meta->uid, meta::sc_matcher_stereo.uid));

            // Initialize other parameters
            vChannels       = NULL;

            nRefSource      = REF_CAPTURE;
            nCapSource      = CAP_NONE;

            pBypass         = NULL;
            pGainIn         = NULL;
            pGainOut        = NULL;
            pFftSize        = NULL;
            pResetIn        = NULL;
            pResetRef       = NULL;
            pResetCap       = NULL;
            pInReactivity   = NULL;
            pRefReactivity  = NULL;
            pRefSource      = NULL;
            pCapSource      = NULL;
            pProfile        = NULL;
            pCapture        = NULL;
            pListen         = NULL;
            pStereoLink     = NULL;

            pData           = NULL;
        }

        matcher::~matcher()
        {
            do_destroy();
        }

        void matcher::init(plug::IWrapper *wrapper, plug::IPort **ports)
        {
            // Call parent class for initialization
            Module::init(wrapper, ports);

            // Estimate the number of bytes to allocate
            size_t szof_channels    = align_size(sizeof(channel_t) * nChannels, OPTIMAL_ALIGN);
            size_t buf_sz           = BUFFER_SIZE * sizeof(float);
            size_t alloc            = szof_channels + buf_sz;

            // Allocate memory-aligned data
            uint8_t *ptr            = alloc_aligned<uint8_t>(pData, alloc, OPTIMAL_ALIGN);
            if (ptr == NULL)
                return;

            // Initialize pointers to channels and temporary buffer
            vChannels               = advance_ptr_bytes<channel_t>(ptr, szof_channels);

            for (size_t i=0; i < nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];

                // Construct in-place DSP processors
                c->sBypass.construct();

                c->vIn                  = NULL;
                c->vOut                 = NULL;
                c->vSc                  = NULL;
                c->vShmIn               = NULL;

                c->pIn                  = NULL;
                c->pOut                 = NULL;
                c->pSc                  = NULL;
                c->pShmIn               = NULL;
            }

            // Initialize processor
            if (!sProcessor.init(3 * nChannels, meta::matcher::FFT_RANK_MAX))
                return;
            sProcessor.bind_handler(process_block, this, NULL);

            // Bind ports
            lsp_trace("Binding ports");
            size_t port_id      = 0;

            // Bind input audio ports
            for (size_t i=0; i<nChannels; ++i)
                BIND_PORT(vChannels[i].pIn);

            // Bind output audio ports
            for (size_t i=0; i<nChannels; ++i)
                BIND_PORT(vChannels[i].pOut);

            if (bSidechain)
            {
                // Bind output audio ports
                for (size_t i=0; i<nChannels; ++i)
                    BIND_PORT(vChannels[i].pSc);
            }

            lsp_trace("Binding shared memory link");
            SKIP_PORT("Shared memory link name");
            for (size_t i=0; i<nChannels; ++i)
                BIND_PORT(vChannels[i].pShmIn);

            // Bind bypass
            BIND_PORT(pBypass);
            BIND_PORT(pGainIn);
            BIND_PORT(pGainOut);
            BIND_PORT(pFftSize);
            BIND_PORT(pResetIn);
            BIND_PORT(pResetRef);
            BIND_PORT(pResetCap);
            BIND_PORT(pInReactivity);
            BIND_PORT(pRefReactivity);
            BIND_PORT(pRefSource);
            BIND_PORT(pCapSource);
            BIND_PORT(pProfile);
            BIND_PORT(pCapture);
            BIND_PORT(pListen);
            if (nChannels > 1)
            {
                BIND_PORT(pStereoLink);
            }
        }

        void matcher::destroy()
        {
            Module::destroy();
            do_destroy();
        }

        void matcher::do_destroy()
        {
            // Destroy channels
            if (vChannels != NULL)
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c    = &vChannels[i];
                    c->sBypass.destroy();
                }
                vChannels   = NULL;
            }

            // Free previously allocated data chunk
            if (pData != NULL)
            {
                free_aligned(pData);
                pData       = NULL;
            }
        }

        void matcher::update_sample_rate(long sr)
        {
            // Update sample rate for the bypass processors
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c    = &vChannels[i];
                c->sBypass.init(sr);
            }
        }

        uint32_t matcher::decode_reference_source(size_t ref) const
        {
            if (bSidechain)
            {
                switch (ref)
                {
                    case 0: return REF_CAPTURE;
                    case 1: return REF_FILE;
                    case 2: return REF_EQUALIZER;
                    case 3: return REF_SIDECHAIN;
                    case 4: return REF_LINK;
                    default: break;
                }
            }
            else
            {
                switch (ref)
                {
                    case 0: return REF_CAPTURE;
                    case 1: return REF_FILE;
                    case 2: return REF_EQUALIZER;
                    case 3: return REF_LINK;
                    default: break;
                }
            }

            return REF_CAPTURE;
        }

        uint32_t matcher::decode_capture_source(size_t cap, bool capture, size_t ref) const
        {
            if (!capture)
                return CAP_NONE;

            if (bSidechain)
            {
                switch (cap)
                {
                    case 0: return CAP_INPUT;
                    case 1: return (ref == REF_SIDECHAIN) ? CAP_REFERENCE : CAP_SIDECHAIN;
                    case 2: return (ref == REF_LINK) ? CAP_REFERENCE : CAP_LINK;
                    default: break;
                }
            }
            else
            {
                switch (ref)
                {
                    case 0: return CAP_INPUT;
                    case 1: return (ref == REF_LINK) ? CAP_REFERENCE : CAP_LINK;
                    default: break;
                }
            }

            return CAP_NONE;
        }

        void matcher::update_settings()
        {
            const bool bypass       = pBypass->value() >= 0.5f;
            const uint32_t rank     = lsp_limit(meta::matcher::FFT_RANK_MIN + ssize_t(pFftSize->value()), meta::matcher::FFT_RANK_MIN, meta::matcher::FFT_RANK_MAX);

            nRefSource              = decode_reference_source(pRefSource->value());
            nCapSource              = decode_capture_source(pCapSource->value(), pCapture->value() >= 0.5f, nRefSource);

            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];
                c->sBypass.set_bypass(bypass);
            }

            sProcessor.set_rank(rank);
        }

        void matcher::bind_buffers()
        {
            // Process each channel independently
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];

                // Get input and output buffers
                c->vIn                  = c->pIn->buffer<float>();
                c->vOut                 = c->pOut->buffer<float>();
                c->vSc                  = (c->pSc != NULL) ? c->pSc->buffer<float>() : NULL;
                core::AudioBuffer *buf  = (c->pShmIn != NULL) ? c->pShmIn->buffer<core::AudioBuffer>() : NULL;
                if ((buf != NULL) && (buf->active()))
                    c->vShmIn               = buf->buffer();

                // Bind processor buffers
                const size_t base       = i * PC_TOTAL;

                // Route input
                sProcessor.bind(base + PC_INPUT, c->vOut, c->vIn);

                // Route reference input signal
                switch (nRefSource)
                {
                    case REF_SIDECHAIN:
                        sProcessor.bind(base + PC_REFERENCE, NULL, c->vSc);
                        break;
                    case REF_LINK:
                        sProcessor.bind(base + PC_REFERENCE, NULL, c->vShmIn);
                        break;

                    case REF_CAPTURE:
                    case REF_FILE:
                    case REF_EQUALIZER:
                    default:
                        sProcessor.bind(base + PC_REFERENCE, NULL, NULL);
                        break;
                }

                // Route capture input signal
                switch (nCapSource)
                {
                    case CAP_SIDECHAIN:
                        sProcessor.bind(base + PC_CAPTURE, NULL, c->vSc);
                        break;
                    case CAP_LINK:
                        sProcessor.bind(base + PC_CAPTURE, NULL, c->vShmIn);
                        break;
                    default:
                        sProcessor.bind(base + PC_CAPTURE, NULL, NULL);
                        break;
                }
            }
        }

        void matcher::process_signal(size_t to_do)
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];
                dsp::copy(c->vOut, c->vIn, to_do);
            }
        }

        void matcher::process_block(void *object, void *subject, float * const * spectrum, size_t rank)
        {
//            matcher *self = static_cast<matcher *>(object);
            // TODO
        }

        void matcher::process(size_t samples)
        {
            bind_buffers();

            for (size_t offset=0; offset<samples; )
            {
                const size_t to_do  = lsp_min(samples - offset, BUFFER_SIZE, sProcessor.remaining());

                // Perform processing
                sProcessor.process(to_do);

                // Update position
                offset             += to_do;
                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    c->vIn             += to_do;
                    c->vOut            += to_do;
                    if (c->vSc != NULL)
                        c->vSc             += to_do;
                    if (c->vShmIn != NULL)
                        c->vShmIn          += to_do;
                }
            }
        }

        void matcher::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            // TODO
        }

    } /* namespace plugins */
} /* namespace lsp */


