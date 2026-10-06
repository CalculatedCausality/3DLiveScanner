// Minimal GL declaration boundary for host call-count tests; not a GPU driver.
#pragma once
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
#define GL_TRIANGLES 4
#define GL_POINTS 0
#define GL_LINES 1
#define GL_TEXTURE_2D 3553
#define GL_TEXTURE0 33984
#define GL_TEXTURE1 33985
#define GL_TEXTURE2 33986
#define GL_LEQUAL 515
#define GL_LESS 513
#define GL_BLEND 3042
#define GL_DEPTH_TEST 2929
#define GL_SRC_ALPHA 770
#define GL_DST_ALPHA 772
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glDeleteTextures(GLsizei, const GLuint*);
void glActiveTexture(GLenum);
void glBindTexture(GLenum, GLuint);
void glDepthFunc(GLenum);
void glLineWidth(float);
void glEnable(GLenum);
void glDisable(GLenum);
void glBlendFunc(GLenum, GLenum);
