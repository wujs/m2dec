/** Yet Another H.264 decoder
 *  Copyright 2011 Takayuki Minegishi
 *
 *  Permission is hereby granted, free of charge, to any person
 *  obtaining a copy of this software and associated documentation
 *  files (the "Software"), to deal in the Software without
 *  restriction, including without limitation the rights to use, copy,
 *  modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *  
 *  The above copyright notice and this permission notice shall be
 *  included in all copies or substantial portions of the Software.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <limits.h>

#if defined(_M_IX86) || defined(_M_AMD64)
#define ALIGN16VC __declspec(align(16))
#else
#define ALIGN16VC
#endif
#if (defined(__GNUC__) && defined(__SSE2__)) || defined(_M_IX86) || defined(_M_AMD64)
#define X86ASM
#include <emmintrin.h>
#elif defined(__RENESAS_VERSION__)
extern "C" {
void exit(int);
}
#endif

#include <functional>
#include <algorithm>
#include "bitio.h"
#include "m2d_macro.h"
#include "h264.h"
#include "h264vld.h"

#define MIN(a, b) ((a) <= (b) ? (a) : (b))

static inline int ABS(int a) {
	return (0 <= a) ? a : -a;
}

/** dual 6-tap filter [1, -5, 20, 20, -5, 1] / 32.
 * 0x00008000 are guard-bit to block borrow.
 */
#define FILTER6TAP_DUAL(a, b, c, d, e, f, rnd) (((((c) + (d)) * 4) - (b) - (e)) * 5 + (a) + (f) + ((RND) | 0x00008000))

#define READ_UE_RANGE(dst, st, max) {uint32_t t = ue_golomb(st); (dst) = t; if ((max) < t) return -1;}
#define READ_SE_RANGE(dst, st, min, max) {int32_t t = se_golomb(st); (dst) = t; if (t < (min) || (max) < t) return -1;}

#define UNPACK(a, num) (((a) >> ((num) * 4)) & 15)
#define PACK(a, val, num) ((a) | ((val) << ((num) * 4)))

static const int8_t me_golomb_lut[2][48] = {
	{
		47, 31, 15, 0, 23, 27, 29, 30,
		7, 11, 13, 14, 39, 43, 45, 46,
		16, 3, 5, 10, 12, 19, 21, 26,
		28, 35, 37, 42, 44, 1, 2, 4,
		8, 17, 18, 20, 24, 6, 9, 22,
		25, 32, 33, 34, 36, 40, 38, 41
	},
	{
		0, 16, 1, 2, 4, 8, 32, 3,
		5, 10, 12, 15, 47, 7, 11, 13,
		14, 6, 9, 31, 35, 37, 42, 44,
		33, 34, 36, 40, 39, 43, 45, 46,
		17, 18, 20, 24, 19, 21, 26, 28,
		23, 27, 29, 30, 22, 25, 38, 41
	}
};

static inline int32_t me_golomb(dec_bits *stream, const int8_t *me_lut)
{
	uint32_t ue = ue_golomb(stream);
	return me_lut[(ue < 48) ? ue : 0];
}

static inline int32_t te_golomb(dec_bits *stream, int range)
{
	if (range == 1) {
		return get_onebit_inline(stream) ^ 1;
	} else {
		int32_t ue = ue_golomb(stream);
		return (ue <= range) ? ue : range;
	}
}

static uint32_t get_32bits(dec_bits *stream)
{
	uint32_t t = get_bits(stream, 16);
	return (t << 16) | get_bits(stream, 16);
}


static void hrd_parameters(hrd_parameters_t *hrd, dec_bits *stream)
{
	int max;

	hrd->cpb_cnt_minus1 = max = ue_golomb(stream);
	hrd->bit_rate_scale = get_bits(stream, 4);
	hrd->cpb_size_scale = get_bits(stream, 4);
	hrd->cbr_flag = 0;
	for (int i = 0; i <= max; ++i) {
		hrd->bit_rate_value_minus1[i] = ue_golomb(stream);
		hrd->cpb_size_value_minus1[i] = ue_golomb(stream);
		hrd->cbr_flag |= get_onebit(stream) << i;
	}
	hrd->initial_cpb_removal_delay_length_minus1 = get_bits(stream, 5);
	hrd->cpb_removal_delay_length_minus1 = get_bits(stream, 5);
	hrd->dpb_output_delay_length_minus1 = get_bits(stream, 5);
	hrd->time_offset_length = get_bits(stream, 5);
}

static int vui_parameters(vui_parameters_t *vui, dec_bits *stream)
{
	if ((vui->aspect_ratio_info_present_flag = get_onebit(stream)) != 0) {
		if ((vui->aspect_ratio_idc = get_bits(stream, 8)) == EXTENDED_SAR) {
			vui->sar_width = get_bits(stream, 16);
			vui->sar_height = get_bits(stream, 16);
		}
	}
	if ((vui->overscan_info_present_flag = get_onebit(stream)) != 0) {
		vui->overscan_appropriate_flag = get_onebit(stream);
	}
	if ((vui->video_signal_type_present_flag = get_onebit(stream)) != 0) {
		vui->video_format = get_bits(stream, 3);
		vui->video_full_range_flag = get_onebit(stream);
		if ((vui->colour_description_present_flag = get_onebit(stream)) != 0) {
			vui->colour_primaries = get_bits(stream, 8);
			vui->transfer_characteristics = get_bits(stream, 8);
			vui->matrix_coefficients = get_bits(stream, 8);
		}
	}
	if ((vui->chroma_loc_info_present_flag = get_onebit(stream)) != 0) {
		vui->chroma_sample_loc_type_top_field = ue_golomb(stream);
		vui->chroma_sample_loc_type_bottom_field = ue_golomb(stream);
	}
	if ((vui->timing_info_present_flag = get_onebit(stream)) != 0) {
		vui->num_units_in_tick = get_32bits(stream);
		vui->time_scale = get_32bits(stream);
		vui->fixed_frame_rate_flag = get_onebit(stream);
	}
	if ((vui->nal_hrd_parameters_present_flag = get_onebit(stream)) != 0) {
		hrd_parameters(&vui->nal_hrd_parameters, stream);
	}
	if ((vui->vcl_hrd_parameters_present_flag = get_onebit(stream)) != 0) {
		hrd_parameters(&vui->vcl_hrd_parameters, stream);
	}
	if (vui->nal_hrd_parameters_present_flag || vui->vcl_hrd_parameters_present_flag) {
		vui->low_delay_hrd_flag = get_onebit(stream);
	}
	vui->pic_struct_present_flag = get_onebit(stream);
	if ((vui->bitstream_restriction_flag = get_onebit(stream)) != 0) {
		vui->motion_vectors_over_pic_boundaries_flag = get_onebit(stream);
		vui->max_bytes_per_pic_denom = ue_golomb(stream);
		vui->max_bits_per_mb_denom = ue_golomb(stream);
		vui->log2_max_mv_length_horizontal = ue_golomb(stream);
		vui->log2_max_mv_length_vertical = ue_golomb(stream);
		vui->num_reorder_frames = ue_golomb(stream);
		vui->max_dec_frame_buffering = ue_golomb(stream);
	}
	return 0;
}

static void read_poc_type1_cycle(h264d_sps *sps, dec_bits *st, int max_cycles)
{
	int32_t *offset = sps->offset_for_ref_frame;
	int32_t delta = 0;
	for (int i = 0; i < max_cycles; ++i) {
		delta += se_golomb(st);
		offset[i] = delta;
	}
}

static inline int max_dpb_mbs(int profile_idc, int level_idc, unsigned constrained_set)
{
	int max_dpb;
	if (profile_idc == 100) {
		if (level_idc == 9) {
			level_idc = 10; /* 1b */
		}
	} else {
		if ((level_idc == 10) && (constrained_set & 16)) {
			level_idc = 10; /* 1b */
		}
	}
	switch (level_idc) {
	case 10:
		max_dpb = 396;
		break;
	case 11:
		max_dpb = 900;
		break;
	case 12:
	case 13:
	case 20:
		max_dpb = 2376;
		break;
	case 21:
		max_dpb = 4752;
		break;
	case 22:
	case 30:
		max_dpb = 8100;
		break;
	case 31:
		max_dpb = 18000;
		break;
	case 32:
		max_dpb = 20480;
		break;
	case 40:
	case 41:
		max_dpb = 32768;
		break;
	case 42:
		max_dpb = 34816;
		break;
	case 50:
		max_dpb = 110400;
		break;
	case 51:
		max_dpb = 184320;
		break;
	default:
		max_dpb = -1;
		break;
	}
	return max_dpb;
}


static inline bool is_high_profile(uint32_t profile_idc)
{
	return (profile_idc == 44) || (profile_idc == 83) || (profile_idc == 86) || (profile_idc == 100) || (profile_idc == 110) || (profile_idc == 118) || (profile_idc == 128) || (profile_idc == 122) || (profile_idc == 244);
}

static inline int scaling_list(dec_bits *st, int size, bool& use_default)
{
	int last_scale = 8;
	int next_scale = 8;
	int scale;
	for (int i = 0; i < size; ++i) {
		if (next_scale != 0) {
			int32_t delta_scale;
			READ_SE_RANGE(delta_scale, st, -128, 127);
			next_scale = (last_scale + delta_scale + 256) & 255;
			if ((i == 0) && (next_scale == 0)) {
				use_default = true;
			}
		}
		scale = (next_scale == 0) ? last_scale : next_scale;
		last_scale = scale;
	}
	return 0;
}

static inline int read_seq_high_extension(h264d_sps *sps, dec_bits *stream)
{
	uint32_t chroma_idc;
	uint32_t tmp;

	READ_UE_RANGE(chroma_idc, stream, 3);
	if (chroma_idc == 3) {
		get_onebit(stream);
	}
	READ_UE_RANGE(tmp, stream, 6);
	READ_UE_RANGE(tmp, stream, 6);
	get_onebit(stream);
	if (get_onebit(stream)) {
		int max = (chroma_idc != 3) ? 8 : 12;
		bool use_default;
		for (int i = 0; i < 6; ++i) {
			if (get_onebit(stream)) {
				if (scaling_list(stream, 16, use_default) < 0) {
					return -1;
				}
			}
		}
		for (int i = 0; i < max; ++i) {
			if (get_onebit(stream)) {
				if (scaling_list(stream, 64, use_default) < 0) {
					return -1;
				}
			}
		}
	}
	return 0;
}

static int read_seq_parameter_set(h264d_sps *sps, dec_bits *stream)
{
	uint32_t sps_profile_idc, sps_constraint_set_flag;
	uint32_t sps_level_idc, sps_id;
	uint32_t tmp;

	sps_profile_idc = get_bits(stream, 8);
	sps_constraint_set_flag = get_bits(stream, 8);
	sps_level_idc = get_bits(stream, 8);
	READ_UE_RANGE(sps_id, stream, 31);
	sps += sps_id;
	sps->profile_idc = sps_profile_idc;
	sps->constraint_set_flag = sps_constraint_set_flag;
	sps->level_idc = sps_level_idc;
	sps->is_high_profile = is_high_profile(sps->profile_idc);
	if (sps->is_high_profile) {
		if (read_seq_high_extension(sps, stream) < 0) {
			return -1;
		}
	}
	READ_UE_RANGE(tmp, stream, 27);
	sps->log2_max_frame_num = tmp + 4;
	READ_UE_RANGE(sps->poc_type, stream, 2);
	if (sps->poc_type == 0) {
		READ_UE_RANGE(tmp, stream, 27);
		sps->log2_max_poc_lsb = tmp + 4;
	} else if (sps->poc_type == 1) {
		sps->delta_pic_order_always_zero_flag = get_onebit(stream);
		sps->offset_for_non_ref_pic = se_golomb(stream);
		sps->offset_for_top_to_bottom_field = se_golomb(stream);
		READ_UE_RANGE(sps->num_ref_frames_in_pic_order_cnt_cycle, stream, 255);
		read_poc_type1_cycle(sps, stream, sps->num_ref_frames_in_pic_order_cnt_cycle);
	}
	READ_UE_RANGE(sps->num_ref_frames, stream, 16);
	sps->gaps_in_frame_num_value_allowed_flag = get_onebit(stream);
	sps->pic_width = (ue_golomb(stream) + 1) * 16;
	sps->pic_height = (ue_golomb(stream) + 1) * 16;
	sps->max_dpb_in_mbs = max_dpb_mbs(sps->profile_idc, sps->level_idc, sps->constraint_set_flag);
	if ((sps->frame_mbs_only_flag = get_onebit(stream)) == 0) {
		sps->mb_adaptive_frame_field_flag = get_onebit(stream);
	}
	sps->direct_8x8_inference_flag = get_onebit(stream);
	if ((sps->frame_cropping_flag = get_onebit(stream)) != 0) {
		for (int i = 0; i < 4; ++i) {
			sps->frame_crop[i] = ue_golomb(stream) * 2;
		}
	} else {
		memset(sps->frame_crop, 0, sizeof(sps->frame_crop));
	}
	if ((sps->vui_parameters_present_flag = get_onebit(stream)) != 0) {
		int err = vui_parameters(&sps->vui, stream);
		if (err < 0) {
			return err;
		}
	}
	return sps_id;
}

static int get_sei_message_size(dec_bits *stream);
static int skip_sei_message(dec_bits *stream);

static int skip_sei(dec_bits *stream)
{
	uint32_t next3bytes;
	do {
		skip_sei_message(stream);
		byte_align(stream);
		next3bytes = show_bits(stream, 24);
	} while ((1 < next3bytes) && (0x80 != (next3bytes >> 16)));
	return 0;
}

static void skip_sei_data(dec_bits *st, int byte_len)
{
	while (byte_len-- != 0) {
		skip_bits(st, 8);
	}
}

static int skip_sei_message(dec_bits *stream)
{
	int d;
	d = get_sei_message_size(stream);
	d = get_sei_message_size(stream);
	skip_sei_data(stream, d);
	return 0;
}

static int get_sei_message_size(dec_bits *stream)
{
	int c;
	int d = -255;
	do {
		c = get_bits(stream, 8);
		d += 255;
	} while (c == 0xff);
	return d + c;
}

static int more_rbsp_data(dec_bits *st);

static int read_pic_parameter_set(h264d_pps *pps, dec_bits *stream)
{
	uint8_t pps_id;
	int tmp;

	READ_UE_RANGE(pps_id, stream, 255);
	pps += pps_id;
	READ_UE_RANGE(pps->seq_parameter_set_id, stream, 31);
	pps->entropy_coding_mode_flag = get_onebit(stream);
	pps->pic_order_present_flag = get_onebit(stream);
	if (0 < (pps->num_slice_groups_minus1 = ue_golomb(stream))) {
		/* FMO not implemented */
		return -1;
	}
	READ_UE_RANGE(pps->num_ref_idx_l0_active_minus1, stream, 31);
	READ_UE_RANGE(pps->num_ref_idx_l1_active_minus1, stream, 31);
	pps->weighted_pred_flag = get_onebit(stream);
	pps->weighted_bipred_idc = get_bits(stream, 2);
	READ_SE_RANGE(tmp, stream, -26, 25);
	pps->pic_init_qp = tmp + 26;
	READ_SE_RANGE(tmp, stream, -26, 25);
	pps->pic_init_qs = tmp + 26;
	READ_SE_RANGE(pps->chroma_qp_index[0], stream, -12, 12);
	pps->chroma_qp_index[1] = pps->chroma_qp_index[0];
	pps->deblocking_filter_control_present_flag = get_onebit(stream);
	pps->constrained_intra_pred_flag = get_onebit(stream);
	pps->redundant_pic_cnt_present_flag = get_onebit(stream);
	if (more_rbsp_data(stream)) {
		pps->transform_8x8_mode_flag = get_onebit(stream);
		if ((pps->pic_scaling_list_present_flag = get_onebit(stream)) != 0) {
		}
		READ_SE_RANGE(pps->chroma_qp_index[1], stream, -12, 12);
	}
	return 0;
}

static inline void dpb_init(h264d_dpb_t *dpb, int maxsize);

int h264d_init(h264d_context *h2d, int dpb_max, int (*header_callback)(void *, void *), void *arg)
{
	if (!h2d) {
		return -1;
	}
	memset(h2d, 0, sizeof(*h2d));
	h2d->stream = &h2d->stream_i;
	h2d->slice_header = &h2d->slice_header_i;
	h2d->mb_current.bdirect = &h2d->mb_current.bdirect_i;
	h2d->mb_current.frame = &h2d->mb_current.frame_i;
	h2d->mb_current.cabac = &h2d->mb_current.cabac_i;
	h2d->mb_current.cabac_i.context = h2d->mb_current.cabac_context;
	h2d->header_callback = header_callback ? header_callback : header_dummyfunc;
	h2d->header_callback_arg = arg;
	h2d->mb_current.num_ref_idx_lx_active_minus1[0] = &h2d->slice_header->num_ref_idx_lx_active_minus1[0];
	h2d->mb_current.num_ref_idx_lx_active_minus1[1] = &h2d->slice_header->num_ref_idx_lx_active_minus1[1];
	dpb_init(&h2d->mb_current.frame->dpb, dpb_max);
	dec_bits_open(h2d->stream, m2d_load_bytes_skip03);
	return 0;
}

static dec_bits *h264d_stream_pos(h264d_context *h2d)
{
	return &h2d->stream_i;
}

static void set_mb_size(h264d_mb_current *mb, int width, int height);

int h264d_read_header(h264d_context *h2d, const byte_t *data, size_t len)
{
	dec_bits *st;
	int nal_type;
	int err;
	int sps_id;

	st = h2d->stream;
	dec_bits_open(st, m2d_load_bytes_skip03);
	err = dec_bits_set_data(st, data, len, 0);
	if (err < 0) {
		return err;
	}
	if (setjmp(st->jmp) != 0) {
		return 0;
	}
	do {
		err = m2d_find_mpeg_data(st);
		if (err < 0) {
			return err;
		}
		nal_type = get_bits(st, 8) & 31;
	} while (nal_type != SPS_NAL);
	sps_id = read_seq_parameter_set(h2d->sps_i, st);
	if (sps_id < 0) {
		return sps_id;
	}
	set_mb_size(&h2d->mb_current, h2d->sps_i[sps_id].pic_width, h2d->sps_i[sps_id].pic_height);
	return 0;
}

int h264d_get_info(h264d_context *h2d, m2d_info_t *info)
{
	int src_width;
	if (!h2d || !info) {
		return -1;
	}
	h264d_sps *sps = &h2d->sps_i[h2d->pps_i[h2d->slice_header->pic_parameter_set_id].seq_parameter_set_id];
	info->src_width = src_width = sps->pic_width;
	info->src_height = sps->pic_height;
	info->disp_width = sps->pic_width;
	info->disp_height = sps->pic_height;
	info->frame_num = sps->num_ref_frames + 1;
	for (int i = 0; i < 4; ++i) {
		info->crop[i] = sps->frame_crop[i];
	}
	info->additional_size = sizeof(prev_mb_t) * ((src_width >> 4) + 1)
		+ sizeof(uint32_t) * (src_width >> 2) * 2
		+ (sizeof(deblock_info_t) + (sizeof(h264d_col_mb_t) * 17)) * ((src_width * info->src_height) >> 8)
		+ sizeof(h264d_col_pic_t) * 17;
	return 0;
}

static int init_mb_buffer(h264d_mb_current *mb, uint8_t *buffer, int len)
{
	uint8_t *src = buffer;
	mb->mb_base = (prev_mb_t *)src;
	src += sizeof(*mb->mb_base) * (mb->max_x + 1);
	mb->top4x4pred_base = (int32_t *)src;
	src += sizeof(*mb->top4x4pred_base) * mb->max_x;
	mb->top4x4coef_base = (int32_t *)src;
	src += sizeof(*mb->top4x4coef_base) * mb->max_x;
	mb->deblock_base = (deblock_info_t *)src;
	int mb_num = mb->max_x * mb->max_y;
	src += sizeof(*mb->deblock_base) * mb_num;
	for (int i = 0; i < 16; ++i) {
		mb->frame->refs[1][i].col = (h264d_col_pic_t *)src;
		src += sizeof(h264d_col_pic_t) + sizeof(h264d_col_mb_t) * (mb_num - 1);
	}
	mb->frame->curr_col = (h264d_col_pic_t *)src;
	src += sizeof(h264d_col_pic_t) + sizeof(h264d_col_mb_t) * (mb_num - 1);
	return (uintptr_t)(buffer + len) < (uintptr_t)src ? -1 : 0;
}

static void set_mb_size(h264d_mb_current *mb, int width, int height)
{
	mb->max_x = width >> 4;
	mb->max_y = height >> 4;
}

/**Invoked just before each slice_data.
 */
static void set_mb_pos(h264d_mb_current *mb, int mbpos)
{
	int x, y;
	div_t d;

	d = div(mbpos, mb->max_x);
	mb->y = y = d.quot;
	mb->x = x = d.rem;
	mb->luma = mb->frame->curr_luma + y * mb->max_x * 16 * 16 + x * 16;
	mb->chroma = mb->frame->curr_chroma + y * mb->max_x * 16 * 8 + x * 16;
	mb->firstline = mb->max_x;
	mb->left4x4pred = 0x22222222;
	mb->prev_qp_delta = 0;
	memset(mb->top4x4pred_base, 0x22, mb->max_x * sizeof(*mb->top4x4pred_base));
	mb->top4x4pred = mb->top4x4pred_base + x;
	mb->top4x4coef = mb->top4x4coef_base + x;
	mb->deblock_curr = mb->deblock_base + mbpos;
	mb->left4x4pred = 0;
	*mb->top4x4pred = 0;
	mb->top4x4inter = mb->mb_base + 1 + x;
	mb->left4x4inter = mb->mb_base;
	mb->col_curr = mb->frame->curr_col->col_mb + y * mb->max_x + x;
	mb->cbf = 0;
}

static inline uint16_t cbf_top(uint32_t cbf)
{
	return ((cbf >> 16) & 0x700) | ((cbf >> 14) & 0xc0) | ((cbf >> 12) & 0x3c) | ((cbf >> 10) & 3);
}

static inline uint16_t cbf_left(uint32_t cbf)
{
	return ((cbf >> 16) & 0x600) | ((cbf >> 15) & 0x100) | ((cbf >> 14) & 0x80) | ((cbf >> 13) & 0x40) | ((cbf >> 12) & 0x38) | ((cbf >> 11) & 4) | ((cbf >> 6) & 2) | ((cbf >> 5) & 1);
}

static int increment_mb_pos(h264d_mb_current *mb)
{
	int mb_type;
	int x;

	mb_type = mb->type;
	mb->top4x4inter->type = mb_type;
	mb->left4x4inter->type = mb_type;
	mb->top4x4inter->cbp = mb->cbp;
	mb->top4x4inter->cbf = cbf_top(mb->cbf);
	mb->left4x4inter->cbp = mb->cbp;
	mb->left4x4inter->cbf = cbf_left(mb->cbf);
	mb->top4x4inter->chroma_pred_mode = mb->chroma_pred_mode;
	mb->left4x4inter->chroma_pred_mode = mb->chroma_pred_mode;
	mb->cbf = 0;
	x = mb->x + 1;
	mb->top4x4pred++;
	mb->top4x4coef++;
	mb->top4x4inter++;
	mb->col_curr++;
	mb->deblock_curr++;
	mb->luma += 16;
	mb->chroma += 16;
	if (mb->max_x <= x) {
		int stride;
		int y = mb->y + 1;
		stride = mb->max_x * 16;
		x = 0;
		mb->y = y;
		if (mb->max_y <= y) {
			return -1;
		}
		mb->luma += stride * 15;
		mb->chroma += stride * 7;
		mb->top4x4pred = mb->top4x4pred_base;
		mb->top4x4coef = mb->top4x4coef_base;
		mb->top4x4inter = mb->mb_base + 1;
	}
	mb->x = x;
	mb->deblock_curr->idc = 0;
	if (0 <= mb->firstline) {
		mb->firstline--;
	}
	return 0;
}

static inline void frames_init(h264d_mb_current *mb, int num_frame, const m2d_frame_t *frame)
{
	h264d_frame_info_t *frm = mb->frame;
	frm->num = num_frame;
	std::copy(frame, frame + num_frame, frm->frames);
	memset(frm->lru, 0, sizeof(frm->lru));
}

#define NUM_ARRAY(x) (sizeof(x) / sizeof(x[0]))

int h264d_set_frames(h264d_context *h2d, int num_frame, m2d_frame_t *frame, uint8_t *second_frame, int second_frame_size)
{
	h264d_mb_current *mb;

	if (!h2d || (num_frame < 3) || (NUM_ARRAY(mb->frame->frames) < (size_t)num_frame) || !frame || !second_frame) {
		return -1;
	}
	mb = &h2d->mb_current;
	frames_init(mb, num_frame, frame);
	h2d->slice_header->reorder[0].ref_frames = mb->frame->refs[0];
	h2d->slice_header->reorder[1].ref_frames = mb->frame->refs[1];
	return init_mb_buffer(mb, second_frame, second_frame_size);
}

static int h2d_dispatch_one_nal(h264d_context *h2d, int code_type);

int h264d_decode_picture(h264d_context *h2d)
{
	dec_bits *stream;
	int code_type;
	int err;

	if (!h2d) {
		return -1;
	}
	stream = h2d->stream;
	if (setjmp(stream->jmp) != 0) {
		return -2;
	}
	h2d->slice_header->first_mb_in_slice = UINT_MAX;
	err = 0;
	code_type = 0;
	do {
		if (0 <= (err = m2d_find_mpeg_data(stream))) {
			code_type = get_bits(stream, 8);
			err = h2d_dispatch_one_nal(h2d, code_type);
		} else {
			err = -2;
			break;
		}
		VC_CHECK;
	} while (err == 0 || (code_type == SPS_NAL && 0 < err));
#ifdef DUMP_COEF
	print_coefs();
#endif
	return err;
}

static inline void dpb_init(h264d_dpb_t *dpb, int maxsize)
{
	dpb->size = 0;
	dpb->max = maxsize;
	dpb->output = -1;
}

//#define DUMP_DPB
static void dump_dpb(h264d_dpb_t *dpb)
{
#if defined(DUMP_DPB) && !defined(NDEBUG) && !defined(__RENESAS_VERSION__)
	printf("DPB length: %d\n", dpb->size);
	for (int i = 0; i < dpb->size; ++i) {
		printf("\t[%d] :\tpoc: %d idx: %d\n", i, dpb->data[i].poc, dpb->data[i].frame_idx);
	}
#endif
}

static inline void dpb_insert_non_idr(h264d_dpb_t *dpb, int poc, int frame_idx)
{
	int size = dpb->size;
	h264d_dpb_elem_t *end = dpb->data + size;
	h264d_dpb_elem_t *d = end;

	if (0 < size) {
		do {
			--d;
		} while (d != dpb->data && !d->is_terminal && (poc < d->poc));
		if (size < dpb->max) {
			dpb->size = size + 1;
			dpb->output = -1;
			if (d->is_terminal || (d->poc < poc)) {
				++d;
			}
			memmove(d + 1, d, (end - d) * sizeof(*d));
		} else {
			dpb->output = dpb->data[0].frame_idx;
			if (dpb->data[0].is_terminal) {
				dpb->is_ready = 0;
			}
			memmove(dpb->data, dpb->data + 1, (d - dpb->data) * sizeof(*d));
		}
	} else {
		dpb->size = 1;
		dpb->output = -1;
	}
	d->poc = poc;
	d->frame_idx = frame_idx;
	d->is_idr = 0;
	d->is_terminal = 0;
}

static inline void dpb_insert_idr(h264d_dpb_t *dpb, int poc, int frame_idx)
{
	int size = dpb->size;
	if (size < dpb->max) {
		dpb->size = size + 1;
	} else {
		size--;
		dpb->output = dpb->data[0].frame_idx;
		if (dpb->data[0].is_terminal) {
			dpb->is_ready = 0;
		}
		memmove(dpb->data, dpb->data + 1, size * sizeof(dpb->data[0]));
	}
	h264d_dpb_elem_t *d = &dpb->data[size];
	d->poc = 0;
	d->frame_idx = frame_idx;
	d->is_idr = 1;
	d->is_terminal = 0;
	if (0 < size) {
		d[-1].is_terminal = 1;
		dpb->is_ready = 1;
	}
}

static int dpb_force_pop(h264d_dpb_t *dpb)
{
	int size = dpb->size;
	int pop_idx = dpb->output;
	if (0 <= pop_idx) {
		dpb->output = -1;
		return pop_idx;
	} else if (size == 0) {
		return -1;
	}
	size -= 1;
	dpb->size = size;
	dpb->output = -1;
	if (dpb->data[0].is_terminal) {
		dpb->is_ready = 0;
	}
	pop_idx = dpb->data[0].frame_idx;
	memmove(dpb->data, dpb->data + 1, size * sizeof(dpb->data[0]));
	return pop_idx;
}

static inline int dpb_exist(const h264d_dpb_t *dpb, int frame_idx) {
	const h264d_dpb_elem_t *d = dpb->data;
	const h264d_dpb_elem_t *end = d + dpb->size;
	while (d != end) {
		if (d->frame_idx == frame_idx) {
			return 1;
		}
		++d;
	}
	return 0;
}

static int dpb_force_peek(h264d_dpb_t *dpb)
{
	int size = dpb->size;
	int pop_idx = dpb->output;
	if (0 <= pop_idx) {
		return pop_idx;
	} else if (size == 0) {
		return -1;
	} else {
		return dpb->data[0].frame_idx;
	}
}

int h264d_peek_decoded_frame(h264d_context *h2d, m2d_frame_t *frame, int bypass_dpb)
{
	h264d_frame_info_t *frm;
	int frame_idx;

	if (!h2d || !frame) {
		return -1;
	}
	frm = h2d->mb_current.frame;
	if (!bypass_dpb) {
		if (frm->dpb.is_ready) {
			frame_idx = dpb_force_peek(&frm->dpb);
		} else {
			frame_idx = frm->dpb.output;
		}
	} else {
		frame_idx = dpb_force_peek(&frm->dpb);
	}
	if (frame_idx < 0) {
		return 0;
	}
	*frame = frm->frames[frame_idx];
	return 1;
}

int h264d_get_decoded_frame(h264d_context *h2d, m2d_frame_t *frame, int bypass_dpb)
{
	h264d_frame_info_t *frm;
	int frame_idx;

	if (!h2d || !frame) {
		return -1;
	}
	frm = h2d->mb_current.frame;
	if (!bypass_dpb) {
		if (frm->dpb.is_ready) {
			frame_idx = dpb_force_pop(&frm->dpb);
		} else {
			frame_idx = frm->dpb.output;
			frm->dpb.output = -1;
		}
	} else {
		frame_idx = dpb_force_pop(&frm->dpb);
	}
	dump_dpb(&frm->dpb);
	if (frame_idx < 0) {
		return 0;
	}
	*frame = frm->frames[frame_idx];
	return 1;
}

static int read_slice(h264d_context *h2d, dec_bits *st);

static int h2d_dispatch_one_nal(h264d_context *h2d, int code_type)
{
	int err;
	dec_bits *st = h2d->stream;

	switch(code_type & 31) {
	case SLICE_NONIDR_NAL:
	case SLICE_IDR_NAL:
		h2d->id = code_type;
		err = read_slice(h2d, st);
		break;
	case SEI_NAL:
		err = skip_sei(st);
		break;
	case SPS_NAL:
		err = read_seq_parameter_set(h2d->sps_i, st);
		if (0 <= err) {
			set_mb_size(&h2d->mb_current, h2d->sps_i[err].pic_width, h2d->sps_i[err].pic_height);
			h2d->header_callback(h2d->header_callback_arg, st->id);
		}
		break;
	case PPS_NAL:
		err = read_pic_parameter_set(h2d->pps_i, st);
		break;
	default:
		err = 0;
		break;
	}
	return err;
}

static int slice_header(h264d_context *h2d, dec_bits *st);
static int slice_data(h264d_context *h2d, dec_bits *st);

static int read_slice(h264d_context *h2d, dec_bits *st)
{
	int err;

	err = slice_header(h2d, st);
	if (err < 0) {
		return err;
	}
	return slice_data(h2d, st);
}

static int ref_pic_list_reordering(h264d_reorder_t *rdr, dec_bits *st, int num_ref_frames, int num_frames, int max_num_frames);
static int dec_ref_pic_marking(int nal_id, h264d_marking_t *mrk, dec_bits *st);

static int slice_type_adjust(int slice_type)
{
	return (SI_SLICE < slice_type) ? slice_type - SI_SLICE - 1 : slice_type;
}

static inline void find_empty_frame(h264d_mb_current *mb)
{
	h264d_frame_info_t *frm = mb->frame;
	h264d_ref_frame_t *refs0 = frm->refs[0];
	h264d_ref_frame_t *refs1 = frm->refs[1];
	int8_t *lru = frm->lru;
	int max_idx = 0;
	int max_val = -1;
	int frm_num = frm->num;
	h264d_dpb_t *dpb;

	dpb = &frm->dpb;
	for (int i = 0; i < frm_num; ++i) {
		if (dpb_exist(dpb, i)) {
			lru[i] = 0;
		} else {
			lru[i] += 1;
		}
	}
	for (int i = 0; i < 16; ++i) {
		if (refs0[i].in_use) {
			lru[refs0[i].frame_idx] = 0;
		}
		if (refs1[i].in_use) {
			lru[refs1[i].frame_idx] = 0;
		}
	}
	for (int i = 0; i < frm_num; ++i) {
		int val = lru[i];
		if (max_val < val) {
			max_val = val;
			max_idx = i;
		}
	}
	lru[max_idx] = 0;
	frm->index = max_idx;
	frm->curr_luma = frm->frames[max_idx].luma;
	frm->curr_chroma = frm->frames[max_idx].chroma;
}

static void qp_matrix(int16_t *matrix, int scale, int shift)
{
	static const int8_t normAdjust[6][3] = {
		{10, 16, 13},
		{11, 18, 14},
		{13, 20, 16},
		{14, 23, 18},
		{16, 25, 20},
		{18, 29, 23}
	};
	int v0 = normAdjust[scale][0] << shift;
	int v1 = normAdjust[scale][1] << shift;
	int v2 = normAdjust[scale][2] << shift;
	/* after inverse-zig-zag scan, normAdjust shall be:
	   0, 2, 0, 2,
	   2, 1, 2, 1,
	   0, 2, 0, 2,
	   2, 1, 2, 1
	 */
	matrix += 16; /* write backward for SuperH. */
	int i = 2;
	do {
		*--matrix = v1;
		*--matrix = v2;
		*--matrix = v1;
		*--matrix = v2;
		*--matrix = v2;
		*--matrix = v0;
		*--matrix = v2;
		*--matrix = v0;
	} while (--i);
}

static void qp_matrix8x8(int16_t *matrix, int scale, int shift)
{
	static const int8_t normAdjust[6][6] = {
		{20, 18, 32, 19, 25, 24},
		{22, 19, 35, 21, 28, 26},
		{26, 23, 42, 24, 33, 31},
		{28, 25, 45, 26, 35, 33},
		{32, 28, 51, 30, 40, 38},
		{36, 32, 58, 34, 46, 43},
	};
	const int8_t *adj = normAdjust[scale];
	int v0, v1, v2, v3, v4, v5;
	v0 = *adj++;
	v1 = *adj++;
	v2 = *adj++;
	v3 = *adj++;
	v4 = *adj++;
	v5 = *adj;
	if (shift) {
		if (0 < shift) {
			v0 <<= shift;
			v1 <<= shift;
			v2 <<= shift;
			v3 <<= shift;
			v4 <<= shift;
			v5 <<= shift;
		} else {
			shift = -shift;
			v0 >>= shift;
			v1 >>= shift;
			v2 >>= shift;
			v3 >>= shift;
			v4 >>= shift;
			v5 >>= shift;
		}
	}
	/* after inverse-zigzag-scan, order of normAdjust v shall be:
	   0, 3, 4, 3, 0, 3, 4, 3,
	   3, 1, 5, 1, 3, 1, 5, 1,
	   4, 5, 2, 5, 4, 5, 2, 5,
	   3, 1, 5, 1, 3, 1, 5, 1,
	   0, 3, 4, 3, 0, 3, 4, 3,
	   3, 1, 5, 1, 3, 1, 5, 1,
	   4, 5, 2, 5, 4, 5, 2, 5,
	   3, 1, 5, 1, 3, 1, 5, 1,
	 */
	matrix += 64; /* write backward for SuperH architecture. */
	int j;
#define QMAT8x8LINE(mt, x0, x1, x2) j = 2; do { *--mt = x0; *--mt = x1; *--mt = x0; *--mt = x2; } while (--j)
	QMAT8x8LINE(matrix, v1, v5, v3); /* [63]..[48] */
	QMAT8x8LINE(matrix, v5, v2, v4);
	QMAT8x8LINE(matrix, v1, v5, v3); /* [47]..[32] */
	QMAT8x8LINE(matrix, v3, v4, v0);
	QMAT8x8LINE(matrix, v1, v5, v3); /* [31]..[16] */
	QMAT8x8LINE(matrix, v5, v2, v4);
	QMAT8x8LINE(matrix, v1, v5, v3); /* [15]..[0] */
	QMAT8x8LINE(matrix, v3, v4, v0);
}

static int qpc_adjust(int qpy, int qpc_diff)
{
	static const int8_t adjust_lut[22] = {
		29, 30, 31, 32, 32, 33, 34, 34,
		35, 35, 36, 36, 37, 37, 37, 38,
		38, 38, 39, 39, 39, 39
	};
	int qpc = qpy + qpc_diff;
	if (0 < qpc) {
		if (30 <= qpc) {
			if (51 < qpc) {
				qpc = 51;
			}
			qpc = adjust_lut[qpc - 30];
		}
	} else {
		qpc = 0;
	}
	return qpc;
}

static void set_qpc(h264d_mb_current *mb, int qpy, int idx, int qpc_dif)
{
	int div, mod;
	int qpc;
	mb->qp_chroma[idx] = qpc = qpc_adjust(qpy, qpc_dif);
	if (qpc != qpy) {
		div = (unsigned)qpc / 6;
		mod = qpc - div * 6;
		mb->qmatc_p[idx] = mb->qmatc[idx];
		qp_matrix(mb->qmatc[idx], mod, div);
	} else {
		mb->qmatc_p[idx] = mb->qmaty;
	}
}

static void set_qp(h264d_mb_current *mb, int qpy)
{
	int div;
	int mod;
	int qpc_dif0, qpc_dif1;

	if (qpy < 0) {
		qpy += 52;
	} else if (52 <= qpy) {
		qpy -= 52;
	}
	mb->qp = qpy;
	div = (unsigned)qpy / 6;
	mod = qpy - div * 6;
	qp_matrix(mb->qmaty, mod, div);
	if (mb->pps->transform_8x8_mode_flag) {
		qp_matrix8x8(mb->qmaty8x8, mod, div - 2);
	}
	qpc_dif0 = mb->pps->chroma_qp_index[0];
	set_qpc(mb, qpy, 0, qpc_dif0);
	qpc_dif1 = mb->pps->chroma_qp_index[1];
	if (qpc_dif0 == qpc_dif1) {
		mb->qp_chroma[1] = mb->qp_chroma[0];
		mb->qmatc_p[1] = mb->qmatc_p[0];
	} else {
		set_qpc(mb, qpy, 1, qpc_dif1);
	}
}

static inline void calc_poc0(h264d_slice_header *hdr, int log2_max_lsb, int lsb)
{
	int prev_lsb, prev_msb;
	int msb;
	int max_lsb_2;
	if (hdr->first_mb_in_slice != 0) {
		return;
	}
	if (hdr->marking.idr || hdr->marking.mmco5) {
		prev_msb = 0;
		if (hdr->marking.mmco5 && hdr->field_pic_flag && hdr->bottom_field_flag) {
			prev_lsb = hdr->poc0.lsb;
		} else {
			prev_lsb = 0;
		}
	} else  {
		prev_lsb = hdr->poc0.lsb;
		prev_msb = hdr->poc0.msb;
	}
	hdr->poc0.lsb = lsb;
	max_lsb_2 = (1 << log2_max_lsb) >> 1;
	if ((lsb < prev_lsb) && (max_lsb_2 <= (prev_lsb - lsb))) {
		msb = prev_msb + max_lsb_2 * 2;
	} else if ((prev_lsb < lsb) && (max_lsb_2 < (lsb - prev_lsb))) {
		msb = prev_msb - max_lsb_2 * 2;
	} else {
		msb = prev_msb;
	}
	hdr->poc0.msb = msb;
	hdr->poc = msb + lsb;
	hdr->poc_bottom = hdr->poc + hdr->poc0.delta_pic_order_cnt_bottom;
}

static inline void calc_poc1(h264d_slice_header *hdr, const h264d_sps *sps, int nal_id)
{
	unsigned frame_num;
	int poc;

	if (hdr->first_mb_in_slice != 0) {
		return;
	}
	frame_num = hdr->frame_num;
	if (!hdr->marking.idr && !hdr->marking.mmco5) {
		if (frame_num < hdr->prev_frame_num) {
			hdr->poc1.num_offset += 1 << sps->log2_max_frame_num;
		}
	} else {
		hdr->poc1.num_offset = 0;
	}
	if (sps->num_ref_frames_in_pic_order_cnt_cycle) {
		frame_num += hdr->poc1.num_offset;
		if (frame_num != 0) {
			int cycle_cnt = 0;
			int cycle_sum = sps->offset_for_ref_frame[sps->num_ref_frames_in_pic_order_cnt_cycle - 1];
			frame_num--;
			if (frame_num != 0 && !(nal_id & 0x60)) {
				frame_num--;
			}
			while (cycle_sum <= (int)frame_num) {
				frame_num -= cycle_sum;
				cycle_cnt++;
			}
			poc = cycle_cnt * cycle_sum + sps->offset_for_ref_frame[frame_num & 255];
		} else {
			poc = sps->offset_for_ref_frame[0];
		}
		if ((nal_id & 0x60) == 0) {
			poc += sps->offset_for_non_ref_pic;
		}
	} else {
		poc = 0;
	}
	hdr->poc = poc = poc + hdr->poc1.delta_pic_order_cnt[0];
	hdr->poc_bottom = poc + sps->offset_for_top_to_bottom_field + hdr->poc1.delta_pic_order_cnt[1];
}

static inline void calc_poc2(h264d_slice_header *hdr, const h264d_sps *sps, int nal_id)
{
	int poc;

	if (hdr->first_mb_in_slice != 0) {
		return;
	}
	uint32_t frame_num = hdr->frame_num;
	if (hdr->marking.idr || hdr->marking.mmco5) {
		hdr->poc2_prev_frameoffset = 0;
	} else {
		if (frame_num < hdr->prev_frame_num) {
			hdr->poc2_prev_frameoffset += (1 << sps->log2_max_frame_num);
		}
	}
	poc = (frame_num + hdr->poc2_prev_frameoffset) * 2 - ((nal_id & 0x60) == 0);
	hdr->poc = poc;
	hdr->poc_bottom = poc;
}

static inline void ref_pic_init_p(h264d_slice_header *hdr, int max_frame_num, int num_ref_frames);
static inline void ref_pic_init_b(h264d_slice_header *hdr, int num_ref_frames);
static inline void set_dpb_max(h264d_dpb_t *dpb, const h264d_sps *sps)
{
	if (dpb->max < 0) {
		/* FIXME: dpb shall exists for each sps, so this method would be unnecessary. */
		int dpb_num = sps->max_dpb_in_mbs / ((uint32_t)(sps->pic_width * sps->pic_height) >> 8);
		dpb->max = 16 < dpb_num ? 16 : dpb_num;
	}
}

static inline int find_col_idx(const h264d_ref_frame_t *ref0, int len, int col_frameidx)
{
	int i;
	if (col_frameidx < 0) {
		return -1;
	}
	for (i = 0; i < len; ++i) {
		if (ref0[i].frame_idx == col_frameidx) {
			break;
		}
	}
	return (i < len) ? i : -1;
}

static inline int CLIP_P(int lower, int upper, int val)
{
	return (val < lower) ? lower : ((upper < val) ? upper : val);
}

static inline int dist_scale_factor(int poc0, int poc1, int curr_poc)
{
	if (poc1 == poc0) {
		return 256;
	} else {
		int td = CLIP_P(-128, 127, poc1 - poc0);
		int tb = CLIP_P(-128, 127, curr_poc - poc0);
		int tx = (16384 + ABS(td / 2)) / td;
		return (tb * tx + 32) >> 6;
	}
}

static void create_map_col_to_list0(int8_t *map_col_to_list0, int16_t *scale, const h264d_ref_frame_t *ref0, const h264d_ref_frame_t *ref1, int curr_poc, int len)
{
	int poc1 = ref1->poc;
	const int8_t *map = ref1[0].col->map_col_frameidx;
	for (int i = 0; i < len; ++i) {
		map_col_to_list0[i] = find_col_idx(ref0, len, map[i]);
		scale[i] = CLIP_P(-1024, 1023, dist_scale_factor(ref0[i].poc, poc1, curr_poc));
	}
}

static void pred_direct4x4_temporal(h264d_mb_current *mb, int blk_idx, prev8x8_t *curr_blk, int avail, prev8x8_t *ref_blk, int type0_cnt);
static void pred_direct8x8_temporal(h264d_mb_current *mb, int blk_idx, prev8x8_t *curr_blk, int avail, prev8x8_t *ref_blk, int type0_cnt);
template <int DIRECT8x8INFERENCE>
static void b_skip_mb_temporal(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_set_t *mv);
template <int BLOCK>
static void pred_direct8x8_spatial(h264d_mb_current *mb, int blk_idx, prev8x8_t *curr_blk, int avail, prev8x8_t *ref_blk, int type0_cnt);
static void b_skip_mb_spatial(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_set_t *mv);
template <int DIRECT8x8INFERENCE>
static void store_info_inter(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4, int mb_type);
static void direct_mv_pred_nocol(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);
static void pred_direct16x16_col_ref1_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);
static void pred_direct16x16_col_ref2_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);
static void pred_direct16x16_col_ref3_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);
static void pred_direct16x16_col_ref1_4x4(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);
static void pred_direct16x16_col_ref2_4x4(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);
static void pred_direct16x16_col_ref3_4x4(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv);

static void (* const pred_direct16x16_col8x8[2][4])(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv) = {
	{
		direct_mv_pred_nocol,
		pred_direct16x16_col_ref1_4x4,
		pred_direct16x16_col_ref2_4x4,
		pred_direct16x16_col_ref3_4x4
	},
	{
		direct_mv_pred_nocol,
		pred_direct16x16_col_ref1_8x8,
		pred_direct16x16_col_ref2_8x8,
		pred_direct16x16_col_ref3_8x8
	}
};

static bool not_need_transform_size_8x8_in_sub8x8(const int8_t sub_mb_type[]) { // 8x8b
#define IS8x8(sub) ((unsigned)((sub) - 1) < 3)
	return IS8x8(sub_mb_type[0]) && IS8x8(sub_mb_type[1]) && IS8x8(sub_mb_type[2]) && IS8x8(sub_mb_type[3]);
}

static bool need_transform_size_8x8(const int8_t sub_mb_type[]) {
	return true;
}

const h264d_bdirect_functions_t bdirect_functions[2][2][2] = {
	{
		{
			{
				pred_direct4x4_temporal,
				b_skip_mb_temporal<0>,
				0,
				store_info_inter<0>,
				not_need_transform_size_8x8_in_sub8x8
			},
			{
				pred_direct8x8_spatial<4>,
				b_skip_mb_spatial,
				&pred_direct16x16_col8x8[0][0],
				store_info_inter<0>,
				not_need_transform_size_8x8_in_sub8x8
			}
		},
		{
			{
				pred_direct4x4_temporal,
				b_skip_mb_temporal<0>,
				0,
				store_info_inter<0>,
				not_need_transform_size_8x8_in_sub8x8
			},
			{
				pred_direct8x8_spatial<4>,
				b_skip_mb_spatial,
				&pred_direct16x16_col8x8[0][0],
				store_info_inter<0>,
				not_need_transform_size_8x8_in_sub8x8
			}
		}
	},
	{
		{
			{
				pred_direct8x8_temporal,
				b_skip_mb_temporal<1>,
				0,
				store_info_inter<1>,
				not_need_transform_size_8x8_in_sub8x8
			},
			{
				pred_direct8x8_spatial<8>,
				b_skip_mb_spatial,
				pred_direct16x16_col8x8[1],
				store_info_inter<1>,
				not_need_transform_size_8x8_in_sub8x8
			}
		},
		{
			{
				pred_direct8x8_temporal,
				b_skip_mb_temporal<1>,
				0,
				store_info_inter<1>,
				need_transform_size_8x8
			},
			{
				pred_direct8x8_spatial<8>,
				b_skip_mb_spatial,
				pred_direct16x16_col8x8[1],
				store_info_inter<1>,
				need_transform_size_8x8
			}
		}
	}
};

static void set_mb_decode(h264d_mb_current *mb, const h264d_pps *pps);
static int pred_weight_table(h264d_weighted_table_pair_t *weight_offset, dec_bits *st, int active_num, const int8_t shift[]);
static void inter_pred_basic(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety);
static void inter_pred_weighted1(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety);
static void inter_pred_weighted2(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety);

static int set_weighted_info(h264d_mb_current *mb, dec_bits *st, h264d_slice_header *hdr, int slice_type, int pred_type) {
	mb->header = hdr;
	if (pred_type == 1) {
		READ_UE_RANGE(hdr->pred_weighted_info.type1.shift[0], st, 7);
		READ_UE_RANGE(hdr->pred_weighted_info.type1.shift[1], st, 7);
		pred_weight_table(&hdr->pred_weighted_info.type1.weight[0][0], st, hdr->num_ref_idx_lx_active_minus1[0], hdr->pred_weighted_info.type1.shift);
		if (slice_type == B_SLICE) {
			pred_weight_table(&hdr->pred_weighted_info.type1.weight[0][1], st, hdr->num_ref_idx_lx_active_minus1[1], hdr->pred_weighted_info.type1.shift);
		}
		mb->inter_pred = inter_pred_weighted1;
	} else {
		h264d_weighted_cache_t* t2 = &hdr->pred_weighted_info.type2;
		memset(t2->idx, -1, sizeof(t2->idx));
		mb->inter_pred = inter_pred_weighted2;
	}
	return 0;
}

static void build_4x4offset_table(int dst[], int stride) {
	int offset = 0;
	for (int i = 0; i < 4; ++i) {
		dst[0] = offset;
		dst[1] = offset + 4;
		dst[2] = offset + stride * 4;
		dst[3] = offset + (stride + 1) * 4;
		dst += 4;
		offset += (i & 1) ? (stride - 1) * 8 : 8;
	}
}

static int slice_header(h264d_context *h2d, dec_bits *st)
{
	h264d_slice_header *hdr = h2d->slice_header;
	h264d_sps *sps;
	h264d_pps *pps;
	h264d_mb_current *mb;
	uint32_t prev_first_mb = hdr->first_mb_in_slice;
	int slice_type;

	mb = &h2d->mb_current;
	if ((hdr->first_mb_in_slice = ue_golomb(st)) <= prev_first_mb) {
		if (prev_first_mb != UINT_MAX) {
			return -2;
		}
		find_empty_frame(mb);
		memset(mb->deblock_base, 0, sizeof(*mb->deblock_base) * mb->max_x * mb->max_y);
	}
	READ_UE_RANGE(slice_type, st, 9);
	hdr->slice_type = slice_type_adjust(slice_type);
	if (3U <= (unsigned)hdr->slice_type) {
		return -1;
	}
	READ_UE_RANGE(hdr->pic_parameter_set_id, st, 255);
	pps = &h2d->pps_i[hdr->pic_parameter_set_id];
	sps = &h2d->sps_i[pps->seq_parameter_set_id];
	mb->pps = pps;
	mb->is_constrained_intra = pps->constrained_intra_pred_flag;
	set_mb_decode(mb, pps);

	if (hdr->first_mb_in_slice <= prev_first_mb) {
		m2d_frame_t *frm = &mb->frame->frames[mb->frame->index];
		frm->width = sps->pic_width;
		frm->height = sps->pic_height;
		memcpy(frm->crop, sps->frame_crop, sizeof(sps->frame_crop));
	}
	hdr->frame_num = get_bits(st, sps->log2_max_frame_num);
	if (!sps->frame_mbs_only_flag) {
		if ((hdr->field_pic_flag = get_onebit(st)) != 0) {
			hdr->bottom_field_flag = get_onebit(st);
		}
	} else {
		hdr->field_pic_flag = 0;
	}
	if ((h2d->id & 31) == 5) {
		hdr->marking.idr = 1;
		READ_UE_RANGE(hdr->idr_pic_id, st, 65535);
	} else {
		hdr->marking.idr = 0;
	}
	mb->is_field = hdr->field_pic_flag;
	set_mb_size(mb, sps->pic_width, sps->pic_height);
	build_4x4offset_table(mb->offset4x4, sps->pic_width);
	set_dpb_max(&mb->frame->dpb, sps);
	set_mb_pos(mb, hdr->first_mb_in_slice);
	if (sps->poc_type == 0) {
		uint32_t lsb = get_bits(st, sps->log2_max_poc_lsb);
		if (!hdr->field_pic_flag && pps->pic_order_present_flag) {
			hdr->poc0.delta_pic_order_cnt_bottom = se_golomb(st);
		} else {
			hdr->poc0.delta_pic_order_cnt_bottom = 0;
		}
		calc_poc0(hdr, sps->log2_max_poc_lsb, lsb);
	} else if (sps->poc_type == 1) {
		if (!sps->delta_pic_order_always_zero_flag) {
			hdr->poc1.delta_pic_order_cnt[0] = se_golomb(st);
			if (!hdr->field_pic_flag && pps->pic_order_present_flag) {
				hdr->poc1.delta_pic_order_cnt[1] = se_golomb(st);
			}
		} else {
			hdr->poc1.delta_pic_order_cnt[0] = 0;
			hdr->poc1.delta_pic_order_cnt[1] = 0;
		}
		calc_poc1(hdr, sps, h2d->id);
	} else {
		calc_poc2(hdr, sps, h2d->id);
	}
	mb->frame->frames[mb->frame->index].cnt = hdr->poc;
	if (pps->redundant_pic_cnt_present_flag) {
		hdr->redundant_pic_cnt = ue_golomb(st);
	}
	int max_frame_num = 1 << sps->log2_max_frame_num;
	switch (hdr->slice_type)
	{
	case B_SLICE:
		hdr->direct_spatial_mv_pred_flag = get_onebit(st);
		/* FALLTHROUGH */
	case P_SLICE:
		if ((hdr->num_ref_idx_active_override_flag = get_onebit(st)) != 0) {
			READ_UE_RANGE(hdr->num_ref_idx_lx_active_minus1[0], st, 31);
			if (hdr->slice_type == B_SLICE) {
				READ_UE_RANGE(hdr->num_ref_idx_lx_active_minus1[1], st, 31);
			}
		} else {
			hdr->num_ref_idx_lx_active_minus1[0] = pps->num_ref_idx_l0_active_minus1;
			hdr->num_ref_idx_lx_active_minus1[1] = pps->num_ref_idx_l1_active_minus1;
		}
		if (hdr->slice_type == P_SLICE) {
			ref_pic_init_p(hdr, max_frame_num, sps->num_ref_frames);
		} else {
			ref_pic_init_b(hdr, sps->num_ref_frames);
		}
		if (ref_pic_list_reordering(&hdr->reorder[0], st, sps->num_ref_frames, hdr->frame_num, max_frame_num)) {
			return -1;
		}
		mb->inter_pred = inter_pred_basic;
		if (hdr->slice_type == B_SLICE) {
			if (ref_pic_list_reordering(&hdr->reorder[1], st, sps->num_ref_frames, hdr->frame_num, max_frame_num)) {
				return -1;
			}
			mb->sub_mb_ref_map = sub_mb_ref_map_b;
			if (hdr->direct_spatial_mv_pred_flag == 0) {
				create_map_col_to_list0(mb->bdirect->map_col_to_list0, mb->bdirect->scale, mb->frame->refs[0], mb->frame->refs[1], hdr->poc, sps->num_ref_frames);
			}
			if (pps->weighted_bipred_idc != 0) {
				if (set_weighted_info(mb, st, hdr, B_SLICE, pps->weighted_bipred_idc) < 0) {
					return -1;
				}
			}
		} else {
			mb->sub_mb_ref_map = sub_mb_ref_map_p;
			if (pps->weighted_pred_flag && (set_weighted_info(mb, st, hdr, P_SLICE, 1) < 0)) {
				return -1;
			}
		}
		mb->bdirect->func = &bdirect_functions[sps->direct_8x8_inference_flag][pps->transform_8x8_mode_flag][hdr->direct_spatial_mv_pred_flag];
	}
	if (h2d->id & 0x60) {
		if (dec_ref_pic_marking(h2d->id & 31, &hdr->marking, st) < 0) {
			return -1;
		}
	} else {
		hdr->marking.mmco5 = 0;
	}
	if (pps->entropy_coding_mode_flag) {
		if ((hdr->slice_type != I_SLICE)
		    && (hdr->slice_type != SI_SLICE)) {
			READ_UE_RANGE(hdr->cabac_init_idc, st, 2);
		}
	}
	hdr->qp_delta = se_golomb(st);
	set_qp(&h2d->mb_current, pps->pic_init_qp + hdr->qp_delta);
	if ((hdr->slice_type == SP_SLICE)
	    || (hdr->slice_type == SI_SLICE)) {
		if (hdr->slice_type == SP_SLICE) {
			hdr->sp_for_switch_flag = get_onebit(st);
		}
		hdr->qs_delta = se_golomb(st);
	}
	deblock_info_t *firstmb = h2d->mb_current.deblock_base + hdr->first_mb_in_slice;
	if (pps->deblocking_filter_control_present_flag) {
		READ_UE_RANGE(hdr->disable_deblocking_filter_idc, st, 2);
		if (hdr->disable_deblocking_filter_idc != 1) {
			READ_SE_RANGE(hdr->slice_alpha_c0_offset_div2, st, -6, 6);
			READ_SE_RANGE(hdr->slice_beta_offset_div2, st, -6, 6);
			ENC_SLICEHDR(firstmb->slicehdr, hdr->slice_alpha_c0_offset_div2, hdr->slice_beta_offset_div2);
		} else {
			ENC_SLICEHDR(firstmb->slicehdr, 0, 0);
		}
	} else {
		hdr->disable_deblocking_filter_idc = 0;
		ENC_SLICEHDR(firstmb->slicehdr, 0, 0);
	}
	firstmb->idc = hdr->disable_deblocking_filter_idc + 1;
	return 0;
}

static inline int calc_short_term(int idc, int num, int frame_num, int max_frame_num)
{
	int no_wrap;
	assert(idc == 0 || idc == 1);
	if (idc == 0) {
		no_wrap = frame_num - num - 1;
		while (no_wrap < 0) {
			no_wrap += max_frame_num;
		}
	} else {
		no_wrap = frame_num + num + 1;
		while (max_frame_num <= no_wrap) {
			no_wrap -= max_frame_num;
		}
	}
	return no_wrap;
}

//#define DUMP_REF_LIST
static void dump_ref_list(h264d_ref_frame_t *refs, int num_ref_frames)
{
#if defined(DUMP_REF_LIST) && !defined(NDEBUG) && !defined(__RENESAS_VERSION__)
	printf("refs(%d) use\tnum\tpoc\n", num_ref_frames);
	for (int i = 0; i < 16; ++i) {
		if (!refs[i].in_use) {
			break;
		}
		printf("\t%d,\t%d,\t%d\n", refs[i].in_use, refs[i].num, refs[i].poc);
	}
#endif
}

struct is_target_ref {
	int num_, mode_;
	is_target_ref(int num, int mode) : num_(num), mode_(mode) {}
	bool operator()(const h264d_ref_frame_t& ref) {
		return (ref.num == num_) && (ref.in_use == mode_);
	}
};

static int ref_pic_list_reordering(h264d_reorder_t *rdr, dec_bits *st, int num_ref_frames, int frame_num, int max_frame_num)
{
#define REF_MAX 16
	assert((unsigned)num_ref_frames <= REF_MAX);
	if ((rdr->ref_pic_list_reordering_flag = get_onebit(st)) != 0) {
		h264d_ref_frame_t *refs = rdr->ref_frames;
		h264d_ref_frame_t *refs_end = rdr->ref_frames + REF_MAX;
		int refIdxLx = -1;
		while (++refIdxLx < REF_MAX) {
			int idc;
			uint32_t num;
			int mode;

			READ_UE_RANGE(idc, st, 3);
			if (3 <= idc) {
				if (3 < idc) {
					return -1;
				}
				break;
			}
			num = ue_golomb(st);
			if (idc < 2) {
				num = calc_short_term(idc, num, frame_num, max_frame_num);
				frame_num = num;
				mode = SHORT_TERM;
			} else {
				mode = LONG_TERM;
			}
			if (refs[refIdxLx].num == num && refs[refIdxLx].in_use == mode) {
				std::remove_if(&refs[refIdxLx + 1], refs_end, is_target_ref(num, mode));
			} else {
				const h264d_ref_frame_t *target = std::find_if(refs, refs_end, is_target_ref(num, mode));
				if (target != refs_end) {
					h264d_ref_frame_t tmp_ref = *target;
					std::remove_if(&refs[refIdxLx + 1], refs_end, is_target_ref(num, mode));
					memmove(&refs[refIdxLx + 1], &refs[refIdxLx], (refs_end - &refs[refIdxLx + 1]) * sizeof(refs[0]));
					refs[refIdxLx] = tmp_ref;
				}
			}
		}
	}
	dump_ref_list(rdr->ref_frames, num_ref_frames);
	return 0;
}

static int pred_weight_table(h264d_weighted_table_pair_t weight_offset[], dec_bits *st, int active_num, const int8_t shift[])
{
	int default_weight_luma = 1 << shift[0];
	int default_weight_chroma = 1 << shift[1];
	int i = active_num + 1;
	do {
		if (get_onebit(st)) {
			READ_SE_RANGE(weight_offset->e[0].weight, st, -128, 127);
			READ_SE_RANGE(weight_offset->e[0].offset, st, -128, 127);
		} else {
			weight_offset->e[0].weight = default_weight_luma;
			weight_offset->e[0].offset = 0;
		}
		if (get_onebit(st)) {
			for (int j = 1; j < 3; ++j) {
				READ_SE_RANGE(weight_offset->e[j].weight, st, -128, 127);
				READ_SE_RANGE(weight_offset->e[j].offset, st, -128, 127);
			}
		} else {
			for (int j = 1; j < 3; ++j) {
				weight_offset->e[j].weight = default_weight_chroma;
				weight_offset->e[j].offset = 0;
			}
		}
		weight_offset += 2;
	} while (--i);
	return 0;
}

static int dec_ref_pic_marking(int nal_unit_type, h264d_marking_t *mrk, dec_bits *st)
{
	uint32_t tmp = get_onebit(st);
	int op5_detect = 0;

	if (nal_unit_type == 5) {
		mrk->no_output_of_prior_pic_flag = tmp;
		mrk->long_term_reference_flag = get_onebit(st);
	} else {
		mrk->no_output_of_prior_pic_flag = 0;
		mrk->adaptive_ref_pic_marking_mode_flag = tmp;
		if (tmp) {
			h264d_mmco *mmco = mrk->mmco;
			int i = 16;
			do {
				READ_UE_RANGE(mmco->op, st, 6);
				if (mmco->op == 0) {
					break;
				} else if (mmco->op == 5) {
					op5_detect = 1;
				} else {
					tmp = ue_golomb(st);
					switch (mmco->op) {
					case 3:
						mmco->arg2 = ue_golomb(st);
						/* FALLTHROUGH */
					case 1:
					case 2:
					case 4:
					case 6:
						mmco->arg1 = tmp;
						break;
					}
				}
				mmco++;
			} while (--i);
		}
	}
	mrk->mmco5 = op5_detect;
	return 0;
}

static inline int get_nC(int na, int nb)
{
	int nc;
	if (0 <= na) {
		if (0 <= nb) {
			nc = (na + nb + 1) >> 1;
		} else {
			nc = na;
		}
	} else if (0 <= nb) {
		nc = nb;
	} else {
		nc = 0;
	}
	return nc;
}

static inline void read_trailing_ones(dec_bits *st, int *level, int trailing_ones)
{
	if (trailing_ones) {
		uint32_t ones = get_bits(st, trailing_ones) * 2;
		level += trailing_ones;
		do {
			*--level = 1 - (ones & 2);
			ones >>= 1;
		} while (--trailing_ones);
	}
}

static inline int level_prefix(dec_bits *st)
{
	const vlc_t *d = &level_prefix_bit8[show_bits(st, 8)];
	int val = d->pattern;
	int len = d->length;
	while (len < 0) {
		skip_bits(st, 8);
		d = &level_prefix_bit8[show_bits(st, 8)];
		val += d->pattern;
		len = d->length;
	}
	skip_bits(st, len);
	return val;
}

static int8_t total_zeros16(dec_bits *st, int total_coeff)
{
	const vlc_t *d;
	int zeros = 0;
	switch (total_coeff) {
	case 0:
		/* FALLTHROUGH */
	case 1:
		zeros = m2d_dec_vld_unary(st, total_zeros1_bit6, 6);
		break;
	case 2:
		d = &total_zeros2_bit6[show_bits(st, 6)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 3:
		d = &total_zeros3_bit6[show_bits(st, 6)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 4:
		d = &total_zeros4_bit5[show_bits(st, 5)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 5:
		d = &total_zeros5_bit5[show_bits(st, 5)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 6:
		d = &total_zeros6_bit6[show_bits(st, 6)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 7:
		d = &total_zeros7_bit6[show_bits(st, 6)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 8:
		d = &total_zeros8_bit6[show_bits(st, 6)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 9:
		d = &total_zeros9_bit6[show_bits(st, 6)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 10:
		d = &total_zeros10_bit5[show_bits(st, 5)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 11:
		d = &total_zeros11_bit4[show_bits(st, 4)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 12:
		d = &total_zeros12_bit4[show_bits(st, 4)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 13:
		d = &total_zeros13_bit3[show_bits(st, 3)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 14:
		d = &total_zeros14_bit2[show_bits(st, 2)];
		zeros = d->pattern;
		skip_bits(st, d->length);
		break;
	case 15:
		zeros = get_onebit_inline(st);
		break;
	}
	return zeros;
}

/**Read total_zeros for Chroma DC.
 */
static int8_t total_zeros4(dec_bits *st, int total_coeff)
{
	if (get_onebit(st)) {
		return 0;
	}
	if (total_coeff == 1) {
		if (get_onebit(st)) {
			return 1;
		} else {
			return 3 - get_onebit(st);
		}
	} else if (total_coeff == 2) {
		return 2 - get_onebit(st);
	} else {
		return 1;
	}
}

static inline int run_before(dec_bits *st, int zeros_left)
{
	const vlc_t *d;
	int r;

	switch (zeros_left) {
	case 0:
		r = 0;
		break;
	case 1:
		r = get_onebit(st) ^ 1;
		break;
	case 2:
		d = &run_before_2_bit2[show_bits(st, 2)];
		r = d->pattern;
		skip_bits(st, d->length);
		break;
	case 3:
		r = 3 - get_bits(st, 2);
		break;
	case 4:
		d = &run_before_4_bit3[show_bits(st, 3)];
		r = d->pattern;
		skip_bits(st, d->length);
		break;
	case 5:
		d = &run_before_5_bit3[show_bits(st, 3)];
		r = d->pattern;
		skip_bits(st, d->length);
		break;
	case 6:
		d = &run_before_6_bit3[show_bits(st, 3)];
		r = d->pattern;
		skip_bits(st, d->length);
		break;
	default:
		r = m2d_dec_vld_unary(st, run_before_7_bit3, 3);
		break;
	}
	return r;
}

static const int8_t inverse_zigzag4x4dc[2][16] = {
	{
		0, 1, 4, 8,
		5, 2, 3, 6,
		9, 12, 13, 10,
		7, 11, 14, 15,
	},
	{
		0, 4, 1, 8,
		12, 5, 9, 13,
		2, 6, 10, 14,
		3, 7, 11, 15
	}
};

#if 1
static const int8_t inverse_zigzag4x4[2][16] = {
	{
		0, 4, 1, 2,
		5, 8, 12, 9,
		6, 3, 7, 10,
		13, 14, 11, 15
	},
	{
		0, 1, 4, 2,
		3, 5, 6, 7,
		8, 9, 10, 11,
		12, 13, 14, 15
	}
};
#endif

static const int8_t inverse_zigzag2x2[2][4] = {
	{0, 1, 2, 3},
	{0, 1, 2, 3}
};

static const int8_t inverse_zigzag8x8[2][64] = {
	{
		0, 1, 8, 16, 9, 2, 3, 10,
		17, 24, 32, 25, 18, 11, 4, 5,
		12, 19, 26, 33, 40, 48, 41, 34,
		27, 20, 13, 6, 7, 14, 21, 28,
		35, 42, 49, 56, 57, 50, 43, 36,
		29, 22, 15, 23, 30, 37, 44, 51,
		58, 59, 52, 45, 38, 31, 39, 46,
		53, 60, 61, 54, 47, 55, 62, 63,
	},
	{
		0, 8, 16, 1, 9, 24, 32, 17,
		2, 25, 40, 48, 56, 33, 10, 3,
		18, 41, 49, 57, 26, 11, 4, 19,
		34, 42, 50, 58, 27, 12, 5, 20,
		35, 43, 51, 59, 28, 13, 6, 21,
		36, 44, 52, 60, 29, 14, 22, 37,
		45, 53, 61, 30, 7, 15, 38, 46,
		54, 62, 23, 31, 39, 47, 55, 63,
	}
};

static const int8_t * const inverse_zigzag[6] = {
	inverse_zigzag4x4dc[0],
	inverse_zigzag4x4[0],
	inverse_zigzag4x4[0],
	inverse_zigzag2x2[0],
	inverse_zigzag4x4[0],
	inverse_zigzag8x8[0]
};

static const struct {
	uint8_t cabac_coeff_abs_level_offset;
	int8_t coeff_offset;
	int8_t num_coeff;
	int8_t coeff_dc_mask;
} coeff_ofs[6] = {
	{0, 0, 16, 0}, {10, 1, 15, 15}, {20, 0, 16, 15}, {30, 0, 4, 0}, {39, 1, 15, 15}, {426 - 227, 0, 64, 63}
};

static inline void coeff_writeback(int *coeff, int total_coeff, const int8_t *run, const int *level, const int16_t *qmat, int cat)
{
	static const int8_t error_idx_mask[6] = {
		15, 15, 15, 3, 15, 63
	};
	const int8_t *zigzag = inverse_zigzag[cat];
	int idx = coeff_ofs[cat].coeff_offset;
	memset(coeff + idx, 0, sizeof(*coeff) * coeff_ofs[cat].num_coeff);
	uint32_t dc_mask = coeff_ofs[cat].coeff_dc_mask;
	uint32_t err_mask = error_idx_mask[cat];
	idx--;
	for (int i = total_coeff - 1; 0 <= i; --i) {
		idx = (idx + 1 + run[i]) & err_mask;
		int zig_idx = zigzag[idx];
		coeff[zig_idx] = level[i] * qmat[zig_idx & dc_mask];
	}
	
}

struct transform_size_8x8_flag_dummy {
	void operator()() const {}
};

struct transform_size_8x8_flag_cavlc {
	int operator()(h264d_mb_current *mb, dec_bits_t *st, int avail) const {
		return get_onebit_inline(st);
	}
};

static inline int SQUARE(int x) {
	return x * x;
}

struct residual_block_cavlc {
	int operator()(h264d_mb_current *mb, int na, int nb, dec_bits *st, int *coeff, const int16_t *qmat, int avail, int pos4x4, int cat, uint32_t dc_mask) const {
		int level[16];
		int8_t run[16];
		const vlc_t *tbl;
		int zeros_left;
		int num_coeff = coeff_ofs[cat].num_coeff;

		if (num_coeff <= 4) {
			tbl = total_ones_nc_chroma_bit6;
		} else {
			int nc = get_nC(na, nb);
			if (8 <= nc) {
				tbl = total_ones_nc8_bit6;
			} else if (4 <= nc) {
				tbl = total_ones_nc48_bit6;
			} else if (2 <= nc) {
				tbl = total_ones_nc24_bit6;
			} else {
				tbl = total_ones_nc02_bit6;
			}
		}
		int val = m2d_dec_vld_unary(st, tbl, 6);
		int total_coeff = val & 31;
		if (total_coeff == 0) {
			return 0;
		}
		int trailing_ones = val >> 5;
		read_trailing_ones(st, level, trailing_ones);
		int suffix_len = ((10 < total_coeff) && (trailing_ones < 3));
		for (int i = trailing_ones; i < total_coeff; ++i) {
			int lvl_prefix = level_prefix(st);
			int lvl = lvl_prefix << suffix_len;
			if (0 < suffix_len || 14 <= lvl_prefix) {
				int size = suffix_len;
				if (lvl_prefix == 14 && !size) {
					size = 4;
				} else if (lvl_prefix == 15) {
					size = 12;
				}
				if (size) {
					lvl += get_bits(st, size);
				}
			}
			if (suffix_len == 0 && lvl_prefix == 15) {
				lvl += 15;
			}
			if (i == trailing_ones && trailing_ones < 3) {
				lvl += 2;
			}
			level[i] = lvl = ((-(lvl & 1) ^ lvl) >> 1) + ((lvl & 1) ^ 1); // ((lvl & 1) ? ~lvl : (lvl + 2)) >> 1;
			suffix_len = suffix_len ? suffix_len : 1;
			suffix_len += ((suffix_len < 6) && (SQUARE(3 << (suffix_len - 1)) < SQUARE(lvl)));
		}
		if (total_coeff < num_coeff) {
			if (4 < num_coeff) {
				zeros_left = total_zeros16(st, total_coeff);
			} else {
				zeros_left = total_zeros4(st, total_coeff);
			}
		} else {
			zeros_left = 0;
		}
		for (int i = 0; i < total_coeff - 1; ++i) {
			int r = run_before(st, zeros_left);
			run[i] = r;
			zeros_left -= r;
		}
		run[total_coeff - 1] = zeros_left;
		coeff_writeback(coeff, total_coeff, run, level, qmat, cat);
		return total_coeff <= 15 ? total_coeff : 15;
	}
};

static void intra_chroma_dc_transform(const int *src, int *dst);
static void ac4x4transform_dconly_chroma(uint8_t *dst, int dc, int stride)
{
	int y;
	dc = (dc + 32) >> 6;
	y = 4;
	do {
		int t;
		t = dst[0] + dc;
		dst[0] = CLIP255C(t);
		t = dst[2] + dc;
		dst[2] = CLIP255C(t);
		t = dst[4] + dc;
		dst[4] = CLIP255C(t);
		t = dst[6] + dc;
		dst[6] = CLIP255C(t);
		dst += stride;
	} while (--y);
}

#ifdef X86ASM
#define TRANSPOSE4x4(a, b, c, d) {\
	__m128i t0 = _mm_unpackhi_epi32(a, c);\
	a = _mm_unpacklo_epi32(a, c);\
	__m128i t1 = _mm_unpackhi_epi32(b, d);\
	b = _mm_unpacklo_epi32(b, d);\
	c = _mm_unpacklo_epi32(t0, t1);\
	d = _mm_unpackhi_epi32(t0, t1);\
	t0 = _mm_unpackhi_epi32(a, b);\
	a = _mm_unpacklo_epi32(a, b);\
	b = t0;\
}

static void ac4x4transform_acdc_luma(uint8_t *dst, const int *coeff, int stride)
{
	__m128i d0 = _mm_load_si128((__m128i const *)coeff);
	__m128i rnd = _mm_cvtsi32_si128(32);
	__m128i d1 = _mm_load_si128((__m128i const *)(coeff + 4));
	__m128i d2 = _mm_load_si128((__m128i const *)(coeff + 8));
	__m128i d3 = _mm_load_si128((__m128i const *)(coeff + 12));
	d0 = _mm_add_epi32(d0, rnd);
	__m128i t0 = _mm_add_epi32(d0, d2);
	__m128i t1 = _mm_sub_epi32(d0, d2);
	__m128i t2 = _mm_srai_epi32(d1, 1);
	__m128i t3 = _mm_srai_epi32(d3, 1);
	t2 = _mm_sub_epi32(t2, d3);
	t3 = _mm_add_epi32(t3, d1);
	d0 = _mm_add_epi32(t0, t3);
	d1 = _mm_add_epi32(t1, t2);
	d2 = _mm_sub_epi32(t1, t2);
	d3 = _mm_sub_epi32(t0, t3);
	TRANSPOSE4x4(d0, d1, d2, d3);
	t0 = _mm_add_epi32(d0, d2);
	t1 = _mm_sub_epi32(d0, d2);
	t2 = _mm_srai_epi32(d1, 1);
	t3 = _mm_srai_epi32(d3, 1);
	t2 = _mm_sub_epi32(t2, d3);
	t3 = _mm_add_epi32(t3, d1);
	d0 = _mm_add_epi32(t0, t3);
	d1 = _mm_add_epi32(t1, t2);
	d2 = _mm_sub_epi32(t1, t2);
	d3 = _mm_sub_epi32(t0, t3);
	t1 = _mm_set_epi32(*(int *)(dst + stride * 3), *(int *)(dst + stride * 2), *(int *)(dst + stride), *(int *)dst);
	d0 = _mm_srai_epi32(d0, 6);
	d1 = _mm_srai_epi32(d1, 6);
	d2 = _mm_srai_epi32(d2, 6);
	d3 = _mm_srai_epi32(d3, 6);
	t0 = _mm_setzero_si128();
	d2 = _mm_packs_epi32(d2, d3);
	d0 = _mm_packs_epi32(d0, d1);
	t2 = _mm_unpackhi_epi8(t1, t0);
	t1 = _mm_unpacklo_epi8(t1, t0);
	d2 = _mm_adds_epi16(d2, t2);
	d0 = _mm_adds_epi16(d0, t1);
	d0 = _mm_packus_epi16(d0, d2);
	_mm_store_ss((float *)dst, _mm_castsi128_ps(d0));
	dst += stride;
	d0 = _mm_shuffle_epi32(d0, 0x39);
	_mm_store_ss((float *)dst, _mm_castsi128_ps(d0));
	dst += stride;
	d0 = _mm_shuffle_epi32(d0, 0x39);
	_mm_store_ss((float *)dst, _mm_castsi128_ps(d0));
	dst += stride;
	d0 = _mm_shuffle_epi32(d0, 0x39);
	_mm_store_ss((float *)dst, _mm_castsi128_ps(d0));
}

static void ac4x4transform_acdc_chroma(uint8_t *dst, const int *coeff, int stride)
{
	__m128i d0 = _mm_load_si128((__m128i const *)coeff);
	__m128i rnd = _mm_cvtsi32_si128(32);
	__m128i d1 = _mm_load_si128((__m128i const *)(coeff + 4));
	__m128i d2 = _mm_load_si128((__m128i const *)(coeff + 8));
	__m128i d3 = _mm_load_si128((__m128i const *)(coeff + 12));
	d0 = _mm_add_epi32(d0, rnd);
	__m128i t0 = _mm_add_epi32(d0, d2);
	__m128i t1 = _mm_sub_epi32(d0, d2);
	__m128i t2 = _mm_srai_epi32(d1, 1);
	__m128i t3 = _mm_srai_epi32(d3, 1);
	t2 = _mm_sub_epi32(t2, d3);
	t3 = _mm_add_epi32(t3, d1);
	d0 = _mm_add_epi32(t0, t3);
	d1 = _mm_add_epi32(t1, t2);
	d2 = _mm_sub_epi32(t1, t2);
	d3 = _mm_sub_epi32(t0, t3);
	TRANSPOSE4x4(d0, d1, d2, d3);
	t0 = _mm_add_epi32(d0, d2);
	t1 = _mm_sub_epi32(d0, d2);
	t2 = _mm_srai_epi32(d1, 1);
	t3 = _mm_srai_epi32(d3, 1);
	t2 = _mm_sub_epi32(t2, d3);
	t3 = _mm_add_epi32(t3, d1);
	d0 = _mm_add_epi32(t0, t3);
	d1 = _mm_add_epi32(t1, t2);
	d2 = _mm_sub_epi32(t1, t2);
	d3 = _mm_sub_epi32(t0, t3);
	d0 = _mm_srai_epi32(d0, 6);
	d1 = _mm_srai_epi32(d1, 6);
	d2 = _mm_srai_epi32(d2, 6);
	d3 = _mm_srai_epi32(d3, 6);
	d2 = _mm_packs_epi32(d2, d3);
	d0 = _mm_packs_epi32(d0, d1);
	__m128i mask = _mm_cvtsi32_si128(0x0000ffff << (((uintptr_t)dst & 1) * 16));
	dst = (uint8_t *)((uintptr_t)dst & ~1);
	t0 = _mm_castpd_si128(_mm_load_sd((double const *)dst));
	t2 = _mm_castpd_si128(_mm_load_sd((double const *)(dst + stride * 2)));
	t0 = _mm_castpd_si128(_mm_loadh_pd(_mm_castsi128_pd(t0), (double const *)(dst + stride)));
	t2 = _mm_castpd_si128(_mm_loadh_pd(_mm_castsi128_pd(t2), (double const *)(dst + stride * 3)));
	mask = _mm_shuffle_epi32(mask, 0);
	d3 = _mm_unpackhi_epi16(d2, d2);
	d2 = _mm_unpacklo_epi16(d2, d2);
	d1 = _mm_unpackhi_epi16(d0, d0);
	d0 = _mm_unpacklo_epi16(d0, d0);
	d3 = _mm_and_si128(d3, mask);
	d2 = _mm_and_si128(d2, mask);
	d1 = _mm_and_si128(d1, mask);
	d0 = _mm_and_si128(d0, mask);
	mask = _mm_setzero_si128();
	t1 = _mm_unpackhi_epi8(t0, mask);
	t0 = _mm_unpacklo_epi8(t0, mask);
	t3 = _mm_unpackhi_epi8(t2, mask);
	t2 = _mm_unpacklo_epi8(t2, mask);
	d1 = _mm_adds_epi16(d1, t1);
	d0 = _mm_adds_epi16(d0, t0);
	d3 = _mm_adds_epi16(d3, t3);
	d2 = _mm_adds_epi16(d2, t2);
	d0 = _mm_packus_epi16(d0, d1);
	d2 = _mm_packus_epi16(d2, d3);
	_mm_storel_epi64((__m128i *)dst, d0);
	dst += stride;
	d0 = _mm_shuffle_epi32(d0, 0x4e);
	_mm_storel_epi64((__m128i *)dst, d0);
	dst += stride;
	_mm_storel_epi64((__m128i *)dst, d2);
	dst += stride;
	d2 = _mm_shuffle_epi32(d2, 0x4e);
	_mm_storel_epi64((__m128i *)dst, d2);
}

#else
static inline void transform4x4_vert_loop(int *dst, const int *src)
{
	int t0, t1, t2, t3;
	int d0 = src[0] + 32;
	int d1 = src[4];
	int d2 = src[8];
	int d3 = src[12];
	t0 = d0 + d2;
	t1 = d0 - d2;
	t2 = (d1 >> 1) - d3;
	t3 = d1 + (d3 >> 1);
	dst[0] = t0 + t3;
	dst[1] = t1 + t2;
	dst[2] = t1 - t2;
	dst[3] = t0 - t3;
	d0 = src[1];
	d1 = src[5];
	d2 = src[9];
	d3 = src[13];
	t0 = d0 + d2;
	t1 = d0 - d2;
	t2 = (d1 >> 1) - d3;
	t3 = d1 + (d3 >> 1);
	dst[4] = t0 + t3;
	dst[5] = t1 + t2;
	dst[6] = t1 - t2;
	dst[7] = t0 - t3;
	d0 = src[2];
	d1 = src[6];
	d2 = src[10];
	d3 = src[14];
	t0 = d0 + d2;
	t1 = d0 - d2;
	t2 = (d1 >> 1) - d3;
	t3 = d1 + (d3 >> 1);
	dst[8] = t0 + t3;
	dst[9] = t1 + t2;
	dst[10] = t1 - t2;
	dst[11] = t0 - t3;
	d0 = src[3];
	d1 = src[7];
	d2 = src[11];
	d3 = src[15];
	t0 = d0 + d2;
	t1 = d0 - d2;
	t2 = (d1 >> 1) - d3;
	t3 = d1 + (d3 >> 1);
	dst[12] = t0 + t3;
	dst[13] = t1 + t2;
	dst[14] = t1 - t2;
	dst[15] = t0 - t3;
}

static inline void transform4x4_horiz_loop(uint8_t *dst, const int *src, int stride, int gap)
{
	int x = 4;
	do {
		int e1 = src[4];
		int e2 = src[8];
		int e3 = src[12];
		int e0 = *src++;
		uint8_t *d = dst;
		int f0 = e0 + e2;
		int f1 = e0 - e2;
		int f2 = (e1 >> 1) - e3;
		int f3 = e1 + (e3 >> 1);
		int t0 = *d + ((f0 + f3) >> 6);
		*d = CLIP255C(t0);
		d += stride;
		t0 = *d + ((f1 + f2) >> 6);
		*d = CLIP255C(t0);
		d += stride;
		t0 = *d + ((f1 - f2) >> 6);
		*d = CLIP255C(t0);
		d += stride;
		t0 = *d + ((f0 - f3) >> 6);
		*d = CLIP255C(t0);
		dst += gap;
	} while (--x);
}

/** Reconstruct 4x4 coefficients.
 */
static void ac4x4transform_acdc_base(uint8_t *dst, const int *coeff, int stride, int gap)
{
	int tmp[16];
	transform4x4_vert_loop(tmp, coeff);
	transform4x4_horiz_loop(dst, tmp, stride, gap);
}

static inline void ac4x4transform_acdc_luma(uint8_t *dst, const int *coeff, int stride)
{
	ac4x4transform_acdc_base(dst, coeff, stride, 1);
}

static inline void ac4x4transform_acdc_chroma(uint8_t *dst, const int *coeff, int stride)
{
	ac4x4transform_acdc_base(dst, coeff, stride, 2);
}
#endif

template <typename F0>
static inline int residual_chroma(h264d_mb_current *mb, uint32_t cbp, dec_bits *st, int avail, F0 ResidualBlock)
{
	int ALIGN16VC coeff[16] __attribute__((aligned(16)));
	int dc[2][4];
	uint8_t *chroma;
	int stride;
	int *dcp;

	cbp >>= 4;
	if (!cbp) {
		mb->left4x4coef &= 0x0000ffff;
		*mb->top4x4coef &= 0x0000ffff;
		return 0;
	}
	for (int i = 0; i < 2; ++i) {
		if (ResidualBlock(mb, 0, 0, st, coeff, mb->qmatc_p[i], avail, 16 + i, 3, 0)) {
			intra_chroma_dc_transform(coeff, dc[i]);
		} else {
			memset(dc[i], 0, sizeof(dc[0][0]) * 4);
		}
	}
	chroma = mb->chroma;
	stride = mb->max_x * 16;
	dcp = dc[0];
	if (cbp & 2) {
		int c0, c1, c2, c3;
		uint32_t left = mb->left4x4coef >> 16;
		uint32_t top = *mb->top4x4coef >> 16;
		for (int i = 0; i < 2; ++i) {
			int c0left, c2left;
			int c0top, c1top;
			if (avail & 1) {
				c0left = UNPACK(left, 0);
				c2left = UNPACK(left, 1);
			} else {
				c0left = c2left = -1;
			}
			if (avail & 2) {
				c0top = UNPACK(top, 0);
				c1top = UNPACK(top, 1);
			} else {
				c0top = c1top = -1;
			}
			if ((c0 = ResidualBlock(mb, c0left, c0top, st, coeff, mb->qmatc_p[i], avail, 18 + i * 4, 4, 0x1f)) != 0) {
				coeff[0] = *dcp++;
				ac4x4transform_acdc_chroma(chroma, coeff, stride);
			} else {
				ac4x4transform_dconly_chroma(chroma, *dcp++, stride);
			}
			if ((c1 = ResidualBlock(mb, c0, c1top, st, coeff, mb->qmatc_p[i], avail, 19 + i * 4, 4, 0x1f)) != 0) {
				coeff[0] = *dcp++;
				ac4x4transform_acdc_chroma(chroma + 8, coeff, stride);
			} else {
				ac4x4transform_dconly_chroma(chroma + 8, *dcp++, stride);
			}
			if ((c2 = ResidualBlock(mb, c2left, c0, st, coeff, mb->qmatc_p[i], avail, 20 + i * 4, 4, 0x1f)) != 0) {
				coeff[0] = *dcp++;
				ac4x4transform_acdc_chroma(chroma + stride * 4, coeff, stride);
			} else {
				ac4x4transform_dconly_chroma(chroma + stride * 4, *dcp++, stride);
			}
			if ((c3 = ResidualBlock(mb, c2, c1, st, coeff, mb->qmatc_p[i], avail, 21 + i * 4, 4, 0x1f)) != 0) {
				coeff[0] = *dcp++;
				ac4x4transform_acdc_chroma(chroma + stride * 4 + 8, coeff, stride);
			} else {
				ac4x4transform_dconly_chroma(chroma + stride * 4 + 8, *dcp++, stride);
			}
			left = ((left >> 8) & 0xff) | (c3 << 12) | (c1 << 8);
			top = ((top >> 8) & 0xff)| (c3 << 12) | (c2 << 8);
			chroma++;
		}
		mb->left4x4coef = (mb->left4x4coef & 0x0000ffff) | (left << 16);
		*mb->top4x4coef = (*mb->top4x4coef & 0x0000ffff) | (top << 16);
	} else {
		ac4x4transform_dconly_chroma(chroma, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + 8, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + stride * 4, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + stride * 4 + 8, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + 1, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + 9, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + stride * 4 + 1, *dcp++, stride);
		ac4x4transform_dconly_chroma(chroma + stride * 4 + 9, *dcp++, stride);
		mb->left4x4coef &= 0x0000ffff;
		*mb->top4x4coef &= 0x0000ffff;
	}
	VC_CHECK;
	return 0;
}

/* intra4x4 prediction */
template <int N>
static uint32_t sum_top(uint8_t *src, int stride)
{
	uint32_t dc = 0;
	int i = N / 4;
	src -= stride;
	do {
		dc += *src++;
		dc += *src++;
		dc += *src++;
		dc += *src++;
	} while (--i);
	return dc;
}

template <int N>
static uint32_t sum_left(uint8_t *src, int stride)
{
	uint32_t dc = 0;
	int i = N / 4;
	src--;
	do {
		dc += *src;
		src += stride;
		dc += *src;
		src += stride;
		dc += *src;
		src += stride;
		dc += *src;
		src += stride;
	} while (--i);
	return dc;
}

struct intra4x4pred_mode_cavlc {
	int operator()(int a, int b, dec_bits *st, h264d_cabac_t *cb) const {
		int pred = MIN(a, b);
		if (!get_onebit_inline(st)) {
			int rem = get_bits(st, 3);
			pred = (rem < pred) ? rem : rem + 1;
		}
		return pred;
	}
};

template <int N>
static int intraNxNpred_dc(uint8_t *dst, int stride, int avail)
{
	uint32_t dc;

	if (avail & 1) {
		if (avail & 2) {
			dc = (sum_left<N>(dst, stride) + sum_top<N>(dst, stride) + N) >> ((N / 8) + 3);
		} else {
			dc = (sum_left<N>(dst, stride) + (N / 2)) >> ((N / 8) + 2);
		}
	} else if (avail & 2) {
		dc = (sum_top<N>(dst, stride) + (N / 2)) >> ((N / 8) + 2);
	} else {
		dc = 0x80;
	}
	dc = dc * 0x01010101U;
	int i = N;
	do {
		for (int j = 0; j < N / 4; ++j) {
			((uint32_t *)dst)[j] = dc;
		}
		dst += stride;
	} while (--i);
	return 0;
}

template <int N>
static int intraNxNpred_horiz(uint8_t *dst, int stride, int avail)
{
	if (!(avail & 1)) {
		return -1;
	}
	int i = N;
	do {
		uint32_t t0 = dst[-1] * 0x01010101U;
		for (int j = 0; j < N / 4; ++j) {
			((uint32_t *)dst)[j] = t0;
		}
		dst = dst + stride;
	} while (--i);
	return 0;
}

static int intra4x4pred_vert(uint8_t *dst, int stride, int avail)
{
	uint32_t *src;
	uint32_t t0;

	if (!(avail & 2)) {
		return -1;
	}
	src = (uint32_t *)(dst - stride);

	t0 = *src;
	*(uint32_t *)dst = t0;
	dst += stride;
	*(uint32_t *)dst = t0;
	dst += stride;
	*(uint32_t *)dst = t0;
	dst += stride;
	*(uint32_t *)dst = t0;
	return 0;
}

#define FIR3(a, b, c) (((a) + (b) * 2 + (c) + 2) >> 2)
#define FIR2(a, b) (((a) + (b) + 1) >> 1)

/**Intra 4x4 prediction Diagonal Down Left.
 */
static int intra4x4pred_ddl(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	uint32_t t0, t1, t2, d0;
	src = dst - stride;
	t0 = *src++;
	t1 = *src++;
	t2 = *src++;
	d0 = FIR3(t0, t1, t2);
	t0 = *src++;
	if (avail & 4) {
		d0 = (d0 << 8) | FIR3(t1, t2, t0);
		t1 = *src++;
		d0 = (d0 << 8) | FIR3(t2, t0, t1);
		t2 = *src++;
		d0 = (d0 << 8) | FIR3(t0, t1, t2);
#ifndef WORDS_BIGENDIAN
		d0 = bswap32(d0);
#endif
		t0 = *src++;
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | (FIR3(t1, t2, t0);
#else
		d0 = (FIR3(t1, t2, t0) << 24) | (d0 >> 8);
#endif
		t1 = *src++;
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | FIR3(t1, t2, t0);
#else
		d0 = (FIR3(t2, t0, t1) << 24) | (d0 >> 8);
#endif
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | FIR3(t1, t2, t0);
#else
		d0 = (FIR3(t0, t1, t1) << 24) | (d0 >> 8);
#endif
	} else {
		d0 = (d0 << 8) | FIR3(t1, t2, t0);
		d0 = (d0 << 8) | FIR3(t2, t0, t0);
		d0 = (d0 << 8) | t0;
#ifndef WORDS_BIGENDIAN
		d0 = bswap32(d0);
		t0 = t0 << 24;
#endif
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | t0;
#else
		d0 = t0 | (d0 >> 8);
#endif
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | t0;
#else
		d0 = t0 | (d0 >> 8);
#endif
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | t0;
#else
		d0 = t0 | (d0 >> 8);
#endif
	}
	*(uint32_t *)dst = d0;
	return 0;
}

/** Intra 4x4 prediction Diagonal Down Right.
 */
static int intra4x4pred_ddr(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	uint32_t t0, t1, t2, t3, d0;
	if ((avail & 3) != 3) {
		return -1;
	}
	src = dst - stride - 1;
	t0 = *src++;
	t1 = *src++;
	t2 = *src++;
	d0 = FIR3(t0, t1, t2);
	t3 = *src++;
	d0 = (d0 << 8) | FIR3(t1, t2, t3);
	d0 = (d0 << 8) | FIR3(t2, t3, *src);
	src = dst - 1;
	t3 =  *src;
	d0 = (FIR3(t3, t0, t1) << 24) | d0;
#ifndef WORDS_BIGENDIAN
	d0 = bswap32(d0);
#endif
	src += stride;
	*(uint32_t *)dst = d0;

	t2 = *src;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 >> 8) | (FIR3(t2, t3, t0) << 24);
#else
	d0 = (d0 << 8) | FIR3(t2, t3, t0);
#endif
	src += stride;
	*(uint32_t *)dst = d0;

	t1 = *src;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 >> 8) | (FIR3(t1, t2, t3) << 24);
#else
	d0 = (d0 << 8) | FIR3(t1, t2, t3);
#endif
	src += stride;
	*(uint32_t *)dst = d0;

	t0 = *src;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 >> 8) | (FIR3(t0, t1, t2) << 24);
#else
	d0 = (d0 << 8) | FIR3(t0, t1, t2);
#endif
	*(uint32_t *)dst = d0;
	return 0;
}

/** Intra 4x4 prediction Vertical Right.
 */
static int intra4x4pred_vr(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	uint32_t t0, t1, t2, t3, t4, d0, d1;
	if ((avail & 3) != 3) {
		return -1;
	}
	src = dst - stride - 1;
	t0 = *src++;
	t1 = *src++;
	d0 = FIR2(t0, t1);
	t2 = *src++;
	d0 = (FIR2(t1, t2) << 8) | d0;
	d1 = FIR3(t0, t1, t2) << 8;
	t3 = *src++;
	d0 = (FIR2(t2, t3) << 16) | d0;
	d1 = (FIR3(t1, t2, t3) << 16) | d1;

	t4 = *src++;
	d0 = (FIR2(t3, t4) << 24) | d0;
	d1 = (FIR3(t2, t3, t4) << 24) | d1;
#ifdef WORDS_BIGENDIAN
	d0 = bswap32(d0);
#endif
	src = dst - 1;
	*(uint32_t *)dst = d0;

	t4 = *src;
	dst += stride;
	d1 = d1 | FIR3(t4, t0, t1);
#ifdef WORDS_BIGENDIAN
	d0 = bswap32(d0);
#endif
	src += stride;
	*(uint32_t *)dst = d1;

	t3 = *src;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (FIR3(t3, t4, t0) << 24)| (d0 >> 8);
#else
	d0 = (d0 << 8) | FIR3(t3, t4, t0);
#endif
	src += stride;
	*(uint32_t *)dst = d0;

	t2 = *src;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d1 = (FIR3(t2, t3, t4) << 24)| (d1 >> 8);
#else
	d1 = (d1 << 8) | FIR3(t2, t3, t4);
#endif
	*(uint32_t *)dst = d1;
	return 0;
}

/** Intra 4x4 prediction Horizontal Down.
 */
static int intra4x4pred_hd(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	uint32_t t0, t1, t2, d0;
	if ((avail & 3) != 3) {
		return -1;
	}
	src = dst - stride - 1;
	t0 = *src++;
	t1 = *src++;
	t2 = *src++;
	d0 = FIR3(t1, t2, *src);
	src = dst - 1;
	d0 = (d0 << 8) | FIR3(t0, t1, t2);
	t2 = *src;
	d0 = (d0 << 8) | FIR3(t1, t0, t2);
	src += stride;
	d0 = (d0 << 8) | FIR2(t0, t2);
#ifdef WORDS_BIGENDIAN
	d0 = bswap32(d0);
#endif
	*(uint32_t *)dst = d0;

	t1 = *src;
	dst += stride;
	src += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (FIR3(t0, t2, t1) << 16) | (d0 >> 16);
	d0 = (FIR2(t2, t1) << 24) | d0;
#else
	d0 = (d0 << 8) | FIR3(t0, t2, t1);
	d0 = (d0 << 8) | FIR2(t2, t1);
#endif
	*(uint32_t *)dst = d0;

	t0 = *src;
	dst += stride;
	src += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (FIR3(t2, t1, t0) << 16) | (d0 >> 16);
	d0 = (FIR2(t1, t0) << 24) | d0;
#else
	d0 = (d0 << 8) | FIR3(t2, t1, t0);
	d0 = (d0 << 8) | FIR2(t1, t0);
#endif
	*(uint32_t *)dst = d0;

	t2 = *src;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (FIR3(t1, t0, t2) << 16) | (d0 >> 16);
	d0 = (FIR2(t0, t2) << 24) | d0;
#else
	d0 = (d0 << 8) | FIR3(t1, t0, t2);
	d0 = (d0 << 8) | FIR2(t0, t2);
#endif
	*(uint32_t *)dst = d0;
	return 0;
}

/** Intra 4x4 prediction Vertical Left.
 */
static int intra4x4pred_vl(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	uint32_t t0, t1, t2, d0, d1;

	src = dst - stride;
	t0 = *src++;
	t1 = *src++;
	t2 = *src++;
	d0 = FIR2(t0, t1);
#ifdef WORDS_BIGENDIAN
	d0 = (d0 << 8) | FIR2(t1, t2);
#else
	d0 = (FIR2(t1, t2) << 8) | d0;
#endif
	d1 = FIR3(t0, t1, t2);
	t0 = *src++;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 << 8) | FIR2(t2, t0);
	d1 = (d1 << 8) | FIR3(t1, t2, t0);
#else
	d0 = (FIR2(t2, t0) << 16) | d0;
	d1 = (FIR3(t1, t2, t0) << 8) | d1;
#endif
	if (avail & 4) {
		t1 = *src++;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | FIR2(t1, t0);
#else
		d0 = (FIR2(t1, t0) << 24) | d0;
#endif
		*(uint32_t *)dst = d0;

#ifdef WORDS_BIGENDIAN
		d1 = (d1 << 8) | FIR3(t1, t0, t2) << 16);
#else
		d1 = (FIR3(t1, t0, t2) << 16) | d1;
#endif
		dst += stride;
		t2 = *src++;
#ifdef WORDS_BIGENDIAN
		d1 = (d1 << 8) | FIR3(t2, t1, t0);
#else
		d1 = (FIR3(t2, t1, t0) << 24) | d1;
#endif
		*(uint32_t *)dst = d1;

		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | FIR2(t2, t1);
#else
		d0 = (FIR2(t2, t1) << 24) | (d0 >> 8);
#endif
		*(uint32_t *)dst = d0;

		t0 = *src;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d1 = (d1 << 8) | FIR3(t1, t2, t0);
#else
		d1 = (FIR3(t1, t2, t0) << 24) | (d1 >> 8);
#endif
	} else {
		t1 = FIR3(t2, t0, t0);
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | t0;
#else
		t0 <<= 24;
		d0 = t0 | d0;
#endif
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d1 = (d1 << 16) | (t1 << 8) | t0;
#else
		d1 = (t1 << 16) | d1 | t0;
#endif
		*(uint32_t *)dst = d1;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d0 = (d0 << 8) | t0;
#else
		d0 = (d0 >> 8) | t0;
#endif
		*(uint32_t *)dst = d0;
		dst += stride;
#ifdef WORDS_BIGENDIAN
		d1 = (d1 << 8) | t0;
#else
		d1 = (d1 >> 8) | t0;
#endif
	}
	*(uint32_t *)dst = d1;
	return 0;
}

/** Intra 4x4 prediction Horizontal Up.
 */
static int intra4x4pred_hu(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	uint32_t t0, t1, t2, d0;
	if (!(avail & 1)) {
		return -1;
	}
	src = dst - 1;
	t0 = *src;
	src += stride;
	t1 = *src;
	src += stride;
	d0 = FIR2(t0, t1);
	t2 = *src;
	src += stride;
	d0 = (FIR3(t0, t1, t2) << 8) | d0;
	d0 = (FIR2(t1, t2) << 16) | d0;
	t0 = *src;
	d0 = (FIR3(t1, t2, t0) << 24) | d0;
#ifdef WORDS_BIGENDIAN
	d0 = bswap32(d0);
#endif
	*(uint32_t *)dst = d0;

	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 << 8) | FIR2(t2, t0);
	d0 = (d0 << 8) | FIR3(t2, t0, t0);
#else
	d0 = (FIR2(t2, t0) << 16) | (d0 >> 16);
	d0 = (FIR3(t2, t0, t0) << 24) | d0;
#endif
	*(uint32_t *)dst = d0;

	dst += stride;
#ifdef WORDS_BIGENDIAN
	t1 = (t0 << 8) | d0;
	d0 = (d0 << 16) | t1;
#else
	t1 = (t0 << 24) | (t0 << 16);
	d0 = t1 | (d0 >> 16);
#endif
	*(uint32_t *)dst = d0;

	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 << 16) | t1;
#else
	d0 = t1 | (d0 >> 16 );
#endif
	*(uint32_t *)dst = d0;
	return 0;
}

static int (* const intra4x4pred_func[9])(uint8_t *dst, int stride, int avail) = {
	intra4x4pred_vert,
	intraNxNpred_horiz<4>,
	intraNxNpred_dc<4>,
	intra4x4pred_ddl,
	intra4x4pred_ddr,
	intra4x4pred_vr,
	intra4x4pred_hd,
	intra4x4pred_vl,
	intra4x4pred_hu
};

template <typename F>
static int mb_pred_intra4x4(h264d_mb_current *mb, dec_bits *st, int avail, int8_t *pred4x4, F I4x4PredMode) {
	uint32_t left = mb->left4x4pred;
	uint32_t top = *mb->top4x4pred;
	h264d_cabac_t *cb = mb->cabac;
	pred4x4[0] = I4x4PredMode(avail & 2 ? UNPACK(left, 0) : 2, avail & 1 ? UNPACK(top, 0) : 2, st, cb);
	pred4x4[1] = I4x4PredMode(avail & 2 ? pred4x4[0] : 2, UNPACK(top, 1), st, cb);
	pred4x4[2] = I4x4PredMode(UNPACK(left, 1), avail & 1 ? pred4x4[0] : 2, st, cb);
	pred4x4[3] = I4x4PredMode(pred4x4[2], pred4x4[1], st, cb);
	pred4x4[4] = I4x4PredMode(avail & 2 ? pred4x4[1] : 2, UNPACK(top, 2), st, cb);
	pred4x4[5] = I4x4PredMode(avail & 2 ? pred4x4[4] : 2, UNPACK(top, 3), st, cb);
	pred4x4[6] = I4x4PredMode(pred4x4[3], pred4x4[4], st, cb);
	pred4x4[7] = I4x4PredMode(pred4x4[6], pred4x4[5], st, cb);

	pred4x4[8] = I4x4PredMode(UNPACK(left, 2), avail & 1 ? pred4x4[2] : 2, st, cb);
	pred4x4[9] = I4x4PredMode(pred4x4[8], pred4x4[3], st, cb);
	pred4x4[10] = I4x4PredMode(UNPACK(left, 3), avail & 1 ? pred4x4[8] : 2, st, cb);
	pred4x4[11] = I4x4PredMode(pred4x4[10], pred4x4[9], st, cb);
	pred4x4[12] = I4x4PredMode(pred4x4[9], pred4x4[6], st, cb);
	pred4x4[13] = I4x4PredMode(pred4x4[12], pred4x4[7], st, cb);
	pred4x4[14] = I4x4PredMode(pred4x4[11], pred4x4[12], st, cb);
	pred4x4[15] = I4x4PredMode(pred4x4[14], pred4x4[13], st, cb);

	mb->left4x4pred = (pred4x4[15] << 12) | (pred4x4[13] << 8) | (pred4x4[7] << 4)| pred4x4[5];
	*mb->top4x4pred = (pred4x4[15] << 12) | (pred4x4[14] << 8) | (pred4x4[11] << 4)| pred4x4[10];
	return 0;
}

static inline void fill_dc_if_unavailable(h264d_mb_current *mb, int avail)
{
	if (!(avail & 1)) {
		mb->left4x4pred = 0x22222222;
	}
	if (!(avail & 2)) {
		*mb->top4x4pred = 0x22222222;
	}
}

static int mb_intra_chroma_pred_dc(uint8_t *dst, int stride, int avail);
static int mb_intra_chroma_pred_horiz(uint8_t *dst, int stride, int avail);
static int mb_intra_chroma_pred_planer(uint8_t *dst, int stride, int avail);

template <int N>
static int mb_intra16xpred_vert(uint8_t *dst, int stride, int avail)
{
	uint32_t *src;
	uint32_t t0, t1, t2, t3;
	int i;

	if (!(avail & 2)) {
		return -1;
	}
	src = (uint32_t *)(dst - stride);
	t0 = *src++;
	t1 = *src++;
	t2 = *src++;
	t3 = *src++;
	i = N;
	do {
		*((uint32_t *)dst) = t0;
		*((uint32_t *)dst + 1) = t1;
		*((uint32_t *)dst + 2) = t2;
		*((uint32_t *)dst + 3) = t3;
		dst += stride;
	} while (--i);
	return 0;
}


static int (* const intra_chroma_pred[4])(uint8_t *dst, int stride, int avail) = {
	mb_intra_chroma_pred_dc,
	mb_intra_chroma_pred_horiz,
	mb_intra16xpred_vert<8>,
	mb_intra_chroma_pred_planer
};

static inline void ac4x4transform_maybe(uint8_t *dst, const int *coeff, int stride, int num_coeff);
static void mb_intra_save_info(h264d_mb_current *mb, int8_t transform8x8)
{
	mb->lefttop_ref[0] = mb->top4x4inter->ref[1][0];
	mb->lefttop_ref[1] = mb->top4x4inter->ref[1][1];
	mb->lefttop_mv[0].vector = mb->top4x4inter->mov[3].mv[0].vector;
	mb->lefttop_mv[1].vector = mb->top4x4inter->mov[3].mv[1].vector;
	mb->left4x4inter->transform8x8 = transform8x8;
	mb->top4x4inter->transform8x8 = transform8x8;
	mb->left4x4inter->direct8x8 = 0;
	mb->top4x4inter->direct8x8 = 0;
	memset(mb->left4x4inter->mov, 0, sizeof(mb->left4x4inter->mov));
	memset(mb->left4x4inter->mvd, 0, sizeof(mb->left4x4inter->mvd));
	memset(mb->top4x4inter->mov, 0, sizeof(mb->top4x4inter->mov));
	memset(mb->top4x4inter->mvd, 0, sizeof(mb->top4x4inter->mvd));
	memset(mb->left4x4inter->ref, -1, sizeof(mb->left4x4inter->ref));
	memset(mb->left4x4inter->frmidx, -1, sizeof(mb->left4x4inter->frmidx));
	memset(mb->top4x4inter->ref, -1, sizeof(mb->top4x4inter->ref));
	memset(mb->top4x4inter->frmidx, -1, sizeof(mb->top4x4inter->frmidx));
	mb->col_curr->type = COL_MB16x16;
	memset(mb->col_curr->ref, -1, sizeof(mb->col_curr->ref));
}

static inline deblock_info_t *store_strength_intra_base(h264d_mb_current *mb) {
	deblock_info_t *deb = mb->deblock_curr;
	deb->qpy = mb->qp;
	deb->qpc[0] = mb->qp_chroma[0];
	deb->qpc[1] = mb->qp_chroma[1];
	deb->str4_horiz = 1;
	deb->str4_vert = 1;
	return deb;
}

static inline void store_strength_intra(h264d_mb_current *mb) {
	deblock_info_t *deb = store_strength_intra_base(mb);
	deb->str_horiz = 0xffffffff;
	deb->str_vert = 0xffffffff;
}

static inline void store_strength_intra8x8(h264d_mb_current *mb) {
	deblock_info_t *deb = store_strength_intra_base(mb);
	deb->str_horiz = 0x00ff00ff;
	deb->str_vert = 0x00ff00ff;
}

template <typename F>
static inline void luma_intra4x4_with_residual(h264d_mb_current *mb, dec_bits *st, uint32_t cbp, int avail, int avail_intra, const int8_t *pr, int stride,
						    F ResidualBlock)
{
	int ALIGN16VC coeff[16] __attribute__((aligned(16)));
	uint32_t top, left;
	int c0, c1, c2, c3, c4, c5;
	uint8_t *luma = mb->luma;
	const int *offset = mb->offset4x4;
	const int16_t *qmat = mb->qmaty;

	if (cbp & 1) {
		intra4x4pred_func[*pr++](luma, stride, avail_intra | (avail_intra & 2 ? 4 : 0));
		c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 0) : -1, avail & 2 ? UNPACK(*mb->top4x4coef, 0) : -1, st, coeff, qmat, avail_intra, 0, 2, 0xf);
		ac4x4transform_maybe(luma, coeff, stride, c0);
		intra4x4pred_func[*pr++](luma + 4, stride, avail_intra | (avail_intra & 2 ? 5 : 1));
		c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 1) : -1, st, coeff, qmat, avail_intra, 1, 2, 0xf);
		ac4x4transform_maybe(luma + 4, coeff, stride, c1);
		intra4x4pred_func[*pr++](luma + offset[2], stride, avail_intra | 6);
		c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 1) : -1, c0, st, coeff, qmat, avail_intra, 2, 2, 0xf);
		ac4x4transform_maybe(luma + offset[2], coeff, stride, c2);
		intra4x4pred_func[*pr++](luma + offset[3], stride, 3);
		c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 3, 2, 0xf);
		ac4x4transform_maybe(luma + offset[3], coeff, stride, c3);
	} else {
		intra4x4pred_func[*pr++](luma, stride, avail_intra | (avail_intra & 2 ? 4 : 0));
		intra4x4pred_func[*pr++](luma + 4, stride, avail_intra | (avail_intra & 2 ? 5 : 1));
		intra4x4pred_func[*pr++](luma + offset[2], stride, avail_intra | 6);
		intra4x4pred_func[*pr++](luma + offset[3], stride, 3);
		c0 = 0;
		c1 = 0;
		c2 = 0;
		c3 = 0;
	}
	if (cbp & 2) {
		intra4x4pred_func[*pr++](luma + offset[4], stride, avail_intra | (avail_intra & 2 ? 5 : 1));
		c0 = ResidualBlock(mb, c1, avail & 2 ? UNPACK(*mb->top4x4coef, 2) : -1, st, coeff, qmat, avail_intra, 4, 2, 0xf);
		ac4x4transform_maybe(luma + offset[4], coeff, stride, c0);
		intra4x4pred_func[*pr++](luma + offset[5], stride, avail_intra | 1);
		c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 3) : -1, st, coeff, qmat, avail_intra, 5, 2, 0xf);
		left = PACK(0, c1, 0);
		ac4x4transform_maybe(luma + offset[5], coeff, stride, c1);
		intra4x4pred_func[*pr++](luma + offset[6], stride, 7);
		c4 = ResidualBlock(mb, c3, c0, st, coeff, qmat, avail_intra, 6, 2, 0xf);
		ac4x4transform_maybe(luma + offset[6], coeff, stride, c4);
		intra4x4pred_func[*pr++](luma + offset[7], stride, 3);
		c5 = ResidualBlock(mb, c4, c1, st, coeff, qmat, avail_intra, 7, 2, 0xf);
		left = PACK(left, c5, 1);
		ac4x4transform_maybe(luma + offset[7], coeff, stride, c5);
	} else {
		intra4x4pred_func[*pr++](luma + offset[4], stride, avail_intra | (avail_intra & 2 ? 5 : 1));
		intra4x4pred_func[*pr++](luma + offset[5], stride, avail_intra | 1);
		intra4x4pred_func[*pr++](luma + offset[6], stride, 7);
		intra4x4pred_func[*pr++](luma + offset[7], stride, 3);
		c0 = 0;
		c1 = 0;
		c4 = 0;
		c5 = 0;
		left = 0;
	}
	if (cbp & 4) {
		intra4x4pred_func[*pr++](luma + offset[8], stride, avail_intra | 6);
		c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 2) : -1, c2, st, coeff, qmat, avail_intra, 8, 2, 0xf);
		ac4x4transform_maybe(luma + offset[8], coeff, stride, c0);
		intra4x4pred_func[*pr++](luma + offset[9], stride, 7);
		c1 = ResidualBlock(mb, c0, c3, st, coeff, qmat, avail_intra, 9, 2, 0xf);
		ac4x4transform_maybe(luma + offset[9], coeff, stride, c1);
		intra4x4pred_func[*pr++](luma + offset[10], stride, avail_intra | 6);
		c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 3) : -1, c0, st, coeff, qmat, avail_intra, 10, 2, 0xf);
		top = PACK(0, c2, 0);
		ac4x4transform_maybe(luma + offset[10], coeff, stride, c2);
		intra4x4pred_func[*pr++](luma + offset[11], stride, 3);
		c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 11, 2, 0xf);
		top = PACK(top, c3, 1);
		ac4x4transform_maybe(luma + offset[11], coeff, stride, c3);
	} else {
		intra4x4pred_func[*pr++](luma + offset[8], stride, avail_intra | 6);
		intra4x4pred_func[*pr++](luma + offset[9], stride, 7);
		intra4x4pred_func[*pr++](luma + offset[10], stride, avail_intra | 6);
		intra4x4pred_func[*pr++](luma + offset[11], stride, 3);
		c0 = 0;
		c1 = 0;
		c2 = 0;
		c3 = 0;
		top = 0;
	}
	if (cbp & 8) {
		intra4x4pred_func[*pr++](luma + offset[12], stride, 7);
		c0 = ResidualBlock(mb, c1, c4, st, coeff, qmat, avail_intra, 12, 2, 0xf);
		ac4x4transform_maybe(luma + offset[12], coeff, stride, c0);
		intra4x4pred_func[*pr++](luma + offset[13], stride, 3);
		c1 = ResidualBlock(mb, c0, c5, st, coeff, qmat, avail_intra, 13, 2, 0xf);
		left = PACK(left, c1, 2);
		ac4x4transform_maybe(luma + offset[13], coeff, stride, c1);
		intra4x4pred_func[*pr++](luma + offset[14], stride, 7);
		c2 = ResidualBlock(mb, c3, c0, st, coeff, qmat, avail_intra, 14, 2, 0xf);
		top = PACK(top, c2, 2);
		ac4x4transform_maybe(luma + offset[14], coeff, stride, c2);
		intra4x4pred_func[*pr++](luma + offset[15], stride, 3);
		c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 15, 2, 0xf);
		ac4x4transform_maybe(luma + offset[15], coeff, stride, c3);
	} else {
		intra4x4pred_func[*pr++](luma + offset[12], stride, 7);
		intra4x4pred_func[*pr++](luma + offset[13], stride, 3);
		intra4x4pred_func[*pr++](luma + offset[14], stride, 7);
		intra4x4pred_func[*pr++](luma + offset[15], stride, 3);
		c3 = 0; 
	}
	mb->left4x4coef = (mb->left4x4coef & 0xffff0000) | PACK(left, c3, 3);
	*mb->top4x4coef = (*mb->top4x4coef & 0xffff0000) | PACK(top, c3, 3);
}

static inline void luma_intra4x4_pred(h264d_mb_current *mb, int avail_intra, const int8_t *pr, int stride)
{
	uint8_t *luma = mb->luma;
	const int *offset = mb->offset4x4;
	intra4x4pred_func[*pr++](luma, stride, avail_intra | (avail_intra & 2 ? 4 : 0));
	intra4x4pred_func[*pr++](luma + 4, stride, avail_intra | (avail_intra & 2 ? 5 : 1));
	intra4x4pred_func[*pr++](luma + offset[2], stride, avail_intra | 6);
	intra4x4pred_func[*pr++](luma + offset[3], stride, 3);
	intra4x4pred_func[*pr++](luma + offset[4], stride, avail_intra | (avail_intra & 2 ? 5 : 1));
	intra4x4pred_func[*pr++](luma + offset[5], stride, avail_intra | 1);
	intra4x4pred_func[*pr++](luma + offset[6], stride, 7);
	intra4x4pred_func[*pr++](luma + offset[7], stride, 3);
	intra4x4pred_func[*pr++](luma + offset[8], stride, avail_intra | 6);
	intra4x4pred_func[*pr++](luma + offset[9], stride, 7);
	intra4x4pred_func[*pr++](luma + offset[10], stride, avail_intra | 6);
	intra4x4pred_func[*pr++](luma + offset[11], stride, 3);
	intra4x4pred_func[*pr++](luma + offset[12], stride, 7);
	intra4x4pred_func[*pr++](luma + offset[13], stride, 3);
	intra4x4pred_func[*pr++](luma + offset[14], stride, 7);
	intra4x4pred_func[*pr](luma + offset[15], stride, 3);
	mb->left4x4coef &= 0xffff0000;
	*mb->top4x4coef &= 0xffff0000;
}

template <typename F0, typename F1, typename F2, typename F3, typename F4>
static inline int mb_intra4x4(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
				F0 Intra4x4PredMode,
				F1 IntraChromaPredMode,
				F2 CodedBlockPattern,
				F3 QpDelta,
				F4 ResidualBlock)
{
	int8_t pred4x4[16];
	uint32_t intra_chroma_pred_mode;
	int stride;
	uint32_t cbp;
	int avail_intra;

	avail_intra = avail;
	if (mb->is_constrained_intra) {
		avail_intra &= ~((MB_IPCM < mb->top4x4inter[1].type) * 4 | ((MB_IPCM < mb->top4x4inter->type) * 2) | (MB_IPCM < mb->left4x4inter->type));
	}
	fill_dc_if_unavailable(mb, avail_intra);
	mb_pred_intra4x4(mb, st, avail_intra, pred4x4, Intra4x4PredMode);
	VC_CHECK;
	intra_chroma_pred_mode = IntraChromaPredMode(mb, st, avail_intra);
	stride = mb->max_x * 16;
	intra_chroma_pred[intra_chroma_pred_mode](mb->chroma, stride, avail_intra);
	cbp = CodedBlockPattern(mb, st, avail);
	if (cbp) {
		int32_t qp_delta = QpDelta(mb, st, avail);
		if (qp_delta) {
			set_qp(mb, mb->qp + qp_delta);
		}
	} else {
		mb->prev_qp_delta = 0;
	}
	if (cbp & 15) {
		luma_intra4x4_with_residual(mb, st, cbp, avail, avail_intra, pred4x4, stride, ResidualBlock);
	} else {
		luma_intra4x4_pred(mb, avail, pred4x4, stride);
	}
	store_strength_intra(mb);
	mb_intra_save_info(mb, 0);
	mb->cbp = cbp;
	VC_CHECK;
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

template <typename F>
static int mb_pred_intra8x8(h264d_mb_current *mb, dec_bits *st, int avail, int8_t *pred8x8, F I8x8PredMode) {
	uint32_t left = mb->left4x4pred;
	uint32_t top = *mb->top4x4pred;
	h264d_cabac_t *cb = mb->cabac;
	pred8x8[0] = I8x8PredMode(avail & 2 ? UNPACK(left, 0) : 2, avail & 1 ? UNPACK(top, 0) : 2, st, cb);
	pred8x8[1] = I8x8PredMode(avail & 2 ? pred8x8[0] : 2, UNPACK(top, 2), st, cb);
	pred8x8[2] = I8x8PredMode(UNPACK(left, 2), avail & 1 ? pred8x8[0] : 2, st, cb);
	pred8x8[3] = I8x8PredMode(pred8x8[2], pred8x8[1], st, cb);
	mb->left4x4pred = pred8x8[1] * 0x11 + pred8x8[3] * 0x1100;
	*mb->top4x4pred = pred8x8[2] * 0x11 + pred8x8[3] * 0x1100;
	return 0;
}

static int intra8x8pred_horiz(uint8_t *dst, int stride, int avail)
{
	const uint8_t *src = dst - 1;
	int s0, s1, s2;
	uint32_t dc;
	if (!(avail & 1)) {
		return -1;
	}
	s0 = src[0];
	if (avail & 8) {
		s2 = src[-stride];
	} else {
		s2 = s0;
	}
	src += stride;
	s1 = src[0];
	int i = 7;
	do {
		src += stride;
		dc = ((s2 + s0 * 2 + s1 + 2) >> 2) * 0x01010101;
		((uint32_t *)dst)[0] = dc;
		((uint32_t *)dst)[1] = dc;
		dst += stride;
		s2 = s0;
		s0 = s1;
		s1 = src[0];
	} while (--i);
	dc = ((s2 + s0 * 3 + 2) >> 2) * 0x01010101;
	((uint32_t *)dst)[0] = dc;
	((uint32_t *)dst)[1] = dc;
	return 0;
}

static int intra8x8pred_vert(uint8_t *dst, int stride, int avail)
{
	uint8_t *src;
	int s0, s1, s2;

	if (!(avail & 2)) {
		return -1;
	}
	src = dst - stride;
	s0 = *src++;
	if (avail & 8) {
		s2 = src[-2];
	} else {
		s2 = s0;
	}
	s1 = *src++;
	dst[0] = FIR3(s2, s0, s1);
	s2 = *src++;
	dst[1] = FIR3(s0, s1, s2);
	s0 = *src++;
	dst[2] = FIR3(s1, s2, s0);
	s1 = *src++;
	dst[3] = FIR3(s2, s0, s1);
	s2 = *src++;
	dst[4] = FIR3(s0, s1, s2);
	s0 = *src++;
	dst[5] = FIR3(s1, s2, s0);
	s1 = *src++;
	dst[6] = FIR3(s2, s0, s1);
	s2 = (avail & 4) ? *src : s1;
	dst[7] = FIR3(s0, s1, s2);
	uint64_t d0 = ((uint64_t *)dst)[0];
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	dst += stride;
	((uint64_t *)dst)[0] = d0;
	return 0;
}

static int sum8x8left(uint8_t *dst, int stride, int avail)
{
	const uint8_t *src = dst - 1;
	int s0, s1, s2;
	int sum;
	s1 = src[0];
	if (avail & 8) {
		s0 = src[-stride];
	} else {
		s0 = s1;
	}
	src += stride;
	s2 = src[0];
	src += stride;
	sum = FIR3(s0, s1, s2);
	s0 = src[0];
	src += stride;
	sum += FIR3(s1, s2, s0);
	s1 = src[0];
	src += stride;
	sum += FIR3(s2, s0, s1);
	s2 = src[0];
	src += stride;
	sum += FIR3(s0, s1, s2);
	s0 = src[0];
	src += stride;
	sum += FIR3(s1, s2, s0);
	s1 = src[0];
	src += stride;
	sum += FIR3(s2, s0, s1);
	s2 = src[0];
	return sum + FIR3(s0, s1, s2) + FIR3(s1, s2, s2);
}

static int sum8x8top(uint8_t *dst, int stride, int avail)
{
	const uint8_t *src = dst - stride;
	int s0, s1, s2;
	int sum;
	s1 = *src++;
	if (avail & 8) {
		s0 = src[-2];
	} else {
		s0 = s1;
	}
	s2 = *src++;
	sum = FIR3(s0, s1, s2);
	s0 = *src++;
	sum += FIR3(s1, s2, s0);
	s1 = *src++;
	sum += FIR3(s2, s0, s1);
	s2 = *src++;
	sum += FIR3(s0, s1, s2);
	s0 = *src++;
	sum += FIR3(s1, s2, s0);
	s1 = *src++;
	sum += FIR3(s2, s0, s1);
	s2 = *src++;
	sum += FIR3(s0, s1, s2);
	s0 = (avail & 4) ? *src : s2;
	return sum + FIR3(s1, s2, s0);
}

/**Intra 8x8 prediction DC.
 */
static int intra8x8pred_dc(uint8_t *dst, int stride, int avail)
{
	uint32_t dc;
	if (avail & 1) {
		if (avail & 2) {
			dc = (sum8x8left(dst, stride, avail) + sum8x8top(dst, stride, avail) + 8) >> 4;
		} else {
			dc = (sum8x8left(dst, stride, avail) + 4) >> 3;
		}
	} else if (avail & 2) {
		dc = (sum8x8top(dst, stride, avail) + 4) >> 3;
	} else {
		dc = 0x80;
	}
	dc = dc * 0x01010101;
	int i = 8;
	do {
		((uint32_t *)dst)[0] = dc;
		((uint32_t *)dst)[1] = dc;
		dst += stride;
	} while (--i);
	return 0;
}

template <typename F>
static inline void top8x8line(const uint8_t *src, uint32_t *dst, int avail, F Latter)
{
	uint32_t s0, s1, s2;
	s1 = *src++;
	if (avail & 8) {
		s0 = src[-2];
	} else {
		s0 = s1;
	}
	int i = 8 / 2 - 1;
	do {
		s2 = *src++;
		dst[0] = (s0 + s1 * 2 + s2 + 2) >> 2;
		s0 = *src++;
		dst[1] = (s1 + s2 * 2 + s0 + 2) >> 2;
		s1 = s0;
		s0 = s2;
		dst += 2;
	} while (--i);
	s2 = *src++;
	dst[0] = (s0 + s1 * 2 + s2 + 2) >> 2;
	Latter(dst, src, avail, s1, s2);
}

struct top8x8line_latter0 {
	void operator()(uint32_t *dst, const uint8_t *src, int avail, uint32_t s1, uint32_t s2) const {}
};

struct top8x8line_latter1 {
	void operator()(uint32_t *dst, const uint8_t *src, int avail, uint32_t s1, uint32_t s2) const {
		uint32_t s0 = (avail & 4) ? *src : s2;
		dst[1] = (s1 + s2 * 2 + s0 + 2) >> 2;
	}
};

struct top8x8line_latter8 {
	void operator()(uint32_t *dst, const uint8_t *src, int avail, uint32_t s1, uint32_t s2) const {
		uint32_t s0;
		if (avail & 4) {
			dst += 1;
			int x = 8 / 2;
			do {
				s0 = *src++;
				dst[0] = FIR3(s1, s2, s0);
				s1 = *src++;
				dst[1] = FIR3(s2, s0, s1);
				s2 = s1;
				s1 = s0;
				dst += 2;
			} while (--x);
			dst[0] = FIR3(s1, s2, s2);
		} else {
			dst[1] = (s1 + s2 * 3 + 2) >> 2;
			dst += 2;
			for (int x = 0; x < 8; ++x) {
				dst[x] = s2;
			}
		}
	}
};

static inline void left8x8line(const uint8_t *src, uint32_t *dst, int stride, int avail)
{
	uint32_t s0, s1, s2;
	s1 = *src;
	if (avail & 8) {
		s0 = src[-stride];
	} else {
		s0 = s1;
	}
	src += stride;
	int i = 8 / 2 - 1;
	do {
		s2 = *src;
		src += stride;
		dst[0] = (s0 + s1 * 2 + s2 + 2) >> 2;
		s0 = *src;
		src += stride;
		dst[1] = (s1 + s2 * 2 + s0 + 2) >> 2;
		s1 = s0;
		s0 = s2;
		dst += 2;
	} while (--i);
	s2 = *src;
	dst[0] = (s0 + s1 * 2 + s2 + 2) >> 2;
	dst[1] = (s1 + s2 * 3 + 2) >> 2;
}

#ifdef WORDS_BIGENDIAN
#define SHIFT8LEFT(l, r, f) (l = ((l) << 8) | ((r) >> 24), r = ((r) << 8) | (f))
#else
#define SHIFT8LEFT(l, r, f) (l = ((r) << 24) | ((l) >> 8), r = ((f) << 24)| (r >> 8))
#endif

/**Intra 8x8 prediction Diagonal Down Left.
 */
static int intra8x8pred_ddl(uint8_t *dst, int stride, int avail)
{
	if ((avail & 2) == 0) {
		return -1;
	}
	uint32_t tmp[16];
	top8x8line(dst - stride, &tmp[0], avail, top8x8line_latter8());
	const uint32_t *src = tmp;
	uint32_t t0 = *src++;
	uint32_t t1 = *src++;
	uint32_t t2;
	for (int x = 0; x < 8; ++x) {
		t2 = *src++;
		dst[x] = FIR3(t0, t1, t2);
		t0 = t1;
		t1 = t2;
	}
	uint32_t d0 = ((uint32_t *)dst)[0];
	uint32_t d1 = ((uint32_t *)dst)[1];
	int y = 6;
	do {
		t2 = *src++;
		dst += stride;
		SHIFT8LEFT(d0, d1, FIR3(t0, t1, t2));
		((uint32_t *)dst)[0] = d0;
		((uint32_t *)dst)[1] = d1;
		t0 = t1;
		t1 = t2;
	} while (--y);
	dst += stride;
	SHIFT8LEFT(d0, d1, FIR3(t0, t1, t1));
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	return 0;
}

#ifdef WORDS_BIGENDIAN
#define SHIFT8RIGHT(l, r, f) (r = ((l) << 24) | ((r) >> 8), l = ((f) << 24) | ((l) >> 8))
#else
#define SHIFT8RIGHT(l, r, f) (r = ((r) << 8) | ((l) >> 24), l = ((l) << 8) | (f))
#endif

/**Intra 8x8 prediction Diagonal Down Right.
 */
static int intra8x8pred_ddr(uint8_t *dst, int stride, int avail)
{
	if ((avail & 3) != 3) {
		return -1;
	}
	uint32_t tmp[1 + 8 + 8];
	uint32_t t0, t1, t2;
	top8x8line(dst - stride, &tmp[1], avail, top8x8line_latter1());
	left8x8line(dst - 1, &tmp[1 + 8], stride, avail);
	tmp[0] = t0 = (dst[-1] + dst[-stride - 1] * 2 + dst[-stride] + 2) >> 2; // (-1, -1)
	const uint32_t *src = tmp + 1;
	t1 = *src++;
	dst[0] = FIR3(t1, t0, tmp[1 + 8]);
	for (int x = 1; x < 8; ++x) {
		t2 = *src++;
		dst[x] = FIR3(t0, t1, t2);
		t0 = t1;
		t1 = t2;
	}
	uint32_t d0 = ((uint32_t *)dst)[0];
	uint32_t d1 = ((uint32_t *)dst)[1];
	t0 = tmp[0];
	t1 = *src++;
	int y = 7;
	do {
		t2 = *src++;
		dst += stride;
		SHIFT8RIGHT(d0, d1, FIR3(t0, t1, t2));
		((uint32_t *)dst)[0] = d0;
		((uint32_t *)dst)[1] = d1;
		t0 = t1;
		t1 = t2;
	} while (--y);
	return 0;
}

/** Intra 8x8 prediction Vertical Right.
 */
static int intra8x8pred_vr(uint8_t *dst, int stride, int avail)
{
/*
  zVR:
   0,  2,  4,  6,  8, 10, 12, 14,
  -1,  1,  3,  5,  7,  9, 11, 13,
  -2,  0,  2,  4,  6,  8, 10, 12,
  -3, -1,  1,  3,  5,  7,  9, 11,
  -4, -2,  0,  2,  4,  6,  8, 10,
  -5, -3, -1,  1,  3,  5,  7,  9,
  -6, -4, -2,  0,  2,  4,  6,  8,
  -7, -5, -3, -1,  1,  3,  5,  7,
*/
	if ((avail & 11) != 11) {
		return -1;
	}
	uint32_t tmp[1 + 8 + 8];
	uint32_t t0, t1, t2;
	top8x8line(dst - stride, &tmp[1], avail, top8x8line_latter1());
	left8x8line(dst - 1, &tmp[1 + 8], stride, avail);
	tmp[0] = t0 = (dst[-1] + dst[-stride - 1] * 2 + dst[-stride] + 2) >> 2; // (-1, -1)
	const uint32_t *src = tmp + 1;
	uint8_t *dst2 = dst + stride;
	t1 = *src++;
	dst[0] = FIR2(t0, t1);
	dst2[0] = FIR3(t1, t0, tmp[1 + 8]);
	for (int x = 1; x < 8; ++x) {
		t2 = *src++;
		dst[x] = FIR2(t1, t2);
		dst2[x] = FIR3(t0, t1, t2);
		t0 = t1;
		t1 = t2;
	}
	uint32_t d0 = ((const uint32_t *)dst)[0];
	uint32_t d1 = ((const uint32_t *)dst)[1];
	uint32_t d2 = ((const uint32_t *)dst2)[0];
	uint32_t d3 = ((const uint32_t *)dst2)[1];
	t0 = tmp[0];
	t1 = *src++;
	int y = 6 / 2;
	stride = stride * 2;
	do {
		dst += stride;
		dst2 += stride;
		t2 = *src++;
		SHIFT8RIGHT(d0, d1, FIR3(t0, t1, t2));
		t0 = *src++;
		SHIFT8RIGHT(d2, d3, FIR3(t1, t2, t0));
		((uint32_t *)dst)[0] = d0;
		((uint32_t *)dst)[1] = d1;
		((uint32_t *)dst2)[0] = d2;
		((uint32_t *)dst2)[1] = d3;
		t1 = t0;
		t0 = t2;
	} while (--y);
	return 0;
}

#ifdef WORDS_BIGENDIAN
#define SHIFT16RIGHT(l, r, f0, f1) (r = ((l) << 16) | ((r) >> 16), l = ((f1) << 24) | ((f0) << 16) | ((l) >> 16))
#else
#define SHIFT16RIGHT(l, r, f0, f1) (r = ((r) << 16) | ((l) >> 16), l = ((l) << 16) | ((f0) << 8) | (f1))
#endif

/** Intra 8x8 prediction Horizontal Down.
 */
static int intra8x8pred_hd(uint8_t *dst, int stride, int avail)
{
/*
  zHD:
  0, -1, -2, -3, -4, -5, -6, -7
  2,  1,  0, -1, -2, -3, -4, -5
  4,  3,  2,  1,  0, -1, -2, -3
  6,  5,  4,  3,  2,  1,  0, -1
  8,  7,  6,  5,  4,  3,  2,  1
  10, 9,  8,  7,  6,  5,  4,  3
  12, 11, 10, 9,  8,  7,  6,  5,
  14, 13, 12, 11, 10, 9,  8,  7
 */
	uint32_t tmp[7 + 1 + 8];
	const uint32_t *src;
	uint32_t t0, t1, t2, d0, d1;
	if ((avail & 11) != 11) {
		return -1;
	}
	top8x8line(dst - stride, tmp, avail, top8x8line_latter0());
	left8x8line(dst - 1, &tmp[7 + 1], stride, avail);
	tmp[7] = t0 = (dst[-1] + dst[-stride - 1] * 2 + dst[-stride] + 2) >> 2; // (-1, -1)
	t2 = tmp[7 + 1]; // (-1, 0)
	src = tmp;
	t1 = *src++; // (0, -1)
	dst[0] = FIR2(t2, t0);
	dst[1] = FIR3(t2, t0, t1);
	t2 = *src++;
	dst[2] = FIR3(t0, t1, t2);
	t0 = *src++;
	dst[3] = FIR3(t1, t2, t0);
	t1 = *src++;
	dst[4] = FIR3(t2, t0, t1);
	t2 = *src++;
	dst[5] = FIR3(t0, t1, t2);
	t0 = *src++;
	dst[6] = FIR3(t1, t2, t0);
	t1 = *src++;
	dst[7] = FIR3(t2, t0, t1);
	d0 = ((uint32_t *)dst)[0];
	d1 = ((uint32_t *)dst)[1];
	t0 = *src++; // (-1, -1)
	t1 = *src++; // (-1, 0)
	int y = 8 / 2 - 1;
	do {
		t2 = *src++; // (-1, 1), (-1, 3), (-1, 5)
		dst += stride;
		SHIFT16RIGHT(d0, d1, FIR3(t0, t1, t2), FIR2(t1, t2));
		((uint32_t *)dst)[0] = d0;
		((uint32_t *)dst)[1] = d1;
		t0 = *src++; // (-1, 2), (-1, 4), (-1, 6)
		dst += stride;
		SHIFT16RIGHT(d0, d1, FIR3(t1, t2, t0), FIR2(t2, t0));
		((uint32_t *)dst)[0] = d0;
		((uint32_t *)dst)[1] = d1;
		t1 = t0;
		t0 = t2;
	} while (--y);
	t2 = *src; // (-1, 7)
	dst += stride;
	SHIFT16RIGHT(d0, d1, FIR3(t0, t1, t2), FIR2(t1, t2));
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;  // line 7
	return 0;
}

/** Intra 8x8 prediction Vertical Left.
 */
static int intra8x8pred_vl(uint8_t *dst, int stride, int avail)
{
	if ((avail & 2) == 0) {
		return -1;
	}
	uint32_t tmp[16];
	top8x8line(dst - stride, &tmp[0], avail, top8x8line_latter8());
	const uint32_t *src = tmp;
	uint8_t *dst2 = dst + stride;
	uint32_t t0 = *src++;
	uint32_t t1 = *src++;
	uint32_t t2;
	for (int x = 0; x < 8; ++x) {
		t2 = *src++;
		dst[x] = FIR2(t0, t1);
		dst2[x] = FIR3(t0, t1, t2);
		t0 = t1;
		t1 = t2;
	}
	uint32_t d0 = ((const uint32_t *)dst)[0];
	uint32_t d1 = ((const uint32_t *)dst)[1];
	uint32_t d2 = ((const uint32_t *)dst2)[0];
	uint32_t d3 = ((const uint32_t *)dst2)[1];
	stride = stride * 2;
	int y = 6 / 2;
	do {
		t2 = *src++;
		dst += stride;
		dst2 += stride;
		SHIFT8LEFT(d0, d1, FIR2(t0, t1));
		SHIFT8LEFT(d2, d3, FIR3(t0, t1, t2));
		((uint32_t *)dst)[0] = d0;
		((uint32_t *)dst)[1] = d1;
		((uint32_t *)dst2)[0] = d2;
		((uint32_t *)dst2)[1] = d3;
		t0 = t1;
		t1 = t2;
	} while (--y);
	return 0;
}

#ifdef WORDS_BIGENDIAN
#define SHIFT16LEFT(l, r, f0, f1) (l = ((l) << 16) | ((r) >> 16), r = ((r) << 16) | ((f0) <<8) | (f1))
#else
#define SHIFT16LEFT(l, r, f0, f1) (l = ((r) << 16) | ((l) >> 16), r = ((f1) << 24) | ((f0) << 16) | ((r) >> 16))
#endif

/** Intra 8x8 prediction Horizontal Up.
 */
static int intra8x8pred_hu(uint8_t *dst, int stride, int avail)
{
/*
  zHU:
   0,  1,  2,  3,  4,  5,  6,  7,
   2,  3,  4,  5,  6,  7,  8,  9,
   4,  5,  6,  7,  8,  9, 10, 11,
   6,  7,  8,  9, 10, 11, 12, 13,
   8,  9, 10, 11, 12, 13, 14, 15,
  10, 11, 12, 13, 14, 15, 16, 17,
  12, 13, 14, 15, 16, 17, 18, 19,
  14, 15, 16, 17, 18, 19, 20, 21
*/
	if ((avail & 1) == 0) {
		return -1;
	}
	uint32_t tmp[8];
	left8x8line(dst - 1, &tmp[0], stride, avail);
	const uint32_t *src = tmp;
	uint32_t t0 = *src++;
	uint32_t t1 = *src++;
	uint32_t t2 = *src++;
	dst[0] = FIR2(t0, t1);
	dst[1] = FIR3(t0, t1, t2);
	dst[2] = FIR2(t1, t2);
	t0 = *src++;
	dst[3] = FIR3(t1, t2, t0);
	dst[4] = FIR2(t2, t0);
	t1 = *src++;
	dst[5] = FIR3(t2, t0, t1);
	dst[6] = FIR2(t0, t1);
	t2 = *src++;
	dst[7] = FIR3(t0, t1, t2);
	uint32_t d0 = ((uint32_t *)dst)[0];
	uint32_t d1 = ((uint32_t *)dst)[1];
	t0 = *src++;
	dst += stride;
	SHIFT16LEFT(d0, d1, FIR2(t1, t2), FIR3(t1, t2, t0));
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	t1 = *src;
	dst += stride;
	SHIFT16LEFT(d0, d1, FIR2(t2, t0), FIR3(t2, t0, t1));
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	dst += stride;
	SHIFT16LEFT(d0, d1, FIR2(t0, t1), FIR3(t0, t1, t1));
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	dst += stride;
	SHIFT16LEFT(d0, d1, t1, t1);
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	dst += stride;
	SHIFT16LEFT(d0, d1, t1, t1);
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	dst += stride;
#ifdef WORDS_BIGENDIAN
	d0 = (d0 << 16) | (d1 >> 16);
#else
	d0 = (d1 << 16) | (d0 >> 16);
#endif
	((uint32_t *)dst)[0] = d0;
	((uint32_t *)dst)[1] = d1;
	dst += stride;
	((uint32_t *)dst)[0] = d1;
	((uint32_t *)dst)[1] = d1;
	return 0;
}

static int (* const intra8x8pred_func[9])(uint8_t *dst, int stride, int avail) = {
	intra8x8pred_vert,
	intra8x8pred_horiz,
	intra8x8pred_dc,
	intra8x8pred_ddl,
	intra8x8pred_ddr,
	intra8x8pred_vr,
	intra8x8pred_hd,
	intra8x8pred_vl,
	intra8x8pred_hu
};

// t0 = src[0] + src[4];
// t1 = src[5] - src[3] - src[7] - (src[7] >> 1);
// t2 = src[0] - src[4];
// t3 = src[1] + src[7] - src[3] - (src[3] >> 1);
// t4 = (src[2] >> 1) - src[6];
// t5 = src[5] + (src[5] >> 1) + src[7] - src[1];
// t6 = src[2] + (src[6] >> 1);
// t7 = src[3] + src[5] + src[1] + (src[1] >> 1);

/** Assumes that parameter t2 has initial value src[0] already. Other t0, t1, t3,... don't have initial value.
 */
#define ac8x8transform_interim(src, t0, t1, t2, t3, t4, t5, t6, t7) {\
	{\
		int s = src[4];\
		t0 = t2 + s;\
		t2 = t2 - s;\
	}\
	{\
		int s = src[6];\
		t6 = src[2];\
		t4 = (t6 >> 1) - s;\
		t6 = t6 + (s >> 1);\
	}\
	{\
		int s1 = src[1];\
		int s7 = src[7];\
		t3 = src[3];\
		t5 = src[5];\
		t1 = t5 - t3 - s7 - (s7 >> 1);\
		t7 = t3 + t5 + s1 + (s1 >> 1);\
		t3 = s1 + s7 - t3 - (t3 >> 1);\
		t5 = t5 + (t5 >> 1) + s7 - s1;\
	}\
	{\
		int s = t0;\
		t0 = t0 + t6;\
		t6 = s - t6;\
	}\
	{\
		int s = t2;\
		t2 = t2 + t4;\
		t4 = s - t4;\
	}\
	{\
		int s = t1;\
		t1 = t1 + (t7 >> 2);\
		t7 = t7 - (s >> 2);\
	}\
	{\
		int s = t3;\
		t3 = t3 + (t5 >> 2);\
		t5 = (s >> 2) - t5;\
	}\
}

static inline void ac8x8transform_horiz(int *dst, const int *src)
{
	int i = 8;
	int t2 = src[0] + 32;
	do {
		int t0, t1, t3, t4, t5, t6, t7;
		ac8x8transform_interim(src, t0, t1, t2, t3, t4, t5, t6, t7);
#if 1
		dst[0] = t0 + t7;
		dst[8] = t2 + t5;
		dst += 16;
		dst[0] = t4 + t3;
		dst[8] = t6 + t1;
		dst += 16;
		dst[0] = t6 - t1;
		dst[8] = t4 - t3;
		dst += 16;
		dst[0] = t2 - t5;
		dst[8] = t0 - t7;
		src += 8;
		dst = dst - 16 * 3 + 1;
#else
		dst[0] = t0 + t7;
		dst[8] = t2 + t5;
		dst[16] = t4 + t3;
		dst[24] = t6 + t1;
		dst[32] = t6 - t1;
		dst[40] = t4 - t3;
		dst[48] = t2 - t5;
		dst[56] = t0 - t7;
		src += 8;
		dst += 1;
#endif
		t2 = src[0];
	} while (--i);
}

static inline void ac8x8transform_vert(uint8_t *dst, const int *src, int stride)
{
	int i = 8;
	int t2 = src[0];
	do {
		int t0, t1, t3, t4, t5, t6, t7;
		ac8x8transform_interim(src, t0, t1, t2, t3, t4, t5, t6, t7);
		uint8_t *d = dst;
		int t;
		t = d[0] + ((t0 + t7) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t2 + t5) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t4 + t3) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t6 + t1) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t6 - t1) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t4 - t3) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t2 - t5) >> 6);
		d[0] = CLIP255C(t);
		d += stride;
		t = d[0] + ((t0 - t7) >> 6);
		d[0] = CLIP255C(t);
		src += 8;
		dst += 1;
		t2 = src[0];
	} while (--i);
}

/** Reconstruct 8x8 coefficients.
 */
static void ac8x8transform_acdc(uint8_t *dst, const int *coeff, int stride)
{
	int tmp[8 * 8];
	ac8x8transform_horiz(tmp, coeff);
	ac8x8transform_vert(dst, tmp, stride);
}

/** Reconstruct 8x8 coefficients.
 */
static void ac8x8transform(uint8_t *dst, const int *coeff, int stride, int coeff_num)
{
	int c0;
	if ((coeff_num == 1) && ((c0 = coeff[0]) != 0)) {
		acNxNtransform_dconly<8, 6, 0, cache_t>(dst, c0, stride);
	} else {
		ac8x8transform_acdc(dst, coeff, stride);
	}
}

template <typename F>
static inline void luma_intra8x8_with_residual(h264d_mb_current *mb, dec_bits *st, uint32_t cbp, int avail, int avail_intra, const int8_t *pr, int stride,
						    F ResidualBlock)
{
	int coeff[64];
	uint32_t top, left;
	int c0, c1, c2, c3;
	uint8_t *luma = mb->luma;
	const int *offset = mb->offset4x4;
	const int16_t *qmat = mb->qmaty8x8;

	intra8x8pred_func[*pr++](luma, stride, (avail_intra & ~4) | ((avail_intra & 2) * 2));
	if (cbp & 1) {
		c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 0) : -1, avail & 2 ? UNPACK(*mb->top4x4coef, 0) : -1, st, coeff, qmat, avail_intra, 0, 5, 0x3f);
		ac8x8transform(luma, coeff, stride, c0);
	} else {
		c0 = 0;
	}
	intra8x8pred_func[*pr++](luma + 8, stride, (avail_intra & ~8) | ((avail_intra & 2) * 4) | 1);
	if (cbp & 2) {
		c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 2) : -1, st, coeff, qmat, avail_intra, 4, 5, 0x3f);
		ac8x8transform(luma + 8, coeff, stride, c1);
		left = c1 * 0x11;
	} else {
		c1 = 0;
		left = 0;
	}
	intra8x8pred_func[*pr++](luma + offset[8], stride, 6 | ((avail_intra & 1) * 9));
	if (cbp & 4) {
		c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 2) : -1, c1, st, coeff, qmat, avail_intra, 8, 5, 0x3f);
		ac8x8transform(luma + offset[8], coeff, stride, c2);
		top = c2 * 0x11;
	} else {
		c2 = 0;
		top = 0;
	}
	intra8x8pred_func[*pr++](luma + offset[12], stride, 11);
	if (cbp & 8) {
		c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 12, 5, 0x3f);
		ac8x8transform(luma + offset[12], coeff, stride, c3);
		left |= c3 * 0x1100;
		top |= c3 * 0x1100;
	}
	mb->left4x4coef = (mb->left4x4coef & 0xffff0000) | left;
	*mb->top4x4coef = (*mb->top4x4coef & 0xffff0000) | top;
}

template <typename F0, typename F1, typename F2, typename F3, typename F4>
static inline int mb_intra8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
				F0 Intra8x8PredMode,
				F1 IntraChromaPredMode,
				F2 CodedBlockPattern,
				F3 QpDelta,
				F4 ResidualBlock)
{
	int8_t pred8x8[4];
	uint32_t intra_chroma_pred_mode;
	int stride;
	uint32_t cbp;
	int avail_intra;

	avail_intra = avail;
	if (mb->is_constrained_intra) {
		avail_intra &= ~((MB_IPCM < mb->top4x4inter[1].type) * 4 | ((MB_IPCM < mb->top4x4inter->type) * 2) | (MB_IPCM < mb->left4x4inter->type));
	}
	fill_dc_if_unavailable(mb, avail_intra);
	mb_pred_intra8x8(mb, st, avail_intra, pred8x8, Intra8x8PredMode);
	VC_CHECK;
	intra_chroma_pred_mode = IntraChromaPredMode(mb, st, avail_intra);
	stride = mb->max_x * 16;
	intra_chroma_pred[intra_chroma_pred_mode](mb->chroma, stride, avail_intra);
	cbp = CodedBlockPattern(mb, st, avail);
	if (cbp) {
		int32_t qp_delta = QpDelta(mb, st, avail);
		if (qp_delta) {
			set_qp(mb, mb->qp + qp_delta);
		}
	} else {
		mb->prev_qp_delta = 0;
	}
	luma_intra8x8_with_residual(mb, st, cbp, avail, avail_intra, pred8x8, stride, ResidualBlock);
	store_strength_intra8x8(mb);
	mb_intra_save_info(mb, 1);
	mb->cbp = cbp;
	VC_CHECK;
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

template <typename F0, typename F1, typename F2, typename F3, typename F4, typename F5>
static inline int mb_intraNxN(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
				F0 Transform8x8Flag,
				F1 IntraNxNPredMode,
				F2 IntraChromaPredMode,
				F3 CodedBlockPattern,
				F4 QpDelta,
				F5 ResidualBlock)
{
	if (Transform8x8Flag(mb, st, avail)) {
		return mb_intra8x8(mb, mbc, st, avail, IntraNxNPredMode, IntraChromaPredMode, CodedBlockPattern, QpDelta, ResidualBlock);
	} else {
		return mb_intra4x4(mb, mbc, st, avail, IntraNxNPredMode, IntraChromaPredMode, CodedBlockPattern, QpDelta, ResidualBlock);
	}
}

struct intra_chroma_pred_mode_cavlc {
	uint32_t operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		uint32_t pred_mode = ue_golomb(st);
		pred_mode = pred_mode <= 3 ? pred_mode : 0;
		mb->chroma_pred_mode = pred_mode;
		return pred_mode;
	}
};

struct cbp_intra_cavlc {
	uint32_t operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		return me_golomb(st, me_golomb_lut[0]);
	}
};

struct cbp_inter_cavlc {
	uint32_t operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		return me_golomb(st, me_golomb_lut[1]);
	}
};

struct qp_delta_cavlc {
	int operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		int delta = se_golomb(st);
		return (delta < -26) ? -26 : ((25 < delta) ? 25 : delta);
	}
};

static int mb_intra4x4_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intra4x4(mb, mbc, st, avail, intra4x4pred_mode_cavlc(), intra_chroma_pred_mode_cavlc(), cbp_intra_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
} 

static int mb_intraNxN_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intraNxN(mb, mbc, st, avail, transform_size_8x8_flag_cavlc(), intra4x4pred_mode_cavlc(), intra_chroma_pred_mode_cavlc(), cbp_intra_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
} 

static int mb_intra16x16pred_planer(uint8_t *dst, int stride, int avail)
{
	const uint8_t *src, *src2;
	int t0, p0, h, v;
	int y;
	
	src = dst - stride;
	p0 = src[15];
	src -= 1;
	t0 = p0 - src[0];
	h = t0;
	t0 = t0 + src[15] - src[1];
	h += t0;
	t0 = t0 + src[14] - src[2];
	h += t0;
	t0 = t0 + src[13] - src[3];
	h += t0;
	t0 = t0 + src[12] - src[4];
	h += t0;
	t0 = t0 + src[11] - src[5];
	h += t0;
	t0 = t0 + src[10] - src[6];
	h += t0;
	t0 = t0 + src[9] - src[7];
	h += t0;
	h = ((h * 5) + 32) >> 6;

	src = dst - 1;
	src2 = src + (stride * 15);
	src = src - stride;
	t0 = src2[0];
	p0 = (p0 + t0) * 16;
	t0 = t0 - src[0];
	v = t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	src2 -= stride;
	src += stride;
	t0 = t0 + src2[0] - src[0];
	v += t0;
	v = ((v * 5) + 32) >> 6;

	dst += 16 + (stride * 15);
	p0 = p0 + ((h + v) * 8) + 16;
	y = 16;
	stride -= 16;
	do {
		int x = 16;
		t0 = p0;
		do {
			int s = t0 >> 5;
			*--dst = CLIP255C(s);
			t0 -= h;
		} while (--x);
		p0 -= v;
		dst -= stride;
	} while (--y);
	return 0;
}

/** Inverse 16x16 luma DC transformation.
 * Output is 4x4 block scan order.
 */
static void intra16x16_dc_transform(const int *src, int *dst)
{
	int c0, c1, c2, c3;
	int t0, t1;

	c0 = src[0] + src[1] + src[2] + src[3];
	c1 = src[4] + src[5] + src[6] + src[7];
	c2 = src[8] + src[9] + src[10] + src[11];
	c3 = src[12] + src[13] + src[14] + src[15];
	t0 = c0 + c1;
	t1 = c2 + c3;
	dst[0] = (t0 + t1 + 2) >> 2;
	dst[2] = (t0 - t1 + 2) >> 2;
	t0 = c0 - c1;
	t1 = c2 - c3;
	dst[8] = (t0 - t1 + 2) >> 2;
	dst[10] = (t0 + t1 + 2) >> 2;

	c0 = src[0] + src[1] - src[2] - src[3];
	c1 = src[4] + src[5] - src[6] - src[7];
	c2 = src[8] + src[9] - src[10] - src[11];
	c3 = src[12] + src[13] - src[14] - src[15];
	t0 = c0 + c1;
	t1 = c2 + c3;
	dst[1] = (t0 + t1 + 2) >> 2;
	dst[3] = (t0 - t1 + 2) >> 2;
	t0 = c0 - c1;
	t1 = c2 - c3;
	dst[9] = (t0 - t1 + 2) >> 2;
	dst[11] = (t0 + t1 + 2) >> 2;

	c0 = src[0] - src[1] - src[2] + src[3];
	c1 = src[4] - src[5] - src[6] + src[7];
	c2 = src[8] - src[9] - src[10] + src[11];
	c3 = src[12] - src[13] - src[14] + src[15];
	t0 = c0 + c1;
	t1 = c2 + c3;
	dst[4] = (t0 + t1 + 2) >> 2;
	dst[6] = (t0 - t1 + 2) >> 2;
	t0 = c0 - c1;
	t1 = c2 - c3;
	dst[12] = (t0 - t1 + 2) >> 2;
	dst[14] = (t0 + t1 + 2) >> 2;

	c0 = src[0] - src[1] + src[2] - src[3];
	c1 = src[4] - src[5] + src[6] - src[7];
	c2 = src[8] - src[9] + src[10] - src[11];
	c3 = src[12] - src[13] + src[14] - src[15];
	t0 = c0 + c1;
	t1 = c2 + c3;
	dst[5] = (t0 + t1 + 2) >> 2;
	dst[7] = (t0 - t1 + 2) >> 2;
	t0 = c0 - c1;
	t1 = c2 - c3;
	dst[13] = (t0 - t1 + 2) >> 2;
	dst[15] = (t0 + t1 + 2) >> 2;
}

static inline void ac4x4transform_maybe(uint8_t *dst, const int *coeff, int stride, int num_coeff)
{
	if (num_coeff) {
		ac4x4transform_acdc_luma(dst, coeff, stride);
	}
}

static inline void ac4x4transform(uint8_t *dst, int *coeff, int stride, int num_coeff, int dc)
{
	if (num_coeff) {
		coeff[0] = dc;
		ac4x4transform_acdc_luma(dst, coeff, stride);
	} else {
		acNxNtransform_dconly<4, 6, 0, uint32_t>(dst, dc, stride);
	}
}

/** Inverse 8x8 chroma DC transformation.
 * Output is 4x4 block scan order.
 */
static void intra_chroma_dc_transform(const int *src, int *dst)
{
	int c0, c1, c2, c3;
	int t0, t1;

	c0 = src[0];
	c1 = src[1];
	c2 = src[2];
	c3 = src[3];
	t0 = c0 + c1;
	t1 = c2 + c3;
	dst[0] = (t0 + t1) >> 1;
	dst[2] = (t0 - t1) >> 1;
	t0 = c0 - c1;
	t1 = c2 - c3;
	dst[1] = (t0 + t1) >> 1;
	dst[3] = (t0 - t1) >> 1;
}

template <typename F0, typename F1, typename F2>
static int mb_intra16x16_dconly(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
				F0 IntraChromaPredMode,
				F1 QpDelta,
				F2 ResidualBlock)
{
	int coeff[16];
	int dc[16];
	uint8_t *luma;
	int stride;
	int avail_intra;
	uint32_t intra_chroma_pred_mode;
	int32_t qp_delta;
	const int *offset;

	luma = mb->luma;
	stride = mb->max_x * 16;
	avail_intra = avail;
	if (mb->is_constrained_intra) {
		avail_intra &= ~((MB_IPCM < mb->top4x4inter[1].type) * 4 | ((MB_IPCM < mb->top4x4inter->type) * 2) | (MB_IPCM < mb->left4x4inter->type));
	}
	mbc->mb_pred(luma, stride, avail_intra);
	intra_chroma_pred_mode = IntraChromaPredMode(mb, st, avail_intra);
	intra_chroma_pred[intra_chroma_pred_mode](mb->chroma, stride, avail_intra);
	qp_delta = QpDelta(mb, st, avail);
	if (qp_delta) {
		set_qp(mb, mb->qp + qp_delta);
	}
	if (ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 0) : -1, avail & 2 ? UNPACK(*mb->top4x4coef, 0) : -1, st, coeff, mb->qmaty, avail_intra, 26, 0, 0)) {
		intra16x16_dc_transform(coeff, dc);
		offset = mb->offset4x4;
		for (int i = 0; i < 16; ++i) {
			acNxNtransform_dconly<4, 6, 0, uint32_t>(luma + *offset++, dc[i], stride);
		}
	}
	mb->left4x4coef &= 0xffff0000;
	*mb->top4x4coef &= 0xffff0000;
	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	store_strength_intra(mb);
	mb_intra_save_info(mb, 0);
	mb->cbp = mbc->cbp;
	return residual_chroma(mb, mbc->cbp, st, avail, ResidualBlock);
}

static int mb_intra16x16_dconly_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intra16x16_dconly(mb, mbc, st, avail, intra_chroma_pred_mode_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
}

template <typename F0, typename F1, typename F2>
static int mb_intra16x16_acdc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
				F0 IntraChromaPredMode,
				F1 QpDelta,
				F2 ResidualBlock)
{
	int dc[16];
	int ALIGN16VC coeff[16] __attribute__((aligned(16)));
	uint8_t *luma;
	int stride;
	int avail_intra;
	uint32_t intra_chroma_pred_mode;
	int32_t qp_delta;
	const int16_t *qmat;
	uint32_t top, left;
	int c0, c1, c2, c3, c4, c5;
	int na, nb;
	const int *offset;
	int *dcp;

	luma = mb->luma;
	stride = mb->max_x * 16;
	avail_intra = avail;
	if (mb->is_constrained_intra) {
		avail_intra &= ~((MB_IPCM < mb->top4x4inter[1].type) * 4 | ((MB_IPCM < mb->top4x4inter->type) * 2) | (MB_IPCM < mb->left4x4inter->type));
	}
	mbc->mb_pred(luma, stride, avail_intra);
	intra_chroma_pred_mode = IntraChromaPredMode(mb, st, avail_intra);
	intra_chroma_pred[intra_chroma_pred_mode](mb->chroma, stride, avail_intra);
	qp_delta = QpDelta(mb, st, avail);
	if (qp_delta) {
		set_qp(mb, mb->qp + qp_delta);
	}

	na = avail & 1 ? UNPACK(mb->left4x4coef, 0) : -1;
	nb = avail & 2 ? UNPACK(*mb->top4x4coef, 0) : -1;
	qmat = mb->qmaty;
	if (ResidualBlock(mb, na, nb, st, coeff, qmat, avail_intra, 26, 0, 0)) {
		intra16x16_dc_transform(coeff, dc);
	} else {
		memset(dc, 0, sizeof(dc));
	}
	offset = mb->offset4x4;
	dcp = dc;
	c0 = ResidualBlock(mb, na, nb, st, coeff, qmat, avail_intra, 0, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c0, *dcp++);
	c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 1) : -1, st, coeff, qmat, avail_intra, 1, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c1, *dcp++);
	c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 1) : -1, c0, st, coeff, qmat, avail_intra, 2, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c2, *dcp++);
	c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 3, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c3, *dcp++);

	c0 = ResidualBlock(mb, c1, avail & 2 ? UNPACK(*mb->top4x4coef, 2) : -1, st, coeff, qmat, avail_intra, 4, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c0, *dcp++);
	c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 3) : -1, st, coeff, qmat, avail_intra, 5, 1, 0x1f);
	left = mb->left4x4coef & 0xffff0000;
	left = PACK(left, c1, 0);
	ac4x4transform(luma + *offset++, coeff, stride, c1, *dcp++);
	c4 = ResidualBlock(mb, c3, c0, st, coeff, qmat, avail_intra, 6, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c4, *dcp++);
	c5 = ResidualBlock(mb, c4, c1, st, coeff, qmat, avail_intra, 7, 1, 0x1f);
	left = PACK(left, c5, 1);
	ac4x4transform(luma + *offset++, coeff, stride, c5, *dcp++);

	c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 2) : -1, c2, st, coeff, qmat, avail_intra, 8, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c0, *dcp++);
	c1 = ResidualBlock(mb, c0, c3, st, coeff, qmat, avail_intra, 9, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c1, *dcp++);
	c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 3) : -1, c0, st, coeff, qmat, avail_intra, 10, 1, 0x1f);
	top = *mb->top4x4coef & 0xffff0000;
	top = PACK(top, c2, 0);
	ac4x4transform(luma + *offset++, coeff, stride, c2, *dcp++);
	c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 11, 1, 0x1f);
	top = PACK(top, c3, 1);
	ac4x4transform(luma + *offset++, coeff, stride, c3, *dcp++);

	c0 = ResidualBlock(mb, c1, c4, st, coeff, qmat, avail_intra, 12, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c0, *dcp++);
	c1 = ResidualBlock(mb, c0, c5, st, coeff, qmat, avail_intra, 13, 1, 0x1f);
	left = PACK(left, c1, 2);
	ac4x4transform(luma + *offset++, coeff, stride, c1, *dcp++);
	c2 = ResidualBlock(mb, c3, c0, st, coeff, qmat, avail_intra, 14, 1, 0x1f);
	top = PACK(top, c2, 2);
	ac4x4transform(luma + *offset++, coeff, stride, c2, *dcp++);
	c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail_intra, 15, 1, 0x1f);
	ac4x4transform(luma + *offset++, coeff, stride, c3, *dcp++);

	mb->left4x4coef = PACK(left, c3, 3);
	*mb->top4x4coef = PACK(top, c3, 3);
	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	store_strength_intra(mb);
	mb_intra_save_info(mb, 0);
	mb->cbp = mbc->cbp;
	return residual_chroma(mb, mbc->cbp, st, avail, ResidualBlock);
}

static int mb_intra16x16_acdc_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intra16x16_acdc(mb, mbc, st, avail, intra_chroma_pred_mode_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
}

/** Sum of top of 4x4 block, NV12 chroma part.
 */
static inline uint32_t sum_top_chroma(const uint8_t *src, int stride) {
	src -= stride;
	return src[0] + src[2] + src[4] + src[6];
}

static inline void fill_4x4_chroma(uint8_t *dst, uint32_t dc, int stride)
{
	int y;
	dc = dc * 0x00010001U;
#ifdef WORDS_BIGENDIAN
	dc = bswap32(dc);
#endif
	y = 4;
	do {
		*(uint32_t *)dst = dc;
		*((uint32_t *)dst + 1) = dc;
		dst += stride;
	} while (--y);
}

static int mb_intra_chroma_pred_dc(uint8_t *dst, int stride, int avail)
{
	uint32_t dc0, dc1, dc2, dc3;

	if (avail & 1) {
		if (avail & 2) {
			uint32_t left0, left1, top0, top1;

			left0 = sum_left<4>(dst - 1, stride);
			left1 = sum_left<4>(dst, stride);
			top0 = sum_top_chroma(dst, stride);
			top1 = sum_top_chroma(dst + 1, stride);
			dc0 = ((left0 + top0 + 4) >> 3) | (((left1 + top1 + 4) >> 3) << 8);

			top0 = sum_top_chroma(dst + 8, stride);
			top1 = sum_top_chroma(dst + 9, stride);
			dc1 = ((top0 + 2) >> 2) | (((top1 + 2) >> 2) << 8);

			left0 = sum_left<4>(dst + 4 * stride - 1, stride);
			left1 = sum_left<4>(dst + 4 * stride, stride);
			dc2 = ((left0 + 2) >> 2) | (((left1 + 2) >> 2) << 8);
			dc3 = ((left0 + top0 + 4) >> 3) | (((left1 + top1 + 4) >> 3) << 8);
		} else {
			dc1 = dc0 = (((sum_left<4>(dst, stride) + 2) >> 2) << 8) | ((sum_left<4>(dst - 1, stride) + 2) >> 2);
			dc3 = dc2 = (((sum_left<4>(dst + 4 * stride, stride) + 2) >> 2) << 8) | ((sum_left<4>(dst + 4 * stride - 1, stride) + 2) >> 2);
		}
	} else if (avail & 2) {
		int l0, l1, t0, t1;
		l0 = sum_top_chroma(dst + 1, stride);
		l1 = sum_top_chroma(dst + 0, stride);
		dc2 = dc0 = (((l0 + 2) >> 2) << 8) | ((l1 + 2) >> 2);
		t0 = sum_top_chroma(dst + 9, stride);
		t1 = sum_top_chroma(dst + 8, stride);
		dc3 = dc1 = (((t0 + 2) >> 2) << 8) | ((t1 + 2) >> 2);
	} else {
		dc0 = dc1 = dc2 = dc3 = 0x8080;
	}
	fill_4x4_chroma(dst, dc0, stride);
	fill_4x4_chroma(dst + 8, dc1, stride);
	fill_4x4_chroma(dst + 4 * stride, dc2, stride);
	fill_4x4_chroma(dst + 4 * (stride + 2), dc3, stride);
	return 0;
}

static int mb_intra_chroma_pred_horiz(uint8_t *dst, int stride, int avail)
{
	int i;

	if (!(avail & 1)) {
		return -1;
	}
	i = 8;
	do {
		uint32_t t0 = *((uint16_t *)dst - 1) * 0x00010001U;
		*(uint32_t *)dst = t0;
		*((uint32_t *)dst + 1) = t0;
		*((uint32_t *)dst + 2) = t0;
		*((uint32_t *)dst + 3) = t0;
		dst = dst + stride;
	} while (--i);
	return 0;
}

static int mb_intra_chroma_pred_planer(uint8_t *dst, int stride, int avail)
{
	const uint8_t *src, *src2;
	int a0, a1, h0, h1, v0, v1;
	int y;

	src = dst - stride + 14;
	a0 = src[0];
	a1 = src[1];
	src = src - 16;
	h0 = ((int)src[10] - src[6]) + ((int)src[12] - src[4]) * 2 + ((int)src[14] - src[2]) * 3 + (a0 - src[0]) * 4;
	h1 = ((int)src[11] - src[7]) + ((int)src[13] - src[5]) * 2 + ((int)src[15] - src[3]) * 3 + (a1 - src[1]) * 4;
	h0 = (h0 * 17 + 16) >> 5;
	h1 = (h1 * 17 + 16) >> 5;

	src = dst + (stride * 7) - 2;
	a0 = (a0 + src[0]) * 16;
	a1 = (a1 + src[1]) * 16;

	src = dst + stride * 4 - 2;
	src2 = src - stride * 2;
	v0 = (int)src[0] - src2[0];
	v1 = (int)src[1] - src2[1];
	src += stride;
	src2 -= stride;
	v0 += ((int)src[0] - src2[0]) * 2;
	v1 += ((int)src[1] - src2[1]) * 2;
	src += stride;
	src2 -= stride;
	v0 += ((int)src[0] - src2[0]) * 3;
	v1 += ((int)src[1] - src2[1]) * 3;
	src += stride;
	src2 -= stride;
	v0 += ((int)src[0] - src2[0]) * 4;
	v1 += ((int)src[1] - src2[1]) * 4;
	v0 = (v0 * 17 + 16) >> 5;
	v1 = (v1 * 17 + 16) >> 5;

	a0 = a0 - ((h0 + v0) * 3) + 16;
	a1 = a1 - ((h1 + v1) * 3) + 16;
	y = 8;
	do {
		int at0 = a0;
		int at1 = a1;
		int x = 8;
		uint8_t *d = dst;
		do {
			int t0, t1;
			t0 = at0 >> 5;
			d[0] = CLIP255C(t0);
			at0 += h0;
			t1 = at1 >> 5;
			d[1] = CLIP255C(t1);
			at1 += h1;
			d += 2;
		} while (--x);
		a0 += v0;
		a1 += v1;
		dst += stride;
	} while (--y);
	return 0;
}

template <int STEP>
static inline void intrapcm_block(uint8_t *dst, int stride, dec_bits *st)
{
	int y = 16 / STEP;
	do {
		for (int x = 0; x < 16; x += (STEP * 2)) {
			dst[x] = get_bits(st, 8);
			dst[x + STEP] = get_bits(st, 8);
		}
		dst += stride;
	} while (--y);
}

static inline void intrapcm_luma(uint8_t *dst, int stride, dec_bits *st)
{
#ifdef WORDS_BIGENDIAN
	int y = 16;
	do {
		*(uint32_t *)dst = get_bits(st, 32);
		*(uint32_t *)(dst + 4) = get_bits(st, 32);
		*(uint32_t *)(dst + 8) = get_bits(st, 32);
		*(uint32_t *)(dst + 12) = get_bits(st, 32);
		dst += stride;
	} while (--y);
#else
	intrapcm_block<1>(dst, stride, st);
#endif
}

static int mb_intrapcm(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	int stride;

	stride = mb->max_x * 16;
	byte_align(st);
	intrapcm_luma(mb->luma, stride, st);
	intrapcm_block<2>(mb->chroma, stride, st);
	intrapcm_block<2>(mb->chroma + 1, stride, st);
	mb->left4x4coef = 0xffffffff;
	*mb->top4x4coef = 0xffffffff;
	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	mb->deblock_curr->qpy = 0;
	mb->deblock_curr->qpc[0] = mb->qp_chroma[0] - mb->qp;
	mb->deblock_curr->qpc[1] = mb->qp_chroma[1] - mb->qp;
	mb->deblock_curr->str4_horiz = 1;
	mb->deblock_curr->str4_vert = 1;
	mb->deblock_curr->str_horiz = 0xff00ff;
	mb->deblock_curr->str_vert = 0xff00ff;
	mb->prev_qp_delta = 0;
	mb->cbp = 0x3f;
	mb->cbf = 0x7ffffff;
	mb_intra_save_info(mb, 0);
	return 0;
}

static void copy_inter16x_align8(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	do {
		((uint64_t *)dst)[0] = ((uint64_t *)src)[0];
		((uint64_t *)dst)[1] = ((uint64_t *)src)[1];
		dst += stride;
		src += src_stride;
	} while (--height);
}

static void copy_inter8x_align8(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	do {
		((uint64_t *)dst)[0] = ((uint64_t *)src)[0];
		dst += stride;
		src += src_stride;
	} while (--height);
}

static void copy_inter16x_align4(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	do {
		((uint32_t *)dst)[0] = ((uint32_t *)src)[0];
		((uint32_t *)dst)[1] = ((uint32_t *)src)[1];
		((uint32_t *)dst)[2] = ((uint32_t *)src)[2];
		((uint32_t *)dst)[3] = ((uint32_t *)src)[3];
		dst += stride;
		src += src_stride;
	} while (--height);
}

static void copy_inter8x_align4(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	do {
		((uint32_t *)dst)[0] = ((uint32_t *)src)[0];
		((uint32_t *)dst)[1] = ((uint32_t *)src)[1];
		dst += stride;
		src += src_stride;
	} while (--height);
}

static void copy_inter4x_align4(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	do {
		((uint32_t *)dst)[0] = ((uint32_t *)src)[0];
		dst += stride;
		src += src_stride;
	} while (--height);
}

template<int WIDTH>
static void copy_inter_align2(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	do {
		const int16_t *s = (const int16_t *)src;
		for (int i = 0; i < WIDTH / 2; ++i) {
			((uint16_t *)dst)[i] = *s++;
		}
		dst += stride;
		src += src_stride;
	} while (--height);
}

template<int WIDTH>
static void copy_inter_align1(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride)
{
	src_stride -= WIDTH;
	do {
		for (int i = 0; i < WIDTH; ++i) {
			dst[i] = *src++;
		}
		dst += stride;
		src += src_stride;
	} while (--height);
}

static inline void copy_inter(const uint8_t *src, uint8_t *dst, int width, int height, int src_stride, int stride)
{
	static void (* const copy_func[8][3])(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride) = {
		{copy_inter4x_align4, copy_inter8x_align8, copy_inter16x_align8},
		{copy_inter_align1<4>, copy_inter_align1<8>, copy_inter_align1<16>},
		{copy_inter_align2<4>, copy_inter_align2<8>, copy_inter_align2<16>},
		{copy_inter_align1<4>, copy_inter_align1<8>, copy_inter_align1<16>},
		{copy_inter4x_align4, copy_inter8x_align4, copy_inter16x_align4},
		{copy_inter_align1<4>, copy_inter_align1<8>, copy_inter_align1<16>},
		{copy_inter_align2<4>, copy_inter_align2<8>, copy_inter_align2<16>},
		{copy_inter_align1<4>, copy_inter_align1<8>, copy_inter_align1<16>},
	};
	copy_func[(intptr_t)src & 3][(unsigned)width >> 3](src, dst, height, src_stride, stride);
}

static inline int inter_pred_mvoffset_luma(int mvint_x, int mvint_y, int stride)
{
	return mvint_y * stride + mvint_x;
}

static inline void filter_chroma_horiz(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int frac, int src_stride, int dst_stride)
{
	int c1 = frac * 8;
	int c0 = 64 - c1;
	int width = size.v[0];
	int height = size.v[1] >> 1;
	src_stride = src_stride - width - 2;
	dst_stride = dst_stride - width;
	width >>= 1;
	do {
		int x = width;
		int s0 = *src++;
		int s1 = *src++;
		do {
			int s2 = *src++;
			int s3 = *src++;
			dst[0] = (s2 * c1 + s0 * c0 + 32) >> 6;
			dst[1] = (s3 * c1 + s1 * c0 + 32) >> 6;
			s0 = s2;
			s1 = s3;
			dst += 2;
		} while (--x);
		src += src_stride;
		dst += dst_stride;
	} while (--height);
}

static inline void filter_chroma_vert(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int frac, int src_stride, int dst_stride)
{
	int c1 = frac * 8;
	int c0 = 64 - c1;
	int width = size.v[0] >> 1;
	int height = size.v[1] >> 1;
	do {
		const uint8_t *s = src;
		uint8_t *d = dst;
		int t0 = (s[0] << 16) | s[1];
		s += src_stride;
		int y = height;
		do {
			int t1 = (s[0] << 16) | s[1];
			t0 = (t0 * c0 + t1 * c1 + 0x00200020) >> 6;
			d[0] = t0 >> 16;
			d[1] = t0;
			t0 = t1;
			s += src_stride;
			d += dst_stride;
		} while (--y);
		src += 2;
		dst += 2;
	} while (--width);
}

static inline void filter_chroma_vert_horiz(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int fracx, int fracy, int src_stride, int stride)
{
	int c1 = fracx * 8;
	int c2 = fracy * 8;
	int c3 = fracx * fracy;
	int width = size.v[0];
	int height = size.v[1] >> 1;
#ifdef X86ASM
	__m128i m0 = _mm_cvtsi32_si128(64 - c1 - c2 + c3);
	__m128i m1 = _mm_cvtsi32_si128(c1 - c3);
	__m128i m2 = _mm_cvtsi32_si128(c2 - c3);
	__m128i m3 = _mm_cvtsi32_si128(c3);
	__m128i rnd = _mm_cvtsi32_si128(0x00200020);
	__m128i zero = _mm_setzero_si128();
	m0 = _mm_shufflelo_epi16(m0, 0);
	m1 = _mm_shufflelo_epi16(m1, 0);
	m2 = _mm_shufflelo_epi16(m2, 0);
	m3 = _mm_shufflelo_epi16(m3, 0);
	rnd = _mm_shuffle_epi32(rnd, 0);
	m0 = _mm_shuffle_epi32(m0, 0);
	m1 = _mm_shuffle_epi32(m1, 0);
	m2 = _mm_shuffle_epi32(m2, 0);
	m3 = _mm_shuffle_epi32(m3, 0);
	__m128i d0 = _mm_loadu_si128((__m128i const *)&src[0]);
	__m128i d1 = _mm_loadu_si128((__m128i const *)&src[2]);
	if (width == 16) {
		__m128i d0h = _mm_unpackhi_epi8(d0, zero);
		__m128i d1h = _mm_unpackhi_epi8(d1, zero);
		d0 = _mm_unpacklo_epi8(d0, zero);
		d1 = _mm_unpacklo_epi8(d1, zero);
		do {
			src += src_stride;
			__m128i d2 = _mm_loadu_si128((__m128i const *)&src[0]);
			__m128i d3 = _mm_loadu_si128((__m128i const *)&src[2]);
			d0 = _mm_mullo_epi16(d0, m0);
			d1 = _mm_mullo_epi16(d1, m1);
			d0h = _mm_mullo_epi16(d0h, m0);
			d1h = _mm_mullo_epi16(d1h, m1);
			__m128i d2h = _mm_unpackhi_epi8(d2, zero);
			__m128i d3h = _mm_unpackhi_epi8(d3, zero);
			d2 = _mm_unpacklo_epi8(d2, zero);
			d3 = _mm_unpacklo_epi8(d3, zero);
			d0 = _mm_add_epi16(d0, d1);
			d0h = _mm_add_epi16(d0h, d1h);
			__m128i t2 = _mm_mullo_epi16(d2, m2);
			__m128i t3 = _mm_mullo_epi16(d3, m3);
			__m128i t2h = _mm_mullo_epi16(d2h, m2);
			__m128i t3h = _mm_mullo_epi16(d3h, m3);
			d0 = _mm_add_epi16(d0, rnd);
			d0h = _mm_add_epi16(d0h, rnd);
			d0 = _mm_add_epi16(d0, t2);
			d0h = _mm_add_epi16(d0h, t2h);
			d0 = _mm_add_epi16(d0, t3);
			d0h = _mm_add_epi16(d0h, t3h);
			d0 = _mm_srai_epi16(d0, 6);
			d0h = _mm_srai_epi16(d0h, 6);
			d0 = _mm_packus_epi16(d0, d0h);
			_mm_store_si128((__m128i *)&dst[0], d0);
			d0 = d2;
			d0h = d2h;
			d1 = d3;
			d1h = d3h;
			dst += stride;
		} while (--height);
	} else if (width == 8) {
		d0 = _mm_unpacklo_epi8(d0, zero);
		d1 = _mm_unpacklo_epi8(d1, zero);
		do {
			src += src_stride;
			__m128i d2 = _mm_loadu_si128((__m128i const *)&src[0]);
			__m128i d3 = _mm_loadu_si128((__m128i const *)&src[2]);
			d0 = _mm_mullo_epi16(d0, m0);
			d1 = _mm_mullo_epi16(d1, m1);
			d2 = _mm_unpacklo_epi8(d2, zero);
			d3 = _mm_unpacklo_epi8(d3, zero);
			d0 = _mm_add_epi16(d0, d1);
			__m128i t2 = _mm_mullo_epi16(d2, m2);
			__m128i t3 = _mm_mullo_epi16(d3, m3);
			d0 = _mm_add_epi16(d0, rnd);
			d0 = _mm_add_epi16(d0, t2);
			d0 = _mm_add_epi16(d0, t3);
			d0 = _mm_srai_epi16(d0, 6);
			d0 = _mm_packus_epi16(d0, zero);
			_mm_storel_epi64((__m128i *)&dst[0], d0);
			d0 = d2;
			d1 = d3;
			dst += stride;
		} while (--height);
	} else {
		d0 = _mm_unpacklo_epi8(d0, zero);
		d1 = _mm_unpacklo_epi8(d1, zero);
		do {
			src += src_stride;
			__m128i d2 = _mm_loadu_si128((__m128i const *)&src[0]);
			__m128i d3 = _mm_loadu_si128((__m128i const *)&src[2]);
			d0 = _mm_mullo_epi16(d0, m0);
			d1 = _mm_mullo_epi16(d1, m1);
			d2 = _mm_unpacklo_epi8(d2, zero);
			d3 = _mm_unpacklo_epi8(d3, zero);
			d0 = _mm_add_epi16(d0, d1);
			__m128i t2 = _mm_mullo_epi16(d2, m2);
			__m128i t3 = _mm_mullo_epi16(d3, m3);
			d0 = _mm_add_epi16(d0, rnd);
			d0 = _mm_add_epi16(d0, t2);
			d0 = _mm_add_epi16(d0, t3);
			d0 = _mm_srai_epi16(d0, 6);
			d0 = _mm_packus_epi16(d0, zero);
			*(uint32_t *)dst = _mm_cvtsi128_si32(d0);
			d0 = d2;
			d1 = d3;
			dst += stride;
		} while (--height);
	}
#else
	int c0 = 64 - c1 - c2 + c3;
	const uint8_t *src1 = src + src_stride;
	c1 = c1 - c3;
	c2 = c2 - c3;
	stride -= width;
	src_stride = src_stride - width - 2;
	width >>= 1;
	do {
		int x = width;
		uint32_t t0 = *src++;
		uint32_t t2 = *src1++;
		t0 = (t0 << 16) | *src++;
		t2 = (t2 << 16) | *src1++;
		do {
			uint32_t t1 = *src++;
			uint32_t t3 = *src1++;
			t1 = (t1 << 16) | *src++;
			t3 = (t3 << 16) | *src1++;
			t0 = (t0 * c0 + t2 * c2 + t1 * c1 + t3 * c3 + 0x00200020);
			t0 = t0 >> 6;
			dst[0] = t0 >> 16;
			dst[1] = t0;
			t0 = t1;
			t2 = t3;
			dst += 2;
		} while (--x);
		src += src_stride;
		src1 += src_stride;
		dst += stride;
	} while (--height);
#endif
}

static inline void extend_left_chroma(uint8_t *dst, int left, int width, int height)
{
	do {
		int c = *(int16_t *)(dst + left);
		int x = left >> 1;
		int16_t *d = (int16_t *)dst;
		do {
			*d++ = c;
		} while (--x);
		dst += width;
	} while (--height);
}

static inline void fill_left_top_chroma(const uint8_t *src, uint8_t *buf, int left, int top, const h264d_vector_t& size, int stride)
{
	int width = size.v[0];
	int height = size.v[1] >> 1;
	int y;

	src += top * stride + left;
	left = (width == left) ? left - 2 : left;
	uint8_t *dst = buf + left;
	for (y = 0; y < top; ++y) {
		memcpy(dst, src, width - left);
		dst += width;
	}
	for (; y < height; ++y) {
		memcpy(dst, src, width - left);
		src += stride;
		dst += width;
	}
	if (left) {
		extend_left_chroma(buf, left, width, height);
	}
}

static inline void fill_left_bottom_chroma(const uint8_t *src, uint8_t *buf, int left, int bottom, const h264d_vector_t& size, int stride)
{
	int width = size.v[0];
	int height = size.v[1] >> 1;
	int y;

	src += left;
	left = (width == left) ? left - 2 : left;
	uint8_t *dst = buf + left;
	for (y = 0; y < height - bottom; ++y) {
		memcpy(dst, src, width - left);
		src += stride;
		dst += width;
	}
	src = dst - width;
	for (; y < height; ++y) {
		memcpy(dst, src, width - left);
		dst += width;
	}
	if (left) {
		extend_left_chroma(buf, left, width, height);
	}
}

static inline void extend_right_chroma(uint8_t *dst, int right, int width, int height)
{
	dst = dst + width - right;
	do {
		int c = ((int16_t *)dst)[-1];
		int x = right >> 1;
		int16_t *d = (int16_t *)dst;
		do {
			*d++ = c;
		} while (--x);
		dst += width;
	} while (--height);
}

static inline void fill_right_top_chroma(const uint8_t *src, uint8_t *buf, int right, int top, const h264d_vector_t& size, int stride)
{
	uint8_t *dst = buf;
	int width = size.v[0];
	int height = size.v[1] >> 1;
	int y;

	src = src + top * stride;
	for (y = 0; y < top; ++y) {
		memcpy(dst, src, width - right);
		dst += width;
	}
	for (; y < height; ++y) {
		memcpy(dst, src, width - right);
		src += stride;
		dst += width;
	}
	if (right) {
		extend_right_chroma(buf, right, width, height);
	}
}

static inline void fill_right_bottom_chroma(const uint8_t *src, uint8_t *buf, int right, int bottom, const h264d_vector_t& size, int stride)
{
	uint8_t *dst = buf;
	int width = size.v[0];
	int height = size.v[1] >> 1;
	int y;
	for (y = 0; y < height - bottom; ++y) {
		memcpy(dst, src, width - right);
		src += stride;
		dst += width;
	}
	src -= stride;
	for (; y < height; ++y) {
		memcpy(dst, src, width - right);
		dst += width;
	}
	if (right) {
		extend_right_chroma(buf, right, width, height);
	}
}

static inline void fill_rect_umv_chroma(const uint8_t *src, uint8_t *buf, const h264d_vector_t& size, int stride, int vert_size, int posx, int posy)
{
	int left, right, top, bottom;

	left = -posx;
	top = -posy;
	if (0 < left) {
		if (0 < top) {
			/* left, top */
			fill_left_top_chroma(src, buf, left, top, size, stride);
		} else {
			bottom = posy - vert_size + (size.v[1] >> 1);
			if (0 < bottom) {
				/* left, bottom */
				fill_left_bottom_chroma(src, buf, left, bottom, size, stride);
			} else {
				/* left */
				fill_left_top_chroma(src, buf, left, 0, size, stride);
			}
		}
	} else {
		right = posx - stride + size.v[0];
		if (0 < top) {
			if (0 < right) {
				/* top, right */
				fill_right_top_chroma(src, buf, right, top, size, stride);
			} else {
				/* top */
				fill_left_top_chroma(src, buf, 0, top, size, stride);
			}
		} else {
			bottom = posy - vert_size + (size.v[1] >> 1);
			if (0 < right) {
				if (0 < bottom) {
					/* right, bottom */
					fill_right_bottom_chroma(src, buf, right, bottom, size, stride);
				} else {
					/* right */
					fill_right_top_chroma(src, buf, right, 0, size, stride);
				}
			} else {
				if (0 < bottom) {
					/* bottom */
					fill_right_bottom_chroma(src, buf, 0, bottom, size, stride);
				} else {
					/* in range */
					fill_right_top_chroma(src, buf, 0, 0, size, stride);
				}
			}
		}
	}
}

static inline void chroma_inter_umv(const uint8_t *src, uint8_t *dst, int posx, int posy, const h264d_vector_t& size, int src_stride, int vert_size, int dst_stride, const h264d_vector_t* mv)
{
	static void (* const copy_inter_align[3])(const uint8_t *src, uint8_t *dst, int height, int src_stride, int stride) = {
		copy_inter4x_align4,
		copy_inter8x_align8,
		copy_inter16x_align8
	};
	uint32_t buf[18 * 9 / sizeof(uint32_t) + 1];
	int width = size.v[0];
	int height = (unsigned)size.v[1] >> 1;

	if (posx < -width) {
		src += -width - posx;
		posx = -width;
	} else if (src_stride - 2 < posx) {
		src -= posx - src_stride + 2;
		posx = src_stride - 2;
	}
	if (posy < -height) {
		src -= (height + posy) * src_stride;
		posy = -height;
	} else if (vert_size - 1 < posy) {
		src -= (posy - vert_size + 1) * src_stride;
		posy = vert_size - 1;
	}
	h264d_vector_t size_filter;
	if (mv) {
		size_filter.v[0] = width + 2;
		size_filter.v[1] = size.v[1] + 2;
	}
	fill_rect_umv_chroma(src, (uint8_t *)buf, mv ? size_filter : size, src_stride, vert_size, posx, posy);
	if (mv) {
		filter_chroma_vert_horiz((const uint8_t *)buf, dst, size, mv->v[0] & 7, mv->v[1] & 7, size.v[0] + 2, dst_stride);
	} else {
		copy_inter_align[(unsigned)width >> 3]((const uint8_t *)buf, dst, height, width, dst_stride);
	}
}

static void inter_pred_chroma_base(const uint8_t *src_chroma, int posx, int posy, const h264d_vector_t& mv, const h264d_vector_t& size_c, int src_stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	int mvx = mv.v[0] & 7;
	int mvy = mv.v[1] & 7;
	src_chroma = src_chroma + inter_pred_mvoffset_luma(posx, posy, src_stride);
	int height = size_c.v[1] >> 1;
	if (mvx || mvy) {
		if ((unsigned)posx <= (unsigned)(src_stride - size_c.v[0] - 2) && (unsigned)posy <= (unsigned)(vert_stride - height - 1)) {
			if (mvy) {
				if (mvx) {
					filter_chroma_vert_horiz(src_chroma, dst, size_c, mvx, mvy, src_stride, dst_stride);
				} else {
					filter_chroma_vert(src_chroma, dst, size_c, mvy, src_stride, dst_stride);
				}
			} else {
				filter_chroma_horiz(src_chroma, dst, size_c, mvx, src_stride, dst_stride);
			}
		} else {
			/* UMV */
			chroma_inter_umv(src_chroma, dst, posx, posy, size_c, src_stride, vert_stride, dst_stride, &mv);
		}

	} else {
		if ((unsigned)posx <= (unsigned)(src_stride - size_c.v[0]) && (unsigned)posy <= (unsigned)(vert_stride - height)) {
			copy_inter(src_chroma, dst, size_c.v[0], height, src_stride, dst_stride);
		} else {
			chroma_inter_umv(src_chroma, dst, posx, posy, size_c, src_stride, vert_stride, dst_stride, 0);
		}
	}
}

static inline uint32_t AVERAGE2(uint32_t s1, uint32_t s2)
{
	uint32_t x = s1 ^ s2;
	return (s1 & s2) + ((x & ~0x01010101) >> 1) + (x & 0x01010101);
}

static inline void add_bidir(const uint8_t *src, uint8_t *dst, int width, int height, int stride)
{
	int x_len = (uint32_t)width >> 2;
	do {
		int x = x_len;
		const uint32_t *s = (const uint32_t *)src;
		uint32_t *d = (uint32_t *)dst;
		do {
			*d = AVERAGE2(*s++, *d);
			d++;
		} while (--x);
		src += width;
		dst += stride;
	} while (--height);
}

static void inter_pred_chroma_bidir(const uint8_t *src_chroma, int posx, int posy, const h264d_vector_t& mv, const h264d_vector_t& size_c, int src_stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	uint32_t tmp[(16 * 8) / sizeof(uint32_t)];
	inter_pred_chroma_base(src_chroma, posx, posy, mv, size_c, src_stride, vert_stride, (uint8_t *)tmp, size_c.v[0]);
	add_bidir((const uint8_t *)tmp, dst, size_c.v[0], size_c.v[1] >> 1, src_stride);
}

static void (* const inter_pred_chroma[2])(const uint8_t *src_chroma, int posx, int posy, const h264d_vector_t& mv, const h264d_vector_t& size_c, int src_stride, int vert_stride, uint8_t *dst, int dst_stride) = {
	inter_pred_chroma_base,
	inter_pred_chroma_bidir
};

template <int RND, typename DSTTYPE, typename F>
static inline void inter_pred_luma_filter02_core_base(const uint8_t *src, DSTTYPE *dst, const h264d_vector_t& size, int src_stride, int stride, F Store)
{
	int width = size.v[0];
	int height = size.v[1];
	src_stride = src_stride - width - 6;
	stride = stride - width;
	width = (unsigned)width >> 2;
	do {
		int c0, c1, c2, c3, c4;
		int x = width;

		c0 = *src++;
		c0 = (c0 << 16) | *src++;
		c1 = (c0 << 16) | *src++;
		c2 = (c1 << 16) | *src++;
		c3 = (c2 << 16) | *src++;
		c4 = (c3 << 16) | *src++;
		do {
			int t;
			int c5 = (c4 << 16) | *src++;
			t = FILTER6TAP_DUAL(c0, c1, c2, c3, c4, c5, RND);
			c0 = (c5 << 16) | *src++;
			Store(t, dst);
			c1 = (c0 << 16) | *src++;
			t = FILTER6TAP_DUAL(c2, c3, c4, c5, c0, c1, RND);
			Store(t, dst + 2);
			c2 = c0;
			c3 = c1;
			c0 = c4;
			c4 = (c3 << 16) | *src++;
			c1 = c5;
			dst += 4;
		} while (--x);
		src += src_stride;
		dst += stride;
	} while (--height);
}

#ifndef X86ASM
struct clip_store8dual {
	void operator()(uint32_t t, uint8_t *dst) const {
		t >>= 5;
		dst[0] = CLIP255H(t >> 16);
		dst[1] = CLIP255H(t);
	}
};
#endif

static inline void inter_pred_luma_filter02_core(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
#ifdef X86ASM
	__m128i c5 = _mm_cvtsi32_si128(0xfffbfffb);
	__m128i c20 = _mm_cvtsi32_si128(0x00140014);
	__m128i rnd = _mm_cvtsi32_si128(0x00100010);
	__m128i zero = _mm_setzero_si128();
	int width = size.v[0];
	int height = size.v[1];
	c5 = _mm_shuffle_epi32(c5, 0);
	c20 = _mm_shuffle_epi32(c20, 0);
	rnd = _mm_shuffle_epi32(rnd, 0);
	if (width != 4) {
		width = (unsigned)width >> 3;
		do {
			for (int x = 0; x < width; ++x) {
				__m128i r0 = _mm_loadu_si128((__m128i const *)&src[x * 8]);
				__m128i r4 = _mm_loadu_si128((__m128i const *)&src[x * 8 + 5]);
				__m128i r1 = _mm_loadu_si128((__m128i const *)&src[x * 8 + 1]);
				__m128i r2 = _mm_loadu_si128((__m128i const *)&src[x * 8 + 2]);
				__m128i r3 = _mm_loadu_si128((__m128i const *)&src[x * 8 + 3]);
				r0 = _mm_unpacklo_epi8(r0, zero);
				r4 = _mm_unpacklo_epi8(r4, zero);
				r0 = _mm_add_epi16(r0, r4);
				r4 = _mm_loadu_si128((__m128i const *)&src[x * 8 + 4]);
				r1 = _mm_unpacklo_epi8(r1, zero);
				r2 = _mm_unpacklo_epi8(r2, zero);
				r3 = _mm_unpacklo_epi8(r3, zero);
				r4 = _mm_unpacklo_epi8(r4, zero);
				r2 = _mm_add_epi16(r2, r3);
				r1 = _mm_add_epi16(r1, r4);
				r2 = _mm_mullo_epi16(r2, c20);
				r1 = _mm_mullo_epi16(r1, c5);
				r2 = _mm_add_epi16(r2, r0);
				r2 = _mm_add_epi16(r2, rnd);
				r2 = _mm_add_epi16(r2, r1);
				r2 = _mm_srai_epi16(r2, 5);
				r2 = _mm_packus_epi16(r2, zero);
				_mm_storel_epi64((__m128i *)&dst[x * 8], r2);
			}
			src += src_stride;
			dst += stride;
		} while (--height);
	} else {
		do {
			__m128i r0 = _mm_loadu_si128((__m128i const *)&src[0]);
			__m128i r4 = _mm_loadu_si128((__m128i const *)&src[5]);
			__m128i r1 = _mm_loadu_si128((__m128i const *)&src[1]);
			__m128i r2 = _mm_loadu_si128((__m128i const *)&src[2]);
			__m128i r3 = _mm_loadu_si128((__m128i const *)&src[3]);
			r0 = _mm_unpacklo_epi8(r0, zero);
			r4 = _mm_unpacklo_epi8(r4, zero);
			r0 = _mm_add_epi16(r0, r4);
			r4 = _mm_loadu_si128((__m128i const *)&src[4]);
			r1 = _mm_unpacklo_epi8(r1, zero);
			r2 = _mm_unpacklo_epi8(r2, zero);
			r3 = _mm_unpacklo_epi8(r3, zero);
			r4 = _mm_unpacklo_epi8(r4, zero);
			r2 = _mm_add_epi16(r2, r3);
			r1 = _mm_add_epi16(r1, r4);
			r2 = _mm_mullo_epi16(r2, c20);
			r1 = _mm_mullo_epi16(r1, c5);
			r2 = _mm_add_epi16(r2, r0);
			r2 = _mm_add_epi16(r2, rnd);
			r2 = _mm_add_epi16(r2, r1);
			r2 = _mm_srai_epi16(r2, 5);
			r2 = _mm_packus_epi16(r2, zero);
			*(uint32_t *)dst = _mm_cvtsi128_si32(r2);
			src += src_stride;
			dst += stride;
		} while (--height);
	}
#else
	inter_pred_luma_filter02_core_base<0x00100010>(src, dst, size, src_stride, stride, clip_store8dual());
#endif
}

#ifndef X86ASM
template <int RND, typename DSTTYPE, typename F>
static inline void inter_pred_luma_filter20_core_base(const uint8_t *src, DSTTYPE *dst, const h264d_vector_t& size, int src_stride, int stride, F Store)
{
	int width = size.v[0] >> 1;
	int height = size.v[1] >> 1;
	do {
		uint32_t c0, c1, c2, c3, c4;
		const uint8_t *s = src;
		DSTTYPE *d = dst;
		int y = height;

		c0 = (s[0] << 16) | s[1];
		s += src_stride;
		c1 = (s[0] << 16) | s[1];
		s += src_stride;
		c2 = (s[0] << 16) | s[1];
		s += src_stride;
		c3 = (s[0] << 16) | s[1];
		s += src_stride;
		c4 = (s[0] << 16) | s[1];
		s += src_stride;
		do {
			uint32_t t, c5;
			c5 = (s[0] << 16) | s[1];
			t = FILTER6TAP_DUAL(c0, c1, c2, c3, c4, c5, RND);
			s += src_stride;
			Store(t, d);
			c0 = (s[0] << 16) | s[1];
			d += stride;
			t = FILTER6TAP_DUAL(c1, c2, c3, c4, c5, c0, RND);
			s += src_stride;
			Store(t, d);
			t = c0;
			c0 = c2;
			c1 = c3;
			c2 = c4;
			c3 = c5;
			c4 = t;
			d += stride;
		} while (--y);
		src += 2;
		dst += 2;
	} while (--width);
}
#endif

static inline void inter_pred_luma_filter20_core(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
#ifdef X86ASM
	__m128i c5 = _mm_cvtsi32_si128(0xfffbfffb);
	__m128i c20 = _mm_cvtsi32_si128(0x00140014);
	__m128i rnd = _mm_cvtsi32_si128(0x00100010);
	__m128i zero = _mm_setzero_si128();
	int width = size.v[0];
	int height = size.v[1];
	c5 = _mm_shuffle_epi32(c5, 0);
	c20 = _mm_shuffle_epi32(c20, 0);
	rnd = _mm_shuffle_epi32(rnd, 0);
	if (width != 4) {
		width = (unsigned)width >> 3;
		do {
			const uint8_t *s = src;
			uint8_t *d = dst;
			__m128i r0 = _mm_loadu_si128((__m128i const *)s);
			s += src_stride;
			__m128i r1 = _mm_loadu_si128((__m128i const *)s);
			s += src_stride;
			__m128i r2 = _mm_loadu_si128((__m128i const *)s);
			s += src_stride;
			__m128i r3 = _mm_loadu_si128((__m128i const *)s);
			s += src_stride;
			__m128i r4 = _mm_loadu_si128((__m128i const *)s);
			s += src_stride;
			int y = height;
			r0 = _mm_unpacklo_epi8(r0, zero);
			r1 = _mm_unpacklo_epi8(r1, zero);
			r2 = _mm_unpacklo_epi8(r2, zero);
			r3 = _mm_unpacklo_epi8(r3, zero);
			r4 = _mm_unpacklo_epi8(r4, zero);
			do {
				__m128i r5 = _mm_loadu_si128((__m128i const *)s);
				__m128i t0 = _mm_add_epi16(r2, r3);
				__m128i t1 = _mm_add_epi16(r1, r4);
				s += src_stride;
				r5 = _mm_unpacklo_epi8(r5, zero);
				t0 = _mm_mullo_epi16(t0, c20);
				t1 = _mm_mullo_epi16(t1, c5);
				t0 = _mm_add_epi16(t0, r0);
				t0 = _mm_add_epi16(t0, r5);
				t0 = _mm_add_epi16(t0, rnd);
				t0 = _mm_add_epi16(t0, t1);
				t0 = _mm_srai_epi16(t0, 5);
				t0 = _mm_packus_epi16(t0, zero);
				_mm_storel_epi64((__m128i *)d, t0);
				r0 = r1;
				r1 = r2;
				r2 = r3;
				r3 = r4;
				r4 = r5;
				d += stride;
			} while (--y);
			src += 8;
			dst += 8;
		} while (--width);
	} else {
		__m128i r0 = _mm_cvtsi32_si128(*(uint32_t const *)src);
		src += src_stride;
		__m128i r1 = _mm_cvtsi32_si128(*(uint32_t const *)src);
		src += src_stride;
		__m128i r2 = _mm_cvtsi32_si128(*(uint32_t const *)src);
		src += src_stride;
		__m128i r3 = _mm_cvtsi32_si128(*(uint32_t const *)src);
		src += src_stride;
		__m128i r4 = _mm_cvtsi32_si128(*(uint32_t const *)src);
		src += src_stride;
		r0 = _mm_unpacklo_epi8(r0, zero);
		r1 = _mm_unpacklo_epi8(r1, zero);
		r2 = _mm_unpacklo_epi8(r2, zero);
		r3 = _mm_unpacklo_epi8(r3, zero);
		r4 = _mm_unpacklo_epi8(r4, zero);
		do {
			__m128i r5 = _mm_cvtsi32_si128(*(uint32_t const *)src);
			__m128i t0 = _mm_add_epi16(r2, r3);
			__m128i t1 = _mm_add_epi16(r1, r4);
			src += src_stride;
			r5 = _mm_unpacklo_epi8(r5, zero);
			t0 = _mm_mullo_epi16(t0, c20);
			t1 = _mm_mullo_epi16(t1, c5);
			t0 = _mm_add_epi16(t0, r0);
			t0 = _mm_add_epi16(t0, r5);
			t0 = _mm_add_epi16(t0, rnd);
			t0 = _mm_add_epi16(t0, t1);
			t0 = _mm_srai_epi16(t0, 5);
			t0 = _mm_packus_epi16(t0, zero);
			*(uint32_t *)dst = _mm_cvtsi128_si32(t0);
			r0 = r1;
			r1 = r2;
			r2 = r3;
			r3 = r4;
			r4 = r5;
			dst += stride;
		} while (--height);
	}
#else
	inter_pred_luma_filter20_core_base<0x00100010>(src, dst, size, src_stride, stride, clip_store8dual());
#endif
}

static inline void filter_1_3_v_post(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	uint32_t buf[16 * 22 / sizeof(uint32_t)] __attribute__((aligned(8)));
	inter_pred_luma_filter20_core(src, (uint8_t *)buf, size, src_stride, size.v[0]);
	add_bidir((uint8_t *)buf, dst, size.v[0], size.v[1], stride);
}

static inline int sign_extend15bit(uint32_t t) {
	return ((t & 0x40004000) * 2) | (t & ~0x8000);
}

struct store32dual {
	void operator()(uint32_t t, int16_t *dst) const {
		t = sign_extend15bit(t);
#ifdef WORDS_BIGENDIAN
		*(uint32_t *)dst = t;
#elif defined(__RENESAS_VERSION__)
		*(uint32_t *)dst = swapw(t);
#else
		dst[0] = (int16_t)(t >> 16);
		dst[1] = (int16_t)t;
#endif
	}
};

template <typename F>
static inline void inter_pred_luma_filter22_horiz(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride, F Pred)
{
	int16_t buf[16 * 22] __attribute__((aligned(8)));
	h264d_vector_t size_f = {{size.v[0], size.v[1] + 5}};
	inter_pred_luma_filter02_core_base<0>(src, buf, size_f, src_stride, size.v[0], store32dual());

	const int16_t *dd = buf;
	int width = size.v[0];
	int height = size.v[1];
	int y = width;
	height >>= 1;
	do {
		int c0, c1, c2, c3, c4;
		int yy = height;
		const int16_t *d = dd++;
		uint8_t *dest = dst++;

		c0 = *d;
		d += width;
		c1 = *d;
		d += width;
		c2 = *d;
		d += width;
		c3 = *d;
		d += width;
		c4 = *d;
		d += width;
		do {
			int c5, c6;
			c5 = *d;
			d += width;
			*dest = Pred(c0, c1, c2, c3, c4, c5);
			c6 = *d;
			dest += stride;
			d += width;
			*dest = Pred(c1, c2, c3, c4, c5, c6);
			c0 = c2;
			c1 = c3;
			c2 = c4;
			c3 = c5;
			c4 = c6;
			dest += stride;
		} while (--yy);
	} while (--y);
}

#ifdef X86ASM
static inline void inter_pred_luma_filter20_interim(const uint8_t *src, int16_t *dst, const h264d_vector_t& size, int src_stride)
{
	__m128i c5 = _mm_cvtsi32_si128(0xfffbfffb);
	__m128i c20 = _mm_cvtsi32_si128(0x00140014);
	__m128i zero = _mm_setzero_si128();
	int stride = (((unsigned)size.v[0] >> 1) & 8) + 16;
	int height = size.v[1] >> 1;
	int width = (unsigned)stride >> 3;
	c5 = _mm_shuffle_epi32(c5, 0);
	c20 = _mm_shuffle_epi32(c20, 0);
	do {
		const uint8_t *s = src;
		int16_t *d = dst;
		__m128i r0 = _mm_loadu_si128((__m128i const *)s);
		s += src_stride;
		__m128i r1 = _mm_loadu_si128((__m128i const *)s);
		s += src_stride;
		__m128i r2 = _mm_loadu_si128((__m128i const *)s);
		s += src_stride;
		__m128i r3 = _mm_loadu_si128((__m128i const *)s);
		s += src_stride;
		__m128i r4 = _mm_loadu_si128((__m128i const *)s);
		s += src_stride;
		int y = height;
		r0 = _mm_unpacklo_epi8(r0, zero);
		r1 = _mm_unpacklo_epi8(r1, zero);
		r2 = _mm_unpacklo_epi8(r2, zero);
		r3 = _mm_unpacklo_epi8(r3, zero);
		r4 = _mm_unpacklo_epi8(r4, zero);
		do {
			__m128i r5 = _mm_loadu_si128((__m128i const *)s);
			__m128i t0 = _mm_add_epi16(r2, r3);
			__m128i t1 = _mm_add_epi16(r1, r4);
			s += src_stride;
			r5 = _mm_unpacklo_epi8(r5, zero);
			t0 = _mm_mullo_epi16(t0, c20);
			t1 = _mm_mullo_epi16(t1, c5);
			t0 = _mm_add_epi16(t0, r0);
			t0 = _mm_add_epi16(t0, r5);
			t0 = _mm_add_epi16(t0, t1);
			_mm_store_si128((__m128i *)d, t0);
			r0 = _mm_loadu_si128((__m128i const *)s);
			d += stride;
			t0 = _mm_add_epi16(r3, r4);
			t1 = _mm_add_epi16(r2, r5);
			s += src_stride;
			r0 = _mm_unpacklo_epi8(r0, zero);
			t0 = _mm_mullo_epi16(t0, c20);
			t1 = _mm_mullo_epi16(t1, c5);
			t0 = _mm_add_epi16(t0, r1);
			t0 = _mm_add_epi16(t0, r0);
			t0 = _mm_add_epi16(t0, t1);
			_mm_store_si128((__m128i *)d, t0);
			t0 = r0;
			r0 = r2;
			r1 = r3;
			r2 = r4;
			r3 = r5;
			r4 = t0;
			d += stride;
		} while (--y);
		src += 8;
		dst += 8;
	} while (--width);
}
#endif

template <typename F>
static inline void inter_pred_luma_filter22_vert(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride, F Pred)
{
	int width = size.v[0];
	int height = size.v[1];
#ifdef X86ASM
	int16_t ALIGN16VC buf[32 * 16] __attribute__((aligned(16)));
	inter_pred_luma_filter20_interim(src, buf, size, src_stride);
	int tmp_stride = (width & 4) + 3;
#else
	int16_t buf[22 * 16];
	int tmp_stride = width + 6;
	h264d_vector_t size_f = {{tmp_stride, size.v[1]}};
	inter_pred_luma_filter20_core_base<0>(src, buf, size_f, src_stride, tmp_stride, store32dual());
#endif
	const int16_t *dd = buf;
	stride -= width;
	width = (unsigned)width >> 1;
	do {
		int c0, c1, c2, c3, c4;
		int x = width;

		c0 = *dd++;
		c1 = *dd++;
		c2 = *dd++;
		c3 = *dd++;
		c4 = *dd++;
		do {
			int c5, c6;
			c5 = *dd++;
			dst[0] = Pred(c0, c1, c2, c3, c4, c5);
			c6 = *dd++;
			dst[1] = Pred(c1, c2, c3, c4, c5, c6);
			c0 = c2;
			c1 = c3;
			c2 = c4;
			c3 = c5;
			c4 = c6;
			dst += 2;
		} while (--x);
		dst += stride;
#ifdef X86ASM
		dd += tmp_stride;
#else
		dd++;
#endif
	} while (--height);
	VC_CHECK;
}

struct PPred22 {
	int operator()(int c0, int c1, int c2, int c3, int c4, int c5) const {
		int t = (((c2 + c3) * 4 - c1 - c4) * 5 + c0 + c5 + 512) >> 10;
		return CLIP255C(t);
	}
};

struct PPred12 {
	int operator()(int c0, int c1, int c2, int c3, int c4, int c5) const {
		int t = (((c2 + c3) * 4 - c1 - c4) * 5 + c0 + c5 + 512) >> 10;
		int c = (c2 + 16) >> 5;
		return (CLIP255I(t) + CLIP255I(c) + 1) >> 1;
	}
};

struct PPred32 {
	int operator()(int c0, int c1, int c2, int c3, int c4, int c5) const {
		return PPred12()(c0, c1, c3, c2, c4, c5);
	}
};

static inline void inter_pred_luma_filter_add(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	int width = size.v[0];
	int height = size.v[1];
	stride -= width;
	src_stride -= width;
	width = (unsigned)width >> 2;
	do {
		int x = width;
		do {
			*(uint32_t *)dst = AVERAGE2(*(uint32_t *)dst, read4_unalign((const uint32_t *)src));
			src += 4;
			dst += 4;
		} while (--x);
		src += src_stride;
		dst += stride;
	} while (--height);
}

static void inter_pred_luma_filter00(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	int width = size.v[0];
	int height = size.v[1];
	src = src + 2 + src_stride * 2;
	do {
		memcpy(dst, src, width);
		src += src_stride;
		dst += stride;
	} while (--height);
}

static void inter_pred_luma_filter01(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 2, dst, size, src_stride, stride);
	inter_pred_luma_filter_add(src + src_stride * 2 + 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter02(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter03(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 2, dst, size, src_stride, stride);
	inter_pred_luma_filter_add(src + src_stride * 2 + 3, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter10(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter20_core(src + 2, dst, size, src_stride, stride);
	inter_pred_luma_filter_add(src + src_stride * 2 + 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter11(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 2, dst, size, src_stride, stride);
	filter_1_3_v_post(src + 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter12(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter22_horiz(src, dst, size, src_stride, stride, PPred12());
}

static void inter_pred_luma_filter13(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 2, dst, size, src_stride, stride);
	filter_1_3_v_post(src + 3, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter20(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter20_core(src + 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter21(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter22_vert(src, dst, size, src_stride, stride, PPred12());
}

static inline void inter_pred_luma_filter22(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter22_horiz(src, dst, size, src_stride, stride, PPred22());
}

static void inter_pred_luma_filter23(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter22_vert(src, dst, size, src_stride, stride, PPred32());
}

static void inter_pred_luma_filter30(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter20_core(src + 2, dst, size, src_stride, stride);
	inter_pred_luma_filter_add(src + src_stride * 3 + 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter31(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 3, dst, size, src_stride, stride);
	filter_1_3_v_post(src + 2, dst, size, src_stride, stride);
}

static void inter_pred_luma_filter32(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter22_horiz(src, dst, size, src_stride, stride, PPred32());
}

static void inter_pred_luma_filter33(const uint8_t *src, uint8_t *dst, const h264d_vector_t& size, int src_stride, int stride)
{
	inter_pred_luma_filter02_core(src + src_stride * 3, dst, size, src_stride, stride);
	filter_1_3_v_post(src + 3, dst, size, src_stride, stride);
}

static inline void extend_left_luma(uint8_t *dst, int left, int width, int height)
{
	do {
		int c = dst[left];
		memset(dst, c, left);
		dst += width;
	} while (--height);
}

static inline void fill_left_top(const uint8_t *src, uint8_t *buf, int left, int top, int width, int height, int stride)
{
	uint8_t *dst;
	int y;

	assert(left <= width);
	src += top * stride + left;
	dst = buf + left;
	for (y = 0; y < top; ++y) {
		memcpy(dst, src, width - left);
		dst += width;
	}
	for (; y < height; ++y) {
		memcpy(dst, src, width - left);
		src += stride;
		dst += width;
	}
	if (left) {
		extend_left_luma(buf, left, width, height);
	}
}

static inline void fill_left_bottom(const uint8_t *src, uint8_t *buf, int left, int bottom, int width, int height, int stride)
{
	uint8_t *dst;
	int y;

	assert(left <= width);
	src += left;
	dst = buf + left;
	for (y = 0; y < height - bottom; ++y) {
		memcpy(dst, src, width - left);
		src += stride;
		dst += width;
	}
	src = dst - width;
	for (; y < height; ++y) {
		memcpy(dst, src, width - left);
		dst += width;
	}
	if (left) {
		extend_left_luma(buf, left, width, height);
	}
}

static inline void extend_right_luma(uint8_t *dst, int right, int width, int height)
{
	dst = dst + width - right;
	do {
		int c = ((int8_t *)dst)[-1];
		memset(dst, c, right);
		dst += width;
	} while (--height);
}

static inline void fill_right_top(const uint8_t *src, uint8_t *buf, int right, int top, int width, int height, int stride)
{
	uint8_t *dst;
	int y;

	assert(right <= width);
	src = src + top * stride;
	dst = buf;
	for (y = 0; y < top; ++y) {
		memcpy(dst, src, width - right);
		dst += width;
	}
	for (; y < height; ++y) {
		memcpy(dst, src, width - right);
		src += stride;
		dst += width;
	}
	if (right) {
		extend_right_luma(buf, right, width, height);
	}
}

static inline void fill_right_bottom(const uint8_t *src, uint8_t *buf, int right, int bottom, int width, int height, int stride)
{
	uint8_t *dst;
	int y;

	assert(right <= width);
	dst = buf;
	for (y = 0; y < height - bottom; ++y) {
		memcpy(dst, src, width - right);
		src += stride;
		dst += width;
	}
	src -= stride;
	for (; y < height; ++y) {
		memcpy(dst, src, width - right);
		dst += width;
	}
	if (right) {
		extend_right_luma(buf, right, width, height);
	}
}

static inline void fill_rect_umv_luma(const uint8_t *src, uint8_t *buf, int width, int height, int stride, int vert_size, int posx, int posy)
{
	int left, right, top, bottom;

	left = -posx;
	top = -posy;
	if (0 < left) {
		if (0 < top) {
			/* left, top */
			fill_left_top(src, buf, left, top, width, height, stride);
		} else {
			bottom = posy - vert_size + height;
			if (0 < bottom) {
				/* left, bottom */
				fill_left_bottom(src, buf, left, bottom, width, height, stride);
			} else {
				/* left */
				fill_left_top(src, buf, left, 0, width, height, stride);
			}
		}
	} else {
		right = posx - stride + width;
		if (0 < top) {
			if (0 < right) {
				/* top, right */
				fill_right_top(src, buf, right, top, width, height, stride);
			} else {
				/* top */
				fill_left_top(src, buf, 0, top, width, height, stride);
			}
		} else {
			bottom = posy - vert_size + height;
			if (0 < right) {
				if (0 < bottom) {
					/* right, bottom */
					fill_right_bottom(src, buf, right, bottom, width, height, stride);
				} else {
					/* right */
					fill_right_top(src, buf, right, 0, width, height, stride);
				}
			} else {
				if (0 < bottom) {
					/* bottom */
					fill_right_bottom(src, buf, 0, bottom, width, height, stride);
				} else {
					/* in range */
					fill_right_top(src, buf, 0, 0, width, height, stride);
				}
			}
		}
	}
}

static void inter_pred_luma_umv(const uint8_t *src, int posx, int posy, const h264d_vector_t& size, int src_stride, int vert_size, int dst_stride, void (* const filter)(const uint8_t *, uint8_t *, const h264d_vector_t&, int, int), uint8_t *dst)
{
	uint8_t buf[22 * 22];
	int width = size.v[0] + 6;
	int height = size.v[1] + 6;

	/* rewind if beyond boundary */
	if (posx < 3 - width) {
		src += 3 - width - posx;
		posx = 3 - width;
	} else if (src_stride - 1 < posx - 2) {
		src -= posx - src_stride - 1;
		posx = src_stride + 1;
	}
	if (posy < 3 - height) {
		src += (3 - height - posy) * src_stride;
		posy = 3 - height;
	} else if (vert_size - 1 < posy - 2) {
		src -= (posy - vert_size - 1) * src_stride;
		posy = vert_size + 1;
	}
	fill_rect_umv_luma(src, buf, width, height, src_stride, vert_size, posx - 2, posy - 2);
	filter(buf, dst, size, width, dst_stride);
}

static void inter_pred_luma_frac00(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if ((unsigned)posx <= (unsigned)(stride - size.v[0]) && (unsigned)posy <= (unsigned)(vert_stride - size.v[1])) {
		src_luma = src_luma + inter_pred_mvoffset_luma(2, 2, stride);
		copy_inter(src_luma, dst, size.v[0], size.v[1], stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter00, dst);
	}
}

static void inter_pred_luma_frac01(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && (unsigned)posy < (unsigned)(vert_stride - size.v[1])) {
		inter_pred_luma_filter01(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter01, dst);
	}
}

static void inter_pred_luma_frac02(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride) 
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && (unsigned)posy < (unsigned)(vert_stride - size.v[1])) {
		inter_pred_luma_filter02(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter02, dst);
	}
}

static void inter_pred_luma_frac03(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && (unsigned)posy < (unsigned)(vert_stride - size.v[1])) {
		inter_pred_luma_filter03(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter03, dst);
	}
}

static void inter_pred_luma_frac10(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if ((unsigned)posx < (unsigned)(stride - size.v[0]) && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter10(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter10, dst);
	}
}

static void inter_pred_luma_frac11(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter11(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter11, dst);
	}
}

static void inter_pred_luma_frac12(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter12(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter12, dst);
	}
}

static void inter_pred_luma_frac13(const uint8_t *src_luma, int  posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter13(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter13, dst);
	}
}

static void inter_pred_luma_frac20(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if ((unsigned)posx < (unsigned)(stride - size.v[0]) && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter20(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter20, dst);
	}
}

static void inter_pred_luma_frac21(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter21(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter21, dst);
	}
}

static void inter_pred_luma_frac22(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter22(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter22, dst);
	}
}

static void inter_pred_luma_frac23(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter23(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter23, dst);
	}
}

static void inter_pred_luma_frac30(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if ((unsigned)posx < (unsigned)(stride - size.v[0]) && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter30(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter30, dst);
	}
}

static void inter_pred_luma_frac31(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter31(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter31, dst);
	}
}

static void inter_pred_luma_frac32(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter32(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter32, dst);
	}
}

static void inter_pred_luma_frac33(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dst_stride)
{
	if (2 <= posx && posx < stride - size.v[0] - 2 && 2 <= posy && posy < vert_stride - size.v[1] - 2) {
		inter_pred_luma_filter33(src_luma, dst, size, stride, dst_stride);
	} else {
		inter_pred_luma_umv(src_luma, posx, posy, size, stride, vert_stride, dst_stride, inter_pred_luma_filter33, dst);
	}
}

template <typename F>
static inline void inter_pred_luma_bidir_latter(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst,
						F InterPred)
{
	uint32_t tmp[(16 * 16) / sizeof(uint32_t)];

	InterPred(src_luma, posx, posy, size, stride, vert_stride, (uint8_t *)tmp, size.v[0]);
	add_bidir((const uint8_t *)tmp, dst, size.v[0], size.v[1], stride);
}

static void inter_pred_luma_bidir_latter00(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac00);
}

static void inter_pred_luma_bidir_latter01(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac01);
}

static void inter_pred_luma_bidir_latter02(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac02);
}

static void inter_pred_luma_bidir_latter03(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac03);
}

static void inter_pred_luma_bidir_latter10(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac10);
}

static void inter_pred_luma_bidir_latter11(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac11);
}

static void inter_pred_luma_bidir_latter12(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac12);
}

static void inter_pred_luma_bidir_latter13(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac13);
}

static void inter_pred_luma_bidir_latter20(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac20);
}

static void inter_pred_luma_bidir_latter21(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac21);
}

static void inter_pred_luma_bidir_latter22(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac22);
}

static void inter_pred_luma_bidir_latter23(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac23);
}

static void inter_pred_luma_bidir_latter30(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac30);
}

static void inter_pred_luma_bidir_latter31(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac31);
}

static void inter_pred_luma_bidir_latter32(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac32);
}

static void inter_pred_luma_bidir_latter33(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int stride, int vert_stride, uint8_t *dst, int dummy)
{
	inter_pred_luma_bidir_latter(src_luma, posx, posy, size, stride, vert_stride, dst, inter_pred_luma_frac33);
}

static void (* const inter_pred_luma[2][4][4])(const uint8_t *src_luma, int posx, int posy, const h264d_vector_t& size, int src_stride, int vert_stride, uint8_t *dst, int dst_stride) = {
	{
		{
			inter_pred_luma_frac00,
			inter_pred_luma_frac01,
			inter_pred_luma_frac02,
			inter_pred_luma_frac03
		},
		{
			inter_pred_luma_frac10,
			inter_pred_luma_frac11,
			inter_pred_luma_frac12,
			inter_pred_luma_frac13
		},
		{
			inter_pred_luma_frac20,
			inter_pred_luma_frac21,
			inter_pred_luma_frac22,
			inter_pred_luma_frac23
		},
		{
			inter_pred_luma_frac30,
			inter_pred_luma_frac31,
			inter_pred_luma_frac32,
			inter_pred_luma_frac33
		}
	},
	{
		{
			inter_pred_luma_bidir_latter00,
			inter_pred_luma_bidir_latter01,
			inter_pred_luma_bidir_latter02,
			inter_pred_luma_bidir_latter03
		},
		{
			inter_pred_luma_bidir_latter10,
			inter_pred_luma_bidir_latter11,
			inter_pred_luma_bidir_latter12,
			inter_pred_luma_bidir_latter13
		},
		{
			inter_pred_luma_bidir_latter20,
			inter_pred_luma_bidir_latter21,
			inter_pred_luma_bidir_latter22,
			inter_pred_luma_bidir_latter23
		},
		{
			inter_pred_luma_bidir_latter30,
			inter_pred_luma_bidir_latter31,
			inter_pred_luma_bidir_latter32,
			inter_pred_luma_bidir_latter33
		}
	}
};

static uint32_t transposition(uint32_t a)
{
	uint32_t b = 0U;
	for (int y = 0; y < 4 * 2; y += 2) {
		for (int x = 0; x < 4 * 8; x += 8) {
			b |= (a & 3U) << (x + y);
			a >>= 2;
		}
	}
	return b;
}

template <typename F0>
static inline void residual_luma_inter4x4(h264d_mb_current *mb, uint32_t cbp, dec_bits *st, int avail,
				    F0 ResidualBlock)
{
	int ALIGN16VC coeff[16] __attribute__((aligned(16)));
	const int16_t *qmat;
	const int *offset;
	uint32_t top, left;
	int c0, c1, c2, c3, c4, c5;
	uint32_t str_map; /* deblocking filter strength 3, vertical */
	uint8_t *luma;
	int stride;

	qmat = mb->qmaty;
	offset = mb->offset4x4;
	luma = mb->luma;
	stride = mb->max_x * 16;
	str_map = 0;
	if (cbp & 1) {
		if ((c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 0) : -1, avail & 2 ? UNPACK(*mb->top4x4coef, 0) : -1, st, coeff, qmat, avail, 0, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[0], coeff, stride);
			str_map = 0x2;
		}
		if ((c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 1) : -1, st, coeff, qmat, avail, 1, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[1], coeff, stride);
			str_map |= 0x8;
		}
		if ((c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 1) : -1, c0, st, coeff, qmat, avail, 2, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[2], coeff, stride);
			str_map |= 0x200;
		}
		if ((c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail, 3, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[3], coeff, stride);
			str_map |= 0x800;
		}
	} else {
		c0 = 0;
		c1 = 0;
		c2 = 0;
		c3 = 0;
	}
	if (cbp & 2) {
		if ((c0 = ResidualBlock(mb, c1, avail & 2 ? UNPACK(*mb->top4x4coef, 2) : -1, st, coeff, qmat, avail, 4, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[4], coeff, stride);
			str_map |= 0x20;
		}
		if ((c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 3) : -1, st, coeff, qmat, avail, 5, 2, 0xf)) != 0) {
			left = PACK(0, c1, 0);
			str_map |= 0x80;
			ac4x4transform_acdc_luma(luma + offset[5], coeff, stride);
		} else {
			left = 0;
		}
		if ((c4 = ResidualBlock(mb, c3, c0, st, coeff, qmat, avail, 6, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[6], coeff, stride);
			str_map |= 0x2000;
		}
		if ((c5 = ResidualBlock(mb, c4, c1, st, coeff, qmat, avail, 7, 2, 0xf)) != 0) {
			left = PACK(left, c5, 1);
			str_map |= 0x8000;
			ac4x4transform_acdc_luma(luma + offset[7], coeff, stride);
		}
	} else {
		c0 = 0;
		c1 = 0;
		c4 = 0;
		c5 = 0;
		left = 0;
	}
	if (cbp & 4) {
		if ((c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 2) : -1, c2, st, coeff, qmat, avail, 8, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[8], coeff, stride);
			str_map |= 0x20000;
		}
		if ((c1 = ResidualBlock(mb, c0, c3, st, coeff, qmat, avail, 9, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[9], coeff, stride);
			str_map |= 0x80000;
		}
		if ((c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 3) : -1, c0, st, coeff, qmat, avail, 10, 2, 0xf)) != 0) {
			top = PACK(0, c2, 0);
			str_map |= 0x2000000;
			ac4x4transform_acdc_luma(luma + offset[10], coeff, stride);
		} else {
			top = 0;
		}
		if ((c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail, 11, 2, 0xf)) != 0) {
			top = PACK(top, c3, 1);
			str_map |= 0x8000000;
			ac4x4transform_acdc_luma(luma + offset[11], coeff, stride);
		}
	} else {
		c0 = 0;
		c1 = 0;
		c2 = 0;
		c3 = 0;
		top = 0;
	}
	if (cbp & 8) {
		if ((c0 = ResidualBlock(mb, c1, c4, st, coeff, qmat, avail, 12, 2, 0xf)) != 0) {
			ac4x4transform_acdc_luma(luma + offset[12], coeff, stride);
			str_map |= 0x200000;
		}
		if ((c1 = ResidualBlock(mb, c0, c5, st, coeff, qmat, avail, 13, 2, 0xf)) != 0) {
			left = PACK(left, c1, 2);
			str_map |= 0x800000;
			ac4x4transform_acdc_luma(luma + offset[13], coeff, stride);
		}
		if ((c2 = ResidualBlock(mb, c3, c0, st, coeff, qmat, avail, 14, 2, 0xf)) != 0) {
			top = PACK(top, c2, 2);
			str_map |= 0x20000000;
			ac4x4transform_acdc_luma(luma + offset[14], coeff, stride);
		}
		if ((c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail, 15, 2, 0xf)) != 0) {
			str_map |= 0x80000000;
			ac4x4transform_acdc_luma(luma + offset[15], coeff, stride);
		}
	} else {
		c3 = 0; 
	}
	mb->left4x4coef = (mb->left4x4coef & 0xffff0000) | PACK(left, c3, 3);
	*mb->top4x4coef = (*mb->top4x4coef & 0xffff0000) | PACK(top, c3, 3);
	uint32_t str_horiz = transposition(str_map);
	mb->deblock_curr->str_vert = (str_map << 8) | str_map;
	mb->deblock_curr->str_horiz = (str_horiz << 8) | str_horiz;
}

struct residual_luma_inter {
	template <typename F0, typename F1, typename F2>
	void operator()(h264d_mb_current *mb, uint32_t cbp, dec_bits *st, int avail,
				    F0 Transform8x8Flag,
				    F1 QpDelta,
				    F2 ResidualBlock) const {
		int32_t qp_delta = QpDelta(mb, st, avail);
		if (qp_delta) {
			set_qp(mb, mb->qp + qp_delta);
		}
		residual_luma_inter4x4(mb, cbp, st, avail, ResidualBlock);
	}
};

static inline int8_t cbp_transposition8x8(uint32_t cbp_luma)
{
	static const int8_t transpos[16] = {
		0, 1, 4, 5,
		2, 3, 6, 7,
		8, 9, 12, 13,
		10, 11, 14, 15
	};
	return transpos[cbp_luma];
}

static inline uint32_t expand_str8x8(uint32_t cbp_luma) {
	static const uint32_t strmap[16] = {
		0x00000000, 0x000a000a, 0x00a000a0, 0x00aa00aa,
		0x000a0000, 0x000a000a, 0x00aa00a0, 0x00aa00aa,
		0x00a00000, 0x00aa000a, 0x00a000a0, 0x00aa00aa,
		0x00aa0000, 0x00aa000a, 0x00aa00a0, 0x00aa00aa
	};
	return strmap[cbp_luma];
}

template <typename F0>
static inline void residual_luma_inter8x8(h264d_mb_current *mb, uint32_t cbp, dec_bits *st, int avail,
				    F0 ResidualBlock)
{
	int coeff[64];
	const int16_t *qmat;
	const int *offset;
	uint32_t top, left;
	int c0, c1, c2, c3;
	int stride;

	qmat = mb->qmaty8x8;
	offset = mb->offset4x4;
	stride = mb->max_x * 16;
	cbp &= 15;
	if (cbp & 1) {
		c0 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 0) : -1, avail & 2 ? UNPACK(*mb->top4x4coef, 0) : -1, st, coeff, qmat, avail, 0, 5, 0x3f);
		ac8x8transform(mb->luma, coeff, stride, c0);
	} else {
		c0 = 0;
	}
	if (cbp & 2) {
		c1 = ResidualBlock(mb, c0, avail & 2 ? UNPACK(*mb->top4x4coef, 2) : -1, st, coeff, qmat, avail, 4, 5, 0x3f);
		ac8x8transform(mb->luma + 8, coeff, stride, c1);
		left = c1 * 0x11;
	} else {
		c1 = 0;
		left = 0;
	}
	if (cbp & 4) {
		c2 = ResidualBlock(mb, avail & 1 ? UNPACK(mb->left4x4coef, 2) : -1, c1, st, coeff, qmat, avail, 8, 5, 0x3f);
		ac8x8transform(mb->luma + offset[8], coeff, stride, c2);
		top = c2 * 0x11;
	} else {
		c2 = 0;
		top = 0;
	}
	if (cbp & 8) {
		c3 = ResidualBlock(mb, c2, c1, st, coeff, qmat, avail, 12, 5, 0x3f);
		ac8x8transform(mb->luma + offset[12], coeff, stride, c3);
		left |= c3 * 0x1100;
		top |= c3 * 0x1100;
	}
	mb->left4x4coef = (mb->left4x4coef & 0xffff0000) | left;
	*mb->top4x4coef = (*mb->top4x4coef & 0xffff0000) | top;
	mb->deblock_curr->str_horiz = expand_str8x8(cbp_transposition8x8(cbp));
	mb->deblock_curr->str_vert = expand_str8x8(cbp);
}

struct residual_luma_interNxN {
	template <typename F0, typename F1, typename F2>
	void operator()(h264d_mb_current *mb, uint32_t cbp, dec_bits *st, int avail,
				    F0 Transform8x8Flag,
				    F1 QpDelta,
				    F2 ResidualBlock) const {
		bool transform8x8mode = (0x80 < (cbp & 0x8f)) && Transform8x8Flag(mb, st, avail);
		int32_t qp_delta = QpDelta(mb, st, avail);
		if (qp_delta) {
			set_qp(mb, mb->qp + qp_delta);
		}
		mb->left4x4inter->transform8x8 = transform8x8mode;
		mb->top4x4inter->transform8x8 = transform8x8mode;
		if (transform8x8mode) {
			residual_luma_inter8x8(mb, cbp, st, avail, ResidualBlock);
		} else {
			residual_luma_inter4x4(mb, cbp, st, avail, ResidualBlock);
		}
	}
};

static inline int MEDIAN(int a, int b, int c)
{
	return (a <= b) ? ((b <= c) ? b : (a <= c ? c : a)) : (a <= c ? a : (b <= c ? c : b));
}

static const int16_t zero_mv[2 * 8] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0
};

static const int8_t non_ref[4] = {
	-1, -1, -1, -1
};

static const h264d_vector_t zero_mov[2] = {
	{{0, 0}}, {{0, 0}}
};

static inline void determine_pmv(const int16_t *mva, const int16_t *mvb, const int16_t *mvc, int16_t pmv[], int avail, int idx_map)
{
	static const int not_one_hot = 0xe9;
	int pmvx, pmvy;
	if (((avail & 7) == 1) || (idx_map == 1)) {
		pmvx = mva[0];
		pmvy = mva[1];
	} else if (not_one_hot & (1 << idx_map)) {
		pmvx = MEDIAN(mva[0], mvb[0], mvc[0]);
		pmvy = MEDIAN(mva[1], mvb[1], mvc[1]);
	} else if (idx_map == 2) {
		pmvx = mvb[0];
		pmvy = mvb[1];
	} else {
		pmvx = mvc[0];
		pmvy = mvc[1];
	}
	pmv[0] = pmvx;
	pmv[1] = pmvy;
}

static inline void calc_mv16x16(h264d_mb_current *mb, int16_t *pmv, const int16_t *&mvd_a, const int16_t *&mvd_b, int lx, int ref_idx, int avail)
{
	prev_mb_t *pmb;
	const int16_t *mva, *mvb, *mvc;
	int idx_map;

	if (avail & 1) {
		pmb = mb->left4x4inter;
		idx_map = (ref_idx == pmb->ref[0][lx]);
		mva = pmb->mov[0].mv[lx].v;
		mvd_a = pmb->mvd[0].mv[lx].v;
	} else {
		idx_map = 0;
		mvd_a = mva = zero_mv;
	}
	if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[0][lx]) * 2;
		mvb = pmb->mov[0].mv[lx].v;
		mvd_b = pmb->mvd[0].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}
	if (avail & 4) {
		pmb = mb->top4x4inter + 1;
		idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
		mvc = pmb->mov[0].mv[lx].v;
	} else if (avail & 8) {
		idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
		mvc = mb->lefttop_mv[lx].v;
	} else {
		mvc = zero_mv;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void inter_pred_basic(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety)
{
	int bidir = 0;
	int stride = mb->max_x * 16;
	int vert_size = mb->max_y * 16;
	uint8_t *dst_luma = mb->luma + offsety * stride + offsetx;
	uint8_t *dst_chroma = mb->chroma + (offsety >> 1) * stride + offsetx;
	offsetx = mb->x * 16 + offsetx;
	offsety = mb->y * 16 + offsety;
	for (int lx = 0; lx < 2; ++lx) {
		int idx;
		if ((idx = *ref_idx++) < 0) {
			continue;
		}
		m2d_frame_t *frms = &(mb->frame->frames[mb->frame->refs[lx][idx].frame_idx]);
		int mvx = mv[lx].v[0];
		int mvy = mv[lx].v[1];
		int posx = (mvx >> 2) + offsetx;
		int posy = (mvy >> 2) + offsety;
		inter_pred_luma[bidir][mvy & 3][mvx & 3](frms->luma + inter_pred_mvoffset_luma(posx - 2, posy - 2, stride), posx, posy, size, stride, vert_size, dst_luma, stride);
		inter_pred_chroma[bidir](frms->chroma, (mvx >> 3) * 2 + offsetx, (mvy >> 3) + (offsety >> 1), mv[lx], size, stride, vert_size >> 1, dst_chroma, stride);
		bidir++;
	}
}

#ifdef X86ASM

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
static void weighted_copy(const h264d_weighted_table_elem_t* elem, int shift, uint8_t *dst, int width, int height, int stride) __attribute__((noinline));
#endif

static void weighted_copy(const h264d_weighted_table_elem_t* elem, int shift, uint8_t *dst, int width, int height, int stride)
{
	__m128i w0, ofs, rnd, zero;
	int shift_org = shift;
	shift &= 15;
	rnd = _mm_cvtsi32_si128((1 << shift) >> 1);
	if (shift == shift_org) {
		w0 = _mm_cvtsi32_si128(elem[0].weight);
		ofs = _mm_cvtsi32_si128(elem[0].offset);
		w0 = _mm_shufflelo_epi16(w0, 0);
		ofs = _mm_shufflelo_epi16(ofs, 0);
	} else {
		w0 = _mm_cvtsi32_si128((elem[1].weight << 16) | (uint16_t)elem[0].weight);
		ofs = _mm_cvtsi32_si128((elem[1].offset << 16) | (uint16_t)elem[0].offset);
	}
	zero = w0;
	rnd = _mm_shufflelo_epi16(rnd, 0);
	w0 = _mm_shuffle_epi32(w0, 0);
	ofs = _mm_shuffle_epi32(ofs, 0);
	rnd = _mm_shuffle_epi32(rnd, 0);
	zero = _mm_xor_si128(zero, zero);
	if (width == 4) {
		do {
			__m128i r0 = _mm_cvtsi32_si128(*(uint32_t const *)dst);
			r0 = _mm_unpacklo_epi8(r0, zero);
			r0 = _mm_mullo_epi16(r0, w0);
			r0 = _mm_adds_epi16(r0, rnd);
			r0 = _mm_srai_epi16(r0, shift);
			r0 = _mm_adds_epi16(r0, ofs);
			r0 = _mm_packus_epi16(r0, zero);
			*(uint32_t *)dst = _mm_cvtsi128_si32(r0);
			dst += stride;
		} while (--height);
	} else {
		stride -= width;
		width = (unsigned)width >> 3;
		do {
			int x = width;
			do {
				__m128i r0 = _mm_loadl_epi64((__m128i const *)dst);
				r0 = _mm_unpacklo_epi8(r0, zero);
				r0 = _mm_mullo_epi16(r0, w0);
				r0 = _mm_adds_epi16(r0, rnd);
				r0 = _mm_srai_epi16(r0, shift);
				r0 = _mm_adds_epi16(r0, ofs);
				r0 = _mm_packus_epi16(r0, zero);
				_mm_storel_epi64((__m128i *)dst, r0);
				dst += 8;
			} while (--x);
			dst += stride;
		} while (--height);
	}
}
#else
template <int N>
static inline void weighted_copy_base(const h264d_weighted_table_elem_t& elem, int shift, uint8_t *dst, int width, int height, int stride)
{
	int w0 = elem.weight;
	int ofs = elem.offset;
	int rnd = shift ? (1 << (shift - 1)) : 0;
	stride -= width;
	width = (unsigned)width >> N;
	do {
		int x = width;
		do {
			dst[0] = CLIP255C(((dst[0] * w0 + rnd) >> shift) + ofs);
			dst[N] = CLIP255C(((dst[N] * w0 + rnd) >> shift) + ofs);
			dst += N * 2;
		} while (--x);
		dst += stride;
	} while (--height);
}

static inline void weighted_copy(const h264d_weighted_table_elem_t* elem, int shift, uint8_t *dst, int width, int height, int stride)
{
	if ((shift & 256) == 0) {
		weighted_copy_base<1>(elem[0], shift, dst, width, height, stride);
	} else {
		shift &= 15;
		weighted_copy_base<2>(elem[0], shift, dst, width, height, stride);
		weighted_copy_base<2>(elem[1], shift, dst + 1, width, height, stride);
	}
}
#endif

static inline void inter_pred_weighted_onedir(const h264d_mb_current *mb, int frame_idx, const h264d_vector_t& mv, const h264d_vector_t& size, int offsetx, int offsety, const h264d_weighted_pred_t& pred)
{
	const m2d_frame_t& frms = mb->frame->frames[frame_idx];
	int stride = mb->max_x * 16;
	int vert_size = mb->max_y * 16;
	int ofsx = mb->x * 16 + offsetx;
	int ofsy = mb->y * 16 + offsety;
	int mvx = mv.v[0];
	int mvy = mv.v[1];
	int posx = (mvx >> 2) + ofsx;
	int posy = (mvy >> 2) + ofsy;
	uint8_t *dst = mb->luma + offsety * stride + offsetx;
	inter_pred_luma[0][mvy & 3][mvx & 3](frms.luma + inter_pred_mvoffset_luma(posx - 2, posy - 2, stride), posx, posy, size, stride, vert_size, dst, stride);
	weighted_copy(&pred.weight_offset.e[0], pred.shift[0], dst, size.v[0], size.v[1], stride);
	dst = mb->chroma + (offsety >> 1) * stride + offsetx;
	inter_pred_chroma[0](frms.chroma, (mvx >> 3) * 2 + ofsx, (mvy >> 3) + (ofsy >> 1), mv, size, stride, vert_size >> 1, dst, stride);
	weighted_copy(&pred.weight_offset.e[1], pred.shift[1] | 256, dst, size.v[0], size.v[1] >> 1, stride);
}

template <typename F0, typename F1>
static inline void inter_pred_weighted_bidir(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety, const h264d_weighted_pred_t *pred,
						F0 AddBidirWeightedLuma,
						F1 AddBidirWeightedChroma)
{
	uint8_t ALIGN16VC luma_buf[16 * 16] __attribute__((aligned(16)));
	uint8_t ALIGN16VC chroma_buf[16 * 8] __attribute__((aligned(16)));
	int stride = mb->max_x * 16;
	int vert_size = mb->max_y * 16;
	int ofsx = mb->x * 16 + offsetx;
	int ofsy = mb->y * 16 + offsety;
	int mvx = mv[0].v[0];
	int mvy = mv[0].v[1];
	int posx = (mvx >> 2) + ofsx;
	int posy = (mvy >> 2) + ofsy;
	const m2d_frame_t *frms = &mb->frame->frames[mb->frame->refs[0][ref_idx[0]].frame_idx];
	uint8_t *dst_luma = mb->luma + offsety * stride + offsetx;
	inter_pred_luma[0][mvy & 3][mvx & 3](frms->luma + inter_pred_mvoffset_luma(posx - 2, posy - 2, stride), posx, posy, size, stride, vert_size, dst_luma, stride);
	uint8_t *dst_chroma = mb->chroma + (offsety >> 1) * stride + offsetx;
	inter_pred_chroma[0](frms->chroma, (mvx >> 3) * 2 + ofsx, (mvy >> 3) + (ofsy >> 1), mv[0], size, stride, vert_size >> 1, dst_chroma, stride);
	mvx = mv[1].v[0];
	mvy = mv[1].v[1];
	posx = (mvx >> 2) + ofsx;
	posy = (mvy >> 2) + ofsy;
	frms = &mb->frame->frames[mb->frame->refs[1][ref_idx[1]].frame_idx];
	inter_pred_luma[0][mvy & 3][mvx & 3](frms->luma + inter_pred_mvoffset_luma(posx - 2, posy - 2, stride), posx, posy, size, stride, vert_size, luma_buf, size.v[0]);
	inter_pred_chroma[0](frms->chroma, (mvx >> 3) * 2 + ofsx, (mvy >> 3) + (ofsy >> 1), mv[1], size, stride, vert_size >> 1, chroma_buf, size.v[0]);
	AddBidirWeightedLuma(pred, luma_buf, dst_luma, size.v[0], size.v[1], stride);
	AddBidirWeightedChroma(pred, chroma_buf, dst_chroma, size.v[0], size.v[1] >> 1, stride);
}

template <int N>
struct add_bidir_weighted_type1 {
	void operator()(const h264d_weighted_pred_t pred[], const uint8_t *src1, uint8_t *dst, int width, int height, int stride) const {
#ifdef X86ASM
		const h264d_weighted_table_elem_t* e0 = &pred[0].weight_offset.e[N - 1];
		const h264d_weighted_table_elem_t* e1 = &pred[1].weight_offset.e[N - 1];
		int shift = pred[0].shift[N - 1];
		__m128i w0 = _mm_cvtsi32_si128((e0[N / 2].weight << 16) | (uint16_t)e0[0].weight);
		__m128i w1 = _mm_cvtsi32_si128((e1[N / 2].weight << 16) | (uint16_t)e1[0].weight);
		__m128i rnd = _mm_cvtsi32_si128(1 << shift++);
		__m128i ofs = _mm_cvtsi32_si128((((e0[N / 2].offset + e1[N / 2].offset + 1) >> 1) << 16) | (uint16_t)((e0[0].offset + e1[0].offset + 1) >> 1));
		__m128i zero = w0;
		rnd = _mm_shufflelo_epi16(rnd, 0);
		w0 = _mm_shuffle_epi32(w0, 0);
		w1 = _mm_shuffle_epi32(w1, 0);
		ofs = _mm_shuffle_epi32(ofs, 0);
		zero = _mm_xor_si128(zero, zero);
		if (width == 4) {
			do {
				__m128i r0 = _mm_cvtsi32_si128(*(uint32_t const *)dst);
				__m128i r1 = _mm_cvtsi32_si128(*(uint32_t const *)src1);
				src1 += 4;
				r0 = _mm_unpacklo_epi8(r0, zero);
				r1 = _mm_unpacklo_epi8(r1, zero);
				r0 = _mm_mullo_epi16(r0, w0);
				r1 = _mm_mullo_epi16(r1, w1);
				r0 = _mm_adds_epi16(r0, rnd);
				r0 = _mm_adds_epi16(r0, r1);
				r0 = _mm_srai_epi16(r0, shift);
				r0 = _mm_adds_epi16(r0, ofs);
				r0 = _mm_packus_epi16(r0, zero);
				*(uint32_t *)dst = _mm_cvtsi128_si32(r0);
				dst += stride;
			} while (--height);
		} else {
			rnd = _mm_shuffle_epi32(rnd, 0);
			stride -= width;
			width = (unsigned)width >> 3;
			do {
				int x = width;
				do {
					__m128i r0 = _mm_loadl_epi64((__m128i const *)dst);
					__m128i r1 = _mm_loadl_epi64((__m128i const *)src1);
					src1 += 8;
					r0 = _mm_unpacklo_epi8(r0, zero);
					r1 = _mm_unpacklo_epi8(r1, zero);
					r0 = _mm_mullo_epi16(r0, w0);
					r1 = _mm_mullo_epi16(r1, w1);
					r0 = _mm_adds_epi16(r0, rnd);
					r0 = _mm_adds_epi16(r0, r1);
					r0 = _mm_srai_epi16(r0, shift);
					r0 = _mm_adds_epi16(r0, ofs);
					r0 = _mm_packus_epi16(r0, zero);
					_mm_storel_epi64((__m128i *)dst, r0);
					dst += 8;
				} while (--x);
				dst += stride;
			} while (--height);
		}
#else
		const h264d_weighted_table_elem_t* e0 = &pred[0].weight_offset.e[N - 1];
		const h264d_weighted_table_elem_t* e1 = &pred[1].weight_offset.e[N - 1];
		int shift = pred[0].shift[N - 1];
		int wa0 = e0[0].weight;
		int wb0 = e0[N / 2].weight;
		int wa1 = e1[0].weight;
		int wb1 = e1[N / 2].weight;
		int ofsa = (e0[0].offset + e1[0].offset + 1) >> 1;
		int ofsb = (e0[N / 2].offset + e1[N / 2].offset + 1) >> 1;
		int rnd = 1 << shift++;
		stride -= width;
		width = (unsigned)width >> 2;
		do {
			int x = width;
			do {
				dst[0] = CLIP255C(((*src1++ * wa1 + dst[0] * wa0 + rnd) >> shift) + ofsa);
				dst[1] = CLIP255C(((*src1++ * wb1 + dst[1] * wb0 + rnd) >> shift) + ofsb);
				dst[2] = CLIP255C(((*src1++ * wa1 + dst[2] * wa0 + rnd) >> shift) + ofsa);
				dst[3] = CLIP255C(((*src1++ * wb1 + dst[3] * wb0 + rnd) >> shift) + ofsb);
				dst += 4;
			} while (--x);
			dst += stride;
		} while (--height);
#endif
	}
};

static inline void inter_pred_weighted1(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety)
{
	int ref0 = ref_idx[0];
	int ref1 = ref_idx[1];
	const h264d_weighted_table_t *tbl = &mb->header->pred_weighted_info.type1;
	h264d_weighted_pred_t pred[2];
	pred[0].shift[0] = tbl->shift[0];
	pred[0].shift[1] = tbl->shift[1];
	if (0 <= ref0) {
		pred[0].weight_offset = tbl->weight[ref0][0];
		if (0 <= ref1) {
			pred[1].weight_offset = tbl->weight[ref1][1];
			inter_pred_weighted_bidir(mb, ref_idx, mv, size, offsetx, offsety, pred, add_bidir_weighted_type1<1>(), add_bidir_weighted_type1<2>());
		} else {
			inter_pred_weighted_onedir(mb, mb->frame->refs[0][ref0].frame_idx, mv[0], size, offsetx, offsety, pred[0]);
		}
	} else {
		pred[0].weight_offset = tbl->weight[ref1][1];
		inter_pred_weighted_onedir(mb, mb->frame->refs[1][ref1].frame_idx, mv[1], size, offsetx, offsety, pred[0]);
	}
}

static inline void pred_weight_type2(h264d_weighted_cache_t *weighted, const h264d_mb_current *mb, int idx0, int idx1)
{
	const h264d_ref_frame_t *refs0 = &mb->frame->refs[0][idx0];
	const h264d_ref_frame_t *refs1 = &mb->frame->refs[1][idx1];
	int poc0 = refs0->poc;
	int poc1 = refs1->poc;
	int w0, w1;

	weighted->idx[0] = idx0;
	weighted->idx[1] = idx1;
	if ((poc0 == poc1) || (refs0->in_use != SHORT_TERM) || (refs1->in_use != SHORT_TERM)) {
		w0 = 32;
		w1 = 32;
	} else {
		w1 = dist_scale_factor(poc0, poc1, mb->header->poc) >> 2;
		if ((w1 < -64) || (128 < w1)) {
			w0 = 32;
			w1 = 32;
		} else {
			w0 = 64 - w1;
		}
	}
	weighted->weight[0] = w0;
	weighted->weight[1] = w1;
}

struct add_bidir_weighted_type2 {
	void operator()(const h264d_weighted_pred_t pred[], const uint8_t *src1, uint8_t *dst, int width, int height, int stride) const {
#ifdef X86ASM
		__m128i w0, w1, rnd, r0, r1, zero;
		w0 = _mm_cvtsi32_si128(pred[0].weight_offset.e[0].weight);
		w1 = _mm_cvtsi32_si128(pred[1].weight_offset.e[0].weight);
		rnd = _mm_cvtsi32_si128(32);
		w0 = _mm_shufflelo_epi16(w0, 0);
		w1 = _mm_shufflelo_epi16(w1, 0);
		rnd = _mm_shufflelo_epi16(rnd, 0);
		zero = w0;
		zero = _mm_xor_si128(zero, zero);
		if (width == 4) {
			do {
				r0 = _mm_cvtsi32_si128(*(uint32_t const *)dst);
				r1 = _mm_cvtsi32_si128(*(uint32_t const *)src1);
				src1 += 4;
				r0 = _mm_unpacklo_epi8(r0, zero);
				r1 = _mm_unpacklo_epi8(r1, zero);
				r0 = _mm_mullo_epi16(r0, w0);
				r1 = _mm_mullo_epi16(r1, w1);
				r0 = _mm_adds_epi16(r0, rnd);
				r0 = _mm_adds_epi16(r0, r1);
				r0 = _mm_srai_epi16(r0, 6);
				r0 = _mm_packus_epi16(r0, zero);
				*(uint32_t *)dst = _mm_cvtsi128_si32(r0);
				dst += stride;
			} while (--height);
		} else {
			w0 = _mm_shuffle_epi32(w0, 0);
			w1 = _mm_shuffle_epi32(w1, 0);
			rnd = _mm_shuffle_epi32(rnd, 0);
			stride -= width;
			width = (unsigned)width >> 3;
			do {
				int x = width;
				do {
					r0 = _mm_loadl_epi64((__m128i const *)dst);
					r1 = _mm_loadl_epi64((__m128i const *)src1);
					src1 += 8;
					r0 = _mm_unpacklo_epi8(r0, zero);
					r1 = _mm_unpacklo_epi8(r1, zero);
					r0 = _mm_mullo_epi16(r0, w0);
					r1 = _mm_mullo_epi16(r1, w1);
					r0 = _mm_adds_epi16(r0, rnd);
					r0 = _mm_adds_epi16(r0, r1);
					r0 = _mm_srai_epi16(r0, 6);
					r0 = _mm_packus_epi16(r0, zero);
					_mm_storel_epi64((__m128i *)dst, r0);
					dst += 8;
				} while (--x);
				dst += stride;
			} while (--height);
		}
#else
		int w0 = pred[0].weight_offset.e[0].weight;
		int w1 = pred[1].weight_offset.e[0].weight;
		stride -= width;
		width = (unsigned)width >> 2;
		do {
			int x = width;
			do {
				dst[0] = CLIP255C((*src1++ * w1 + dst[0] * w0 + (1 << 5)) >> 6);
				dst[1] = CLIP255C((*src1++ * w1 + dst[1] * w0 + (1 << 5)) >> 6);
				dst[2] = CLIP255C((*src1++ * w1 + dst[2] * w0 + (1 << 5)) >> 6);
				dst[3] = CLIP255C((*src1++ * w1 + dst[3] * w0 + (1 << 5)) >> 6);
				dst += 4;
			} while (--x);
			dst += stride;
		} while (--height);
#endif
	}
};

static inline void inter_pred_weighted2(const h264d_mb_current *mb, const int8_t ref_idx[], const h264d_vector_t mv[], const h264d_vector_t& size, int offsetx, int offsety)
{
	int idx0 = ref_idx[0];
	int idx1 = ref_idx[1];
	h264d_weighted_cache_t *weighted = &mb->header->pred_weighted_info.type2;
	if ((0 <= idx0) && (0 <= idx1)) {
		if ((weighted->idx[0] != idx0) || (weighted->idx[1] != idx1)) {
			pred_weight_type2(weighted, mb, idx0, idx1);
		}
		h264d_weighted_pred_t pred[2];
		pred[0].weight_offset.e[0].weight = weighted->weight[0];
		pred[1].weight_offset.e[0].weight = weighted->weight[1];
		inter_pred_weighted_bidir(mb, weighted->idx, mv, size, offsetx, offsety, pred, add_bidir_weighted_type2(), add_bidir_weighted_type2());
	} else {
		inter_pred_basic(mb, ref_idx, mv, size, offsetx, offsety);
	}
}

static inline uint32_t str_previous_coef(uint32_t map, uint32_t prev4x4)
{
	if (prev4x4) {
		for (int i = 0; i < 4; ++i) {
			if ((prev4x4 & 0xf) != 0) {
				map |= 2 << (i * 2);
			}
			prev4x4 >>= 4;
		}
	}
	return map;
}

static inline int DIF_SQUARE(int a, int b) {
	int t = a - b;
	return t * t;
}

static inline bool DIF_ABS_LARGER_THAN4(int a, int b) {
	return 16 <= DIF_SQUARE(a, b);
}

template <int MV_STEP>
static inline uint32_t str_mv_calc16x16_bidir_both(uint32_t str, int offset, const h264d_vector_set_t *mvxy, const prev_mb_t *prev)
{
	uint32_t mask = 2 << (offset * 2);
	for (int j = 0; j < 2; ++j) {
		if (!(str & mask)) {
			const int16_t *mv_prev = &(prev->mov[j + offset].mv[0].v[0]);
			int prev0x = *mv_prev++;
			int prev0y = *mv_prev++;
			int prev1x = *mv_prev++;
			int prev1y = *mv_prev;
			if ((DIF_ABS_LARGER_THAN4(mvxy->mv[0].v[0], prev0x)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[0].v[1], prev0y)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[1].v[0], prev1x)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[1].v[1], prev1y))
				&& (DIF_ABS_LARGER_THAN4(mvxy->mv[0].v[0], prev1x)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[0].v[1], prev1y)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[1].v[0], prev0x)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[1].v[1], prev0y))) {
				str = str | (mask >> 1);
			}
		}
		mask <<= 2;
		mvxy += MV_STEP;
	}
	return str;
}

template <int MV_STEP>
static inline uint32_t str_mv_calc16x16_bidir_one(uint32_t str, int ref0, int prev_ref0, int offset, const h264d_vector_set_t *mvxy, const prev_mb_t *prev)
{
	int lx0 = (ref0 != prev_ref0);
	int lx1 = lx0 ^ 1;
	uint32_t mask = 2 << (offset * 2);
	for (int j = 0; j < 2; ++j) {
		if (!(str & mask)) {
			const int16_t *mv_prev = &(prev->mov[j + offset].mv[0].v[0]);
			if (DIF_ABS_LARGER_THAN4(mvxy->mv[lx0].v[0], *mv_prev++)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[lx0].v[1], *mv_prev++)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[lx1].v[0], *mv_prev++)
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[lx1].v[1], *mv_prev)) {
				str = str | (mask >> 1);
			}
		}
		mask <<= 2;
		mvxy += MV_STEP;
	}
	return str;
}

template <int MV_STEP>
static inline uint32_t str_mv_calc16x16_bidir(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const h264d_vector_set_t *mvxy, const prev_mb_t *prev)
{
	if (ref0 == ref1) {
		return str_mv_calc16x16_bidir_both<MV_STEP>(str, offset, mvxy, prev);
	} else {
		return str_mv_calc16x16_bidir_one<MV_STEP>(str, ref0, prev_ref0, offset, mvxy, prev);
	}
}

template <int MV_STEP>
static inline uint32_t str_mv_calc16x16_onedir(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const h264d_vector_set_t *mvxy, const prev_mb_t *prev)
{
	int lx_curr, lx_prev;
	if (0 <= ref0) {
		lx_curr = 0;
		lx_prev = (ref0 != prev_ref0);
	} else {
		lx_curr = 1;
		lx_prev = (ref1 != prev_ref0);
	}
	uint32_t mask = 2 << (offset * 2);
	for (int j = 0; j < 2; ++j) {
		if ((str & mask) == 0) {
			if (DIF_ABS_LARGER_THAN4(mvxy->mv[lx_curr].v[0], prev->mov[j + offset].mv[lx_prev].v[0])
				|| DIF_ABS_LARGER_THAN4(mvxy->mv[lx_curr].v[1], prev->mov[j + offset].mv[lx_prev].v[1])) {
				str |= mask >> 1;
			}
		}
		mask <<= 2;
		mvxy += MV_STEP;
	}
	return str;
}

static inline int frame_idx_of_ref(const h264d_mb_current *mb, int ref_idx, int lx) {
	return (0 <= ref_idx) ? mb->frame->refs[lx][ref_idx].frame_idx : -1;
}

template <int MV_STEP>
static inline uint32_t str_mv_calc16x16_mv(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const h264d_vector_set_t *mvxy, const prev_mb_t *prev)
{
	if ((0 <= ref0) && (0 <= ref1)) {
		return str_mv_calc16x16_bidir<MV_STEP>(str, ref0, ref1, prev_ref0, offset, mvxy, prev);
	} else {
		return str_mv_calc16x16_onedir<MV_STEP>(str, ref0, ref1, prev_ref0, offset, mvxy, prev);
	}
}

static inline uint32_t str_mv_calc16x16(const h264d_mb_current *mb, uint32_t str, const h264d_vector_set_t *mvxy, const int8_t ref_idx[], const prev_mb_t *prev)
{
	int ref0 = frame_idx_of_ref(mb, *ref_idx++, 0);
	int ref1 = frame_idx_of_ref(mb, *ref_idx, 1);
	uint32_t mask = 0xa;
	for (int i = 0; i < 2; ++i) {
		if ((str & mask) != mask) {
			int prev0 = prev->frmidx[i][0];
			int prev1 = prev->frmidx[i][1];
			if (((prev0 != ref0) || (prev1 != ref1)) && ((prev1 != ref0) || (prev0 != ref1))) {
				uint32_t m = mask >> 1;
				str = str | (((str >> 1) ^ m) & m);
			} else {
				str = str_mv_calc16x16_mv<0>(str, ref0, ref1, prev0, i * 2, mvxy, prev);
			}
		}
		mask <<= 4;
	}
	return str;
}

static void store_str_inter16xedge(const h264d_mb_current *mb, const prev_mb_t *prev4x4inter, int8_t& str4, const h264d_vector_set_t *mv, const int8_t ref_idx[], uint32_t& str, uint32_t coeff4x4)
{
	if (prev4x4inter->type <= MB_IPCM) {
		str4 = 1;
		str |= 0xaa;
	} else {
		str = str_previous_coef(str, coeff4x4);
		str = str_mv_calc16x16(mb, str, mv, ref_idx, prev4x4inter);
	}
}

static void store_info_inter16x16(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4)
{
	deblock_info_t *deb = mb->deblock_curr;
	deb->qpy = mb->qp;
	deb->qpc[0] = mb->qp_chroma[0];
	deb->qpc[1] = mb->qp_chroma[1];
	if (mb->y != 0) {
		store_str_inter16xedge(mb, mb->top4x4inter, deb->str4_vert, mv, ref_idx, deb->str_vert, top4x4);
	}
	if (mb->x != 0) {
		store_str_inter16xedge(mb, mb->left4x4inter, deb->str4_horiz, mv, ref_idx, deb->str_horiz, left4x4);
	}
	*mb->top4x4pred = 0x22222222;
	mb->left4x4pred = 0x22222222;

	mb->left4x4inter->direct8x8 = 0;
	mb->top4x4inter->direct8x8 = 0;
	for (int i = 0; i < 2; ++i) {
		mb->lefttop_ref[i] = mb->top4x4inter->ref[1][i];
		mb->lefttop_mv[i].vector = mb->top4x4inter->mov[3].mv[i].vector;
		int ref = ref_idx[i];
		int frm_idx = frame_idx_of_ref(mb, ref, i);
		for (int j = 0; j < 2; ++j) {
			mb->top4x4inter->ref[j][i] = ref;
			mb->top4x4inter->frmidx[j][i] = frm_idx;
			mb->left4x4inter->ref[j][i] = ref;
			mb->left4x4inter->frmidx[j][i] = frm_idx;
		}
	}
	for (int i = 0; i < 4; ++i) {
		mb->left4x4inter->mov[i] = mv[0];
		mb->left4x4inter->mvd[i] = mv[1];
		mb->top4x4inter->mov[i] = mv[0];
		mb->top4x4inter->mvd[i] = mv[1];
	}
	int refcol;
	uint32_t mvcol;
	if (0 <= ref_idx[0]) {
		refcol = ref_idx[0];
		mvcol = mv->mv[0].vector;
	} else {
		refcol = ref_idx[1];
		mvcol = mv->mv[1].vector;
	}
	mb->col_curr->type = COL_MB16x16;
	memset(mb->col_curr->ref, refcol, sizeof(mb->col_curr->ref));
	h264d_vector_t *mvdst = mb->col_curr->mv;
	for (int i = 0; i < 16; ++i) {
		mvdst[i].vector = mvcol;
	}
}

static inline void no_residual_inter(h264d_mb_current *mb)
{
	mb->prev_qp_delta = 0;
	mb->left4x4coef = 0;
	*mb->top4x4coef = 0;
	mb->left4x4inter->transform8x8 = 0;
	mb->top4x4inter->transform8x8 = 0;
	mb->deblock_curr->str_horiz = 0;
	mb->deblock_curr->str_vert = 0;
}

template <typename F0 ,typename F1, typename F2, typename F3, typename F4, typename F5, typename F6>
static int mb_inter16x16(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
			 F0 RefIdx16x16,
			 F1 MvdXY,
			 F2 CodedBlockPattern,
			 F3 ResidualLumaInter,
			 F4 Transform8x8Flag,
			 F5 QpDelta,
			 F6 ResidualBlock)
{
	const int16_t *mvd_a;
	const int16_t *mvd_b;
	h264d_vector_set_t mv[2];
	uint32_t predmap;
	uint32_t cbp;
	int8_t ref_idx[2];
	static const h264d_vector_t size = {{16, 16}};

	predmap = mbc->cbp;
	for (int lx = 0; lx < 2; ++lx) {
		ref_idx[lx] = (predmap & (1 << lx)) ? RefIdx16x16(mb, st, lx, avail) : -1;
	}
	memset(&mv, 0, sizeof(mv));
	for (int lx = 0; lx < 2; ++lx) {
		if (predmap & (1 << lx)) {
			calc_mv16x16(mb, mv[0].mv[lx].v, mvd_a, mvd_b, lx, ref_idx[lx], avail);
			MvdXY(mb, st, mv[1].mv[lx].v, mvd_a, mvd_b);
			mv[0].mv[lx].v[0] += mv[1].mv[lx].v[0];
			mv[0].mv[lx].v[1] += mv[1].mv[lx].v[1];
		}
	}
	mb->inter_pred(mb, ref_idx, mv[0].mv, size, 0, 0);
	uint32_t left4x4 = mb->left4x4coef;
	uint32_t top4x4 = *mb->top4x4coef;
	mb->cbp = cbp = CodedBlockPattern(mb, st, avail);
	if (cbp) {
		ResidualLumaInter(mb, 0x80 | cbp, st, avail, Transform8x8Flag, QpDelta, ResidualBlock);
	} else {
		no_residual_inter(mb);
	}
	store_info_inter16x16(mb, &mv[0], ref_idx, left4x4, top4x4);
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

static inline void calc_mv16x8top(h264d_mb_current *mb, int16_t pmv[], const int16_t *&mvd_a, const int16_t *&mvd_b, int lx, int ref_idx, int avail)
{
	prev_mb_t *pmb;
	const int16_t *mva, *mvb, *mvc;
	int idx_map;

	if (avail & 2) {
		pmb = mb->top4x4inter;
		mvd_b = pmb->mvd[0].mv[lx].v;
		if (ref_idx == pmb->ref[0][lx]) {
			pmv[0] = pmb->mov[0].mv[lx].v[0];
			pmv[1] = pmb->mov[0].mv[lx].v[1];
			mvd_a = (avail & 1) ? mb->left4x4inter->mvd[0].mv[lx].v : zero_mv;
			return;
		}
		mvb = pmb->mov[0].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}
	if (avail & 1) {
		pmb = mb->left4x4inter;
		idx_map = (ref_idx == pmb->ref[0][lx]);
		mva = pmb->mov[0].mv[lx].v;
		mvd_a = pmb->mvd[0].mv[lx].v;
	} else {
		mvd_a = mva = zero_mv;
		idx_map = 0;
	}
	if (avail & 4) {
		pmb = mb->top4x4inter + 1;
		idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
		mvc = pmb->mov[0].mv[lx].v;
	} else if (avail & 8) {
		idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
		mvc = mb->lefttop_mv[lx].v;
	} else {
		mvc = zero_mv;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void calc_mv16x8bottom(h264d_mb_current *mb, int16_t mv[], const int16_t *&mvd_a, const int16_t *&mvd_b, int lx, int ref_idx, int avail, int prev_ref, const h264d_vector_set_t *prev_mv)
{
	prev_mb_t *pmb;
	const int16_t *mva, *mvb, *mvc;
	int idx_map;

	if (avail & 1) {
		pmb = mb->left4x4inter;
		mvd_a = pmb->mvd[2].mv[lx].v;
		if (ref_idx == pmb->ref[1][lx]) {
			mv[0] = pmb->mov[2].mv[lx].v[0];
			mv[1] = pmb->mov[2].mv[lx].v[1];
			mvd_b = prev_mv[2].mv[lx].v;
			return;
		}
		idx_map = (ref_idx == pmb->ref[0][lx]) * 4;
		mva = pmb->mov[2].mv[lx].v;
		mvc = pmb->mov[1].mv[lx].v;
	} else {
		idx_map = 0;
		mvd_a = mva = zero_mv;
		mvc = zero_mv;
	}
	/* upper block: always exists */
	mvb = prev_mv->mv[lx].v;
	mvd_b = prev_mv[2].mv[lx].v;
	idx_map |= (ref_idx == prev_ref) * 2;
	avail |= 2;
	determine_pmv(mva, mvb, mvc, mv, avail, idx_map);
}

template <int IS_HORIZ, int MV_WIDTH, int MV_STEP2>
static inline uint32_t str_mv_calc16x8_left(const h264d_mb_current *mb, uint32_t str, const int8_t ref_idx[], const h264d_vector_set_t *mv, const prev_mb_t *prev)
{
	for (int i = 0; i < 2; ++i) {
		uint32_t mask = 0xa << (i * 4);
		if ((str & mask) != mask) {
			int prev_ref0 = prev->frmidx[i][0];
			int prev_ref1 = prev->frmidx[i][1];
			int ref0 = frame_idx_of_ref(mb, ref_idx[0], 0);
			int ref1 = frame_idx_of_ref(mb, ref_idx[1], 1);
			if (((prev_ref0 != ref0) || (prev_ref1 != ref1))
			    && ((prev_ref1 != ref0) || (prev_ref0 != ref1))) {
				uint32_t m = mask >> 1;
				str |= (((str >> 1) ^ m) & m);
			} else {
				str = str_mv_calc16x16_mv<MV_WIDTH * (MV_STEP2 / 2)>(str, ref0, ref1, prev_ref0, i * 2, &mv[0], prev);
			}
		}
		ref_idx += (MV_WIDTH == 1) ? 2 : 4;
		mv += MV_WIDTH * MV_STEP2;
	}
	return str;
}


static inline bool is_str_mv_calc16x8_center_bidir(int top_ref0, int bot_ref0, const h264d_vector_set_t *mv)
{
	const int16_t *mv_top0, *mv_top1;
	const int16_t *mv_bot0, *mv_bot1;
	if (top_ref0 == bot_ref0) {
		mv_top0 = mv[0].mv[0].v;
		mv_top1 = mv[0].mv[1].v;
	} else {
		mv_top1 = mv[0].mv[0].v;
		mv_top0 = mv[0].mv[1].v;
	}
	mv_bot0 = mv[1].mv[0].v;
	mv_bot1 = mv[1].mv[1].v;
	return (DIF_ABS_LARGER_THAN4(*mv_top0++, *mv_bot0++)
		|| DIF_ABS_LARGER_THAN4(*mv_top1++, *mv_bot1++)
		|| DIF_ABS_LARGER_THAN4(*mv_top0, *mv_bot0)
		|| DIF_ABS_LARGER_THAN4(*mv_top1, *mv_bot1));
}

static inline bool is_str_mv_calc16x8_center_onedir(int top_ref0, int bot_ref0, const h264d_vector_set_t *mv)
{
	const int16_t *top_mv = mv[0].mv[top_ref0 < 0].v;
	const int16_t *bot_mv = mv[1].mv[bot_ref0 < 0].v;
	return (DIF_ABS_LARGER_THAN4(*top_mv++, *bot_mv++)
		|| DIF_ABS_LARGER_THAN4(*top_mv, *bot_mv));
}

static inline uint32_t str_mv_calc16x8_vert(const h264d_mb_current *mb, uint32_t str, const int8_t ref_idx[], const h264d_vector_set_t *mv)
{
	if ((str & 0xaa0000) == 0xaa0000) {
		return str;
	}
	int top_ref0 = frame_idx_of_ref(mb, *ref_idx++, 0);
	int top_ref1 = frame_idx_of_ref(mb, *ref_idx++, 1);
	int bot_ref0 = frame_idx_of_ref(mb, *ref_idx++, 0);
	int bot_ref1 = frame_idx_of_ref(mb, *ref_idx, 1);
	if ((((top_ref0 != bot_ref0) || (top_ref1 != bot_ref1)) && ((top_ref1 != bot_ref0) || (top_ref0 != bot_ref1)))
		|| (((0 <= top_ref0) && (0 <= top_ref1)) ? is_str_mv_calc16x8_center_bidir : is_str_mv_calc16x8_center_onedir)(top_ref0, bot_ref0, mv)) {
		uint32_t mask = 0x550000;
		str |= (((str >> 1) ^ mask) & mask);
	}
	return str;
}

static inline void store_col16x8(h264d_col_mb_t *col, const int8_t *ref_idx, const h264d_vector_set_t *mv)
{
	h264d_vector_t *mvdst = col->mv;
	int8_t *refdst = col->ref;
	col->type = COL_MB16x8;
	for (int y = 0; y < 2; ++y) {
		int refcol;
		uint32_t mvcol;
		if (0 <= ref_idx[0]) {
			refcol = ref_idx[0];
			mvcol = mv[y].mv[0].vector;
		} else {
			refcol = ref_idx[1];
			mvcol = mv[y].mv[1].vector;
		}
		refdst[0] = refcol;
		refdst[1] = refcol;
		for (int i = 0; i < 16 / 2; ++i) {
			mvdst[i].vector = mvcol; 
		}
		ref_idx += 2;
		refdst += 2;
		mvdst += 8;
	}
}

template <int IS_HORIZ, int MV_WIDTH, int MV_STEP2>
static void store_str_inter8xedge(const h264d_mb_current *mb, const prev_mb_t *prev4x4inter, int8_t& str4,  const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t& str, uint32_t coeff4x4)
{
	if (prev4x4inter->type <= MB_IPCM) {
		str4 = 1;
		str |= 0xaa;
	} else {
		str = str_previous_coef(str, coeff4x4);
		str = str_mv_calc16x8_left<IS_HORIZ, MV_WIDTH, MV_STEP2>(mb, str, ref_idx, mv, prev4x4inter);
	}
}

static void store_info_inter16x8(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4)
{
	deblock_info_t *deb = mb->deblock_curr;
	deb->qpy = mb->qp;
	deb->qpc[0] = mb->qp_chroma[0];
	deb->qpc[1] = mb->qp_chroma[1];
	if (mb->y != 0) {
		store_str_inter16xedge(mb, mb->top4x4inter, deb->str4_vert, mv, ref_idx, deb->str_vert, top4x4);
	}
	deb->str_vert = str_mv_calc16x8_vert(mb, deb->str_vert, ref_idx, mv);
	if (mb->x != 0) {
		store_str_inter8xedge<1, 1, 1>(mb, mb->left4x4inter, deb->str4_horiz, mv, ref_idx, deb->str_horiz, left4x4);
	}
	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	mb->lefttop_ref[0] = mb->top4x4inter->ref[1][0];
	mb->lefttop_ref[1] = mb->top4x4inter->ref[1][1];
	mb->lefttop_mv[0].vector = mb->top4x4inter->mov[3].mv[0].vector;
	mb->lefttop_mv[1].vector = mb->top4x4inter->mov[3].mv[1].vector;
	mb->left4x4inter->direct8x8 = 0;
	mb->top4x4inter->direct8x8 = 0;
	for (int i = 0; i < 4; ++i) {
		mb->top4x4inter->mov[i] = mv[1];
		mb->top4x4inter->mvd[i] = mv[3];
	}
	int ref2 = ref_idx[2];
	int ref3 = ref_idx[3];
	int frm2 = frame_idx_of_ref(mb, ref2, 0);
	int frm3 = frame_idx_of_ref(mb, ref3, 1);
	for (int i = 0; i < 2; ++i) {
		mb->top4x4inter->ref[i][0] = ref2;
		mb->top4x4inter->ref[i][1] = ref3;
		mb->top4x4inter->frmidx[i][0] = frm2;
		mb->top4x4inter->frmidx[i][1] = frm3;
		mb->left4x4inter->mov[i] = mv[0];
		mb->left4x4inter->mvd[i] = mv[2];
		mb->left4x4inter->mov[2 + i] = mv[1];
		mb->left4x4inter->mvd[2 + i] = mv[3];
		mb->left4x4inter->ref[0][i] = ref_idx[i];
		mb->left4x4inter->frmidx[0][i] = frame_idx_of_ref(mb, ref_idx[i], i);
	}
	mb->left4x4inter->ref[1][0] = ref2;
	mb->left4x4inter->ref[1][1] = ref3;
	mb->left4x4inter->frmidx[1][0] = frm2;
	mb->left4x4inter->frmidx[1][1] = frm3;
	store_col16x8(mb->col_curr, ref_idx, mv);
}

template <typename F0 ,typename F1, typename F2, typename F3, typename F4, typename F5, typename F6>
static int mb_inter16x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
			 F0 RefIdx16x8,
			 F1 MvdXY,
			 F2 CodedBlockPattern,
			 F3 ResidualLumaInter,
			 F4 Transform8x8Flag,
			 F5 QpDelta,
			 F6 ResidualBlock)
{
	uint32_t left4x4, top4x4;
	uint32_t refmap;
	uint32_t cbp;
	const int16_t *mvd_a, *mvd_b;
	h264d_vector_set_t mv[2][2];
	int8_t ref_idx[4];
	static const h264d_vector_t size = {{16, 8}};

	refmap = mbc->cbp;
	RefIdx16x8(mb, st, ref_idx, refmap, avail);
	memset(mv, 0, sizeof(mv));
	for (int lx = 0; lx < 2; ++lx) {
		if (refmap & 1) {
			calc_mv16x8top(mb, mv[0][0].mv[lx].v, mvd_a, mvd_b, lx, ref_idx[lx], avail);
			MvdXY(mb, st, mv[1][0].mv[lx].v, mvd_a, mvd_b);
			mv[0][0].mv[lx].v[0] += mv[1][0].mv[lx].v[0];
			mv[0][0].mv[lx].v[1] += mv[1][0].mv[lx].v[1];
		}
		if (refmap & 2) {
			calc_mv16x8bottom(mb, mv[0][1].mv[lx].v, mvd_a, mvd_b, lx, ref_idx[lx + 2], avail, ref_idx[lx], &mv[0][0]);
			MvdXY(mb, st, mv[1][1].mv[lx].v, mvd_a, mvd_b);
			mv[0][1].mv[lx].v[0] += mv[1][1].mv[lx].v[0];
			mv[0][1].mv[lx].v[1] += mv[1][1].mv[lx].v[1];
		}
		refmap >>= 2;
	}
	mb->inter_pred(mb, &ref_idx[0], mv[0][0].mv, size, 0, 0);
	mb->inter_pred(mb, &ref_idx[2], mv[0][1].mv, size, 0, 8);
	
	left4x4 = mb->left4x4coef;
	top4x4 = *mb->top4x4coef;
	mb->cbp = cbp = CodedBlockPattern(mb, st, avail);
	if (cbp) {
		ResidualLumaInter(mb, 0x80 | cbp, st, avail, Transform8x8Flag, QpDelta, ResidualBlock);
	} else {
		no_residual_inter(mb);
	}
	store_info_inter16x8(mb, &mv[0][0], ref_idx, left4x4, top4x4);
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

static inline void calc_mv8x16left(h264d_mb_current *mb, int16_t pmv[], const int16_t *&mvd_a, const int16_t *&mvd_b, int lx, int ref_idx, int avail)
{
	prev_mb_t *pmb;
	const int16_t *mva, *mvb, *mvc;
	int idx_map;

	if (avail & 1) {
		pmb = mb->left4x4inter;
		mvd_a = pmb->mvd[0].mv[lx].v;
		if (ref_idx == pmb->ref[0][lx]) {
			pmv[0] = pmb->mov[0].mv[lx].v[0];
			pmv[1] = pmb->mov[0].mv[lx].v[1];
			mvd_b = (avail & 2) ? mb->top4x4inter->mvd[0].mv[lx].v : zero_mv;
			return;
		}
		mva = pmb->mov[0].mv[lx].v;
	} else {
		mvd_a = mva = zero_mv;
	}
	idx_map = 0;
	if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[0][lx]) * 2;
		idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
		avail |= 4;
		mvb = pmb->mov[0].mv[lx].v;
		mvd_b = pmb->mvd[0].mv[lx].v;
		mvc = pmb->mov[2].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
		avail &= ~4;
		if (avail & 8) {
			idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
			mvc = mb->lefttop_mv[lx].v;
		} else {
			mvc = zero_mv;
		}
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void calc_mv8x16right(h264d_mb_current *mb, int16_t pmv[], const int16_t *&mvd_a, const int16_t *&mvd_b, int lx, int ref_idx, int avail, int prev_ref, const h264d_vector_set_t *prev_mv)
{
	prev_mb_t *pmb;
	const int16_t *mva, *mvb, *mvc;
	int idx_map;

	idx_map = 0;
	if (avail & 4) {
		pmb = mb->top4x4inter + 1;
		if (ref_idx == pmb->ref[0][lx]) {
			pmv[0] = pmb->mov[0].mv[lx].v[0];
			pmv[1] = pmb->mov[0].mv[lx].v[1];
			mvd_a = prev_mv[2].mv[lx].v;
			mvd_b = (avail & 2) ? mb->top4x4inter->mvd[2].mv[lx].v : zero_mv;
			return;
		}
		mvc = pmb->mov[0].mv[lx].v;
	} else if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map = (ref_idx == pmb->ref[0][lx]) * 4;
		mvd_b = pmb->mvd[2].mv[lx].v;
		if (idx_map) {
			pmv[0] = pmb->mov[1].mv[lx].v[0];
			pmv[1] = pmb->mov[1].mv[lx].v[1];
			mvd_a = prev_mv[2].mv[lx].v;
			return;
		} else {
			mvc = pmb->mov[1].mv[lx].v;
		}
	} else {
		mvc = zero_mv;
	}
	/* left block are always available */
	idx_map |= (ref_idx == prev_ref);
	mva = prev_mv->mv[lx].v;
	mvd_a = prev_mv[2].mv[lx].v;
	avail |= 1;
	if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[1][lx]) * 2;
		mvb = pmb->mov[2].mv[lx].v;
		mvd_b = pmb->mvd[2].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void store_col8x16(h264d_col_mb_t *col, const int8_t *ref_idx, const h264d_vector_set_t *mv)
{
	h264d_vector_t *mvdst = col->mv;
	int8_t *refdst = col->ref;
	col->type = COL_MB8x16;
	for (int x = 0; x < 2; ++x) {
		int refcol;
		uint32_t mvcol;
		if (0 <= ref_idx[0]) {
			refcol = ref_idx[0];
			mvcol = mv[x].mv[0].vector;
		} else {
			refcol = ref_idx[1];
			mvcol = mv[x].mv[1].vector;
		}
		refdst[0] = refcol;
		refdst[2] = refcol;
		uint32_t *dst = &mvdst[0].vector;
		int i = 4;
		do {
			dst[0] = mvcol;
			dst[1] = mvcol;
			dst += 4;
		} while (--i);
		ref_idx += 2;
		refdst += 1;
		mvdst += 2;
	}
}

static void store_info_inter8x16(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4)
{
	deblock_info_t *deb = mb->deblock_curr;
	deb->qpy = mb->qp;
	deb->qpc[0] = mb->qp_chroma[0];
	deb->qpc[1] = mb->qp_chroma[1];
	if (mb->y != 0) {
		store_str_inter8xedge<0, 1, 1>(mb, mb->top4x4inter, deb->str4_vert, mv, ref_idx, deb->str_vert, top4x4);
	}
	if (mb->x != 0) {
		store_str_inter16xedge(mb, mb->left4x4inter, deb->str4_horiz, mv, ref_idx, deb->str_horiz, left4x4);
	}
	deb->str_horiz = str_mv_calc16x8_vert(mb, deb->str_horiz, ref_idx, mv);

	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	mb->left4x4inter->direct8x8 = 0;
	mb->top4x4inter->direct8x8 = 0;
	int ref2 = ref_idx[2];
	int ref3 = ref_idx[3];
	int frm2 = frame_idx_of_ref(mb, ref2, 0);
	int frm3 = frame_idx_of_ref(mb, ref3, 1);
	for (int i = 0; i < 2; ++i) {
		mb->lefttop_ref[i] = mb->top4x4inter->ref[1][i];
		mb->left4x4inter->ref[i][0] = ref2;
		mb->left4x4inter->ref[i][1] = ref3;
		mb->left4x4inter->frmidx[i][0] = frm2;
		mb->left4x4inter->frmidx[i][1] = frm3;
		mb->top4x4inter->ref[0][i] = ref_idx[i];
		mb->top4x4inter->frmidx[0][i] = frame_idx_of_ref(mb, ref_idx[i], i);
		mb->lefttop_mv[i].vector = mb->top4x4inter->mov[3].mv[i].vector;
		mb->top4x4inter->mov[i] = mv[0];
		mb->top4x4inter->mvd[i] = mv[2];
		mb->top4x4inter->mov[i + 2] = mv[1];
		mb->top4x4inter->mvd[i + 2] = mv[3];
	}
	mb->top4x4inter->ref[1][0] = ref2;
	mb->top4x4inter->ref[1][1] = ref3;
	mb->top4x4inter->frmidx[1][0] = frm2;
	mb->top4x4inter->frmidx[1][1] = frm3;
	for (int i = 0; i < 4; ++i) {
		mb->left4x4inter->mov[i] = mv[1];
		mb->left4x4inter->mvd[i] = mv[3];
	}
	store_col8x16(mb->col_curr, ref_idx, mv);
}

template <typename F0 ,typename F1, typename F2, typename F3, typename F4, typename F5, typename F6>
static int mb_inter8x16(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
			 F0 RefIdx8x16,
			 F1 MvdXY,
			 F2 CodedBlockPattern,
			 F3 ResidualLumaInter,
			 F4 Transform8x8Flag,
			 F5 QpDelta,
			 F6 ResidualBlock)
{
	h264d_vector_set_t mv[2][2];
	const int16_t *mvd_a, *mvd_b;
	uint32_t refmap;
	uint32_t cbp;
	uint32_t top4x4, left4x4;
	int8_t ref_idx[4];
	static const h264d_vector_t size = {{8, 16}};

	refmap = mbc->cbp;
	RefIdx8x16(mb, st, ref_idx, refmap, avail);
	memset(mv, 0, sizeof(mv));
	for (int lx = 0; lx < 2; ++lx) {
		if (refmap & 1) {
			calc_mv8x16left(mb, mv[0][0].mv[lx].v, mvd_a, mvd_b, lx, ref_idx[lx], avail);
			MvdXY(mb, st, mv[1][0].mv[lx].v, mvd_a, mvd_b);
			mv[0][0].mv[lx].v[0] += mv[1][0].mv[lx].v[0];
			mv[0][0].mv[lx].v[1] += mv[1][0].mv[lx].v[1];
		}
		if (refmap & 2) {
			calc_mv8x16right(mb, mv[0][1].mv[lx].v, mvd_a, mvd_b, lx, ref_idx[lx + 2], avail, ref_idx[lx], &mv[0][0]);
			MvdXY(mb, st, mv[1][1].mv[lx].v, mvd_a, mvd_b);
			mv[0][1].mv[lx].v[0] += mv[1][1].mv[lx].v[0];
			mv[0][1].mv[lx].v[1] += mv[1][1].mv[lx].v[1];
		}
		refmap >>= 2;
	}
	mb->inter_pred(mb, &ref_idx[0], mv[0][0].mv, size, 0, 0);
	mb->inter_pred(mb, &ref_idx[2], mv[0][1].mv, size, 8, 0);
	left4x4 = mb->left4x4coef;
	top4x4 = *mb->top4x4coef;
	mb->cbp = cbp = CodedBlockPattern(mb, st, avail);
	if (cbp) {
		ResidualLumaInter(mb, 0x80 | cbp, st, avail, Transform8x8Flag, QpDelta, ResidualBlock);
	} else {
		no_residual_inter(mb);
	}
	store_info_inter8x16(mb, &mv[0][0], ref_idx, left4x4, top4x4);
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

static inline void calc_mv8x8_sub8x8(h264d_mb_current *mb, int16_t *pmv, const int16_t *&mvd_a, const int16_t *&mvd_b, int avail, int lx, int ref_idx, int blk_idx, prev8x8_t *pblk)
{
	const int16_t *mva;
	const int16_t *mvb;
	const int16_t *mvc;
	prev_mb_t *pmb;
	int idx_map;

	if (blk_idx & 1) {
		idx_map = (ref_idx == pblk[blk_idx - 1].ref[lx]);
		mva = pblk[blk_idx - 1].mv[1][lx].v;
		mvd_a = pblk[blk_idx - 1].mvd[1][lx].v;
		avail |= 1;
	} else if (avail & 1) {
		pmb = mb->left4x4inter;
		idx_map = (ref_idx == pmb->ref[blk_idx >> 1][lx]);
		mva = pmb->mov[blk_idx].mv[lx].v;
		mvd_a = pmb->mvd[blk_idx].mv[lx].v;
	} else {
		idx_map = 0;
		mvd_a = mva = zero_mv;
	}

	if (blk_idx & 2) {
		idx_map |= (ref_idx == pblk[blk_idx - 2].ref[lx]) * 2;
		mvb = pblk[blk_idx - 2].mv[2][lx].v;
		mvd_b = pblk[blk_idx - 2].mvd[2][lx].v;
		avail |= 2;
	} else if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[blk_idx][lx]) * 2;
		mvb = pmb->mov[blk_idx * 2].mv[lx].v;
		mvd_b = pmb->mvd[blk_idx * 2].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}

	switch (blk_idx) {
	case 0:
		if (avail & 2) {
			pmb = mb->top4x4inter;
			idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
			mvc = pmb->mov[2].mv[lx].v;
			avail |= 4;
		} else if (avail & 8) {
			idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
			mvc = mb->lefttop_mv[lx].v;
			avail |= 4;
		} else {
			avail &= ~4;
			mvc = zero_mv;
		}
		break;
	case 1:
		if (avail & 4) {
			pmb = mb->top4x4inter + 1;
			idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
			mvc = pmb->mov[0].mv[lx].v;
		} else if (avail & 2) {
			pmb = mb->top4x4inter;
			idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
			mvc = pmb->mov[1].mv[lx].v;
		} else {
			mvc = zero_mv;
		}
		break;
	case 2:
		idx_map |= (ref_idx == pblk[1].ref[lx]) * 4;
		mvc = pblk[1].mv[2][lx].v;
		avail |= 4;
		break;
	case 3:
		idx_map |= (ref_idx == pblk[0].ref[lx]) * 4;
		mvc = pblk[0].mv[3][lx].v;
		avail |= 4;
		break;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void calc_mv8x8_sub8x4(h264d_mb_current *mb, int16_t *pmv, const int16_t *&mvd_a, const int16_t *&mvd_b, int avail, int lx, int ref_idx, int blk_idx, prev8x8_t *pblk, int y)
{
	const int16_t *mva;
	const int16_t *mvb;
	const int16_t *mvc;
	prev_mb_t *pmb;
	int idx_map;

	if (blk_idx & 1) {
		idx_map = (ref_idx == pblk[blk_idx - 1].ref[lx]);
		mva = pblk[blk_idx - 1].mv[y * 2 + 1][lx].v;
		mvd_a = pblk[blk_idx - 1].mvd[y * 2 + 1][lx].v;
		avail |= 1;
	} else if (avail & 1) {
		pmb = mb->left4x4inter;
		idx_map = (ref_idx == pmb->ref[blk_idx >> 1][lx]);
		mva = pmb->mov[(blk_idx & 2) + y].mv[lx].v;
		mvd_a = pmb->mvd[(blk_idx & 2) + y].mv[lx].v;
	} else {
		idx_map = 0;
		mvd_a = mva = zero_mv;
	}

	if (y != 0) {
		idx_map |= 2;
		mvb = pblk[blk_idx].mv[0][lx].v;
		mvd_b = pblk[blk_idx].mvd[0][lx].v;
		avail |= 2;
	} else if (blk_idx & 2) {
		idx_map |= (ref_idx == pblk[blk_idx - 2].ref[lx]) * 2;
		mvb = pblk[blk_idx - 2].mv[2][lx].v;
		mvd_b = pblk[blk_idx - 2].mvd[2][lx].v;
		avail |= 2;
	} else if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[blk_idx & 1][lx]) * 2;
		mvb = pmb->mov[blk_idx * 2].mv[lx].v;
		mvd_b = pmb->mvd[blk_idx * 2].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}

	switch (blk_idx) {
	case 0:
		if (y == 0) {
			if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
				avail |= 4;
				mvc = pmb->mov[2].mv[lx].v;
			} else if (avail & 8) {
				idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
				avail |= 4;
				mvc = mb->lefttop_mv[lx].v;
			} else {
				avail &= ~4;
				mvc = zero_mv;
			}
		} else if (avail & 1) {
			pmb = mb->left4x4inter;
			idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
			mvc = pmb->mov[0].mv[lx].v;
			avail |= 4;
		} else {
			avail &= ~4;
			mvc = zero_mv;
		}
		break;
	case 1:
		if (y == 0) {
			if (avail & 4) {
				pmb = mb->top4x4inter + 1;
				idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
				mvc = pmb->mov[0].mv[lx].v;
				avail |= 4;
			} else if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
				mvc = pmb->mov[1].mv[lx].v;
				avail |= 4;
			} else {
				mvc = zero_mv;
			}
		} else {
			idx_map |= (ref_idx == pblk[0].ref[lx]) * 4;
			mvc = pblk[0].mv[1][lx].v;
			avail |= 4;
		}
		break;
	case 2:
		if (y == 0) {
			idx_map |= (ref_idx == pblk[1].ref[lx]) * 4;
			mvc = pblk[1].mv[2][lx].v;
			avail |= 4;
		} else if (avail & 1) {
			pmb = mb->left4x4inter;
			idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
			mvc = pmb->mov[2].mv[lx].v;
			avail |= 4;
		} else {
			avail &= ~4;
			mvc = zero_mv;
		}
		break;
	case 3:
		idx_map |= (ref_idx == pblk[y * 2].ref[lx]) * 4;
		mvc = pblk[y * 2].mv[3 - y * 2][lx].v;
		avail |= 4;
		break;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void calc_mv8x8_sub4x8(h264d_mb_current *mb, int16_t *pmv, const int16_t *&mvd_a, const int16_t *&mvd_b, int avail, int lx, int ref_idx, int blk_idx, prev8x8_t *pblk, int x)
{
	const int16_t *mva;
	const int16_t *mvb;
	const int16_t *mvc;
	prev_mb_t *pmb;
	int idx_map;

	if (x != 0) {
		idx_map = 1;
		mva = pblk[blk_idx].mv[0][lx].v;
		mvd_a = pblk[blk_idx].mvd[0][lx].v;
		avail |= 1;
	} else if (blk_idx & 1) {
		idx_map = (ref_idx == pblk[blk_idx - 1].ref[lx]);
		mva = pblk[blk_idx - 1].mv[1][lx].v;
		mvd_a = pblk[blk_idx - 1].mvd[1][lx].v;
		avail |= 1;
	} else if (avail & 1) {
		pmb = mb->left4x4inter;
		idx_map = (ref_idx == pmb->ref[blk_idx >> 1][lx]);
		mva = pmb->mov[blk_idx].mv[lx].v; /* blk_idx shall be 0 or 2 */
		mvd_a = pmb->mvd[blk_idx].mv[lx].v;
	} else {
		idx_map = 0;
		mvd_a = mva = zero_mv;
	}

	if (blk_idx & 2) {
		idx_map |= (ref_idx == pblk[blk_idx - 2].ref[lx]) * 2;
		mvb = pblk[blk_idx - 2].mv[2 + x][lx].v;
		mvd_b = pblk[blk_idx - 2].mvd[2 + x][lx].v;
		avail |= 2;
	} else if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[blk_idx & 1][lx]) * 2;
		mvb = pmb->mov[blk_idx * 2 + x].mv[lx].v;
		mvd_b = pmb->mvd[blk_idx * 2 + x].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}

	switch (blk_idx) {
	case 0:
		if (avail & 2) {
			pmb = mb->top4x4inter;
			idx_map |= (ref_idx == pmb->ref[x][lx]) * 4;
			mvc = pmb->mov[x + 1].mv[lx].v;
			avail |= 4;
		} else {
			avail &= ~4;
			if (x == 0 && (avail & 8)) {
 				idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
				mvc = mb->lefttop_mv[lx].v;
			} else {
				mvc = zero_mv;
			}
		}
		break;
	case 1:
		if (x == 0) {
			if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
				mvc = pmb->mov[3].mv[lx].v;
				avail |= 4;
			} else {
				avail &= ~4;
				mvc = zero_mv;
			}
		} else {
			if (avail & 4) {
				pmb = mb->top4x4inter + 1;
				idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
				mvc = pmb->mov[0].mv[lx].v;
			} else if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
				if (0 <= pmb->ref[1][lx]) {
					mvc = pmb->mov[2].mv[lx].v;
				} else {
					mvc = zero_mv;
				}
			} else {
				mvc = zero_mv;
			}
		}
		break;
	case 2:
		avail |= 4;
		idx_map |= (ref_idx == pblk[x].ref[lx]) * 4;
		mvc = pblk[x].mv[3 - x][lx].v;
		break;
	case 3:
		avail |= 4;
		idx_map |= (ref_idx == pblk[1].ref[lx]) * 4;
		mvc = pblk[1].mv[3 - x][lx].v;
		break;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void calc_mv8x8_sub4x4(h264d_mb_current *mb, int16_t *pmv, const int16_t *&mvd_a, const int16_t *&mvd_b, int avail, int lx, int ref_idx, int blk_idx, prev8x8_t *pblk, int xy)
{
	const int16_t *mva;
	const int16_t *mvb;
	const int16_t *mvc;
	prev_mb_t *pmb;
	int idx_map;

	if (xy & 1) {
		idx_map = 1;
		mva = pblk[blk_idx].mv[xy - 1][lx].v;
		mvd_a = pblk[blk_idx].mvd[xy - 1][lx].v;
		avail |= 1;
	} else if (blk_idx & 1) {
		idx_map = (ref_idx == pblk[blk_idx - 1].ref[lx]);
		mva = pblk[blk_idx - 1].mv[xy + 1][lx].v; /* xy shall be 0 or 2 */
		mvd_a = pblk[blk_idx - 1].mvd[xy + 1][lx].v;
		avail |= 1;
	} else if (avail & 1) {
		pmb = mb->left4x4inter;
		idx_map = (ref_idx == pmb->ref[blk_idx >> 1][lx]);
		mva = pmb->mov[blk_idx + (xy >> 1)].mv[lx].v; /* blk_idx shall be 0 or 2 */
		mvd_a = pmb->mvd[blk_idx + (xy >> 1)].mv[lx].v;
	} else {
		idx_map = 0;
		mvd_a = mva = zero_mv;
	}

	if (xy & 2) {
		idx_map |= 2;
		mvb = pblk[blk_idx].mv[xy - 2][lx].v;
		mvd_b = pblk[blk_idx].mvd[xy - 2][lx].v;
		avail |= 2;
	} else if (blk_idx & 2) {
		idx_map |= (ref_idx == pblk[blk_idx - 2].ref[lx]) * 2;
		mvb = pblk[blk_idx - 2].mv[2 + (xy & 1)][lx].v;
		mvd_b = pblk[blk_idx - 2].mvd[2 + (xy & 1)][lx].v;
		avail |= 2;
	} else if (avail & 2) {
		pmb = mb->top4x4inter;
		idx_map |= (ref_idx == pmb->ref[blk_idx & 1][lx]) * 2;
		mvb = pmb->mov[blk_idx * 2 + (xy & 1)].mv[lx].v;
		mvd_b = pmb->mvd[blk_idx * 2 + (xy & 1)].mv[lx].v;
	} else {
		mvd_b = mvb = zero_mv;
	}

	switch (blk_idx) {
	case 0:
		switch (xy) {
		case 0:
			if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
				avail |= 4;
				mvc = pmb->mov[1].mv[lx].v;
			} else if (avail & 8) {
				avail &= ~4;
				idx_map |= (ref_idx == mb->lefttop_ref[lx]) * 4;
				mvc = mb->lefttop_mv[lx].v;
			} else {
				avail &= ~4;
				mvc = zero_mv;
			}
			break;
		case 1:
			if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
				avail |= 4;
				mvc = pmb->mov[2].mv[lx].v;
			} else {
				avail &= ~4;
				mvc = zero_mv;
			}
			break;
		case 2:
			idx_map |= 4;
			avail |= 4;
			mvc = pblk[blk_idx].mv[1][lx].v;
			break;
		case 3:
			idx_map |= 4;
			avail |= 4;
			mvc = pblk[blk_idx].mv[0][lx].v;
			break;
		}
		break;
	case 1:
		switch (xy) {
		case 0:
			if (avail & 2) {
				pmb = mb->top4x4inter;
			idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
				mvc = pmb->mov[3].mv[lx].v;
				avail |= 4;
			} else {
				avail &= ~4;
				mvc = zero_mv;
			}
			break;
		case 1:
			if (avail & 4) {
				pmb = mb->top4x4inter + 1;
				idx_map |= (ref_idx == pmb->ref[0][lx]) * 4;
				mvc = pmb->mov[0].mv[lx].v;
			} else if (avail & 2) {
				pmb = mb->top4x4inter;
				idx_map |= (ref_idx == pmb->ref[1][lx]) * 4;
				mvc = pmb->mov[2].mv[lx].v;
				avail |= 4;
			} else {
				mvc = zero_mv;
			}
			break;
		case 2:
		case 3:
			idx_map |= 4;
			avail |= 4;
			mvc = pblk[blk_idx].mv[3 - xy][lx].v;
			break;
		}
		break;
	case 2:
		avail |= 4;
		switch (xy) {
		case 0:
		case 1:
			idx_map |= (ref_idx == pblk[xy].ref[lx]) * 4;
			mvc = pblk[xy].mv[3 - xy][lx].v;
			break;
		case 2:
		case 3:
			idx_map |= 4;
			mvc = pblk[2].mv[3 - xy][lx].v;
			break;
		}
		break;
	case 3:
		avail |= 4;
		switch (xy) {
		case 0:
		case 1:
			idx_map |= (ref_idx == pblk[1].ref[lx]) * 4;
			mvc = pblk[1].mv[3 - xy][lx].v;
			break;
		case 2:
		case 3:
			idx_map |= 4;
			mvc = pblk[3].mv[3 - xy][lx].v;
			break;
		}
		break;
	}
	determine_pmv(mva, mvb, mvc, pmv, avail, idx_map);
}

static inline void b_skip_ref_mv(int8_t *ref_idx, int16_t *mv, const int8_t *ref_a, const int8_t *ref_b, const int8_t *ref_c, const h264d_vector_t *mv_a, const h264d_vector_t *mv_b, const h264d_vector_t *mv_c)
{
	for (int lx = 0; lx < 2; ++lx) {
		int ra = *ref_a++;
		int rb = *ref_b++;
		int rc = *ref_c++;
		int ref = MIN((unsigned)ra, (unsigned)rb);
		ref = MIN((unsigned)ref, (unsigned)rc);
		if (ref < 0) {
			*(uint32_t *)mv = 0U;
		} else if ((ra == ref) && (rb != ref) && (rc != ref)) {
			*(uint32_t *)mv = mv_a->vector;
		} else if ((ra != ref) && (rb == ref) && (rc != ref)) {
			*(uint32_t *)mv = mv_b->vector;
		} else if ((ra != ref) && (rb != ref) && (rc == ref)) {
			*(uint32_t *)mv = mv_c->vector;
		} else {
			mv[0] = MEDIAN(mv_a->v[0], mv_b->v[0], mv_c->v[0]);
			mv[1] = MEDIAN(mv_a->v[1], mv_b->v[1], mv_c->v[1]);
		}
		*ref_idx++ = ref;
		mv_a++;
		mv_b++;
		mv_c++;
		mv += 2;
	}
}

static void b_direct_ref_mv_calc(h264d_mb_current *mb, int avail, int8_t *ref_idx, int16_t *mv)
{
	const int8_t *ref_a;
	const int8_t *ref_b;
	const int8_t *ref_c;
	const h264d_vector_t *mv_a;
	const h264d_vector_t *mv_b;
	const h264d_vector_t *mv_c;

	if (avail & 1) {
		ref_a = mb->left4x4inter->ref[0];
		mv_a = mb->left4x4inter->mov[0].mv;
	} else {
		ref_a = non_ref;
		mv_a = zero_mov;
	}
	if (avail & 2) {
		ref_b = mb->top4x4inter->ref[0];
		mv_b = mb->top4x4inter->mov[0].mv;
	} else {
		ref_b = non_ref;
		mv_b = zero_mov;
	}
	if (avail & 4) {
		ref_c = mb->top4x4inter[1].ref[0];
		mv_c = mb->top4x4inter[1].mov[0].mv;
	} else if (avail & 8) {
		ref_c = mb->lefttop_ref;
		mv_c = mb->lefttop_mv;
	} else {
		ref_c = non_ref;
		mv_c = zero_mov;
	}
	b_skip_ref_mv(ref_idx, mv, ref_a, ref_b, ref_c, mv_a, mv_b, mv_c);
}

static inline void direct_mv_pred(h264d_mb_current *mb, const int8_t *ref_idx, const h264d_vector_t *mv, const h264d_vector_t& size, int xoffset, int yoffset)
{
	mb->inter_pred(mb, ref_idx, mv, size, xoffset, yoffset);
}

template <int N, int X, int Y>
struct pred_direct_col_block_bidir {
	void operator()(h264d_mb_current *mb, const h264d_vector_t *mvcol, h264d_vector_t *mv_curr, const int8_t *ref_idx, int xofs, int yofs) const {
		static const h264d_vector_t size = {{X, Y}};
		if ((mv_curr[0].vector != 0U || mv_curr[1].vector != 0U) && ((unsigned)(mvcol->v[0] + 1) <= 2U) && ((unsigned)(mvcol->v[1] + 1) <= 2U)) {
			mv_curr[0].vector = 0U;
			mv_curr[1].vector = 0U;
			if ((N == 8) && (X == 8) && (Y == 8)) {
				mv_curr[2].vector = 0U;
				mv_curr[3].vector = 0U;
				mv_curr[4].vector = 0U;
				mv_curr[5].vector = 0U;
				mv_curr[6].vector = 0U;
				mv_curr[7].vector = 0U;
			}
			mb->inter_pred(mb, (const int8_t *)zero_mov, zero_mov, size, xofs, yofs);
		} else {
			direct_mv_pred(mb, ref_idx, mv_curr, size, xofs, yofs);
		}
	}
};

template <int LX, int N, int X, int Y>
struct pred_direct_col_block_onedir {
	void operator()(h264d_mb_current *mb, const h264d_vector_t *mvcol, h264d_vector_t *mv_curr, const int8_t *ref_idx, int xofs, int yofs) const {
		static const h264d_vector_t size = {{X, Y}};
		if ((mv_curr[LX].vector != 0U) && ((unsigned)(mvcol->v[0] + 1) <= 2U) && ((unsigned)(mvcol->v[1] + 1) <= 2U)) {
			mv_curr[LX].vector = 0U;
			if ((N == 8) && (X == 8) && (Y == 8)) {
				mv_curr[LX + 2].vector = 0U;
				mv_curr[LX + 4].vector = 0U;
				mv_curr[LX + 6].vector = 0U;
			}
		}
		direct_mv_pred(mb, ref_idx, mv_curr, size, xofs, yofs);
	}
};

static void pred_direct8x8_block8x8_nocol(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	static const h264d_vector_t size = {{8, 8}};
	direct_mv_pred(mb, ref_idx, mv, size, (blk_idx & 1) * 8, (blk_idx & 2) * 4);
}

template <int N, int BLOCK, typename F0>
static inline void pred_direct_block(h264d_mb_current *mb, const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx,
				    F0 PredDirectBlock)
{
	int xoffset = (blk_idx & 1) * 8;
	int yoffset = (blk_idx & 2) * 4;
	for (int i = 0; i < 64 / (BLOCK * BLOCK); ++i) {
		int xofs = xoffset + (i & 1) * 4;
		int yofs = yoffset + (i & 2) * 2;
		h264d_vector_t *mv_curr = &mv[(i & 2) * (N / 4) + (i & 1) * 2];
		PredDirectBlock(mb, mvcol, mv_curr, ref_idx, xofs, yofs);
		mvcol += (i & 1) ? 3 : 1;
	}
}

static void pred_direct8x8_block8x8_l0(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	pred_direct_block<8, 8>(mb, mvcol, ref_idx, mv, blk_idx, pred_direct_col_block_onedir<0, 8, 8, 8>());
}

static void pred_direct8x8_block8x8_l1(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	pred_direct_block<8, 8>(mb, mvcol, ref_idx, mv, blk_idx, pred_direct_col_block_onedir<1, 8, 8, 8>());
}

static void pred_direct8x8_block8x8_bidir(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	pred_direct_block<8, 8>(mb, mvcol, ref_idx, mv, blk_idx, pred_direct_col_block_bidir<8, 8, 8>());
}

static void pred_direct8x8_block4x4_l0(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	pred_direct_block<8, 4>(mb, mvcol, ref_idx, mv, blk_idx, pred_direct_col_block_onedir<0, 8, 4, 4>());
}

static void pred_direct8x8_block4x4_l1(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	pred_direct_block<8, 4>(mb, mvcol, ref_idx, mv, blk_idx, pred_direct_col_block_onedir<1, 8, 4, 4>());
}

static void pred_direct8x8_block4x4_bidir(h264d_mb_current *mb,  const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	pred_direct_block<8, 4>(mb, mvcol, ref_idx, mv, blk_idx, pred_direct_col_block_bidir<8, 4, 4>());
}

template <int BLOCK>
static void pred_direct8x8_spatial_dec(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk)
{
	static void (* const pred_direct8x8_block_lut8x8[4])(h264d_mb_current *mb, const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx) = {
		pred_direct8x8_block8x8_nocol,
		pred_direct8x8_block8x8_l0,
		pred_direct8x8_block8x8_l1,
		pred_direct8x8_block8x8_bidir
	};
	static void (* const pred_direct8x8_block_lut4x4[4])(h264d_mb_current *mb, const h264d_vector_t *mvcol, const int8_t *ref_idx, h264d_vector_t *mv, int blk_idx) = {
		/* COL_MB8x8 */
		pred_direct8x8_block8x8_nocol,
		pred_direct8x8_block4x4_l0,
		pred_direct8x8_block4x4_l1,
		pred_direct8x8_block4x4_bidir
	};

	int xoffset = (blk_idx & 1) * 8;
	int yoffset = (blk_idx & 2) * 4;
	int8_t *ref_idx = pblk->ref;
	static const h264d_vector_t size = {{8, 8}};
	if ((0 <= ref_idx[0]) || (0 <= ref_idx[1])) {
		const h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
		const h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
		if ((colpic->in_use == SHORT_TERM) && (col_mb->ref[blk_idx] == 0)) {
			const h264d_vector_t *mvcol = &col_mb->mv[(blk_idx & 2) * ((BLOCK == 8) ? 6 : 4) + (blk_idx & 1) * ((BLOCK == 8) ? 3 : 2)];
			int refs = (ref_idx[0] == 0) + (ref_idx[1] == 0) * 2;
			if ((BLOCK == 8) || (col_mb->type != COL_MB8x8)) {
				pred_direct8x8_block_lut8x8[refs](mb, mvcol, pblk->ref, pblk->mv[0], blk_idx);
			} else {
				pred_direct8x8_block_lut4x4[refs](mb, mvcol, pblk->ref, pblk->mv[0], blk_idx);
			}
		} else {
			direct_mv_pred(mb, ref_idx, &pblk->mv[0][0], size, xoffset, yoffset);
		}
	} else {
		pblk->ref[0] = 0;
		pblk->ref[1] = 0;
		memset(pblk->mv, 0, sizeof(pblk->mv));
		mb->inter_pred(mb, (const int8_t *)zero_mov, zero_mov, size, xoffset, yoffset);
	}
}

static inline void fill_direct8x8_mv(prev8x8_t *pblk, const prev8x8_t *src)
{
	uint32_t mov0 = src->mv[0][0].vector;
	uint32_t mov1 = src->mv[0][1].vector;
	pblk->ref[0] = src->ref[0];
	pblk->ref[1] = src->ref[1];
	for (int i = 0; i < 4; ++i) {
		pblk->mv[i][0].vector = mov0;
		pblk->mv[i][1].vector = mov1;
	}
}

template <int BLOCK>
static void pred_direct8x8_spatial(h264d_mb_current *mb, int blk_idx, prev8x8_t *curr_blk, int avail, prev8x8_t *ref_blk, int type0_cnt)
{
	if (type0_cnt == 0) {
		b_direct_ref_mv_calc(mb, avail, ref_blk->ref, ref_blk->mv[0][0].v);
	}
	fill_direct8x8_mv(&curr_blk[blk_idx], ref_blk);
	pred_direct8x8_spatial_dec<BLOCK>(mb, blk_idx, &curr_blk[blk_idx]);
}

static void pred_direct8x8(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk)
{
	/* do nothing */
}

static void sub_mb8x8_direct(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	/* do nothing */
}

template<typename F0>
static void sub_mb8x8_mv(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx,
			 F0 MvdXY)
{
	prev8x8_t *p = pblk + blk_idx;
	int idx = p->ref[lx];
	if (0 <= idx) {
		const int16_t *mvd_a;
		const int16_t *mvd_b;
		h264d_vector_t mv;
		h264d_vector_t mvd;

		calc_mv8x8_sub8x8(mb, mv.v, mvd_a, mvd_b, avail, lx, idx, blk_idx, pblk);
		MvdXY(mb, st, mvd.v, mvd_a, mvd_b);
		mv.v[0] += mvd.v[0];
		mv.v[1] += mvd.v[1];
		for (int i = 0; i < 4; ++i) {
			p->mv[i][lx].vector = mv.vector;
			p->mvd[i][lx].vector = mvd.vector;
		}
	}
}

template<typename F0>
static void sub_mb8x4_mv(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx,
			 F0 MvdXY)
{
	prev8x8_t *p = pblk + blk_idx;
	int idx = p->ref[lx];
	if (0 <= idx) {
		for (int y = 0; y < 2; ++y) {
			const int16_t *mvd_a;
			const int16_t *mvd_b;
			h264d_vector_t mv;
			h264d_vector_t mvd;

			calc_mv8x8_sub8x4(mb, mv.v, mvd_a, mvd_b, avail, lx, idx, blk_idx, pblk, y);
			MvdXY(mb, st, mvd.v, mvd_a, mvd_b);
			mv.v[0] += mvd.v[0];
			mv.v[1] += mvd.v[1];
			p->mv[y * 2][lx].vector = mv.vector;
			p->mvd[y * 2][lx].vector = mvd.vector;
			p->mv[y * 2 + 1][lx].vector = mv.vector;
			p->mvd[y * 2 + 1][lx].vector = mvd.vector;
		}
	}
}

template <typename F0>
static void sub_mb4x8_mv(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx,
			 F0 MvdXY)
{
	prev8x8_t *p = pblk + blk_idx;
	int idx = p->ref[lx];
	if (0 <= idx) {
		for (int x = 0; x < 2; ++x) {
			const int16_t *mvd_a;
			const int16_t *mvd_b;
			h264d_vector_t mv;
			h264d_vector_t mvd;

			calc_mv8x8_sub4x8(mb, mv.v, mvd_a, mvd_b, avail, lx, idx, blk_idx, pblk, x);
			MvdXY(mb, st, mvd.v, mvd_a, mvd_b);
			mv.v[0] += mvd.v[0];
			mv.v[1] += mvd.v[1];
			p->mv[x][lx].vector = mv.vector;
			p->mvd[x][lx].vector = mvd.vector;
			p->mv[x + 2][lx].vector = mv.vector;
			p->mvd[x + 2][lx].vector = mvd.vector;
		}
	}
}

template <typename F0>
static void sub_mb4x4_mv(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx,
			 F0 MvdXY)
{
	prev8x8_t *p = pblk + blk_idx;
	int idx = p->ref[lx];
	if (0 <= idx) {
		for (int xy = 0; xy < 4; ++xy) {
			const int16_t *mvd_a;
			const int16_t *mvd_b;
			h264d_vector_t mv;
			h264d_vector_t mvd;

			calc_mv8x8_sub4x4(mb, mv.v, mvd_a, mvd_b, avail, lx, idx, blk_idx, pblk, xy);
			MvdXY(mb, st, mvd.v, mvd_a, mvd_b);
			mv.v[0] += mvd.v[0];
			mv.v[1] += mvd.v[1];
			p->mv[xy][lx].vector = mv.vector;
			p->mvd[xy][lx].vector = mvd.vector;
		}
	}
}

struct mvd_xy_cavlc {
	void operator()(h264d_mb_current *mb, dec_bits *st, int16_t mv[], const int16_t mva[], const int16_t mvb[]) const {
	       mv[0] = se_golomb(st);
	       mv[1] = se_golomb(st);
	}
};

static void sub_mb8x8_mv_cavlc(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb8x8_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cavlc());
}

static void sub_mb8x4_mv_cavlc(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb8x4_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cavlc());
}

static void sub_mb4x8_mv_cavlc(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb4x8_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cavlc());
}

static void sub_mb4x4_mv_cavlc(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb4x4_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cavlc());
}

static void (* const sub_mb_p_cavlc[4])(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx) = {
	sub_mb8x8_mv_cavlc,
	sub_mb8x4_mv_cavlc,
	sub_mb4x8_mv_cavlc,
	sub_mb4x4_mv_cavlc,
};

static void (* const sub_mb_b_cavlc[13])(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx) = {
	sub_mb8x8_direct,
	sub_mb8x8_mv_cavlc,
	sub_mb8x8_mv_cavlc,
	sub_mb8x8_mv_cavlc,
	sub_mb8x4_mv_cavlc,
	sub_mb4x8_mv_cavlc,
	sub_mb8x4_mv_cavlc,
	sub_mb4x8_mv_cavlc,
	sub_mb8x4_mv_cavlc,
	sub_mb4x8_mv_cavlc,
	sub_mb4x4_mv_cavlc,
	sub_mb4x4_mv_cavlc,
	sub_mb4x4_mv_cavlc
};

struct sub_mbs_p_cavlc {
	void operator()(h264d_mb_current *mb, dec_bits *st, int avail, int8_t sub_mb_type[], prev8x8_t curr_blk[], int lx) const {
		if (lx == 0) {
			for (int i = 0; i < 4; ++i) {
				sub_mb_p_cavlc[sub_mb_type[i]](mb, st, avail, i, curr_blk, lx);
			}
		}
	}
};

struct sub_mbs_b_cavlc {
	void operator()(h264d_mb_current *mb, dec_bits *st, int avail, int8_t sub_mb_type[], prev8x8_t curr_blk[], int lx) const {
		for (int i = 0; i < 4; ++i) {
			sub_mb_b_cavlc[sub_mb_type[i]](mb, st, avail, i, curr_blk, lx);
		}
	}
};

static void sub_mb8x8_dec(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk)
{
	prev8x8_t *p = pblk + blk_idx;
	static const h264d_vector_t size = {{8, 8}};
	mb->inter_pred(mb, p->ref, p->mv[0], size, (blk_idx & 1) * 8, (blk_idx & 2) * 4);
}

static void sub_mb8x4_dec(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk)

{
	prev8x8_t *p = pblk + blk_idx;
	static const h264d_vector_t size = {{8, 4}};
	for (int y = 0; y < 2; ++y) {
		mb->inter_pred(mb, p->ref, p->mv[y * 2], size, (blk_idx & 1) * 8, ((blk_idx & 2) + y) * 4);
	}
}

static void sub_mb4x8_dec(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk)
{
	prev8x8_t *p = pblk + blk_idx;
	static const h264d_vector_t size = {{4, 8}};
	for (int x = 0; x < 2; ++x) {
		mb->inter_pred(mb, p->ref, p->mv[x], size, (blk_idx & 1) * 8 + x * 4, (blk_idx & 2) * 4);
	}
}

static void sub_mb4x4_dec(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk)
{
	prev8x8_t *p = pblk + blk_idx;
	static const h264d_vector_t size = {{4, 4}};
	for (int xy = 0; xy < 4; ++xy) {
		mb->inter_pred(mb, p->ref, p->mv[xy], size, (blk_idx & 1) * 8 + (xy & 1) * 4, (blk_idx & 2) * 4 + (xy & 2) * 2);
	}
}

static void (* const sub_mb_dec_p[4])(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk) = {
	sub_mb8x8_dec,
	sub_mb8x4_dec,
	sub_mb4x8_dec,
	sub_mb4x4_dec
};

static void (* const sub_mb_dec_b[13])(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk) = {
	pred_direct8x8,
	sub_mb8x8_dec,
	sub_mb8x8_dec,
	sub_mb8x8_dec,
	sub_mb8x4_dec,
	sub_mb4x8_dec,
	sub_mb8x4_dec,
	sub_mb4x8_dec,
	sub_mb8x4_dec,
	sub_mb4x8_dec,
	sub_mb4x4_dec,
	sub_mb4x4_dec,
	sub_mb4x4_dec
};

struct sub_mbs_dec_p {
	void operator()(h264d_mb_current *mb, const int8_t sub_mb_type[], prev8x8_t curr_blk[], int avail) {
		for (int i = 0; i < 4; ++i) {
			sub_mb_dec_p[sub_mb_type[i]](mb, i, curr_blk);
		}
	}
};

struct sub_mbs_dec_b {
	void operator()(h264d_mb_current *mb, const int8_t sub_mb_type[], prev8x8_t curr_blk[], int avail) {
		for (int i = 0; i < 4; ++i) {
			sub_mb_dec_b[sub_mb_type[i]](mb, i, curr_blk);
		}
	}
};

template <int N>
static inline uint32_t str_mv_calc8x8_edge_bidir(uint32_t str, int ref0, int prev_ref0, int offset, const prev8x8_t *p, const prev_mb_t *prev)
{
	int lx = (ref0 != prev_ref0);
	for (int j = 0; j < 2; ++j) {
		if (((str & (2 << ((j + offset) * 2))) == 0)
			&& (DIF_ABS_LARGER_THAN4(p->mv[j * N][lx].v[0], prev->mov[j + offset].mv[0].v[0])
			|| DIF_ABS_LARGER_THAN4(p->mv[j * N][lx].v[1], prev->mov[j + offset].mv[0].v[1])
			|| DIF_ABS_LARGER_THAN4(p->mv[j * N][lx ^ 1].v[0], prev->mov[j + offset].mv[1].v[0])
			|| DIF_ABS_LARGER_THAN4(p->mv[j * N][lx ^ 1].v[1], prev->mov[j + offset].mv[1].v[1]))) {
			str = str | (1 << ((j + offset) * 2));
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_edge_onedir(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const prev8x8_t *p, const prev_mb_t *prev)
{
	int lx_s, lx_d;
	if (0 <= ref0) {
		lx_s = 0;
		lx_d = (ref0 != prev_ref0);
	} else {
		lx_s = 1;
		lx_d = (ref1 != prev_ref0);
	}
	for (int j = 0; j < 2; ++j) {
		if (((str & (2 << ((j + offset) * 2))) == 0)
			&& (DIF_ABS_LARGER_THAN4(p->mv[j * N][lx_s].v[0], prev->mov[j + offset].mv[lx_d].v[0])
			|| DIF_ABS_LARGER_THAN4(p->mv[j * N][lx_s].v[1], prev->mov[j + offset].mv[lx_d].v[1]))) {
			str = str | (1 << ((j + offset) * 2));
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_mv_edge(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const prev8x8_t *p, const prev_mb_t *prev)
{
	if ((0 <= ref0) && (0 <= ref1)) {
		return str_mv_calc8x8_edge_bidir<N>(str, ref0, prev_ref0, offset, p, prev);
	} else {
		return str_mv_calc8x8_edge_onedir<N>(str, ref0, ref1, prev_ref0, offset, p, prev);
	}
}

template <int N>
static uint32_t str_mv_calc8x8_edge(const h264d_mb_current *mb, uint32_t str, const prev8x8_t *p, const prev_mb_t *prev)
{
	for (int i = 0; i < 2; ++i) {
		uint32_t mask = 0xa << (i * 4);
		if ((str & mask) != mask) {
			int prev_ref0 = prev->frmidx[i][0];
			int prev_ref1 = prev->frmidx[i][1];
			int ref0 = frame_idx_of_ref(mb, p[i * N].ref[0], 0);
			int ref1 = frame_idx_of_ref(mb, p[i * N].ref[1], 1);
			if (((prev_ref0 != ref0) || (prev_ref1 != ref1)) && ((prev_ref1 != ref0) || (prev_ref0 != ref1))) {
				mask >>= 1;
				str |= (((str >> 1) ^ mask) & mask);
			} else {
				str = str_mv_calc8x8_mv_edge<N>(str, ref0, ref1, prev_ref0, i * 2, &p[i * N], prev);
			}
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_mid_bidir(uint32_t str, bool same_ref, int offset, const prev8x8_t *p)
{
	if (same_ref) {
		for (int j = 0; j < 2; ++j) {
			if ((str & (2 << ((j + offset) * 2))) == 0) {
				int pmv0x = p->mv[j * N][0].v[0];
				int pmv0y = p->mv[j * N][0].v[1];
				int qmv0x = p->mv[j * N + (3 - N)][0].v[0];
				int qmv0y = p->mv[j * N + (3 - N)][0].v[1];
				int pmv1x = p->mv[j * N][1].v[0];
				int pmv1y = p->mv[j * N][1].v[1];
				int qmv1x = p->mv[j * N + (3 - N)][1].v[0];
				int qmv1y = p->mv[j * N + (3 - N)][1].v[1];
				if ((DIF_ABS_LARGER_THAN4(pmv0x, qmv0x)	|| DIF_ABS_LARGER_THAN4(pmv0y, qmv0y)
					|| DIF_ABS_LARGER_THAN4(pmv1x, qmv1x) || DIF_ABS_LARGER_THAN4(pmv1y, qmv1y))
					&& (DIF_ABS_LARGER_THAN4(pmv0x, qmv1x) || DIF_ABS_LARGER_THAN4(pmv0y, qmv1y)
					|| DIF_ABS_LARGER_THAN4(pmv1x, qmv0x) || DIF_ABS_LARGER_THAN4(pmv1y, qmv0y))) {
					str = str | (1 << ((j + offset) * 2));
				}
			}
		}
	} else {
		for (int j = 0; j < 2; ++j) {
			if ((str & (2 << ((j + offset) * 2))) == 0) {
				if (DIF_ABS_LARGER_THAN4(p->mv[j * N][0].v[0], p->mv[j * N + (3 - N)][0].v[0])
					|| DIF_ABS_LARGER_THAN4(p->mv[j * N][0].v[1], p->mv[j * N + (3 - N)][0].v[1])
					|| DIF_ABS_LARGER_THAN4(p->mv[j * N][1].v[0], p->mv[j * N + (3 - N)][1].v[0])
					|| DIF_ABS_LARGER_THAN4(p->mv[j * N][1].v[1], p->mv[j * N + (3 - N)][1].v[1])) {
					str = str | (1 << ((j + offset) * 2));
				}
			}
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_mid_onedir(uint32_t str, int lx, int offset, const prev8x8_t *p)
{
	for (int j = 0; j < 2; ++j) {
		if ((str & (2 << ((j + offset) * 2))) == 0) {
			if (DIF_ABS_LARGER_THAN4(p->mv[j * N][lx].v[0], p->mv[j * N + (3 - N)][lx].v[0])
				|| DIF_ABS_LARGER_THAN4(p->mv[j * N][lx].v[1], p->mv[j * N + (3 - N)][lx].v[1])) {
				str = str | (1 << ((j + offset) * 2));
			}
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_mv_mid(uint32_t str, int ref0, int ref1, int offset, const prev8x8_t *p)
{
	if ((0 <= ref0) && (0 <= ref1)) {
		return str_mv_calc8x8_mid_bidir<N>(str, ref0 == ref1, offset, p);
	} else {
		return str_mv_calc8x8_mid_onedir<N>(str, (0 <= ref1), offset, p);
	}
}

template <int N>
static inline uint32_t str_mv_calc8x8_half_bidir(uint32_t str, int ref0, int prev_ref0, int offset, const prev8x8_t *p)
{
	int lx = (ref0 != prev_ref0);
	for (int j = 0; j < 2; ++j) {
		if ((str & (2 << ((j + offset) * 2))) == 0) {
			const int16_t *mv0 = p[0].mv[j * N + (3 - N)][0].v;
			const int16_t *mv1a = p[3 - N].mv[j * N][lx].v;
			const int16_t *mv1b = p[3 - N].mv[j * N][lx ^ 1].v;
			if (DIF_ABS_LARGER_THAN4(*mv0++, *mv1a++)
				|| DIF_ABS_LARGER_THAN4(*mv0++, *mv1a)
				|| DIF_ABS_LARGER_THAN4(*mv0++, *mv1b++)
				|| DIF_ABS_LARGER_THAN4(*mv0, *mv1b)) {
				str = str | (1 << ((j + offset) * 2));
			}
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_half_onedir(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const prev8x8_t *p)
{
	int lx_d, lx_s;
	if (0 <= ref0) {
		lx_d = 0;
		lx_s = (ref0 != prev_ref0);
	} else {
		lx_d = 1;
		lx_s = (ref1 != prev_ref0);
	}
	for (int j = 0; j < 2; ++j) {
		if ((str & (2 << ((j + offset) * 2))) == 0) {
			const int16_t *mv0 = p[0].mv[j * N + (3 - N)][lx_s].v;
			const int16_t *mv1 = p[3 - N].mv[j * N][lx_d].v;
			if (DIF_ABS_LARGER_THAN4(*mv0++, *mv1++) || DIF_ABS_LARGER_THAN4(*mv0, *mv1)) {
				str = str | (1 << ((j + offset) * 2));
			}
		}
	}
	return str;
}

template <int N>
static inline uint32_t str_mv_calc8x8_half_mv(uint32_t str, int ref0, int ref1, int prev_ref0, int offset, const prev8x8_t *p)
{
	if ((0 <= ref0) && (0 <= ref1)) {
		return str_mv_calc8x8_half_bidir<N>(str, ref0, prev_ref0, offset, p);
	} else {
		return str_mv_calc8x8_half_onedir<N>(str, ref0, ref1, prev_ref0, offset, p);
	}
}

template <int N>
static inline uint32_t str_mv_calc8x8_half(const h264d_mb_current *mb, uint32_t str, int offset, const prev8x8_t *p)
{
	int prev_ref0 = frame_idx_of_ref(mb, p[0].ref[0], 0);
	int prev_ref1 = frame_idx_of_ref(mb, p[0].ref[1], 1);
	int ref0 = frame_idx_of_ref(mb, p[3 - N].ref[0], 0);
	int ref1 = frame_idx_of_ref(mb, p[3 - N].ref[1], 1);
	if (((prev_ref0 != ref0) || (prev_ref1 != ref1)) && ((prev_ref1 != ref0) || (prev_ref0 != ref1))) {
		uint32_t mask = 5 << (offset * 2);
		str |= (((str >> 1) ^ mask) & mask);
	} else {
		str = str_mv_calc8x8_half_mv<N>(str, ref0, ref1, prev_ref0, offset, p);
	}
	return str;
}

template <int N>
static uint32_t str_mv_calc8x8_inner(const h264d_mb_current *mb, uint32_t str, const prev8x8_t *p)
{
	int ref0, ref1;
	for (int i = 0; i < 2; ++i) {
		ref0 = frame_idx_of_ref(mb, p[i * N].ref[0], 0);
		ref1 = frame_idx_of_ref(mb, p[i * N].ref[1], 1);
		uint32_t mask = 0xa00 << (i * 4);
		if ((str & mask) != mask) {
			str = str_mv_calc8x8_mv_mid<N>(str, ref0, ref1, i * 2 + 4, p + i * N);
		}
	}
	for (int i = 0; i < 2; ++i) {
		uint32_t mask = 0xa0000 << (i * 4);
		if ((str & mask) != mask) {
			str = str_mv_calc8x8_half<N>(mb, str, i * 2 + 8, p + i * N);
		}
	}
	for (int i = 0; i < 2; ++i) {
		ref0 = frame_idx_of_ref(mb, p[i * N + (3 - N)].ref[0], 0);
		ref1 = frame_idx_of_ref(mb, p[i * N + (3 - N)].ref[1], 1);
		uint32_t mask = 0xa000000 << (i * 4);
		if ((str & mask) != mask) {
			str = str_mv_calc8x8_mv_mid<N>(str, ref0, ref1, i * 2 + 12, p + i * N + (3 - N));
		}
	}
	return str;
}

static inline void store_info_intermb8x8(h264d_mb_current *mb, const prev8x8_t *curr_blk, uint32_t left4x4, uint32_t top4x4)
{
	deblock_info_t *deb = mb->deblock_curr;
	deb->qpy = mb->qp;
	deb->qpc[0] = mb->qp_chroma[0];
	deb->qpc[1] = mb->qp_chroma[1];
	if (mb->y != 0) {
		if (mb->top4x4inter->type <= MB_IPCM) {
			deb->str4_vert = 1;
			deb->str_vert |= 0xaa;
		} else {
			deb->str_vert = str_mv_calc8x8_edge<1>(mb, str_previous_coef(deb->str_vert, top4x4), curr_blk, mb->top4x4inter);
		}
	}
	deb->str_vert = str_mv_calc8x8_inner<1>(mb, deb->str_vert, curr_blk);
	if (mb->x != 0) {
		if (mb->left4x4inter->type <= MB_IPCM) {
			deb->str4_horiz = 1;
			deb->str_horiz |= 0xaa;
		} else {
			deb->str_horiz = str_mv_calc8x8_edge<2>(mb, str_previous_coef(deb->str_horiz, left4x4), curr_blk, mb->left4x4inter);
		}
	}
	deb->str_horiz = str_mv_calc8x8_inner<2>(mb, deb->str_horiz, curr_blk);
	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	for (int i = 0; i < 2; ++i) {
		mb->lefttop_mv[i].vector = mb->top4x4inter->mov[3].mv[i].vector;
		mb->top4x4inter->mov[0].mv[i].vector = curr_blk[2].mv[2][i].vector;
		mb->top4x4inter->mov[1].mv[i].vector = curr_blk[2].mv[3][i].vector;
		mb->top4x4inter->mov[2].mv[i].vector = curr_blk[3].mv[2][i].vector;
		mb->top4x4inter->mov[3].mv[i].vector = curr_blk[3].mv[3][i].vector;
		mb->top4x4inter->mvd[0].mv[i].vector = curr_blk[2].mvd[2][i].vector;
		mb->top4x4inter->mvd[1].mv[i].vector = curr_blk[2].mvd[3][i].vector;
		mb->top4x4inter->mvd[2].mv[i].vector = curr_blk[3].mvd[2][i].vector;
		mb->top4x4inter->mvd[3].mv[i].vector = curr_blk[3].mvd[3][i].vector;
		mb->lefttop_ref[i] = mb->top4x4inter->ref[1][i];
		mb->left4x4inter->ref[0][i] = curr_blk[1].ref[i];
		mb->left4x4inter->frmidx[0][i] = frame_idx_of_ref(mb, curr_blk[1].ref[i], i);
		mb->left4x4inter->ref[1][i] = curr_blk[3].ref[i];
		mb->left4x4inter->frmidx[1][i] = frame_idx_of_ref(mb, curr_blk[3].ref[i], i);
		mb->top4x4inter->ref[0][i] = curr_blk[2].ref[i];
		mb->top4x4inter->frmidx[0][i] = frame_idx_of_ref(mb, curr_blk[2].ref[i], i);
		mb->top4x4inter->ref[1][i] = curr_blk[3].ref[i];
		mb->top4x4inter->frmidx[1][i] = frame_idx_of_ref(mb, curr_blk[3].ref[i], i);
	}
	for (int i = 0; i < 4; ++i) {
		const prev8x8_t *p = &curr_blk[(i & 2) + 1];
		int idx = (i & 1) * 2 + 1;
		for (int j = 0; j < 2; ++j) {
			mb->left4x4inter->mov[i].mv[j].vector = p->mv[idx][j].vector;
			mb->left4x4inter->mvd[i].mv[j].vector = p->mvd[idx][j].vector;
		}
	}
}

static inline void store_col8x8(h264d_col_mb_t *col_mb, const prev8x8_t *curr_blk)
{
	int8_t *refdst = col_mb->ref;
	h264d_vector_t *mvdst = col_mb->mv;

	col_mb->type = COL_MB8x8;
	for (int blk = 0; blk < 4; ++blk) {
		int refcol = curr_blk[blk].ref[0];
		const h264d_vector_t *mvcol;

		if (0 <= refcol) {
			mvcol = &curr_blk[blk].mv[0][0];
		} else {
			mvcol = &curr_blk[blk].mv[0][1];
			refcol = curr_blk[blk].ref[1];
		}
		refdst[blk] = refcol;
		mvdst[0].vector = mvcol[0].vector;
		mvdst[1].vector = mvcol[2].vector;
		mvdst[4].vector = mvcol[4].vector;
		mvdst[5].vector = mvcol[6].vector;
		mvdst = mvdst + ((blk & 1) * 4) + 2;
	}
}

struct store_direct8x8_info_b {
	void operator()(h264d_mb_current *mb, const int8_t *sub_mb_type) {
		mb->left4x4inter->direct8x8 = ((sub_mb_type[3] == 0) * 2) | (sub_mb_type[1] == 0);
		mb->top4x4inter->direct8x8 = ((sub_mb_type[3] == 0) * 2) | (sub_mb_type[2] == 0);
	}
};

struct store_direct8x8_info_p {
	void operator()(h264d_mb_current *mb, const int8_t *sub_by_type) {
		mb->left4x4inter->direct8x8 = 0;
		mb->top4x4inter->direct8x8 = 0;
	}
};

template <typename F0, typename F1, typename F2, typename F3, typename F4, typename F5, typename F6, typename F7, typename F8, typename F9, typename F10>
static int mb_inter8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
		       F0 SubMbTypes,
		       F1 RefIdx8x8,
		       F2 SubMbsMv,
		       F3 SubMbsDec,
		       F4 CodedBlockPattern,
		       F5 ResidualLumaInter,
		       F6 NeedTransform8x8,
		       F7 Transform8x8Flag,
		       F8 QpDelta,
		       F9 StoreDirect8x8Info,
		       F10 ResidualBlock)
{
	prev8x8_t curr_blk[4];
	int8_t sub_mb_type[4];
	uint32_t cbp;
	uint32_t left4x4, top4x4;

	memset(curr_blk, 0, sizeof(curr_blk));
	for (int i = 0; i < 4; ++i) {
		curr_blk[i].ref[0] = -1;
		curr_blk[i].ref[1] = -1;
	}
	if (SubMbTypes(mb, st, sub_mb_type, curr_blk, avail) < 0) {
		return -1;
	}
	for (int lx = 0; lx < 2; ++lx) {
		RefIdx8x8(mb, st, sub_mb_type, curr_blk, avail, lx);
	}
	for (int lx = 0; lx < 2; ++lx) {
		SubMbsMv(mb, st, avail, sub_mb_type, curr_blk, lx);
	}
	SubMbsDec(mb, sub_mb_type, curr_blk, avail);
	mb->cbp = cbp = CodedBlockPattern(mb, st, avail);
	left4x4 = mb->left4x4coef;
	top4x4 = *mb->top4x4coef;
	if (cbp) {
		ResidualLumaInter(mb, (NeedTransform8x8(mb, sub_mb_type) << 7) | cbp, st, avail, Transform8x8Flag, QpDelta, ResidualBlock);
	} else {
		no_residual_inter(mb);
	}
	store_info_intermb8x8(mb, curr_blk, left4x4, top4x4);
	StoreDirect8x8Info(mb, sub_mb_type);
	store_col8x8(mb->col_curr, curr_blk);
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc8x8_inner_onedir_center(const h264d_vector_set_t *mv, int top_lx, int bot_lx, uint32_t str, int shift)
{
	uint32_t mask = (N == 8 ? 0x000a0000U : 0x00020000U) << shift;
	const int16_t *mv_top = mv[IS_HORIZ ? 4 / N : 8 - N].mv[top_lx].v;
	const int16_t *mv_bot = mv[IS_HORIZ ? 8 / N : 16 * 8 / (N * N)].mv[bot_lx].v;
	uint32_t bits = 0U;
	for (int x = 0; x < 8 / N; ++x) {
		if ((str & mask) != mask) {
			if (DIF_ABS_LARGER_THAN4(mv_top[0], mv_bot[0])
				|| DIF_ABS_LARGER_THAN4(mv_top[1], mv_bot[1])) {
				bits |= (mask >> 1);
			}
		}
		mv_top += (IS_HORIZ) ? 16 : 4;
		mv_bot += (IS_HORIZ) ? 16 : 4;
		mask <<= 2;
	}
	return bits;
}

template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc4x4_inner_onedir(const h264d_vector_set_t *mv, int lx, int shift, uint32_t str)
{
	uint32_t bits = 0U;
	if (N <= 4) {
		uint32_t mask = 0x000200U << shift;
		const int16_t *mv_top = mv[0].mv[lx].v;
		const int16_t *mv_bot = mv[IS_HORIZ ? 4 / N : 16 / N].mv[lx].v;
		for (int x = 0; x < 2; ++x) {
			if ((str & mask) != mask) {
				if (DIF_ABS_LARGER_THAN4(mv_top[0], mv_bot[0])
					|| DIF_ABS_LARGER_THAN4(mv_top[1], mv_bot[1])) {
					bits |= (mask >> 1);
				}
			}
			mv_top += (IS_HORIZ) ? 16 * 4 / N : 4;
			mv_bot += (IS_HORIZ) ? 16 * 4 / N : 4;
			mask <<= 2;
		}
	}
	return bits;
}

template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc8x8_inner_onedir(const h264d_vector_set_t *mv, int top_lx, int bot_lx, uint32_t str, int shift)
{
	return str_mv_calc4x4_inner_onedir<N, IS_HORIZ>(mv + (IS_HORIZ ? 2 : 8), bot_lx, 16 + shift, str) | str_mv_calc8x8_inner_onedir_center<N, IS_HORIZ>(mv, top_lx, bot_lx, str, shift) | str_mv_calc4x4_inner_onedir<N, IS_HORIZ>(mv, top_lx, shift, str); 
}


template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc8x8_inner_bidir_center(const h264d_vector_set_t *mv, int lx, uint32_t str, int shift)
{
	uint32_t mask = (N == 8 ? 0x000a0000U : 0x00020000U) << shift;
	const int16_t *mv_top = mv[IS_HORIZ ? 4 / N : 8 - N].mv[0].v;
	const int16_t *mv_bot0 = mv[IS_HORIZ ? 8 / N : 16 * 8 / (N * N)].mv[lx].v;
	const int16_t *mv_bot1 = mv[IS_HORIZ ? 8 / N : 16 * 8 / (N * N)].mv[lx ^ 1].v;
	uint32_t bits = 0U;
	for (int x = 0; x < 8 / N; ++x) {
		if ((str & mask) != mask) {
			if (DIF_ABS_LARGER_THAN4(mv_top[0], mv_bot0[0])
				|| DIF_ABS_LARGER_THAN4(mv_top[1], mv_bot0[1])
				|| DIF_ABS_LARGER_THAN4(mv_top[2], mv_bot1[0])
				|| DIF_ABS_LARGER_THAN4(mv_top[3], mv_bot1[1])) {
				bits |= (mask >> 1);
			}
		}
		mv_top += (IS_HORIZ) ? 16 : 4;
		mv_bot0 += (IS_HORIZ) ? 16 : 4;
		mv_bot1 += (IS_HORIZ) ? 16 : 4;
		mask <<= 2;
	}
	return bits;
}

template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc4x4_inner_bidir(const h264d_vector_set_t *mv, int shift, uint32_t str)
{
	uint32_t bits = 0U;
	if (N <= 4) {
		uint32_t mask = 0x000200U << shift;
		const int16_t *mv_top = mv[0].mv[0].v;
		const int16_t *mv_bot = mv[IS_HORIZ ? 4 / N : 16 / N].mv[0].v;
		for (int x = 0; x < 2; ++x) {
			if ((str & mask) != mask) {
				if (DIF_ABS_LARGER_THAN4(mv_top[0], mv_bot[0])
					|| DIF_ABS_LARGER_THAN4(mv_top[1], mv_bot[1])
					|| DIF_ABS_LARGER_THAN4(mv_top[2], mv_bot[2])
					|| DIF_ABS_LARGER_THAN4(mv_top[3], mv_bot[3])) {
					bits |= (mask >> 1);
				}
			}
			mv_top += (IS_HORIZ) ? 16 * 4 / N : 4;
			mv_bot += (IS_HORIZ) ? 16 * 4 / N : 4;
			mask <<= 2;
		}
	}
	return bits;
}

template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc8x8_inner_bidir(const h264d_vector_set_t *mv, int lx, uint32_t str, int shift)
{
	return str_mv_calc4x4_inner_bidir<N, IS_HORIZ>(mv + (IS_HORIZ ? 2 : 8), 16 + shift, str) | str_mv_calc8x8_inner_bidir_center<N, IS_HORIZ>(mv, lx, str, shift) | str_mv_calc4x4_inner_bidir<N, IS_HORIZ>(mv, shift, str); 
}

template <int N, int IS_HORIZ>
static inline uint32_t str_mv_calc8x8_inner(const h264d_mb_current *mb, uint32_t str, const int8_t ref_idx[], const h264d_vector_set_t *mv)
{
	uint32_t mask = 0U;
	for (int x = 0; x < 2; ++x) {
		int top_ref0 = frame_idx_of_ref(mb, ref_idx[0], 0);
		int top_ref1 = frame_idx_of_ref(mb, ref_idx[1], 1);
		int bot_ref0 = frame_idx_of_ref(mb, ref_idx[IS_HORIZ ? 2 : 4], 0);
		int bot_ref1 = frame_idx_of_ref(mb, ref_idx[IS_HORIZ ? 3 : 5], 1);
		int shift = x * 4;
		uint32_t str_bits;
		if ((top_ref0 != bot_ref0 || top_ref1 != bot_ref1) && (top_ref1 != bot_ref0 || top_ref0 != bot_ref1)) {
			if ((0 <= top_ref0) && (0 <= top_ref1)) {
				str_bits = str_mv_calc4x4_inner_bidir<N, IS_HORIZ>(mv + (IS_HORIZ ? 2 : 8), 16 + shift, str) | str_mv_calc4x4_inner_bidir<N, IS_HORIZ>(mv, shift, str) | (0x00050000U << shift);
			} else {
				str_bits = str_mv_calc4x4_inner_onedir<N, IS_HORIZ>(mv + (IS_HORIZ ? 2 : 8), 0 <= bot_ref1, 16 + shift, str) | str_mv_calc4x4_inner_onedir<N, IS_HORIZ>(mv, 0 <= top_ref1, shift, str) | (0x00050000U << shift);
			}
		} else {
			if ((0 <= top_ref0) && (0 <= top_ref1)) {
				str_bits = str_mv_calc8x8_inner_bidir<N, IS_HORIZ>(mv, top_ref0 != bot_ref0, str, shift);
			} else {
				str_bits = str_mv_calc8x8_inner_onedir<N, IS_HORIZ>(mv, top_ref0 < 0, bot_ref0 < 0, str, shift);
			}
		}
		mask |= str_bits;
		ref_idx += IS_HORIZ ? 4 : 2;
		mv += IS_HORIZ ? (64 * 2 / (N * N)) : (8 / N);
	}
	return str | (((str >> 1) ^ mask) & mask);
}

template <int N>
static void store_info_inter8x8(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4)
{
	deblock_info_t *deb = mb->deblock_curr;
	deb->qpy = mb->qp;
	deb->qpc[0] = mb->qp_chroma[0];
	deb->qpc[1] = mb->qp_chroma[1];
	if (mb->y != 0) {
		store_str_inter8xedge<0, 1, 8 / N>(mb, mb->top4x4inter, deb->str4_vert, mv, ref_idx, deb->str_vert, top4x4);
	}
	deb->str_vert = str_mv_calc8x8_inner<N, 0>(mb, deb->str_vert, ref_idx, mv);
	if (mb->x != 0) {
		store_str_inter8xedge<1, 16 / N, 8 / N>(mb, mb->left4x4inter, deb->str4_horiz, mv, ref_idx, deb->str_horiz, left4x4);
	}
	deb->str_horiz = str_mv_calc8x8_inner<N, 1>(mb, deb->str_horiz, ref_idx, mv);

	mb->left4x4pred = 0x22222222;
	*mb->top4x4pred = 0x22222222;
	for (int i = 0; i < 2; ++i) {
		int t;
		mb->lefttop_ref[i] = mb->top4x4inter->ref[1][i];
		mb->lefttop_mv[i].vector = mb->top4x4inter->mov[3].mv[i].vector;
		t = ref_idx[i * 2 + 4];
		mb->top4x4inter->ref[i][0] = t;
		mb->top4x4inter->frmidx[i][0] = frame_idx_of_ref(mb, t, 0);
		t = ref_idx[i * 2 + 5];
		mb->top4x4inter->ref[i][1] = t;
		mb->top4x4inter->frmidx[i][1] = frame_idx_of_ref(mb, t, 1);
		t = ref_idx[i * 4 + 2];
		mb->left4x4inter->ref[i][0] = t;
		mb->left4x4inter->frmidx[i][0] = frame_idx_of_ref(mb, t, 0);
		t = ref_idx[i * 4 + 3];
		mb->left4x4inter->ref[i][1] = t;
		mb->left4x4inter->frmidx[i][1] = frame_idx_of_ref(mb, t, 1);
	}
	for (int i = 0; i < 4; ++i) {
		mb->top4x4inter->mov[i] = mv[(i >> (N >> 3)) + ((N == 4) ? 12 : 2)];
		mb->left4x4inter->mov[i] = mv[(i >> (N >> 3)) * (16 / N) + ((N == 4) ? 3 : 1)];
	}
	memset(mb->top4x4inter->mvd, 0, sizeof(mb->top4x4inter->mvd));
	memset(mb->left4x4inter->mvd, 0, sizeof(mb->left4x4inter->mvd));

	h264d_col_mb_t *col_mb = mb->col_curr;
	int8_t *refdst = col_mb->ref;
	h264d_vector_t *mvdst = col_mb->mv;

	col_mb->type = COL_MB8x8;
	for (int blk = 0; blk < 4; ++blk) {
		int refcol = ref_idx[blk * 2];
		int lx;

		if (0 <= refcol) {
			lx = 0;
		} else {
			lx = 1;
			refcol = ref_idx[blk * 2 + 1];
		}
		refdst[blk] = refcol;
		if (N == 4) {
			mvdst[0].vector = mv[0].mv[lx].vector;
			mvdst[1].vector = mv[1].mv[lx].vector;
			mvdst[4].vector = mv[4].mv[lx].vector;
			mvdst[5].vector = mv[5].mv[lx].vector;
			if (blk & 1) {
				mvdst += 6;
				mv += 6;
			} else {
				mvdst += 2;
				mv += 2;
			}
		} else {
			uint32_t src = mv[0].mv[lx].vector;
			mvdst[0].vector = src;
			mvdst[1].vector = src;
			mvdst[4].vector = src;
			mvdst[5].vector = src;
			mv += 1;
			if (blk & 1) {
				mvdst += 6;
			} else {
				mvdst += 2;
			}
		}
	}
}

template <int DIRECT8x8INFERENCE>
static void store_info_inter(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4, int mb_type)
{
	static void (* const store_func[4])(h264d_mb_current *mb, const h264d_vector_set_t mv[], const int8_t ref_idx[], uint32_t left4x4, uint32_t top4x4) = {
		store_info_inter16x16,
		store_info_inter16x8,
		store_info_inter8x16,
		store_info_inter8x8<(DIRECT8x8INFERENCE + 1) * 4>
	};
	store_func[mb_type](mb, mv, ref_idx, left4x4, top4x4);
}

template <typename F0, typename F1, typename F2, typename F3, typename F4>
static int mb_bdirect16x16(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail,
			   F0 CodedBlockPattern,
			   F1 ResidualLumaInter,
			   F2 Transform8x8Flag,
			   F3 QpDelta,
			   F4 ResidualBlock)
{
	h264d_vector_set_t mv[16 * 2];
	int8_t ref_idx[2 * 4];
	uint32_t left4x4, top4x4;
	uint32_t cbp;

	mb->bdirect->func->direct16x16(mb, ref_idx, mv);
	left4x4 = mb->left4x4coef;
	top4x4 = *mb->top4x4coef;
	mb->cbp = cbp = CodedBlockPattern(mb, st, avail);
	if (cbp) {
		ResidualLumaInter(mb, 0x80 | cbp, st, avail, Transform8x8Flag, QpDelta, ResidualBlock);
	} else {
		no_residual_inter(mb);
	}
	const h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
	const h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
	mb->bdirect->func->store_info_inter(mb, mv, ref_idx, left4x4, top4x4, col_mb->type);
	mb->left4x4inter->direct8x8 = 3;
	mb->top4x4inter->direct8x8 = 3;
	return residual_chroma(mb, cbp, st, avail, ResidualBlock);
}

struct sub_mb_type_p_cavlc {
	int operator()(h264d_mb_current *mb, dec_bits *st, int8_t *sub_mb_type, prev8x8_t *curr_blk, int avail) const {
		for (int i = 0; i < 4; ++i) {
			READ_UE_RANGE(sub_mb_type[i], st, 3);
		}
		return 0;
	}
};


template <typename F>
static inline int sub_mb_type_b_base(h264d_mb_current *mb, dec_bits *st, int8_t sub_mb_type[], prev8x8_t curr_blk[], int avail,
				      F SubMbTypeB)
{
	prev8x8_t ref_blk;
	int type0_cnt = 0;
	for (int i = 0; i < 4; ++i) {
		int type = SubMbTypeB(mb, st);
		if (type < 0) {
			return -1;
		}
		sub_mb_type[i] = type;
		if (type == 0) {
			mb->bdirect->func->direct8x8(mb, i, curr_blk, avail, &ref_blk, type0_cnt++);
		}
	}
	return 0;
}


struct sub_mb_type_b_cavlc {
	int operator()(h264d_mb_current *mb, dec_bits *st) const {
		int sub_mb_type;
		READ_UE_RANGE(sub_mb_type, st, 12);
		return sub_mb_type;
	}
};

struct sub_mb_types_b_cavlc {
	int operator()(h264d_mb_current *mb, dec_bits *st, int8_t *sub_mb_type, prev8x8_t *curr_blk, int avail) const {
		return sub_mb_type_b_base(mb, st, sub_mb_type, curr_blk, avail, sub_mb_type_b_cavlc());
	}
};

struct ref_idx16x16_cavlc {
	 int operator()(h264d_mb_current *mb, dec_bits *st, int lx, int avail) const {
		int t = *(mb->num_ref_idx_lx_active_minus1[lx]);
		return t ? te_golomb(st, t) : 0;
	}
};

struct ref_idx16x8_cavlc {
	void operator()(h264d_mb_current *mb, dec_bits *st, int8_t *ref_idx, uint32_t blk_map, int avail) const {
		int8_t * const *num = mb->num_ref_idx_lx_active_minus1;
		for (int lx = 0; lx < 2; ++lx) {
			int t = *(num[0]);
			ref_idx[0] = (blk_map & 1) ? (t ? te_golomb(st, t) : 0) : -1;
			ref_idx[2] = (blk_map & 2) ? (t ? te_golomb(st, t) : 0) : -1;
			blk_map >>= 2;
			ref_idx++;
			num++;
		}
	}
};

struct ref_idx8x8_cavlc {
	void operator()(h264d_mb_current *mb, dec_bits *st, const int8_t *sub_mb_type, prev8x8_t *pblk, int avail, int lx) const {
		int t = (mb->type != MB_P8x8REF0) ? *(mb->num_ref_idx_lx_active_minus1[lx]) : 0;
		int dir = 1 << lx;
		const int8_t *sub_mb_ref_map = mb->sub_mb_ref_map;
		for (int i = 0; i < 4; ++i) {
			int sub_dir = sub_mb_ref_map[*sub_mb_type++];
			if (0 <= sub_dir) {
				pblk[i].ref[lx] = (dir & sub_dir) ? (t ? te_golomb(st, t) : 0) : -1;
			}
		}
	}
};

static int mb_inter16x16_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x16(mb, mbc, st, avail, ref_idx16x16_cavlc(), mvd_xy_cavlc(), cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cavlc(), residual_block_cavlc());
}

static int mb_inter16x8_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x8(mb, mbc, st, avail, ref_idx16x8_cavlc(), mvd_xy_cavlc(), cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cavlc(), residual_block_cavlc());
}

static int mb_inter8x16_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x16(mb, mbc, st, avail, ref_idx16x8_cavlc(), mvd_xy_cavlc(), cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cavlc(), residual_block_cavlc());
}

struct not_need_transform_size_8x8 {
	bool operator()(const h264d_mb_current *mb, const int8_t sub_mb_type[]) const {
		return false;
	}
};

struct need_transform_size_8x8p {
	bool operator()(const h264d_mb_current *mb, const int8_t sub_mb_type[]) const {
		return (sub_mb_type[0] == 0) && (sub_mb_type[1] == 0) && (sub_mb_type[2] == 0) && (sub_mb_type[3] == 0);
	}
};

struct need_transform_size_8x8b {
	bool operator()(const h264d_mb_current *mb, const int8_t sub_mb_type[]) const {
		return mb->bdirect->func->need_transform_size_8x8_flag(sub_mb_type);	
}
};

static int mb_inter8x8p_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_type_p_cavlc(), ref_idx8x8_cavlc(), sub_mbs_p_cavlc(), sub_mbs_dec_p(), cbp_inter_cavlc(), residual_luma_inter(), not_need_transform_size_8x8(), transform_size_8x8_flag_dummy(), qp_delta_cavlc(), store_direct8x8_info_p(), residual_block_cavlc());
}

static int mb_inter8x8b_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_types_b_cavlc(), ref_idx8x8_cavlc(), sub_mbs_b_cavlc(), sub_mbs_dec_b(), cbp_inter_cavlc(), residual_luma_inter(), not_need_transform_size_8x8(), transform_size_8x8_flag_dummy(), qp_delta_cavlc(), store_direct8x8_info_b(), residual_block_cavlc());
}

static int mb_bdirect16x16_cavlc(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_bdirect16x16(mb, mbc, st, avail, cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cavlc(), residual_block_cavlc());
}

static int mb_inter16x16_cavlc8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x16(mb, mbc, st, avail, ref_idx16x16_cavlc(), mvd_xy_cavlc(), cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
}

static int mb_inter16x8_cavlc8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x8(mb, mbc, st, avail, ref_idx16x8_cavlc(), mvd_xy_cavlc(), cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
}

static int mb_inter8x16_cavlc8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x16(mb, mbc, st, avail, ref_idx16x8_cavlc(), mvd_xy_cavlc(), cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
}

static int mb_inter8x8p_cavlc8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_type_p_cavlc(), ref_idx8x8_cavlc(), sub_mbs_p_cavlc(), sub_mbs_dec_p(), cbp_inter_cavlc(), residual_luma_inter(), need_transform_size_8x8p(), transform_size_8x8_flag_cavlc(), qp_delta_cavlc(), store_direct8x8_info_p(), residual_block_cavlc());
}

static int mb_inter8x8b_cavlc8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_types_b_cavlc(), ref_idx8x8_cavlc(), sub_mbs_b_cavlc(), sub_mbs_dec_b(), cbp_inter_cavlc(), residual_luma_inter(), need_transform_size_8x8b(), transform_size_8x8_flag_cavlc(), qp_delta_cavlc(), store_direct8x8_info_b(), residual_block_cavlc());
}

static int mb_bdirect16x16_cavlc8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_bdirect16x16(mb, mbc, st, avail, cbp_inter_cavlc(), residual_luma_inter(), transform_size_8x8_flag_cavlc(), qp_delta_cavlc(), residual_block_cavlc());
}

static const mb_code mb_decode[2][54] = {
	{
		{mb_intra4x4_cavlc, 0, 0},
		{mb_intra16x16_dconly_cavlc, mb_intra16xpred_vert<16>, 0},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_horiz<16>, 0},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_dc<16>, 0},
		{mb_intra16x16_dconly_cavlc, mb_intra16x16pred_planer, 0},
		{mb_intra16x16_dconly_cavlc, mb_intra16xpred_vert<16>, 0x10},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_horiz<16>, 0x10},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_dc<16>, 0x10},
		{mb_intra16x16_dconly_cavlc, mb_intra16x16pred_planer, 0x10},
		{mb_intra16x16_dconly_cavlc, mb_intra16xpred_vert<16>, 0x20},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_horiz<16>, 0x20},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_dc<16>, 0x20},
		{mb_intra16x16_dconly_cavlc, mb_intra16x16pred_planer, 0x20},
		{mb_intra16x16_acdc_cavlc, mb_intra16xpred_vert<16>, 0x0f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_horiz<16>, 0x0f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_dc<16>, 0x0f},
		{mb_intra16x16_acdc_cavlc, mb_intra16x16pred_planer, 0x0f},
		{mb_intra16x16_acdc_cavlc, mb_intra16xpred_vert<16>, 0x1f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_horiz<16>, 0x1f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_dc<16>, 0x1f},
		{mb_intra16x16_acdc_cavlc, mb_intra16x16pred_planer, 0x1f},
		{mb_intra16x16_acdc_cavlc, mb_intra16xpred_vert<16>, 0x2f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_horiz<16>, 0x2f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_dc<16>, 0x2f},
		{mb_intra16x16_acdc_cavlc, mb_intra16x16pred_planer, 0x2f},
		{mb_intrapcm, 0, 0},
		{mb_inter16x16_cavlc, 0, 1},
		{mb_inter16x8_cavlc, 0, 3},
		{mb_inter8x16_cavlc, 0, 3},
		{mb_inter8x8p_cavlc, 0, 0xf},
		{mb_inter8x8p_cavlc, 0, 0xf},
		{mb_bdirect16x16_cavlc, 0, 0},
		{mb_inter16x16_cavlc, 0, 1}, {mb_inter16x16_cavlc, 0, 2}, {mb_inter16x16_cavlc, 0, 3},
		{mb_inter16x8_cavlc, 0, 0x3}, {mb_inter8x16_cavlc, 0, 0x3},
		{mb_inter16x8_cavlc, 0, 0xc}, {mb_inter8x16_cavlc, 0, 0xc},
		{mb_inter16x8_cavlc, 0, 0x9}, {mb_inter8x16_cavlc, 0, 0x9},
		{mb_inter16x8_cavlc, 0, 0x6}, {mb_inter8x16_cavlc, 0, 0x6},
		{mb_inter16x8_cavlc, 0, 0xb}, {mb_inter8x16_cavlc, 0, 0xb},
		{mb_inter16x8_cavlc, 0, 0xe}, {mb_inter8x16_cavlc, 0, 0xe},
		{mb_inter16x8_cavlc, 0, 0x7}, {mb_inter8x16_cavlc, 0, 0x7},
		{mb_inter16x8_cavlc, 0, 0xd}, {mb_inter8x16_cavlc, 0, 0xd},
		{mb_inter16x8_cavlc, 0, 0xf}, {mb_inter8x16_cavlc, 0, 0xf},
		{mb_inter8x8b_cavlc, 0, 0}
	},
	{
		{mb_intraNxN_cavlc, 0, 0},
		{mb_intra16x16_dconly_cavlc, mb_intra16xpred_vert<16>, 0},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_horiz<16>, 0},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_dc<16>, 0},
		{mb_intra16x16_dconly_cavlc, mb_intra16x16pred_planer, 0},
		{mb_intra16x16_dconly_cavlc, mb_intra16xpred_vert<16>, 0x10},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_horiz<16>, 0x10},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_dc<16>, 0x10},
		{mb_intra16x16_dconly_cavlc, mb_intra16x16pred_planer, 0x10},
		{mb_intra16x16_dconly_cavlc, mb_intra16xpred_vert<16>, 0x20},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_horiz<16>, 0x20},
		{mb_intra16x16_dconly_cavlc, intraNxNpred_dc<16>, 0x20},
		{mb_intra16x16_dconly_cavlc, mb_intra16x16pred_planer, 0x20},
		{mb_intra16x16_acdc_cavlc, mb_intra16xpred_vert<16>, 0x0f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_horiz<16>, 0x0f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_dc<16>, 0x0f},
		{mb_intra16x16_acdc_cavlc, mb_intra16x16pred_planer, 0x0f},
		{mb_intra16x16_acdc_cavlc, mb_intra16xpred_vert<16>, 0x1f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_horiz<16>, 0x1f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_dc<16>, 0x1f},
		{mb_intra16x16_acdc_cavlc, mb_intra16x16pred_planer, 0x1f},
		{mb_intra16x16_acdc_cavlc, mb_intra16xpred_vert<16>, 0x2f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_horiz<16>, 0x2f},
		{mb_intra16x16_acdc_cavlc, intraNxNpred_dc<16>, 0x2f},
		{mb_intra16x16_acdc_cavlc, mb_intra16x16pred_planer, 0x2f},
		{mb_intrapcm, 0, 0},
		{mb_inter16x16_cavlc8x8, 0, 1},
		{mb_inter16x8_cavlc8x8, 0, 3},
		{mb_inter8x16_cavlc8x8, 0, 3},
		{mb_inter8x8p_cavlc8x8, 0, 0xf},
		{mb_inter8x8p_cavlc8x8, 0, 0xf},
		{mb_bdirect16x16_cavlc8x8, 0, 0},
		{mb_inter16x16_cavlc8x8, 0, 1}, {mb_inter16x16_cavlc8x8, 0, 2}, {mb_inter16x16_cavlc8x8, 0, 3},
		{mb_inter16x8_cavlc8x8, 0, 0x3}, {mb_inter8x16_cavlc8x8, 0, 0x3},
		{mb_inter16x8_cavlc8x8, 0, 0xc}, {mb_inter8x16_cavlc8x8, 0, 0xc},
		{mb_inter16x8_cavlc8x8, 0, 0x9}, {mb_inter8x16_cavlc8x8, 0, 0x9},
		{mb_inter16x8_cavlc8x8, 0, 0x6}, {mb_inter8x16_cavlc8x8, 0, 0x6},
		{mb_inter16x8_cavlc8x8, 0, 0xb}, {mb_inter8x16_cavlc8x8, 0, 0xb},
		{mb_inter16x8_cavlc8x8, 0, 0xe}, {mb_inter8x16_cavlc8x8, 0, 0xe},
		{mb_inter16x8_cavlc8x8, 0, 0x7}, {mb_inter8x16_cavlc8x8, 0, 0x7},
		{mb_inter16x8_cavlc8x8, 0, 0xd}, {mb_inter8x16_cavlc8x8, 0, 0xd},
		{mb_inter16x8_cavlc8x8, 0, 0xf}, {mb_inter8x16_cavlc8x8, 0, 0xf},
		{mb_inter8x8b_cavlc8x8, 0, 0}
	}
};

/** Convert MB type number into unified order:
 * Intra < Inter < Bidirectional
 */
static int adjust_mb_type(int mb_type, int slice_type)
{
	if (slice_type == P_SLICE) {
		if (mb_type <= 30) {
			mb_type -= 5;
			return mb_type < 0 ? mb_type + MB_BDIRECT16x16 : mb_type;
		} else {
			return -1;
		}
	} else if (slice_type == B_SLICE) {
		mb_type -= 23;
		return mb_type < 0 ? mb_type + 23 + MB_BDIRECT16x16 : mb_type;
	} else if ((slice_type == I_SLICE) && (mb_type <= 25)) {
		return  mb_type;
	} else {
		return -1;
	}
}

static inline int get_availability(h264d_mb_current *mb)
{
	int mbx, max_x, firstline;

	mbx = mb->x;
	max_x = mb->max_x;
	firstline = mb->firstline;
	return ((mbx != 0 && firstline < 0) * 8) /* bit3: top left */
		| ((mbx != max_x - 1 && firstline <= 1) * 4) /* bit2: top right */
		| ((firstline <= 0) * 2) /* bit1: top */
		| (mbx != 0 && firstline != max_x); /* bit0: left */
}

static inline int macroblock_layer_cabac(h264d_mb_current *mb, h264d_slice_header *hdr, dec_bits *st);

static inline int macroblock_layer(h264d_mb_current *mb, h264d_slice_header *hdr, dec_bits *st)
{
	const mb_code *mbc;
	int mbtype;
	int avail;

	READ_UE_RANGE(mbtype, st, 48);
	if ((mb->type = mbtype = adjust_mb_type(mbtype, hdr->slice_type)) < 0) {
		return -1;
	}
	mbc = &mb->mb_decode[mbtype];
	avail = get_availability(mb);
	mbc->mb_dec(mb, mbc, st, avail);
	VC_CHECK;
	return 0;
}

static void calc_mv_pskip(h264d_mb_current *mb, int16_t mv[], int avail)
{
	prev_mb_t *pmb;
	int16_t pmv[2];
	const int16_t *mvd_a, *mvd_b; /* ignored */

	mv[0] = 0;
	mv[1] = 0;
	if ((avail & 3) != 3) {
		return;
	}
	pmb = mb->left4x4inter;
	if (pmb->ref[0][0] == 0 && pmb->mov[0].mv[0].vector == 0U) {
		return;
	}
	pmb = mb->top4x4inter;
	if (pmb->ref[0][0] == 0 && pmb->mov[0].mv[0].vector == 0U) {
		return;
	}
	calc_mv16x16(mb, pmv, mvd_a, mvd_b, 0, 0, avail);
	mv[0] = pmv[0];
	mv[1] = pmv[1];
}

static void p_skip_mb(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	static const h264d_vector_t size = {{16, 16}};
	calc_mv_pskip(mb, mv->mv->v, get_availability(mb));
	memset(&mv[1], 0, sizeof(mv[1]));
	mb->inter_pred(mb, ref_idx, mv->mv, size, 0, 0);
}

template <int BLOCK>
static inline void fill_bskip_mv(h264d_vector_t *mv)
{
	uint32_t *mv32 = &(mv->vector);
	uint32_t d0 = mv32[0];
	uint32_t d1 = mv32[1];
	int i = (4 * 64 / (BLOCK * BLOCK)) - 1;
	do {
		mv32 += 2;
		mv32[0] = d0;
		mv32[1] = d1;
	} while (--i);
}

static void direct_mv_pred_nocol(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	static const h264d_vector_t size = {{16, 16}};
	direct_mv_pred(mb, ref_idx, mv, size, 0, 0);
	((h264d_col_mb_t *)col_mb)->type = COL_MB16x16;
	memset(&mv[2], 0, sizeof(mv[2]) * 2);
}

template <typename F>
static inline void pred_direct16x16_col_base16x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv,
						      F PredDirectCol)
{
	static const h264d_vector_t size = {{16, 16}};
	if (col_mb->ref[0] == 0) {
		const h264d_vector_t *mvcol = &col_mb->mv[0];
		pred_direct_block<16, 8>(mb, mvcol, ref_idx, mv, 0, PredDirectCol);
	} else {
		direct_mv_pred(mb, ref_idx, mv, size, 0, 0);
	}
	memset(&mv[2], 0, sizeof(mv[2]) * 2);
}

template <typename F>
static inline void pred_direct16x16_col_base16x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv,
						     F PredDirectCol)
{
	static const h264d_vector_t size = {{16, 8}};
	memcpy(&mv[2], &mv[0], sizeof(mv[0]) * 2);
	for (int y = 0; y < 2; ++y) {
		if (col_mb->ref[y * 2] == 0) {
			const h264d_vector_t *mvcol = &col_mb->mv[y * 8];
			pred_direct_block<16, 8>(mb, mvcol, ref_idx, mv, y * 2, PredDirectCol);
		} else {
			direct_mv_pred(mb, ref_idx, mv, size, 0, y * 8);
		}
		mv += 2;
	}
	memset(&mv[0], 0, sizeof(mv[0]) * 4);
}

template <typename F>
static inline void pred_direct16x16_col_base8x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv,
						     F PredDirectCol)
{
	static const h264d_vector_t size = {{8, 16}};
	memcpy(&mv[2], &mv[0], sizeof(mv[0]) * 2);
	for (int x = 0; x < 2; ++x) {
		if (col_mb->ref[x] == 0) {
			const h264d_vector_t *mvcol = &col_mb->mv[x * 2];
			pred_direct_block<16, 8>(mb, mvcol, ref_idx, mv, x, PredDirectCol);
		} else {
			direct_mv_pred(mb, ref_idx, mv, size, x * 8, 0);
		}
		mv += 2;
	}
	memset(&mv[0], 0, sizeof(mv[0]) * 4);
}

template <int BLOCK, typename F>
static inline void pred_direct16x16_col_base8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv,
					    F PredDirectCol)
{
	static const h264d_vector_t size = {{8, 8}};
	fill_bskip_mv<BLOCK>(mv);
	for (int blk8x8 = 0; blk8x8 < 4; ++blk8x8) {
		int yoffset = (blk8x8 & 2) * 4;
		if (col_mb->ref[blk8x8] == 0) {
			const h264d_vector_t *mvcol = &col_mb->mv[(blk8x8 & 2) * ((BLOCK == 8) ? 6 : 4) + (blk8x8 & 1) * ((BLOCK == 8) ? 3 : 2)];
			pred_direct_block<16, BLOCK>(mb, mvcol, ref_idx, mv, blk8x8, PredDirectCol);
		} else {
			direct_mv_pred(mb, ref_idx, mv, size, (blk8x8 & 1) * 8, yoffset);
		}
		mv += (BLOCK == 8) ? 2 : ((blk8x8 & 1) ? 12 : 4);
	}
}

static void pred_direct16x16_col_ref1_16x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base16x16(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<0, 16, 16, 16>());
}

static void pred_direct16x16_col_ref2_16x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base16x16(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<1, 16, 16, 16>());
}

static void pred_direct16x16_col_ref3_16x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base16x16(mb, col_mb, ref_idx, mv, pred_direct_col_block_bidir<16, 16, 16>());
}

static void pred_direct16x16_col_ref1_16x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base16x8(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<0, 16, 16, 8>());
}

static void pred_direct16x16_col_ref2_16x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base16x8(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<1, 16, 16, 8>());
}

static void pred_direct16x16_col_ref3_16x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base16x8(mb, col_mb, ref_idx, mv, pred_direct_col_block_bidir<16, 16, 8>());
}

static void pred_direct16x16_col_ref1_8x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x16(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<0, 16, 8, 16>());
}

static void pred_direct16x16_col_ref2_8x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x16(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<1, 16, 8, 16>());
}

static void pred_direct16x16_col_ref3_8x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x16(mb, col_mb, ref_idx, mv, pred_direct_col_block_bidir<16, 8, 16>());
}

static void pred_direct16x16_col_ref1_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x8<8>(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<0, 16, 8, 8>());
}

static void pred_direct16x16_col_ref2_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x8<8>(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<1, 16, 8, 8>());
}

static void pred_direct16x16_col_ref3_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x8<8>(mb, col_mb, ref_idx, mv, pred_direct_col_block_bidir<16, 8, 8>());
}

static void pred_direct16x16_col_ref1_4x4(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x8<4>(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<0, 16, 4, 4>());
}

static void pred_direct16x16_col_ref2_4x4(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x8<4>(mb, col_mb, ref_idx, mv, pred_direct_col_block_onedir<1, 16, 4, 4>());
}

static void pred_direct16x16_col_ref3_4x4(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv)
{
	pred_direct16x16_col_base8x8<4>(mb, col_mb, ref_idx, mv, pred_direct_col_block_bidir<16, 4, 4>());
}

static void (* const pred_direct16x16_col[3][4])(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, const int8_t *ref_idx, h264d_vector_t *mv) = {
	{
		direct_mv_pred_nocol,
		pred_direct16x16_col_ref1_16x16,
		pred_direct16x16_col_ref2_16x16,
		pred_direct16x16_col_ref3_16x16
	},
	{
		direct_mv_pred_nocol,
		pred_direct16x16_col_ref1_16x8,
		pred_direct16x16_col_ref2_16x8,
		pred_direct16x16_col_ref3_16x8
	},
	{
		direct_mv_pred_nocol,
		pred_direct16x16_col_ref1_8x16,
		pred_direct16x16_col_ref2_8x16,
		pred_direct16x16_col_ref3_8x16
	}
};

static void pred_direct16x16(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_t *mv)
{
	h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
	h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
	static const h264d_vector_t size = {{16, 16}};
	if ((0 <= ref_idx[0]) || (0 <= ref_idx[1])) {
		if (colpic->in_use == SHORT_TERM) {
			int refs = (ref_idx[0] == 0) + (ref_idx[1] == 0) * 2;
			if (col_mb->type == COL_MB8x8) {
				mb->bdirect->func->direct16x16_col8x8[refs](mb, col_mb, ref_idx, mv);
			} else {
				pred_direct16x16_col[col_mb->type][refs](mb, col_mb, ref_idx, mv);
			}
		} else {
			col_mb->type = COL_MB16x16;
			memset(&mv[2], 0, sizeof(mv[2]) * 2);
			direct_mv_pred(mb, ref_idx, mv, size, 0, 0);
		}
	} else {
		ref_idx[0] = 0;
		ref_idx[1] = 0;
		col_mb->type = COL_MB16x16;
		memset(&mv[2], 0, sizeof(mv[2]) * 2);
		mb->inter_pred(mb, ref_idx, mv, size, 0, 0);
	}
}

static void b_skip_mb_spatial(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	int avail = get_availability(mb);
	b_direct_ref_mv_calc(mb, avail, ref_idx, mv->mv->v);
	for (int i = 1; i < 4; ++i) {
		ref_idx[i * 2] = ref_idx[0];
		ref_idx[i * 2 + 1] = ref_idx[1];
	}
	pred_direct16x16(mb, ref_idx, mv->mv);
}

template <int N, int BLOCK>
static inline void fill_temporal_mv(h264d_vector_t *mv)
{
	if (4 < BLOCK) { 
		uint32_t xy0 = mv[0].vector;
		uint32_t xy1 = mv[1].vector;
		for (int y = 0; y < BLOCK / 4; ++y) {
			for (int x = 0; x < BLOCK / 4; ++x) {
				mv[x * 2].vector = xy0;
				mv[x * 2 + 1].vector = xy1;
			}
			mv += N / 2;
		}
	}
}

struct tempral_vector_zero {
	void operator()(const h264d_vector_t *mvcol, int scale, h264d_vector_t *tmv) const {
		tmv[0].vector = mvcol->vector;
		tmv[1].vector = 0U;
	}
};

struct tempral_vector_nonzero {
	static inline void temporal_vector(int mvcol, int scale, int16_t& mv0, int16_t& mv1) {
		int t = (mvcol * scale + 128) >> 8;
		mv0 = t;
		mv1 = t - mvcol;
	}
	void operator()(const h264d_vector_t *mvcol, int scale, h264d_vector_t *tmv) const {
		temporal_vector(mvcol->v[0], scale, tmv[0].v[0], tmv[1].v[0]);
		temporal_vector(mvcol->v[1], scale, tmv[0].v[1], tmv[1].v[1]);
	}
};

template <int N, int BLOCK, int X, int Y, typename F>
static inline void temporal_direct_block_base(h264d_mb_current *mb, const h264d_vector_t *mvcol, int8_t *ref_idx, h264d_vector_t *mv, int blk_idx, int scale,
						 F TemporalVector)
{
	int xoffset = (blk_idx & 1) * 8;
	int yoffset = (blk_idx & 2) * 4;
	static const h264d_vector_t size = {{X, Y}};
	for (int i = 0; i < 64 / (BLOCK * BLOCK); ++i) {
		h264d_vector_t *tmv = &mv[N == 8 ? i * 2 : ((i & 2) * 4 + (i & 1) * 2)];
		TemporalVector(mvcol, scale, tmv);
		direct_mv_pred(mb, ref_idx, tmv, size, xoffset + (i & 1) * 4, yoffset + (i & 2) * 2);
		mvcol += (i & 1) ? 3 : 1;
	}
}

template <int N, int BLOCK, int X, int Y>
static inline void temporal_direct_block(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, int8_t *ref_idx, h264d_vector_t *mv, int blk_idx)
{
	int map_idx = col_mb->ref[blk_idx];
	int ref = (0 <= map_idx) ? mb->bdirect->map_col_to_list0[map_idx] : 0;
	ref_idx[0] = ref;
	ref_idx[1] = 0;
	if ((0 <= map_idx) && (mb->frame->refs[0][ref].in_use != LONG_TERM)) {
		const h264d_vector_t *mvcol = &col_mb->mv[(blk_idx & 2) * ((BLOCK == 8) ? 6 : 4) + (blk_idx & 1) * ((BLOCK == 8) ? 3 : 2)];
		temporal_direct_block_base<N, BLOCK, X, Y>(mb, mvcol, ref_idx, mv, blk_idx, mb->bdirect->scale[ref], tempral_vector_nonzero());
	} else {
		temporal_direct_block_base<N, BLOCK, X, Y>(mb, zero_mov, ref_idx, mv, blk_idx, 0, tempral_vector_zero());
	}
}

static void pred_direct4x4_temporal(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk, int avail, prev8x8_t *ref_blk, int type0_cnt)
{
	const h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
	const h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
	pblk += blk_idx;
	if (col_mb->type == COL_MB8x8) {
		temporal_direct_block<8, 4, 4, 4>(mb, col_mb, pblk->ref, pblk->mv[0], blk_idx);
	} else {
		temporal_direct_block<8, 8, 8, 8>(mb, col_mb, pblk->ref, pblk->mv[0], blk_idx);
		memcpy(pblk->mv[1], pblk->mv[0], sizeof(pblk->mv[0]));
		memcpy(pblk->mv[2], pblk->mv[0], sizeof(pblk->mv[0]));
		memcpy(pblk->mv[3], pblk->mv[0], sizeof(pblk->mv[0]));
	}
}

static void pred_direct8x8_temporal(h264d_mb_current *mb, int blk_idx, prev8x8_t *pblk, int avail, prev8x8_t *ref_blk, int type0_cnt)
{
	const h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
	const h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
	pblk += blk_idx;
	temporal_direct_block<8, 8, 8, 8>(mb, col_mb, pblk->ref, pblk->mv[0], blk_idx);
	memcpy(pblk->mv[1], pblk->mv[0], sizeof(pblk->mv[0]));
	memcpy(pblk->mv[2], pblk->mv[0], sizeof(pblk->mv[0]));
	memcpy(pblk->mv[3], pblk->mv[0], sizeof(pblk->mv[0]));
}

static void temporal_direct16x16_block8x8_16x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	temporal_direct_block<16, 8, 16, 16>(mb, col_mb, &ref_idx[0], &mv[0].mv[0], 0);
	memset(&mv[1], 0, sizeof(mv[1]));
}

static void temporal_direct16x16_block8x8_16x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	for (int y = 0; y < 2; ++y) {
		temporal_direct_block<16, 8, 16, 8>(mb, col_mb, &ref_idx[y * 2], &mv[y].mv[0], y * 2);
	}
	memset(&mv[2], 0, sizeof(mv[2]) * 2);
}

static void temporal_direct16x16_block8x8_8x16(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	for (int x = 0; x < 2; ++x) {
		temporal_direct_block<16, 8, 8, 16>(mb, col_mb, &ref_idx[x * 2], &mv[x].mv[0], x);
	}
	memset(&mv[2], 0, sizeof(mv[2]) * 2);
}

template <int N>
void temporal_direct16x16_blockNxN_8x8(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	for (int blk_idx = 0; blk_idx < 4; ++blk_idx) {
		temporal_direct_block<16, N, N, N>(mb, col_mb, &ref_idx[blk_idx * 2], &mv[(blk_idx & 2) * 64 / (N * N) + (blk_idx & 1) * 8 / N].mv[0], blk_idx);
	}
//	memset(&mv[16 * 4 / N], 0, sizeof(mv[16 * 4 / N]) * 16 * 4 / N);
}

template <int DIRECT8x8INFERENCE>
static void b_skip_mb_temporal(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_set_t *mv)
{
	static void (* const temporal_direct16x16[4])(h264d_mb_current *mb, const h264d_col_mb_t *col_mb, int8_t *ref_idx, h264d_vector_set_t *mv) = {
		temporal_direct16x16_block8x8_16x16,
		temporal_direct16x16_block8x8_16x8,
		temporal_direct16x16_block8x8_8x16,
		temporal_direct16x16_blockNxN_8x8<(DIRECT8x8INFERENCE + 1) * 4>
	};
	const h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
	const h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
	temporal_direct16x16[col_mb->type](mb, col_mb, ref_idx, mv);
}

static int skip_mbs(h264d_mb_current *mb, uint32_t skip_mb_num, int slice_type)
{
	uint32_t max_mb_run = mb->max_x * mb->max_y - (mb->y * mb->max_x + mb->x);
	uint32_t left4x4, top4x4;
	int8_t *ref_idx;
	void (*skip_mb)(h264d_mb_current *mb, int8_t *ref_idx, h264d_vector_set_t *mv);
	int8_t ref_idx_b[2 * 4];
	static const int8_t ref_idx_p[2] = {
		0, -1
	};

	skip_mb_num = skip_mb_num < max_mb_run ? skip_mb_num : max_mb_run;
	mb->left4x4pred = 0x22222222;
	left4x4 = mb->left4x4coef;
	mb->left4x4coef = 0;
	mb->cbp = 0;
	mb->cbf = 0;
	if (slice_type == P_SLICE) {
		ref_idx = (int8_t *)ref_idx_p;
		skip_mb = p_skip_mb;
	} else {
		ref_idx = ref_idx_b;
		skip_mb = mb->bdirect->func->direct16x16;
	}
	do {
		h264d_vector_set_t mv[16];
		col_mbtype_t col_mb_type;

		skip_mb(mb, ref_idx, mv);
		*mb->top4x4pred = 0x22222222;
		top4x4 = *mb->top4x4coef;
		*mb->top4x4coef = 0;
		if (slice_type == B_SLICE) {
			const h264d_ref_frame_t *colpic = &(mb->frame->refs[1][0]);
			const h264d_col_mb_t *col_mb = &colpic->col->col_mb[mb->y * mb->max_x + mb->x];
			col_mb_type = col_mb->type;
		} else {
			col_mb_type = COL_MB16x16;
		}
		no_residual_inter(mb);
		mb->bdirect->func->store_info_inter(mb, mv, ref_idx, left4x4, top4x4, col_mb_type);
		left4x4 = 0;
		mb->prev_qp_delta = 0;
		mb->type = MB_PSKIP;
		mb->left4x4inter->type = MB_PSKIP;
		mb->left4x4inter->mb_skip = 1;
		mb->left4x4inter->direct8x8 = 3;
		mb->top4x4inter->type = MB_PSKIP;
		mb->top4x4inter->direct8x8 = 3;
		mb->top4x4inter->mb_skip = 1;
		if (increment_mb_pos(mb) < 0) {
			return -1;
		}
	} while (--skip_mb_num);
	return 0;
}

static int check_more_data(dec_bits *st)
{
	const uint8_t *mem = dec_bits_current(st);
	return (mem[1] != 0) || (mem[2] != 0) || (1 < mem[3]);
}

static int more_rbsp_data(dec_bits *st)
{
	int bits;

	bits = not_aligned_bits(st);
	if (bits == 0) {
		bits = 8;
	}
	if (show_bits(st, bits) == (1U << (bits - 1))) {
		return (1 < (show_bits(st, bits + 24) & 0xffffff)) ? 1 : check_more_data(st);
	} else {
		return 1;
	}
}

static int post_process(h264d_context *h2d, h264d_mb_current *mb);
static inline int cabac_decode_terminate(h264d_cabac_t *cb, dec_bits *st);
static int mb_skip_cabac(h264d_mb_current *mb, dec_bits *st, int slice_type);

static int slice_data(h264d_context *h2d, dec_bits *st)
{
	h264d_slice_header *hdr = h2d->slice_header;
	h264d_pps *pps = &h2d->pps_i[hdr->pic_parameter_set_id];
	h264d_mb_current *mb = &h2d->mb_current;
	int is_ae = pps->entropy_coding_mode_flag;
	if (is_ae) {
		int idc = (hdr->slice_type == I_SLICE) ? 0 : hdr->cabac_init_idc + 1;
		init_cabac_context(&mb->cabac->cabac, mb->cabac->context, mb->qp, ctx_idx_mn_IPB[idc], NUM_ARRAY(ctx_idx_mn_IPB[idc]));
		byte_align(st);
		init_cabac_engine(&mb->cabac->cabac, st);
	}
	do {
		uint32_t skip_num;
		if ((hdr->slice_type != I_SLICE)
		    && (hdr->slice_type != SI_SLICE)) {
			skip_num = is_ae ? mb_skip_cabac(mb, st, hdr->slice_type) : ue_golomb(st);
			if (skip_num) {
				if (skip_mbs(mb, skip_num, hdr->slice_type) < 0) {
					break;
				}
				if (is_ae) {
					continue;
				}
			}
			if (!is_ae && !more_rbsp_data(st)) {
				break;
			}
		}
		if (is_ae) {
			macroblock_layer_cabac(mb, hdr, st);
		} else {
			macroblock_layer(mb, hdr, st);
		}
		mb->left4x4inter->mb_skip = 0;
		mb->top4x4inter->mb_skip = 0;
		if (increment_mb_pos(mb) < 0) {
			break;
		}
	} while (is_ae ? !cabac_decode_terminate(mb->cabac, st) : more_rbsp_data(st));
	return post_process(h2d, mb);
}

#define AlphaBeta(a, b, q, alpha_offset, beta_offset) {\
	a = q + alpha_offset;\
	b = q + beta_offset;\
	a = (a <= 51 ? a : 51) - 16;\
	b = (b <= 51 ? b : 51) - 16;\
}

#define SMALLER_THAN(x, tbl) *(tbl + x)

template<int N>
static inline void strength4h(uint8_t *dst, int q0, int q1, int p0, int p1, const int8_t *alpha_r, const int8_t *beta) {
	if ((N == 1) && (SMALLER_THAN(q0 - p0, alpha_r))) {
		int q2 = dst[-N];
		if (SMALLER_THAN(q0 - q2, beta)) {
			int t = q0 + q1 + p0 + 2;
			dst[1 * N] = (t * 2 + p1 + q2) >> 3;
			dst[0] = (t + q2) >> 2;
			dst[-N] = (dst[-N * 2] * 2 + q2 * 3 + t + 2) >> 3;
		} else {
			dst[1 * N] = (q1 * 2 + q0 + p1 + 2) >> 2;
		}
		int p2 = dst[4 * N];
		if (SMALLER_THAN(p0 - p2, beta)) {
			int t = p0 + p1 + q0 + 2;
			dst[2 * N] = (t * 2 + q1 + p2) >> 3;
			dst[3 * N] = (t + p2) >> 2;
			dst[4 * N] = (dst[5 * N] * 2 + p2 * 3 + t + 2) >> 3;
		} else {
			dst[2 * N] = (p1 * 2 + p0 + q1 + 2) >> 2;
		}
	} else {
		int t = q1 + p1 + 2;
		dst[1 * N] = (q1 + q0 + t) >> 2;
		dst[2 * N] = (p1 + p0 + t) >> 2;
	}
}

#define CLIP3(x, tbl) ((tbl)[x])

template<int N>
static inline void strength1_3h(uint8_t *dst, int q0, int q1, int p0, int p1, const int8_t *tc0p, const int8_t *beta) {
	if (N == 1) {
		int q2 = dst[-N];
		int p2 = dst[4 * N];
		int aq_smaller = SMALLER_THAN(q2 - q0, beta);
		int ap_smaller = SMALLER_THAN(p2 - p0, beta);
		if (tc0p[1]) {
			if (aq_smaller || ap_smaller) {
				int t0 = (p0 + q0 + 1) >> 1;
				if (aq_smaller) {
					int t = (q2 + t0 - (q1 * 2)) >> 1;
					if (t) {
						dst[0] = CLIP3(t, tc0p) + q1;
					}
				}
				if (ap_smaller) {
					int t = (p2 + t0 - (p1 * 2)) >> 1;
					if (t) {
						dst[3 * N] = CLIP3(t, tc0p) + p1;
					}
					tc0p += 511;
				}
				tc0p = aq_smaller ? tc0p + 511 : tc0p;
			}
		} else {
			tc0p += 511 * (aq_smaller + ap_smaller);
			if (!tc0p[1]) {
				return;
			}
		}
	} else {
		tc0p += 511;
	}
	int delta = (((p0 - q0) * 4) + q1 - p1 + 4) >> 3;
	if (delta) {
		delta = CLIP3(delta, tc0p);
		q0 = q0 + delta;
		p0 = p0 - delta;
		dst[1 * N] = CLIP255C(q0);
		dst[2 * N] = CLIP255C(p0);
	}
}

template <int STR, int N>
static inline void deblock_horiz_base(int a, const int8_t *beta, uint8_t *dst, int str, int len, int stride)
{
	const int8_t *alpha = alpha_offset_base[a];
	const int8_t *tc0;
	dst = dst - 2 * N;
	if ((STR == 4) && (N == 1)) {
		tc0 = alpha_r_offset_base[a];
	} else if (STR == 4) {
		tc0 = 0;
	} else {
		tc0 = deblock_clip3[tc0_tbl[a][str]];
	}
	do {
		int q0 = dst[1 * N];
		int q1 = dst[0];
		if (SMALLER_THAN(q1 - q0, beta)) {
			int p0 = dst[2 * N];
			if (SMALLER_THAN(q0 - p0, alpha)) {
				int p1 = dst[3 * N];
				if (SMALLER_THAN(p0 - p1, beta)) {
					if (STR == 4) {
						strength4h<N>(dst, q0, q1, p0, p1, tc0, beta);
					} else {
						strength1_3h<N>(dst, q0, q1, p0, p1, tc0, beta);
					}
				}
			}
		}
		dst += stride;
	} while (--len);
}

template <int INC>
static inline void deblock_horiz_str4(int a, int b, uint8_t *dst, int stride) {
	deblock_horiz_base<4, 4 / INC>(a, beta_offset_base[b], dst, 0, INC * 4, stride);
}

template <int INC>
static inline void deblock_horiz_str1_3(int a, int b, uint8_t *dst, int str, int stride)
{
	str &= 255;
	const int8_t *beta = beta_offset_base[b];
	while (str) {
		int len = 0;
		int str1 = str & 3;
		do {
			str = (unsigned)str >> 2;
			len += INC;
		} while (str1 == (str & 3));
		if (str1) {
			deblock_horiz_base<1, 4 / INC>(a, beta, dst, str1 - 1, len, stride);
		}
		dst += stride * len;
	}
}

template <int N>
static inline void strength4v(uint8_t *dst, int q0, int q1, int p0, int p1, const int8_t *alpha_r, const int8_t *beta, int stride) {
	if ((N == 1) && (SMALLER_THAN(q0 - p0, alpha_r))) {
		int q2 = dst[-stride];
		if (SMALLER_THAN(q0 - q2, beta)) {
			int t = q0 + q1 + p0 + 2;
			dst[stride * 1] = (t * 2 + p1 + q2) >> 3;
			dst[0] = (t + q2) >> 2;
			dst[-stride] = (dst[-stride * 2] * 2 + q2 * 3 + t + 2) >> 3;
		} else {
			dst[stride * 1] = (q1 * 2 + q0 + p1 + 2) >> 2;
		}
		int p2 = dst[stride * 4];
		if (SMALLER_THAN(p0 - p2, beta)) {
			int t = p0 + p1 + q0 + 2;
			dst[stride * 2] = (t * 2 + q1 + p2) >> 3;
			dst[stride * 3] = (t + p2) >> 2;
			dst[stride * 4] = (dst[stride * 5] * 2 + p2 * 3 + t + 2) >> 3;
		} else {
			dst[stride * 2] = (p1 * 2 + p0 + q1 + 2) >> 2;
		}
	} else {
		int t = q1 + p1 + 2;
		dst[stride * 1] = (q1 + q0 + t) >> 2;
		dst[stride * 2] = (p1 + p0 + t) >> 2;
	}
}

template <int N>
static inline void strength1_3v(uint8_t *dst, int q0, int q1, int p0, int p1, const int8_t *tc0p, const int8_t *beta, int stride) {
	if (N == 1) {
		int q2 = dst[-stride];
		int p2 = dst[stride * 4];
		int aq_smaller = SMALLER_THAN(q2 - q0, beta);
		int ap_smaller = SMALLER_THAN(p2 - p0, beta);
		if (tc0p[1]) {
			if (aq_smaller || ap_smaller) {
				int t0 = (p0 + q0 + 1) >> 1;
				if (aq_smaller) {
					int t = (q2 + t0 - (q1 * 2)) >> 1;
					if (t) {
						dst[0] = CLIP3(t, tc0p) + q1;
					}
				}
				if (ap_smaller) {
					int t = (p2 + t0 - (p1 * 2)) >> 1;
					if (t) {
						dst[stride * 3] = CLIP3(t, tc0p) + p1;
					}
					tc0p += 511;
				}
				tc0p = aq_smaller ? tc0p + 511 : tc0p;
			}
		} else {
			tc0p += 511 * (aq_smaller + ap_smaller);
			if (!tc0p[1]) {
				return;
			}
		}
	} else {
		tc0p += 511;
	}
	int delta = (((p0 - q0) * 4) + q1 - p1 + 4) >> 3;
	if (delta) {
		delta = CLIP3(delta, tc0p);
		q0 = q0 + delta;
		p0 = p0 - delta;
		dst[stride * 1] = CLIP255C(q0);
		dst[stride * 2] = CLIP255C(p0);
	}
}

template <int STR, int GAP>
static inline void deblock_vert_base(int a, const int8_t *beta, uint8_t *dst, int str, int len, int stride)
{
	const int8_t *alpha = alpha_offset_base[a];
	const int8_t *tc0;
	dst = dst - stride * 2;
	if ((STR == 4) && (GAP == 1)) {
		tc0 = alpha_r_offset_base[a];
	} else if (STR == 4) {
		tc0 = 0;
	} else {
		tc0 = deblock_clip3[tc0_tbl[a][str]];
	}
	do {
		int q1 = dst[0];
		int q0 = dst[stride];
		if (SMALLER_THAN(q1 - q0, beta)) {
			int p0 = dst[stride * 2];
			if (SMALLER_THAN(q0 - p0, alpha)) {
				int p1 = dst[stride * 3];
				if (SMALLER_THAN(p0 - p1, beta)) {
					if (STR == 4) {
						strength4v<GAP>(dst, q0, q1, p0, p1, tc0, beta, stride);
					} else {
						strength1_3v<GAP>(dst, q0, q1, p0, p1, tc0, beta, stride);
					}
				}
			}
		}
		dst += GAP;
	} while (--len);
}

template <int INC>
static inline void deblock_vert_str4(int a, int b, uint8_t *dst, int stride) {
	deblock_vert_base<4, 4 / INC>(a, beta_offset_base[b], dst, 0, INC * 4, stride);
}

template <int INC>
static inline void deblock_vert_str1_3(int a, int b, uint8_t *dst, int str, int stride)
{
	str &= 255;
	const int8_t *beta = beta_offset_base[b];
	while (str) {
		int len = 0;
		int str1 = str & 3;
		do {
			str = (unsigned)str >> 2;
			len += INC;
		} while (str1 == (str & 3));
		if (str1) {
			deblock_vert_base<1, 4 / INC>(a, beta, dst, str1 - 1, len, stride);
		}
		dst += len * (4 / INC);
	}
}

static inline void deblock_luma_inner_horiz(int a, int b, uint8_t *luma, uint32_t str, int stride)
{
	for (int i = 0; i < 3; ++i) {
		str >>= 8;
		luma += 4;
		deblock_horiz_str1_3<4>(a, b, luma, str, stride);
	}
}

static inline void deblock_luma_inner_vert(int a, int b, uint8_t *luma, uint32_t str, int stride)
{
	for (int i = 0; i < 3; ++i) {
		str >>= 8;
		luma += stride * 4;
		deblock_vert_str1_3<4>(a, b, luma, str, stride);
	}
}

static inline void deblock_pb(h264d_mb_current *mb)
{
	int qp;
	int a, b;
	int alpha_offset, beta_offset;
	int max_x = mb->max_x;
	int max_y = mb->max_y;
	int stride = max_x * 16;
	uint8_t *luma = mb->frame->curr_luma;
	uint8_t *chroma = mb->frame->curr_chroma;
	deblock_info_t *curr = mb->deblock_base;
	int idc = 0;

	for (int y = 0; y < max_y; ++y) {
		for (int x = 0; x < max_x; ++x) {
			uint32_t str;
			if (curr->idc) {
				idc = curr->idc - 1;
				DEC_SLICEHDR(curr->slicehdr, alpha_offset, beta_offset);
			}
			if (idc == 1) {
				curr++;
				luma += 16;
				chroma += 16;
				continue;
			}
			str = curr->str_horiz;
			if ((x != 0) && (!idc || mb->firstline != max_x) && (str & 255)) {
				/* alpha, beta of MB left edge */
				qp = (curr->qpy + (curr - 1)->qpy + 1) >> 1;
				AlphaBeta(a, b, qp, alpha_offset, beta_offset);
				if (0 <= a) {
					if (curr->str4_horiz) {
						deblock_horiz_str4<4>(a, b, luma, stride);
					} else {
						deblock_horiz_str1_3<4>(a, b, luma, str, stride);
					}
				}
				for (int c = 0; c < 2; ++c) {
					qp = (curr->qpc[c] + (curr - 1)->qpc[c] + 1) >> 1;
					AlphaBeta(a, b, qp, alpha_offset, beta_offset);
					if (0 <= a) {
						if (curr->str4_horiz) {
							deblock_horiz_str4<2>(a, b, chroma + c, stride);
						} else {
							deblock_horiz_str1_3<2>(a, b, chroma + c, str, stride);
						}
					}
				}
			}
			if (str & ~255) {
				AlphaBeta(a, b, curr->qpy, alpha_offset, beta_offset);
				if (0 <= a) {
					deblock_luma_inner_horiz(a, b, luma, str, stride);
				}
				str >>= 16;
				if (str & 0xff) {
					if (curr->qpy != curr->qpc[0]) {
						AlphaBeta(a, b, curr->qpc[0], alpha_offset, beta_offset);
					}
					if (0 <= a) {
						deblock_horiz_str1_3<2>(a, b, chroma + 8, str, stride);
					}
					if (curr->qpc[0] != curr->qpc[1]) {
						AlphaBeta(a, b, curr->qpc[1], alpha_offset, beta_offset);
					}
					if (0 <= a) {
						deblock_horiz_str1_3<2>(a, b, chroma + 8 + 1, str, stride);
					}
				}
			}
			str = curr->str_vert;
			if ((y != 0) && (!idc || mb->firstline < 0) && (str & 255)) {
				/* top edge of MB */
				qp = (curr->qpy + (curr - max_x)->qpy + 1) >> 1;
				AlphaBeta(a, b, qp, alpha_offset, beta_offset);
				if (0 <= a) {
					if (curr->str4_vert) {
						deblock_vert_str4<4>(a, b, luma, stride);
					} else {
						deblock_vert_str1_3<4>(a, b, luma, str, stride);
					}
				}
				for (int c = 0; c < 2; ++c) {
					qp = (curr->qpc[c] + (curr - max_x)->qpc[c] + 1) >> 1;
					AlphaBeta(a, b, qp, alpha_offset, beta_offset);
					if (0 <= a) {
						if (curr->str4_vert) {
							deblock_vert_str4<2>(a, b, chroma + c, stride);
						} else {
							deblock_vert_str1_3<2>(a, b, chroma + c, str, stride);
						}
					}
				}
			}
			if (str & ~255) {
				AlphaBeta(a, b, curr->qpy, alpha_offset, beta_offset);
				if (0 <= a) {
					deblock_luma_inner_vert(a, b, luma, str, stride);
				}
				str >>= 16;
				if (str & 0xff) {
					if (curr->qpy != curr->qpc[0]) {
						AlphaBeta(a, b, curr->qpc[0], alpha_offset, beta_offset);
					}
					if (0 <= a) {
						deblock_vert_str1_3<2>(a, b, chroma + stride * 4, str, stride);
					}
					if (curr->qpc[0] != curr->qpc[1]) {
						AlphaBeta(a, b, curr->qpc[1], alpha_offset, beta_offset);
					}
					if (0 <= a) {
						deblock_vert_str1_3<2>(a, b, chroma + stride * 4 + 1, str, stride);
					}
				}
			}
			curr++;
			luma += 16;
			chroma += 16;
		}
		luma += stride * 15;
		chroma += stride * 7;
	}
}

static inline h264d_ref_frame_t *marking_sliding_window(h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	int min_frm_num = INT_MAX;
	int min_idx = 0;
	int empty_idx = -1;
	int num_long = 0;
	int num_short = 0;

	for (int i = 0; i < 16; ++i) {
		int in_use = refs[i].in_use;
		if (in_use == NOT_IN_USE) {
			if (empty_idx < 0) {
				empty_idx = i;
			}
		} else if (in_use == SHORT_TERM) {
			int num = refs[i].num;
			if (frame_num < num) {
				num -= max_frame_num;
			}
			if (num < min_frm_num) {
				min_frm_num = num;
				min_idx = i;
			}
			num_short++;
		} else {
			num_long++;
		}
	}
	if (num_short + num_long < num_ref_frames) {
		refs +=	(0 <= empty_idx) ? empty_idx : num_ref_frames - 1;
	} else {
		refs += min_idx;
	}
	refs->in_use = SHORT_TERM;
	refs->frame_idx = frame_ptr;
	refs->num = frame_num;
	refs->poc = poc;
	return refs;
}

static void mmco_discard(h264d_ref_frame_t *refs, int in_use, uint32_t target_num)
{
	int i = 16;
	do {
		if (refs->num == target_num && refs->in_use == in_use) {
			refs->in_use = NOT_IN_USE;
			break;
		}
		refs++;
	} while (--i);
}

static void mmco_op1(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	int num = frame_num - mmco->arg1 - 1;
	while (num < 0) {
		num += max_frame_num;
	}
	mmco_discard(refs, SHORT_TERM, num);
}

static void mmco_op2(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	mmco_discard(refs, LONG_TERM, mmco->arg1);
}

static void mmco_op3(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	uint32_t long_num = mmco->arg2;
	uint32_t target_num = frame_num - mmco->arg1 - 1;
	int i = 16;

	while ((int)target_num < 0) {
		target_num += max_frame_num;
	}
	do {
		int in_use = refs->in_use;
		if ((in_use == LONG_TERM) && (refs->num == long_num)) {
			refs->in_use = NOT_IN_USE;
		} else if ((in_use == SHORT_TERM) && (refs->num == target_num)) {
			refs->in_use = LONG_TERM;
			refs->num = long_num;
		}
		refs++;
	} while (--i);
}

static void mmco_op4(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	int i = 16;
	uint32_t max_long_term_idx_plus1 = mmco->arg1;
	do {
		if (refs->in_use == LONG_TERM && max_long_term_idx_plus1 <= refs->num) {
			refs->in_use = NOT_IN_USE;
		}
		refs++;
	} while (--i);
}

static void mmco_op5(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	int i = 16;
	do {
		refs->in_use = NOT_IN_USE;
		refs++;
	} while (--i);
}

static void mmco_op6(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	h264d_ref_frame_t *ref = marking_sliding_window(refs, frame_ptr, frame_num, max_frame_num, num_ref_frames, poc);
	ref->in_use = LONG_TERM;
	ref->num = mmco->arg1;
}

static void (* const mmco_ops[6])(const h264d_mmco *mmco, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc) = {
	mmco_op1, mmco_op2, mmco_op3,
	mmco_op4, mmco_op5, mmco_op6,
};

static inline int marking_mmco(h264d_marking_t *mrk, h264d_ref_frame_t *refs, int frame_ptr, int frame_num, int max_frame_num, int num_ref_frames, int poc)
{
	const h264d_mmco *mmco = mrk->mmco;
	int op5_detect = 0;
	int op6_detect = 0;
	int i = 16;
	do {
		int op = mmco->op;
		if (op == 0) {
			break;
		} else if (5 <= op) {
			if (op == 5) {
				op5_detect = 1;
			} else {
				op6_detect = 1;
			}
		}
		mmco_ops[op - 1](mmco, refs, frame_ptr, frame_num, max_frame_num, num_ref_frames, poc);
		mmco++;
	} while (--i);
	if (!op6_detect) {
		if (op5_detect) {
			frame_num = poc = 0;
		}
		marking_sliding_window(refs, frame_ptr, frame_num, max_frame_num, num_ref_frames, poc);
	}
	return op5_detect;
}

static inline void gap_mbs(h264d_slice_header *hdr, h264d_mb_current *mb, h264d_ref_frame_t *refs, int max_frame_num, int num_ref_frames)
{
	int frame_num = hdr->frame_num;
	int prev_frame_num = hdr->prev_frame_num;
	int gap = frame_num - prev_frame_num;
	while (gap < 0) {
		gap += max_frame_num;
	}
	if (0 < --gap) {
		int poc = hdr->poc;
		if (16 < gap) {
			gap = 16;
			prev_frame_num = frame_num - 17;
		}
		do {
			if (max_frame_num <= ++prev_frame_num) {
				prev_frame_num -= max_frame_num;
			}
			marking_sliding_window(refs, mb->frame->index, prev_frame_num, max_frame_num, num_ref_frames, poc);
		} while (--gap);
	}
}

static inline void post_ref_pic_marking(h264d_slice_header *hdr, int nal_unit_type, int max_frame_num, int num_ref_frames, h264d_mb_current *mb, int lx)
{
	h264d_ref_frame_t *refs = hdr->reorder[lx].ref_frames;
	h264d_marking_t *mrk = &hdr->marking;
	int frame_num = hdr->frame_num;
	int poc = hdr->poc;

	if (nal_unit_type == SLICE_IDR_NAL) {
		refs[0].in_use = mrk->long_term_reference_flag ? LONG_TERM : SHORT_TERM;
		refs[0].frame_idx = mb->frame->index;
		refs[0].num = frame_num;
		refs[0].poc = poc;
		for (int i = 1; i < 16; ++i) {
			refs[i].in_use = NOT_IN_USE;
		}
	} else {
		if (!hdr->marking.idr && !hdr->marking.mmco5) {
			gap_mbs(hdr, mb, refs, max_frame_num, num_ref_frames);
		}
		if (mrk->adaptive_ref_pic_marking_mode_flag) {
			if (marking_mmco(mrk, refs, mb->frame->index, frame_num, max_frame_num, num_ref_frames, poc)) {
				hdr->frame_num = 0;
			}
		} else {
			marking_sliding_window(refs, mb->frame->index, frame_num, max_frame_num, num_ref_frames, poc);
		}
	}
}

static inline void insert_dpb(h264d_dpb_t *dpb, int poc, int frame_idx, int is_idr)
{
	if (is_idr) {
		dpb_insert_idr(dpb, poc, frame_idx);
	} else {
		dpb_insert_non_idr(dpb, poc, frame_idx);
	}
}

struct frame_num_descent_p {
	static int unwrap(int s, int frame_num, int max_frame_num) {
		return (frame_num < s) ? s - max_frame_num : s;
	}
	bool operator()(const int& l, const int& r, int frame_num, int max_frame_num) const {
		return unwrap(l, frame_num, max_frame_num) > unwrap(r, frame_num, max_frame_num);
	}
};

struct poc_order_b_l0 {
	bool operator()(const int& l, const int& r, int curr_poc, int na) const {
		if (l < curr_poc) {
			return (curr_poc < r) || (l > r);
		} else {
			return (curr_poc < r) && (l < r);
		}
	}
};

struct poc_order_b_l1 {
	bool operator()(const int& l, const int& r, int curr_poc, int na) const {
		if (l > curr_poc) {
			return (curr_poc > r) || (l < r);
		} else {
			return (curr_poc > r) && (l > r);
		}
	}
};

struct get_frame_num {
	int operator()(const h264d_ref_frame_t&ref) const {
		return ref.num;
	}
};

struct get_poc {
	int operator()(const h264d_ref_frame_t&ref) const {
		return ref.poc;
	}
};

template <typename F0, typename F1>
static inline bool ref_list_order(const h264d_ref_frame_t& lhs, const h264d_ref_frame_t& rhs, int curr_num, int max_num,
				 F0 GetNum,
				 F1 LessShortTerm)
{
	int l_use = lhs.in_use;
	int r_use = rhs.in_use;
	if (l_use == SHORT_TERM) {
		if (r_use == SHORT_TERM) {
			return LessShortTerm(GetNum(lhs), GetNum(rhs), curr_num, max_num);
		} else {
			return true;
		}
	} else if (l_use == LONG_TERM) {
		if (r_use == SHORT_TERM) {
			return false;
		} else if (r_use == LONG_TERM) {
			return GetNum(lhs) < GetNum(rhs);
		} else {
			return true;
		}
	} else {
		return false;
	}
}

struct ref_list_less_p {
	ref_list_less_p(int curr_num, int max_num) : curr_num_(curr_num), max_num_(max_num) {}
	bool operator()(const h264d_ref_frame_t& lhs, const h264d_ref_frame_t& rhs) const {
		return ref_list_order(lhs, rhs, curr_num_, max_num_, get_frame_num(), frame_num_descent_p());
	}
private:
	int curr_num_;
	int max_num_;
};

struct ref_list_order_b_ref0 {
	ref_list_order_b_ref0(int curr_poc) : curr_poc_(curr_poc) {}
	bool operator()(const h264d_ref_frame_t& lhs, const h264d_ref_frame_t& rhs) const {
		return ref_list_order(lhs, rhs, curr_poc_, 0, get_poc(), poc_order_b_l0());
	}
private:
	int curr_poc_;
};

struct ref_list_order_b_ref1 {
	ref_list_order_b_ref1(int curr_poc) : curr_poc_(curr_poc) {}
	bool operator()(const h264d_ref_frame_t& lhs, const h264d_ref_frame_t& rhs) const {
		return ref_list_order(lhs, rhs, curr_poc_, 0, get_poc(), poc_order_b_l1());
	}
private:
	int curr_poc_;
};

static inline void ref_pic_init_p(h264d_slice_header *hdr, int max_frame_num, int num_ref_frames)
{
	h264d_ref_frame_t *ref = hdr->reorder[0].ref_frames;
	std::sort(ref, ref + num_ref_frames, ref_list_less_p(hdr->frame_num, max_frame_num));
}

static inline bool is_same_list(const h264d_ref_frame_t *a, const h264d_ref_frame_t *b, int num_elem)
{
	return !memcmp(a, b, sizeof(*a) * num_elem); /* FIXME */
}

static inline void ref_pic_init_b(h264d_slice_header *hdr, int num_ref_frames)
{
	h264d_ref_frame_t *ref0 = hdr->reorder[0].ref_frames;
	h264d_ref_frame_t *ref1 = hdr->reorder[1].ref_frames;

	std::sort(ref0, ref0 + num_ref_frames, ref_list_order_b_ref0(hdr->poc));
	std::sort(ref1, ref1 + num_ref_frames, ref_list_order_b_ref1(hdr->poc));
	if ((1 < num_ref_frames) && is_same_list(ref0, ref1, num_ref_frames)) {
		std::swap(ref1[0], ref1[1]);
	}
	for (int i = num_ref_frames; i < 16; ++i) {
		ref0[i].in_use = NOT_IN_USE;
		ref1[i].in_use = NOT_IN_USE;
	}
}

static inline void record_map_col_ref_frameidx(int8_t *map, const h264d_ref_frame_t *refs1, int num_ref_frames)
{
	for (int i = 0; i < num_ref_frames; ++i) {
		map[i] = (int8_t)refs1[i].frame_idx;
	}
	memset(&map[num_ref_frames], refs1[0].frame_idx, 16 - num_ref_frames);
}

static inline h264d_ref_frame_t *find_l1_curr_pic(h264d_ref_frame_t *refs, int poc)
{
	h264d_ref_frame_t *refs_found = 0;
	for (int i = 0; i < 16; ++i) {
		if (refs->in_use) {
			if (refs->poc == poc) {
				return refs;
			}
			if (!refs_found) {
				refs_found = refs;
			}
		}
		++refs;
	}
	return refs_found ? refs_found : refs - 16;
}

static int post_process(h264d_context *h2d, h264d_mb_current *mb)
{
	h264d_slice_header *hdr;
	int nal_id;
	int is_filled = (mb->y >= mb->max_y);

	hdr = h2d->slice_header;
	if (is_filled) {
		h264d_frame_info_t *frame;
		deblock_pb(mb);
		h264d_sps *sps = &h2d->sps_i[h2d->pps_i[hdr->pic_parameter_set_id].seq_parameter_set_id];
		int max_frame_num = 1 << sps->log2_max_frame_num;
		int num_ref_frames = sps->num_ref_frames;
		nal_id = h2d->id;
		frame = h2d->mb_current.frame;
		if (nal_id & 0x60) {
			post_ref_pic_marking(hdr, nal_id & 31, max_frame_num, num_ref_frames, mb, 0);
			post_ref_pic_marking(hdr, nal_id & 31, max_frame_num, num_ref_frames, mb, 1);
			record_map_col_ref_frameidx(mb->frame->curr_col->map_col_frameidx, mb->frame->refs[0], num_ref_frames);
			std::swap(mb->frame->curr_col, find_l1_curr_pic(mb->frame->refs[1], hdr->marking.mmco5 ? 0 : hdr->poc)->col);
			insert_dpb(&frame->dpb, hdr->poc, frame->index, hdr->marking.idr | hdr->marking.mmco5);
		} else {
			dpb_insert_non_idr(&frame->dpb, hdr->poc, frame->index);
		}
		hdr->prev_frame_num = hdr->frame_num;
		hdr->first_mb_in_slice = mb->max_x * mb->max_x;
	}
	return is_filled;
}

#define cabac_decode_decision_raw(cb, st, ctx) cabac_decode_decision_raw(&((cb)->cabac), (st), (ctx))
#define cabac_decode_bypass(cb, st) cabac_decode_bypass(&((cb)->cabac), (st))
#define cabac_decode_multibypass(cb, st, len) cabac_decode_multibypass(&((cb)->cabac), (st), (len))
#define cabac_decode_decision(cb, st, ctxIdx) cabac_decode_decision_raw((cb), (st), &((cb)->context[ctxIdx]))

static inline int cabac_decode_terminate(h264d_cabac_t *cb, dec_bits *st)
{
	int range = cb->cabac.range - 2;
	int offset = cb->cabac.offset;
	if (range <= offset) {
		cb->cabac.range = range;
		return 1;
	} else {
		if (range < 256) {
			cabac_renorm(&(cb->cabac), st, range, offset);
		} else {
			cb->cabac.range = range;
		}
		return 0;
	}
}

static int mb_type_cabac_I(h264d_mb_current *mb, dec_bits *st, int avail, int ctx_idx, int slice_type)
{
	h264d_cabac_t *cb = mb->cabac;
	int is_i_slice = (slice_type == I_SLICE);
	int mb_type;

	if (is_i_slice) {
		int add = ((avail & 2) && (mb->top4x4inter->type != MB_INxN)) + ((avail & 1) && (mb->left4x4inter->type != MB_INxN));
		if (!cabac_decode_decision(cb, st, ctx_idx + add)) {
			return MB_INxN;
		}
		ctx_idx = 5;
	} else if (!cabac_decode_decision(cb, st, ctx_idx)) {
		return MB_INxN;
	}
	if (cabac_decode_terminate(cb, st)) {
		return MB_IPCM;
	}
	mb_type = cabac_decode_decision(cb, st, ctx_idx + 1) * 12 + 1;
	if (cabac_decode_decision(cb, st, ctx_idx + 2)) {
		mb_type = mb_type + cabac_decode_decision(cb, st, ctx_idx + 2 + is_i_slice) * 4 + 4;
	}
	mb_type += cabac_decode_decision(cb, st, ctx_idx + 3 + is_i_slice) * 2;
	mb_type += cabac_decode_decision(cb, st, ctx_idx + 3 + is_i_slice * 2);
	return mb_type;
}

static int mb_type_cabac_P(h264d_mb_current *mb, dec_bits *st, int avail, int ctx_idx, int slice_type)
{
	h264d_cabac_t *cb = mb->cabac;
	if (cabac_decode_decision(cb, st, 14)) {
		return 5 + mb_type_cabac_I(mb, st, avail, 17, 0);
	}
	if (cabac_decode_decision(cb, st, 15)) {
		return cabac_decode_decision(cb, st, 17) ? 1 : 2;
	} else {
		return cabac_decode_decision(cb, st, 16) ? 3 : 0;
	}
}

static int mb_type_cabac_B(h264d_mb_current *mb, dec_bits *st, int avail, int ctx_idx, int slice_type)
{
	h264d_cabac_t *cb = mb->cabac;
	int8_t *ctx = &cb->context[27 + ((avail & 1) && (mb->left4x4inter->type != MB_BDIRECT16x16)) + ((avail & 2) && (mb->top4x4inter->type != MB_BDIRECT16x16))];
	int mode;

	if (!cabac_decode_decision_raw(cb, st, ctx)) {
		return 0;
	}
	ctx = &cb->context[27 + 3];
	if (!cabac_decode_decision_raw(cb, st, ctx)) {
		return 1 + cabac_decode_decision(cb, st, 27 + 5);
	}
	ctx++;
	mode = cabac_decode_decision_raw(cb, st, ctx) * 8;
	ctx++;
	mode += cabac_decode_decision_raw(cb, st, ctx) * 4;
	mode += cabac_decode_decision_raw(cb, st, ctx) * 2;
	mode += cabac_decode_decision_raw(cb, st, ctx);
	if (mode < 8) {
		return mode + 3;
	} else if (mode < 13) {
		return mode * 2 + cabac_decode_decision_raw(cb, st, ctx) - 4;
	} else if (mode == 13) {
		return 23 + mb_type_cabac_I(mb, st, avail, 32, 0);
	} else if (mode == 14) {
		return 11;
	} else {
		/* mode == 15 */
		return 22;
	}
}

static int mb_skip_cabac(h264d_mb_current *mb, dec_bits *st, int slice_type)
{
	int avail = get_availability(mb);
	int offset = (slice_type == P_SLICE) ? 11 : 24;
	if ((avail & 1) && (mb->left4x4inter->mb_skip == 0)) {
		offset += 1;
	}
	if ((avail & 2) && (mb->top4x4inter->mb_skip == 0)) {
		offset += 1;
	}
	return cabac_decode_decision(mb->cabac, st, offset);
}

struct transform_size_8x8_flag_cabac {
	int operator()(h264d_mb_current *mb, dec_bits_t *st, int avail) const {
		int offset = 399 + ((avail & 2) && mb->top4x4inter->transform8x8) + ((avail & 1) && mb->left4x4inter->transform8x8);
		return cabac_decode_decision(mb->cabac, st, offset);
	}
};

struct intra4x4pred_mode_cabac {
	int operator()(int a, int b, dec_bits *st, h264d_cabac_t *cb) const {
		int pred = MIN(a, b);
		int8_t *ctx = &cb->context[68];
		if (!cabac_decode_decision_raw(cb, st, ctx)) {
			int rem;
			ctx++;
			rem = cabac_decode_decision_raw(cb, st, ctx);
			rem += cabac_decode_decision_raw(cb, st, ctx) * 2;
			rem += cabac_decode_decision_raw(cb, st, ctx) * 4;
			pred = (rem < pred) ? rem : rem + 1;
		}
		return pred;
	}
};

struct intra_chroma_pred_mode_cabac {
	uint32_t operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		h264d_cabac_t *cb = mb->cabac;
		int ctx_idx = 64 + ((avail & 2) && (mb->top4x4inter->type < MB_IPCM) && mb->top4x4inter->chroma_pred_mode) + ((avail & 1) && (mb->left4x4inter->type < MB_IPCM) && mb->left4x4inter->chroma_pred_mode);
		int pred_mode = cabac_decode_decision(cb, st, ctx_idx);
		if (pred_mode) {
			while ((pred_mode < 3) && cabac_decode_decision(cb, st, 64 + 3)) {
				pred_mode++;
			}
		}
		mb->chroma_pred_mode = pred_mode;
		return pred_mode;
	}
};

struct cbp_cabac {
	uint32_t operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		int cbp;
		int inc;
		h264d_cabac_t *cb = mb->cabac;
		int cbp_a = (avail & 1) ? mb->left4x4inter->cbp : 0x0f;
		int cbp_b = (avail & 2) ? mb->top4x4inter->cbp : 0x0f;
		/* luma */
		inc = (!(cbp_a & 2)) + (!(cbp_b & 4)) * 2;
		cbp = cabac_decode_decision(cb, st, 73 + inc);
		inc = !(cbp & 1) + (!(cbp_b & 8)) * 2;
		cbp += cabac_decode_decision(cb, st, 73 + inc) * 2;
		inc = (!(cbp_a & 8)) + !(cbp & 1) * 2;
		cbp += cabac_decode_decision(cb, st, 73 + inc) * 4;
		inc = !(cbp & 4) + !(cbp & 2) * 2;
		cbp += cabac_decode_decision(cb, st, 73 + inc) * 8;
		/* chroma */
		cbp_a >>= 4;
		cbp_b >>= 4;
		inc = (cbp_a != 0) + (cbp_b != 0) * 2;
		if (cabac_decode_decision(cb, st, 77 + inc)) {
			inc = (cbp_a >> 1) + (cbp_b & 2);
			cbp = cbp + cabac_decode_decision(cb, st, 77 + 4 + inc) * 16 + 16;
		}
		return cbp;
	}
};

static inline uint32_t unary_cabac(h264d_cabac_t *cb, dec_bits *st, int limit)
{
	int x = 0;
	int idx = 62;
	do {
		if (cabac_decode_decision(cb, st, idx)) {
			x = x + 1;
			idx = 63;
		} else {
			break;
		}
	} while (--limit);
	return x;
}

struct qp_delta_cabac {
	int operator()(h264d_mb_current *mb, dec_bits *st, int avail) const {
		int ctx_idx = 60 + (mb->prev_qp_delta != 0);
		h264d_cabac_t *cb = mb->cabac;
		int qp_delta = cabac_decode_decision(cb, st, ctx_idx);
		if (qp_delta) {
			qp_delta = unary_cabac(cb, st, 52) + 1;
			qp_delta = (((qp_delta & 1) ? qp_delta : -qp_delta) + 1) >> 1;
		}
		mb->prev_qp_delta = qp_delta;
		return qp_delta;
	}
};

static int ctxidxinc_cbf0(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab;
	if (avail & 1) {
		ab = mb->left4x4inter->cbf & 1;
	} else {
		ab = (mb->type < MB_IPCM);
	}
	if (avail & 2) {
		ab += (mb->top4x4inter->cbf & 1) * 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

static int ctxidxinc_cbf1(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab = cbf & 1;
	if (avail & 2) {
		ab += mb->top4x4inter->cbf & 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

static int ctxidxinc_cbf2(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab;
	if (avail & 1) {
		ab = (mb->left4x4inter->cbf >> 1) & 1;
	} else {
		ab = (mb->type < MB_IPCM);
	}
	ab += (cbf * 2) & 2;
	return ab;
}

template <int N>
static int ctxidxinc_cbf_inner3(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	return ((cbf >> (N + 2)) & 1) | ((cbf >> N) & 2);
}

static int ctxidxinc_cbf4(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab = (cbf >> 1) & 1;
	if (avail & 2) {
		ab += (mb->top4x4inter->cbf >> 1) & 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

static int ctxidxinc_cbf5(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab = (cbf >> 4) & 1;
	if (avail & 2) {
		ab += (mb->top4x4inter->cbf >> 2) & 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

static int ctxidxinc_cbf6(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	return (cbf >> 3) & 3;
}

static int ctxidxinc_cbf8(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab;
	if (avail & 1) {
		ab = (mb->left4x4inter->cbf >> 2) & 1;
	} else {
		ab = (mb->type < MB_IPCM);
	}
	ab += (cbf >> 1) & 2;
	return ab;
}

static int ctxidxinc_cbf9(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	return ((cbf >> 8) & 1) | ((cbf >> 2) & 2);
}

static int ctxidxinc_cbf10(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab;
	if (avail & 1) {
		ab = (mb->left4x4inter->cbf >> 3) & 1;
	} else {
		ab = (mb->type < MB_IPCM);
	}
	ab += (cbf >> 7) & 2;
	return ab;
}

static int ctxidxinc_cbf12(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	return ((cbf >> 9) & 1) | ((cbf >> 5) & 2);
}

static int ctxidxinc_cbf13(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	return ((cbf >> 12) & 1) | ((cbf >> 6) & 2);
}

static int ctxidxinc_cbf14(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	return (cbf >> 11) & 3;
}

template <int N>
static int ctxidxinc_cbf_chroma_dc(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab;
	if (avail & 1) {
		ab = (mb->left4x4inter->cbf >> (4 + N)) & 1;
	} else {
		ab = (mb->type < MB_IPCM);
	}
	if (avail & 2) {
		ab += (mb->top4x4inter->cbf >> (3 + N)) & 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

template <int N>
static int ctxidxinc_cbf_chroma_ac0(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab;
	if (avail & 1) {
		ab = (mb->left4x4inter->cbf >> (6 + N * 2)) & 1;
	} else {
		ab = (mb->type < MB_IPCM);
	}
	if (avail & 2) {
		ab += (mb->top4x4inter->cbf >> (5 + N * 2)) & 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

template <int N>
static int ctxidxinc_cbf_chroma_ac1(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab = (cbf >> (18 + N * 4)) & 1;
	if (avail & 2) {
		ab += (mb->top4x4inter->cbf >> (6 + N * 2)) & 2;
	} else {
		ab += (mb->type < MB_IPCM) * 2;
	}
	return ab;
}

template <int N>
static int ctxidxinc_cbf_chroma_ac2(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int ab = (cbf >> (17 + N * 4)) & 2;
	if (avail & 1) {
		ab += (mb->left4x4inter->cbf >> (7 + N * 2)) & 1;
	} else {
		ab += (mb->type < MB_IPCM);
	}
	return ab;
}

static int ctxidxinc_cbf_intra16x16dc(h264d_mb_current *mb, uint32_t cbf, int avail)
{
	int inc;
	if (avail & 1) {
		inc = (mb->left4x4inter->cbf >> 10) & 1;
	} else {
		inc = 1;
	}
	if (avail & 2) {
		inc += (mb->top4x4inter->cbf >> 9) & 2;
	} else {
		inc += 2;
	}
	return inc;
}

static int (* const ctxidxinc_cbf[16 + 2 + 8 + 1])(h264d_mb_current *mb, uint32_t cbf, int avail) = {
	ctxidxinc_cbf0, ctxidxinc_cbf1,
	ctxidxinc_cbf2, ctxidxinc_cbf_inner3<0>,
	ctxidxinc_cbf4, ctxidxinc_cbf5,
	ctxidxinc_cbf6, ctxidxinc_cbf_inner3<4>,

	ctxidxinc_cbf8, ctxidxinc_cbf9,
	ctxidxinc_cbf10, ctxidxinc_cbf_inner3<8>,
	ctxidxinc_cbf12, ctxidxinc_cbf13,
	ctxidxinc_cbf14, ctxidxinc_cbf_inner3<12>,

	ctxidxinc_cbf_chroma_dc<0>, ctxidxinc_cbf_chroma_dc<1>,

	ctxidxinc_cbf_chroma_ac0<0>, ctxidxinc_cbf_chroma_ac1<0>,
	ctxidxinc_cbf_chroma_ac2<0>, ctxidxinc_cbf_inner3<18>,
	ctxidxinc_cbf_chroma_ac0<1>, ctxidxinc_cbf_chroma_ac1<1>,
	ctxidxinc_cbf_chroma_ac2<1>, ctxidxinc_cbf_inner3<22>,

	ctxidxinc_cbf_intra16x16dc
};

static inline int get_coeff_map_cabac(h264d_cabac_t *cb, dec_bits *st, int cat, int is_field, int *coeff_map)
{
	static const int8_t significant_coeff_flag_offset16[16][3] = {
		{0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3},
		{4, 4, 4}, {5, 5, 5}, {6, 6, 6}, {7, 7, 7},
		{8, 8, 8}, {9, 9, 9}, {10, 10, 10}, {11, 11, 11},
		{12, 12, 12}, {13, 13, 13}, {14, 14, 14}, {15, 15, 15},
	};
	static const int8_t significant_coeff_flag_offset64[63][3] = {
		{0, 0, 0}, {1, 1, 1}, {1, 2, 1}, {1, 3, 2},
		{1, 4, 2}, {1, 5, 3}, {1, 5, 3}, {1, 4, 4},
		{1, 4, 5}, {1, 3, 6}, {1, 3, 7}, {1, 4, 7},
		{1, 4, 7}, {1, 4, 8}, {1, 5, 4}, {1, 5, 5},
		{2, 4, 6}, {2, 4, 9}, {2, 4, 10}, {2, 4, 10},
		{2, 3, 8}, {2, 3, 11}, {2, 6, 12}, {2, 7, 11},
		{2, 7, 9}, {2, 7, 9}, {2, 8, 10}, {2, 9, 10},
		{2, 10, 8}, {2, 9, 11}, {2, 8, 12}, {2, 7, 11},
		{3, 7, 9}, {3, 6, 9}, {3, 11, 10}, {3, 12, 10},
		{3, 13, 8}, {3, 11, 11}, {3, 6, 12}, {3, 7, 11},
		{4, 8, 9}, {4, 9, 9}, {4, 14, 10}, {4, 10, 10},
		{4, 9, 8}, {4, 8, 13}, {4, 6, 13}, {4, 11, 9},
		{5, 12, 9}, {5, 13, 10}, {5, 11, 10}, {5, 6, 8},
		{6, 9, 13}, {6, 14, 13}, {6, 10, 9}, {6, 9, 9},
		{7, 11, 10}, {7, 12, 10}, {7, 13, 14}, {7, 11, 14},
		{8, 14, 14}, {8, 10, 14}, {8, 12, 14},
	};
	static const int16_t significant_coeff_flag_offset[2][6][2] = {
		{
			{105, 166}, {105 + 15, 166 + 15}, {105 + 29, 166 + 29},
			{105 + 44, 166 + 44}, {105 + 47, 166 + 47}, {402, 417}
		},
		{
			/* field */
			{277, 338}, {277 + 15, 338 + 15}, {277 + 29, 338 + 29},
			{277 + 44, 338 + 44}, {277 + 47, 338 + 47}, {436, 451}
		}
	};
	const int16_t *ofs_elem = significant_coeff_flag_offset[is_field][cat];
	int8_t *sigc_offset = &cb->context[ofs_elem[0]];
	int8_t *last_offset = &cb->context[ofs_elem[1]];
	const int8_t (*latter)[3] = (cat == 5) ? significant_coeff_flag_offset64 : significant_coeff_flag_offset16;
	int num_coeff = coeff_ofs[cat].num_coeff;
	int map_cnt = 0;
	int i;

	for (i = 0; i < num_coeff - 1; ++i) {
		if (cabac_decode_decision_raw(cb, st, sigc_offset + latter[i][1])) {
			coeff_map[map_cnt++] = i;
			if (cabac_decode_decision_raw(cb, st, last_offset + latter[i][0])) {
				return map_cnt;
			}
		}
	}
	if (i == num_coeff - 1) {
		coeff_map[map_cnt++] = i;
	}
	return map_cnt;
}

static inline int cabac_decode_bypass_coeff(h264d_cabac_t *cb, dec_bits *st)
{
	int len = 0;
	while (cabac_decode_bypass(cb, st)) {
		len++;
	}
	int v0 = (1 << len) - 1;
	if (len) {
		v0 += cabac_decode_multibypass(cb, st, len);
	}
	return v0;
}

static inline void get_coeff_from_map_cabac(h264d_cabac_t *cb, dec_bits *st, int cat, int *coeff_map, int map_cnt, int *coeff, const int16_t *qmat)
{
	static const int8_t coeff_abs_level_ctx[2][8] = {
		{1, 2, 3, 4, 0, 0, 0, 0},
		{5, 5, 5, 5, 6, 7, 8, 9}
	};
	static const int8_t coeff_abs_level_transition[2][8] = {
		{1, 2, 3, 3, 4, 5, 6, 7},
		{4, 4, 4, 4, 5, 6, 7, 7}
	};
	int coeff_offset = coeff_ofs[cat].coeff_offset;
	memset(coeff + coeff_offset, 0, sizeof(*coeff) * coeff_ofs[cat].num_coeff);
	int8_t *abs_offset = &cb->context[coeff_ofs[cat].cabac_coeff_abs_level_offset + 227];
	uint32_t dc_mask = coeff_ofs[cat].coeff_dc_mask;
	int node_ctx = 0;
	int mp = map_cnt;
	const int8_t *zigzag = inverse_zigzag[cat];
	do {
		int8_t *ctx = abs_offset + coeff_abs_level_ctx[0][node_ctx];
		int abs_level;
		int idx;
		if (!cabac_decode_decision_raw(cb, st, ctx)) {
			abs_level = 1;
			node_ctx = coeff_abs_level_transition[0][node_ctx];
		} else {
			abs_level = 2;
			ctx = abs_offset + coeff_abs_level_ctx[1][node_ctx];
			node_ctx = coeff_abs_level_transition[1][node_ctx];
			while (abs_level < 15 && cabac_decode_decision_raw(cb, st, ctx)) {
				abs_level++;
			}
			if (abs_level == 15) {
				abs_level += cabac_decode_bypass_coeff(cb, st);
			}
		}
		idx = zigzag[coeff_map[--mp] + coeff_offset];
		coeff[idx] = (cabac_decode_bypass(cb, st) ? -abs_level : abs_level) * qmat[idx & dc_mask];
	} while (mp);
}

struct residual_block_cabac {
	int operator()(h264d_mb_current *mb, int na, int nb, dec_bits *st, int *coeff, const int16_t *qmat, int avail, int pos4x4, int cat, uint32_t dc_mask) const {
		int coeff_map[8 * 8];
		int coded_block_flag;
		h264d_cabac_t *cb = mb->cabac;
		if (cat != 5) {
			int coded_flag_inc;
			coded_flag_inc = ctxidxinc_cbf[pos4x4](mb, mb->cbf, avail);
			coded_block_flag = cabac_decode_decision(cb, st, 85 + coded_flag_inc + cat * 4);
			if (!coded_block_flag) {
				return 0;
			}
		} else {
			coded_block_flag = 0xf;
		}
		mb->cbf |= coded_block_flag << pos4x4;
		int map_cnt = get_coeff_map_cabac(cb, st, cat, mb->is_field, coeff_map);
		get_coeff_from_map_cabac(cb, st, cat, coeff_map, map_cnt, coeff, qmat);
		return map_cnt <= 15 ? map_cnt : 15;
	}
};

static int mb_intra4x4_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intra4x4(mb, mbc, st, avail, intra4x4pred_mode_cabac(), intra_chroma_pred_mode_cabac(), cbp_cabac(), qp_delta_cabac(), residual_block_cabac());
} 

static int mb_intraNxN_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intraNxN(mb, mbc, st, avail, transform_size_8x8_flag_cabac(), intra4x4pred_mode_cabac(), intra_chroma_pred_mode_cabac(), cbp_cabac(), qp_delta_cabac(), residual_block_cabac());
} 

static int mb_intra16x16_dconly_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intra16x16_dconly(mb, mbc, st, avail, intra_chroma_pred_mode_cabac(), qp_delta_cabac(), residual_block_cabac());
} 

static int mb_intra16x16_acdc_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_intra16x16_acdc(mb, mbc, st, avail, intra_chroma_pred_mode_cabac(), qp_delta_cabac(), residual_block_cabac());
} 

struct sub_mb_type_p_cabac {
	int operator()(h264d_mb_current *mb, dec_bits *st, int8_t *sub_mb_type, prev8x8_t *curr_blk, int avail) const {
		h264d_cabac_t *cb = mb->cabac;
		int8_t *ctx = &cb->context[21];
		for (int i = 0; i < 4; ++i) {
			int t;
			if (cabac_decode_decision_raw(cb, st, ctx)) {
				t = 0;
			} else if (!cabac_decode_decision_raw(cb, st, ctx + 1)) {
				t = 1;
			} else if (cabac_decode_decision_raw(cb, st, ctx + 2)) {
				t = 2;
			} else {
				t = 3;
			}
			sub_mb_type[i] = t;
		}
		return 0;
	}
};

static inline int sub_mb_type_b_one_cabac(h264d_cabac_t *cb, dec_bits *st)
{
	int t;
	if (!cabac_decode_decision(cb, st, 36)) {
		return 0;
	} else if (!cabac_decode_decision(cb, st, 37)) {
		return 1 + cabac_decode_decision(cb, st, 39);
	} else if (cabac_decode_decision(cb, st, 38)) {
		if (cabac_decode_decision(cb, st, 39)) {
			return 11 + cabac_decode_decision(cb, st, 39);
		} else {
			t = 7;
		}
	} else {
		t = 3;
	}
	t += cabac_decode_decision(cb, st, 39) * 2;
	return t + cabac_decode_decision(cb, st, 39);
}

struct sub_mb_type_b_cabac {
	int operator()(h264d_mb_current *mb, dec_bits *st) const {
		return sub_mb_type_b_one_cabac(mb->cabac, st);
	}
};

struct sub_mb_types_b_cabac {
	int operator()(h264d_mb_current *mb, dec_bits *st, int8_t *sub_mb_type, prev8x8_t *curr_blk, int avail) const {
		return sub_mb_type_b_base(mb, st, sub_mb_type, curr_blk, avail, sub_mb_type_b_cabac());
	}
};

static inline int mvd_cabac(h264d_mb_current *mb, dec_bits *st, h264d_cabac_t *cb, int8_t *ctx, int mva, int mvb)
{
	int mvd;
	int sum;
	int inc;

	sum = ABS(mva) + ABS(mvb);
	if (sum < 3) {
		inc = 0;
	} else if (sum <= 32) {
		inc = 1;
	} else {
		inc = 2;
	}
	if (!cabac_decode_decision_raw(cb, st, ctx + inc)) {
		return 0;
	}
	mvd = 1;
	ctx += 3;
	while (cabac_decode_decision_raw(cb, st, ctx)) {
		ctx += (mvd < 4) ? 1 : 0;
		mvd += 1;
		if (9 <= mvd) {
			unsigned exp = 3;
			while (cabac_decode_bypass(cb, st) && (exp < sizeof(mvd) * 4)) {
				mvd += (1 << exp);
				exp += 1;
			}
			while(exp--) {
				mvd += cabac_decode_bypass(cb, st) << exp;
			}
			break;
		}
	}
	return cabac_decode_bypass(cb, st) ? -mvd : mvd;
}

struct mvd_xy_cabac {
	void operator()(h264d_mb_current *mb, dec_bits *st, int16_t mv[], const int16_t mva[], const int16_t mvb[]) const {
		h264d_cabac_t *cb = mb->cabac;
		mv[0] = mvd_cabac(mb, st, cb, &cb->context[40], mva[0], mvb[0]);
		mv[1] = mvd_cabac(mb, st, cb, &cb->context[47], mva[1], mvb[1]);
	}
};

static void sub_mb8x8_mv_cabac(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb8x8_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cabac());
}

static void sub_mb8x4_mv_cabac(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb8x4_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cabac());
}

static void sub_mb4x8_mv_cabac(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb4x8_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cabac());
}

static void sub_mb4x4_mv_cabac(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx)
{
	sub_mb4x4_mv(mb, st, avail, blk_idx, pblk, lx, mvd_xy_cabac());
}

static void (* const sub_mb_p_cabac[4])(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx) = {
	sub_mb8x8_mv_cabac,
	sub_mb8x4_mv_cabac,
	sub_mb4x8_mv_cabac,
	sub_mb4x4_mv_cabac
};

static void (* const sub_mb_b_cabac[13])(h264d_mb_current *mb, dec_bits *st, int avail, int blk_idx, prev8x8_t *pblk, int lx) = {
	sub_mb8x8_direct,
	sub_mb8x8_mv_cabac,
	sub_mb8x8_mv_cabac,
	sub_mb8x8_mv_cabac,
	sub_mb8x4_mv_cabac,
	sub_mb4x8_mv_cabac,
	sub_mb8x4_mv_cabac,
	sub_mb4x8_mv_cabac,
	sub_mb8x4_mv_cabac,
	sub_mb4x8_mv_cabac,
	sub_mb4x4_mv_cabac,
	sub_mb4x4_mv_cabac,
	sub_mb4x4_mv_cabac
};

struct sub_mbs_p_cabac {
	void operator()(h264d_mb_current *mb, dec_bits *st, int avail, int8_t sub_mb_type[], prev8x8_t curr_blk[], int lx) const {
		if (lx == 0) {
			for (int i = 0; i < 4; ++i) {
				sub_mb_p_cabac[sub_mb_type[i]](mb, st, avail, i, curr_blk, lx);
			}
		}
	}
};

struct sub_mbs_b_cabac {
	void operator()(h264d_mb_current *mb, dec_bits *st, int avail, int8_t sub_mb_type[], prev8x8_t curr_blk[], int lx) const {
		for (int i = 0; i < 4; ++i) {
			sub_mb_b_cabac[sub_mb_type[i]](mb, st, avail, i, curr_blk, lx);
		}
	}
};

static inline int ref_idx_cabac_sub(dec_bits *st, h264d_cabac_t *cb, int inc)
{
	int idx = 0;
	while (cabac_decode_decision(cb, st, 54 + inc)) {
		inc = ((unsigned)inc >> 2) + 4;
		idx += 1;
	}
	return idx;
}

struct ref_idx16x16_cabac {
	int operator()(h264d_mb_current *mb, dec_bits *st, int lx, int avail) const {
		h264d_cabac_t *cb = mb->cabac;
		if (*mb->num_ref_idx_lx_active_minus1[lx]) {
			int inc = ((avail & 1) && !(mb->left4x4inter->direct8x8 & 1) && (0 < mb->left4x4inter->ref[0][lx])) + ((avail & 2) && !(mb->top4x4inter->direct8x8 & 1) && (0 < mb->top4x4inter->ref[0][lx])) * 2;
			return ref_idx_cabac_sub(st, cb, inc);
		} else {
			return 0;
		}
	}
};

struct ref_idx16x8_cabac {
	void operator()(h264d_mb_current *mb, dec_bits *st, int8_t *ref_idx, uint32_t blk_map, int avail) const {
		h264d_cabac_t *cb = mb->cabac;
		int8_t * const *num = mb->num_ref_idx_lx_active_minus1;
		for (int lx = 0; lx < 2; ++lx) {
			int t = *(num[0]);
			ref_idx[0] = (blk_map & 1) ?
				(t ? ref_idx_cabac_sub(st, cb,
							((avail & 1) && !(mb->left4x4inter->direct8x8 & 1) && (0 < mb->left4x4inter->ref[0][lx]))
							+ ((avail & 2) && !(mb->top4x4inter->direct8x8 & 1) && (0 < mb->top4x4inter->ref[0][lx])) * 2) : 0)
				: -1;
			ref_idx[2] = (blk_map & 2) ?
				(t ? ref_idx_cabac_sub(st, cb,
							((avail & 1) && !(mb->left4x4inter->direct8x8 & 2) && (0 < mb->left4x4inter->ref[1][lx]))
							+ (0 < ref_idx[0]) * 2) : 0)
				: -1;
			blk_map >>= 2;
			ref_idx++;
			num++;
		}
	}
};

struct ref_idx8x16_cabac {
	void operator()(h264d_mb_current *mb, dec_bits *st, int8_t *ref_idx, uint32_t blk_map, int avail) const {
		h264d_cabac_t *cb = mb->cabac;
		int8_t * const *num = mb->num_ref_idx_lx_active_minus1;
		for (int lx = 0; lx < 2; ++lx) {
			int t = *(num[0]);
			ref_idx[0] = (blk_map & 1) ?
				(t ? ref_idx_cabac_sub(st, cb,
							((avail & 1) && !(mb->left4x4inter->direct8x8 & 1) && (0 < mb->left4x4inter->ref[0][lx]))
							+ ((avail & 2) && !(mb->top4x4inter->direct8x8 & 1) && (0 < mb->top4x4inter->ref[0][lx])) * 2) : 0)
				: -1;
			ref_idx[2] = (blk_map & 2) ?
				(t ? ref_idx_cabac_sub(st, cb,
							(0 < ref_idx[0])
							+ ((avail & 2) && !(mb->top4x4inter->direct8x8 & 2) && (0 < mb->top4x4inter->ref[1][lx])) * 2) : 0)
				: -1;
			blk_map >>= 2;
			ref_idx++;
			num++;
		}
	}
};

static inline int valid_block(const prev8x8_t *pblk, int blk_idx, int lx, int non_direct) {
	return (0 <= non_direct) && (0 < pblk[blk_idx].ref[lx]);
}

struct ref_idx8x8_cabac {
	void operator()(h264d_mb_current *mb, dec_bits *st, const int8_t *sub_mb_type, prev8x8_t *pblk, int avail, int lx) const {
		int t = (mb->type != MB_P8x8REF0) ? *(mb->num_ref_idx_lx_active_minus1[lx]) : 0;
		int dir = 1 << lx;
		h264d_cabac_t *cb = mb->cabac;
		const int8_t *sub_mb_ref_map = mb->sub_mb_ref_map;
		int sub_dir0, sub_dir1, sub_dir2, sub_dir3;

		sub_dir0 = sub_mb_ref_map[*sub_mb_type++];
		if ((0 <= sub_dir0) && (dir & sub_dir0)) {
			pblk[0].ref[lx] = t ? ref_idx_cabac_sub(st, cb, ((avail & 1) && !(mb->left4x4inter->direct8x8 & 1) && (0 < mb->left4x4inter->ref[0][lx])) + ((avail & 2) && !(mb->top4x4inter->direct8x8 & 1) && (0 < mb->top4x4inter->ref[0][lx])) * 2) : 0;
		}
		sub_dir1 = sub_mb_ref_map[*sub_mb_type++];
		if ((0 <= sub_dir1) && (dir & sub_dir1)) {
			pblk[1].ref[lx] = t ? ref_idx_cabac_sub(st, cb, valid_block(pblk, 0, lx, sub_dir0) + ((avail & 2) && !(mb->top4x4inter->direct8x8 & 2) && (0 < mb->top4x4inter->ref[1][lx])) * 2) : 0;
		}
		sub_dir2 = sub_mb_ref_map[*sub_mb_type++];
		if ((0 <= sub_dir2) && (dir & sub_dir2)) {
			pblk[2].ref[lx] = t ? ref_idx_cabac_sub(st, cb, ((avail & 1) && !(mb->left4x4inter->direct8x8 & 2) && (0 < mb->left4x4inter->ref[1][lx])) + valid_block(pblk, 0, lx, sub_dir0) * 2) : 0;
		}
		sub_dir3 = sub_mb_ref_map[*sub_mb_type];
		if ((0 <= sub_dir3) && (dir & sub_dir3)) {
			pblk[3].ref[lx] = t ? ref_idx_cabac_sub(st, cb, valid_block(pblk, 2, lx, sub_dir2) + valid_block(pblk, 1, lx, sub_dir1) * 2) : 0;
		}
	}
};

static int mb_inter16x16_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x16(mb, mbc, st, avail, ref_idx16x16_cabac(), mvd_xy_cabac(), cbp_cabac(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter16x8_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x8(mb, mbc, st, avail, ref_idx16x8_cabac(), mvd_xy_cabac(), cbp_cabac(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter8x16_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x16(mb, mbc, st, avail, ref_idx8x16_cabac(), mvd_xy_cabac(), cbp_cabac(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter8x8p_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_type_p_cabac(), ref_idx8x8_cabac(), sub_mbs_p_cabac(), sub_mbs_dec_p(), cbp_cabac(), residual_luma_inter(), not_need_transform_size_8x8(), transform_size_8x8_flag_dummy(), qp_delta_cabac(), store_direct8x8_info_p(), residual_block_cabac());
}

static int mb_inter8x8b_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_types_b_cabac(), ref_idx8x8_cabac(), sub_mbs_b_cabac(), sub_mbs_dec_b(), cbp_cabac(), residual_luma_inter(), not_need_transform_size_8x8(), transform_size_8x8_flag_dummy(), qp_delta_cabac(), store_direct8x8_info_b(), residual_block_cabac());
}

static int mb_bdirect16x16_cabac(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_bdirect16x16(mb, mbc, st, avail, cbp_cabac(), residual_luma_inter(), transform_size_8x8_flag_dummy(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter16x16_cabac8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x16(mb, mbc, st, avail, ref_idx16x16_cabac(), mvd_xy_cabac(), cbp_cabac(), residual_luma_interNxN(), transform_size_8x8_flag_cabac(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter16x8_cabac8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter16x8(mb, mbc, st, avail, ref_idx16x8_cabac(), mvd_xy_cabac(), cbp_cabac(), residual_luma_interNxN(), transform_size_8x8_flag_cabac(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter8x16_cabac8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x16(mb, mbc, st, avail, ref_idx8x16_cabac(), mvd_xy_cabac(), cbp_cabac(), residual_luma_interNxN(), transform_size_8x8_flag_cabac(), qp_delta_cabac(), residual_block_cabac());
}

static int mb_inter8x8p_cabac8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_type_p_cabac(), ref_idx8x8_cabac(), sub_mbs_p_cabac(), sub_mbs_dec_p(), cbp_cabac(), residual_luma_interNxN(), need_transform_size_8x8p(), transform_size_8x8_flag_cabac(), qp_delta_cabac(), store_direct8x8_info_p(), residual_block_cabac());
}

static int mb_inter8x8b_cabac8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_inter8x8(mb, mbc, st, avail, sub_mb_types_b_cabac(), ref_idx8x8_cabac(), sub_mbs_b_cabac(), sub_mbs_dec_b(), cbp_cabac(), residual_luma_interNxN(), need_transform_size_8x8b(), transform_size_8x8_flag_cabac(), qp_delta_cabac(), store_direct8x8_info_b(), residual_block_cabac());
}

static int mb_bdirect16x16_cabac8x8(h264d_mb_current *mb, const mb_code *mbc, dec_bits *st, int avail)
{
	return mb_bdirect16x16(mb, mbc, st, avail, cbp_cabac(), residual_luma_interNxN(), transform_size_8x8_flag_cabac(), qp_delta_cabac(), residual_block_cabac());
}

static const mb_code mb_decode_cabac[2][54] = {
	{
		{mb_intra4x4_cabac, 0, 0},
		{mb_intra16x16_dconly_cabac, mb_intra16xpred_vert<16>, 0},
		{mb_intra16x16_dconly_cabac, intraNxNpred_horiz<16>, 0},
		{mb_intra16x16_dconly_cabac, intraNxNpred_dc<16>, 0},
		{mb_intra16x16_dconly_cabac, mb_intra16x16pred_planer, 0},
		{mb_intra16x16_dconly_cabac, mb_intra16xpred_vert<16>, 0x10},
		{mb_intra16x16_dconly_cabac, intraNxNpred_horiz<16>, 0x10},
		{mb_intra16x16_dconly_cabac, intraNxNpred_dc<16>, 0x10},
		{mb_intra16x16_dconly_cabac, mb_intra16x16pred_planer, 0x10},
		{mb_intra16x16_dconly_cabac, mb_intra16xpred_vert<16>, 0x20},
		{mb_intra16x16_dconly_cabac, intraNxNpred_horiz<16>, 0x20},
		{mb_intra16x16_dconly_cabac, intraNxNpred_dc<16>, 0x20},
		{mb_intra16x16_dconly_cabac, mb_intra16x16pred_planer, 0x20},
		{mb_intra16x16_acdc_cabac, mb_intra16xpred_vert<16>, 0x0f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_horiz<16>, 0x0f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_dc<16>, 0x0f},
		{mb_intra16x16_acdc_cabac, mb_intra16x16pred_planer, 0x0f},
		{mb_intra16x16_acdc_cabac, mb_intra16xpred_vert<16>, 0x1f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_horiz<16>, 0x1f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_dc<16>, 0x1f},
		{mb_intra16x16_acdc_cabac, mb_intra16x16pred_planer, 0x1f},
		{mb_intra16x16_acdc_cabac, mb_intra16xpred_vert<16>, 0x2f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_horiz<16>, 0x2f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_dc<16>, 0x2f},
		{mb_intra16x16_acdc_cabac, mb_intra16x16pred_planer, 0x2f},
		{mb_intrapcm, 0, 0},
		{mb_inter16x16_cabac, 0, 1},
		{mb_inter16x8_cabac, 0, 3},
		{mb_inter8x16_cabac, 0, 3},
		{mb_inter8x8p_cabac, 0, 0xf},
		{mb_inter8x8p_cabac, 0, 0xf},
		{mb_bdirect16x16_cabac, 0, 0},
		{mb_inter16x16_cabac, 0, 1}, {mb_inter16x16_cabac, 0, 2}, {mb_inter16x16_cabac, 0, 3},
		{mb_inter16x8_cabac, 0, 0x3}, {mb_inter8x16_cabac, 0, 0x3},
		{mb_inter16x8_cabac, 0, 0xc}, {mb_inter8x16_cabac, 0, 0xc},
		{mb_inter16x8_cabac, 0, 0x9}, {mb_inter8x16_cabac, 0, 0x9},
		{mb_inter16x8_cabac, 0, 0x6}, {mb_inter8x16_cabac, 0, 0x6},
		{mb_inter16x8_cabac, 0, 0xb}, {mb_inter8x16_cabac, 0, 0xb},
		{mb_inter16x8_cabac, 0, 0xe}, {mb_inter8x16_cabac, 0, 0xe},
		{mb_inter16x8_cabac, 0, 0x7}, {mb_inter8x16_cabac, 0, 0x7},
		{mb_inter16x8_cabac, 0, 0xd}, {mb_inter8x16_cabac, 0, 0xd},
		{mb_inter16x8_cabac, 0, 0xf}, {mb_inter8x16_cabac, 0, 0xf},
		{mb_inter8x8b_cabac, 0, 0}
	},
	{
		{mb_intraNxN_cabac, 0, 0},
		{mb_intra16x16_dconly_cabac, mb_intra16xpred_vert<16>, 0},
		{mb_intra16x16_dconly_cabac, intraNxNpred_horiz<16>, 0},
		{mb_intra16x16_dconly_cabac, intraNxNpred_dc<16>, 0},
		{mb_intra16x16_dconly_cabac, mb_intra16x16pred_planer, 0},
		{mb_intra16x16_dconly_cabac, mb_intra16xpred_vert<16>, 0x10},
		{mb_intra16x16_dconly_cabac, intraNxNpred_horiz<16>, 0x10},
		{mb_intra16x16_dconly_cabac, intraNxNpred_dc<16>, 0x10},
		{mb_intra16x16_dconly_cabac, mb_intra16x16pred_planer, 0x10},
		{mb_intra16x16_dconly_cabac, mb_intra16xpred_vert<16>, 0x20},
		{mb_intra16x16_dconly_cabac, intraNxNpred_horiz<16>, 0x20},
		{mb_intra16x16_dconly_cabac, intraNxNpred_dc<16>, 0x20},
		{mb_intra16x16_dconly_cabac, mb_intra16x16pred_planer, 0x20},
		{mb_intra16x16_acdc_cabac, mb_intra16xpred_vert<16>, 0x0f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_horiz<16>, 0x0f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_dc<16>, 0x0f},
		{mb_intra16x16_acdc_cabac, mb_intra16x16pred_planer, 0x0f},
		{mb_intra16x16_acdc_cabac, mb_intra16xpred_vert<16>, 0x1f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_horiz<16>, 0x1f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_dc<16>, 0x1f},
		{mb_intra16x16_acdc_cabac, mb_intra16x16pred_planer, 0x1f},
		{mb_intra16x16_acdc_cabac, mb_intra16xpred_vert<16>, 0x2f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_horiz<16>, 0x2f},
		{mb_intra16x16_acdc_cabac, intraNxNpred_dc<16>, 0x2f},
		{mb_intra16x16_acdc_cabac, mb_intra16x16pred_planer, 0x2f},
		{mb_intrapcm, 0, 0},
		{mb_inter16x16_cabac8x8, 0, 1},
		{mb_inter16x8_cabac8x8, 0, 3},
		{mb_inter8x16_cabac8x8, 0, 3},
		{mb_inter8x8p_cabac8x8, 0, 0xf},
		{mb_inter8x8p_cabac8x8, 0, 0xf},
		{mb_bdirect16x16_cabac8x8, 0, 0},
		{mb_inter16x16_cabac8x8, 0, 1}, {mb_inter16x16_cabac8x8, 0, 2}, {mb_inter16x16_cabac8x8, 0, 3},
		{mb_inter16x8_cabac8x8, 0, 0x3}, {mb_inter8x16_cabac8x8, 0, 0x3},
		{mb_inter16x8_cabac8x8, 0, 0xc}, {mb_inter8x16_cabac8x8, 0, 0xc},
		{mb_inter16x8_cabac8x8, 0, 0x9}, {mb_inter8x16_cabac8x8, 0, 0x9},
		{mb_inter16x8_cabac8x8, 0, 0x6}, {mb_inter8x16_cabac8x8, 0, 0x6},
		{mb_inter16x8_cabac8x8, 0, 0xb}, {mb_inter8x16_cabac8x8, 0, 0xb},
		{mb_inter16x8_cabac8x8, 0, 0xe}, {mb_inter8x16_cabac8x8, 0, 0xe},
		{mb_inter16x8_cabac8x8, 0, 0x7}, {mb_inter8x16_cabac8x8, 0, 0x7},
		{mb_inter16x8_cabac8x8, 0, 0xd}, {mb_inter8x16_cabac8x8, 0, 0xd},
		{mb_inter16x8_cabac8x8, 0, 0xf}, {mb_inter8x16_cabac8x8, 0, 0xf},
		{mb_inter8x8b_cabac8x8, 0, 0}
	}
};

static void set_mb_decode(h264d_mb_current *mb, const h264d_pps *pps)
{
	mb->mb_decode = (pps->entropy_coding_mode_flag ? mb_decode_cabac : mb_decode)[pps->transform_8x8_mode_flag];
}

static inline int macroblock_layer_cabac(h264d_mb_current *mb, h264d_slice_header *hdr, dec_bits *st)
{
	static int (* const mb_type_cabac[3])(h264d_mb_current *mb, dec_bits *st, int avail, int ctx_idx, int slice_type) = {
		mb_type_cabac_P,
		mb_type_cabac_B,
		mb_type_cabac_I,
	};
	const mb_code *mbc;
	int mbtype;
	int avail;
	avail = get_availability(mb);
	int slice_type = hdr->slice_type;
	mb->type = mbtype = adjust_mb_type(mb_type_cabac[slice_type](mb, st, avail, 3, slice_type), slice_type);
	mbc = &mb->mb_decode[mbtype];
	mbc->mb_dec(mb, mbc, st, avail);
	if (mbtype == MB_IPCM) {
		init_cabac_engine(&mb->cabac->cabac, st);
	}
	return 0;
}

static const m2d_func_table_t h264d_func_ = {
	sizeof(h264d_context),
	(int (*)(void *, int, int (*)(void *, void *), void *))h264d_init,
	(dec_bits *(*)(void *))h264d_stream_pos,
	(int (*)(void *, m2d_info_t *))h264d_get_info,
	(int (*)(void *, int, m2d_frame_t *, uint8_t *, int))h264d_set_frames,
	(int (*)(void *))h264d_decode_picture,
	(int (*)(void *, m2d_frame_t *, int))h264d_peek_decoded_frame,
	(int (*)(void *, m2d_frame_t *, int))h264d_get_decoded_frame
};

const m2d_func_table_t * const h264d_func = &h264d_func_;
