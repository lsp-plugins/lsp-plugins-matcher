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

#include <lsp-plug.in/plug-fw/meta/ports.h>
#include <lsp-plug.in/shared/meta/developers.h>
#include <private/meta/matcher.h>

#define LSP_PLUGINS_MATCHER_VERSION_MAJOR       1
#define LSP_PLUGINS_MATCHER_VERSION_MINOR       0
#define LSP_PLUGINS_MATCHER_VERSION_MICRO       0

#define LSP_PLUGINS_MATCHER_VERSION  \
    LSP_MODULE_VERSION( \
        LSP_PLUGINS_MATCHER_VERSION_MAJOR, \
        LSP_PLUGINS_MATCHER_VERSION_MINOR, \
        LSP_PLUGINS_MATCHER_VERSION_MICRO  \
    )

namespace lsp
{
    namespace meta
    {
        //-------------------------------------------------------------------------
        // Plugin metadata

        static const port_t matcher_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            BYPASS,

            PORTS_END
        };

        static const port_t sc_matcher_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            PORTS_MONO_SIDECHAIN,
            BYPASS,

            PORTS_END
        };

        static const port_t matcher_stereo_ports[] =
        {
            PORTS_STEREO_PLUGIN,
            BYPASS,

            PORTS_END
        };

        static const port_t sc_matcher_stereo_ports[] =
        {
            PORTS_STEREO_PLUGIN,
            PORTS_STEREO_SIDECHAIN,
            BYPASS,

            PORTS_END
        };

        static const int plugin_classes[]       = { C_MULTI_EQ, -1 };
        static const int clap_features_mono[]   = { CF_AUDIO_EFFECT, CF_EQUALIZER, CF_MONO, -1 };
        static const int clap_features_stereo[] = { CF_AUDIO_EFFECT, CF_EQUALIZER, CF_STEREO, -1 };

        const meta::bundle_t matcher_bundle =
        {
            "matcher",
            "Matcher",
            B_EQUALIZERS,
            "", // TODO: provide ID of the video on YouTube
            "This plugins performs static and dynamic spectral matching of the audio signal according to the spectrum of the reference signal"
        };

        const plugin_t matcher_mono =
        {
            "Matcher Mono",
            "Matcher Mono",
            "Matcher Mono",
            "MQ1M",
            &developers::v_sadovnikov,
            "matcher_mono",
            {
                LSP_LV2_URI("matcher_mono"),
                LSP_LV2UI_URI("matcher_mono"),
                "MQ1M",
                LSP_VST3_UID("mq1m    MQ1M"),
                LSP_VST3UI_UID("mq1m    MQ1M"),
                0,
                NULL,
                LSP_CLAP_URI("matcher_mono"),
                LSP_GST_UID("matcher_mono"),
            },
            LSP_PLUGINS_MATCHER_VERSION,
            plugin_classes,
            clap_features_mono,
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY,
            matcher_mono_ports,
            "equalizer/matcher.xml",
            NULL,
            mono_plugin_port_groups,
            &matcher_bundle
        };

        const plugin_t matcher_stereo =
        {
            "Matcher Stereo",
            "Matcher Stereo",
            "Matcher Stereo",
            "MQ1S",
            &developers::v_sadovnikov,
            "matcher_stereo",
            {
                LSP_LV2_URI("matcher_stereo"),
                LSP_LV2UI_URI("matcher_stereo"),
                "MQ1S",
                LSP_VST3_UID("mq1s    MQ1S"),
                LSP_VST3UI_UID("mq1s    MQ1S"),
                0,
                NULL,
                LSP_CLAP_URI("matcher_stereo"),
                LSP_GST_UID("matcher_stereo"),
            },
            LSP_PLUGINS_MATCHER_VERSION,
            plugin_classes,
            clap_features_stereo,
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY,
            matcher_stereo_ports,
            "equalizer/matcher.xml",
            NULL,
            stereo_plugin_port_groups,
            &matcher_bundle
        };

        const plugin_t sc_matcher_mono =
        {
            "Sidechain Matcher Mono",
            "Sidechain Matcher Mono",
            "Sidechain Matcher Mono",
            "SCMQ1M",
            &developers::v_sadovnikov,
            "matcher_mono",
            {
                LSP_LV2_URI("matcher_mono"),
                LSP_LV2UI_URI("matcher_mono"),
                "MQsM",
                LSP_VST3_UID("scmq1m  MQsM"),
                LSP_VST3UI_UID("scmq1m  MQsM"),
                0,
                NULL,
                LSP_CLAP_URI("matcher_mono"),
                LSP_GST_UID("matcher_mono"),
            },
            LSP_PLUGINS_MATCHER_VERSION,
            plugin_classes,
            clap_features_mono,
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY,
            sc_matcher_mono_ports,
            "equalizer/matcher.xml",
            NULL,
            mono_plugin_port_groups,
            &matcher_bundle
        };

        const plugin_t sc_matcher_stereo =
        {
            "Sidechain Matcher Stereo",
            "Sidechain Matcher Stereo",
            "Sidechain Matcher Stereo",
            "SCMQ1S",
            &developers::v_sadovnikov,
            "matcher_stereo",
            {
                LSP_LV2_URI("matcher_stereo"),
                LSP_LV2UI_URI("matcher_stereo"),
                "MQsS",
                LSP_VST3_UID("scmq1s  MQsS"),
                LSP_VST3UI_UID("scmq1s  MQsS"),
                0,
                NULL,
                LSP_CLAP_URI("matcher_stereo"),
                LSP_GST_UID("matcher_stereo"),
            },
            LSP_PLUGINS_MATCHER_VERSION,
            plugin_classes,
            clap_features_stereo,
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY,
            sc_matcher_stereo_ports,
            "equalizer/matcher.xml",
            NULL,
            stereo_plugin_port_groups,
            &matcher_bundle
        };
    } /* namespace meta */
} /* namespace lsp */



