/**
 * Copyright (C) 2011-2016 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   oggmuxfltprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia - Ogg muxer filter processor
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <alloca.h>

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

#include <OMX_TizoniaExt.h>

#include <tizplatform.h>

#include <tizkernel.h>
#include <tizscheduler.h>

#include "oggmux.h"
#include "oggmuxfltprc.h"
#include "oggmuxfltprc_decls.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.ogg_muxer.filter.prc"
#endif

#define on_oggz_error_ret_omx_oom(expr)                    \
  do                                                       \
    {                                                      \
      int oggz_error = 0;                                  \
      if (0 != (oggz_error = (expr)))                      \
        {                                                  \
          TIZ_ERROR (handleOf (ap_prc),                    \
                     "[OMX_ErrorInsufficientResources] : " \
                     "oggz (%s)",                          \
                     strerror (errno));                    \
          return OMX_ErrorInsufficientResources;           \
        }                                                  \
    }                                                      \
  while (0)

/* Forward declarations */
static OMX_ERRORTYPE
oggmuxflt_prc_deallocate_resources (void *);

static OMX_ERRORTYPE
audio_hungry (oggmuxflt_prc_t * ap_prc)
{
  OMX_BUFFERHEADERTYPE * p_hdr = NULL;
  ogg_packet op;
  assert (ap_prc);
  if ((p_hdr = tiz_filter_prc_get_header (
         ap_prc, ARATELIA_OGG_MUXER_FILTER_PORT_0_INDEX)))
    {
      op.packet = p_hdr->pBuffer + p_hdr->nOffset;
      op.bytes = p_hdr->nFilledLen;
      op.granulepos = ap_prc->oggz_audio_granulepos_;
      op.packetno = ap_prc->oggz_audio_packetno_;
      if (op.packetno == 0)
        {
          op.b_o_s = 1;
        }
      else
        {
          op.b_o_s = 0;
        }
      if (op.packetno == 9)
        {
          op.e_o_s = 1;
        }
      else
        {
          op.e_o_s = 0;
        }
      oggz_write_feed (ap_prc->p_oggz_, &op, ap_prc->oggz_audio_serialno_,
                       OGGZ_FLUSH_AFTER, NULL);
      ap_prc->oggz_audio_granulepos_ += 100;
      ap_prc->oggz_audio_packetno_++;
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
video_hungry (oggmuxflt_prc_t * ap_prc)
{
  OMX_BUFFERHEADERTYPE * p_hdr = NULL;
  ogg_packet op;
  assert (ap_prc);
  if ((p_hdr = tiz_filter_prc_get_header (
         ap_prc, ARATELIA_OGG_MUXER_FILTER_PORT_1_INDEX)))
    {
      op.packet = p_hdr->pBuffer + p_hdr->nOffset;
      op.bytes = p_hdr->nFilledLen;
      op.granulepos = ap_prc->oggz_video_granulepos_;
      op.packetno = ap_prc->oggz_video_packetno_;
      if (op.packetno == 0)
        {
          op.b_o_s = 1;
        }
      else
        {
          op.b_o_s = 0;
        }
      if (op.packetno == 9)
        {
          op.e_o_s = 1;
        }
      else
        {
          op.e_o_s = 0;
        }
      oggz_write_feed (ap_prc->p_oggz_, &op, ap_prc->oggz_video_serialno_,
                       OGGZ_FLUSH_AFTER, NULL);
      ap_prc->oggz_video_granulepos_ += 1;
      ap_prc->oggz_video_packetno_++;
    }
  return OMX_ErrorNone;
}

static int
og_hungry (OGGZ * oggz, int empty, void * user_data)
{
  oggmuxflt_prc_t * p_prc = user_data;

  assert (p_prc);
  audio_hungry (p_prc);
  video_hungry (p_prc);

  return 0;
}

static OMX_ERRORTYPE
mux_streams (oggmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNotReady;
  long oggz_rc = OGGZ_ERR_OK;
  while (OGGZ_ERR_OK == oggz_rc)
    {
      if ((oggz_rc = oggz_write (ap_prc->p_oggz_, 32)) > 0)
        {
          oggz_rc = OGGZ_ERR_OK;
        }
    }
  return rc;
}

static size_t
og_io_write (void * ap_user_handle, void * ap_buf, size_t n)
{
  oggmuxflt_prc_t * p_prc = ap_user_handle;
  ssize_t bytes_written = 0;

  if (n > 0 && ap_buf)
    {
      OMX_BUFFERHEADERTYPE * p_hdr = p_hdr = tiz_filter_prc_get_header (
        p_prc, ARATELIA_OGG_MUXER_FILTER_PORT_2_INDEX);

      assert (p_prc);

      if (p_hdr)
        {
          bytes_written = MIN (n, TIZ_OMX_BUF_AVAIL (p_hdr));
          (void) memcpy (TIZ_OMX_BUF_PTR (p_hdr) + p_hdr->nFilledLen, ap_buf,
                         bytes_written);
          p_hdr->nFilledLen += bytes_written;
          TIZ_DEBUG (handleOf (p_prc), "bytes - n [%d] nFilledLen [%d]", n,
                     p_hdr->nFilledLen);
        }
      else
        {
          TIZ_DEBUG (handleOf (p_prc), "No output headers available");
        }
    }
  return bytes_written;
}

static int
og_io_flush (void * ap_user_handle)
{
  return 0;
}

static OMX_ERRORTYPE
alloc_uri (oggmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  const long pathname_max = PATH_MAX + NAME_MAX;

  assert (ap_prc);
  assert (!ap_prc->p_uri_);

  if (!(ap_prc->p_uri_ = tiz_mem_calloc (
          1, sizeof (OMX_PARAM_CONTENTURITYPE) + pathname_max + 1)))
    {
      TIZ_ERROR (handleOf (ap_prc),
                 "Error allocating memory for the content uri struct");
      rc = OMX_ErrorInsufficientResources;
    }
  else
    {
      ap_prc->p_uri_->nSize
        = sizeof (OMX_PARAM_CONTENTURITYPE) + pathname_max + 1;
      ap_prc->p_uri_->nVersion.nVersion = OMX_VERSION;

      if (OMX_ErrorNone
          != (rc = tiz_api_GetParameter (
                tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                OMX_IndexParamContentURI, ap_prc->p_uri_)))
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "[%s] : Error retrieving the URI param from port",
                     tiz_err_to_str (rc));
        }
      else
        {
          TIZ_NOTICE (handleOf (ap_prc), "URI [%s]",
                      ap_prc->p_uri_->contentURI);
        }
    }
  return rc;
}

static OMX_ERRORTYPE
alloc_data_store (oggmuxflt_prc_t * ap_prc, tiz_buffer_t * ap_store,
                  const OMX_U32 a_pid)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (port_def, a_pid);
  tiz_check_omx_err (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &port_def));
  assert (ap_store == NULL);
  tiz_check_omx_err (tiz_buffer_init (&(ap_store), port_def.nBufferSize * 4));
  return OMX_ErrorNone;
  ;
}

static OMX_ERRORTYPE
alloc_oggz (oggmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (ap_prc);

  /* Allocate the oggz object */
  tiz_check_null_ret_oom ((ap_prc->p_oggz_ = oggz_new (OGGZ_WRITE)));

  /* Obtain the serial numbers */
  ap_prc->oggz_audio_serialno_ = oggz_serialno_new (ap_prc->p_oggz_);
  ap_prc->oggz_video_serialno_ = oggz_serialno_new (ap_prc->p_oggz_);

  /* Set the 'hungry' callback */
  on_oggz_error_ret_omx_oom (
    oggz_write_set_hungry_callback (ap_prc->p_oggz_, og_hungry, 1, ap_prc));

  /* Set the io callbacks */
  on_oggz_error_ret_omx_oom (
    oggz_io_set_write (ap_prc->p_oggz_, og_io_write, ap_prc));
  on_oggz_error_ret_omx_oom (
    oggz_io_set_flush (ap_prc->p_oggz_, og_io_flush, ap_prc));

  return rc;
}

static OMX_ERRORTYPE
am_i_able_to_mux (oggmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  bool audio_data_avail = (tiz_buffer_available (ap_prc->p_audio_store_) > 0);
  bool video_data_avail = (tiz_buffer_available (ap_prc->p_video_store_) > 0);
  if (!audio_data_avail && !video_data_avail)
    {
      TIZ_DEBUG (handleOf (ap_prc), "Audio or Video data available");
      rc = OMX_ErrorNotReady;
    }
  else
    {
      OMX_BUFFERHEADERTYPE * p_hdr = tiz_filter_prc_get_header (
        ap_prc, ARATELIA_OGG_MUXER_FILTER_PORT_2_INDEX);
      if (!p_hdr)
        {
          TIZ_DEBUG (handleOf (ap_prc), "No output buffers available");
          rc = OMX_ErrorNotReady;
        }
    }
  return rc;
}

static void
reset_stream_parameters (oggmuxflt_prc_t * ap_prc)
{
  assert (ap_prc);

  tiz_buffer_clear (ap_prc->p_audio_store_);
  tiz_filter_prc_update_eos_flag (ap_prc, false);
}

static inline void
dealloc_uri (/*@special@ */ oggmuxflt_prc_t * ap_prc)
/*@releases ap_prc->p_uri_ @ */
/*@ensures isnull ap_prc->p_uri_ @ */
{
  assert (ap_prc);
  tiz_mem_free (ap_prc->p_uri_);
  ap_prc->p_uri_ = NULL;
}

static inline void
dealloc_data_store (tiz_buffer_t ** app_store)
{
  assert (app_store);
  assert (*app_store);
  tiz_buffer_destroy (*app_store);
  *app_store = NULL;
}

static inline void
dealloc_oggz (
  /*@special@ */ oggmuxflt_prc_t * ap_prc)
/*@releases ap_prc->p_oggz_@ */
/*@ensures isnull ap_prc->p_oggz_@ */
{
  assert (ap_prc);
  if (ap_prc->p_oggz_)
    {
      /* TODO: delete oggz */
      /*   oggz_close (ap_prc->p_oggz_); */
      ap_prc->p_oggz_ = NULL;
    }
}

static inline OMX_ERRORTYPE
do_flush (oggmuxflt_prc_t * ap_prc, OMX_U32 a_pid)
{
  assert (ap_prc);
  TIZ_TRACE (handleOf (ap_prc), "do_flush");
  if (OMX_ALL == a_pid || ARATELIA_OGG_MUXER_FILTER_PORT_0_INDEX == a_pid)
    {
      reset_stream_parameters (ap_prc);
    }
  /* Release any buffers held  */
  return tiz_filter_prc_release_header (ap_prc, a_pid);
}

/* TODO: move this functionality to tiz_filter_prc_t */
static OMX_ERRORTYPE
release_input_header (oggmuxflt_prc_t * ap_prc, const OMX_U32 a_pid,
                      OMX_BUFFERHEADERTYPE * ap_hdr)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (ap_prc);
  assert (ap_hdr);
  if (ap_hdr)
    {
      TIZ_DEBUG (handleOf (ap_prc), "[%p] nFlags [%d]", ap_hdr, ap_hdr->nFlags);

      if ((ap_hdr->nFlags & OMX_BUFFERFLAG_EOS) > 0)
        {
          tiz_filter_prc_update_eos_flag (ap_prc, true);
          ap_hdr->nFlags &= ~(1 << OMX_BUFFERFLAG_EOS);
        }
      rc = tiz_filter_prc_release_header (ap_prc, a_pid);
    }
  return rc;
}

static OMX_ERRORTYPE
store_data (oggmuxflt_prc_t * ap_prc, const OMX_U32 a_pid,
            tiz_buffer_t ** app_store)
{
  bool rc = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE * p_in = NULL;
  tiz_buffer_t * p_store = NULL;
  assert (ap_prc);
  assert (app_store);
  assert (*app_store);

  p_store = *app_store;
  p_in = tiz_filter_prc_get_header (ap_prc, a_pid);

  if (p_in)
    {
      TIZ_TRACE (handleOf (ap_prc), "avail [%d] pid [%d] incoming [%d]",
                 tiz_buffer_available (p_store), a_pid,
                 p_in->nFilledLen - p_in->nOffset);
      if (tiz_buffer_push (p_store, p_in->pBuffer + p_in->nOffset,
                           p_in->nFilledLen)
          < p_in->nFilledLen)
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "[OMX_ErrorInsufficientResources] : Unable to store "
                     "all the data - pid [%d].",
                     a_pid);
          rc = OMX_ErrorInsufficientResources;
        }
      else
        {
          rc = release_input_header (ap_prc, a_pid, p_in);
        }
    }
  return rc;
}

/*
 * oggmuxfltprc
 */

static void *
oggmuxflt_prc_ctor (void * ap_prc, va_list * app)
{
  oggmuxflt_prc_t * p_prc
    = super_ctor (typeOf (ap_prc, "oggmuxfltprc"), ap_prc, app);
  assert (p_prc);
  p_prc->p_audio_store_ = NULL;
  p_prc->p_video_store_ = NULL;
  p_prc->p_uri_ = NULL;
  p_prc->p_oggz_ = NULL;
  p_prc->oggz_audio_serialno_ = 0;
  p_prc->oggz_video_serialno_ = 0;
  p_prc->oggz_audio_granulepos_ = 0;
  p_prc->oggz_video_granulepos_ = 0;
  p_prc->oggz_audio_packetno_ = 0;
  p_prc->oggz_video_packetno_ = 0;
  reset_stream_parameters (p_prc);
  return p_prc;
}

static void *
oggmuxflt_prc_dtor (void * ap_obj)
{
  (void) oggmuxflt_prc_deallocate_resources (ap_obj);
  return super_dtor (typeOf (ap_obj, "oggmuxfltprc"), ap_obj);
}

/*
 * from tizsrv class
 */

static OMX_ERRORTYPE
oggmuxflt_prc_allocate_resources (void * ap_prc, OMX_U32 a_pid)
{
  oggmuxflt_prc_t * p_prc = ap_prc;
  assert (p_prc);
  tiz_check_omx_err (alloc_uri (p_prc));
  tiz_check_omx_err (alloc_oggz (p_prc));
  tiz_check_omx_err (alloc_data_store (p_prc, p_prc->p_audio_store_,
                                       ARATELIA_OGG_MUXER_FILTER_PORT_0_INDEX));
  tiz_check_omx_err (alloc_data_store (p_prc, p_prc->p_video_store_,
                                       ARATELIA_OGG_MUXER_FILTER_PORT_1_INDEX));
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggmuxflt_prc_deallocate_resources (void * ap_prc)
{
  oggmuxflt_prc_t * p_prc = ap_prc;
  assert (p_prc);
  dealloc_data_store (&(p_prc->p_audio_store_));
  dealloc_data_store (&(p_prc->p_video_store_));
  dealloc_oggz (p_prc);
  dealloc_uri (p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggmuxflt_prc_prepare_to_transfer (void * ap_prc, OMX_U32 a_pid)
{
  return OMX_ErrorNone;
  ;
}

static OMX_ERRORTYPE
oggmuxflt_prc_transfer_and_process (void * ap_prc, OMX_U32 a_pid)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggmuxflt_prc_stop_and_return (void * ap_prc)
{
  /* Do flush on all ports; this will reset the stream parameters and release
     any buffers held */
  return do_flush (ap_prc, OMX_ALL);
}

/*
 * from tizprc class
 */

static OMX_ERRORTYPE
oggmuxflt_prc_buffers_ready (const void * ap_prc)
{
  oggmuxflt_prc_t * p_prc = (oggmuxflt_prc_t *) ap_prc;
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (ap_prc);

  tiz_check_omx_err (store_data (p_prc, ARATELIA_OGG_MUXER_FILTER_PORT_0_INDEX,
                                 &(p_prc->p_audio_store_)));
  tiz_check_omx_err (store_data (p_prc, ARATELIA_OGG_MUXER_FILTER_PORT_1_INDEX,
                                 &(p_prc->p_video_store_)));

  while (OMX_ErrorNone == (rc = am_i_able_to_mux (p_prc)))
    {
      rc = mux_streams (p_prc);
    }

  if (OMX_ErrorNotReady == rc)
    {
      rc = OMX_ErrorNone;
    }

  return rc;
}

static OMX_ERRORTYPE
oggmuxflt_prc_pause (const void * ap_obj)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggmuxflt_prc_resume (const void * ap_obj)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
oggmuxflt_prc_port_flush (const void * ap_prc, OMX_U32 a_pid)
{
  oggmuxflt_prc_t * p_prc = (oggmuxflt_prc_t *) ap_prc;
  return do_flush (p_prc, a_pid);
}

static OMX_ERRORTYPE
oggmuxflt_prc_port_disable (const void * ap_prc, OMX_U32 a_pid)
{
  oggmuxflt_prc_t * p_prc = (oggmuxflt_prc_t *) ap_prc;
  OMX_ERRORTYPE rc = tiz_filter_prc_release_header (p_prc, a_pid);
  reset_stream_parameters (p_prc);
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, true);
  return rc;
}

static OMX_ERRORTYPE
oggmuxflt_prc_port_enable (const void * ap_prc, OMX_U32 a_pid)
{
  oggmuxflt_prc_t * p_prc = (oggmuxflt_prc_t *) ap_prc;
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, false);
  return OMX_ErrorNone;
}

/*
 * oggmuxflt_prc_class
 */

static void *
oggmuxflt_prc_class_ctor (void * ap_prc, va_list * app)
{
  /* NOTE: Class methods might be added in the future. None for now. */
  return super_ctor (typeOf (ap_prc, "oggmuxfltprc_class"), ap_prc, app);
}

/*
 * initialization
 */

void *
oggmuxflt_prc_class_init (void * ap_tos, void * ap_hdl)
{
  void * tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void * oggmuxfltprc_class = factory_new
    /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
    (classOf (tizfilterprc), "oggmuxfltprc_class", classOf (tizfilterprc),
     sizeof (oggmuxflt_prc_class_t),
     /* TIZ_CLASS_COMMENT: */
     ap_tos, ap_hdl,
     /* TIZ_CLASS_COMMENT: class constructor */
     ctor, oggmuxflt_prc_class_ctor,
     /* TIZ_CLASS_COMMENT: stop value*/
     0);
  return oggmuxfltprc_class;
}

void *
oggmuxflt_prc_init (void * ap_tos, void * ap_hdl)
{
  void * tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void * oggmuxfltprc_class = tiz_get_type (ap_hdl, "oggmuxfltprc_class");
  TIZ_LOG_CLASS (oggmuxfltprc_class);
  void * oggmuxfltprc = factory_new
    /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
    (oggmuxfltprc_class, "oggmuxfltprc", tizfilterprc, sizeof (oggmuxflt_prc_t),
     /* TIZ_CLASS_COMMENT: */
     ap_tos, ap_hdl,
     /* TIZ_CLASS_COMMENT: class constructor */
     ctor, oggmuxflt_prc_ctor,
     /* TIZ_CLASS_COMMENT: class destructor */
     dtor, oggmuxflt_prc_dtor,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_allocate_resources, oggmuxflt_prc_allocate_resources,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_deallocate_resources, oggmuxflt_prc_deallocate_resources,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_prepare_to_transfer, oggmuxflt_prc_prepare_to_transfer,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_transfer_and_process, oggmuxflt_prc_transfer_and_process,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_stop_and_return, oggmuxflt_prc_stop_and_return,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_buffers_ready, oggmuxflt_prc_buffers_ready,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_pause, oggmuxflt_prc_pause,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_resume, oggmuxflt_prc_resume,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_flush, oggmuxflt_prc_port_flush,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_disable, oggmuxflt_prc_port_disable,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_enable, oggmuxflt_prc_port_enable,
     /* TIZ_CLASS_COMMENT: stop value */
     0);

  return oggmuxfltprc;
}