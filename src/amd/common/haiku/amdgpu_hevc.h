/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_HEVC_H
#define AMDGPU_HEVC_H
#include <stdint.h>

// Metadata for the owned UVD HEVC path. No addresses or firmware commands.
// One session per open file, owned by the calling team, as with H.264.
struct amdgpu_hevc_config {
	uint32_t width, height;
	uint32_t profile; // 1 Main, 2 Main 10
	uint32_t bit_depth, max_references, log2_ctb_size;
	uint32_t reserved[2];
};

struct amdgpu_hevc_picture {
	// SPS flags follow scaling_list_enabled through strong_intra_smoothing
	// in bits 0..7. Separate colour planes and range extensions are unsupported.
	// PPS bits 0..19 match UVD's dependent_slice_segments through
	// slice_segment_header_extension_present, as defined in ac_uvd_dec.h.
	uint32_t sps_flags, pps_flags;
	uint8_t nal_type, current_slot, log2_max_poc_lsb_minus4;
	uint8_t max_dec_pic_buffering_minus1;
	uint8_t log2_min_cb_size_minus3, log2_diff_max_min_cb_size;
	uint8_t log2_min_tb_size_minus2, log2_diff_max_min_tb_size;
	uint8_t max_transform_hierarchy_depth_inter, max_transform_hierarchy_depth_intra;
	uint8_t pcm_bit_depth_luma_minus1, pcm_bit_depth_chroma_minus1;
	uint8_t log2_min_pcm_cb_size_minus3, log2_diff_max_min_pcm_cb_size;
	uint8_t num_extra_slice_header_bits, num_short_term_ref_pic_sets;
	uint8_t num_long_term_ref_pics_sps, ref_l0_minus1, ref_l1_minus1;
	int8_t cb_qp_offset, cr_qp_offset, beta_offset_div2, tc_offset_div2;
	uint8_t diff_cu_qp_delta_depth, tile_columns_minus1, tile_rows_minus1;
	uint8_t log2_parallel_merge_level_minus2;
	int8_t initial_qp_minus26;
	uint8_t num_delta_pocs_ref_rps_idx, reserved[3];
	// Explicit tile dimensions in CTBs, excluding the final column/row.
	uint16_t column_width_minus1[19], row_height_minus1[21];
	int32_t current_poc, reference_poc[16];
	// 0x7f denotes an absent reference slot; used slots are in 0..15.
	uint8_t reference_slot[16];
	// Indices into the reference arrays, or 0xff for an unused entry.
	uint8_t st_before[8], st_after[8], lt_current[8];
	uint8_t scaling4x4[6][16], scaling8x8[6][64];
	uint8_t scaling16x16[6][64], scaling32x32[2][64];
	uint8_t dc16x16[6], dc32x32[2];
};

enum { AMDGPU_VIDEO_NV12 = 0x4e563132, AMDGPU_VIDEO_P010 = 0x50303130 };
struct amdgpu_hevc_create {
	uint32_t version, size;
	amdgpu_hevc_config config;
	uint64_t handle;
	uint32_t pitch, output_height, output_bytes, pixel_format;
	uint64_t allocated_bytes;
};
struct amdgpu_hevc_decode {
	uint32_t version, size;
	uint64_t handle, bitstream, output;
	uint32_t bitstream_bytes, output_capacity;
	amdgpu_hevc_picture picture;
	uint32_t sequence, fence, rptr, wptr;
	uint32_t guard_mismatches, vm_fault_status, vm_fault_address, feedback[8];
};
// Both codecs use amdgpu_video_destroy and AMDGPU_VIDEO_DESTROY.
#endif
