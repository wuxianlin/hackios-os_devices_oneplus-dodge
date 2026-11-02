#include <stdio.h>
#include <string.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>
int main(void) {
   OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
   if (!ctx) { printf("no ctx\n"); return 1; }
   static unsigned char buf[64*64*4];
   if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, 64, 64)) { printf("mc fail\n"); return 1; }
   printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
   printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
   glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();
   int r = buf[0], g = buf[1], b = buf[2], a = buf[3];
   printf("pixel: %d %d %d %d\n", r, g, b, a);
   OSMesaDestroyContext(ctx);
   return (r==64 && g==128 && b==191) ? 0 : 2;
}
