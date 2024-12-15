#include "main.h"
PacketQueue audioq;

SDL_Window * screen;
SDL_mutex * screen_mutex;

VideoState * global_video_state;
char* path;

int main(int argc, char* argv [])
{
  
  if(argc != 3)
  {
    printf("Invalid arguments.\n\n");
    printf("Usage: ./fideo <filename> <max-frames-to-decode>\n\n");
    printf("e.g: ./fideo /home/zaky/Videos/sample_video.mp4 200\n");
    return -1;
  }

  int ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
  if(ret != 0)
  {
    printf("Couldn't initialize SDL - %s\n", SDL_GetError());
    return -1;
  }

  VideoState* videoState = NULL;
  videoState = av_mallocz(sizeof(VideoState));
  av_strlcpy(videoState->filename, argv[1], sizeof(videoState->filename));

  char* pEnd;
  path = (char*)malloc(sizeof(char) * 1000);
  videoState->maxFramesToDecode = strtol(argv[2], &pEnd, 10);

  path = get_filename(argv[1]);
  printf("%s\n", path);


  videoState->pictq_mutex = SDL_CreateMutex();
  videoState->pictq_cond = SDL_CreateCond();

  schedule_refresh(videoState, 100);
  videoState->decode_tid = SDL_CreateThread(decode_thread, "Decoding Thread", videoState);
  if(!videoState->decode_tid)
  {
    printf("Couldn't start decoding SDL_Thread - exiting\n");
    av_free(videoState);
    return -1;
  }

  av_init_packet(&flush_pkt);
  flush_pkt.data = "FLUSH";

  SDL_Event event;
  while(1)
  {
    double incr, pos;

    ret = SDL_WaitEvent(&event);
    if(ret == 0)
    {
      printf("SDL_WaitEvent failed: %s\n", SDL_GetError());
    }
    switch(event.type)
    {
      case SDL_KEYDOWN:
      {
          switch(event.key.keysym.sym)
          {
              case SDLK_LEFT:
              {
                  incr = -10.0;
                  goto do_seek;
              }
              case SDLK_RIGHT:
              {
                  incr = 10.0;
                  goto do_seek;
              }
              case SDLK_DOWN:
              {
                  incr = -60.0;
                  goto do_seek;
              }
              case SDLK_UP:
              {
                  incr = 60.0;
                  goto do_seek;
              }

              do_seek:
              {
                if(global_video_state)
                {
                  pos = get_master_clock(global_video_state);
                  pos += incr;
                  stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
                }
                break;
              };

              default:
              {
                  // nothing to do
              }
              break;
          }
      }
      break;

      case FF_QUIT_EVENT:
      case SDL_QUIT:
      {
        videoState->quit = 1;
        SDL_CondSignal(videoState->audioq.cond);
        SDL_CondSignal(videoState->videoq.cond);
        SDL_Quit();
      }
      break;
      case FF_REFRESH_EVENT:
      {
        video_refresh_timer(event.user.data1);
      }
      break;
      default:
      {

      }
      break;
    }
    if(videoState->quit)
    {
      break;
    }
  }

  av_free(videoState);
  return 0;
}

int decode_thread(void* arg)
{
  VideoState* videoState = (VideoState*) arg;
  int ret = -1;

  AVFormatContext* pFormatCtx = NULL;
  ret = avformat_open_input(&pFormatCtx, videoState->filename, NULL, NULL);
  if(ret < 0)
  {
    printf("Coudn't open file %s\n", videoState->filename);
    return -1;
  }

  videoState->videoStream = -1;
  videoState->audioStream = -1;

  global_video_state = videoState;
  
  videoState->pFormatCtx = pFormatCtx;
  ret = avformat_find_stream_info(pFormatCtx, NULL);
  if(ret < 0)
  {
    printf("Couldn't find stream information %s\n", videoState->filename);
    return -1;
  }

  av_dump_format(pFormatCtx, 0, videoState->filename, 0);

  int videoStream = -1;
  int audioStream = -1;

  for(int i=0; i<pFormatCtx->nb_streams; ++i)
  {
    if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0)
    {
      videoStream = i;
    }
    if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0)
    {
      audioStream = i;
    }
  }
  if(videoStream == -1)
  {
    printf("Coudn't find video stream\n");
    goto fail;
  }
  else
  {
    ret = stream_component_open(videoState, videoStream);
    if(ret < 0)
    {
      printf("Couldn't open video codec\n");
      goto fail;
    }
  }
  if(audioStream == -1)
  {
    printf("Couldn't find audio stream\n");
    goto fail;
  }
  else
  {
    ret = stream_component_open(videoState, audioStream);
    if(ret < 0)
    {
      printf("Coudln't open codecs: %s", videoState->filename);
      goto fail;
    }
  }
  
  if(videoState->videoStream < 0 || videoState->audioStream < 0)
  {
    printf("Couldn't open codes: %s\n", videoState->filename);
    goto fail;
  }

  // alloc the AVPacket used to read the media file
  AVPacket * packet = av_packet_alloc();
  if (packet == NULL)
  {
      printf("Could not alloc packet.\n");
      return -1;
  }

  while(1)
  {
    if(videoState->quit)
    {
      break;
    }

    if(videoState->seek_req)
    {
      int video_stream_index = -1;
      int audio_stream_index = -1;
      int64_t seek_target_audio = videoState->seek_pos;
      int64_t seek_target_video = videoState->seek_pos;

      if(videoState->videoStream >= 0)
      {
        video_stream_index = videoState->videoStream;
      }
      if(videoState->audioStream >= 0)
      {
        audio_stream_index = videoState->audioStream;
      }
      if(video_stream_index >= 0 && audio_stream_index >= 0)
      {
        seek_target_video = av_rescale_q(seek_target_video, AV_TIME_BASE_Q, pFormatCtx->streams[video_stream_index]->time_base);
        seek_target_audio = av_rescale_q(seek_target_audio, AV_TIME_BASE_Q, pFormatCtx->streams[video_stream_index]->time_base);
      }

      ret = av_seek_frame(videoState->pFormatCtx, video_stream_index, seek_target_video, videoState->seek_flags);
      ret &= av_seek_frame(videoState->pFormatCtx, audio_stream_index, seek_target_audio, videoState->seek_flags);

      if (ret < 0)
      {
        fprintf(stderr, "%s: error while seeking\n", videoState->pFormatCtx->filename);
      }
      else
      {
        if(videoState->videoStream >= 0)
        {
          packet_queue_flush(&videoState->videoq);
          packet_queue_put(&videoState->videoq, &flush_pkt);
        }
        if(videoState->audioStream >= 0)
        {
          packet_queue_flush(&videoState->audioq);
          packet_queue_put(&videoState->audioq, &flush_pkt);
        }
      }
      videoState->seek_req = 0;
    }

    if(videoState->audioq.size > MAX_AUDIOQ_SIZE || videoState->videoq.size > MAX_VIDEOQ_SIZE)
    {
      SDL_Delay(10);
      continue;
    }

    ret = av_read_frame(videoState->pFormatCtx, packet);
    if(ret < 0)
    {
      if(ret == AVERROR_EOF)
      {
        videoState->quit = 1;
        break;
      }
      else if(videoState->pFormatCtx->pb->error == 0)
      {
        SDL_Delay(100);
        continue;
      }
      else
      {
        break;
      }
    }
    if(packet->stream_index == videoState->videoStream)
    {
      packet_queue_put(&videoState->videoq, packet);
    }
    else if(packet->stream_index == videoState->audioStream)
    {
      packet_queue_put(&videoState->audioq, packet);
    }
    else
    {
      av_packet_unref(packet);
    }
  }

  while(!videoState->quit)
  {
    SDL_Delay(100);
  }
  avformat_close_input(&pFormatCtx);

  fail:
  {
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = videoState;

    SDL_PushEvent(&event);
    return -1;
  }

  return 0;
}

int stream_component_open(VideoState * videoState, int stream_index)
{
  AVFormatContext* pFormatCtx = videoState->pFormatCtx;
  if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams)
  {
    printf("Invalid stream index");
    return -1;
  }

  AVCodec* codec = NULL;
  codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
  if(codec == NULL)
  {
    printf("Unsupported codec\n");
    return -1;
  }

  AVCodecContext* codecCtx = NULL;
  codecCtx = avcodec_alloc_context3(codec);
  int ret = avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar);
  if(ret != 0)
  {
    printf("Couldn't copy codec context\n");
    return -1;
  }

  if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
  {
    SDL_AudioSpec wanted_specs;
    SDL_AudioSpec specs;

    wanted_specs.freq = codecCtx->sample_rate;
    wanted_specs.format = AUDIO_S16SYS;
    wanted_specs.channels = codecCtx->channels;
    wanted_specs.silence = 0;
    wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_specs.callback = audio_callback;
    wanted_specs.userdata = videoState;


    ret = SDL_OpenAudio(&wanted_specs, &specs);
    // SDL_OpenAudioDevice returns a valid device ID that is > 0 on success or 0 on failure
    if (ret < 0)
    {
      printf("Failed to open audio device: %s.\n", SDL_GetError());
      return -1;
    }
  }

  if(avcodec_open2(codecCtx, codec, NULL) < 0)
  {
    printf("Unsupported codec\n");
    return -1;
  }

  switch(codecCtx->codec_type)
  {
    case AVMEDIA_TYPE_AUDIO:
    {
      videoState->audioStream = stream_index;
      videoState->audio_st = pFormatCtx->streams[stream_index];
      videoState->audio_ctx = codecCtx;
      videoState->audio_buf_size = 0;
      videoState->audio_buf_index = 0;

      memset(&videoState->audio_pkt, 0, sizeof(videoState->audio_pkt));
      packet_queue_init(&videoState->audioq);
      SDL_PauseAudio(0);
    }
    break;

    case AVMEDIA_TYPE_VIDEO:
    {
      videoState->videoStream = stream_index;
      videoState->video_st = pFormatCtx->streams[stream_index];
      videoState->video_ctx = codecCtx;

      videoState->frame_timer = (double)av_gettime() / 1000000.0;
      videoState->frame_last_delay = 40e-3;

      packet_queue_init(&videoState->videoq);
      videoState->video_tid = SDL_CreateThread(video_thread, "Video Thread", videoState);
      videoState->sws_ctx = sws_getContext(
        videoState->video_ctx->width,
        videoState->video_ctx->height,
        videoState->video_ctx->pix_fmt,
        videoState->video_ctx->width,
        videoState->video_ctx->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
      );

      screen_mutex = SDL_CreateMutex();
    }
      break;
    
    default:
    {

    }
      break;
  }

  return 0;
}

void alloc_picture(void* userdata)
{
  VideoState* videoState = (VideoState*)userdata;

  VideoPicture* videoPicture;
  videoPicture = &videoState->pictq[videoState->pictq_windex];

  if(videoPicture->frame)
  {
    av_frame_free(&videoPicture->frame);
    av_free(videoPicture->frame);
  }

  SDL_LockMutex(screen_mutex);

  int numBytes;
  numBytes = av_image_get_buffer_size(
    AV_PIX_FMT_YUV420P,
    videoState->video_ctx->width,
    videoState->video_ctx->height,
    32
  );

  uint8_t* buffer = NULL;
  buffer = (uint8_t*) av_malloc(numBytes * sizeof(uint8_t));

  videoPicture->frame = av_frame_alloc();
  if(videoPicture->frame == NULL)
  {
    printf("Couldn't allocate frame\n");
    return;
  }

  av_image_fill_arrays(
    videoPicture->frame->data,
    videoPicture->frame->linesize,
    buffer,
    AV_PIX_FMT_YUV420P,
    videoState->video_ctx->width,
    videoState->video_ctx->height,
    32
  );

  SDL_UnlockMutex(screen_mutex);

  videoPicture->width = videoState->video_ctx->width;
  videoPicture->height = videoState->video_ctx->height;
  videoPicture->allocated = 1;
}

int queue_picture(VideoState* videoState, AVFrame* pFrame, double pts)
{
  SDL_LockMutex(videoState->pictq_mutex);
  while(videoState->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !videoState->quit)
  {
    SDL_CondWait(videoState->pictq_cond, videoState->pictq_mutex);
  }
  SDL_UnlockMutex(videoState->pictq_mutex);
  if(videoState->quit)
  {
    return -1;
  }

  VideoPicture* videoPicture;
  videoPicture = &videoState->pictq[videoState->pictq_windex];

  if(
    !videoPicture->frame ||
    videoPicture->width != videoState->video_ctx->width ||
    videoPicture->height != videoState->video_ctx->height
  )
  {
    videoPicture->allocated = 0;
    alloc_picture(videoState);
    if(videoState->quit)
    {
      return -1;
    }
  }

  if(videoPicture->frame)
  {
    videoPicture->pts = pts;
    videoPicture->frame->pict_type = pFrame->pict_type;
    videoPicture->frame->pts = pFrame->pts;
    videoPicture->frame->pkt_dts = pFrame->pkt_dts;
    videoPicture->frame->key_frame = pFrame->key_frame;
    videoPicture->frame->coded_picture_number = pFrame->coded_picture_number;
    videoPicture->frame->display_picture_number = pFrame->display_picture_number;
    videoPicture->frame->width = pFrame->width;
    videoPicture->frame->height = pFrame->height;

    sws_scale(
      videoState->sws_ctx,
      (uint8_t const * const *)pFrame->data,
      pFrame->linesize,
      0,
      videoState->video_ctx->height,
      videoPicture->frame->data,
      videoPicture->frame->linesize
    );
    ++videoState->pictq_windex;

    if(videoState->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
    {
      videoState->pictq_windex = 0;
    }

    SDL_LockMutex(videoState->pictq_mutex);
    videoState->pictq_size++;
    SDL_UnlockMutex(videoState->pictq_mutex);
  }

  return 0;
}

int video_thread(void * arg)
{
  VideoState* videoState = (VideoState*)arg;
  AVPacket* packet = av_packet_alloc();
  if(NULL == packet)
  {
    printf("Couldn't alloc packet\n");
    return -1;
  }

  int frameFinished = 0;
  static AVFrame* pFrame = NULL;
  pFrame = av_frame_alloc();
  if(!pFrame)
  {
    printf("Couldn't allocate AVFrame\n");
    return -1;
  }

  double pts;
  while(1)
  {
    int ret = packet_queue_get(&videoState->videoq, packet, 1);
    if(ret < 0)
    {
      break;
    }

    if (packet->data == flush_pkt.data)
    {
      avcodec_flush_buffers(videoState->video_ctx);
      continue;
    }

    ret = avcodec_send_packet(videoState->video_ctx, packet);
    if(ret < 0)
    {
      printf("Error sending packet for decoding\n");
      return -1;
    }

    while(ret >= 0)
    {
      ret = avcodec_receive_frame(videoState->video_ctx, pFrame);
      if(AVERROR(EAGAIN) == ret || AVERROR_EOF == ret)
      {
        break;
      }
      else if(ret < 0)
      {
        printf("Error while decoding\n");
        return -1;
      }
      else
      {
        frameFinished = 1;
      }

      pts = guess_correct_pts(videoState->video_ctx, pFrame->pts, pFrame->pkt_dts);

      // if undefined timestamp
      if(pts == AV_NOPTS_VALUE)
      {
        pts = 0;
      }

      pts *= av_q2d(videoState->video_st->time_base);

      if(frameFinished)
      {
        pts = synchronize_video(videoState, pFrame, pts);
        if(queue_picture(videoState, pFrame, pts) < 0)
        {
          break;
        }
      }
    }
    av_packet_unref(packet);
  }

  av_frame_free(&pFrame);
  av_free(pFrame);

  return 0;
}

static int64_t guess_correct_pts(AVCodecContext * ctx, int64_t reordered_pts, int64_t dts)
{
  int64_t pts;

  if(dts != AV_NOPTS_VALUE)
  {
    ctx->pts_correction_num_faulty_dts += dts <= ctx->pts_correction_last_dts;
    ctx->pts_correction_last_dts = dts;
  }
  else if(reordered_pts != AV_NOPTS_VALUE)
  {
    ctx->pts_correction_last_dts = reordered_pts;
  }

  if(reordered_pts != AV_NOPTS_VALUE)
  {
    ctx->pts_correction_num_faulty_pts += reordered_pts <= ctx->pts_correction_last_pts;
    ctx->pts_correction_last_pts = reordered_pts;
  }
  else if(dts != AV_NOPTS_VALUE)
  {
    ctx->pts_correction_last_pts = dts;
  }

  if((ctx->pts_correction_num_faulty_pts <= ctx->pts_correction_num_faulty_dts || 
    dts == AV_NOPTS_VALUE) && 
    reordered_pts != AV_NOPTS_VALUE
  )
  {
    pts = reordered_pts;
  }
  else
  {
    pts = dts;
  }

  return pts;
}

int synchronize_audio(VideoState* videoState, short* samples, int samples_size)
{
  int n;
  double ref_clock;
  
  n = 2 * videoState->audio_ctx->channels;
  if(videoState->av_sync_type != AV_SYNC_AUDIO_MASTER)
  {
    double diff, avg_diff;
    int wanted_size, min_size, max_size;

    ref_clock = get_master_clock(videoState);
    diff = get_audio_clock(videoState) - ref_clock;

    if(diff < AV_NOSYNC_THRESHOLD)
    {
      videoState->audio_diff_cum = diff + videoState->audio_diff_avg_coef * videoState->audio_diff_cum;

      if(videoState->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
      {
        videoState->audio_diff_avg_count++;
      }
      else
      {
        avg_diff = videoState->audio_diff_cum  * (1.0 - videoState->audio_diff_avg_coef);

        if(fabs(avg_diff) >= videoState->audio_diff_threshold)
        {
          wanted_size = samples_size + ((int)(diff * videoState->audio_diff_avg_coef));
          min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
          max_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);

          if(wanted_size < min_size)
          {
            wanted_size = min_size;
          }
          else if(wanted_size > max_size)
          {
            wanted_size = max_size;
          }

          if(wanted_size < samples_size)
          {
            samples_size = wanted_size;
          }
          else if(wanted_size > samples_size)
          {
            uint8_t* samples_end;
            uint8_t* q;
            int nb = (samples_size - wanted_size);
            samples_end = (uint8_t*)samples + samples_size - n;
            q = samples_end + n;

            while(nb > 0)
            {
              memcpy(q, samples_end, n);
              q += n;
              nb -= n;
            }
            samples_size = wanted_size;
          }
        }
      }
    }
    else
    {
      videoState->audio_diff_avg_count = 0;
      videoState->audio_diff_cum = 0;
    }
  }

  return samples_size;
}

double synchronize_video(VideoState* videoState, AVFrame* src_frame, double pts)
{
  double frame_delay;
  if(pts != 0)
  {
    videoState->video_clock = pts;
  }
  else
  {
    pts = videoState->video_clock;
  }
  frame_delay = av_q2d(videoState->video_ctx->time_base);
  frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
  videoState->video_clock += frame_delay;

  return pts;
}

void video_refresh_timer(void* userdata)
{
  VideoState* videoState = (VideoState*)userdata;
  VideoPicture* videoPicture;

  double pts_delay;
  double audio_ref_clock;
  double sync_threshold;
  double real_delay;
  double audio_video_delay;

  if(videoState->video_st)
  {
    if(videoState->pictq_size == 0)
    {
      schedule_refresh(videoState, 1);
    }
    else
    {
      videoPicture = &videoState->pictq[videoState->pictq_rindex];

      if(_DEBUG_)
      {
        printf("Current Frame PTS:\t\t%f\n", videoPicture->pts);
        printf("Last Frame PTS:\t\t\t%f\n", videoState->frame_last_pts);
      }

      pts_delay = videoPicture->pts - videoState->frame_last_pts;

      if(_DEBUG_)
      {
        printf("PTS Delay:\t\t\t\t%f\n", pts_delay);
      }

      if (pts_delay <= 0 || pts_delay >= 1.0)
      {
        // use the previously calculated delay
        pts_delay = videoState->frame_last_delay;
      }

      if(_DEBUG_)
      {
        printf("Corrected PTS Delay:\t%f\n", pts_delay);
      }

      videoState->frame_last_delay = pts_delay;
      videoState->frame_last_pts = videoPicture->pts;

      if(videoState->av_sync_type != AV_SYNC_VIDEO_MASTER)
      {
        audio_ref_clock = get_master_clock(videoState);
        if(_DEBUG_)
        {
          printf("Ref clock: \t\t%f\n", audio_ref_clock);
        }
        audio_video_delay = videoPicture->pts - audio_ref_clock;
        if(_DEBUG_)
        {
          printf("Audio Video Delay:\t\t%f\n", audio_video_delay);
        }
        sync_threshold = (pts_delay > AV_SYNC_THRESHOLD) ? pts_delay : AV_SYNC_THRESHOLD;
        if(_DEBUG_)
        {
          printf("Sync Threshold:\t\t\t%f\n", sync_threshold);
        }
        if (fabs(audio_video_delay) < AV_NOSYNC_THRESHOLD)
        {
          if (audio_video_delay <= -sync_threshold)
          {
            pts_delay = 0;
          }
          else if (audio_video_delay >= sync_threshold)
          {
            pts_delay = 2 * pts_delay;
          }
        }
      }

      if(_DEBUG_)
      {
        printf("Corrected PTS delay:\t%f\n", pts_delay);
      }
      videoState->frame_timer += pts_delay;

      real_delay = videoState->frame_timer - (av_gettime() / 1000000.0);
      if(_DEBUG_)
      {
        printf("Corrected Real Delay:\t%f\n", real_delay);
      }

      if(real_delay < 0.010)
      {
        real_delay = 0.010;
      }

      if(_DEBUG_)
      {
        printf("Corrected Real Delay: \t%f\n", real_delay);
      }
      schedule_refresh(videoState, (int)(real_delay * 1000 + 0.5));
      
      if(_DEBUG_)
      {
        printf("Next Scheduled Refresh:\t%f\n\n", (double)(real_delay * 1000 + 0.5));
      }

      video_display(videoState);

      if(++videoState->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
      {
        videoState->pictq_rindex = 0;
      }

      SDL_LockMutex(videoState->pictq_mutex);
      videoState->pictq_size--;
      SDL_CondSignal(videoState->pictq_cond);
      SDL_UnlockMutex(videoState->pictq_mutex);
    }
  }
  else
  {
    schedule_refresh(videoState, 100);
  }
}

double get_audio_clock(VideoState * videoState)
{
  double pts = videoState->audio_clock;
  int hw_buf_size = videoState->audio_buf_size - videoState->audio_buf_index;
  int bytes_per_sec = 0;
  int n = 2 * videoState->audio_ctx->channels;

  if(videoState->audio_st)
  {
    bytes_per_sec = videoState->audio_ctx->sample_rate * n;
  }
  if(bytes_per_sec)
  {
    pts -= (double) hw_buf_size / bytes_per_sec;
  }

  return pts;
}

double get_video_clock(VideoState * videoState)
{
  double delta = (av_gettime() - videoState->video_current_pts_time) / 1000000.0;
  return videoState->video_current_pts + delta;
}

double get_external_clock(VideoState * videoState)
{
  videoState->external_clock_time = av_gettime();
  videoState->external_clock = videoState->external_clock_time / 1000000.0;

  return videoState->external_clock;
}

double get_master_clock(VideoState * videoState)
{
  if (videoState->av_sync_type == AV_SYNC_VIDEO_MASTER)
  {
    return get_video_clock(videoState);
  }
  else if (videoState->av_sync_type == AV_SYNC_AUDIO_MASTER)
  {
    return get_audio_clock(videoState);
  }
  else if (videoState->av_sync_type == AV_SYNC_EXTERNAL_MASTER)
  {
    return get_external_clock(videoState);
  }
  else
  {
    fprintf(stderr, "Error: Undefined A/V sync type.");
    return -1;
  }
}


static void schedule_refresh(VideoState * videoState, int delay)
{
  int ret = SDL_AddTimer(delay, sdl_refresh_timer_cb, videoState);
  if(0 == ret)
  {
    printf("Couldn't schedule refresh callback: %s\n", SDL_GetError());
  }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void * opaque)
{
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;

    SDL_PushEvent(&event);

    // return 0 to stop the timer
    return 0;
}

void video_display(VideoState * videoState)
{
  // create window, renderer and textures if not already created
  if (!screen)
  {
    // create a window with the specified position, dimensions, and flags.
    screen = SDL_CreateWindow(
      path,
      SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED,
      videoState->video_ctx->width / 2,
      videoState->video_ctx->height / 2,
      SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
    );
    SDL_GL_SetSwapInterval(1);
  }
  
  // check window was correctly created
  if (!screen)
  {
    printf("SDL: could not create window - exiting.\n");
    return;
  }

  if (!videoState->renderer)
  {
    videoState->renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
  }

  if (!videoState->texture)
  {
    videoState->texture = SDL_CreateTexture(
      videoState->renderer,
      SDL_PIXELFORMAT_YV12,
      SDL_TEXTUREACCESS_STREAMING,
      videoState->video_ctx->width,
      videoState->video_ctx->height
    );
  }

  // reference for the next VideoPicture to be displayed
  VideoPicture * videoPicture;

  float aspect_ratio;

  int w, h, x, y;

  // get next VideoPicture to be displayed from the VideoPicture queue
  videoPicture = &videoState->pictq[videoState->pictq_rindex];

  if (videoPicture->frame)
  {
    if (videoState->video_ctx->sample_aspect_ratio.num == 0)
    {
      aspect_ratio = 0;
    }
    else
    {
      aspect_ratio = av_q2d(videoState->video_ctx->sample_aspect_ratio) * videoState->video_ctx->width / videoState->video_ctx->height;
    }

    if (aspect_ratio <= 0.0)
    {
      aspect_ratio = (float)videoState->video_ctx->width /
      (float)videoState->video_ctx->height;
    }

    int screen_width;
    int screen_height;
    SDL_GetWindowSize(screen, &screen_width, &screen_height);

    h = screen_height;
    w = ((int) rint(h * aspect_ratio)) & -3; // & -3 mean round to the nearest 4

    if (w > screen_width)
    {
      w = screen_width;
      h = ((int) rint(w / aspect_ratio)) & -3;
    }

    x = (screen_width - w);
    y = (screen_height - h);

    // check the number of frames to decode was not exceeded
    if (++videoState->currentFrameIndex < videoState->maxFramesToDecode)
    {
      printf(
        "Frame %c (%d) pts %ld dts %ld key_frame %d [coded_picture_number %d, display_picture_number %d, %dx%d]\n",
        av_get_picture_type_char(videoPicture->frame->pict_type),
        videoState->video_ctx->frame_number,
        videoPicture->frame->pts,
        videoPicture->frame->pkt_dts,
        videoPicture->frame->key_frame,
        videoPicture->frame->coded_picture_number,
        videoPicture->frame->display_picture_number,
        videoPicture->frame->width,
        videoPicture->frame->height
      );

      SDL_Rect rect;
      rect.x = x;
      rect.y = y;
      rect.w = 2*w;
      rect.h = 2*h;

      SDL_LockMutex(screen_mutex);

      SDL_UpdateYUVTexture(
        videoState->texture,
        &rect,
        videoPicture->frame->data[0],
        videoPicture->frame->linesize[0],
        videoPicture->frame->data[1],
        videoPicture->frame->linesize[1],
        videoPicture->frame->data[2],
        videoPicture->frame->linesize[2]
      );
      // clear the current rendering target with the drawing color
      SDL_RenderClear(videoState->renderer);

      SDL_RenderCopy(videoState->renderer, videoState->texture, NULL, NULL);
      SDL_RenderPresent(videoState->renderer);

      SDL_UnlockMutex(screen_mutex);
    }
    else
    {
      SDL_Event event;
      event.type = FF_QUIT_EVENT;
      event.user.data1 = videoState;

      SDL_PushEvent(&event);
    }
  }
}

// initialize PacketQueue
void packet_queue_init(PacketQueue * q)
{
  memset(q, 0, sizeof(PacketQueue));

  // Create a new uninitialized mutex
  q->mutex = SDL_CreateMutex();
  if (!q->mutex)
  {
    printf("SDL_CreateMutex Error: %s.\n", SDL_GetError());
    return;
  }

  // Create a new condition variable
  q->cond = SDL_CreateCond();
  if (!q->cond)
  {
    // could not create condition variable
    printf("SDL_CreateCond Error: %s.\n", SDL_GetError());
    return;
  }
}

// put packet into PacketQueue
int packet_queue_put(PacketQueue* queue, AVPacket* packet)
{
  AVPacketList* avPacketList;
  avPacketList = av_malloc(sizeof(AVPacketList));

  if(!avPacketList)
  {
    return -1;
  }

  avPacketList->pkt = *packet;
  avPacketList->next = NULL;
  SDL_LockMutex(queue->mutex);

  if(!queue->last_pkt)
  {
    queue->first_pkt = avPacketList;
  }
  else
  {
    queue->last_pkt->next = avPacketList;
  }
  
  queue->last_pkt = avPacketList;
  queue->nb_packets++;
  queue->size += avPacketList->pkt.size;

  // notify packet_queue_get which is waiting that a new packet is available
  SDL_CondSignal(queue->cond);
  SDL_UnlockMutex(queue->mutex);
  
  return 0;
}

// get first packet from PacketQueue, store it in pkt
int packet_queue_get(PacketQueue* queue, AVPacket* pkt, int block)
{
  int ret;
  AVPacketList* avPacketList;

  SDL_LockMutex(queue->mutex);
  while(1)
  {
    if (global_video_state->quit)
    {
        ret = -1;
        break;
    }

    avPacketList = queue->first_pkt;
    if(avPacketList)
    {
      queue->first_pkt = avPacketList->next;
      if(!queue->first_pkt)
      {
        queue->last_pkt = NULL;
      }
      queue->nb_packets--;
      queue->size -= avPacketList->pkt.size;
      *pkt = avPacketList->pkt;
      av_free(avPacketList);

      ret = 1;
      break;
    }
    else if(!block)
    {
      ret = 0;
      break;
    }
    else
    {
      SDL_CondWait(queue->cond, queue->mutex);
    }
  }
  SDL_UnlockMutex(queue->mutex);
  return ret;
}

// get data from audio_decode_frame, store it in immediate buffer
// then write len number of bytes to stream 
void audio_callback(void* userdata, Uint8* stream, int len)
{
  VideoState * videoState = (VideoState *)userdata;
  double pts;

  while (len > 0)
  {
    // check global quit flag
    if (global_video_state->quit)
    {
      return;
    }

    if (videoState->audio_buf_index >= videoState->audio_buf_size)
    {
      // we have already sent all avaialble data; get more
      int audio_size = audio_decode_frame(videoState, videoState->audio_buf, sizeof(videoState->audio_buf), &pts);

      // if error
      if (audio_size < 0)
      {
        // output silence
        videoState->audio_buf_size = 1024;

        // clear memory
        memset(videoState->audio_buf, 0, videoState->audio_buf_size);
        printf("audio_decode_frame() failed.\n");
      }
      else
      {
        audio_size = synchronize_audio(videoState, (int16_t *)videoState->audio_buf, audio_size);

        // cast to usigned just to get rid of annoying warning messages
        videoState->audio_buf_size = (unsigned)audio_size;
      }
      videoState->audio_buf_index = 0;
    }

    int len1 = videoState->audio_buf_size - videoState->audio_buf_index;
    if (len1 > len)
    {
      len1 = len;
    }

    // copy data from audio buffer to the SDL stream
    memcpy(stream, (uint8_t *)videoState->audio_buf + videoState->audio_buf_index, len1);

    len -= len1;
    stream += len1;
    videoState->audio_buf_index += len1;
  }
}

// get packet data
int audio_decode_frame(VideoState* videoState, uint8_t* audio_buf, int buf_size, double* pts_ptr)
{
  AVPacket * avPacket = av_packet_alloc();
  if (avPacket == NULL)
  {
    printf("Couldn't allocate AVPacket.\n");
    return -1;
  }
  static uint8_t * audio_pkt_data = NULL;
  static int audio_pkt_size = 0;

  int pts;
  int n;

  // allocate a new frame, used to decode audio packets
  static AVFrame * avFrame = NULL;
  avFrame = av_frame_alloc();
  if (!avFrame)
  {
    printf("Couldn't allocate AVFrame.\n");
    return -1;
  }

  int len1 = 0;
  int data_size = 0;

  while(1)
  {
    // check global quit flag
    if (videoState->quit)
    {
      return -1;
    }

    while (audio_pkt_size > 0)
    {
      int got_frame = 0;

      int ret = avcodec_receive_frame(videoState->audio_ctx, avFrame);
      if (0 == ret)
      {
        got_frame = 1;
      }
      if (AVERROR(EAGAIN) == ret)
      {
        ret = 0;
      }
      if (0 == ret)
      {
        ret = avcodec_send_packet(videoState->audio_ctx, avPacket);
      }
      if (AVERROR(EAGAIN) == ret)
      {
        ret = 0;
      }
      else if (ret < 0)
      {
        printf("avcodec_receive_frame error");
        return -1;
      }
      else
      {
        len1 = avPacket->size;
      }

      if (len1 < 0)
      {
        // if error, skip frame
        audio_pkt_size = 0;
        break;
      }

      audio_pkt_data += len1;
      audio_pkt_size -= len1;
      data_size = 0;

      if (got_frame)
      {
        // resample audio to the decoded frame
        data_size = audio_resampling(
          videoState,
          avFrame,
          AV_SAMPLE_FMT_S16,
          audio_buf
        );

        assert(data_size <= buf_size);
      }

      if (data_size <= 0)
      {
          // no data yet, get more frames
          continue;
      }

      pts = videoState->audio_clock;
      *pts_ptr = pts;
      n = 2 * videoState->audio_ctx->channels;
      videoState->audio_clock += (double)data_size / (double)(n * videoState->audio_ctx->sample_rate);

      return data_size;
    }

    if (avPacket->data)
    {
      // wipe the packet
      av_packet_unref(avPacket);
    }

    // get more audio AVPacket
    int ret = packet_queue_get(&videoState->audioq, avPacket, 1);

    // if packet_queue_get returns < 0, the global quit flag was set
    if (ret < 0)
    {
      return -1;
    }

    audio_pkt_data = avPacket->data;
    audio_pkt_size = avPacket->size;

    /* And once more when we are done processing the packet */
    if (avPacket->pts != AV_NOPTS_VALUE)
    {
      videoState->audio_clock = av_q2d(videoState->audio_st->time_base)*avPacket->pts;
    }
  }

  return 0;
}


// do audio resampling, put the result to out_buf
static int audio_resampling(
  VideoState * videoState,
  AVFrame * decoded_audio_frame,
  enum AVSampleFormat out_sample_fmt,
  uint8_t * out_buf
)
{
  AudioResamplingState* arState = getAudioResampling(videoState->audio_ctx->channel_layout);

  if(!arState->swr_ctx)
  {
    printf("swr_alloc error\n");
    return -1;
  }

  arState->in_channel_layout = (
    videoState->audio_ctx->channels == av_get_channel_layout_nb_channels(
      videoState->audio_ctx->channel_layout
    )
  ) ? videoState->audio_ctx->channel_layout : av_get_default_channel_layout(videoState->audio_ctx->channels);

  // check input audio channels correctly retrieved
  if (arState->in_channel_layout <= 0)
  {
    printf("in_channel_layout error.\n");
    return -1;
  }

  // set output audio channels based on the input audio channels
  if (videoState->audio_ctx->channels == 1)
  {
    arState->out_channel_layout = AV_CH_LAYOUT_MONO;
  }
  else if (videoState->audio_ctx->channels == 2)
  {
    arState->out_channel_layout = AV_CH_LAYOUT_STEREO;
  }
  else
  {
    arState->out_channel_layout = AV_CH_LAYOUT_SURROUND;
  }

  arState->in_nb_samples = decoded_audio_frame->nb_samples;
  if(arState->in_nb_samples <= 0)
  {
    printf("in_nb_samples error\n");
    return -1;
  }

  // retrieve number of audio samples (per channel)
  arState->in_nb_samples = decoded_audio_frame->nb_samples;
  if (arState->in_nb_samples <= 0)
  {
    printf("in_nb_samples error.\n");
    return -1;
  }

  // set software resampler context
  av_opt_set_int(
    arState->swr_ctx,
    "in_channel_layout",
    arState->in_channel_layout,
    0
  );
  av_opt_set_int(
    arState->swr_ctx,
    "in_sample_rate",
    videoState->audio_ctx->sample_rate,
    0
  );
  av_opt_set_sample_fmt(
    arState->swr_ctx,
    "in_sample_fmt",
    videoState->audio_ctx->sample_fmt,
    0
  );
  av_opt_set_int(
    arState->swr_ctx,
    "out_channel_layout",
    arState->out_channel_layout,
    0
  );
  av_opt_set_int(
    arState->swr_ctx,
    "out_sample_rate",
    videoState->audio_ctx->sample_rate,
    0
  );
  av_opt_set_sample_fmt(
    arState->swr_ctx,
    "out_sample_fmt",
    out_sample_fmt,
    0
  );

  int ret = swr_init(arState->swr_ctx);
  if(ret < 0)
  {
    printf("Failed to initialize resampling context\n");
    return -1;
  }

  arState->max_out_nb_samples = arState->out_nb_samples = av_rescale_rnd(
    arState->in_nb_samples,
    videoState->audio_ctx->sample_rate,
    videoState->audio_ctx->sample_rate,
    AV_ROUND_UP
  );

  if(arState->max_out_nb_samples <= 0)
  {
    printf("av_rescale_rnd error\n");
    return -1;
  }

  // get number of output audio channels
  arState->out_nb_channels = av_get_channel_layout_nb_channels(arState->out_channel_layout);
  ret = av_samples_alloc_array_and_samples(
    &arState->resampled_data,
    &arState->out_linesize,
    arState->out_nb_channels,
    arState->out_nb_samples,
    out_sample_fmt,
    0
  );

  if(ret < 0)
  {
    printf("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.\n");
    return -1;
  }

  arState->out_nb_samples = av_rescale_rnd(
    swr_get_delay(arState->swr_ctx, videoState->audio_ctx->sample_rate) + arState->in_nb_samples,
    videoState->audio_ctx->sample_rate,
    videoState->audio_ctx->sample_rate,
    AV_ROUND_UP
  );
  if (arState->out_nb_samples <= 0)
  {
    printf("av_rescale_rnd error\n");
    return -1;
  }
  
  if (arState->out_nb_samples > arState->max_out_nb_samples)
  {
    av_free(arState->resampled_data[0]);

    ret = av_samples_alloc(
      arState->resampled_data,
      &arState->out_linesize,
      arState->out_nb_channels,
      arState->out_nb_samples,
      out_sample_fmt,
      1
    );

    if (ret < 0)
    {
        printf("av_samples_alloc failed.\n");
        return -1;
    }

    arState->max_out_nb_samples = arState->out_nb_samples;
  }

  if(arState->swr_ctx)
  {
    ret = swr_convert(
      arState->swr_ctx,
      arState->resampled_data,
      arState->out_nb_samples,
      (const uint8_t**)decoded_audio_frame->data,
      decoded_audio_frame->nb_samples
    );
    if(ret < 0)
    {
      printf("swr_convert_error\n");
      return -1;
    }
    arState->resampled_data_size = av_samples_get_buffer_size(
      &arState->out_linesize,
      arState->out_nb_channels,
      ret,
      out_sample_fmt,
      1
    );
    if(arState->resampled_data_size < 0)
    {
      printf("av_samples_get_buffer_size error\n");
      return -1;
    }
  }
  else
  {
    printf("swr_ctx null error\n");
    return -1;
  }
  memcpy(out_buf, arState->resampled_data[0], arState->resampled_data_size);

  if(arState->resampled_data)
  {
    av_freep(&arState->resampled_data[0]);
  }
  av_freep(&arState->resampled_data);
  arState->resampled_data = NULL;

  if(arState->swr_ctx)
  {
    swr_free(&arState->swr_ctx);
  }
  return arState->resampled_data_size;
}

char* get_filename(char* full_path)
{
  char *ssc;
  int l = 0;
  ssc = strstr(full_path, "/");
  do{
      l = strlen(ssc) + 1;
      full_path = &full_path[strlen(full_path)-l+2];
      ssc = strstr(full_path, "/");
  }while(ssc);
  return full_path;
}

// initialize AudioResamplingState
AudioResamplingState * getAudioResampling(uint64_t channel_layout)
{
  AudioResamplingState * audioResampling = av_mallocz(sizeof(AudioResamplingState));

  audioResampling->swr_ctx = swr_alloc();
  audioResampling->in_channel_layout = channel_layout;
  audioResampling->out_channel_layout = AV_CH_LAYOUT_STEREO;
  audioResampling->out_nb_channels = 0;
  audioResampling->out_linesize = 0;
  audioResampling->in_nb_samples = 0;
  audioResampling->out_nb_samples = 0;
  audioResampling->max_out_nb_samples = 0;
  audioResampling->resampled_data = NULL;
  audioResampling->resampled_data_size = 0;

  return audioResampling;
}

void stream_seek(VideoState * videoState, int64_t pos, int rel)
{
  if (!videoState->seek_req)
  {
    videoState->seek_pos = pos;
    videoState->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
    videoState->seek_req = 1;
  }
}

static void packet_queue_flush(PacketQueue * queue)
{
  AVPacketList *pkt, *pkt1;

  SDL_LockMutex(queue->mutex);

  for (pkt = queue->first_pkt; pkt != NULL; pkt = pkt1)
  {
    pkt1 = pkt->next;
    av_free_packet(&pkt->pkt);
    av_freep(&pkt);
  }

  queue->last_pkt = NULL;
  queue->first_pkt = NULL;
  queue->nb_packets = 0;
  queue->size = 0;

  SDL_UnlockMutex(queue->mutex);
}
