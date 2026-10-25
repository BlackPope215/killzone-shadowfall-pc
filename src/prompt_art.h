/* SPDX-License-Identifier: GPL-2.0-or-later
 * Prompt artwork from pearlxcore/KillzoneShadowFall-PC (src/kzsf_prompt_art.h): drawn in code,
 * monochrome; no game textures or fonts. */
#ifndef GOW3_PROMPT_ART_H
#define GOW3_PROMPT_ART_H
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Five-bit rows, seven rows per letter/digit. Small labels are scaled to fit
 * the native icon rectangle rather than extending over the action text. */
static const uint8_t gow3_prompt_letters[36][7]={
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
 {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14}
};
static inline unsigned gow3_prompt_font_row(char c,unsigned row) {
    if(c>='A' && c<='Z') return gow3_prompt_letters[c-'A'][row];
    if(c>='0' && c<='9') return gow3_prompt_letters[26+c-'0'][row];
    if(c=='-') return row==3 ? 14u : 0u;
    if(c=='.') return row==6 ? 4u : 0u;
    return 0;
}

/* Xbox-style controller buttons (kind 4), drawn here as well: monochrome like the game's HUD
 * glyphs, a dark fill, a white outline and a white symbol, antialiased from distances. Shapes
 * are in pixels, y down. */
typedef struct { float x0,y0,x1,y1; } Gow3PromptSegment;
enum { GOW3_PROMPT_SEGMENTS=96 };

static inline float gow3_prompt_clamp01(float v) { return v<0.0f ? 0.0f : v>1.0f ? 1.0f : v; }
/* Distance from (px, py) to a box centred at (cx, cy) with half size (hx, hy) and corner radii
 * top-left, top-right, bottom-right, bottom-left; negative inside. */
static inline float gow3_prompt_box(float px,float py,float cx,float cy,float hx,float hy,const float r[4]) {
    float x=px-cx,y=py-cy;
    float radius=y<0.0f ? (x<0.0f ? r[0] : r[1]) : (x<0.0f ? r[3] : r[2]);
    float qx=fabsf(x)-hx+radius,qy=fabsf(y)-hy+radius;
    float ox=qx>0.0f ? qx : 0.0f,oy=qy>0.0f ? qy : 0.0f;
    float inside=qx>qy ? qx : qy;
    return sqrtf(ox*ox+oy*oy)+(inside<0.0f ? inside : 0.0f)-radius;
}
static inline float gow3_prompt_segment(float px,float py,const Gow3PromptSegment *s) {
    float dx=s->x1-s->x0,dy=s->y1-s->y0,len=dx*dx+dy*dy;
    float t=len>0.0f ? gow3_prompt_clamp01(((px-s->x0)*dx+(py-s->y0)*dy)/len) : 0.0f;
    float ex=px-s->x0-t*dx,ey=py-s->y0-t*dy;
    return sqrtf(ex*ex+ey*ey);
}
/* An elliptical arc from angle a0 to a1 (degrees, y down) as segments; returns the new count. */
static inline unsigned gow3_prompt_arc(Gow3PromptSegment *out,unsigned n,float cx,float cy,float rx,float ry,
                                       float a0,float a1) {
    const unsigned steps=10;
    for(unsigned i=0;i<steps && n<GOW3_PROMPT_SEGMENTS;++i) {
        float t0=(a0+(a1-a0)*i/steps)*0.0174532925f,t1=(a0+(a1-a0)*(i+1)/steps)*0.0174532925f;
        out[n++]=(Gow3PromptSegment){cx+rx*cosf(t0),cy+ry*sinf(t0),cx+rx*cosf(t1),cy+ry*sinf(t1)};
    }
    return n;
}
static inline unsigned gow3_prompt_line(Gow3PromptSegment *out,unsigned n,float x0,float y0,float x1,float y1) {
    if(n<GOW3_PROMPT_SEGMENTS) out[n++]=(Gow3PromptSegment){x0,y0,x1,y1};
    return n;
}
/* One stroke letter in the box (x, y) of size w x h. */
static inline unsigned gow3_prompt_letter(Gow3PromptSegment *o,unsigned n,char c,float x,float y,float w,float h) {
#define P(px,py) x+(px)*w,y+(py)*h
    switch(c) {
    case 'A': n=gow3_prompt_line(o,n,P(0,1),P(0.5f,0)); n=gow3_prompt_line(o,n,P(0.5f,0),P(1,1));
              n=gow3_prompt_line(o,n,P(0.2f,0.64f),P(0.8f,0.64f)); break;
    case 'B': n=gow3_prompt_line(o,n,P(0,0),P(0,1)); n=gow3_prompt_line(o,n,P(0,0),P(0.55f,0));
              n=gow3_prompt_arc(o,n,x+0.55f*w,y+0.24f*h,0.4f*w,0.24f*h,-90,90);
              n=gow3_prompt_line(o,n,P(0,0.48f),P(0.6f,0.48f));
              n=gow3_prompt_arc(o,n,x+0.6f*w,y+0.74f*h,0.4f*w,0.26f*h,-90,90);
              n=gow3_prompt_line(o,n,P(0,1),P(0.6f,1)); break;
    case 'X': n=gow3_prompt_line(o,n,P(0,0),P(1,1)); n=gow3_prompt_line(o,n,P(1,0),P(0,1)); break;
    case 'Y': n=gow3_prompt_line(o,n,P(0,0),P(0.5f,0.5f)); n=gow3_prompt_line(o,n,P(1,0),P(0.5f,0.5f));
              n=gow3_prompt_line(o,n,P(0.5f,0.5f),P(0.5f,1)); break;
    case 'L': n=gow3_prompt_line(o,n,P(0,0),P(0,1)); n=gow3_prompt_line(o,n,P(0,1),P(0.9f,1)); break;
    case 'R': n=gow3_prompt_line(o,n,P(0,0),P(0,1)); n=gow3_prompt_line(o,n,P(0,0),P(0.55f,0));
              n=gow3_prompt_arc(o,n,x+0.55f*w,y+0.25f*h,0.42f*w,0.25f*h,-90,90);
              n=gow3_prompt_line(o,n,P(0,0.5f),P(0.55f,0.5f)); n=gow3_prompt_line(o,n,P(0.45f,0.5f),P(1,1)); break;
    case 'T': n=gow3_prompt_line(o,n,P(0,0),P(1,0)); n=gow3_prompt_line(o,n,P(0.5f,0),P(0.5f,1)); break;
    case 'S': n=gow3_prompt_arc(o,n,x+0.5f*w,y+0.25f*h,0.48f*w,0.25f*h,-20,-270);
              n=gow3_prompt_arc(o,n,x+0.5f*w,y+0.75f*h,0.5f*w,0.25f*h,-90,160); break;
    default: break;
    }
#undef P
    return n;
}
/* Letters centred at (cx, cy), cap height h. */
static inline unsigned gow3_prompt_text(Gow3PromptSegment *o,unsigned n,const char *text,float cx,float cy,float h) {
    const float lw=h*0.72f,gap=h*0.32f;
    const size_t len=strlen(text);
    float x=cx-(len*lw+(len-1)*gap)*0.5f;
    for(size_t i=0;i<len;++i,x+=lw+gap) n=gow3_prompt_letter(o,n,text[i],x,cy-h*0.5f,lw,h);
    return n;
}

/* A button's art in rect: label A B X Y LB RB LT RT LS RS MENU VIEW UP DOWN LEFT RIGHT. */
static inline int gow3_prompt_pad_art(uint32_t *pixels,unsigned width,const int32_t rect[4],const char *label) {
    const float x0=(float)rect[0],y0=(float)rect[1],w=(float)(rect[2]-rect[0]),h=(float)(rect[3]-rect[1]);
    const float cx=x0+w*0.5f,cy=y0+h*0.5f,size=(w<h ? w : h)-4.0f;
    const float outline=size*0.07f>2.0f ? size*0.07f : 2.0f,stroke=size*0.085f>1.6f ? size*0.085f : 1.6f;
    Gow3PromptSegment seg[GOW3_PROMPT_SEGMENTS];
    unsigned n=0;
    /* The shape: 0 circle, 1 bumper, 2 trigger, 3 d-pad. */
    int shape=0,lit=-1;
    float hx=size*0.5f,hy=size*0.5f,radius[4];
    if(!strcmp(label,"A") || !strcmp(label,"B") || !strcmp(label,"X") || !strcmp(label,"Y")) {
        n=gow3_prompt_text(seg,n,label,cx,cy,size*0.46f);
    } else if(!strcmp(label,"LS") || !strcmp(label,"RS")) {
        n=gow3_prompt_text(seg,n,label,cx,cy,size*0.34f);
    } else if(!strcmp(label,"LB") || !strcmp(label,"RB")) {
        shape=1;
        hx=(w-4.0f)*0.5f; if(hx>size*0.75f) hx=size*0.75f;
        hy=size*0.32f;
        const float big=hy*1.2f,small=hy*0.35f;
        radius[0]=label[0]=='L' ? big : small; radius[1]=label[0]=='L' ? small : big;
        radius[2]=radius[3]=small;
        n=gow3_prompt_text(seg,n,label,cx,cy+hy*0.05f,hy*0.95f);
    } else if(!strcmp(label,"LT") || !strcmp(label,"RT")) {
        shape=2;
        hx=size*0.4f; hy=size*0.48f;
        radius[0]=radius[1]=hx*0.9f; radius[2]=radius[3]=hx*0.25f;
        n=gow3_prompt_text(seg,n,label,cx,cy+hy*0.14f,size*0.25f);
    } else if(!strcmp(label,"MENU")) {
        for(int i=-1;i<=1;++i)
            n=gow3_prompt_line(seg,n,cx-size*0.2f,cy+i*size*0.15f,cx+size*0.2f,cy+i*size*0.15f);
    } else if(!strcmp(label,"VIEW")) {
        const float s=size*0.2f,d=size*0.07f;
        n=gow3_prompt_line(seg,n,cx-s+d,cy-s-d,cx+s+d,cy-s-d); n=gow3_prompt_line(seg,n,cx+s+d,cy-s-d,cx+s+d,cy+s-d);
        n=gow3_prompt_line(seg,n,cx-s+d,cy-s-d,cx-s+d,cy-s+d);
        n=gow3_prompt_line(seg,n,cx-s-d,cy-s+d,cx+s-d,cy-s+d); n=gow3_prompt_line(seg,n,cx+s-d,cy-s+d,cx+s-d,cy+s+d);
        n=gow3_prompt_line(seg,n,cx+s-d,cy+s+d,cx-s-d,cy+s+d); n=gow3_prompt_line(seg,n,cx-s-d,cy+s+d,cx-s-d,cy-s+d);
    } else if(!strcmp(label,"UP")) { shape=3; lit=0; }
    else if(!strcmp(label,"RIGHT")) { shape=3; lit=1; }
    else if(!strcmp(label,"DOWN")) { shape=3; lit=2; }
    else if(!strcmp(label,"LEFT")) { shape=3; lit=3; }
    else return 0;
    const float arm=size*0.2f,reach=size*0.5f,corner[4]={arm*0.3f,arm*0.3f,arm*0.3f,arm*0.3f};
    for(int y=rect[1];y<rect[3];++y) for(int x=rect[0];x<rect[2];++x) {
        const float px=x+0.5f,py=y+0.5f;
        float d,sym=1e9f;
        if(shape==0) d=sqrtf((px-cx)*(px-cx)+(py-cy)*(py-cy))-size*0.5f;
        else if(shape==3) {
            /* A plus: the lit direction's arm filled white. */
            float v=gow3_prompt_box(px,py,cx,cy,arm,reach,corner),hz=gow3_prompt_box(px,py,cx,cy,reach,arm,corner);
            d=v<hz ? v : hz;
            const float ax=lit==1 ? 1.0f : lit==3 ? -1.0f : 0.0f,ay=lit==0 ? -1.0f : lit==2 ? 1.0f : 0.0f;
            const float near=arm*0.35f,far=reach-outline*1.7f,half=(far-near)*0.5f;
            const float bx=cx+ax*(near+half),by=cy+ay*(near+half);
            const float rr[4]={arm*0.2f,arm*0.2f,arm*0.2f,arm*0.2f};
            sym=gow3_prompt_box(px,py,bx,by,ax ? half : arm-outline*1.7f,ay ? half : arm-outline*1.7f,rr)+stroke*0.5f;
        } else d=gow3_prompt_box(px,py,cx,cy,hx,hy,radius);
        for(unsigned i=0;i<n;++i) { float s=gow3_prompt_segment(px,py,&seg[i]); if(s<sym) sym=s; }
        const float fill=gow3_prompt_clamp01(0.5f-d);
        if(fill<=0.0f) continue;
        const float edge=gow3_prompt_clamp01(0.5f-(fabsf(d+outline*0.5f)-outline*0.5f));
        const float mark=gow3_prompt_clamp01(0.5f-(sym-stroke*0.5f));
        /* A dark translucent fill under a white outline and symbol (straight alpha). */
        float white=edge>mark ? edge : mark;
        float a=fill*(0.44f+0.56f*white);
        float c=a>0.0f ? fill*white/a : 0.0f;
        if(c>1.0f) c=1.0f;
        const uint32_t level=(uint32_t)(c*255.0f+0.5f),alpha=(uint32_t)(a*255.0f+0.5f);
        pixels[(size_t)y*width+x]=alpha<<24|level<<16|level<<8|level;
    }
    return 1;
}

static inline int gow3_prompt_art(uint32_t *pixels,unsigned width,unsigned height,
                                  const int32_t rect[4],int kind,const char *label) {
    if(!pixels || !rect || rect[0]<0 || rect[1]<0 || rect[2]<=rect[0] || rect[3]<=rect[1] ||
       (unsigned)rect[2]>width || (unsigned)rect[3]>height || kind<1 || kind>4) return 0;
    int w=rect[2]-rect[0],h=rect[3]-rect[1];
    if(w<20 || h<20) return 0;
    if(kind==4) return label && gow3_prompt_pad_art(pixels,width,rect,label);
    const int x0=rect[0]+2,x1=rect[2]-3,y0=rect[1]+3,y1=rect[3]-4;
    if(kind==1) {
        size_t len=label ? strlen(label) : 0;
        if(!len || len>8) return 0;
        for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
            int edge=x<x0+2 || x>x1-2 || y<y0+2 || y>y1-2;
            if((x==x0 || x==x1) && (y==y0 || y==y1)) continue;
            pixels[(size_t)y*width+x]=edge ? UINT32_C(0xe0ffffff) : UINT32_C(0x70000000);
        }
        int logical=(int)len*6-1,tw=logical*2,th=14;
        if(tw>w-10) tw=w-10;
        if(th>h-12) th=h-12;
        int left=rect[0]+(w-tw)/2,top=rect[1]+(h-th)/2;
        for(int y=0;y<th;++y) for(int x=0;x<tw;++x) {
            unsigned sx=(unsigned)(x*logical/tw),row=(unsigned)(y*7/th);
            if(sx%6<5 && (gow3_prompt_font_row(label[sx/6],row) & (1u<<(4-sx%6))))
                pixels[(size_t)(top+y)*width+left+x]=UINT32_C(0xffffffff);
        }
    } else {
        /* Mouse silhouette: a rounded top, outlined body, two buttons and
         * wheel. The selected left/right button is filled bright white. */
        int mw=h*3/5;if(mw>w-8) mw=w-8;
        int mh=h-6,left=rect[0]+(w-mw)/2,top=rect[1]+3;
        int radius=mw/3,mid=left+mw/2,split=top+mh*2/5;
        for(int y=0;y<mh;++y) for(int x=0;x<mw;++x) {
            int inset=y<radius ? (radius-y)/2 : y>=mh-radius ? (y-mh+radius)/2 : 0;
            if(x<inset || x>=mw-inset) continue;
            int edge=x<inset+2 || x>=mw-inset-2 || y<2 || y>=mh-2;
            int selected=top+y<split && (kind==2 ? left+x<mid : left+x>=mid);
            uint32_t colour=edge || selected ? UINT32_C(0xffffffff) : UINT32_C(0x70383838);
            if(!edge && ((top+y==split || top+y==split+1) ||
               (top+y<split && left+x>=mid-1 && left+x<=mid))) colour=UINT32_C(0xff000000);
            pixels[(size_t)(top+y)*width+left+x]=colour;
        }
    }
    return 1;
}
#endif
