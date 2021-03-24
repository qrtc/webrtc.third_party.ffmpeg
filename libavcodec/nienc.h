/*
 * NetInt XCoder H.264/HEVC Encoder common code header
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

#ifndef AVCODEC_NIENC_H
#define AVCODEC_NIENC_H

#include <ni_rsrc_api.h>
#include <ni_util.h>

#include "libavutil/internal.h"

#include "avcodec.h"
#include "internal.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"

#include "nicodec.h"

int xcoder_encode_init(AVCodecContext *avctx);
  
int xcoder_encode_close(AVCodecContext *avctx);

int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame);

int xcoder_receive_packet(AVCodecContext *avctx, AVPacket *pkt);

int xcoder_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
const AVFrame *frame, int *got_packet);

#endif /* AVCODEC_NIENC_H */
