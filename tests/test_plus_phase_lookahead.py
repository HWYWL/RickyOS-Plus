"""Run the real LCD controller, workers and atomic rings with concurrent host threads.

This checks byte/order/lifetime contracts, not electrical timing or screen quality.
"""
from pathlib import Path
import signal
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
DISPLAY = PORT / 'firmware/rickyos-plus-1.2/components/metalio_board/display'
STUBS = {
    'sdkconfig.h': '#define CONFIG_IDF_TARGET_ESP32S31 1\n',
    'esp_attr.h': '#pragma once\n#define IRAM_ATTR\n',
    'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n',
    'driver/gpio.h': '#pragma once\ntypedef int gpio_num_t;\n',
    'esp_log.h': '#pragma once\nvoid host_error_log(const char*);\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n#define ESP_LOGE(tag,fmt,...) host_error_log(fmt)\n',
    'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
    'rom/cache.h': '#pragma once\n',
    'epd_board.h': '#pragma once\n#include <assert.h>\n#include <stdbool.h>\nvoid epd_set_mode(bool);\n',
    'esp_heap_caps.h': r'''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
static inline void* heap_caps_aligned_alloc(size_t a,size_t n,int caps){
 (void)caps;void* p=NULL;return posix_memalign(&p,a,n)==0?p:NULL;
}
static inline void heap_caps_free(void* p){free(p);}
''',
    'freertos/FreeRTOS.h': r'''#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(n) (n)
#define portYIELD_FROM_ISR() ((void)0)
''',
    'freertos/semphr.h': r'''#pragma once
#include "FreeRTOS.h"
typedef struct HostSemaphore* SemaphoreHandle_t;
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
int xSemaphoreGiveFromISR(SemaphoreHandle_t,BaseType_t*);
''',
    'freertos/task.h': r'''#pragma once
#include "FreeRTOS.h"
typedef struct HostTask* TaskHandle_t;
void xTaskNotifyGive(TaskHandle_t);
void vTaskDelay(TickType_t);
int xPortGetCoreID(void);
''',
}

MAIN = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include "output_common/render_context.h"
#include "output_lcd/render_lcd.h"
#include "output_lcd/lcd_driver.h"
#include "output_lcd/lcd_fault.h"
struct HostSemaphore {pthread_mutex_t mutex;pthread_cond_t cond;int count;};
struct HostTask {pthread_mutex_t mutex;pthread_cond_t cond;int count,id;bool stop;pthread_t thread;};
#ifndef HOST_READER_PHASES
#define HOST_READER_PHASES 37
#endif
#ifndef HOST_READER_SPREAD
#define HOST_READER_SPREAD 7
#endif
#ifndef HOST_MOTION_PERIOD_EXPECTED
#define HOST_MOTION_PERIOD_EXPECTED 0
#endif
static int64_t host_previous_start;
static RenderContext_t ctx;
static struct HostSemaphore frame_sem,feed_sem[2];
static struct HostTask tasks[2];
static _Atomic(line_cb_func_t) source_cb;
static _Atomic(frame_done_func_t) done_cb;
static _Atomic(void*) source_payload;
static _Atomic(void*) done_payload;
static pthread_t scan_thread;
static bool scan_joinable;
static atomic_bool scanning,aborted;
#ifndef RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME
#define LOOKAHEAD_AFTER_FRAME 0
#else
#define LOOKAHEAD_AFTER_FRAME RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME
#endif
static atomic_int scan_count,expected_phase,overlapped,render_mode;
static atomic_int active_feeders;
static int fault,slow,animated,compact,completion_only;
static int animation_tick(int row,int global) {
 if(!animated)return global;
 const bool delayed=animated==1 || (row>=ctx.turn_region.y && row<ctx.turn_region.y+ctx.turn_region.height);
 const int phase=global-(!completion_only&&delayed?(row/16)%(animated==2?8:HOST_READER_SPREAD+1):0);
 return phase>=0 && phase<HOST_READER_PHASES?phase:-1;
}
static atomic_bool fault_taken;
static atomic_bool stalled_row;
static uint8_t* input;
static bool* dirty;
static uint8_t lut[1024],mask[4]={255,15,240,0};
void host_error_log(const char* format){
 if(strstr(format,"cannot reuse render context"))fputs("PRODUCTION_QUIESCE_GUARD\n",stderr);
}
// Reset is a controller operation, after DMA stops and both workers finish.
// Execute the actual atomic ring implementation behind this ownership check.
#define lq_reset lq_reset_unchecked
#include "output_common/line_queue.c"
#undef lq_reset
void lq_reset(LineQueue_t* q){
 assert(!atomic_load(&scanning) && !atomic_load(&active_feeders));
 lq_reset_unchecked(q);
}
int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000LL+t.tv_nsec/1000;}
static void nap(int us){struct timespec t={us/1000000,(us%1000000)*1000};nanosleep(&t,NULL);}
// Host threads have no FreeRTOS priorities. A tight sched_yield() can consume
// the legacy feeder's 30,000-iteration guard before a sleeping scan thread is
// scheduled, especially while a firmware build runs. Give the host scheduler
// time rather than using its loop speed as a physical LCD timing model. The
// actual ISR still rejects every unready row; no readiness wait is added here.
void vTaskDelay(TickType_t t){nap(t?t*1000:20);}
int xPortGetCoreID(void){return 0;}
static void sem_init_host(struct HostSemaphore* s){
 memset(s,0,sizeof(*s));assert(!pthread_mutex_init(&s->mutex,NULL));assert(!pthread_cond_init(&s->cond,NULL));
}
static void sem_give(struct HostSemaphore* s){
 pthread_mutex_lock(&s->mutex);assert(s->count==0);s->count=1;pthread_cond_signal(&s->cond);pthread_mutex_unlock(&s->mutex);
}
int xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t* awoken){(void)awoken;sem_give(s);return pdTRUE;}
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ms){
 if(s==&frame_sem && fault==1 && atomic_load(&scan_count)==2 && !atomic_exchange(&fault_taken,true))return pdFALSE;
 if(s==&frame_sem && fault==9 && !atomic_exchange(&fault_taken,true))return pdFALSE;
 if(fault==5 && s==&feed_sem[0])return pdFALSE; // Lost/stuck worker acknowledgement: must fail closed.
 if(fault==6 && s==&feed_sem[0] && !atomic_exchange(&fault_taken,true))return pdFALSE; // Deadline expires; completion follows.
 pthread_mutex_lock(&s->mutex);
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);
 end.tv_sec+=ms/1000;end.tv_nsec+=(long)(ms%1000)*1000000;
 if(end.tv_nsec>=1000000000){end.tv_sec++;end.tv_nsec-=1000000000;}
 while(!s->count){
   if(!ms || pthread_cond_timedwait(&s->cond,&s->mutex,&end)){pthread_mutex_unlock(&s->mutex);return pdFALSE;}
 }
 s->count=0;pthread_mutex_unlock(&s->mutex);return pdTRUE;
}
void xTaskNotifyGive(TaskHandle_t task){
 pthread_mutex_lock(&task->mutex);task->count++;pthread_cond_signal(&task->cond);pthread_mutex_unlock(&task->mutex);
}
static void* feeder(void* pointer){
 struct HostTask* task=pointer;
 for(;;){
   pthread_mutex_lock(&task->mutex);
   while(!task->count&&!task->stop)pthread_cond_wait(&task->cond,&task->mutex);
   if(task->stop){pthread_mutex_unlock(&task->mutex);return NULL;}
   assert(task->count==1);task->count=0;pthread_mutex_unlock(&task->mutex);
   atomic_fetch_add(&active_feeders,1);
   lcd_calculate_frame(&ctx,task->id);
   atomic_fetch_sub(&active_feeders,1);sem_give(&feed_sem[task->id]);
 }
}
void epd_set_mode(bool value){atomic_store(&render_mode,value);}
bool epd_lcd_set_line_timing(int line,int gate){assert(line==0&&gate==0);return fault!=4;}
// Disabling a callback must not invalidate the payload of an already acquired
// callback. The real device's serialized ISR contract is represented by these
// atomic publications; all phases use the same context payload.
void epd_lcd_line_source_cb(line_cb_func_t cb,void* payload){if(cb)atomic_store(&source_payload,payload);atomic_store(&source_cb,cb);}
void epd_lcd_frame_done_cb(frame_done_func_t cb,void* payload){if(cb)atomic_store(&done_payload,payload);atomic_store(&done_cb,cb);}
static uint8_t sample(int row,int phase,int x){
 const int local=animation_tick(row,phase);
 if(compact) {
   uint8_t result=0;
   for(int lane=0;lane<4;lane++) {
     const int pixel=input[row*16+x*4+lane];
     result|=(local<0?0:(pixel+local)%3+1)<<(6-lane*2);
   }
   return result&mask[x];
 }
 return local<0?0:(uint8_t)((row+local*17+x*31)&255)&mask[x];
}
static void* scanner(void* unused){
 (void)unused;const int phase=atomic_load(&expected_phase);
 if(fault==7 && phase==0){
   // Coordinate the injection, not physical ISR timing: exactly one producer
   // stalls after prefill. All earlier rows are committed before scanning.
   while(!atomic_load(&stalled_row) || !__atomic_load_n(&ctx.line_ready[192],__ATOMIC_ACQUIRE))nap(10);
 }
 uint8_t row_data[4];
 for(int row=0;row<ctx.lines_total && !atomic_load(&aborted) && fault!=8;row++){
   line_cb_func_t cb=atomic_load(&source_cb);if(!cb)break;
   cb(atomic_load(&source_payload),row_data);
   if(!__atomic_load_n(&ctx.error,__ATOMIC_ACQUIRE)) {
     bool drawn=row<ctx.display_height && row>=10 && row<610 && dirty[row];
     for(int x=0;x<4;x++)assert(row_data[x]==(drawn?sample(row,phase,x):0));
   }
   if(row%8==7)nap(140);
 }
 atomic_store(&scanning,false);
 frame_done_func_t done=atomic_load(&done_cb);if(done && fault!=9)done(atomic_load(&done_payload));
 return NULL;
}
int64_t epd_lcd_start_frame_at(int64_t deadline){
 if(scan_joinable){pthread_join(scan_thread,NULL);scan_joinable=false;}
 int phase=atomic_fetch_add(&scan_count,1);assert(ctx.current_frame==phase);
 const bool fixed=(HOST_MOTION_PERIOD_EXPECTED > 0) && animated==1 && !completion_only;
 assert(deadline==(fixed && phase>0 ? host_previous_start+HOST_MOTION_PERIOD_EXPECTED : 0));
 while(deadline>esp_timer_get_time())nap(50);
 atomic_store(&expected_phase,phase);atomic_store(&aborted,false);atomic_store(&scanning,true);
 int64_t started=esp_timer_get_time();host_previous_start=started;
 assert(!pthread_create(&scan_thread,NULL,scanner,NULL));scan_joinable=true;return started;
}
void epd_lcd_start_frame(void){(void)epd_lcd_start_frame_at(0);}
void epd_lcd_abort_frame(void){
 atomic_store(&aborted,true);
 if(scan_joinable){pthread_join(scan_thread,NULL);scan_joinable=false;}
}
void epd_apply_line_mask_VE(uint8_t* row,const uint8_t* bits,int len){for(int x=0;x<len;x++)row[x]&=bits[x];}
void epd_apply_line_mask(uint8_t* row,const uint8_t* bits,int len){epd_apply_line_mask_VE(row,bits,len);}
static void build(uint8_t* dest,const EpdWaveformPhases* phases,int phase){
 assert(phases->phases==ctx.cycle_frames);memset(dest,phase,1024);
}
static void lookup(const uint32_t* source,uint8_t* dest,const uint8_t* table,uint32_t width){
 assert(width==16);const uint8_t* bytes=(const uint8_t*)source;int row=bytes[0]+256*bytes[1];int phase=table[0];
 if(slow&&row==11)nap(3000); // One producer is late: committed-prefix barrier must wait.
 if(fault==7 && phase==0 && row==193){
   atomic_store(&stalled_row,true);
   while(!__atomic_load_n(&ctx.error,__ATOMIC_ACQUIRE))nap(10);
 }
 if(animated)assert(phase==animation_tick(row,ctx.turn_producer_tick));
 const int producer=animated?ctx.turn_producer_tick:phase;
 if(producer>0 && atomic_load(&scanning)&&atomic_load(&expected_phase)==producer-1)atomic_fetch_add(&overlapped,1);
 if((fault==2&&phase==1&&row==100)||(fault==3&&phase==0&&row==10))
   __atomic_fetch_or(&ctx.error,EPD_DRAW_EMPTY_LINE_QUEUE,__ATOMIC_RELEASE);
 for(int x=0;x<4;x++)dest[x]=(uint8_t)((row+phase*17+x*31)&255);
}
int main(int argc,char** argv){
 fault=argc>1?atoi(argv[1]):0;slow=argc>2?atoi(argv[2]):0;const bool gl16=argc>3&&atoi(argv[3]);
 const bool legacy=argc>4&&atoi(argv[4]);
 animated=argc>5?atoi(argv[5]):0;
 completion_only=animated==5;
 compact=animated>2;if(compact)animated-=2;
 if(completion_only)animated=1;
 memset(&ctx,0,sizeof(ctx));ctx.display_width=16;ctx.display_height=684;ctx.lines_total=688;
 ctx.area=(EpdRect){0,0,16,684};ctx.crop_to=(EpdRect){0,10,16,600};
 ctx.mode=(legacy?MODE_GC16:gl16?MODE_GL16:MODE_GL16_FAST)|MODE_PACKING_1PPB_DIFFERENCE;ctx.cycle_frames=48;
 ctx.data_ptr=input=calloc(684,16);dirty=calloc(684,sizeof(bool));assert(input&&dirty);
 for(int row=0;row<684;row++){input[row*16]=row&255;input[row*16+1]=row>>8;dirty[row]=row%7!=0;}
 ctx.drawn_lines=dirty;ctx.line_mask=mask;ctx.conversion_lut=lut;ctx.lut_build_func=build;ctx.lut_lookup_func=lookup;
 if(animated) {
   ctx.turn_phases=HOST_READER_PHASES;ctx.turn_cols=1;ctx.cycle_frames=HOST_READER_PHASES+(completion_only?0:animated==2?8:HOST_READER_SPREAD);
   ctx.reader_text_finish=completion_only || ((HOST_MOTION_PERIOD_EXPECTED > 0) && animated==1);
   ctx.turn_luts=calloc(HOST_READER_PHASES,1024);ctx.turn_delays=calloc(43,1);
   // Padded lookup cases retain fault/slow-peer instrumentation; separate cases
   // execute the actual S31 compact hot converter through both real feeders.
   ctx.turn_window=compact?calloc(HOST_READER_SPREAD+1>9?HOST_READER_SPREAD+1:9,256):NULL;
   assert(ctx.turn_luts && ctx.turn_delays && (!compact || ctx.turn_window));
   for(int p=0;p<HOST_READER_PHASES;p++) {
     if(compact)for(int pair=0;pair<256;pair++)ctx.turn_luts[p*1024+pair*4]=(pair+p)%3+1;
     else memset(ctx.turn_luts+p*1024,p,1024);
   }
   for(int r=0;r<43;r++)ctx.turn_delays[r]=completion_only?0:r%(animated==2?8:HOST_READER_SPREAD+1);
   if(animated==2)ctx.turn_region=(RickyNativeTurnRegion){0,37,16,503,8};
 }
 const int expected_scans=ctx.cycle_frames;
 EpdWaveformPhases phases={animated?HOST_READER_PHASES:48,NULL,NULL};const EpdWaveformPhases* ranges[]={&phases};
 EpdWaveformMode mode={6,1,ranges};const EpdWaveformMode* modes[]={&mode};EpdWaveform wave={1,1,modes,NULL};ctx.waveform=&wave;
 uint8_t* original_ready=ctx.line_ready=calloc(688,1);uint8_t* original_threads=ctx.line_threads=calloc(688,1);
 sem_init_host(&frame_sem);ctx.frame_done=&frame_sem;
 for(int i=0;i<2;i++){
   sem_init_host(&feed_sem[i]);ctx.feed_done_smphr[i]=&feed_sem[i];ctx.line_queues[i]=lq_init(128,4);
   ctx.feed_line_buffers[i]=calloc(16,1);struct HostTask* t=&tasks[i];t->id=i;
   assert(!pthread_mutex_init(&t->mutex,NULL));assert(!pthread_cond_init(&t->cond,NULL));ctx.feed_tasks[i]=t;
   assert(!pthread_create(&t->thread,NULL,feeder,t));
 }
 lcd_do_update(&ctx);
 if(fault==5)return 97; // A returned context is a failure, distinct from production abort().
 if(scan_joinable){pthread_join(scan_thread,NULL);scan_joinable=false;}
 assert(ctx.line_ready==original_ready&&ctx.line_threads==original_threads);
 assert(!atomic_load(&source_cb)&&!atomic_load(&done_cb)&&!atomic_load(&render_mode));
 const uint32_t first_fault=epd_lcd_get_first_fault();
 if(fault==7)assert(first_fault==((193u<<16)|(1u<<8)|LCD_FAULT_ROW_NOT_READY));
 if(fault==8)assert(first_fault==((0u<<16)|(255u<<8)|LCD_FAULT_FRAME_SHORT));
 if(fault==9)assert(first_fault==((65535u<<16)|(255u<<8)|LCD_FAULT_FRAME_TIMEOUT));
 if(fault){assert(ctx.error);}else{
   assert(!ctx.error&&ctx.current_frame==expected_scans&&atomic_load(&scan_count)==expected_scans);
   // Default: the next phase overlaps the frame tail; the opt-in
   // RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME=1 starts it only after the scan.
   if(!compact)assert((legacy||LOOKAHEAD_AFTER_FRAME)?atomic_load(&overlapped)==0:atomic_load(&overlapped)>0);
 }
 if(!fault && (HOST_MOTION_PERIOD_EXPECTED > 0) && animated==1 && !completion_only)
   assert(esp_timer_get_time()-host_previous_start>=HOST_MOTION_PERIOD_EXPECTED); // Last phase settles too.
 // Context reuse / sleep may follow only after ALL callbacks and workers stop.
 assert(!atomic_load(&scanning) && !atomic_load(&active_feeders));
 for(int i=0;i<2;++i){
   assert(!feed_sem[i].count);
   assert(atomic_load(&ctx.line_queues[i].current)==atomic_load(&ctx.line_queues[i].last));
 }
 free(ctx.turn_luts);free(ctx.turn_delays);free(ctx.turn_window);
 ctx.turn_luts=ctx.turn_delays=ctx.turn_window=NULL;ctx.turn_phases=0;ctx.turn_region=(RickyNativeTurnRegion){0};animated=compact=completion_only=0;
 ctx.reader_text_finish=false;
 if(!fault || legacy){
   // Reuse the same context on the legacy GC path after a pipelined cycle.
   fault=0;
   ctx.error=0;ctx.current_frame=0;ctx.cycle_frames=3;phases.phases=3;
   ctx.mode=MODE_GC16|MODE_PACKING_1PPB_DIFFERENCE;atomic_store(&scan_count,0);
   lcd_do_update(&ctx);assert(!ctx.error&&ctx.current_frame==3&&atomic_load(&scan_count)==3);
   if(scan_joinable){pthread_join(scan_thread,NULL);scan_joinable=false;}
   assert(epd_lcd_get_first_fault()==first_fault); // A later successful redraw cannot overwrite first evidence.
 }
 for(int i=0;i<2;i++){
   assert(!feed_sem[i].count);pthread_mutex_lock(&tasks[i].mutex);tasks[i].stop=true;
   pthread_cond_signal(&tasks[i].cond);pthread_mutex_unlock(&tasks[i].mutex);pthread_join(tasks[i].thread,NULL);
   assert(!tasks[i].count);lq_free(&ctx.line_queues[i]);free(ctx.feed_line_buffers[i]);
   pthread_mutex_destroy(&tasks[i].mutex);pthread_cond_destroy(&tasks[i].cond);
   pthread_mutex_destroy(&feed_sem[i].mutex);pthread_cond_destroy(&feed_sem[i].cond);
 }
 assert(!frame_sem.count);
 pthread_mutex_destroy(&frame_sem.mutex);pthread_cond_destroy(&frame_sem.cond);
 free(input);free(dirty);free(original_ready);free(original_threads);
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='plus-phase-lookahead-') as folder:
    work = Path(folder)
    for name, code in STUBS.items():
        dest = work / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(code)
    (work / 'main.c').write_text(MAIN)
    binary = work / 'test'
    compile_args = ['clang', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DRICKYOS_PLUS_PHASE_LOOKAHEAD=1',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unknown-attributes', '-Wno-unused-variable',
                    '-Wno-unused-but-set-variable', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-pthread', '-include', 'assert.h',
                    '-I', str(work), '-I', str(DISPLAY), str(work / 'main.c'),
                    str(DISPLAY / 'output_lcd/render_lcd.c'), str(DISPLAY / 'output_common/render_context.c'),
                    '-o', str(binary)]
    subprocess.run(compile_args, check=True)
    # Ordinary GC/DU scheduling shares the two workers but has its own recovery.
    # A timed-out acknowledgement arriving later must be drained before reuse.
    for fault, slow in ((6, 0), (0, 0), (0, 1), (1, 0), (2, 0), (3, 0), (4, 0), (7, 0), (8, 0), (9, 0)):
        subprocess.run([str(binary), str(fault), str(slow), '0', '1'], check=True, timeout=20)
    for fault, slow in ((0, 0), (0, 1), (1, 0), (2, 0), (3, 0), (4, 0), (7, 0), (8, 0), (9, 0)):
        subprocess.run([str(binary), str(fault), str(slow)], check=True, timeout=20)
    # The opt-in after-frame schedule stays buildable and correct for A/B.
    subprocess.run([*compile_args, '-DRICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME=1'], check=True)
    for fault, slow in ((0, 0), (0, 1), (1, 0), (2, 0), (3, 0), (4, 0), (7, 0), (8, 0), (9, 0)):
        subprocess.run([str(binary), str(fault), str(slow)], check=True, timeout=20)
    subprocess.run(compile_args, check=True)
    # UI area updates (GL16) take the same pipeline, with the same exact order and cleanup.
    for fault, slow in ((0, 0), (0, 1), (2, 0)):
        subprocess.run([str(binary), str(fault), str(slow), '1'], check=True, timeout=20)
    # Real staggered row generation while the ISR still drains the prior phase.
    # Both reader and UI tick/window must be producer-owned, not current_frame.
    for animation in (1, 2):
        for fault, slow in ((0,0),(0,1),(1,0),(2,0),(4,0),(6,0),(8,0),(9,0)):
            subprocess.run([str(binary),str(fault),str(slow),'1','0',str(animation)],check=True,timeout=20)
    for animation in (3, 4, 5):
        for fault in (0, 1, 4, 6, 8, 9):
            subprocess.run([str(binary),str(fault),'0','1','0',str(animation)],check=True,timeout=20)
    def check_stuck_worker(*args):
        fatal = subprocess.run([str(binary), *args], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
        assert fatal.returncode == -signal.SIGABRT and b'PRODUCTION_QUIESCE_GUARD' in fatal.stderr, \
            'Production quiescence guard must abort; a harness assertion or returned context is not success'
    check_stuck_worker('5', '0', '1', '0', '2')
    check_stuck_worker('5', '0')
    check_stuck_worker('5', '0', '0', '1')
    for animation in (1, 2):
        subprocess.run([str(binary),'0','0','0','1',str(animation)],check=True,timeout=20)
    # The non-lookahead build must retain the same recovery ownership contract.
    compile_args[3] = '-DRICKYOS_PLUS_PHASE_LOOKAHEAD=0'
    subprocess.run(compile_args, check=True)
    for fault, slow in ((6, 0), (0, 0), (0, 1), (1, 0), (2, 0), (3, 0), (4, 0), (7, 0), (8, 0), (9, 0)):
        subprocess.run([str(binary), str(fault), str(slow), '0', '1'], check=True, timeout=20)
    check_stuck_worker('5', '0', '0', '1')
    for animation in (1, 2):
        subprocess.run([str(binary),'0','0','0','1',str(animation)],check=True,timeout=20)
    # Quality reader: same production queues/ISR with 48 base phases and 55
    # motion ticks, including compact S31 feed, completion-only and recovery.
    compile_args[3] = '-DRICKYOS_PLUS_PHASE_LOOKAHEAD=1'
    subprocess.run([*compile_args, '-DHOST_READER_PHASES=48'], check=True)
    for animation in (1, 3, 5):
        for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0)):
            subprocess.run([str(binary), str(fault), str(slow), '1', '0', str(animation)], check=True, timeout=20)
    # Fine ripple uses24 direct byte-action slots; no packed decoding in feeders.
    subprocess.run([*compile_args, '-DHOST_READER_PHASES=48', '-DHOST_READER_SPREAD=23',
                    '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1'], check=True)
    for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0), (8, 0), (9, 0)):
        subprocess.run([str(binary), str(fault), str(slow), '1', '0', '3'], check=True, timeout=20)
    # Fixed optical cadence: actual start-at deadlines, slow producer/recovery,
    # no catch-up shortening, final HOLD; UI/completion-only retain deadline0.
    subprocess.run([*compile_args, '-DHOST_READER_PHASES=48', '-DHOST_READER_SPREAD=23',
                    '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1', '-DRICKYOS_PLUS_READER_MOTION_PERIOD_US=20000',
                    '-DHOST_MOTION_PERIOD_EXPECTED=20000'], check=True)
    for animation in (3, 4, 5):
        for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0)):
            subprocess.run([str(binary), str(fault), str(slow), '1', '0', str(animation)], check=True, timeout=20)
    # Compact ripple retains20ms and all48 local phases;15 delayed launches.
    subprocess.run([*compile_args, '-DHOST_READER_PHASES=48', '-DHOST_READER_SPREAD=15',
                    '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1', '-DRICKYOS_PLUS_READER_RIPPLE_SPREAD=15',
                    '-DRICKYOS_PLUS_READER_MOTION_PERIOD_US=20000',
                    '-DHOST_MOTION_PERIOD_EXPECTED=20000'], check=True)
    for animation in (3, 4, 5):
        for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0)):
            subprocess.run([str(binary), str(fault), str(slow), '1', '0', str(animation)], check=True, timeout=20)
    # q9 compact ripple restores q6 unforced cadence; all48 phases still reach
    # the same queues/ISR. No new fixed launch deadline or shortened final hold.
    subprocess.run([*compile_args, '-DHOST_READER_PHASES=48', '-DHOST_READER_SPREAD=15',
                    '-DRICKYOS_PLUS_READER_FINE_RIPPLE=1', '-DRICKYOS_PLUS_READER_RIPPLE_SPREAD=15',
                    '-DRICKYOS_PLUS_READER_MOTION_PERIOD_US=0'], check=True)
    for animation in (3, 4, 5):
        for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0)):
            subprocess.run([str(binary), str(fault), str(slow), '1', '0', str(animation)], check=True, timeout=20)
    # Compact skim/arc use five launches with complete48 local phases. Exercise
    # their actual smaller ring modulo while controller/ISR retain q6 cadence.
    subprocess.run([*compile_args, '-DHOST_READER_PHASES=48', '-DHOST_READER_SPREAD=4',
                    '-DRICKYOS_PLUS_READER_SWEEP_SPREAD=4',
                    '-DRICKYOS_PLUS_READER_MOTION_PERIOD_US=0'], check=True)
    for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0)):
        subprocess.run([str(binary), str(fault), str(slow), '1', '0', '3'], check=True, timeout=20)
    # Short motion (ripple spread 5, skim/arc spread 2): the same feed and fault paths.
    for spread, flags in ((5, ['-DRICKYOS_PLUS_READER_FINE_RIPPLE=1', '-DRICKYOS_PLUS_READER_RIPPLE_SPREAD=5']),
                          (2, ['-DRICKYOS_PLUS_READER_SWEEP_SPREAD=2'])):
        subprocess.run([*compile_args, '-DHOST_READER_PHASES=48', f'-DHOST_READER_SPREAD={spread}', *flags,
                        '-DRICKYOS_PLUS_READER_MOTION_PERIOD_US=0'], check=True)
        for fault, slow in ((0, 0), (0, 1), (4, 0), (6, 0)):
            subprocess.run([str(binary), str(fault), str(slow), '1', '0', '3'], check=True, timeout=20)
print('Actual LCD legacy GC and lookahead GL16_FAST/GL16: exact row/mask/crop order, delayed peer/ack, exclusive queue reset, frame/prefix/feed failure cleanup, recovery redraw, and production fatal quiesce guards passed (ASan/UBSan)')
