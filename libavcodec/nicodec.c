/*
 * XCoder Codec Lib Wrapper
 * Copyright (c) 2018 NetInt
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

/**
 * @file
 * XCoder codec lib wrapper.
 */

#include <ni_rsrc_api.h>
#include "nicodec.h"
#include "nidec.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/hevc.h"
#include "libavcodec/h264.h"

static inline void ni_align_free(void *opaque, uint8_t *data)
{
  ni_buf_t *buf = (ni_buf_t *)opaque;
  if (buf)
  {
    ni_decoder_frame_buffer_pool_return_buf(buf, (ni_buf_pool_t *)buf->pool);
  }
}

int ff_xcoder_dec_init(AVCodecContext *avctx,
                       XCoderH264DecContext *s)
{
  /* ToDo: call xcode_dec_open to open a decoder instance */
  int ret = 0;
  s->api_ctx.hw_id = s->dev_dec_idx;
  strcpy(s->api_ctx.dev_xcoder, s->dev_xcoder);
  int rc = ni_device_session_open(&s->api_ctx, NI_DEVICE_TYPE_DECODER);

  if (rc != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open decoder (status = %d), "
           "resource unavailable\n", rc);
    ret = AVERROR_EXTERNAL;
    ff_xcoder_dec_close(avctx, s);
  }
  else
  {
    s->dev_xcoder = s->api_ctx.dev_xcoder_name;
    s->blk_xcoder = s->api_ctx.blk_xcoder_name;
    s->dev_dec_idx = s->api_ctx.hw_id;
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened successfully\n",
           s->dev_xcoder, s->dev_dec_idx, s->api_ctx.session_id);
  }

  return ret;
}

int ff_xcoder_dec_close(AVCodecContext *avctx,
                        XCoderH264DecContext *s)
{
  ni_retcode_t ret = NI_RETCODE_FAILURE;
  
  ret = ni_device_session_close(&s->api_ctx, s->eos, NI_DEVICE_TYPE_DECODER);
  if (NI_RETCODE_SUCCESS != ret)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to close Decode Session (status = %d)\n", ret);
  }
#ifdef _WIN32
  ni_device_close(s->api_ctx.device_handle);
#elif __linux__
  ni_device_close(s->api_ctx.device_handle);
  ni_device_close(s->api_ctx.blk_io_handle);
#endif
  s->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  s->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  
  return 0;
}

/* the start_index must be a valid index of the pkt's user data unreg sei start. */
static int ff_xcoder_extract_user_data_unreg_sei(AVCodecContext *avctx, AVPacket *pkt,
                                                   int start_index, ni_packet_t *p_packet)
{
  int i;
  uint8_t *udata;
  uint8_t *sei_data;
  int len = 0;
  int sei_size = 0;
  int index = start_index;

  /* extract SEI payload size */
  while ((index < pkt->size) && (pkt->data[index] == 255))
  {
    sei_size += pkt->data[index++];
  }

  if (index >= pkt->size)
  {
    av_log(avctx, AV_LOG_WARNING, "user data unregistered sei corrupted: length truncated.\n");
    return AVERROR(EINVAL);
  }
  sei_size += pkt->data[index++];

  udata = &pkt->data[index];

  /* max size */
  sei_data = malloc(sei_size);
  if (sei_data == NULL)
  {
    av_log(avctx, AV_LOG_ERROR, "failed to allocate user data unregistered sei buffer.\n");
    return AVERROR(ENOMEM);
  }

  /* extract SEI payload data */
  for (i = 0; (i < (pkt->size - index)) && len < sei_size; i++)
  {
    /* if the latest 3-byte data pattern matchs '00 00 03' which means udata[i] is an escaping byte,
     * discard udata[i]. */
    if (i >= 2 && udata[i - 2] == 0 && udata[i - 1] == 0 && udata[i] == 3)
    {
      continue;
    }
    sei_data[len++] = udata[i];
  }

  if (len != sei_size)
  {
    av_log(avctx, AV_LOG_WARNING, "user data unregistered sei corrupted: data truncated.\n");
    free(sei_data);
    return AVERROR(EINVAL);
  }

  p_packet->p_user_data_unreg_sei = sei_data;
  p_packet->user_data_unreg_sei_len = sei_size;

  return 0;
}

/* we assume the user data unregistered SEI payload belongs to its next slice NAL.
 * so for efficiency the SEI payload search would stop when slices NALs encounter. */
static int ff_xcoder_detect_user_data_unreg_sei(AVCodecContext *avctx, AVPacket *pkt, ni_packet_t *p_packet)
{
  int ret = 0;
  const uint8_t *ptr = pkt->data;
  const uint8_t *end = pkt->data + pkt->size;
  uint8_t nalu_type;
  uint32_t stc = -1;
  int got_sei = 0;
  int got_slice = 0;

  if (!pkt->data || !avctx)
  {
    return ret;
  }

  stc = -1;
  ptr = avpriv_find_start_code(ptr, end, &stc);
  while (ptr < end)
  {
    if (avctx->codec_id == AV_CODEC_ID_H264)
    { // if h264
      nalu_type = stc & 0x1F;
      if (!got_sei && (nalu_type == H264_NAL_SEI) && (*ptr == 5)) //found USER DATA UNREGISTERED SEI
      {
        /* extract SEI payload, store in ni_packet and pass to libxcoder. */
        ret = ff_xcoder_extract_user_data_unreg_sei(avctx, pkt, ptr + 1 - pkt->data, p_packet);
        if (ret == AVERROR(ENOMEM))
        {
          return ret;
        }
        else if (ret != 0)
        {
          return 0;
        }
        if (p_packet->p_user_data_unreg_sei)
        {
          got_sei = 1;
        }
      }
      else if ((nalu_type >= H264_NAL_SLICE) && (nalu_type <= H264_NAL_IDR_SLICE)) //VCL
      {
        /* if slice data is met, then stop searching for user data SEI. */
        ret = 0;
        got_slice = 1;
        break;
      }
    }
    else if (avctx->codec_id == AV_CODEC_ID_HEVC)
    { //if hevc
      nalu_type = (stc >> 1) & 0x3F;
      if (!got_sei && (nalu_type == HEVC_NAL_SEI_PREFIX) && (*ptr == 1) && (*(ptr + 1) == 5)) //found USER DATA UNREGISTERED SEI
      {
        /* extract SEI payload, store in ni_packet and pass to libxcoder. */
        ret = ff_xcoder_extract_user_data_unreg_sei(avctx, pkt, ptr + 2 - pkt->data, p_packet);
        if (ret == AVERROR(ENOMEM))
        {
          return ret;
        }
        else if (ret != 0)
        {
          return 0;
        }
        if (p_packet->p_user_data_unreg_sei)
        {
          got_sei = 1;
        }
      }
      else if (nalu_type >= HEVC_NAL_TRAIL_N && nalu_type <= HEVC_NAL_RSV_VCL31) //found VCL
      {
        /* if slice data is met, then stop searching for user data SEI. */
        ret = 0;
        got_slice = 1;
        break;
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "%s wrong codec %d !\n", __func__,
             avctx->codec_id);
      break;
    }

    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
  }

  if (got_sei && !got_slice)
  {
    /* mark this ni_packet when the av_packet contains sei payload but no slice.
     * libxcoder handles this kind ni_packet differently. */
    p_packet->no_slice = 1;
  }
  return ret;
}

// return 1 if need to prepend saved header to pkt data, 0 otherwise
int ff_xcoder_add_headers(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderH264DecContext *s = avctx->priv_data;
  int ret = 0;
  const uint8_t *ptr = pkt->data;
  const uint8_t *end = pkt->data + pkt->size;
  uint32_t stc = -1;
  uint8_t nalu_type = 0;

  if (!pkt->data || !avctx)
  {
    return ret;
  }

  while (ptr < end)
  {
    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
    if (ptr == end)
    {
      break;
    }

    if (AV_CODEC_ID_H264 == avctx->codec_id)
    {
      nalu_type = stc & 0x1F;

      // update saved header if it has changed
      if (s->got_first_idr && H264_NAL_IDR_SLICE == nalu_type)
      {
        if (s->extradata_size != avctx->extradata_size ||
            memcmp(s->extradata, avctx->extradata, s->extradata_size))
        {
          s->got_first_idr = 0;
        }
      }

      if (! s->got_first_idr && H264_NAL_IDR_SLICE == nalu_type)
      {
        free(s->extradata);
        s->extradata = malloc(avctx->extradata_size);
        if (! s->extradata)
        {
          av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory allocation "
                 "failed !\n");
          ret = 0;
          break;
        }
        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
               avctx->extradata_size);
        memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
        s->extradata_size = avctx->extradata_size;
        s->got_first_idr = 1;
        ret = 1;
        break;
      }

      // when header (SPS/PPS) already exists, no need to prepend it again;
      // we use one of the header info to simplify the checking.
      if (H264_NAL_SPS == nalu_type || H264_NAL_PPS == nalu_type)
      {
        // save the header if not done yet for subsequent comparison
        if (! s->extradata_size || ! s->extradata)
        {
          s->extradata = malloc(avctx->extradata_size);
          if (! s->extradata)
          {
            av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory "
                   "allocation failed !\n");
            ret = 0;
            break;
          }
          av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
                 avctx->extradata_size);
          memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
          s->extradata_size = avctx->extradata_size;
        }
        s->got_first_idr = 1;
        ret = 0;
        break;
      }
      else if (nalu_type >= H264_NAL_SLICE && nalu_type <= H264_NAL_IDR_SLICE)
      {
        // VCL types results in no header inserted
        ret = 0;
        break;
      }
    }
    else if (AV_CODEC_ID_HEVC == avctx->codec_id)
    {
      nalu_type = (stc >> 1) & 0x3F;

      // IRAP picture types include: BLA, CRA, IDR and IRAP reserve types,
      // 16-23, and insert header in front of IRAP at start or if header changes
      if (s->got_first_idr && (nalu_type >= HEVC_NAL_BLA_W_LP &&
                               nalu_type <= HEVC_NAL_IRAP_VCL23))
      {
        if (s->extradata_size != avctx->extradata_size ||
            memcmp(s->extradata, avctx->extradata, s->extradata_size))
        {
          s->got_first_idr = 0;
        }
      }

      if (! s->got_first_idr && (nalu_type >= HEVC_NAL_BLA_W_LP &&
                                 nalu_type <= HEVC_NAL_IRAP_VCL23))
      {
        free(s->extradata);
        s->extradata = malloc(avctx->extradata_size);
        if (! s->extradata)
        {
          av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory allocation "
                 "failed !\n");
          ret = 0;
          break;
        }
        av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
               avctx->extradata_size);
        memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
        s->extradata_size = avctx->extradata_size;
        s->got_first_idr = 1;
        ret = 1;
        break;
      }

      // when header (VPS/SPS/PPS) already exists, no need to prepend it again;
      // we use one of the header info to simplify the checking.
      if (HEVC_NAL_VPS == nalu_type || HEVC_NAL_SPS == nalu_type ||
	  HEVC_NAL_PPS == nalu_type)
      {
        // save the header if not done yet for subsequent comparison
        if (! s->extradata_size || ! s->extradata)
        {
          s->extradata = malloc(avctx->extradata_size);
          if (! s->extradata)
          {
            av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers memory "
                   "allocation failed !\n");
            ret = 0;
            break;
          }
          av_log(avctx, AV_LOG_TRACE, "ff_xcoder_add_headers size %d\n",
                 avctx->extradata_size);
          memcpy(s->extradata, avctx->extradata, avctx->extradata_size);
          s->extradata_size = avctx->extradata_size;
        }
        s->got_first_idr = 1;
        ret = 0;
        break;
      }
      else if (nalu_type >= HEVC_NAL_TRAIL_N && nalu_type <= HEVC_NAL_RSV_VCL31)
      {
        // VCL types results in no header inserted
        ret = 0;
        break;
      }
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "ff_xcoder_add_headers wrong codec %d !\n",
             avctx->codec_id);
      break;
    }
  }
  return ret;
}

// check if the packet is SEI only, 1 yes, 0 otherwise
static int pkt_is_sei_only(AVCodecContext *avctx, AVPacket *pkt)
{
  int pkt_sei_alone = 0;
  const uint8_t *ptr = pkt->data;
  const uint8_t *end = pkt->data + pkt->size;
  uint32_t stc = -1;
  uint8_t nalu_type = 0;
  int nalu_count = 0;

  if (!pkt->data || !avctx)
  {
    return pkt_sei_alone;
  }

  pkt_sei_alone = 1;
  while (pkt_sei_alone && ptr < end)
  {
    stc = -1;
    ptr = avpriv_find_start_code(ptr, end, &stc);
    if (ptr == end)
    {
      if (0 == nalu_count)
      {
        pkt_sei_alone = 0;
        av_log(avctx, AV_LOG_TRACE, "pkt_is_sei_only: no NAL found in pkt.\n");
      }
      break;
    }
    nalu_count++;

    if (AV_CODEC_ID_H264 == avctx->codec_id)
    {
      nalu_type = stc & 0x1F;
      pkt_sei_alone = (pkt_sei_alone && H264_NAL_SEI == nalu_type);
    }
    else if (AV_CODEC_ID_HEVC == avctx->codec_id)
    {
      nalu_type = (stc >> 1) & 0x3F;
      pkt_sei_alone = (pkt_sei_alone && (HEVC_NAL_SEI_PREFIX == nalu_type ||
                                         HEVC_NAL_SEI_SUFFIX == nalu_type));
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "pkt_is_sei_only wrong codec %d !\n",
             avctx->codec_id);
      pkt_sei_alone = 0;
      break;
    }
  }
  return pkt_sei_alone;
}

int ff_xcoder_dec_send(AVCodecContext *avctx,
                       XCoderH264DecContext *s,
                       AVPacket *pkt)
{
  /* call ni_decoder_session_write to send compressed video packet to the decoder
     instance */
  int need_draining = 0;
  size_t size;
  ni_packet_t *xpkt = &(s->api_pkt.data.packet);
  int ret;
  int sent;
  int send_size = 0;
  int new_packet = 0;

  size = pkt->size;

  if (s->flushing)
  {
    av_log(avctx, AV_LOG_ERROR, "Decoder is flushing and cannot accept new "
                                "buffer until all output buffers have been released\n");
    return AVERROR_EXTERNAL;
  }

  if (pkt->size == 0)
  {
    need_draining = 1;
  }

  if (s->draining && s->eos)
  {
    av_log(avctx, AV_LOG_DEBUG, "Decoder is draining, eos\n");
    return AVERROR_EOF;
  }

  if (xpkt->data_len == 0)
  {
    memset(xpkt, 0, sizeof(ni_packet_t));
    xpkt->pts = pkt->pts;
    xpkt->dts = pkt->dts;
    //    xpkt->pos = pkt->pos;
    xpkt->video_width = avctx->width;
    xpkt->video_height = avctx->height;
    xpkt->p_data = NULL;
    xpkt->data_len = pkt->size;

    if (avctx->extradata_size > 0 &&
        (avctx->extradata_size != s->extradata_size) &&
        ff_xcoder_add_headers(avctx, pkt))
    {
      if (avctx->extradata_size > s->api_ctx.max_nvme_io_size * 2)
      {
        av_log(avctx, AV_LOG_ERROR, "ff_xcoder_dec_send extradata_size %d "
               "exceeding max size supported: %d\n", avctx->extradata_size,
               s->api_ctx.max_nvme_io_size * 2);
      }
      else
      {
        av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send extradata_size %d "
               "copied to pkt start.\n", avctx->extradata_size);
        s->api_ctx.prev_size = avctx->extradata_size;
        memcpy(s->api_ctx.p_leftover, avctx->extradata, avctx->extradata_size);
      }
    }

    // if the pkt is a lone SEI, save it to be sent with the next data frame
    if (pkt_is_sei_only(avctx, pkt))
    {
      memcpy(s->api_ctx.buf_lone_sei + s->api_ctx.lone_sei_size,
             pkt->data, pkt->size);
      s->api_ctx.lone_sei_size += pkt->size;
      xpkt->data_len = 0;

      av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send pkt lone SEI, saved, "
             "and return %d\n", pkt->size);
      return pkt->size;
    }

    // embed lone SEI saved previously (if any) to send to decoder
    if (s->api_ctx.lone_sei_size)
    {
      av_log(avctx, AV_LOG_TRACE, "ff_xcoder_dec_send copy over lone SEI "
             "data size: %d\n", s->api_ctx.lone_sei_size);

      memcpy(s->api_ctx.p_leftover + s->api_ctx.prev_size,
             s->api_ctx.buf_lone_sei, s->api_ctx.lone_sei_size);
      s->api_ctx.prev_size += s->api_ctx.lone_sei_size;
      s->api_ctx.lone_sei_size = 0;
    }
#if 0
    if (s->enable_user_data_sei_sw_passthru == 1)
    {
      ret = ff_xcoder_detect_user_data_unreg_sei(avctx, pkt, xpkt);
      if (ret != 0)
      {
        goto fail;
      }
    }
#endif
    if ((pkt->size + s->api_ctx.prev_size) > 0)
    {
      ni_packet_buffer_alloc(xpkt, (pkt->size + s->api_ctx.prev_size));
      if (!xpkt->p_data)
      {
        ret = AVERROR(ENOMEM);
        goto fail;
      }
    }
    new_packet = 1;
  }
  else
  {
    send_size = xpkt->data_len;
  }

  av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send: pkt->size=%d\n", pkt->size);

  if (s->started == 0)
  {
    xpkt->start_of_stream = 1;
    s->started = 1;
  }

  if (need_draining && !s->draining)
  {
    av_log(avctx, AV_LOG_DEBUG, "Sending End Of Stream signal\n");
    xpkt->end_of_stream = 1;
    xpkt->data_len = 0;

    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy before: size=%d, s->prev_size=%d, send_size=%d (end of stream)\n", pkt->size, s->api_ctx.prev_size, send_size);
    if (new_packet)
    {
      send_size = ni_packet_copy(xpkt->p_data, pkt->data, pkt->size, s->api_ctx.p_leftover, &s->api_ctx.prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = s->offset;
      if (s->api_ctx.is_dec_pkt_512_aligned)
      {
        s->offset += send_size;
      }
      else
      {
        s->offset += pkt->size;
      }
    }
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, xpkt->data_len=%d (end of stream)\n", pkt->size, s->api_ctx.prev_size, send_size, xpkt->data_len);

    if (send_size < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to copy pkt (status = "
                                  "%d)\n",
             send_size);
      ret = AVERROR_EXTERNAL;
      goto fail;
    }
    if (s->api_ctx.is_dec_pkt_512_aligned)
    {
      xpkt->data_len = send_size;
    }

    sent = 0;
    if (xpkt->data_len > 0)
    {
      sent = ni_device_session_write(&(s->api_ctx), &(s->api_pkt), NI_DEVICE_TYPE_DECODER);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send eos signal (status = %d)\n",
             sent);
      if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
      {
        ret = xcoder_decode_reset(avctx);
        if (0 == ret)
        {
          ret = AVERROR(EAGAIN);
        }
      }
      else
      {
        ret = AVERROR_EOF;
      }
      goto fail;
    }
    av_log(avctx, AV_LOG_DEBUG, "Queued eos (status = %d) ts=%llu\n",
           sent, xpkt->pts);
    s->draining = 1;

    ni_device_session_flush(&(s->api_ctx), NI_DEVICE_TYPE_DECODER);
  }
  else
  {
#if 0
    if (pkt->pts == AV_NOPTS_VALUE)
      av_log(avctx, AV_LOG_DEBUG, "DEC avpkt pts : NOPTS size %d  pos %lld \n",
       pkt->size,  pkt->pos);
    else
      av_log(avctx, AV_LOG_DEBUG, "DEC avpkt pts : %lld  dts : %lld  size %d  pos %lld \n", pkt->pts, pkt->dts, pkt->size,
       pkt->pos);
#endif
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy before: size=%d, s->prev_size=%d, send_size=%d\n", pkt->size, s->api_ctx.prev_size, send_size);
    if (new_packet)
    {
      send_size = ni_packet_copy(xpkt->p_data, pkt->data, pkt->size, s->api_ctx.p_leftover, &s->api_ctx.prev_size);
      // increment offset of data sent to decoder and save it
      xpkt->pos = s->offset;
      if (s->api_ctx.is_dec_pkt_512_aligned)
      {
        s->offset += send_size;
      }
      else
      {
        s->offset += pkt->size;
      }
    }
    av_log(avctx, AV_LOG_TRACE, "ni_packet_copy after: size=%d, s->prev_size=%d, send_size=%d, xpkt->data_len=%d\n", pkt->size, s->api_ctx.prev_size, send_size, xpkt->data_len);

    if (send_size < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to copy pkt (status = "
                                  "%d)\n",
             send_size);
      ret = AVERROR_EXTERNAL;
      goto fail;
    }
    if (s->api_ctx.is_dec_pkt_512_aligned)
    {
      xpkt->data_len = send_size;
    }

    sent = 0;
    if (xpkt->data_len > 0)
    {
      sent = ni_device_session_write(&s->api_ctx, &(s->api_pkt), NI_DEVICE_TYPE_DECODER);
      av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_send pts=%" PRIi64 ", dts=%" PRIi64 ", pos=%" PRIi64 ", sent=%d\n", pkt->pts, pkt->dts, pkt->pos, sent);
    }
    if (sent < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to send compressed pkt (status = "
                                  "%d)\n",
             sent);
      if (NI_RETCODE_ERROR_VPU_RECOVERY == sent)
      {
        ret = xcoder_decode_reset(avctx);
        if (0 == ret)
        {
          ret = AVERROR(EAGAIN);
        }
      }
      else
      {
        ret = AVERROR_EOF;
      }
      goto fail;
    }
    else if (sent == 0)
    {
      av_log(avctx, AV_LOG_DEBUG, "Queued input buffer size=0\n");
    }
    else if (sent < size)
    { /* partial sent; keep trying */
      av_log(avctx, AV_LOG_DEBUG, "Queued input buffer size=%d\n", sent);
    }
  }

  if (sent != 0)
  { //keep the current pkt to resend next time
    ni_packet_buffer_free(xpkt);
  }

  if (xpkt->data_len == 0)
  {
    /* if this packet is done sending, free any sei buffer. */
    if (xpkt->p_user_data_unreg_sei)
    {
      free(xpkt->p_user_data_unreg_sei);
      xpkt->p_user_data_unreg_sei = NULL;
      xpkt->user_data_unreg_sei_len = 0;
    }
  }

  if (sent == 0)
  {
    return AVERROR(EAGAIN);
  }
  
  return sent;

fail:
  ni_packet_buffer_free(xpkt);
  if (xpkt->p_user_data_unreg_sei)
  {
    free(xpkt->p_user_data_unreg_sei);
    xpkt->p_user_data_unreg_sei = NULL;
    xpkt->user_data_unreg_sei_len = 0;
  }
  return ret;
}

int retrieve_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                   ni_frame_t *xfme)
{
  XCoderH264DecContext *s = avctx->priv_data;

  int buf_size = xfme->data_len[0] + xfme->data_len[1] + xfme->data_len[2];
  uint8_t *buf = xfme->p_data[0];
  int stride = 0;
  int res = 0;

  AVFrame *frame = data;

  av_log(avctx, AV_LOG_DEBUG, "decoding %" PRId64 " frame ...\n", s->api_ctx.frame_num);

  if (avctx->width <= 0)
  {
    av_log(avctx, AV_LOG_ERROR, "width is not set\n");
    return AVERROR_INVALIDDATA;
  }
  if (avctx->height <= 0)
  {
    av_log(avctx, AV_LOG_ERROR, "height is not set\n");
    return AVERROR_INVALIDDATA;
  }

  stride = s->api_ctx.active_video_width;

  av_log(avctx, AV_LOG_DEBUG, "XFRAME SIZE: %d, STRIDE: %d\n", buf_size, stride);

  if (stride == 0 || buf_size < stride * avctx->height)
  {
    av_log(avctx, AV_LOG_ERROR, "Packet too small (%d)\n", buf_size);
    return AVERROR_INVALIDDATA;
  }

  frame->key_frame = 0;

  switch (xfme->ni_pict_type)
  {
    case PIC_TYPE_I:
    case DECODER_PIC_TYPE_IDR:
      frame->pict_type = AV_PICTURE_TYPE_I;
      frame->key_frame = 1;
      break;
    case PIC_TYPE_P:
      frame->pict_type = AV_PICTURE_TYPE_P;
      break;
    case PIC_TYPE_B:
      frame->pict_type = AV_PICTURE_TYPE_B;
      break;
    default:
      frame->pict_type = AV_PICTURE_TYPE_NONE;
  }

  res = ff_decode_frame_props(avctx, frame);
  if (res < 0)
    return res;

  frame->pkt_pos = avctx->internal->last_pkt_props->pos;
  frame->pkt_duration = avctx->internal->last_pkt_props->duration;

  if ((res = av_image_check_size(xfme->video_width, xfme->video_height, 0, avctx)) < 0)
    return res;

  frame->buf[0] = av_buffer_create(buf, buf_size, ni_align_free,
                                   xfme->dec_buf, 0);

  buf = frame->buf[0]->data;

  // User Data Unregistered SEI if available
  if (s->enable_user_data_sei_sw_passthru &&
      xfme->sei_user_data_unreg_len && xfme->sei_user_data_unreg_offset)
  {
    uint8_t *sei_buf = (uint8_t *)xfme->p_data[0] +
    xfme->sei_user_data_unreg_offset;

    AVFrameSideData* sd = av_frame_new_side_data(
      frame, AV_FRAME_DATA_NETINT_UDU_SEI, xfme->sei_user_data_unreg_len);

    if (sd)
    {
      memcpy(sd->data, sei_buf, xfme->sei_user_data_unreg_len);
    }
  }

  // close caption data if available
  if (xfme->sei_cc_len && xfme->sei_cc_offset)
  {
    uint8_t *sei_buf = (uint8_t *)xfme->p_data[0] + xfme->sei_cc_offset;
    AVFrameSideData* sd = av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC,
                                                 xfme->sei_cc_len);
    if (sd)
    {
      memcpy(sd->data, sei_buf, xfme->sei_cc_len);
    }
    avctx->properties |= FF_CODEC_PROPERTY_CLOSED_CAPTIONS;
  }

  if (xfme->p_user_data_unreg_sei)
  {
    AVFrameSideData *sd = av_frame_new_side_data(frame, AV_FRAME_DATA_NETINT_UDU_SEI,
                                                 xfme->user_data_unreg_sei_len);
    if (sd)
    {
      memcpy(sd->data, xfme->p_user_data_unreg_sei, xfme->user_data_unreg_sei_len);
    }
  }

  frame->pkt_dts = xfme->pts;
  if (xfme->pts != NI_NOPTS_VALUE)
  {
    frame->pts = xfme->pts;
  }
  else
  {
    s->current_pts += frame->pkt_duration;
    frame->pts = s->current_pts;
  }

  av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: frame->buf[0]=%p, frame->data=%p, frame->pts=%" PRId64 ", frame size=%d, s->current_pts=%" PRId64 ", frame->pkt_pos=%" PRId64 ", frame->pkt_duration=%" PRId64 " sei size %d offset %u\n", frame->buf[0], frame->data, frame->pts, buf_size, s->current_pts, frame->pkt_pos, frame->pkt_duration, xfme->sei_cc_len, xfme->sei_cc_offset);

  /* av_buffer_ref(avpkt->buf); */
  if (!frame->buf[0])
    return AVERROR(ENOMEM);

  if ((res = av_image_fill_arrays(frame->data, frame->linesize,
                                  buf, avctx->pix_fmt,
                                  s->api_ctx.active_video_width,
                                  s->api_ctx.active_video_height, 1)) < 0)
  {
    av_buffer_unref(&frame->buf[0]);
    return res;
  }

  av_log(avctx, AV_LOG_DEBUG, "retrieve_frame: success av_image_fill_arrays "
         "return %d\n", res);
  frame->width = s->api_ctx.active_video_width;
  frame->height = s->api_ctx.active_video_height;
  frame->crop_top = xfme->crop_top;
  frame->crop_bottom = s->api_ctx.active_video_height - xfme->crop_bottom;
  frame->crop_left = xfme->crop_left;
  frame->crop_right = s->api_ctx.active_video_width - xfme->crop_right;

  *got_frame = 1;
  return buf_size;
}

int ff_xcoder_dec_receive(AVCodecContext *avctx, XCoderH264DecContext *s,
                          AVFrame *frame, bool wait)
{
  /* call xcode_dec_receive to get a decoded YUV frame from the decoder
     instance */
  int ret = 0;
  int got_frame = 0;
  ni_session_data_io_t session_io_data = {0};
  ni_session_data_io_t * p_session_data = &session_io_data;
  int width, height;

  if (s->draining && s->eos)
  {
    return AVERROR_EOF;
  }

  // if active video resolution has been obtained we just use it as it's the 
  // exact size of frame to be returned, otherwise we use what we are told by 
  // upper stream as the initial setting and it will be adjusted.
  width = s->api_ctx.active_video_width > 0 ? s->api_ctx.active_video_width :  avctx->width;
  height = s->api_ctx.active_video_height > 0 ? s->api_ctx.active_video_height : avctx->height;

  // allocate memory only after resolution is known (buffer pool set up)
  int alloc_mem = (s->api_ctx.active_video_width > 0 && 
                   s->api_ctx.active_video_height > 0 ? 1 : 0);
  ret = ni_decoder_frame_buffer_alloc(
    s->api_ctx.dec_fme_buf_pool, &(p_session_data->data.frame), alloc_mem,
    width, height,
    (avctx->codec_id == AV_CODEC_ID_H264), s->api_ctx.bit_depth_factor);
                          
  if (NI_RETCODE_SUCCESS != ret)
  {
    return AVERROR_EXTERNAL;
  }

  ret = ni_device_session_read(&s->api_ctx, p_session_data, NI_DEVICE_TYPE_DECODER);
  if (ret == 0)
  {

    s->eos = p_session_data->data.frame.end_of_stream;
    ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
    return AVERROR(EAGAIN);
  }
  else if (ret > 0)
  {
    av_log(avctx, AV_LOG_DEBUG, "Got output buffer pts=%lld "
                                  "dts=%lld eos=%d sos=%d\n",
           p_session_data->data.frame.pts, p_session_data->data.frame.dts,
           p_session_data->data.frame.end_of_stream, p_session_data->data.frame.start_of_stream);

    s->eos = p_session_data->data.frame.end_of_stream;

    // update ctxt resolution if change has been detected
    frame->width = p_session_data->data.frame.video_width;
    frame->height = p_session_data->data.frame.video_height;
    if (frame->width != avctx->width || frame->height != avctx->height)
    {
      // Do not show resolution changed if cropped,eg 1920x1088
      if((p_session_data->data.frame.crop_right - p_session_data->data.frame.crop_left) != avctx->width
         || (p_session_data->data.frame.crop_bottom - p_session_data->data.frame.crop_top) != avctx->height)
           av_log(avctx, AV_LOG_WARNING, "ff_xcoder_dec_receive: resolution "
             "changed: %dx%d to %dx%d\n", avctx->width, avctx->height,
             frame->width, frame->height);
      avctx->width = frame->width;
      avctx->height = frame->height;
    }

    frame->format = avctx->pix_fmt;          /* ??? AV_PIX_FMT_YUV420P */

    retrieve_frame(avctx, frame, &got_frame, &(p_session_data->data.frame));
    av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_receive: got_frame=%d, frame->width=%d, frame->height=%d, crop top %" SIZE_SPECIFIER " bottom %" SIZE_SPECIFIER " left %" SIZE_SPECIFIER " right %" SIZE_SPECIFIER ", frame->format=%d, frame->linesize=%d/%d/%d\n", got_frame, frame->width, frame->height, frame->crop_top, frame->crop_bottom, frame->crop_left, frame->crop_right, frame->format, frame->linesize[0], frame->linesize[1], frame->linesize[2]);

#if FF_API_PKT_PTS
    FF_DISABLE_DEPRECATION_WARNINGS
    frame->pkt_pts = frame->pts;
    FF_ENABLE_DEPRECATION_WARNINGS
#endif
    frame->best_effort_timestamp = frame->pts;
#if 0
    av_log(avctx, AV_LOG_DEBUG, "\n   NI dec out frame: pts  %lld  pkt_dts  %lld   pkt_pts  %lld \n\n", frame->pts, frame->pkt_dts,
     frame->pkt_pts);
#endif
    av_log(avctx, AV_LOG_DEBUG, "ff_xcoder_dec_receive: pkt_timebase= %d/%d, frame_rate=%d/%d, frame->pts=%" PRId64 ", frame->pkt_dts=%" PRId64 "\n", avctx->pkt_timebase.num, avctx->pkt_timebase.den, avctx->framerate.num, avctx->framerate.den, frame->pts, frame->pkt_dts);

    // release buffer ownership and let frame owner return frame buffer to 
    // buffer pool later
    p_session_data->data.frame.dec_buf = NULL;

    if (p_session_data->data.frame.p_user_data_unreg_sei)
    {
      free(p_session_data->data.frame.p_user_data_unreg_sei);
      p_session_data->data.frame.p_user_data_unreg_sei = NULL;
      p_session_data->data.frame.user_data_unreg_sei_len = 0;
    }
  }
  else
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to get output buffer (status = %d)\n",
           ret);
    
    if (NI_RETCODE_ERROR_VPU_RECOVERY == ret)
    {
      av_log(avctx, AV_LOG_WARNING, "ff_xcoder_dec_receive VPU recovery, need to reset ..\n");
      ni_decoder_frame_buffer_free(&(p_session_data->data.frame));
      return ret;
    }

    return AVERROR_EOF;
  }

  ret = 0;

  return ret;
}

int ff_xcoder_dec_is_flushing(AVCodecContext *avctx,
                              XCoderH264DecContext *s)
{
  return s->flushing;
}

int ff_xcoder_dec_flush(AVCodecContext *avctx,
                        XCoderH264DecContext *s)
{
  s->draining = 0;
  s->flushing = 0;
  s->eos = 0;

#if 0
  int ret;
  ret = ni_device_session_flush(s, NI_DEVICE_TYPE_DECODER);
  if (ret < 0) {
    av_log(avctx, AV_LOG_ERROR, "Failed to flush decoder (status = %d)\n", ret);
    return AVERROR_EXTERNAL;
  }
#endif

  /* Future: for now, always return 1 to indicate the codec has been flushed
     and it leaves the flushing state and can process again ! will consider
     case of user retaining frames in HW "surface" usage */
  return 1;
}
