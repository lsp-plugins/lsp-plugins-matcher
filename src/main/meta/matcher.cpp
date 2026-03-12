/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/common/status.h>
#include <lsp-plug.in/plug-fw/meta/ports.h>
#include <lsp-plug.in/plug-fw/meta/registry.h>
#include <lsp-plug.in/shared/meta/developers.h>
#include <private/meta/matcher.h>

#define LSP_PLUGINS_MATCHER_VERSION_MAJOR       1
#define LSP_PLUGINS_MATCHER_VERSION_MINOR       0
#define LSP_PLUGINS_MATCHER_VERSION_MICRO       1

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
            { "None",               "matcher.ref.none"              },
            { "Capture",            "matcher.ref.capture"           },
            { "File",               "matcher.ref.file"              },
            { "Envelope",           "matcher.ref.envelope"          },
            { "Link",               "matcher.ref.link"              },
            { NULL, NULL }
        };

        static const port_item_t sc_matcher_references[] =
        {
            { "None",               "matcher.ref.none"               },
            { "Capture",            "matcher.ref.capture"           },
            { "File",               "matcher.ref.file"              },
            { "Envelope",           "matcher.ref.envelope"          },
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

        static const port_item_t matcher_input_source[] =
        {
            { "Static",             "matcher.mode.static"           },
            { "Dynamic",            "matcher.mode.dynamic"          },
            { NULL, NULL }
        };

        const float matcher::eq_frequencies[] =
        {
            25.0f, 50.0f, 107.0f, 227.0f, 484.0f, 1000.0f, 2200.0f, 4700.0f, 9000.0f, 16000.0f
        };

        #define MATCHER_COMMON(sources, captures, cap_default) \
            BYPASS, \
            IN_GAIN, \
            OUT_GAIN, \
            COMBO("fft_sz", "FFT size", "FFT size", matcher::FFT_RANK_IDX_DFL, matcher_fft_ranks), \
            LOG_CONTROL("rct_in", "Input profile reactivity", "React In", U_SEC, matcher::PROFILE_REACT_TIME), \
            LOG_CONTROL("rct_ref", "Reference profile reactivity", "React Ref", U_SEC, matcher::PROFILE_REACT_TIME), \
            COMBO("in_src", "Input source", "Input src", 0, matcher_input_source), \
            COMBO("ref_src", "Reference source", "Reference src", 1, sources), \
            COMBO("cap_src", "Capture source", "Capture src", cap_default, captures), \
            PERCENTS("blend", "Blend signal", "Blend signal", 100.0f, 0.05f), \
            SWITCH("profile", "Profile", "Profile", 0.0f), \
            SWITCH("capture", "Capture", "Capture", 0.0f), \
            SWITCH("listen", "Listen capture", "Listen", 0.0f), \
            SWITCH("hpf_on", "High-pass filter enable", "HPF On", 1.0f), \
            LOG_CONTROL("hpf_f", "High-pass filter frequency", "HPF Freq", U_HZ, matcher::HPF_FREQ), \
            CONTROL("hpf_s", "High-pass filter slope", "HPF Slope", U_DB, matcher::FLT_SLOPE), \
            SWITCH("lpf_on", "Low-pass filter enable", "LPF On", 1.0f), \
            LOG_CONTROL("lpf_f", "Low-pass filter frequency", "LPF Freq", U_HZ, matcher::LPF_FREQ), \
            CONTROL("lpf_s", "Low-pass filter slope", "LPF Slope", U_DB, matcher::FLT_SLOPE), \
            SWITCH("clip_on", "Enable brickwall clipping of high frequencies", "Clip On", 1.0f), \
            LOG_CONTROL("clip_f", "Brickwall clipping frequency", "Clip Freq", U_HZ, matcher::CLIP_FREQ), \
            BLINK("in_rdy", "Match input profile ready"), \
            BLINK("ref_rdy", "Match reference profile ready"), \
            BLINK("sprdy", "Static input profile ready"), \
            BLINK("cprdy", "Capture profile ready"), \
            BLINK("fprdy", "File profile ready"), \
            MESH("fltmesh", "Filter mesh characteristics", 2, matcher::FFT_MESH_SIZE + 4)

        #define MATCHER_COMMON_STEREO \
            PERCENTS("slink", "Stereo link", "Stereo link", 0.0f, 0.05f)

        #define MATCHER_EQ_BAND(id, freq) \
            CONTROL("ref_" #id, "Reference level " freq "Hz", "Ref " freq "Hz", U_DB, matcher::BAND_REF_GAIN), \
            CONTROL("amp_" #id, "Amplification threshold " freq "Hz", "Amp " freq "Hz", U_DB, matcher::BAND_AMP_GAIN), \
            CONTROL("red_" #id, "Reduction threshold " freq "Hz", "Red " freq "Hz", U_DB, matcher::BAND_RED_GAIN), \
            CONTROL("spd_" #id, "Reactivity " freq "Hz", "Speed " freq "Hz", U_SEC, matcher::BAND_REACT)

        #define MATCHER_EQ(channels) \
            SWITCH("showfft", "Show any FFT analysis for all channels", "Show FFT", 1), \
            SWITCH("showpro", "Show any profiles", "Show profiles", 1), \
            SWITCH("showenv", "Show profile envelope", "Show env", 0), \
            SWITCH("showlim", "Show profile limiting", "Show limit", 0), \
            SWITCH("showmrp", "Show profile morphing", "Show morph", 0), \
            SWITCH("showflt", "Show filters", "Show filters", 0), \
            SWITCH("track", "Enable tracking of dynamic profiles", "Track dynamic", 1), \
            SWITCH("tlimit", "Enable profile limiting from top", "Top limit", 1), \
            SWITCH("blimit", "Enable profile limiting from bottom", "Bottom limit", 1), \
            SWITCH("limit", "Enable profile limiting", "Limit", 0), \
            TRIGGER("match", "Perform immediate matching", "Match"), \
            SWITCH("mrplink", "Morph time linking", "Morph Link", 0), \
            MATCHER_EQ_BAND(1, "25"), \
            MATCHER_EQ_BAND(2, "50"), \
            MATCHER_EQ_BAND(3, "107"), \
            MATCHER_EQ_BAND(4, "227"), \
            MATCHER_EQ_BAND(5, "484"), \
            MATCHER_EQ_BAND(6, "1 k"), \
            MATCHER_EQ_BAND(7, "2.2 k"), \
            MATCHER_EQ_BAND(8, "4.7 k"), \
            MATCHER_EQ_BAND(9, "9 k"), \
            MATCHER_EQ_BAND(10, "16 k"), \
            MESH("pmesh", "Match profile mesh characteristics", 1 + 10 * channels, matcher::FFT_MESH_SIZE + 4)

        #define MATCHER_FILE_SOURCE(channels) \
            SWITCH("showfil", "Show file loading", "Show file", 0), \
            PATH("file", "Reference file"),    \
            CONTROL("fpitch", "File pitch", "File pitch", U_SEMITONES, matcher::SAMPLE_PITCH), \
            CONTROL("fhcut", "Head cut", "Head cut", U_SEC, matcher::SAMPLE_LENGTH), \
            CONTROL("ftcut", "Tail cut", "Tail cut", U_SEC, matcher::SAMPLE_LENGTH), \
            TRIGGER("fplay", "Listen file preview", "Play"), \
            TRIGGER("fstop", "File preview stop", "Stop"), \
            STATUS("fstatus", "File load status"), \
            METER("flength", "File length", U_SEC, matcher::SAMPLE_LENGTH), \
            MESH("fmesh", "File contents", channels, matcher::SAMPLE_MESH_SIZE), \
            METER("fppos", "Sample play position", U_SEC, matcher::SAMPLE_PLAYBACK)

        #define MATCHER_METERS_COMMON(channels) \
            LOG_CONTROL("react", "FFT reactivity", "Reactivity", U_MSEC, matcher::REACT_TIME), \
            AMP_GAIN("shift", "FFT Shift gain", "Shift gain", 1.0f, 100.0f), \
            MESH("fft", "Signal metering mesh", 1 + 4*channels, matcher::FFT_MESH_SIZE + 4)

        #define MATCHER_METERS(id, label, alias) \
            SWITCH("ife" id, "Input FFT enabled", "FFT In" alias, 1), \
            SWITCH("ofe" id, "Output FFT enabled", "FFT Out" alias, 1), \
            SWITCH("cfe" id, "Capture FFT enabled", "FFT Cap" alias, 1), \
            SWITCH("rfe" id, "Reference FFT enabled", "FFT Ref" alias, 1), \
            SWITCH("psfe" id, "Draw static input profile", "Stat prof" alias, 0), \
            SWITCH("pcfe" id, "Draw capture profile", "Capt prof" alias, 0), \
            SWITCH("pffe" id, "Draw file profile", "File prof" alias, 0), \
            SWITCH("pefe" id, "Draw envelope profile", "Env prof" alias, 0), \
            SWITCH("pife" id, "Draw dynamic input profile", "In prof" alias, 0), \
            SWITCH("prfe" id, "Draw dynamic reference profile", "Ref prof" alias, 0), \
            SWITCH("pmfe" id, "Draw resulting match profile", "Match prof" alias, 1), \
            METER_GAIN("ilm" id, "Input level meter" label, GAIN_AMP_P_24_DB), \
            METER_GAIN("olm" id, "Output level meter" label, GAIN_AMP_P_24_DB), \
            METER_GAIN("clm" id, "Capture level meter" label, GAIN_AMP_P_24_DB), \
            METER_GAIN("rlm" id, "Reference level meter" label, GAIN_AMP_P_24_DB)

        #define MATCHER_IR_FILE \
            PATH("ir_file", "Output IR file name"), \
            TRIGGER("ir_save", "Save output IR file command", "Save IR"), \
            STATUS("ir_stat", "Output IR saving status"), \
            METER_PERCENT("ir_prog", "IR file saving progress")

        #define MATCHER_METERS_MONO \
            MATCHER_METERS_COMMON(1), \
            MATCHER_METERS("", "", "")

        #define MATCHER_METERS_STEREO \
            MATCHER_METERS_COMMON(2), \
            MATCHER_METERS("_l", " Left", "L"), \
            MATCHER_METERS("_r", " Right", "R")

        #define MATCHER_SHM_LINK_MONO \
            OPT_RETURN_MONO("link", "shml", "Side-chain shared memory link")

        #define MATCHER_SHM_LINK_STEREO \
            OPT_RETURN_STEREO("link", "shml_", "Side-chain shared memory link")

        static const port_t matcher_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            MATCHER_SHM_LINK_MONO,
            MATCHER_COMMON(matcher_references, matcher_capture_source, 0),
            MATCHER_FILE_SOURCE(1),
            MATCHER_EQ(1),
            MATCHER_IR_FILE,
            MATCHER_METERS_MONO,

            PORTS_END
        };

        static const port_t matcher_stereo_ports[] =
        {
            PORTS_STEREO_PLUGIN,
            MATCHER_SHM_LINK_STEREO,
            MATCHER_COMMON(matcher_references, matcher_capture_source, 0),
            MATCHER_COMMON_STEREO,
            MATCHER_FILE_SOURCE(2),
            MATCHER_EQ(2),
            MATCHER_IR_FILE,
            MATCHER_METERS_STEREO,

            PORTS_END
        };

        static const port_t sc_matcher_mono_ports[] =
        {
            PORTS_MONO_PLUGIN,
            PORTS_MONO_SIDECHAIN,
            MATCHER_SHM_LINK_MONO,
            MATCHER_COMMON(sc_matcher_references, sc_matcher_capture_source, 1),
            MATCHER_FILE_SOURCE(1),
            MATCHER_EQ(1),
            MATCHER_IR_FILE,
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
            MATCHER_FILE_SOURCE(2),
            MATCHER_EQ(2),
            MATCHER_IR_FILE,
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
            "sOlWV0B-dUI",
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
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY | E_FILE_PREVIEW,
            matcher_mono_ports,
            "plugins/equalizer/matcher.xml",
            NULL,
            mono_plugin_port_groups,
            &matcher_bundle,
            3
        };
        LSP_REGISTER_METADATA(matcher_mono);

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
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY | E_FILE_PREVIEW,
            matcher_stereo_ports,
            "plugins/equalizer/matcher.xml",
            NULL,
            stereo_plugin_port_groups,
            &matcher_bundle,
            1
        };
        LSP_REGISTER_METADATA(matcher_stereo);

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
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY | E_FILE_PREVIEW,
            sc_matcher_mono_ports,
            "plugins/equalizer/matcher.xml",
            NULL,
            mono_plugin_port_groups,
            &matcher_bundle,
            4
        };
        LSP_REGISTER_METADATA(sc_matcher_mono);

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
            E_DUMP_STATE | E_KVT_SYNC | E_INLINE_DISPLAY | E_FILE_PREVIEW,
            sc_matcher_stereo_ports,
            "plugins/equalizer/matcher.xml",
            NULL,
            stereo_plugin_port_groups,
            &matcher_bundle,
            2
        };
        LSP_REGISTER_METADATA(sc_matcher_stereo);

    } /* namespace meta */
} /* namespace lsp */
