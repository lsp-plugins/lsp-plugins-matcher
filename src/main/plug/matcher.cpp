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
            pEqProfile      = NULL;
            vIndices        = NULL;
            vBuffer         = NULL;

            bProfile        = false;
            bCapture        = false;

            nRefSource      = REF_CAPTURE;
            nCapSource      = CAP_NONE;
            nRank           = 0;
            fFftTau         = 1.0f;
            fFftShift       = GAIN_AMP_0_DB;
            fInTau          = 1.0f;

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
                szof_freqs +        // vFreqs
                szof_fft_buf +      // vEnvelope
                szof_idx +          // vIndices
                szof_tmp_buf +      // vBuffer
                nChannels * (       // channel_t
                    szof_fft_buf * SM_TOTAL     // vFft
                );

            // Allocate memory-aligned data
            uint8_t *ptr            = alloc_aligned<uint8_t>(pData, alloc, OPTIMAL_ALIGN);
            if (ptr == NULL)
                return;
            lsp_guard_assert(const uint8_t *base = ptr);

            // Initialize pointers to channels and temporary buffer
            vChannels               = advance_ptr_bytes<channel_t>(ptr, szof_channels);
            vFreqs                  = advance_ptr_bytes<float>(ptr, szof_freqs);
            vEnvelope               = advance_ptr_bytes<float>(ptr, szof_fft_buf);
            vIndices                = advance_ptr_bytes<uint16_t>(ptr, szof_idx);
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

            lsp_assert(ptr <= &base[alloc]);

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

            // Create empty profiles with highest resolution
            profile_data_t *prof    = create_default_profile();
            if (prof == NULL)
                return;
            pEqProfile           = prof;

            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                prof    = create_default_profile();
                if (prof == NULL)
                    return;

                vProfileData[i].set_deleter(free_profile_data);
                vProfileData[i].push(prof);
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

            // Free profiles
            if (pEqProfile != NULL)
            {
                free_profile_data(pEqProfile);
                pEqProfile = NULL;
            }
            for (size_t i=0; i<PROF_TOTAL; ++i)
                vProfileData[i].flush();

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

        uint32_t matcher::decode_capture_source(size_t cap, size_t ref) const
        {
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
            const float in_react    = pInReactivity->value();
            const size_t fft_csize  = fft_period + 1;

            nRefSource              = decode_reference_source(pRefSource->value());
            nCapSource              = decode_capture_source(pCapSource->value(), nRefSource);
            fFftTau                 = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), reactivity));
            fFftShift               = pFftShift->value() * 100.0f / float(1 << rank);
            fInTau                  = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), in_react));

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

            // Trigger profile recordings
            const float profile_on = pProfile->value() >= 0.5f;
            if (profile_on != bProfile)
            {
                bProfile                = profile_on;
                if (profile_on)
                    clear_profile_data(vProfileData[PROF_INPUT].get());
            }
            const float capture_on = pCapture->value() >= 0.5f;
            if (capture_on != bCapture)
            {
                bCapture                = capture_on;
                if (capture_on)
                    clear_profile_data(vProfileData[PROF_CAPTURE].get());
            }

            // Update channel configuration
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

        void matcher::clear_profile_data(profile_data_t *profile)
        {
            if (profile == NULL)
                return;

            const size_t fft_csize      = (1 << (nRank - 1)) + 1;
            profile->nFlags             = PFLAGS_DIRTY | PFLAGS_SYNC;
            profile->nFrames            = 0;
            for (size_t i=0; i<nChannels; ++i)
                dsp::fill_zero(profile->vOriginData[i], fft_csize);
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

        matcher::profile_data_t *matcher::allocate_profile_data()
        {
            const size_t channels       = nChannels;
            const size_t fft_citems     = (1 << (meta::matcher::FFT_RANK_MAX - 1)) + 1;
            const size_t table_size     = sizeof(float *) * channels;
            const size_t szof_data_hdr  = align_size(sizeof(profile_data_t), DEFAULT_ALIGN);
            const size_t szof_header    = align_size(szof_data_hdr + table_size * 2, OPTIMAL_ALIGN);
            const size_t prof_data_size = align_size(sizeof(float) * fft_citems, OPTIMAL_ALIGN);

            const size_t to_alloc       = szof_header + nChannels * (prof_data_size + prof_data_size);

            // Allocate memory
            uint8_t *ptr                = static_cast<uint8_t *>(malloc(to_alloc));
            if (ptr == NULL)
                return NULL;
            lsp_guard_assert(uint8_t * const base = ptr);

            // Initialize profile data
            profile_data_t *profile     = advance_ptr_bytes<profile_data_t>(ptr, szof_header);
            profile->nOriginRate        = 0;
            profile->nActualRate        = 0;
            profile->nOriginRank        = 0;
            profile->nActualRank        = 0;
            profile->fLoudness          = GAIN_AMP_M_INF_DB;
            profile->nFlags             = PFLAGS_NONE;
            profile->nFrames            = 0;
            profile->vOriginData        = add_ptr_bytes<float *>(profile, szof_data_hdr);
            profile->vActualData        = &profile->vOriginData[channels];

            for (size_t i=0; i<channels; ++i)
            {
                profile->vOriginData[i]     = advance_ptr_bytes<float>(ptr, prof_data_size);
                dsp::fill(profile->vOriginData[i], GAIN_AMP_0_DB, fft_citems);
            }

            for (size_t i=0; i<channels; ++i)
            {
                profile->vActualData[i]     = advance_ptr_bytes<float>(ptr, prof_data_size);
                dsp::fill(profile->vActualData[i], GAIN_AMP_0_DB, fft_citems);
            }

            lsp_assert(ptr <= &base[to_alloc]);

            return profile;
        }

        matcher::profile_data_t *matcher::create_default_profile()
        {
            profile_data_t *res = allocate_profile_data();
            if (res == NULL)
                return res;

            res->nFlags                 = PFLAGS_DEFAULT;
            res->nOriginRate            = fSampleRate;
            res->nOriginRank            = nRank;
            res->nActualRank            = nRank;
            return res;
        }

        void matcher::free_profile_data(profile_data_t *profile)
        {
            free(profile);
        }

        void matcher::analyze_spectrum(channel_t *c, sig_meters_t meter, const float *fft)
        {
            if (!c->bFft[meter])
                return;

            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            float * const dst       = c->vFft[meter];

            if (fft != NULL)
            {
                dsp::pcomplex_mod(vBuffer, fft, fft_csize);
                dsp::mix2(dst, vBuffer, 1.0f - fFftTau, fFftTau, fft_csize);
            }
            else
                dsp::mul_k2(dst, 1.0f - fFftTau, fft_csize);
        }

        void matcher::capture_profile(profile_data_t *profile, float * const * spectrum, size_t channel)
        {
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            const size_t frames     = profile->nFrames + 1;
            const float k           = 1.0f / float(frames);
            const float kp          = float(frames - 1) * k;

            for (size_t i=0; i<nChannels; ++i)
            {
                const float *src        = spectrum[i * PC_TOTAL + channel];
                float *dst              = profile->vOriginData[i];

                // Decide the strategy
                if (frames > 0)
                {
                    // Append new frame to the profile
                    if (src != NULL)
                    {
                        dsp::pcomplex_mod(vBuffer, src, fft_csize);
                        dsp::mix2(dst, vBuffer, kp, k, fft_csize);
                    }
                    else
                        dsp::mul_k2(dst, kp, fft_csize);
                }
                else if (src != NULL)
                {
                    // Fill empty profile
                    dsp::pcomplex_mod(dst, src, fft_csize);
                }

                // Actualize profile
                dsp::copy(profile->vActualData[i], dst, fft_csize);
            }

            // Check that profile has enough frames for processing
            profile->nFrames        = lsp_min(frames, size_t(0x100));
            profile->nFlags        |= (profile->nFrames >= 8) ?
                PFLAGS_READY | PFLAGS_DIRTY | PFLAGS_SYNC :
                PFLAGS_DIRTY | PFLAGS_SYNC;
        }

        void matcher::process_block(float * const * spectrum, size_t rank)
        {
            // Analyze capture signal channel
            ssize_t cap_channel = -1;
            switch (nCapSource)
            {
                case CAP_SIDECHAIN:
                case CAP_LINK:
                    cap_channel = PC_CAPTURE;
                    break;
                case CAP_INPUT:
                    cap_channel = PC_INPUT;
                    break;
                case CAP_REFERENCE:
                    cap_channel = PC_REFERENCE;
                    break;
                default:
                    break;
            }

            // Analyze input signal
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c = &vChannels[i];
                const size_t base = i*PC_TOTAL;

                analyze_spectrum(c, SM_IN, spectrum[base + PC_INPUT]);
                analyze_spectrum(c, SM_REFERENCE, spectrum[base + PC_REFERENCE]);
                analyze_spectrum(c, SM_CAPTURE, (cap_channel >= 0) ? spectrum[base + cap_channel] : NULL);
            }

            // Record input profile if enabled
            profile_data_t * const in_profile  = vProfileData[PROF_INPUT].current();
            if ((bProfile) && (in_profile != NULL))
                capture_profile(in_profile, spectrum, PC_INPUT);

            // Record capture if enabled
            profile_data_t * const cap_profile  = vProfileData[PROF_CAPTURE].current();
            if ((bCapture) && (cap_channel >= 0) && (cap_profile != NULL))
                capture_profile(cap_profile, spectrum, cap_channel);

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
            output_profile_mesh_data();
        }

        bool matcher::check_need_profile_sync()
        {
            if ((pEqProfile != NULL) && (pEqProfile->nFlags & PFLAGS_SYNC))
                return true;
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i].current();
                if ((profile != NULL) && (profile->nFlags & PFLAGS_SYNC))
                    return true;
            }
            return false;
        }

        void matcher::output_fft_mesh_data()
        {
            plug::mesh_t * mesh = (pFftMesh != NULL) ? pFftMesh->buffer<plug::mesh_t>() : NULL;
            if ((mesh == NULL) || (!mesh->isEmpty()))
                return;

            size_t index        = 0;

            // Frequencies
            float *ptr          = mesh->pvData[index++];
            ptr[0]              = SPEC_FREQ_MIN * 0.5f;
            ptr[1]              = ptr[0];
            dsp::copy(&ptr[2], vFreqs, meta::matcher::FFT_MESH_SIZE);
            ptr                += meta::matcher::FFT_MESH_SIZE + 2;
            ptr[0]              = SPEC_FREQ_MAX * 2.0f;
            ptr[1]              = ptr[0];

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

        void matcher::output_profile_mesh_data()
        {
            if (!check_need_profile_sync())
                return;

            plug::mesh_t * mesh = (pMatchMesh != NULL) ? pMatchMesh->buffer<plug::mesh_t>() : NULL;
            if ((mesh == NULL) || (!mesh->isEmpty()))
                return;

            size_t index        = 0;

            // Frequencies
            float *ptr          = mesh->pvData[index++];
            dsp::copy(&ptr[2], vFreqs, meta::matcher::FFT_MESH_SIZE);
            ptr[0]              = SPEC_FREQ_MIN * 0.5f;
            ptr[1]              = ptr[0];
            ptr                += meta::matcher::FFT_MESH_SIZE + 2;
            ptr[0]              = SPEC_FREQ_MAX * 2.0f;
            ptr[1]              = ptr[0];

            // Mesh data
            for (size_t i=0; i<nChannels; ++i)
            {
                output_profile_mesh(mesh->pvData[index++], pEqProfile, i, false);
                for (size_t j=0; j<PROF_TOTAL; ++j)
                    output_profile_mesh(mesh->pvData[index++], vProfileData[j].current(), i, j != PROF_EQUALIZER);
            }

            // Reset sync flags
            if (pEqProfile != NULL)
                pEqProfile->nFlags     &= ~PFLAGS_SYNC;
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i].current();
                if (profile != NULL)
                    profile->nFlags        &= ~PFLAGS_SYNC;
            }

            // Report mesh size
            mesh->data(index, meta::matcher::FFT_MESH_SIZE + 4);
        }

        void matcher::output_profile_mesh(float *dst, const profile_data_t *profile, size_t channel, bool envelope)
        {
            // Is profile present?
            if (profile == NULL)
            {
                dsp::fill(dst, GAIN_AMP_0_DB, meta::matcher::FFT_MESH_SIZE + 4);
                return;
            }

            const float * const fft = profile->vActualData[channel];

            dst                += 2;

            if (envelope)
            {
                for (size_t i=0; i<meta::matcher::FFT_MESH_SIZE; ++i)
                {
                    const size_t idx    = vIndices[i];
                    dst[i]              = fft[idx] * vEnvelope[idx];
                }
            }
            else
            {
                for (size_t i=0; i<meta::matcher::FFT_MESH_SIZE; ++i)
                {
                    const size_t idx    = vIndices[i];
                    dst[i]              = fft[idx];
                }
            }

            dst[-2]             = 0.0f;
            dst[-1]             = dst[0];
            dst                += meta::matcher::FFT_MESH_SIZE;
            dst[0]              = dst[-1];
            dst[1]              = 0.0f;
        }

        void matcher::ui_activated()
        {
            if (pEqProfile != NULL)
                pEqProfile->nFlags |= PFLAGS_SYNC;
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i].current();
                if (profile != NULL)
                    profile->nFlags    |= PFLAGS_SYNC;
            }
        }

        void matcher::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            // TODO
        }

    } /* namespace plugins */
} /* namespace lsp */


