// glbuild_xbox.c — Xbox (nxdk) GL shim for POLYMOST
// Milestone 2: Real pbkit NV2A GPU rendering backend.
//
// Replaces the Milestone 1 stubs with implementations that drive the GPU
// via pbkit push buffers. The POLYMOST renderer calls GL functions through
// the glfunc dispatch table; we translate those into NV2A register writes.

#include "build.h"

#if USE_OPENGL

#include "glbuild_priv.h"
#include "osd.h"
#include "baselayer.h"
#include "baselayer_priv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <pbkit/pbkit_draw.h>
#include <pbkit/nv_objects.h>
#include <xboxkrnl/xboxkrnl.h>

// NV2A CRT controller register for framebuffer scanout address
#define XBOX_VIDEO_BASE  0xFD000000
#define PCRTC_START      0x00600800

// NV2A index data registers (from nv_regs.h):
// 0x1800 = NV097_ARRAY_ELEMENT16: each 32-bit write = 2 packed 16-bit indices
// 0x1808 = NV097_ARRAY_ELEMENT32: each 32-bit write = 1 individual 32-bit index
#define NV097_ARRAY_ELEMENT32 0x00001808

// NV2A vertex buffer cache invalidation (from nv_regs.h):
// Writing to this register forces the NV2A to re-fetch vertex data from
// the current array offsets, discarding any cached vertex buffer contents.
#define NV097_BREAK_VERTEX_BUFFER_CACHE 0x00001710

// NV2A immediate vertex data registers (from nv_regs.h):
// Per-vertex attribute submission: write data directly, no vertex arrays needed.
// Slot offsets: 2F uses slot*8, 4F uses slot*16.
#define NV097_SET_VERTEX_DATA2F_M 0x00001880
#define NV097_SET_VERTEX_DATA4F_M 0x00001A00

struct glbuild_funcs glfunc;

// ---- NV2A helpers ----
#define MAXRAM 0x03FFAFFF
#define MASK(mask, val) (((val) << (ffs(mask)-1)) & (mask))

// ---- GL string responses ----
static const GLubyte xbox_gl_version[]    = "2.0 NV2A Xbox";
static const GLubyte xbox_gl_vendor[]     = "NVIDIA (Xbox)";
static const GLubyte xbox_gl_renderer[]   = "NV2A (pbkit)";
static const GLubyte xbox_gl_extensions[] = "GL_EXT_texture_filter_anisotropic "
                                            "GL_EXT_bgra "
                                            "GL_EXT_texture_compression_s3tc "
                                            "GL_ARB_shading_language_100";
static const GLubyte xbox_glsl_version[]  = "1.10";

// ---- ID generator ----
static GLuint xbox_next_id = 1;

// ---- pbkit state ----
int xbox_pbkit_initialized = 0;  // Exposed for sdlayer2.c
static int screen_width, screen_height;

// ---- Uniform location IDs ----
// glGetUniformLocation returns these for known uniform names.
enum {
	XLOC_MODELVIEW = 1,
	XLOC_PROJECTION,
	XLOC_TEXTURE,
	XLOC_GLOWTEXTURE,
	XLOC_ALPHACUT,
	XLOC_COLOUR,
	XLOC_FOGCOLOUR,
	XLOC_FOGDENSITY,
	XLOC_GAMMA,
	XLOC_BGCOLOUR,
	XLOC_MODE,
};

// ---- Attribute slot IDs (NV2A vertex attribute slots) ----
#define XATTR_VERTEX   0
#define XATTR_TEXCOORD 9

// ---- Render state cache ----
static struct {
	int blend_enabled, depth_test_enabled, cull_enabled, alpha_test_enabled;
	GLenum blend_sfactor, blend_dfactor;
	GLenum depth_func;
	GLboolean depth_mask;
	GLenum cull_face;
	GLenum front_face;
	float clear_r, clear_g, clear_b, clear_a;
	GLenum active_texture;  // GL_TEXTURE0 or GL_TEXTURE1
	GLuint bound_texture[2]; // per-unit
	GLuint bound_vbo;        // currently bound GL_ARRAY_BUFFER
	GLuint bound_ibo;        // currently bound GL_ELEMENT_ARRAY_BUFFER
} gl_state;

// ---- Uniform cache ----
static struct {
	float modelview[16], projection[16];
	float colour[4], fogcolour[4];
	float fogdensity, alphacut, gamma;
} gl_uniforms;

// ---- Texture table ----
#define MAX_TEXTURES 2048
static struct xbox_texture {
	void *addr;        // contiguous GPU memory
	int width, height, pitch;
	int alloc_w, alloc_h;  // allocated dimensions (may be POT-padded from original w,h)
	float u_scale, v_scale; // UV scale: original_w/alloc_w, original_h/alloc_h (1.0 if no padding)
	int allocated;
	int swizzled;      // 1 = SZ (Morton-swizzled) format, supports REPEAT wrap
	int wrap_s, wrap_t;
	int min_filter, mag_filter;
	int last_used_frame; // frame number when last bound (for LRU eviction)
	int alloc_size;      // size in bytes of the contiguous allocation
} texture_table[MAX_TEXTURES];
static int total_texture_bytes = 0;  // total contiguous memory allocated for textures
static int xbox_tex_alloc_failed = 0;  // set when MmAllocateContiguous fails (signals precache to stop)
static int xbox_tex_alloc_fail_count = 0;  // throttle alloc failure logging per level
#define TEX_BUDGET_BYTES (8 * 1024 * 1024)  // proactive eviction threshold

// Free list for recycling texture IDs deleted via glDeleteTextures.
// Only explicitly-deleted IDs go here (not LRU-evicted ones), because
// pt_unload also clears the polymosttex glpic references — making reuse safe.
static GLuint tex_free_list[MAX_TEXTURES];
static int tex_free_count = 0;

// ---- NV2A texture swizzling (Morton code) ----
// NV2A's swizzled (SZ_) texture format interleaves x and y coordinate bits
// for cache-efficient 2D access. Required for REPEAT wrapping support.

static inline int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
static inline int next_pot(int v) { int p = 1; while (p < v) p <<= 1; return p; }

// Swizzle a linear ARGB8 image into NV2A Morton-code order.
// dst must be w*h*4 bytes. w and h must be powers of two.
// NV2A swizzle address for non-square POT textures.
// Interleave bits up to min(log2w, log2h), then append remaining
// bits of the larger dimension as MSBs.
static inline unsigned int nv2a_swizzle_addr(int x, int y, int log2w, int log2h)
{
	int min_log2 = log2w < log2h ? log2w : log2h;
	unsigned int offset = 0;
	// Standard Morton interleave for the lower min_log2 bits
	for (int i = 0; i < min_log2; i++) {
		offset |= ((x >> i) & 1) << (2 * i);
		offset |= ((y >> i) & 1) << (2 * i + 1);
	}
	// Remaining bits of the larger dimension appended as high bits
	int shift = 2 * min_log2;
	if (log2w > log2h)
		offset |= ((unsigned int)x >> min_log2) << shift;
	else if (log2h > log2w)
		offset |= ((unsigned int)y >> min_log2) << shift;
	return offset;
}

static void swizzle_rect(const void *src, int src_pitch,
                         void *dst, int w, int h, int bpp)
{
	const uint8_t *s = (const uint8_t *)src;
	uint8_t *d = (uint8_t *)dst;
	int log2w = 0, log2h = 0;
	{ int v = w; while (v > 1) { v >>= 1; log2w++; } }
	{ int v = h; while (v > 1) { v >>= 1; log2h++; } }

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			unsigned int offset = nv2a_swizzle_addr(x, y, log2w, log2h) * bpp;
			memcpy(d + offset, s + y * src_pitch + x * bpp, bpp);
		}
	}
}

// ---- Buffer table (CPU-side + GPU-mapped) ----
#define MAX_BUFFERS 64
static struct xbox_buffer {
	void *cpu_data;    // malloc'd copy (for IBO)
	void *gpu_addr;    // pointer into VBO streaming pool (for VBO)
	int size;
	int is_vbo;        // 1 if this buffer was used as GL_ARRAY_BUFFER
} buffer_table[MAX_BUFFERS];

// ---- Vertex streaming pool (contiguous GPU memory) ----
#define VBO_POOL_SIZE (256*1024)  // Peak usage ~33KB; 256KB = 8x headroom
static void *vbo_pool = NULL;
static int vbo_pool_offset = 0;

// ---- Persistent staging buffer (reusable across texture uploads) ----
#define STAGING_BUF_SIZE (512 * 512 * 4)  // 1MB — covers max practical Duke3D texture
static unsigned char *staging_buf = NULL;

// ---- Null texture (16x16 white, for glow stage when no texture bound) ----
// NV2A requires minimum pitch alignment; tiny textures get rejected.
#define NULL_TEX_SIZE 16
#define NULL_TEX_PITCH (NULL_TEX_SIZE * 4)
static void *null_texture_addr = NULL;
static GLuint null_texture_id = 0;

// ---- Debug counters ----
static int viewport_set_count = 0;
static int frame_draw_count = 0;   // per-frame draw counter (reset in glClear)
static int frame_skip_count = 0;   // per-frame skipped draws (early returns)
static int frame_clip_count = 0;   // per-frame clipped draws (near-plane)
static int global_frame_num = 0;   // monotonic frame counter
static int draw_since_sync = 0;    // draws since last GPU sync (push buffer overflow prevention)
static int frame_2d_count = 0;     // per-frame non-perspective (rotatesprite/HUD) draws
static int frame_depthoff_count = 0; // per-frame draws with depth test disabled
static int frame_state_skip_count = 0; // per-frame state writes skipped by dirty tracking

// ---- GPU state cache (dirty-bit tracking to skip redundant NV2A writes) ----
// Tracks what was last written to the GPU so we can skip unchanged state.
// Reset at start of each frame (glClear) to ensure clean baseline.
static struct {
	// Shader constants
	float mvp[16];              // c[0]-c[3]
	float colour[4];            // c[4]
	float texscale[2];          // c[6].xy
	int constants_valid;        // 0 = must write all constants

	// Texture binding
	uintptr_t tex_addr;         // GPU address of bound texture
	int tex_swizzled;
	int tex_alloc_w, tex_alloc_h;
	int tex_width, tex_height;
	uint32_t tex_pitch;
	uint32_t tex_filter;
	uint32_t tex_wrap;
	int tex_valid;              // 0 = must write texture state

	// Alpha test
	int alpha_test_enabled;
	int alpha_ref;
	int alpha_valid;

	// Blend / depth / cull
	int blend_enabled;
	uint32_t blend_sfactor, blend_dfactor;
	int depth_test_enabled;
	uint32_t depth_func;
	int depth_mask;
	int cull_enabled;
	uint32_t front_face;
	uint32_t cull_face;
	int renderstate_valid;
} gpu_cache;


// ---- Vertex attrib pointer state ----
static struct {
	GLint size;
	GLsizei stride;
	intptr_t offset;
	int enabled;
} attrib_state[16];

// ---- Viewport params (saved from glViewport, used to build viewport matrix) ----
static float vp_x, vp_y, vp_w, vp_h;
static int vp_valid = 0;

// ---- Depth range (saved from glDepthRange, used in viewport matrix z-transform) ----
static float depth_range_near = 0.0f;
static float depth_range_far  = 1.0f;

// ---- Compiled shaders (generated from .cg by nxdk toolchain) ----
// 11 instructions × 4 uint32_t per instruction = 44 words (w-clamp + perspective divide + TEX0.q=1)
static uint32_t vs_program[] = {
	#include "polymost_vs.inl"
};
#define VS_PROGRAM_SLOTS (sizeof(vs_program) / 16)

// ---- Forward declarations ----
static int osdcmd_glinfo(const osdfuncparm_t *);
static void xbox_load_shaders(void);
static void xbox_setup_combiners(void);
static void xbox_set_attrib_pointer(unsigned int index, unsigned int size,
	unsigned int stride, const void *data);
static uint32_t xbox_tex_format_argb8(void);
static uint32_t xbox_tex_format_argb8_swizzled(int w, int h);
static void build_viewport_matrix(float *out);

// ====================================================================
// NV2A register combiner setup: output = texture0 * diffuse_colour
// This is applied once at init and whenever we re-init pbkit.
// The .ps.cg compiles to register combiner config via fp20compiler.
// ====================================================================
static void xbox_setup_combiners(void)
{
	uint32_t *p;
	p = pb_begin();
	#include "polymost_ps.inl"
	pb_end(p);
}

// ====================================================================
// Load vertex shader program into NV2A transform engine
// ====================================================================
static void xbox_load_shaders(void)
{
	uint32_t *p;
	int i;

	p = pb_begin();
	p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
	p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
		     NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM)
		| MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
		       NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
	p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
	pb_end(p);

	// Set load cursor to slot 0
	p = pb_begin();
	p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
	pb_end(p);

	// Upload vertex program instructions (4 uint32_t per instruction)
	for (i = 0; i < (int)VS_PROGRAM_SLOTS; i++) {
		p = pb_begin();
		pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
		memcpy(p, &vs_program[i*4], 4*4);
		p += 4;
		pb_end(p);
	}
}


// ====================================================================
// GL function implementations
// ====================================================================

// ====================================================================
// Diagnostic test draw: THREE methods to isolate rendering issues.
//
// Test 1 (RED bar, top): Direct CPU framebuffer write — no GPU at all.
//   If visible → display path (PCRTC scanout) works.
//   If invisible → framebuffer address or pixel format is wrong.
//
// Test 2 (GREEN bar, middle): NV2A indexed draw with ARRAY_ELEMENT32.
//   Uses vertex arrays in contiguous memory + our vertex shader.
//   If visible → index fix + vertex shader + 3D pipeline all work.
//
// Test 3 (BLUE bar, bottom): NV2A immediate vertex data (SET_VERTEX_DATA4F).
//   No vertex arrays, no indices, no contiguous memory.
//   If visible → 3D pipeline works but vertex arrays may be broken.
//
// Called EVERY frame from glClear to be persistently visible.
// ====================================================================
static void *test_verts = NULL;
static int test_log_count = 0;

static void xbox_test_draw(void)
{
	if (!xbox_pbkit_initialized) return;

	// ================================================================
	// TEST 1: Direct CPU framebuffer write — RED bar, top 60 pixels
	// ================================================================
	{
		DWORD *fb = (DWORD *)pb_back_buffer();
		if (fb) {
			int w = screen_width > 0 ? screen_width : 640;
			for (int y = 10; y < 70; y++) {
				for (int x = 10; x < w - 10; x++) {
					fb[y * w + x] = 0xFFFF0000; // ARGB red
				}
			}
		}
		if (test_log_count < 2)
			xbox_log("Xbox TEST1: CPU fb write red bar to %p\n", (void *)pb_back_buffer());
	}

	// Wait for GPU to finish any pending 2D ops (pb_fill etc.)
	while (pb_busy()) { /* spin */ }

	// ================================================================
	// TEST 2: PROGRAM mode — GREEN quad, large and centered.
	// Uses our vertex shader with a VIEWPORT matrix as MVP.
	// This matches the nxdk triangle sample's approach exactly:
	//   - Shader does: pos = mul(vertex, viewport_matrix); pos.xyz /= pos.w;
	//   - Output is screen-space coords (e.g. 100,200)
	//   - NV2A uses output directly as window coords (no hw viewport)
	//
	// KEY BUG FIX: Constants uploaded to physical slot 96, NOT 0.
	// On NV2A, shader c[0] maps to physical constant slot 96.
	// The first 96 slots (0-95) are fixed-function pipeline constants.
	// ================================================================
	if (null_texture_addr) {
		// Allocate test vertex buffer once
		if (!test_verts) {
			test_verts = MmAllocateContiguousMemoryEx(256, 0, MAXRAM, 0,
				PAGE_READWRITE | PAGE_WRITECOMBINE);
			if (test_verts && test_log_count < 2)
				xbox_log("Xbox TEST2: alloc verts at %p\n", test_verts);
		}

		if (test_verts) {
			// Vertex data in NDC-like coords [-1,1] with Z=1.
			// The viewport matrix in the MVP will map these to screen pixels.
			// Z=1 maps to 65536 in screen depth (within 24-bit range).
			float verts[] = {
				// pos_x,  pos_y, pos_z,  tex_u, tex_v
				-0.8f, -0.6f,  1.0f,   0.0f, 0.0f,  // maps to ~(64, 384)
				 0.8f, -0.6f,  1.0f,   1.0f, 0.0f,  // maps to ~(576, 384)
				 0.8f,  0.6f,  1.0f,   1.0f, 1.0f,  // maps to ~(576, 96)
				-0.8f,  0.6f,  1.0f,   0.0f, 1.0f,  // maps to ~(64, 96)
			};
			memcpy(test_verts, verts, sizeof(verts));

			// Ensure PROGRAM mode with our vertex shader
			uint32_t *p = pb_begin();
			p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
				MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
				     NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM)
				| MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
				       NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
			p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
			p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
			p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
			p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
			p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
			p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
			p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
			p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
			pb_end(p);

			// Re-load vertex shader
			xbox_load_shaders();

			// Build viewport matrix: maps NDC [-1,1] to screen [0,640]×[0,480]
			// Same as the nxdk triangle sample's matrix_viewport().
			// Row-major for Cg's mul(row_vector, matrix):
			//   [w/2    0      0     0]
			//   [0    -h/2     0     0]
			//   [0      0    65536   0]
			//   [w/2   h/2     0     1]
			float sw = (float)screen_width;
			float sh = (float)screen_height;
			float viewport_mvp[16] = {
				sw/2.0f,    0.0f,       0.0f, 0.0f,
				0.0f,      -sh/2.0f,    0.0f, 0.0f,
				0.0f,       0.0f,   65536.0f, 0.0f,
				sw/2.0f,    sh/2.0f,    0.0f, 1.0f
			};
			float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

			// Upload constants c[0]-c[6] to PHYSICAL SLOT 96 in one batch
			p = pb_begin();
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID, 96);
			pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 28);
			memcpy(p, viewport_mvp, 64); p += 16;               // c[0]-c[3]: MVP
			memcpy(p, green, 16); p += 4;                        // c[4]: colour
			{                                                     // c[5]: wclamp
				static const float c5[4] = { 0.0001f, 1.0f, 0.0f, 0.0f };
				memcpy(p, c5, 16); p += 4;
			}
			{                                                     // c[6]: texscale
				static const float c6[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
				memcpy(p, c6, 16); p += 4;
			}
			pb_end(p);

			// Combiners + null texture (white 16x16) — SZ format
			xbox_setup_combiners();
			p = pb_begin();
			p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
				(DWORD)(uintptr_t)null_texture_addr & 0x03ffffff,
				xbox_tex_format_argb8_swizzled(NULL_TEX_SIZE, NULL_TEX_SIZE));
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0), NULL_TEX_PITCH << 16);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0),
				(NULL_TEX_SIZE << 16) | NULL_TEX_SIZE);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), 0x00030303);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), 0x04074000);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(1), 0x0003ffc0);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(2), 0x0003ffc0);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(3), 0x0003ffc0);
			pb_end(p);

			// Clear all 16 attrib slots then set pos + texcoord
			p = pb_begin();
			pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
			for (int i = 0; i < 16; i++) *(p++) = 2;
			pb_end(p);
			xbox_set_attrib_pointer(0, 3, 20, test_verts);
			xbox_set_attrib_pointer(9, 2, 20, (char *)test_verts + 12);

			// Draw quad as triangle fan
			p = pb_begin();
			p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLE_FAN);
			pb_push(p++, 0x40000000 | NV097_ARRAY_ELEMENT32, 4);
			*p++ = 0; *p++ = 1; *p++ = 2; *p++ = 3;
			p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
			pb_end(p);

			if (test_log_count < 2)
				xbox_log("Xbox TEST2: PROGRAM mode green quad (viewport MVP, const@96)\n");
		}
	}

	test_log_count++;
}

static void APIENTRY xbox_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	gl_state.clear_r = r;
	gl_state.clear_g = g;
	gl_state.clear_b = b;
	gl_state.clear_a = a;
}

static int clear_frame_number = 0;  // frame counter (reset on pbkit shutdown)
static int frame_setup_done = 0;    // 1 = frame-start setup has been done for current frame

// Frame-start setup: pb_reset, render target, VBO pool reset, GPU state.
// Called at the start of every frame — either from glClear (if the game
// calls it) or from xbox_polymost_frame_start (called by showframe after
// the previous frame's pb_finished, ensuring the next frame is ready).
// The frame_setup_done flag prevents double-setup when glClear IS called.
static void xbox_do_frame_start(void)
{
	int frame_number = clear_frame_number;

	pb_wait_for_vbl();
	pb_reset();
	pb_target_back_buffer();
	if (frame_number == 0)
		xbox_log("Xbox: first frame reset done\n");

	// Per-frame draw stats (log first 60 frames only — periodic logging
	// causes disk I/O stalls that produce visible hitching every few seconds)
	if (frame_number > 0 && frame_number <= 3) {
		xbox_log("Xbox: FRAME %d end: draws=%d skips=%d clips=%d 2d=%d depthoff=%d tex=%d vbo=%d stskip=%d\n",
			frame_number - 1, frame_draw_count, frame_skip_count, frame_clip_count,
			frame_2d_count, frame_depthoff_count,
			total_texture_bytes, vbo_pool_offset, frame_state_skip_count);
	}
	frame_draw_count = 0;
	frame_skip_count = 0;
	frame_clip_count = 0;
	frame_2d_count = 0;
	frame_depthoff_count = 0;
	frame_state_skip_count = 0;
	global_frame_num = frame_number;

	// Invalidate GPU state cache at frame start so first draw writes everything
	gpu_cache.constants_valid = 0;
	gpu_cache.tex_valid = 0;
	gpu_cache.alpha_valid = 0;
	gpu_cache.renderstate_valid = 0;
	clear_frame_number = ++frame_number;

	draw_since_sync = 0;  // reset since pb_reset was just called

	// Log peak VBO usage before resetting (diagnostic for pool sizing)
	{
		static int peak_vbo = 0;
		if (vbo_pool_offset > peak_vbo) {
			peak_vbo = vbo_pool_offset;
			if (peak_vbo > VBO_POOL_SIZE / 2) {
				xbox_log("Xbox: VBO peak usage: %d/%d bytes (%.0f%%)\n",
					peak_vbo, VBO_POOL_SIZE,
					100.0f * peak_vbo / VBO_POOL_SIZE);
			}
		}
	}
	// Reset VBO streaming pool for the new frame.
	vbo_pool_offset = 0;
	draw_since_sync = 0;

	// ---- Per-frame static GPU state (set ONCE, not per-draw) ----
	{
		uint32_t *p = pb_begin();

		// Re-establish PROGRAM mode
		p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
			MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
			     NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM)
			| MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
			       NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
		p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
		p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
		p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);

		// Disable texture stages 1-3 (only stage 0 is used)
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(1), 0x0003ffc0);
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(2), 0x0003ffc0);
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(3), 0x0003ffc0);
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(1), 0x02022000);
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(2), 0x02022000);
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(3), 0x02022000);

		// Re-set critical pipeline state to known-good values.
		p = pb_push1(p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL);
		p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
		p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
		p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
		p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
		p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);

		// Clear all 16 vertex attribute slots
		pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
		for (int i = 0; i < 16; i++) {
			*(p++) = 2; // TYPE_F with size=0, stride=0 (disabled)
		}
		pb_end(p);

		// Register combiners (fragment processing)
		xbox_setup_combiners();
	}

	frame_setup_done = 1;
}

// Called by showframe (sdlayer2.c) after pb_finished to prepare the next
// frame.  This ensures every frame gets pb_reset + GPU state setup even
// when the game doesn't call glClear between frames (e.g. main menu).
void xbox_polymost_frame_start(void)
{
	if (!xbox_pbkit_initialized) return;
	xbox_do_frame_start();
}

// Force a full frame re-setup on next rendering call.
// Use after an external pb_reset (e.g. level restart mid-frame).
void xbox_force_frame_reset(void)
{
	frame_setup_done = 0;
}

static void APIENTRY xbox_glClear(GLbitfield mask)
{
	if (!xbox_pbkit_initialized) return;

	// Ensure frame-start setup has been done (pb_reset, render target, GPU state).
	// If showframe already called xbox_polymost_frame_start, this is a no-op.
	if (!frame_setup_done) {
		xbox_do_frame_start();
	}

	int frame_number = clear_frame_number;
	if (frame_number <= 3) {
		xbox_log("Xbox: glClear(%x) scr=%dx%d col=%d,%d,%d/255 vp_set=%d\n",
			mask, screen_width, screen_height,
			(int)(gl_state.clear_r * 255), (int)(gl_state.clear_g * 255),
			(int)(gl_state.clear_b * 255), viewport_set_count);
	}

	/* Respect the current viewport for clears — polymost sets glViewport
	 * to the game window area before glClear, so the border area drawn by
	 * drawbackground() is preserved.  Fall back to full screen if no
	 * viewport has been set or if it covers the entire screen. */
	{
		int cx = 0, cy = 0, cw = screen_width, ch = screen_height;
		if (vp_valid && ((int)vp_w < screen_width || (int)vp_h < screen_height)) {
			cx = (int)vp_x;
			/* OpenGL viewport Y is bottom-up; convert to top-down for NV2A */
			cy = screen_height - (int)vp_y - (int)vp_h;
			cw = (int)vp_w;
			ch = (int)vp_h;
			if (cx < 0) cx = 0;
			if (cy < 0) cy = 0;
			if (cx + cw > screen_width) cw = screen_width - cx;
			if (cy + ch > screen_height) ch = screen_height - cy;
		}

		pb_set_depth_stencil_buffer_region(
			NV097_SET_SURFACE_FORMAT_ZETA_Z24S8,
			0xFFFFFF, 0x00,
			cx, cy, cw, ch);

		if (mask & GL_COLOR_BUFFER_BIT) {
			unsigned char cr = (unsigned char)(gl_state.clear_r * 255.0f);
			unsigned char cg = (unsigned char)(gl_state.clear_g * 255.0f);
			unsigned char cb = (unsigned char)(gl_state.clear_b * 255.0f);
			unsigned char ca = (unsigned char)(gl_state.clear_a * 255.0f);
			DWORD color = (ca << 24) | (cr << 16) | (cg << 8) | cb;
			pb_fill(cx, cy, cw, ch, color);
		}
	}

	// Wait for clears to complete and reset push buffer before draws
	{
		DWORD t0 = KeTickCount;
		while (pb_busy()) {
			if (KeTickCount - t0 > 500) {
				xbox_log("Xbox: glClear pb_busy TIMEOUT after 500 ticks! frame=%d\n", clear_frame_number);
				break;
			}
		}
		pb_reset();
	}
}

static void APIENTRY xbox_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	if (!xbox_pbkit_initialized) return;
	uint32_t mask_val = 0;
	if (r) mask_val |= (1 << 16);  // R
	if (g) mask_val |= (1 << 8);   // G
	if (b) mask_val |= (1 << 0);   // B
	if (a) mask_val |= (1 << 24);  // A
	uint32_t *p = pb_begin();
	p = pb_push1(p, NV097_SET_COLOR_MASK, mask_val);
	pb_end(p);
}

static void APIENTRY xbox_glEnable(GLenum cap)
{
	// Cache only — actual GPU state is flushed at draw time
	switch (cap) {
		case GL_BLEND:      gl_state.blend_enabled = 1; break;
		case GL_DEPTH_TEST: gl_state.depth_test_enabled = 1; break;
		case GL_CULL_FACE:  gl_state.cull_enabled = 1; break;
		default: break;
	}
}

static void APIENTRY xbox_glDisable(GLenum cap)
{
	switch (cap) {
		case GL_BLEND:      gl_state.blend_enabled = 0; break;
		case GL_DEPTH_TEST: gl_state.depth_test_enabled = 0; break;
		case GL_CULL_FACE:  gl_state.cull_enabled = 0; break;
		default: break;
	}
}

static void APIENTRY xbox_glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	gl_state.blend_sfactor = sfactor;
	gl_state.blend_dfactor = dfactor;
}

static void APIENTRY xbox_glCullFace(GLenum mode)
{
	gl_state.cull_face = mode;
}

static void APIENTRY xbox_glFrontFace(GLenum mode)
{
	gl_state.front_face = mode;
}

static void APIENTRY xbox_glPolygonOffset(GLfloat factor, GLfloat units)
{
	(void)factor; (void)units;
	// Stub for now — polygon offset deferred
}

static void APIENTRY xbox_glPolygonMode(GLenum face, GLenum mode)
{
	(void)face; (void)mode;
	// Xbox: always fill mode
}

static void APIENTRY xbox_glDepthFunc(GLenum func)
{
	gl_state.depth_func = func;
}

static void APIENTRY xbox_glDepthMask(GLboolean flag)
{
	gl_state.depth_mask = flag;
}

static void APIENTRY xbox_glDepthRange(GLdouble n, GLdouble f)
{
	depth_range_near = (float)n;
	depth_range_far  = (float)f;
}

static void APIENTRY xbox_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	if (!xbox_pbkit_initialized) return;

	if (viewport_set_count < 3) {
		xbox_log("Xbox: glViewport(%d, %d, %d, %d)\n", x, y, w, h);
		viewport_set_count++;
	}

	// Save viewport params for building the viewport matrix at draw time.
	// In NV2A PROGRAM mode, the vertex shader must output screen-space
	// coordinates — the hardware does NOT do perspective divide or viewport
	// transform. We bake the viewport into the MVP matrix on the CPU.
	vp_x = (float)x;
	vp_y = (float)y;
	vp_w = (float)w;
	vp_h = (float)h;
	vp_valid = 1;

	// In PROGRAM mode, the vertex shader handles viewport transform via the
	// MVP matrix (baked on CPU). The mesh sample does NOT write hardware
	// viewport registers (NV097_SET_VIEWPORT_OFFSET/SCALE) — it relies on
	// pb_init() defaults. We do the same: just save params for CPU use.
}

static void APIENTRY xbox_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	(void)x; (void)y; (void)w; (void)h;
	// Stub — scissor test not commonly used
}

static void APIENTRY xbox_glMinSampleShadingARB(GLfloat val)
{
	(void)val;
}

static void APIENTRY xbox_glGetFloatv(GLenum pname, GLfloat *data)
{
	if (pname == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT && data) *data = 4.0f;
	else if (data) *data = 0.0f;
}

static void APIENTRY xbox_glGetIntegerv(GLenum pname, GLint *data)
{
	if (!data) return;
	switch (pname) {
		case GL_MAX_TEXTURE_SIZE:        *data = 4096; break;
		case GL_MAX_TEXTURE_IMAGE_UNITS: *data = 4;    break;
		case GL_MAX_VERTEX_ATTRIBS:      *data = 16;   break;
		default:                         *data = 0;    break;
	}
}

static const GLubyte * APIENTRY xbox_glGetString(GLenum name)
{
	switch (name) {
		case GL_VERSION:                  return xbox_gl_version;
		case GL_VENDOR:                   return xbox_gl_vendor;
		case GL_RENDERER:                 return xbox_gl_renderer;
		case GL_EXTENSIONS:               return xbox_gl_extensions;
		case GL_SHADING_LANGUAGE_VERSION: return xbox_glsl_version;
		default:                          return (const GLubyte *)"";
	}
}

static GLenum APIENTRY xbox_glGetError(void) { return GL_NO_ERROR; }
static void APIENTRY xbox_glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
static void APIENTRY xbox_glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }

static void APIENTRY xbox_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *px)
{
	(void)x; (void)y; (void)w; (void)h; (void)fmt; (void)type;
	if (px) memset(px, 0, w * h * 4);
}


// ====================================================================
// Texture management
// ====================================================================

static void APIENTRY xbox_glGenTextures(GLsizei n, GLuint *textures)
{
	for (GLsizei i = 0; i < n; i++) {
		GLuint id;
		if (tex_free_count > 0) {
			id = tex_free_list[--tex_free_count];
		} else {
			id = xbox_next_id++;
		}
		textures[i] = id;
		if (id < MAX_TEXTURES) {
			memset(&texture_table[id], 0, sizeof(texture_table[id]));
			// Default wrap/filter
			texture_table[id].wrap_s = GL_REPEAT;
			texture_table[id].wrap_t = GL_REPEAT;
			texture_table[id].min_filter = GL_NEAREST;
			texture_table[id].mag_filter = GL_NEAREST;
			texture_table[id].u_scale = 1.0f;
			texture_table[id].v_scale = 1.0f;
		}
	}
}

static void APIENTRY xbox_glDeleteTextures(GLsizei n, const GLuint *textures)
{
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = textures[i];
		if (id > 0 && id < MAX_TEXTURES) {
			if (texture_table[id].allocated && texture_table[id].addr) {
				total_texture_bytes -= texture_table[id].alloc_size;
				MmFreeContiguousMemory(texture_table[id].addr);
			}
			memset(&texture_table[id], 0, sizeof(texture_table[id]));
			// Recycle this ID for future glGenTextures calls.
			// Safe because the caller (pt_unload) also clears the
			// polymosttex glpic references, so no dangling matches.
			if (tex_free_count < MAX_TEXTURES) {
				tex_free_list[tex_free_count++] = id;
			}
		}
	}
}

static void APIENTRY xbox_glBindTexture(GLenum target, GLuint texture)
{
	(void)target;
	int unit = (gl_state.active_texture == GL_TEXTURE1) ? 1 : 0;
	gl_state.bound_texture[unit] = texture;
	// Track last-used frame for LRU eviction
	if (texture > 0 && texture < MAX_TEXTURES && texture_table[texture].allocated) {
		texture_table[texture].last_used_frame = global_frame_num;
	}
}

static void APIENTRY xbox_glTexImage2D(GLenum target, GLint level, GLint ifmt,
	GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *px)
{
	(void)target; (void)ifmt; (void)border; (void)type;

	// Only store mipmap level 0 — NV2A is configured for single LOD.
	// Polymosttex uploads all mip levels; without this guard the smallest
	// (1x1) mip overwrites the full-res texture, causing flat solid colors.
	if (level != 0) return;

	int unit = (gl_state.active_texture == GL_TEXTURE1) ? 1 : 0;
	GLuint id = gl_state.bound_texture[unit];

	int native_pot = is_pow2(w) && is_pow2(h);
	static int tex_upload_count = 0;
	if (tex_upload_count < 10) {
		xbox_log("Xbox: glTexImage2D id=%d %dx%d fmt=%x %s px=%p\n",
			id, w, h, fmt, native_pot ? "SZ" : "SZ-PAD", px);
		tex_upload_count++;
	}

	if (id == 0 || id >= MAX_TEXTURES) return;

	struct xbox_texture *tex = &texture_table[id];

	// Free previous allocation if sizes differ
	if (tex->allocated && tex->addr && (tex->width != w || tex->height != h)) {
		total_texture_bytes -= tex->alloc_size;
		MmFreeContiguousMemory(tex->addr);
		tex->addr = NULL;
		tex->allocated = 0;
		tex->alloc_size = 0;
	}

	// ALL textures use swizzled (SZ) format for reliable NV2A rendering.
	// NPOT textures are padded to the next power-of-two dimensions.
	// UV scaling compensates for the padding (stored in tex->u_scale/v_scale).
	// NV2A SZ format rejects non-square textures where one dimension is 1
	// (e.g. 1x4 produces NPOT_SIZE=0x00010004 → "invalid data error").
	// Enforce minimum 2 for both dimensions to avoid this.
	int aw = next_pot(w);  // allocated width (POT)
	int ah = next_pot(h);  // allocated height (POT)
	if (aw < 2) aw = 2;
	if (ah < 2) ah = 2;
	int alloc_size = aw * ah * 4;

	// Proactive LRU eviction: keep texture memory under budget to avoid
	// exhausting heap memory (which causes calloc failures and corruption)
	if (!tex->allocated && total_texture_bytes + alloc_size > TEX_BUDGET_BYTES) {
		pb_reset();
		// Wait for GPU 3D pipeline to fully idle before freeing texture memory.
		// pb_reset() only waits for the DMA engine; the texture fetch units may
		// still be reading from GPU memory we're about to free.
		{
			DWORD t0 = KeTickCount;
			while (pb_busy()) {
				if (KeTickCount - t0 > 500) break;
			}
		}
		draw_since_sync = 0;
		int proactive_evicted = 0;
		while (total_texture_bytes + alloc_size > TEX_BUDGET_BYTES) {
			int lru_id = -1, lru_frame = 0x7FFFFFFF;
			for (int t = 1; t < MAX_TEXTURES; t++) {
				if (t == (int)id) continue;
				if (!texture_table[t].allocated || !texture_table[t].addr) continue;
				if (texture_table[t].last_used_frame >= global_frame_num) continue;
				if (texture_table[t].last_used_frame < lru_frame) {
					lru_frame = texture_table[t].last_used_frame;
					lru_id = t;
				}
			}
			if (lru_id < 0) break;
			total_texture_bytes -= texture_table[lru_id].alloc_size;
			MmFreeContiguousMemory(texture_table[lru_id].addr);
			texture_table[lru_id].addr = NULL;
			texture_table[lru_id].allocated = 0;
			proactive_evicted++;
		}
		if (0) { /* proactive eviction logging disabled — fires every frame, causes HDD I/O hitching */
			xbox_log("Xbox: proactive eviction: freed %d textures, total=%d budget=%d\n",
				proactive_evicted, total_texture_bytes, TEX_BUDGET_BYTES);
		}
	}

	if (!tex->allocated) {
		tex->addr = MmAllocateContiguousMemoryEx(alloc_size, 0, MAXRAM, 0,
			PAGE_READWRITE | PAGE_WRITECOMBINE);
		// LRU eviction: if allocation fails, free the least-recently-used textures
		if (!tex->addr) {
			// Sync GPU before freeing any texture memory — ensures no pending
			// draw commands reference textures we're about to free
			pb_reset();
			// Wait for GPU 3D pipeline to fully idle (see proactive eviction comment)
			{
				DWORD t0 = KeTickCount;
				while (pb_busy()) {
					if (KeTickCount - t0 > 500) break;
				}
			}
			draw_since_sync = 0;

			int evicted = 0;
			for (int attempt = 0; attempt < 64 && !tex->addr; attempt++) {
				// Find the LRU texture (oldest last_used_frame)
				int lru_id = -1, lru_frame = 0x7FFFFFFF;
				for (int t = 1; t < MAX_TEXTURES; t++) {
					if (t == (int)id) continue; // don't evict ourselves
					if (!texture_table[t].allocated || !texture_table[t].addr) continue;
					// Don't evict textures used this frame
					if (texture_table[t].last_used_frame >= global_frame_num) continue;
					if (texture_table[t].last_used_frame < lru_frame) {
						lru_frame = texture_table[t].last_used_frame;
						lru_id = t;
					}
				}
				if (lru_id < 0) break; // nothing to evict
				// Evict the LRU texture
				total_texture_bytes -= texture_table[lru_id].alloc_size;
				MmFreeContiguousMemory(texture_table[lru_id].addr);
				texture_table[lru_id].addr = NULL;
				texture_table[lru_id].allocated = 0;
				evicted++;
				// Retry allocation
				tex->addr = MmAllocateContiguousMemoryEx(alloc_size, 0, MAXRAM, 0,
					PAGE_READWRITE | PAGE_WRITECOMBINE);
			}
			if (0) { /* tex eviction logging disabled */
				xbox_log("Xbox: tex evicted %d LRU textures to alloc %dx%d (%d bytes), result=%p total=%d\n",
					evicted, aw, ah, alloc_size, tex->addr, total_texture_bytes);
			}
			if (!tex->addr) {
				/* Only log first few alloc failures per level to avoid flooding */
				if (xbox_tex_alloc_fail_count < 3)
					xbox_log("Xbox: tex alloc FAILED %dx%d (%d bytes) total=%d\n",
						aw, ah, alloc_size, total_texture_bytes);
				xbox_tex_alloc_fail_count++;
				xbox_tex_alloc_failed = 1;
				return;
			}
		}
		total_texture_bytes += alloc_size;
		tex->alloc_size = alloc_size;
		tex->width = w;
		tex->height = h;
		tex->alloc_w = aw;
		tex->alloc_h = ah;
		tex->pitch = aw * 4;
		tex->allocated = 1;
		tex->swizzled = 1;  // always swizzled now
		tex->u_scale = (float)w / (float)aw;
		tex->v_scale = (float)h / (float)ah;
		tex->last_used_frame = global_frame_num;
	} else {
		// Existing texture being re-uploaded — update LRU timestamp
		tex->last_used_frame = global_frame_num;
	}

	if (px) {
		// Build a linear BGRA buffer at the padded POT dimensions,
		// copy source data into top-left corner, then swizzle into GPU memory.
		// Use persistent staging buffer to avoid per-call heap allocation failures.
		int staging_needed = aw * ah * 4;
		unsigned char *linear;
		int staging_is_dynamic = 0;

		if (staging_buf && staging_needed <= STAGING_BUF_SIZE) {
			linear = staging_buf;
		} else {
			linear = (unsigned char *)malloc(staging_needed);
			staging_is_dynamic = 1;
			if (!linear) {
				xbox_log("Xbox: tex staging FAILED %dx%d (%d bytes) — undoing GPU alloc\n",
					aw, ah, staging_needed);
				// Undo GPU allocation so texture is retried next frame
				total_texture_bytes -= tex->alloc_size;
				MmFreeContiguousMemory(tex->addr);
				tex->addr = NULL;
				tex->allocated = 0;
				tex->alloc_size = 0;
				return;
			}
		}
		/* Fill the full POT staging buffer by wrapping the source texture.
		 * NV2A requires POT dimensions for swizzled textures.  If the source
		 * is NPOT (w < aw or h < ah), the padding must contain wrapped copies
		 * of the source data so that GL_REPEAT produces correct tiling.
		 * Without this, the padding is zeroed (black + alpha=0), creating
		 * visible voids on masked walls with alpha testing. */
		const unsigned char *sp = (const unsigned char *)px;
		if (fmt == GL_BGRA) {
			for (int row = 0; row < ah; row++) {
				int srcrow = (h > 0) ? (row % h) : 0;
				memcpy(linear + row * aw * 4, sp + srcrow * w * 4, w * 4);
				/* Wrap extra columns */
				for (int x = w; x < aw; x++) {
					memcpy(linear + row * aw * 4 + x * 4,
					       linear + row * aw * 4 + (x % w) * 4, 4);
				}
			}
		} else if (fmt == GL_RGBA) {
			for (int row = 0; row < ah; row++) {
				int srcrow = (h > 0) ? (row % h) : 0;
				unsigned char *dst = linear + row * aw * 4;
				const unsigned char *src = sp + srcrow * w * 4;
				for (int x = 0; x < w; x++) {
					dst[x*4+0] = src[x*4+2]; // B
					dst[x*4+1] = src[x*4+1]; // G
					dst[x*4+2] = src[x*4+0]; // R
					dst[x*4+3] = src[x*4+3]; // A
				}
				/* Wrap extra columns (already BGRA-swapped) */
				for (int x = w; x < aw; x++) {
					memcpy(dst + x * 4, dst + (x % w) * 4, 4);
				}
			}
		} else {
			for (int row = 0; row < ah; row++) {
				int srcrow = (h > 0) ? (row % h) : 0;
				memcpy(linear + row * aw * 4, sp + srcrow * w * 4, w * 4);
				for (int x = w; x < aw; x++) {
					memcpy(linear + row * aw * 4 + x * 4,
					       linear + row * aw * 4 + (x % w) * 4, 4);
				}
			}
		}

		swizzle_rect(linear, aw * 4, tex->addr, aw, ah, 4);

		if (staging_is_dynamic) {
			free(linear);
		}

		// Flush CPU write-combining buffers so GPU sees texture data
		__asm__ volatile("sfence" ::: "memory");
	}
}

static void APIENTRY xbox_glTexSubImage2D(GLenum target, GLint level,
	GLint xo, GLint yo, GLsizei w, GLsizei h,
	GLenum fmt, GLenum type, const void *px)
{
	(void)target; (void)type;
	if (level != 0) return;

	int unit = (gl_state.active_texture == GL_TEXTURE1) ? 1 : 0;
	GLuint id = gl_state.bound_texture[unit];
	if (id == 0 || id >= MAX_TEXTURES || !texture_table[id].allocated || !px) return;

	struct xbox_texture *tex = &texture_table[id];
	const unsigned char *src = (const unsigned char *)px;

	// All textures are now swizzled (POT-padded). Use alloc_w/alloc_h for Morton addressing.
	{
		uint8_t *d = (uint8_t *)tex->addr;
		int log2w = 0, log2h = 0;
		{ int v = tex->alloc_w; while (v > 1) { v >>= 1; log2w++; } }
		{ int v = tex->alloc_h; while (v > 1) { v >>= 1; log2h++; } }
		for (int row = 0; row < h; row++) {
			int gy = yo + row;
			for (int col = 0; col < w; col++) {
				int gx = xo + col;
				unsigned int off = nv2a_swizzle_addr(gx, gy, log2w, log2h) * 4;
				if (fmt == GL_RGBA) {
					d[off+0] = src[col*4+2]; // B
					d[off+1] = src[col*4+1]; // G
					d[off+2] = src[col*4+0]; // R
					d[off+3] = src[col*4+3]; // A
				} else {
					memcpy(d + off, src + col*4, 4);
				}
			}
			src += w * 4;
		}
	}
	// Flush CPU write-combining buffers so GPU sees texture data
	__asm__ volatile("sfence" ::: "memory");
}

static void APIENTRY xbox_glCompressedTexImage2D(GLenum target, GLint level,
	GLenum ifmt, GLsizei w, GLsizei h, GLint border,
	GLsizei sz, const void *data)
{
	(void)target; (void)border;

	// Only store mipmap level 0 (single LOD on NV2A)
	if (level != 0) return;

	int unit = (gl_state.active_texture == GL_TEXTURE1) ? 1 : 0;
	GLuint id = gl_state.bound_texture[unit];
	if (id == 0 || id >= MAX_TEXTURES) return;

	struct xbox_texture *tex = &texture_table[id];

	// Free previous
	if (tex->allocated && tex->addr) {
		MmFreeContiguousMemory(tex->addr);
		tex->addr = NULL;
		tex->allocated = 0;
	}

	// For compressed textures, NV2A supports DXT1/DXT5 natively.
	// We store the compressed data directly.
	int src_pitch;
	if (ifmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
		src_pitch = ((w + 3) / 4) * 8;  // DXT1: 8 bytes per 4x4 block
	} else {
		src_pitch = ((w + 3) / 4) * 16; // DXT5: 16 bytes per 4x4 block
	}
	// NV2A requires 64-byte pitch alignment
	int pitch = (src_pitch + 63) & ~63;

	int block_rows = (h + 3) / 4;
	int total_size = pitch * block_rows;
	tex->addr = MmAllocateContiguousMemoryEx(total_size, 0, MAXRAM, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!tex->addr) return;

	// Row-by-row copy for pitch padding
	const unsigned char *csrc = (const unsigned char *)data;
	unsigned char *cdst = (unsigned char *)tex->addr;
	int copy_per_row = src_pitch < (int)sz ? src_pitch : (int)sz;
	for (int row = 0; row < block_rows; row++) {
		int remaining = sz - (int)(csrc - (const unsigned char *)data);
		int to_copy = copy_per_row < remaining ? copy_per_row : remaining;
		if (to_copy > 0) memcpy(cdst, csrc, to_copy);
		csrc += src_pitch;
		cdst += pitch;
	}
	// Flush CPU write-combining buffers so GPU sees texture data
	__asm__ volatile("sfence" ::: "memory");
	tex->width = w;
	tex->height = h;
	tex->alloc_w = w;
	tex->alloc_h = h;
	tex->u_scale = 1.0f;
	tex->v_scale = 1.0f;
	tex->pitch = pitch;
	tex->allocated = 2; // 2 = compressed
	tex->swizzled = 0;  // compressed data is not Morton-swizzled
}

static void APIENTRY xbox_glTexParameteri(GLenum target, GLenum pname, GLint param);

static void APIENTRY xbox_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
	xbox_glTexParameteri(target, pname, (GLint)param);
}

static void APIENTRY xbox_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	(void)target;
	int unit = (gl_state.active_texture == GL_TEXTURE1) ? 1 : 0;
	GLuint id = gl_state.bound_texture[unit];
	if (id == 0 || id >= MAX_TEXTURES) return;

	switch (pname) {
		case GL_TEXTURE_WRAP_S: texture_table[id].wrap_s = param; break;
		case GL_TEXTURE_WRAP_T: texture_table[id].wrap_t = param; break;
		case GL_TEXTURE_MIN_FILTER: texture_table[id].min_filter = param; break;
		case GL_TEXTURE_MAG_FILTER: texture_table[id].mag_filter = param; break;
		default: break;
	}
}

static void APIENTRY xbox_glActiveTexture(GLenum texture)
{
	gl_state.active_texture = texture;
}


// ====================================================================
// Buffer objects (VBO / IBO)
// ====================================================================

static void APIENTRY xbox_glGenBuffers(GLsizei n, GLuint *bufs)
{
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = xbox_next_id++;
		bufs[i] = id;
		if (id < MAX_BUFFERS) {
			memset(&buffer_table[id], 0, sizeof(buffer_table[id]));
		}
	}
}

static void APIENTRY xbox_glDeleteBuffers(GLsizei n, const GLuint *bufs)
{
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = bufs[i];
		if (id > 0 && id < MAX_BUFFERS) {
			if (buffer_table[id].cpu_data) {
				free(buffer_table[id].cpu_data);
			}
			memset(&buffer_table[id], 0, sizeof(buffer_table[id]));
		}
	}
}

static void APIENTRY xbox_glBindBuffer(GLenum target, GLuint buffer)
{
	if (target == GL_ARRAY_BUFFER) {
		gl_state.bound_vbo = buffer;
	} else if (target == GL_ELEMENT_ARRAY_BUFFER) {
		gl_state.bound_ibo = buffer;
	}
}

static void APIENTRY xbox_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	(void)usage;
	GLuint id = (target == GL_ARRAY_BUFFER) ? gl_state.bound_vbo : gl_state.bound_ibo;
	if (id == 0 || id >= MAX_BUFFERS) return;

	struct xbox_buffer *buf = &buffer_table[id];

	if (target == GL_ARRAY_BUFFER) {
		// VBO: copy into contiguous GPU memory streaming pool
		buf->is_vbo = 1;
		// Keep a system-memory shadow for CPU-side clipping reads.
		// Must be done BEFORE updating buf->size so the realloc check
		// compares against the OLD allocation size, not the new data size.
		if (data && size > 0) {
			if (buf->cpu_data && buf->size < (int)size) {
				free(buf->cpu_data);
				buf->cpu_data = NULL;
			}
			if (!buf->cpu_data) {
				buf->cpu_data = malloc(size);
			}
			if (buf->cpu_data) {
				memcpy(buf->cpu_data, data, size);
			}
		}
		if (vbo_pool && data) {
			// Align to 16 bytes
			int aligned_offset = (vbo_pool_offset + 15) & ~15;
			if (aligned_offset + (int)size <= VBO_POOL_SIZE) {
				buf->gpu_addr = (char *)vbo_pool + aligned_offset;
				memcpy(buf->gpu_addr, data, size);
				__asm__ volatile("sfence" ::: "memory");
				buf->size = (int)size;
				vbo_pool_offset = aligned_offset + (int)size;
			} else {
				// Pool full — can't render this batch
				buf->gpu_addr = NULL;
				buf->size = 0;
				static int pool_full_log = 0;
				if (pool_full_log < 3) {
					xbox_log("Xbox: VBO POOL FULL off=%d+sz=%d > %d\n",
						aligned_offset, (int)size, VBO_POOL_SIZE);
					pool_full_log++;
				}
			}
		}
	} else {
		// IBO: CPU-side buffer
		buf->is_vbo = 0;
		if (buf->cpu_data && buf->size < (int)size) {
			free(buf->cpu_data);
			buf->cpu_data = NULL;
		}
		if (!buf->cpu_data && size > 0) {
			buf->cpu_data = malloc(size);
		}
		if (buf->cpu_data && data) {
			memcpy(buf->cpu_data, data, size);
		}
		buf->size = (int)size;
	}
}

static void APIENTRY xbox_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GLuint id = (target == GL_ARRAY_BUFFER) ? gl_state.bound_vbo : gl_state.bound_ibo;
	if (id == 0 || id >= MAX_BUFFERS || !data) return;

	struct xbox_buffer *buf = &buffer_table[id];
	if (buf->is_vbo && buf->gpu_addr) {
		memcpy((char *)buf->gpu_addr + offset, data, size);
	} else if (buf->cpu_data) {
		memcpy((char *)buf->cpu_data + offset, data, size);
	}
}

static void APIENTRY xbox_glEnableVertexAttribArray(GLuint index)
{
	if (index < 16) attrib_state[index].enabled = 1;
}

static void APIENTRY xbox_glDisableVertexAttribArray(GLuint index)
{
	if (index < 16) attrib_state[index].enabled = 0;
}

static void APIENTRY xbox_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
	GLboolean norm, GLsizei stride, const void *ptr)
{
	(void)type; (void)norm;
	if (index < 16) {
		attrib_state[index].size = size;
		attrib_state[index].stride = stride;
		attrib_state[index].offset = (intptr_t)ptr;
	}
}


// ====================================================================
// Shader/program stubs (real shaders are Cg-based, loaded at init)
// ====================================================================

static void APIENTRY xbox_glAttachShader(GLuint program, GLuint shader) { (void)program; (void)shader; }
static void APIENTRY xbox_glCompileShader(GLuint shader) { (void)shader; }
static GLuint APIENTRY xbox_glCreateProgram(void) { return xbox_next_id++; }
static GLuint APIENTRY xbox_glCreateShader(GLenum type) { (void)type; return xbox_next_id++; }
static void APIENTRY xbox_glDeleteProgram(GLuint program) { (void)program; }
static void APIENTRY xbox_glDeleteShader(GLuint shader) { (void)shader; }
static void APIENTRY xbox_glDetachShader(GLuint program, GLuint shader) { (void)program; (void)shader; }

static GLint APIENTRY xbox_glGetAttribLocation(GLuint program, const GLchar *name)
{
	(void)program;
	if (!name) return -1;
	if (strcmp(name, "a_vertex") == 0)   return XATTR_VERTEX;   // 0
	if (strcmp(name, "a_texcoord") == 0) return XATTR_TEXCOORD; // 9
	return 0;
}

static void APIENTRY xbox_glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
	(void)program;
	if (!params) return;
	switch (pname) {
		case GL_LINK_STATUS:     *params = GL_TRUE; break;
		case GL_INFO_LOG_LENGTH: *params = 0; break;
		default:                 *params = 0; break;
	}
}

static void APIENTRY xbox_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
	(void)program;
	if (infoLog && bufSize > 0) infoLog[0] = '\0';
	if (length) *length = 0;
}

static void APIENTRY xbox_glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
	(void)shader;
	if (!params) return;
	switch (pname) {
		case GL_COMPILE_STATUS:  *params = GL_TRUE; break;
		case GL_INFO_LOG_LENGTH: *params = 0; break;
		default:                 *params = 0; break;
	}
}

static void APIENTRY xbox_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
	(void)shader;
	if (infoLog && bufSize > 0) infoLog[0] = '\0';
	if (length) *length = 0;
}

static GLint APIENTRY xbox_glGetUniformLocation(GLuint program, const GLchar *name)
{
	(void)program;
	if (!name) return -1;
	if (strcmp(name, "u_modelview") == 0)    return XLOC_MODELVIEW;
	if (strcmp(name, "u_projection") == 0)   return XLOC_PROJECTION;
	if (strcmp(name, "u_texture") == 0)      return XLOC_TEXTURE;
	if (strcmp(name, "u_glowtexture") == 0)  return XLOC_GLOWTEXTURE;
	if (strcmp(name, "u_alphacut") == 0)     return XLOC_ALPHACUT;
	if (strcmp(name, "u_colour") == 0)       return XLOC_COLOUR;
	if (strcmp(name, "u_fogcolour") == 0)    return XLOC_FOGCOLOUR;
	if (strcmp(name, "u_fogdensity") == 0)   return XLOC_FOGDENSITY;
	if (strcmp(name, "u_gamma") == 0)        return XLOC_GAMMA;
	if (strcmp(name, "u_bgcolour") == 0)     return XLOC_BGCOLOUR;
	if (strcmp(name, "u_mode") == 0)         return XLOC_MODE;
	return -1;  // Unknown uniform
}

static void APIENTRY xbox_glLinkProgram(GLuint program) { (void)program; }
static void APIENTRY xbox_glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length)
{
	(void)shader; (void)count; (void)string; (void)length;
}

static void APIENTRY xbox_glUseProgram(GLuint program) { (void)program; }


// ====================================================================
// Uniform setters — store values for use at draw time
// ====================================================================

static void APIENTRY xbox_glUniform1i(GLint loc, GLint v0) { (void)loc; (void)v0; }

static void APIENTRY xbox_glUniform1f(GLint loc, GLfloat v0)
{
	switch (loc) {
		case XLOC_ALPHACUT:   gl_uniforms.alphacut = v0; break;
		case XLOC_FOGDENSITY: gl_uniforms.fogdensity = v0; break;
		case XLOC_GAMMA:      gl_uniforms.gamma = v0; break;
		default: break;
	}
}

static void APIENTRY xbox_glUniform2f(GLint loc, GLfloat v0, GLfloat v1) { (void)loc; (void)v0; (void)v1; }
static void APIENTRY xbox_glUniform3f(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2) { (void)loc; (void)v0; (void)v1; (void)v2; }

static void APIENTRY xbox_glUniform4f(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
	switch (loc) {
		case XLOC_COLOUR:
			gl_uniforms.colour[0] = v0;
			gl_uniforms.colour[1] = v1;
			gl_uniforms.colour[2] = v2;
			gl_uniforms.colour[3] = v3;
			{
				static int col_log = 0;
				if (col_log < 5) {
					xbox_log("Xbox: set colour=%d,%d,%d,%d/1000\n",
						(int)(v0*1000), (int)(v1*1000),
						(int)(v2*1000), (int)(v3*1000));
					col_log++;
				}
			}
			break;
		case XLOC_FOGCOLOUR:
			gl_uniforms.fogcolour[0] = v0;
			gl_uniforms.fogcolour[1] = v1;
			gl_uniforms.fogcolour[2] = v2;
			gl_uniforms.fogcolour[3] = v3;
			break;
		default: break;
	}
}

static void APIENTRY xbox_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *val)
{
	(void)count; (void)transpose;
	if (!val) return;
	switch (loc) {
		case XLOC_MODELVIEW:
			memcpy(gl_uniforms.modelview, val, 16 * sizeof(float));
			{
				static int mv_log = 0;
				if (mv_log < 3) {
					xbox_log("Xbox: MV row0=(%d,%d,%d,%d)/1000\n",
						(int)(val[0]*1000),(int)(val[1]*1000),(int)(val[2]*1000),(int)(val[3]*1000));
					xbox_log("      row1=(%d,%d,%d,%d)/1000\n",
						(int)(val[4]*1000),(int)(val[5]*1000),(int)(val[6]*1000),(int)(val[7]*1000));
					xbox_log("      row2=(%d,%d,%d,%d)/1000\n",
						(int)(val[8]*1000),(int)(val[9]*1000),(int)(val[10]*1000),(int)(val[11]*1000));
					xbox_log("      row3=(%d,%d,%d,%d)/1000\n",
						(int)(val[12]*1000),(int)(val[13]*1000),(int)(val[14]*1000),(int)(val[15]*1000));
					mv_log++;
				}
			}
			break;
		case XLOC_PROJECTION:
			memcpy(gl_uniforms.projection, val, 16 * sizeof(float));
			{
				static int pj_log = 0;
				if (pj_log < 3) {
					xbox_log("Xbox: PJ row0=(%d,%d,%d,%d)/1000\n",
						(int)(val[0]*1000),(int)(val[1]*1000),(int)(val[2]*1000),(int)(val[3]*1000));
					xbox_log("      row1=(%d,%d,%d,%d)/1000\n",
						(int)(val[4]*1000),(int)(val[5]*1000),(int)(val[6]*1000),(int)(val[7]*1000));
					xbox_log("      row2=(%d,%d,%d,%d)/1000\n",
						(int)(val[8]*1000),(int)(val[9]*1000),(int)(val[10]*1000),(int)(val[11]*1000));
					xbox_log("      row3=(%d,%d,%d,%d)/1000\n",
						(int)(val[12]*1000),(int)(val[13]*1000),(int)(val[14]*1000),(int)(val[15]*1000));
					pj_log++;
				}
			}
			break;
		default: break;
	}
}


// ====================================================================
// Draw call — the core rendering path
// ====================================================================

// Helper: 4x4 matrix multiply (result stores M_B × M_A when arrays are column-major GL format)
static void mat4_multiply(float *out, const float *a, const float *b)
{
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			out[i*4+j] =
				a[i*4+0]*b[0*4+j] +
				a[i*4+1]*b[1*4+j] +
				a[i*4+2]*b[2*4+j] +
				a[i*4+3]*b[3*4+j];
		}
	}
}

// Build viewport matrix (column-major GL format) for baking into MVP.
// NV2A in PROGRAM mode: the vertex shader must output screen-space coordinates.
// This matrix transforms from NDC [-1,1] to screen pixels + depth buffer range.
//
// Z mapping: NDC z [-1,1] → depth [0, 65536] via z_depth = z_ndc * 32768 + 32768.
// Polymost's projection produces NDC z in approx [-1, 0] for visible geometry.
// Previous mapping (scale=65536, offset=0) clamped ALL near geometry to depth=0,
// breaking depth testing entirely (every draw overwrote every pixel).
//
// IMPORTANT: Polymost sets glViewport to logical resolution (e.g. 320x240)
// but the NV2A framebuffer is the physical display (e.g. 640x480).
// We scale the viewport to physical dimensions so rendering fills the screen.
static void build_viewport_matrix(float *out)
{
	// NV2A in PROGRAM mode does NOT apply hardware viewport. The vertex shader
	// must output physical framebuffer pixel coordinates. We bake the GL
	// viewport transform into the MVP matrix.
	//
	// Polymost sets glViewport in logical pixel space (e.g. 320x240).
	// The NV2A framebuffer is in physical pixels (e.g. 640x480).
	// Scale factor = physical / logical (constant for all viewports).
	extern int xdim, ydim;  // engine.c: logical rendering resolution
	float scale_x = (xdim > 0) ? (float)screen_width / (float)xdim : 1.0f;
	float scale_y = (ydim > 0) ? (float)screen_height / (float)ydim : 1.0f;
	float phys_x = vp_x * scale_x;
	float phys_y = vp_y * scale_y;
	float phys_w = vp_w * scale_x;
	float phys_h = vp_h * scale_y;

	// Convert GL viewport Y (origin at bottom) to NV2A framebuffer Y (origin at top).
	// GL viewport (x, y, w, h): y is pixels from the bottom of the screen.
	// NV2A framebuffer: y=0 is the top row.
	// NV2A top of viewport = screen_height - phys_y - phys_h
	float nv2a_top = (float)screen_height - phys_y - phys_h;

	// Row-major matrix for v * M (matching mesh sample's matrix_viewport):
	//   out[0][0]=w/2, out[1][1]=-h/2, out[2][2]=zrange,
	//   out[3][0]=x+w/2, out[3][1]=nv2a_top+h/2, out[3][2]=zmin, out[3][3]=1
	memset(out, 0, 16 * sizeof(float));
	out[0]  = phys_w / 2.0f;           // [0][0] = half-width
	out[5]  = phys_h / -2.0f;          // [1][1] = negative half-height (Y flip)
	// Z mapping: apply glDepthRange. Maps NDC z [-1,1] to depth buffer
	// [near*65536, far*65536]. Polymost uses this to give sprites a slight
	// depth advantage over coplanar walls (wall range [0.00001,1], sprite [0,0.99999]).
	float dr_n = depth_range_near;
	float dr_f = depth_range_far;
	out[10] = (dr_f - dr_n) * 0.5f * 65536.0f;  // [2][2] = z_scale
	out[12] = phys_x + phys_w / 2.0f;  // [3][0] = x center
	out[13] = nv2a_top + phys_h / 2.0f;  // [3][1] = y center (NV2A coords)
	out[14] = (dr_f + dr_n) * 0.5f * 65536.0f;  // [3][2] = z_offset
	out[15] = 1.0f;                     // [3][3]
}

// Helper: set a vertex attribute pointer on NV2A
static void xbox_set_attrib_pointer(unsigned int index, unsigned int size,
	unsigned int stride, const void *data)
{
	uint32_t *p = pb_begin();
	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE,
		     NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F)
		| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size)
		| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4,
		(uint32_t)(uintptr_t)data & 0x03ffffff);
	pb_end(p);
}

// Helper: disable a vertex attribute
static void xbox_clear_attrib(unsigned int index)
{
	uint32_t *p = pb_begin();
	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE,
		     NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F)
		| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 0)
		| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, 0));
	pb_end(p);
}

// NV2A texture format for uncompressed ARGB8 (LU_IMAGE) — linear, NPOT.
// CONTEXT_DMA=2, BORDER_SOURCE=COLOR, DIMENSIONALITY=2D,
// COLOR=LU_IMAGE_A8R8G8B8 (0x12), MIPMAP_LEVELS=1, BASE_SIZE_U=0, BASE_SIZE_V=0
// BASE_SIZE fields are 0 because NPOT_SIZE/NPOT_PITCH handle dimensions.
static uint32_t xbox_tex_format_argb8(void)
{
	return 0x0001122a;
}

// NV2A texture format for uncompressed ARGB8 (SZ) — swizzled, POT only.
// Swizzled textures support REPEAT wrapping (linear textures do NOT).
// Register layout (from nv_regs.h):
//   [1:0]   CONTEXT_DMA = 2
//   [2]     CUBEMAP_ENABLE = 0
//   [3]     BORDER_SOURCE = 1 (COLOR)
//   [7:4]   DIMENSIONALITY = 2 (2D)
//   [15:8]  COLOR = SZ_A8R8G8B8 (0x06)
//   [19:16] MIPMAP_LEVELS = 1
//   [23:20] BASE_SIZE_U = log2(width)
//   [27:24] BASE_SIZE_V = log2(height)
//   [31:28] BASE_SIZE_P = 0
// Compare LU_IMAGE: 0x0001122a = DMA2 | BORDER_COLOR | DIM_2D | COLOR_0x12 | MIP1
static uint32_t xbox_tex_format_argb8_swizzled(int w, int h)
{
	int log2w = 0, log2h = 0;
	{ int v = w; while (v > 1) { v >>= 1; log2w++; } }
	{ int v = h; while (v > 1) { v >>= 1; log2h++; } }
	// Base = 0x0001062a: same as LU (0x0001122a) but COLOR=0x06 instead of 0x12
	return 0x0001062a
	     | (log2w << 20)    // BASE_SIZE_U
	     | (log2h << 24);   // BASE_SIZE_V
}

// Helper: build NV2A texture wrap value from GL wrap modes
static uint32_t xbox_tex_wrap(int wrap_s, int wrap_t)
{
	// NV2A wrap: 1=REPEAT, 2=MIRRORED_REPEAT, 3=CLAMP_TO_EDGE, 4=CLAMP_TO_BORDER
	uint32_t nv_s = 1, nv_t = 1; // default REPEAT
	if (wrap_s == GL_CLAMP_TO_EDGE || wrap_s == GL_CLAMP) nv_s = 3;
	if (wrap_t == GL_CLAMP_TO_EDGE || wrap_t == GL_CLAMP) nv_t = 3;
	// Wrap register format: bits [3:0]=wrap_S, bits [11:8]=wrap_T, bits [19:16]=wrap_R
	return nv_s | (nv_t << 8) | (1 << 16); // R=REPEAT
}

// Helper: build NV2A texture filter value
static uint32_t xbox_tex_filter(int min_filter, int mag_filter)
{
	// NV097_SET_TEXTURE_FILTER layout:
	//   MAG: bits [27:24] (mask 0x0F000000)  — 1=NEAREST, 2=LINEAR
	//   MIN: bits [23:16] (mask 0x00FF0000)  — 1=NEAREST, 2=LINEAR
	//   LOD_BIAS: bits [12:0]
	// 0x2000 base matches mesh sample's disabled-stage value
	uint32_t nv_min = 1, nv_mag = 1;  // NEAREST
	if (min_filter == GL_LINEAR || min_filter == GL_LINEAR_MIPMAP_NEAREST ||
	    min_filter == GL_LINEAR_MIPMAP_LINEAR) nv_min = 2;
	if (mag_filter == GL_LINEAR) nv_mag = 2;
	return (nv_mag << 24) | (nv_min << 16) | 0x2000;
}

// ---- CPU-side frustum clipping ----
// NV2A in PROGRAM mode has NO hardware frustum clipping. The rasterizer also
// has a limited guard band (~2048 pixels from viewport center). Vertices outside
// the guard band cause stretched triangles across the entire screen. We clip
// against 5 frustum planes on CPU before submitting geometry to the GPU:
//   0: near   (w >= NEAR_CLIP_W)
//   1: left   (x >= -GUARD_BAND * w)
//   2: right  (x <=  GUARD_BAND * w)
//   3: bottom (y >= -GUARD_BAND * w)
//   4: top    (y <=  GUARD_BAND * w)

#define NEAR_CLIP_W      1.0f    // minimum clip-space w for 3D perspective draws
#define NEAR_CLIP_W_2D   0.00001f // minimum clip-space w for sprite/HUD draws (must be << 1/1024)
#define GUARD_BAND_NDC 1.0f   // clip at NDC ±1 = viewport edges (matches real GL frustum)

struct clip_vert {
	float x, y, z, u, v;   // object-space position + texture coords
	float cx, cy, cw;       // clip-space coordinates (from mvp_clip)
};

#define MAX_CLIP_POLY 8  // max vertices per polygon after 5-plane clipping

// Signed distance to clip plane (positive = inside)
// near_w: minimum clip-space w threshold for near plane (plane 0)
static float clip_near_w_threshold = NEAR_CLIP_W;  // set per-draw before clipping

static inline float clip_plane_dist(const struct clip_vert *cv, int plane)
{
	switch (plane) {
		case 0: return cv->cw - clip_near_w_threshold;
		case 1: return cv->cx + GUARD_BAND_NDC * cv->cw;
		case 2: return GUARD_BAND_NDC * cv->cw - cv->cx;
		case 3: return cv->cy + GUARD_BAND_NDC * cv->cw;
		case 4: return GUARD_BAND_NDC * cv->cw - cv->cy;
		default: return 1.0f;
	}
}

// Interpolate between two clip vertices
static inline void clip_vert_lerp(struct clip_vert *out,
	const struct clip_vert *a, const struct clip_vert *b, float t)
{
	out->x  = a->x  + t * (b->x  - a->x);
	out->y  = a->y  + t * (b->y  - a->y);
	out->z  = a->z  + t * (b->z  - a->z);
	out->u  = a->u  + t * (b->u  - a->u);
	out->v  = a->v  + t * (b->v  - a->v);
	out->cx = a->cx + t * (b->cx - a->cx);
	out->cy = a->cy + t * (b->cy - a->cy);
	out->cw = a->cw + t * (b->cw - a->cw);
}

// Sutherland-Hodgman: clip convex polygon against one plane.
static int clip_poly_plane(const struct clip_vert *in, int n_in,
                           struct clip_vert *out, int plane)
{
	if (n_in < 3) return 0;
	int n_out = 0;
	for (int i = 0; i < n_in; i++) {
		int j = (i + 1 < n_in) ? i + 1 : 0;
		float di = clip_plane_dist(&in[i], plane);
		float dj = clip_plane_dist(&in[j], plane);
		if (di >= 0)
			out[n_out++] = in[i];
		if ((di >= 0) != (dj >= 0)) {
			float t = di / (di - dj);
			clip_vert_lerp(&out[n_out], &in[i], &in[j], t);
			n_out++;
		}
	}
	return n_out;
}

// Clip a triangle against all 5 frustum planes, output triangulated result.
// clip_near_w_threshold must be set before calling (NEAR_CLIP_W or NEAR_CLIP_W_2D).
// Returns number of output vertices (multiple of 3, or 0).
static int clip_triangle_frustum(const struct clip_vert tri[3], float *out)
{
	struct clip_vert pa[MAX_CLIP_POLY], pb[MAX_CLIP_POLY];
	int n;

	memcpy(pa, tri, 3 * sizeof(struct clip_vert));
	n = 3;

	n = clip_poly_plane(pa, n, pb, 0); if (n < 3) return 0; // near
	n = clip_poly_plane(pb, n, pa, 1); if (n < 3) return 0; // left
	n = clip_poly_plane(pa, n, pb, 2); if (n < 3) return 0; // right
	n = clip_poly_plane(pb, n, pa, 3); if (n < 3) return 0; // bottom
	n = clip_poly_plane(pa, n, pb, 4); if (n < 3) return 0; // top

	// Triangulate output polygon (fan from vertex 0)
	int nout = 0;
	for (int i = 1; i < n - 1; i++) {
		float *o;
		o = out + nout*5; o[0]=pb[0].x; o[1]=pb[0].y; o[2]=pb[0].z; o[3]=pb[0].u; o[4]=pb[0].v; nout++;
		o = out + nout*5; o[0]=pb[i].x; o[1]=pb[i].y; o[2]=pb[i].z; o[3]=pb[i].u; o[4]=pb[i].v; nout++;
		o = out + nout*5; o[0]=pb[i+1].x; o[1]=pb[i+1].y; o[2]=pb[i+1].z; o[3]=pb[i+1].u; o[4]=pb[i+1].v; nout++;
	}
	return nout;
}

static void APIENTRY xbox_glDrawElements(GLenum mode, GLsizei count,
	GLenum type, const void *indices_offset)
{
	if (!xbox_pbkit_initialized || count <= 0) return;

	// Periodic GPU sync + push buffer reset: pbkit's push buffer is 512KB.
	// Each draw emits ~300 bytes across multiple pb_begin/pb_end blocks.
	// pb_busy() only waits for GPU to catch up but does NOT reclaim buffer
	// space — pb_Put keeps advancing linearly. pb_reset() both waits AND
	// resets pb_Put back to pb_Head, giving us a fresh 512KB.
	// Theoretical limit ~1700 draws; threshold provides safety margin.
	if (++draw_since_sync >= 1500) {
		/* mid-frame pb_reset logging disabled — normal at high draw counts */
		pb_reset();
		draw_since_sync = 0;
	}

	GLuint vbo_id = gl_state.bound_vbo;
	GLuint ibo_id = gl_state.bound_ibo;
	if (vbo_id == 0 || vbo_id >= MAX_BUFFERS) {
		frame_skip_count++;
		return;
	}
	if (ibo_id == 0 || ibo_id >= MAX_BUFFERS) {
		frame_skip_count++;
		return;
	}

	struct xbox_buffer *vbo = &buffer_table[vbo_id];
	struct xbox_buffer *ibo = &buffer_table[ibo_id];

	if (!vbo->gpu_addr || !ibo->cpu_data) {
		frame_skip_count++;
		return;
	}

	// ---- 1. Compute MVP and check for degenerate projection ----
	// Cgc allocated: c[0]-c[3] = u_mvp (4x4), c[4] = u_colour (vec4)
	// NV2A in PROGRAM mode: shader must output screen-space coords.
	// We bake MVP × viewport on CPU, then the shader does perspective divide.
	// NV2A PROGRAM mode does NOT apply hardware viewport. The vertex shader
	// must output screen-space coords. Bake viewport into MVP (matching mesh sample).
	float mvp_clip[16], mvp[16];
	mat4_multiply(mvp_clip, gl_uniforms.modelview, gl_uniforms.projection);

	if (vp_valid) {
		float vp_mat[16];
		build_viewport_matrix(vp_mat);
		mat4_multiply(mvp, mvp_clip, vp_mat);
	} else {
		memcpy(mvp, mvp_clip, sizeof(mvp));
	}

	// Note: mvp[15]==0 is normal for gdrawroomsprojmat and grotatespriteprojmat.
	// The shader's w-clamp (max(pos.w, 0.001)) prevents divide-by-zero for
	// degenerate vertices. Do NOT add 1 to mvp[15] — that would shift ALL
	// vertex w values by 1, breaking grotatespriteprojmat (2D sprites rendered
	// at half size in top-left corner) and slightly perturbing 3D perspective.
	// NOTE: glDepthRange near/far is baked into the viewport matrix z-transform.

	// ---- CPU-side frustum clipping ----
	// NV2A PROGRAM mode has no hardware frustum clipping, and the rasterizer's
	// guard band is limited (~2048px). Vertices outside the guard band cause
	// stretched triangles. We clip ALL draws against 5 frustum planes (near + 4 sides).
	// For 3D perspective draws, near-clip at w >= 1.0.
	// For sprite/HUD draws, near-clip at w >= 0.001 (catches behind-camera vertices
	// that would invert clip-plane equations when w < 0).
	int need_clip = 0;
	float w_col_sq = mvp_clip[3]*mvp_clip[3] + mvp_clip[7]*mvp_clip[7] + mvp_clip[11]*mvp_clip[11];
	int is_3d_perspective = (w_col_sq > 100.0f);
	float near_w = is_3d_perspective ? NEAR_CLIP_W : NEAR_CLIP_W_2D;
	{
		const GLushort *idx = (const GLushort *)((char *)ibo->cpu_data + (intptr_t)indices_offset);
		int pos_off = attrib_state[XATTR_VERTEX].offset;
		int pos_stride = attrib_state[XATTR_VERTEX].stride;
		// Read vertex data from cpu_data (system memory) if available,
		// otherwise fall back to gpu_addr (write-combined memory).
		const char *vbo_read = (const char *)(vbo->cpu_data ? vbo->cpu_data : vbo->gpu_addr);
		int all_out[5] = {1, 1, 1, 1, 1}; // per-plane: all vertices outside?
		int any_out = 0;
		for (int i = 0; i < count; i++) {
			int vi = idx[i];
			const float *v = (const float *)(vbo_read + pos_off + vi * pos_stride);
			float cw = v[0]*mvp_clip[3] + v[1]*mvp_clip[7] + v[2]*mvp_clip[11] + mvp_clip[15];
			float cx = v[0]*mvp_clip[0] + v[1]*mvp_clip[4] + v[2]*mvp_clip[8] + mvp_clip[12];
			float cy = v[0]*mvp_clip[1] + v[1]*mvp_clip[5] + v[2]*mvp_clip[9] + mvp_clip[13];
			float d[5];
			d[0] = cw - near_w;
			d[1] = cx + GUARD_BAND_NDC * cw;
			d[2] = GUARD_BAND_NDC * cw - cx;
			d[3] = cy + GUARD_BAND_NDC * cw;
			d[4] = GUARD_BAND_NDC * cw - cy;
			for (int p = 0; p < 5; p++) {
				if (d[p] >= 0) all_out[p] = 0;
				else any_out = 1;
			}
		}
		// All vertices outside any single plane → fully invisible
		for (int p = 0; p < 5; p++) {
			if (all_out[p]) {
				frame_skip_count++;
	#ifdef XBOX_DRAW_LOGGING
				if (!is_3d_perspective && global_frame_num < 60) {
					xbox_log("Xbox: F%d 2D CULLED plane=%d cnt=%d tex=%d wcolsq=%d/1000\n",
						global_frame_num, p, count, gl_state.bound_texture[0],
						(int)(w_col_sq*1000));
				}
#endif
				return;
			}
		}
		if (any_out) { need_clip = 1; frame_clip_count++; }
	}

	frame_draw_count++;
	if (!is_3d_perspective) frame_2d_count++;
	if (!gl_state.depth_test_enabled) frame_depthoff_count++;

#ifdef XBOX_DRAW_LOGGING
	if (!is_3d_perspective && global_frame_num < 60 && frame_2d_count <= 10) {
		const GLushort *sp_idx = (const GLushort *)((char *)ibo->cpu_data + (intptr_t)indices_offset);
		GLuint tid = gl_state.bound_texture[0];
		xbox_log("Xbox: F%d 2D#%d cnt=%d tex=%d depth=%d blend=%d needclip=%d mode=%x\n",
			global_frame_num, frame_2d_count, count, tid,
			gl_state.depth_test_enabled, gl_state.blend_enabled, need_clip, mode);
		xbox_log("  col=%d,%d,%d,%d/1000 acut=%d/1000 wcolsq=%d/1000\n",
			(int)(gl_uniforms.colour[0]*1000), (int)(gl_uniforms.colour[1]*1000),
			(int)(gl_uniforms.colour[2]*1000), (int)(gl_uniforms.colour[3]*1000),
			(int)(gl_uniforms.alphacut*1000), (int)(w_col_sq*1000));
		if (tid > 0 && tid < MAX_TEXTURES && texture_table[tid].allocated) {
			struct xbox_texture *t = &texture_table[tid];
			xbox_log("  tex: %dx%d alloc=%dx%d sc=%d,%d/1000 addr=%p\n",
				t->width, t->height, t->alloc_w, t->alloc_h,
				(int)(t->u_scale*1000), (int)(t->v_scale*1000), t->addr);
		} else {
			xbox_log("  tex: NOT FOUND id=%d\n", tid);
		}
		{
			int pos_off = attrib_state[XATTR_VERTEX].offset;
			int pos_stride = attrib_state[XATTR_VERTEX].stride;
			const char *rd = (const char *)(vbo->cpu_data ? vbo->cpu_data : vbo->gpu_addr);
			int nlog = count < 4 ? count : 4;
			for (int j = 0; j < nlog; j++) {
				int vi = sp_idx[j];
				const float *vp = (const float *)(rd + pos_off + vi * pos_stride);
				float cw = vp[0]*mvp_clip[3] + vp[1]*mvp_clip[7] + vp[2]*mvp_clip[11] + mvp_clip[15];
				float cx = vp[0]*mvp_clip[0] + vp[1]*mvp_clip[4] + vp[2]*mvp_clip[8] + mvp_clip[12];
				float cy = vp[0]*mvp_clip[1] + vp[1]*mvp_clip[5] + vp[2]*mvp_clip[9] + mvp_clip[13];
				xbox_log("  v[%d] obj=(%d,%d,%d)/1e6 clip=(%d,%d,w=%d)/1e6\n",
					vi, (int)(vp[0]*1e6), (int)(vp[1]*1e6), (int)(vp[2]*1e6),
					(int)(cx*1e6), (int)(cy*1e6), (int)(cw*1e6));
			}
		}
		xbox_log("  vp=(%d,%d,%d,%d) dr=(%d,%d)/1e6\n",
			(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h,
			(int)(depth_range_near*1e6), (int)(depth_range_far*1e6));
	}
#endif

#ifdef XBOX_DRAW_LOGGING
	if (global_frame_num < 10 && frame_draw_count <= 5) {
		const GLushort *log_idx = (const GLushort *)((char *)ibo->cpu_data + (intptr_t)indices_offset);
		int n = count < 4 ? count : 4;
		xbox_log("Xbox: F%d D%d mode=%x cnt=%d tex=%d vbo=%p\n",
			global_frame_num, frame_draw_count, mode, count,
			gl_state.bound_texture[0], vbo->gpu_addr);
		xbox_log("  colour=%d,%d,%d,%d/1000 acut=%d depth=%d blend=%d cull=%d\n",
			(int)(gl_uniforms.colour[0]*1000), (int)(gl_uniforms.colour[1]*1000),
			(int)(gl_uniforms.colour[2]*1000), (int)(gl_uniforms.colour[3]*1000),
			(int)(gl_uniforms.alphacut*1000),
			gl_state.depth_test_enabled, gl_state.blend_enabled, gl_state.cull_enabled);
		{
			GLuint tid = gl_state.bound_texture[0];
			if (tid > 0 && tid < MAX_TEXTURES && texture_table[tid].allocated) {
				struct xbox_texture *t = &texture_table[tid];
				xbox_log("  tex: %dx%d alloc=%dx%d SZ phys=%x\n",
					t->width, t->height, t->alloc_w, t->alloc_h,
					(unsigned)((uintptr_t)t->addr & 0x03ffffff));
			}
		}
		xbox_log("  idx=");
		for (int j = 0; j < n; j++) xbox_log("%d ", (int)log_idx[j]);
		xbox_log("\n");
		if (attrib_state[XATTR_TEXCOORD].enabled) {
			int tc_off = attrib_state[XATTR_TEXCOORD].offset;
			int tc_stride = attrib_state[XATTR_TEXCOORD].stride;
			xbox_log("  tc: off=%d stride=%d size=%d\n",
				tc_off, tc_stride, attrib_state[XATTR_TEXCOORD].size);
			for (int j = 0; j < n && j < 4; j++) {
				int vi = log_idx[j];
				uint32_t *raw = (uint32_t *)((char *)vbo->gpu_addr + tc_off + vi * tc_stride);
				xbox_log("  uv[%d] hex=%08x %08x\n", vi, raw[0], raw[1]);
			}
		}
		{
			int pos_off = attrib_state[XATTR_VERTEX].offset;
			int pos_stride = attrib_state[XATTR_VERTEX].stride;
			for (int j = 0; j < n && j < 2; j++) {
				int vi = log_idx[j];
				const float *vp = (const float *)((char *)vbo->gpu_addr + pos_off + vi * pos_stride);
				xbox_log("  v[%d] pos=(%d,%d,%d)/1000\n", vi,
					(int)(vp[0]*1000), (int)(vp[1]*1000), (int)(vp[2]*1000));
			}
		}
		xbox_log("  is3d=%d needclip=%d nearw=%d/1000 mvp15=%d/1000\n",
			is_3d_perspective, need_clip, (int)(near_w*1000), (int)(mvp[15]*1000));
		xbox_log("  mvp diag=%d,%d,%d,%d/1000\n",
			(int)(mvp[0]*1000), (int)(mvp[5]*1000),
			(int)(mvp[10]*1000), (int)(mvp[15]*1000));
		if (global_frame_num < 2 && frame_draw_count <= 2) {
			xbox_log("  mvp row0=(%d,%d,%d,%d)/1000\n",
				(int)(mvp[0]*1000),(int)(mvp[1]*1000),(int)(mvp[2]*1000),(int)(mvp[3]*1000));
			xbox_log("      row1=(%d,%d,%d,%d)/1000\n",
				(int)(mvp[4]*1000),(int)(mvp[5]*1000),(int)(mvp[6]*1000),(int)(mvp[7]*1000));
			xbox_log("      row2=(%d,%d,%d,%d)/1000\n",
				(int)(mvp[8]*1000),(int)(mvp[9]*1000),(int)(mvp[10]*1000),(int)(mvp[11]*1000));
			xbox_log("      row3=(%d,%d,%d,%d)/1000\n",
				(int)(mvp[12]*1000),(int)(mvp[13]*1000),(int)(mvp[14]*1000),(int)(mvp[15]*1000));
		}
	}
#endif

	// ---- DIAGNOSTIC: Full GPU sync before each draw ----
	// Set to 1 to force the GPU to finish all pending work before each draw.
	// If this fixes streaking, the root cause is GPU pipelining: the previous
	// draw's vertex fetch reads from attribute pointers that get overwritten
	// by the current draw's state setup before the fetch completes.
	#define SYNC_EVERY_DRAW 0
	#if SYNC_EVERY_DRAW
	while (pb_busy()) { /* spin */ }
	#endif

	// ---- 1+2. Per-draw state with dirty-bit tracking ----
	// Write all NV2A state for this draw call.
	{
		GLuint tex_id = gl_state.bound_texture[0];
		struct xbox_texture *tex = NULL;
		if (tex_id > 0 && tex_id < MAX_TEXTURES && texture_table[tex_id].allocated) {
			tex = &texture_table[tex_id];
		}

		float cur_texscale[2] = { 1.0f, 1.0f };
		if (tex && tex->addr) {
			cur_texscale[0] = tex->u_scale;
			cur_texscale[1] = tex->v_scale;
		}

#ifdef XBOX_DRAW_LOGGING
		if (global_frame_num >= 1 && global_frame_num <= 2) {
			xbox_log("D%d m=%x c=%d t=%d(%dx%d>%dx%d) 3d=%d cl=%d d=%d b=%d\n",
				frame_draw_count, mode, count, tex_id,
				tex ? tex->width : 0, tex ? tex->height : 0,
				tex ? tex->alloc_w : 0, tex ? tex->alloc_h : 0,
				is_3d_perspective, need_clip,
				gl_state.depth_test_enabled, gl_state.blend_enabled);
		}
#endif

		uint32_t *p = pb_begin();

		// -- Shader constants c[0]-c[6] (dirty-tracked) --
		// Only re-upload when MVP, colour, or texscale actually changed.
		{
			int need_constants = !gpu_cache.constants_valid
				|| memcmp(gpu_cache.mvp, mvp, 16 * 4) != 0
				|| memcmp(gpu_cache.colour, gl_uniforms.colour, 4 * 4) != 0
				|| gpu_cache.texscale[0] != cur_texscale[0]
				|| gpu_cache.texscale[1] != cur_texscale[1];
			if (need_constants) {
				p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID, 96);
				pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 28);
				memcpy(p, mvp, 16 * 4); p += 16;              // c[0]-c[3]: MVP
				memcpy(p, gl_uniforms.colour, 4 * 4); p += 4;  // c[4]: colour
				{                                                // c[5]: wclamp
					static const float c5[4] = { 0.0001f, 1.0f, 0.0f, 0.0f };
					memcpy(p, c5, 4 * 4); p += 4;
				}
				{                                                // c[6]: texscale
					float c6[4] = { cur_texscale[0], cur_texscale[1], 0.0f, 0.0f };
					memcpy(p, c6, 4 * 4); p += 4;
				}
				memcpy(gpu_cache.mvp, mvp, 16 * 4);
				memcpy(gpu_cache.colour, gl_uniforms.colour, 4 * 4);
				gpu_cache.texscale[0] = cur_texscale[0];
				gpu_cache.texscale[1] = cur_texscale[1];
				gpu_cache.constants_valid = 1;
			} else {
				frame_state_skip_count++;
			}
		}

		// -- Texture stage 0 (dirty-tracked) --
		{
			uintptr_t new_addr = 0;
			int new_swizzled = 0;
			int new_aw = 0, new_ah = 0, new_w = 0, new_h = 0;
			uint32_t new_pitch = 0, new_filter = 0, new_wrap = 0;

			if (tex && tex->addr) {
				new_addr = (uintptr_t)tex->addr;
				new_swizzled = tex->swizzled;
				new_aw = tex->alloc_w; new_ah = tex->alloc_h;
				new_w = tex->width; new_h = tex->height;
				new_pitch = tex->pitch;
				new_filter = xbox_tex_filter(tex->min_filter, tex->mag_filter);
				new_wrap = tex->swizzled ? xbox_tex_wrap(tex->wrap_s, tex->wrap_t) : 0x00030303;
			} else if (null_texture_addr) {
				new_addr = (uintptr_t)null_texture_addr;
				new_swizzled = 1;
				new_aw = NULL_TEX_SIZE; new_ah = NULL_TEX_SIZE;
				new_w = NULL_TEX_SIZE; new_h = NULL_TEX_SIZE;
				new_pitch = NULL_TEX_PITCH;
				new_filter = 0x04074000;
				new_wrap = 0x00030303;
			}

			int need_tex = !gpu_cache.tex_valid
				|| gpu_cache.tex_addr != new_addr
				|| gpu_cache.tex_swizzled != new_swizzled
				|| gpu_cache.tex_alloc_w != new_aw
				|| gpu_cache.tex_alloc_h != new_ah
				|| gpu_cache.tex_filter != new_filter
				|| gpu_cache.tex_wrap != new_wrap;

			if (need_tex && new_addr) {
				if (new_swizzled) {
					p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
						(DWORD)new_addr & 0x03ffffff,
						xbox_tex_format_argb8_swizzled(new_aw, new_ah));
					p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0),
						new_pitch << 16);
					p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0),
						(new_aw << 16) | new_ah);
					p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), new_wrap);
				} else {
					p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
						(DWORD)new_addr & 0x03ffffff,
						xbox_tex_format_argb8());
					p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0),
						new_pitch << 16);
					p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0),
						(new_w << 16) | new_h);
					p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), new_wrap);
				}
				p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
				p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), new_filter);

				gpu_cache.tex_addr = new_addr;
				gpu_cache.tex_swizzled = new_swizzled;
				gpu_cache.tex_alloc_w = new_aw;
				gpu_cache.tex_alloc_h = new_ah;
				gpu_cache.tex_width = new_w;
				gpu_cache.tex_height = new_h;
				gpu_cache.tex_pitch = new_pitch;
				gpu_cache.tex_filter = new_filter;
				gpu_cache.tex_wrap = new_wrap;
				gpu_cache.tex_valid = 1;
			} else if (need_tex) {
				// Texture became NULL — invalidate cache so next textured draw re-sets it
				gpu_cache.tex_valid = 0;
			} else {
				frame_state_skip_count++;
			}
		}

		// -- Alpha test (dirty-tracked) --
		{
			int new_alpha_en = (gl_uniforms.alphacut > 0.0f) ? 1 : 0;
			int new_alpha_ref = 0;
			if (new_alpha_en) {
				new_alpha_ref = (int)(gl_uniforms.alphacut * 255.0f);
				if (new_alpha_ref > 255) new_alpha_ref = 255;
				if (new_alpha_ref < 0) new_alpha_ref = 0;
			}
			int need_alpha = !gpu_cache.alpha_valid
				|| gpu_cache.alpha_test_enabled != new_alpha_en
				|| (new_alpha_en && gpu_cache.alpha_ref != new_alpha_ref);
			if (need_alpha) {
				if (new_alpha_en) {
					p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 1);
					p = pb_push1(p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER);
					p = pb_push1(p, NV097_SET_ALPHA_REF, new_alpha_ref);
				} else {
					p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
				}
				gpu_cache.alpha_test_enabled = new_alpha_en;
				gpu_cache.alpha_ref = new_alpha_ref;
				gpu_cache.alpha_valid = 1;
			} else {
				frame_state_skip_count++;
			}
		}

		// -- Render state: blend, depth, cull (dirty-tracked) --
		{
			uint32_t nv_front = (gl_state.front_face == GL_CW)
				? NV097_SET_FRONT_FACE_V_CW : NV097_SET_FRONT_FACE_V_CCW;
			int dm = gl_state.depth_mask ? 1 : 0;
			int need_rs = !gpu_cache.renderstate_valid
				|| gpu_cache.blend_enabled != gl_state.blend_enabled
				|| (gl_state.blend_enabled && (gpu_cache.blend_sfactor != (uint32_t)gl_state.blend_sfactor
					|| gpu_cache.blend_dfactor != (uint32_t)gl_state.blend_dfactor))
				|| gpu_cache.depth_test_enabled != gl_state.depth_test_enabled
				|| (gl_state.depth_test_enabled && gpu_cache.depth_func != (uint32_t)gl_state.depth_func)
				|| gpu_cache.depth_mask != dm
				|| gpu_cache.cull_enabled != gl_state.cull_enabled
				|| (gl_state.cull_enabled && (gpu_cache.front_face != nv_front
					|| gpu_cache.cull_face != (uint32_t)gl_state.cull_face));
			if (need_rs) {
				p = pb_push1(p, NV097_SET_BLEND_ENABLE, gl_state.blend_enabled);
				if (gl_state.blend_enabled) {
					p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, (uint32_t)gl_state.blend_sfactor);
					p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, (uint32_t)gl_state.blend_dfactor);
				}
				p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, gl_state.depth_test_enabled);
				if (gl_state.depth_test_enabled) {
					p = pb_push1(p, NV097_SET_DEPTH_FUNC, (uint32_t)gl_state.depth_func);
				}
				p = pb_push1(p, NV097_SET_DEPTH_MASK, dm);
				p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, gl_state.cull_enabled);
				if (gl_state.cull_enabled) {
					p = pb_push1(p, NV097_SET_FRONT_FACE, nv_front);
					p = pb_push1(p, NV097_SET_CULL_FACE, (uint32_t)gl_state.cull_face);
				}
				gpu_cache.blend_enabled = gl_state.blend_enabled;
				gpu_cache.blend_sfactor = (uint32_t)gl_state.blend_sfactor;
				gpu_cache.blend_dfactor = (uint32_t)gl_state.blend_dfactor;
				gpu_cache.depth_test_enabled = gl_state.depth_test_enabled;
				gpu_cache.depth_func = (uint32_t)gl_state.depth_func;
				gpu_cache.depth_mask = dm;
				gpu_cache.cull_enabled = gl_state.cull_enabled;
				gpu_cache.front_face = nv_front;
				gpu_cache.cull_face = (uint32_t)gl_state.cull_face;
				gpu_cache.renderstate_valid = 1;
			} else {
				frame_state_skip_count++;
			}
		}

		// -- Vertex attribute pointers (always written — VBO offset changes every draw) --
		{
			void *vbo_base = vbo->gpu_addr;
			{
				unsigned int idx = XATTR_VERTEX;
				p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + idx * 4,
					MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE,
					     NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F)
					| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, attrib_state[idx].size)
					| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, attrib_state[idx].stride));
				p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + idx * 4,
					(uint32_t)(uintptr_t)((char *)vbo_base + attrib_state[idx].offset) & 0x03ffffff);
			}
			{
				unsigned int idx = XATTR_TEXCOORD;
				p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + idx * 4,
					MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE,
					     NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F)
					| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, attrib_state[idx].size)
					| MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, attrib_state[idx].stride));
				p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + idx * 4,
					(uint32_t)(uintptr_t)((char *)vbo_base + attrib_state[idx].offset) & 0x03ffffff);
			}
		}

		// -- Invalidate the NV2A vertex buffer cache (always required) --
		p = pb_push1(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);

		pb_end(p);
	}

	// ---- 3. Submit geometry ----
	{
		const GLushort *src_indices = (const GLushort *)((char *)ibo->cpu_data + (intptr_t)indices_offset);

		// DEBUG: set to 1 to skip all clipped draws (diagnostic for streaking)
		#define SKIP_CLIPPED_DRAWS 0

		if (need_clip && mode != GL_LINES && mode != GL_POINTS) {
			#if SKIP_CLIPPED_DRAWS
			return; // diagnostic: skip clipped draws to test if streaks come from clipper
			#endif
			// Frustum clipping path: clip each triangle against all 5 planes,
			// write clipped vertices to VBO pool, draw as GL_TRIANGLES.
			int pos_off = attrib_state[XATTR_VERTEX].offset;
			int pos_stride = attrib_state[XATTR_VERTEX].stride;
			int tc_off = attrib_state[XATTR_TEXCOORD].offset;
			int tc_stride = attrib_state[XATTR_TEXCOORD].stride;
			// Read from system memory shadow (avoids stale WC memory reads)
			const char *clip_vbo_read = (const char *)(vbo->cpu_data ? vbo->cpu_data : vbo->gpu_addr);

			// Set near-clip threshold for this draw
			clip_near_w_threshold = near_w;

			// Max output: each triangle can produce up to 18 verts (6 triangles)
			float clipped[512 * 5];
			int clipped_count = 0;
			int max_verts = 512;

			// Compute triangle count based on primitive mode
			int num_tris;
			if (mode == GL_TRIANGLES)
				num_tris = count / 3;
			else
				num_tris = count - 2;  // fan or strip

			// Decompose fan/strip into individual triangles, clip each
			for (int t = 0; t < num_tris && clipped_count + 18 <= max_verts; t++) {
				int i0, i1, i2;
				if (mode == GL_TRIANGLE_FAN) {
					i0 = 0; i1 = t + 1; i2 = t + 2;
				} else if (mode == GL_TRIANGLE_STRIP) {
					if (t & 1) { i0 = t + 1; i1 = t; i2 = t + 2; }
					else { i0 = t; i1 = t + 1; i2 = t + 2; }
				} else { // GL_TRIANGLES
					i0 = t * 3; i1 = t * 3 + 1; i2 = t * 3 + 2;
				}

				struct clip_vert tri[3];
				int vidx[3] = { src_indices[i0], src_indices[i1], src_indices[i2] };
				for (int k = 0; k < 3; k++) {
					const float *pos = (const float *)(clip_vbo_read + pos_off + vidx[k] * pos_stride);
					tri[k].x = pos[0]; tri[k].y = pos[1]; tri[k].z = pos[2];
					{
						const float *uv = (const float *)(clip_vbo_read + tc_off + vidx[k] * tc_stride);
						tri[k].u = uv[0]; tri[k].v = uv[1];
					}
					tri[k].cx = pos[0]*mvp_clip[0] + pos[1]*mvp_clip[4] + pos[2]*mvp_clip[8] + mvp_clip[12];
					tri[k].cy = pos[0]*mvp_clip[1] + pos[1]*mvp_clip[5] + pos[2]*mvp_clip[9] + mvp_clip[13];
					tri[k].cw = pos[0]*mvp_clip[3] + pos[1]*mvp_clip[7] + pos[2]*mvp_clip[11] + mvp_clip[15];
				}

				int n = clip_triangle_frustum(tri, clipped + clipped_count * 5);
				clipped_count += n;
			}

			if (clipped_count < 3) { return; }

			// Allocate clipped vertices in VBO streaming pool
			int vert_bytes = clipped_count * 20; // 5 floats * 4 bytes
			int aligned_off = (vbo_pool_offset + 15) & ~15;
			if (aligned_off + vert_bytes > VBO_POOL_SIZE) { return; }
			void *clip_vbo = (char *)vbo_pool + aligned_off;
			memcpy(clip_vbo, clipped, vert_bytes);
			__asm__ volatile("sfence" ::: "memory");
			vbo_pool_offset = aligned_off + vert_bytes;

			// Re-point attribs to clipped vertex data (stride=20, pos@0, uv@12)
			xbox_set_attrib_pointer(XATTR_VERTEX, 3, 20, clip_vbo);
			xbox_set_attrib_pointer(XATTR_TEXCOORD, 2, 20, (char *)clip_vbo + 12);
			// Invalidate vertex buffer cache (clipped data at new address)
			{
				uint32_t *pc = pb_begin();
				pc = pb_push1(pc, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
				pb_end(pc);
			}

			// Draw clipped triangles with sequential indices
			#define MAX_BATCH 240
			for (int i = 0; i < clipped_count; ) {
				int batch = clipped_count - i;
				if (batch > MAX_BATCH) batch = MAX_BATCH;
				int packed_count = (batch + 1) / 2;

				uint32_t *p = pb_begin();
				p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
				pb_push(p++, 0x40000000 | NV097_ARRAY_ELEMENT16, packed_count);
				for (int j = 0; j < batch - 1; j += 2) {
					*p++ = (uint32_t)((i + j + 1) << 16) | (uint32_t)(i + j);
				}
				if (batch & 1) {
					uint32_t last = (uint32_t)(i + batch - 1);
					*p++ = (last << 16) | last;
				}
				p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
				pb_end(p);
				i += batch;
			}
			#undef MAX_BATCH
		} else {
			// Normal path: all vertices in front of near plane, draw as-is
			#define MAX_BATCH 240
			uint32_t nv_prim;
			switch (mode) {
				case GL_TRIANGLE_FAN:   nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLE_FAN; break;
				case GL_TRIANGLE_STRIP: nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP; break;
				case GL_TRIANGLES:      nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLES; break;
				case GL_LINES:          nv_prim = NV097_SET_BEGIN_END_OP_LINES; break;
				case GL_POINTS:         nv_prim = NV097_SET_BEGIN_END_OP_POINTS; break;
				default:                nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLES; break;
			}

			for (int i = 0; i < count; ) {
				int batch = count - i;
				if (batch > MAX_BATCH) batch = MAX_BATCH;
				int packed_count = (batch + 1) / 2;

				uint32_t *p = pb_begin();
				p = pb_push1(p, NV097_SET_BEGIN_END, nv_prim);
				pb_push(p++, 0x40000000 | NV097_ARRAY_ELEMENT16, packed_count);
				for (int j = 0; j < batch - 1; j += 2) {
					*p++ = ((uint32_t)src_indices[i + j + 1] << 16)
					     | (uint32_t)src_indices[i + j];
				}
				if (batch & 1) {
					uint32_t last_idx = (uint32_t)src_indices[i + batch - 1];
					*p++ = (last_idx << 16) | last_idx;
				}
				p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
				pb_end(p);
				i += batch;
			}
			#undef MAX_BATCH
		}
	}
}


// ====================================================================
// pbkit initialization
// ====================================================================

static void xbox_init_pbkit(void)
{
	if (xbox_pbkit_initialized) return;

	xbox_log("Xbox: calling pb_init()...\n");
	int pb_err = pb_init();
	if (pb_err) {
		xbox_log("Xbox: pb_init() FAILED with error %d\n", pb_err);
		return;
	}

	screen_width = pb_back_buffer_width();
	screen_height = pb_back_buffer_height();
	xbox_log("Xbox: pbkit initialized, framebuffer %dx%d, bb=%p\n",
		screen_width, screen_height, (void *)pb_back_buffer());

	// Show front screen (matching mesh sample — sets PCRTC scanout)
	pb_show_front_screen();

	// Allocate vertex streaming pool
	vbo_pool = MmAllocateContiguousMemoryEx(VBO_POOL_SIZE, 0, MAXRAM, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!vbo_pool) {
		xbox_log("Xbox: FAILED to allocate VBO streaming pool!\n");
	} else {
		xbox_log("Xbox: VBO pool allocated at %p\n", vbo_pool);
	}
	vbo_pool_offset = 0;

	// Create null texture (16x16 white ARGB)
	null_texture_addr = MmAllocateContiguousMemoryEx(NULL_TEX_PITCH * NULL_TEX_SIZE,
		0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (null_texture_addr) {
		memset(null_texture_addr, 0xFF, NULL_TEX_PITCH * NULL_TEX_SIZE); // all white, opaque
		__asm__ volatile("sfence" ::: "memory");
	}

	// Allocate persistent staging buffer for texture uploads (avoids per-call heap alloc)
	staging_buf = (unsigned char *)malloc(STAGING_BUF_SIZE);
	if (staging_buf) {
		xbox_log("Xbox: staging buffer allocated (%d bytes)\n", STAGING_BUF_SIZE);
	} else {
		xbox_log("Xbox: WARNING: staging buffer alloc FAILED, using per-call malloc\n");
	}

	// Target the back buffer FIRST — this sets up render surface, DMA channels,
	// depth/stencil, and buffer format. Must happen before shader setup so that
	// set_draw_buffer's state doesn't overwrite our PROGRAM mode.
	pb_target_back_buffer();

	// Load vertex shader (sets PROGRAM mode, uploads program)
	xbox_load_shaders();

	// Set up register combiners (fragment processing)
	xbox_setup_combiners();

	// Initialize gl_state with proper defaults (avoid zero-initialized invalid values)
	gl_state.depth_func = GL_LEQUAL;
	gl_state.depth_mask = GL_TRUE;
	gl_state.blend_sfactor = GL_SRC_ALPHA;
	gl_state.blend_dfactor = GL_ONE_MINUS_SRC_ALPHA;
	gl_state.front_face = GL_CCW;
	gl_state.cull_face = GL_BACK;
	gl_state.active_texture = GL_TEXTURE0;

	// Set default NV2A state (matching pb_init defaults)
	{
		uint32_t *p = pb_begin();
		p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
		p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
		p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
		p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
		p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
		p = pb_push1(p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL);
		p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
		p = pb_push1(p, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CCW);
		p = pb_push1(p, NV097_SET_CULL_FACE, NV097_SET_CULL_FACE_V_BACK);
		pb_end(p);
	}

	// Wait for GPU to finish processing all init commands
	while (pb_busy()) { /* spin */ }

	xbox_pbkit_initialized = 1;
	frame_setup_done = 1;  // init already set up the first frame
}


// ====================================================================
// glbuild interface
// ====================================================================

int glbuild_loadfunctions(void)
{
	glfunc.glClearColor   = xbox_glClearColor;
	glfunc.glClear        = xbox_glClear;
	glfunc.glColorMask    = xbox_glColorMask;
	glfunc.glBlendFunc    = xbox_glBlendFunc;
	glfunc.glCullFace     = xbox_glCullFace;
	glfunc.glFrontFace    = xbox_glFrontFace;
	glfunc.glPolygonOffset = xbox_glPolygonOffset;
	glfunc.glPolygonMode  = xbox_glPolygonMode;
	glfunc.glEnable       = xbox_glEnable;
	glfunc.glDisable      = xbox_glDisable;
	glfunc.glGetFloatv    = xbox_glGetFloatv;
	glfunc.glGetIntegerv  = xbox_glGetIntegerv;
	glfunc.glGetString    = xbox_glGetString;
	glfunc.glGetError     = xbox_glGetError;
	glfunc.glHint         = xbox_glHint;
	glfunc.glPixelStorei  = xbox_glPixelStorei;
	glfunc.glViewport     = xbox_glViewport;
	glfunc.glScissor      = xbox_glScissor;
	glfunc.glMinSampleShadingARB = xbox_glMinSampleShadingARB;

	glfunc.glDepthFunc    = xbox_glDepthFunc;
	glfunc.glDepthMask    = xbox_glDepthMask;
	glfunc.glDepthRange   = xbox_glDepthRange;

	glfunc.glReadPixels   = xbox_glReadPixels;

	glfunc.glGenTextures  = xbox_glGenTextures;
	glfunc.glDeleteTextures = xbox_glDeleteTextures;
	glfunc.glBindTexture  = xbox_glBindTexture;
	glfunc.glTexImage2D   = xbox_glTexImage2D;
	glfunc.glTexSubImage2D = xbox_glTexSubImage2D;
	glfunc.glTexParameterf = xbox_glTexParameterf;
	glfunc.glTexParameteri = xbox_glTexParameteri;
	glfunc.glCompressedTexImage2D = xbox_glCompressedTexImage2D;

	glfunc.glBindBuffer   = xbox_glBindBuffer;
	glfunc.glBufferData   = xbox_glBufferData;
	glfunc.glBufferSubData = xbox_glBufferSubData;
	glfunc.glDeleteBuffers = xbox_glDeleteBuffers;
	glfunc.glGenBuffers   = xbox_glGenBuffers;
	glfunc.glDrawElements = xbox_glDrawElements;
	glfunc.glEnableVertexAttribArray = xbox_glEnableVertexAttribArray;
	glfunc.glDisableVertexAttribArray = xbox_glDisableVertexAttribArray;
	glfunc.glVertexAttribPointer = xbox_glVertexAttribPointer;

	glfunc.glActiveTexture = xbox_glActiveTexture;
	glfunc.glAttachShader  = xbox_glAttachShader;
	glfunc.glCompileShader = xbox_glCompileShader;
	glfunc.glCreateProgram = xbox_glCreateProgram;
	glfunc.glCreateShader  = xbox_glCreateShader;
	glfunc.glDeleteProgram = xbox_glDeleteProgram;
	glfunc.glDeleteShader  = xbox_glDeleteShader;
	glfunc.glDetachShader  = xbox_glDetachShader;
	glfunc.glGetAttribLocation = xbox_glGetAttribLocation;
	glfunc.glGetProgramiv  = xbox_glGetProgramiv;
	glfunc.glGetProgramInfoLog = xbox_glGetProgramInfoLog;
	glfunc.glGetShaderiv   = xbox_glGetShaderiv;
	glfunc.glGetShaderInfoLog = xbox_glGetShaderInfoLog;
	glfunc.glGetUniformLocation = xbox_glGetUniformLocation;
	glfunc.glLinkProgram   = xbox_glLinkProgram;
	glfunc.glShaderSource  = xbox_glShaderSource;
	glfunc.glUniform1i     = xbox_glUniform1i;
	glfunc.glUniform1f     = xbox_glUniform1f;
	glfunc.glUniform2f     = xbox_glUniform2f;
	glfunc.glUniform3f     = xbox_glUniform3f;
	glfunc.glUniform4f     = xbox_glUniform4f;
	glfunc.glUniformMatrix4fv = xbox_glUniformMatrix4fv;
	glfunc.glUseProgram    = xbox_glUseProgram;

	return 0;
}

void glbuild_unloadfunctions(void)
{
	memset(&glfunc, 0, sizeof(glfunc));
}

int glbuild_init(void)
{
	if (glbuild_loadfunctions()) {
		return -1;
	}

	memset(&gl_state, 0, sizeof(gl_state));
	gl_state.active_texture = GL_TEXTURE0;
	gl_state.depth_func = GL_LEQUAL;
	gl_state.depth_mask = GL_TRUE;
	gl_state.front_face = GL_CCW;
	gl_state.cull_face = GL_BACK;

	memset(&gl_uniforms, 0, sizeof(gl_uniforms));
	gl_uniforms.colour[0] = 1.0f;
	gl_uniforms.colour[1] = 1.0f;
	gl_uniforms.colour[2] = 1.0f;
	gl_uniforms.colour[3] = 1.0f;
	gl_uniforms.gamma = 1.0f;

	memset(&glinfo, 0, sizeof(glinfo));
	glinfo.majver = 2;
	glinfo.minver = 0;
	glinfo.glslmajver = 1;
	glinfo.glslminver = 10;
	glinfo.maxtexsize = 4096;
	glinfo.multitex = 4;
	glinfo.maxvertexattribs = 16;
	glinfo.maxanisotropy = 4.0f;
	glinfo.bgra = 1;
	glinfo.clamptoedge = 1;
	glinfo.texnpot = 0;  /* NV2A requires POT for swizzled textures; let polymosttex pad */
	glinfo.texcomprdxt1 = 1;
	glinfo.texcomprdxt5 = 1;
	glinfo.loaded = 1;

	OSD_RegisterFunction("glinfo", "glinfo: shows OpenGL information about the current OpenGL mode", osdcmd_glinfo);

	return 0;
}

void glbuild_check_errors(const char *file, int line)
{
	(void)file; (void)line;
}

// Initialize pbkit for polymost rendering.
// Called from setvideomode when switching to 32-bit mode.
void xbox_pbkit_init_for_polymost(void)
{
	xbox_init_pbkit();
}

// Check if a GL texture's GPU memory is still valid (not evicted).
// Returns 1 if valid, 0 if evicted and needs re-upload.
int xbox_gl_texture_valid(unsigned int glpic)
{
	if (glpic == 0 || glpic >= MAX_TEXTURES) return 0;
	return (texture_table[glpic].allocated && texture_table[glpic].addr != NULL) ? 1 : 0;
}

int xbox_tex_over_budget(void)
{
	return (total_texture_bytes >= TEX_BUDGET_BYTES) || xbox_tex_alloc_failed;
}

/* Reset per-level texture allocation state so precache starts fresh.
 * Without this, a single alloc failure permanently poisons the budget
 * check, collapsing precache to 1 texture for all subsequent levels. */
void xbox_tex_reset_level_state(void)
{
	xbox_tex_alloc_failed = 0;
	xbox_tex_alloc_fail_count = 0;
}

int xbox_tex_get_total_bytes(void) { return total_texture_bytes; }

int xbox_tex_get_alloc_count(void)
{
	int n = 0;
	for (int i = 1; i < MAX_TEXTURES; i++)
		if (texture_table[i].allocated) n++;
	return n;
}

// Shut down pbkit and release all GPU resources.
// Called from setvideomode when switching back to 8-bit software mode.
// After this, the PCRTC scanout is restored to the original framebuffer
// so SDL rendering becomes visible again.
void xbox_pbkit_shutdown_for_software(void)
{
	if (!xbox_pbkit_initialized) return;

	xbox_log("Xbox: pbkit shutdown for software mode\n");

	// Wait for GPU to finish all pending work
	while (pb_busy()) { /* spin */ }

	// Free all allocated textures
	for (int i = 0; i < MAX_TEXTURES; i++) {
		if (texture_table[i].allocated && texture_table[i].addr) {
			MmFreeContiguousMemory(texture_table[i].addr);
		}
		memset(&texture_table[i], 0, sizeof(texture_table[i]));
	}
	total_texture_bytes = 0;
	xbox_tex_alloc_failed = 0;

	// Free buffer table entries (CPU-side copies)
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (buffer_table[i].cpu_data) {
			free(buffer_table[i].cpu_data);
		}
		memset(&buffer_table[i], 0, sizeof(buffer_table[i]));
	}

	// Free VBO pool
	if (vbo_pool) {
		MmFreeContiguousMemory(vbo_pool);
		vbo_pool = NULL;
	}
	vbo_pool_offset = 0;

	// Free null texture
	if (null_texture_addr) {
		MmFreeContiguousMemory(null_texture_addr);
		null_texture_addr = NULL;
	}

	// Free staging buffer
	if (staging_buf) {
		free(staging_buf);
		staging_buf = NULL;
	}

	// Shut down pbkit DMA engine, restore GPU registers
	pb_kill();

	// pb_kill() restores old GPU register state, but the CRT controller
	// and video encoder may not be fully functional. Re-establish the
	// video mode to ensure the display pipeline is working again.
	{
		VIDEO_MODE vm = XVideoGetMode();
		int w = vm.width > 0 ? vm.width : 640;
		int h = vm.height > 0 ? vm.height : 480;
		xbox_log("Xbox: re-establishing video mode %dx%d after pb_kill\n", w, h);
		XVideoSetMode(w, h, 32, REFRESH_DEFAULT);
	}

	xbox_pbkit_initialized = 0;
	screen_width = 0;
	screen_height = 0;

	// Reset all per-session counters so re-init works cleanly
	viewport_set_count = 0;
	clear_frame_number = 0;
	global_frame_num = 0;
	frame_setup_done = 0;
	draw_since_sync = 0;
	frame_draw_count = 0;
	frame_skip_count = 0;
	frame_clip_count = 0;
	frame_2d_count = 0;
	frame_depthoff_count = 0;
	frame_state_skip_count = 0;
	gpu_cache.constants_valid = 0;
	gpu_cache.tex_valid = 0;
	gpu_cache.alpha_valid = 0;
	gpu_cache.renderstate_valid = 0;
	xbox_next_id = 1;
	tex_free_count = 0;

	xbox_log("Xbox: pbkit shutdown complete\n");
}

// Force the CRT controller to display the back buffer we just rendered to.
// This bypasses pbkit's triple-buffering mechanism (which has timing issues).
void xbox_show_back_buffer(void)
{
	if (!xbox_pbkit_initialized) return;
	DWORD *bb = pb_back_buffer();
	volatile unsigned int *crtc = (volatile unsigned int *)(XBOX_VIDEO_BASE + PCRTC_START);
	*crtc = (DWORD)(uintptr_t)bb & 0x03FFFFFF;
}


// ====================================================================
// Shader compilation / linking stubs
// ====================================================================

GLuint glbuild_compile_shader(GLuint type, const GLchar *source)
{
	(void)source; (void)type;
	return xbox_next_id++;
}

GLuint glbuild_link_program(int shadercount, GLuint *shaders)
{
	(void)shadercount; (void)shaders;
	return xbox_next_id++;
}


// ====================================================================
// 8-bit shader path — stubs (SDL texture path handles 8-bit on Xbox)
// ====================================================================

int glbuild_prepare_8bit_shader(glbuild8bit *state, int resx, int resy, int stride, int winx, int winy)
{
	(void)stride;
	memset(state, 0, sizeof(*state));
	state->resx = resx;
	state->resy = resy;
	state->winx = winx;
	state->winy = winy;
	state->tx = 1.0f;
	state->ty = 1.0f;
	return 0;
}

void glbuild_delete_8bit_shader(glbuild8bit *state)
{
	memset(state, 0, sizeof(*state));
}

void glbuild_update_8bit_palette(glbuild8bit *state, const GLvoid *pal)
{
	(void)state; (void)pal;
}

void glbuild_set_8bit_gamma(glbuild8bit *state, GLfloat gamma)
{
	(void)state; (void)gamma;
}

void glbuild_update_8bit_frame(glbuild8bit *state, const GLvoid *frame, int stride, int resy)
{
	(void)state; (void)frame; (void)stride; (void)resy;
}

void glbuild_update_window_size(glbuild8bit *state, int winx, int winy)
{
	state->winx = winx;
	state->winy = winy;
}

void glbuild_draw_8bit_frame(glbuild8bit *state)
{
	(void)state;
}


// ====================================================================
// OSD command
// ====================================================================

static int osdcmd_glinfo(const osdfuncparm_t *parm)
{
	(void)parm;
	buildprintf(
		"OpenGL Information (Xbox NV2A pbkit):\n"
		" Version:      %s\n"
		" Vendor:       %s\n"
		" Renderer:     %s\n"
		" GLSL version: %s\n"
		" Max tex size: %d\n"
		" Multitex:     %d\n"
		" Anisotropy:   %.1f\n"
		" BGRA:         %s\n"
		" NPOT:         %s\n"
		" DXT1:         %s\n"
		" DXT5:         %s\n"
		" pbkit init:   %s\n"
		" Screen:       %dx%d\n"
		" VBO pool:     %s (%d/%d bytes used)\n",
		xbox_gl_version, xbox_gl_vendor, xbox_gl_renderer, xbox_glsl_version,
		glinfo.maxtexsize, glinfo.multitex, glinfo.maxanisotropy,
		glinfo.bgra ? "yes" : "no",
		glinfo.texnpot ? "yes" : "no",
		glinfo.texcomprdxt1 ? "yes" : "no",
		glinfo.texcomprdxt5 ? "yes" : "no",
		xbox_pbkit_initialized ? "yes" : "no",
		screen_width, screen_height,
		vbo_pool ? "allocated" : "NONE",
		vbo_pool_offset, VBO_POOL_SIZE
	);
	return OSDCMD_OK;
}


#endif  //USE_OPENGL
