/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

#include <assert.h>
#include <glib/gprintf.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/atomic.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "common/tags.h"
#include "common/presets.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/lightroom.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "imageio/imageio_common.h"

#ifdef USE_LUA
#include "lua/call.h"
#endif

#define DT_DEV_AVERAGE_DELAY_START 250
#define DT_DEV_PREVIEW_AVERAGE_DELAY_START 50
#define DT_DEV_AVERAGE_DELAY_COUNT 5

void dt_dev_init(dt_develop_t *dev,
                 const gboolean gui_attached)
{
  memset(dev, 0, sizeof(dt_develop_t));
  dev->full_preview = FALSE;
  dev->gui_module = NULL;
  dev->timestamp = 0;
  dev->average_delay = DT_DEV_AVERAGE_DELAY_START;
  dev->preview_average_delay = DT_DEV_PREVIEW_AVERAGE_DELAY_START;
  dev->preview2_average_delay = DT_DEV_PREVIEW_AVERAGE_DELAY_START;
  dev->gui_leaving = FALSE;
  dev->gui_synch = FALSE;
  dt_pthread_mutex_init(&dev->history_mutex, NULL);
  dev->history_end = 0;
  dev->history = NULL; // empty list
  dev->history_postpone_invalidate = FALSE;

  dev->gui_attached = gui_attached;
  dev->width = -1;
  dev->height = -1;

  dt_image_init(&dev->image_storage);
  dev->image_status = dev->preview_status = dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->history_updating = dev->image_force_reload = FALSE;
  dev->image_loading = dev->preview_loading = FALSE;
  dev->preview2_loading = dev->preview_input_changed = dev->preview2_input_changed = FALSE;
  dev->autosaving = FALSE;
  dev->image_invalid_cnt = 0;
  dev->pipe = dev->preview_pipe = dev->preview2_pipe = NULL;
  dt_pthread_mutex_init(&dev->pipe_mutex, NULL);
  dt_pthread_mutex_init(&dev->preview_pipe_mutex, NULL);
  dt_pthread_mutex_init(&dev->preview2_pipe_mutex, NULL);
  dev->histogram_pre_tonecurve = NULL;
  dev->histogram_pre_levels = NULL;
  dev->preview_downsampling = dt_dev_get_preview_downsampling();
  dev->forms = NULL;
  dev->form_visible = NULL;
  dev->form_gui = NULL;
  dev->allforms = NULL;

  if(dev->gui_attached)
  {
    dev->pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview2_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->pipe);
    dt_dev_pixelpipe_init_preview(dev->preview_pipe);
    dt_dev_pixelpipe_init_preview2(dev->preview2_pipe);
    dev->histogram_pre_tonecurve = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));
    dev->histogram_pre_levels = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));

    // FIXME: these are uint32_t, setting to -1 is confusing
    dev->histogram_pre_tonecurve_max = -1;
    dev->histogram_pre_levels_max = -1;
    dev->darkroom_mouse_in_center_area = FALSE;
    dev->darkroom_skip_mouse_events = FALSE;
  }

  dev->iop_instance = 0;
  dev->iop = NULL;
  dev->alliop = NULL;

  dev->allprofile_info = NULL;

  dev->iop_order_version = 0;
  dev->iop_order_list = NULL;

  dev->proxy.exposure.module = NULL;
  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_is_D65 = TRUE; // don't display error messages until
                               // we know for sure it's FALSE
  dev->proxy.wb_coeffs[0] = 0.f;

  dev->rawoverexposed.enabled = FALSE;
  dev->rawoverexposed.mode =
    dt_conf_get_int("darkroom/ui/rawoverexposed/mode");
  dev->rawoverexposed.colorscheme =
    dt_conf_get_int("darkroom/ui/rawoverexposed/colorscheme");
  dev->rawoverexposed.threshold =
    dt_conf_get_float("darkroom/ui/rawoverexposed/threshold");

  dev->overexposed.enabled = FALSE;
  dev->overexposed.mode = dt_conf_get_int("darkroom/ui/overexposed/mode");
  dev->overexposed.colorscheme = dt_conf_get_int("darkroom/ui/overexposed/colorscheme");
  dev->overexposed.lower = dt_conf_get_float("darkroom/ui/overexposed/lower");
  dev->overexposed.upper = dt_conf_get_float("darkroom/ui/overexposed/upper");

  dev->iso_12646.enabled = FALSE;

  dev->second_window.zoom = DT_ZOOM_FIT;
  dev->second_window.closeup = 0;
  dev->second_window.zoom_x = dev->second_window.zoom_y = 0;
  dev->second_window.zoom_scale = 1.0f;
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  if(!dev) return;
  // image_cache does not have to be unref'd, this is done outside develop module.
  dt_pthread_mutex_destroy(&dev->pipe_mutex);
  dt_pthread_mutex_destroy(&dev->preview_pipe_mutex);
  dt_pthread_mutex_destroy(&dev->preview2_pipe_mutex);
  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_coeffs[0] = 0.f;
  if(dev->pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->pipe);
    free(dev->pipe);
  }
  if(dev->preview_pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview_pipe);
    free(dev->preview_pipe);
  }
  if(dev->preview2_pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview2_pipe);
    free(dev->preview2_pipe);
  }
  while(dev->history)
  {
    dt_dev_free_history_item(((dt_dev_history_item_t *)dev->history->data));
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->iop->data);
    free(dev->iop->data);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  g_list_free_full(dev->iop_order_list, free);
  while(dev->allprofile_info)
  {
    dt_ioppr_cleanup_profile_info
      ((dt_iop_order_iccprofile_info_t *)dev->allprofile_info->data);
    dt_free_align(dev->allprofile_info->data);
    dev->allprofile_info = g_list_delete_link(dev->allprofile_info, dev->allprofile_info);
  }
  dt_pthread_mutex_destroy(&dev->history_mutex);
  free(dev->histogram_pre_tonecurve);
  free(dev->histogram_pre_levels);

  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);

  dt_conf_set_int("darkroom/ui/rawoverexposed/mode",
                  dev->rawoverexposed.mode);
  dt_conf_set_int("darkroom/ui/rawoverexposed/colorscheme",
                  dev->rawoverexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/rawoverexposed/threshold",
                    dev->rawoverexposed.threshold);

  dt_conf_set_int("darkroom/ui/overexposed/mode", dev->overexposed.mode);
  dt_conf_set_int("darkroom/ui/overexposed/colorscheme", dev->overexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/overexposed/lower", dev->overexposed.lower);
  dt_conf_set_float("darkroom/ui/overexposed/upper", dev->overexposed.upper);
}

float dt_dev_get_preview_downsampling()
{
  const char *preview_downsample = dt_conf_get_string_const("preview_downsampling");
  const float downsample = (g_strcmp0(preview_downsample, "original") == 0) ? 1.0f
        : (g_strcmp0(preview_downsample, "to 1/2")==0) ? 0.5f
        : (g_strcmp0(preview_downsample, "to 1/3")==0) ? 1/3.0f
        : 0.25f;
  return downsample;
}

void dt_dev_process_image(dt_develop_t *dev)
{
  if(!dev->gui_attached || dev->pipe->processing) return;
  const int err
      = dt_control_add_job_res(darktable.control,
                               dt_dev_process_image_job_create(dev), DT_CTL_WORKER_ZOOM_1);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[dev_process_image] job queue exceeded!\n");
}

void dt_dev_process_preview(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;
  const int err = dt_control_add_job_res(darktable.control,
                                         dt_dev_process_preview_job_create(dev),
                                         DT_CTL_WORKER_ZOOM_FILL);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[dev_process_preview] job queue exceeded!\n");
}

void dt_dev_process_preview2(dt_develop_t *dev)
{
  const int err = dt_control_add_job_res(darktable.control,
                                         dt_dev_process_preview2_job_create(dev),
                                         DT_CTL_WORKER_ZOOM_2);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[dev_process_preview2] job queue exceeded!\n");
}

void dt_dev_invalidate(dt_develop_t *dev)
{
  dev->image_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
  if(dev->preview_pipe) dev->preview_pipe->input_timestamp = dev->timestamp;
  if(dev->preview2_pipe) dev->preview2_pipe->input_timestamp = dev->timestamp;
}

void dt_dev_invalidate_all(dt_develop_t *dev)
{
  dev->image_status = dev->preview_status = dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
}

void dt_dev_invalidate_preview(dt_develop_t *dev)
{
  dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
  if(dev->pipe) dev->pipe->input_timestamp = dev->timestamp;
  if(dev->preview2_pipe) dev->preview2_pipe->input_timestamp = dev->timestamp;
}

void dt_dev_process_preview_job(dt_develop_t *dev)
{
  if(dev->image_loading)
  {
    // raw is already loading, no use starting another file access, we wait.
    return;
  }

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);

  if(dev->gui_leaving)
  {
    dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
    return;
  }

  dt_control_log_busy_enter();
  dt_control_toast_busy_enter();
  dev->preview_pipe->input_timestamp = dev->timestamp;
  dev->preview_status = DT_DEV_PIXELPIPE_RUNNING;

  // lock if there, issue a background load, if not (best-effort for mip f).
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf,
                      dev->image_storage.id, DT_MIPMAP_F, DT_MIPMAP_BEST_EFFORT,
                      'r');
  if(!buf.buf)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
    return; // not loaded yet. load will issue a gtk redraw on
            // completion, which in turn will trigger us again later.
  }
  // init pixel pipeline for preview.
  dt_dev_pixelpipe_set_input(dev->preview_pipe, dev,
                             (float *)buf.buf, buf.width, buf.height, buf.iscale);

  if(dev->preview_loading)
  {
    dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
    dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
    dt_dev_pixelpipe_cache_flush(dev->preview_pipe);
    dev->preview_loading = FALSE;
  }

  // if raw loaded, get new mipf
  if(dev->preview_input_changed)
  {
    dt_dev_pixelpipe_cache_flush(dev->preview_pipe);
    dev->preview_input_changed = FALSE;
  }

// always process the whole downsampled mipf buffer, to allow for fast
// scrolling and mip4 write-through.
restart:
  if(dev->gui_leaving)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    dev->preview_status = DT_DEV_PIXELPIPE_INVALID;
    dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }
  // adjust pipeline according to changed flag set by {add,pop}_history_item.
  // this locks dev->history_mutex.
  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_change(dev->preview_pipe, dev);
  if(dt_dev_pixelpipe_process(
         dev->preview_pipe, dev, 0, 0,
         dev->preview_pipe->processed_width * dev->preview_downsampling,
         dev->preview_pipe->processed_height * dev->preview_downsampling,
         dev->preview_downsampling))
  {
    if(dev->preview_loading || dev->preview_input_changed)
    {
      dt_control_log_busy_leave();
      dt_control_toast_busy_leave();
      dev->preview_status = DT_DEV_PIXELPIPE_INVALID;
      dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      return;
    }
    else
      goto restart;
  }

  dev->preview_status = DT_DEV_PIXELPIPE_VALID;

  if(!dev->history_postpone_invalidate)
    dt_image_update_final_size(dev->preview_pipe->output_imgid);

  dt_show_times(&start, "[dev_process_preview] pixel pipeline processing");
  dt_dev_average_delay_update(&start, &dev->preview_average_delay);
  dev->gui_previous_pipe_time = dt_get_wtime();

  // if a widget needs to be redraw there's the DT_SIGNAL_*_PIPE_FINISHED signals
  dt_control_log_busy_leave();
  dt_control_toast_busy_leave();
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "pixelpipe-processing-complete",
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(dev->image_storage.id),
      LUA_ASYNC_DONE);
#endif
}

void dt_dev_process_preview2_job(dt_develop_t *dev)
{
  if(dev->image_loading)
  {
    // raw is already loading, no use starting another file access, we wait.
    return;
  }

  if(!(dev->second_window.widget && GTK_IS_WIDGET(dev->second_window.widget)))
  {
    return;
  }

  dt_pthread_mutex_lock(&dev->preview2_pipe_mutex);

  if(dev->gui_leaving)
  {
    dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
    return;
  }

  dt_control_log_busy_enter();
  dt_control_toast_busy_enter();
  dev->preview2_pipe->input_timestamp = dev->timestamp;
  dev->preview2_status = DT_DEV_PIXELPIPE_RUNNING;

  // lock if there, issue a background load, if not (best-effort for mip f).
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf,
                      dev->image_storage.id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  if(!buf.buf)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
    return; // not loaded yet. load will issue a gtk redraw on
            // completion, which in turn will trigger us again later.
  }
  // init pixel pipeline for preview2.
  dt_dev_pixelpipe_set_input(dev->preview2_pipe, dev,
                             (float *)buf.buf, buf.width, buf.height, 1.0 /*buf.iscale*/);

  if(dev->preview2_loading)
  {
    dt_dev_pixelpipe_cleanup_nodes(dev->preview2_pipe);
    dt_dev_pixelpipe_create_nodes(dev->preview2_pipe, dev);
    dt_dev_pixelpipe_cache_flush(dev->preview2_pipe);
    dev->preview2_loading = FALSE;
  }

  // if raw loaded, get new mipf
  if(dev->preview2_input_changed)
  {
    dt_dev_pixelpipe_cache_flush(dev->preview2_pipe);
    dev->preview2_input_changed = 0;
  }

// always process the whole downsampled mipf buffer, to allow for fast
// scrolling and mip4 write-through.
restart:
  if(dev->gui_leaving)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    dev->preview2_status = DT_DEV_PIXELPIPE_INVALID;
    dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }

  const dt_dev_pixelpipe_change_t pipe_changed = dev->pipe->changed;

  // adjust pipeline according to changed flag set by {add,pop}_history_item.
  // this locks dev->history_mutex.
  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_change(dev->preview2_pipe, dev);

  const dt_dev_zoom_t zoom = dt_second_window_get_dev_zoom(dev);
  const int closeup = dt_second_window_get_dev_closeup(dev);
  float zoom_x = dt_second_window_get_dev_zoom_x(dev);
  float zoom_y = dt_second_window_get_dev_zoom_y(dev);
  // if just changed to an image with a different aspect ratio or
  // altered image orientation, the prior zoom xy could now be beyond
  // the image boundary
  if(dev->preview2_loading || (pipe_changed != DT_DEV_PIPE_UNCHANGED))
  {
    dt_second_window_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_second_window_set_dev_zoom_x(dev, zoom_x);
    dt_second_window_set_dev_zoom_y(dev, zoom_y);
  }
  const float scale =
    dt_second_window_get_zoom_scale(dev, zoom, 1.0f, 0) * dev->second_window.ppd;
  int window_width = dev->second_window.width * dev->second_window.ppd;
  int window_height = dev->second_window.height * dev->second_window.ppd;
  if(closeup)
  {
    window_width /= 1 << closeup;
    window_height /= 1 << closeup;
  }

  const int wd = MIN(window_width, dev->preview2_pipe->processed_width * scale);
  const int ht = MIN(window_height, dev->preview2_pipe->processed_height * scale);
  int x = MAX(0, scale * dev->preview2_pipe->processed_width * (.5 + zoom_x) - wd / 2);
  int y = MAX(0, scale * dev->preview2_pipe->processed_height * (.5 + zoom_y) - ht / 2);

  if(dt_dev_pixelpipe_process(dev->preview2_pipe, dev, x, y, wd, ht, scale))
  {
    if(dev->preview2_loading || dev->preview2_input_changed)
    {
      dt_control_log_busy_leave();
      dt_control_toast_busy_leave();
      dev->preview2_status = DT_DEV_PIXELPIPE_INVALID;
      dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      return;
    }
    else
      goto restart;
  }

  dev->preview2_pipe->backbuf_scale = scale;
  dev->preview2_pipe->backbuf_zoom_x = zoom_x;
  dev->preview2_pipe->backbuf_zoom_y = zoom_y;

  dev->preview2_status = DT_DEV_PIXELPIPE_VALID;

  dt_show_times(&start, "[dev_process_preview2] pixel pipeline processing");
  dt_dev_average_delay_update(&start, &dev->preview2_average_delay);

  dt_control_log_busy_leave();
  dt_control_toast_busy_leave();
  dt_pthread_mutex_unlock(&dev->preview2_pipe_mutex);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED);
}

void dt_dev_process_image_job(dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&dev->pipe_mutex);

  if(dev->gui_leaving)
  {
    dt_pthread_mutex_unlock(&dev->pipe_mutex);
    return;
  }

  dt_control_log_busy_enter();
  dt_control_toast_busy_enter();
  // let gui know to draw preview instead of us, if it's there:
  dev->image_status = DT_DEV_PIXELPIPE_RUNNING;

  dt_mipmap_buffer_t buf;
  dt_times_t start;
  dt_get_times(&start);
  dt_mipmap_cache_get(darktable.mipmap_cache,
                      &buf, dev->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  dt_show_times(&start, "[dt_dev_process_image_job] loading image.");

  // failed to load raw?
  if(!buf.buf)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    dev->image_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_pthread_mutex_unlock(&dev->pipe_mutex);
    dev->image_invalid_cnt++;
    return;
  }

  dt_dev_pixelpipe_set_input(dev->pipe, dev, (float *)buf.buf, buf.width, buf.height, 1.0);

  if(dev->image_loading)
  {
    // init pixel pipeline
    dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
    dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
    if(dev->image_force_reload) dt_dev_pixelpipe_cache_flush(dev->pipe);
    dev->image_force_reload = FALSE;
    if(dev->gui_attached)
    {
      // during load, a mipf update could have been issued.
      dev->preview_input_changed = TRUE;
      dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
      dev->preview2_input_changed = TRUE;
      dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;
      dev->gui_synch = TRUE; // notify gui thread we want to synch
                             // (call gui_update in the modules)
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
      dev->preview2_pipe->changed |= DT_DEV_PIPE_SYNCH;
    }
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  }

// adjust pipeline according to changed flag set by {add,pop}_history_item.
restart:
  if(dev->gui_leaving)
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    dev->image_status = DT_DEV_PIXELPIPE_INVALID;
    dt_pthread_mutex_unlock(&dev->pipe_mutex);
    return;
  }
  dev->pipe->input_timestamp = dev->timestamp;
  // dt_dev_pixelpipe_change() will clear the changed value
  const dt_dev_pixelpipe_change_t pipe_changed = dev->pipe->changed;
  // this locks dev->history_mutex.
  dt_dev_pixelpipe_change(dev->pipe, dev);
  // determine scale according to new dimensions
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  float zoom_x = dt_control_get_dev_zoom_x();
  float zoom_y = dt_control_get_dev_zoom_y();
  // if just changed to an image with a different aspect ratio or
  // altered image orientation, the prior zoom xy could now be beyond
  // the image boundary
  if(dev->image_loading || (pipe_changed != DT_DEV_PIPE_UNCHANGED))
  {
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);
  }

  const float scale = dt_dev_get_zoom_scale(dev, zoom, 1.0f, 0) * darktable.gui->ppd;
  int window_width = dev->width * darktable.gui->ppd;
  int window_height = dev->height * darktable.gui->ppd;
  if(closeup)
  {
    window_width /= 1<<closeup;
    window_height /= 1<<closeup;
  }
  const int wd = MIN(window_width, dev->pipe->processed_width * scale);
  const int ht = MIN(window_height, dev->pipe->processed_height * scale);
  const int x = MAX(0, scale * dev->pipe->processed_width  * (.5 + zoom_x) - wd / 2);
  const int y = MAX(0, scale * dev->pipe->processed_height * (.5 + zoom_y) - ht / 2);

  dt_get_times(&start);
  if(dt_dev_pixelpipe_process(dev->pipe, dev, x, y, wd, ht, scale))
  {
    // interrupted because image changed?
    if(dev->image_force_reload)
    {
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      dt_control_log_busy_leave();
      dt_control_toast_busy_leave();
      dev->image_status = DT_DEV_PIXELPIPE_INVALID;
      dt_pthread_mutex_unlock(&dev->pipe_mutex);
      return;
    }
    // or because the pipeline changed?
    else
      goto restart;
  }
  dt_show_times_f(&start,
                  "[dev_process_image] pixel pipeline", "processing `%s'",
                  dev->image_storage.filename);
  dt_dev_average_delay_update(&start, &dev->average_delay);

  // maybe we got zoomed/panned in the meantime?
  if(dev->pipe->changed != DT_DEV_PIPE_UNCHANGED) goto restart;

  // cool, we got a new image!
  dev->pipe->backbuf_scale = scale;
  dev->pipe->backbuf_zoom_x = zoom_x;
  dev->pipe->backbuf_zoom_y = zoom_y;

  dev->image_status = DT_DEV_PIXELPIPE_VALID;
  dev->image_loading = FALSE;
  dev->image_invalid_cnt = 0;
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  // if a widget needs to be redraw there's the DT_SIGNAL_*_PIPE_FINISHED signals
  dt_control_log_busy_leave();
  dt_control_toast_busy_leave();
  dt_pthread_mutex_unlock(&dev->pipe_mutex);

  if(dev->gui_attached && !dev->gui_leaving)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
}


static inline void _dt_dev_load_pipeline_defaults(dt_develop_t *dev)
{
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_iop_reload_defaults(module);
  }
}

// load the raw and get the new image struct, blocking in gui thread
static inline void _dt_dev_load_raw(dt_develop_t *dev,
                                    const dt_imgid_t imgid)
{
  // first load the raw, to make sure dt_image_t will contain all and correct data.
  dt_mipmap_buffer_t buf;
  dt_times_t start;
  dt_get_times(&start);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  dt_show_times(&start, "[dt_dev_load_raw] loading the image.");

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  dev->image_storage = *image;
  dt_image_cache_read_release(darktable.image_cache, image);

  dev->requested_id = dev->image_storage.id;
}

void dt_dev_reload_image(dt_develop_t *dev,
                         const dt_imgid_t imgid)
{
  _dt_dev_load_raw(dev, imgid);
  dev->image_force_reload = dev->image_loading =
    dev->preview_loading = dev->preview2_loading = TRUE;

  dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_invalidate(dev); // only invalidate image, preview will follow once it's loaded.
}

float dt_dev_get_zoom_scale(dt_develop_t *dev,
                            dt_dev_zoom_t zoom,
                            const int closeup_factor,
                            const int preview)
{
  float zoom_scale;

  const float w = preview
    ? dev->preview_pipe->processed_width
    : dev->pipe->processed_width;

  const float h = preview
    ? dev->preview_pipe->processed_height
    : dev->pipe->processed_height;

  const float ps = dev->pipe->backbuf_width
    ? dev->pipe->processed_width / (float)dev->preview_pipe->processed_width
    : dev->preview_pipe->iscale;

  switch(zoom)
  {
    case DT_ZOOM_FIT:
      zoom_scale = fminf(dev->width / w, dev->height / h);
      break;
    case DT_ZOOM_FILL:
      zoom_scale = fmaxf(dev->width / w, dev->height / h);
      break;
    case DT_ZOOM_1:
      zoom_scale = closeup_factor;
      if(preview) zoom_scale *= ps;
      break;
    default: // DT_ZOOM_FREE
      zoom_scale = dt_control_get_dev_zoom_scale();
      if(preview) zoom_scale *= ps;
      break;
  }
  if(preview) zoom_scale /= dev->preview_downsampling;

  return zoom_scale;
}

void dt_dev_load_image_ext(dt_develop_t *dev,
                           const dt_imgid_t imgid,
                           const int32_t snapshot_id)
{
  dt_lock_image(imgid);

  _dt_dev_load_raw(dev, imgid);

  if(dev->pipe)
  {
    dev->pipe->processed_width = 0;
    dev->pipe->processed_height = 0;
  }
  dev->image_loading = dev->first_load = dev->preview_loading =
    dev->preview2_loading = TRUE;

  dev->image_status = dev->preview_status = dev->preview2_status = DT_DEV_PIXELPIPE_DIRTY;

  // we need a global lock as the dev->iop set must not be changed
  // until read history is terminated
  dt_pthread_mutex_lock(&darktable.dev_threadsafe);
  dev->iop = dt_iop_load_modules(dev);

  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE, snapshot_id);
  dt_pthread_mutex_unlock(&darktable.dev_threadsafe);

  dev->first_load = FALSE;

  dt_unlock_image(imgid);
}

void dt_dev_load_image(dt_develop_t *dev,
                       const dt_imgid_t imgid)
{
  dt_dev_load_image_ext(dev, imgid, -1);
}

void dt_dev_configure(dt_develop_t *dev,
                      int wd,
                      int ht)
{
  // fixed border on every side
  const int32_t tb = dev->border_size;
  wd -= 2*tb;
  ht -= 2*tb;
  if(dev->width != wd || dev->height != ht)
  {
    dev->width = wd;
    dev->height = ht;
    dev->preview_pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dev->preview2_pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dev->pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dt_dev_invalidate(dev);
  }
}

void dt_dev_second_window_configure(dt_develop_t *dev,
                                    int wd,
                                    int ht)
{
  // fixed border on every side
  int32_t tb = 0;
  if(dev->iso_12646.enabled)
  {
    const int bsize =
      dev->second_window.dpi * dt_conf_get_float("darkroom/ui/iso12464_border") / 2.54f;
    // we want to make some some stupid settings are avoided.
    tb = MIN(MAX(2, bsize), 0.3f * MIN(wd, ht));
  }

  wd -= 2*tb;
  ht -= 2*tb;

  if(dev->second_window.width != wd || dev->second_window.height != ht)
  {
    dev->second_window.width = wd;
    dev->second_window.height = ht;
    dev->second_window.border_size = tb;
    dev->preview2_pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dt_dev_invalidate(dev);
    dt_dev_reprocess_center(dev);
  }
}

// helper used to synch a single history item with db
static void _dev_write_history_item(const dt_imgid_t imgid,
                                    dt_dev_history_item_t *h,
                                    const int32_t num)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT num FROM main.history WHERE imgid = ?1 AND num = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT INTO main.history (imgid, num) VALUES (?1, ?2)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
    sqlite3_step(stmt);
  }

  sqlite3_finalize(stmt);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.history"
     " SET operation = ?1, op_params = ?2, module = ?3, enabled = ?4, "
     "     blendop_params = ?7, blendop_version = ?8, multi_priority = ?9,"
     "     multi_name = ?10, multi_name_hand_edited = ?11"
     " WHERE imgid = ?5 AND num = ?6",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, h->module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, h->params, h->module->params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, h->module->version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, h->enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, num);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, h->blend_params,
                             sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, dt_develop_blend_version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, h->multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, h->multi_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, h->multi_name_hand_edited);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // write masks (if any)
  for(GList *forms = h->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form)
      dt_masks_write_masks_history_item(imgid, num, form);
  }
}

static void _dev_auto_save(dt_develop_t *dev)
{
  // keep track of last saving time
  static double last = 0.0;

  const double user_delay = (double)dt_conf_get_int("autosave_interval");
  const double now = dt_get_wtime();

  const dt_imgid_t imgid = dev->image_storage.id;

  /* We can only autosave database & xmp while we have a valid image id
     and we are not currently loading or changing it in main darkroom
  */
  const gboolean saving = (user_delay >= 1.0)
                        && ((now - last) > user_delay)
                        && !dev->image_loading
                        && dev->requested_id == imgid
                        && dt_is_valid_imgid(imgid);

  if(saving)
  {
    // Ok, lets save status for image
    dt_dev_write_history(dev);
    dt_image_write_sidecar_file(imgid);
    last = now;

    const double spent = dt_get_wtime() - now;
    dt_print(DT_DEBUG_DEV, "autosave history took %fsec\n", spent);

    // if writing to database and the xmp took too long we disable
    // autosaving mode for this session
    if(spent > 0.5)
    {
      dev->autosaving = FALSE;
      dt_control_log(_("autosaving history has been disabled"
                       " for this session because of a slow drive used"));
    }
  }
}

static void _dev_auto_module_label(dt_develop_t *dev,
                                   dt_iop_module_t *module)
{
  // adjust the label to match presets if possible or otherwise the default
  // multi_name for this module.
  if(!dt_iop_is_hidden(module)
    && !module->multi_name_hand_edited
    && dt_conf_get_bool("darkroom/ui/auto_module_name_update"))
  {
    const gboolean is_default_params =
      memcmp(module->params, module->default_params, module->params_size) == 0;

    char *preset_name = dt_presets_get_module_label
      (module->op,
       module->params, module->params_size, is_default_params,
       module->blend_params, sizeof(dt_develop_blend_params_t));

    // if we have a preset-name, use it. otherwise set the label to the multi-priority
    // except for 0 where the multi-name is cleared.
    if(preset_name)
      snprintf(module->multi_name,
               sizeof(module->multi_name), "%s", preset_name);
    else if(module->multi_priority != 0)
      snprintf(module->multi_name,
               sizeof(module->multi_name), "%d", module->multi_priority);
    else
      g_strlcpy(module->multi_name, "", sizeof(module->multi_name));

    g_free(preset_name);

    if(dev->gui_attached)
      dt_iop_gui_update_header(module);
  }
}

static void _dev_add_history_item_ext(dt_develop_t *dev,
                                      dt_iop_module_t *module,
                                      const gboolean enable,
                                      const gboolean new_item,
                                      const gboolean no_image,
                                      const gboolean include_masks,
                                      const gboolean auto_name_module)
{
  // try to auto-name the module based on the presets if possible
  if(auto_name_module)
  {
    _dev_auto_module_label(dev, module);
  }

  int kept_module = 0;
  GList *history = g_list_nth(dev->history, dev->history_end);
  // look for leaks on top of history in two steps
  // first remove obsolete items above history_end
  // but keep the always-on modules
  while(history)
  {
    GList *next = g_list_next(history);
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    // printf("removing obsoleted history item: %s\n", hist->module->op);

    //check if an earlier instance of the module exists
    gboolean earlier_entry = FALSE;
    GList *prior_history = g_list_nth(dev->history, dev->history_end - 1);
    while(prior_history)
    {
      dt_dev_history_item_t *prior_hist = (dt_dev_history_item_t *)(prior_history->data);
      if(prior_hist->module->so == hist->module->so)
      {
        earlier_entry = TRUE;
        break;
      }
      prior_history = g_list_previous(prior_history);
    }

    if((!hist->module->hide_enable_button && !hist->module->default_enabled)
        || earlier_entry)
    {
      dt_dev_free_history_item(hist);
      dev->history = g_list_delete_link(dev->history, history);
    }
    else
      kept_module++;
    history = next;
  }
  // then remove NIL items there
  while((dev->history_end>0) && (! g_list_nth(dev->history, dev->history_end - 1)))
    dev->history_end--;

  dev->history_end += kept_module;

  history = g_list_nth(dev->history, dev->history_end - 1);
  dt_dev_history_item_t *hist = history ? (dt_dev_history_item_t *)(history->data) : 0;

  // if module should be enabled, do it now
  if(enable)
  {
    module->enabled = TRUE;
    if(!no_image)
    {
      if(module->off)
      {
        ++darktable.gui->reset;
        dt_iop_gui_set_enable_button(module);
        --darktable.gui->reset;
      }
    }
  }

  if(!history                                              // no history yet, push new item
     || new_item                                           // a new item is requested
     || module != hist->module
     || module->instance != hist->module->instance         // add new item for different op
     || module->multi_priority != hist->module->multi_priority // or instance
     || ((dev->focus_hash != hist->focus_hash)                 // or if focused out and in
         // but only add item if there is a difference at all for the same module
         && ((module->params_size != hist->module->params_size)
             || include_masks
             || (module->params_size == hist->module->params_size
                 && memcmp(hist->params, module->params, module->params_size)))))
  {
    // new operation, push new item
    dev->history_end++;

    hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));

    g_strlcpy(hist->op_name, module->op, sizeof(hist->op_name));
    hist->focus_hash = dev->focus_hash;
    hist->enabled = module->enabled;
    hist->module = module;
    hist->params = malloc(module->params_size);
    hist->iop_order = module->iop_order;
    hist->multi_priority = module->multi_priority;
    hist->multi_name_hand_edited = module->multi_name_hand_edited;
    g_strlcpy(hist->multi_name, module->multi_name, sizeof(hist->multi_name));
    /* allocate and set hist blend_params */
    hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));
    memcpy(hist->params, module->params, module->params_size);
    memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));
    if(include_masks)
      hist->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
    else
      hist->forms = NULL;

    dev->history = g_list_append(dev->history, hist);
    if(!no_image)
    {
      dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
      // topology remains, as modules are fixed for now:
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
      dev->preview2_pipe->changed |= DT_DEV_PIPE_SYNCH;
    }
  }
  else
  {
    // same operation, change params
    hist = (dt_dev_history_item_t *)history->data;
    memcpy(hist->params, module->params, module->params_size);

    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));

    hist->iop_order = module->iop_order;
    hist->multi_priority = module->multi_priority;
    hist->multi_name_hand_edited = module->multi_name_hand_edited;
    memcpy(hist->multi_name, module->multi_name, sizeof(module->multi_name));
    hist->enabled = module->enabled;

    if(include_masks)
    {
      g_list_free_full(hist->forms, (void (*)(void *))dt_masks_free_form);
      hist->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
    }
    if(!no_image)
    {
      dev->pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
      dev->preview_pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
      dev->preview2_pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
    }
  }
  if((module->enabled) && (!no_image))
    module->write_input_hint = TRUE;

  // possibly save database and sidecar file
  if(dev->autosaving)
    _dev_auto_save(dev);
}

const dt_dev_history_item_t *dt_dev_get_history_item(dt_develop_t *dev, const char *op)
{
  for(GList *l = g_list_last(dev->history); l; l = g_list_previous(l))
  {
    dt_dev_history_item_t *item = (dt_dev_history_item_t *)l->data;
    if(!g_strcmp0(item->op_name, op))
    {
      return item;
      break;
    }
  }

  return NULL;
}

void dt_dev_add_history_item_ext(dt_develop_t *dev,
                                 dt_iop_module_t *module,
                                 const gboolean enable,
                                 const int no_image)
{
  _dev_add_history_item_ext(dev, module, enable, FALSE, no_image, FALSE, TRUE);
}

static gboolean _dev_undo_start_record_target(dt_develop_t *dev, gpointer target)
{
  const double this_time = dt_get_wtime();
  const double merge_time =
    dev->gui_previous_time + dt_conf_get_float("darkroom/undo/merge_same_secs");
  const double review_time =
    dev->gui_previous_pipe_time + dt_conf_get_float("darkroom/undo/review_secs");
  dev->gui_previous_pipe_time = merge_time;
  if(target && target == dev->gui_previous_target
     && this_time < MIN(merge_time, review_time))
  {
    return FALSE;
  }

  dt_dev_undo_start_record(dev);

  dev->gui_previous_target = target;
  dev->gui_previous_time = this_time;

  return TRUE;
}

static void _dev_add_history_item(dt_develop_t *dev,
                                  dt_iop_module_t *module,
                                  const gboolean enable,
                                  const gboolean new_item,
                                  const gpointer target)
{
  if(!darktable.gui || darktable.gui->reset) return;

  // record current name, needed to ensure we do an undo record
  // if the module name is changed.

  gchar *saved_name = g_strdup(module->multi_name);

  _dev_auto_module_label(dev, module);

  const gboolean multi_name_changed = strcmp(saved_name, module->multi_name) != 0;

  const gboolean need_end_record =
    _dev_undo_start_record_target(dev, multi_name_changed ? NULL : target);

  g_free(saved_name);

  dt_pthread_mutex_lock(&dev->history_mutex);

  if(dev->gui_attached)
  {
    _dev_add_history_item_ext(dev, module, enable, new_item, FALSE, FALSE, FALSE);
  }

  /* attach changed tag reflecting actual change */
  const dt_imgid_t imgid = dev->image_storage.id;
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  const gboolean tag_change = dt_tag_attach(tagid, imgid, FALSE, FALSE);

  /* register export timestamp in cache */
  dt_image_cache_set_change_timestamp(darktable.image_cache, imgid);

  // invalidate buffers and force redraw of darkroom
  if(!dev->history_postpone_invalidate
     || module != dev->gui_module)
    dt_dev_invalidate_all(dev);

  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(need_end_record)
    dt_dev_undo_end_record(dev);

  if(dev->gui_attached)
  {
    /* signal that history has changed */
    if(tag_change)
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_add_history_item(dt_develop_t *dev,
                             dt_iop_module_t *module,
                             const gboolean enable)
{
  _dev_add_history_item(dev, module, enable, FALSE, NULL);
}

void dt_dev_add_history_item_target(dt_develop_t *dev,
                                    dt_iop_module_t *module,
                                    const gboolean enable,
                                    gpointer target)
{
  _dev_add_history_item(dev, module, enable, FALSE, target);
}

void dt_dev_add_new_history_item(dt_develop_t *dev,
                                 dt_iop_module_t *module,
                                 const gboolean enable)
{
  _dev_add_history_item(dev, module, enable, TRUE, NULL);
}

void dt_dev_add_masks_history_item_ext(dt_develop_t *dev,
                                       dt_iop_module_t *_module,
                                       const gboolean _enable,
                                       const gboolean no_image)
{
  dt_iop_module_t *module = _module;
  gboolean enable = _enable;

  // no module means that is called from the mask manager, so find the iop
  if(module == NULL)
  {
    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);
      if(dt_iop_module_is(mod->so, "mask_manager"))
      {
        module = mod;
        break;
      }
    }
    enable = FALSE;
  }
  if(module)
  {
    _dev_add_history_item_ext(dev, module, enable, FALSE, no_image, TRUE, TRUE);
  }
  else
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_dev_add_masks_history_item_ext] can't find mask manager module\n");
}

void dt_dev_add_masks_history_item(
        dt_develop_t *dev,
        dt_iop_module_t *module,
        const gboolean enable)
{
  gpointer target = NULL;

  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(form && gui)
  {
    dt_masks_point_group_t *fpt = g_list_nth_data(form->points, gui->group_edited);
    if(fpt) target = GINT_TO_POINTER(fpt->formid);
  }

  const gboolean need_end_record =
    _dev_undo_start_record_target(dev, target);

  dt_pthread_mutex_lock(&dev->history_mutex);

  if(dev->gui_attached)
  {
    dt_dev_add_masks_history_item_ext(dev, module, enable, FALSE);
  }

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(need_end_record)
    dt_dev_undo_end_record(dev);

  if(dev->gui_attached)
  {
    /* recreate mask list */
    dt_dev_masks_list_change(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_free_history_item(gpointer data)
{
  dt_dev_history_item_t *item = (dt_dev_history_item_t *)data;
  free(item->params);
  free(item->blend_params);
  g_list_free_full(item->forms, (void (*)(void *))dt_masks_free_form);
  free(item);
}

void dt_dev_reload_history_items(dt_develop_t *dev)
{
  dev->focus_hash = 0;

  dt_lock_image(dev->image_storage.id);

  dt_ioppr_set_default_iop_order(dev, dev->image_storage.id);
  dt_dev_pop_history_items(dev, 0);

  // remove unused history items:
  GList *history = g_list_nth(dev->history, dev->history_end);
  while(history)
  {
    GList *next = g_list_next(history);
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    hist->module->multi_name_hand_edited = FALSE;
    g_strlcpy(hist->module->multi_name, "", sizeof(hist->module->multi_name));
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, history);
    history = next;
  }
  dt_dev_read_history(dev);

  // we have to add new module instances first
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module) && !module->expander)
      {
        dt_iop_gui_init(module);

        /* add module to right panel */
        dt_iop_gui_set_expander(module);
        dt_iop_gui_set_expanded(module, TRUE, FALSE);

        dt_iop_reload_defaults(module);
        dt_iop_gui_update_blending(module);

        // the pipe need to be reconstruct
        dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
        dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
        dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
      }
    }
    else if(!dt_iop_is_hidden(module) && module->expander)
    {
      // we have to ensure that the name of the widget is correct
      dt_iop_gui_update_header(module);
    }
  }

  dt_dev_pop_history_items(dev, dev->history_end);

  dt_ioppr_resync_iop_list(dev);

  // set the module list order
  dt_dev_reorder_gui_module_list(dev);

  dt_unlock_image(dev->image_storage.id);
}

void dt_dev_pop_history_items_ext(dt_develop_t *dev, const int32_t cnt)
{
  if(darktable.unmuted & DT_DEBUG_IOPORDER)
    dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items_ext begin");

  const int end_prev = dev->history_end;
  dev->history_end = cnt;

  // reset gui params for all modules
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    memcpy(module->params, module->default_params, module->params_size);
    dt_iop_commit_blend_params(module, module->default_blendop_params);
    module->enabled = module->default_enabled;

    if(module->multi_priority == 0)
      module->iop_order =
        dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
    else
    {
      module->iop_order = INT_MAX;
    }
  }

  // go through history and set gui params
  GList *forms = NULL;
  GList *history = dev->history;
  for(int i = 0; i < cnt && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    if(hist->module->params_size == 0)
      memcpy(hist->module->params, hist->module->default_params, hist->module->params_size);
    else
      memcpy(hist->module->params, hist->params, hist->module->params_size);
    dt_iop_commit_blend_params(hist->module, hist->blend_params);

    hist->module->iop_order = hist->iop_order;
    hist->module->enabled = hist->enabled;
    g_strlcpy(hist->module->multi_name, hist->multi_name, sizeof(hist->module->multi_name));
    if(hist->forms) forms = hist->forms;
    hist->module->multi_name_hand_edited = hist->multi_name_hand_edited;

    history = g_list_next(history);
  }

  dt_ioppr_resync_modules_order(dev);

  dt_ioppr_check_duplicate_iop_order(&dev->iop, dev->history);

  if(darktable.unmuted & DT_DEBUG_IOPORDER)
    dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items_ext end");

  // check if masks have changed
  gboolean masks_changed = FALSE;
  if(cnt < end_prev)
    history = g_list_nth(dev->history, cnt);
  else if(cnt > end_prev)
    history = g_list_nth(dev->history, end_prev);
  else
    history = NULL;
  for(int i = MIN(cnt, end_prev); i < MAX(cnt, end_prev) && history && !masks_changed; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->forms != NULL)
      masks_changed = TRUE;

    history = g_list_next(history);
  }
  if(masks_changed)
    dt_masks_replace_current_forms(dev, forms);
}

void dt_dev_pop_history_items(dt_develop_t *dev, const int32_t cnt)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  ++darktable.gui->reset;
  GList *dev_iop = g_list_copy(dev->iop);

  dt_dev_pop_history_items_ext(dev, cnt);

  darktable.develop->history_updating = TRUE;

  // update all gui modules
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_iop_gui_update(module);
    modules = g_list_next(modules);
  }

  darktable.develop->history_updating = FALSE;

  // check if the order of modules has changed
  gboolean dev_iop_changed = (g_list_length(dev_iop) != g_list_length(dev->iop));
  if(!dev_iop_changed)
  {
    modules = dev->iop;
    GList *modules_old = dev_iop;
    while(modules && modules_old)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      dt_iop_module_t *module_old = (dt_iop_module_t *)(modules_old->data);

      if(module->iop_order != module_old->iop_order)
      {
        dev_iop_changed = TRUE;
        break;
      }

      modules = g_list_next(modules);
      modules_old = g_list_next(modules_old);
    }
  }
  g_list_free(dev_iop);

  if(!dev_iop_changed)
  {
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dev->preview2_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
  }
  else
  {
    dt_dev_pixelpipe_rebuild(dev);
  }

  --darktable.gui->reset;
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_dev_masks_list_change(dev);

  dt_control_queue_redraw_center();
}

static void _cleanup_history(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_dev_write_history_ext(dt_develop_t *dev,
                              const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;
  dt_lock_image(imgid);

  _cleanup_history(imgid);

  // write history entries

  GList *history = dev->history;
  dt_print(DT_DEBUG_IOPORDER,
           "[dt_dev_write_history_ext] Writing history image id=%d `%s', iop version: %i\n",
           imgid, dev->image_storage.filename, dev->iop_order_version);
  for(int i = 0; history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    _dev_write_history_item(imgid, hist, i);

    dt_print(DT_DEBUG_IOPORDER, "%20s, num %2i, order %2d, v(%i), multiprio %i%s\n",
      hist->module->op, i, hist->iop_order, hist->module->version(), hist->multi_priority,
      (hist->enabled) ? ", enabled" : "");

    history = g_list_next(history);
  }

  // update history end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = ?1 WHERE id = ?2", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->history_end);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // write the current iop-order-list for this image

  dt_ioppr_write_iop_order_list(dev->iop_order_list, imgid);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_unlock_image(imgid);
}

void dt_dev_write_history(dt_develop_t *dev)
{
  dt_database_start_transaction(darktable.db);
  dt_dev_write_history_ext(dev, dev->image_storage.id);
  dt_database_release_transaction(darktable.db);
}

static int _dev_get_module_nb_records(void)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count (*) FROM  memory.history",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  const int cnt = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return cnt;
}

static void _dev_insert_module(dt_develop_t *dev,
                               dt_iop_module_t *module,
                               const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  // we make sure that the multi-name is updated if possible with the
  // actual preset name if any is defined for the default parameters.

  char *preset_name = dt_presets_get_module_label
    (module->op,
     module->default_params, module->params_size, TRUE,
     module->blend_params, sizeof(dt_develop_blend_params_t));

  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "INSERT INTO memory.history VALUES (?1, 0, ?2, ?3, ?4, 1, NULL, 0, 0, ?5, 0)",
    -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, module->default_params, module->params_size,
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, preset_name ? preset_name : "", -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(preset_name);

  dt_print(DT_DEBUG_PARAMS, "[dev_insert_module] `%s' inserted to history\n", module->op);
}

static gboolean _dev_auto_apply_presets(dt_develop_t *dev)
{
  // NOTE: the presets/default iops will be *prepended* into the history.

  const dt_imgid_t imgid = dev->image_storage.id;

  if(!dt_is_valid_imgid(imgid)) return FALSE;

  gboolean run = FALSE;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!(image->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)) run = TRUE;

  const gboolean is_raw = dt_image_is_raw(image);
  const gboolean is_modern_chroma = dt_is_scene_referred();

  // flag was already set? only apply presets once in the lifetime of
  // a history stack.  (the flag will be cleared when removing it).
  if(!run || !dt_is_valid_imgid(image->id))
  {
    // Next section is to recover old edits where all modules with
    // default parameters were not recorded in the db nor in the .XMP.
    //
    // One crucial point is the white-balance which has automatic
    // default based on the camera and depends on the
    // chroma-adaptation. In modern mode the default won't be the same
    // used in legacy mode and if the white-balance is not found on
    // the history one will be added by default using current
    // defaults. But if we are in modern chromatic adaptation the
    // default will not be equivalent to the one used to develop this
    // old edit.

    // So if the current mode is the modern chromatic-adaptation, do check the history.

    if(is_modern_chroma && is_raw)
    {
      // loop over all modules and display a message for default-enabled modules that
      // are not found on the history.

      for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
      {
        dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

        if(module->default_enabled
           && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
           && !dt_history_check_module_exists(imgid, module->op, FALSE))
        {
          dt_print(DT_DEBUG_PARAMS,
                   "[_dev_auto_apply_presets] missing mandatory module %s"
                   " for image id=%d `%s'\n",
                   module->op, imgid, dev->image_storage.filename);

          // If the module is white-balance and we are dealing with a
          // raw file we need to add one now with the default legacy
          // parameters. And we want to do this only for old edits.
          //
          // For new edits the temperature will be added back
          // depending on the chromatic adaptation the standard way.

          if(dt_iop_module_is(module->so, "temperature")
             && (image->change_timestamp == -1))
          {
            // it is important to recover temperature in this case
            // (modern chroma and not module present as we need to
            // have the pre 3.0 default parameters used.
            const gchar *current_workflow =
              dt_conf_get_string_const("plugins/darkroom/workflow");

            dt_conf_set_string("plugins/darkroom/workflow", "display-referred (legacy)");
            dt_iop_reload_defaults(module);
            _dev_insert_module(dev, module, imgid);
            dt_conf_set_string("plugins/darkroom/workflow", current_workflow);
            dt_iop_reload_defaults(module);
          }
        }
      }
    }

    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    return FALSE;
  }

  //  get current workflow and image characteristics

  const gboolean is_scene_referred = dt_is_scene_referred();
  const gboolean is_display_referred = dt_is_display_referred();
  const gboolean is_workflow_none = !is_scene_referred && !is_display_referred;
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);

  //  set filters

  int iformat = 0;
  if(dt_image_is_raw(image))
    iformat |= FOR_RAW;
  else
    iformat |= FOR_LDR;

  if(has_matrix)
    iformat |= FOR_MATRIX;

  if(dt_image_is_hdr(image))
    iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image))
    excluded |= FOR_NOT_MONO;
  else
    excluded |= FOR_NOT_COLOR;

  // select all presets from one of the following table and add them
  // into memory.history. Note that this is appended to possibly
  // already present default modules.
  //
  // Also it may be possible that multiple presets for a module not
  // supporting multiple instances (e.g. demosaic) may be added. Those
  // instances are properly merged in dt_dev_read_history_ext.

  const char *preset_table[2] = { "data.presets", "main.legacy_presets" };
  const int legacy = (image->flags & DT_IMAGE_NO_LEGACY_PRESETS) ? 0 : 1;
  char query[2048];
  // clang-format off

  const gboolean auto_module = dt_conf_get_bool("darkroom/ui/auto_module_name_update");

  snprintf(query, sizeof(query),
           "INSERT OR REPLACE INTO memory.history"
           " SELECT ?1, 0, op_version, operation AS op, op_params,"
           "       enabled, blendop_params, blendop_version,"
           "       ROW_NUMBER() OVER (PARTITION BY operation ORDER BY operation) - 1,"
           "       %s, multi_name_hand_edited"
           " FROM %s"
           // only auto-applied presets matching the camera/lens/focal/format/exposure
           " WHERE ( (autoapply=1"
           "          AND ((?2 LIKE model AND ?3 LIKE maker)"
           "               OR (?4 LIKE model AND ?5 LIKE maker))"
           "          AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "          AND ?8 BETWEEN exposure_min AND exposure_max"
           "          AND ?9 BETWEEN aperture_min AND aperture_max"
           "          AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "          AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))))"
           // skip non iop modules:
           "   AND operation NOT IN"
           "       ('ioporder', 'metadata', 'modulegroups', 'export',"
           "        'tagging', 'collect', '%s')"
           // select all user's auto presets or the hard-coded presets (for the workflow)
           // if non auto-presets for the same operation and matching
           // camera/lens/focal/format/exposure found.
           "   AND (writeprotect = 0"
           "        OR (SELECT NOT EXISTS"
           "             (SELECT op"
           "              FROM presets"
           "              WHERE autoapply = 1 AND operation = op AND writeprotect = 0"
           "                    AND ((?2 LIKE model AND ?3 LIKE maker)"
           "                         OR (?4 LIKE model AND ?5 LIKE maker))"
           "                    AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "                    AND ?8 BETWEEN exposure_min AND exposure_max"
           "                    AND ?9 BETWEEN aperture_min AND aperture_max"
           "                    AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "                    AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0)))))"
           " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
           // auto module:
           //  ON  : we take as the preset label either the multi-name
           //        if defined or the preset name.
           //  OFF : we take the multi-name only if hand-edited otherwise a
           //        simple incremental instance number (equivalent to the multi_priority
           //        field is used).
           auto_module
             ? "COALESCE(NULLIF(multi_name,''), NULLIF(name,''))"
             : "CASE WHEN multi_name_hand_edited"
               "  THEN multi_name"
               "  ELSE (ROW_NUMBER() OVER (PARTITION BY operation ORDER BY operation) - 1)"
               " END",
           preset_table[legacy],
           is_display_referred ? "" : "basecurve");
  // clang-format on

  // query for all modules at once:
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // now we want to auto-apply the iop-order list if one corresponds and none are
  // still applied. Note that we can already have an iop-order list set when
  // copying an history or applying a style to a not yet developed image.

  if(!dt_ioppr_has_iop_order_list(imgid))
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT op_params"
       " FROM data.presets"
       " WHERE autoapply=1"
       "       AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
       "       AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
       "       AND ?8 BETWEEN exposure_min AND exposure_max"
       "       AND ?9 BETWEEN aperture_min AND aperture_max"
       "       AND ?10 BETWEEN focal_length_min AND focal_length_max"
       "       AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))"
       "       AND operation = 'ioporder'"
       " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
       -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f,
                                                fminf(1000000, image->exif_exposure)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f,
                                                fminf(1000000, image->exif_aperture)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f,
                                                 fminf(1000000, image->exif_focal_length)));
    // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);

    GList *iop_list = NULL;

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const char *params = (char *)sqlite3_column_blob(stmt, 0);
      const int32_t params_len = sqlite3_column_bytes(stmt, 0);
      iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
    }
    else
    {
      // we have no auto-apply order, so apply iop order, depending of the workflow
      if(is_scene_referred || is_workflow_none)
        iop_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
      else
        iop_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
    }

    // add multi-instance entries that could have been added if more
    // than one auto-applied preset was found for a single iop.

    GList *mi_list = dt_ioppr_get_multiple_instances_iop_order_list(imgid, TRUE);
    GList *final_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi_list);

    dt_ioppr_write_iop_order_list(final_list, imgid);
    g_list_free_full(mi_list, free);
    g_list_free_full(final_list, free);
    dt_ioppr_set_default_iop_order(dev, imgid);

    sqlite3_finalize(stmt);
  }

  image->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS;

  // make sure these end up in the image_cache; as the history is not correct right now
  // we don't write the sidecar here but later in dt_dev_read_history_ext
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);

  return TRUE;
}

static void _dev_add_default_modules(dt_develop_t *dev,
                                     const dt_imgid_t imgid)
{
  //start with those modules that cannot be disabled
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

    if(!dt_history_check_module_exists(imgid, module->op, FALSE)
       && module->default_enabled
       && module->hide_enable_button
       && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK))
    {
      _dev_insert_module(dev, module, imgid);
    }
  }
  //now modules that can be disabled but are auto-on
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

    if(!dt_history_check_module_exists(imgid, module->op, FALSE)
       && module->default_enabled
       && !module->hide_enable_button
       && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK))
    {
      _dev_insert_module(dev, module, imgid);
    }
  }
}

static void _dev_merge_history(dt_develop_t *dev,
                               const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  // count what we found:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM memory.history", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // if there is anything..
    const int cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // workaround a sqlite3 "feature". The above statement to insert
    // items into memory.history is complex and in this case sqlite
    // does not give rowid a linear increment. But the following code
    // really expect that the rowid in this table starts from 0 and
    // increment one by one. So in the following code we rewrite the
    // "num" values from 0 to cnt-1.

    if(cnt > 0)
    {
      // get all rowids
      GList *rowids = NULL;

      // get the rowids in descending order since building the list will reverse the order
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT rowid FROM memory.history ORDER BY rowid DESC",
                                  -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
        rowids = g_list_prepend(rowids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      sqlite3_finalize(stmt);

      // update num accordingly
      int v = 0;

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE memory.history SET num=?1 WHERE rowid=?2",
                                  -1, &stmt, NULL);

      // let's wrap this into a transaction, it might make it a little faster.
      dt_database_start_transaction(darktable.db);

      for(GList *r = rowids; r; r = g_list_next(r))
      {
        DT_DEBUG_SQLITE3_CLEAR_BINDINGS(stmt);
        DT_DEBUG_SQLITE3_RESET(stmt);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, v);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, GPOINTER_TO_INT(r->data));

        if(sqlite3_step(stmt) != SQLITE_DONE) break;

        v++;
      }

      dt_database_release_transaction(darktable.db);

      g_list_free(rowids);

      // advance the current history by cnt amount, that is, make space
      // for the preset/default iops that will be *prepended* into the
      // history.
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.history SET num=num+?1 WHERE imgid=?2",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

      if(sqlite3_step(stmt) == SQLITE_DONE)
      {
        sqlite3_finalize(stmt);
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "UPDATE main.images"
                                    " SET history_end=history_end+?1"
                                    " WHERE id=?2",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

        if(sqlite3_step(stmt) == SQLITE_DONE)
        {
          // and finally prepend the rest with increasing numbers (starting at 0)
          sqlite3_finalize(stmt);
          // clang-format off
          DT_DEBUG_SQLITE3_PREPARE_V2(
            dt_database_get(darktable.db),
            "INSERT INTO main.history"
            " SELECT imgid, num, module, operation, op_params, enabled, "
            "        blendop_params, blendop_version, multi_priority,"
            "        multi_name, multi_name_hand_edited"
            " FROM memory.history",
            -1, &stmt, NULL);
          // clang-format on
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
      }
    }
  }
}

static void _dev_write_history(dt_develop_t *dev,
                               const dt_imgid_t imgid)
{
  _cleanup_history(imgid);
  // write history entries
  GList *history = dev->history;
  for(int i = 0; history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    _dev_write_history_item(imgid, hist, i);
    history = g_list_next(history);
  }
}

// helper function for debug strings
char *_print_validity(const gboolean state)
{
  if(state)
    return "ok";
  else
    return "WRONG";
}

void dt_dev_read_history_ext(dt_develop_t *dev,
                             const dt_imgid_t imgid,
                             const gboolean no_image,
                             const int32_t snapshot_id)
{
  if(!dt_is_valid_imgid(imgid)) return;
  if(!dev->iop) return;

  dt_lock_image(imgid);

  dt_dev_undo_start_record(dev);

  int auto_apply_modules_count = 0;
  gboolean first_run = FALSE;
  gboolean legacy_params = FALSE;

  dt_ioppr_set_default_iop_order(dev, imgid);

  if(!no_image)
  {
    // cleanup
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                          "DELETE FROM memory.history", NULL, NULL, NULL);

    dt_print(DT_DEBUG_PARAMS, "[dt_dev_read_history_ext] temporary history deleted\n");

    // Make sure all modules default params are loaded to init
    // history. This is important as some modules have specific
    // defaults based on metadata.

    _dt_dev_load_pipeline_defaults(dev);

    // Prepend all default modules to memory.history

    _dev_add_default_modules(dev, imgid);
    const int default_modules_count = _dev_get_module_nb_records();

    // Maybe add auto-presets to memory.history
    first_run = _dev_auto_apply_presets(dev);
    auto_apply_modules_count = _dev_get_module_nb_records() - default_modules_count;

    dt_print(DT_DEBUG_PARAMS,
             "[dt_dev_read_history_ext] temporary history initialised with"
             " default params and presets\n");

    // Now merge memory.history into main.history

    _dev_merge_history(dev, imgid);

    dt_print(DT_DEBUG_PARAMS,
             "[dt_dev_read_history_ext] temporary history merged with image history\n");

    //  first time we are loading the image, try to import lightroom .xmp if any
    if(dev->image_loading && first_run)
      dt_lightroom_import(dev->image_storage.id, dev, TRUE);

    // if a snapshot move all auto-presets into the history_snapshot table

    if(snapshot_id != -1)
    {
      DT_DEBUG_SQLITE3_EXEC
        (dt_database_get(darktable.db),
         "INSERT INTO memory.history_snapshot SELECT * FROM memory.history",
         NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC
        (dt_database_get(darktable.db),
         "DELETE FROM memory.history", NULL, NULL, NULL);
    }
  }

  sqlite3_stmt *stmt;

  // Get the end of the history - What's that ???

  int history_end_current = 0;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW) // seriously, this should never fail
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
      history_end_current = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // Load current image history from DB
  // clang-format off
  if(snapshot_id == -1)
  {
    // not a snapshot, read from main history
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT imgid, num, module, operation,"
                                "       op_params, enabled, blendop_params,"
                                "       blendop_version, multi_priority, multi_name,"
                                "       multi_name_hand_edited"
                                " FROM main.history"
                                " WHERE imgid = ?1"
                                " ORDER BY num",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  }
  else
  {
    // a snapshot, read from history_snapshot
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT id, num, module, operation,"
                                "       op_params, enabled, blendop_params,"
                                "       blendop_version, multi_priority, multi_name,"
                                "       multi_name_hand_edited"
                                " FROM memory.history_snapshot"
                                " WHERE id = ?1"
                                " ORDER BY num",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snapshot_id);
  }
  // clang-format on

  dev->history_end = 0;

  // Specific handling for None workflow (interdependency)

  const gboolean is_workflow_none = dt_conf_is_equal("plugins/darkroom/workflow", "none");

  dt_iop_module_t *channelmixerrgb = NULL;
  dt_iop_module_t *temperature = NULL;

  // Strip rows from DB lookup. One row == One module in history
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // Unpack the DB blobs
    const int id = sqlite3_column_int(stmt, 0);
    const int num = sqlite3_column_int(stmt, 1);
    const int modversion = sqlite3_column_int(stmt, 2);
    const char *module_name = (const char *)sqlite3_column_text(stmt, 3);
    const void *module_params = sqlite3_column_blob(stmt, 4);
    const int enabled = sqlite3_column_int(stmt, 5);
    const void *blendop_params = sqlite3_column_blob(stmt, 6);
    const int blendop_version = sqlite3_column_int(stmt, 7);
    const int multi_priority = sqlite3_column_int(stmt, 8);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);
    const int multi_name_hand_edited = sqlite3_column_int(stmt, 10);

    const int param_length = sqlite3_column_bytes(stmt, 4);
    const int bl_length = sqlite3_column_bytes(stmt, 6);

    // Sanity checks
    const gboolean is_valid_id = (id == imgid || (snapshot_id != -1));
    const gboolean has_module_name = (module_name != NULL);

    if(!(has_module_name && is_valid_id))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dev_read_history_ext] database history for image"
               " id=%d `%s' seems to be corrupted!\n",
               imgid, dev->image_storage.filename);
      continue;
    }

    const int iop_order =
      dt_ioppr_get_iop_order(dev->iop_order_list, module_name, multi_priority);

    dt_dev_history_item_t *hist =
      (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
    hist->module = NULL;

    // Find a .so file that matches our history entry, aka a module to
    // run the params stored in DB
    dt_iop_module_t *find_op = NULL;

    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      if(dt_iop_module_is(module->so, module_name))
      {
        // make sure that module not supporting multiple instances are
        // selected here.
        if(module->multi_priority == multi_priority
           || (module->flags() & IOP_FLAGS_ONE_INSTANCE))
        {
          hist->module = module;
          if(multi_name)
            g_strlcpy(module->multi_name, multi_name, sizeof(module->multi_name));
          else
            memset(module->multi_name, 0, sizeof(module->multi_name));
          module->multi_name_hand_edited = multi_name_hand_edited;
          break;
        }
        else if(multi_priority > 0)
        {
          // we just say that we find the name, so we just have to add
          // new instance of this module
          find_op = module;
        }
      }
    }
    if(!hist->module && find_op)
    {
      // we have to add a new instance of this module and set index to modindex
      dt_iop_module_t *new_module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(!dt_iop_load_module(new_module, find_op->so, dev))
      {
        dt_iop_update_multi_priority(new_module, multi_priority);
        new_module->iop_order = iop_order;

        g_strlcpy(new_module->multi_name, multi_name, sizeof(new_module->multi_name));
        new_module->multi_name_hand_edited = multi_name_hand_edited;

        dev->iop = g_list_append(dev->iop, new_module);

        new_module->instance = find_op->instance;
        hist->module = new_module;
      }
    }

    if(!hist->module)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dev_read_history] the module `%s' requested by image"
               " id=%d `%s' is not installed on this computer!\n",
               module_name, imgid, dev->image_storage.filename);
      free(hist);
      continue;
    }

    if(is_workflow_none && hist->module->enabled)
    {
      if(dt_iop_module_is(hist->module->so, "temperature"))
        temperature = hist->module;
      if(dt_iop_module_is(hist->module->so, "channelmixerrgb"))
        channelmixerrgb = hist->module;
    }

    // module has no user params and won't bother us in GUI - exit early, we are done
    if(hist->module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
    {
      free(hist);
      continue;
    }

    // Run a battery of tests
    const gboolean is_valid_module_name =
      (strcmp(module_name, hist->module->op) == 0);
    const gboolean is_valid_blendop_version =
      (blendop_version == dt_develop_blend_version());
    const gboolean is_valid_blendop_size =
      (bl_length == sizeof(dt_develop_blend_params_t));
    const gboolean is_valid_module_version =
      (modversion == hist->module->version());
    const gboolean is_valid_params_size =
      (param_length == hist->module->params_size);

    dt_print(DT_DEBUG_PARAMS,
             "[history] successfully loaded module %s from history\n"
             "\t\t\tblendop v. %i:\tversion %s\tparams %s\n"
             "\t\t\tparams v. %i:\tversion %s\tparams %s\n",
             module_name,
             blendop_version, _print_validity(is_valid_blendop_version),
             _print_validity(is_valid_blendop_size),
             modversion, _print_validity(is_valid_module_version),
             _print_validity(is_valid_params_size));

    // Init buffers and values
    hist->enabled = enabled;
    hist->num = num;
    hist->iop_order = iop_order;
    hist->multi_priority = (hist->module->flags() & IOP_FLAGS_ONE_INSTANCE)
      ? 0
      : multi_priority;

    hist->multi_name_hand_edited = multi_name_hand_edited;
    g_strlcpy(hist->op_name, hist->module->op, sizeof(hist->op_name));
    g_strlcpy(hist->multi_name, multi_name, sizeof(hist->multi_name));
    hist->params = malloc(hist->module->params_size);
    hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));

    // update module iop_order only on active history entries
    if(history_end_current > dev->history_end)
      hist->module->iop_order = hist->iop_order;

    // Copy blending params if valid, else try to convert legacy params
    if(blendop_params
       && is_valid_blendop_version
       && is_valid_blendop_size)
    {
      memcpy(hist->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params
            (hist->module, blendop_params, blendop_version,
             hist->blend_params, dt_develop_blend_version(), bl_length) == 0)
    {
      legacy_params = TRUE;
    }
    else
    {
      memcpy(hist->blend_params, hist->module->default_blendop_params,
             sizeof(dt_develop_blend_params_t));
    }

    // Copy module params if valid, else try to convert legacy params
    if(param_length == 0)
    {
      // special case of auto-init presets being loaded in history
      memcpy(hist->params, hist->module->default_params, hist->module->params_size);
    }
    else if(is_valid_module_version && is_valid_params_size && is_valid_module_name)
    {
      memcpy(hist->params, module_params, hist->module->params_size);
    }
    else
    {
      if(dt_iop_legacy_params
         (hist->module,
          module_params, param_length, modversion,
          &hist->params, hist->module->version()) == 1)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[dev_read_history] module `%s' version mismatch: history"
                 " is %d, darktable is %d, image id=%d `%s'\n",
                 hist->module->op, modversion, hist->module->version(),
                 imgid, dev->image_storage.filename);

        const char *fname =
          dev->image_storage.filename + strlen(dev->image_storage.filename);
        while(fname > dev->image_storage.filename && *fname != '/') fname--;

        if(fname > dev->image_storage.filename) fname++;
        dt_control_log(_("%s: module `%s' version mismatch: %d != %d"),
                       fname, hist->module->op,
                       hist->module->version(), modversion);
        dt_dev_free_history_item(hist);
        continue;
      }
      else
      {
        if(dt_iop_module_is(hist->module->so, "spots")
           && modversion == 1)
        {
          // quick and dirty hack to handle spot removal legacy_params
          memcpy(hist->blend_params, hist->module->blend_params,
                 sizeof(dt_develop_blend_params_t));
        }
        legacy_params = TRUE;
      }

      /*
       * Fix for flip iop: previously it was not always needed, but it might be
       * in history stack as "orientation (off)", but now we always want it
       * by default, so if it is disabled, enable it, and replace params with
       * default_params. if user want to, he can disable it.
       */
      if(dt_iop_module_is(hist->module->so, "flip")
         && !hist->enabled
         && labs(modversion) == 1)
      {
        memcpy(hist->params, hist->module->default_params, hist->module->params_size);
        hist->enabled = TRUE;
      }
    }

    // make sure that always-on modules are always on. duh.
    if(hist->module->default_enabled && hist->module->hide_enable_button)
      hist->enabled = TRUE;

    dev->history = g_list_append(dev->history, hist);
    dev->history_end++;
  }
  sqlite3_finalize(stmt);

  // Both modules are actives and found on the history stack, let's
  // again reload the defaults to ensure the whiteblance is properly
  // set depending on the CAT handling.
  if(temperature && channelmixerrgb)
  {
    dt_print(DT_DEBUG_PARAMS,
             "[dt_dev_read_history_ext] reset defaults for workflow none\n");
    temperature->reload_defaults(temperature);
  }

  dt_ioppr_resync_modules_order(dev);

  if(snapshot_id == -1)
  {
    // find the new history end
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT history_end FROM main.images WHERE id = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW) // seriously, this should never fail
      if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        dev->history_end = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  if(darktable.unmuted & DT_DEBUG_IOPORDER)
    dt_ioppr_check_iop_order(dev, imgid, "dt_dev_read_history_no_image end");

  dt_masks_read_masks_history(dev, imgid);

  // FIXME : this probably needs to capture dev thread lock
  if(dev->gui_attached && !no_image)
  {
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dev->preview2_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dt_dev_invalidate_all(dev);

    /* signal history changed */
    dt_dev_undo_end_record(dev);
  }
  dt_dev_masks_list_change(dev);

  // make sure module_dev is in sync with history
  if(snapshot_id != -1) goto end_rh;

  _dev_write_history(dev, imgid);
  dt_ioppr_write_iop_order_list(dev->iop_order_list, imgid);

  dt_history_hash_t flags = DT_HISTORY_HASH_CURRENT;
  if(first_run)
  {
    const dt_history_hash_t hash_status = dt_history_hash_get_status(imgid);
    // if altered doesn't mask it
    if(!(hash_status & DT_HISTORY_HASH_CURRENT))
    {
      flags = flags
        | (auto_apply_modules_count
           ? DT_HISTORY_HASH_AUTO
           : DT_HISTORY_HASH_BASIC);
    }
    dt_history_hash_write_from_history(imgid, flags);
    // As we have a proper history right now and this is first_run we
    // possibly write the xmp now
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    // depending on the xmp_writing mode we either us safe or relaxed
    const gboolean always = (dt_image_get_xmp_mode() == DT_WRITE_XMP_ALWAYS);
    dt_image_cache_write_release
      (darktable.image_cache,
       image,
       always ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED);
  }
  else if(legacy_params)
  {
    const dt_history_hash_t hash_status = dt_history_hash_get_status(imgid);
    if(hash_status & (DT_HISTORY_HASH_BASIC | DT_HISTORY_HASH_AUTO))
    {
      // if image not altered keep the current status
      flags = flags | hash_status;
    }
    dt_history_hash_write_from_history(imgid, flags);
  }
  else
  {
    dt_history_hash_write_from_history(imgid, flags);
  }

  end_rh:
  dt_unlock_image(imgid);
}

void dt_dev_read_history(dt_develop_t *dev)
{
  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE, -1);
}

void dt_dev_reprocess_all(dt_develop_t *dev)
{
  if(darktable.gui->reset) return;
  if(dev && dev->gui_attached)
  {
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview2_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->pipe->cache_obsolete = TRUE;
    dev->preview_pipe->cache_obsolete = TRUE;
    dev->preview2_pipe->cache_obsolete = TRUE;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_reprocess_center(dt_develop_t *dev)
{
  if(darktable.gui->reset) return;
  if(dev && dev->gui_attached)
  {
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->pipe->cache_obsolete = TRUE;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_reprocess_preview(dt_develop_t *dev)
{
  if(darktable.gui->reset || !dev || !dev->gui_attached)
    return;

  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview_pipe->cache_obsolete = TRUE;

  dt_dev_invalidate_preview(dev);
  dt_control_queue_redraw_center();
}

void dt_dev_check_zoom_bounds(dt_develop_t *dev,
                              float *zoom_x,
                              float *zoom_y,
                              const dt_dev_zoom_t zoom,
                              const int closeup,
                              float *boxww,
                              float *boxhh)
{
  int procw = 0, proch = 0;
  dt_dev_get_processed_size(dev, &procw, &proch);
  float boxw = 1.0f, boxh = 1.0f; // viewport in normalised space
                            //   if(zoom == DT_ZOOM_1)
                            //   {
                            //     const float imgw = (closeup ? 2 : 1)*procw;
                            //     const float imgh = (closeup ? 2 : 1)*proch;
                            //     const float devw = MIN(imgw, dev->width);
                            //     const float devh = MIN(imgh, dev->height);
                            //     boxw = fminf(1.0, devw/imgw);
                            //     boxh = fminf(1.0, devh/imgh);
                            //   }
  if(zoom == DT_ZOOM_FIT)
  {
    *zoom_x = *zoom_y = 0.0f;
    boxw = boxh = 1.0f;
  }
  else
  {
    const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    const float imgw = procw;
    const float imgh = proch;
    const float devw = dev->width;
    const float devh = dev->height;
    boxw = devw / (imgw * scale);
    boxh = devh / (imgh * scale);
  }

  if(*zoom_x < boxw / 2 - .5) *zoom_x = boxw / 2 - .5;
  if(*zoom_x > .5 - boxw / 2) *zoom_x = .5 - boxw / 2;
  if(*zoom_y < boxh / 2 - .5) *zoom_y = boxh / 2 - .5;
  if(*zoom_y > .5 - boxh / 2) *zoom_y = .5 - boxh / 2;
  if(boxw > 1.0) *zoom_x = 0.0f;
  if(boxh > 1.0) *zoom_y = 0.0f;

  if(boxww) *boxww = boxw;
  if(boxhh) *boxhh = boxh;
}

void dt_dev_get_processed_size(const dt_develop_t *dev,
                               int *procw,
                               int *proch)
{
  if(!dev) return;

  // if pipe is processed, lets return its size
  if(dev->pipe && dev->pipe->processed_width)
  {
    *procw = dev->pipe->processed_width;
    *proch = dev->pipe->processed_height;
    return;
  }

  // fallback on preview pipe
  if(dev->preview_pipe && dev->preview_pipe->processed_width)
  {
    const float scale = (dev->preview_pipe->iscale / dev->preview_downsampling);
    *procw = scale * dev->preview_pipe->processed_width;
    *proch = scale * dev->preview_pipe->processed_height;
    return;
  }

  // no processed pipes, lets return 0 size
  *procw = *proch = 0;
  return;
}

void dt_dev_get_pointer_zoom_pos(dt_develop_t *dev,
                                 const float px,
                                 const float py,
                                 float *zoom_x,
                                 float *zoom_y)
{
  dt_dev_zoom_t zoom;
  int closeup = 0, procw = 0, proch = 0;
  float zoom2_x = 0.0f, zoom2_y = 0.0f;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom2_x = dt_control_get_dev_zoom_x();
  zoom2_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = px - .5 * dev->width, mouse_off_y = py - .5 * dev->height;
  zoom2_x += mouse_off_x / (procw * scale);
  zoom2_y += mouse_off_y / (proch * scale);
  *zoom_x = zoom2_x;
  *zoom_y = zoom2_y;
}

gboolean dt_dev_is_current_image(dt_develop_t *dev,
                                 const dt_imgid_t imgid)
{
  return (dev->image_storage.id == imgid) ? TRUE : FALSE;
}

static dt_dev_proxy_exposure_t *find_last_exposure_instance(dt_develop_t *dev)
{
  if(!dev->proxy.exposure.module) return NULL;

  dt_dev_proxy_exposure_t *instance = &dev->proxy.exposure;

  return instance;
};

gboolean dt_dev_exposure_hooks_available(dt_develop_t *dev)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  /* check if exposure iop module has registered its hooks */
  if(instance
     && instance->module
     && instance->set_black
     && instance->get_black && instance->set_exposure
     && instance->get_exposure)
    return TRUE;

  return FALSE;
}

void dt_dev_exposure_reset_defaults(dt_develop_t *dev)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(!(instance && instance->module)) return;

  dt_iop_module_t *exposure = instance->module;
  memcpy(exposure->params, exposure->default_params, exposure->params_size);
  dt_iop_gui_update(exposure);
  dt_dev_add_history_item(exposure->dev, exposure, TRUE);
}

void dt_dev_exposure_set_exposure(dt_develop_t *dev,
                                  const float exposure)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(instance && instance->module && instance->set_exposure)
    instance->set_exposure(instance->module, exposure);
}

float dt_dev_exposure_get_exposure(dt_develop_t *dev)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(instance && instance->module && instance->get_exposure)
    return instance->get_exposure(instance->module);

  return 0.0;
}

void dt_dev_exposure_set_black(dt_develop_t *dev,
                               const float black)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(instance && instance->module && instance->set_black)
    instance->set_black(instance->module, black);
}

float dt_dev_exposure_get_black(dt_develop_t *dev)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(instance && instance->module && instance->get_black)
    return instance->get_black(instance->module);

  return 0.0;
}

void dt_dev_modulegroups_set(dt_develop_t *dev,
                             const uint32_t group)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.set
     && dev->first_load == FALSE)
    dev->proxy.modulegroups.set(dev->proxy.modulegroups.module, group);
}

uint32_t dt_dev_modulegroups_get_activated(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.get_activated)
    return dev->proxy.modulegroups.get_activated(dev->proxy.modulegroups.module);

  return 0;
}

uint32_t dt_dev_modulegroups_get(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.get)
    return dev->proxy.modulegroups.get(dev->proxy.modulegroups.module);

  return 0;
}

gboolean dt_dev_modulegroups_test(dt_develop_t *dev,
                                  const uint32_t group,
                                  dt_iop_module_t *module)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.test)
    return dev->proxy.modulegroups.test(dev->proxy.modulegroups.module, group, module);
  return FALSE;
}

void dt_dev_modulegroups_switch(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.switch_group && dev->first_load == FALSE)
    dev->proxy.modulegroups.switch_group(dev->proxy.modulegroups.module, module);
}

void dt_dev_modulegroups_update_visibility(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.switch_group
     && dev->first_load == FALSE)
    dev->proxy.modulegroups.update_visibility(dev->proxy.modulegroups.module);
}

gboolean dt_dev_modulegroups_is_visible(dt_develop_t *dev,
                                        gchar *module)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.test_visible)
    return dev->proxy.modulegroups.test_visible(dev->proxy.modulegroups.module, module);
  return FALSE;
}

int dt_dev_modulegroups_basics_module_toggle(dt_develop_t *dev,
                                             GtkWidget *widget,
                                             const gboolean doit)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.basics_module_toggle)
    return dev->proxy.modulegroups.basics_module_toggle
      (dev->proxy.modulegroups.module, widget, doit);
  return 0;
}

void dt_dev_masks_list_change(dt_develop_t *dev)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_change)
    dev->proxy.masks.list_change(dev->proxy.masks.module);
}
void dt_dev_masks_list_update(dt_develop_t *dev)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_update)
    dev->proxy.masks.list_update(dev->proxy.masks.module);
}

void dt_dev_masks_list_remove(dt_develop_t *dev,
                              const dt_mask_id_t formid,
                              const dt_mask_id_t parentid)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_remove)
    dev->proxy.masks.list_remove(dev->proxy.masks.module, formid, parentid);
}
void dt_dev_masks_selection_change(dt_develop_t *dev,
                                   struct dt_iop_module_t *module,
                                   const dt_mask_id_t selectid)
{
  if(dev->proxy.masks.module && dev->proxy.masks.selection_change)
    dev->proxy.masks.selection_change(dev->proxy.masks.module, module, selectid);
}

void dt_dev_average_delay_update(const dt_times_t *start,
                                 uint32_t *average_delay)
{
  dt_times_t end;
  dt_get_times(&end);

  *average_delay += ((end.clock - start->clock) * 1000 / DT_DEV_AVERAGE_DELAY_COUNT
                     - *average_delay / DT_DEV_AVERAGE_DELAY_COUNT);
}


/** duplicate a existent module */
dt_iop_module_t *dt_dev_module_duplicate_ext(dt_develop_t *dev,
                                             dt_iop_module_t *base,
                                             const gboolean reorder_iop)
{
  // we create the new module
  dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, base->so, base->dev))
    return NULL;
  module->instance = base->instance;

  // we set the multi-instance priority and the iop order
  int pmax = 0;
  for(GList *modules = base->dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod->instance == base->instance)
    {
      if(pmax < mod->multi_priority) pmax = mod->multi_priority;
    }
  }
  // create a unique multi-priority
  pmax += 1;
  dt_iop_update_multi_priority(module, pmax);

  // add this new module position into the iop-order-list
  dt_ioppr_insert_module_instance(dev, module);

  // since we do not rename the module we need to check that an old
  // module does not have the same name. Indeed the multi_priority are
  // always rebased to start from 0, to it may be the case that the
  // same multi_name be generated when duplicating a module.
  int pname = module->multi_priority;
  char mname[128];

  do
  {
    snprintf(mname, sizeof(mname), "%d", pname);
    gboolean dup = FALSE;

    for(GList *modules = base->dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->instance == base->instance)
      {
        if(strcmp(mname, mod->multi_name) == 0)
        {
          dup = TRUE;
          break;
        }
      }
    }

    if(dup)
      pname++;
    else
      break;
  } while(1);

  // the multi instance name
  g_strlcpy(module->multi_name, mname, sizeof(module->multi_name));
  module->multi_name_hand_edited = FALSE;

  // we insert this module into dev->iop
  base->dev->iop = g_list_insert_sorted(base->dev->iop, module, dt_sort_iop_by_order);

  // always place the new instance after the base one
  if(reorder_iop && !dt_ioppr_move_iop_after(base->dev, module, base))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_dev_module_duplicate] can't move new instance after the base one\n");
    dt_control_log(_("module duplicate, can't move new instance after the base one\n"));
  }

  // that's all. rest of insertion is gui work !
  return module;
}

dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, dt_iop_module_t *base)
{
  return dt_dev_module_duplicate_ext(dev, base, TRUE);
}

void dt_dev_invalidate_history_module(GList *list,
                                      dt_iop_module_t *module)
{
  for(; list; list = g_list_next(list))
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)list->data;
    if(hitem->module == module)
    {
      hitem->module = NULL;
    }
  }
}

void dt_dev_module_remove(dt_develop_t *dev,
                          dt_iop_module_t *module)
{
  // if(darktable.gui->reset) return;
  dt_pthread_mutex_lock(&dev->history_mutex);
  int del = 0;

  if(dev->gui_attached)
  {
    dt_dev_undo_start_record(dev);

    GList *elem = dev->history;
    while(elem != NULL)
    {
      GList *next = g_list_next(elem);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(elem->data);

      if(module == hist->module)
      {
        dt_dev_free_history_item(hist);
        dev->history = g_list_delete_link(dev->history, elem);
        dev->history_end--;
        del = 1;
      }
      elem = next;
    }
  }

  dt_pthread_mutex_unlock(&dev->history_mutex);

  // and we remove it from the list
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      dev->iop = g_list_remove_link(dev->iop, modules);
      break;
    }
  }

  if(dev->gui_attached && del)
  {
    /* signal that history has changed */
    dt_dev_undo_end_record(dev);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                  DT_SIGNAL_DEVELOP_MODULE_REMOVE, module);
    /* redraw */
    dt_control_queue_redraw_center();
  }
}

gchar *dt_history_item_get_name(const struct dt_iop_module_t *module)
{
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = g_strdup(module->name());
  else
    label = g_strdup_printf("%s • %s", module->name(), module->multi_name);
  return label;
}

int dt_dev_distort_transform(dt_develop_t *dev,
                             float *points,
                             const size_t points_count)
{
  return dt_dev_distort_transform_plus
    (dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}
int dt_dev_distort_backtransform(dt_develop_t *dev,
                                 float *points,
                                 const size_t points_count)
{
  return dt_dev_distort_backtransform_plus
    (dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}

// only call directly or indirectly from
// dt_dev_distort_transform_plus, so that it runs with the history
// locked
int dt_dev_distort_transform_locked(dt_develop_t *dev,
                                    dt_dev_pixelpipe_t *pipe,
                                    const double iop_order,
                                    const int transf_direction,
                                    float *points,
                                    const size_t points_count)
{
  GList *modules = pipe->iop;
  GList *pieces = pipe->nodes;
  while(modules)
  {
    if(!pieces)
    {
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->enabled
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
               && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
               && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
               && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
               && module->iop_order < iop_order))
       && !(dev->gui_module && dev->gui_module != module
            && (dev->gui_module->operation_tags_filter() & module->operation_tags())))
    {
      module->distort_transform(module, piece, points, points_count);
    }
    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  return 1;
}

int dt_dev_distort_transform_plus(dt_develop_t *dev,
                                  dt_dev_pixelpipe_t *pipe,
                                  const double iop_order,
                                  const int transf_direction,
                                  float *points,
                                  const size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  const int success =
    dt_dev_distort_transform_locked(dev, pipe, iop_order, transf_direction,
                                    points, points_count);

  if(success
      && (dev->preview_downsampling != 1.0f)
      && (transf_direction == DT_DEV_TRANSFORM_DIR_ALL
          || transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
          || transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL))
    for(size_t idx=0; idx < 2 * points_count; idx++)
      points[idx] *= dev->preview_downsampling;

  dt_pthread_mutex_unlock(&dev->history_mutex);
  return 1;
}

// only call directly or indirectly from
// dt_dev_distort_transform_plus, so that it runs with the history
// locked
int dt_dev_distort_backtransform_locked(dt_develop_t *dev,
                                        dt_dev_pixelpipe_t *pipe,
                                        const double iop_order,
                                        const int transf_direction,
                                        float *points,
                                        const size_t points_count)
{
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->enabled
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
               && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
               && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
               && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
               && module->iop_order < iop_order))
       && !(dev->gui_module && dev->gui_module != module
            && (dev->gui_module->operation_tags_filter() & module->operation_tags())))
    {
      module->distort_backtransform(module, piece, points, points_count);
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  return 1;
}

int dt_dev_distort_backtransform_plus(dt_develop_t *dev,
                                      dt_dev_pixelpipe_t *pipe,
                                      const double iop_order,
                                      const int transf_direction,
                                      float *points,
                                      const size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  if((dev->preview_downsampling != 1.0f) && (transf_direction == DT_DEV_TRANSFORM_DIR_ALL
    || transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
    || transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL))
  {
    for(size_t idx=0; idx < 2 * points_count; idx++)
      points[idx] /= dev->preview_downsampling;
  }

  const int success = dt_dev_distort_backtransform_locked
    (dev, pipe, iop_order,
     transf_direction, points, points_count);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return success;
}

dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev,
                                                    struct dt_dev_pixelpipe_t *pipe,
                                                    struct dt_iop_module_t *module)
{
  for(const GList *pieces = g_list_last(pipe->nodes);
      pieces;
      pieces = g_list_previous(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->module == module)
    {
      return piece;
    }
  }
  return NULL;
}

uint64_t dt_dev_hash(dt_develop_t *dev)
{
  return dt_dev_hash_plus(dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL);
}

uint64_t dt_dev_hash_plus(dt_develop_t *dev,
                          struct dt_dev_pixelpipe_t *pipe,
                          const double iop_order,
                          const int transf_direction)
{
  uint64_t hash = 5381;
  dt_pthread_mutex_lock(&dev->history_mutex);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      dt_pthread_mutex_unlock(&dev->history_mutex);
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->enabled && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
                              && module->iop_order >= iop_order)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
                              && module->iop_order > iop_order)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
                              && module->iop_order <= iop_order)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
                              && module->iop_order < iop_order)))
    {
      hash = ((hash << 5) + hash) ^ piece->hash;
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return hash;
}

int dt_dev_wait_hash(dt_develop_t *dev,
                     struct dt_dev_pixelpipe_t *pipe,
                     const double iop_order,
                     const int transf_direction,
                     dt_pthread_mutex_t *lock,
                     const volatile uint64_t *const hash)
{
  const int usec = 5000;
  int nloop;

#ifdef HAVE_OPENCL
  if(pipe->devid >= 0)
    nloop = darktable.opencl->opencl_synchronization_timeout;
  else
    nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#else
  nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#endif

  if(nloop <= 0) return TRUE;  // non-positive values omit pixelpipe synchronization

  for(int n = 0; n < nloop; n++)
  {
    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;  // stop waiting if pipe shuts down

    uint64_t probehash;

    if(lock)
    {
      dt_pthread_mutex_lock(lock);
      probehash = *hash;
      dt_pthread_mutex_unlock(lock);
    }
    else
      probehash = *hash;

    if(probehash == dt_dev_hash_plus(dev, pipe, iop_order, transf_direction))
      return TRUE;

    dt_iop_nap(usec);
  }

  return FALSE;
}

int dt_dev_sync_pixelpipe_hash(dt_develop_t *dev,
                               struct dt_dev_pixelpipe_t *pipe,
                               const double iop_order,
                               const int transf_direction,
                               dt_pthread_mutex_t *lock,
                               const volatile uint64_t *const hash)
{
  // first wait for matching hash values
  if(dt_dev_wait_hash(dev, pipe, iop_order, transf_direction, lock, hash))
    return TRUE;

  // timed out. let's see if history stack has changed
  if(pipe->changed & (DT_DEV_PIPE_TOP_CHANGED | DT_DEV_PIPE_REMOVE | DT_DEV_PIPE_SYNCH))
  {
    // history stack has changed. let's trigger reprocessing
    dt_control_queue_redraw_center();
    // pretend that everything is fine
    return TRUE;
  }

  // no way to get pixelpipes in sync
  return FALSE;
}

uint64_t dt_dev_hash_distort(dt_develop_t *dev)
{
  return dt_dev_hash_distort_plus(dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL);
}

uint64_t dt_dev_hash_distort_plus(dt_develop_t *dev,
                                  struct dt_dev_pixelpipe_t *pipe,
                                  const double iop_order,
                                  const int transf_direction)
{
  uint64_t hash = 5381;
  dt_pthread_mutex_lock(&dev->history_mutex);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      dt_pthread_mutex_unlock(&dev->history_mutex);
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->enabled && module->operation_tags() & IOP_TAG_DISTORT
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
               && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
               && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
               && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
               && module->iop_order < iop_order)))
    {
      hash = ((hash << 5) + hash) ^ piece->hash;
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return hash;
}

int dt_dev_wait_hash_distort(dt_develop_t *dev,
                             struct dt_dev_pixelpipe_t *pipe,
                             const double iop_order,
                             const int transf_direction,
                             dt_pthread_mutex_t *lock,
                             const volatile uint64_t *const hash)
{
  const int usec = 5000;
  int nloop = 0;

#ifdef HAVE_OPENCL
  if(pipe->devid >= 0)
    nloop = darktable.opencl->opencl_synchronization_timeout;
  else
    nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#else
  nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#endif

  if(nloop <= 0) return TRUE;  // non-positive values omit pixelpipe synchronization

  for(int n = 0; n < nloop; n++)
  {
    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;  // stop waiting if pipe shuts down

    uint64_t probehash = 0;

    if(lock)
    {
      dt_pthread_mutex_lock(lock);
      probehash = *hash;
      dt_pthread_mutex_unlock(lock);
    }
    else
      probehash = *hash;

    if(probehash == dt_dev_hash_distort_plus(dev, pipe, iop_order, transf_direction))
      return TRUE;

    dt_iop_nap(usec);
  }

  return FALSE;
}

int dt_dev_sync_pixelpipe_hash_distort(dt_develop_t *dev,
                                       struct dt_dev_pixelpipe_t *pipe,
                                       const double iop_order,
                                       const int transf_direction,
                                       dt_pthread_mutex_t *lock,
                                       const volatile uint64_t *const hash)
{
  // first wait for matching hash values
  if(dt_dev_wait_hash_distort(dev, pipe, iop_order, transf_direction, lock, hash))
    return TRUE;

  // timed out. let's see if history stack has changed
  if(pipe->changed & (DT_DEV_PIPE_TOP_CHANGED | DT_DEV_PIPE_REMOVE | DT_DEV_PIPE_SYNCH))
  {
    // history stack has changed. let's trigger reprocessing
    dt_control_queue_redraw_center();
    // pretend that everything is fine
    return TRUE;
  }

  // no way to get pixelpipes in sync
  return FALSE;
}

// set the module list order
void dt_dev_reorder_gui_module_list(dt_develop_t *dev)
{
  int pos_module = 0;
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    GtkWidget *expander = module->expander;
    if(expander)
    {
      gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui,
                                                DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                            expander,
                            pos_module++);
    }
  }
}

//-----------------------------------------------------------
// second darkroom window
//-----------------------------------------------------------

dt_dev_zoom_t dt_second_window_get_dev_zoom(dt_develop_t *dev)
{
  return dev->second_window.zoom;
}

void dt_second_window_set_dev_zoom(dt_develop_t *dev,
                                   const dt_dev_zoom_t value)
{
  dev->second_window.zoom = value;
}

int dt_second_window_get_dev_closeup(dt_develop_t *dev)
{
  return dev->second_window.closeup;
}

void dt_second_window_set_dev_closeup(dt_develop_t *dev,
                                      const int value)
{
  dev->second_window.closeup = value;
}

float dt_second_window_get_dev_zoom_x(dt_develop_t *dev)
{
  return dev->second_window.zoom_x;
}

void dt_second_window_set_dev_zoom_x(dt_develop_t *dev,
                                     const float value)
{
  dev->second_window.zoom_x = value;
}

float dt_second_window_get_dev_zoom_y(dt_develop_t *dev)
{
  return dev->second_window.zoom_y;
}

void dt_second_window_set_dev_zoom_y(dt_develop_t *dev,
                                     const float value)
{
  dev->second_window.zoom_y = value;
}

float dt_second_window_get_free_zoom_scale(dt_develop_t *dev)
{
  return dev->second_window.zoom_scale;
}

float dt_second_window_get_zoom_scale(dt_develop_t *dev,
                                      const dt_dev_zoom_t zoom,
                                      const int closeup_factor,
                                      const int preview)
{
  float zoom_scale = 0.0f;

  const float w = preview
    ? dev->preview_pipe->processed_width
    : dev->preview2_pipe->processed_width;

  const float h = preview
    ? dev->preview_pipe->processed_height
    : dev->preview2_pipe->processed_height;

  const float ps = dev->preview2_pipe->backbuf_width
    ? dev->preview2_pipe->processed_width / (float)dev->preview_pipe->processed_width
    : dev->preview_pipe->iscale;

  switch(zoom)
  {
    case DT_ZOOM_FIT:
      zoom_scale = fminf(dev->second_window.width / w, dev->second_window.height / h);
      break;
    case DT_ZOOM_FILL:
      zoom_scale = fmaxf(dev->second_window.width / w, dev->second_window.height / h);
      break;
    case DT_ZOOM_1:
      zoom_scale = closeup_factor;
      if(preview) zoom_scale *= ps;
      break;
    default: // DT_ZOOM_FREE
      zoom_scale = dt_second_window_get_free_zoom_scale(dev);
      if(preview) zoom_scale *= ps;
      break;
  }
  if(preview) zoom_scale /= dev->preview_downsampling;
  return zoom_scale;
}

void dt_second_window_set_zoom_scale(dt_develop_t *dev,
                                     const float value)
{
  dev->second_window.zoom_scale = value;
}

void dt_second_window_get_processed_size(const dt_develop_t *dev,
                                         int *procw,
                                         int *proch)
{
  if(!dev) return;

  // if preview2 is processed, lets return its size
  if(dev->preview2_pipe && dev->preview2_pipe->processed_width)
  {
    *procw = dev->preview2_pipe->processed_width;
    *proch = dev->preview2_pipe->processed_height;
    return;
  }

  // fallback on preview pipe
  if(dev->preview_pipe && dev->preview_pipe->processed_width)
  {
    const float scale = (dev->preview_pipe->iscale / dev->preview_downsampling);
    *procw = scale * dev->preview_pipe->processed_width;
    *proch = scale * dev->preview_pipe->processed_height;
    return;
  }

  // no processed pipes, lets return 0 size
  *procw = *proch = 0;
  return;
}

void dt_second_window_check_zoom_bounds(dt_develop_t *dev,
                                        float *zoom_x,
                                        float *zoom_y,
                                        const dt_dev_zoom_t zoom,
                                        const int closeup,
                                        float *boxww,
                                        float *boxhh)
{
  int procw = 0, proch = 0;
  dt_second_window_get_processed_size(dev, &procw, &proch);
  float boxw = 1.0f, boxh = 1.0f; // viewport in normalised space
                            //   if(zoom == DT_ZOOM_1)
                            //   {
                            //     const float imgw = (closeup ? 2 : 1)*procw;
                            //     const float imgh = (closeup ? 2 : 1)*proch;
                            //     const float devw = MIN(imgw, dev->width);
                            //     const float devh = MIN(imgh, dev->height);
                            //     boxw = fminf(1.0, devw/imgw);
                            //     boxh = fminf(1.0, devh/imgh);
                            //   }
  if(zoom == DT_ZOOM_FIT)
  {
    *zoom_x = *zoom_y = 0.0f;
    boxw = boxh = 1.0f;
  }
  else
  {
    const float scale = dt_second_window_get_zoom_scale(dev, zoom, 1 << closeup, 0);
    const float imgw = procw;
    const float imgh = proch;
    const float devw = dev->second_window.width;
    const float devh = dev->second_window.height;
    boxw = devw / (imgw * scale);
    boxh = devh / (imgh * scale);
  }

  if(*zoom_x < boxw / 2 - .5) *zoom_x = boxw / 2 - .5;
  if(*zoom_x > .5 - boxw / 2) *zoom_x = .5 - boxw / 2;
  if(*zoom_y < boxh / 2 - .5) *zoom_y = boxh / 2 - .5;
  if(*zoom_y > .5 - boxh / 2) *zoom_y = .5 - boxh / 2;
  if(boxw > 1.0) *zoom_x = 0.0f;
  if(boxh > 1.0) *zoom_y = 0.0f;

  if(boxww) *boxww = boxw;
  if(boxhh) *boxhh = boxh;
}

void dt_dev_undo_start_record(dt_develop_t *dev)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  /* record current history state : before change (needed for undo) */
  if(dev->gui_attached && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                  DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);

  dev->gui_previous_target = NULL;
}

void dt_dev_undo_end_record(dt_develop_t *dev)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  /* record current history state : after change (needed for undo) */
  if(dev->gui_attached && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
}

void dt_dev_image_ext(const dt_imgid_t imgid,
                      const size_t width,
                      const size_t height,
                      const int history_end,
                      uint8_t **buf,
                      size_t *processed_width,
                      size_t *processed_height,
                      const int border_size,
                      const gboolean iso_12646,
                      const int32_t snapshot_id)
{
  dt_develop_t dev;
  dt_dev_init(&dev, TRUE);
  dev.border_size = border_size;
  dev.iso_12646.enabled = iso_12646;

  // create the full pipe

  dev.gui_attached = FALSE;
  dt_dev_pixelpipe_init(dev.pipe);

  // load image and set history_end

  dt_dev_load_image_ext(&dev, imgid, snapshot_id);

  if(history_end != -1 && snapshot_id == -1)
    dt_dev_pop_history_items_ext(&dev, history_end);

  // configure the actual dev width & height

  dt_dev_configure(&dev, width, height);

  // process the pipe

  dt_dev_process_image_job(&dev);

  // record resulting image and dimensions

  const uint32_t bufsize =
    sizeof(uint32_t) * dev.pipe->backbuf_width * dev.pipe->backbuf_height;
  *buf = dt_alloc_align(64, bufsize);
  memcpy(*buf, dev.pipe->backbuf, bufsize);
  *processed_width  = dev.pipe->backbuf_width;
  *processed_height = dev.pipe->backbuf_height;

  // we take the backbuf, avoid it to be released

  dt_dev_cleanup(&dev);
}

void dt_dev_image(const dt_imgid_t imgid,
                  const size_t width,
                  const size_t height,
                  const int history_end,
                  uint8_t **buf,
                  size_t *processed_width,
                  size_t *processed_height)
{
  // create a dev

  dt_dev_image_ext(imgid, width, height,
                   history_end,
                   buf, processed_width, processed_height,
                   darktable.develop->border_size,
                   darktable.develop->iso_12646.enabled, -1);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
