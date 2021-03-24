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
 * XCoder codec lib wrapper header.
 */

#ifndef AVCODEC_NICODEC_H
#define AVCODEC_NICODEC_H

#include <stdbool.h>
#include <time.h>
#include "avcodec.h"
#include "libavutil/fifo.h"

#include <ni_device_api.h>

/* enum for specifying xcoder device/coder index; can be specified in either
   decoder or encoder options. */
enum {
  BEST_DEVICE_INST = -2,
  BEST_DEVICE_LOAD = -1
};

typedef struct XCoderH264DecContext {
  AVClass *avclass;

  char *dev_xcoder;     /* dev name of the xcoder card to use */
  char *blk_xcoder;     /* blk name of the xcoder card to use */
  int  dev_dec_idx;     /* index of the decoder on the xcoder card */
  int  nvme_io_size;    /* custom nvme io size */
  int  yuv_copy_bypass; /* enable YUV seperate write on encoder write, reduce memcpy on host */
  ni_device_context_t *rsrc_ctx;  /* resource management context */

  ni_session_context_t api_ctx;
  ni_encoder_params_t  api_param;
  ni_session_data_io_t   api_pkt;

  AVPacket buffered_pkt;

  // stream header copied/saved from AVCodecContext.extradata
  int got_first_idr;
  uint8_t *extradata;
  int extradata_size;

  int64_t current_pts;
  unsigned long long offset;

  int started;
  int draining;
  int flushing;
  int eos;

  /* below are all command line options */
  int enable_user_data_sei_sw_passthru;
} XCoderH264DecContext;

typedef struct XCoderH265EncContext {
  AVClass *avclass;

  char *dev_xcoder;     /* dev name of the xcoder card to use */
  char *blk_xcoder;     /* blk name of the xcoder card to use */
  int  dev_enc_idx;     /* index of the encoder on the xcoder card */
  int  nvme_io_size;    /* custom nvme io size */
  int  yuv_copy_bypass; /* enable YUV seperate write on encoder write, reduce memcpy on host */

  ni_device_context_t *rsrc_ctx;  /* resource management context */
  unsigned long xcode_load_pixel; /* xcode load in pixels by this encode task */

  // frame fifo, to be used for sequence change frame buffering
  AVFifoBuffer *fme_fifo;
  int eos_fme_received;
  AVFrame *buffered_fme;

  ni_session_data_io_t  api_pkt; /* used for receiving bitstream from xcoder */
  ni_session_data_io_t   api_fme; /* used for sending YUV data to xcoder */
  ni_session_context_t api_ctx;
  ni_encoder_params_t  api_param;

  int started;
  uint8_t *p_spsPpsHdr;
  int spsPpsHdrLen;
  int spsPpsArrived;
  int firstPktArrived;
  int64_t dtsOffset;
  int gop_offset_count;/*this is a counter to guess the pts only dtsOffset times*/
  uint64_t total_frames_received;
  int64_t first_frame_pts;
  int64_t latest_dts;

  int encoder_flushing;
  int encoder_eof;

  /* backup copy of original values of -xcoder and -enc command line option */
  char orig_dev_xcoder[MAX_DEVICE_NAME_LEN];
  int  orig_dev_enc_idx;
  
  /* below are all command line options */
  char *xcoder_opts;
  char *xcoder_gop;

  int reconfigCount;
  ni_encoder_change_params_t *g_enc_change_params;
  // low delay mode flags
  int gotPacket; /* used to stop receiving packets when a packet is already received */
  int sentFrame; /* used to continue receiving packets when a frame is sent and a packet is not yet received */
  AVFrame frame_hold;
} XCoderH265EncContext;


int ff_xcoder_dec_close(AVCodecContext *avctx,
                        XCoderH264DecContext *s);

int ff_xcoder_dec_init(AVCodecContext *avctx,
                       XCoderH264DecContext *s);

int ff_xcoder_dec_send(AVCodecContext *avctx,
                       XCoderH264DecContext *s,
                       AVPacket *pkt);

int ff_xcoder_dec_receive(AVCodecContext *avctx,
                          XCoderH264DecContext *s,
                          AVFrame *frame,
                          bool wait);

int ff_xcoder_dec_is_flushing(AVCodecContext *avctx,
                              XCoderH264DecContext *s);

int ff_xcoder_dec_flush(AVCodecContext *avctx,
                        XCoderH264DecContext *s);

int retrieve_frame(AVCodecContext *avctx, AVFrame *data, int *got_frame,
                   ni_frame_t *xfme);
int ff_xcoder_add_headers(AVCodecContext *avctx, AVPacket *pkt);
#endif /* AVCODEC_NICODEC_H */
