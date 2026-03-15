/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HODL wave chart — newspaper style. White background, bold black text,
 * clean lines. 4x supersampled for smooth anti-aliased rendering.
 * Pure C23 + X11. No external libraries.
 *
 * Build:  cc -std=c23 -O2 -Ilib/util/include -o hodlwave_view \
 *         tools/hodlwave_view.c lib/util/src/bitmap_font.c \
 *         lib/util/src/png_writer.c -lsqlite3 -lm -lX11
 * Usage:  ./hodlwave_view [node.db] [start_year] */

#include "util/bitmap_font.h"
#include "util/png_writer.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define GENESIS_TIME 1478403829LL
#define BUTTERCUP_HT 707000
#define NB 10
#define SS 4

static int64_t h2ts(int h) {
    if (h<=0) return GENESIS_TIME;
    if (h<BUTTERCUP_HT) return GENESIS_TIME+(int64_t)h*150;
    return GENESIS_TIME+(int64_t)BUTTERCUP_HT*150+(int64_t)(h-BUTTERCUP_HT)*75;
}
static const int64_t at[NB]={86400,604800,2592000,2592000LL*3,2592000LL*6,
    31557600LL,31557600LL*2,31557600LL*3,31557600LL*5,0};

static void blend(uint8_t *b,int bw,int bh,int x,int y,uint8_t r,uint8_t g,uint8_t bl2,double a){
    if(x<0||x>=bw||y<0||y>=bh||a<=0)return;if(a>1)a=1;
    int o=(y*bw+x)*3;
    b[o]=(uint8_t)(b[o]*(1-a)+r*a);b[o+1]=(uint8_t)(b[o+1]*(1-a)+g*a);b[o+2]=(uint8_t)(b[o+2]*(1-a)+bl2*a);
}

static void thick_line(uint8_t *buf,int bw,int bh,double x0,double y0,double x1,double y1,
    uint8_t r,uint8_t g,uint8_t b,double radius){
    double dx=x1-x0,dy=y1-y0,len=sqrt(dx*dx+dy*dy);if(len<0.1)return;
    int steps=(int)(len*1.5)+1;
    for(int s=0;s<=steps;s++){double t=(double)s/steps;double cx=x0+dx*t,cy=y0+dy*t;
        int ir=(int)(radius+1);
        for(int iy=-ir;iy<=ir;iy++)for(int ix=-ir;ix<=ir;ix++){
            double dist=sqrt((double)(ix*ix+iy*iy));if(dist>radius+0.5)continue;
            double alpha=dist<radius-0.5?1.0:radius+0.5-dist;
            blend(buf,bw,bh,(int)(cx+ix),(int)(cy+iy),r,g,b,alpha);
        }
    }
}

static void fill_rect(uint8_t *buf,int bw,int bh,int x0,int y0,int w,int h,uint8_t r,uint8_t g,uint8_t b){
    for(int y=y0;y<y0+h&&y<bh;y++)for(int x=x0;x<x0+w&&x<bw;x++)
        if(x>=0&&y>=0){int o=(y*bw+x)*3;buf[o]=r;buf[o+1]=g;buf[o+2]=b;}
}

static void downsample(const uint8_t *src,int sw,int sh,uint8_t *dst,int dw,int dh){
    for(int y=0;y<dh;y++)for(int x=0;x<dw;x++){
        int r=0,g=0,b=0,cnt=0;
        for(int sy=y*SS;sy<(y+1)*SS&&sy<sh;sy++)for(int sx=x*SS;sx<(x+1)*SS&&sx<sw;sx++){
            int o=(sy*sw+sx)*3;r+=src[o];g+=src[o+1];b+=src[o+2];cnt++;}
        int o=(y*dw+x)*3;dst[o]=(uint8_t)(r/cnt);dst[o+1]=(uint8_t)(g/cnt);dst[o+2]=(uint8_t)(b/cnt);
    }
}

/* Bold text: draw at offset positions for fake bold effect */
static void bold_text(uint8_t *buf,int bw,int bh,int x,int y,const char *s,
    uint8_t r,uint8_t g,uint8_t b,int scale){
    for(int dx=0;dx<=scale/3;dx++)for(int dy=0;dy<=scale/4;dy++)
        font_draw_string(buf,bw,bh,x+dx,y+dy,s,r,g,b,scale);
}

int main(int argc, char **argv){
    const char *db_path=argc>1?argv[1]:"/home/bob/.zclassic-c23/node.db";
    int start_year=argc>2?atoi(argv[2]):0;

    sqlite3 *db;
    if(sqlite3_open_v2(db_path,&db,SQLITE_OPEN_READONLY,NULL)){fprintf(stderr,"err\n");return 1;}
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,"SELECT height,value FROM utxos WHERE height>=0 AND height<=3100000 AND value>0",-1,&stmt,NULL);
    size_t cap=2000000,n=0;
    int64_t *uts=malloc(cap*8),*uval=malloc(cap*8);
    while(sqlite3_step(stmt)==SQLITE_ROW){
        if(n>=cap){cap*=2;uts=realloc(uts,cap*8);uval=realloc(uval,cap*8);}
        uts[n]=h2ts(sqlite3_column_int(stmt,0));uval[n]=sqlite3_column_int64(stmt,1);n++;
    }
    sqlite3_finalize(stmt);sqlite3_close(db);
    printf("Loaded %zu UTXOs\n",n);

    int64_t tip_ts=h2ts(3043007),vs=GENESIS_TIME;
    if(start_year>=2017&&start_year<=2025){struct tm sy={0};sy.tm_year=start_year-1900;sy.tm_mday=1;vs=(int64_t)timegm(&sy);}
    int NC=(int)((tip_ts-vs)/(86400*7))+1;
    if(NC>2000)NC=2000;if(NC<10)NC=10;
    printf("Computing %d columns...\n",NC);

    int64_t *cts_arr=malloc((size_t)NC*8);
    for(int c=0;c<NC;c++)cts_arr[c]=vs+(int64_t)c*86400*7;
    int64_t *grid=calloc((size_t)(NC*NB),8);
    for(size_t u=0;u<n;u++){
        if(u%200000==0)printf("  %zu/%zu\n",u,n);
        int64_t ct=uts[u],val=uval[u];int s=0;
        if(ct>cts_arr[0]){int lo=0,hi=NC-1;while(lo<hi){int m=(lo+hi)/2;if(cts_arr[m]<ct)lo=m+1;else hi=m;}s=lo;}
        for(int c=s;c<NC;c++){int64_t age=cts_arr[c]-ct;if(age<0)continue;int b=NB-1;for(int i=0;i<NB-1;i++)if(age<at[i]){b=i;break;}grid[c*NB+b]+=val;}
    }
    free(uts);free(uval);free(cts_arr);

    /* % unmoved > 6mo and > 1yr */
    double *p1y=malloc((size_t)NC*sizeof(double)),*p6m=malloc((size_t)NC*sizeof(double));
    for(int c=0;c<NC;c++){
        int64_t tot=0,o1=0,o6=0;
        for(int b=0;b<NB;b++){tot+=grid[c*NB+b];if(b>=5)o1+=grid[c*NB+b];if(b>=4)o6+=grid[c*NB+b];}
        p1y[c]=tot>0?(double)o1/(double)tot*100:0;p6m[c]=tot>0?(double)o6/(double)tot*100:0;
    }
    /* Smooth */
    for(int pass=0;pass<3;pass++){
        double *t=malloc((size_t)NC*sizeof(double));
        for(int c=0;c<NC;c++){double s=0;int cnt=0;for(int d=-3;d<=3;d++){int cc=c+d;if(cc>=0&&cc<NC){s+=p1y[cc];cnt++;}}t[c]=s/cnt;}
        memcpy(p1y,t,(size_t)NC*sizeof(double));
        for(int c=0;c<NC;c++){double s=0;int cnt=0;for(int d=-3;d<=3;d++){int cc=c+d;if(cc>=0&&cc<NC){s+=p6m[cc];cnt++;}}t[c]=s/cnt;}
        memcpy(p6m,t,(size_t)NC*sizeof(double));free(t);
    }

    /* Y range */
    double ylo=100,yhi=0;
    for(int c=0;c<NC;c++){if(p1y[c]<ylo)ylo=p1y[c];if(p6m[c]>yhi)yhi=p6m[c];}
    ylo=floor(ylo/5)*5-2; if(ylo<0)ylo=0;
    yhi=ceil(yhi/5)*5+2; if(yhi>100)yhi=100;

    /* Screen */
    Display *dpy=XOpenDisplay(NULL);
    if(!dpy){fprintf(stderr,"No X\n");return 1;}
    int scr=DefaultScreen(dpy);
    int sw=DisplayWidth(dpy,scr),sh=DisplayHeight(dpy,scr);
    int DW=sw*92/100,DH=sh*88/100;
    int BW=DW*SS,BH=DH*SS;

    printf("Rendering %dx%d (4x=%dx%d)...\n",DW,DH,BW,BH);

    /* Layout in SS coords */
    int ML=BW*12/100, MR=BW*10/100, MT=BH*20/100, MB=BH*16/100;
    int PW=BW-ML-MR, PH=BH-MT-MB;
    int FS=SS*3; /* font scale for labels */
    int TFS=SS*4; /* title font scale */

    uint8_t *buf=calloc((size_t)(BW*BH*3),1);

    /* WHITE background */
    for(int i=0;i<BW*BH*3;i++)buf[i]=255;

    /* Light gray plot area */
    fill_rect(buf,BW,BH,ML,MT,PW,PH, 248,248,252);

    /* Horizontal gridlines: light gray */
    for(double pct=ylo;pct<=yhi;pct+=5){
        double norm=(pct-ylo)/(yhi-ylo);
        int y=MT+PH-(int)(norm*PH);
        fill_rect(buf,BW,BH,ML,y,PW,SS, 220,220,228);
        /* Label: black bold */
        char lb[8];snprintf(lb,8,"%.0f%%",pct);
        int lx=ML-(int)strlen(lb)*FONT_W*FS-SS*8;
        bold_text(buf,BW,BH,lx,y-FONT_H*FS/2,lb, 40,40,50, FS);
    }

    /* Year gridlines */
    int fy=start_year>0?start_year:2017;
    for(int yr=fy;yr<=2025;yr++){
        struct tm yt={0};yt.tm_year=yr-1900;yt.tm_mday=1;
        double frac=(double)((int64_t)timegm(&yt)-vs)/(tip_ts-vs);
        int x=ML+(int)(frac*PW);if(x<ML||x>=ML+PW)continue;
        fill_rect(buf,BW,BH,x,MT,SS,PH, 220,220,228);
        /* Year label: bold black */
        char yb[8];snprintf(yb,8,"%d",yr);
        bold_text(buf,BW,BH,x-FONT_W*FS*2,MT+PH+SS*10,yb, 40,40,50, FS);
    }

    /* Shaded area between lines */
    for(int px=0;px<PW;px++){
        double cf=(double)px/PW*(NC-1);int c0=(int)cf,c1=c0+1;if(c1>=NC)c1=NC-1;double t=cf-c0;
        double v1=p1y[c0]+(p1y[c1]-p1y[c0])*t;
        double v6=p6m[c0]+(p6m[c1]-p6m[c0])*t;
        int y1=MT+PH-(int)((v1-ylo)/(yhi-ylo)*PH);
        int y6=MT+PH-(int)((v6-ylo)/(yhi-ylo)*PH);
        int x=ML+px;
        for(int y=y6;y<y1;y++) if(y>=MT&&y<MT+PH) blend(buf,BW,BH,x,y, 200,180,240, 0.3);
    }

    /* 6-month line: dark blue, thick */
    for(int px=0;px<PW-1;px++){
        double cf0=(double)px/PW*(NC-1),cf1=(double)(px+1)/PW*(NC-1);
        int c0a=(int)cf0,c1a=c0a+1;if(c1a>=NC)c1a=NC-1;
        int c0b=(int)cf1,c1b=c0b+1;if(c1b>=NC)c1b=NC-1;
        double v0=p6m[c0a]+(p6m[c1a]-p6m[c0a])*(cf0-c0a);
        double v1=p6m[c0b]+(p6m[c1b]-p6m[c0b])*(cf1-c0b);
        thick_line(buf,BW,BH,ML+px,MT+PH-(v0-ylo)/(yhi-ylo)*PH,
            ML+px+1,MT+PH-(v1-ylo)/(yhi-ylo)*PH, 50,80,180, SS*2.0);
    }

    /* 1-year line: dark red, thicker */
    for(int px=0;px<PW-1;px++){
        double cf0=(double)px/PW*(NC-1),cf1=(double)(px+1)/PW*(NC-1);
        int c0a=(int)cf0,c1a=c0a+1;if(c1a>=NC)c1a=NC-1;
        int c0b=(int)cf1,c1b=c0b+1;if(c1b>=NC)c1b=NC-1;
        double v0=p1y[c0a]+(p1y[c1a]-p1y[c0a])*(cf0-c0a);
        double v1=p1y[c0b]+(p1y[c1b]-p1y[c0b])*(cf1-c0b);
        thick_line(buf,BW,BH,ML+px,MT+PH-(v0-ylo)/(yhi-ylo)*PH,
            ML+px+1,MT+PH-(v1-ylo)/(yhi-ylo)*PH, 200,40,40, SS*2.5);
    }

    /* Plot border: solid black */
    fill_rect(buf,BW,BH,ML,MT,PW,SS*2,30,30,40);
    fill_rect(buf,BW,BH,ML,MT+PH,PW,SS*2,30,30,40);
    fill_rect(buf,BW,BH,ML,MT,SS*2,PH,30,30,40);
    fill_rect(buf,BW,BH,ML+PW,MT,SS*2,PH+SS*2,30,30,40);

    /* Title: big bold black */
    bold_text(buf,BW,BH,ML,SS*8, "ZClassic HODL Wave", 20,20,30, TFS);

    /* Subtitle */
    int64_t ftot=0;for(int b=0;b<NB;b++)ftot+=grid[(NC-1)*NB+b];
    char sub[128];snprintf(sub,sizeof(sub),"Percentage of supply unmoved over time");
    bold_text(buf,BW,BH,ML,SS*8+FONT_H*TFS+SS*6, sub, 100,100,115, FS);

    /* Endpoint labels — big, bold, colored to match lines */
    double e1y=p1y[NC-1],e6m=p6m[NC-1];
    int ey1=MT+PH-(int)((e1y-ylo)/(yhi-ylo)*PH);
    int ey6=MT+PH-(int)((e6m-ylo)/(yhi-ylo)*PH);

    /* 1yr label */
    char l1[32];snprintf(l1,32,"%.1f%%",e1y);
    int tw1=(int)strlen(l1)*FONT_W*FS;
    fill_rect(buf,BW,BH,ML+PW+SS*6,ey1-FONT_H*FS/2-SS*2,tw1+SS*12,FONT_H*FS+SS*4, 200,40,40);
    bold_text(buf,BW,BH,ML+PW+SS*12,ey1-FONT_H*FS/2, l1, 255,255,255, FS);

    /* 6mo label */
    char l6[32];snprintf(l6,32,"%.1f%%",e6m);
    int tw6=(int)strlen(l6)*FONT_W*FS;
    fill_rect(buf,BW,BH,ML+PW+SS*6,ey6-FONT_H*FS/2-SS*2,tw6+SS*12,FONT_H*FS+SS*4, 50,80,180);
    bold_text(buf,BW,BH,ML+PW+SS*12,ey6-FONT_H*FS/2, l6, 255,255,255, FS);

    /* Start labels */
    double s1y=p1y[0],s6m=p6m[0];
    int sy1=MT+PH-(int)((s1y-ylo)/(yhi-ylo)*PH);
    int sy6=MT+PH-(int)((s6m-ylo)/(yhi-ylo)*PH);
    char sl1[16];snprintf(sl1,16,"%.0f%%",s1y);
    bold_text(buf,BW,BH,ML+SS*6,sy1+SS*4, sl1, 200,40,40, FS);
    char sl6[16];snprintf(sl6,16,"%.0f%%",s6m);
    bold_text(buf,BW,BH,ML+SS*6,sy6-FONT_H*FS-SS*2, sl6, 50,80,180, FS);

    /* Legend at bottom */
    int lgy=MT+PH+SS*38;
    /* Red line sample + label */
    thick_line(buf,BW,BH,ML,lgy+FONT_H*FS/2,ML+SS*30,lgy+FONT_H*FS/2, 200,40,40, SS*2.5);
    bold_text(buf,BW,BH,ML+SS*36,lgy, "Unmoved > 1 year", 200,40,40, FS);
    /* Blue line sample + label */
    thick_line(buf,BW,BH,ML+PW/2,lgy+FONT_H*FS/2,ML+PW/2+SS*30,lgy+FONT_H*FS/2, 50,80,180, SS*2.0);
    bold_text(buf,BW,BH,ML+PW/2+SS*36,lgy, "Unmoved > 6 months", 50,80,180, FS);

    /* Downsample */
    printf("Downsampling 4x...\n");
    uint8_t *disp=malloc((size_t)(DW*DH*3));
    downsample(buf,BW,BH,disp,DW,DH);
    free(buf);

    png_write_rgb("/tmp/hodlwave_chart.png",disp,(uint32_t)DW,(uint32_t)DH);
    printf("Saved /tmp/hodlwave_chart.png\n");

    /* X11 */
    int depth=DefaultDepth(dpy,scr);Visual *vis=DefaultVisual(dpy,scr);
    char *xdata=malloc((size_t)(DW*DH*4));
    for(int i=0;i<DW*DH;i++){xdata[i*4]=(char)disp[i*3+2];xdata[i*4+1]=(char)disp[i*3+1];xdata[i*4+2]=(char)disp[i*3];xdata[i*4+3]=0;}
    free(disp);free(p1y);free(p6m);free(grid);

    XImage *ximg=XCreateImage(dpy,vis,(unsigned)depth,ZPixmap,0,xdata,(unsigned)DW,(unsigned)DH,32,0);
    Window win=XCreateSimpleWindow(dpy,RootWindow(dpy,scr),(sw-DW)/2,(sh-DH)/2,(unsigned)DW,(unsigned)DH,0,
        WhitePixel(dpy,scr),WhitePixel(dpy,scr));
    XStoreName(dpy,win,"ZClassic HODL Wave");
    XSelectInput(dpy,win,ExposureMask|KeyPressMask|StructureNotifyMask);
    Atom wm_del=XInternAtom(dpy,"WM_DELETE_WINDOW",0);
    XSetWMProtocols(dpy,win,&wm_del,1);
    GC gc=XCreateGC(dpy,win,0,NULL);
    XMapWindow(dpy,win);

    printf("Press Q or Escape to close.\n");
    bool running=true;
    while(running){XEvent ev;XNextEvent(dpy,&ev);
        if(ev.type==Expose&&ev.xexpose.count==0)XPutImage(dpy,win,gc,ximg,0,0,0,0,(unsigned)DW,(unsigned)DH);
        if(ev.type==KeyPress){KeySym k=XLookupKeysym(&ev.xkey,0);if(k==XK_Escape||k==XK_q)running=false;}
        if(ev.type==ClientMessage&&(Atom)ev.xclient.data.l[0]==wm_del)running=false;
    }
    XDestroyImage(ximg);XFreeGC(dpy,gc);XDestroyWindow(dpy,win);XCloseDisplay(dpy);
    return 0;
}
