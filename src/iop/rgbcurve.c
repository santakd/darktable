/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/iop_profile.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/rgb_norms.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/presets.h"
#include "gui/accelerators.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)
#define DT_IOP_RGBCURVE_RES 256
#define DT_IOP_RGBCURVE_MAXNODES 20
#define DT_IOP_RGBCURVE_MIN_X_DISTANCE 0.0025f
// max iccprofile file name length
// must be in synch with filename in dt_colorspaces_color_profile_t in colorspaces.h
#define DT_IOP_COLOR_ICC_LEN 512

DT_MODULE_INTROSPECTION(1, dt_iop_rgbcurve_params_t)

typedef enum rgbcurve_channel_t
{
  DT_IOP_RGBCURVE_R = 0,
  DT_IOP_RGBCURVE_G = 1,
  DT_IOP_RGBCURVE_B = 2,
  DT_IOP_RGBCURVE_MAX_CHANNELS = 3
} rgbcurve_channel_t;

typedef enum dt_iop_rgbcurve_autoscale_t
{
  DT_S_SCALE_AUTOMATIC_RGB = 0,
  DT_S_SCALE_MANUAL_RGB = 1
} dt_iop_rgbcurve_autoscale_t;

typedef struct dt_iop_rgbcurve_node_t
{
  float x;
  float y;
} dt_iop_rgbcurve_node_t;

typedef struct dt_iop_rgbcurve_params_t
{
  dt_iop_rgbcurve_node_t curve_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS]
                                    [DT_IOP_RGBCURVE_MAXNODES]; // actual nodes for each curve
  int curve_num_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS];            // number of nodes per curve
  int curve_type[DT_IOP_RGBCURVE_MAX_CHANNELS]; // curve type (CATMULL_ROM, MONOTONE_HERMITE, CUBIC_SPLINE)
  int curve_autoscale;        // (DT_S_SCALE_MANUAL_RGB, DT_S_SCALE_AUTOMATIC_RGB)
  int compensate_middle_grey; // scale the curve and histogram so middle gray is at .5
  int preserve_colors;
} dt_iop_rgbcurve_params_t;

typedef struct dt_iop_rgbcurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[DT_IOP_RGBCURVE_MAX_CHANNELS]; // curves for gui to draw
  int minmax_curve_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS];
  int minmax_curve_type[DT_IOP_RGBCURVE_MAX_CHANNELS];
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkSizeGroup *sizegroup;
  GtkWidget *autoscale; // (DT_S_SCALE_MANUAL_RGB, DT_S_SCALE_AUTOMATIC_RGB)
  GtkNotebook *channel_tabs;
  GtkWidget *colorpicker;
  GtkWidget *colorpicker_set_values;
  GtkWidget *interpolator;
  rgbcurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  int timeout_handle;
  float draw_ys[DT_IOP_RGBCURVE_RES];
  float draw_min_ys[DT_IOP_RGBCURVE_RES];
  float draw_max_ys[DT_IOP_RGBCURVE_RES];
  GtkWidget *chk_compensate_middle_grey;
  GtkWidget *cmb_preserve_colors;
  float zoom_factor;
  float offset_x, offset_y;
} dt_iop_rgbcurve_gui_data_t;

typedef struct dt_iop_rgbcurve_data_t
{
  dt_iop_rgbcurve_params_t params;
  dt_draw_curve_t *curve[DT_IOP_RGBCURVE_MAX_CHANNELS];    // curves for pipe piece and pixel processing
  float table[DT_IOP_RGBCURVE_MAX_CHANNELS][0x10000];      // precomputed look-up tables for tone curve
  float unbounded_coeffs[DT_IOP_RGBCURVE_MAX_CHANNELS][3]; // approximation for extrapolation
  int curve_changed[DT_IOP_RGBCURVE_MAX_CHANNELS];         // curve type or number of nodes changed?
  dt_colorspaces_color_profile_type_t type_work; // working color profile
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_rgbcurve_data_t;

typedef struct dt_iop_rgbcurve_global_data_t
{
  int kernel_rgbcurve;
} dt_iop_rgbcurve_global_data_t;

const char *name()
{
  return _("rgb curve");
}

int default_group()
{
  return IOP_GROUP_TONE;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "interpolation method"));
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "preserve colors"));
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "mode"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  dt_accel_connect_combobox_iop(self, "interpolation method", GTK_WIDGET(g->interpolator));
  dt_accel_connect_combobox_iop(self, "preserve colors", GTK_WIDGET(g->cmb_preserve_colors));
  dt_accel_connect_combobox_iop(self, "mode", GTK_WIDGET(g->autoscale));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_rgbcurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.curve_num_nodes[DT_IOP_RGBCURVE_R] = 6;
  p.curve_num_nodes[DT_IOP_RGBCURVE_G] = 7;
  p.curve_num_nodes[DT_IOP_RGBCURVE_B] = 7;
  p.curve_type[DT_IOP_RGBCURVE_R] = CUBIC_SPLINE;
  p.curve_type[DT_IOP_RGBCURVE_G] = CUBIC_SPLINE;
  p.curve_type[DT_IOP_RGBCURVE_B] = CUBIC_SPLINE;
  p.curve_autoscale = DT_S_SCALE_AUTOMATIC_RGB;
  p.compensate_middle_grey = 1;
  p.preserve_colors = 1;

  float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves for presets
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_G][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_G][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_B][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_B][k].y = linear_ab[k];

  // More useful low contrast curve (based on Samsung NX -2 Contrast)
  p.curve_nodes[DT_IOP_RGBCURVE_R][0].x = 0.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].x = 0.003862;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].x = 0.076613;
  p.curve_nodes[DT_IOP_RGBCURVE_R][3].x = 0.169355;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].x = 0.774194;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].x = 1.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][0].y = 0.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y = 0.007782;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y = 0.156182;
  p.curve_nodes[DT_IOP_RGBCURVE_R][3].y = 0.290352;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y = 0.773852;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y = 1.000000;
  dt_gui_presets_add_generic(_("contrast compression"), self->op, self->version(), &p, sizeof(p), 1);

  p.curve_num_nodes[DT_IOP_RGBCURVE_R] = 7;
  float linear_L[7] = { 0.0, 0.08, 0.17, 0.50, 0.83, 0.92, 1.0 };

  // Linear - no contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("gamma 1.0 (linear)"), self->op, self->version(), &p, sizeof(p), 1);

  // Linear contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.020;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.020;
  dt_gui_presets_add_generic(_("contrast - med (linear)"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.040;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.040;
  dt_gui_presets_add_generic(_("contrast - high (linear)"), self->op, self->version(), &p, sizeof(p), 1);

  // Gamma contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.020;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.020;
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].x, 2.2f);
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - med (gamma 2.2)"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.040;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.040;
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].x, 2.2f);
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - high (gamma 2.2)"), self->op, self->version(), &p, sizeof(p), 1);

  /** for pure power-like functions, we need more nodes close to the bounds**/

  p.curve_type[DT_IOP_RGBCURVE_R] = MONOTONE_HERMITE;

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];

  // Gamma 2.0 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(linear_L[k], 2.0f);
  dt_gui_presets_add_generic(_("gamma 2.0"), self->op, self->version(), &p, sizeof(p), 1);

  // Gamma 0.5 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(linear_L[k], 0.5f);
  dt_gui_presets_add_generic(_("gamma 0.5"), self->op, self->version(), &p, sizeof(p), 1);

  // Log2 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = logf(linear_L[k] + 1.0f) / logf(2.0f);
  dt_gui_presets_add_generic(_("logarithm (base 2)"), self->op, self->version(), &p, sizeof(p), 1);

  // Exp2 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(2.0f, linear_L[k]) - 1.0f;
  dt_gui_presets_add_generic(_("exponential (base 2)"), self->op, self->version(), &p, sizeof(p), 1);
}

static float _curve_to_mouse(const float x, const float zoom_factor, const float offset)
{
  return (x - offset) * zoom_factor;
}

static float _mouse_to_curve(const float x, const float zoom_factor, const float offset)
{
  return (x / zoom_factor) + offset;
}

static void picker_scale(const float *const in, float *out, dt_iop_rgbcurve_params_t *p,
                         const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(p->curve_autoscale)
  {
    case DT_S_SCALE_MANUAL_RGB:
      if(p->compensate_middle_grey && work_profile)
      {
        for(int c = 0; c < 3; c++) out[c] = dt_ioppr_compensate_middle_grey(in[c], work_profile);
      }
      else
      {
        for(int c = 0; c < 3; c++) out[c] = in[c];
      }
      break;
    case DT_S_SCALE_AUTOMATIC_RGB:
    {
      const float val
          = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(in,
                                                               work_profile->matrix_in,
                                                               work_profile->lut_in,
                                                               work_profile->unbounded_coeffs_in,
                                                               work_profile->lutsize,
                                                               work_profile->nonlinearlut)
                           : dt_camera_rgb_luminance(in);
      if(p->compensate_middle_grey && work_profile)
      {
        out[0] = dt_ioppr_compensate_middle_grey(val, work_profile);
      }
      else
      {
        out[0] = val;
      }
      out[1] = out[2] = 0.f;
    }
    break;
  }

  for(int c = 0; c < 3; c++) out[c] = CLAMP(out[c], 0.0f, 1.0f);
}

static void _rgbcurve_show_hide_controls(dt_iop_rgbcurve_params_t *p, dt_iop_rgbcurve_gui_data_t *g)
{
  switch(p->curve_autoscale)
  {
    case DT_S_SCALE_MANUAL_RGB:
      gtk_notebook_set_show_tabs(g->channel_tabs, TRUE);
      break;
    case DT_S_SCALE_AUTOMATIC_RGB:
      gtk_notebook_set_show_tabs(g->channel_tabs, FALSE);
      break;
  }

  if(p->curve_autoscale == DT_S_SCALE_AUTOMATIC_RGB)
    gtk_widget_set_visible(g->cmb_preserve_colors, TRUE);
  else
    gtk_widget_set_visible(g->cmb_preserve_colors, FALSE);
}

static gboolean _is_identity(dt_iop_rgbcurve_params_t *p, rgbcurve_channel_t channel)
{
  for(int k=0; k<p->curve_num_nodes[channel]; k++)
    if(p->curve_nodes[channel][k].x != p->curve_nodes[channel][k].y) return FALSE;

  return TRUE;
}

static void autoscale_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  const int combo = dt_bauhaus_combobox_get(widget);

  g->channel = DT_IOP_RGBCURVE_R;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), DT_IOP_RGBCURVE_R);
  p->curve_autoscale = combo;

  _rgbcurve_show_hide_controls(p, g);

  // swithing to manual scale, if G and B not touched yet, just make them identical to global setting (R)
  if(combo == DT_S_SCALE_MANUAL_RGB
     && _is_identity(p, DT_IOP_RGBCURVE_G)
     && _is_identity(p, DT_IOP_RGBCURVE_B))
  {
    for(int k=0; k<DT_IOP_RGBCURVE_MAXNODES; k++)
      p->curve_nodes[DT_IOP_RGBCURVE_G][k]
        = p->curve_nodes[DT_IOP_RGBCURVE_B][k] = p->curve_nodes[DT_IOP_RGBCURVE_R][k];

    p->curve_num_nodes[DT_IOP_RGBCURVE_G] = p->curve_num_nodes[DT_IOP_RGBCURVE_B]
      = p->curve_num_nodes[DT_IOP_RGBCURVE_R];
    p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
      = p->curve_type[DT_IOP_RGBCURVE_R];
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  const int combo = dt_bauhaus_combobox_get(widget);

  if(combo == 0)
    p->curve_type[DT_IOP_RGBCURVE_R] = p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = CUBIC_SPLINE;
  else if(combo == 1)
    p->curve_type[DT_IOP_RGBCURVE_R] = p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = CATMULL_ROM;
  else if(combo == 2)
    p->curve_type[DT_IOP_RGBCURVE_R] = p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = MONOTONE_HERMITE;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void compensate_middle_grey_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  if(work_profile == NULL) return;

  const int compensate = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  if(compensate && !p->compensate_middle_grey)
  {
    // we transform the curve nodes from the image colorspace to lab
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      {
        p->curve_nodes[ch][k].x = dt_ioppr_compensate_middle_grey(p->curve_nodes[ch][k].x, work_profile);
        p->curve_nodes[ch][k].y = dt_ioppr_compensate_middle_grey(p->curve_nodes[ch][k].y, work_profile);
      }
    }
  }
  else if(!compensate && p->compensate_middle_grey)
  {
    // we transform the curve nodes from lab to the image colorspace
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      {
        p->curve_nodes[ch][k].x = dt_ioppr_uncompensate_middle_grey(p->curve_nodes[ch][k].x, work_profile);
        p->curve_nodes[ch][k].y = dt_ioppr_uncompensate_middle_grey(p->curve_nodes[ch][k].y, work_profile);
      }
    }
  }

  p->compensate_middle_grey = compensate;
  self->histogram_middle_grey = compensate;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void preserve_colors_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  p->preserve_colors = dt_bauhaus_combobox_get(widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tab_switch_callback(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  g->channel = (rgbcurve_channel_t)page_num;

  gtk_widget_queue_draw(self->widget);
}

static gboolean _area_resized_callback(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkRequisition r;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

static inline int _add_node(dt_iop_rgbcurve_node_t *curve_nodes, int *nodes, float x, float y)
{
  int selected = -1;
  if(curve_nodes[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(curve_nodes[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    curve_nodes[i].x = curve_nodes[i - 1].x;
    curve_nodes[i].y = curve_nodes[i - 1].y;
  }
  // found a new point
  curve_nodes[selected].x = x;
  curve_nodes[selected].y = y;
  (*nodes)++;
  return selected;
}

static inline int _add_node_from_picker(dt_iop_rgbcurve_params_t *p, const float *const in, const float increment,
                                        const int ch, const dt_iop_order_iccprofile_info_t *const work_profile)
{
  float x = 0.f;
  float y = 0.f;
  float val = 0.f;

  if(p->curve_autoscale == DT_S_SCALE_AUTOMATIC_RGB)
    val = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(in,
                                                             work_profile->matrix_in,
                                                             work_profile->lut_in,
                                                             work_profile->unbounded_coeffs_in,
                                                             work_profile->lutsize,
                                                             work_profile->nonlinearlut)
                         : dt_camera_rgb_luminance(in);
  else
    val = in[ch];

  if(p->compensate_middle_grey && work_profile)
    y = x = dt_ioppr_compensate_middle_grey(val, work_profile);
  else
    y = x = val;

  x -= increment;
  y += increment;

  CLAMP(x, 0.f, 1.f);
  CLAMP(y, 0.f, 1.f);

  return _add_node(p->curve_nodes[ch], &p->curve_num_nodes[ch], x, y);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  if(picker == g->colorpicker_set_values)
  {
    dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
    dt_iop_rgbcurve_params_t *d = (dt_iop_rgbcurve_params_t *)self->default_params;

    const int ch = g->channel;
    const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

    // reset current curve
    p->curve_num_nodes[ch] = d->curve_num_nodes[ch];
    p->curve_type[ch] = d->curve_type[ch];
    for(int k = 0; k < DT_IOP_RGBCURVE_MAXNODES; k++)
    {
      p->curve_nodes[ch][k].x = d->curve_nodes[ch][k].x;
      p->curve_nodes[ch][k].y = d->curve_nodes[ch][k].y;
    }

    guint state = gdk_keymap_get_modifier_state(gdk_keymap_get_for_display(gdk_display_get_default()));
    GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
    int picker_set_upper_lower;
    if((state & modifiers) == GDK_CONTROL_MASK) // flat=0, lower=-1, upper=1
      picker_set_upper_lower = 1;
    else if((state & modifiers) == GDK_SHIFT_MASK)
      picker_set_upper_lower = -1;
    else
      picker_set_upper_lower = 0;

    // now add 4 nodes: min, avg, center, max
    const float increment = 0.05f * picker_set_upper_lower;

    _add_node_from_picker(p, self->picked_color_min, 0.f, ch, work_profile);
    _add_node_from_picker(p, self->picked_color, increment, ch, work_profile);
    _add_node_from_picker(p, self->picked_color_max, 0.f, ch, work_profile);

    if(p->curve_num_nodes[ch] == 5)
      _add_node(p->curve_nodes[ch], &p->curve_num_nodes[ch],
                p->curve_nodes[ch][1].x - increment + (p->curve_nodes[ch][3].x - p->curve_nodes[ch][1].x) / 2.f,
                p->curve_nodes[ch][1].y + increment + (p->curve_nodes[ch][3].y - p->curve_nodes[ch][1].y) / 2.f);

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  dt_control_queue_redraw_widget(self->widget);
}

static gboolean _sanity_check(const float x, const int selected, const int nodes,
                              const dt_iop_rgbcurve_node_t *curve)
{
  gboolean point_valid = TRUE;

  // check if it is not too close to other node
  const float min_dist = DT_IOP_RGBCURVE_MIN_X_DISTANCE; // in curve coordinates
  if((selected > 0 && x - curve[selected - 1].x <= min_dist)
     || (selected < nodes - 1 && curve[selected + 1].x - x <= min_dist))
    point_valid = FALSE;

  // for all points, x coordinate of point must be strictly larger than
  // the x coordinate of the previous point
  if((selected > 0 && (curve[selected - 1].x >= x)) || (selected < nodes - 1 && (curve[selected + 1].x <= x)))
  {
    point_valid = FALSE;
  }

  return point_valid;
}

static gboolean postponed_value_change(gpointer data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)data;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  g->timeout_handle = 0;

  return FALSE;
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  const int ch = g->channel;
  dt_iop_rgbcurve_node_t *curve = p->curve_nodes[ch];

  float multiplier;

  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  if((state & modifiers) == GDK_SHIFT_MASK)
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  }
  else if((state & modifiers) == GDK_CONTROL_MASK)
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  }
  else
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");
  }

  dx *= multiplier;
  dy *= multiplier;

  const float new_x = CLAMP(curve[g->selected].x + dx, 0.0f, 1.0f);
  const float new_y = CLAMP(curve[g->selected].y + dy, 0.0f, 1.0f);

  gtk_widget_queue_draw(widget);

  if(_sanity_check(new_x, g->selected, p->curve_num_nodes[ch], p->curve_nodes[ch]))
  {
    curve[g->selected].x = new_x;
    curve[g->selected].y = new_y;

    const int delay = CLAMP(darktable.develop->average_delay * 3 / 2, 10, 1000);

    if(!g->timeout_handle)
      g->timeout_handle = g_timeout_add(delay, postponed_value_change, self);
  }

  return TRUE;
}

#define RGBCURVE_DEFAULT_STEP (0.001f)

static gboolean _area_scrolled_callback(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  gdouble delta_y;

  if(((event->state & gtk_accelerator_get_default_mod_mask()) == darktable.gui->sidebar_scroll_mask) != dt_conf_get_bool("darkroom/ui/sidebar_scroll_default")) return FALSE;
  if(darktable.develop->darkroom_skip_mouse_events)
  {
    if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
    {
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);

      const float mx = g->mouse_x;
      const float my = g->mouse_y;
      const float linx = _mouse_to_curve(mx, g->zoom_factor, g->offset_x),
                  liny = _mouse_to_curve(my, g->zoom_factor, g->offset_y);

      g->zoom_factor *= 1.0 - 0.1 * delta_y;
      if(g->zoom_factor < 1.f) g->zoom_factor = 1.f;

      g->offset_x = linx - (mx / g->zoom_factor);
      g->offset_y = liny - (my / g->zoom_factor);

      g->offset_x = CLAMP(g->offset_x, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);
      g->offset_y = CLAMP(g->offset_y, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);

      gtk_widget_queue_draw(self->widget);
    }

    return TRUE;
  }

  // if autoscale is on: do not modify g and b curves
  if((p->curve_autoscale != DT_S_SCALE_MANUAL_RGB) && g->channel != DT_IOP_RGBCURVE_R) return TRUE;

  if(g->selected < 0) return TRUE;

  dt_iop_color_picker_reset(self, TRUE);

  if(dt_gui_get_scroll_delta(event, &delta_y))
  {
    delta_y *= -RGBCURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean _area_key_press_callback(GtkWidget *widget, GdkEventKey *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  if(darktable.develop->darkroom_skip_mouse_events) return TRUE;

  // if autoscale is on: do not modify g and b curves
  if((p->curve_autoscale != DT_S_SCALE_MANUAL_RGB) && g->channel != DT_IOP_RGBCURVE_R) return TRUE;

  if(g->selected < 0) return TRUE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = RGBCURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -RGBCURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = RGBCURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -RGBCURVE_DEFAULT_STEP;
  }

  if(!handled) return TRUE;

  dt_iop_color_picker_reset(self, TRUE);
  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef RGBCURVE_DEFAULT_STEP

static gboolean _area_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _area_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _area_draw_callback(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_develop_t *dev = darktable.develop;

  const int ch = g->channel;
  const int nodes = p->curve_num_nodes[ch];
  const int autoscale = p->curve_autoscale;
  dt_iop_rgbcurve_node_t *curve_nodes = p->curve_nodes[ch];

  if(g->minmax_curve_type[ch] != p->curve_type[ch] || g->minmax_curve_nodes[ch] != p->curve_num_nodes[ch])
  {
    dt_draw_curve_destroy(g->minmax_curve[ch]);
    g->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->curve_type[ch]);
    g->minmax_curve_nodes[ch] = p->curve_num_nodes[ch];
    g->minmax_curve_type[ch] = p->curve_type[ch];
    for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(g->minmax_curve[ch], p->curve_nodes[ch][k].x, p->curve_nodes[ch][k].y);
  }
  else
  {
    for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      dt_draw_curve_set_point(g->minmax_curve[ch], k, p->curve_nodes[ch][k].x, p->curve_nodes[ch][k].y);
  }
  dt_draw_curve_t *minmax_curve = g->minmax_curve[ch];
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_RGBCURVE_RES, NULL, g->draw_ys);

  float unbounded_coeffs[3];
  const float xm = curve_nodes[nodes - 1].x;
  {
    const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
    const float y[4] = { g->draw_ys[CLAMP((int)(x[0] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)],
                         g->draw_ys[CLAMP((int)(x[1] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)],
                         g->draw_ys[CLAMP((int)(x[2] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)],
                         g->draw_ys[CLAMP((int)(x[3] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)] };
    dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);
  }

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;
  char text[256];

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);

  cairo_translate(cr, 0, height);

  dt_draw_grid_zoomed(cr, 4, 0.f, 0.f, 1.f, 1.f, width, height, g->zoom_factor, g->offset_x, g->offset_y);

  const double dashed[] = { 4.0, 4.0 };
  const int len = sizeof(dashed) / sizeof(dashed[0]);
  cairo_set_dash(cr, dashed, len, 0);
  dt_draw_grid_zoomed(cr, 8, 0.f, 0.f, 1.f, 1.f, width, height, g->zoom_factor, g->offset_x, g->offset_y);
  cairo_set_dash(cr, dashed, 0, 0);

  // draw identity line
  cairo_move_to(cr, _curve_to_mouse(0.f, g->zoom_factor, g->offset_x) * width,
                _curve_to_mouse(0.f, g->zoom_factor, g->offset_y) * -height);
  cairo_line_to(cr, _curve_to_mouse(1.f, g->zoom_factor, g->offset_x) * width,
                _curve_to_mouse(1.f, g->zoom_factor, g->offset_y) * -height);
  cairo_stroke(cr);

  // if autoscale is on: do not display g and b curves
  if((autoscale != DT_S_SCALE_MANUAL_RGB) && ch != DT_IOP_RGBCURVE_R) goto finally;

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);

  for(int k = 0; k < nodes; k++)
  {
    const float x = _curve_to_mouse(curve_nodes[k].x, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  if(g->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = _curve_to_mouse(curve_nodes[g->selected].x, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(curve_nodes[g->selected].y, g->zoom_factor, g->offset_y);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw histogram in background
  // only if module is enabled
  if(self->enabled)
  {
    const uint32_t *hist = self->histogram;
    float hist_max;

    if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
      hist_max = fmaxf(self->histogram_max[DT_IOP_RGBCURVE_R], fmaxf(self->histogram_max[DT_IOP_RGBCURVE_G],self->histogram_max[DT_IOP_RGBCURVE_B]));
    else
      hist_max = self->histogram_max[ch];

    if (dev->histogram_type != DT_DEV_HISTOGRAM_LINEAR)
      hist_max = logf(1.0 + hist_max);

    if(hist && hist_max > 0.0f)
    {
      cairo_save(cr);
      cairo_scale(cr, width / 255.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);

      if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
      {
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);

        cairo_set_source_rgba(cr, 1., 0., 0., 0.2);
        dt_draw_histogram_8_zoomed(cr, hist, 4, DT_IOP_RGBCURVE_R, g->zoom_factor, g->offset_x * 255.0, g->offset_y * hist_max,
                                   dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR);

        cairo_set_source_rgba(cr, 0., 1., 0., 0.2);
        dt_draw_histogram_8_zoomed(cr, hist, 4, DT_IOP_RGBCURVE_G, g->zoom_factor, g->offset_x * 255.0, g->offset_y * hist_max,
                                   dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR);

        cairo_set_source_rgba(cr, 0., 0., 1., 0.2);
        dt_draw_histogram_8_zoomed(cr, hist, 4, DT_IOP_RGBCURVE_B, g->zoom_factor, g->offset_x * 255.0, g->offset_y * hist_max,
                                   dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR);
        }
        else if(autoscale == DT_S_SCALE_MANUAL_RGB)
        {
          if(ch == DT_IOP_RGBCURVE_R)
            cairo_set_source_rgba(cr, 1., 0., 0., 0.2);
          else if(ch == DT_IOP_RGBCURVE_G)
            cairo_set_source_rgba(cr, 0., 1., 0., 0.2);
          else
            cairo_set_source_rgba(cr, 0., 0., 1., 0.2);
          dt_draw_histogram_8_zoomed(cr, hist, 4, ch, g->zoom_factor, g->offset_x * 255.0, g->offset_y * hist_max,
                                     dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR);
        }

      cairo_restore(cr);
    }

    if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
    {
      const dt_iop_order_iccprofile_info_t *const work_profile
          = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

      float picker_mean[4], picker_min[4], picker_max[4];

      // the global live samples ...
      GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
      if(samples)
      {
        const dt_iop_order_iccprofile_info_t *const histogram_profile = dt_ioppr_get_histogram_profile_info(dev);
        if(work_profile && histogram_profile)
        {
          dt_colorpicker_sample_t *sample = NULL;
          while(samples)
          {
            sample = samples->data;

            // this functions need a 4c image
            for(int k = 0; k < 3; k++)
            {
              picker_mean[k] = sample->picked_color_rgb_mean[k];
              picker_min[k] = sample->picked_color_rgb_min[k];
              picker_max[k] = sample->picked_color_rgb_max[k];
            }
            picker_mean[3] = picker_min[3] = picker_max[3] = 1.f;

            dt_ioppr_transform_image_colorspace_rgb(picker_mean, picker_mean, 1, 1, histogram_profile,
                                                    work_profile, "rgb curve");
            dt_ioppr_transform_image_colorspace_rgb(picker_min, picker_min, 1, 1, histogram_profile, work_profile,
                                                    "rgb curve");
            dt_ioppr_transform_image_colorspace_rgb(picker_max, picker_max, 1, 1, histogram_profile, work_profile,
                                                    "rgb curve");

            picker_scale(picker_mean, picker_mean, p, work_profile);
            picker_scale(picker_min, picker_min, p, work_profile);
            picker_scale(picker_max, picker_max, p, work_profile);

            // Convert abcissa to log coordinates if needed
            picker_min[ch] = _curve_to_mouse(picker_min[ch], g->zoom_factor, g->offset_x);
            picker_max[ch] = _curve_to_mouse(picker_max[ch], g->zoom_factor, g->offset_x);
            picker_mean[ch] = _curve_to_mouse(picker_mean[ch], g->zoom_factor, g->offset_x);

            cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.15);
            cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                            -height);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.5);
            cairo_move_to(cr, width * picker_mean[ch], 0);
            cairo_line_to(cr, width * picker_mean[ch], -height);
            cairo_stroke(cr);

            samples = g_slist_next(samples);
          }
      }
      }

      // ... and the local sample
      if(self->picked_color_max[ch] >= 0.0f)
      {
        PangoLayout *layout;
        PangoRectangle ink;
        PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        pango_font_description_set_absolute_size(desc, PANGO_SCALE);
        layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);

        picker_scale(self->picked_color, picker_mean, p, work_profile);
        picker_scale(self->picked_color_min, picker_min, p, work_profile);
        picker_scale(self->picked_color_max, picker_max, p, work_profile);

        // scale conservatively to 100% of width:
        snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        pango_font_description_set_absolute_size(desc, width * 1.0 / ink.width * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);

        picker_min[ch] = _curve_to_mouse(picker_min[ch], g->zoom_factor, g->offset_x);
        picker_max[ch] = _curve_to_mouse(picker_max[ch], g->zoom_factor, g->offset_x);
        picker_mean[ch] = _curve_to_mouse(picker_mean[ch], g->zoom_factor, g->offset_x);

        cairo_set_source_rgba(cr, 0.7, 0.5, 0.5, 0.33);
        cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.9, 0.7, 0.7, 0.5);
        cairo_move_to(cr, width * picker_mean[ch], 0);
        cairo_line_to(cr, width * picker_mean[ch], -height);
        cairo_stroke(cr);

        picker_scale(self->picked_color, picker_mean, p, work_profile);
        picker_scale(self->picked_output_color, picker_min, p, work_profile);
        snprintf(text, sizeof(text), "%.1f → %.1f", picker_mean[ch] * 255.f, picker_min[ch] * 255.f);

        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(0.04) * height);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, 0.02f * width, -0.94 * height - ink.height - ink.y);
        pango_cairo_show_layout(cr, layout);
        cairo_stroke(cr);
        pango_font_description_free(desc);
        g_object_unref(layout);
      }
    }
  }

  // draw zoom info
  if(darktable.develop->darkroom_skip_mouse_events)
  {
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "zoom: 100 x: 100 y: 100");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width * 1.0 / ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    snprintf(text, sizeof(text), "zoom: %i x: %i y: %i", (int)((g->zoom_factor - 1.f) * 100.f),
             (int)(g->offset_x * 100.f), (int)(g->offset_y * 100.f));

    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.5);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  else if(g->selected >= 0)
  {
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width * 1.0 / ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    const float min_scale_value = 0.0f;
    const float max_scale_value = 255.0f;

    const float x_node_value = curve_nodes[g->selected].x * (max_scale_value - min_scale_value) + min_scale_value;
    const float y_node_value = curve_nodes[g->selected].y * (max_scale_value - min_scale_value) + min_scale_value;
    const float d_node_value = y_node_value - x_node_value;
    snprintf(text, sizeof(text), "%.1f / %.1f ( %+.1f)", x_node_value, y_node_value, d_node_value);

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

    // enlarge selected node
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = _curve_to_mouse(curve_nodes[g->selected].x, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(curve_nodes[g->selected].y, g->zoom_factor, g->offset_y);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);

  const float y_offset = _curve_to_mouse(g->draw_ys[0], g->zoom_factor, g->offset_y);
  cairo_move_to(cr, 0, -height * y_offset);

  for(int k = 1; k < DT_IOP_RGBCURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_RGBCURVE_RES - 1.0f);
    float yy;

    if(xx > xm)
    {
      yy = dt_iop_eval_exp(unbounded_coeffs, xx);
    }
    else
    {
      yy = g->draw_ys[k];
    }

    const float x = _curve_to_mouse(xx, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(yy, g->zoom_factor, g->offset_y);

    cairo_line_to(cr, x * width, -height * y);
  }
  cairo_stroke(cr);

finally:
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _area_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;

  // drag the draw area
  if(darktable.develop->darkroom_skip_mouse_events)
  {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;

    const float mx = g->mouse_x;
    const float my = g->mouse_y;

    g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
    g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

    if(event->state & GDK_BUTTON1_MASK)
    {
      g->offset_x += (mx - g->mouse_x) / g->zoom_factor;
      g->offset_y += (my - g->mouse_y) / g->zoom_factor;

      g->offset_x = CLAMP(g->offset_x, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);
      g->offset_y = CLAMP(g->offset_y, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);

      gtk_widget_queue_draw(self->widget);
    }
    return TRUE;
  }

  const int ch = g->channel;
  const int nodes = p->curve_num_nodes[ch];
  dt_iop_rgbcurve_node_t *curve_nodes = p->curve_nodes[ch];

  // if autoscale is on: do not modify g and b curves
  if((p->curve_autoscale != DT_S_SCALE_MANUAL_RGB) && g->channel != DT_IOP_RGBCURVE_R) goto finally;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;

  const double old_m_x = g->mouse_x;
  const double old_m_y = g->mouse_y;

  g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

  const float mx = g->mouse_x;
  const float my = g->mouse_y;
  const float linx = _mouse_to_curve(mx, g->zoom_factor, g->offset_x),
              liny = _mouse_to_curve(my, g->zoom_factor, g->offset_y);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(g->selected >= 0)
    {
      // this is used to translate mause position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x
          = old_m_x - _curve_to_mouse(curve_nodes[g->selected].x, g->zoom_factor, g->offset_x);
      const float translate_mouse_y
          = old_m_y - _curve_to_mouse(curve_nodes[g->selected].y, g->zoom_factor, g->offset_y);
      // dx & dy are in linear coordinates
      const float dx = _mouse_to_curve(g->mouse_x - translate_mouse_x, g->zoom_factor, g->offset_x)
                       - _mouse_to_curve(old_m_x - translate_mouse_x, g->zoom_factor, g->offset_x);
      const float dy = _mouse_to_curve(g->mouse_y - translate_mouse_y, g->zoom_factor, g->offset_y)
                       - _mouse_to_curve(old_m_y - translate_mouse_y, g->zoom_factor, g->offset_y);

      dt_iop_color_picker_reset(self, TRUE);
      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < DT_IOP_RGBCURVE_MAXNODES && g->selected >= -1)
    {
      dt_iop_color_picker_reset(self, TRUE);
      // no vertex was close, create a new one!
      g->selected = _add_node(curve_nodes, &p->curve_num_nodes[ch], linx, liny);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = .04f * .04f; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      const float dist = (my - _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y))
                             * (my - _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y))
                         + (mx - _curve_to_mouse(curve_nodes[k].x, g->zoom_factor, g->offset_x))
                               * (mx - _curve_to_mouse(curve_nodes[k].x, g->zoom_factor, g->offset_x));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    g->selected = nearest;
  }
finally:
  if(g->selected >= 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _area_button_press_callback(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_params_t *d = (dt_iop_rgbcurve_params_t *)self->default_params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  if(darktable.develop->darkroom_skip_mouse_events) return TRUE;

  const int ch = g->channel;
  const int autoscale = p->curve_autoscale;
  const int nodes = p->curve_num_nodes[ch];
  dt_iop_rgbcurve_node_t *curve_nodes = p->curve_nodes[ch];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK
       && nodes < DT_IOP_RGBCURVE_MAXNODES && g->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      const int width = allocation.width - 2 * inset;
      const int height = allocation.height - 2 * inset;

      g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
      g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

      const float mx = g->mouse_x;
      const float linx = _mouse_to_curve(mx, g->zoom_factor, g->offset_x);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(curve_nodes[0].x > mx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(curve_nodes[k].x > mx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;

        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(g->minmax_curve[ch], linx);

        if(y >= 0.0f && y <= 1.0f) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(curve_nodes, &p->curve_num_nodes[ch], linx, y);

          // maybe set the new one as being selected
          const float min = .04f * .04f; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            const float other_y = _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y);
            const float dist = (y - other_y) * (y - other_y);
            if(dist < min) g->selected = selected;
          }

          dt_iop_color_picker_reset(self, TRUE);
          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }

      return TRUE;
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset current curve
      // if autoscale is on: allow only reset of L curve
      if(!((autoscale != DT_S_SCALE_MANUAL_RGB) && ch != DT_IOP_RGBCURVE_R))
      {
        p->curve_num_nodes[ch] = d->curve_num_nodes[ch];
        p->curve_type[ch] = d->curve_type[ch];
        for(int k = 0; k < d->curve_num_nodes[ch]; k++)
        {
          p->curve_nodes[ch][k].x = d->curve_nodes[ch][k].x;
          p->curve_nodes[ch][k].y = d->curve_nodes[ch][k].y;
        }
        g->selected = -2; // avoid motion notify re-inserting immediately.
        dt_bauhaus_combobox_set(g->interpolator, p->curve_type[DT_IOP_RGBCURVE_R]);
        dt_iop_color_picker_reset(self, TRUE);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        gtk_widget_queue_draw(self->widget);
      }
      else
      {
        if(ch != DT_IOP_RGBCURVE_R)
        {
          p->curve_autoscale = DT_S_SCALE_MANUAL_RGB;
          g->selected = -2; // avoid motion notify re-inserting immediately.
          dt_bauhaus_combobox_set(g->autoscale, 1);
          dt_iop_color_picker_reset(self, TRUE);
          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
  }
  else if(event->button == 3 && g->selected >= 0)
  {
    if(g->selected == 0 || g->selected == nodes - 1)
    {
      const float reset_value = g->selected == 0 ? 0.f : 1.f;
      curve_nodes[g->selected].y = curve_nodes[g->selected].x = reset_value;
      dt_iop_color_picker_reset(self, TRUE);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
      return TRUE;
    }

    for(int k = g->selected; k < nodes - 1; k++)
    {
      curve_nodes[k].x = curve_nodes[k + 1].x;
      curve_nodes[k].y = curve_nodes[k + 1].y;
    }
    g->selected = -2; // avoid re-insertion of that point immediately after this
    p->curve_num_nodes[ch]--;
    dt_iop_color_picker_reset(self, TRUE);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
    return TRUE;
  }
  return FALSE;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  g->channel = DT_IOP_RGBCURVE_R;
  g->selected = -1;
  g->offset_x = g->offset_y = 0.f;
  g->zoom_factor = 1.f;

  dt_bauhaus_combobox_set(g->interpolator, p->curve_type[DT_IOP_RGBCURVE_R]);

  gtk_widget_queue_draw(self->widget);
}

void change_image(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  if(g)
  {
    if(!g->channel)
      g->channel = DT_IOP_RGBCURVE_R;
    g->mouse_x = g->mouse_y = -1.0;
    g->selected = -1;
    g->offset_x = g->offset_y = 0.f;
    g->zoom_factor = 1.f;
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rgbcurve_gui_data_t));
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    g->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->curve_type[ch]);
    g->minmax_curve_nodes[ch] = p->curve_num_nodes[ch];
    g->minmax_curve_type[ch] = p->curve_type[ch];
    for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(g->minmax_curve[ch], p->curve_nodes[ch][k].x, p->curve_nodes[ch][k].y);
  }

  g->channel = DT_IOP_RGBCURVE_R;
  g->timeout_handle = 0;
  change_image(self);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->autoscale = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->autoscale, NULL, _("mode"));
  dt_bauhaus_combobox_add(g->autoscale, _("RGB, linked channels"));
  dt_bauhaus_combobox_add(g->autoscale, _("RGB, independent channels"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->autoscale, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->autoscale, _("choose between linked and independent channels."));
  g_signal_connect(G_OBJECT(g->autoscale), "value-changed", G_CALLBACK(autoscale_callback), self);

  // tabs
  g->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(g->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("  R  ")));
  gtk_widget_set_tooltip_text(
      gtk_notebook_get_tab_label(g->channel_tabs, gtk_notebook_get_nth_page(g->channel_tabs, -1)),
      _("curve nodes for r channel"));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("  G  ")));
  gtk_widget_set_tooltip_text(
      gtk_notebook_get_tab_label(g->channel_tabs, gtk_notebook_get_nth_page(g->channel_tabs, -1)),
      _("curve nodes for g channel"));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->channel_tabs), GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("  B  ")));
  gtk_widget_set_tooltip_text(
      gtk_notebook_get_tab_label(g->channel_tabs, gtk_notebook_get_nth_page(g->channel_tabs, -1)),
      _("curve nodes for b channel"));

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(g->channel_tabs, g->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), g->channel);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->channel_tabs), TRUE, TRUE, 0);
  

  // color pickers
  g->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, hbox);
  gtk_widget_set_tooltip_text(g->colorpicker, _("pick GUI color from image\nctrl+click to select an area"));
  gtk_widget_set_name(g->colorpicker, "keep-active");
  g->colorpicker_set_values = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, hbox);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->colorpicker_set_values),
                               dtgtk_cairo_paint_colorpicker_set_values, 
                               CPF_STYLE_FLAT | CPF_BG_TRANSPARENT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(g->colorpicker_set_values, DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14));
  gtk_widget_set_tooltip_text(g->colorpicker_set_values, _("create a curve based on an area from the image\n"
                                                           "drag to create a flat curve\n"
                                                           "ctrl+drag to create a positive curve\n"
                                                           "shift+drag to create a negative curve"));

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), vbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->channel_tabs), "switch_page", G_CALLBACK(tab_switch_callback), self);

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->area), TRUE, TRUE, 0);

  // FIXME: that tooltip goes in the way of the numbers when you hover a node to get a reading
  // gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("double click to reset curve"));

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                 | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_area_draw_callback), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(_area_button_press_callback), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(_area_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(_area_leave_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "enter-notify-event", G_CALLBACK(_area_enter_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "configure-event", G_CALLBACK(_area_resized_callback), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(_area_scrolled_callback), self);
  g_signal_connect(G_OBJECT(g->area), "key-press-event", G_CALLBACK(_area_key_press_callback), self);

  /* From src/common/curve_tools.h :
    #define CUBIC_SPLINE 0
    #define CATMULL_ROM 1
    #define MONOTONE_HERMITE 2
  */
  g->interpolator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->interpolator, NULL, _("interpolation method"));
  dt_bauhaus_combobox_add(g->interpolator, _("cubic spline"));
  dt_bauhaus_combobox_add(g->interpolator, _("centripetal spline"));
  dt_bauhaus_combobox_add(g->interpolator, _("monotonic spline"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->interpolator, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(
      g->interpolator,
      _("change this method if you see oscillations or cusps in the curve\n"
        "- cubic spline is better to produce smooth curves but oscillates when nodes are too close\n"
        "- centripetal is better to avoids cusps and oscillations with close nodes but is less smooth\n"
        "- monotonic is better for accuracy of pure analytical functions (log, gamma, exp)\n"));
  g_signal_connect(G_OBJECT(g->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  g->chk_compensate_middle_grey = gtk_check_button_new_with_label(_("compensate middle grey"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chk_compensate_middle_grey), p->compensate_middle_grey);
  gtk_widget_set_tooltip_text(g->chk_compensate_middle_grey, _("compensate middle grey"));
  g_signal_connect(G_OBJECT(g->chk_compensate_middle_grey), "toggled", G_CALLBACK(compensate_middle_grey_callback),
                   self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->chk_compensate_middle_grey, TRUE, TRUE, 0);

  g->cmb_preserve_colors = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cmb_preserve_colors, NULL, _("preserve colors"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("none"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("luminance"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("max RGB"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("average RGB"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("sum RGB"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("norm RGB"));
  dt_bauhaus_combobox_add(g->cmb_preserve_colors, _("basic power"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->cmb_preserve_colors, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->cmb_preserve_colors, _("method to preserve colors when applying contrast"));
  g_signal_connect(G_OBJECT(g->cmb_preserve_colors), "value-changed", G_CALLBACK(preserve_colors_callback), self);

  g->sizegroup = GTK_SIZE_GROUP(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
  gtk_size_group_add_widget(g->sizegroup, GTK_WIDGET(g->area));
  gtk_size_group_add_widget(g->sizegroup, GTK_WIDGET(g->channel_tabs));
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  dt_bauhaus_combobox_set(g->autoscale, p->curve_autoscale);
  dt_bauhaus_combobox_set(g->interpolator, p->curve_type[DT_IOP_RGBCURVE_R]);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chk_compensate_middle_grey), p->compensate_middle_grey);
  dt_bauhaus_combobox_set(g->cmb_preserve_colors, p->preserve_colors);

  if(g->timeout_handle)
  {
    g_source_remove(g->timeout_handle);
    g->timeout_handle = 0;
  }

  _rgbcurve_show_hide_controls(p, g);

  // that's all, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  // this one we need to unref manually. not so the initially unowned widgets.
  g_object_unref(g->sizegroup);

  for(int k = 0; k < DT_IOP_RGBCURVE_MAX_CHANNELS; k++) dt_draw_curve_destroy(g->minmax_curve[k]);

  if(g->timeout_handle) g_source_remove(g->timeout_handle);

  free(self->gui_data);
  self->gui_data = NULL;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)malloc(sizeof(dt_iop_rgbcurve_data_t));
  dt_iop_rgbcurve_params_t *default_params = (dt_iop_rgbcurve_params_t *)self->default_params;
  piece->data = (void *)d;
  memcpy(&d->params, default_params, sizeof(dt_iop_rgbcurve_params_t));

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->curve_type[ch]);
    d->params.curve_num_nodes[ch] = default_params->curve_num_nodes[ch];
    d->params.curve_type[ch] = default_params->curve_type[ch];
    for(int k = 0; k < default_params->curve_num_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->curve_nodes[ch][k].x,
                                    default_params->curve_nodes[ch][k].y);
  }

  for(int k = 0; k < 0x10000; k++) d->table[DT_IOP_RGBCURVE_R][k] = k / 0x10000; // identity for r
  for(int k = 0; k < 0x10000; k++) d->table[DT_IOP_RGBCURVE_G][k] = k / 0x10000; // identity for g
  for(int k = 0; k < 0x10000; k++) d->table[DT_IOP_RGBCURVE_B][k] = k / 0x10000; // identity for b
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)(piece->data);
  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_rgbcurve_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_rgbcurve_params_t));

  module->default_enabled = 0;
  module->request_histogram |= (DT_REQUEST_ON);
  module->params_size = sizeof(dt_iop_rgbcurve_params_t);
  module->gui_data = NULL;

  dt_iop_rgbcurve_params_t tmp = (dt_iop_rgbcurve_params_t){
    // three curves (r, g, b) with a number of nodes
    .curve_nodes
    = { { { 0.0, 0.0 }, { 1.0, 1.0 } }, { { 0.0, 0.0 }, { 1.0, 1.0 } }, { { 0.0, 0.0 }, { 1.0, 1.0 } } },
    .curve_num_nodes = { 2, 2, 2 }, // number of nodes per curve
    // .curve_type = { CATMULL_ROM, CATMULL_ROM, CATMULL_ROM},  // curve types
    .curve_type = { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
    // .curve_type = { CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE},
    .curve_autoscale = DT_S_SCALE_AUTOMATIC_RGB, // autoscale
    .compensate_middle_grey = 1,
    .preserve_colors = DT_RGB_NORM_LUMINANCE
  };

  module->histogram_middle_grey = tmp.compensate_middle_grey;

  memcpy(module->params, &tmp, sizeof(dt_iop_rgbcurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_rgbcurve_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 25; // rgbcurve.cl, from programs.conf
  dt_iop_rgbcurve_global_data_t *gd
      = (dt_iop_rgbcurve_global_data_t *)malloc(sizeof(dt_iop_rgbcurve_global_data_t));
  module->data = gd;

  gd->kernel_rgbcurve = dt_opencl_create_kernel(program, "rgbcurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_rgbcurve_global_data_t *gd = (dt_iop_rgbcurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_rgbcurve);
  free(module->data);
  module->data = NULL;
}

// this will be called from process*()
// it must be executed only if profile info has changed
static void _generate_curve_lut(dt_dev_pixelpipe_t *pipe, dt_iop_rgbcurve_data_t *d)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);

  dt_iop_rgbcurve_node_t curve_nodes[3][DT_IOP_RGBCURVE_MAXNODES];

  if(work_profile)
  {
    if(d->type_work == work_profile->type && strcmp(d->filename_work, work_profile->filename) == 0) return;
  }

  if(work_profile && d->params.compensate_middle_grey)
  {
    d->type_work = work_profile->type;
    g_strlcpy(d->filename_work, work_profile->filename, sizeof(d->filename_work));

    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
      {
        curve_nodes[ch][k].x = dt_ioppr_uncompensate_middle_grey(d->params.curve_nodes[ch][k].x, work_profile);
        curve_nodes[ch][k].y = dt_ioppr_uncompensate_middle_grey(d->params.curve_nodes[ch][k].y, work_profile);
      }
    }
  }
  else
  {
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      memcpy(curve_nodes[ch], d->params.curve_nodes[ch], DT_IOP_RGBCURVE_MAXNODES * sizeof(dt_iop_rgbcurve_node_t));
    }
  }

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    // take care of possible change of curve type or number of nodes (not yet implemented in UI)
    if(d->curve_changed[ch])
    {
      dt_draw_curve_destroy(d->curve[ch]);
      d->curve[ch] = dt_draw_curve_new(0.0, 1.0, d->params.curve_type[ch]);
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
        (void)dt_draw_curve_add_point(d->curve[ch], curve_nodes[ch][k].x, curve_nodes[ch][k].y);
    }
    else
    {
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
        dt_draw_curve_set_point(d->curve[ch], k, curve_nodes[ch][k].x, curve_nodes[ch][k].y);
    }

    dt_draw_curve_calc_values(d->curve[ch], 0.0f, 1.0f, 0x10000, NULL, d->table[ch]);
  }

  // extrapolation for each curve (right hand side only):
  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    const float xm_L = curve_nodes[ch][d->params.curve_num_nodes[ch] - 1].x;
    const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
    const float y_L[4] = { d->table[ch][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
    dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs[ch]);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)(piece->data);
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)p1;

  if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= (DT_REQUEST_ON);
  else
    piece->request_histogram &= ~(DT_REQUEST_ON);

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    d->curve_changed[ch]
        = (d->params.curve_type[ch] != p->curve_type[ch] || d->params.curve_nodes[ch] != p->curve_nodes[ch]);

  memcpy(&d->params, p, sizeof(dt_iop_rgbcurve_params_t));

  // working color profile
  d->type_work = DT_COLORSPACE_NONE;
  d->filename_work[0] = '\0';
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)piece->data;
  dt_iop_rgbcurve_global_data_t *gd = (dt_iop_rgbcurve_global_data_t *)self->global_data;

  _generate_curve_lut(piece->pipe, d);

  cl_int err = CL_SUCCESS;

  cl_mem dev_r = NULL;
  cl_mem dev_g = NULL;
  cl_mem dev_b = NULL;
  cl_mem dev_coeffs_r = NULL;
  cl_mem dev_coeffs_g = NULL;
  cl_mem dev_coeffs_b = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int autoscale = d->params.curve_autoscale;
  const int preserve_colors = d->params.preserve_colors;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto cleanup;

  dev_r = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_R], 256, 256, sizeof(float));
  if(dev_r == NULL)
  {
    fprintf(stderr, "[rgbcurve process_cl] error allocating memory 1\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_g = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_G], 256, 256, sizeof(float));
  if(dev_g == NULL)
  {
    fprintf(stderr, "[rgbcurve process_cl] error allocating memory 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_b = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_B], 256, 256, sizeof(float));
  if(dev_b == NULL)
  {
    fprintf(stderr, "[rgbcurve process_cl] error allocating memory 3\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_coeffs_r = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs[0]);
  if(dev_coeffs_r == NULL)
  {
    fprintf(stderr, "[rgbcurve process_cl] error allocating memory 4\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_coeffs_g = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs[1]);
  if(dev_coeffs_g == NULL)
  {
    fprintf(stderr, "[rgbcurve process_cl] error allocating memory 5\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_coeffs_b = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, d->unbounded_coeffs[2]);
  if(dev_coeffs_b == NULL)
  {
    fprintf(stderr, "[rgbcurve process_cl] error allocating memory 6\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 4, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 5, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 6, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 7, sizeof(cl_mem), (void *)&dev_coeffs_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 8, sizeof(cl_mem), (void *)&dev_coeffs_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 9, sizeof(cl_mem), (void *)&dev_coeffs_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 10, sizeof(int), (void *)&autoscale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 11, sizeof(int), (void *)&preserve_colors);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 12, sizeof(cl_mem), (void *)&dev_profile_info);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 13, sizeof(cl_mem), (void *)&dev_profile_lut);
  dt_opencl_set_kernel_arg(devid, gd->kernel_rgbcurve, 14, sizeof(int), (void *)&use_work_profile);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_rgbcurve, sizes);
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[rgbcurve process_cl] error %i enqueue kernel\n", err);
    goto cleanup;
  }

cleanup:
  if(dev_r) dt_opencl_release_mem_object(dev_r);
  if(dev_g) dt_opencl_release_mem_object(dev_g);
  if(dev_b) dt_opencl_release_mem_object(dev_b);
  if(dev_coeffs_r) dt_opencl_release_mem_object(dev_coeffs_r);
  if(dev_coeffs_g) dt_opencl_release_mem_object(dev_coeffs_g);
  if(dev_coeffs_b) dt_opencl_release_mem_object(dev_coeffs_b);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_rgbcurve] couldn't enqueue kernel! %d\n", err);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const int ch = piece->colors;
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)(piece->data);

  _generate_curve_lut(piece->pipe, d);

  const float xm_L = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_R][0];
  const float xm_g = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_G][0];
  const float xm_b = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_B][0];

  const int width = roi_out->width;
  const int height = roi_out->height;
  const int autoscale = d->params.curve_autoscale;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(autoscale, ch, height, ivoid, ovoid, work_profile, xm_b, \
                      xm_g, xm_L, width) \
  shared(d) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *in = ((float *)ivoid) + (size_t)y * ch * width;
    float *out = ((float *)ovoid) + (size_t)y * ch * width;

    for(int x = 0; x < width; x++, in += ch, out += ch)
    {
      if(autoscale == DT_S_SCALE_MANUAL_RGB)
      {
        int c = 0;
        out[c] = (in[c] < xm_L) ? d->table[DT_IOP_RGBCURVE_R][CLAMP((int)(in[c] * 0x10000ul), 0, 0xffff)]
                                : dt_iop_eval_exp(d->unbounded_coeffs[DT_IOP_RGBCURVE_R], in[c]);
        c = 1;
        out[c] = (in[c] < xm_g) ? d->table[DT_IOP_RGBCURVE_G][CLAMP((int)(in[c] * 0x10000ul), 0, 0xffff)]
                                : dt_iop_eval_exp(d->unbounded_coeffs[DT_IOP_RGBCURVE_G], in[c]);
        c = 2;
        out[c] = (in[c] < xm_b) ? d->table[DT_IOP_RGBCURVE_B][CLAMP((int)(in[c] * 0x10000ul), 0, 0xffff)]
                                : dt_iop_eval_exp(d->unbounded_coeffs[DT_IOP_RGBCURVE_B], in[c]);
      }
      else if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
      {
        if(d->params.preserve_colors == DT_RGB_NORM_NONE)
        {
          for(int c = 0; c < 3; c++)
          {
            out[c] = (in[c] < xm_L) ? d->table[DT_IOP_RGBCURVE_R][CLAMP((int)(in[c] * 0x10000ul), 0, 0xffff)]
                                    : dt_iop_eval_exp(d->unbounded_coeffs[DT_IOP_RGBCURVE_R], in[c]);
          }
        }
        else
        {
          float ratio = 1.f;
          const float lum = dt_rgb_norm(in, d->params.preserve_colors, work_profile);
          if(lum > 0.f)
          {
            const float curve_lum = (lum < xm_L)
                                        ? d->table[DT_IOP_RGBCURVE_R][CLAMP((int)(lum * 0x10000ul), 0, 0xffff)]
                                        : dt_iop_eval_exp(d->unbounded_coeffs[DT_IOP_RGBCURVE_R], lum);
            ratio = curve_lum / lum;
          }
          for(size_t c = 0; c < 3; c++)
          {
            out[c] = (ratio * in[c]);
          }
        }
      }
      out[3] = in[3];
    }
  }
}

#undef DT_GUI_CURVE_EDITOR_INSET
#undef DT_IOP_RGBCURVE_RES
#undef DT_IOP_RGBCURVE_MAXNODES
#undef DT_IOP_RGBCURVE_MIN_X_DISTANCE
#undef DT_IOP_COLOR_ICC_LEN

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
