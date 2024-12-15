#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef __MINGW32__
#undef main
#endif

#define _DEBUG_ 1

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 1.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20
#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_AUDIO_MASTER

typedef struct PacketQueue
{
    AVPacketList *  first_pkt;
    AVPacketList *  last_pkt;
    int             nb_packets;
    int             size;
    SDL_mutex *     mutex;
    SDL_cond *      cond;
} PacketQueue;

typedef struct VideoPicture
{
    AVFrame *   frame;
    int         width;
    int         height;
    int         allocated;
    double      pts;
} VideoPicture;

typedef struct VideoState
{
  /**
    * File I/O Context.
    */
  AVFormatContext * pFormatCtx;

  /**
    * Audio Stream.
    */
  int                 audioStream;
  AVStream *          audio_st;
  AVCodecContext *    audio_ctx;
  PacketQueue         audioq;
  uint8_t             audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) /2];
  unsigned int        audio_buf_size;
  unsigned int        audio_buf_index;
  AVFrame             audio_frame;
  AVPacket            audio_pkt;
  uint8_t *           audio_pkt_data;
  int                 audio_pkt_size;
  double              audio_clock;

  /**
  * Video Stream.
  */
  int                 videoStream;
  AVStream *          video_st;
  AVCodecContext *    video_ctx;
  SDL_Texture *       texture;
  SDL_Renderer *      renderer;
  PacketQueue         videoq;
  struct SwsContext * sws_ctx;
  double              frame_timer;
  double              frame_last_pts;
  double              frame_last_delay;
  double              video_clock;
  double              video_current_pts;
  int64_t             video_current_pts_time;
  double              audio_diff_cum;
  double              audio_diff_avg_coef;
  double              audio_diff_threshold;
  int                 audio_diff_avg_count;

  /**
  * VideoPicture Queue.
  */
  VideoPicture        pictq[VIDEO_PICTURE_QUEUE_SIZE];
  int                 pictq_size;
  int                 pictq_rindex;
  int                 pictq_windex;
  SDL_mutex *         pictq_mutex;
  SDL_cond *          pictq_cond;

  /**
  * AV Sync.
  */
  int                 av_sync_type;
  double              external_clock;
  int64_t             external_clock_time;

  /**
  * Seeking.
  */
  int                 seek_req;
  int                 seek_flags;
  int64_t             seek_pos;

  /**
    * Threads.
    */
  SDL_Thread *        decode_tid;
  SDL_Thread *        video_tid;

  /**
    * Input file name.
    */
  char                filename[1024];

  /**
    * Global quit flag.
    */
  int                 quit;

  /**
    * Maximum number of frames to be decoded.
    */
  long                maxFramesToDecode;
  int                 currentFrameIndex;
} VideoState;

/**
 * Struct used to hold data fields used for audio resampling.
 */
typedef struct AudioResamplingState
{
    SwrContext* swr_ctx;
    int64_t in_channel_layout;
    uint64_t out_channel_layout;
    int out_nb_channels;
    int out_linesize;
    int in_nb_samples;
    int64_t out_nb_samples;
    int64_t max_out_nb_samples;
    uint8_t** resampled_data;
    int resampled_data_size;

} AudioResamplingState;

/**
 * Audio Video Sync Types.
 */
enum
{
  /**
    * Sync to audio clock.
    */
  AV_SYNC_AUDIO_MASTER,

  /**
    * Sync to video clock.
    */
  AV_SYNC_VIDEO_MASTER,

  /**
    * Sync to external clock: the computer clock
    */
  AV_SYNC_EXTERNAL_MASTER,
};


SDL_Window* screen;
SDL_mutex* screen_mutex;

VideoState* global_video_state;
AVPacket flush_pkt;

int decode_thread(void * arg);

int stream_component_open(
  VideoState* videoState,
  int stream_index
);

void alloc_picture(void * userdata);

int queue_picture(
  VideoState* videoState,
  AVFrame* pFrame,
  double ptr
);

int video_thread(void *arg);

static int64_t guess_correct_pts(AVCodecContext * ctx, int64_t reordered_pts, int64_t dts);

double synchronize_video(
  VideoState* videoState,
  AVFrame* src_frame,
  double pts
);

int synchronize_audio(
  VideoState* videoState,
  short* samples,
  int samples_size
);

void video_refresh_timer(void* userdata);

double get_audio_clock(VideoState* videoState);

double get_video_clock(VideoState* videoState);

double get_external_clock(VideoState* videoState);

double get_master_clock(VideoState* videoState);

static void schedule_refresh(
  VideoState* videoState,
  int delay
);

static Uint32 sdl_refresh_timer_cb(
  Uint32 interval,
  void* param
);

void video_display(VideoState* videoState);

void packet_queue_init(PacketQueue* queue);

int packet_queue_put(
  PacketQueue* queue,
  AVPacket* packet
);

static int packet_queue_get(
  PacketQueue * queue,
  AVPacket * packet,
  int blocking
);

static void packet_queue_flush(PacketQueue * queue);

void audio_callback(
  void * userdata,
  Uint8 * stream,
  int len
);

int audio_decode_frame(
  VideoState* videoState,
  uint8_t* audio_buf,
  int buf_size,
  double* pts_ptr
);

static int audio_resampling(
  VideoState* videoState,
  AVFrame* decoded_audio_frame,
  enum AVSampleFormat out_sample_fmt,
  uint8_t* out_buf
);

AudioResamplingState* getAudioResampling(uint64_t channel_layout);
void stream_seek(VideoState* videoState, int64_t pos, int rel);
char* get_filename(char* full_path);
