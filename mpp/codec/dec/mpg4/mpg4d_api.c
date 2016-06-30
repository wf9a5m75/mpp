/*
*
* Copyright 2015 Rockchip Electronics Co. LTD
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#define MODULE_TAG "mpg4d_api"

#include <string.h>

#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_common.h"
#include "mpp_packet.h"

#include "mpg4d_api.h"
#include "mpg4d_parser.h"

#define MPG4D_INIT_STREAM_SIZE      SZ_64K

typedef struct {
    // parameter interact with mpp_dec
    MppBufSlots     frame_slots;
    MppBufSlots     packet_slots;
    RK_S32          task_count;
    RK_U8           *stream;
    size_t          stream_size;
    MppPacket       task_pkt;
    RK_S64          task_pts;
    RK_U32          task_eos;

    // runtime parameter
    RK_U32          need_split;
    RK_U32          frame_count;
    IOInterruptCB   notify_cb;

    // parser context
    Mpg4dParser     parser;
} Mpg4dCtx;

MPP_RET mpg4d_init(void *dec, ParserCfg *cfg)
{
    Mpg4dParser parser = NULL;
    MppPacket task_pkt = NULL;
    Mpg4dCtx *p;
    MPP_RET ret;
    RK_U8 *stream;
    size_t stream_size = MPG4D_INIT_STREAM_SIZE;

    if (NULL == dec) {
        mpp_err_f("found NULL intput dec %p cfg %p\n", dec, cfg);
        return MPP_ERR_NULL_PTR;
    }

    stream = mpp_malloc_size(RK_U8, stream_size);
    if (NULL == stream) {
        mpp_err_f("failed to malloc stream buffer size %d\n", stream_size);
        return MPP_ERR_MALLOC;
    }

    ret = mpp_packet_init(&task_pkt, stream, stream_size);
    if (ret) {
        mpp_err_f("failed to create mpp_packet for task\n");
        goto ERR_RET;
    }

    // reset task packet length to zero
    // NOTE: set length must after set pos
    mpp_packet_set_pos(task_pkt, stream);
    mpp_packet_set_length(task_pkt, 0);

    ret = mpp_mpg4_parser_init(&parser, cfg->frame_slots);
    if (ret) {
        mpp_err_f("failed to init parser\n");
        goto ERR_RET;
    }

    p = (Mpg4dCtx *)dec;
    p->frame_slots  = cfg->frame_slots;
    p->packet_slots = cfg->packet_slots;
    p->need_split   = cfg->need_split;
    p->task_count   = cfg->task_count = 2;
    p->notify_cb    = cfg->notify_cb;
    p->stream       = stream;
    p->stream_size  = stream_size;
    p->task_pkt     = task_pkt;
    p->parser       = parser;

    return MPP_OK;
ERR_RET:
    if (task_pkt) {
        mpp_packet_deinit(&task_pkt);
    }
    if (stream) {
        mpp_free(stream);
        stream = NULL;
    }
    return ret;
}

MPP_RET mpg4d_deinit(void *dec)
{
    Mpg4dCtx *p;
    if (NULL == dec) {
        mpp_err_f("found NULL intput\n");
        return MPP_ERR_NULL_PTR;
    }

    p = (Mpg4dCtx *)dec;
    if (p->parser) {
        mpp_mpg4_parser_deinit(p->parser);
        p->parser = NULL;
    }

    if (p->task_pkt) {
        mpp_packet_deinit(&p->task_pkt);
    }

    if (p->stream) {
        mpp_free(p->stream);
        p->stream = NULL;
    }
    return MPP_OK;
}

MPP_RET mpg4d_reset(void *dec)
{
    if (NULL == dec) {
        mpp_err_f("found NULL intput\n");
        return MPP_ERR_NULL_PTR;
    }
    return MPP_OK;
}


MPP_RET mpg4d_flush(void *dec)
{
    if (NULL == dec) {
        mpp_err_f("found NULL intput\n");
        return MPP_ERR_NULL_PTR;
    }
    return MPP_OK;
}


MPP_RET mpg4d_control(void *dec, RK_S32 cmd_type, void *param)
{
    if (NULL == dec) {
        mpp_err_f("found NULL intput\n");
        return MPP_ERR_NULL_PTR;
    }
    (void)cmd_type;
    (void)param;
    return MPP_OK;
}

MPP_RET mpg4d_prepare(void *dec, MppPacket pkt, HalDecTask *task)
{
    Mpg4dCtx *p;
    RK_U8 *pos;
    size_t length;

    if (NULL == dec || NULL == pkt || NULL == task) {
        mpp_err_f("found NULL intput dec %p pkt %p task %p\n", dec, pkt, task);
        return MPP_ERR_NULL_PTR;
    }

    p = (Mpg4dCtx *)dec;
    pos     = mpp_packet_get_pos(pkt);
    length  = mpp_packet_get_length(pkt);

    if (NULL == p->stream) {
        mpp_err("failed to malloc task buffer for hardware with size %d\n", length);
        return MPP_ERR_UNKNOW;
    }

    if (!p->need_split) {
        /*
         * Copy packet mode:
         * Decoder's user will insure each packet is one frame for process
         * Parser will just copy packet to the beginning of stream buffer
         */
        if (length > p->stream_size) {
            // NOTE: here we double the buffer length to reduce frequency of realloc
            do {
                p->stream_size <<= 1;
            } while (length > p->stream_size);

            mpp_free(p->stream);
            p->stream = mpp_malloc_size(RK_U8, p->stream_size);
            mpp_assert(p->stream);
            mpp_packet_set_data(p->task_pkt, p->stream);
            mpp_packet_set_size(p->task_pkt, p->stream_size);
        }

        memcpy(p->stream, pos, length);
        mpp_packet_set_pos(p->task_pkt, p->stream);
        mpp_packet_set_length(p->task_pkt, length);
        // set input packet length to 0 here
        // indicate that the input packet has been all consumed
        mpp_packet_set_pos(pkt, pos + length);
        // always use latest pts for current packet
        p->task_pts = mpp_packet_get_pts(pkt);
        p->task_eos = mpp_packet_get_eos(pkt);
        /* this step will enable the task and goto parse stage */
        task->valid = 1;
    } else {
        /*
         * Split packet mode:
         * Input packet can be any length and no need to be bound of on frame
         * Parser will do split frame operation to find the beginning and end of one frame
         */
        /*
         * NOTE: on split mode total length is the left size plus the new incoming
         *       packet length.
         */
        size_t remain_length = mpp_packet_get_length(p->task_pkt);
        size_t total_length = remain_length + length;
        if (total_length > p->stream_size) {
            RK_U8 *dst;
            do {
                p->stream_size <<= 1;
            } while (length > p->stream_size);

            // NOTE; split mode need to copy remaining stream to new buffer
            dst = mpp_malloc_size(RK_U8, p->stream_size);
            mpp_assert(dst);

            memcpy(dst, p->stream, remain_length);
            mpp_free(p->stream);
            p->stream = dst;
            mpp_packet_set_data(p->task_pkt, p->stream);
            mpp_packet_set_size(p->task_pkt, p->stream_size);
        }

        // start parser split
        if (MPP_OK == mpp_mpg4_parser_split(p->parser, p->task_pkt, pkt)) {
            task->valid = 1;
        }
        p->task_pts = mpp_packet_get_pts(p->task_pkt);
        p->task_eos = mpp_packet_get_eos(p->task_pkt);
    }

    task->input_packet = p->task_pkt;
    task->flags.eos    = p->task_eos;
    return MPP_OK;
}

MPP_RET mpg4d_parse(void *dec, HalDecTask *task)
{
    MPP_RET ret;
    Mpg4dCtx *p;

    if (NULL == dec || NULL == task) {
        mpp_err_f("found NULL intput dec %p task %p\n", dec, task);
        return MPP_ERR_NULL_PTR;
    }
    p = (Mpg4dCtx *)dec;
    ret = mpp_mpg4_parser_decode(p->parser, task->input_packet);
    if (ret) {
        // found error on decoding drop this task and clear remaining length
        task->output = -1;
        mpp_packet_set_length(&task->input_packet, 0);
        return MPP_NOK;
    }

    mpp_mpg4_parser_setup_syntax(p->parser, task->syntax);
    mpp_mpg4_parser_setup_output(p->parser, &task->output);
    mpp_mpg4_parser_setup_refer(p->parser, task->refer, MAX_DEC_REF_NUM);
    mpp_mpg4_parser_setup_display(p->parser);

    p->frame_count++;

    return MPP_OK;
}

MPP_RET mpg4d_callback(void *dec, void *err_info)
{
    (void)dec;
    (void)err_info;
    return MPP_OK;
}
const ParserApi api_mpg4d_parser = {
    "api_mpg4d_parser",
    MPP_VIDEO_CodingMPEG4,
    sizeof(Mpg4dCtx),
    0,
    mpg4d_init,
    mpg4d_deinit,
    mpg4d_prepare,
    mpg4d_parse,
    mpg4d_reset,
    mpg4d_flush,
    mpg4d_control,
    mpg4d_callback,
};
