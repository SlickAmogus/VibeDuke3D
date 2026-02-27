// xbox_gl_defs.h — GL types, constants, and function pointer typedefs for Xbox (nxdk)
// Replaces <GL/gl.h> and glext.h since Xbox has no OpenGL headers.
// All definitions here are consumed by glbuild_priv.h and the POLYMOST renderer.

#ifndef __xbox_gl_defs_h__
#define __xbox_gl_defs_h__

#include <stddef.h>
#include <stdint.h>

// ---- Calling convention macros (no-ops on Xbox) ----
#define APIENTRY
#define APIENTRYP *
#define GLAPI

// ---- GL base types ----
typedef unsigned int   GLenum;
typedef unsigned int   GLbitfield;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef double         GLdouble;
typedef double         GLclampd;
typedef float          GLclampf;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef char           GLchar;
typedef unsigned short GLushort;
typedef void           GLvoid;
typedef ptrdiff_t      GLsizeiptr;
typedef ptrdiff_t      GLintptr;

// ---- Boolean ----
#define GL_FALSE  0
#define GL_TRUE   1

// ---- Error codes ----
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_STACK_OVERFLOW                 0x0503
#define GL_STACK_UNDERFLOW                0x0504
#define GL_OUT_OF_MEMORY                  0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION  0x0506

// ---- Enable/Disable caps ----
#define GL_DEPTH_TEST     0x0B71
#define GL_BLEND          0x0BE2
#define GL_DITHER         0x0BD0
#define GL_SCISSOR_TEST   0x0C11
#define GL_CULL_FACE      0x0B44
#define GL_MULTISAMPLE    0x809D

// ---- Clear bits ----
#define GL_COLOR_BUFFER_BIT  0x00004000
#define GL_DEPTH_BUFFER_BIT  0x00000100

// ---- Primitive types ----
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006

// ---- Polygon mode ----
#define GL_POINT          0x1B00
#define GL_LINE           0x1B01
#define GL_FILL           0x1B02

// ---- Depth function ----
#define GL_LESS           0x0201
#define GL_LEQUAL         0x0203
#define GL_ALWAYS         0x0207

// ---- Blend factors ----
#define GL_SRC_ALPHA              0x0302
#define GL_ONE_MINUS_SRC_ALPHA    0x0303

// ---- Cull face ----
#define GL_FRONT          0x0404
#define GL_BACK           0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW             0x0900
#define GL_CCW            0x0901

// ---- Texture targets & params ----
#define GL_TEXTURE_2D              0x0DE1
#define GL_TEXTURE_MAG_FILTER      0x2800
#define GL_TEXTURE_MIN_FILTER      0x2801
#define GL_TEXTURE_WRAP_S          0x2802
#define GL_TEXTURE_WRAP_T          0x2803
#define GL_NEAREST                 0x2600
#define GL_LINEAR                  0x2601
#define GL_NEAREST_MIPMAP_NEAREST  0x2700
#define GL_LINEAR_MIPMAP_NEAREST   0x2701
#define GL_NEAREST_MIPMAP_LINEAR   0x2702
#define GL_LINEAR_MIPMAP_LINEAR    0x2703
#define GL_REPEAT                  0x2901
#define GL_CLAMP                   0x2900
#define GL_CLAMP_TO_EDGE           0x812F

// ---- Texture units ----
#define GL_TEXTURE0   0x84C0
#define GL_TEXTURE1   0x84C1

// ---- Pixel formats ----
#define GL_RED            0x1903
#define GL_RGB            0x1907
#define GL_RGBA           0x1908
#define GL_BGRA           0x80E1
#define GL_LUMINANCE      0x1909
#define GL_UNSIGNED_BYTE  0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT          0x1406

// ---- Pixel store ----
#define GL_UNPACK_ALIGNMENT  0x0CF5

// ---- Hint ----
#define GL_DONT_CARE              0x1100
#define GL_FASTEST                0x1101
#define GL_NICEST                 0x1102
#define GL_GENERATE_MIPMAP_HINT   0x8192

// ---- Multisample ----
#define GL_MULTISAMPLE_FILTER_HINT_NV  0x8534
#define GL_SAMPLE_SHADING_ARB          0x8C36

// ---- String queries ----
#define GL_VENDOR                     0x1F00
#define GL_RENDERER                   0x1F01
#define GL_VERSION                    0x1F02
#define GL_EXTENSIONS                 0x1F03
#define GL_SHADING_LANGUAGE_VERSION   0x8B8C
#define GL_NUM_EXTENSIONS             0x821D

// ---- Integer queries ----
#define GL_MAX_TEXTURE_SIZE           0x0D33
#define GL_MAX_TEXTURE_IMAGE_UNITS    0x8872
#define GL_MAX_VERTEX_ATTRIBS         0x8869

// ---- Buffer objects ----
#define GL_ARRAY_BUFFER            0x8892
#define GL_ELEMENT_ARRAY_BUFFER    0x8893
#define GL_STATIC_DRAW             0x88E4
#define GL_STREAM_DRAW             0x88E0

// ---- Shader types ----
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30

// ---- Shader/program queries ----
#define GL_COMPILE_STATUS     0x8B81
#define GL_LINK_STATUS        0x8B82
#define GL_INFO_LOG_LENGTH    0x8B84

// ---- Compressed textures (S3TC) ----
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3

// ---- ETC1 ----
#define GL_ETC1_RGB8_OES  0x8D64

// ---- Anisotropic filtering ----
#define GL_TEXTURE_MAX_ANISOTROPY_EXT      0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT  0x84FF

// ---- Debug output (stubs — not used on Xbox but needed for compilation) ----
// GL_KHR_debug is NOT defined on Xbox to skip debug callback code paths.
// GL_ARB_debug_output is NOT defined on Xbox to skip debug callback code paths.

#define GL_DEBUG_OUTPUT  0x92E0

// ---- Function pointer typedefs NOT in glbuild_priv.h lines 39-68 ----
// (glbuild_priv.h defines 30 typedefs for GL 1.x base functions;
//  these are the additional ones needed for GL 2.0+ / extensions)

// Compressed textures
typedef void (APIENTRYP PFNGLCOMPRESSEDTEXIMAGE2DPROC) (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);

// Buffer objects
typedef void (APIENTRYP PFNGLBINDBUFFERPROC) (GLenum target, GLuint buffer);
typedef void (APIENTRYP PFNGLBUFFERDATAPROC) (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void (APIENTRYP PFNGLBUFFERSUBDATAPROC) (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
typedef void (APIENTRYP PFNGLDELETEBUFFERSPROC) (GLsizei n, const GLuint *buffers);
typedef void (APIENTRYP PFNGLGENBUFFERSPROC) (GLsizei n, GLuint *buffers);

// Vertex attribs
typedef void (APIENTRYP PFNGLENABLEVERTEXATTRIBARRAYPROC) (GLuint index);
typedef void (APIENTRYP PFNGLDISABLEVERTEXATTRIBARRAYPROC) (GLuint index);
typedef void (APIENTRYP PFNGLVERTEXATTRIBPOINTERPROC) (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);

// Shaders & programs
typedef void (APIENTRYP PFNGLACTIVETEXTUREPROC) (GLenum texture);
typedef void (APIENTRYP PFNGLATTACHSHADERPROC) (GLuint program, GLuint shader);
typedef void (APIENTRYP PFNGLCOMPILESHADERPROC) (GLuint shader);
typedef GLuint (APIENTRYP PFNGLCREATEPROGRAMPROC) (void);
typedef GLuint (APIENTRYP PFNGLCREATESHADERPROC) (GLenum type);
typedef void (APIENTRYP PFNGLDELETEPROGRAMPROC) (GLuint program);
typedef void (APIENTRYP PFNGLDELETESHADERPROC) (GLuint shader);
typedef void (APIENTRYP PFNGLDETACHSHADERPROC) (GLuint program, GLuint shader);
typedef GLint (APIENTRYP PFNGLGETATTRIBLOCATIONPROC) (GLuint program, const GLchar *name);
typedef void (APIENTRYP PFNGLGETPROGRAMIVPROC) (GLuint program, GLenum pname, GLint *params);
typedef void (APIENTRYP PFNGLGETPROGRAMINFOLOGPROC) (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void (APIENTRYP PFNGLGETSHADERIVPROC) (GLuint shader, GLenum pname, GLint *params);
typedef void (APIENTRYP PFNGLGETSHADERINFOLOGPROC) (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLint (APIENTRYP PFNGLGETUNIFORMLOCATIONPROC) (GLuint program, const GLchar *name);
typedef void (APIENTRYP PFNGLLINKPROGRAMPROC) (GLuint program);
typedef void (APIENTRYP PFNGLSHADERSOURCEPROC) (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
typedef void (APIENTRYP PFNGLUNIFORM1IPROC) (GLint location, GLint v0);
typedef void (APIENTRYP PFNGLUNIFORM1FPROC) (GLint location, GLfloat v0);
typedef void (APIENTRYP PFNGLUNIFORM2FPROC) (GLint location, GLfloat v0, GLfloat v1);
typedef void (APIENTRYP PFNGLUNIFORM3FPROC) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void (APIENTRYP PFNGLUNIFORM4FPROC) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void (APIENTRYP PFNGLUNIFORMMATRIX4FVPROC) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRYP PFNGLUSEPROGRAMPROC) (GLuint program);

// GLES2 depth range (not used on Xbox, but typedef needed to avoid ifdefs)
typedef void (APIENTRYP PFNGLDEPTHRANGEFPROC) (GLfloat n, GLfloat f);

// Min sample shading (ARB extension, optional)
typedef void (APIENTRYP PFNGLMINSAMPLESHADINGARBPROC) (GLfloat value);

#endif // __xbox_gl_defs_h__
