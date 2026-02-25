// glbuild_xbox.c — Xbox (nxdk) GL shim for POLYMOST
// Provides stub implementations of all GL functions that POLYMOST calls
// through the glfunc dispatch table. No actual rendering yet (Milestone 1).

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

struct glbuild_funcs glfunc;

// ---- Stub GL string responses ----
static const GLubyte xbox_gl_version[]     = "2.0 NV2A Xbox";
static const GLubyte xbox_gl_vendor[]      = "NVIDIA (Xbox)";
static const GLubyte xbox_gl_renderer[]    = "NV2A (pbkit shim)";
static const GLubyte xbox_gl_extensions[]  = "GL_EXT_texture_filter_anisotropic "
                                             "GL_EXT_bgra "
                                             "GL_EXT_texture_compression_s3tc "
                                             "GL_ARB_texture_non_power_of_two "
                                             "GL_ARB_shading_language_100";
static const GLubyte xbox_glsl_version[]   = "1.10";

// ---- ID generators for textures, buffers, shaders, programs ----
static GLuint xbox_next_id = 1;

// ---- Stub GL function implementations ----

static void APIENTRY stub_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
static void APIENTRY stub_glClear(GLbitfield mask) { (void)mask; }
static void APIENTRY stub_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r; (void)g; (void)b; (void)a; }
static void APIENTRY stub_glBlendFunc(GLenum s, GLenum d) { (void)s; (void)d; }
static void APIENTRY stub_glCullFace(GLenum mode) { (void)mode; }
static void APIENTRY stub_glFrontFace(GLenum mode) { (void)mode; }
static void APIENTRY stub_glPolygonOffset(GLfloat factor, GLfloat units) { (void)factor; (void)units; }
static void APIENTRY stub_glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }
static void APIENTRY stub_glEnable(GLenum cap) { (void)cap; }
static void APIENTRY stub_glDisable(GLenum cap) { (void)cap; }

static void APIENTRY stub_glGetFloatv(GLenum pname, GLfloat *data) {
	if (pname == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT && data) *data = 4.0f;
	else if (data) *data = 0.0f;
}
static void APIENTRY stub_glGetIntegerv(GLenum pname, GLint *data) {
	if (!data) return;
	switch (pname) {
		case GL_MAX_TEXTURE_SIZE:        *data = 4096; break;
		case GL_MAX_TEXTURE_IMAGE_UNITS: *data = 4;    break;
		case GL_MAX_VERTEX_ATTRIBS:      *data = 16;   break;
		default:                         *data = 0;    break;
	}
}

static const GLubyte * APIENTRY stub_glGetString(GLenum name) {
	switch (name) {
		case GL_VERSION:                    return xbox_gl_version;
		case GL_VENDOR:                     return xbox_gl_vendor;
		case GL_RENDERER:                   return xbox_gl_renderer;
		case GL_EXTENSIONS:                 return xbox_gl_extensions;
		case GL_SHADING_LANGUAGE_VERSION:   return xbox_glsl_version;
		default:                            return (const GLubyte *)"";
	}
}

static GLenum APIENTRY stub_glGetError(void) { return GL_NO_ERROR; }
static void APIENTRY stub_glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
static void APIENTRY stub_glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }
static void APIENTRY stub_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
static void APIENTRY stub_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
static void APIENTRY stub_glMinSampleShadingARB(GLfloat val) { (void)val; }

// Depth
static void APIENTRY stub_glDepthFunc(GLenum func) { (void)func; }
static void APIENTRY stub_glDepthMask(GLboolean flag) { (void)flag; }
static void APIENTRY stub_glDepthRange(GLdouble n, GLdouble f) { (void)n; (void)f; }

// Raster
static void APIENTRY stub_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *px) {
	(void)x; (void)y; (void)w; (void)h; (void)fmt; (void)type;
	if (px) memset(px, 0, w * h * 4);
}

// Texture mapping
static void APIENTRY stub_glGenTextures(GLsizei n, GLuint *textures) {
	for (GLsizei i = 0; i < n; i++) textures[i] = xbox_next_id++;
}
static void APIENTRY stub_glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }
static void APIENTRY stub_glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture; }
static void APIENTRY stub_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *px) {
	(void)target; (void)level; (void)ifmt; (void)w; (void)h; (void)border; (void)fmt; (void)type; (void)px;
}
static void APIENTRY stub_glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *px) {
	(void)target; (void)level; (void)xo; (void)yo; (void)w; (void)h; (void)fmt; (void)type; (void)px;
}
static void APIENTRY stub_glTexParameterf(GLenum target, GLenum pname, GLfloat param) { (void)target; (void)pname; (void)param; }
static void APIENTRY stub_glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; }
static void APIENTRY stub_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei sz, const void *data) {
	(void)target; (void)level; (void)ifmt; (void)w; (void)h; (void)border; (void)sz; (void)data;
}

// Buffer objects
static void APIENTRY stub_glBindBuffer(GLenum target, GLuint buffer) { (void)target; (void)buffer; }
static void APIENTRY stub_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) { (void)target; (void)size; (void)data; (void)usage; }
static void APIENTRY stub_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) { (void)target; (void)offset; (void)size; (void)data; }
static void APIENTRY stub_glDeleteBuffers(GLsizei n, const GLuint *bufs) { (void)n; (void)bufs; }
static void APIENTRY stub_glGenBuffers(GLsizei n, GLuint *bufs) {
	for (GLsizei i = 0; i < n; i++) bufs[i] = xbox_next_id++;
}
static void APIENTRY stub_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) { (void)mode; (void)count; (void)type; (void)indices; }
static void APIENTRY stub_glEnableVertexAttribArray(GLuint index) { (void)index; }
static void APIENTRY stub_glDisableVertexAttribArray(GLuint index) { (void)index; }
static void APIENTRY stub_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr) {
	(void)index; (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
}

// Shaders
static void APIENTRY stub_glActiveTexture(GLenum texture) { (void)texture; }
static void APIENTRY stub_glAttachShader(GLuint program, GLuint shader) { (void)program; (void)shader; }
static void APIENTRY stub_glCompileShader(GLuint shader) { (void)shader; }
static GLuint APIENTRY stub_glCreateProgram(void) { return xbox_next_id++; }
static GLuint APIENTRY stub_glCreateShader(GLenum type) { (void)type; return xbox_next_id++; }
static void APIENTRY stub_glDeleteProgram(GLuint program) { (void)program; }
static void APIENTRY stub_glDeleteShader(GLuint shader) { (void)shader; }
static void APIENTRY stub_glDetachShader(GLuint program, GLuint shader) { (void)program; (void)shader; }
static GLint APIENTRY stub_glGetAttribLocation(GLuint program, const GLchar *name) { (void)program; (void)name; return 0; }
static void APIENTRY stub_glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
	(void)program;
	if (!params) return;
	switch (pname) {
		case GL_LINK_STATUS:    *params = GL_TRUE; break;
		case GL_INFO_LOG_LENGTH: *params = 0; break;
		default:                *params = 0; break;
	}
}
static void APIENTRY stub_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
	(void)program;
	if (infoLog && bufSize > 0) infoLog[0] = '\0';
	if (length) *length = 0;
}
static void APIENTRY stub_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
	(void)shader;
	if (!params) return;
	switch (pname) {
		case GL_COMPILE_STATUS:  *params = GL_TRUE; break;
		case GL_INFO_LOG_LENGTH: *params = 0; break;
		default:                 *params = 0; break;
	}
}
static void APIENTRY stub_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
	(void)shader;
	if (infoLog && bufSize > 0) infoLog[0] = '\0';
	if (length) *length = 0;
}
static GLint APIENTRY stub_glGetUniformLocation(GLuint program, const GLchar *name) { (void)program; (void)name; return 0; }
static void APIENTRY stub_glLinkProgram(GLuint program) { (void)program; }
static void APIENTRY stub_glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) {
	(void)shader; (void)count; (void)string; (void)length;
}
static void APIENTRY stub_glUniform1i(GLint loc, GLint v0) { (void)loc; (void)v0; }
static void APIENTRY stub_glUniform1f(GLint loc, GLfloat v0) { (void)loc; (void)v0; }
static void APIENTRY stub_glUniform2f(GLint loc, GLfloat v0, GLfloat v1) { (void)loc; (void)v0; (void)v1; }
static void APIENTRY stub_glUniform3f(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2) { (void)loc; (void)v0; (void)v1; (void)v2; }
static void APIENTRY stub_glUniform4f(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) { (void)loc; (void)v0; (void)v1; (void)v2; (void)v3; }
static void APIENTRY stub_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *val) { (void)loc; (void)count; (void)transpose; (void)val; }
static void APIENTRY stub_glUseProgram(GLuint program) { (void)program; }


// ========================================================================
// glbuild interface — replaces glbuild.c for Xbox
// ========================================================================

static int osdcmd_glinfo(const osdfuncparm_t *);

int glbuild_loadfunctions(void)
{
	glfunc.glClearColor   = stub_glClearColor;
	glfunc.glClear        = stub_glClear;
	glfunc.glColorMask    = stub_glColorMask;
	glfunc.glBlendFunc    = stub_glBlendFunc;
	glfunc.glCullFace     = stub_glCullFace;
	glfunc.glFrontFace    = stub_glFrontFace;
	glfunc.glPolygonOffset = stub_glPolygonOffset;
	glfunc.glPolygonMode  = stub_glPolygonMode;
	glfunc.glEnable       = stub_glEnable;
	glfunc.glDisable      = stub_glDisable;
	glfunc.glGetFloatv    = stub_glGetFloatv;
	glfunc.glGetIntegerv  = stub_glGetIntegerv;
	glfunc.glGetString    = stub_glGetString;
	glfunc.glGetError     = stub_glGetError;
	glfunc.glHint         = stub_glHint;
	glfunc.glPixelStorei  = stub_glPixelStorei;
	glfunc.glViewport     = stub_glViewport;
	glfunc.glScissor      = stub_glScissor;
	glfunc.glMinSampleShadingARB = stub_glMinSampleShadingARB;

	// Depth
	glfunc.glDepthFunc    = stub_glDepthFunc;
	glfunc.glDepthMask    = stub_glDepthMask;
	glfunc.glDepthRange   = stub_glDepthRange;

	// Raster
	glfunc.glReadPixels   = stub_glReadPixels;

	// Texture mapping
	glfunc.glGenTextures  = stub_glGenTextures;
	glfunc.glDeleteTextures = stub_glDeleteTextures;
	glfunc.glBindTexture  = stub_glBindTexture;
	glfunc.glTexImage2D   = stub_glTexImage2D;
	glfunc.glTexSubImage2D = stub_glTexSubImage2D;
	glfunc.glTexParameterf = stub_glTexParameterf;
	glfunc.glTexParameteri = stub_glTexParameteri;
	glfunc.glCompressedTexImage2D = stub_glCompressedTexImage2D;

	// Buffer objects
	glfunc.glBindBuffer   = stub_glBindBuffer;
	glfunc.glBufferData   = stub_glBufferData;
	glfunc.glBufferSubData = stub_glBufferSubData;
	glfunc.glDeleteBuffers = stub_glDeleteBuffers;
	glfunc.glGenBuffers   = stub_glGenBuffers;
	glfunc.glDrawElements = stub_glDrawElements;
	glfunc.glEnableVertexAttribArray = stub_glEnableVertexAttribArray;
	glfunc.glDisableVertexAttribArray = stub_glDisableVertexAttribArray;
	glfunc.glVertexAttribPointer = stub_glVertexAttribPointer;

	// Shaders
	glfunc.glActiveTexture = stub_glActiveTexture;
	glfunc.glAttachShader  = stub_glAttachShader;
	glfunc.glCompileShader = stub_glCompileShader;
	glfunc.glCreateProgram = stub_glCreateProgram;
	glfunc.glCreateShader  = stub_glCreateShader;
	glfunc.glDeleteProgram = stub_glDeleteProgram;
	glfunc.glDeleteShader  = stub_glDeleteShader;
	glfunc.glDetachShader  = stub_glDetachShader;
	glfunc.glGetAttribLocation = stub_glGetAttribLocation;
	glfunc.glGetProgramiv  = stub_glGetProgramiv;
	glfunc.glGetProgramInfoLog = stub_glGetProgramInfoLog;
	glfunc.glGetShaderiv   = stub_glGetShaderiv;
	glfunc.glGetShaderInfoLog = stub_glGetShaderInfoLog;
	glfunc.glGetUniformLocation = stub_glGetUniformLocation;
	glfunc.glLinkProgram   = stub_glLinkProgram;
	glfunc.glShaderSource  = stub_glShaderSource;
	glfunc.glUniform1i     = stub_glUniform1i;
	glfunc.glUniform1f     = stub_glUniform1f;
	glfunc.glUniform2f     = stub_glUniform2f;
	glfunc.glUniform3f     = stub_glUniform3f;
	glfunc.glUniform4f     = stub_glUniform4f;
	glfunc.glUniformMatrix4fv = stub_glUniformMatrix4fv;
	glfunc.glUseProgram    = stub_glUseProgram;

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

	memset(&glinfo, 0, sizeof(glinfo));

	// Fill in NV2A capabilities.
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
	// No-op on Xbox — stub GL never errors.
}


// ---- Shader compilation / linking stubs ----
// These are called by the 8-bit shader path and by polymost_glinit.
// On Xbox they "succeed" immediately — the actual shaders will be
// Cg-based NV2A programs in a later milestone.

GLuint glbuild_compile_shader(GLuint type, const GLchar *source)
{
	(void)source;
	(void)type;
	return xbox_next_id++;
}

GLuint glbuild_link_program(int shadercount, GLuint *shaders)
{
	(void)shadercount; (void)shaders;
	return xbox_next_id++;
}


// ---- 8-bit shader path stubs ----
// The software renderer's 8-bit framebuffer blitting through GL shaders.
// On Xbox with software mode we use the SDL texture path instead,
// so these are no-ops that succeed.

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
	// Return success so the engine believes GL 8-bit blitting is ready.
	// Actual display goes through showframe()'s SDL path on Xbox.
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


// ---- OSD command ----

static int osdcmd_glinfo(const osdfuncparm_t *parm)
{
	(void)parm;
	buildprintf(
		"OpenGL Information (Xbox NV2A shim):\n"
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
		" Clamp-to-edge: %s\n",
		xbox_gl_version, xbox_gl_vendor, xbox_gl_renderer, xbox_glsl_version,
		glinfo.maxtexsize, glinfo.multitex, glinfo.maxanisotropy,
		glinfo.bgra ? "yes" : "no",
		glinfo.texnpot ? "yes" : "no",
		glinfo.texcomprdxt1 ? "yes" : "no",
		glinfo.texcomprdxt5 ? "yes" : "no",
		glinfo.clamptoedge ? "yes" : "no"
	);
	return OSDCMD_OK;
}


#endif  //USE_OPENGL
