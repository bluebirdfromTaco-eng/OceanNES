#include "../src/nes.h"
#include <stdio.h>
#include <stdlib.h>
static Nes nes;
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");fseek(f,0,2);long sz=ftell(f);fseek(f,0,0);uint8_t*d=malloc(sz);if(fread(d,1,sz,f)){};char e[100];nes_load(&nes,d,sz,e,100);
int fr=atoi(argv[2]);extern int apu_debug; for(int i=0;i<fr;i++){ if(i==fr-1) apu_debug=1;nes_run_frame(&nes); if(i%60==0)printf("f%d pc=%04X\n",i,nes.cpu.pc);}
printf("text:");for(int i=0;i<300;i++){uint8_t c=bus_peek(&nes,0x6004+i);if(!c)break;putchar(c);}puts("");}
