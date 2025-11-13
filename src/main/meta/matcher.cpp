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
        static const port_item_t matcher_fft_ranks[] =
        {
            { "256",                NULL },
            { "512",                NULL },
            { "1024",               NULL },
            { "2048",               NULL },
            { "4096",               NULL },
            { "8192",               NULL },
            { "16384",              NULL },
            { NULL, NULL }
        };

        static const port_item_t matcher_references[] =
        {
            { "Capture",            "matcher.ref.capture"           },
            { "File",               "matcher.ref.file"              },
            { "Equalizer",          "matcher.ref.equalizer"         },
            { "Link",               "matcher.ref.link"              },
            { NULL, NULL }
        };

        static const port_item_t sc_matcher_references[] =
        {
            { "Capture",            "matcher.ref.capture"           },
            { "File",               "matcher.ref.file"              },
            { "Equalizer",          "matcher.ref.equalizer"         },
            { "Sidechain",          "matcher.ref.sidechain"         },
            { "Link",               "matcher.ref.link"              },
            { NULL, NULL }
        };

        static const port_item_t matcher_capture_source[] =
        {
            { "In",                 "matcher.capture.in"            },
            { "Link",               "matcher.capture.link"          },
            { NULL, NULL }
        };

        static const port_item_t sc_matcher_capture_source[] =
        {
            { "In",                 "matcher.capture.in"            },
            { "Sidehcain",          "matcher.capture.sidechain"     },
            { "Link",               "matcher.capture.link"          },
            { NULL, NULL }
        };

        static const port_item_t matcher_modes[] =
        {
            { "Static",             "matcher.mode.static"           },
            { "Dynamic",            "matcher.mode.dynamic"          },
            { NULL, NULL }
        };

        #define MATCHER_COMMON(sources, captures, cap_default) \
            BYPASS, \
            IN_GAIN, \
            OUT_GAIN, \
            COMBO("fft", "FFT size", "FFT size", matcher::FFT_RANK_IDX_DFL, matcher_fft_ranks), \
            SWITCH("rst_in", "Reset input signal profile", "Reset In", 1.0f), \
            SWITCH("rst_ref", "Reset reference signal profile", "Reset Ref", 1.0f), \
            SWITCH("rst_cap", "Reset captured signal profile", "Reset Cap", 1.0f), \
            LOG_CONTROL("rct_in", "Input profile reactivity", "React In", U_SEC, matcher::PROFILE_REACT_TIME), \
            LOG_CONTROL("rct_ref", "Reference profile reactivity", "React Ref", U_SEC, matcher::PROFILE_REACT_TIME), \
            COMBO("ref_src", "Reference source", "Reference", 0, sources), \
            COMBO("cap_src", "Capture source", "Capture src", cap_default, captures), \
            SWITCH("profile", "Profile", "Profile", 1.0f), \
            SWITCH("capture", "Capture", "Capture", 0.0f), \
            SWITCH("listen", "Listen capture", "Listen", 1.0f)

        #define MATCHER_COMMON_STEREO \
            PERCENTS("slink", "Stereo link", "Stereo link", 100.0f, 0.1f)

        #define MATCHER_EQ_BAND(id, freq) \
            CONTROL("amp_" #id, "Amplification " freq "Hz", "Amp " freq "Hz", U_DB, matcher::BAND_AMP_GAIN), \
            CONTROL("red_" #id, "Reduction " freq "Hz", "Red " freq "Hz", U_DB, matcher::BAND_RED_GAIN), \
            CONTROL("spd_" #id, "Reactivity " freq "Hz", "Red " freq "Hz", U_DB, matcher::BAND_REACT)

        #define MATCHER_EQ(channels) \
            COMBO("mode", "Operating mode", "Mode", 0, matcher_modes), \
            TRIGGER("reset", "Reset match curves", "Reset"), \
            TRIGGER("match", "Perform immediate match", "Match"), \
            MATCHER_EQ_BAND(0, "25"), \
            MATCHER_EQ_BAND(1, "50"), \
            MATCHER_EQ_BAND(2, "107"), \
            MATCHER_EQ_BAND(3, "227"), \
            MATCHER_EQ_BAND(4, "484"), \
            MATCHER_EQ_BAND(5, "1 k"), \
            MATCHER_EQ_BAND(6, "2.2 k"), \
            MATCHER_EQ_BAND(7, "4.7 k"), \
            MATCHER_EQ_BAND(8, "9 k"), \
            MATCHER_EQ_BAND(9, "16 k"), \
            MESH("pmesh", "Match profile mesh characteristics", 3 + 2 * channels, matcher::FFT_MESH_SIZE)

        #define MATCHER_METERS(id, label, alias) \
            SWITCH("ifft" id, "Input FFT enabled", "FFT In" alias, 1), \
            SWITCH("offt" id, "Output FFT enabled", "FFT Out" alias, 1), \
            SWITCH("cfft" id, "Capture FFT enabled", "FFT Cap" alias, 1), \
            METER_GAIN("ilm" id, "Input level meter" label, GAIN_AMP_P_24_DB), \
            METER_GAIN("olm" id, "Output level meter" label, GAIN_AMP_P_24_DB), \
            METER_GAIN("clm" id, "Capture level meter" label, GAIN_AMP_P_24_DB)

        #define MATCHER_METERS_MONO \
            MATCHER_METERS("", "", ""), \
            MESH("fft", "Signal metering mesh", 1 + 3*1, matcher::FFT_MESH_SIZE + 4)

        #define MATCHER_METERS_STEREO \
            MATCHER_METERS("_l", " Left", "L"), \
            MATCHER_METERS("_r", " Right", "R"), \
            MESH("fft", "Signal metering mesh", 1 + 3*2, matcher::FFT_MESH_SIZE + 4)

        #define MATCHER_SHM_LINK_MONO \
            OPT_RETURN_MONO("link", "shml", "Side-chain shared memory link")

        #define MATCHER_SHM_LINK_STEREO \
            OPT_RETURN_STEREO("link", "shml_", "Side-chain shared memory link")

        static const port_t matcher_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            MATCHER_SHM_LINK_MONO,
            MATCHER_COMMON(matcher_references, matcher_capture_source, 0),
            MATCHER_EQ(1),
            MATCHER_METERS_MONO,

            PORTS_END
        };

        static const port_t matcher_stereo_ports[] =
        {
            PORTS_STEREO_PLUGIN,
            MATCHER_SHM_LINK_STEREO,
            MATCHER_COMMON(matcher_references, matcher_capture_source, 0),
            MATCHER_COMMON_STEREO,
            MATCHER_EQ(2),
            MATCHER_METERS_STEREO,

            PORTS_END
        };

        static const port_t sc_matcher_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            PORTS_MONO_SIDECHAIN,
            MATCHER_SHM_LINK_MONO,
            MATCHER_COMMON(sc_matcher_references, sc_matcher_capture_source, 1),
            MATCHER_EQ(1),
            MATCHER_METERS_MONO,

            PORTS_END
        };

        static const port_t sc_matcher_stereo_ports[] =
        {
            PORTS_STEREO_PLUGIN,
            PORTS_STEREO_SIDECHAIN,
            MATCHER_SHM_LINK_STEREO,
            MATCHER_COMMON(sc_matcher_references, sc_matcher_capture_source, 1),
            MATCHER_COMMON_STEREO,
            MATCHER_EQ(2),
            MATCHER_METERS_STEREO,

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
            "sc_matcher_mono",
            {
                LSP_LV2_URI("sc_matcher_mono"),
                LSP_LV2UI_URI("sc_matcher_mono"),
                "MQsM",
                LSP_VST3_UID("scmq1m  MQsM"),
                LSP_VST3UI_UID("scmq1m  MQsM"),
                0,
                NULL,
                LSP_CLAP_URI("sc_matcher_mono"),
                LSP_GST_UID("sc_matcher_mono"),
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
            "sc_matcher_stereo",
            {
                LSP_LV2_URI("sc_matcher_stereo"),
                LSP_LV2UI_URI("sc_matcher_stereo"),
                "MQsS",
                LSP_VST3_UID("scmq1s  MQsS"),
                LSP_VST3UI_UID("scmq1s  MQsS"),
                0,
                NULL,
                LSP_CLAP_URI("sc_matcher_stereo"),
                LSP_GST_UID("sc_matcher_stereo"),
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



