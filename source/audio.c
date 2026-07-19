/* audio.c -- minimal OpenSL ES implementation backed by libnx audout.
 *
 * NBA Jam's bundled OpenAL uses an OpenSL ES backend: it creates one engine,
 * one output mix and one AudioPlayer whose source is an Android simple buffer
 * queue, then feeds already-mixed PCM through Enqueue(). We implement just that
 * surface: engine/outputmix/player objects with the Object/Engine/Play/Volume/
 * BufferQueue interfaces, and a feeder thread that resamples each enqueued
 * buffer to 48 kHz stereo s16 and streams it through a pool of audout buffers.
 *
 * Playback is gapless: several audout buffers stay in flight and the buffer
 * queue callback fires as soon as a buffer is consumed, so the game keeps the
 * queue full. Resampling is a phase-continuous linear interpolator (state
 * carried across buffers) so there are no clicks at buffer boundaries.
 *
 * Interface vtable layouts follow the OpenSL ES 1.0.1 specification order.
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#include "util.h"

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint8_t  SLboolean;
typedef uint32_t SLresult;
typedef int32_t  SLmillibel;

#define SL_RESULT_SUCCESS 0
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3
#define SL_DATAFORMAT_PCM 2

typedef struct { SLuint32 formatType; SLuint32 numChannels; SLuint32 samplesPerSec;
                 SLuint32 bitsPerSample; SLuint32 containerSize; SLuint32 channelMask;
                 SLuint32 endianness; } SLDataFormat_PCM;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;

static const int iid_engine, iid_play, iid_record, iid_bufferqueue, iid_androidcfg,
                 iid_volume, iid_outputmix, iid_plainbq, iid_effectsend, iid_envreverb;
const void *SL_IID_ENGINE                   = &iid_engine;
const void *SL_IID_PLAY                     = &iid_play;
const void *SL_IID_RECORD                   = &iid_record;
const void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_bufferqueue;
const void *SL_IID_ANDROIDCONFIGURATION     = &iid_androidcfg;
const void *SL_IID_VOLUME                   = &iid_volume;
const void *SL_IID_OUTPUTMIX                = &iid_outputmix;
// Godot's OpenSL driver queries the player with the plain BUFFERQUEUE id (the
// spec-level twin of the Android simple buffer queue; same vtable layout) and
// requests EFFECTSEND/ENVIRONMENTALREVERB it never uses when not required.
const void *SL_IID_BUFFERQUEUE              = &iid_plainbq;
const void *SL_IID_EFFECTSEND               = &iid_effectsend;
const void *SL_IID_ENVIRONMENTALREVERB      = &iid_envreverb;

typedef struct { const void *vtbl; void *self; } Itf;

// ---------------------------------------------------------------------------
// player + audout playback
// ---------------------------------------------------------------------------

#define QUEUE_CAP 16
#define NUM_OUT_BUFFERS 4

typedef struct {
  AudioOutBuffer ab;
  int16_t *mem;
  size_t cap;      // 0x1000-aligned byte capacity
  int in_flight;
} OutBuf;

typedef struct {
  Itf obj, play, bq, vol;
  int realized;
  int state;
  SLuint32 src_rate, src_ch, src_bits;
  void (*cb)(void *bq, void *ctx);
  void *cb_ctx;
  struct { const void *data; SLuint32 size; } q[QUEUE_CAP];
  int q_head, q_tail, q_count;
  Mutex lock;
  CondVar cond;
  Thread thread;
  int thread_started;
  volatile int quit;
  OutBuf bufs[NUM_OUT_BUFFERS];
  // phase-continuous linear resampler state
  double rphase;
  int16_t prevL, prevR;
} Player;

static int s_audout_ready = 0;
static Mutex s_audout_lock;

static void audout_ensure(void) {
  mutexLock(&s_audout_lock);
  if (!s_audout_ready) {
    if (R_SUCCEEDED(audoutInitialize())) {
      audoutStartAudioOut();
      s_audout_ready = 1;
      debugPrintf("[audio] audout started: %luHz %uch\n",
                  (unsigned long)audoutGetSampleRate(), (unsigned)audoutGetChannelCount());
    } else {
      debugPrintf("[audio] audoutInitialize failed\n");
    }
  }
  mutexUnlock(&s_audout_lock);
}

static int ensure_cap(OutBuf *b, size_t bytes) {
  size_t need = (bytes + 0xFFF) & ~(size_t)0xFFF;
  if (need <= b->cap) return 1;
  free(b->mem);
  b->mem = memalign(0x1000, need);
  b->cap = b->mem ? need : 0;
  return b->mem != NULL;
}

// Convert one enqueued buffer into b->mem as 48 kHz stereo s16. Returns frames.
static size_t convert(Player *p, OutBuf *b, const void *data, SLuint32 bytes) {
  const unsigned ch = p->src_ch ? p->src_ch : 2;
  const unsigned rate = p->src_rate ? p->src_rate : 48000;
  const size_t in_frames = bytes / (2u * ch); // s16
  if (in_frames == 0) return 0;
  const int16_t *in = data;

  if (rate == 48000) {
    if (!ensure_cap(b, in_frames * 2 * sizeof(int16_t))) return 0;
    int16_t *o = b->mem;
    for (size_t i = 0; i < in_frames; i++) {
      int16_t l = in[i * ch];
      *o++ = l;
      *o++ = (ch >= 2) ? in[i * ch + 1] : l;
    }
    return in_frames;
  }

  const double r = (double)rate / 48000.0; // input frames advanced per output frame
  size_t maxout = (size_t)((double)in_frames / r) + 4;
  if (!ensure_cap(b, maxout * 2 * sizeof(int16_t))) return 0;
  int16_t *o = b->mem;
  size_t oc = 0;
  for (size_t i = 0; i < in_frames; i++) {
    int16_t curL = in[i * ch];
    int16_t curR = (ch >= 2) ? in[i * ch + 1] : curL;
    while (p->rphase < 1.0 && oc < maxout) {
      double t = p->rphase;
      o[oc * 2]     = (int16_t)(p->prevL + (curL - p->prevL) * t);
      o[oc * 2 + 1] = (int16_t)(p->prevR + (curR - p->prevR) * t);
      oc++;
      p->rphase += r;
    }
    p->rphase -= 1.0;
    p->prevL = curL; p->prevR = curR;
  }
  return oc;
}

static void free_one(Player *p, AudioOutBuffer *rel) {
  if (!rel) return;
  for (int i = 0; i < NUM_OUT_BUFFERS; i++)
    if (&p->bufs[i].ab == rel) { p->bufs[i].in_flight = 0; break; }
}
static void reclaim(Player *p) {
  for (;;) {
    AudioOutBuffer *rel = NULL;
    u32 cnt = 0;
    if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &cnt)) || !rel)
      break;
    free_one(p, rel);
  }
}
static OutBuf *find_free(Player *p) {
  for (int i = 0; i < NUM_OUT_BUFFERS; i++)
    if (!p->bufs[i].in_flight) return &p->bufs[i];
  return NULL;
}
static int any_in_flight(Player *p) {
  for (int i = 0; i < NUM_OUT_BUFFERS; i++)
    if (p->bufs[i].in_flight) return 1;
  return 0;
}

static void feeder(void *arg) {
  Player *p = arg;
  tls_setup_guard(); // the buffer-queue callback runs game code
  audout_ensure();

  for (;;) {
    if (s_audout_ready) reclaim(p);

    mutexLock(&p->lock);
    if (p->quit) { mutexUnlock(&p->lock); break; }
    int have = (p->q_count > 0 && p->state == SL_PLAYSTATE_PLAYING);
    mutexUnlock(&p->lock);

    OutBuf *b = (have && s_audout_ready) ? find_free(p) : NULL;
    if (have && (b || !s_audout_ready)) {
      mutexLock(&p->lock);
      const void *data = p->q[p->q_head].data;
      SLuint32 size = p->q[p->q_head].size;
      p->q_head = (p->q_head + 1) % QUEUE_CAP;
      p->q_count--;
      mutexUnlock(&p->lock);

      if (b) {
        size_t frames = convert(p, b, data, size);
        if (frames) {
          size_t bytes = frames * 2 * sizeof(int16_t);
          b->ab.next = NULL;
          b->ab.buffer = b->mem;
          b->ab.buffer_size = b->cap;
          b->ab.data_size = bytes;
          b->ab.data_offset = 0;
          b->in_flight = 1;
          audoutAppendAudioOutBuffer(&b->ab);
          static int logged_first = 0;
          if (!logged_first) { logged_first = 1; debugPrintf("[audio] streaming (%u frames/buf)\n", (unsigned)frames); }
        }
      }
      if (p->cb) p->cb((void *)&p->bq, p->cb_ctx); // refill promptly -> gapless
      continue;
    }

    if (s_audout_ready && any_in_flight(p)) {
      // WaitPlayFinish returns (and consumes) the released buffer, so free it
      // here directly -- GetReleased won't report it again.
      AudioOutBuffer *rel = NULL; u32 c = 0;
      audoutWaitPlayFinish(&rel, &c, 100000000ULL);
      free_one(p, rel);
      continue;
    }

    // nothing playable and nothing in flight: wait for Enqueue / play state
    mutexLock(&p->lock);
    while (!p->quit && !(p->q_count > 0 && p->state == SL_PLAYSTATE_PLAYING))
      condvarWait(&p->cond, &p->lock);
    mutexUnlock(&p->lock);
  }
}

// ---------------------------------------------------------------------------
// AndroidSimpleBufferQueue interface
// ---------------------------------------------------------------------------

typedef struct {
  SLresult (*Enqueue)(void *self, const void *buffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *state);
  SLresult (*RegisterCallback)(void *self, void (*cb)(void *bq, void *ctx), void *ctx);
} BufferQueueItf_;

static SLresult bq_Enqueue(void *self, const void *buffer, SLuint32 size) {
  Player *p = ((Itf *)self)->self;
  mutexLock(&p->lock);
  if (p->q_count < QUEUE_CAP) {
    p->q[p->q_tail].data = buffer;
    p->q[p->q_tail].size = size;
    p->q_tail = (p->q_tail + 1) % QUEUE_CAP;
    p->q_count++;
    condvarWakeOne(&p->cond);
  }
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(void *self) {
  Player *p = ((Itf *)self)->self;
  mutexLock(&p->lock);
  p->q_head = p->q_tail = p->q_count = 0;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(void *self, void *state) {
  Player *p = ((Itf *)self)->self;
  if (state) { SLuint32 *s = state; s[0] = p->q_count; s[1] = 0; }
  return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(void *self, void (*cb)(void *, void *), void *ctx) {
  Player *p = ((Itf *)self)->self;
  p->cb = cb; p->cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}
static const BufferQueueItf_ bq_vtbl = { bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback };

// ---------------------------------------------------------------------------
// Play interface
// ---------------------------------------------------------------------------

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} PlayItf_;

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = ((Itf *)self)->self;
  mutexLock(&p->lock);
  p->state = (int)state;
  condvarWakeOne(&p->cond);
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = ((Itf *)self)->self;
  if (pState) *pState = (SLuint32)p->state;
  return SL_RESULT_SUCCESS;
}
static SLresult sl_ok0(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult sl_ok1(void *self, void *a) { (void)self; if (a) *(SLuint32 *)a = 0; return SL_RESULT_SUCCESS; }
static SLresult sl_ok_u(void *self, SLuint32 a) { (void)self; (void)a; return SL_RESULT_SUCCESS; }
static SLresult sl_ok_pv(void *self, void *a, void *b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static const PlayItf_ play_vtbl = {
  play_SetPlayState, play_GetPlayState,
  (void *)sl_ok1, (void *)sl_ok1,
  (void *)sl_ok_pv, (void *)sl_ok_u, (void *)sl_ok1,
  (void *)sl_ok_u, (void *)sl_ok0, (void *)sl_ok1,
  (void *)sl_ok_u, (void *)sl_ok1,
};

// ---------------------------------------------------------------------------
// Volume interface (accepted, not applied)
// ---------------------------------------------------------------------------

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *pLevel);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *pMax);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *pMute);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *pEnable);
  SLresult (*SetStereoPosition)(void *self, SLint32 p);
  SLresult (*GetStereoPosition)(void *self, SLint32 *pP);
} VolumeItf_;
static SLresult vol_GetMax(void *self, SLmillibel *pMax) { (void)self; if (pMax) *pMax = 0; return SL_RESULT_SUCCESS; }
static const VolumeItf_ vol_vtbl = {
  (void *)sl_ok_u, (void *)sl_ok1, vol_GetMax,
  (void *)sl_ok_u, (void *)sl_ok1, (void *)sl_ok_u,
  (void *)sl_ok1, (void *)sl_ok_u, (void *)sl_ok1,
};

// ---------------------------------------------------------------------------
// Object interface (shared by engine / output mix / player)
// ---------------------------------------------------------------------------

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const void *iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  void (*AbortAsyncOperation)(void *self);
  void (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 prio, SLboolean preempt);
  SLresult (*GetPriority)(void *self, SLint32 *pPrio);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, void *ids, SLboolean enabled);
} ObjectItf_;

static SLresult player_Realize(void *self, SLboolean async) {
  (void)async;
  Player *p = ((Itf *)self)->self;
  if (!p->realized) {
    p->realized = 1;
    p->state = SL_PLAYSTATE_STOPPED;
    mutexInit(&p->lock);
    condvarInit(&p->cond);
    if (R_SUCCEEDED(threadCreate(&p->thread, feeder, p, NULL, 64 * 1024, 0x2C, -2))) {
      threadStart(&p->thread);
      p->thread_started = 1;
    }
    debugPrintf("[audio] player realized: %uHz %uch %ubit\n",
                (unsigned)p->src_rate, (unsigned)p->src_ch, (unsigned)p->src_bits);
  }
  return SL_RESULT_SUCCESS;
}
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = 2; return SL_RESULT_SUCCESS; }
static void obj_void(void *self) { (void)self; }

static SLresult player_GetInterface(void *self, const void *iid, void *pInterface) {
  Player *p = ((Itf *)self)->self;
  if (iid == SL_IID_PLAY)                          *(void **)pInterface = &p->play;
  else if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE ||
           iid == SL_IID_BUFFERQUEUE)              *(void **)pInterface = &p->bq;
  else if (iid == SL_IID_VOLUME)                   *(void **)pInterface = &p->vol;
  else { *(void **)pInterface = NULL; return 0xC; }
  return SL_RESULT_SUCCESS;
}
static const ObjectItf_ player_obj_vtbl = {
  player_Realize, (void *)sl_ok_u, obj_GetState, player_GetInterface,
  (void *)sl_ok_pv, obj_void, obj_void, (void *)sl_ok_u, (void *)sl_ok1, (void *)sl_ok_pv,
};

typedef struct { Itf obj; } OutputMix;
static SLresult mix_GetInterface(void *self, const void *iid, void *pInterface) {
  (void)self; (void)iid; if (pInterface) *(void **)pInterface = NULL; return 0xC;
}
static const ObjectItf_ mix_obj_vtbl = {
  (void *)sl_ok_u, (void *)sl_ok_u, obj_GetState, mix_GetInterface,
  (void *)sl_ok_pv, obj_void, obj_void, (void *)sl_ok_u, (void *)sl_ok1, (void *)sl_ok_pv,
};

// ---------------------------------------------------------------------------
// Engine interface
// ---------------------------------------------------------------------------

typedef struct {
  SLresult (*CreateLEDDevice)(void *self, void **o, SLuint32 id, SLuint32 n, void *ids, void *req);
  SLresult (*CreateVibraDevice)(void *self, void **o, SLuint32 id, SLuint32 n, void *ids, void *req);
  SLresult (*CreateAudioPlayer)(void *self, void **pPlayer, SLDataSource *src, SLDataSink *snk, SLuint32 n, const void *ids, const SLboolean *req);
  SLresult (*CreateAudioRecorder)(void *self, void **o, void *src, void *snk, SLuint32 n, void *ids, void *req);
  SLresult (*CreateMidiPlayer)(void *self, void **o, void *a, void *b, void *c, void *d, void *e, SLuint32 n, void *ids, void *req);
  SLresult (*CreateListener)(void *self, void **o, SLuint32 n, void *ids, void *req);
  SLresult (*Create3DGroup)(void *self, void **o, SLuint32 n, void *ids, void *req);
  SLresult (*CreateOutputMix)(void *self, void **pMix, SLuint32 n, const void *ids, const SLboolean *req);
  SLresult (*CreateMetadataExtractor)(void *self, void **o, void *src, SLuint32 n, void *ids, void *req);
  SLresult (*CreateExtensionObject)(void *self, void **o, void *p, void *cs, SLuint32 n, void *ids, void *req);
  SLresult (*QueryNumSupportedInterfaces)(void *self, SLuint32 objId, SLuint32 *pNum);
  SLresult (*QuerySupportedInterfaces)(void *self, SLuint32 objId, SLuint32 idx, void *pIID);
  SLresult (*QueryNumSupportedExtensions)(void *self, SLuint32 *pNum);
  SLresult (*QuerySupportedExtension)(void *self, SLuint32 idx, char *p, SLint32 *len);
  SLresult (*IsExtensionSupported)(void *self, const char *ext, SLboolean *pSup);
} EngineItf_;

static SLresult eng_CreateAudioPlayer(void *self, void **pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 n, const void *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)n; (void)ids; (void)req;
  Player *p = calloc(1, sizeof(*p));
  if (!p) return 0x2;
  p->obj.vtbl = &player_obj_vtbl; p->obj.self = p;
  p->play.vtbl = &play_vtbl;      p->play.self = p;
  p->bq.vtbl = &bq_vtbl;          p->bq.self = p;
  p->vol.vtbl = &vol_vtbl;        p->vol.self = p;
  p->src_ch = 2; p->src_rate = 48000; p->src_bits = 16;
  if (src && src->pFormat) {
    SLDataFormat_PCM *fmt = src->pFormat;
    if (fmt->formatType == SL_DATAFORMAT_PCM) {
      p->src_ch = fmt->numChannels;
      p->src_rate = fmt->samplesPerSec / 1000; // milliHz -> Hz
      p->src_bits = fmt->bitsPerSample;
    }
  }
  *pPlayer = &p->obj;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, void **pMix, SLuint32 n, const void *ids, const SLboolean *req) {
  (void)self; (void)n; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m) return 0x2;
  m->obj.vtbl = &mix_obj_vtbl; m->obj.self = m;
  *pMix = &m->obj;
  return SL_RESULT_SUCCESS;
}
static SLresult eng_unsupported(void) { return 0xC; }

static const EngineItf_ eng_vtbl = {
  (void *)eng_unsupported, (void *)eng_unsupported,
  eng_CreateAudioPlayer,
  (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported,
  eng_CreateOutputMix,
  (void *)eng_unsupported, (void *)eng_unsupported,
  (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported,
  (void *)eng_unsupported, (void *)eng_unsupported,
};

typedef struct { Itf obj; Itf eng; } Engine;
static SLresult engine_GetInterface(void *self, const void *iid, void *pInterface) {
  Engine *e = ((Itf *)self)->self;
  if (iid == SL_IID_ENGINE) { *(void **)pInterface = &e->eng; return SL_RESULT_SUCCESS; }
  if (pInterface) *(void **)pInterface = NULL;
  return 0xC;
}
static const ObjectItf_ engine_obj_vtbl = {
  (void *)sl_ok_u, (void *)sl_ok_u, obj_GetState, engine_GetInterface,
  (void *)sl_ok_pv, obj_void, obj_void, (void *)sl_ok_u, (void *)sl_ok1, (void *)sl_ok_pv,
};

// ---------------------------------------------------------------------------
// slCreateEngine
// ---------------------------------------------------------------------------

SLresult slCreateEngine(void **pEngine, SLuint32 numOptions, const void *pEngineOptions,
                        SLuint32 numInterfaces, const void *pInterfaceIds,
                        const SLboolean *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  static int inited = 0;
  if (!inited) { mutexInit(&s_audout_lock); inited = 1; }
  Engine *e = calloc(1, sizeof(*e));
  if (!e) return 0x2;
  e->obj.vtbl = &engine_obj_vtbl; e->obj.self = e;
  e->eng.vtbl = &eng_vtbl;        e->eng.self = e;
  *pEngine = &e->obj;
  debugPrintf("[audio] slCreateEngine\n");
  return SL_RESULT_SUCCESS;
}
