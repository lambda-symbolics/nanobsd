/*
 * LispBSD vcam(4): a virtual camera.  A feeder opens /dev/vcam, sets the
 * frame format, and writes one whole MJPEG frame per write(2); the frames
 * come out of the video(4) device vcam attached (videoN) for V4L2 readers.
 */
#ifndef _DEV_VCAMIO_H_
#define _DEV_VCAMIO_H_

#include <sys/ioccom.h>

struct vcam_format {
	uint32_t	width;
	uint32_t	height;
	uint32_t	maxsize;	/* largest frame in bytes */
};

struct vcam_state {
	uint32_t	streaming;	/* a reader has started the stream */
	uint32_t	readers;	/* open video(4) descriptors */
	uint64_t	frames;		/* delivered */
	uint64_t	dropped;	/* written with no stream running */
};

#define VCAMIOCSFORMAT	_IOW('V', 1, struct vcam_format)
#define VCAMIOCGFORMAT	_IOR('V', 2, struct vcam_format)
#define VCAMIOCGSTATE	_IOR('V', 3, struct vcam_state)
/* Sleep until a reader streams (arg: timeout in ms, 0 = forever); 0 or EWOULDBLOCK. */
#define VCAMIOCWAIT	_IOW('V', 4, uint32_t)

#endif /* _DEV_VCAMIO_H_ */
