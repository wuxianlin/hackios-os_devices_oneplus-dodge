#include <stdio.h>
#include <GL/osmesa.h>
#include <GLES2/gl2.h>
int main(void) {
   int attribs[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE, 0 };
   OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, NULL);
   if (!ctx) { printf("no es ctx\n"); return 1; }
   static unsigned char buf[32*32*4];
   if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, 32, 32)) { printf("mc fail\n"); return 1; }
   printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
   printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
   OSMesaDestroyContext(ctx);
   return 0;
}
