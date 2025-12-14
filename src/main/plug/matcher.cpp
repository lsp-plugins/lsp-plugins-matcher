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
#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/misc/envelope.h>
#include <lsp-plug.in/dsp-units/misc/fft_crossover.h>
#include <lsp-plug.in/dsp-units/misc/windows.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/plug-fw/core/AudioBuffer.h>
#include <lsp-plug.in/plug-fw/core/KVTStorage.h>
#include <lsp-plug.in/plug-fw/meta/func.h>
#include <lsp-plug.in/shared/debug.h>

#include <private/plugins/matcher.h>

namespace lsp
{
    namespace plugins
    {
        /* The size of temporary buffer for audio processing */
        static const char *profile_blob_ctype           = "application/x-lsp-fft-profile";

        static constexpr size_t BUFFER_SIZE             = 0x200;
        static constexpr float FFT_TIME_CONST           = -1.2279471773f; // logf(1.0f - float(M_SQRT1_2));
        static constexpr float NORMING_SHIFT            = 100.0f;

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
        matcher::KVTSync::KVTSync(matcher *core)
        {
            pCore       = core;
            nChanges    = 0;

            for (size_t i=0; i<SPROF_TOTAL; ++i)
                vProfiles[i] = NULL;
        }

        matcher::KVTSync::~KVTSync()
        {
            // Unbind self as listener
            core::KVTStorage *kvt = pCore->kvt_lock();
            if (kvt != NULL)
            {
                lsp_finally { pCore->kvt_release(); };
                kvt->unbind(this);
            }

            for (size_t i=0; i<SPROF_TOTAL; ++i)
                free_profile_data(vProfiles[i]);

            pCore       = NULL;
        }

        status_t matcher::KVTSync::run()
        {
            dsp::context_t ctx;
            dsp::start(&ctx);
            lsp_finally { dsp::finish(&ctx); };

            // Lock the KVT storage
            core::KVTStorage *kvt = pCore->kvt_lock();
            if (kvt == NULL)
                return STATUS_UNKNOWN_ERR;
            lsp_finally { pCore->kvt_release(); };

            // Save profiles
            pCore->save_profile(kvt, "/profile/static", vProfiles[SPROF_STATIC]);
            pCore->save_profile(kvt, "/profile/capture", vProfiles[SPROF_CAPTURE]);

            // Reset change counter
            nChanges        = 0;

            return STATUS_OK;
        }

        status_t matcher::KVTSync::init()
        {
            // Allocate profiles
            for (size_t i=0; i<SPROF_TOTAL; ++i)
            {
                if (i == SPROF_FILE)
                    continue;

                profile_data_t * const prof    = pCore->create_default_profile();
                if (prof == NULL)
                    return STATUS_NO_MEM;

                vProfiles[i] = prof;
            }

            // Bind self as listener
            core::KVTStorage *kvt = pCore->kvt_lock();
            if (kvt != NULL)
            {
                lsp_finally { pCore->kvt_release(); };
                kvt->bind(this);
            }

            return STATUS_OK;
        }

        bool matcher::KVTSync::submit_profile(uint32_t type, profile_data_t *profile)
        {
            if (type >= SPROF_TOTAL)
                return false;
            if (!(profile->nFlags & PFLAGS_DIRTY))
                return false;

            // Copy profile data
            profile_data_t * const dst = vProfiles[type];
            if (dst == NULL)
                return false;

            dst->nSampleRate    = profile->nSampleRate;
            dst->nChannels      = profile->nChannels;
            dst->nRank          = profile->nRank;
            dst->nFlags         = profile->nFlags;
            dst->nFrames        = profile->nFrames;
            dst->fRMS           = profile->fRMS;

            const size_t fft_csize = (1 << (profile->nRank - 1)) + 1;
            for (size_t i=0; i<dst->nChannels; ++i)
            {
                const size_t j              = i % profile->nChannels;
                dsp::copy(dst->vData[i], profile->vData[j], fft_csize);
            }

            // Reset dirty flag
            profile->nFlags &= ~PFLAGS_DIRTY;

            // Increment number of changes
            ++nChanges;

            return true;
        }

        bool matcher::KVTSync::pending() const
        {
            return nChanges > 0;
        }

        void matcher::KVTSync::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
            v->begin_array("vProfiles", vProfiles, SPROF_TOTAL);

            for (size_t i=0; i<SPROF_TOTAL; ++i)
            {
                profile_data_t * const prof    = vProfiles[i];
                matcher::dump(v, "pProfile", prof);
            }
            v->end_array();
        }

        void matcher::KVTSync::created(core::KVTStorage *storage, const char *id, const core::kvt_param_t *param, size_t pending)
        {
            lsp_trace("KVT parameter '%s' has been created", id);
        }

        void matcher::KVTSync::changed(core::KVTStorage *storage, const char *id, const core::kvt_param_t *oval, const core::kvt_param_t *nval, size_t pending)
        {
            lsp_trace("KVT parameter '%s' has been changed", id);
        }

        void matcher::KVTSync::parse_profile(const char *id, const core::kvt_param_t *param, uint32_t type)
        {
            // Load profile
            profile_data_t *profile = pCore->load_profile(id, param);
            if (profile == NULL)
                return;

            // Submit profile to the profile state
            pCore->vProfileState[type].push(profile);
        }

        void matcher::KVTSync::commit(core::KVTStorage *storage, const char *id, const core::kvt_param_t *param, size_t pending)
        {
            if (!(pending & core::KVT_TO_DSP))
                return;

            lsp_trace("KVT parameter '%s' has been committed to DSP", id);

            // Here we can parse profile
            if (strcmp(id, "/profile/static") == 0)
                parse_profile(id, param, SPROF_STATIC);
            if (strcmp(id, "/profile/capture") == 0)
                parse_profile(id, param, SPROF_CAPTURE);
        }

        //-------------------------------------------------------------------------
        matcher::IRSaver::IRSaver(matcher *core)
        {
            pCore       = core;
            pProfile    = NULL;
            bPending    = false;
            sFile[0]    = '\0';
        }

        matcher::IRSaver::~IRSaver()
        {
            if (pProfile != NULL)
            {
                free_profile_data(pProfile);
                pProfile    = NULL;
            }

            pCore       = NULL;
        }

        status_t matcher::IRSaver::init()
        {
            pProfile    = pCore->create_default_profile();
            if (pProfile == NULL)
                return STATUS_NO_MEM;

            return STATUS_OK;
        }

        void matcher::IRSaver::submit_command(bool save)
        {
            lsp_trace("set pending: %s", (save) ? "true" : "false");
            bPending                    = save;
        }

        void matcher::IRSaver::submit_profile(const profile_data_t *src)
        {
            // Fill profile data
            profile_data_t * const profile = pProfile;
            if (profile == NULL)
                return;

            profile->nSampleRate        = src->nSampleRate;
            profile->nChannels          = src->nChannels;
            profile->nRank              = src->nRank;
            profile->nFlags             = src->nFlags;
            profile->nFrames            = src->nFrames;
            profile->fRMS               = src->fRMS;

            // Copy profile data
            const size_t fft_csize = (1 << (profile->nRank - 1)) + 1;
            for (size_t i=0; i<profile->nChannels; ++i)
            {
                const size_t j              = i % src->nChannels;
                dsp::copy(profile->vData[i], src->vData[j], fft_csize);
            }
        }

        void matcher::IRSaver::submit_file_name(const char *fname)
        {
            if (fname != NULL)
            {
                strncpy(sFile, fname, PATH_MAX);
                sFile[PATH_MAX - 1] = '\0';
            }
            else
                sFile[0] = '\0';
        }

        bool matcher::IRSaver::pending() const
        {
            return (bPending) && (sFile[0] != '\0') && (pProfile != NULL);
        }

        void matcher::IRSaver::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
            v->write("bPending", bPending);
            v->write("sFile", sFile);
            matcher::dump(v, "pProfile", pProfile);
        }

        status_t matcher::IRSaver::run()
        {
            // Save IR sample to file
            LSPString path;
            if (!path.set_utf8(sFile))
                return STATUS_NO_MEM;

            lsp_trace("Saving IR sample to file '%s'", path.get_utf8());

            // Compute FFT parameters
            const size_t fft_size   = 1 << pProfile->nRank;
            const size_t fft_half   = fft_size >> 1;

            // Initialize detination sample
            dspu::Sample s;
            if (!s.init(pProfile->nChannels, fft_size, fft_size))
                return STATUS_NO_MEM;
            s.set_sample_rate(pProfile->nSampleRate);

            // Allocate buffers
            const size_t to_alloc   = fft_size * 2 + fft_size;
            float *fft              = static_cast<float *>(malloc(to_alloc * sizeof(float)));
            if (fft == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(fft); };
            float *tmp              = add_ptr<float>(fft, fft_size * 2);

            // Build the IR sample
            for (size_t i=0; i<pProfile->nChannels; ++i)
            {
                float *dst = s.channel(i);
                const float *src = pProfile->vData[i];

                // Create symmetric frequency chart
                dsp::copy(tmp, src, fft_half + 1);
                dsp::reverse2(&tmp[fft_half + 1], &src[1], fft_half - 1);
                dsp::pcomplex_r2c(fft, tmp, fft_size);

                // Convert frequency chart to IR
                dsp::packed_reverse_fft(fft, fft, pProfile->nRank);
                dsp::pcomplex_c2r(&dst[fft_half], fft, fft_half);
                dsp::pcomplex_c2r(dst, &fft[fft_size], fft_half);

                // Apply window function
                dspu::windows::blackman_nuttall(tmp, fft_size);
                dsp::mul2(dst, tmp, fft_size);
            }

            const ssize_t saved = s.save(&path);
            status_t res =
                (saved < 0) ? status_t(-saved) :
                (saved == ssize_t(fft_size)) ? STATUS_OK :
                STATUS_IO_ERROR;

            lsp_trace("Saving IR sample to file '%s' status: %d", path.get_utf8(), int(res));

            return res;
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
            sKVTSync(this),
            sIRSaver(this),
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
            vChannels           = NULL;
            vIndices            = NULL;
            vFreqs              = NULL;
            vFilterCurve        = NULL;
            vEnvelope           = NULL;
            vRevEnvelope        = NULL;
            vBuffer             = NULL;
            vEmptyBuf           = NULL;
            pExecutor           = NULL;
            pGCList             = NULL;
            pReactivity         = NULL;
            pTempProfile        = NULL;
            pFilterProfile      = NULL;
            pMatchProfile       = NULL;

            nInSource           = IN_STATIC;
            nRefSource          = REF_CAPTURE;
            nRawCapSource       = RAW_CAP_INPUT;
            nCapSource          = CAP_NONE;
            nRank               = 0;
            fGainIn             = GAIN_AMP_0_DB;
            fGainOut            = GAIN_AMP_0_DB;
            fFftTau             = 1.0f;
            fFftShift           = GAIN_AMP_0_DB;
            fInTau              = 1.0f;
            fRefTau             = 1.0f;
            fBlend              = 0.0f;
            fHpfFreq            = 0.0f;
            fHpfSlope           = 0.0f;
            fLpfFreq            = 0.0f;
            fLpfSlope           = 0.0f;
            fClipFreq           = -0.0f;
            fStereoLink         = 0.0f;

            nFileProcessReq     = 0;
            nFileProcessResp    = 0;

            bProfile            = false;
            bCapture            = false;
            bListen             = false;
            bSyncRefFFT         = true;
            bSyncFilter         = true;
            bUpdateMatch        = true;
            bMatchTopLimit      = true;
            bMatchBottomLimit   = true;

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

            pBypass             = NULL;
            pGainIn             = NULL;
            pGainOut            = NULL;
            pFftSize            = NULL;
            pInReactivity       = NULL;
            pRefReactivity      = NULL;
            pInSource           = NULL;
            pRefSource          = NULL;
            pCapSource          = NULL;
            pBlend              = NULL;
            pProfile            = NULL;
            pCapture            = NULL;
            pListen             = NULL;
            pHpfOn              = NULL;
            pHpfFreq            = NULL;
            pHpfSlope           = NULL;
            pLpfOn              = NULL;
            pLpfFreq            = NULL;
            pLpfSlope           = NULL;
            pClipOn             = NULL;
            pClipFreq           = NULL;
            pMatchInReady       = NULL;
            pMatchRefReady      = NULL;
            pInReady            = NULL;
            pCapReady           = NULL;
            pFileReady          = NULL;
            pFilterMesh         = NULL;
            pStereoLink         = NULL;

            pMatchTopLimit      = NULL;
            pMatchBottomLimit   = NULL;
            pMatchLimit         = NULL;
            pMatchImmediate     = NULL;
            pMatchMesh          = NULL;

            pIRFile             = NULL;
            pIRSave             = NULL;
            pIRStatus           = NULL;
            pIRProgress         = NULL;

            pFftReact           = NULL;
            pFftShift           = NULL;
            pFftMesh            = NULL;

            pData               = NULL;
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

            // Initialize KVT sync
            if (sKVTSync.init() != STATUS_OK)
            {
                lsp_warn("Failed to initialize KVT sync");
                return;
            }

            // Initialize IR saver
            if (sIRSaver.init() != STATUS_OK)
            {
                lsp_warn("Failed to initialize IR Saver");
                return;
            }

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
                szof_freqs +        // vFilterCurve
                szof_fft_buf +      // vEnvelope
                szof_fft_buf +      // vRevEnvelope
                szof_idx +          // vIndices
                szof_tmp_buf +      // vBuffer
                szof_buf +          // vEmptyBuf
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
            vFilterCurve            = advance_ptr_bytes<float>(ptr, szof_freqs);
            vEnvelope               = advance_ptr_bytes<float>(ptr, szof_fft_buf);
            vRevEnvelope            = advance_ptr_bytes<float>(ptr, szof_fft_buf);
            vIndices                = advance_ptr_bytes<uint16_t>(ptr, szof_idx);
            vBuffer                 = advance_ptr_bytes<float>(ptr, szof_tmp_buf);
            vEmptyBuf               = advance_ptr_bytes<float>(ptr, szof_buf);

            for (size_t i=0; i < nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];

                // Construct in-place DSP processors
                c->sBypass.construct();
                c->sPlayer.construct();
                c->sPlayback.construct();
                c->sDryDelay.construct();
                c->sScDelay.construct();

                if (!c->sPlayer.init(1, 32))
                    return;

                if (!c->sDryDelay.init(1 << meta::matcher::FFT_RANK_MAX))
                    return;
                if (!c->sScDelay.init(1 << meta::matcher::FFT_RANK_MAX))
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
                    c->vLevel[j]            = GAIN_AMP_M_INF_DB;
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
            f->bCanListen   = false;
            f->fPitch       = 0.0f;
            f->fHeadCut     = 0.0f;
            f->fTailCut     = 0.0f;

            f->fDuration    = 0.0f;

            f->pShowOverlay = NULL;
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
            BIND_PORT(pInReactivity);
            BIND_PORT(pRefReactivity);
            BIND_PORT(pInSource);
            BIND_PORT(pRefSource);
            BIND_PORT(pCapSource);
            BIND_PORT(pBlend);
            BIND_PORT(pProfile);
            BIND_PORT(pCapture);
            BIND_PORT(pListen);
            BIND_PORT(pHpfOn);
            BIND_PORT(pHpfFreq);
            BIND_PORT(pHpfSlope);
            BIND_PORT(pLpfOn);
            BIND_PORT(pLpfFreq);
            BIND_PORT(pLpfSlope);
            BIND_PORT(pClipOn);
            BIND_PORT(pClipFreq);
            BIND_PORT(pMatchInReady);
            BIND_PORT(pMatchRefReady);
            BIND_PORT(pInReady);
            BIND_PORT(pCapReady);
            BIND_PORT(pFileReady);
            BIND_PORT(pFilterMesh);
            if (nChannels > 1)
            {
                BIND_PORT(pStereoLink);
            }

            // Bind audio file ports
            lsp_trace("Binding audio file ports");
            BIND_PORT(f->pShowOverlay);
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

            // Bind equalizer setup
            lsp_trace("Binding match equalizer");
            SKIP_PORT("Show FFT");
            SKIP_PORT("Show profiles");
            SKIP_PORT("Show envelope");
            SKIP_PORT("Show limiting");
            SKIP_PORT("Show profile morphing");
            SKIP_PORT("Show filters");
            SKIP_PORT("Show selected profiles");

            BIND_PORT(pMatchTopLimit);
            BIND_PORT(pMatchBottomLimit);
            BIND_PORT(pMatchLimit);
            BIND_PORT(pMatchImmediate);
            SKIP_PORT("Morph time linking");
            for (size_t i=0; i<meta::matcher::MATCH_BANDS; ++i)
            {
                match_band_t *b     = &vMatchBands[i];
                for (size_t j=0; j<EQP_TOTAL; ++j)
                    BIND_PORT(b->pParams[j]);
            }
            BIND_PORT(pMatchMesh);

            // Bind IR file settings
            BIND_PORT(pIRFile);
            BIND_PORT(pIRSave);
            BIND_PORT(pIRStatus);
            BIND_PORT(pIRProgress);

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

                SKIP_PORT("Draw static input profile");
                SKIP_PORT("Draw capture profile");
                SKIP_PORT("Draw file profile");
                SKIP_PORT("Draw equalizer profile");
                SKIP_PORT("Draw dynamic input profile");
                SKIP_PORT("Draw dynamic reference profile");
                SKIP_PORT("Draw resulting match profile");

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
            if (pTempProfile == NULL)
                return;

            pFilterProfile      = create_default_profile(1);
            if (pFilterProfile == NULL)
                return;

            pMatchProfile       = create_default_profile();
            if (pMatchProfile == NULL)
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

            dsp::fill(vFilterCurve, GAIN_AMP_0_DB, meta::matcher::FFT_MESH_SIZE);
            dsp::fill_zero(vEmptyBuf, BUFFER_SIZE);
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
                    c->sDryDelay.destroy();
                    c->sScDelay.destroy();
                    c->sPlayback.destroy();

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
            if (pFilterProfile != NULL)
            {
                free_profile_data(pFilterProfile);
                pFilterProfile = NULL;
            }
            if (pMatchProfile != NULL)
            {
                free_profile_data(pMatchProfile);
                pMatchProfile = NULL;
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

            // Mark source profiles as being changed
            for (size_t i=0; i<SPROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileState[i].get();
                if (profile != NULL)
                    profile->nFlags            |= PFLAGS_CHANGED;
            }

            // Force frequency mapping to be updated
            nRank           = 0;
            bUpdateMatch    = true;
            bSyncFilter     = true;
            bSyncRefFFT     = true;
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

            // Compute direct and inverse envelopes for pink noise
            dspu::envelope::noise_lin(
                vEnvelope,
                0, fSampleRate * 0.5f, SPEC_FREQ_CENTER,
                fft_csize,
                dspu::envelope::PINK_NOISE);
            dspu::envelope::reverse_noise_lin(
                vRevEnvelope,
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
                    case 0: return REF_NONE;
                    case 1: return REF_CAPTURE;
                    case 2: return REF_FILE;
                    case 3: return REF_EQUALIZER;
                    case 4: return REF_SIDECHAIN;
                    case 5: return REF_LINK;
                    default: break;
                }
            }
            else
            {
                switch (ref)
                {
                    case 0: return REF_NONE;
                    case 1: return REF_CAPTURE;
                    case 2: return REF_FILE;
                    case 3: return REF_EQUALIZER;
                    case 4: return REF_LINK;
                    default: break;
                }
            }

            return REF_NONE;
        }

        uint32_t matcher::decode_raw_capture_source(size_t cap) const
        {
            if (bSidechain)
            {
                switch (cap)
                {
                    case 0: return RAW_CAP_INPUT;
                    case 1: return RAW_CAP_SIDECHAIN;
                    case 2: return RAW_CAP_LINK;
                    default: break;
                }
            }
            else
            {
                switch (cap)
                {
                    case 0: return RAW_CAP_INPUT;
                    case 1: return RAW_CAP_LINK;
                    default: break;
                }
            }

            return CAP_NONE;
        }

        uint32_t matcher::decode_capture_source(size_t raw_cap, size_t ref) const
        {
            switch (raw_cap)
            {
                case RAW_CAP_INPUT: return CAP_INPUT;
                case RAW_CAP_SIDECHAIN: return (ref == REF_SIDECHAIN) ? CAP_REFERENCE : CAP_SIDECHAIN;
                case RAW_CAP_LINK: return (ref == REF_LINK) ? CAP_REFERENCE : CAP_LINK;
                default: break;
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
            const bool match_limit  = pMatchLimit->value() >= 0.5f;
            bool rebuild_eq_profiles= false;

            fGainIn                 = pGainIn->value();
            fGainOut                = pGainOut->value();

            const size_t old_ref_source = nRefSource;
            const size_t old_cap_source = nCapSource;
            const bool old_match_top    = bMatchTopLimit;
            const bool old_match_bottom = bMatchBottomLimit;
            const size_t old_in_source  = nInSource;
            const float old_slink       = fStereoLink;
            const float old_blend       = fBlend;


            nRefSource              = decode_reference_source(pRefSource->value());
            nRawCapSource           = decode_raw_capture_source(pCapSource->value());
            nCapSource              = decode_capture_source(nRawCapSource, nRefSource);
            bMatchTopLimit          = (match_limit) && (pMatchTopLimit->value() >= 0.5f);
            bMatchBottomLimit       = (match_limit) && (pMatchBottomLimit->value() >= 0.5f);
            nInSource               = pInSource->value();
            fBlend                  = (100.0f - pBlend->value()) * 0.01f;
            fStereoLink             = (pStereoLink != NULL) ? pStereoLink->value() * 0.01f : 0.0f;
            bUpdateMatch            = rebuild_eq_profiles;

            sMatchImmediate.submit(pMatchImmediate->value());
            sIRSaver.submit_command(pIRSave->value() >= 0.5f);

            if ((old_ref_source != nRefSource) || (old_cap_source != nCapSource) || (old_in_source != nInSource))
            {
                profile_data_t * const profile = vProfileData[PROF_MATCH];
                profile->nFlags        |= PFLAGS_CHANGED;

                bUpdateMatch            = true;
                if (old_ref_source != nRefSource)
                    bSyncRefFFT             = true;
            }
            if ((bMatchTopLimit != old_match_top) ||
                (bMatchBottomLimit != old_match_bottom) ||
                (fStereoLink != old_slink) ||
                (fBlend != old_blend))
                bUpdateMatch            = true;

            fFftTau                 = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), reactivity));
            fFftShift               = pFftShift->value();
            fInTau                  = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), in_react));
            fRefTau                 = 1.0f - expf(FFT_TIME_CONST / dspu::seconds_to_samples(float(fSampleRate) / float(fft_period), ref_react));

            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];
                c->sBypass.set_bypass(bypass);
            }

            // Apply filter
            const float old_hpf_freq    = fHpfFreq;
            const float old_hpf_slope   = fHpfSlope;
            const float old_lpf_freq    = fLpfFreq;
            const float old_lpf_slope   = fLpfSlope;
            const float old_clip_freq   = fClipFreq;
            fHpfFreq                    = pHpfFreq->value();
            fHpfSlope                   = (pHpfOn->value() >= 0.5f) ? -pHpfSlope->value() : 0.0f;
            fLpfFreq                    = pLpfFreq->value();
            fLpfSlope                   = (pLpfOn->value() >= 0.5f) ? -pLpfSlope->value() : 0.0f;
            fClipFreq                   = (pClipOn->value() >= 0.5f) ? pClipFreq->value() : -1.0f;

            if ((old_hpf_freq != fHpfFreq) ||
                (old_hpf_slope != fHpfSlope) ||
                (old_lpf_freq != fLpfFreq) ||
                (old_lpf_slope != fLpfSlope) ||
                (old_clip_freq != fClipFreq) ||
                (rebuild_eq_profiles))
            {
                if (pFilterProfile != NULL)
                    pFilterProfile->nFlags      |= PFLAGS_CHANGED;

                bUpdateMatch            = true;
            }

            // Apply FFT rank
            sProcessor.set_rank(rank);
            if (rank != nRank)
            {
                if (pFilterProfile != NULL)
                    pFilterProfile->nFlags      |= PFLAGS_CHANGED;

                nRank                   = rank;
                bSyncRefFFT             = true;
                rebuild_eq_profiles     = true;
                update_frequency_mapping();

                // Mark source profiles as being changed
                for (size_t i=0; i<SPROF_TOTAL; ++i)
                {
                    profile_data_t * const profile = vProfileState[SPROF_STATIC].get();
                    if (profile != NULL)
                        profile->nFlags            |= PFLAGS_CHANGED;
                }
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
            bListen                 = pListen->value() >= 0.5f;

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
                        c->bFft[j]      = fft;
                        if (j == SM_REFERENCE)
                        {
                            if ((nRefSource == REF_SIDECHAIN) || (nRefSource == REF_LINK))
                                dsp::fill_zero(c->vFft[j], fft_csize);
                            else
                                bSyncRefFFT = true;
                        }
                        else
                            dsp::fill_zero(c->vFft[j], fft_csize);
                    }
                }
            }

            // Update equalizer settings
            uint32_t eq_dirty = (rebuild_eq_profiles) ? (1 << EQP_TOTAL) - 1 : 0;
            for (size_t i=0; i<EQP_TOTAL; ++i)
            {
                if (i != EQP_REACTIVITY)
                {
                    for (size_t j=0; j<meta::matcher::MATCH_BANDS; ++j)
                    {
                        float *vold = &vMatchBands[j].vParams[i];
                        float vnew = dspu::db_to_gain(vMatchBands[j].pParams[i]->value());
                        if (*vold != vnew)
                        {
                            *vold           = vnew;
                            eq_dirty       |= 1 << i;
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
                            eq_dirty       |= 1 << i;
                        }
                    }
                }
            }
            if (eq_dirty != 0)
            {
                if (eq_dirty & (1 << EQP_REF_LEVEL))
                    build_eq_profile(vProfileData[PROF_ENVELOPE], EQP_REF_LEVEL, true);
                if (eq_dirty & (1 << EQP_MAX_AMPLIFICATION))
                {
                    build_eq_profile(vProfileData[PROF_MAX_EQUALIZER], EQP_MAX_AMPLIFICATION, false);
                    if (bMatchTopLimit)
                        bUpdateMatch        = true;
                }
                if (eq_dirty & (1 << EQP_MAX_REDUCTION))
                {
                    build_eq_profile(vProfileData[PROF_MIN_EQUALIZER], EQP_MAX_REDUCTION, false);
                    if (bMatchBottomLimit)
                        bUpdateMatch        = true;
                }
                if (eq_dirty & (1 << EQP_REACTIVITY))
                    build_eq_profile(pReactivity, EQP_REACTIVITY, false);
                if (nRefSource == REF_EQUALIZER)
                    bSyncRefFFT         = true;
            }

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
            af->bCanListen          = af->pShowOverlay->value() >= 0.5f;
            if (af->bCanListen)
            {
                if (af->pListen != NULL)
                    af->sListen.submit(af->pListen->value());
                if (af->pStop != NULL)
                    af->sStop.submit(af->pStop->value());
            }

            // Set latency
            const size_t latency    = sProcessor.latency();
            set_latency(latency);
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t * const c = &vChannels[i];
                c->sDryDelay.set_delay(latency);
                c->sScDelay.set_delay(latency);
            }
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

        void matcher::init_buffers()
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];

                // Get input and output buffers
                c->vIn                  = c->pIn->buffer<float>();
                c->vOut                 = c->pOut->buffer<float>();
                c->vSc                  = (c->pSc != NULL) ? c->pSc->buffer<float>() : NULL;
                c->vShmIn               = NULL;
                core::AudioBuffer *buf  = (c->pShmIn != NULL) ? c->pShmIn->buffer<core::AudioBuffer>() : NULL;
                if ((buf != NULL) && (buf->active()))
                    c->vShmIn               = buf->buffer();
            }
        }

        void matcher::bind_buffers(size_t samples)
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c            = &vChannels[i];

                // Bind processor buffers
                const size_t base       = i * PC_TOTAL;

                // Route input
                const float in_level    = dsp::abs_max(c->vIn, samples) * fGainIn;
                sProcessor.bind(base + PC_INPUT, c->vBuffer, c->vIn);
                sProcessor.bind(base + PC_REFERENCE, NULL, NULL);
                sProcessor.bind(base + PC_CAPTURE, NULL, NULL);

                // Route reference input signal
                float ref_level         = GAIN_AMP_M_INF_DB;
                switch (nRefSource)
                {
                    case REF_SIDECHAIN:
                        sProcessor.bind(base + PC_REFERENCE, NULL, c->vSc);
                        if (c->vSc != NULL)
                            ref_level       = dsp::abs_max(c->vSc, samples);
                        break;
                    case REF_LINK:
                        sProcessor.bind(base + PC_REFERENCE, NULL, c->vShmIn);
                        if (c->vShmIn != NULL)
                            ref_level       = dsp::abs_max(c->vShmIn, samples);
                        break;

                    case REF_NONE:
                    case REF_CAPTURE:
                    case REF_FILE:
                    case REF_EQUALIZER:
                    default:
                        break;
                }

                // Route capture input signal
                float cap_level         = GAIN_AMP_M_INF_DB;
                switch (nCapSource)
                {
                    case CAP_SIDECHAIN:
                        sProcessor.bind(base + PC_CAPTURE, NULL, c->vSc);
                        if (c->vSc != NULL)
                            cap_level       = dsp::abs_max(c->vSc, samples);
                        break;
                    case CAP_LINK:
                        sProcessor.bind(base + PC_CAPTURE, NULL, c->vShmIn);
                        if (c->vShmIn != NULL)
                            cap_level       = dsp::abs_max(c->vShmIn, samples);
                        break;
                    case CAP_INPUT:
                        cap_level           = in_level;
                        break;
                    case CAP_REFERENCE:
                        cap_level           = ref_level;
                        break;
                    default:

                        break;
                }

                // Update levels
                c->vLevel[SM_IN]        = lsp_max(c->vLevel[SM_IN], in_level);
                c->vLevel[SM_CAPTURE]   = lsp_max(c->vLevel[SM_CAPTURE], cap_level);
                c->vLevel[SM_REFERENCE] = lsp_max(c->vLevel[SM_REFERENCE], ref_level);
            }
        }

        void matcher::advance_buffers(size_t samples)
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];
                c->vIn             += samples;
                c->vOut            += samples;
                if (c->vSc != NULL)
                    c->vSc             += samples;
                if (c->vShmIn != NULL)
                    c->vShmIn          += samples;
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

        matcher::profile_data_t *matcher::allocate_profile_data(size_t channels)
        {
            if (channels == 0)
                channels                    = nChannels;
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
            profile->nFlags             = PFLAGS_NONE;
            profile->nFrames            = 0;
            profile->vData              = add_ptr_bytes<float *>(profile, szof_data_hdr);
            profile->fRMS               = GAIN_AMP_M_INF_DB;

            for (size_t i=0; i<channels; ++i)
            {
                profile->vData[i]           = advance_ptr_bytes<float>(ptr, prof_data_size);
                dsp::fill_zero(profile->vData[i], fft_citems);
            }

            lsp_assert(ptr <= &base[to_alloc]);

            return profile;
        }

        matcher::profile_data_t *matcher::create_default_profile(size_t channels)
        {
            profile_data_t *res = allocate_profile_data(channels);
            if (res == NULL)
                return res;

            res->nFlags                 = PFLAGS_DEFAULT;
            res->nSampleRate            = fSampleRate;
            res->nRank                  = nRank;
            return res;
        }

        void matcher::free_profile_data(profile_data_t *profile)
        {
            if (profile != NULL)
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
                dsp::mul_k2(vBuffer, NORMING_SHIFT / float(1 << nRank), fft_csize);
                dsp::mix2(dst, vBuffer, 1.0f - fFftTau, fFftTau, fft_csize);
            }
            else
                dsp::mul_k2(dst, 1.0f - fFftTau, fft_csize);
        }

        void matcher::record_profile(profile_data_t *profile, float * const * spectrum, size_t channel)
        {
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            const size_t frames     = profile->nFrames;
            const float norm        = NORMING_SHIFT / float(1 << nRank);
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
                        dsp::mix2(dst, vBuffer, kp, k * norm, fft_csize);
                    }
                    else
                        dsp::mul_k2(dst, kp, fft_csize);
                }
                else if (src != NULL)
                {
                    // Fill empty profile
                    dsp::pcomplex_mod(dst, src, fft_csize);
                    dsp::mul_k2(dst, norm, fft_csize);
                }
            }

            // Check that profile has enough frames for processing
            profile->nSampleRate    = fSampleRate;
            profile->nRank          = nRank;
            profile->nFrames        = lsp_min(frames + 1, size_t(0x100));
            profile->nFlags        &= ~PFLAGS_DEFAULT;
            profile->nFlags        |= PFLAGS_DIRTY | PFLAGS_CHANGED | PFLAGS_SYNC;
            profile->fRMS           = GAIN_AMP_M_INF_DB;

            if (profile->nFrames >= 8)
            {
                const float rms_norm        = 1.0f / float(fft_csize);
                for (size_t i=0; i<nChannels; ++i)
                    profile->fRMS              += sqrtf(dsp::h_sqr_sum(profile->vData[i], fft_csize) * rms_norm);

                if (profile->fRMS >= GAIN_AMP_M_72_DB)
                    profile->nFlags            |= PFLAGS_READY;
            }
        }

        void matcher::track_profile(profile_data_t *profile, float * const *spectrum, float tau, size_t channel)
        {
            const float norm        = NORMING_SHIFT / float(1 << nRank);
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            const float rms_norm    = 1.0f / float(fft_csize);

            // Apply changes
            float * const tmp       = pTempProfile->vData[0];
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
                        dsp::pcomplex_mod(tmp, src, fft_csize);
                        dsp::mix2(dst, tmp, 1.0f - tau, tau * norm, fft_csize);
                    }
                    else
                        dsp::mul_k2(dst, 1.0f - tau, fft_csize);
                }
                else if (src != NULL)
                {
                    // Fill empty profile
                    dsp::pcomplex_mod(dst, src, fft_csize);
                    dsp::mul_k3(dst, tmp, norm, fft_csize);
                }
            }

            profile->nSampleRate    = fSampleRate;
            profile->nRank          = nRank;
            if (profile->nFrames < ~uint32_t(0))
                ++profile->nFrames;
            profile->nFlags        &= ~(PFLAGS_DEFAULT | PFLAGS_EMPTY);
            profile->nFlags        |= PFLAGS_CHANGED | PFLAGS_SYNC | PFLAGS_DYNAMIC;
            profile->fRMS           = GAIN_AMP_M_INF_DB;

            // Compute RMS for the profile
            for (size_t i=0; i<nChannels; ++i)
                profile->fRMS              += sqrtf(dsp::h_sqr_sum(profile->vData[i], fft_csize) * rms_norm);

            if (profile->fRMS >= GAIN_AMP_M_72_DB)
                profile->nFlags            |= PFLAGS_READY;
            else
                profile->nFlags            |= PFLAGS_EMPTY;
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
            dst->nFlags             = src->nFlags & (~PFLAGS_DYNAMIC);
            dst->nFrames            = src->nFrames;
            dst->fRMS               = src->fRMS;

            for (size_t i=0; i<dst->nChannels; ++i)
                dsp::copy(dst->vData[i], src->vData[i % src->nChannels], fft_csize);

            // Reset dirty flag for the original profile
            src->nFlags            &= ~PFLAGS_CHANGED;
        }

        void matcher::build_match_profile(profile_data_t *in, profile_data_t *ref, bool dynamic)
        {
            profile_data_t * const profile = vProfileData[PROF_MATCH];
            if (profile == NULL)
                return;

            profile_data_t * const match    = pMatchProfile;

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

            const bool is_dynamic   = (in->nFlags | ref->nFlags) & PFLAGS_DYNAMIC;
            const bool need_sync    = ((in->nFlags | ref->nFlags | profile->nFlags) & PFLAGS_CHANGED) || (bUpdateMatch);

            // Check that we need to blend profile
            profile_data_t * src        = ref;
            if ((fBlend > 0.0f) && ((is_dynamic) || (need_sync)))
            {
                const float norm    = ref->fRMS / in->fRMS;
                src                 = pTempProfile;
                if (src == NULL)
                {
                    profile->nFlags    &= ~PFLAGS_READY;
                    return;
                }

                // Compute blending profile
                for (size_t i=0; i<nChannels; ++i)
                {
                    dsp::mix_copy2(
                        src->vData[i],
                        in->vData[i], ref->vData[i],
                        fBlend * norm, 1.0f - fBlend,
                        fft_csize); // Blend with reference
                }

                // Replace reference with blending profile
                src->fRMS           = ref->fRMS;
                src->nFlags        &= ~(PFLAGS_READY | PFLAGS_EMPTY);
                if (src->fRMS >= GAIN_AMP_M_72_DB)
                    src->nFlags                |= PFLAGS_READY;
                else
                    src->nFlags                |= PFLAGS_EMPTY;
            }

            if (is_dynamic)
            {
                // Check that temporary profile is present
                profile_data_t * const tmp = pTempProfile;
                if (tmp == NULL)
                {
                    profile->nFlags    &= ~PFLAGS_READY;
                    return;
                }

                const float norm = src->fRMS / in->fRMS;
                const bool match_immediate  = sMatchImmediate.pending();
                const bool towards_0db      = (src->nFlags | in->nFlags) & PFLAGS_EMPTY;

                if (towards_0db)
                {
                    // One of the profiles has low RMS, we need to correct the dynamic profile towards 0 dB level.
                    for (size_t i=0; i<nChannels; ++i)
                    {
                        // Apply reactivity to the changes or perform immediate match
                        if (match_immediate)
                            dsp::fill(match->vData[i], GAIN_AMP_0_DB, fft_csize);
                        else
                        {
                            if (i == 0)
                                dsp::fill(tmp->vData[0], GAIN_AMP_0_DB, fft_csize);
                            dsp::pmix_v1(match->vData[i], tmp->vData[0], pReactivity->vData[i], fft_csize);
                        }
                    }
                }
                else
                {
                    // Normally dynamically changing profile
                    for (size_t i=0; i<nChannels; ++i)
                    {
                        // Compute new profile value
                        dsp::clamp_kk2(vBuffer, in->vData[i], GAIN_AMP_M_72_DB, GAIN_AMP_P_72_DB, fft_csize);

                        // Apply reactivity to the changes or perform immediate match
                        if (match_immediate)
                        {
                            dsp::clamp_kk2(match->vData[i], src->vData[i], GAIN_AMP_M_72_DB, GAIN_AMP_P_72_DB, fft_csize);
                            dsp::fmdiv_k3(match->vData[i], vBuffer, norm, fft_csize); // src / (in * norm)
                        }
                        else
                        {
                            dsp::clamp_kk2(tmp->vData[i], src->vData[i], GAIN_AMP_M_72_DB, GAIN_AMP_P_72_DB, fft_csize);
                            dsp::fmdiv_k3(tmp->vData[i], vBuffer, norm, fft_csize); // src / (in * norm)
                            dsp::pmix_v1(match->vData[i], tmp->vData[i], pReactivity->vData[i], fft_csize);
                        }
                    }
                }
            }
            else if (need_sync)
            {
                const float norm = src->fRMS / in->fRMS;

                // Compute new static profile value
                for (size_t i=0; i<nChannels; ++i)
                {
                    dsp::clamp_kk2(vBuffer, in->vData[i], GAIN_AMP_M_72_DB, GAIN_AMP_P_72_DB, fft_csize);
                    dsp::clamp_kk2(match->vData[i], src->vData[i], GAIN_AMP_M_72_DB, GAIN_AMP_P_72_DB, fft_csize);
                    dsp::fmdiv_k3(match->vData[i], vBuffer, norm, fft_csize); // src / (in * norm)
                }
            }

            // Synchronize state of computed profile with filters
            if ((is_dynamic) || (need_sync))
            {
                // Copy profile data
                for (size_t i=0; i<nChannels; ++i)
                    dsp::copy(profile->vData[i], match->vData[i], fft_csize);

                // Apply stereo linking
                if ((nChannels > 1) && (fStereoLink > 0.0f))
                {
                    dsp::lr_to_mid(vBuffer, profile->vData[0], profile->vData[1], fft_csize);
                    dsp::pmix_k1(profile->vData[0], vBuffer, fStereoLink, fft_csize);
                    dsp::pmix_k1(profile->vData[1], vBuffer, fStereoLink, fft_csize);
                }

                // Apply limitations if present
                profile_data_t * const min = (bMatchBottomLimit) ? vProfileData[PROF_MIN_EQUALIZER] : NULL;
                profile_data_t * const max = (bMatchTopLimit) ? vProfileData[PROF_MAX_EQUALIZER] : NULL;
                for (size_t i=0; i<nChannels; ++i)
                {
                    if (min != NULL)
                    {
                        if (max != NULL)
                            dsp::clamp_vv1(profile->vData[i], min->vData[i], max->vData[i], fft_csize);
                        else
                            dsp::pmax2(profile->vData[i], min->vData[i], fft_csize);
                    }
                    else if (max != NULL)
                        dsp::pmin2(profile->vData[i], max->vData[i], fft_csize);
                }

                // Apply filters
                if (pFilterProfile->nFlags & PFLAGS_READY)
                {
                    profile_data_t * const flt  = pFilterProfile;
                    for (size_t i=0; i<nChannels; ++i)
                        dsp::mul2(profile->vData[i], flt->vData[0], fft_csize);
                }
            }

            // Update profile flags
            in->nFlags         &= ~PFLAGS_CHANGED;
            ref->nFlags        &= ~PFLAGS_CHANGED;
            match->nSampleRate  = fSampleRate;
            match->nRank        = nRank;
            match->nFlags      &= ~(PFLAGS_CHANGED | PFLAGS_DIRTY | PFLAGS_DEFAULT);
            match->nFlags      |= PFLAGS_READY | PFLAGS_SYNC;
            profile->nSampleRate= fSampleRate;
            profile->nRank      = nRank;
            profile->nFlags    &= ~(PFLAGS_CHANGED | PFLAGS_DIRTY | PFLAGS_DEFAULT);
            profile->nFlags    |= PFLAGS_READY | PFLAGS_SYNC;
            bUpdateMatch        = false;
        }

        void matcher::process_block(float * const * spectrum, size_t rank)
        {
            const size_t fft_half   = (1 << (nRank - 1));
            const size_t fft_csize  = fft_half + 1;

            // Select capture signal channel mode
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

            // Select reference channel mode
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
                    ref_profile     = PROF_ENVELOPE;
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

            // Select input profile mode
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

            // Apply input gain
            if (fGainIn != GAIN_AMP_0_DB)
            {
                for (size_t i=0; i<nChannels; ++i)
                    dsp::mul_k2(spectrum[i*PC_TOTAL + PC_INPUT], fGainIn, fft_half * 4);
            }

            // Build filter profile if needed
            build_filter_profile();

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
            profile_data_t * const match = vProfileData[PROF_MATCH];
            if ((ref_profile >= 0) && (in_profile >= 0))
                build_match_profile(vProfileData[in_profile], vProfileData[ref_profile], match_dynamic);
            else
                match->nFlags &= ~PFLAGS_READY;

            // Analyze input signal
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c = &vChannels[i];
                const size_t base = i*PC_TOTAL;

                analyze_spectrum(c, SM_IN, spectrum[base + PC_INPUT]);
                if (cap_channel >= 0)
                    analyze_spectrum(c, SM_CAPTURE, spectrum[base + cap_channel]);
                if (ref_channel >= 0)
                    analyze_spectrum(c, SM_REFERENCE, spectrum[base + ref_channel]);
                else if (ref_profile >= 0)
                {
                    profile_data_t * const profile = vProfileData[ref_profile];
                    if ((profile != NULL) && ((profile->nFlags & PFLAGS_CHANGED) || (bSyncRefFFT)))
                        dsp::copy(c->vFft[SM_REFERENCE], profile->vData[i], fft_csize);
                }
            }
            bSyncRefFFT     = false;

            // Apply profile
            if ((match != NULL) && (match->nFlags & PFLAGS_READY))
            {

                float * const tmp = vBuffer;

                for (size_t i=0; i<nChannels; ++i)
                {
                    float * const fft = spectrum[i*PC_TOTAL + PC_INPUT];
                    const float * const prof = match->vData[i];

                    dsp::pcomplex_r2c_mul2(fft, prof, fft_csize);
                    dsp::reverse2(tmp, &prof[1], fft_half - 1);
                    dsp::pcomplex_r2c_mul2(&fft[fft_csize * 2], tmp, fft_half - 1);
                }
            }

            // Analyze output signal
            for (size_t i=0; i<nChannels; ++i)
            {
                // Apply output gain
                float * const fft = spectrum[i*PC_TOTAL + PC_INPUT];
                if (fGainOut != GAIN_AMP_0_DB)
                    dsp::mul_k2(fft, fGainOut, fft_half * 4); // Complex numbers

                analyze_spectrum(&vChannels[i], SM_OUT, fft);
            }

            // Commit state update
            commit_profiles();
            sMatchImmediate.commit();
        }

        bool matcher::resample_profile(profile_data_t *profile, size_t srate, size_t rank)
        {
            if ((profile->nSampleRate == srate) && (profile->nRank == rank))
                return false;

            if (profile->nFlags & PFLAGS_DEFAULT)
            {
                profile->nSampleRate    = srate;
                profile->nRank          = rank;
                return false;
            }

            lsp_trace("resample profile ptr=%p, srate = %d -> %d, rank %d -> %d",
                profile, int(profile->nSampleRate), int(srate), int(profile->nRank), int(rank));

            const float dsrate          = lsp_min(srate, profile->nSampleRate);
            const size_t src_fft_csize  = (1 << (profile->nRank - 1)) + 1;
            const size_t dst_fft_csize  = (1 << (rank - 1)) + 1;
            const size_t src_bins       = (float(1 << profile->nRank) * dsrate) / float (profile->nSampleRate);
            const size_t dst_bins       = (float(1 << rank) * dsrate) / float (srate);
            const float rk              = float(src_bins) / float(dst_bins);

            float * const tmp           = vBuffer;
            if (rk < 1.0f)
            {
                // Stretch profile using simple linear interpolation
                for (size_t i=0; i<profile->nChannels; ++i)
                {
                    float *src      = profile->vData[i];
                    for (size_t j_dst=0; j_dst<dst_fft_csize; ++j_dst)
                    {
                        const float j_time      = j_dst * rk;
                        const size_t j_src      = truncf(j_time);
                        const float k_src       = j_time - j_src;

                        const float s0          = (j_src < src_fft_csize) ? src[j_src] : 0.0f;
                        const float s1          = ((j_src + 1) < src_fft_csize) ? src[j_src+1] : 0.0f;
                        tmp[j_dst]              = s0 + (s1 - s0) * k_src;
                    }

                    dsp::copy(src, tmp, dst_fft_csize);
                }
            }
            else if (rk > 1.0f)
            {
                // Collapse profile using averaging
                for (size_t i=0; i<profile->nChannels; ++i)
                {
                    float *src      = profile->vData[i];
                    for (size_t j_dst=0; j_dst<dst_fft_csize; ++j_dst)
                    {
                        const float j0_t        = j_dst * rk;
                        const float j1_t        = j0_t + rk;
                        size_t j0_src           = truncf(j0_t);
                        size_t j1_src           = truncf(j1_t);

                        float s                 = 0.0f;
                        for (size_t t = j0_src; t < j1_src; ++t)
                            s                      += (t < src_fft_csize) ? src[t] : 0.0f;
                        tmp[j_dst]              = s / float(j1_src - j0_src);
                    }

                    dsp::copy(src, tmp, dst_fft_csize);
                }
            }

            profile->nSampleRate    = srate;
            profile->nRank          = rank;
            profile->fRMS           = 0.0f;

            const float rms_norm        = 1.0f / float(dst_fft_csize);
            for (size_t i=0; i<nChannels; ++i)
                profile->fRMS              += sqrtf(dsp::h_sqr_sum(profile->vData[i], dst_fft_csize) * rms_norm);

            return true;
        }

        bool matcher::profile_is_relative(size_t profile)
        {
            switch (profile)
            {
                case PROF_MATCH:
                case PROF_MIN_EQUALIZER:
                case PROF_MAX_EQUALIZER:
                    return true;
                default:
                    break;
            }

            return false;
        }

        void matcher::update_profiles()
        {
            // Synchronize state of profiles
            sync_profile(vProfileData[PROF_STATIC], vProfileState[SPROF_STATIC].get());
            sync_profile(vProfileData[PROF_CAPTURE], vProfileState[SPROF_CAPTURE].get());
            sync_profile(vProfileData[PROF_FILE], vProfileState[SPROF_FILE].get());

            // Check that profile needs to be resampled
            bool resampled;
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                resampled = resample_profile(vProfileData[i], fSampleRate, nRank);
                if (resampled)
                    lsp_trace("profile id=%d was resampled", int(i));
            }
            resampled = resample_profile(pMatchProfile, fSampleRate, nRank);
            if (resampled)
                lsp_trace("match profile was resampled");
        }

        void matcher::commit_profiles()
        {
            for (size_t i=0; i<PROF_TOTAL; ++i)
            {
                profile_data_t * const profile = vProfileData[i];
                profile->nFlags        &= ~PFLAGS_CHANGED;
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

        void matcher::build_eq_profile(profile_data_t *profile, eq_param_t param, bool envelope)
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
            if (envelope)
                dsp::mul2(dst, vEnvelope, fft_csize);
            for (size_t i=1; i<nChannels; ++i)
                dsp::copy(profile->vData[i], profile->vData[0], fft_csize);

            profile->nSampleRate    = fSampleRate;
            profile->nRank          = nRank;
            profile->fRMS           = GAIN_AMP_0_DB;
            profile->nFlags         = PFLAGS_READY | PFLAGS_SYNC | PFLAGS_CHANGED;
            profile->nFrames        = 0;
            if (param != EQP_REACTIVITY)
            {
                const float rms_norm    = 1.0 / float(fft_csize);
                profile->fRMS           = sqrtf(dsp::h_sqr_sum(profile->vData[0], fft_csize) * rms_norm) * float(nChannels);
            }
            else
                profile->fRMS           = GAIN_AMP_M_INF_DB;
        }

        void matcher::build_filter_profile()
        {
            profile_data_t * const profile = pFilterProfile;
            if (profile == NULL)
                return;
            if (!(profile->nFlags & PFLAGS_CHANGED))
                return;

            // Build the filter profile
            const float half_sr     = fSampleRate * 0.5f;
            const size_t fft_csize  = (1 << (nRank - 1)) + 1;
            dsp::lramp_set1(vBuffer, 0.0f, half_sr, fft_csize - 1);
            vBuffer[fft_csize - 1]  = half_sr;

            profile->nFlags        &= ~(PFLAGS_CHANGED | PFLAGS_READY);

            if (fHpfSlope < 0.0f)
            {
                dspu::crossover::hipass_set(profile->vData[0], vBuffer, fHpfFreq, fHpfSlope, fft_csize);
                dspu::crossover::hipass_set(vFilterCurve, vFreqs, fHpfFreq, fHpfSlope, meta::matcher::FFT_MESH_SIZE);

                if (fLpfSlope < 0.0f)
                {
                    dspu::crossover::lopass_apply(profile->vData[0], vBuffer, fLpfFreq, fLpfSlope, fft_csize);
                    dspu::crossover::lopass_apply(vFilterCurve, vFreqs, fLpfFreq, fLpfSlope, meta::matcher::FFT_MESH_SIZE);
                }

                profile->nFlags        |= PFLAGS_READY;
            }
            else if (fLpfSlope < 0.0f)
            {
                dspu::crossover::lopass_set(profile->vData[0], vBuffer, fLpfFreq, fLpfSlope, fft_csize);
                dspu::crossover::lopass_set(vFilterCurve, vFreqs, fLpfFreq, fLpfSlope, meta::matcher::FFT_MESH_SIZE);

                profile->nFlags        |= PFLAGS_READY;
            }
            else
            {
                dsp::fill(profile->vData[0], GAIN_AMP_0_DB, fft_csize);
                dsp::fill(vFilterCurve, GAIN_AMP_0_DB, meta::matcher::FFT_MESH_SIZE);
            }

            if (fClipFreq > 0.0f)
            {
                const float norm        = logf(SPEC_FREQ_MAX/SPEC_FREQ_MIN) / (meta::matcher::FFT_MESH_SIZE - 1);
                const size_t mesh_i     = truncf(logf(fClipFreq/SPEC_FREQ_MIN) / norm);
                if (mesh_i < meta::matcher::FFT_MESH_SIZE)
                    dsp::fill(&vFilterCurve[mesh_i], GAIN_AMP_M_INF_DB, meta::matcher::FFT_MESH_SIZE - mesh_i);

                const size_t fft_i      = truncf((fClipFreq / fSampleRate) * float(1 << nRank));
                if (fft_i < fft_csize)
                    dsp::fill(&profile->vData[0][fft_i], GAIN_AMP_M_INF_DB, fft_csize - fft_i);

                profile->nFlags        |= PFLAGS_READY;
            }

            bSyncFilter             = true;
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

        void matcher::process_listen_output(channel_t *c, size_t samples)
        {
            const float *cap_src = vEmptyBuf;
            switch (nRawCapSource)
            {
                case RAW_CAP_INPUT:
                    cap_src = c->vIn;
                    break;
                case RAW_CAP_SIDECHAIN:
                    if (c->vSc != NULL)
                        cap_src     = c->vSc;
                    break;
                case RAW_CAP_LINK:
                    if (c->vShmIn != NULL)
                        cap_src     = c->vShmIn;
                    break;
                default:
                    cap_src = vEmptyBuf;
                    break;
            }

            // Measure input levels
            if (cap_src != vEmptyBuf)
                c->vLevel[SM_CAPTURE]   = lsp_max(c->vLevel[SM_CAPTURE], dsp::abs_max(cap_src, samples));

            if (bListen)
                c->sScDelay.process(c->vBuffer, cap_src, samples);
            else
                c->sScDelay.append(cap_src, samples);

            c->sPlayer.process(c->vBuffer, c->vBuffer, samples);

            // Measure output levels
            c->vLevel[SM_OUT]       = lsp_max(c->vLevel[SM_OUT], dsp::abs_max(c->vBuffer, samples));
        }

        void matcher::init_level_meters()
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];
                for (size_t j=0; j<SM_TOTAL; ++j)
                    c->vLevel[j]        = GAIN_AMP_M_INF_DB;
            }
        }

        void matcher::output_level_meters()
        {
            for (size_t i=0; i<nChannels; ++i)
            {
                channel_t *c        = &vChannels[i];
                for (size_t j=0; j<SM_TOTAL; ++j)
                    c->pMeter[j]->set_value(c->vLevel[j]);
            }
        }

        void matcher::set_profile_ready(plug::IPort *port, ssize_t id)
        {
            if (port == NULL)
                return;

            const profile_data_t * const profile = (id >= 0) ? vProfileData[id] : NULL;
            const bool ready = (profile != NULL) ? (profile->nFlags & PFLAGS_READY) : false;

            port->set_value((ready) ? 1.0f : 0.0f);
        }

        void matcher::output_profile_status()
        {
            ssize_t profile_id;

            // Input profile readiness
            switch (nInSource)
            {
                case IN_DYNAMIC:
                    profile_id = PROF_INPUT;
                    break;
                case IN_STATIC:
                default:
                    profile_id = PROF_STATIC;
                    break;
            }
            set_profile_ready(pMatchInReady, profile_id);

            // Reference profile readiness
            switch (nRefSource)
            {
                case REF_CAPTURE:
                    profile_id = PROF_CAPTURE;
                    break;
                case REF_FILE:
                    profile_id = PROF_FILE;
                    break;
                case REF_EQUALIZER:
                    profile_id = PROF_ENVELOPE;
                    break;
                case REF_SIDECHAIN:
                case REF_LINK:
                    profile_id = PROF_REFERENCE;
                    break;
                case REF_NONE:
                default:
                    profile_id = -1;
                    break;
            }
            set_profile_ready(pMatchRefReady, profile_id);

            // Other static profile readiness
            set_profile_ready(pInReady, PROF_STATIC);
            set_profile_ready(pCapReady, PROF_CAPTURE);
            set_profile_ready(pFileReady, PROF_FILE);
        }

        void matcher::process(size_t samples)
        {
            init_buffers();
            init_level_meters();
            process_file_loading_tasks();
            process_file_processing_tasks();
            process_listen_events();
            process_gc_tasks();
            update_profiles();
            process_kvt_sync_tasks();

            for (size_t offset=0; offset < samples; )
            {
                const size_t to_do  = lsp_min(samples - offset, BUFFER_SIZE, sProcessor.remaining());

                // Perform processing
                bind_buffers(to_do);
                sProcessor.process(to_do);

                for (size_t i=0; i<nChannels; ++i)
                {
                    channel_t *c        = &vChannels[i];

                    process_listen_output(c, to_do);

                    c->vLevel[SM_OUT]   = lsp_max(c->vLevel[SM_OUT], dsp::abs_max(c->vBuffer, to_do));

                    c->sDryDelay.process(vBuffer, c->vIn, to_do);
                    c->sBypass.process(c->vOut, vBuffer, c->vBuffer, to_do);
                }

                // Update position
                advance_buffers(to_do);
                offset             += to_do;
            }

            output_level_meters();
            post_process_profiles();
            process_save_ir_events();
            output_fft_mesh_data();
            output_profile_mesh_data();
            output_file_mesh_data();
            output_filter_mesh_data();
            output_profile_status();
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

                        ptr                += 2;
                        for (size_t k=0; k<meta::matcher::FFT_MESH_SIZE; ++k)
                        {
                            const size_t idx    = vIndices[k];
                            ptr[k]              = fft[idx] * vRevEnvelope[idx] * fFftShift;
                        }

                        ptr[-2]             = 0.0f;
                        ptr[-1]             = ptr[0];
                        ptr                += meta::matcher::FFT_MESH_SIZE;
                        ptr[0]              = ptr[-1];
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
                    else if (!(profile->nFlags & PFLAGS_READY))
                    {
                        const float value = (j == PROF_MATCH) ? GAIN_AMP_0_DB : GAIN_AMP_M_INF_DB;
                        dsp::fill(dst, value, meta::matcher::FFT_MESH_SIZE + 4);
                        continue;
                    }

                    const bool relative_profile = profile_is_relative(j);

                    // Copy profile data
                    const float * const fft     = profile->vData[i];
                    dst                += 2;

                    if (relative_profile)
                    {
                        for (size_t k=0; k<meta::matcher::FFT_MESH_SIZE; ++k)
                        {
                            const size_t idx    = vIndices[k];
                            dst[k]              = fft[idx];
                        }
                    }
                    else
                    {
                        for (size_t k=0; k<meta::matcher::FFT_MESH_SIZE; ++k)
                        {
                            const size_t idx    = vIndices[k];
                            dst[k]              = fft[idx] * vRevEnvelope[idx];
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

        void matcher::output_filter_mesh_data()
        {
            if (!bSyncFilter)
                return;

            plug::mesh_t * mesh = (pFilterMesh != NULL) ? pFilterMesh->buffer<plug::mesh_t>() : NULL;
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
            ptr                 = mesh->pvData[index++];
            dsp::copy(&ptr[2], vFilterCurve, meta::matcher::FFT_MESH_SIZE);
            ptr[0]              = 0.0f;
            ptr[1]              = ptr[2];
            ptr                += meta::matcher::FFT_MESH_SIZE + 2;
            ptr[0]              = ptr[-1];
            ptr[1]              = 0.0f;

            // Report mesh size
            mesh->data(index, meta::matcher::FFT_MESH_SIZE + 4);
            bSyncFilter         = false;
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
            bSyncFilter     = true;
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
            profile->nFlags             = PFLAGS_CHANGED | PFLAGS_SYNC;
            profile->nFrames            = 0;
            profile->fRMS               = GAIN_AMP_M_INF_DB;

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
            const float norm            = NORMING_SHIFT / float(1 << nRank);
            const float k               = 1.0f / float(profile->nFrames);
            const float rms_norm        = 1.0f / float(fft_csize);
            for (size_t i=0; i<profile->nChannels; ++i)
            {
                dsp::mul_k2(profile->vData[i], k*norm, fft_csize);
                profile->fRMS              += dsp::h_sum(profile->vData[i], fft_csize) * rms_norm;
            }
            if (profile->fRMS >= GAIN_AMP_M_72_DB)
                profile->nFlags            |= PFLAGS_READY;

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
                    if (af->nStatus != STATUS_OK)
                    {
                        profile_data_t * const profile = vProfileState[SPROF_FILE].get();
                        if (profile != NULL)
                        {
                            profile->nFlags    &= ~PFLAGS_READY;
                            profile->nFlags    |= PFLAGS_CHANGED | PFLAGS_SYNC;
                        }
                    }
                    ++nFileProcessReq;

                    // Now we surely can commit changes and reset task state
                    path->commit();
                    sFileLoader.reset();
                }
            }
        }

        void matcher::process_kvt_sync_tasks()
        {
            size_t dirty = 0;

            if (sKVTSync.completed())
            {
                ++dirty;
                sKVTSync.reset();
            }

            if (sKVTSync.idle())
            {
                // Check that we have dirty profiles.
                for (size_t i=0; i<SPROF_TOTAL; ++i)
                {
                    if (sKVTSync.submit_profile(i, vProfileState[i].get()))
                        ++dirty;
                }

                if (sKVTSync.pending())
                {
                    // Try to submit task
                    if (pExecutor->submit(&sKVTSync))
                        lsp_trace("Successfully submitted KVT synchronization task");
                }
            }

            // Notify wrapper about state change
            if (dirty > 0)
                pWrapper->state_changed();
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
                if (nRefSource == REF_FILE)
                    bUpdateMatch    = true;

                // Reset configurator task
                sFileProcessor.reset();
                pWrapper->state_changed();
            }
        }

        void matcher::process_save_ir_events()
        {
            if (sIRSaver.idle())
            {
                // Check that path has changed
                plug::path_t *path = (pIRFile != NULL) ? pIRFile->buffer<plug::path_t>() : NULL;
                if ((path != NULL) && (path->pending()))
                {
                    // Accept new file name
                    path->accept();
                    lsp_trace("set IR file name to %s", path->path());
                    sIRSaver.submit_file_name(path->path());

                    // Commit path
                    path->commit();
                }

                // Check that save command is pending
                if (sIRSaver.pending())
                {
                    const profile_data_t * const match = vProfileData[PROF_MATCH];
                    if ((match != NULL) && (match->nFlags & PFLAGS_READY))
                    {
                        sIRSaver.submit_profile(match);
                        if (pExecutor->submit(&sIRSaver))
                        {
                            lsp_trace("Successfully submitted IR save task");
                            pIRStatus->set_value(STATUS_IN_PROCESS);
                        }
                    }
                }
            }

            if (sIRSaver.completed())
            {
                // Update save status
                const status_t code = sIRSaver.code();
                if (code == STATUS_OK)
                    pIRProgress->set_value(100.0f);
                pIRStatus->set_value(code);

                sIRSaver.reset();
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

            // Need to immediately stop playback?
            if (!f->bCanListen)
            {
                for (size_t j=0; j<nChannels; ++j)
                {
                    channel_t *c = &vChannels[j];
                    c->sPlayback.cancel(fadeout, 0);
                }
                return;
            }

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

        bool matcher::save_profile(core::KVTStorage *kvt, const char *path, profile_data_t *profile)
        {
            if (profile == NULL)
                return false;
            if (!(profile->nFlags & PFLAGS_DIRTY))
                return false;

            const size_t fft_csize      = (profile->nFlags & PFLAGS_READY) ? (1 << (profile->nRank - 1)) + 1 : 0;
            const size_t hdr_size       = align_size(sizeof(kvt_profile_header_t), 0x10);
            const size_t channel_size   = fft_csize * sizeof(float);
            const size_t to_alloc       = hdr_size + profile->nChannels * channel_size;

            // Initialize KVT parameter
            core::kvt_param_t p;
            p.type                      = core::KVT_BLOB;
            p.blob.ctype                = strdup(profile_blob_ctype);
            if (p.blob.ctype == NULL)
                return false;
            p.blob.size                 = to_alloc;

            // Allocate memory for BLOB data
            uint8_t *ptr                = static_cast<uint8_t *>(malloc(to_alloc));
            if (ptr == NULL)
            {
                free(const_cast<char *>(p.blob.ctype));
                return false;
            }

            kvt_profile_header_t * const hdr    = advance_ptr_bytes<kvt_profile_header_t>(ptr, hdr_size);
            p.blob.data                 = hdr;
            bzero(hdr, hdr_size);

            uint32_t flags      = KVT_PFLAGS_NONE;
            if (profile->nFlags & PFLAGS_DEFAULT)
                flags              |= KVT_PFLAGS_DEFAULT;
            if (profile->nFlags & PFLAGS_READY)
                flags              |= KVT_PFLAGS_READY;

            hdr->nVersion       = 0;
            hdr->nChannels      = uint8_t(profile->nChannels);
            hdr->nRank          = uint8_t(profile->nRank);
            hdr->nFlags         = flags;
            hdr->nSampleRate    = profile->nSampleRate;
            hdr->nFrames        = profile->nFrames;
            hdr->fRMS           = profile->fRMS;

            // Convert to right endianess
            hdr->nVersion       = CPU_TO_BE(hdr->nVersion);
            hdr->nChannels      = CPU_TO_BE(hdr->nChannels);
            hdr->nRank          = CPU_TO_BE(hdr->nRank);
            hdr->nFlags         = CPU_TO_BE(hdr->nFlags);
            hdr->nSampleRate    = CPU_TO_BE(hdr->nSampleRate);
            hdr->nFrames        = CPU_TO_BE(hdr->nFrames);
            hdr->fRMS           = CPU_TO_BE(hdr->fRMS);

            for (size_t i=0; i<profile->nChannels; ++i)
            {
                float *dst          = advance_ptr_bytes<float>(ptr, channel_size);
                CPU_TO_VBE_COPY(dst, profile->vData[i], fft_csize);
            }

            // Submit data to KVT
            status_t res = kvt->put(path, &p, core::KVT_DELEGATE | core::KVT_TO_UI);
            if (res != STATUS_OK)
            {
                free(const_cast<char *>(p.blob.ctype));
                free(hdr);
                return false;
            }

            // Reset dirty flag
            profile->nFlags    &= ~PFLAGS_DIRTY;

            lsp_trace("profile '%s' saved to KVT", path);

            return true;
        }

        matcher::profile_data_t *matcher::load_profile(const char *path, const core::kvt_param_t *param)
        {
            if (!(param->type == core::KVT_BLOB))
            {
                lsp_warn("KVT parameter '%s' has invalid type, should be BLOB", path);
                return NULL;
            }

            const core::kvt_blob_t *blob = &param->blob;
            if ((blob == NULL) || (blob->ctype == NULL) || (blob->data == NULL))
            {
                lsp_warn("KVT blob for parameter '%s' is NULL", path);
                return NULL;
            }
            if (strcmp(blob->ctype, profile_blob_ctype) != 0)
            {
                lsp_warn("Invalid content type for parameter '%s': %s", path, blob->ctype);
                return NULL;
            }

            const kvt_profile_header_t * hdr = reinterpret_cast<const kvt_profile_header_t *>(blob->data);
            const uint16_t version = BE_TO_CPU(hdr->nVersion);
            if (version != 0)
            {
                lsp_warn("Unsupported BLOB version for parameter '%s': %d", path, int(version));
                return NULL;
            }
            const uint8_t channels = BE_TO_CPU(hdr->nChannels);
            if (channels <= 0)
            {
                lsp_warn("Invalid number of audio channels for parameter '%s': %d", path, int(channels));
                return NULL;
            }
            const uint8_t rank = BE_TO_CPU(hdr->nRank);
            if ((rank < meta::matcher::FFT_RANK_MIN) || (rank > meta::matcher::FFT_RANK_MAX))
            {
                lsp_warn("Invalid FFT rank for parameter '%s': %d", path, int(rank));
                return NULL;
            }
            const uint32_t flags = BE_TO_CPU(hdr->nFlags);
            const uint32_t srate = BE_TO_CPU(hdr->nSampleRate);
            const uint32_t frames = BE_TO_CPU(hdr->nFrames);
            const float rms = BE_TO_CPU(hdr->fRMS);

            // Estimate the real size of BLOB
            const size_t fft_csize      = (1 << (rank - 1)) + 1;
            const size_t hdr_size       = align_size(sizeof(kvt_profile_header_t), 0x10);
            const size_t channel_size   = fft_csize * sizeof(float);
            const size_t to_alloc       = hdr_size + channels * channel_size;
            if (blob->size < to_alloc)
            {
                lsp_warn("Invalid BLOB size for parameter '%s': %d", path, int(rank));
                return NULL;
            }

            // Allocate profile data
            profile_data_t *profile = allocate_profile_data();
            if (profile == NULL)
            {
                lsp_warn("Out of memory while fetching parameter '%s'", path);
                return NULL;
            }

            // Fill profile data
            profile->nSampleRate        = srate;
            profile->nChannels          = channels;
            profile->nRank              = rank;
            profile->nFlags             = PFLAGS_CHANGED | PFLAGS_SYNC;
            if (flags & KVT_PFLAGS_DEFAULT)
                profile->nFlags            |= PFLAGS_DEFAULT;
            if (flags & KVT_PFLAGS_READY)
                profile->nFlags            |= PFLAGS_READY;
            profile->nFrames            = frames;
            profile->fRMS               = rms;

            // Copy profile data
            const float *data           = advance_ptr_bytes<const float>(hdr, hdr_size);
            for (size_t i=0; i<channels; ++i)
            {
                data                        = advance_ptr_bytes<const float>(hdr, channel_size);
                VBE_TO_CPU_COPY(profile->vData[i], data, fft_csize);
            }

            return profile;
        }

        void matcher::dump(dspu::IStateDumper *v, const char *name, const profile_data_t *p)
        {
            // Check profile for NULL
            if (p == NULL)
            {
                if (name != NULL)
                    v->write(name, static_cast<const void *>(NULL));
                else
                    v->write(static_cast<const void *>(NULL));
                return;
            }

            // Write profile object
            if (name != NULL)
                v->begin_object(name, p, sizeof(profile_data_t));
            else
                v->begin_object(p, sizeof(profile_data_t));

            {
                v->write("nSampleRate", p->nSampleRate);
                v->write("nChannels", p->nChannels);
                v->write("nRank", p->nRank);
                v->write("nFlags", p->nFlags);
                v->write("nFrames", p->nFrames);
                v->write("fRMS", p->fRMS);
                v->writev("vData", p->vData, p->nChannels);
            }

            v->end_object();
        }

        void matcher::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            v->write("nChannels", nChannels);

            v->begin_array("vChannels", vChannels, nChannels);
            {
                for (size_t i=0; i<nChannels; ++i)
                {
                    const channel_t * const c = &vChannels[i];

                    v->write_object("sBypass", &c->sBypass);
                    v->write_object("sPlayer", &c->sPlayer);
                    v->write_object("sPlayback", &c->sPlayback);
                    v->write_object("sDryDelay", &c->sDryDelay);
                    v->write_object("sScDelay", &c->sScDelay);

                    v->write("vIn", c->vIn);
                    v->write("vOut", c->vOut);
                    v->write("vSc", c->vSc);
                    v->write("vShmIn", c->vShmIn);
                    v->writev("vFft", c->vFft, SM_TOTAL);
                    v->write("vBuffer", c->vBuffer);
                    v->writev("vLevel", c->vLevel, SM_TOTAL);

                    v->writev("bFft", c->bFft, SM_TOTAL);

                    v->write("pIn", c->pIn);
                    v->write("pOut", c->pOut);
                    v->write("pSc", c->pSc);
                    v->write("pShmIn", c->pShmIn);

                    v->writev("pFft", c->pFft, SM_TOTAL);
                    v->writev("pMeter", c->pMeter, SM_TOTAL);
                }
            }
            v->end_array();

            v->write("nInSource", nInSource);
            v->write("nRefSource", nRefSource);
            v->write("nRawCapSource", nRawCapSource);
            v->write("nCapSource", nCapSource);
            v->write("nRank", nRank);

            v->write("fGainIn", fGainIn);
            v->write("fGainOut", fGainOut);
            v->write("fFftTau", fFftTau);
            v->write("fFftShift", fFftShift);
            v->write("fInTau", fInTau);
            v->write("fRefTau", fRefTau);
            v->write("fStereoLink", fStereoLink);
            v->write("fBlend", fBlend);
            v->write("fHpfFreq", fHpfFreq);
            v->write("fHpfSlope", fHpfSlope);
            v->write("fLpfFreq", fLpfFreq);
            v->write("fLpfSlope", fLpfSlope);
            v->write("fClipFreq", fClipFreq);

            v->write("nFileProcessReq", nFileProcessReq);
            v->write("nFileProcessResp", nFileProcessResp);

            v->write("bSidechain", bSidechain);
            v->write("bProfile", bProfile);
            v->write("bCapture", bCapture);
            v->write("bListen", bListen);
            v->write("bSyncRefFFT", bSyncRefFFT);
            v->write("bSyncFilter", bSyncFilter);
            v->write("bUpdateMatch", bUpdateMatch);
            v->write("bMatchTopLimit", bMatchTopLimit);
            v->write("bMatchBottomLimit", bMatchBottomLimit);

            v->write_object("sProcessor", &sProcessor);
            v->write_object("sMatchImmediate", &sMatchImmediate);

            v->begin_object("sFile", &sFile, sizeof(af_descriptor_t));
            {
                const af_descriptor_t * const af = &sFile;

                v->write_object("sListen", &af->sListen);
                v->write_object("sStop", &af->sStop);
                v->write_object("pOriginal", af->pOriginal);
                v->write_object("pProcessed", af->pProcessed);
                v->writev("vThumbs", af->vThumbs, 2);

                v->write("fNorm", af->fNorm);
                v->write("nStatus", af->nStatus);
                v->write("bSync", af->bSync);
                v->write("bCanListen", af->bCanListen);

                v->write("fPitch", af->fPitch);
                v->write("fHeadCut", af->fHeadCut);
                v->write("fTailCut", af->fTailCut);
                v->write("fDuration", af->fDuration);

                v->write("pShowOverlay", af->pShowOverlay);
                v->write("pFile", af->pFile);
                v->write("pPitch", af->pPitch);
                v->write("pHeadCut", af->pHeadCut);
                v->write("pTailCut", af->pTailCut);
                v->write("pListen", af->pListen);
                v->write("pStop", af->pStop);
                v->write("pStatus", af->pStatus);
                v->write("pLength", af->pLength);
                v->write("pThumbs", af->pThumbs);
                v->write("pPlayPosition", af->pPlayPosition);
            }
            v->end_object();

            v->write_object("sFileLoader", &sFileLoader);
            v->write_object("sFileProcessor", &sFileProcessor);
            v->write_object("sKVTSync", &sKVTSync);
            v->write_object("sIRSaver", &sIRSaver);
            v->write_object("sGCTask", &sGCTask);

            v->begin_array("vMatchBands", vMatchBands, meta::matcher::MATCH_BANDS);
            {
                for (size_t i=0; i<meta::matcher::MATCH_BANDS; ++i)
                {
                    const match_band_t * const b = &vMatchBands[i];

                    v->writev("vParams", b->vParams, EQP_TOTAL);
                    v->writev("pParams", b->pParams, EQP_TOTAL);
                }
            }
            v->end_array();

            v->write("pExecutor", pExecutor);
            v->write("pGCList", &pGCList);
            dump(v, "pReactivity", pReactivity);
            dump(v, "pTempProfile", pTempProfile);
            dump(v, "pFilterProfile", pFilterProfile);
            dump(v, "pMatchProfile", pMatchProfile);

            v->begin_array("vProfileData", vProfileData, PROF_TOTAL);
            {
                for (size_t i=0; i<PROF_TOTAL; ++i)
                {
                    const profile_data_t * const p = vProfileData[i];
                    dump(v, NULL, p);
                }
            }
            v->end_array();

            v->begin_array("vProfileState", vProfileState, SPROF_TOTAL);
            {
                for (size_t i=0; i<SPROF_TOTAL; ++i)
                {
                    const profile_data_t * const p = vProfileState[i].current();
                    dump(v, NULL, p);
                }
            }
            v->end_array();

            v->write("vIndices", vIndices);
            v->write("vFreqs", vFreqs);
            v->write("vFilterCurve", vFilterCurve);
            v->write("vEnvelope", vEnvelope);
            v->write("vRevEnvelope", vRevEnvelope);
            v->write("vBuffer", vBuffer);
            v->write("vEmptyBuf", vEmptyBuf);

            v->write("pBypass", pBypass);
            v->write("pGainIn", pGainIn);
            v->write("pGainOut", pGainOut);
            v->write("pFftSize", pFftSize);
            v->write("pInReactivity", pInReactivity);
            v->write("pRefReactivity", pRefReactivity);
            v->write("pInSource", pInSource);
            v->write("pRefSource", pRefSource);
            v->write("pCapSource", pCapSource);
            v->write("pBlend", pBlend);
            v->write("pProfile", pProfile);
            v->write("pCapture", pCapture);
            v->write("pListen", pListen);
            v->write("pHpfOn", pHpfOn);
            v->write("pHpfFreq", pHpfFreq);
            v->write("pHpfSlope", pHpfSlope);
            v->write("pLpfOn", pLpfOn);
            v->write("pLpfFreq", pLpfFreq);
            v->write("pLpfSlope", pLpfSlope);
            v->write("pClipOn", pClipOn);
            v->write("pClipFreq", pClipFreq);
            v->write("pMatchInReady", pMatchInReady);
            v->write("pMatchRefReady", pMatchRefReady);
            v->write("pInReady", pInReady);
            v->write("pCapReady", pCapReady);
            v->write("pFileReady", pFileReady);
            v->write("pFilterMesh", pFilterMesh);
            v->write("pStereoLink", pStereoLink);

            v->write("pIRFile", pIRFile);
            v->write("pIRSave", pIRSave);
            v->write("pIRStatus", pIRStatus);
            v->write("pIRProgress", pIRProgress);

            v->write("pMatchTopLimit", pMatchTopLimit);
            v->write("pMatchBottomLimit", pMatchBottomLimit);
            v->write("pMatchLimit", pMatchLimit);
            v->write("pMatchImmediate", pMatchImmediate);
            v->write("pMatchMesh", pMatchMesh);

            v->write("pFftReact", pFftReact);
            v->write("pFftShift", pFftShift);
            v->write("pFftMesh", pFftMesh);

            v->write("pData", pData);
        }

    } /* namespace plugins */
} /* namespace lsp */


