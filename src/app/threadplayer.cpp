#ifdef ENABLE_DISPLAY
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <functional>
#include <vector>
#include <list>
#include <map>
#include "frames.h"
#include "m2decoder.h"
#include "filewrite.h"
#include "getopt.h"
#include "md5.h"
#ifdef __GNUC__
#include <stdint.h>
#endif

#include "unithread.h"

struct Buffer {
	uint8_t *data;
	int len;
};

template <typename T>
class Queue {
	T *data_;
	int max_;
	int head_;
	int tail_;
	UniMutex *mutex_;
	UniCond *cond_;
	bool terminated_;
	int next(int idx) const {
		++idx;
		return idx < max_ ? idx : 0;
	}
public:
	Queue(T *data, int buf_num)
		: data_(data), max_(buf_num), head_(0), tail_(0),
		  mutex_(UniCreateMutex()), cond_(UniCreateCond()),
		  terminated_(false) {}
	~Queue() {
		UniDestroyCond(cond_);
		UniDestroyMutex(mutex_);
	}
	bool full() const {
		return next(head_) == tail_;
	}
	bool empty() const {
		return head_ == tail_;
	}
	int size() const {
		return max_;
	}
	bool terminated() const {
		return terminated_;
	}
	void terminate() {
		UniLockMutex(mutex_);
		terminated_ = true;
		UniCondSignal(cond_);
		UniUnlockMutex(mutex_);
	}
	T& emptybuf() {
		UniLockMutex(mutex_);
		while (full()) {
			UniCondWait(cond_, mutex_);
		}
		T& d = data_[head_];
		UniUnlockMutex(mutex_);
		return d;
	}
	void setfilled(T& dat) {
		assert(!full());
		UniLockMutex(mutex_);
		data_[head_] = dat;
		head_ = next(head_);
		UniCondSignal(cond_);
		UniUnlockMutex(mutex_);
	}
	T& getfilled() {
		if (empty() && terminated()) {
			return data_[tail_];
		}
		UniLockMutex(mutex_);
		while (empty() && !terminated()) {
			UniCondWait(cond_, mutex_);
		}
		int tail = tail_;
		tail_ = next(tail_);
		T& d = data_[tail];
		UniCondSignal(cond_);
		UniUnlockMutex(mutex_);
		return d;
	}
};

class FileReader {
	std::list<char *> infiles_;
	FILE *fd_;
	int insize_;
public:
	FileReader(std::list<char *> &infiles, int insize)
		: infiles_(infiles), fd_(0), insize_(insize) {
		if (infiles_.empty() || !(fd_ = fopen(infiles_.front(), "rb"))) {
			fprintf(stderr, "Error on Input File.\n");
			return;
		}
		infiles_.pop_front();
	}
	int read_block(Buffer& dst) {
		int read_size = fread(dst.data, 1, insize_, fd_);
		dst.len = read_size;
		if (read_size == 0) {
			fclose(fd_);
			if (!infiles_.empty() && (fd_ = fopen(infiles_.front(), "rb"))) {
				infiles_.pop_front();
				dst.len = read_size = fread(dst.data, 1, insize_, fd_);
			} else {
				return -1;
			}
		}
		return read_size;
	}
};

class FileReaderUnit {
public:
	typedef Queue<Buffer> QueueType;
	FileReaderUnit(Buffer *src_p, int buf_num, int insize, std::list<char *> &infiles)
		: fr_(infiles, insize), outqueue_(FileReaderUnit::QueueType(src_p, buf_num)) {}
	FileReaderUnit::QueueType& outqueue() {
		return outqueue_;
	}
	static int run(void *data) {
		return ((FileReaderUnit *)data)->run_impl();
	}
private:
	FileReader fr_;
	QueueType outqueue_;
	int run_impl() {
		int err;
		RecordTime(1);
		for (;;) {
			Buffer& buf = outqueue_.emptybuf();
			err = fr_.read_block(buf);
			if (err < 0) {
				break;
			}
			outqueue_.setfilled(buf);
		}
		outqueue_.terminate();
		fprintf(stderr, "File terminate.\n");
		RecordTime(0);
		return 0;
	}
};

class M2DecoderUnit {
public:
	typedef Queue<Frame> QueueType;
	M2DecoderUnit(FileReaderUnit::QueueType& inqueue, Frame *dst, int dstnum, int codec_mode)
		: m2dec_(codec_mode, reread_file, this),
		  inqueue_(inqueue), outqueue_(QueueType(dst, dstnum)) {}
	FileReaderUnit::QueueType& inqueue() {
		return inqueue_;
	}
	QueueType& outqueue() {
		return outqueue_;
	}
	M2Decoder& dec() {
		return m2dec_;
	}
	static int run(void *data) {
		return ((M2DecoderUnit *)data)->run_impl();
	}
private:
	M2Decoder m2dec_;
	FileReaderUnit::QueueType& inqueue_;
	M2DecoderUnit::QueueType outqueue_;
	static void post_dst(void *obj, Frame& frm) {
		M2DecoderUnit *ths = (M2DecoderUnit *)obj;
		Frame& dst = ths->outqueue().emptybuf();
		dst = frm;
		ths->outqueue().setfilled(dst);
	}
	static int reread_file(void *arg) {
		return ((M2DecoderUnit *)arg)->reread_file_impl();
	}
	int reread_file_impl() {
		Buffer& src = inqueue().getfilled();
		if (src.len <= 0) {
			return -1;
		} else {
			dec_bits *stream = dec().demuxer()->stream;
			stream = stream ? stream : dec().stream();
			dec_bits_set_data(stream, src.data, src.len);
			return 0;
		}
	}
	int run_impl() {
		RecordTime(1);
		while (0 <= dec().decode(this, post_dst)) {}
		outqueue().terminate();
		RecordTime(0);
		return 0;
	}
};

void display_write(uint8_t **dst, const uint8_t *src_luma, const uint8_t *src_chroma, int src_stride,
		   uint16_t *pitches, int width, int height)
{
	uint8_t *dst0 = dst[0];
	for (int i = 0; i < height; ++i) {
		memcpy(dst0, src_luma, width);
		src_luma += src_stride;
		dst0 += pitches[0];
	}
	width >>= 1;
	height >>= 1;
	uint8_t *dst1 = dst[1];
	uint8_t *dst2 = dst[2];
	for (int i = 0; i < height; ++i) {
		for (int j = 0; j < width; ++j) {
			dst1[j] = src_chroma[j * 2];
			dst2[j] = src_chroma[j * 2 + 1];
		}
		src_chroma += src_stride;
		dst1 += pitches[1];
		dst2 += pitches[2];
	}
}

const int MAX_WIDTH = 1920;
const int MAX_HEIGHT = 1088;
const int MAX_LEN = (MAX_WIDTH * MAX_HEIGHT * 3) >> 1;
const int FILE_READ_SIZE = 65536 * 7;
const int BUFNUM = 5;

static void BlameUser() {
	fprintf(stderr,
		"Usage: srview [-s] [-r] [-t interval] [-m outfile(MD5)] [-o outfile(Raw)] infile [infile ...]\n"
		"\t-h : H.264 Elementary Data\n"
		"\t-s : MPEG-2 Program Stream (PS)\n"
		"\t-r : repeat\n"
		"\t-l : log dump\n"
		"\t-t interval : specify interval of each frame in ms unit\n");
	exit(-1);
}

struct Options {
	int interval_;
	std::list<char *> infile_list_;
	std::list<FileWriter *> fw_;
	int codec_mode_;
	bool repeat_;
	bool logdump_;

	Options(int argc, char **argv)
		: interval_(0),
		  codec_mode_(M2Decoder::MODE_MPEG2), repeat_(false), logdump_(false) {
		int opt;
		while ((opt = getopt(argc, argv, "hlm:o:rst:")) != -1) {
			FILE *fo;
			switch (opt) {
			case 'h':
				codec_mode_ = M2Decoder::MODE_H264;
				break;
			case 'l':
				logdump_ = true;
				break;
			case 'm':
				fo = fopen(optarg, "wb");
				if (fo) {
					fw_.push_back(new FileWriterMd5(fo));
				}
				break;
			case 'o':
				fo = fopen(optarg, "wb");
				if (fo) {
					fw_.push_back(new FileWriterRaw(fo));
				}
				break;
			case 'r':
				repeat_ = true;
				break;
			case 's':
				codec_mode_ = M2Decoder::MODE_MPEG2PS;
				break;
			case 't':
				interval_ = strtoul(optarg, 0, 0);
				if (interval_ == 0) {
					interval_ = 1;
				}
				break;
			default:
				BlameUser();
				/* NOTREACHED */
			}
		}
		if (argc <= optind) {
			BlameUser();
			/* NOTREACHED */
		}
		do {
			infile_list_.push_back(argv[optind]);
		} while (++optind < argc);
	}
};

static void waitevents_with_timer(SDL_Overlay *yuv, SDL_Rect *rect, int &suspended)
{
	UniEvent ev;
	UniWaitEvent(&ev);
	do {
		switch (ev.type) {
		case SDL_USEREVENT:
			SDL_DisplayYUVOverlay(yuv, rect);
			break;
		case SDL_MOUSEBUTTONDOWN:
			suspended ^= 1;
			break;
		case SDL_QUIT:
			exit(0);
			/* NOTREACHED */
			break;
		}
	} while (SDL_PollEvent(&ev));
}

static void waitevents(SDL_Overlay *yuv, SDL_Rect *rect,  int &suspended)
{
	UniEvent ev;
	SDL_DisplayYUVOverlay(yuv, rect);
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_MOUSEBUTTONDOWN:
			suspended ^= 1;
			break;
		case SDL_QUIT:
			exit(0);
			/* NOTREACHED */
			break;
		}
	}
}

struct WriteFrame : std::binary_function<FileWriter *, Frame *, void> {
	void operator()(FileWriter *fw, const Frame *out) const {
		fw->writeframe(out);
	}
};

struct DeleteFw {
	void operator()(FileWriter *fw) const {
		delete fw;
	}
};

void run_loop(Options& opt) {
	int width = 0;
	int height = 0;
	SDL_Surface *surface;
	SDL_Overlay *yuv = 0;
	SDL_Rect rect = {
		0, 0, width, height
	};

	LogTags.insert(std::pair<int, const char *> (UniThreadID(), "Main"));
	RecordTime(1);

	/* Run File Loader */
	Buffer src[BUFNUM];
	for (int i = 0; i < BUFNUM; ++i) {
		src[i].data = new unsigned char[FILE_READ_SIZE];
		src[i].len = FILE_READ_SIZE;
	}
	FileReaderUnit fr(src, BUFNUM,  FILE_READ_SIZE, opt.infile_list_);
	UniThread *thr_file = UniCreateThread(FileReaderUnit::run, (void *)&fr);
	LogTags.insert(std::pair<int, const char *> (UniGetThreadID(thr_file), "FileLoader"));

	/* Run Video Decoder */
	Frame dst_align[BUFNUM];
 	M2DecoderUnit m2dec(fr.outqueue(), dst_align, BUFNUM, opt.codec_mode_);
	UniThread *thr_m2d = UniCreateThread(M2DecoderUnit::run, (void *)&m2dec);
	M2DecoderUnit::QueueType &outqueue = m2dec.outqueue();
	LogTags.insert(std::pair<int, const char *> (UniGetThreadID(thr_m2d), "Decoder"));

	UniEvent ev;
	while (UniPollEvent(&ev)) ;
	int suspended = 0;
	while (1) {
		if (!suspended) {
			if (outqueue.terminated() && outqueue.empty()) {
				goto endloop;
			}
			Frame& out = outqueue.getfilled();
			if ((width != (out.width - out.crop[1])) || (height != (out.height - out.crop[3]))) {
				width = out.width - out.crop[1];
				height = out.height - out.crop[3];
				rect.w = width;
				rect.h = height;
				surface = SDL_SetVideoMode(width, height, 0, SDL_SWSURFACE);
				if (yuv) {
					SDL_FreeYUVOverlay(yuv);
				}
				yuv = SDL_CreateYUVOverlay(width, height, SDL_IYUV_OVERLAY, surface);
			}
			SDL_LockYUVOverlay(yuv);
			display_write(yuv->pixels, out.luma, out.chroma, out.width, yuv->pitches, yuv->w, yuv->h);
			SDL_UnlockYUVOverlay(yuv);
			for_each(opt.fw_.begin(), opt.fw_.end(), std::bind2nd(WriteFrame(), &out));
		} else {
			UniDelay(100);
		}
		if (opt.interval_) {
			waitevents_with_timer(yuv, &rect, suspended);
		} else {
			waitevents(yuv, &rect, suspended);
		}
	}
endloop:
	int status;

	UniWaitThread(thr_m2d, &status);
	RecordTime(0);
	for_each(opt.fw_.begin(), opt.fw_.end(), DeleteFw());
	if (yuv) {
		SDL_FreeYUVOverlay(yuv);
		yuv = 0;
	}
	for (int i = 0; i < BUFNUM; ++i) {
		delete[] src[i].data;
	}
}

Uint32 DispTimer(Uint32 interval, void *param)
{
	SDL_Event ev;
	SDL_UserEvent user_ev;
	user_ev.type = SDL_USEREVENT;
	user_ev.code = 0;
	user_ev.data1 = 0;
	user_ev.data2 = 0;
	ev.type = SDL_USEREVENT;
	ev.user = user_ev;
	UniPushEvent(&ev);
	return interval;
}

#ifdef main
#undef main
#endif
#ifdef _M_IX86
#include <crtdbg.h>
#endif
#endif /* ENABLE_DISPLAY */

int main(int argc, char **argv)
{
#ifdef ENABLE_DISPLAY
	SDL_TimerID timer;

#ifdef _M_IX86
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_WNDW);
//	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_WNDW);
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|_CRTDBG_LEAK_CHECK_DF);
	atexit((void (*)(void))_CrtCheckMemory);
#endif
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO) < 0) {
		return -1;
	}
	atexit(SDL_Quit);
	Options opt(argc, argv);
	LogInit();
	atexit(LogFin);
	if (opt.logdump_) {
		atexit(LogDump);
	}
	SDL_EventState(SDL_ACTIVEEVENT, SDL_IGNORE);
	SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);
	if (opt.interval_) {
		timer = SDL_AddTimer(opt.interval_, DispTimer, 0);
	}
	do {
		run_loop(opt);
	} while (opt.repeat_);

	if (opt.interval_) {
		SDL_RemoveTimer(timer);
	}
#endif /* ENABLE_DISPLAY */
	return 0;
}

