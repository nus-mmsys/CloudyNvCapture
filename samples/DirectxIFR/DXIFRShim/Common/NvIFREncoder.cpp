/*!
 * \brief
 * Base class of NvIFREncoder for which the D3Dx Encoders are derived.
 * These classes are defined to expose an convenient NvIFR interface to the
 * standard D3D functions.
 *
 * \file
 *
 * This is a more advanced D3D sample showing how to use NVIFR to 
 * capture a render target to a file. It shows how to load the NvIFR dll, 
 * initialize the function pointers, and create the NvIFR object.
 * 
 * It then goes on to show how to set up the target buffers and events in 
 * the NvIFR object and then calls NvIFRTransferRenderTargetToH264HWEncoder to 
 * transfer the render target to the hardware encoder, and get the encoded 
 * stream.
 *
 * \copyright
 * CopyRight 1993-2016 NVIDIA Corporation.  All rights reserved.
 * NOTICE TO LICENSEE: This source code and/or documentation ("Licensed Deliverables")
 * are subject to the applicable NVIDIA license agreement
 * that governs the use of the Licensed Deliverables.
 */

/* We must include d3d9.h here. NvIFRLibrary needs d3d9.h to be included before itself.*/
#include <d3d9.h>
#include <process.h>
#include <NvIFRLibrary.h>
#include "NvIFREncoder.h"
#include "Util4Streamer.h"
#include <chrono>
#include <thread>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern "C"
{
	#include <libavutil/avassert.h>
	#include <libavutil/channel_layout.h>
	#include <libavutil/opt.h>
	#include <libavutil/mathematics.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libswresample/swresample.h>
	#include <libavutil/buffer.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avdevice.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "postproc.lib")
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "avutil.lib")

#pragma comment(lib, "winmm.lib")

extern simplelogger::Logger *logger;

#define PIXEL_SIZE 1.5
#define NUMFRAMESINFLIGHT 1 // Limit is 3? Putting 4 causes an invalid parameter error to be thrown.
#define MAX_PLAYERS 12

HANDLE gpuEvent = NULL;
uint8_t *buffer = NULL;

#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */
#define SCALE_FLAGS SWS_BICUBIC

int splitWidth, splitHeight;
int bufferWidth, bufferHeight;
int rows, cols;
int topRightX[MAX_PLAYERS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int topRightY[MAX_PLAYERS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

AVFormatContext *outCtxArray[MAX_PLAYERS] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
AVDictionary *optionsOutput[MAX_PLAYERS] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
int ret[MAX_PLAYERS] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
AVDictionary *opt[MAX_PLAYERS] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
AVOutputFormat *fmt[MAX_PLAYERS] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

int numThreads[MAX_PLAYERS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
bool serverOpened[MAX_PLAYERS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// a wrapper around a single output AVStream
typedef struct OutputStream {
	AVStream *st;
	AVCodecContext *enc;

	/* pts of the next frame that will be generated */
	int64_t next_pts;
	int samples_count;

	AVFrame *frame;
	AVFrame *tmp_frame;

	float t, tincr, tincr2;

	struct SwsContext *sws_ctx;
	struct SwrContext *swr_ctx;
} OutputStream;

OutputStream video_st[MAX_PLAYERS];
//OutputStream video_st2 = { 0 };

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/* Write the compressed frame to the media file. */
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
	AVCodec **codec,
enum AVCodecID codec_id)
{
	AVCodecContext *c;

	/* find the encoder */
	*codec = avcodec_find_encoder(codec_id);
	if (!(*codec)) {
		fprintf(stderr, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
		exit(1);
	}

	ost->st = avformat_new_stream(oc, NULL);
	if (!ost->st) {
		fprintf(stderr, "Could not allocate stream\n");
		exit(1);
	}
	ost->st->id = oc->nb_streams - 1;
	c = avcodec_alloc_context3(*codec);
	if (!c) {
		fprintf(stderr, "Could not alloc an encoding context\n");
		exit(1);
	}
	ost->enc = c;

    AVRational time_base = { 1, STREAM_FRAME_RATE };
    AVRational framerate = { 25, 1 };

	c->codec_id = codec_id;
	c->bit_rate = 400000;
	/* Resolution must be a multiple of two. */
	c->width = splitWidth;
	c->height = splitHeight;
	/* timebase: This is the fundamental unit of time (in seconds) in terms
	* of which frame timestamps are represented. For fixed-fps content,
	* timebase should be 1/framerate and timestamp increments should be
	* identical to 1. */
	ost->st->time_base = time_base;
	c->time_base = ost->st->time_base;
	c->delay = 0;
	c->framerate = framerate;
	c->has_b_frames = 0;
	c->max_b_frames = 0;
	c->rc_min_vbv_overflow_use = 400000;
	c->thread_count = 1;

	c->gop_size = 30; /* emit one intra frame every twelve frames at most */
	c->pix_fmt = STREAM_PIX_FMT;
	if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
		/* just for testing, we also add B-frames */
		c->max_b_frames = 0; // original: 2
	}
	if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
		/* Needed to avoid using macroblocks in which some coeffs overflow.
		* This does not happen with normal video, it just happens here as
		* the motion of the chroma plane does not match the luma plane. */
		c->mb_decision = 2;
	}

	av_opt_set(c->priv_data, "preset", "ultrafast", 0);
	av_opt_set(c->priv_data, "tune", "zerolatency", 0);
	av_opt_set(c->priv_data, "x264opts", "crf=2:vbv-maxrate=4000:vbv-bufsize=160:intra-refresh=1:slice-max-size=2000:keyint=30:ref=1", 0);

	/* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
}

/**************************************************************/
/* video output */

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	int ret;

	picture = av_frame_alloc();
	if (!picture)
		return NULL;

	picture->format = pix_fmt;
	picture->width = width;
	picture->height = height;

	/* allocate the buffers for the frame data */
	ret = av_frame_get_buffer(picture, 32);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate frame data.\n");
		exit(1);
	}

	return picture;
}

static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
	int ret;
	AVCodecContext *c = ost->enc;
	AVDictionary *opt = NULL;

	av_dict_copy(&opt, opt_arg, 0);

	/* open the codec */
	ret = avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);
	if (ret < 0) {
		fprintf(stderr, "Could not open video codec\n");
		exit(1);
	}

	/* allocate and init a re-usable frame */
	ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
	if (!ost->frame) {
		fprintf(stderr, "Could not allocate video frame\n");
		exit(1);
	}

	/* If the output format is not YUV420P, then a temporary YUV420P
	* picture is needed too. It is then converted to the required
	* output format. */
	ost->tmp_frame = NULL;
	if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
		ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
		if (!ost->tmp_frame) {
			fprintf(stderr, "Could not allocate temporary picture\n");
			exit(1);
		}
	}

	/* copy the stream parameters to the muxer */
	ret = avcodec_parameters_from_context(ost->st->codecpar, c);
	if (ret < 0) {
		fprintf(stderr, "Could not copy the stream parameters\n");
		exit(1);
	}
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, uint8_t *buffer, int topRightX, int topRightY)
{
    pict->width = bufferWidth; // This has to be the original dimensions of the original frame buffer
	pict->height = bufferHeight;

    // width and height parameters are the width and height of actual output.
    // Function requires top right corner coordinates.

	pict->format = AV_PIX_FMT_YUV420P; 

    AVFrame *temp = av_frame_alloc();
    avpicture_fill((AVPicture*)temp, buffer, AV_PIX_FMT_YUV420P, pict->width, pict->height);
    av_picture_crop((AVPicture*)pict, (AVPicture*)temp, AV_PIX_FMT_YUV420P, topRightX, topRightY);
}

static AVFrame* get_video_frame(OutputStream *ost, uint8_t *buffer, int topRightX, int topRightY)
{
	AVCodecContext *c = ost->enc;

	/* when we pass a frame to the encoder, it may keep a reference to it
	* internally; make sure we do not overwrite it here */
	if (av_frame_make_writable(ost->frame) < 0)
		exit(1);

	if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
		/* as we only generate a YUV420P picture, we must convert it
		* to the codec pixel format if needed */
		if (!ost->sws_ctx) {
			ost->sws_ctx = sws_getContext(c->width, c->height,
				AV_PIX_FMT_YUV420P,
				c->width, c->height,
				c->pix_fmt,
				SCALE_FLAGS, NULL, NULL, NULL);
			if (!ost->sws_ctx) {
				fprintf(stderr, "Could not initialize the conversion context\n");
				exit(1);
			}
		}
        fill_yuv_image(ost->tmp_frame, buffer, topRightX, topRightY);
		sws_scale(ost->sws_ctx,
			(const uint8_t * const *)ost->tmp_frame->data, ost->tmp_frame->linesize,
			0, c->height, ost->frame->data, ost->frame->linesize);
	}
	else {
        fill_yuv_image(ost->frame, buffer, topRightX, topRightY);
	}

	ost->frame->pts = ost->next_pts++;

	return ost->frame;
}

/*
* encode one video frame and send it to the muxer
* return 1 when encoding is finished, 0 otherwise
*/
static int write_video_frame(AVFormatContext *oc, OutputStream *ost, uint8_t *buffer, int topRightX, int topRightY)
{
	int ret;
	AVCodecContext *c;
	AVFrame *frame;
	int got_packet = 0;
	AVPacket pkt = { 0 };

	c = ost->enc;

	frame = get_video_frame(ost, buffer, topRightX, topRightY);

	av_init_packet(&pkt);

	/* encode the image */
	ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
	if (ret < 0) {
		fprintf(stderr, "Error encoding video frame\n");
		exit(1);
	}

	if (got_packet) {
		ret = write_frame(oc, &c->time_base, ost->st, &pkt);
	}
	else {
		ret = 0;
	}

	if (ret < 0) {
		fprintf(stderr, "Error while writing video frame\n");
		exit(1);
	}

	return (frame || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
	avcodec_free_context(&ost->enc);
	av_frame_free(&ost->frame);
	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
	swr_free(&ost->swr_ctx);
}


BOOL NvIFREncoder::StartEncoder() 
{
	hevtStopEncoder = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!hevtStopEncoder) {
		LOG_ERROR(logger, "Failed to create hevtStopEncoder");
		return FALSE;
	}
	bStopEncoder = FALSE;

	hevtInitEncoderDone = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!hevtInitEncoderDone) {
		LOG_ERROR(logger, "Failed to create hevtInitEncoderDone");
		return FALSE;
	}
	bInitEncoderSuccessful = FALSE;

	hthEncoder = (HANDLE)_beginthread(EncoderThreadStartProc, 0, this);
	if (!hthEncoder) {
		return FALSE;
	}

	WaitForSingleObject(hevtInitEncoderDone, INFINITE);
	CloseHandle(hevtInitEncoderDone);
	hevtInitEncoderDone = NULL;

	return bInitEncoderSuccessful;
}

void NvIFREncoder::StopEncoder() 
{
	if (bStopEncoder || !hevtStopEncoder || !hthEncoder) {
		return;
	}

	bStopEncoder = TRUE;
	SetEvent(hevtStopEncoder);
	WaitForSingleObject(hthEncoder, INFINITE);
	CloseHandle(hevtStopEncoder);
	hevtStopEncoder = NULL;
}

void NvIFREncoder::FFMPEGThreadProc(int playerIndex)
{
    if (serverOpened[playerIndex] == false)
    {
        const char *filename = NULL;
        AVCodec *video_codec;

        topRightX[playerIndex] = playerIndex % cols * splitWidth;

        if (playerIndex >= 0 && playerIndex <= 3) {
            topRightY[playerIndex] = 0;
        }
        else if (playerIndex >= 4 && playerIndex <= 7) {
            topRightY[playerIndex] = splitHeight;
        }
        else if (playerIndex >= 8 && playerIndex <= 11) {
            topRightY[playerIndex] = splitHeight * 2;
        }
        
        /* Initialize libavcodec, and register all codecs and formats. */
        av_register_all();
        // Global initialization of network components
        avformat_network_init();

        /* allocate the output media context */
        avformat_alloc_output_context2(&outCtxArray[playerIndex], NULL, NULL, "output.h264");
        if (!outCtxArray[playerIndex]) {
            printf("Could not deduce output format from file extension: using h264.\n");
            LOG_WARN(logger, "Could not deduce output format from file extension: using h264.");
            avformat_alloc_output_context2(&outCtxArray[playerIndex], NULL, "h264", filename);
        }
        if (!outCtxArray[playerIndex]) {
            fprintf(stderr, "No output context.\n");
            LOG_WARN(logger, "No output context.");
            return;
        }

        fmt[playerIndex] = outCtxArray[playerIndex]->oformat;

        /* Add the audio and video streams using the default format codecs
        * and initialize the codecs. */
        if (fmt[playerIndex]->video_codec != AV_CODEC_ID_NONE) {
            add_stream(&video_st[playerIndex], outCtxArray[playerIndex], &video_codec, fmt[playerIndex]->video_codec);
        }

        if ((ret[playerIndex] = av_dict_set(&opt[playerIndex], "re", "", 0)) < 0) {
            fprintf(stderr, "Failed to set -re mode.\n");
            return;
        }

        /* Now that all the parameters are set, we can open the audio and
        * video codecs and allocate the necessary encode buffers. */
        open_video(outCtxArray[playerIndex], video_codec, &video_st[playerIndex], opt[playerIndex]);
        av_dump_format(outCtxArray[playerIndex], 0, filename, 1);

        AVDictionary *optionsOutput[MAX_PLAYERS] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

        if ((ret[playerIndex] = av_dict_set(&optionsOutput[playerIndex], "listen", "1", 0)) < 0) {
            fprintf(stderr, "Failed to set listen mode for server.\n");
            return;
        }

        if ((ret[playerIndex] = av_dict_set(&optionsOutput[playerIndex], "an", "", 0)) < 0) {
            fprintf(stderr, "Failed to set -an mode.\n");
            return;
        }

        std::stringstream *HTTPUrl = new std::stringstream();
        *HTTPUrl << "http://172.26.186.80:" << 30000 + playerIndex;

        // Open server
        if ((avio_open2(&outCtxArray[playerIndex]->pb, HTTPUrl->str().c_str(), AVIO_FLAG_WRITE, NULL, &optionsOutput[playerIndex])) < 0) {
            fprintf(stderr, "Failed to open server %d.\n", playerIndex);
            LOG_ERROR(logger, "Failed to open server " << playerIndex << ".");
            return;
        }
        LOG_DEBUG(logger, "Server " << playerIndex << " opened.");

        /* Write the stream header, if any. */
        ret[playerIndex] = avformat_write_header(outCtxArray[playerIndex], &opt[playerIndex]);
        if (ret[playerIndex] < 0) {
            fprintf(stderr, "Error occurred when opening output file.\n");
            LOG_ERROR(logger, "Error occurred when opening output file.\n");
            return;
        }

        serverOpened[playerIndex] = true;
    }

    write_video_frame(outCtxArray[playerIndex], &video_st[playerIndex], buffer, topRightY[playerIndex], topRightX[playerIndex]);
    numThreads[playerIndex]--;    

	_endthread();
}

void NvIFREncoder::EncoderThreadProc() 
{
    splitWidth = pAppParam->splitWidth;
    splitHeight = pAppParam->splitHeight;
    bufferWidth = pAppParam->width;
    bufferHeight = pAppParam->height;
    rows = pAppParam->rows;
    cols = pAppParam->cols;

	/*Note: 
	1. The D3D device for encoding must be create on a seperate thread other than the game rendering thread. 
	Otherwise, some games (such as Mass Effect 2) will run abnormally. That's why SetupNvIFR()
	is called here instead of inside the subclass constructor.
	2. The D3D device (or swapchain) and the window bound with it must be created in 
	the same thread, or you get D3DERR_INVALIDCALL.*/
	if (!SetupNvIFR()) {
		LOG_ERROR(logger, "Failed to setup NvIFR.");
		SetEvent(hevtInitEncoderDone);
		CleanupNvIFR();
		return;
	}
    
	NVIFR_TOSYS_SETUP_PARAMS params = { 0 }; 
	params.dwVersion = NVIFR_TOSYS_SETUP_PARAMS_VER; 
	params.eFormat = NVIFR_FORMAT_YUV_420;
	params.eSysStereoFormat = NVIFR_SYS_STEREO_NONE; 
	params.dwNBuffers = NUMFRAMESINFLIGHT; 
	params.dwTargetWidth = pAppParam->width;
	params.dwTargetHeight = pAppParam->height;
	params.ppPageLockedSysmemBuffers = &buffer;
	params.ppTransferCompletionEvents = &gpuEvent; 
    
	NVIFRRESULT nr = pIFR->NvIFRSetUpTargetBufferToSys(&params);
    
	if (nr != NVIFR_SUCCESS) {
		LOG_ERROR(logger, "NvIFRSetUpTargetBufferToSys failed, nr=" << nr);
		SetEvent(hevtInitEncoderDone);
		CleanupNvIFR();
		return;
	}
	LOG_DEBUG(logger, "NvIFRSetUpTargetBufferToSys succeeded");

    // At this point, servers will all be open, but not all of them have received a client.
    // A flag will need to be toggled to let the main thread know which servers have clients.
    // Then, we can write frames to only those servers with clients.

    bInitEncoderSuccessful = TRUE;
    SetEvent(hevtInitEncoderDone);

    while (!bStopEncoder)
    {
        if (!UpdateBackBuffer())
        {
            LOG_DEBUG(logger, "UpdateBackBuffer() failed");
        }
   
        NVIFRRESULT res = pIFR->NvIFRTransferRenderTargetToSys(0);
   
        if (res == NVIFR_SUCCESS)
        {
            DWORD dwRet = WaitForSingleObject(gpuEvent, INFINITE);
            if (dwRet != WAIT_OBJECT_0)// If not signalled
            {
                if (dwRet != WAIT_OBJECT_0 + 1)
                {
                    LOG_WARN(logger, "Abnormally break from encoding loop, dwRet=" << dwRet);
                }
                return;
            }
   
            if (pAppParam->numPlayers > 0 && numThreads[0] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc0, 0, this);
                numThreads[0]++;
            }
            if (pAppParam->numPlayers > 1 && numThreads[1] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc1, 0, this);
                numThreads[1]++;
            }
            if (pAppParam->numPlayers > 2 && numThreads[2] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc2, 0, this);
                numThreads[2]++;
            }
            if (pAppParam->numPlayers > 3 && numThreads[3] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc3, 0, this);
                numThreads[3]++;
            }
            if (pAppParam->numPlayers > 4 && numThreads[4] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc4, 0, this);
                numThreads[4]++;
            }
            if (pAppParam->numPlayers > 5 && numThreads[5] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc5, 0, this);
                numThreads[5]++;
            }
            if (pAppParam->numPlayers > 6 && numThreads[6] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc6, 0, this);
                numThreads[6]++;
            }
            if (pAppParam->numPlayers > 7 && numThreads[7] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc7, 0, this);
                numThreads[7]++;
            }
            if (pAppParam->numPlayers > 8 && numThreads[8] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc8, 0, this);
                numThreads[8]++;
            }
            if (pAppParam->numPlayers > 9 && numThreads[9] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc9, 0, this);
                numThreads[9]++;
            }
            if (pAppParam->numPlayers > 10 && numThreads[10] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc10, 0, this);
                numThreads[10]++;
            }
            if (pAppParam->numPlayers > 11 && numThreads[11] < 1)
            {
                FFMPEGThread = (HANDLE)_beginthread(FFMPEGThreadStartProc11, 0, this);
                numThreads[11]++;
            }
            
            ResetEvent(gpuEvent);
        }
        else
        {
            LOG_ERROR(logger, "NvIFRTransferRenderTargetToSys failed, res=" << res);
        }
        // Prevent doing extra work (25 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    LOG_DEBUG(logger, "Quit encoding loop");

    for (int i = 0; i < pAppParam->numPlayers; i++)
    {
        /* Write the trailer, if any. The trailer must be written before you
         * close the CodecContexts open when you wrote the header; otherwise
         * av_write_trailer() may try to use memory that was freed on
         * av_codec_close(). */
        av_write_trailer(outCtxArray[i]);

        /* Close each codec. */
        close_stream(outCtxArray[i], &video_st[i]);

        if (!(fmt[i]->flags & AVFMT_NOFILE))
            /* Close the output file. */
            avio_closep(&outCtxArray[i]->pb);

        /* free the stream */
        avformat_free_context(outCtxArray[i]);
    }

	CleanupNvIFR();
}

Streamer * NvIFREncoder::pSharedStreamer = NULL;