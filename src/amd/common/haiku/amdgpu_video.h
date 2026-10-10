/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_VIDEO_H
#define AMDGPU_VIDEO_H
#include <stdint.h>

// Native, synchronous UVD H.264 interface. No firmware messages, GPU addresses
// or commands cross this boundary. One session per open file, owned by its team.
// Progressive 8-bit 4:2:0 only; coded dimensions are multiples of 16. Cropping,
// timestamps, picture ordering and reference marking belong to the media add-on.
struct amdgpu_h264_config {
	uint32_t width, height, profile, level, max_references, reserved;
};

enum { AMDGPU_H264_IDR = 1 };
#define AMDGPU_VIDEO_MAX_BITSTREAM (4 * 1024 * 1024)

struct amdgpu_h264_picture {
	uint32_t flags;
	// SPS: direct_8x8 (0), frame_mbs_only (2, required), delta_poc_zero (3), gaps (5).
	// PPS: transform8x8 (0), redundant (1), constrained (2), deblock (3),
	// weighted_bipred (4..5), weighted_pred (6), bottom_poc (7), CABAC (8).
	uint32_t sps_flags, pps_flags, frame_num;
	uint8_t log2_frame_num_minus4, poc_type, log2_poc_lsb_minus4, reserved0;
	uint8_t ref_l0_minus1, ref_l1_minus1;
	int8_t initial_qp_minus26, chroma_qp_offset, second_chroma_qp_offset;
	uint8_t reserved1[3];
	uint8_t scaling4x4[6][16], scaling8x8[2][64];
	uint32_t reference_frame_num[16];
	int32_t field_order_count[2], reference_field_order_count[16][2];
};

struct amdgpu_video_create {
	uint32_t version, size;
	amdgpu_h264_config config;
	uint64_t handle; // output, also required by DECODE/DESTROY
	uint32_t pitch, output_bytes; // linear NV12, includes row padding
	uint64_t allocated_bytes;
};

struct amdgpu_video_decode {
	uint32_t version, size;
	uint64_t handle;
	uint64_t bitstream, output; // userspace pointers, copied synchronously
	uint32_t bitstream_bytes, output_capacity;
	amdgpu_h264_picture picture;
	uint32_t sequence, fence, rptr, wptr; // completed engine work, not a CPU success flag
	uint32_t guard_mismatches, vm_fault_status, vm_fault_address, feedback[8];
};

struct amdgpu_video_destroy {
	uint32_t version, size;
	uint64_t handle;
};
#endif
