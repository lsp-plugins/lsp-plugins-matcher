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

#ifndef PRIVATE_META_MATCHER_H_
#define PRIVATE_META_MATCHER_H_

#include <lsp-plug.in/plug-fw/meta/types.h>
#include <lsp-plug.in/plug-fw/const.h>

namespace lsp
{
    //-------------------------------------------------------------------------
    // Plugin metadata
    namespace meta
    {
        typedef struct matcher
        {
            static constexpr size_t         FFT_RANK_MIN                = 8;
            static constexpr size_t         FFT_RANK_MAX                = 14;
            static constexpr size_t         FFT_RANK_IDX_DFL            = 4;
            static constexpr size_t         FFT_MESH_SIZE               = 640;
            static constexpr size_t         MATCH_BANDS                 = 10;
            static constexpr size_t         SAMPLE_MESH_SIZE            = 600;          // Maximum mesh size for sample file

            static constexpr float          PROFILE_REACT_TIME_MIN      = 0.100f;
            static constexpr float          PROFILE_REACT_TIME_MAX      = 10.000f;
            static constexpr float          PROFILE_REACT_TIME_DFL      = 1.000f;
            static constexpr float          PROFILE_REACT_TIME_STEP     = 0.01f;

            static constexpr float          BAND_AMP_GAIN_MIN           = 0.0f;
            static constexpr float          BAND_AMP_GAIN_MAX           = 36.0f;
            static constexpr float          BAND_AMP_GAIN_DFL           = 12.0f;
            static constexpr float          BAND_AMP_GAIN_STEP          = 0.05f;

            static constexpr float          BAND_RED_GAIN_MIN           = -36.0f;
            static constexpr float          BAND_RED_GAIN_MAX           = 0.0f;
            static constexpr float          BAND_RED_GAIN_DFL           = -12.0f;
            static constexpr float          BAND_RED_GAIN_STEP          = 0.05f;

            static constexpr float          BAND_REF_GAIN_MIN           = -36.0f;
            static constexpr float          BAND_REF_GAIN_MAX           = 36.0f;
            static constexpr float          BAND_REF_GAIN_DFL           = 0.0f;
            static constexpr float          BAND_REF_GAIN_STEP          = 0.05f;

            static constexpr float          BAND_REACT_MIN              = 0.5f;
            static constexpr float          BAND_REACT_MAX              = 10.0f;
            static constexpr float          BAND_REACT_DFL              = 1.0f;
            static constexpr float          BAND_REACT_STEP             = 0.05f;

            static constexpr float          REACT_TIME_MIN              = 0.000f;
            static constexpr float          REACT_TIME_MAX              = 1.000f;
            static constexpr float          REACT_TIME_DFL              = 0.200f;
            static constexpr float          REACT_TIME_STEP             = 0.001f;

            static constexpr float          SAMPLE_LENGTH_MIN           = 0.0f;         // Minimum length (s)
            static constexpr float          SAMPLE_LENGTH_MAX           = 1000.0f;      // Maximum sample length (s)
            static constexpr float          SAMPLE_LENGTH_DFL           = 0.0f;         // Sample length (s)
            static constexpr float          SAMPLE_LENGTH_STEP          = 0.005f;       // Sample step (s)

            static constexpr float          SAMPLE_PLAYBACK_MIN         = -1.0f;        // Minimum playback position (s)
            static constexpr float          SAMPLE_PLAYBACK_MAX         = 1000.0f;      // Maximum playback posotion (s)
            static constexpr float          SAMPLE_PLAYBACK_DFL         = -1.0f;        // Default playback position (s)
            static constexpr float          SAMPLE_PLAYBACK_STEP        = 0.01f;        // Playback step (s)

            static constexpr float          SAMPLE_PITCH_MIN            = -24.0f;       // Minimum pitch (st)
            static constexpr float          SAMPLE_PITCH_MAX            = 24.0f;        // Maximum pitch (st)
            static constexpr float          SAMPLE_PITCH_DFL            = 0.0f;         // Pitch (st)
            static constexpr float          SAMPLE_PITCH_STEP           = 0.01f;        // Pitch step (st)

            static const float              eq_frequencies[];
        } matcher;

        // Plugin type metadata
        extern const plugin_t matcher_mono;
        extern const plugin_t matcher_stereo;
        extern const plugin_t sc_matcher_mono;
        extern const plugin_t sc_matcher_stereo;

    } /* namespace meta */
} /* namespace lsp */

#endif /* PRIVATE_META_MATCHER_H_ */
