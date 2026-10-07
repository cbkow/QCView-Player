/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — preset tables, transcribed from upstream OpenDRT_v1.1.0.dctl.
 *
 * Every number here is checked by tools/probe_core against the DCTL's own preset
 * code compiled as C++ (tools/dctl_ref.cpp), so a transcription slip fails ctest
 * rather than shipping. Keep the DCTL's field order; the initializers are
 * designated so a reordering is a compile error, not a silent swap.
 */
#include "opendrt.h"

namespace drt {

const DrtLook kLooks[kLookCount] = {
    { .name = "Standard",
      .tn_con = 1.66f, .tn_sh = 0.5f, .tn_toe = 0.003f, .tn_off = 0.005f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 0, .tn_lcon = 0.0f, .tn_lcon_w = 0.5f, .cwp = 2, .cwp_lm = 0.25f, .rs_sa = 0.35f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.25f, .pt_lml_r = 0.5f, .pt_lml_g = 0.0f, .pt_lml_b = 0.1f, .pt_lmh = 0.25f, .pt_lmh_r = 0.5f, .pt_lmh_b = 0.0f, .ptl_enable = 1, .ptl_c = 0.06f, .ptl_m = 0.08f, .ptl_y = 0.06f, .ptm_enable = 1, .ptm_low = 0.4f, .ptm_low_rng = 0.25f, .ptm_low_st = 0.5f, .ptm_high = -0.8f, .ptm_high_rng = 0.35f, .ptm_high_st = 0.4f, .brl_enable = 1, .brl = 0.0f, .brl_r = -2.5f, .brl_g = -1.5f, .brl_b = -1.5f, .brl_rng = 0.5f, .brl_st = 0.35f, .brlp_enable = 1, .brlp = -0.5f, .brlp_r = -1.25f, .brlp_g = -1.25f, .brlp_b = -0.25f, .hc_enable = 1, .hc_r = 1.0f, .hc_r_rng = 0.3f, .hs_rgb_enable = 1, .hs_r = 0.6f, .hs_r_rng = 0.6f, .hs_g = 0.35f, .hs_g_rng = 1.0f, .hs_b = 0.66f, .hs_b_rng = 1.0f, .hs_cmy_enable = 1, .hs_c = 0.25f, .hs_c_rng = 1.0f, .hs_m = 0.0f, .hs_m_rng = 1.0f, .hs_y = 0.0f, .hs_y_rng = 1.0f },
    { .name = "Arriba",
      .tn_con = 1.05f, .tn_sh = 0.5f, .tn_toe = 0.1f, .tn_off = 0.01f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 1.5f, .tn_lcon_w = 0.2f, .cwp = 2, .cwp_lm = 0.25f, .rs_sa = 0.35f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.25f, .pt_lml_r = 0.45f, .pt_lml_g = 0.0f, .pt_lml_b = 0.1f, .pt_lmh = 0.25f, .pt_lmh_r = 0.25f, .pt_lmh_b = 0.0f, .ptl_enable = 1, .ptl_c = 0.06f, .ptl_m = 0.08f, .ptl_y = 0.06f, .ptm_enable = 1, .ptm_low = 1.0f, .ptm_low_rng = 0.4f, .ptm_low_st = 0.5f, .ptm_high = -0.8f, .ptm_high_rng = 0.66f, .ptm_high_st = 0.6f, .brl_enable = 1, .brl = 0.0f, .brl_r = -2.5f, .brl_g = -1.5f, .brl_b = -1.5f, .brl_rng = 0.5f, .brl_st = 0.35f, .brlp_enable = 1, .brlp = 0.0f, .brlp_r = -1.7f, .brlp_g = -2.0f, .brlp_b = -0.5f, .hc_enable = 1, .hc_r = 1.0f, .hc_r_rng = 0.3f, .hs_rgb_enable = 1, .hs_r = 0.6f, .hs_r_rng = 0.8f, .hs_g = 0.35f, .hs_g_rng = 1.0f, .hs_b = 0.66f, .hs_b_rng = 1.0f, .hs_cmy_enable = 1, .hs_c = 0.15f, .hs_c_rng = 1.0f, .hs_m = 0.0f, .hs_m_rng = 1.0f, .hs_y = 0.0f, .hs_y_rng = 1.0f },
    { .name = "Sylvan",
      .tn_con = 1.6f, .tn_sh = 0.5f, .tn_toe = 0.01f, .tn_off = 0.01f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 0.25f, .tn_lcon_w = 0.75f, .cwp = 2, .cwp_lm = 0.25f, .rs_sa = 0.25f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.15f, .pt_lml_r = 0.5f, .pt_lml_g = 0.15f, .pt_lml_b = 0.1f, .pt_lmh = 0.25f, .pt_lmh_r = 0.15f, .pt_lmh_b = 0.15f, .ptl_enable = 1, .ptl_c = 0.05f, .ptl_m = 0.08f, .ptl_y = 0.05f, .ptm_enable = 1, .ptm_low = 0.5f, .ptm_low_rng = 0.5f, .ptm_low_st = 0.5f, .ptm_high = -0.8f, .ptm_high_rng = 0.5f, .ptm_high_st = 0.5f, .brl_enable = 1, .brl = -1.0f, .brl_r = -2.0f, .brl_g = -2.0f, .brl_b = 0.0f, .brl_rng = 0.25f, .brl_st = 0.25f, .brlp_enable = 1, .brlp = -1.0f, .brlp_r = -0.5f, .brlp_g = -0.25f, .brlp_b = -0.25f, .hc_enable = 1, .hc_r = 1.0f, .hc_r_rng = 0.4f, .hs_rgb_enable = 1, .hs_r = 0.6f, .hs_r_rng = 1.15f, .hs_g = 0.8f, .hs_g_rng = 1.25f, .hs_b = 0.6f, .hs_b_rng = 1.0f, .hs_cmy_enable = 1, .hs_c = 0.25f, .hs_c_rng = 0.25f, .hs_m = 0.25f, .hs_m_rng = 0.5f, .hs_y = 0.35f, .hs_y_rng = 0.5f },
    { .name = "Colorful",
      .tn_con = 1.5f, .tn_sh = 0.5f, .tn_toe = 0.003f, .tn_off = 0.003f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 0.4f, .tn_lcon_w = 0.5f, .cwp = 2, .cwp_lm = 0.25f, .rs_sa = 0.35f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.5f, .pt_lml_r = 1.0f, .pt_lml_g = 0.0f, .pt_lml_b = 0.5f, .pt_lmh = 0.15f, .pt_lmh_r = 0.15f, .pt_lmh_b = 0.15f, .ptl_enable = 1, .ptl_c = 0.05f, .ptl_m = 0.06f, .ptl_y = 0.05f, .ptm_enable = 1, .ptm_low = 0.8f, .ptm_low_rng = 0.5f, .ptm_low_st = 0.4f, .ptm_high = -0.8f, .ptm_high_rng = 0.4f, .ptm_high_st = 0.4f, .brl_enable = 1, .brl = 0.0f, .brl_r = -1.25f, .brl_g = -1.25f, .brl_b = -0.25f, .brl_rng = 0.3f, .brl_st = 0.5f, .brlp_enable = 1, .brlp = -0.5f, .brlp_r = -1.25f, .brlp_g = -1.25f, .brlp_b = -0.5f, .hc_enable = 1, .hc_r = 1.0f, .hc_r_rng = 0.4f, .hs_rgb_enable = 1, .hs_r = 0.5f, .hs_r_rng = 0.8f, .hs_g = 0.35f, .hs_g_rng = 1.0f, .hs_b = 0.5f, .hs_b_rng = 1.0f, .hs_cmy_enable = 1, .hs_c = 0.25f, .hs_c_rng = 1.0f, .hs_m = 0.0f, .hs_m_rng = 1.0f, .hs_y = 0.25f, .hs_y_rng = 1.0f },
    { .name = "Aery",
      .tn_con = 1.15f, .tn_sh = 0.5f, .tn_toe = 0.04f, .tn_off = 0.006f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 0.0f, .tn_hcon_st = 0.5f, .tn_lcon_enable = 1, .tn_lcon = 0.5f, .tn_lcon_w = 2.0f, .cwp = 1, .cwp_lm = 0.25f, .rs_sa = 0.25f, .rs_rw = 0.2f, .rs_bw = 0.5f, .pt_enable = 1, .pt_lml = 0.0f, .pt_lml_r = 0.5f, .pt_lml_g = 0.15f, .pt_lml_b = 0.1f, .pt_lmh = 0.0f, .pt_lmh_r = 0.1f, .pt_lmh_b = 0.0f, .ptl_enable = 1, .ptl_c = 0.05f, .ptl_m = 0.08f, .ptl_y = 0.05f, .ptm_enable = 1, .ptm_low = 0.8f, .ptm_low_rng = 0.35f, .ptm_low_st = 0.5f, .ptm_high = -0.9f, .ptm_high_rng = 0.5f, .ptm_high_st = 0.3f, .brl_enable = 1, .brl = -3.0f, .brl_r = 0.0f, .brl_g = 0.0f, .brl_b = 1.0f, .brl_rng = 0.8f, .brl_st = 0.15f, .brlp_enable = 1, .brlp = -1.0f, .brlp_r = -1.0f, .brlp_g = -1.0f, .brlp_b = 0.0f, .hc_enable = 1, .hc_r = 0.5f, .hc_r_rng = 0.25f, .hs_rgb_enable = 1, .hs_r = 0.6f, .hs_r_rng = 1.0f, .hs_g = 0.35f, .hs_g_rng = 2.0f, .hs_b = 0.5f, .hs_b_rng = 1.5f, .hs_cmy_enable = 1, .hs_c = 0.35f, .hs_c_rng = 1.0f, .hs_m = 0.25f, .hs_m_rng = 1.0f, .hs_y = 0.35f, .hs_y_rng = 0.5f },
    { .name = "Dystopic",
      .tn_con = 1.6f, .tn_sh = 0.5f, .tn_toe = 0.01f, .tn_off = 0.008f, .tn_hcon_enable = 1, .tn_hcon = 0.25f, .tn_hcon_pv = 0.0f, .tn_hcon_st = 1.0f, .tn_lcon_enable = 1, .tn_lcon = 1.0f, .tn_lcon_w = 0.75f, .cwp = 3, .cwp_lm = 0.25f, .rs_sa = 0.2f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.15f, .pt_lml_r = 0.0f, .pt_lml_g = 0.0f, .pt_lml_b = 0.0f, .pt_lmh = 0.0f, .pt_lmh_r = 0.0f, .pt_lmh_b = 0.0f, .ptl_enable = 1, .ptl_c = 0.05f, .ptl_m = 0.08f, .ptl_y = 0.05f, .ptm_enable = 1, .ptm_low = 0.25f, .ptm_low_rng = 0.25f, .ptm_low_st = 0.8f, .ptm_high = -0.8f, .ptm_high_rng = 0.6f, .ptm_high_st = 0.25f, .brl_enable = 1, .brl = -2.0f, .brl_r = -2.0f, .brl_g = -2.0f, .brl_b = 0.0f, .brl_rng = 0.35f, .brl_st = 0.35f, .brlp_enable = 1, .brlp = 0.0f, .brlp_r = -1.0f, .brlp_g = -1.0f, .brlp_b = -1.0f, .hc_enable = 1, .hc_r = 1.0f, .hc_r_rng = 0.25f, .hs_rgb_enable = 1, .hs_r = 0.7f, .hs_r_rng = 1.33f, .hs_g = 1.0f, .hs_g_rng = 2.0f, .hs_b = 0.75f, .hs_b_rng = 2.0f, .hs_cmy_enable = 1, .hs_c = 1.0f, .hs_c_rng = 0.5f, .hs_m = 1.0f, .hs_m_rng = 1.0f, .hs_y = 1.0f, .hs_y_rng = 0.765f },
    { .name = "Umbra",
      .tn_con = 1.8f, .tn_sh = 0.5f, .tn_toe = 0.001f, .tn_off = 0.015f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 1.0f, .tn_lcon_w = 1.0f, .cwp = 5, .cwp_lm = 0.25f, .rs_sa = 0.35f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.0f, .pt_lml_r = 0.5f, .pt_lml_g = 0.0f, .pt_lml_b = 0.15f, .pt_lmh = 0.25f, .pt_lmh_r = 0.25f, .pt_lmh_b = 0.0f, .ptl_enable = 1, .ptl_c = 0.05f, .ptl_m = 0.06f, .ptl_y = 0.05f, .ptm_enable = 1, .ptm_low = 0.4f, .ptm_low_rng = 0.35f, .ptm_low_st = 0.66f, .ptm_high = -0.6f, .ptm_high_rng = 0.45f, .ptm_high_st = 0.45f, .brl_enable = 1, .brl = -2.0f, .brl_r = -4.5f, .brl_g = -3.0f, .brl_b = -4.0f, .brl_rng = 0.35f, .brl_st = 0.3f, .brlp_enable = 1, .brlp = 0.0f, .brlp_r = -2.0f, .brlp_g = -1.0f, .brlp_b = -0.5f, .hc_enable = 1, .hc_r = 1.0f, .hc_r_rng = 0.35f, .hs_rgb_enable = 1, .hs_r = 0.66f, .hs_r_rng = 1.0f, .hs_g = 0.5f, .hs_g_rng = 2.0f, .hs_b = 0.85f, .hs_b_rng = 2.0f, .hs_cmy_enable = 1, .hs_c = 0.0f, .hs_c_rng = 1.0f, .hs_m = 0.25f, .hs_m_rng = 1.0f, .hs_y = 0.66f, .hs_y_rng = 0.66f },
    { .name = "Base",
      .tn_con = 1.66f, .tn_sh = 0.5f, .tn_toe = 0.003f, .tn_off = 0.005f, .tn_hcon_enable = 0, .tn_hcon = 0.0f, .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 0, .tn_lcon = 0.0f, .tn_lcon_w = 0.5f, .cwp = 2, .cwp_lm = 0.25f, .rs_sa = 0.35f, .rs_rw = 0.25f, .rs_bw = 0.55f, .pt_enable = 1, .pt_lml = 0.5f, .pt_lml_r = 0.5f, .pt_lml_g = 0.15f, .pt_lml_b = 0.15f, .pt_lmh = 0.8f, .pt_lmh_r = 0.5f, .pt_lmh_b = 0.0f, .ptl_enable = 1, .ptl_c = 0.05f, .ptl_m = 0.06f, .ptl_y = 0.05f, .ptm_enable = 0, .ptm_low = 0.0f, .ptm_low_rng = 0.5f, .ptm_low_st = 0.5f, .ptm_high = 0.0f, .ptm_high_rng = 0.5f, .ptm_high_st = 0.5f, .brl_enable = 0, .brl = 0.0f, .brl_r = 0.0f, .brl_g = 0.0f, .brl_b = 0.0f, .brl_rng = 0.5f, .brl_st = 0.35f, .brlp_enable = 1, .brlp = -0.5f, .brlp_r = -1.6f, .brlp_g = -1.6f, .brlp_b = -0.8f, .hc_enable = 0, .hc_r = 0.0f, .hc_r_rng = 0.25f, .hs_rgb_enable = 0, .hs_r = 0.0f, .hs_r_rng = 1.0f, .hs_g = 0.0f, .hs_g_rng = 1.0f, .hs_b = 0.0f, .hs_b_rng = 1.0f, .hs_cmy_enable = 0, .hs_c = 0.0f, .hs_c_rng = 1.0f, .hs_m = 0.0f, .hs_m_rng = 1.0f, .hs_y = 0.0f, .hs_y_rng = 1.0f },
};

const DrtTonescale kTonescales[kTonescaleCount] = {
    { .name = "Low Contrast",        .tn_con = 1.4f,  .tn_sh = 0.5f,  .tn_toe = 0.003f, .tn_off = 0.005f, .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 0, .tn_lcon = 0.0f,  .tn_lcon_w = 0.5f },
    { .name = "Medium Contrast",     .tn_con = 1.66f, .tn_sh = 0.5f,  .tn_toe = 0.003f, .tn_off = 0.005f, .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 0, .tn_lcon = 0.0f,  .tn_lcon_w = 0.5f },
    { .name = "High Contrast",       .tn_con = 1.4f,  .tn_sh = 0.5f,  .tn_toe = 0.003f, .tn_off = 0.005f, .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 1.0f,  .tn_lcon_w = 0.5f },
    { .name = "Arriba Tonescale",    .tn_con = 1.05f, .tn_sh = 0.5f,  .tn_toe = 0.1f,   .tn_off = 0.01f,  .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 1.5f,  .tn_lcon_w = 0.2f },
    { .name = "Sylvan Tonescale",    .tn_con = 1.6f,  .tn_sh = 0.5f,  .tn_toe = 0.01f,  .tn_off = 0.01f,  .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 0.25f, .tn_lcon_w = 0.75f },
    { .name = "Colorful Tonescale",  .tn_con = 1.5f,  .tn_sh = 0.5f,  .tn_toe = 0.003f, .tn_off = 0.003f, .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 0.4f,  .tn_lcon_w = 0.5f },
    { .name = "Aery Tonescale",      .tn_con = 1.15f, .tn_sh = 0.5f,  .tn_toe = 0.04f,  .tn_off = 0.006f, .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 0.0f, .tn_hcon_st = 0.5f, .tn_lcon_enable = 1, .tn_lcon = 0.5f,  .tn_lcon_w = 2.0f },
    { .name = "Dystopic Tonescale",  .tn_con = 1.6f,  .tn_sh = 0.5f,  .tn_toe = 0.01f,  .tn_off = 0.008f, .tn_hcon_enable = 1, .tn_hcon = 0.25f, .tn_hcon_pv = 0.0f, .tn_hcon_st = 1.0f, .tn_lcon_enable = 1, .tn_lcon = 1.0f,  .tn_lcon_w = 0.75f },
    { .name = "Umbra Tonescale",     .tn_con = 1.8f,  .tn_sh = 0.5f,  .tn_toe = 0.001f, .tn_off = 0.015f, .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 1.0f,  .tn_lcon_w = 1.0f },
    { .name = "ACES-1.x",            .tn_con = 1.0f,  .tn_sh = 0.35f, .tn_toe = 0.02f,  .tn_off = 0.0f,   .tn_hcon_enable = 1, .tn_hcon = 0.55f, .tn_hcon_pv = 0.0f, .tn_hcon_st = 2.0f, .tn_lcon_enable = 1, .tn_lcon = 1.13f, .tn_lcon_w = 1.0f },
    { .name = "ACES-2.0",            .tn_con = 1.15f, .tn_sh = 0.5f,  .tn_toe = 0.04f,  .tn_off = 0.0f,   .tn_hcon_enable = 0, .tn_hcon = 1.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 1.0f, .tn_lcon_enable = 0, .tn_lcon = 1.0f,  .tn_lcon_w = 0.6f },
    { .name = "Marvelous Tonescape", .tn_con = 1.5f,  .tn_sh = 0.5f,  .tn_toe = 0.003f, .tn_off = 0.01f,  .tn_hcon_enable = 1, .tn_hcon = 0.25f, .tn_hcon_pv = 0.0f, .tn_hcon_st = 4.0f, .tn_lcon_enable = 1, .tn_lcon = 1.0f,  .tn_lcon_w = 1.0f },
    { .name = "DaGrinchi ToneGroan", .tn_con = 1.2f,  .tn_sh = 0.5f,  .tn_toe = 0.02f,  .tn_off = 0.0f,   .tn_hcon_enable = 0, .tn_hcon = 0.0f,  .tn_hcon_pv = 1.0f, .tn_hcon_st = 1.0f, .tn_lcon_enable = 0, .tn_lcon = 0.0f,  .tn_lcon_w = 0.6f },
};

/* DCTL display_encoding_preset order. Surround/gamut/EOTF from the DCTL; default_Lp from the Nuke node. */
const DrtDisplay kDisplays[kDisplayCount] = {
    { "Rec.1886 - 2.4 Power / Rec.709",            DRT_EOTF_POWER_2_4, DRT_DG_REC709,        DRT_SURROUND_DIM,    100.0f },
    { "sRGB Display - 2.2 Power / Rec.709",        DRT_EOTF_POWER_2_2, DRT_DG_REC709,        DRT_SURROUND_BRIGHT, 100.0f },
    { "Display P3 - 2.2 Power / P3-D65",           DRT_EOTF_POWER_2_2, DRT_DG_P3D65,         DRT_SURROUND_BRIGHT, 100.0f },
    { "DCI - 2.6 Power / P3-D60",                  DRT_EOTF_POWER_2_6, DRT_DG_P3D60,         DRT_SURROUND_DARK,   100.0f },
    { "DCI - 2.6 Power / P3-DCI",                  DRT_EOTF_POWER_2_6, DRT_DG_P3DCI,         DRT_SURROUND_DARK,   100.0f },
    { "DCI - 2.6 Power / XYZ",                     DRT_EOTF_POWER_2_6, DRT_DG_XYZ,           DRT_SURROUND_DARK,   100.0f },
    { "Rec.2100 - PQ / Rec.2020 (P3 Limited)",     DRT_EOTF_PQ,        DRT_DG_REC2020_P3LIM, DRT_SURROUND_DARK,   1000.0f },
    { "Rec.2100 - HLG / Rec.2020 (P3 Limited)",    DRT_EOTF_HLG,       DRT_DG_REC2020_P3LIM, DRT_SURROUND_DARK,   1000.0f },
    { "Dolby - PQ / P3-D65",                       DRT_EOTF_PQ,        DRT_DG_P3D65,         DRT_SURROUND_DARK,   1000.0f },
    /* not in upstream: no encoding at all. Linear in the working gamut, so the Output is an
       identity in the Un-tone-mapped view and a linear rendered hand-off in the OpenDRT view. */
    { "None - Linear / Working Gamut",             DRT_EOTF_LINEAR,    DRT_DG_WORKING,       DRT_SURROUND_DARK,   100.0f },
    { "None - Linear / ACES 2065-1",               DRT_EOTF_LINEAR,    DRT_DG_AP0,           DRT_SURROUND_DARK,   100.0f },
    { "None - Linear / ACEScg",                    DRT_EOTF_LINEAR,    DRT_DG_AP1,           DRT_SURROUND_DARK,   100.0f },
    { "None - Linear / Rec.2020",                  DRT_EOTF_LINEAR,    DRT_DG_REC2020,       DRT_SURROUND_DARK,   100.0f },
    { "None - Linear / Rec.709",                   DRT_EOTF_LINEAR,    DRT_DG_REC709,        DRT_SURROUND_DARK,   100.0f },
};

int drt_display_preset_index(int display_gamut, int eotf)
{
    for (int k = 0; k < kDisplayCount; ++k)
        if (kDisplays[k].display_gamut == display_gamut && kDisplays[k].eotf == eotf) return k;
    return -1;
}

const char *const kCwpNames[kCwpCount] = { "D93", "D75", "D65", "D60", "D55", "D50" };

const char *const kInGamutNames[DRT_IN_GAMUT_COUNT] = {
    "XYZ", "ACES 2065-1", "ACEScg", "P3-D65", "Rec.2020", "Rec.709",
    "ARRI Wide Gamut 3", "ARRI Wide Gamut 4", "RED Wide Gamut RGB",
    "Sony S-Gamut3", "Sony S-Gamut3.Cine", "Panasonic V-Gamut",
    "Filmlight E-Gamut", "Filmlight E-Gamut2", "DaVinci Wide Gamut",
};

const char *const kInOetfNames[DRT_OETF_COUNT] = {
    "Linear", "DaVinci Intermediate", "Filmlight T-Log", "ACEScct", "ARRI LogC3",
    "ARRI LogC4", "RED Log3G10", "Panasonic V-Log", "Sony S-Log3", "Fuji F-Log2",
    "Rec.1886 (2.4 power)", "sRGB", "2.2 power", "BT.709 camera", "PQ (100 nits = 1.0)", "HLG 1000 nits",
    "OpenDRT inverse: Rec.1886", "OpenDRT inverse: sRGB Display", "OpenDRT inverse: Display P3",
    "OpenDRT inverse: Rec.2100 PQ", "OpenDRT inverse: Rec.2100 HLG", "OpenDRT inverse: Dolby PQ / P3-D65",
};

/* Which display preset each inverse entry undoes (kDisplays index). */
const int kInverseDisplayMap[DRT_OETF_COUNT - DRT_OETF_INVERSE_FIRST] = { 0, 1, 2, 6, 7, 8 };

bool drt_inverse_display(DrtParams &p)
{
    if (p.in_oetf < DRT_OETF_INVERSE_FIRST || p.in_oetf >= DRT_OETF_COUNT) return false;
    /* the encoding is what the file is; the surround is how it was viewed, and that
       stays on the Surround row (the host writes the preset's value there when the
       entry is picked, as the Output's Display preset does) */
    const DrtDisplay &d = kDisplays[kInverseDisplayMap[p.in_oetf - DRT_OETF_INVERSE_FIRST]];
    p.eotf          = d.eotf;
    p.display_gamut = d.display_gamut;
    return true;
}

DrtParams drt_stickshift_defaults()
{
    DrtParams p = {};
    p.in_gamut = DRT_IN_DAVINCI_WG;
    p.in_oetf  = DRT_OETF_DAVINCI_INTERMEDIATE;
    p.working_gamut = DRT_IN_AP1;
    p.tn_Lp    = 100.0f;
    p.tn_gb    = 0.13f;
    p.pt_hdr   = 0.5f;
    p.tn_Lg    = 10.0f;
    drt_apply_look(p, kLooks[0]);        /* the DEFINE_UI_PARAMS defaults are the Standard look */
    p.clamp_out     = 1;
    /* Output, Render block: ACES 2065-1 linear, Un-tone-mapped (a scene-referred delivery); Mode = View */
    p.out_mode  = 0;
    p.rnd_gamut = DRT_DG_AP0;
    p.rnd_eotf  = DRT_EOTF_LINEAR;
    p.rnd_su    = DRT_SURROUND_DARK;
    p.rnd_view  = 1;
    p.rnd_Lp    = 100.0f;
    p.inv_cap       = 1.0f;
    /* minColor: the view defaults to sRGB Display (a desktop monitor is what AE is
       watched on) at a Dark surround, i.e. the look's contrast with nothing taken
       off. Upstream's preset would pair sRGB Display with Bright; here the
       surround is the room, set by hand, and no preset writes it. */
    p.tn_su         = DRT_SURROUND_DARK;
    /* minColor: the Output's Rendering defaults to Un-tone-mapped (a linear
       conversion, no look); the OpenDRT rendering is one popup away */
    p.out_view      = 1;
    p.display_gamut = DRT_DG_REC709;
    p.eotf          = DRT_EOTF_POWER_2_2;
    return p;
}

void drt_apply_look(DrtParams &p, const DrtLook &l)
{
#define DRT_X_F(n) p.n = l.n;
#define DRT_X_I(n) p.n = l.n;
    DRT_LOOK_FIELDS(DRT_X_F, DRT_X_I)
#undef DRT_X_F
#undef DRT_X_I
}

void drt_apply_look_tonescale(DrtParams &p, const DrtLook &l)
{
#define DRT_X_F(n) p.n = l.n;
#define DRT_X_I(n) p.n = l.n;
    DRT_TONESCALE_FIELDS(DRT_X_F, DRT_X_I)
#undef DRT_X_F
#undef DRT_X_I
}

void drt_output_mode(DrtParams &p)
{
    if (p.out_mode != 1) return;
    p.display_gamut = p.rnd_gamut;
    p.eotf          = p.rnd_eotf;
    p.tn_su         = p.rnd_su;
    p.tn_Lp         = p.rnd_Lp;
    p.out_view      = p.rnd_view;
    p.clamp_out     = p.rnd_eotf == DRT_EOTF_LINEAR ? 0 : 1;
}

void drt_apply_tonescale(DrtParams &p, const DrtTonescale &t)
{
#define DRT_X_F(n) p.n = t.n;
#define DRT_X_I(n) p.n = t.n;
    DRT_TONESCALE_FIELDS(DRT_X_F, DRT_X_I)
#undef DRT_X_F
#undef DRT_X_I
}

void drt_apply_display(DrtParams &p, const DrtDisplay &d)
{
    p.eotf          = d.eotf;
    p.display_gamut = d.display_gamut;
    p.tn_su         = d.tn_su;
}

DrtGradeParams drt_grade_defaults()
{
    DrtGradeParams g = {};
    g.contrast = 1.0f;
    g.pivot_nits = 10.0f;
    g.saturation = 1.0f;
    g.z0_saturation = g.z1_saturation = g.z2_saturation = g.z3_saturation = g.z4_saturation = g.z5_saturation = 1.0f;
    g.zb0 = -3.0f; g.zb1 = -1.5f; g.zb2 = 0.0f; g.zb3 = 1.5f; g.zb4 = 3.0f;   /* stops over grey */
    g.zfalloff = 2.0f;   /* feather past each boundary */
    g.sec_hue = 0.0f; g.sec_hue_width = 60.0f;
    g.sec_sat_min = 0.1f; g.sec_sat_max = 2.0f;
    g.sec_lum_min = 0.0f; g.sec_lum_max = 10000.0f;   /* retired rows */
    g.pivot_stops = 0.0f;                               /* mid grey */
    g.sec_lev_min = -10.0f; g.sec_lev_max = 10.0f;      /* wide open */
    g.sec_softness = 0.5f;
    g.sec_saturation = 1.0f;
    return g;
}

DrtParams drt_resolve(const DrtSettings &s)
{
    DrtParams p = {};
    p.in_gamut = s.in_gamut;
    p.in_oetf  = s.in_oetf;
    p.tn_Lp    = s.tn_Lp;
    p.tn_gb    = s.tn_gb;
    p.pt_hdr   = s.pt_hdr;
    p.tn_Lg    = s.tn_Lg;

    /* LOOK PRESETS */
    const int look = (s.look >= 0 && s.look < kLookCount) ? s.look : 0;
    drt_apply_look(p, kLooks[look]);

    /* TONESCALE PRESETS: 0 = keep the look's */
    if (s.tonescale >= 1 && s.tonescale <= kTonescaleCount)
        drt_apply_tonescale(p, kTonescales[s.tonescale - 1]);

    /* Hard-code clamp if in presets mode */
    p.clamp_out = 1;

    /* CREATIVE WHITE PRESETS: 0 = keep the look's; the limit slider only applies with an override */
    if (s.cwp >= 1 && s.cwp <= kCwpCount) {
        p.cwp_lm = s.cwp_lm;
        p.cwp    = s.cwp - 1;
    }

    /* DISPLAY ENCODING PRESETS */
    const int display = (s.display >= 0 && s.display < kDisplayCount) ? s.display : 0;
    if (kDisplays[display].eotf == DRT_EOTF_LINEAR) p.clamp_out = 0;   /* None: linear carries values above 1 */
    drt_apply_display(p, kDisplays[display]);

    return drt_derive(p);
}

} // namespace drt
