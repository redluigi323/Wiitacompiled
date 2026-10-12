// Host-side stand-in for <vitaGL.h>, used only by the host test build.
//
// gl_submit.cpp is the live renderer and cannot be unit tested without a GL, so the
// test TU includes it directly with this header on the include path and supplies
// recording implementations of the entry points below. Every constant here is copied
// verbatim from third_party/vitaGL/source/vitaGL.h - if that header ever disagrees
// with this one the test is testing the wrong numbers, so they are pasted, not invented.
//
// Note glTexEnvfv takes a NON-const pointer, matching vitaGL (ffp.c:2878).
#pragma once

#include <cstddef>
#include <cstdint>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;
typedef std::ptrdiff_t GLintptr;
typedef std::ptrdiff_t GLsizeiptr;
typedef int GLfixed;

#define GL_ADD                               0x0104
#define GL_ALPHA_SCALE                       0x0D1C
#define GL_ALPHA_TEST                        0x0BC0
#define GL_ALWAYS                            0x0207
#define GL_BACK                              0x0405
#define GL_BLEND                             0x0BE2
#define GL_CLAMP_TO_EDGE                     0x812F
#define GL_COLOR_ARRAY                       0x8076
#define GL_COMBINE                           0x8570
#define GL_COMBINE_ALPHA                     0x8572
#define GL_COMBINE_RGB                       0x8571
#define GL_CONSTANT                          0x8576
#define GL_CULL_FACE                         0x0B44
#define GL_CW                                0x0900
#define GL_DECAL                             0x2101
#define GL_DEPTH_TEST                        0x0B71
#define GL_DST_ALPHA                         0x0304
#define GL_EQUAL                             0x0202
#define GL_FALSE                             0
#define GL_FLOAT                             0x1406
#define GL_FRONT                             0x0404
#define GL_FRONT_AND_BACK                    0x0408
#define GL_FUNC_ADD                          0x8006
#define GL_FUNC_REVERSE_SUBTRACT             0x800B
#define GL_MAX                               0x8008
#define GL_DST_COLOR                         0x0306
#define GL_ONE_MINUS_DST_COLOR               0x0307
#define GL_GEQUAL                            0x0206
#define GL_GREATER                           0x0204
#define GL_INTERPOLATE                       0x8575
#define GL_LEQUAL                            0x0203
#define GL_LESS                              0x0201
#define GL_LINEAR                            0x2601
#define GL_LINEAR_MIPMAP_LINEAR              0x2703
#define GL_LINEAR_MIPMAP_NEAREST             0x2701
#define GL_MIRRORED_REPEAT                   0x8370
#define GL_MODULATE                          0x2100
#define GL_NEAREST                           0x2600
#define GL_NEAREST_MIPMAP_LINEAR             0x2702
#define GL_NEAREST_MIPMAP_NEAREST            0x2700
#define GL_NEVER                             0x0200
#define GL_NOTEQUAL                          0x0205
#define GL_ONE                               1
#define GL_ONE_MINUS_DST_ALPHA               0x0305
#define GL_ONE_MINUS_SRC_ALPHA               0x0303
#define GL_ONE_MINUS_SRC_COLOR               0x0301
#define GL_OPERAND0_ALPHA                    0x8598
#define GL_OPERAND0_RGB                      0x8590
#define GL_OPERAND1_ALPHA                    0x8599
#define GL_OPERAND1_RGB                      0x8591
#define GL_OPERAND2_ALPHA                    0x859A
#define GL_OPERAND2_RGB                      0x8592
#define GL_PRIMARY_COLOR                     0x8577
#define GL_REPEAT                            0x2901
#define GL_REPLACE                           0x1E01
#define GL_RGB 0x1907
#define GL_RGBA                              0x1908
#define GL_RGB_SCALE                         0x8573
#define GL_SCISSOR_TEST                      0x0C11
#define GL_SRC0_ALPHA                        0x8588
#define GL_SRC0_RGB                          0x8580
#define GL_SRC1_ALPHA                        0x8589
#define GL_SRC1_RGB                          0x8581
#define GL_SRC2_ALPHA                        0x858A
#define GL_SRC2_RGB                          0x8582
#define GL_SRC_ALPHA                         0x0302
#define GL_SRC_COLOR                         0x0300
#define GL_SUBTRACT                          0x84E7
#define GL_TEXTURE                           0x1702
#define GL_TEXTURE_2D                        0x0DE1
#define GL_TEXTURE_COORD_ARRAY               0x8078
#define GL_TEXTURE_ENV                       0x2300
#define GL_TEXTURE_ENV_COLOR                 0x2201
#define GL_TEXTURE_ENV_MODE                  0x2200
#define GL_TEXTURE_MAG_FILTER                0x2800
#define GL_TEXTURE_MIN_FILTER                0x2801
#define GL_TEXTURE_WRAP_S                    0x2802
#define GL_TEXTURE_WRAP_T                    0x2803
#define GL_TRIANGLES                         0x0004
#define GL_TRUE                              1
#define GL_UNSIGNED_BYTE                     0x1401
#define GL_UNSIGNED_SHORT                    0x1403
#define GL_VERTEX_ARRAY                      0x8074
#define GL_ZERO                              0

extern "C" {
void glAlphaFunc(GLenum, GLclampf);
void glBindTexture(GLenum, GLuint);
void glBlendEquation(GLenum);
void glDepthRangef(GLfloat, GLfloat);
void glClearDepthf(GLclampf);
void glGenerateMipmap(GLenum);
void vglTexImageDepthBuffer(GLenum);
void glBlendFunc(GLenum, GLenum);
void glColorPointer(GLint, GLenum, GLsizei, const GLvoid*);
void glCullFace(GLenum);
void glDeleteTextures(GLsizei, const GLuint*);
void glDepthFunc(GLenum);
void glDepthMask(GLboolean);
void glDisable(GLenum);
void glDisableClientState(GLenum);
void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*);
void glEnable(GLenum);
void glEnableClientState(GLenum);
void glFrontFace(GLenum);
void glGenTextures(GLsizei, GLuint*);
GLenum glGetError(void);
void glScissor(GLint, GLint, GLsizei, GLsizei);
void glTexCoordPointer(GLint, GLenum, GLsizei, const GLvoid*);
void glTexEnvf(GLenum, GLenum, GLfloat);
void glTexEnvfv(GLenum, GLenum, GLfloat*);
void glTexEnvi(GLenum, GLenum, GLint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
void glTexParameteri(GLenum, GLenum, GLint);
void glVertexPointer(GLint, GLenum, GLsizei, const GLvoid*);
void glViewport(GLint, GLint, GLsizei, GLsizei);
}

typedef char GLchar;

/* pulled from third_party/vitaGL/source/vitaGL.h */
#define GL_CG_FRAGMENT_SHADER_EXT            0x890F
#define GL_CG_VERTEX_SHADER_EXT              0x890E
#define GL_COMPILE_STATUS                    0x8B81
#define GL_LINK_STATUS                       0x8B82
#define GL_TEXTURE0                          0x84C0

extern "C" {
void glActiveTexture(GLenum);
GLuint glCreateShader(GLenum);
void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*);
void glCompileShader(GLuint);
void glGetShaderiv(GLuint, GLenum, GLint*);
void glDeleteShader(GLuint);
GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint);
void glLinkProgram(GLuint);
void glGetProgramiv(GLuint, GLenum, GLint*);
void glDeleteProgram(GLuint);
void glUseProgram(GLuint);
GLint glGetAttribLocation(GLuint, const GLchar*);
GLint glGetUniformLocation(GLuint, const GLchar*);
void glUniform1i(GLint, GLint);
void glUniform2f(GLint, GLfloat, GLfloat);
void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void glUniform4fv(GLint, GLsizei, const GLfloat*);
void glUniform3fv(GLint, GLsizei, const GLfloat*);
void glEnableVertexAttribArray(GLuint);
void glDisableVertexAttribArray(GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
}

#define GL_COLOR_BUFFER_BIT                  0x00004000
#define GL_DEPTH_BUFFER_BIT                  0x00000100

extern "C" {
GLboolean glIsEnabled(GLenum);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLbitfield);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
}

extern "C" { void glReadBuffer(GLenum); }

extern "C" { void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean); }

// Framebuffer objects (the EFB render target in gl_submit.cpp).
#define GL_FRAMEBUFFER                       0x8D40
#define GL_READ_FRAMEBUFFER                  0x8CA8
#define GL_DRAW_FRAMEBUFFER                  0x8CA9
#define GL_RENDERBUFFER                      0x8D41
#define GL_COLOR_ATTACHMENT0                 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT          0x821A
#define GL_DEPTH24_STENCIL8                  0x88F0
#define GL_FRAMEBUFFER_COMPLETE              0x8CD5

extern "C" {
void glGenFramebuffers(GLsizei, GLuint*);
void glBindFramebuffer(GLenum, GLuint);
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void glGenRenderbuffers(GLsizei, GLuint*);
void glBindRenderbuffer(GLenum, GLuint);
void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
GLenum glCheckFramebufferStatus(GLenum);
void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
}

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER                     0x8B31
#define GL_FRAGMENT_SHADER                   0x8B30
#endif

extern "C" {
void glBlitNamedFramebuffer(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*);
void glUniform1f(GLint, GLfloat);
}

extern "C" {
void glFinish(void);
void* vglForceAlloc(uint32_t);
void vglReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
}

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
extern "C" {
void glBindBuffer(GLenum, GLuint);
void glGenBuffers(GLsizei, GLuint*);
void vglBufferData(GLenum, const GLvoid*);
}
