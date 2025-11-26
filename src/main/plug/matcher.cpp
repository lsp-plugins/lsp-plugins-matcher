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
        static constexpr float FFT_TIME_CONST           = -1.2279471773f; // logf(1.0f - float(M_SQRT1_2));

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

        //-------------------------------------------------------------------------
        matcher::FileLoader::FileLoader(matcher *core)
        {
            pCore       = core;
        }

        matcher::FileLoader::~FileLoader()
        {
            pCore       = NULL;
        }

        status_t matcher::FileLoader::run()
        {
            dsp::context_t ctx;
            dsp::start(&ctx);
            lsp_finally { dsp::finish(&ctx); };

            return pCore->load_audio_file(&pCore->sFile);
        }

        void matcher::FileLoader::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
        }

        //-------------------------------------------------------------------------
        matcher::FileProcessor::FileProcessor(matcher *core)
        {
            pCore       = core;
        }

        matcher::FileProcessor::~FileProcessor()
        {
            pCore       = NULL;
        }

        status_t matcher::FileProcessor::run()
        {
            dsp::context_t ctx;
            dsp::start(&ctx);
            lsp_finally { dsp::finish(&ctx); };

            return pCore->process_audio_file();
        }

        void matcher::FileProcessor::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
        }

        //-------------------------------------------------------------------------
        matcher::GCTask::GCTask(matcher *base)
        {
            pCore       = base;
        }

        matcher::GCTask::~GCTask()
        {
            pCore       = NULL;
        }

        status_t matcher::GCTask::run()
        {
            pCore->perform_gc();
            return STATUS_OK;
        }

        void matcher::GCTask::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
        }

        //---------------------------------------------------------------------
        // Implementation
        matcher::matcher(const meta::plugin_t *meta):
            Module(meta),
            sFileLoader(this),
            sFileProcessor(this),
            sGCTask(this)
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
            pExecutor       = NULL;
            pGCList         = NULL;
            pReactivity     = NULL;
            pTempProfile    = NULL;
            vIndices        = NULL;
            vBuffer         = NULL;

            nInSource       = IN_STATIC;
            nRefSource      = REF_CAPTURE;
            nCapSource      = CAP_NONE;
            nRank           = 0;
            fFftTau         = 1.0f;
            fFftShift       = GAIN_AMP_0_DB;
            fInTau          = 1.0f;
            fRefTau         = 1.0f;

            nFileProcessReq = 0;
            nFileProcessResp= 0;

            bProfile        = false;
            bCapture        = false;

            for (size_t i=0; i<meta::matcher::MATCH_BANDS; ++i)
            {
                match_band_t *b     = &vMatchBands[i];
                for (size_t j=0; j<EQP_TOTAL; ++j)
                {
                    b->vParams[j]       = 0.0f;
                    b->pParams[j]       = NULL;
                }
            }

            for (size_t i=0; i<PROF_TOTAL; ++i)
                vProfileData[i]     = NULL;

            pBypass         = NULL;
            pGainIn         = NULL;
            pGainOut        = NULL;
            pFftSize        = NULL;
            pResetIn        = NULL;
            pResetRef       = NULL;
            pResetCap       = NULL;
            pInReactivity   = NULL;
            pRefReactivity  = NULL;
            pInSource       = NULL;
            pRefSource      = NULL;
            pCapSource      = NULL;
            pProfile        = NULL;
            pCapture        = NULL;
            pListen         = NULL;
            pStereoLink     = NULL;

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

            // Remember executor service
            pExecutor       = wrapper->executor();
            lsp_trace("Executor = %p", pExecutor);

            // Estimate the number of bytes to allocate
            const size_t fft_csize      = (1 << (meta::matcher::FFT_RANK_MAX - 1)) + 1;
            const size_t szof_channels  = align_size(sizeof(channel_t) * nChannels, OPTIMAL_ALIGN);
//            const size_t buf_sz         = BUFFER_SIZE * sizeof(float);
            const size_t szof_freqs     = sizeof(float) * meta::matcher::FFT_MESH_SIZE;
            const size_t szof_idx       = sizeof(uint16_t) * meta::matcher::FFT_MESH_SIZE;
            const size_t szof_fft_buf   = align_size(sizeof(float) * fft_csize, OPTIMAL_ALIGN);
            const size_t szof_buf       = sizeof(float) * BUFFER_SIZE;
            const size_t szof_tmp_buf   = lsp_max(szof_fft_buf, szof_buf);
            const size_t szof_thumbs    = meta::matcher::SAMPLE_MESH_SIZE * sizeof(float);
            const size_t alloc          =
                szof_channels +     // vChannels
                szof_freqs +        // vFreqs
                szof_fft_buf +      // vEnvelope
                szof_idx +          // vIndices
                szof_tmp_buf +      // vBuffer
                nChannels * szof_thumbs +   // af_descriptor::vThumbs
                nChannels * (       // channel_t
                    szof_fft_buf * SM_TOTAL +   // vFft
                    szof_buf                    // vBuffer
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
                c->sPlayer.construct();
                c->sPlayback.construct();

                if (!c->sPlayer.init(1, 32))
                    return;

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

                c->vBuffer              = advance_ptr_bytes<float>(ptr, szof_buf);
            }

            // Initialize processor
            if (!sProcessor.init(3 * nChannels, meta::matcher::FFT_RANK_MAX))
                return;
            sProcessor.bind_handler(process_block, this, NULL);

            // Initialize audio file descriptor
            af_descriptor_t * const f   = &sFile;
            f->pOriginal    = NULL;
            f->pProcessed   = NULL;

            for (size_t j=0; j<nChannels; ++j)
                f->vThumbs[j]   = advance_ptr_bytes<float>(ptr, szof_thumbs);

            f->sListen.init();
            f->sStop.init();

            f->nStatus      = STATUS_UNSPECIFIED;
            f->bSync        = true;
            f->fPitch       = 0.0f;
            f->fHeadCut     = 0.0f;
            f->fTailCut     = 0.0f;

            f->fDuration    = 0.0f;

            f->pFile        = NULL;
            f->pPitch       = NULL;
            f->pHeadCut     = NULL;
            f->pTailCut     = NULL;
            f->pListen      = NULL;
            f->pStop        = NULL;
            f->pStatus      = NULL;
            f->pLength      = NULL;
            f->pThumbs      = NULL;
            f->pPlayPosition= NULL;

            lsp_assert(ptr <= &base[alloc]);

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
            BIND_PORT(pInSource);
            BIND_PORT(pRefSource);
            BIND_PORT(pCapSource);
            BIND_PORT(pProfile);
            BIND_PORT(pCapture);
            BIND_PORT(pListen);
            if (nChannels > 1)
            {
                BIND_PORT(pStereoLink);
            }

            // Bind audio file ports
            lsp_trace("Binding audio file ports");
            BIND_PORT(f->pFile);
            BIND_PORT(f->pPitch);
            BIND_PORT(f->pHeadCut);
            BIND_PORT(f->pTailCut);
            BIND_PORT(f->pListen);
            BIND_PORT(f->pStop);
            BIND_PORT(f->pStatus);
            BIND_PORT(f->pLength);
            BIND_PORT(f->pThumbs);
            BIND_PORT(f->pPlayPosition);

            // Bind bypass
            lsp_trace("Binding match equalizer");
            BIND_PORT(pMatchReset);
            BIND_PORT(pMatchImmediate);
            for (size_t i=0; i<meta::matcher::MATCH_BANDS; ++i)
            {
                match_band_t *b     = &vMatchBands[i];
                for (size_t j=0; j<EQP_TOTAL; ++j)
                    BIND_PORT(b->pParams[j]);
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
            pReactivity         = create_default_profile();
            if (pReactivity == NULL)
                return;

            pTempProfile        = create_default_profile();
            if (pReactivity == NULL)
                return;

            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t *prof    = create_default_profile();
                if (prof == NULL)
                    return;
                vProfileData[i] = prof;
            }

            for (size_t i=0; i<SPROF_TOTAL; ++i)
            {
                profile_data_t *prof    = create_default_profile();
                if (prof == NULL)
                    return;

                vProfileState[i].set_deleter(free_profile_data);
                vProfileState[i].push(prof);
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

                    dspu::Sample *gc_list = c->sPlayer.destroy(false);
                    destroy_samples(gc_list);
                }
                vChannels   = NULL;
            }

            // Drestroy samples
            destroy_sample(sFile.pOriginal);
            destroy_sample(sFile.pProcessed);

            // Free profiles
            if (pReactivity != NULL)
            {
                free_profile_data(pReactivity);
                pReactivity = NULL;
            }
            if (pTempProfile != NULL)
            {
                free_profile_data(pTempProfile);
                pTempProfile = NULL;
            }
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t *prof    = vProfileData[i];
                if (prof != NULL)
                {
                    free_profile_data(prof);
                    vProfileData[i] = NULL;
                }
            }
            for (size_t i=0; i<SPROF_TOTAL; ++i)
                vProfileState[i].flush();

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
            const float ref_react   = pRefReactivity->value();
            const size_t fft_csize  = fft_period + 1;
            bool rebuild_eq_profiles= false;

            const size_t old_ref_source = nRefSource;
            const size_t old_cap_source = nCapSource;
            nRefSource              = decode_reference_source(pRefSource->value());
            nCapSource              = decode_capture_source(pCapSource->value(), nRefSource);

            if ((old_ref_source != nRefSource) || (old_cap_source != nCapSource))
            {
                profile_data_t * const profile = vProfileData[PROF_MATCH];
                profile->nFlags        |= PFLAGS_CHANGED;
            }

            fFftTau                 = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), reactivity));
            fFftShift               = pFftShift->value() * 100.0f / float(1 << rank);
            fInTau                  = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), in_react));
            fRefTau                 = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), ref_react));

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
                rebuild_eq_profiles     = true;
                update_frequency_mapping();
            }

            // Trigger profile recordings
            const float profile_on = pProfile->value() >= 0.5f;
            if (profile_on != bProfile)
            {
                bProfile                = profile_on;
                if (profile_on)
                    clear_profile_data(vProfileState[SPROF_STATIC].get());
            }
            const float capture_on = pCapture->value() >= 0.5f;
            if (capture_on != bCapture)
            {
                bCapture                = capture_on;
                if (capture_on)
                    clear_profile_data(vProfileState[SPROF_CAPTURE].get());
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

            // Update equalizer settings
            bool eq_dirty[EQP_TOTAL];
            for (size_t i=0; i<EQP_TOTAL; ++i)
            {
                eq_dirty[i]             = rebuild_eq_profiles;

                if (i != EQP_REACTIVITY)
                {
                    for (size_t j=0; j<meta::matcher::MATCH_BANDS; ++j)
                    {
                        float *vold = &vMatchBands[j].vParams[i];
                        float vnew = dspu::db_to_gain(vMatchBands[j].pParams[i]->value());
                        if (*vold != vnew)
                        {
                            *vold           = vnew;
                            eq_dirty[i]     = true;
                        }
                    }
                }
                else
                {
                    for (size_t j=0; j<meta::matcher::MATCH_BANDS; ++j)
                    {
                        float *vold = &vMatchBands[j].vParams[i];
                        float react = vMatchBands[j].pParams[i]->value();
                        float vnew = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), react));
                        if (*vold != vnew)
                        {
                            *vold           = vnew;
                            eq_dirty[i]     = true;
                        }
                    }
                }
            }
            if (eq_dirty[EQP_REF_LEVEL])
                build_eq_profile(vProfileData[PROF_REF_EQUALIZER], EQP_REF_LEVEL);
            if (eq_dirty[EQP_MAX_AMPLIFICATION])
                build_eq_profile(vProfileData[PROF_MAX_EQUALIZER], EQP_MAX_AMPLIFICATION);
            if (eq_dirty[EQP_MAX_REDUCTION])
                build_eq_profile(vProfileData[PROF_MIN_EQUALIZER], EQP_MAX_REDUCTION);
            if (eq_dirty[EQP_REACTIVITY])
                build_eq_profile(pReactivity, EQP_REACTIVITY);

            // Update file configuration
            af_descriptor_t * const af  = &sFile;

            // Check that file parameters have changed
            const float pitch       = af->pPitch->value();
            const float head_cut    = af->pHeadCut->value();
            const float tail_cut    = af->pTailCut->value();
            if ((af->fPitch != pitch) ||
                (af->fHeadCut != head_cut) ||
                (af->fTailCut != tail_cut))
            {
                af->fPitch           = pitch;
                af->fHeadCut         = head_cut;
                af->fTailCut         = tail_cut;
                nFileProcessReq      ++;
            }

            // Listen button pressed?
            if (af->pListen != NULL)
                af->sListen.submit(af->pListen->value());
            if (af->pStop != NULL)
                af->sStop.submit(af->pStop->value());
        }

        void matcher::clear_profile_data(profile_data_t *profile)
        {
            if (profile == NULL)
                return;

            const size_t fft_csize      = (1 << (nRank - 1)) + 1;
            profile->nFlags             = PFLAGS_DIRTY | PFLAGS_CHANGED | PFLAGS_SYNC;
            profile->nFrames            = 0;
            for (size_t i=0; i<nChannels; ++i)
                dsp::fill_zero(profile->vData[i], fft_csize);
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
                sProcessor.bind_in(base + PC_INPUT, c->vIn);

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

        void matcher::process_sample_block(void *object, void *subject, float * const * spectrum, size_t rank)
        {
            profile_data_t * const profile  = static_cast<profile_data_t *>(object);
            const size_t fft_csize          = (1 << (rank - 1)) + 1;

            // nRank contains number of audio channels in the source file
            // nChannels contains number of autio channels in the profile
            for (size_t i=0; i<profile->nChannels; ++i)
                dsp::pcomplex_mod_add2(profile->vData[i], spectrum[i % profile->nRank], fft_csize);

            ++profile->nFrames;
        }

        matcher::profile_data_t *matcher::allocate_profile_data()
        {
            const size_t channels       = nChannels;
            const size_t fft_citems     = (1 << (meta::matcher::FFT_RANK_MAX - 1)) + 1;
            const size_t table_size     = sizeof(float *) * channels;
            const size_t szof_data_hdr  = align_size(sizeof(profile_data_t), DEFAULT_ALIGN);
            const size_t szof_header    = align_size(szof_data_hdr + table_size * 2, OPTIMAL_ALIGN);
            const size_t prof_data_size = align_size(sizeof(float) * fft_citems, OPTIMAL_ALIGN);

            const size_t to_alloc       = szof_header + nChannels * prof_data_size;

            // Allocate memory
            uint8_t *ptr                = static_cast<uint8_t *>(malloc(to_alloc));
            if (ptr == NULL)
                return NULL;
            lsp_guard_assert(uint8_t * const base = ptr);

            // Initialize profile data
            profile_data_t *profile     = advance_ptr_bytes<profile_data_t>(ptr, szof_header);

            profile->nSampleRate        = 0;
            profile->nChannels          = channels;
            profile->nRank              = 0;
            profile->fLoudness          = GAIN_AMP_M_INF_DB;
            profile->nFlags             = PFLAGS_NONE;
            profile->nFrames            = 0;
            profile->vData              = add_ptr_bytes<float *>(profile, szof_data_hdr);

            for (size_t i=0; i<channels; ++i)
            {
                profile->vData[i]           = advance_ptr_bytes<float>(ptr, prof_data_size);
                dsp::fill_zero(profile->vData[i], fft_citems);
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
            res->nSampleRate            = fSampleRate;
            res->nRank                  = nRank;
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

        void matcher::record_profile(profile_data_t *profile, float * const * spectrum, size_t channel)
        {
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            const size_t frames     = profile->nFrames;
            const float k           = 1.0f / (float(frames) + 1.0f);
            const float kp          = float(frames) * k;

            for (size_t i=0; i<nChannels; ++i)
            {
                const float *src        = spectrum[i * PC_TOTAL + channel];
                float *dst              = profile->vData[i];

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
            }

            // Check that profile has enough frames for processing
            profile->nSampleRate    = fSampleRate;
            profile->nRank          = nRank;
            profile->nFrames        = lsp_min(frames + 1, size_t(0x100));
            profile->nFlags        |= (profile->nFrames >= 8) ?
                PFLAGS_READY | PFLAGS_DIRTY | PFLAGS_CHANGED | PFLAGS_SYNC :
                PFLAGS_DIRTY | PFLAGS_CHANGED | PFLAGS_SYNC;
        }

        void matcher::track_profile(profile_data_t *profile, float * const * spectrum, float tau, size_t channel)
        {
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            const size_t frames     = profile->nFrames;

            for (size_t i=0; i<nChannels; ++i)
            {
                const float *src        = spectrum[i * PC_TOTAL + channel];
                float *dst              = profile->vData[i];

                // Decide the strategy
                if (frames > 0)
                {
                    // Append new frame to the profile
                    if (src != NULL)
                    {
                        dsp::pcomplex_mod(vBuffer, src, fft_csize);
                        dsp::mix2(dst, vBuffer, 1.0f - tau, tau, fft_csize);
                    }
                    else
                        dsp::mul_k2(dst, tau, fft_csize);
                }
                else if (src != NULL)
                {
                    // Fill empty profile
                    dsp::pcomplex_mod(dst, src, fft_csize);
                }
            }

            profile->nSampleRate    = fSampleRate;
            profile->nRank          = nRank;
            if (profile->nFrames < ~uint32_t(0))
                ++profile->nFrames;
            profile->nFlags        |= (profile->nFrames >= 4) ?
                PFLAGS_READY | PFLAGS_CHANGED | PFLAGS_SYNC | PFLAGS_DYNAMIC :
                PFLAGS_CHANGED | PFLAGS_SYNC | PFLAGS_DYNAMIC;
        }

        void matcher::sync_profile(profile_data_t *dst, profile_data_t *src)
        {
            if ((src == NULL) || (dst == NULL))
                return;

            // Synchronize sample rate if needed
            if (src->nFlags & PFLAGS_DEFAULT)
            {
                src->nSampleRate    = fSampleRate;
                src->nRank          = nRank;
            }

            if (!(src->nFlags & PFLAGS_CHANGED))
                return;

            const size_t fft_csize  = (1 << (src->nRank - 1)) + 1;

            dst->nSampleRate        = src->nSampleRate;
            dst->nRank              = src->nRank;
            dst->fLoudness          = src->fLoudness;
            dst->nFlags             = src->nFlags & (~PFLAGS_DYNAMIC);
            dst->nFrames            = src->nFrames;

            for (size_t i=0; i<dst->nChannels; ++i)
                dsp::copy(dst->vData[i], src->vData[i % src->nChannels], fft_csize);

            // Reset dirty flag for the original profile
            src->nFlags            &= ~PFLAGS_CHANGED;
        }

        void matcher::compute_eq_profile(profile_data_t *in, profile_data_t *ref, bool dynamic)
        {
            profile_data_t *profile = vProfileData[PROF_MATCH];
            if (profile == NULL)
                return;

            // Check that input and reference profiles are present
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            if ((in == NULL) || (ref == NULL))
            {
                profile->nFlags    &= ~PFLAGS_READY;
                return;
            }
            if (!((in->nFlags & PFLAGS_READY) && (ref->nFlags & PFLAGS_READY)))
            {
                profile->nFlags    &= ~PFLAGS_READY;
                return;
            }

            if ((in->nFlags | ref->nFlags) & PFLAGS_DYNAMIC)
            {
                // Check that temporary profile is present
                profile_data_t *tmp = pTempProfile;
                if (tmp == NULL)
                {
                    profile->nFlags    &= ~PFLAGS_READY;
                    return;
                }

                // Dynamically changing profile
                for (size_t i=0; i<nChannels; ++i)
                {
                    // Compute new profile value
                    dsp::clamp_kk2(tmp->vData[i], ref->vData[i], GAIN_AMP_M_96_DB, GAIN_AMP_P_96_DB, fft_csize);
                    dsp::clamp_kk2(vBuffer, in->vData[i], GAIN_AMP_M_96_DB, GAIN_AMP_P_96_DB, fft_csize);
                    dsp::div2(tmp->vData[i], vBuffer, fft_csize);

                    // TODO: apply minimum and maximum equalizer

                    // Apply reactivity to the changes
                    dsp::pmix_v1(profile->vData[i], tmp->vData[i], pReactivity->vData[i], fft_csize);
                }
            }
            else if ((in->nFlags | ref->nFlags | profile->nFlags) & PFLAGS_CHANGED)
            {
                // Compute new static profile value
                for (size_t i=0; i<nChannels; ++i)
                {
                    dsp::clamp_kk2(profile->vData[i], ref->vData[i], GAIN_AMP_M_96_DB, GAIN_AMP_P_96_DB, fft_csize);
                    dsp::clamp_kk2(vBuffer, in->vData[i], GAIN_AMP_M_96_DB, GAIN_AMP_P_96_DB, fft_csize);
                    dsp::div2(profile->vData[i], vBuffer, fft_csize);
                }
            }

            in->nFlags         &= ~PFLAGS_CHANGED;
            ref->nFlags        &= ~PFLAGS_CHANGED;
            profile->nFlags    &= ~(PFLAGS_CHANGED | PFLAGS_DIRTY);
            profile->nFlags    |= PFLAGS_READY | PFLAGS_SYNC;
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

            // Analyze reference channel
            ssize_t ref_channel = -1;
            ssize_t ref_profile = -1;
            bool match_dynamic  = false;
            switch (nRefSource)
            {
                case REF_CAPTURE:
                    ref_profile     = PROF_CAPTURE;
                    break;
                case REF_FILE:
                    ref_profile     = PROF_FILE;
                    break;
                case REF_EQUALIZER:
                    ref_profile     = PROF_REF_EQUALIZER;
                    break;
                case REF_SIDECHAIN:
                case REF_LINK:
                    ref_profile     = PROF_REFERENCE;
                    ref_channel     = PC_REFERENCE;
                    match_dynamic   = true;
                    break;
                default:
                    break;
            }

            // Analyze input profile
            ssize_t in_profile  = -1;
            switch (nInSource)
            {
                case IN_STATIC:
                    in_profile      = PROF_STATIC;
                    break;
                case IN_DYNAMIC:
                    in_profile      = PROF_INPUT;
                    match_dynamic   = true;
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
            if (bProfile)
            {
                profile_data_t * const static_profile  = vProfileState[SPROF_STATIC].current();
                if (static_profile != NULL)
                {
                    record_profile(static_profile, spectrum, PC_INPUT);
                    sync_profile(vProfileData[PROF_STATIC], static_profile);
                }
            }

            // Track dynamic input profile
            profile_data_t * const input_profile  = vProfileData[PROF_INPUT];
            if (input_profile != NULL)
                track_profile(input_profile, spectrum, fInTau, PC_INPUT);

            // Record capture if enabled
            if ((bCapture) && (cap_channel >= 0))
            {
                profile_data_t * const capture_profile  = vProfileState[SPROF_CAPTURE].current();
                if (capture_profile != NULL)
                {
                    record_profile(capture_profile, spectrum, cap_channel);
                    sync_profile(vProfileData[PROF_CAPTURE], capture_profile);
                }
            }

            // Track dynamic reference input profile
            if (ref_channel >= 0)
            {
                profile_data_t * const reference_profile  = vProfileData[PROF_REFERENCE];
                if (input_profile != NULL)
                    track_profile(reference_profile, spectrum, fRefTau, ref_channel);
            }

            // Compute the new profile state
            if ((ref_profile >= 0) && (in_profile >= 0))
                compute_eq_profile(vProfileData[in_profile], vProfileData[ref_profile], match_dynamic);

            // Analyze output signal
            for (size_t i=0; i<nChannels; ++i)
                analyze_spectrum(&vChannels[i], SM_OUT, spectrum[i*PC_TOTAL + PC_INPUT]);
        }

        void matcher::resample_profile(profile_data_t *profile, size_t srate)
        {
            // TODO

            profile->nSampleRate    = srate;
        }

        void matcher::update_profiles()
        {
            // Synchronize state of profiles
            sync_profile(vProfileData[PROF_STATIC], vProfileState[SPROF_STATIC].get());
            sync_profile(vProfileData[PROF_CAPTURE], vProfileState[SPROF_CAPTURE].get());
            sync_profile(vProfileData[PROF_FILE], vProfileState[SPROF_FILE].get());

            // Check that profile needs to be resampled
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i];

                if (profile->nFlags & PFLAGS_DEFAULT)
                {
                    profile->nSampleRate    = fSampleRate;
                    profile->nRank          = nRank;
                }
                else if (profile->nSampleRate != fSampleRate)
                    resample_profile(profile, fSampleRate);
            }
        }

        void matcher::smooth_eq_curve(float *dst, float x1, float y1, float x2, float y2, size_t count)
        {
            if (count < 2)
            {
                if (count == 1)
                    dst[0] = y1;
                return;
            }

            const float lx1         = logf(x1);
            const float ldx         = 1.0f / logf(x2/x1);
            const float ldy         = 2.0f * logf(y2/y1);

            for (size_t i=0; i<count; ++i)
            {
                const float f           = x1 + i;
                const float x           = (logf(f) - lx1) * ldx;
                dst[i] = y1 * expf(ldy * x*x * (1.5f - x));
            }
        }

        void matcher::build_eq_profile(profile_data_t *profile, eq_param_t param)
        {
            if (profile == NULL)
                return;

            const float fft_csize   = (1 << (nRank - 1)) + 1;
            const float kf          = (2.0f * fft_csize) / float(fSampleRate);
            float *dst              = profile->vData[0];

            // Make equalization curve
            float f_prev    = meta::matcher::eq_frequencies[0];
            float v_prev    = vMatchBands[0].vParams[param];
            size_t i_prev   = lsp_min(size_t(kf * f_prev), fft_csize);
            dsp::fill(&dst[0], v_prev, i_prev);

            for (size_t i=1; i<meta::matcher::MATCH_BANDS; ++i)
            {
                const float f_next  = meta::matcher::eq_frequencies[i];
                const size_t i_next = lsp_min(size_t(kf * f_next), fft_csize);
                const float v_next  = vMatchBands[i].vParams[param];

                smooth_eq_curve(&dst[i_prev], i_prev, v_prev, i_next, v_next, i_next - i_prev);
                f_prev              = f_next;
                i_prev              = i_next;
                v_prev              = v_next;

                if (i_next >= fft_csize)
                    break;
            }

            // Copy equalization curve
            dsp::fill(&dst[i_prev], v_prev, fft_csize - i_prev);
            for (size_t i=1; i<nChannels; ++i)
                dsp::copy(profile->vData[i], profile->vData[0], fft_csize);

            profile->nSampleRate    = fSampleRate;
            profile->nRank          = nRank;
            profile->fLoudness      = GAIN_AMP_0_DB;
            profile->nFlags         = PFLAGS_READY | PFLAGS_SYNC | PFLAGS_READY;
            profile->nFrames        = 0;
        }

        inline void matcher::sync_profile_with_state(profile_data_t * profile)
        {
            if (profile == NULL)
                return;

            if (profile->nFlags & PFLAGS_DIRTY)
            {
                profile->nFlags &= ~PFLAGS_DIRTY;
                pWrapper->state_changed();
                lsp_trace("state changed");
            }
        }

        void matcher::post_process_profiles()
        {
            sync_profile_with_state(vProfileData[PROF_STATIC]);
            sync_profile_with_state(vProfileData[PROF_CAPTURE]);
        }

        void matcher::process(size_t samples)
        {
            process_file_loading_tasks();
            process_file_processing_tasks();
            process_listen_events();
            process_gc_tasks();
            update_profiles();

            bind_buffers();

            for (size_t offset=0; offset<samples; )
            {
                const size_t to_do  = lsp_min(samples - offset, BUFFER_SIZE, sProcessor.remaining());

                // Perform processing
                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    const size_t base   = i * PC_TOTAL;
                    sProcessor.bind_out(base + PC_INPUT, c->vBuffer);
                }

                sProcessor.process(to_do);

                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    c->sPlayer.process(c->vBuffer, c->vBuffer, to_do);
                    c->sBypass.process(c->vOut, c->vIn, c->vBuffer, to_do);
                }

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

            post_process_profiles();
            output_fft_mesh_data();
            output_profile_mesh_data();
            output_file_mesh_data();
        }

        bool matcher::check_need_profile_sync()
        {
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i];
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
                for (size_t j=0; j<PROF_TOTAL; ++j)
                {
                    float *dst          = mesh->pvData[index++];

                    profile_data_t * const profile = vProfileData[j];
                    if (profile == NULL)
                    {
                        dsp::fill_zero(dst, meta::matcher::FFT_MESH_SIZE + 4);
                        continue;
                    }

                    bool has_envelope   = true;
                    switch (j)
                    {
                        case PROF_MATCH:
                        case PROF_REF_EQUALIZER:
                        case PROF_MIN_EQUALIZER:
                        case PROF_MAX_EQUALIZER:
                            has_envelope    = false;
                            break;
                        default:
                            break;
                    }

                    // Copy profile data
                    const float * const fft     = profile->vData[i];
                    dst                += 2;

                    if (has_envelope)
                    {
                        for (size_t k=0; k<meta::matcher::FFT_MESH_SIZE; ++k)
                        {
                            const size_t idx    = vIndices[k];
                            dst[k]              = fft[idx] * vEnvelope[idx];
                        }
                    }
                    else
                    {
                        for (size_t k=0; k<meta::matcher::FFT_MESH_SIZE; ++k)
                        {
                            const size_t idx    = vIndices[k];
                            dst[k]              = fft[idx];
                        }
                    }

                    dst[-2]             = 0.0f;
                    dst[-1]             = dst[0];
                    dst                += meta::matcher::FFT_MESH_SIZE;
                    dst[0]              = dst[-1];
                    dst[1]              = 0.0f;
                }
            }

            // Reset sync flag
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i];
                if (profile != NULL)
                    profile->nFlags        &= ~PFLAGS_SYNC;
            }

            // Report mesh size
            mesh->data(index, meta::matcher::FFT_MESH_SIZE + 4);
        }

        void matcher::ui_activated()
        {
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i];
                if (profile != NULL)
                    profile->nFlags    |= PFLAGS_SYNC;
            }

            // Request for file processing (if file is present)
            ++nFileProcessReq;
            sFile.bSync     = true;
        }

        void matcher::destroy_samples(dspu::Sample *gc_list)
        {
            // Iterate over the list and destroy each sample in the list
            while (gc_list != NULL)
            {
                dspu::Sample *next = gc_list->gc_next();
                destroy_sample(gc_list);
                gc_list = next;
            }
        }

        void matcher::destroy_sample(dspu::Sample * &s)
        {
            if (s == NULL)
                return;
            s->destroy();
            delete s;
            lsp_trace("Destroyed sample %p", s);
            s   = NULL;
        }

        status_t matcher::load_audio_file(af_descriptor_t *descr)
        {
            lsp_trace("descr = %p", descr);

            // Check state
            if (descr == NULL)
                return STATUS_UNKNOWN_ERR;

            // Destroy previously loaded sample
            destroy_sample(descr->pOriginal);

            // Check state
            if (descr->pFile == NULL)
                return STATUS_UNKNOWN_ERR;

            // Get path
            plug::path_t *path = descr->pFile->buffer<plug::path_t>();
            if (path == NULL)
                return STATUS_UNKNOWN_ERR;

            // Get file name
            const char *fname = path->path();
            if (strlen(fname) <= 0)
                return STATUS_UNSPECIFIED;

            // Load audio file
            dspu::Sample *af    = new dspu::Sample();
            if (af == NULL)
                return STATUS_NO_MEM;
            lsp_trace("Allocated sample %p", af);
            lsp_finally { destroy_sample(af); };

            // Try to load file
            const float sample_length_max_seconds = meta::matcher::SAMPLE_LENGTH_MAX;
            status_t status = af->load(fname,  sample_length_max_seconds);
            if (status != STATUS_OK)
            {
                lsp_trace("load failed: status=%d (%s)", status, get_status(status));
                return status;
            }

            // Determine the normalizing factor
            size_t channels         = af->channels();
            float max = 0.0f;

            for (size_t i=0; i<channels; ++i)
            {
                // Determine the maximum amplitude
                float a_max = dsp::abs_max(af->channel(i), af->samples());
                if (max < a_max)
                    max     = a_max;
            }
            descr->fNorm    = (max != 0.0f) ? 1.0f / max : 1.0f;

            // File was successfully loaded, pass result to the caller
            lsp::swap(descr->pOriginal, af);

            return STATUS_OK;
        }

        status_t matcher::preprocess_sample(af_descriptor_t *f)
        {
            // Get sample to process
            const dspu::Sample *af  = f->pOriginal;
            if (af == NULL)
                return STATUS_UNSPECIFIED;

            // Copy data of original sample to temporary sample and perform resampling if needed
            dspu::Sample temp;
            const size_t sample_rate_dst  = fSampleRate * dspu::semitones_to_frequency_shift(-f->fPitch);
            if (sample_rate_dst != af->sample_rate())
            {
                if (temp.copy(af) != STATUS_OK)
                {
                    lsp_warn("Error copying source sample");
                    return STATUS_NO_MEM;
                }
                if (temp.resample(sample_rate_dst) != STATUS_OK)
                {
                    lsp_warn("Error resampling source sample");
                    return STATUS_NO_MEM;
                }

                af          = &temp;
            }

            // Allocate new sample
            dspu::Sample *s     = new dspu::Sample();
            if (s == NULL)
                return STATUS_NO_MEM;
            lsp_trace("Allocated sample %p", s);
            lsp_finally { destroy_sample(s); };

            // Obtain new sample parameters
            const ssize_t flen  = af->samples();
            size_t channels     = lsp_min(af->channels(), nChannels);
            size_t head_cut     = dspu::seconds_to_samples(fSampleRate, f->fHeadCut);
            size_t tail_cut     = dspu::seconds_to_samples(fSampleRate, f->fTailCut);
            ssize_t fsamples    = flen - head_cut - tail_cut;
            if (fsamples <= 0)
            {
                for (size_t j=0; j<channels; ++j)
                    dsp::fill_zero(f->vThumbs[j], meta::matcher::SAMPLE_MESH_SIZE);
                s->set_length(0);
            }

            // Now ensure that we have enough space for sample
            if (!s->init(channels, flen, fsamples))
                return STATUS_NO_MEM;

            // Copy sample data and render the file thumbnail
            for (size_t i=0; i<channels; ++i)
            {
                float *dst = s->channel(i);
                const float *src = af->channel(i);

                // Copy sample data
                dsp::copy(dst, &src[head_cut], fsamples);

                // Now render thumbnail
                src                 = dst;
                dst                 = f->vThumbs[i];
                for (size_t k=0; k<meta::matcher::SAMPLE_MESH_SIZE; ++k)
                {
                    size_t first    = (k * fsamples) / meta::matcher::SAMPLE_MESH_SIZE;
                    size_t last     = ((k + 1) * fsamples) / meta::matcher::SAMPLE_MESH_SIZE;
                    if (first < last)
                        dst[k]          = dsp::abs_max(&src[first], last - first);
                    else
                        dst[k]          = fabs(src[first]);
                }

                // Normalize graph if possible
                if (f->fNorm != 1.0f)
                    dsp::mul_k2(dst, f->fNorm, meta::matcher::SAMPLE_MESH_SIZE);
            }

            // Commit sample to the processed list
            lsp::swap(f->pProcessed, s);
            f->fDuration        = dspu::samples_to_seconds(fSampleRate, flen);

            return STATUS_OK;
        }

        status_t matcher::profile_sample(af_descriptor_t *f)
        {
            const dspu::Sample * const s = f->pProcessed;
            const size_t channels       = s->channels();

            // Allocate profile
            profile_data_t *profile     = allocate_profile_data();
            if (profile == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free_profile_data(profile); };

            profile->nSampleRate        = fSampleRate;
            profile->nRank              = channels; // Store number of channels of original file here
            profile->fLoudness          = 0.0f;
            profile->nFlags             = PFLAGS_READY | PFLAGS_CHANGED | PFLAGS_SYNC;
            profile->nFrames            = 0;

            // Create and initialize spectral processor
            dspu::MultiSpectralProcessor    processor;
            if (!processor.init(nChannels, nRank))
                return STATUS_NO_MEM;

            processor.set_rank(nRank);
            processor.bind_handler(process_sample_block, profile, NULL);
            for (size_t i=0; i<nChannels; ++i)
                processor.bind_in(i, s->channel(i % channels));

            // Process the signal
            processor.process(s->length());
            const size_t remaining      = (profile->nFrames > 0) ? processor.remaining() : processor.frame_size();
            if (remaining > 0)
            {
                const size_t to_alloc       = align_size(sizeof(float) * remaining, OPTIMAL_ALIGN);
                float *ptr                  = static_cast<float *>(malloc(to_alloc));
                if (ptr == NULL)
                    return STATUS_NO_MEM;
                lsp_finally { free(ptr); };
                dsp::fill_zero(ptr, to_alloc / sizeof(float));

                for (size_t i=0; i<channels; ++i)
                    processor.bind_in(i, ptr);
                processor.process(remaining);
            }

            // Destroy the processor
            processor.destroy();

            // Finalize and publish profile
            const size_t fft_csize      = (1 << (nRank - 1)) + 1;
            profile->nRank              = nRank;
            const float k               = 1.0f / float(profile->nFrames);
            for (size_t i=0; i<profile->nChannels; ++i)
                dsp::mul_k2(profile->vData[i], k, fft_csize);

            // Commit profile
            vProfileState[SPROF_FILE].push(profile);
            profile     = NULL;

            return STATUS_OK;
        }

        status_t matcher::process_audio_file()
        {
            // Destroy previously processed sample
            destroy_sample(sFile.pProcessed);

            // Pre-process sample
            status_t res        = preprocess_sample(&sFile);
            if (res != STATUS_OK)
                return res;

            // Make profile of the sample
            res                 = profile_sample(&sFile);
            if (res != STATUS_OK)
                return res;

            return STATUS_OK;
        }

        void matcher::process_file_loading_tasks()
        {
            // Do nothing with loading while configurator is active
            if (!sFileProcessor.idle())
                return;

            af_descriptor_t * const af     = &sFile;
            if (af->pFile == NULL)
                return;

            // Get path and check task state
            if (sFileLoader.idle())
            {
                // Get path
                plug::path_t *path      = af->pFile->buffer<plug::path_t>();
                if ((path != NULL) && (path->pending()))
                {
                    // Try to submit task
                    if (pExecutor->submit(&sFileLoader))
                    {
                        lsp_trace("Successfully submitted load task for file");
                        af->nStatus         = STATUS_LOADING;
                        path->accept();
                    }
                }
            }
            else if (sFileLoader.completed())
            {
                plug::path_t *path = af->pFile->buffer<plug::path_t>();
                if ((path != NULL) && (path->accepted()))
                {
                    // Update file status and set re-rendering flag
                    af->nStatus         = sFileLoader.code();
                    ++nFileProcessReq;

                    // Now we surely can commit changes and reset task state
                    path->commit();
                    sFileLoader.reset();
                }
            }
        }

        void matcher::process_file_processing_tasks()
        {
            // Do nothing if file load is in progress
            if (!sFileLoader.idle())
                return;

            // Check the status and look for a job
            if ((nFileProcessReq != nFileProcessResp) && (sFileProcessor.idle()))
            {
                // Try to submit task
                if (pExecutor->submit(&sFileProcessor))
                {
                    // Clear render state and reconfiguration request
                    nFileProcessResp    = nFileProcessReq;
                    lsp_trace("Successfully submitted file processing task");
                }
            }
            else if (sFileProcessor.completed())
            {
                // Bind processed samples to the sampler
                af_descriptor_t * const f   = &sFile;
                for (size_t i=0; i<nChannels; ++i)
                    vChannels[i].sPlayer.bind(0, f->pProcessed);
                f->pProcessed   = NULL;
                f->bSync        = true;

                // Reset configurator task
                sFileProcessor.reset();
                pWrapper->state_changed();
            }
        }

        void matcher::process_gc_tasks()
        {
            if (sGCTask.completed())
                sGCTask.reset();

            if (sGCTask.idle())
            {
                // Obtain the list of samples for destroy
                if (pGCList == NULL)
                {
                    for (size_t i=0; i<nChannels; ++i)
                        if ((pGCList = vChannels[i].sPlayer.gc()) != NULL)
                            break;
                }
                if (pGCList != NULL)
                    pExecutor->submit(&sGCTask);
            }
        }

        void matcher::process_listen_events()
        {
            const size_t fadeout = dspu::millis_to_samples(fSampleRate, 5.0f);
            dspu::PlaySettings ps;

            af_descriptor_t * const f  = &sFile;

            // Need to start audio preview playback?
            if (f->sListen.pending())
            {
                lsp_trace("Submitted listen toggle");
                dspu::Sample *s = vChannels[0].sPlayer.get(0);
                const size_t n_c = (s != NULL) ? s->channels() : 0;
                if (n_c > 0)
                {
                    for (size_t j=0; j<nChannels; ++j)
                    {
                        channel_t *c = &vChannels[j];
                        ps.set_channel(0, j % n_c);
                        ps.set_playback(0, 0, GAIN_AMP_0_DB);

                        c->sPlayback.cancel(fadeout, 0);
                        c->sPlayback = c->sPlayer.play(&ps);
                    }
                }
                f->sListen.commit();
            }

            // Need to cancel audio preview playback?
            if (f->sStop.pending())
            {
                for (size_t j=0; j<nChannels; ++j)
                {
                    channel_t *c = &vChannels[j];
                    c->sPlayback.cancel(fadeout, 0);
                }
                f->sStop.commit();
            }
        }

        void matcher::output_file_mesh_data()
        {
            // Do not output meshes until configuration finishes
            if (!sFileProcessor.idle())
                return;
            // Do nothing if loader task is active
            if (!sFileLoader.idle())
                return;

            af_descriptor_t * const af     = &sFile;

            // Output information about the file
            dspu::Sample *active    = vChannels[0].sPlayer.get(0);
            size_t channels         = (active != NULL) ? active->channels() : 0;
            channels                = lsp_min(channels, nChannels);

            // Output activity indicator
            const float duration    = (af->pOriginal != NULL) ? af->fDuration : 0.0f;
            af->pLength->set_value(duration);
            af->pStatus->set_value(af->nStatus);

            const ssize_t ppos      = vChannels[0].sPlayback.position();
            const float s_ppos      = (ppos >= 0) ? dspu::samples_to_seconds(fSampleRate, ppos) : -1.0f;
            af->pPlayPosition->set_value(s_ppos);

            // Store file dump to mesh if it is ready
            if (af->bSync)
            {
                plug::mesh_t *mesh      = af->pThumbs->buffer<plug::mesh_t>();
                if ((mesh != NULL) && (mesh->isEmpty()))
                {
                    if (channels > 0)
                    {
                        // Copy thumbnails
                        for (size_t j=0; j<channels; ++j)
                            dsp::copy(mesh->pvData[j], af->vThumbs[j], meta::matcher::SAMPLE_MESH_SIZE);
                        mesh->data(channels, meta::matcher::SAMPLE_MESH_SIZE);
                    }
                    else
                        mesh->data(0, 0);
                }

                // Reset synchronization flag
                af->bSync           = false;
            }
        }

        void matcher::perform_gc()
        {
            dspu::Sample *gc_list = lsp::atomic_swap(&pGCList, NULL);
            destroy_samples(gc_list);
        }

        void matcher::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            // TODO
        }

    } /* namespace plugins */
} /* namespace lsp */


