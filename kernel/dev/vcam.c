/*
 * LispBSD vcam(4): a virtual camera for Firefox and other V4L2 readers.
 *
 * A feeder process opens /dev/vcam (one at a time), sets the format with
 * VCAMIOCSFORMAT and writes one complete MJPEG frame per write(2).  While
 * a reader has started the stream on the video(4) device (videoN at
 * vcam0), each frame is handed to the video layer as one payload; with no
 * stream running the frames are counted and dropped, and VCAMIOCWAIT lets
 * the feeder sleep until someone wants pictures, so an unused camera costs
 * nothing.
 */
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

#include <dev/video_if.h>
#include <dev/vcamio.h>

#include "ioconf.h"

#define VCAM_DEFAULT_W		1024
#define VCAM_DEFAULT_H		768
#define VCAM_DEFAULT_MAX	(1024 * 1024)
#define VCAM_MAX_MAX		(16 * 1024 * 1024)

struct vcam_softc {
	device_t		sc_dev;
	device_t		sc_videodev;
	kmutex_t		sc_lock;
	kcondvar_t		sc_cv;
	struct video_format	sc_format;
	bool			sc_feeder;	/* /dev/vcam is open */
	bool			sc_streaming;	/* start_transfer .. stop_transfer */
	u_int			sc_readers;
	int			sc_frameno;
	uint8_t			*sc_buf;
	size_t			sc_bufsize;
	uint64_t		sc_frames, sc_dropped;
};

static int	vcam_match(device_t, cfdata_t, void *);
static void	vcam_attach(device_t, device_t, void *);
static int	vcam_detach(device_t, int);

CFATTACH_DECL3_NEW(vcam, sizeof(struct vcam_softc),
    vcam_match, vcam_attach, vcam_detach, NULL, NULL, NULL,
    DVF_DETACH_SHUTDOWN);

static dev_type_open(vcamopen);
static dev_type_close(vcamclose);
static dev_type_write(vcamwrite);
static dev_type_ioctl(vcamioctl);

const struct cdevsw vcam_cdevsw = {
	.d_open = vcamopen,
	.d_close = vcamclose,
	.d_read = noread,
	.d_write = vcamwrite,
	.d_ioctl = vcamioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER | D_MPSAFE
};

/* video(4) side */
static int	vcam_video_open(void *, int);
static void	vcam_video_close(void *);
static const char *vcam_get_devname(void *);
static const char *vcam_get_businfo(void *);
static int	vcam_enum_format(void *, uint32_t, struct video_format *);
static int	vcam_get_format(void *, struct video_format *);
static int	vcam_set_format(void *, struct video_format *);
static int	vcam_try_format(void *, struct video_format *);
static int	vcam_start_transfer(void *);
static int	vcam_stop_transfer(void *);

static const struct video_hw_if vcam_hw_if = {
	.open = vcam_video_open,
	.close = vcam_video_close,
	.get_devname = vcam_get_devname,
	.get_businfo = vcam_get_businfo,
	.enum_format = vcam_enum_format,
	.get_format = vcam_get_format,
	.set_format = vcam_set_format,
	.try_format = vcam_try_format,
	.start_transfer = vcam_start_transfer,
	.stop_transfer = vcam_stop_transfer,
};

static void
vcam_set_dims(struct vcam_softc *sc, uint32_t w, uint32_t h, uint32_t max)
{
	memset(&sc->sc_format, 0, sizeof(sc->sc_format));
	sc->sc_format.pixel_format = VIDEO_FORMAT_MJPEG;
	sc->sc_format.width = w;
	sc->sc_format.height = h;
	sc->sc_format.aspect_x = 4;
	sc->sc_format.aspect_y = 3;
	sc->sc_format.sample_size = max;
	sc->sc_format.stride = 0;
	sc->sc_format.color.primaries = VIDEO_COLOR_PRIMARIES_UNSPECIFIED;
	sc->sc_format.color.gamma_function = VIDEO_GAMMA_FUNCTION_UNSPECIFIED;
	sc->sc_format.color.matrix_coeff = VIDEO_MATRIX_COEFF_UNSPECIFIED;
	sc->sc_format.interlace_flags = VIDEO_INTERLACE_ON;
}

static int
vcam_match(device_t parent, cfdata_t cf, void *aux)
{
	return 1;
}

static void
vcam_attach(device_t parent, device_t self, void *aux)
{
	struct vcam_softc *sc = device_private(self);

	sc->sc_dev = self;
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&sc->sc_cv, "vcamwait");
	vcam_set_dims(sc, VCAM_DEFAULT_W, VCAM_DEFAULT_H, VCAM_DEFAULT_MAX);
	sc->sc_bufsize = VCAM_DEFAULT_MAX;
	sc->sc_buf = kmem_alloc(sc->sc_bufsize, KM_SLEEP);

	aprint_naive("\n");
	aprint_normal(": LispBSD virtual camera (MJPEG from /dev/vcam)\n");

	sc->sc_videodev = video_attach_mi(&vcam_hw_if, self, sc);
	if (sc->sc_videodev == NULL)
		aprint_error_dev(self, "no video(4) device attached\n");

	if (!pmf_device_register(self, NULL, NULL))
		aprint_error_dev(self, "couldn't establish power handler\n");
}

static int
vcam_detach(device_t self, int flags)
{
	struct vcam_softc *sc = device_private(self);
	int error;

	if (sc->sc_feeder)
		return EBUSY;
	if (sc->sc_videodev != NULL) {
		error = config_detach(sc->sc_videodev, flags);
		if (error != 0)
			return error;
		sc->sc_videodev = NULL;
	}
	pmf_device_deregister(self);
	kmem_free(sc->sc_buf, sc->sc_bufsize);
	cv_destroy(&sc->sc_cv);
	mutex_destroy(&sc->sc_lock);
	return 0;
}

/* pseudo-device entry: one unit, attached at boot */
void
vcamattach(int num)
{
	cfdata_t cf;
	int error;

	error = config_cfattach_attach(vcam_cd.cd_name, &vcam_ca);
	if (error != 0) {
		aprint_error("%s: unable to register cfattach\n",
		    vcam_cd.cd_name);
		return;
	}
	cf = kmem_alloc(sizeof(*cf), KM_SLEEP);
	cf->cf_name = vcam_cd.cd_name;
	cf->cf_atname = vcam_cd.cd_name;
	cf->cf_unit = 0;
	cf->cf_fstate = FSTATE_STAR;
	(void)config_attach_pseudo(cf);
}

/* ---- the feeder's character device ---- */

static int
vcamopen(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct vcam_softc *sc = device_lookup_private(&vcam_cd, minor(dev));

	if (sc == NULL)
		return ENXIO;
	mutex_enter(&sc->sc_lock);
	if (sc->sc_feeder) {
		mutex_exit(&sc->sc_lock);
		return EBUSY;
	}
	sc->sc_feeder = true;
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
vcamclose(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct vcam_softc *sc = device_lookup_private(&vcam_cd, minor(dev));

	mutex_enter(&sc->sc_lock);
	sc->sc_feeder = false;
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
vcamwrite(dev_t dev, struct uio *uio, int flags)
{
	struct vcam_softc *sc = device_lookup_private(&vcam_cd, minor(dev));
	struct video_payload payload;
	size_t len;
	int error;

	len = uio->uio_resid;
	if (len == 0)
		return 0;
	if (len > sc->sc_bufsize)
		return EMSGSIZE;
	/* One feeder at a time (exclusive open), so the buffer is ours. */
	error = uiomove(sc->sc_buf, len, uio);
	if (error != 0)
		return error;

	mutex_enter(&sc->sc_lock);
	if (!sc->sc_streaming || sc->sc_videodev == NULL) {
		sc->sc_dropped++;
		mutex_exit(&sc->sc_lock);
		return 0;
	}
	sc->sc_frameno ^= 1;
	payload.data = sc->sc_buf;
	payload.size = len;
	payload.frameno = sc->sc_frameno;
	payload.end_of_frame = true;
	sc->sc_frames++;
	mutex_exit(&sc->sc_lock);

	/* copies into the stream's sample buffer */
	video_submit_payload(sc->sc_videodev, &payload);
	return 0;
}

static int
vcamioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct vcam_softc *sc = device_lookup_private(&vcam_cd, minor(dev));
	struct vcam_format *vf;
	struct vcam_state *vs;
	uint32_t ms;
	int error = 0;

	switch (cmd) {
	case VCAMIOCSFORMAT:
		vf = data;
		if (vf->width < 16 || vf->height < 16 ||
		    vf->width > 8192 || vf->height > 8192 ||
		    vf->maxsize < 4096 || vf->maxsize > VCAM_MAX_MAX)
			return EINVAL;
		mutex_enter(&sc->sc_lock);
		if (sc->sc_streaming) {
			mutex_exit(&sc->sc_lock);
			return EBUSY;
		}
		if (vf->maxsize > sc->sc_bufsize) {
			uint8_t *nb;
			size_t nsize = vf->maxsize;
			mutex_exit(&sc->sc_lock);
			nb = kmem_alloc(nsize, KM_SLEEP);
			mutex_enter(&sc->sc_lock);
			kmem_free(sc->sc_buf, sc->sc_bufsize);
			sc->sc_buf = nb;
			sc->sc_bufsize = nsize;
		}
		vcam_set_dims(sc, vf->width, vf->height, vf->maxsize);
		mutex_exit(&sc->sc_lock);
		break;
	case VCAMIOCGFORMAT:
		vf = data;
		mutex_enter(&sc->sc_lock);
		vf->width = sc->sc_format.width;
		vf->height = sc->sc_format.height;
		vf->maxsize = sc->sc_format.sample_size;
		mutex_exit(&sc->sc_lock);
		break;
	case VCAMIOCGSTATE:
		vs = data;
		mutex_enter(&sc->sc_lock);
		vs->streaming = sc->sc_streaming;
		vs->readers = sc->sc_readers;
		vs->frames = sc->sc_frames;
		vs->dropped = sc->sc_dropped;
		mutex_exit(&sc->sc_lock);
		break;
	case VCAMIOCWAIT:
		ms = *(uint32_t *)data;
		mutex_enter(&sc->sc_lock);
		while (!sc->sc_streaming) {
			if (ms == 0)
				error = cv_wait_sig(&sc->sc_cv, &sc->sc_lock);
			else {
				error = cv_timedwait_sig(&sc->sc_cv, &sc->sc_lock,
				    mstohz(ms));
				if (error == 0 && !sc->sc_streaming)
					continue;
				if (error == EWOULDBLOCK)
					break;
			}
			if (error != 0)
				break;
		}
		mutex_exit(&sc->sc_lock);
		break;
	default:
		error = ENOTTY;
	}
	return error;
}

/* ---- the video(4) side ---- */

static int
vcam_video_open(void *opaque, int flags)
{
	struct vcam_softc *sc = opaque;

	mutex_enter(&sc->sc_lock);
	sc->sc_readers++;
	mutex_exit(&sc->sc_lock);
	return 0;
}

static void
vcam_video_close(void *opaque)
{
	struct vcam_softc *sc = opaque;

	mutex_enter(&sc->sc_lock);
	if (sc->sc_readers > 0)
		sc->sc_readers--;
	mutex_exit(&sc->sc_lock);
}

static const char *
vcam_get_devname(void *opaque)
{
	return "LispBSD virtual camera";
}

static const char *
vcam_get_businfo(void *opaque)
{
	return "vcam";
}

static int
vcam_enum_format(void *opaque, uint32_t index, struct video_format *format)
{
	struct vcam_softc *sc = opaque;

	if (index != 0)
		return EINVAL;
	mutex_enter(&sc->sc_lock);
	*format = sc->sc_format;
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
vcam_get_format(void *opaque, struct video_format *format)
{
	return vcam_enum_format(opaque, 0, format);
}

static int
vcam_try_format(void *opaque, struct video_format *format)
{
	struct vcam_softc *sc = opaque;

	/* There is one format; a reader gets it whatever it asked for. */
	mutex_enter(&sc->sc_lock);
	*format = sc->sc_format;
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
vcam_set_format(void *opaque, struct video_format *format)
{
	return vcam_try_format(opaque, format);
}

static int
vcam_start_transfer(void *opaque)
{
	struct vcam_softc *sc = opaque;

	mutex_enter(&sc->sc_lock);
	sc->sc_streaming = true;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_lock);
	return 0;
}

static int
vcam_stop_transfer(void *opaque)
{
	struct vcam_softc *sc = opaque;

	mutex_enter(&sc->sc_lock);
	sc->sc_streaming = false;
	mutex_exit(&sc->sc_lock);
	return 0;
}
