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
                                            "GL_ARB_texture_non_power_of_two "
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
	int allocated;
	int wrap_s, wrap_t;
	int min_filter, mag_filter;
} texture_table[MAX_TEXTURES];

// ---- Buffer table (CPU-side + GPU-mapped) ----
#define MAX_BUFFERS 64
static struct xbox_buffer {
	void *cpu_data;    // malloc'd copy (for IBO)
	void *gpu_addr;    // pointer into VBO streaming pool (for VBO)
	int size;
	int is_vbo;        // 1 if this buffer was used as GL_ARRAY_BUFFER
} buffer_table[MAX_BUFFERS];

// ---- Vertex streaming pool (contiguous GPU memory) ----
#define VBO_POOL_SIZE (8*1024*1024)
static void *vbo_pool = NULL;
static int vbo_pool_offset = 0;

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
static int global_frame_num = 0;   // monotonic frame counter
static int draw_since_sync = 0;    // draws since last GPU sync (push buffer overflow prevention)

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

// ---- Compiled shaders (generated from .cg by nxdk toolchain) ----
// 9 instructions × 4 uint32_t per instruction = 36 words (includes perspective divide)
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

			// Upload constants to PHYSICAL SLOT 96 (= shader c[0])
			p = pb_begin();
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID, 96);
			pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 16);
			memcpy(p, viewport_mvp, 64); p += 16;
			pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 4);
			memcpy(p, green, 16); p += 4;
			pb_end(p);

			// Combiners + null texture (white 16x16)
			xbox_setup_combiners();
			p = pb_begin();
			p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
				(DWORD)(uintptr_t)null_texture_addr & 0x03ffffff,
				xbox_tex_format_argb8());
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

static void APIENTRY xbox_glClear(GLbitfield mask)
{
	if (!xbox_pbkit_initialized) return;

	static int frame_number = 0;
	if (frame_number < 5) {
		xbox_log("Xbox: glClear ENTER frame=%d mask=%x\n", frame_number, mask);
	}

	// Frame-start sequence (matching mesh sample pattern):
	// pb_wait_for_vbl + pb_reset + pb_target_back_buffer at frame START.
	// showframe() signals frame-end with pb_finished(); we do the reset here.
	{
		pb_wait_for_vbl();
		pb_reset();
		pb_target_back_buffer();
		if (frame_number == 0)
			xbox_log("Xbox: first frame reset done\n");
	}

	// Per-frame draw stats (log first 10 frames)
	if (frame_number > 0 && frame_number <= 10) {
		xbox_log("Xbox: FRAME %d end: draws=%d skips=%d vp_valid=%d vp=(%d,%d,%d,%d)\n",
			frame_number - 1, frame_draw_count, frame_skip_count,
			vp_valid, (int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h);
	}
	frame_draw_count = 0;
	frame_skip_count = 0;
	global_frame_num = frame_number;
	frame_number++;

	if (frame_number <= 3) {
		xbox_log("Xbox: glClear(%x) scr=%dx%d col=%d,%d,%d/255 vp_set=%d\n",
			mask, screen_width, screen_height,
			(int)(gl_state.clear_r * 255), (int)(gl_state.clear_g * 255),
			(int)(gl_state.clear_b * 255), viewport_set_count);
	}

	// ALWAYS clear depth+stencil, then color — matching mesh sample pattern.
	// The mesh sample clears depth EVERY frame regardless. This ensures the
	// GPU surface state stays consistent. Use the newer pb_set_depth_stencil_buffer_region
	// which uses explicit register writes instead of the shared PARAMETER_A/B registers.
	pb_set_depth_stencil_buffer_region(
		NV097_SET_SURFACE_FORMAT_ZETA_Z24S8,
		0xFFFFFF, 0x00, // depth=max, stencil=0
		0, 0, screen_width, screen_height);

	if (mask & GL_COLOR_BUFFER_BIT) {
		unsigned char cr = (unsigned char)(gl_state.clear_r * 255.0f);
		unsigned char cg = (unsigned char)(gl_state.clear_g * 255.0f);
		unsigned char cb = (unsigned char)(gl_state.clear_b * 255.0f);
		unsigned char ca = (unsigned char)(gl_state.clear_a * 255.0f);
		DWORD color = (ca << 24) | (cr << 16) | (cg << 8) | cb;
		pb_fill(0, 0, screen_width, screen_height, color);
	}

	// Wait for clears to complete before 3D setup (matches mesh sample pattern)
	while (pb_busy()) { /* spin */ }

	// Reset vertex streaming pool and GPU sync counter each frame
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

		// Re-set critical pipeline state to known-good values after clears.
		// pb_init sets these once, but per-draw state changes and the
		// hardware clear operation may leave residual state issues.
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
	(void)n; (void)f;
	// Handled by viewport setup
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
		GLuint id = xbox_next_id++;
		textures[i] = id;
		if (id < MAX_TEXTURES) {
			memset(&texture_table[id], 0, sizeof(texture_table[id]));
			// Default wrap/filter
			texture_table[id].wrap_s = GL_REPEAT;
			texture_table[id].wrap_t = GL_REPEAT;
			texture_table[id].min_filter = GL_NEAREST;
			texture_table[id].mag_filter = GL_NEAREST;
		}
	}
}

static void APIENTRY xbox_glDeleteTextures(GLsizei n, const GLuint *textures)
{
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = textures[i];
		if (id > 0 && id < MAX_TEXTURES && texture_table[id].allocated) {
			if (texture_table[id].addr) {
				MmFreeContiguousMemory(texture_table[id].addr);
			}
			memset(&texture_table[id], 0, sizeof(texture_table[id]));
		}
	}
}

static void APIENTRY xbox_glBindTexture(GLenum target, GLuint texture)
{
	(void)target;
	int unit = (gl_state.active_texture == GL_TEXTURE1) ? 1 : 0;
	gl_state.bound_texture[unit] = texture;
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

	static int tex_upload_count = 0;
	if (tex_upload_count < 10) {
		xbox_log("Xbox: glTexImage2D id=%d %dx%d fmt=%x px=%p\n",
			id, w, h, fmt, px);
		tex_upload_count++;
	}

	if (id == 0 || id >= MAX_TEXTURES) return;

	struct xbox_texture *tex = &texture_table[id];

	// Free previous allocation if sizes differ
	if (tex->allocated && tex->addr && (tex->width != w || tex->height != h)) {
		MmFreeContiguousMemory(tex->addr);
		tex->addr = NULL;
		tex->allocated = 0;
	}

	// NV2A requires texture pitch aligned to 64 bytes minimum.
	int src_pitch = w * 4;
	int pitch = (src_pitch + 63) & ~63;

	if (!tex->allocated) {
		tex->addr = MmAllocateContiguousMemoryEx(pitch * h, 0, MAXRAM, 0,
			PAGE_READWRITE | PAGE_WRITECOMBINE);
		if (!tex->addr) {
			buildprintf("xbox_glTexImage2D: MmAlloc failed for %dx%d texture\n", w, h);
			return;
		}
		tex->width = w;
		tex->height = h;
		tex->pitch = pitch;
		tex->allocated = 1;
	}

	if (px) {
		if (fmt == GL_BGRA) {
			// Already in NV2A native A8R8G8B8 format — row-by-row for pitch padding
			const unsigned char *src = (const unsigned char *)px;
			unsigned char *dst = (unsigned char *)tex->addr;
			for (int row = 0; row < h; row++) {
				memcpy(dst, src, src_pitch);
				src += src_pitch;
				dst += pitch;
			}
		} else if (fmt == GL_RGBA) {
			// Swizzle RGBA → BGRA (swap R and B) — row-by-row for pitch padding
			const unsigned char *src = (const unsigned char *)px;
			unsigned char *dst = (unsigned char *)tex->addr;
			for (int row = 0; row < h; row++) {
				for (int x = 0; x < w; x++) {
					dst[x*4+0] = src[x*4+2]; // B
					dst[x*4+1] = src[x*4+1]; // G
					dst[x*4+2] = src[x*4+0]; // R
					dst[x*4+3] = src[x*4+3]; // A
				}
				src += src_pitch;
				dst += pitch;
			}
		} else {
			// Other formats: row-by-row copy
			const unsigned char *src = (const unsigned char *)px;
			unsigned char *dst = (unsigned char *)tex->addr;
			for (int row = 0; row < h; row++) {
				memcpy(dst, src, src_pitch);
				src += src_pitch;
				dst += pitch;
			}
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
	unsigned char *dst = (unsigned char *)tex->addr + yo * tex->pitch + xo * 4;

	for (int row = 0; row < h; row++) {
		if (fmt == GL_BGRA) {
			memcpy(dst, src, w * 4);
		} else if (fmt == GL_RGBA) {
			for (int i = 0; i < w; i++) {
				dst[i*4+0] = src[i*4+2]; // B
				dst[i*4+1] = src[i*4+1]; // G
				dst[i*4+2] = src[i*4+0]; // R
				dst[i*4+3] = src[i*4+3]; // A
			}
		} else {
			memcpy(dst, src, w * 4);
		}
		src += w * 4;
		dst += tex->pitch;
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
	tex->pitch = pitch;
	tex->allocated = 2; // 2 = compressed
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

	// Row-major matrix for v * M (matching mesh sample's matrix_viewport):
	//   out[0][0]=w/2, out[1][1]=-h/2, out[2][2]=zrange,
	//   out[3][0]=x+w/2, out[3][1]=y+h/2, out[3][2]=zmin, out[3][3]=1
	memset(out, 0, 16 * sizeof(float));
	out[0]  = phys_w / 2.0f;           // [0][0] = half-width
	out[5]  = phys_h / -2.0f;          // [1][1] = negative half-height (Y flip)
	out[10] = 65536.0f;                // [2][2] = z range (matching mesh sample)
	out[12] = phys_x + phys_w / 2.0f;  // [3][0] = x center
	out[13] = phys_y + phys_h / 2.0f;  // [3][1] = y center
	out[14] = 0.0f;                     // [3][2] = z min
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

// NV2A texture format for uncompressed ARGB8 (LU_IMAGE).
// Matches the proven mesh sample value 0x0001122a exactly:
// CONTEXT_DMA=2, BORDER_SOURCE=COLOR, DIMENSIONALITY=2D,
// COLOR=LU_IMAGE_A8R8G8B8, MIPMAP_LEVELS=1, BASE_SIZE_U=0, BASE_SIZE_V=0
// BASE_SIZE fields are 0 because NPOT_SIZE/NPOT_PITCH handle dimensions.
static uint32_t xbox_tex_format_argb8(void)
{
	return 0x0001122a;
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

static void APIENTRY xbox_glDrawElements(GLenum mode, GLsizei count,
	GLenum type, const void *indices_offset)
{
	if (!xbox_pbkit_initialized || count <= 0) return;

	// Periodic GPU sync: pbkit's push buffer is 512KB. Each draw emits ~200
	// bytes, so we can safely do ~2000 draws before risking overflow.
	// Sync every 500 draws to let the GPU catch up.
	if (++draw_since_sync >= 500) {
		while (pb_busy()) { /* let GPU catch up */ }
		draw_since_sync = 0;
	}

	GLuint vbo_id = gl_state.bound_vbo;
	GLuint ibo_id = gl_state.bound_ibo;
	if (vbo_id == 0 || vbo_id >= MAX_BUFFERS) {
		frame_skip_count++;
		if (global_frame_num < 5)
			xbox_log("Xbox: SKIP vbo_id=%d ibo_id=%d (bad id)\n", vbo_id, ibo_id);
		return;
	}
	if (ibo_id == 0 || ibo_id >= MAX_BUFFERS) {
		frame_skip_count++;
		if (global_frame_num < 5)
			xbox_log("Xbox: SKIP ibo_id=%d (bad id)\n", ibo_id);
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
		// Log viewport bake details for first few draws
		if (global_frame_num < 3 && frame_draw_count < 3) {
			xbox_log("  VP BAKE: vp=(%d,%d,%d,%d) scr=%dx%d\n",
				(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h,
				screen_width, screen_height);
			xbox_log("  vp_mat diag=%d,%d,%d,%d/1000\n",
				(int)(vp_mat[0]*1000), (int)(vp_mat[5]*1000),
				(int)(vp_mat[10]*1000), (int)(vp_mat[15]*1000));
		}
	} else {
		memcpy(mvp, mvp_clip, sizeof(mvp));
		if (global_frame_num < 3 && frame_draw_count < 3) {
			xbox_log("  VP SKIP: vp_valid=0\n");
		}
	}

	// Fix degenerate 2D projection: when mvp[15]==0, the projection maps w=z.
	// With z=0 vertices, the shader's pos.xyz/=pos.w divides by zero → NaN,
	// which crashes the NV2A. Fix: set mvp[15]=1 so w=1 after transform.
	if (mvp[15] == 0.0f) {
		mvp[15] = 1.0f;
	}

	frame_draw_count++;

	// Log draws: first 5 per frame, for first 5 frames
	if (global_frame_num < 5 && frame_draw_count <= 5) {
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
				xbox_log("  tex: %dx%d pitch=%d phys=%x\n",
					t->width, t->height, t->pitch,
					(unsigned)((uintptr_t)t->addr & 0x03ffffff));
			}
		}
		xbox_log("  idx=");
		for (int j = 0; j < n; j++) xbox_log("%d ", (int)log_idx[j]);
		xbox_log("\n");
		xbox_log("  mvp diag=%d,%d,%d,%d/1000\n",
			(int)(mvp[0]*1000), (int)(mvp[5]*1000),
			(int)(mvp[10]*1000), (int)(mvp[15]*1000));
		// Log full MVP matrix for first 2 draws of first 2 frames
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

	// ---- 1. Upload shader constants ----
	{
		uint32_t *p = pb_begin();
		p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID, 96);
		// MVP matrix: 4 constant registers (c[0]-c[3], physical slot 96+)
		pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 16);
		memcpy(p, mvp, 16 * 4); p += 16;
		// Colour: 1 constant register (c[4])
		pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 4);
		memcpy(p, gl_uniforms.colour, 4 * 4); p += 4;
		pb_end(p);
	}

	// ---- 2. All state setup (single push buffer block) ----
	// Consolidating texture, alpha test, lighting, and vertex attribs into one
	// pb_begin/pb_end to avoid push buffer fragmentation that causes
	// "object state invalid" GPU errors on heavy draw scenes.
	{
		GLuint tex_id = gl_state.bound_texture[0];
		struct xbox_texture *tex = NULL;
		if (tex_id > 0 && tex_id < MAX_TEXTURES && texture_table[tex_id].allocated) {
			tex = &texture_table[tex_id];
		}

		uint32_t *p = pb_begin();

		// -- Texture stage 0 --
		// DIAGNOSTIC: Force null texture for ALL draws to isolate texture issues
		if (0 && tex && tex->addr) {
			// Check if dimensions are power-of-two (REPEAT only works for POT on NV2A)
			int is_pot = (tex->width & (tex->width - 1)) == 0
			          && (tex->height & (tex->height - 1)) == 0
			          && tex->width > 0 && tex->height > 0;
			uint32_t wrap_val = is_pot
				? xbox_tex_wrap(tex->wrap_s, tex->wrap_t)
				: 0x00030303; // Force CLAMP_TO_EDGE for NPOT
			uint32_t filter_val = xbox_tex_filter(tex->min_filter, tex->mag_filter);

			p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
				(DWORD)(uintptr_t)tex->addr & 0x03ffffff,
				xbox_tex_format_argb8());
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0),
				tex->pitch << 16);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0),
				(tex->width << 16) | tex->height);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), wrap_val);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), filter_val);
		} else if (null_texture_addr) {
			p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
				(DWORD)(uintptr_t)null_texture_addr & 0x03ffffff,
				xbox_tex_format_argb8());
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0), NULL_TEX_PITCH << 16);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0),
				(NULL_TEX_SIZE << 16) | NULL_TEX_SIZE);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), 0x00030303);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
			p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), 0x04074000);
		}

		// -- Alpha test --
		if (gl_uniforms.alphacut > 0.0f) {
			p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 1);
			p = pb_push1(p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER);
			int ref = (int)(gl_uniforms.alphacut * 255.0f);
			if (ref > 255) ref = 255;
			if (ref < 0) ref = 0;
			p = pb_push1(p, NV097_SET_ALPHA_REF, ref);
		} else {
			p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
		}

		// -- Deferred render state (blend, depth, cull) --
		p = pb_push1(p, NV097_SET_BLEND_ENABLE, gl_state.blend_enabled);
		if (gl_state.blend_enabled) {
			p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, (uint32_t)gl_state.blend_sfactor);
			p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, (uint32_t)gl_state.blend_dfactor);
		}
		// DIAGNOSTIC: Force depth test OFF to isolate crash trigger
		p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0 /*gl_state.depth_test_enabled*/);
		if (0 /*gl_state.depth_test_enabled*/) {
			p = pb_push1(p, NV097_SET_DEPTH_FUNC, (uint32_t)gl_state.depth_func);
		}
		p = pb_push1(p, NV097_SET_DEPTH_MASK, 0 /*gl_state.depth_mask ? 1 : 0*/);
		p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, gl_state.cull_enabled);
		if (gl_state.cull_enabled) {
			uint32_t nv_front = (gl_state.front_face == GL_CW)
				? NV097_SET_FRONT_FACE_V_CW : NV097_SET_FRONT_FACE_V_CCW;
			p = pb_push1(p, NV097_SET_FRONT_FACE, nv_front);
			p = pb_push1(p, NV097_SET_CULL_FACE, (uint32_t)gl_state.cull_face);
		}

		pb_end(p);
	}

	// ---- 2b. Set active vertex attribute pointers ----
	{
		void *vbo_base = vbo->gpu_addr;
		if (attrib_state[XATTR_VERTEX].enabled) {
			xbox_set_attrib_pointer(XATTR_VERTEX,
				attrib_state[XATTR_VERTEX].size,
				attrib_state[XATTR_VERTEX].stride,
				(char *)vbo_base + attrib_state[XATTR_VERTEX].offset);
		}
		if (attrib_state[XATTR_TEXCOORD].enabled) {
			xbox_set_attrib_pointer(XATTR_TEXCOORD,
				attrib_state[XATTR_TEXCOORD].size,
				attrib_state[XATTR_TEXCOORD].stride,
				(char *)vbo_base + attrib_state[XATTR_TEXCOORD].offset);
		}
	}

	// ---- 3. Submit indices in batches ----
	// Uses INDEX_DATA (0x1800) with packed 16-bit index pairs, matching the
	// proven nxdk mesh sample approach. Each 32-bit word contains two 16-bit
	// indices: (index[2k+1] << 16) | index[2k].
	{
		#define MAX_BATCH 240
		const GLushort *src_indices = (const GLushort *)((char *)ibo->cpu_data + (intptr_t)indices_offset);

		// Determine NV2A primitive type
		uint32_t nv_prim;
		switch (mode) {
			case GL_TRIANGLE_FAN:   nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLE_FAN; break;
			case GL_TRIANGLE_STRIP: nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP; break;
			case GL_TRIANGLES:      nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLES; break;
			default:                nv_prim = NV097_SET_BEGIN_END_OP_TRIANGLES; break;
		}

		for (int i = 0; i < count; ) {
			int batch = count - i;
			if (batch > MAX_BATCH) batch = MAX_BATCH;
			int packed_count = (batch + 1) / 2; // ceil(batch/2) packed 32-bit words

			uint32_t *p = pb_begin();
			p = pb_push1(p, NV097_SET_BEGIN_END, nv_prim);

			// NV097_ARRAY_ELEMENT16 / INDEX_DATA: packed 16-bit index pairs
			pb_push(p++, 0x40000000 | NV097_ARRAY_ELEMENT16, packed_count);
			for (int j = 0; j < batch - 1; j += 2) {
				*p++ = ((uint32_t)src_indices[i + j + 1] << 16)
				     | (uint32_t)src_indices[i + j];
			}
			if (batch & 1) {
				// Odd count: last word has one real index, duplicate it to
				// create a degenerate triangle that renders nothing visible
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
	glinfo.texnpot = 1;
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
