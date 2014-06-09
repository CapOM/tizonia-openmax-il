/**
 * Copyright (C) 2011-2014 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tizonia is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   aacdecprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia OpenMAX IL - AAC Decoder processor class implementation
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <limits.h>
#include <string.h>

#include <tizplatform.h>

#include <tizkernel.h>

#include "aacdec.h"
#include "aacdecprc_decls.h"
#include "aacdecprc.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.aac_decoder.prc"
#endif

/* Forward declarations */
static OMX_ERRORTYPE aacdec_prc_deallocate_resources (void *);

static OMX_ERRORTYPE alloc_temp_data_store (aacdec_prc_t *ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  assert (NULL != ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (port_def, ARATELIA_AAC_DECODER_INPUT_PORT_INDEX);
  tiz_check_omx_err (
      tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                            OMX_IndexParamPortDefinition, &port_def));

  assert (ap_prc->p_store_ == NULL);
  tiz_check_omx_err (
      tiz_buffer_init (&(ap_prc->p_store_), port_def.nBufferSize));

  return OMX_ErrorNone;
}

static inline void dealloc_temp_data_store (
    /*@special@ */ aacdec_prc_t *ap_prc)
/*@releases ap_prc->p_store_@ */
/*@ensures isnull ap_prc->p_store_@ */
{
  assert (NULL != ap_prc);
  tiz_buffer_destroy (ap_prc->p_store_);
  ap_prc->p_store_ = NULL;
}

static void skip_id3_tag (aacdec_prc_t *ap_prc)
{
  OMX_U8 *p_buffer = tiz_buffer_get_data (ap_prc->p_store_);

  assert (NULL != ap_prc);

  if (!memcmp (p_buffer, "ID3", 3))
    {
      int tagsize = 0;
      /* high bit is not used */
      tagsize = (p_buffer[6] << 21) | (p_buffer[7] << 14) | (p_buffer[8] << 7)
                | (p_buffer[9] << 0);
      tagsize += 10;
      tiz_buffer_advance (ap_prc->p_store_, tagsize);
    }
}

static inline OMX_ERRORTYPE retrieve_aac_settings (
    const void *ap_prc, OMX_AUDIO_PARAM_AACPROFILETYPE *ap_aactype)
{
  const aacdec_prc_t *p_prc = ap_prc;
  assert (NULL != ap_prc);
  assert (NULL != ap_aactype);

  /* Retrieve the aac settings from the input port */
  TIZ_INIT_OMX_PORT_STRUCT (*ap_aactype, ARATELIA_AAC_DECODER_INPUT_PORT_INDEX);
  tiz_check_omx_err (tiz_api_GetParameter (tiz_get_krn (handleOf (p_prc)),
                                           handleOf (p_prc),
                                           OMX_IndexParamAudioAac, ap_aactype));
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE set_decoder_config (aacdec_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorInsufficientResources;
  NeAACDecConfigurationPtr p_config;
  OMX_AUDIO_PARAM_AACPROFILETYPE aactype;

  assert (NULL != ap_prc);
  assert (NULL != ap_prc->p_aac_dec_);

  /* Retrieve the aac settings from the input port */
  tiz_check_omx_err (retrieve_aac_settings (ap_prc, &aactype));

  /* Set the default object type and samplerate */
  /* This is useful for RAW AAC files */
  p_config = NeAACDecGetCurrentConfiguration (ap_prc->p_aac_dec_);
  if (NULL != p_config)
    {
      p_config->defSampleRate = aactype.nSampleRate;
      p_config->defObjectType = aactype.eAACProfile;
      p_config->outputFormat
          = FAAD_FMT_16BIT;           /* we making this fixed for now */
      p_config->downMatrix = 1;       /* Down matrix 5.1 to 2 channels */
      p_config->useOldADTSFormat = 0; /* we making this fixed for now */
      /* config->dontUpSampleImplicitSBR = 1; */

      /* 0 == not OK */
      if (!NeAACDecSetConfiguration (ap_prc->p_aac_dec_, p_config))
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "[%s] : While setting the decoder config "
                     "(nSampleRate %d, eAACProfile %d).",
                     tiz_err_to_str (rc), aactype.nSampleRate,
                     aactype.eAACProfile);
        }
      else
        {
          /* all good */
          rc = OMX_ErrorNone;
        }
    }

  return rc;
}

static OMX_ERRORTYPE init_aac_decoder (aacdec_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorInsufficientResources;
  long nbytes = 0;
  OMX_BUFFERHEADERTYPE *p_in = tiz_filter_prc_get_header (
      ap_prc, ARATELIA_AAC_DECODER_INPUT_PORT_INDEX);

  assert (NULL != ap_prc);
  assert (NULL != ap_prc->p_aac_dec_);
  assert (NULL != p_in);

  if (tiz_buffer_store_data (ap_prc->p_store_, p_in->pBuffer + p_in->nOffset,
                             p_in->nFilledLen) < p_in->nFilledLen)
    {
      TIZ_ERROR (handleOf (ap_prc), "[%s] : Unable to store all the data.",
                 tiz_err_to_str (rc));
      return rc;
    }
  p_in->nFilledLen = 0;

  /* Skip the ID3 tag */
  skip_id3_tag (ap_prc);

  /* Set the decoder configuration according to the configuration found on the
     input port
     (useful in case of raw aac files) */
  tiz_check_omx_err (set_decoder_config (ap_prc));

  /* Retrieve againg the pointer to the current position in the buffer
     (skip_id3_tag may have modified it */

  /* Initialise the library using one of the initialization functions */
  nbytes = NeAACDecInit (ap_prc->p_aac_dec_,
                         tiz_buffer_get_data (ap_prc->p_store_),
                         tiz_buffer_bytes_available (ap_prc->p_store_),
                         &(ap_prc->samplerate_), &(ap_prc->channels_));

  if (nbytes < 0)
    {
      TIZ_ERROR (handleOf (ap_prc),
                 "[%s] : libfaad decoder initialisation failure (%s)",
                 tiz_err_to_str (rc), NeAACDecGetErrorMessage (nbytes));
    }
  else
    {
      /* We will skip this many bytes the next time we read from this buffer */
      tiz_buffer_advance (ap_prc->p_store_, nbytes);
      TIZ_DEBUG (handleOf (ap_prc), "samplerate [%d] channels [%d]",
                 ap_prc->samplerate_, (int)ap_prc->channels_);
      rc = OMX_ErrorNone;
    }
  return rc;
}

static OMX_ERRORTYPE transform_buffer (aacdec_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *p_in = tiz_filter_prc_get_header (
      ap_prc, ARATELIA_AAC_DECODER_INPUT_PORT_INDEX);
  OMX_BUFFERHEADERTYPE *p_out = tiz_filter_prc_get_header (
      ap_prc, ARATELIA_AAC_DECODER_OUTPUT_PORT_INDEX);

  if (NULL == p_in || NULL == p_out)
    {
      TIZ_TRACE (handleOf (ap_prc), "IN HEADER [%p] OUT HEADER [%p]", p_in,
                 p_out);
      return OMX_ErrorNone;
    }

  assert (NULL != ap_prc);
  assert (NULL != ap_prc->p_aac_dec_);

  if (0 == p_in->nFilledLen && tiz_buffer_bytes_available (ap_prc->p_store_)
                               == 0)
    {
      TIZ_TRACE (handleOf (ap_prc), "HEADER [%p] nFlags [%d] is empty", p_in,
                 p_in->nFlags);
      if ((p_in->nFlags & OMX_BUFFERFLAG_EOS) > 0)
        {
          /* Inmediately propagate EOS flag to output */
          TIZ_TRACE (handleOf (ap_prc),
                     "Propagate EOS flag to output HEADER [%p]", p_out);
          p_out->nFlags |= OMX_BUFFERFLAG_EOS;
          tiz_filter_prc_update_eos_flag (ap_prc, true);
          p_in->nFlags = 0;
          tiz_check_omx_err (tiz_filter_prc_release_header (
              ap_prc, ARATELIA_AAC_DECODER_OUTPUT_PORT_INDEX));
        }
    }

  if (p_in->nFilledLen > 0)
    {
      if (tiz_buffer_store_data (
              ap_prc->p_store_, p_in->pBuffer + p_in->nOffset, p_in->nFilledLen)
          < p_in->nFilledLen)
        {
          rc = OMX_ErrorInsufficientResources;
          TIZ_ERROR (handleOf (ap_prc), "[%s] : Unable to store all the data.",
                     tiz_err_to_str (rc));
          return rc;
        }
      p_in->nFilledLen = 0;
    }

  if (tiz_buffer_bytes_available (ap_prc->p_store_) > 0)
    {
      /* Decode the AAC data passed in the buffer. Returns a pointer to a
         sample buffer or NULL. Info about the decoded frame is filled in the
         NeAACDecFrameInfo structure. This structure holds information about
         errors during decoding, number of sample, number of channels and
         samplerate. The returned buffer contains the channel interleaved
         samples of the frame. */
      short *p_sample_buf
          = NeAACDecDecode (ap_prc->p_aac_dec_, &(ap_prc->aac_info_),
                            tiz_buffer_get_data (ap_prc->p_store_),
                            tiz_buffer_bytes_available (ap_prc->p_store_));

      TIZ_TRACE (handleOf (ap_prc),
                 "bytes_available = [%d] bytesconsumed = [%d] "
                 "samples = [%d] error [%d]",
                 tiz_buffer_bytes_available (ap_prc->p_store_),
                 ap_prc->aac_info_.bytesconsumed, ap_prc->aac_info_.samples,
                 ap_prc->aac_info_.error);

      tiz_buffer_advance (ap_prc->p_store_, ap_prc->aac_info_.bytesconsumed);

      if ((ap_prc->aac_info_.error == 0) && (ap_prc->aac_info_.samples > 0))
        {
          int i = 0;
          char *p_data = (char *)(p_out->pBuffer + p_out->nOffset);
          for (i = 0; i < ap_prc->aac_info_.samples; ++i)
            {
              p_data[i * 2] = (char)(p_sample_buf[i] & 0xFF);
              p_data[i * 2 + 1] = (char)((p_sample_buf[i] >> 8) & 0xFF);
            }
          p_out->nFilledLen = ap_prc->aac_info_.samples * ap_prc->channels_ * 1;
        }
      else if (ap_prc->aac_info_.error != 0)
        {
          /* Some error occurred while decoding this frame */
          TIZ_ERROR (handleOf (ap_prc),
                     "[OMX_ErrorStreamCorruptFatal] : "
                     "While decoding the input stream (%s).",
                     NeAACDecGetErrorMessage (ap_prc->aac_info_.error));
          rc = OMX_ErrorStreamCorruptFatal;
        }
    }

  if (OMX_ErrorNone == rc && 0 == p_in->nFilledLen
      && tiz_buffer_bytes_available (ap_prc->p_store_) < FAAD_MIN_STREAMSIZE
                                                         * ap_prc->channels_)
    {
      TIZ_TRACE (handleOf (ap_prc), "HEADER [%p] nFlags [%d] is empty", p_in,
                 p_in->nFlags);
      if ((p_in->nFlags & OMX_BUFFERFLAG_EOS) > 0)
        {
          /* Let's propagate EOS flag to output */
          TIZ_TRACE (handleOf (ap_prc), "Let's propagate EOS flag to output");
          tiz_filter_prc_update_eos_flag (ap_prc, true);
          p_in->nFlags = 0;
        }
      rc = tiz_filter_prc_release_header (
          ap_prc, ARATELIA_AAC_DECODER_INPUT_PORT_INDEX);
    }

  if (tiz_filter_prc_is_eos (ap_prc))
    {
      /* Propagate EOS flag to output */
      p_out->nFlags |= OMX_BUFFERFLAG_EOS;
      tiz_filter_prc_update_eos_flag (ap_prc, false);
      TIZ_TRACE (handleOf (ap_prc), "Propagating EOS flag to output");
    }

  if (OMX_ErrorNone == rc && p_out->nFilledLen > 0)
    {
      TIZ_TRACE (handleOf (ap_prc),
                 "Releasing output HEADER [%p] nFilledLen [%d]", p_out,
                 p_out->nFilledLen);
      rc = tiz_filter_prc_release_header (
          ap_prc, ARATELIA_AAC_DECODER_OUTPUT_PORT_INDEX);
    }

  return rc;
}

static void reset_stream_parameters (aacdec_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  tiz_mem_set (&(ap_prc->aac_info_), 0, sizeof(ap_prc->aac_info_));
  ap_prc->samplerate_ = 0;
  ap_prc->channels_ = 0;
  ap_prc->nbytes_read_ = 0;
  ap_prc->first_buffer_read_ = false;
  tiz_filter_prc_update_eos_flag (ap_prc, false);
}

/*
 * aacdecprc
 */

static void *aacdec_prc_ctor (void *ap_obj, va_list *app)
{
  aacdec_prc_t *p_prc = super_ctor (typeOf (ap_obj, "aacdecprc"), ap_obj, app);
  assert (NULL != p_prc);
  unsigned long cap = NeAACDecGetCapabilities ();
  TIZ_DEBUG (handleOf (ap_obj), "libfaad2 caps: %X", cap);
  /*   Open the faad library */
  p_prc->p_aac_dec_ = NeAACDecOpen ();
  reset_stream_parameters (p_prc);
  p_prc->p_store_ = NULL;
  p_prc->store_size_ = 0;
  p_prc->store_offset_ = 0;
  return p_prc;
}

static void *aacdec_prc_dtor (void *ap_obj)
{
  aacdec_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  (void)aacdec_prc_deallocate_resources (p_prc);
  if (NULL != p_prc->p_aac_dec_)
    {
      NeAACDecClose (p_prc->p_aac_dec_);
      p_prc->p_aac_dec_ = NULL;
    }
  return super_dtor (typeOf (p_prc, "aacdecprc"), p_prc);
}

/*
 * from tizsrv class
 */

static OMX_ERRORTYPE aacdec_prc_allocate_resources (void *ap_obj, OMX_U32 a_pid)
{
  aacdec_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  tiz_check_omx_err (alloc_temp_data_store (p_prc));
  /* Check that the library has been successfully inited */
  tiz_check_null_ret_oom ((p_prc->p_aac_dec_));
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE aacdec_prc_deallocate_resources (void *ap_obj)
{
  dealloc_temp_data_store (ap_obj);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE aacdec_prc_prepare_to_transfer (void *ap_obj,
                                                     OMX_U32 a_pid)
{
  aacdec_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  reset_stream_parameters (p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE aacdec_prc_transfer_and_process (void *ap_obj,
                                                      OMX_U32 a_pid)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE aacdec_prc_stop_and_return (void *ap_obj)
{
  return tiz_filter_prc_release_all_headers (ap_obj);
}

/*
 * from tizprc class
 */

static OMX_ERRORTYPE aacdec_prc_buffers_ready (const void *ap_prc)
{
  aacdec_prc_t *p_prc = (aacdec_prc_t *)ap_prc;
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (NULL != ap_prc);

  TIZ_TRACE (handleOf (p_prc), "eos [%s] ",
             tiz_filter_prc_is_eos (p_prc) ? "YES" : "NO");
  while (tiz_filter_prc_headers_available (p_prc) && OMX_ErrorNone == rc)
    {
      if (!p_prc->first_buffer_read_)
        {
          rc = init_aac_decoder (p_prc);
          if (OMX_ErrorNone == rc)
            {
              p_prc->first_buffer_read_ = true;
            }
        }
      else
        {
          rc = transform_buffer (p_prc);
        }
    }

  return rc;
}

static OMX_ERRORTYPE aacdec_prc_port_enable (const void *ap_prc, OMX_U32 a_pid)
{
  aacdec_prc_t *p_prc = (aacdec_prc_t *)ap_prc;
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, false);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE aacdec_prc_port_disable (const void *ap_prc, OMX_U32 a_pid)
{
  aacdec_prc_t *p_prc = (aacdec_prc_t *)ap_prc;
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  if (OMX_ALL == a_pid)
    {
      rc = tiz_filter_prc_release_all_headers (p_prc);
    }
  else
    {
      rc = tiz_filter_prc_release_header (p_prc, a_pid);
    }
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, true);
  return rc;
}

/*
 * aacdec_prc_class
 */

static void *aacdec_prc_class_ctor (void *ap_obj, va_list *app)
{
  /* NOTE: Class methods might be added in the future. None for now. */
  return super_ctor (typeOf (ap_obj, "aacdecprc_class"), ap_obj, app);
}

/*
 * initialization
 */

void *aacdec_prc_class_init (void *ap_tos, void *ap_hdl)
{
  void *tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void *aacdecprc_class = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (classOf (tizfilterprc), "aacdecprc_class", classOf (tizfilterprc),
       sizeof(aacdec_prc_class_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, aacdec_prc_class_ctor,
       /* TIZ_CLASS_COMMENT: stop value*/
       0);
  return aacdecprc_class;
}

void *aacdec_prc_init (void *ap_tos, void *ap_hdl)
{
  void *tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void *aacdecprc_class = tiz_get_type (ap_hdl, "aacdecprc_class");
  TIZ_LOG_CLASS (aacdecprc_class);
  void *aacdecprc = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (aacdecprc_class, "aacdecprc", tizfilterprc, sizeof(aacdec_prc_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, aacdec_prc_ctor,
       /* TIZ_CLASS_COMMENT: class destructor */
       dtor, aacdec_prc_dtor,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_allocate_resources, aacdec_prc_allocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_deallocate_resources, aacdec_prc_deallocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_prepare_to_transfer, aacdec_prc_prepare_to_transfer,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_transfer_and_process, aacdec_prc_transfer_and_process,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_stop_and_return, aacdec_prc_stop_and_return,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_buffers_ready, aacdec_prc_buffers_ready,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_enable, aacdec_prc_port_enable,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_disable, aacdec_prc_port_disable,
       /* TIZ_CLASS_COMMENT: stop value */
       0);

  return aacdecprc;
}