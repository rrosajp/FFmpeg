/*
 * Filter layer
 * copyright (c) 2007 Bobby Bingham
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "avfilter.h"
#include "allfilters.h"

/** list of registered filters, sorted by name */
static int filter_count = 0;
static AVFilter **filters = NULL;

/* TODO: buffer pool.  see comment for avfilter_default_get_video_buffer() */
void avfilter_default_free_video_buffer(AVFilterPic *pic)
{
    avpicture_free((AVPicture *) pic);
    av_free(pic);
}

/* TODO: set the buffer's priv member to a context structure for the whole
 * filter chain.  This will allow for a buffer pool instead of the constant
 * alloc & free cycle currently implemented. */
AVFilterPicRef *avfilter_default_get_video_buffer(AVFilterLink *link, int perms)
{
    AVFilterPic *pic = av_mallocz(sizeof(AVFilterPic));
    AVFilterPicRef *ref = av_mallocz(sizeof(AVFilterPicRef));

    ref->pic = pic;
    ref->w = link->w;
    ref->h = link->h;
    ref->perms = perms;

    pic->refcount = 1;
    pic->format = link->format;
    pic->free = avfilter_default_free_video_buffer;
    avpicture_alloc((AVPicture *)pic, pic->format, ref->w, ref->h);

    memcpy(ref->data, pic->data, sizeof(pic->data));
    memcpy(ref->linesize, pic->linesize, sizeof(pic->linesize));

    return ref;
}

void avfilter_default_start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    link->cur_pic = picref;
}

void avfilter_default_end_frame(AVFilterLink *link)
{
    avfilter_unref_pic(link->cur_pic);
    link->cur_pic = NULL;
}

AVFilterPicRef *avfilter_ref_pic(AVFilterPicRef *ref)
{
    AVFilterPicRef *ret = av_malloc(sizeof(AVFilterPicRef));
    memcpy(ret, ref, sizeof(AVFilterPicRef));
    ret->pic->refcount ++;
    return ret;
}

void avfilter_unref_pic(AVFilterPicRef *ref)
{
    if(-- ref->pic->refcount == 0)
        ref->pic->free(ref->pic);
    av_free(ref);
}

int avfilter_link(AVFilterContext *src, unsigned srcpad,
                  AVFilterContext *dst, unsigned dstpad)
{
    AVFilterLink *link;

    if(src->outputs[srcpad] || dst->inputs[dstpad])
        return -1;

    src->outputs[srcpad] =
    dst->inputs[dstpad]  = link = av_malloc(sizeof(AVFilterLink));

    link->src = src;
    link->dst = dst;
    link->srcpad = srcpad;
    link->dstpad = dstpad;
    link->cur_pic = NULL;

    src->filter->outputs[dstpad].set_video_props(link);
    return 0;
}

AVFilterPicRef *avfilter_get_video_buffer(AVFilterLink *link, int perms)
{
    AVFilterPicRef *ret = NULL;

    if(link->dst->filter->inputs[link->dstpad].get_video_buffer)
        ret = link->dst->filter->inputs[link->dstpad].get_video_buffer(link, perms);

    if(!ret)
        ret = avfilter_default_get_video_buffer(link, perms);

    return ret;
}

void avfilter_request_frame(AVFilterLink *link)
{
    link->src->filter->outputs[link->srcpad].request_frame(link);
}

/* XXX: should we do the duplicating of the picture ref here, instead of
 * forcing the source filter to do it? */
void avfilter_start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    void (*start_frame)(AVFilterLink *, AVFilterPicRef *);

    start_frame = link->dst->filter->inputs[link->dstpad].start_frame;
    if(!start_frame)
        start_frame = avfilter_default_start_frame;

    start_frame(link, picref);
}

void avfilter_end_frame(AVFilterLink *link)
{
    void (*end_frame)(AVFilterLink *);

    end_frame = link->dst->filter->inputs[link->dstpad].end_frame;
    if(!end_frame)
        end_frame = avfilter_default_end_frame;

    end_frame(link);
}

void avfilter_draw_slice(AVFilterLink *link, uint8_t *data[4], int y, int h)
{
    link->dst->filter->inputs[link->dstpad].draw_slice(link, data, y, h);
}

static int filter_cmp(const void *aa, const void *bb)
{
    const AVFilter *a = *(const AVFilter **)aa, *b = *(const AVFilter **)bb;
    return strcmp(a->name, b->name);
}

AVFilter *avfilter_get_by_name(char *name)
{
    AVFilter key = { .name = name, };
    AVFilter *key2 = &key;
    AVFilter **ret;

    ret = bsearch(&key2, filters, filter_count, sizeof(AVFilter **), filter_cmp);
    if(ret)
        return *ret;
    return NULL;
}

/* FIXME: insert in order, rather than insert at end + resort */
void avfilter_register(AVFilter *filter)
{
    filters = av_realloc(filters, sizeof(AVFilter*) * (filter_count+1));
    filters[filter_count] = filter;
    qsort(filters, ++filter_count, sizeof(AVFilter **), filter_cmp);
}

void avfilter_init(void)
{
    avfilter_register(&vsrc_dummy);
    avfilter_register(&vf_crop);
    avfilter_register(&vf_passthrough);
    avfilter_register(&vo_sdl);
}

void avfilter_uninit(void)
{
    av_freep(&filters);
    filter_count = 0;
}

static int pad_count(const AVFilterPad *pads)
{
    AVFilterPad *p = (AVFilterPad *) pads;
    int count;

    for(count = 0; p->name; count ++) p ++;
    return count;
}

static const char *filter_name(void *p)
{
    AVFilterContext *filter = p;
    return filter->filter->name;
}

AVFilterContext *avfilter_create(AVFilter *filter)
{
    AVFilterContext *ret = av_malloc(sizeof(AVFilterContext));

    ret->av_class = av_mallocz(sizeof(AVClass));
    ret->av_class->item_name = filter_name;
    ret->filter   = filter;
    ret->inputs   = av_mallocz(sizeof(AVFilterLink*) * pad_count(filter->inputs));
    ret->outputs  = av_mallocz(sizeof(AVFilterLink*) * pad_count(filter->outputs));
    ret->priv     = av_mallocz(filter->priv_size);

    return ret;
}

void avfilter_destroy(AVFilterContext *filter)
{
    int i;

    if(filter->filter->uninit)
        filter->filter->uninit(filter);

    for(i = 0; i < pad_count(filter->filter->inputs); i ++) {
        if(filter->inputs[i])
            filter->inputs[i]->src->outputs[filter->inputs[i]->srcpad] = NULL;
        av_free(filter->inputs[i]);
    }
    for(i = 0; i < pad_count(filter->filter->outputs); i ++) {
        if(filter->outputs[i])
            filter->outputs[i]->dst->inputs[filter->outputs[i]->dstpad] = NULL;
        av_free(filter->outputs[i]);
    }

    av_free(filter->inputs);
    av_free(filter->outputs);
    av_free(filter->priv);
    av_free(filter->av_class);
    av_free(filter);
}

AVFilterContext *avfilter_create_by_name(char *name)
{
    AVFilter *filt;

    if(!(filt = avfilter_get_by_name(name))) return NULL;
    return avfilter_create(filt);
}

int avfilter_init_filter(AVFilterContext *filter)
{
    int ret, i;

    if(filter->filter->init)
        if((ret = filter->filter->init(filter))) return ret;
    for(i = 0; i < pad_count(filter->filter->outputs); i ++)
        if(filter->outputs[i])
            filter->filter->outputs[i].set_video_props(filter->outputs[i]);
    return 0;
}

