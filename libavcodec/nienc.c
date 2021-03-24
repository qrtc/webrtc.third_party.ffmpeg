/*
 * NetInt XCoder H.264/HEVC Encoder common code
 * Copyright (c) 2018-2019 NetInt
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#ifdef __linux__
#include <unistd.h>
#endif
#include "nienc.h"
#include "libavcodec/put_bits.h"
#include "libavcodec/golomb.h"

static ni_enc_avc_roi_custom_map_t *g_avc_roi_map = NULL;
// H.265 test roi buffer for up to 8k resolution H.265 - 32 x 32 sub CTUs
static uint8_t *g_hevc_sub_ctu_roi_buf = NULL;
// H.265 custom map buffer for up to 8k resolution  - 64x64 CTU Regions
static ni_enc_hevc_roi_custom_map_t *g_hevc_roi_map = NULL;

// NETINT_INTERNAL - currently only for internal testing
//static ni_encoder_change_params_t *g_enc_change_params = NULL;

//extern const char * const g_xcoder_preset_names[3];
//extern const char * const g_xcoder_log_names[7];
#define ODD2EVEN(X) ((X&1)&&(X>31))?(X+1):(X)
#ifdef _WIN32
//Just enable this macro in windows. This feature will open the device in encode init. So the first will take less time.
#define NI_ENCODER_OPEN_DEVICE 1
#endif

#ifdef NIENC_MULTI_THREAD
threadpool_t pool;
int sessionCounter = 0;

typedef struct _write_thread_arg_struct_t
{
  pthread_mutex_t mutex; //mutex
  pthread_cond_t cond;   //cond
  int running;
  XCoderH265EncContext *ctx;
  ni_retcode_t ret;
}write_thread_arg_struct_t;
#endif

static void setVui(AVCodecContext *avctx, ni_encoder_params_t *p_param,
                   enum AVColorPrimaries color_primaries,
                   enum AVColorTransferCharacteristic color_trc,
                   enum AVColorSpace color_space)
{
  int isHEVC = (AV_CODEC_ID_HEVC == avctx->codec_id ? 1 : 0);
  PutBitContext pbcPutBitContext;
  unsigned int aspect_ratio_idc = 255; // default: extended_sar
  init_put_bits(&pbcPutBitContext, p_param->ui8VuiRbsp, NI_MAX_VUI_SIZE);

  put_bits(&pbcPutBitContext, 1, 1);  //  aspect_ratio_info_present_flag=1
  if (0 == avctx->sample_aspect_ratio.num ||
      ! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(1, 1)))
  {
    aspect_ratio_idc = 1;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(12, 11)))
  {
    aspect_ratio_idc = 2;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(10, 11)))
  {
    aspect_ratio_idc = 3;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(16, 11)))
  {
    aspect_ratio_idc = 4;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(40, 33)))
  {
    aspect_ratio_idc = 5;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(24, 11)))
  {
    aspect_ratio_idc = 6;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(20, 11)))
  {
    aspect_ratio_idc = 7;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(32, 11)))
  {
    aspect_ratio_idc = 8;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(80, 33)))
  {
    aspect_ratio_idc = 9;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(18, 11)))
  {
    aspect_ratio_idc = 10;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(15, 11)))
  {
    aspect_ratio_idc = 11;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(64, 33)))
  {
    aspect_ratio_idc = 12;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(160, 99)))
  {
    aspect_ratio_idc = 13;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(4, 3)))
  {
    aspect_ratio_idc = 14;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(3, 2)))
  {
    aspect_ratio_idc = 15;
  }
  else if (! av_cmp_q(avctx->sample_aspect_ratio, av_make_q(2, 1)))
  {
    aspect_ratio_idc = 16;
  }

  put_bits(&pbcPutBitContext, 8, aspect_ratio_idc);  // aspect_ratio_idc
  if (255 == aspect_ratio_idc)
  {
    put_bits(&pbcPutBitContext, 16, avctx->sample_aspect_ratio.num);//sar_width
    put_bits(&pbcPutBitContext, 16, avctx->sample_aspect_ratio.den);//sar_height
  }

  put_bits(&pbcPutBitContext, 1, 0);  //  overscan_info_present_flag=0

  // VUI Parameters
  put_bits(&pbcPutBitContext, 1, 1);  //  video_signal_type_present_flag=1
  put_bits(&pbcPutBitContext, 3, 5);  //  video_format=5 (unspecified)
  put_bits(&pbcPutBitContext, 1, 0);  //  video_full_range_flag=0
  put_bits(&pbcPutBitContext, 1, 1);  //  colour_description_presenty_flag=1
  put_bits(&pbcPutBitContext, 8, color_primaries);
  put_bits(&pbcPutBitContext, 8, color_trc);
  put_bits(&pbcPutBitContext, 8, color_space);

  put_bits(&pbcPutBitContext, 1, 0);      //  chroma_loc_info_present_flag=0

  if (isHEVC)
  {   // H.265 Only VUI parameters
    put_bits(&pbcPutBitContext, 1, 0);  //  neutral_chroma_indication_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  field_seq_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  frame_field_info_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  default_display_window_flag=0
  }

  put_bits(&pbcPutBitContext, 1, 1);      //  vui_timing_info_present_flag=1
  p_param->pos_num_units_in_tick     = put_bits_count(&pbcPutBitContext);
  put_bits32(&pbcPutBitContext, 0);    //  vui_num_units_in_tick
  p_param->pos_time_scale     = put_bits_count(&pbcPutBitContext);
  put_bits32(&pbcPutBitContext, 0);         //  vui_time_scale

  if (isHEVC)
  {   // H.265 Only VUI parameters
    put_bits(&pbcPutBitContext, 1, 0);  //  vui_poc_proportional_to_timing_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  vui_hrd_parameters_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  bitstream_restriction_flag=0
  }
  else
  {   // H.264 Only VUI parameters
    put_bits(&pbcPutBitContext, 1, 1);  //  fixed_frame_rate_flag=1
    put_bits(&pbcPutBitContext, 1, 0);  //  nal_hrd_parameters_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  vui_hrd_parameters_present_flag=0
    put_bits(&pbcPutBitContext, 1, 0);  //  pic_struct_present_flag=0

    // this flag is set to 1 for H.264 to reduce decode delay, and fill in
    // the rest of the section accordingly
    put_bits(&pbcPutBitContext, 1, 1);  //  bitstream_restriction_flag=1
    put_bits(&pbcPutBitContext, 1, 1);  //  motion_vectors_over_pic_boundaries_flag=1
    set_ue_golomb_long(&pbcPutBitContext, 2); // max_bytes_per_pic_denom=2 (default)
    set_ue_golomb_long(&pbcPutBitContext, 1); // max_bits_per_mb_denom=1 (default)
    set_ue_golomb_long(&pbcPutBitContext, 15); // log2_max_mv_length_horizontal=15 (default)
    set_ue_golomb_long(&pbcPutBitContext, 15); // log2_max_mv_length_vertical=15 (default)

    // max_num_reorder_frames (0 for low delay gops)
    int max_num_reorder_frames = ni_get_num_reorder_of_gop_structure(p_param);
    set_ue_golomb_long(&pbcPutBitContext, max_num_reorder_frames);
    // max_dec_frame_buffering
    int num_ref_frames = ni_get_num_ref_frame_of_gop_structure(p_param);
    int max_dec_frame_buffering = (num_ref_frames > max_num_reorder_frames ?
                                   num_ref_frames : max_num_reorder_frames);
    set_ue_golomb_long(&pbcPutBitContext, max_dec_frame_buffering);
  }

  p_param->ui32VuiDataSizeBits     = put_bits_count(&pbcPutBitContext);
  p_param->ui32VuiDataSizeBytes    = (p_param->ui32VuiDataSizeBits+7)/8;
  flush_put_bits(&pbcPutBitContext);      // flush bits
}

static int xcoder_encoder_headers(AVCodecContext *avctx)
{
  // use a copy of encoder context, take care to restore original config
  // cropping setting
  XCoderH265EncContext ctx;
  memcpy(&ctx, (XCoderH265EncContext *)(avctx->priv_data),
         sizeof(XCoderH265EncContext));

  ni_encoder_params_t *p_param = (ni_encoder_params_t *)ctx.api_ctx.p_session_config;

  int orig_conf_win_right = p_param->hevc_enc_params.conf_win_right;
  int orig_conf_win_bottom = p_param->hevc_enc_params.conf_win_bottom;

  int linesize_aligned = ((avctx->width + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    linesize_aligned = ((avctx->width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_MIN_WIDTH)
  {
    p_param->hevc_enc_params.conf_win_right += NI_MIN_WIDTH - avctx->width;
    linesize_aligned = NI_MIN_WIDTH;
  }
  else if (linesize_aligned > avctx->width)
  {
    p_param->hevc_enc_params.conf_win_right += linesize_aligned - avctx->width;
  }
  p_param->source_width = linesize_aligned;

  int height_aligned = ((avctx->height + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264) {
    height_aligned = ((avctx->height + 15) / 16) * 16;
  }

  if (height_aligned < NI_MIN_HEIGHT)
  {
    p_param->hevc_enc_params.conf_win_bottom += NI_MIN_HEIGHT - avctx->height;
    p_param->source_height = NI_MIN_HEIGHT;
    height_aligned = NI_MIN_HEIGHT;
  }
  else if (height_aligned > avctx->height)
  {
    p_param->hevc_enc_params.conf_win_bottom += height_aligned - avctx->height;
    if (avctx->codec_id != AV_CODEC_ID_H264)
    {
      p_param->source_height = height_aligned;
    }
  }

  // set color metrics
  enum AVColorPrimaries color_primaries = avctx->color_primaries;
  enum AVColorTransferCharacteristic color_trc = avctx->color_trc;
  enum AVColorSpace color_space = avctx->colorspace;

  if (color_primaries == AVCOL_PRI_BT2020 ||
      color_trc == AVCOL_TRC_SMPTE2084 ||
      color_trc == AVCOL_TRC_ARIB_STD_B67 ||
      color_space == AVCOL_SPC_BT2020_NCL ||
      color_space == AVCOL_SPC_BT2020_CL)
  {
    p_param->hdrEnableVUI = 1;
    setVui(avctx, p_param, color_primaries, color_trc, color_space);
    av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
           "color_trc: %d  color_space %d sar %d/%d\n",
           color_primaries, color_trc, color_space,
           avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
  }
  else
  {
    p_param->hdrEnableVUI = 0;
    setVui(avctx, p_param, color_primaries, color_trc, color_space);
  }

  int ret = -1;
  ctx.api_ctx.hw_id = ctx.dev_enc_idx;
  strcpy(ctx.api_ctx.dev_xcoder, ctx.dev_xcoder);
  ret = ni_device_session_open(&ctx.api_ctx, NI_DEVICE_TYPE_ENCODER);

  ctx.dev_xcoder = ctx.api_ctx.dev_xcoder_name;
  ctx.blk_xcoder = ctx.api_ctx.blk_xcoder_name;
  ctx.dev_enc_idx = ctx.api_ctx.hw_id;

  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
           "resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;

    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s.%d (inst: %d) opened successfully\n",
           ctx.dev_xcoder, ctx.dev_enc_idx, ctx.api_ctx.session_id);
  }

  int recv;
  ni_packet_t *xpkt = &ctx.api_pkt.data.packet;
  ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ);

  while (1)
  {
    recv = ni_device_session_read(&ctx.api_ctx, &(ctx.api_pkt),
                                  NI_DEVICE_TYPE_ENCODER);

    if (recv > 0)
    {
      free(avctx->extradata);
      avctx->extradata_size = recv - NI_FW_ENC_BITSTREAM_META_DATA_SIZE;
      avctx->extradata = av_mallocz(avctx->extradata_size +
                                    AV_INPUT_BUFFER_PADDING_SIZE);
      memcpy(avctx->extradata,
             (uint8_t*)xpkt->p_data + NI_FW_ENC_BITSTREAM_META_DATA_SIZE,
             avctx->extradata_size);

      av_log(avctx, AV_LOG_VERBOSE, "Xcoder encoder headers len: %d\n",
             avctx->extradata_size);
      break;
    }
    else if (recv == NI_RETCODE_SUCCESS)
    {
      continue;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "Xcoder encoder headers error: %d", recv);
      break;
    }
  }

  // close and clean up the temporary session
  ret = ni_device_session_close(&ctx.api_ctx, ctx.encoder_eof,
                                NI_DEVICE_TYPE_ENCODER);
#ifdef _WIN32
  ni_device_close(ctx.api_ctx.device_handle);
#elif __linux__
  ni_device_close(ctx.api_ctx.device_handle);
  ni_device_close(ctx.api_ctx.blk_io_handle);
#endif
  ctx.api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  ctx.api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  ni_packet_buffer_free( &(ctx.api_pkt.data.packet) );

  ni_rsrc_free_device_context(ctx.rsrc_ctx);
  ctx.rsrc_ctx = NULL;

  p_param->hevc_enc_params.conf_win_right = orig_conf_win_right;
  p_param->hevc_enc_params.conf_win_bottom = orig_conf_win_bottom;

  return (recv < 0 ? recv : ret);
}

static int xcoder_setup_encoder(AVCodecContext *avctx)
{
  XCoderH265EncContext *s = avctx->priv_data;
  int ret = 0;
  ni_encoder_params_t *p_param = &s->api_param;
  ni_encoder_params_t *pparams = NULL;
  ni_session_run_state_t prev_state = s->api_ctx.session_run_state;
 
  av_log(avctx, AV_LOG_DEBUG, "XCoder setup device encoder\n");
  //s->api_ctx.session_id = NI_INVALID_SESSION_ID;
  ni_device_session_context_init(&(s->api_ctx));
  s->api_ctx.session_run_state = prev_state;
  
  s->api_ctx.codec_format = NI_CODEC_FORMAT_H264;
  if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    s->api_ctx.codec_format = NI_CODEC_FORMAT_H265;
  }
  s->firstPktArrived = 0;
  s->spsPpsArrived = 0;
  s->spsPpsHdrLen = 0;
  s->p_spsPpsHdr = NULL;
  s->xcode_load_pixel = 0;
  s->reconfigCount = 0;
  s->gotPacket = 0;
  s->sentFrame = 0;
  s->latest_dts = 0;

  if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != s->api_ctx.session_run_state)
  {
    av_log(avctx, AV_LOG_INFO, "Session state: %d allocate frame fifo.\n",
           s->api_ctx.session_run_state);
    s->fme_fifo = av_fifo_alloc(sizeof(AVFrame));
  }
  else
  {
    av_log(avctx, AV_LOG_INFO, "Session seq change, fifo size: %lu.\n",
           av_fifo_size(s->fme_fifo) / sizeof(AVFrame));
  }

  if (! s->fme_fifo)
  {
    return AVERROR(ENOMEM);
  }
  s->eos_fme_received = 0;
  s->buffered_fme = av_frame_alloc();
  if (! s->buffered_fme)
  {
    return AVERROR(ENOMEM);
  }

  //Xcoder User Configuration
  ret = ni_encoder_init_default_params(p_param, avctx->time_base.den, (avctx->time_base.num * avctx->ticks_per_frame), avctx->bit_rate, ODD2EVEN(avctx->width), ODD2EVEN(avctx->height));

  if (ret == NI_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_RETCODE_PARAM_ERROR_AREA_TOO_BIG)
  {
      av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width x Height: exceeds %d\n", NI_MAX_RESOLUTION_AREA);
      return AVERROR_EXTERNAL;
  }
  if (ret < 0)
  {

    int i;
    av_log(avctx, AV_LOG_ERROR, "Error setting preset or log.\n");
    av_log(avctx, AV_LOG_INFO, "Possible presets:");
    for (i = 0; g_xcoder_preset_names[i]; i++)
      av_log(avctx, AV_LOG_INFO, " %s", g_xcoder_preset_names[i]);
    av_log(avctx, AV_LOG_INFO, "\n");

    av_log(avctx, AV_LOG_INFO, "Possible log:");
    for (i = 0; g_xcoder_log_names[i]; i++)
      av_log(avctx, AV_LOG_INFO, " %s", g_xcoder_log_names[i]);
    av_log(avctx, AV_LOG_INFO, "\n");

    return AVERROR(EINVAL);
  }

  switch (avctx->pix_fmt)
  {
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUV420P10:
  case AV_PIX_FMT_YUV420P12:
    break;
  case AV_PIX_FMT_YUV422P:
  case AV_PIX_FMT_YUV422P10:
  case AV_PIX_FMT_YUV422P12:
  case AV_PIX_FMT_GBRP:
  case AV_PIX_FMT_GBRP10:
  case AV_PIX_FMT_GBRP12:
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUV444P10:
  case AV_PIX_FMT_YUV444P12:
  case AV_PIX_FMT_GRAY8:
  case AV_PIX_FMT_GRAY10:
  case AV_PIX_FMT_GRAY12:
    return AVERROR_INVALIDDATA;
  default:
    break;
  }

  if (s->xcoder_opts)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_opts, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_encoder_params_set_value(p_param, en->key, en->value);

        switch (parse_ret)
        {
          case NI_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: out of range\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_ZERO:
            av_log(avctx, AV_LOG_ERROR, "Error setting option %s to value 0\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_GOP_INTRA_INCOMPATIBLE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s incompatible with GOP structure.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);
    }
  }

  if (s->xcoder_gop)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_gop, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_encoder_gop_params_set_value(p_param, en->key, en->value);

        switch (parse_ret)
        {
          case NI_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s out of range \n", en->key);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_ERROR_ZERO:
             av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP paramaters: Error setting option %s to value 0 \n", en->key);
             return AVERROR_EXTERNAL;
          case NI_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for GOP param %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);
    }
  }
  if (s->nvme_io_size > 0 && s->nvme_io_size % 4096 != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Error XCoder iosize is not 4KB aligned!\n");
    return AVERROR_EXTERNAL;
  }

  s->api_ctx.p_session_config = &s->api_param;
  pparams = (ni_encoder_params_t *)s->api_ctx.p_session_config;
  switch (pparams->hevc_enc_params.gop_preset_index)
  {
    /* dtsOffset is the max number of non-reference frames in a GOP
     * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
     * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
     * thus we ensure pts>dts in all cases
     * */
    case 1 /*PRESET_IDX_ALL_I*/:
    case 2 /*PRESET_IDX_IPP*/:
    case 3 /*PRESET_IDX_IBBB*/:
    case 6 /*PRESET_IDX_IPPPP*/:
    case 7 /*PRESET_IDX_IBBBB*/:
    case 9 /*PRESET_IDX_SP*/:
      s->dtsOffset = 0;
      break;
    case 4 /*PRESET_IDX_IBPBP*/:
      s->dtsOffset = 1;
      break;
    case 5 /*PRESET_IDX_IBBBP*/:
      s->dtsOffset = 2;
      break;
    case 8 /*PRESET_IDX_RA_IB*/:
      s->dtsOffset = 3;
      break;
    default:
      // TBD need user to specify offset
      s->dtsOffset = 7;
      av_log(avctx, AV_LOG_VERBOSE, "dts offset default to 7, TBD\n");
      break;
  }
  if (1 == pparams->force_frame_type)
  {
    s->dtsOffset = 7;
  }

  s->total_frames_received = 0;
  s->gop_offset_count = 0;
  av_log(avctx, AV_LOG_INFO, "dts offset: %lld, gop_offset_count: %d\n",
        s->dtsOffset, s->gop_offset_count);

  if (0 == strcmp(s->dev_xcoder, LIST_DEVICES_STR))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder: printing out all xcoder devices and their load, and exit ...\n");
    ni_rsrc_print_all_devices_capability();
    return AVERROR_EXIT;
  }

  //overwrite the nvme io size here with a custom value if it was provided
  if (s->nvme_io_size > 0)
  {
    s->api_ctx.max_nvme_io_size = s->nvme_io_size;
    av_log(avctx, AV_LOG_VERBOSE, "Custom NVMEe IO Size set to = %d\n", s->api_ctx.max_nvme_io_size);
  }
  s->encoder_eof = 0;
  avctx->bit_rate = pparams->bitrate;

  s->api_ctx.src_bit_depth = 8;
  s->api_ctx.src_endian = NI_FRAME_LITTLE_ENDIAN;
  s->api_ctx.roi_len = 0;
  s->api_ctx.roi_avg_qp = 0;
  s->api_ctx.bit_depth_factor = 1;
  if (AV_PIX_FMT_YUV420P10BE == avctx->pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
    s->api_ctx.src_bit_depth = 10;
    if (AV_PIX_FMT_YUV420P10BE == avctx->pix_fmt)
    {
      s->api_ctx.src_endian = NI_FRAME_BIG_ENDIAN;
    }
  }

  memset( &(s->api_fme), 0, sizeof(ni_session_data_io_t) );
  memset( &(s->api_pkt), 0, sizeof(ni_session_data_io_t) );
  
  if (avctx->width > 0 && avctx->height > 0)
  {  
    ni_frame_buffer_alloc(&(s->api_fme.data.frame), ODD2EVEN(avctx->width), ODD2EVEN(avctx->height), 0, 0, s->api_ctx.bit_depth_factor);
  }

  // generate encoded bitstream headers in advance if configured to do so
  if (pparams->generate_enc_hdrs)
  {
    ret = xcoder_encoder_headers(avctx);
  }

  return ret;
}

#if NI_ENCODER_OPEN_DEVICE
static int xcoder_open_encoder_device(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_encoder_params_t *p_param = (ni_encoder_params_t *)ctx->api_ctx.p_session_config; // NETINT_INTERNAL - currently only for internal testing

  int frame_width = 0;
  int frame_height = 0;
  frame_width = ODD2EVEN(avctx->width);
  frame_height = ODD2EVEN(avctx->height);

  // if frame stride size is not as we expect it,
  // adjust using xcoder-params conf_win_right
  int linesize_aligned = ((avctx->width + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    linesize_aligned = ((avctx->width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_MIN_WIDTH)
  {
    p_param->hevc_enc_params.conf_win_right += NI_MIN_WIDTH - frame_width;
    linesize_aligned = NI_MIN_WIDTH;
  }
  else if (linesize_aligned > frame_width)
  {
    p_param->hevc_enc_params.conf_win_right += linesize_aligned - frame_width;
  }
  p_param->source_width = linesize_aligned;

  int height_aligned = ((frame_height + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264) {
    height_aligned = ((frame_height + 15) / 16) * 16;
  }

  if (height_aligned < NI_MIN_HEIGHT)
  {
    p_param->hevc_enc_params.conf_win_bottom += NI_MIN_HEIGHT - frame_height;
    p_param->source_height = NI_MIN_HEIGHT;
    height_aligned = NI_MIN_HEIGHT;
  }
  else if (height_aligned > frame_height)
  {
    p_param->hevc_enc_params.conf_win_bottom += height_aligned - frame_height;

    p_param->source_height = height_aligned;
  }

  //Force frame color metrics if specified in command line
  enum AVColorPrimaries color_primaries;// = frame->color_primaries;
  enum AVColorTransferCharacteristic color_trc;// = frame->color_trc;
  enum AVColorSpace color_space;// = frame->colorspace;

  //if ((frame->color_primaries != avctx->color_primaries) && (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED))
  {
    color_primaries = avctx->color_primaries;
  }
  //if ((frame->color_trc != avctx->color_trc) && (avctx->color_trc != AVCOL_TRC_UNSPECIFIED))
  {
    color_trc = avctx->color_trc;
  }
  //if ((frame->colorspace != avctx->colorspace) && (avctx->colorspace != AVCOL_SPC_UNSPECIFIED))
  {
    color_space = avctx->colorspace;
  }

  // HDR HLG support
  if (color_primaries == AVCOL_PRI_BT2020 ||
      color_trc == AVCOL_TRC_SMPTE2084 ||
      color_trc == AVCOL_TRC_ARIB_STD_B67 ||
      color_space == AVCOL_SPC_BT2020_NCL ||
      color_space == AVCOL_SPC_BT2020_CL)
  {
    p_param->hdrEnableVUI = 1;
    setVui(avctx, p_param, color_primaries, color_trc, color_space);
    av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
           "color_trc: %d  color_space %d sar %d/%d\n",
           color_primaries, color_trc, color_space,
           avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
  }
  else
  {
    p_param->hdrEnableVUI = 0;
    setVui(avctx, p_param, color_primaries, color_trc, color_space);
  }


  av_log(avctx, AV_LOG_VERBOSE, "XCoder frame->linesize: %d/%d/%d frame width/height %dx%d"
                                " conf_win_right %d  conf_win_bottom %d \n",
         frame->linesize[0], frame->linesize[1], frame->linesize[2],
         frame->width, frame->height,
         p_param->hevc_enc_params.conf_win_right, p_param->hevc_enc_params.conf_win_bottom);
  
  int ret = -1;
  ctx->api_ctx.hw_id = ctx->dev_enc_idx;
  // User disabled this feature fromm the command line
  
  if (ctx->yuv_copy_bypass != true)
  {
    ctx->api_ctx.yuv_copy_bypass = false;
  }
  else
  {
    ctx->api_ctx.yuv_copy_bypass = true;
  }
  av_log(avctx, AV_LOG_DEBUG, "yuv_copy_bypass from user:%d yuv_copy_bypass:%d\n", ctx->yuv_copy_bypass, ctx->api_ctx.yuv_copy_bypass);

  int ret = -1;
  ctx->api_ctx.hw_id = ctx->dev_enc_idx;
  ctx->api_ctx.yuv_copy_bypass = false;
  strcpy(ctx->api_ctx.dev_xcoder, ctx->dev_xcoder);
  ret = ni_device_session_open(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);
  // // As the file handle may change we need to assign back 
  ctx->dev_xcoder = ctx->api_ctx.dev_xcoder_name;
  ctx->blk_xcoder = ctx->api_ctx.blk_xcoder_name;
  ctx->dev_enc_idx = ctx->api_ctx.hw_id;

  if (ret == NI_RETCODE_INVALID_PARAM)
  {
    av_log(avctx, AV_LOG_ERROR, "%s\n", ctx->api_ctx.param_err_msg);
  }
  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
           "resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;
    // xcoder_encode_close(avctx); will be called at codec close
    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
           ctx->dev_xcoder, ctx->dev_enc_idx, ctx->api_ctx.session_id);
  }

  // set up ROI map if in ROI demo mode
  if (p_param->hevc_enc_params.roi_enable &&
      (1 == p_param->roi_demo_mode || 2 == p_param->roi_demo_mode))
  {
    int i,j, sumQp = 0, ctu;
    // mode 1: Set QP for center 1/3 of picture to highest - lowest quality
    // the rest to lowest - highest quality;
    // mode non-1: reverse of mode 1
    int importanceLevelCentre = p_param->roi_demo_mode == 1 ? 40 : 10;
    int importanceLevelRest = p_param->roi_demo_mode == 1 ? 10 : 40;

    // make the QP map size 16-aligned to meet VPU requirement for subsequent
    // SEI due to layout of data sent to encoder
    if (avctx->codec_id == AV_CODEC_ID_H264)
    {
      // roi for H.264 is specified for 16x16 pixel macroblocks - 1 MB
      // is stored in each custom map entry
  
      // number of MBs in each row
      uint32_t mbWidth = (linesize_aligned + 16 - 1) >> 4;
      // number of MBs in each column
      uint32_t mbHeight = (height_aligned + 16 - 1) >> 4;
      uint32_t numMbs = mbWidth * mbHeight;
      uint32_t customMapSize = sizeof(ni_enc_avc_roi_custom_map_t) * numMbs;
      customMapSize = ((customMapSize + 15) / 16) * 16;
      g_avc_roi_map = (ni_enc_avc_roi_custom_map_t*)calloc(1, customMapSize);
      if (! g_avc_roi_map)
      {
        return AVERROR(ENOMEM);
      }
  
      // copy roi MBs QPs into custom map
      for (i = 0; i < numMbs; i++)
      {
        if ((i % mbWidth > mbWidth/3) && (i % mbWidth < mbWidth*2/3))
        {
          g_avc_roi_map[i].field.mb_qp = importanceLevelCentre;
        }
        else
        {
          g_avc_roi_map[i].field.mb_qp = importanceLevelRest;
        }
        sumQp += g_avc_roi_map[i].field.mb_qp;
      }
      ctx->api_ctx.roi_len = customMapSize;
      ctx->api_ctx.roi_avg_qp = (sumQp + (numMbs>>1)) / numMbs; // round off
    }
    else if (avctx->codec_id == AV_CODEC_ID_HEVC)
    {
      // roi for H.265 is specified for 32x32 pixel subCTU blocks - 4
      // subCTU QPs are stored in each custom CTU map entry
  
      // number of CTUs in each row
      uint32_t ctuWidth = (linesize_aligned + 64 -1) >> 6;
      // number of CTUs in each column
      uint32_t ctuHeight = (height_aligned + 64 - 1) >> 6;
      // number of sub CTUs in each row
      uint32_t subCtuWidth = ctuWidth * 2;
      // number of CTUs in each column
      uint32_t subCtuHeight = ctuHeight * 2;
      uint32_t numSubCtus = subCtuWidth * subCtuHeight;
      uint32_t customMapSize = sizeof(ni_enc_hevc_roi_custom_map_t) *
      ctuWidth * ctuHeight;
      customMapSize = ((customMapSize + 15) / 16) * 16;
  
      g_hevc_sub_ctu_roi_buf = (uint8_t *)malloc(numSubCtus);
      if (! g_hevc_sub_ctu_roi_buf)
      {
        return AVERROR(ENOMEM);
      }
      for (i = 0; i < numSubCtus; i++)
      {
        if ((i % subCtuWidth > subCtuWidth/3) &&
            (i % subCtuWidth < subCtuWidth*2/3))
        {
          g_hevc_sub_ctu_roi_buf[i] = importanceLevelCentre;
        }
        else
        {
          g_hevc_sub_ctu_roi_buf[i] = importanceLevelRest;
        }
      }
      g_hevc_roi_map = (ni_enc_hevc_roi_custom_map_t *)calloc(1, customMapSize);
      if (! g_hevc_roi_map)
      {
        return AVERROR(ENOMEM);
      }
  
      for (i = 0; i < ctuHeight; i++)
      {
        uint8_t *ptr = &g_hevc_sub_ctu_roi_buf[subCtuWidth * i * 2];
        for (j = 0; j < ctuWidth; j++, ptr += 2)
        {
          ctu = i * ctuWidth + j;
          g_hevc_roi_map[ctu].field.sub_ctu_qp_0 = *ptr;
          g_hevc_roi_map[ctu].field.sub_ctu_qp_1 = *(ptr + 1);
          g_hevc_roi_map[ctu].field.sub_ctu_qp_2 = *(ptr + subCtuWidth);
          g_hevc_roi_map[ctu].field.sub_ctu_qp_3 = *(ptr + subCtuWidth + 1);
          sumQp += (g_hevc_roi_map[ctu].field.sub_ctu_qp_0 +
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_1 +
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_2 +
                    g_hevc_roi_map[ctu].field.sub_ctu_qp_3);
        }
      }
      ctx->api_ctx.roi_len = customMapSize;
      ctx->api_ctx.roi_avg_qp = (sumQp + (numSubCtus>>1)) / numSubCtus; // round off.
    }
  }

  return ret;
}
#endif

av_cold int xcoder_encode_init(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;
  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_DEBUG, "XCoder encode init\n");

  if (ctx->api_ctx.session_run_state == SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    ctx->dev_enc_idx = ctx->orig_dev_enc_idx;
    ctx->dev_xcoder = ctx->orig_dev_xcoder;
  }
  else
  {
    ctx->orig_dev_enc_idx = ctx->dev_enc_idx;
    strncpy(ctx->orig_dev_xcoder, ctx->dev_xcoder, MAX_DEVICE_NAME_LEN - 1);
    ctx->orig_dev_xcoder[MAX_DEVICE_NAME_LEN - 1] = '\0';
  }

  av_log(avctx, AV_LOG_VERBOSE, "XCoder options: dev_xcoder: %s dev_enc_idx %d\n",
         ctx->dev_xcoder, ctx->dev_enc_idx);

#ifdef NIENC_MULTI_THREAD
  if (sessionCounter == 0)
  {
    threadpool_init(&pool);
  }
  sessionCounter++;
#endif

  if ((ret = xcoder_setup_encoder(avctx)) < 0)
  {
    xcoder_encode_close(avctx);
    return ret;
  }

#if NI_ENCODER_OPEN_DEVICE
  if ((ret = xcoder_open_encoder_device(avctx)) < 0)
  {
    xcoder_encode_close(avctx);
    return ret;
  }
#endif

  return 0;
}

int xcoder_encode_close(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_retcode_t ret = NI_RETCODE_FAILURE;

#ifdef NIENC_MULTI_THREAD
  sessionCounter--;
  if (sessionCounter == 0)
  {
    threadpool_destroy(&pool);
  }
#endif 

  ret = ni_device_session_close(&ctx->api_ctx, ctx->encoder_eof, NI_DEVICE_TYPE_ENCODER);
  if (NI_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to close Encoder Session (status = %d)\n", ret);
  }
#ifdef _WIN32
  ni_device_close(ctx->api_ctx.device_handle);
#elif __linux__
  ni_device_close(ctx->api_ctx.device_handle);
  ni_device_close(ctx->api_ctx.blk_io_handle);
#endif
  ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

  av_log(avctx, AV_LOG_DEBUG, "XCoder encode close (status = %d\n", ret);
  ni_frame_buffer_free( &(ctx->api_fme.data.frame) );
  ni_packet_buffer_free( &(ctx->api_pkt.data.packet) );

  av_log(avctx, AV_LOG_DEBUG, "fifo size: %lu\n",
         av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  if (ctx->api_ctx.session_run_state != SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    av_fifo_free(ctx->fme_fifo);
    av_log(avctx, AV_LOG_DEBUG, " , freed.\n");
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, " , kept.\n");
  }
  av_frame_free(&ctx->buffered_fme);

  ctx->dev_xcoder = NULL; //Set to Null, else av_opt_free in avcodec_close will try to free it and crash as "string literal" cannot be freed.

  ni_rsrc_free_device_context(ctx->rsrc_ctx);
  ctx->rsrc_ctx = NULL;

  free(g_avc_roi_map);
  g_avc_roi_map = NULL;
  free(g_hevc_sub_ctu_roi_buf);
  g_hevc_sub_ctu_roi_buf = NULL;
  free(g_hevc_roi_map);
  g_hevc_roi_map = NULL;
  free(ctx->g_enc_change_params);
  ctx->g_enc_change_params = NULL;
  free(ctx->p_spsPpsHdr);
  ctx->p_spsPpsHdr = NULL;
  ctx->started = 0;

  return 0;
}

static int xcoder_encode_reset(AVCodecContext *avctx)
{
  XCoderH265EncContext *ctx = avctx->priv_data;

  av_log(avctx, AV_LOG_WARNING, "XCoder encode reset\n");

  xcoder_encode_close(avctx);

  ctx->dev_xcoder = ctx->api_ctx.dev_xcoder_name;

  return xcoder_encode_init(avctx);
}

// frame fifo operations
static int is_input_fifo_empty(XCoderH265EncContext *s)
{
  if (av_fifo_size(s->fme_fifo) < sizeof(AVFrame))
  {
    return 1;
  }
  return 0;
}

static int queue_fme(AVCodecContext *avctx, const AVFrame *inframe)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;

  // expand frame buffer fifo if not enough space
  if (av_fifo_space(ctx->fme_fifo) < sizeof(AVFrame))
  {
    ret = av_fifo_realloc2(ctx->fme_fifo,
                           av_fifo_size(ctx->fme_fifo) + sizeof(AVFrame));
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Enc av_fifo_realloc2 NO MEMORY !!!\n");
      return ret;
    }
    if ((av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame) % 100) == 0)
    {
      av_log(avctx, AV_LOG_INFO, "Enc fifo being extended to: %lu\n",
             av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
    }
    av_assert0(0 == av_fifo_size(ctx->fme_fifo) % sizeof(AVFrame));
  }

  AVFrame *input_fme = av_frame_alloc();
  ret = av_frame_ref(input_fme, inframe);

  if (ret < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Enc av_frame_ref input_fme ERROR !!!\n");
    return ret;
  }
  av_fifo_generic_write(ctx->fme_fifo, input_fme, sizeof(*input_fme), NULL);
  av_log(avctx, AV_LOG_DEBUG, "fme queued, fifo size: %lu\n",
         av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  return ret;
}

#ifdef NIENC_MULTI_THREAD
static void* write_frame_thread(void* arg)
{
  write_thread_arg_struct_t *args = (write_thread_arg_struct_t *) arg;
  XCoderH265EncContext *ctx = args->ctx;
  int ret;
  int sent;

  pthread_mutex_lock(&args->mutex);
  args->running = 1;
  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: session_id %d, device_handle %d\n", ctx->api_ctx.session_id, ctx->api_ctx.device_handle);

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: ctx %p\n", ctx);
  
  sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);

  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: size %d sent to xcoder\n", sent);
  if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
  {
    av_log(ctx, AV_LOG_DEBUG, "write_frame_thread(): Sequence Change in progress, returning EAGAIN\n");
    ret = AVERROR(EAGAIN);
  }
  else if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    sent = xcoder_encode_reset(ctx);
  }

  if (sent < 0)
  {
    ret = AVERROR(EIO);
  }
  else
  {
    //pushing input pts in circular FIFO
    ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
    ctx->api_ctx.enc_pts_w_idx ++;
    ret = 0;
  }

  args->ret = ret;
  av_log(ctx, AV_LOG_DEBUG, "write_frame_thread: ret %d\n", ret);
  pthread_cond_signal(&args->cond);
  args->running = 0;
  pthread_mutex_unlock(&args->mutex);
  return NULL;
}
#endif

int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret = 0;
  int sent;
  int i;
  int plane_idx = 0;
  int orig_avctx_width = avctx->width, orig_avctx_height = avctx->height;
  AVFrameSideData *side_data;
  uint8_t *cc_data = NULL;
  int cc_size = 0;
  uint8_t *udu_sei = NULL;
  int udu_sei_size = 0;
  int ext_udu_sei_size = 0;

#ifdef NIENC_MULTI_THREAD
  av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 000 %p, session_id %d, device_handle %d\n", ctx->api_ctx.session_info, ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
  if ((ctx->api_ctx.session_id != NI_INVALID_SESSION_ID) && (ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE))
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 000 %p\n", ctx->api_ctx.session_info);
    if (ctx->api_ctx.session_info != NULL)
    {
      write_thread_arg_struct_t *write_thread_args = (write_thread_arg_struct_t *)ctx->api_ctx.session_info;
      pthread_mutex_lock(&write_thread_args->mutex);
      av_log(avctx, AV_LOG_DEBUG, "thread start waiting session_id %d\n", ctx->api_ctx.session_id);
      if (write_thread_args->running == 1)
      {
        pthread_cond_wait(&write_thread_args->cond, &write_thread_args->mutex);
        av_log(avctx, AV_LOG_DEBUG, "thread get waiting session_id %d\n", ctx->api_ctx.session_id);
      }
      if (ctx->api_ctx.yuv_copy_bypass == true)
      {
        av_frame_unref(&(ctx->frame_hold));
      }
      if (write_thread_args->ret == AVERROR(EAGAIN))
      {
        av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: ret %d\n", write_thread_args->ret);
        pthread_mutex_unlock(&write_thread_args->mutex);
        free(write_thread_args);
        ctx->api_ctx.session_info = NULL;
        return AVERROR(EAGAIN);
      }
      pthread_mutex_unlock(&write_thread_args->mutex);
      free(write_thread_args);
      ctx->api_ctx.session_info = NULL;
      av_log(avctx, AV_LOG_DEBUG, "thread free session_id %d\n", ctx->api_ctx.session_id);
    }
  }
#endif
  int frame_width = 0;
  int frame_height = 0;
  if (frame){
      frame_width = ODD2EVEN(frame->width);
      frame_height = ODD2EVEN(frame->height);
  }
  ni_encoder_params_t *p_param = (ni_encoder_params_t *)ctx->api_ctx.p_session_config; // NETINT_INTERNAL - currently only for internal testing
  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
#if NI_ENCODER_OPEN_DEVICE
  if ((frame && ctx->started == 0) && 
      ((frame->width != avctx->width) ||
      (frame->height != avctx->height) ||
      (frame->color_primaries != avctx->color_primaries) ||
      (frame->color_trc != avctx->color_trc) ||
      (frame->colorspace != avctx->colorspace)))
#else
  if (frame && ctx->started == 0)
#endif
  {
#if NI_ENCODER_OPEN_DEVICE
    av_log(avctx, AV_LOG_INFO, "WARNING reopen device Width: %d-%d, Height: %d-%d, color_primaries: %d-%d, color_trc: %d-%d, color_space: %d-%d\n",
         frame->width, avctx->width,
         frame->height, avctx->height,
         frame->color_primaries, avctx->color_primaries,
         frame->colorspace, avctx->colorspace);
    // close and clean up the temporary session
    ret = ni_device_session_close(&ctx->api_ctx, ctx->encoder_eof,
                                    NI_DEVICE_TYPE_ENCODER);
#ifdef _WIN32
    ni_device_close(ctx->api_ctx.device_handle);
#elif __linux__
    ni_device_close(ctx.api_ctx.device_handle);
    ni_device_close(ctx.api_ctx.blk_io_handle);
#endif
    ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
    ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;

    // Errror when set this parameters in ni_encoder_params_set_value !!!!!!
    p_param->hevc_enc_params.conf_win_right = 0;
    p_param->hevc_enc_params.conf_win_bottom = 0;
  
#endif

    // if frame stride size is not as we expect it,
    // adjust using xcoder-params conf_win_right
    int linesize_aligned = ((frame_width + 7) / 8) * 8;
    if (avctx->codec_id == AV_CODEC_ID_H264)
    {
      linesize_aligned = ((frame_width + 15) / 16) * 16;
    }

    if (linesize_aligned < NI_MIN_WIDTH)
    {
      p_param->hevc_enc_params.conf_win_right += NI_MIN_WIDTH  - frame_width;
      linesize_aligned = NI_MIN_WIDTH;
    }
    else if (linesize_aligned > frame_width)
    {
      p_param->hevc_enc_params.conf_win_right += linesize_aligned - frame_width;
    }
    p_param->source_width = linesize_aligned;

    int height_aligned = ((frame_height + 7) / 8) * 8;
    if (avctx->codec_id == AV_CODEC_ID_H264) {
      height_aligned = ((frame_height + 15) / 16) * 16;
    }

    if (height_aligned < NI_MIN_HEIGHT)
    {
      p_param->hevc_enc_params.conf_win_bottom += NI_MIN_HEIGHT - frame_height;
      p_param->source_height = NI_MIN_HEIGHT;
      height_aligned = NI_MIN_HEIGHT;
    }
    else if (height_aligned > frame_height)
    {
      p_param->hevc_enc_params.conf_win_bottom += height_aligned - frame_height;

      p_param->source_height = height_aligned;
    }

    //Force frame color metrics if specified in command line
    enum AVColorPrimaries color_primaries = frame->color_primaries;
    enum AVColorTransferCharacteristic color_trc = frame->color_trc;
    enum AVColorSpace color_space = frame->colorspace;

    if ((frame->color_primaries != avctx->color_primaries) && (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED))
    {
      color_primaries = avctx->color_primaries;
    }
    if ((frame->color_trc != avctx->color_trc) && (avctx->color_trc != AVCOL_TRC_UNSPECIFIED))
    {
      color_trc = avctx->color_trc;
    }
    if ((frame->colorspace != avctx->colorspace) && (avctx->colorspace != AVCOL_SPC_UNSPECIFIED))
    {
      color_space = avctx->colorspace;
    }
    // HDR HLG support
    if (color_primaries == AVCOL_PRI_BT2020 ||
        color_trc == AVCOL_TRC_SMPTE2084 ||
        color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        color_space == AVCOL_SPC_BT2020_NCL ||
        color_space == AVCOL_SPC_BT2020_CL)
    {
      p_param->hdrEnableVUI = 1;
      setVui(avctx, p_param, color_primaries, color_trc, color_space);
      av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
             "color_trc: %d  color_space %d sar %d/%d\n",
             color_primaries, color_trc, color_space,
             avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
    }
    else
    {
      p_param->hdrEnableVUI = 0;
      setVui(avctx, p_param, color_primaries, color_trc, color_space);
    }


    av_log(avctx, AV_LOG_VERBOSE, "XCoder frame->linesize: %d/%d/%d frame width/height %dx%d"
                                  " conf_win_right %d  conf_win_bottom %d \n",
           frame->linesize[0], frame->linesize[1], frame->linesize[2],
           frame->width, frame->height,
           p_param->hevc_enc_params.conf_win_right, p_param->hevc_enc_params.conf_win_bottom);

    int ret = -1;
    ctx->api_ctx.hw_id = ctx->dev_enc_idx;
    // User disabled this feature fromm the command line

    if (ctx->yuv_copy_bypass != true)
    {
      ctx->api_ctx.yuv_copy_bypass = false;
    }
    else
    {
      ctx->api_ctx.yuv_copy_bypass = true;
    }
    av_log(avctx, AV_LOG_DEBUG, "yuv_copy_bypass from user:%d yuv_copy_bypass:%d\n", ctx->yuv_copy_bypass, ctx->api_ctx.yuv_copy_bypass);

    
    
    strcpy(ctx->api_ctx.dev_xcoder, ctx->dev_xcoder);

    ret = ni_device_session_open(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);

    // // As the file handle may change we need to assign back 
    ctx->dev_xcoder = ctx->api_ctx.dev_xcoder_name;
    ctx->blk_xcoder = ctx->api_ctx.blk_xcoder_name;
    ctx->dev_enc_idx = ctx->api_ctx.hw_id;


    if (ret == NI_RETCODE_INVALID_PARAM)
    {
      av_log(avctx, AV_LOG_ERROR, "%s\n", ctx->api_ctx.param_err_msg);
    }
    if (ret != 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
             "resource unavailable\n", ret);
      ret = AVERROR_EXTERNAL;
      // xcoder_encode_close(avctx); will be called at codec close
      return ret;
    }
    else
    {
      av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
             ctx->dev_xcoder, ctx->dev_enc_idx, ctx->api_ctx.session_id);
    }

    // set up ROI map if in ROI demo mode
    if (p_param->hevc_enc_params.roi_enable &&
        (1 == p_param->roi_demo_mode || 2 == p_param->roi_demo_mode))
    {
      int i,j, sumQp = 0, ctu;
      // mode 1: Set QP for center 1/3 of picture to highest - lowest quality
      // the rest to lowest - highest quality;
      // mode non-1: reverse of mode 1
      // make the QP map size 16-aligned to meet VPU requirement for subsequent
      // SEI due to layout of data sent to encoder
      int importanceLevelCentre = p_param->roi_demo_mode == 1 ? 40 : 10;
      int importanceLevelRest = p_param->roi_demo_mode == 1 ? 10 : 40;
      if (avctx->codec_id == AV_CODEC_ID_H264)
      {
        // roi for H.264 is specified for 16x16 pixel macroblocks - 1 MB
        // is stored in each custom map entry

        // number of MBs in each row
        uint32_t mbWidth = (linesize_aligned + 16 - 1) >> 4;
        // number of MBs in each column
        uint32_t mbHeight = (height_aligned + 16 - 1) >> 4;
        uint32_t numMbs = mbWidth * mbHeight;
        uint32_t customMapSize = sizeof(ni_enc_avc_roi_custom_map_t) * numMbs;
        customMapSize = ((customMapSize + 15) / 16) * 16;

        g_avc_roi_map = (ni_enc_avc_roi_custom_map_t*)calloc(1, customMapSize);
        if (! g_avc_roi_map)
        {
          return AVERROR(ENOMEM);
        }

        // copy roi MBs QPs into custom map
        for (i = 0; i < numMbs; i++)
        {
          if ((i % mbWidth > mbWidth/3) && (i % mbWidth < mbWidth*2/3))
          {
            g_avc_roi_map[i].field.mb_qp = importanceLevelCentre;
          }
          else
          {
            g_avc_roi_map[i].field.mb_qp = importanceLevelRest;
          }
          sumQp += g_avc_roi_map[i].field.mb_qp;
        }
        ctx->api_ctx.roi_len = customMapSize;
        ctx->api_ctx.roi_avg_qp = (sumQp + (numMbs>>1)) / numMbs; // round off
      }
      else if (avctx->codec_id == AV_CODEC_ID_HEVC)
      {
        // roi for H.265 is specified for 32x32 pixel subCTU blocks - 4
        // subCTU QPs are stored in each custom CTU map entry

        // number of CTUs in each row
        uint32_t ctuWidth = (linesize_aligned + 64 -1) >> 6;
        // number of CTUs in each column
        uint32_t ctuHeight = (height_aligned + 64 - 1) >> 6;
        // number of sub CTUs in each row
        uint32_t subCtuWidth = ctuWidth * 2;
        // number of CTUs in each column
        uint32_t subCtuHeight = ctuHeight * 2;
        uint32_t numSubCtus = subCtuWidth * subCtuHeight;
        uint32_t customMapSize = sizeof(ni_enc_hevc_roi_custom_map_t) *
        ctuWidth * ctuHeight;
        customMapSize = ((customMapSize + 15) / 16) * 16;

        g_hevc_sub_ctu_roi_buf = (uint8_t *)malloc(numSubCtus);
        if (! g_hevc_sub_ctu_roi_buf)
        {
          return AVERROR(ENOMEM);
        }
        for (i = 0; i < numSubCtus; i++)
        {
          if ((i % subCtuWidth > subCtuWidth/3) &&
              (i % subCtuWidth < subCtuWidth*2/3))
          {
            g_hevc_sub_ctu_roi_buf[i] = importanceLevelCentre;
          }
          else
          {
            g_hevc_sub_ctu_roi_buf[i] = importanceLevelRest;
          }
        }
        g_hevc_roi_map = (ni_enc_hevc_roi_custom_map_t *)
        calloc(1, customMapSize);
        if (! g_hevc_roi_map)
        {
          return AVERROR(ENOMEM);
        }

        for (i = 0; i < ctuHeight; i++)
        {
          uint8_t *ptr = &g_hevc_sub_ctu_roi_buf[subCtuWidth * i * 2];
          for (j = 0; j < ctuWidth; j++, ptr += 2)
          {
            ctu = i * ctuWidth + j;
            g_hevc_roi_map[ctu].field.sub_ctu_qp_0 = *ptr;
            g_hevc_roi_map[ctu].field.sub_ctu_qp_1 = *(ptr + 1);
            g_hevc_roi_map[ctu].field.sub_ctu_qp_2 = *(ptr + subCtuWidth);
            g_hevc_roi_map[ctu].field.sub_ctu_qp_3 = *(ptr + subCtuWidth + 1);
            sumQp += (g_hevc_roi_map[ctu].field.sub_ctu_qp_0 +
                      g_hevc_roi_map[ctu].field.sub_ctu_qp_1 +
                      g_hevc_roi_map[ctu].field.sub_ctu_qp_2 +
                      g_hevc_roi_map[ctu].field.sub_ctu_qp_3);
          }
        }
        ctx->api_ctx.roi_len = customMapSize;
        ctx->api_ctx.roi_avg_qp = (sumQp + (numSubCtus>>1)) / numSubCtus; // round off.
      }
    }
    
  }

  av_log(avctx, AV_LOG_DEBUG, "XCoder send frame, pkt_size %d\n",
         frame ? frame->pkt_size : -1);
#if 0  
  if (frame)
    av_log(avctx, AV_LOG_DEBUG, "*** NI enc In avframe pts: %lld  pkt_dts : %lld  best_effort : %lld \n", frame->pts, frame->pkt_dts, frame->best_effort_timestamp);
#endif

  if (ctx->encoder_flushing)
  {
    if (! frame && is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "XCoder EOF: null frame && fifo empty\n");
      return AVERROR_EOF;
    }
  }

  if (! frame)
  {
    ctx->eos_fme_received = 1;
    av_log(avctx, AV_LOG_DEBUG, "null frame, eos_fme_received = 1\n");
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder send frame #%"PRIu64"\n",
           ctx->api_ctx.frame_num);

    // queue up the frame if fifo is NOT empty, or: sequence change ongoing !
    if (! is_input_fifo_empty(ctx) ||
        SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      queue_fme(avctx, frame);

      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder doing sequence change, frame #%"PRIu64" "
               "queued and return 0 !\n", ctx->api_ctx.frame_num);
        return 0;
      }
    }
    else
    {
      ret = av_frame_ref(ctx->buffered_fme, frame);
    }
  }

  if (ctx->started == 0)
  {
    ctx->api_fme.data.frame.start_of_stream = 1;
    ctx->started = 1;
  }
  else
  {
    ctx->api_fme.data.frame.start_of_stream = 0;
  }

  if (is_input_fifo_empty(ctx))
  {
    av_log(avctx, AV_LOG_DEBUG,
           "no frame in fifo to send, just send/receive ..\n");
    if (ctx->eos_fme_received)
    {
      av_log(avctx, AV_LOG_DEBUG, "no frame in fifo to send, send eos ..\n");
      //      ctx->buffered_fme->pkt_size = 0;
    }
  }
  else
  {
    av_fifo_generic_peek(ctx->fme_fifo, ctx->buffered_fme,
                         sizeof(AVFrame), NULL);
  }

  if (0 == frame_width || 0 == frame_height)
  {
    frame_width = ODD2EVEN(ctx->buffered_fme->width);
    frame_height = ODD2EVEN(ctx->buffered_fme->height);
  }

  if ((ctx->buffered_fme->height && ctx->buffered_fme->width) &&
      (ctx->buffered_fme->height != avctx->height ||
       ctx->buffered_fme->width != avctx->width))
  {
    av_log(avctx, AV_LOG_INFO, "xcoder_send_frame resolution change %dx%d "
           "-> %dx%d\n", avctx->width, avctx->height,
           ctx->buffered_fme->width, ctx->buffered_fme->height);
    ctx->api_ctx.session_run_state = SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
    ctx->eos_fme_received = 1;

    // have to queue this frame if not done so: an empty queue
    if (is_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_TRACE, "resolution change when fifo empty, frame "
             "#%"PRIu64" being queued ..\n", ctx->api_ctx.frame_num);
      av_frame_unref(ctx->buffered_fme);
      queue_fme(avctx, frame);
    }
  }

  ctx->api_fme.data.frame.preferred_characteristics_data_len = 0;
  ctx->api_fme.data.frame.end_of_stream = 0;
  ctx->api_fme.data.frame.force_key_frame
  = ctx->api_fme.data.frame.use_cur_src_as_long_term_pic
  = ctx->api_fme.data.frame.use_long_term_ref = 0;

  ctx->api_fme.data.frame.sei_total_len 
  = ctx->api_fme.data.frame.sei_cc_offset = ctx->api_fme.data.frame.sei_cc_len
  = ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_offset
  = ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len
  = ctx->api_fme.data.frame.sei_hdr_content_light_level_info_offset
  = ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len
  = ctx->api_fme.data.frame.sei_hdr_plus_offset
  = ctx->api_fme.data.frame.sei_hdr_plus_len = 0;

  ctx->api_fme.data.frame.roi_len = 0;
  ctx->api_fme.data.frame.reconf_len = 0;
  ctx->api_fme.data.frame.force_pic_qp = 0;

  if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state ||
      (ctx->eos_fme_received && is_input_fifo_empty(ctx)))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder start flushing\n");
    ctx->api_fme.data.frame.end_of_stream = 1;
    ctx->encoder_flushing = 1;
  }
  else
  {
    // NETINT_INTERNAL - currently only for internal testing
    // allocate memory for reconf parameters only once and reuse it
    if (! ctx->g_enc_change_params)
    {
      ctx->g_enc_change_params = calloc(1, sizeof(ni_encoder_change_params_t));
    }

    ctx->api_fme.data.frame.extra_data_len = NI_APP_ENC_FRAME_META_DATA_SIZE;

    ctx->g_enc_change_params->enable_option = 0;
    ctx->api_fme.data.frame.reconf_len = 0;
    switch (p_param->reconf_demo_mode)
    {

      case XCODER_TEST_RECONF_BR:
        if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
        {
          ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_RC_TARGET_RATE;
          ctx->g_enc_change_params->bitRate = p_param->reconf_hash[ctx->reconfigCount][1];
          ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
          ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
          ctx->reconfigCount ++;
        }
        break;
      case XCODER_TEST_RECONF_INTRAPRD:
        if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
        {
          ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_INTRA_PARAM;
          ctx->g_enc_change_params->intraQP =
          p_param->reconf_hash[ctx->reconfigCount][1];
          ctx->g_enc_change_params->intraPeriod =
          p_param->reconf_hash[ctx->reconfigCount][2];
          ctx->g_enc_change_params->repeatHeaders =
          p_param->reconf_hash[ctx->reconfigCount][3];
          av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
                 "intraQP %d intraPeriod %d repeatHeaders %d\n",
                 ctx->api_ctx.frame_num, ctx->g_enc_change_params->intraQP,
                 ctx->g_enc_change_params->intraPeriod,
                 ctx->g_enc_change_params->repeatHeaders);

          ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
          ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
          ctx->reconfigCount ++;
        }
        break;
    case XCODER_TEST_RECONF_LONG_TERM_REF:
      // the reconf file data line format for this is:
      // <frame-number>:useCurSrcAsLongtermPic,useLongtermRef where
      // values will stay the same on every frame until changed.
      if (ctx->api_ctx.frame_num >= p_param->reconf_hash[ctx->reconfigCount][0])
      {
        AVFrameSideData *ltr_sd;
        AVNetintLongTermRef *p_ltr;
        ltr_sd = av_frame_new_side_data(ctx->buffered_fme,
                                        AV_FRAME_DATA_NETINT_LONG_TERM_REF,
                                        sizeof(AVNetintLongTermRef));
        if (ltr_sd)
        {
          p_ltr = (AVNetintLongTermRef *)ltr_sd->data;
          p_ltr->use_cur_src_as_long_term_pic
          = p_param->reconf_hash[ctx->reconfigCount][1];
          p_ltr->use_long_term_ref
          = p_param->reconf_hash[ctx->reconfigCount][2];
        }
      }
      if (ctx->api_ctx.frame_num + 1 ==
          p_param->reconf_hash[ctx->reconfigCount + 1][0])
      {
        ctx->reconfigCount ++;
      }
      break;
    case XCODER_TEST_RECONF_VUI_HRD:
      // the reconf file format for this is:
      // <frame-number>:<vui-file-name-in-digits>,<number-of-bits-of-vui-rbsp>
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        char file_name[64];
        snprintf(file_name, 64, "%d",
                 p_param->reconf_hash[ctx->reconfigCount][1]);
        FILE *vui_file = fopen(file_name, "rb");
        if (! vui_file)
        {
          av_log(avctx, AV_LOG_ERROR, "Error VUI reconf file: %s\n", file_name);
        }
        else
        {
          int nb_bytes_by_bits =
          (p_param->reconf_hash[ctx->reconfigCount][2] + 7) / 8;
          size_t nb_bytes = fread(ctx->g_enc_change_params->vuiRbsp,
                                  1, NI_MAX_VUI_SIZE, vui_file);
          if (nb_bytes != nb_bytes_by_bits)
          {
            av_log(avctx, AV_LOG_ERROR, "Error VUI file size %d bytes != "
                   "specified %d bits (%d bytes) !\n", nb_bytes,
                   p_param->reconf_hash[ctx->reconfigCount][2], nb_bytes_by_bits);
          }
          else
          {
            ctx->g_enc_change_params->enable_option =
            NI_SET_CHANGE_PARAM_VUI_HRD_PARAM;
            ctx->g_enc_change_params->encodeVuiRbsp = 1;
            ctx->g_enc_change_params->vuiDataSizeBits =
            p_param->reconf_hash[ctx->reconfigCount][2];
            ctx->g_enc_change_params->vuiDataSizeBytes = nb_bytes;
            av_log(avctx, AV_LOG_DEBUG, "Reconf VUI %d bytes (%d bits)\n",
                   nb_bytes, p_param->reconf_hash[ctx->reconfigCount][2]);

            ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
            ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
            ctx->reconfigCount++;
          }

          fclose(vui_file);
        }
      }
      break;
    case XCODER_TEST_RECONF_RC:
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_RC;
        ctx->g_enc_change_params->hvsQPEnable =
        p_param->reconf_hash[ctx->reconfigCount][1];
        ctx->g_enc_change_params->hvsQpScale =
        p_param->reconf_hash[ctx->reconfigCount][2];
        ctx->g_enc_change_params->vbvBufferSize =
        p_param->reconf_hash[ctx->reconfigCount][3];
        ctx->g_enc_change_params->mbLevelRcEnable =
        p_param->reconf_hash[ctx->reconfigCount][4];
        ctx->g_enc_change_params->fillerEnable =
        p_param->reconf_hash[ctx->reconfigCount][5];
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
               "hvsQPEnable %d hvsQpScale %d vbvBufferSize %d mbLevelRcEnable "
               "%d fillerEnable %d\n",
               ctx->api_ctx.frame_num, ctx->g_enc_change_params->hvsQPEnable,
               ctx->g_enc_change_params->hvsQpScale,
               ctx->g_enc_change_params->vbvBufferSize,
               ctx->g_enc_change_params->mbLevelRcEnable,
               ctx->g_enc_change_params->fillerEnable);

        ctx->api_fme.data.frame.extra_data_len +=
        sizeof(ni_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
        ctx->reconfigCount ++;
      }
      break;
    case XCODER_TEST_RECONF_RC_MIN_MAX_QP:
      if (ctx->api_ctx.frame_num == p_param->reconf_hash[ctx->reconfigCount][0])
      {
        ctx->g_enc_change_params->enable_option = NI_SET_CHANGE_PARAM_RC_MIN_MAX_QP;
        ctx->g_enc_change_params->minQpI =
        p_param->reconf_hash[ctx->reconfigCount][1];
        ctx->g_enc_change_params->maxQpI =
        p_param->reconf_hash[ctx->reconfigCount][2];
        ctx->g_enc_change_params->maxDeltaQp =
        p_param->reconf_hash[ctx->reconfigCount][3];
        ctx->g_enc_change_params->minQpP =
        p_param->reconf_hash[ctx->reconfigCount][4];
        ctx->g_enc_change_params->minQpB =
        p_param->reconf_hash[ctx->reconfigCount][5];
        ctx->g_enc_change_params->maxQpP =
        p_param->reconf_hash[ctx->reconfigCount][6];
        ctx->g_enc_change_params->maxQpB =
        p_param->reconf_hash[ctx->reconfigCount][7];
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame #%lu reconf "
               "minQpI %d maxQpI %d maxDeltaQp %d minQpP "
               "%d minQpB %d maxQpP %d maxQpB %d\n",
               ctx->api_ctx.frame_num, ctx->g_enc_change_params->minQpI,
               ctx->g_enc_change_params->maxQpI,
               ctx->g_enc_change_params->maxDeltaQp,
               ctx->g_enc_change_params->minQpP,
               ctx->g_enc_change_params->minQpB,
               ctx->g_enc_change_params->maxQpP,
               ctx->g_enc_change_params->maxQpB);

        ctx->api_fme.data.frame.extra_data_len +=
        sizeof(ni_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
        ctx->reconfigCount ++;
      }
      break;

      case XCODER_TEST_RECONF_OFF:
      default:
        ;
    }

    // NetInt long term reference frame support
    side_data = av_frame_get_side_data(ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_LONG_TERM_REF);
    if (side_data && (side_data->size == sizeof(AVNetintLongTermRef)))
    {
      AVNetintLongTermRef *ltr = (const AVNetintLongTermRef*)side_data->data;

      ctx->api_fme.data.frame.use_cur_src_as_long_term_pic
      = ltr->use_cur_src_as_long_term_pic;
      ctx->api_fme.data.frame.use_long_term_ref
      = ltr->use_long_term_ref;
    }

    // NetInt target bitrate reconfiguration support
    side_data = av_frame_get_side_data(ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_BITRATE);
    if (side_data && (side_data->size == sizeof(int32_t)))
    {
      int32_t bitrate = *((int32_t *)side_data->data);

      ctx->g_enc_change_params->enable_option |= NI_SET_CHANGE_PARAM_RC_TARGET_RATE;
      ctx->g_enc_change_params->bitRate = bitrate;
      if (ctx->api_fme.data.frame.reconf_len == 0)
      {
        ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
        ctx->api_fme.data.frame.reconf_len = sizeof(ni_encoder_change_params_t);
        ctx->reconfigCount++;
      }
    }

    // force pic qp demo mode: initial QP (200 frames) -> QP value specified by
    // ForcePicQpDemoMode (100 frames) -> initial QP (remaining frames)
    if (p_param->force_pic_qp_demo_mode)
    {
      if (ctx->api_ctx.frame_num >= 300)
      {
        ctx->api_fme.data.frame.force_pic_qp = 
        p_param->hevc_enc_params.rc.intra_qp;
      }
      else if (ctx->api_ctx.frame_num >= 200)
      {
        ctx->api_fme.data.frame.force_pic_qp = p_param->force_pic_qp_demo_mode;
      }
    }
    // END NETINT_INTERNAL - currently only for internal testing

    // roi data, if enabled, with demo mode
    if (ctx->api_ctx.roi_len && ctx->api_ctx.frame_num > 90 &&
        ctx->api_ctx.frame_num < 300)
    {
      ctx->api_fme.data.frame.extra_data_len += ctx->api_ctx.roi_len;
      ctx->api_fme.data.frame.roi_len = ctx->api_ctx.roi_len;
    }

    // SEI (close caption)
    side_data = av_frame_get_side_data(ctx->buffered_fme, AV_FRAME_DATA_A53_CC);

    if (side_data && side_data->size > 0)
    {
      cc_data = side_data->data;
      cc_size = side_data->size;

      // set header info fields and extra size based on codec
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        ctx->api_fme.data.frame.sei_cc_len =
        NI_CC_SEI_HDR_HEVC_LEN + cc_size + NI_CC_SEI_TRAILER_LEN;

        ctx->api_fme.data.frame.sei_total_len +=
        ctx->api_fme.data.frame.sei_cc_len;

        ctx->api_ctx.itu_t_t35_cc_sei_hdr_hevc[7] = cc_size + 11;
        ctx->api_ctx.itu_t_t35_cc_sei_hdr_hevc[16] = (cc_size / 3) | 0xc0;
      }
      else if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        ctx->api_fme.data.frame.sei_cc_len =
        NI_CC_SEI_HDR_H264_LEN + cc_size + NI_CC_SEI_TRAILER_LEN;

        ctx->api_fme.data.frame.sei_total_len +=
        ctx->api_fme.data.frame.sei_cc_len;

        ctx->api_ctx.itu_t_t35_cc_sei_hdr_h264[6] = cc_size + 11;
        ctx->api_ctx.itu_t_t35_cc_sei_hdr_h264[15] = (cc_size / 3) | 0xc0;
      }
      else
      {
        av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: codec %d not "
               "supported for SEI !\n", avctx->codec_id);
        cc_data = NULL;
        cc_size = 0;
      }
    }

    ctx->api_fme.data.frame.ni_pict_type = 0;
    if (ctx->api_ctx.force_frame_type)
    {
      switch (ctx->buffered_fme->pict_type)
      {
      case AV_PICTURE_TYPE_I:
        ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_IDR;
        break;
      case AV_PICTURE_TYPE_P:
        ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_P;
        break;
      default:
        ;
      }
    }
    else if (AV_PICTURE_TYPE_I == ctx->buffered_fme->pict_type)
    {
      ctx->api_fme.data.frame.force_key_frame = 1;
      ctx->api_fme.data.frame.ni_pict_type = PIC_TYPE_IDR;
    }

    if ((p_param->hevc_enc_params.preferred_transfer_characteristics >= 0) &&
        (0 == ctx->api_ctx.frame_num ||
         PIC_TYPE_IDR == ctx->api_fme.data.frame.ni_pict_type ||
         (p_param->hevc_enc_params.forced_header_enable &&
          p_param->hevc_enc_params.intra_period &&
          0 == (ctx->api_ctx.frame_num %
                p_param->hevc_enc_params.intra_period))))
    {
      if (AV_CODEC_ID_H264 == avctx->codec_id){
        ctx->api_fme.data.frame.preferred_characteristics_data_len = 9;
      }
      else
      {
        ctx->api_fme.data.frame.preferred_characteristics_data_len = 10;
      }
      ctx->api_ctx.preferred_characteristics_data = (uint8_t)p_param->hevc_enc_params.preferred_transfer_characteristics;
      ctx->api_fme.data.frame.sei_total_len += ctx->api_fme.data.frame.preferred_characteristics_data_len;
    }

    side_data = av_frame_get_side_data(ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_UDU_SEI);
    if (side_data && side_data->size > 0)
    {
      uint8_t *sei_data = (uint8_t *)side_data->data;
      int i, sei_len = 0;

     /*
      * worst case:
      * even size: each 2B plus a escape 1B
      * odd size: each 2B plus a escape 1B + 1 byte
      */
      udu_sei = malloc(side_data->size * 3 / 2);
      if (udu_sei == NULL)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to allocate memory for sei data.\n");
        ret = AVERROR(ENOMEM);
        return ret;
      }

      for (i = 0; i < side_data->size; i++)
      {
        if ((2 <= i) && !udu_sei[sei_len - 2] && !udu_sei[sei_len - 1] && (sei_data[i] <= 0x03))
        {
          /* insert 0x3 as emulation_prevention_three_byte */
          udu_sei[sei_len++] = 0x03;
        }
        udu_sei[sei_len++] = sei_data[i];
      }

      udu_sei_size = side_data->size;
      ext_udu_sei_size = sei_len;

      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        /* 4B long start code + 1B nal header + 1B SEI type + Bytes of payload length + Bytes of SEI payload + 1B trailing */
        sei_len = 6 + ((side_data->size + 0xFE) / 0xFF) + ext_udu_sei_size + 1;
      }
      else
      {
        /* 4B long start code + 2B nal header + 1B SEI type + Bytes of payload length + Bytes of SEI payload + 1B trailing */
        sei_len = 7 + ((side_data->size + 0xFE) / 0xFF) + ext_udu_sei_size + 1;
      }

      /* if the total sei data is about to exceed maximum size allowed, discard the user data unregistered SEI data */
      if ((ctx->api_fme.data.frame.sei_total_len + sei_len) > NI_ENC_MAX_SEI_BUF_SIZE)
      {
        av_log(avctx, AV_LOG_WARNING, "xcoder_send_frame: sei total length %u, udu sei_len %u exceeds maximum sei size %u. discard it.\n",
                ctx->api_fme.data.frame.sei_total_len, sei_len, NI_ENC_MAX_SEI_BUF_SIZE);
        free(udu_sei);
        udu_sei = NULL;
        udu_sei_size = 0;
      }
      else
      {
        ctx->api_fme.data.frame.sei_total_len += sei_len;
      }
    }

    if (ctx->api_fme.data.frame.sei_total_len > NI_ENC_MAX_SEI_BUF_SIZE)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame: sei total length %u exceeds maximum sei size %u.\n",
             ctx->api_fme.data.frame.sei_total_len, NI_ENC_MAX_SEI_BUF_SIZE);
      ret = AVERROR(EINVAL);
      return ret;
    }

    ctx->api_fme.data.frame.extra_data_len += ctx->api_fme.data.frame.sei_total_len;
    if ((ctx->api_fme.data.frame.sei_total_len || 
         ctx->api_fme.data.frame.roi_len)
        && !ctx->api_fme.data.frame.reconf_len)
    {
      ctx->api_fme.data.frame.extra_data_len += sizeof(ni_encoder_change_params_t);
    }

    ctx->api_fme.data.frame.pts = ctx->buffered_fme->pts;
    ctx->api_fme.data.frame.dts = ctx->buffered_fme->pkt_dts;

    ctx->api_fme.data.frame.video_width = ODD2EVEN(avctx->width);
    ctx->api_fme.data.frame.video_height = ODD2EVEN(avctx->height);

    ret = av_image_get_buffer_size(ctx->buffered_fme->format,
                                   ctx->buffered_fme->width,
                                   ctx->buffered_fme->height, 1);
#if FF_API_PKT_PTS
    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: pts=%" PRId64 ", pkt_dts=%" PRId64 ", pkt_pts=%" PRId64 "\n",
           ctx->buffered_fme->pts, ctx->buffered_fme->pkt_dts,
           ctx->buffered_fme->pkt_pts);
#endif
    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: frame->format=%d, width=%d, height=%d, pict_type=%d, size=%d\n",
           ctx->buffered_fme->format, ctx->buffered_fme->width,
           ctx->buffered_fme->height, ctx->buffered_fme->pict_type, ret);

    if (ret < 0)
    {
      return ret;
    }
    int dst_stride[4];
    int height_aligned[4];
    memset(dst_stride,0,sizeof(dst_stride));
    memset(height_aligned,0,sizeof(height_aligned));
    dst_stride[0] = ((frame_width + 31) / 32) * 32;

    if (dst_stride[0] < NI_MIN_WIDTH)
    {
      dst_stride[0] = NI_MIN_WIDTH;
    }

    if (frame_width != dst_stride[0]) // If the targer stride does not match the source, can not zero copy
    {
      av_log(avctx, AV_LOG_TRACE, "Width not 32 byte align, data zero copy disabled\n");
      ctx->api_ctx.yuv_copy_bypass = false;
    }

    dst_stride[0] *= ctx->api_ctx.bit_depth_factor;
    dst_stride[1] = dst_stride[2] = dst_stride[0] / 2;

    height_aligned[0] = ((frame_height + 7) / 8) * 8;
    if (avctx->codec_id == AV_CODEC_ID_H264) {
      height_aligned[0] = ((frame_height + 15) / 16) * 16;
    }
    if (height_aligned[0] < NI_MIN_HEIGHT)
    {
      height_aligned[0] = NI_MIN_HEIGHT;
    }
    height_aligned[1] = height_aligned[2] = height_aligned[0] / 2;

    if (frame_height != height_aligned[0]) // If the targer height does not match the source, can not zero copy
    {
      av_log(avctx, AV_LOG_TRACE, "Height not align, data zero copy disabled\n");
      ctx->api_ctx.yuv_copy_bypass = false;
    }

    // alignment(16) extra padding for H.264 encoding
    ni_frame_buffer_alloc_v3(&(ctx->api_fme.data.frame),
                             ODD2EVEN(ctx->buffered_fme->width),
                             ODD2EVEN(ctx->buffered_fme->height),
                             dst_stride,
                             (avctx->codec_id == AV_CODEC_ID_H264),
                             ctx->api_fme.data.frame.extra_data_len);
    if (!ctx->api_fme.data.frame.p_data[0])
    {
      return AVERROR(ENOMEM);
    }

    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: fme.data_len[0]=%d, frame->linesize=%d/%d/%d, ctx->api_fme.force_key_frame=%d, extra_data_len=%d sei_size=%d roi_size=%u reconf_size=%u force_pic_qp=%u udu_sei_size=%u use_cur_src_as_long_term_pic %u use_long_term_ref %u\n", ctx->api_fme.data.frame.data_len[0], ctx->buffered_fme->linesize[0], ctx->buffered_fme->linesize[1], ctx->buffered_fme->linesize[2], ctx->api_fme.data.frame.force_key_frame, ctx->api_fme.data.frame.extra_data_len, ctx->api_fme.data.frame.sei_total_len, ctx->api_fme.data.frame.roi_len, ctx->api_fme.data.frame.reconf_len, ctx->api_fme.data.frame.force_pic_qp, udu_sei_size, ctx->api_fme.data.frame.use_cur_src_as_long_term_pic, ctx->api_fme.data.frame.use_long_term_ref);

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(
      ctx->buffered_fme->format);
    int nb_planes = 0;
    int plane_height[4];

    plane_height[0] = ctx->buffered_fme->height;
    plane_height[1] = ctx->buffered_fme->height / 2;
    plane_height[2] = ctx->buffered_fme->height / 2;
    plane_height[3] = 0;

    for (i = 0; i < desc->nb_components; i++)
    {
      nb_planes = FFMAX(desc->comp[i].plane, nb_planes);
    }
    nb_planes++;

    av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: src linesize = %d/%d/%d "
                                "dst alloc linesize = %d/%d/%d  height = %d/%d/%d\n",
           ctx->buffered_fme->linesize[0], ctx->buffered_fme->linesize[1],
           ctx->buffered_fme->linesize[2],
           dst_stride[0], dst_stride[1], dst_stride[2],
           plane_height[0], plane_height[1], plane_height[2]);

    if (p_param->padding && p_param->hevc_enc_params.conf_win_right)
    {
      av_log(avctx, AV_LOG_TRACE, "There is some padding: padding %d right %d  disable the zero copy\n", p_param->padding, p_param->hevc_enc_params.conf_win_right);
      ctx->api_ctx.yuv_copy_bypass = false;
    }

    else if (ctx->api_ctx.yuv_copy_bypass_fw_support == false)
    {
      av_log(avctx, AV_LOG_TRACE, "FW does not support zero copy,  disable the zero copy\n");
      ctx->api_ctx.yuv_copy_bypass = false;
    }
    else if (frame == NULL)
    {
      ctx->api_ctx.yuv_copy_bypass = false;
    }
    else
    {
      if (((long int)((const uint8_t *)frame->data[NI_Y_ELEMENT])) % sysconf(_SC_PAGESIZE))
      {
         av_log(avctx, AV_LOG_TRACE, "Source address does not page align,Y:%p U:%p V:%p disable the zero copy\n", (const uint8_t *)frame->data[0], (const uint8_t *)frame->data[1],(const uint8_t *)frame->data[2]);
         ctx->api_ctx.yuv_copy_bypass = false;
      }
  
      if (ctx->api_ctx.yuv_copy_bypass == true)
      {
        ctx->api_fme.data.frame.p_buffer_yuv_bypass_y =  (const uint8_t *)frame->data[NI_Y_ELEMENT];
        ctx->api_fme.data.frame.p_buffer_yuv_bypass_u =  (const uint8_t *)frame->data[NI_U_ELEMENT];
        ctx->api_fme.data.frame.p_buffer_yuv_bypass_v =  (const uint8_t *)frame->data[NI_V_ELEMENT];

        if (((const uint8_t *)frame->data[NI_U_ELEMENT] - (const uint8_t *)frame->data[NI_Y_ELEMENT]) != (ctx->api_fme.data.frame.p_data[NI_U_ELEMENT] - ctx->api_fme.data.frame.p_data[NI_Y_ELEMENT]) || 
           ((const uint8_t *)frame->data[NI_V_ELEMENT] - (const uint8_t *)frame->data[NI_U_ELEMENT]) != (ctx->api_fme.data.frame.p_data[NI_V_ELEMENT] - ctx->api_fme.data.frame.p_data[NI_U_ELEMENT]))
        {
            
            ctx->api_ctx.yuv_copy_bypass_seperate = true;
            // The YUV from ffmpeg is seperate, so we check the data[1] alignment
            if (((long int)((const uint8_t *)frame->data[NI_U_ELEMENT])) % sysconf(_SC_PAGESIZE) || ((long int)((const uint8_t *)frame->data[NI_V_ELEMENT])) % sysconf(_SC_PAGESIZE))
            {
              av_log(avctx, AV_LOG_DEBUG, "The UV data does not page align, disable the zero copy\n");
              ctx->api_ctx.yuv_copy_bypass = false;
            }
            else
            {
              if (ctx->api_ctx.max_nvme_io_size > 511*sysconf(_SC_PAGESIZE))
              {
                ctx->api_ctx.max_nvme_io_size = 511*sysconf(_SC_PAGESIZE);
              }
            }
            
            av_log(avctx, AV_LOG_DEBUG, "The source data memory address Y:%p U:%p V:%p, dest data memory address: Y:%p U:%p V:%p\n", (const uint8_t *)frame->data[0], \
                                                                                                                                     (const uint8_t *)frame->data[1], \
                                                                                                                                     (const uint8_t *)frame->data[2], \
                                                                                                                                     ctx->api_fme.data.frame.p_data[0], \
                                                                                                                                     ctx->api_fme.data.frame.p_data[1], \
                                                                                                                                     ctx->api_fme.data.frame.p_data[2]);
        }
      }
    }
    // we check the line size for each plane for scaling and cropping case they will not match
    for (plane_idx = 0 ; plane_idx < nb_planes; plane_idx++)
    {
      if ( ctx->buffered_fme->linesize[plane_idx]  != dst_stride[plane_idx])
      {
        av_log(avctx, AV_LOG_TRACE, "The memory layout does not match(scaling/cropping), disable the zero copy plane:%d\n", i);
        ctx->api_ctx.yuv_copy_bypass = false;
      }
    }

    // width padding length in pixels and bytes, if needed
    int pad_len = 0, pad_len_bytes;
    for (i = 0; i < nb_planes; i++)
    {
      int height = plane_height[i];
      uint8_t *dst = ctx->api_fme.data.frame.p_data[i];
      const uint8_t *src = (const uint8_t *)ctx->buffered_fme->data[i];

      if (0 == i) // Y
      {
        pad_len = dst_stride[i] / ctx->api_ctx.bit_depth_factor -
        ctx->buffered_fme->width;
      }
      else if (1 == i)
      {
        // U/V share the same padding length; Y padding could be odd value
        // so make it even
        pad_len = ((pad_len + 1) / 2) * 2;
      }

      pad_len_bytes = (i == 0 ? pad_len * ctx->api_ctx.bit_depth_factor :
                       pad_len * ctx->api_ctx.bit_depth_factor / 2);
      av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: stride padding: %d pixel"
             " %d bytes.\n", pad_len, pad_len_bytes);

      for (; height > 0; height--)
      {
        if (ctx->api_ctx.yuv_copy_bypass != true)
        {
          memcpy(dst, src, (ctx->buffered_fme->linesize[i] < dst_stride[i] ?
                            ctx->buffered_fme->linesize[i] : dst_stride[i]));
        }
        dst += dst_stride[i];
        // dst is now at the line end

        if (p_param->padding && pad_len)
        {
          if (2 == ctx->api_ctx.bit_depth_factor)
          {
            // repeat last pixel (2 bytes)
            int j;
            uint8_t *tmp_dst = dst - pad_len_bytes;
            for (j = 0; j < pad_len_bytes / 2; j++)
            {
              memcpy(tmp_dst, dst - pad_len_bytes - 2, 2);
              tmp_dst += 2;
            }
          }
          else
          {
            memset(dst - pad_len_bytes, *(dst - pad_len_bytes - 1),
                   pad_len_bytes);
          }
        }
        src += ctx->buffered_fme->linesize[i];
      }
      // height padding if needed
      int padding_height = height_aligned[i] - plane_height[i];
      if (padding_height > 0) {
        av_log(avctx, AV_LOG_TRACE, "xcoder_send_frame: plane %d padding %d\n",
         i, padding_height);

        //src -= frame->linesize[i];
	src = dst - dst_stride[i];
        for (; padding_height > 0; padding_height--) {
          memcpy(dst, src, dst_stride[i]);
          dst += dst_stride[i];
        }
      }
    }

#ifdef NIENC_MULTI_THREAD
    if (ctx->api_ctx.yuv_copy_bypass == true)
    {
       av_frame_ref(&(ctx->frame_hold), frame);
    }
#endif
    av_log(avctx, AV_LOG_TRACE, "After memcpy/zerocopy  p_data Y:%p, U:%p, V:%p  len:0:%d 1:%d 2:%d\n", \
          ctx->api_fme.data.frame.p_data[0], ctx->api_fme.data.frame.p_data[1], ctx->api_fme.data.frame.p_data[2],
          ctx->api_fme.data.frame.data_len[0],
          ctx->api_fme.data.frame.data_len[1],
          ctx->api_fme.data.frame.data_len[2]);

    uint8_t *dst = (uint8_t *)ctx->api_fme.data.frame.p_data[2] + 
    ctx->api_fme.data.frame.data_len[2] + NI_APP_ENC_FRAME_META_DATA_SIZE;

    // fill in reconfig data, if enabled
    //FW accomodation: whatever add reconfig size to dst if sei or roi or reconfig are present
    if ((ctx->api_fme.data.frame.reconf_len || ctx->api_fme.data.frame.roi_len
         || ctx->api_fme.data.frame.sei_total_len) && ctx->g_enc_change_params)
    {
      memcpy(dst, ctx->g_enc_change_params, sizeof(ni_encoder_change_params_t));
      dst += sizeof(ni_encoder_change_params_t);
    }

    // fill in roi map, if enabled
    if (ctx->api_fme.data.frame.roi_len)
    {
      if (AV_CODEC_ID_H264 == avctx->codec_id && g_avc_roi_map)
      {
        memcpy(dst, g_avc_roi_map, ctx->api_fme.data.frame.roi_len);
        dst += ctx->api_fme.data.frame.roi_len;
      }
      else if (AV_CODEC_ID_HEVC == avctx->codec_id && g_hevc_roi_map)
      {
        memcpy(dst, g_hevc_roi_map, ctx->api_fme.data.frame.roi_len);
        dst += ctx->api_fme.data.frame.roi_len;
      }
    }

    // fill in extra data (excluding meta data): close caption
    if (ctx->api_fme.data.frame.sei_cc_len && cc_data && cc_size)
    {
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        memcpy(dst, ctx->api_ctx.itu_t_t35_cc_sei_hdr_hevc,
               NI_CC_SEI_HDR_HEVC_LEN);
        dst += NI_CC_SEI_HDR_HEVC_LEN;
        memcpy(dst, cc_data, cc_size);
        dst += cc_size;
        memcpy(dst, ctx->api_ctx.sei_trailer, NI_CC_SEI_TRAILER_LEN);
        dst += NI_CC_SEI_TRAILER_LEN;
      }
      else if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        memcpy(dst, ctx->api_ctx.itu_t_t35_cc_sei_hdr_h264,
               NI_CC_SEI_HDR_H264_LEN);
        dst += NI_CC_SEI_HDR_H264_LEN;
        memcpy(dst, cc_data, cc_size);
        dst += cc_size;
        memcpy(dst, ctx->api_ctx.sei_trailer, NI_CC_SEI_TRAILER_LEN);
        dst += NI_CC_SEI_TRAILER_LEN;
      }
    }

    //HLG preferred characteristics SEI
    if (ctx->api_fme.data.frame.preferred_characteristics_data_len)
    {
      dst[0] = dst[1] = dst[2] = 0;
      dst[3] = 1;
      if (AV_CODEC_ID_HEVC == avctx->codec_id)
      {
        dst[4] = 0x4e;
        dst[5] = 1;
        dst[6] = 0x93;  // payload type=147
        dst[7] = 1;     // payload size=1
        dst += 8;
      }
      else
      {
        dst[4] = 0x6;
        dst[5] = 0x93;  // payload type=147
        dst[6] = 1;     // payload size=1
        dst += 7;
      }
      *dst =  ctx->api_ctx.preferred_characteristics_data;
      dst ++;
      *dst = 0x80;
      dst++;
    }

    if (udu_sei && udu_sei_size)
    {
      int payload_size = udu_sei_size;
      *dst++ = 0x00;   //long start code
      *dst++ = 0x00;
      *dst++ = 0x00;
      *dst++ = 0x01;
      if (AV_CODEC_ID_H264 == avctx->codec_id)
      {
        *dst++ = 0x06;   //nal type: SEI
      }
      else
      {
        *dst++ = 0x4e;   //nal type: SEI
        *dst++ = 0x01;
      }
      *dst++ = 0x05;   //SEI type: user data unregistered

      /* original payload size */
      while (payload_size > 0)
      {
        *dst++ = (payload_size > 0xFF ? 0xFF : (uint8_t)payload_size);
        payload_size -= 0xFF;
      }

      /* extended payload data */
      memcpy(dst, udu_sei, ext_udu_sei_size);
      dst += ext_udu_sei_size;

      /* trailing byte */
      *dst = 0x80;
      dst++;

      free(udu_sei);
    }
  }  

  ctx->sentFrame = 1;

#ifdef NIENC_MULTI_THREAD
  if (ctx->encoder_flushing)
  {
    sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame encoder_flushing: size %d sent to xcoder\n", sent);
    if (NI_RETCODE_ERROR_RESOURCE_UNAVAILABLE == sent)
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): Sequence Change in progress, returning EAGAIN\n");
      ret = AVERROR(EAGAIN);
      return ret;
    }
    else if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
    {
      sent = xcoder_encode_reset(avctx);
    }
  
    if (sent < 0)
    {
      ret = AVERROR(EIO);
    }
    else
    {
      //pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx ++;
      ret = 0;
    }
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 111 %p, session_info %d, device_handle %d\n", ctx->api_ctx.session_info, ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
    if ((ctx->api_ctx.session_id != NI_INVALID_SESSION_ID) && (ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE))
    {
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame start 111 %p\n", ctx->api_ctx.session_info);
      write_thread_arg_struct_t *write_thread_args = (write_thread_arg_struct_t *)malloc(sizeof(write_thread_arg_struct_t));
      pthread_mutex_init(&write_thread_args->mutex, NULL);
      pthread_cond_init(&write_thread_args->cond, NULL);
      write_thread_args->running = 0;
      write_thread_args->ctx = ctx;
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: session_id %d, device_handle %d\n", ctx->api_ctx.session_id, ctx->api_ctx.device_handle);
      av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: ctx %p\n", write_thread_args->ctx);
      ctx->api_ctx.session_info = (void *)write_thread_args;
      write_thread_args->running = 1;
      int ret = threadpool_auto_add_task_thread(&pool, write_frame_thread, write_thread_args, 1);
      if (ret < 0)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to add_task_thread to threadpool\n");
        return ret;
      }
    }
  }
#else
  sent = ni_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_DEVICE_TYPE_ENCODER);

  av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame: size %d sent to xcoder\n", sent);

  // return EIO at error
  if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    sent = xcoder_encode_reset(avctx);
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): VPU recovery failed:%d, returning EIO\n", sent);
      ret = AVERROR(EIO);
    }
  }
  else if (sent < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): failure sent (%d) , "
           "returning EIO\n", sent);
    ret = AVERROR(EIO);

    // if rejected due to sequence change in progress, revert resolution
    // setting and will do it again next time.
    if (ctx->api_fme.data.frame.start_of_stream &&
        (avctx->width != orig_avctx_width ||
         avctx->height != orig_avctx_height))
    {
      avctx->width = orig_avctx_width;
      avctx->height = orig_avctx_height;
    }
    return ret;
  }
  else
  {
    if (sent == 0)
    {
      // case of sequence change in progress
      if (ctx->api_fme.data.frame.start_of_stream &&
          (avctx->width != orig_avctx_width ||
           avctx->height != orig_avctx_height))
      {
        avctx->width = orig_avctx_width;
        avctx->height = orig_avctx_height;
      }

      // when buffer_full, drop the frame and return EAGAIN if in strict timeout
      // mode, otherwise buffer the frame and it is to be sent out using encode2
      // API: queue the frame only if not done so yet, i.e. queue is empty
      // *and* it's a valid frame. ToWatch: what are other rc cases ?
      if (ctx->api_ctx.status == NI_RETCODE_NVME_SC_WRITE_BUFFER_FULL)
      {
        if (ctx->api_param.strict_timeout_mode)
        {
          av_log(avctx, AV_LOG_ERROR, "xcoder_send_frame(): Error Strict timeout period exceeded, returning EAGAIN\n");
          ret = AVERROR(EAGAIN);
        }
        else
        {
          av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame(): Write buffer full, returning 1\n");
          ret = 1;

          if (frame && is_input_fifo_empty(ctx))
          {
            queue_fme(avctx, frame);
          }
        }
      }
    }
    else
    {
      // only if it's NOT sequence change flushing (in which case only the eos
      // was sent and not the first sc pkt) AND
      // only after successful sending will it be removed from fifo
      if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING !=
          ctx->api_ctx.session_run_state)
      {
        if (! is_input_fifo_empty(ctx))
        {
          av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
          av_log(avctx, AV_LOG_DEBUG, "fme popped, fifo size: %lu\n",
                 av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
        }
        av_frame_unref(ctx->buffered_fme);
      }
      else
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder frame(eos) sent, sequence changing!"
               " NO fifo pop !\n");
      }

      // pushing input pts in circular FIFO
      ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_FIFO_SZ] = ctx->api_fme.data.frame.pts;
      ctx->api_ctx.enc_pts_w_idx ++;
      ret = 0;

      // have another check before return: if no more frames in fifo to send and
      // we've got eos (NULL) frame from upper stream, flag for flushing
      if (ctx->eos_fme_received && is_input_fifo_empty(ctx))
      {
        av_log(avctx, AV_LOG_DEBUG, "Upper stream EOS frame received, fifo "
               "empty, start flushing ..\n");
        ctx->encoder_flushing = 1;
      }
    }
  }
#endif
  if (ctx->encoder_flushing)
  {
    av_log(avctx, AV_LOG_DEBUG, "xcoder_send_frame flushing ..\n");
    ret = ni_device_session_flush(&ctx->api_ctx, NI_DEVICE_TYPE_ENCODER);
  }

  return ret;
}

static int xcoder_encode_reinit(AVCodecContext *avctx)
{
  int ret = 0;
  XCoderH265EncContext *ctx = avctx->priv_data;

  ctx->eos_fme_received = 0;
  ctx->encoder_eof = 0;
  ctx->encoder_flushing = 0;

  if (ctx->api_ctx.pts_table && ctx->api_ctx.dts_queue)
  {
    xcoder_encode_close(avctx);
  }
  ctx->started = 0;
  ctx->firstPktArrived = 0;
  ctx->spsPpsArrived = 0;
  ctx->spsPpsHdrLen = 0;
  ctx->p_spsPpsHdr = NULL;

  // and re-init avctx's resolution to the changed one that is
  // stored in the first frame of the fifo
  AVFrame *tmp_fme = av_frame_alloc();
  av_fifo_generic_peek(ctx->fme_fifo, tmp_fme, sizeof(AVFrame), NULL);
  av_log(avctx, AV_LOG_INFO, "xcoder_receive_packet resolution "
         "changing %dx%d -> %dx%d\n", avctx->width, avctx->height,
         tmp_fme->width, tmp_fme->height);
  avctx->width = tmp_fme->width;
  avctx->height = tmp_fme->height;

  ret = xcoder_encode_init(avctx);
  ctx->api_ctx.session_run_state = SESSION_RUN_STATE_NORMAL;

  return ret;
}

int xcoder_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  ni_encoder_params_t *p_param = (ni_encoder_params_t *)ctx->api_ctx.p_session_config;
  int ret = 0;
  int recv;
  ni_packet_t *xpkt = &ctx->api_pkt.data.packet;

  av_log(avctx, AV_LOG_DEBUG, "XCoder receive packet\n");

  if (ctx->encoder_eof)
  {
    av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: EOS\n");
    return AVERROR_EOF;
  }
  ni_packet_buffer_alloc(xpkt, NI_MAX_TX_SZ);
  while (1)
  {

    recv = ni_device_session_read(&ctx->api_ctx, &(ctx->api_pkt), NI_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: xpkt.end_of_stream=%d, xpkt.data_len=%d, recv=%d, encoder_flushing=%d, encoder_eof=%d\n", xpkt->end_of_stream, xpkt->data_len, recv, ctx->encoder_flushing, ctx->encoder_eof);

    if (recv <= 0)
    {
      ctx->encoder_eof = xpkt->end_of_stream;
      /* not ready ?? */
      if (ctx->encoder_eof || xpkt->end_of_stream)
      {
        if (SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
            ctx->api_ctx.session_run_state)
        {
          // after sequence change completes, reset codec state
          av_log(avctx, AV_LOG_INFO, "xcoder_receive_packet 1: sequence "
                 "change completed, return AVERROR(EAGAIN) and will reopen "
                 "codec!\n");

          ret = xcoder_encode_reinit(avctx);
          if (ret >= 0)
          {
            ret = AVERROR(EAGAIN);
          }
          break;
        }

        ret = AVERROR_EOF;
        av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: got encoder_eof, "
               "return AVERROR_EOF\n");
        break;
      }
      else
      {
        if (NI_RETCODE_ERROR_VPU_RECOVERY == recv)
        {
          recv = xcoder_encode_reset(avctx);
          if (recv < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "xcoder_receive_packet(): VPU recovery failed:%d, returning EIO\n", recv);
            ret = AVERROR(EIO);
            break;
          }
        }

        if (recv < 0)
        {
          if ((NI_RETCODE_ERROR_INVALID_SESSION == recv) && (!ctx->started))  // session may be in recovery state, return EAGAIN
          {
            av_log(avctx, AV_LOG_ERROR, "XCoder receive packet: not started, invalid session id\n");
            ret = AVERROR(EAGAIN);
          }
          else
          {
            av_log(avctx, AV_LOG_ERROR, "XCoder receive packet: Persistent failure, returning EIO,ret=%d\n",recv);
            ret = AVERROR(EIO);
          }
          ctx->gotPacket = 0;
          ctx->sentFrame = 0;
          break;
        }

        if (ctx->api_param.low_delay_mode && ctx->sentFrame && !ctx->gotPacket)
        {
          av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: low delay mode,"
                 " keep reading until pkt arrives\n");
          continue;
        }
        ctx->gotPacket = 0;
        ctx->sentFrame = 0;
        ret = AVERROR(EAGAIN);
        if (! ctx->encoder_flushing && ! ctx->eos_fme_received)
        {
          av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet: NOT encoder_"
                 "flushing, NOT eos_fme_received, return AVERROR(EAGAIN)\n");
          break;
        }
      }
    }
    else
    {
      /* got encoded data back */
      int meta_size = NI_FW_ENC_BITSTREAM_META_DATA_SIZE;
      if (! ctx->spsPpsArrived)
      {
        ret = AVERROR(EAGAIN);
        ctx->spsPpsArrived = 1;
        ctx->spsPpsHdrLen = recv - meta_size;
        ctx->p_spsPpsHdr = malloc(ctx->spsPpsHdrLen);
        if (! ctx->p_spsPpsHdr)
        {
          ret = AVERROR(ENOMEM);
          break;
        }

        memcpy(ctx->p_spsPpsHdr, (uint8_t*)xpkt->p_data + meta_size,
               xpkt->data_len - meta_size);
        //av_log(avctx, AV_LOG_TRACE, "encoder: very first data chunk saved: %d !\n",
        //       ctx->spsPpsHdrLen);

        // start pkt_num counter from 1 to get the real first frame
        ctx->api_ctx.pkt_num = 1;
        // for low-latency mode, keep reading until the first frame is back
        if (ctx->api_param.low_delay_mode)
        {
          av_log(avctx, AV_LOG_TRACE, "XCoder receive packet: low delay mode,"
                 " keep reading until 1st pkt arrives\n");
          continue;
        }
        break;
      }
      ctx->gotPacket = 1;
      ctx->sentFrame = 0;
      if (! ctx->firstPktArrived)
      {
        int sizeof_spspps_attached_to_idr = ctx->spsPpsHdrLen;
        if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
        {
          sizeof_spspps_attached_to_idr = 0;
        }
        ctx->firstPktArrived = 1;
        ctx->first_frame_pts = xpkt->pts;
        ret = ff_alloc_packet2(avctx, pkt,
                               xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr,
                               xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr);

        if (! ret)
        {
          // fill in AVC/HEVC sidedata
          if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
              (avctx->extradata_size != ctx->spsPpsHdrLen ||
               memcmp(avctx->extradata, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen)))
          {
            avctx->extradata_size = ctx->spsPpsHdrLen;
            free(avctx->extradata);
            avctx->extradata = av_mallocz(avctx->extradata_size +
                                          AV_INPUT_BUFFER_PADDING_SIZE);
            if (! avctx->extradata)
            {
              av_log(avctx, AV_LOG_ERROR,
                     "Cannot allocate AVC/HEVC header of size %d.\n",
                     avctx->extradata_size);
              return AVERROR(ENOMEM);
            }
            memcpy(avctx->extradata, ctx->p_spsPpsHdr, avctx->extradata_size);
          }

          uint8_t *p_side_data = av_packet_new_side_data(
              pkt, AV_PKT_DATA_NEW_EXTRADATA, ctx->spsPpsHdrLen);
          if (p_side_data)
          {
            memcpy(p_side_data, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
          }

          if (sizeof_spspps_attached_to_idr)
          {
            memcpy(pkt->data, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
          }
          memcpy(pkt->data + sizeof_spspps_attached_to_idr,
                 (uint8_t*)xpkt->p_data + meta_size,
                 xpkt->data_len - meta_size);
        }
      }
      else
      {
        // insert header when intraRefresh is enabled for every
        // intraRefreshMinPeriod frames, pkt counting starts from 1, e.g. for
        // cycle of 100, the header is forced on frame 102, 202, ...;
        // note that api_ctx.pkt_num returned is the actual index + 1
        int intra_refresh_hdr_sz = 0;
        if (ctx->p_spsPpsHdr && ctx->spsPpsHdrLen &&
            p_param->hevc_enc_params.forced_header_enable &&
            (1 == p_param->hevc_enc_params.intra_mb_refresh_mode ||
             2 == p_param->hevc_enc_params.intra_mb_refresh_mode ||
             3 == p_param->hevc_enc_params.intra_mb_refresh_mode) &&
            p_param->ui32minIntraRefreshCycle > 0 &&
            ctx->api_ctx.pkt_num > 3 &&
            0 == ((ctx->api_ctx.pkt_num - 3) %
                  p_param->ui32minIntraRefreshCycle))
        {
          intra_refresh_hdr_sz = ctx->spsPpsHdrLen;
          av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet pkt %" PRId64 " "
                 " force header on intraRefreshMinPeriod %u\n",
                 ctx->api_ctx.pkt_num - 1, p_param->ui32minIntraRefreshCycle);
        }

        ret = ff_alloc_packet2(
          avctx, pkt, xpkt->data_len - meta_size + intra_refresh_hdr_sz,
          xpkt->data_len - meta_size + intra_refresh_hdr_sz);

        if (! ret)
        {
          if (intra_refresh_hdr_sz)
          {
            memcpy(pkt->data, ctx->p_spsPpsHdr, intra_refresh_hdr_sz);
          }
          memcpy(pkt->data + intra_refresh_hdr_sz,
                 (uint8_t*)xpkt->p_data + meta_size,
                 xpkt->data_len - meta_size);
        }
      }
      if (!ret)
      {
        if (xpkt->frame_type == 0)
        {
          pkt->flags |= AV_PKT_FLAG_KEY;
        }
        pkt->pts = xpkt->pts;
        /* to ensure pts>dts for all frames, we assign a guess pts for the first 'dtsOffset' frames and then the pts from input stream
         * is extracted from input pts FIFO.
         * if GOP = IBBBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -3 -2 -1 0 1 ... and -3 -2 -1 are the guessed values
         * if GOP = IBPBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -1 0 1 2 3 ... and -1 is the guessed value
         * the number of guessed values is equal to dtsOffset
         */
        if (ctx->total_frames_received < ctx->dtsOffset)
        {
          // guess dts
          pkt->dts = ctx->first_frame_pts + ((ctx->gop_offset_count - ctx->dtsOffset)  * avctx->ticks_per_frame);
          ctx->gop_offset_count++;
        }
        else
        {
          // get dts from pts FIFO
          pkt->dts = ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_r_idx % NI_FIFO_SZ];
          ctx->api_ctx.enc_pts_r_idx ++;
        }
        if (ctx->total_frames_received >= 1)
        {
          if (pkt->dts < ctx->latest_dts)
          {
            av_log(NULL, AV_LOG_WARNING, "dts: %ld < latest_dts: %ld.\n",
                    pkt->dts, ctx->latest_dts);
          }
        }
        if(pkt->dts > pkt->pts)
        {
          av_log(NULL, AV_LOG_WARNING, "dts: %ld, pts: %ld. Forcing dts = pts \n",
                  pkt->dts, pkt->dts);
          pkt->dts = pkt->pts;
        }
        ctx->total_frames_received++;
        ctx->latest_dts = pkt->dts;
        av_log(avctx, AV_LOG_DEBUG, "XCoder recv pkt #%" PRId64 ""
               " pts %" PRId64 "  dts %" PRId64 "  size %d  st_index %d \n",
               ctx->api_ctx.pkt_num - 1, pkt->pts, pkt->dts, pkt->size,
               pkt->stream_index);
      }
      ctx->encoder_eof = xpkt->end_of_stream;

      if (ctx->encoder_eof &&
          SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        // after sequence change completes, reset codec state
        av_log(avctx, AV_LOG_TRACE, "xcoder_receive_packet 2: sequence change "
               "completed, return 0 and will reopen codec !\n");
        ret = xcoder_encode_reinit(avctx);
      }
      break;
    }
  }

  return ret;
}

int xcoder_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *frame, int *got_packet)
{
  XCoderH265EncContext *ctx = avctx->priv_data;
  int ret;

  av_log(avctx, AV_LOG_DEBUG, "XCoder encode frame\n");

  if (!ctx->encoder_flushing)
  {
    ret = xcoder_send_frame(avctx, frame);
    if (ret < 0)
    {
      return ret;
    }
  }

  ret = xcoder_receive_packet(avctx, pkt);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
  {
    *got_packet = 0;
  }
  else if (ret < 0)
  {
    return ret;
  }
  else
  {
    *got_packet = 1;
  }

  return 0;
}


