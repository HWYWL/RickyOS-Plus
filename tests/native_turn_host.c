// Execute the production schedule, allocation/fallback and feed-line code on a host.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ricky_native_turn.h>
#include <waveforms/my_waveform.h>
#ifndef RICKYOS_PLUS_READER_QUALITY
#define RICKYOS_PLUS_READER_QUALITY 0
#endif
#ifndef RICKYOS_PLUS_READER_EARLY_ERASE
#define RICKYOS_PLUS_READER_EARLY_ERASE 0
#endif

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA 8
#define MODE_PACKING_1PPB_DIFFERENCE 0x100
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define DRAM_ATTR
#define IRAM_ATTR
typedef struct { int x,y,width,height; } EpdRect;
typedef void (*lut_func_t)(const uint32_t*, uint8_t*, const uint8_t*, uint32_t);
typedef void (*lut_build_func_t)(uint8_t*, const EpdWaveformPhases*, int);
typedef struct { lut_build_func_t build_func; lut_func_t lookup_func; } LutFunctionPair;
typedef struct {
    EpdRect area, crop_to;
    int display_width, display_height, mode, current_frame, cycle_frames;
    uint8_t *turn_luts, *turn_delays, *turn_window;
    int turn_phases, turn_cols, turn_producer_tick, turn_window_tick;
    bool reader_text_finish;
    RickyNativeTurnRegion turn_region;
    lut_func_t lut_lookup_func;
    lut_build_func_t lut_build_func;
} RenderContext_t;
static int fail_at, allocations, live, last_caps;
static bool real_wave;
static void* heap_caps_aligned_alloc(int alignment, size_t bytes, int caps) {
    last_caps = caps;
    if (++allocations == fail_at) return NULL;
    void* p = NULL;
    if (posix_memalign(&p, alignment, bytes)) return NULL;
    ++live; return p;
}
static void* heap_caps_malloc(size_t bytes, int caps) {
    return heap_caps_aligned_alloc(16, bytes, caps);
}
static void heap_caps_free(void* p) { if (p) { --live; free(p); } }
static void build(uint8_t* lut, const EpdWaveformPhases* phases, int frame) {
    assert(frame >= 0 && frame < phases->phases);
    for (int i=0;i<256;i++) {
        const int to=i>>4,from=i&15;
        ((uint32_t*)lut)[i] = real_wave
            ? (phases->luts[(frame*16+to)*4+from/4]>>(6-2*(from%4)))&3
            : (i + frame) % 3 + 1;
    }
}
static void lookup(const uint32_t* src, uint8_t* out, const uint8_t* lut, uint32_t pixels) {
    assert(pixels && pixels % 4 == 0);
    const uint8_t* in = (const uint8_t*)src;
    for (unsigned x=0;x<pixels;x+=4) {
        out[x/4] = 0;
        for (int k=0;k<4;k++) out[x/4] |= lut[in[x+k]*4] << (6-2*k);
    }
}
static LutFunctionPair find_lut_functions(int mode, unsigned bytes) {
    (void)mode; (void)bytes; return (LutFunctionPair){build,lookup};
}
#include <ricky_native_turn.inc>
#include <output_common/ricky_turn_line.h>

static RenderContext_t context(int w, int h) {
    RenderContext_t c = {.area={0,0,w,h},.crop_to={0,0,w,h},
        .display_width=w,.display_height=h,.mode=MODE_PACKING_1PPB_DIFFERENCE,
        .cycle_frames=37,.lut_build_func=build,.lut_lookup_func=lookup};
    return c;
}
static void exercise(int w, int h, int phases) {
    uint8_t* src = malloc(w);
    uint8_t* out = malloc(w/4+16);
    int* count = calloc((size_t)w*h,sizeof(int));
    assert(src && out && count);
    for (int x=0;x<w;x++) src[x] = (x*19)%256;
    for (int style=1;style<=3;style++) for (int o=0;o<4;o++) for (int right=0;right<2;right++) {
        RenderContext_t c = context(w,h);
        const EpdWaveformPhases wave = {.phases=phases};
        allocations = fail_at = 0;
        ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=style,.orientation=o,.from_right=right,
                            .has_origin=style==3,.origin_x=371,.origin_y=693});
        assert(c.turn_phases == phases && c.cycle_frames == phases+ricky_reader_turn_spread((RickyNativeTurn){.style=style}) && live == 2);
        assert(c.turn_window == s_native_turn_window && sizeof(s_native_turn_window) == RICKY_NATIVE_HOT_SLOTS * 256);
        assert(last_caps == MALLOC_CAP_SPIRAM); // no per-turn HP allocation
        const RickyNativeTurn selected={.style=style,.orientation=o,.from_right=right,
            .has_origin=style==3,.origin_x=371,.origin_y=693};
        for(int row=0;row<(h+15)/16;row++)for(int col=0;col<c.turn_cols;col++) {
            const int px=col*16+8<w?col*16+8:w-1;
            const int py=row*16+8<h?row*16+8:h-1;
            const int axis=ricky_turn_axis(selected,px,py,w,h);
            assert(c.turn_delays[row*c.turn_cols+col]==ricky_ripple_delay(axis,ricky_reader_turn_spread(selected)));
        }
        memset(count,0,(size_t)w*h*sizeof(int));
        for (int tick=0;tick<c.cycle_frames;tick++) {
            c.current_frame = tick - 1; // ISR may still be draining the preceding phase.
            c.turn_producer_tick = tick;
            ricky_turn_prepare_window(&c);
            for (int y=0;y<h;y++) {
                memset(out,0xa5,w/4+16);
                ricky_turn_line(&c,(uint32_t*)src,out,y);
                for (int x=0;x<w;x++) {
                    int phase=ricky_turn_phase(tick,c.turn_delays[y/16*c.turn_cols+x/16],phases);
                    int expected=phase<0 ? 0 : (src[x]+phase)%3+1;
                    assert(((out[x/4]>>(6-2*(x%4)))&3) == expected);
                    if (phase>=0) assert(count[y*w+x]++ == phase);
                }
                for (int x=w/4;x<w/4+16;x++) assert(out[x] == 0xa5);
            }
        }
        for (int i=0;i<w*h;i++) assert(count[i] == phases);
        ricky_native_end(&c);
        assert(!live && !c.turn_luts && !c.turn_delays && !c.turn_phases);
    }
    free(src); free(out); free(count);
}
static void exercise_ui(void) {
    enum { W=96,H=64,N=37 };
    uint32_t source[W/4];
    uint8_t output[W/4+8];
    int counts[W*H];
    for (int x=0;x<W;x++) ((uint8_t*)source)[x]=(x*17)%256;
    const EpdWaveformPhases wave={.phases=N};
    for (int o=0;o<4;o++) for(int right=0;right<2;right++) {
        RenderContext_t c=context(W,H);
        const RickyNativeTurnRegion r={7,9,79,41,RICKY_UI_TURN_SPREAD}; // deliberately unaligned edges
        c.turn_region=r;
        allocations=fail_at=0;
        ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=1,.orientation=o,.from_right=right});
        assert(c.turn_phases==N && c.cycle_frames==N+RICKY_UI_TURN_SPREAD);
        assert(c.turn_window == s_native_turn_window && live==2);
        assert(last_caps == MALLOC_CAP_SPIRAM);
        memset(counts,0,sizeof(counts));
        for(int tick=0;tick<c.cycle_frames;tick++) {
            c.current_frame=tick - 1;
            c.turn_producer_tick=tick;
            ricky_turn_prepare_window(&c);
            for(int y=0;y<H;y++) {
                memset(output,0xa5,sizeof(output));
                ricky_turn_line(&c,source,output,y);
                for(int x=0;x<W;x++) {
                    const bool content=x>=r.x && x<r.x+r.width && y>=r.y && y<r.y+r.height;
                    int phase=content ? ricky_turn_phase(tick,c.turn_delays[y/16*c.turn_cols+x/16],N)
                                      : ricky_turn_phase(tick,0,N);
                    const int expected=phase<0 ? 0 : (((uint8_t*)source)[x]+phase)%3+1;
                    assert(((output[x/4]>>(6-2*(x%4)))&3)==expected);
                    if(phase>=0) assert(counts[y*W+x]++==phase);
                }
                for(unsigned i=W/4;i<sizeof(output);i++) assert(output[i]==0xa5);
            }
        }
        for(int i=0;i<W*H;i++) assert(counts[i]==N);
        ricky_native_end(&c);
        assert(!live && !c.turn_region.spread);
    }
    for(int oom=1;oom<=2;oom++) {
        RenderContext_t c=context(W,H);
        c.turn_region=(RickyNativeTurnRegion){7,9,79,41,RICKY_UI_TURN_SPREAD};
        allocations=0;fail_at=oom;
        ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=1,.orientation=0,.from_right=true});
        assert(!live && !c.turn_phases && !c.turn_window && c.cycle_frames==37);
    }
    fail_at=0;
    for(int bad=0;bad<6;bad++) {
        RenderContext_t c=context(W,H);
        c.turn_region=(RickyNativeTurnRegion){1,1,70,40,4};
        if(bad==0)c.turn_region.x=-1;
        if(bad==1)c.turn_region.width=0;
        if(bad==2)c.turn_region.y=H;
        if(bad==3)c.turn_region.height=H+1;
        if(bad==4)c.turn_region.width=INT32_MAX;
        if(bad==5)c.turn_region.spread=255;
        allocations=0;
        ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=1,.orientation=0,.from_right=true});
        assert(!c.turn_phases && !live && allocations==0);
    }
    epd_set_native_ui_turn(0,true,7,9,79,41);
    assert(s_next_native_turn.style==1 && s_next_native_region.spread==RICKY_UI_TURN_SPREAD);
    epd_set_native_turn(0,0,false);
    assert(!s_next_native_region.spread);
}

static int logical_axis(RickyNativeTurn turn, int lx, int ly, int w, int h) {
    int px,py;
    switch(turn.orientation) {
        case 0:px=ly;py=h-1-lx;break;
        case 1:px=w-1-lx;py=h-1-ly;break;
        case 2:px=w-1-ly;py=lx;break;
        default:px=lx;py=ly;break;
    }
    return ricky_turn_axis(turn,px,py,w,h);
}
static void exercise_ripple(void) {
    assert(ricky_turn_isqrt(0)==0 && ricky_turn_isqrt(1)==1);
    assert(ricky_turn_isqrt(UINT64_MAX)==UINT32_MAX);
    for(uint64_t x=1;x<2000;x++) {
        assert(ricky_turn_isqrt(x*x)==x);
        assert(ricky_turn_isqrt(x*x-1)==x-1);
    }
    for(int o=0;o<4;o++) {
        const int w=1216,h=684,lw=(o==0||o==2)?h:w,lh=(o==0||o==2)?w:h;
        RickyNativeTurn t={.style=3,.orientation=o,.from_right=false,.has_origin=true,.origin_x=512,.origin_y=512};
        const int cx=512*(lw-1)/1023,cy=512*(lh-1)/1023;
        const RickyRippleGeometry geometry=ricky_ripple_geometry(t,w,h);
        for(int y=0;y<h;y+=16) for(int x=0;x<w;x+=16)
            assert(ricky_turn_axis(t,x,y,w,h)==ricky_ripple_axis(geometry,x,y));
        assert(logical_axis(t,cx,cy,w,h)==0);
        const int a=logical_axis(t,cx+100,cy,w,h);
        assert(a==logical_axis(t,cx-100,cy,w,h));
        assert(a==logical_axis(t,cx,cy+100,w,h)); // circles, not aspect-stretched ellipses
        assert(a==logical_axis(t,cx,cy-100,w,h));
        int last=-1;
        for(int x=cx;x<lw;x++) {
            const int axis=logical_axis(t,x,cy,w,h);
            assert(axis>=last && axis<=1024);last=axis;
        }
        t.from_right=true;
        assert(a==logical_axis(t,cx+100,cy,w,h)); // actual touch not mirrored by RTL
        t.has_origin=false;
        assert(logical_axis(t,lw-1,(lh-1)/2,w,h)==0);
        const int right=logical_axis(t,lw-101,(lh-1)/2,w,h);
        t.from_right=false;
        assert(logical_axis(t,0,(lh-1)/2,w,h)==0);
        assert(right==logical_axis(t,100,(lh-1)/2,w,h));
        for(int edge=0;edge<=1023;edge+=1023) {
            t.has_origin=true;t.origin_x=edge;t.origin_y=edge;
            assert(logical_axis(t,edge?lw-1:0,edge?lh-1:0,w,h)==0);
            assert(logical_axis(t,edge?0:lw-1,edge?0:lh-1,w,h)==1024);
        }
    }
    epd_set_native_turn(3,0,false);
    epd_set_native_ripple_origin(0,1023);
    assert(s_next_native_turn.has_origin && s_next_native_turn.origin_y==1023);
    epd_set_native_turn(1,0,true);
    assert(!s_next_native_turn.has_origin);
    epd_set_native_ripple_origin(100,100);
    assert(!s_next_native_turn.has_origin);
    epd_set_native_turn(3,0,true);
    epd_set_native_ripple_origin(1024,0);
    assert(!s_next_native_turn.has_origin);
    const EpdWaveformPhases wave={.phases=37};
    for(int oom=1;oom<=2;oom++) {
        RenderContext_t c=context(1216,684);allocations=0;fail_at=oom;
        ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=3,.orientation=0,.from_right=true});
        assert(!live && !c.turn_phases && c.cycle_frames==37);
    }
    fail_at=0;
}
static void exercise_text_finish(void) {
    my_waveform_init();
    const EpdWaveformPhases* wave=NULL;
    for(int i=0;i<MY_WAVEFORM.num_modes;i++)
        if(MY_WAVEFORM.mode_data[i]->type==MY_MODE_READER_TURN)
            wave=MY_WAVEFORM.mode_data[i]->range_data[0];
    assert(wave);
    const int frames=wave->phases;
    assert(frames==(RICKYOS_PLUS_READER_QUALITY?48:37));
    real_wave=true;
    uint8_t original[frames*1024];
    for(int p=0;p<frames;p++)build(original+p*1024,wave,p);
    uint32_t input[64];uint8_t output[64];
    for(int pair=0;pair<256;pair++)((uint8_t*)input)[pair]=pair;
    for(int style=0;style<=3;style++)for(int orientation=0;orientation<4;orientation++) {
        RenderContext_t c=context(256,16);c.cycle_frames=frames;
        c.reader_text_finish=true;allocations=fail_at=0;
        ricky_native_begin(&c,wave,(RickyNativeTurn){.style=style,.orientation=orientation});
        assert(c.turn_phases==frames && c.cycle_frames==(frames+ricky_reader_turn_spread((RickyNativeTurn){.style=style})) && c.reader_text_finish);
        int shifts[256];
        for(int pair=0;pair<256;pair++) {
            const int to=pair>>4,from=pair&15;
            int last=frames-1;while(last>=0&&!original[last*1024+pair*4])last--;
            const int finish=frames-(to==0?8:4);
            int shift=to!=15 && to!=from && last>=0 && last<finish?finish-last:0;
#if RICKYOS_PLUS_READER_EARLY_ERASE
            if(to==15 && from!=15 && last>=0) {
                assert(frames==48);
                int first=0;while(first<frames&&!original[first*1024+pair*4])first++;
                int left=last-27;if(left>first)left=first;
                if(left>0)shift=-left;
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKY_READER_ANY_OVERLAP
                if(style<=3) {  // plain turns overlap too; ripple by its own amount
                    const int overlap=style==3?RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES:RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES;
                    int finish=27+overlap;if(finish>45)finish=45;
                    int delay=finish-(last+shift);
                    if(overlap>0&&delay>0)shift+=delay;
                }
#endif
            }
#endif
            shifts[pair]=shift;
            for(int tick=0;tick<frames;tick++) {
                const int originalPhase=tick-shift;
                const int expected=originalPhase<0||originalPhase>=frames?0:original[originalPhase*1024+pair*4];
                assert(c.turn_luts[tick*1024+pair*4]==expected);
            }
            // Every active pulse AND internal HOLD retains its exact order.
            // Unchanged cells / clearing to white are byte-identical, not muted.
            if(frames==37 && to==0 && from!=0)assert(last<=finish);
            if(shift>0 && to!=15)assert(c.turn_luts[finish*1024+pair*4]!=0);
            if(to==0 && from!=0 && last<=finish)for(int tick=finish+1;tick<frames;tick++)assert(!c.turn_luts[tick*1024+pair*4]);
        }
        for(int tick=0;tick<c.cycle_frames;tick++) {
            c.turn_producer_tick=tick;ricky_turn_prepare_window(&c);
            for(int y=0;y<16;y++) {
                ricky_turn_line(&c,input,output,y);
                for(int pair=0;pair<256;pair++) {
                    int local=ricky_turn_phase(tick,c.turn_delays[(y/16)*c.turn_cols+pair/16],frames);
                    const int originalPhase=local-shifts[pair];
                    const int expected=local<0||originalPhase<0||originalPhase>=frames?0:original[originalPhase*1024+pair*4];
                    assert(((output[pair/4]>>(6-2*(pair%4)))&3)==expected);
                }
            }
        }
        ricky_native_end(&c);assert(!live&&!c.reader_text_finish);
    }
    for(int oom=1;oom<=2;oom++) {
        RenderContext_t c=context(256,16);c.cycle_frames=frames;c.reader_text_finish=true;allocations=0;fail_at=oom;
        ricky_native_begin(&c,wave,(RickyNativeTurn){0});
        assert(!live&&!c.turn_phases&&!c.reader_text_finish&&c.cycle_frames==frames);
    }
    fail_at=0;real_wave=false;
    epd_set_native_reader_text_finish(true);assert(s_next_reader_text_finish);
    epd_set_native_turn(0,0,false);assert(!s_next_reader_text_finish);
    puts("reader AA completion: real waveform x 256 pairs, spatial motion OFF/ON, preserved pulses/white/retained ink and OOM PASS");
}
static void exercise_paper_turn(void) {
    my_waveform_init();
    const EpdWaveformPhases* wave=NULL;
    for(int i=0;i<MY_WAVEFORM.num_modes;i++)
        if(MY_WAVEFORM.mode_data[i]->type==MY_MODE_READER_PAPER_TURN)
            wave=MY_WAVEFORM.mode_data[i]->range_data[0];
    assert(wave);
    const int frames=wave->phases;
    assert(frames==(RICKYOS_PLUS_READER_QUALITY?48:37));
    real_wave=true;
    uint8_t original[frames*1024];
    for(int p=0;p<frames;p++)build(original+p*1024,wave,p);
    uint32_t input[64];uint8_t output[64];
    for(int pair=0;pair<256;pair++)((uint8_t*)input)[pair]=pair;
    for(int style=1;style<=3;style++)for(int orientation=0;orientation<4;orientation++) {
        RenderContext_t c=context(256,16);c.cycle_frames=frames;allocations=fail_at=0;
        ricky_native_begin(&c,wave,(RickyNativeTurn){.style=style,.orientation=orientation});
        assert(c.turn_phases==frames && c.cycle_frames==frames+ricky_reader_turn_spread((RickyNativeTurn){.style=style}) && !c.reader_text_finish);
        // Paper must never get the white-text convergence transformation. All
        // leading erase, drive and internal HOLD pulses remain byte-identical.
        assert(memcmp(c.turn_luts,original,sizeof(original))==0);
        for(int tick=0;tick<c.cycle_frames;tick++) {
            c.turn_producer_tick=tick;ricky_turn_prepare_window(&c);
            for(int y=0;y<16;y++) {
                ricky_turn_line(&c,input,output,y);
                for(int pair=0;pair<256;pair++) {
                    int local=ricky_turn_phase(tick,c.turn_delays[pair/16],frames);
                    int expected=local<0?0:original[local*1024+pair*4];
                    assert(((output[pair/4]>>(6-2*(pair%4)))&3)==expected);
                }
            }
        }
        ricky_native_end(&c);assert(!live);
    }
    RenderContext_t ordinary=context(256,16);ordinary.cycle_frames=frames;allocations=0;
    ricky_native_begin(&ordinary,wave,(RickyNativeTurn){0});
    assert(allocations==0 && !ordinary.turn_phases && ordinary.cycle_frames==frames);
    real_wave=false;
    puts("reading paper: real waveform x 256 pairs x 3 styles x 4 orientations; preserved erase and ordinary fallback PASS");
}
static void exercise_ui_ink_finish(void) {
    my_waveform_init();
    const EpdWaveformPhases* wave=NULL;
    for(int i=0;i<MY_WAVEFORM.num_modes;i++)
        if(MY_WAVEFORM.mode_data[i]->type==MY_MODE_UI_TURN)
            wave=MY_WAVEFORM.mode_data[i]->range_data[0];
    assert(wave && wave->phases==37);
    real_wave=true;
    uint8_t original[37*1024],adjusted[37*1024];
    for(int tick=0;tick<37;tick++)build(original+tick*1024,wave,tick);
    memcpy(adjusted,original,sizeof(original));
    ricky_ui_lock_completion(adjusted,37);
    for(int pair=0;pair<256;pair++) {
        int before[37],after[37],a=0,b=0,last=-1;
        for(int tick=0;tick<37;tick++) {
            int old=original[tick*1024+pair*4],now=adjusted[tick*1024+pair*4];
            if(old)before[a++]=old;
            if(now){after[b++]=now;last=tick;}
        }
        assert(a==b && memcmp(before,after,(size_t)a*sizeof(int))==0);
        if(b) assert(last==((pair>>4)==0?29:33));
        if((pair>>4)==(pair&15)) {
#if RICKYOS_PLUS_UI_WHITE_MAINTENANCE
            if(pair==255) {
                assert(b==1 && after[0]==2 && last==33); // UI white maintenance only.
            } else {
                assert(!b);
            }
#else
            assert(!b); // Default: retained cells remain HOLD.
#endif
        }
        // Full sequence, including internal HOLDs, shifts without pulse edits.
        int first=0,end=36;
        while(first<37 && !original[first*1024+pair*4])first++;
        while(end>=first && !original[end*1024+pair*4])end--;
        if(first<37) {
            int shift=((pair>>4)==0?29:33)-end;
            for(int tick=first;tick<=end;tick++)
                assert(original[tick*1024+pair*4]==adjusted[(tick+shift)*1024+pair*4]);
        }
    }
    real_wave=false;
    puts("UI black finish: actual 256 transitions, pulse counts/order/internal HOLDs preserved, black tail 7 PASS");
}
int main(void) {
    exercise_ui_ink_finish();
    exercise_text_finish();
    exercise_paper_turn();
    exercise_ripple();
    // UI motion gets nine monotonic positions in every logical orientation;
    // its schedule is independent of the accepted smoothstep reader curve.
    for (int o=0;o<4;o++) for(int right=0;right<2;right++) {
        RickyNativeTurn turn={.style=1,.orientation=o,.from_right=right};
        int seen=0,last=right?RICKY_UI_TURN_SPREAD:0;
        int lw=(o==0||o==2)?684:1216;
        for(int lx=0;lx<lw;lx++) {
            int px,py;
            switch(o) {
                case 0:px=608;py=683-lx;break;
                case 1:px=1215-lx;py=342;break;
                case 2:px=608;py=lx;break;
                default:px=lx;py=342;break;
            }
            int d=ricky_ui_turn_delay(turn,px,py,1216,684,RICKY_UI_TURN_SPREAD);
            assert(d>=0&&d<=RICKY_UI_TURN_SPREAD);
            assert(right?d<=last:d>=last);last=d;seen|=1<<d;
        }
        assert(seen==(1<<(RICKY_UI_TURN_SPREAD+1))-1);
    }
    // Every pixel receives every local phase exactly once, including partial
    // final tiles, both directions, every orientation and both front shapes.
    exercise(68,49,37);
    exercise(20,17,64);
    exercise(1216,684,8); // native row stride and non-16-divisible native height
    exercise_ui();
    const EpdWaveformPhases wave={.phases=37};
    for (int oom=1;oom<=2;oom++) {
        RenderContext_t c=context(1216,684);
        allocations=0; fail_at=oom;
        ricky_native_begin(&c,&wave,(RickyNativeTurn){.style=1,.orientation=0,.from_right=true});
        assert(!live && !c.turn_phases && c.cycle_frames==37);
    }
    fail_at=allocations=0;
    for (int bad=0;bad<6;bad++) {
        RenderContext_t c=context(1216,684);
        EpdWaveformPhases p=wave;
        RickyNativeTurn turn={.style=1,.orientation=0,.from_right=true};
        int times[37]={0}; times[1]=1;
        if (bad==0) turn.style=0;
        if (bad==1) turn.orientation=4;
        if (bad==2) c.mode=0;
        if (bad==3) c.crop_to.width--;
        if (bad==4) p.phases=65;
        if (bad==5) p.phase_times=times;
        ricky_native_begin(&c,&p,turn);
        assert(!live && !c.turn_phases && allocations==0);
    }
    // Physical -> logical mapping matches GfxRenderer, including RTL reversal.
    for(int o=0;o<4;o++) {
        RickyNativeTurn t={.style=1,.orientation=o,.from_right=false};
        const int leftX=o==1?1215:0, leftY=o==0?683:0;
        const int rightX=o==3?1215:0;
        const int rightY=o==2?683:(o==0?0:leftY);
        assert(ricky_turn_delay(t,leftX,leftY,1216,684)==0);
        assert(ricky_turn_delay(t,rightX,rightY,1216,684)==7);
        t.from_right=true;
        assert(ricky_turn_delay(t,leftX,leftY,1216,684)==7);
        assert(ricky_turn_delay(t,rightX,rightY,1216,684)==0);
    }
    epd_set_native_turn(255,0,true);
    assert(s_next_native_turn.style==0);
    puts("native turn: phase coverage, HOLD, LUT output, rotations, edges, OOM and fallback PASS");
    return 0;
}
