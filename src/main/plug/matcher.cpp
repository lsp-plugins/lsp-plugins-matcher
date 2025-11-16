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
#include <lsp-plug.in/dsp-units/misc/envelope.h>
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
        static constexpr float FFT_TIME_CONST           = logf(1.0f - float(M_SQRT1_2));

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
            vFreqs          = NULL;
            vEnvelope       = NULL;
            vIndices        = NULL;
            vBuffer         = NULL;

            nRefSource      = REF_CAPTURE;
            nCapSource      = CAP_NONE;
            nRank           = 0;
            fFftTau         = 1.0f;
            fFftShift       = GAIN_AMP_0_DB;

            for (size_t i=0; i<meta::matcher::MATCH_BANDS; ++i)
            {
                match_band_t *b     = &vMatchBands[i];
                b->pMaxAmp          = NULL;
                b->pMaxRed          = NULL;
                b->pReact           = NULL;
            }

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

            pMatchMode      = NULL;
            pMatchReset     = NULL;
            pMatchImmediate = NULL;
            pMatchMesh      = NULL;

            pFftReact       = NULL;
            pFftShift       = NULL;
            pFftMesh        = NULL;

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
            const size_t fft_csize      = (1 << (meta::matcher::FFT_RANK_MAX - 1)) + 1;
            const size_t szof_channels  = align_size(sizeof(channel_t) * nChannels, OPTIMAL_ALIGN);
//            const size_t buf_sz         = BUFFER_SIZE * sizeof(float);
            const size_t szof_freqs     = sizeof(float) * meta::matcher::FFT_MESH_SIZE;
            const size_t szof_idx       = sizeof(uint16_t) * meta::matcher::FFT_MESH_SIZE;
            const size_t szof_fft_buf   = align_size(sizeof(float) * fft_csize, OPTIMAL_ALIGN);
            const size_t szof_tmp_buf   = lsp_max(szof_fft_buf, sizeof(float) * BUFFER_SIZE);
            const size_t alloc          =
                szof_channels +     // vChannels
                szof_freqs + // vFreqs
                szof_fft_buf + // vEnvelope
                szof_idx + // vIndices
                szof_tmp_buf + // vBuffer
                nChannels * ( // channel_t
                    szof_fft_buf * SM_TOTAL     // vFft
                );

            // Allocate memory-aligned data
            uint8_t *ptr            = alloc_aligned<uint8_t>(pData, alloc, OPTIMAL_ALIGN);
            if (ptr == NULL)
                return;

            // Initialize pointers to channels and temporary buffer
            vChannels               = advance_ptr_bytes<channel_t>(ptr, szof_channels);
            vFreqs                  = advance_ptr_bytes<float>(ptr, szof_freqs);
            vEnvelope               = advance_ptr_bytes<float>(ptr, szof_fft_buf);
            vIndices                = advance_ptr_bytes<uint16_t>(ptr, szof_freqs);
            vBuffer                 = advance_ptr_bytes<float>(ptr, szof_tmp_buf);

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

                for (size_t j=0; j<SM_TOTAL; ++j)
                {
                    c->vFft[j]              = advance_ptr_bytes<float>(ptr, szof_fft_buf);
                    dsp::fill_zero(c->vFft[j], szof_fft_buf / sizeof(float));

                    c->bFft[j]              = false;
                    c->pFft[j]              = NULL;
                    c->pMeter[j]            = NULL;
                }
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
            lsp_trace("Binding common ports");
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

            // Bind bypass
            lsp_trace("Binding match equalizer");
            BIND_PORT(pMatchMode);
            BIND_PORT(pMatchReset);
            BIND_PORT(pMatchImmediate);
            for (size_t i=0; i<meta::matcher::MATCH_BANDS; ++i)
            {
                match_band_t *b     = &vMatchBands[i];
                BIND_PORT(b->pMaxAmp);
                BIND_PORT(b->pMaxRed);
                BIND_PORT(b->pReact);
            }
            BIND_PORT(pMatchMesh);

            // Bind meters
            lsp_trace("Binding meter parameters");
            BIND_PORT(pFftReact);
            BIND_PORT(pFftShift);
            BIND_PORT(pFftMesh);

            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];

                BIND_PORT(c->pFft[SM_IN]);
                BIND_PORT(c->pFft[SM_OUT]);
                BIND_PORT(c->pFft[SM_CAPTURE]);
                BIND_PORT(c->pFft[SM_REFERENCE]);
                BIND_PORT(c->pMeter[SM_IN]);
                BIND_PORT(c->pMeter[SM_OUT]);
                BIND_PORT(c->pMeter[SM_CAPTURE]);
                BIND_PORT(c->pMeter[SM_REFERENCE]);
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

            // Force frequency mapping to be updated
            nRank           = 0;
        }

        void matcher::update_frequency_mapping()
        {
            const size_t fft_size   = 1 << nRank;
            const size_t fft_width  = (fft_size >> 1);
            const size_t fft_csize  = fft_width + 1;
            const float scale       = float(fft_size) / float(fSampleRate);
            const float norm        = logf(SPEC_FREQ_MAX/SPEC_FREQ_MIN) / (meta::matcher::FFT_MESH_SIZE - 1);

            for (size_t i=0; i<meta::matcher::FFT_MESH_SIZE; ++i)
            {
                const float f           = SPEC_FREQ_MIN * expf(i * norm);

                vFreqs[i]               = f;
                vIndices[i]             = uint16_t(lsp_min(scale * f, fft_width));
            }

            // Blue noise is reverse version of pink noise
            dspu::envelope::reverse_noise_lin(
                vEnvelope,
                0, fSampleRate * 0.5f, SPEC_FREQ_CENTER,
                fft_csize,
                dspu::envelope::PINK_NOISE);
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
            const float reactivity  = pFftReact->value();
            const size_t fft_period = (1 << (rank - 1));

            nRefSource              = decode_reference_source(pRefSource->value());
            nCapSource              = decode_capture_source(pCapSource->value(), pCapture->value() >= 0.5f, nRefSource);
            fFftTau                 = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), reactivity));
            fFftShift               = pFftShift->value() * 100.0f / float(1 << rank);

            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];
                c->sBypass.set_bypass(bypass);
            }

            // Apply FFT rank
            sProcessor.set_rank(rank);
            if (rank != nRank)
            {
                nRank                   = rank;
                update_frequency_mapping();
            }


            // Update channel configuration
            const size_t fft_csize  = fft_period + 1;
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];
                for (size_t j=0; j<SM_TOTAL; ++j)
                {
                    bool fft        = c->pFft[j]->value() >= 0.5f;
                    if ((fft) && (j == SM_CAPTURE))
                        fft             = nCapSource != CAP_NONE;

                    if (c->bFft[j] != fft)
                    {
                        dsp::fill_zero(c->vFft[j], fft_csize);
                        c->bFft[j]      = fft;
                    }
                }
            }
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
            matcher *self = static_cast<matcher *>(object);
            self->process_block(spectrum, rank);
        }

        void matcher::analyze_spectrum(channel_t *c, sig_meters_t meter, const float *fft)
        {
            if (!c->bFft[meter])
                return;
            if (fft == NULL)
                return;

            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            dsp::pcomplex_mod(vBuffer, fft, fft_csize);
            dsp::mix2(c->vFft[meter], vBuffer, 1.0f - fFftTau, fFftTau, fft_csize);
        }

        void matcher::process_block(float * const * spectrum, size_t rank)
        {
            // Analyze input signal
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c = &vChannels[i];
                const size_t base = i*PC_TOTAL;

                analyze_spectrum(c, SM_IN, spectrum[base + PC_INPUT]);
                analyze_spectrum(c, SM_REFERENCE, spectrum[base + PC_REFERENCE]);

                // Analyze capture signal (if present)
                switch (nCapSource)
                {
                    case CAP_SIDECHAIN:
                    case CAP_LINK:
                        analyze_spectrum(c, SM_CAPTURE, spectrum[base + PC_CAPTURE]);
                        break;
                    case CAP_INPUT:
                        analyze_spectrum(c, SM_CAPTURE, spectrum[base + PC_INPUT]);
                        break;
                    case CAP_REFERENCE:
                        analyze_spectrum(c, SM_CAPTURE, spectrum[base + PC_REFERENCE]);
                        break;
                    default:
                        break;
                }
            }

            // TODO: perform processing

            // Analyze output signal
            for (size_t i=0; i<nChannels; ++i)
                analyze_spectrum(&vChannels[i], SM_OUT, spectrum[i*PC_TOTAL + PC_INPUT]);
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

            output_fft_mesh_data();
        }

        void matcher::output_fft_mesh_data()
        {
            plug::mesh_t * mesh = (pFftMesh != NULL) ? pFftMesh->buffer<plug::mesh_t>() : NULL;
            if ((mesh == NULL) || (!mesh->isEmpty()))
                return;

            size_t index        = 0;

            // Frequencies
            float *ptr          = mesh->pvData[index++];
            dsp::copy(&ptr[2], vFreqs, meta::matcher::FFT_MESH_SIZE);
            ptr[0]              = SPEC_FREQ_MIN * 0.5f;
            ptr[1]              = ptr[2];
            ptr                += meta::matcher::FFT_MESH_SIZE + 2;
            ptr[0]              = ptr[-1];
            ptr[1]              = SPEC_FREQ_MAX * 2.0f;

            // Mesh data
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];

                for (size_t j=0; j<SM_TOTAL; ++j)
                {
                    ptr                 = mesh->pvData[index++];

                    if (c->bFft[j])
                    {
                        const float *fft    = c->vFft[j];

                        ptr[0]              = 0.0f;
                        ptr[1]              = 0.0f;
                        ptr                += 2;

                        for (size_t k=0; k<meta::matcher::FFT_MESH_SIZE; ++k)
                        {
                            const size_t idx    = vIndices[k];
                            ptr[k]              = fft[idx] * vEnvelope[idx] * fFftShift;
                        }

                        ptr                += meta::matcher::FFT_MESH_SIZE;
                        ptr[0]              = 0.0f;
                        ptr[1]              = 0.0f;
                    }
                    else
                        dsp::fill_zero(ptr, meta::matcher::FFT_MESH_SIZE + 4);
                }
            }

            // Report mesh size
            mesh->data(index, meta::matcher::FFT_MESH_SIZE + 4);
        }

        void matcher::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            // TODO
        }

    } /* namespace plugins */
} /* namespace lsp */


