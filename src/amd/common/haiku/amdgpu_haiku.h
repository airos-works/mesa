/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDGPU_HAIKU_H
#define AMDGPU_HAIKU_H

#include <Drivers.h>
#include <amdgpu_video.h>
#include <amdgpu_hevc.h>

// Native bring-up interface. This is not the Linux DRM ABI. Keep it separate
// from the eventual Mesa command-submission interface.
#define AMDGPU_HAIKU_ABI_VERSION 1
#define AMDGPU_DEVICE_NAME "dri/amdgpu/0"

enum {
	AMDGPU_GET_INFO = B_DEVICE_OP_CODES_END + 1,
	AMDGPU_READ_ROM,
	AMDGPU_SDMA_TEST,
	AMDGPU_SMC_BOOTSTRAP,
	AMDGPU_START_DMA,
	AMDGPU_CREATE_BUFFER,
	AMDGPU_MAP_BUFFER,
	AMDGPU_FREE_BUFFER,
	AMDGPU_SUBMIT_DMA,
	AMDGPU_WAIT_FENCE,
	AMDGPU_MEMORY_INFO,
	AMDGPU_CREATE_SYSTEM_BUFFER,
	AMDGPU_GART_INFO,
	AMDGPU_GFX_TEST,
	AMDGPU_UVD_TEST,
	AMDGPU_VIDEO_CREATE,
	AMDGPU_VIDEO_DECODE,
	AMDGPU_VIDEO_DESTROY,
	AMDGPU_HEVC_CREATE,
	AMDGPU_HEVC_DECODE,
	AMDGPU_CREATE_DEVICE_BUFFER,
	AMDGPU_DISPLAY_SNAPSHOT,
	AMDGPU_GFX_MEC_TEST,
	AMDGPU_VM_INFO,
	AMDGPU_VM_MAP,
	AMDGPU_VM_UNMAP,
	AMDGPU_VM_TEST,
	AMDGPU_GFX_SUBMIT,
	AMDGPU_IRQ_INFO,
	AMDGPU_RENDER_INFO,
	AMDGPU_GFX_SUBMIT_COPY
};

// One GPU virtual address space per open client. Mappings retain their BO
// until explicitly unmapped or the client closes; only that client's handles
// may be mapped. UNMAP requires the exact address/size of a prior MAP and
// zero handle, buffer_offset and permissions. No physical address is exposed.
enum {
	AMDGPU_VM_READ = 1,
	AMDGPU_VM_WRITE = 2,
	AMDGPU_VM_EXECUTE = 4
};
struct amdgpu_vm_info {
	uint32 version, size;
	uint64 address_start, address_end; // exclusive upper bound
	uint64 mapped_bytes;
	uint32 mapping_count, page_size;
	uint32 reserved[2];
};
struct amdgpu_vm_mapping {
	uint32 version, size;
	uint64 handle, buffer_offset, address, bytes;
	uint32 permissions, reserved;
};
// Bounded GPU execution for validating these mappings: a kernel-owned shader
// writes 1024 DWORDs at address, value XOR (index * 0x10204081). This is not
// the general command-submission interface; no user program or PM4 is accepted.
struct amdgpu_vm_test {
	uint32 version, size;
	uint64 address;
	uint32 value, reserved;
	uint64 completion;
	uint32 vm_fault_status[2], rptr, wptr;
	int32 status;
	uint32 reserved_out;
};

// Root-only user-command bring-up. The IB must lie entirely within an owned
// READ|EXECUTE mapping. The caller supplies complete PM4 state and shaders;
// command validation and recovery are not yet qualified for untrusted users.
// One synchronous IB, 256-byte address alignment, 256-DWORD length alignment,
// at most 64 KiB. flags/reserved must be zero. All mappings stay retained until
// retirement and VMID detach. completion is an observation, not a wait handle.
struct amdgpu_gfx_submit {
	uint32 version, size;
	uint64 address;
	uint32 dwords, flags;
	uint64 completion, elapsed_us;
	uint32 vm_fault_status[2], rptr, wptr;
	int32 status;
	uint32 reserved;
};

// GFX_SUBMIT_COPY uses the same request, but address is a userspace pointer
// instead of a GPU VA. The kernel copies exactly dwords * 4 bytes once into
// this client's private READ|EXECUTE-only storage before execution. CPU
// alignment is unrestricted; length limits and zero flags/reserved are the
// same as GFX_SUBMIT. The unused tail is zeroed on every copy, including a
// failed user copy. This snapshots only the outer IB, not nested IBs/shaders;
// raw command privilege/recovery qualification still requires root.
// VM_INFO/RENDER_INFO report the usable VA interval; lower addresses are
// reserved for the client's private commands and the public diagnostic shader.
#define AMDGPU_COPY_IB_ADDRESS 0x10000ULL

// Read-only observation after client engine startup; this does not initialize
// graphics or alter interrupt routing. reserved must be zero. No DMA address
// or writable ring mapping is exposed. Counts cover this device's lifetime.
struct amdgpu_irq_info {
	uint32 version, size;
	uint32 enabled, msi, vector, ring_bytes;
	uint64 interrupts, vectors, eop_events, completed_fences, vm_faults, privileged_faults;
	uint64 unknown, overflows, waits;
	uint32 rptr, wptr, last[4];
	uint32 first_eop[2][4];
	int32 status;
	uint32 reserved;
};

// Starts the installed graphics engine, then returns a bounded snapshot for
// the native Mesa backend. flags/reserved must be zero. No caller-selected
// register access is accepted. Bank/SMC selectors are restored after reads;
// timestamp capture is serialized with submissions. Clock defaults are ROM
// boot defaults, not measured clocks or DPM maxima. All sizes are bytes.
enum {
	AMDGPU_RENDER_ROOT_SUBMIT = 1,
	AMDGPU_RENDER_SYNC_SUBMIT = 2,
	AMDGPU_RENDER_COPY_SUBMIT = 4
};
struct amdgpu_render_info {
	uint32 version, size, flags, reserved;
	uint32 capabilities, gfx_major, gfx_minor, chip_revision;
	uint32 shader_engines, shader_arrays_per_engine, cu_per_array, backends_per_engine;
	uint32 tile_pipes, tcc_blocks, active_cus, enabled_backends;
	uint32 cu_mask[4][2], cu_disable[4][2], cu_user_disable[4][2];
	uint32 rb_disable[4][2], rb_user_disable[4][2];
	uint32 raster_config[4][2], raster_config_1[4][2];
	uint32 gb_addr_config, tile_mode[32], macrotile_mode[16];
	uint32 mc_arb_ramcfg, mc_shared_chmap, mc_seq_misc0;
	uint32 vram_bus_width, vram_type; // MC_SEQ_MISC0 MT encoding, not DRM's enum
	uint32 default_engine_khz, default_memory_khz, reference_khz, timestamp_khz;
	uint32 firmware_version[5], firmware_feature[5]; // CE, PFP, ME, RLC, MEC
	uint32 grbm_index_before, grbm_index_after;
	uint64 gpu_timestamp, started_us, finished_us;
	uint64 total_vram, visible_vram, total_gart, address_start, address_end;
	uint64 max_buffer_bytes, max_mapping_bytes, max_mapped_bytes;
	uint32 max_mappings, max_vm_clients, page_size, max_ib_bytes;
	uint32 ib_address_alignment, ib_size_alignment, max_buffers, reserved_out;
};

// Read-only DCE 11.2 observation, available without starting any GPU engine.
// Register reads are sequential, not an atomic modeset or vblank snapshot.
// No MMIO address, display ownership or modesetting capability is exposed.
enum {
	AMDGPU_DCE_CRTC_CONTROL, AMDGPU_DCE_CRTC_STATUS,
	AMDGPU_DCE_POSITION, AMDGPU_DCE_FRAME_COUNT,
	AMDGPU_DCE_H_TOTAL, AMDGPU_DCE_H_BLANK, AMDGPU_DCE_H_SYNC,
	AMDGPU_DCE_V_TOTAL, AMDGPU_DCE_V_BLANK, AMDGPU_DCE_V_SYNC,
	AMDGPU_DCE_GRPH_ENABLE, AMDGPU_DCE_GRPH_CONTROL, AMDGPU_DCE_PITCH,
	AMDGPU_DCE_PRIMARY_LOW, AMDGPU_DCE_PRIMARY_HIGH,
	AMDGPU_DCE_SECONDARY_LOW, AMDGPU_DCE_SECONDARY_HIGH,
	AMDGPU_DCE_VIEWPORT_START, AMDGPU_DCE_VIEWPORT_SIZE,
	AMDGPU_DCE_CURSOR_CONTROL, AMDGPU_DCE_CURSOR_LOW,
	AMDGPU_DCE_CURSOR_HIGH, AMDGPU_DCE_CURSOR_SIZE,
	AMDGPU_DCE_CURSOR_POSITION, AMDGPU_DCE_CURSOR_HOTSPOT,
	AMDGPU_DCE_REGISTER_COUNT
};
#define AMDGPU_DCE_HEAD_COUNT 6
struct amdgpu_display_head {
	uint32 register_base; // dword index for identifying this head
	uint32 registers[AMDGPU_DCE_REGISTER_COUNT];
};
struct amdgpu_display_snapshot {
	uint32 version, size;
	uint32 head_count, reserved; // reserved must be zero
	int64 started_us, finished_us; // kernel monotonic time around the reads
	amdgpu_display_head heads[AMDGPU_DCE_HEAD_COUNT];
	uint32 hpd_status[AMDGPU_DCE_HEAD_COUNT]; // HPD0..5; no connector mapping implied
};

#define AMDGPU_ROM_SIZE (256 * 1024)
struct amdgpu_rom {
	uint32 version;
	uint32 size;
	uint64 data; // userspace destination, AMDGPU_ROM_SIZE bytes
	uint32 capacity;
	uint32 reserved;
};

// Root-only bring-up operation. The kernel builds all packets and addresses;
// userspace supplies only the firmware image, never a GPU program or address.
struct amdgpu_sdma_test_result {
	uint32 version;
	uint32 size;
	uint64 firmware;
	uint32 firmware_size;
	uint32 reserved;
	int32 status;
	uint32 stage; // 1: validated, 2: VRAM mapped, 3: firmware, 4: submitted, 5: done
	uint64 scratch_gpu;
	uint32 firmware_version;
	uint32 checked_bytes;
	uint32 mismatches;
	uint32 fence;
	uint32 rptr;
	uint32 wptr;
	uint32 engine_status;
	uint32 halted;
	uint64 elapsed_us;
};

// Protected-mode SMU startup for the measured Polaris 10 hard-key board.
// This starts signed controller firmware; it does not enable DPM or engines.
struct amdgpu_smc_bootstrap {
	uint32 version;
	uint32 size;
	uint64 firmware;
	uint32 firmware_size;
	uint32 reserved;
	int32 status;
	uint32 stage;
	uint32 firmware_version;
	uint32 clock;
	uint32 pc;
	uint32 security;
	uint32 smu_status;
	uint32 response;
	uint32 soft_registers;
	uint32 reserved_out;
};

// Root-only GFX8 command-processor bring-up. CE, PFP, ME, RLC images.
// Every PM4 packet, shader and destination is kernel-owned; no user programs.
struct amdgpu_gfx_test {
	uint32 version, size;
	uint64 firmware[4];
	uint32 firmware_size[4];
	int32 status;
	uint32 stage, sequence, checked_bytes, mismatches;
	uint32 cp_control, ring_control, rptr, wptr, grbm_status, rlc_status;
	uint32 vm_fault_status, vm_fault_address, vm_fault_client;
};

// Optional root-only diagnostic: also authenticate/start MEC firmware with
// every compute queue inactive. No userspace queue or shader is accepted.
// gfx.size describes this complete structure for AMDGPU_GFX_MEC_TEST.
struct amdgpu_gfx_mec_test {
	amdgpu_gfx_test gfx;
	uint64 mec_firmware;
	uint32 mec_firmware_size;
	uint32 reserved;
};

// Root-only UVD6 bring-up with one fixed H.264 picture. All messages,
// bitstream data and GPU addresses are kernel-owned. Output is a copy only.
#define AMDGPU_UVD_TEST_OUTPUT_BYTES 737280
struct amdgpu_uvd_test {
	uint32 version, size;
	uint64 firmware;
	uint32 firmware_size, output_capacity;
	uint64 output;
	int32 status;
	uint32 stage, sequence, firmware_version;
	uint32 uvd_status, power_status, ring_control, rptr, wptr, context, fence;
	uint32 vm_fault_status, vm_fault_address;
	uint32 checked_bytes, guard_mismatches, checksum;
	uint32 feedback[8];
};

struct amdgpu_dma_init {
	uint32 version;
	uint32 size;
	uint64 firmware;
	uint32 firmware_size;
	uint32 reserved;
};

// CREATE_BUFFER uses CPU-visible VRAM. CREATE_SYSTEM_BUFFER uses wired,
// snooped system RAM; both use the same handle, mapping and command API.
// CREATE_DEVICE_BUFFER uses VRAM beyond the CPU aperture, is zeroed by DMA
// before publication, and rejects MAP. Transfer through a mapped RAM buffer.
// File-local handles; no raw MMIO or arbitrary GPU addresses are accepted.
// MAP creates a non-executable mapping in the calling team. FREE or closing
// the file revokes every mapping (including clones) before memory can be reused.
struct amdgpu_buffer {
	uint32 version;
	uint32 size;
	uint64 handle;
	uint64 bytes;
	uint64 address;
	int32 area;
	uint32 reserved;
};

enum { AMDGPU_DMA_COPY = 1, AMDGPU_DMA_FILL = 2 };
struct amdgpu_dma_submit {
	uint32 version;
	uint32 size;
	uint32 operation;
	uint32 value;
	uint64 source;
	uint64 destination;
	uint64 source_offset;
	uint64 destination_offset;
	uint64 bytes;
	uint64 fence;
};

struct amdgpu_fence_wait {
	uint32 version;
	uint32 size;
	uint64 fence;
	int64 timeout_us; // relative; 0 polls, maximum 5 seconds
	int32 status; // GPU result; ioctl errors describe the wait/request itself
	uint32 reserved;
};

struct amdgpu_memory_info {
	uint32 version;
	uint32 size;
	uint64 total_vram;
	uint64 visible_vram;
	uint64 allocated_bytes;
	uint64 client_bytes;
	uint64 submitted;
	uint64 completed;
	uint32 pending_jobs;
	uint32 faulted;
};

struct amdgpu_gart_info {
	uint32 version;
	uint32 size;
	uint64 total_bytes;
	uint64 allocated_bytes; // includes buffers retained by pending jobs
	uint64 client_bytes;
	uint64 bound_pages; // cumulative successful RAM bindings
	uint64 scatter_boundaries; // physically nonadjacent pages within bindings
	uint32 vm_fault_status;
	uint32 vm_fault_address; // GPU virtual page number, not a CPU address
};

struct amdgpu_register_value {
	uint32 index;	// AMD register indices are in dwords, not bytes
	uint32 value;
};

#define AMDGPU_INFO_REGISTER_COUNT 40

struct amdgpu_info {
	uint32 version;	// caller must supply AMDGPU_HAIKU_ABI_VERSION
	uint32 size;		// caller must supply sizeof(amdgpu_info)
	uint16 vendor;
	uint16 device;
	uint16 subsystem_vendor;
	uint16 subsystem_device;
	uint8 revision;
	uint8 bus;
	uint8 slot;
	uint8 function;
	uint32 interrupt;
	uint32 pci_command;
	uint64 bar_address[6];
	uint64 bar_size[6];
	uint64 vram_size;
	uint64 vram_gpu_base;
	uint64 boot_framebuffer;
	uint64 boot_framebuffer_size;
	uint32 boot_width;
	uint32 boot_height;
	uint32 boot_stride;
	uint32 register_count;
	amdgpu_register_value registers[AMDGPU_INFO_REGISTER_COUNT];
};

#endif
