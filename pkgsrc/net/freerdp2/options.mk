# $NetBSD: options.mk,v 1.5 2024/08/25 06:19:06 wiz Exp $

PKG_OPTIONS_VAR=		PKG_OPTIONS.freerdp2
PKG_SUPPORTED_OPTIONS=		alsa pcsc pulseaudio cups wayland x11 ffmpeg
PKG_SUGGESTED_OPTIONS=		x11 pcsc ffmpeg
PKG_SUGGESTED_OPTIONS.Linux=	alsa

.include "../../devel/wayland/platform.mk"
.if ${PLATFORM_SUPPORTS_WAYLAND} == "yes"
PKG_SUGGESTED_OPTIONS+=		wayland
.endif

.include "../../mk/bsd.options.mk"

.if !empty(PKG_OPTIONS:Malsa)
CMAKE_CONFIGURE_ARGS+=	-DWITH_ALSA=ON
.include "../../audio/alsa-lib/buildlink3.mk"
.else
CMAKE_CONFIGURE_ARGS+=	-DWITH_ALSA=OFF
.endif

.if !empty(PKG_OPTIONS:Mpulseaudio)
CMAKE_CONFIGURE_ARGS+=	-DWITH_PULSE=ON
.include "../../audio/pulseaudio/buildlink3.mk"
.else
CMAKE_CONFIGURE_ARGS+=	-DWITH_PULSE=OFF
.endif

.if !empty(PKG_OPTIONS:Mcups)
CMAKE_CONFIGURE_ARGS+=	-DWITH_CUPS=ON
.include "../../print/libcups/buildlink3.mk"
.else
CMAKE_CONFIGURE_ARGS+=	-DWITH_CUPS=OFF
.endif

PLIST_VARS+=	x11
.if !empty(PKG_OPTIONS:Mx11)
PLIST.x11=	yes
CMAKE_CONFIGURE_ARGS+=	-DWITH_X11=ON
.include "../../x11/libxkbfile/buildlink3.mk"
.include "../../x11/libX11/buildlink3.mk"
.include "../../x11/libXcursor/buildlink3.mk"
.include "../../x11/libXdamage/buildlink3.mk"
.include "../../x11/libXext/buildlink3.mk"
.include "../../x11/libXfixes/buildlink3.mk"
.include "../../x11/libXi/buildlink3.mk"
.include "../../x11/libXinerama/buildlink3.mk"
.include "../../x11/libXrandr/buildlink3.mk"
.include "../../x11/libXrender/buildlink3.mk"
.include "../../x11/libXtst/buildlink3.mk"
.include "../../x11/libXv/buildlink3.mk"
.else
CMAKE_CONFIGURE_ARGS+=	-DWITH_X11=OFF
.endif

.if !empty(PKG_OPTIONS:Mpcsc)
.  include "../../security/pcsc-lite/buildlink3.mk"
CMAKE_CONFIGURE_ARGS+=   -DWITH_PCSC=ON
PLIST.pcsc=    yes
.else
CMAKE_CONFIGURE_ARGS+=   -DWITH_PCSC=OFF
.endif

PLIST_VARS+=	wayland
.if !empty(PKG_OPTIONS:Mwayland)
.  include "../../devel/wayland/buildlink3.mk"
.  include "../../x11/libxkbcommon/buildlink3.mk"
PLIST.wayland=	yes
CMAKE_CONFIGURE_ARGS+=   -DWITH_WAYLAND=ON
.else
CMAKE_CONFIGURE_ARGS+=   -DWITH_WAYLAND=OFF
.endif

# LISPBSD: FFmpeg (H.264 graphics, AAC/MP3 audio) as an option.  Without it
# FreeRDP uses its own codecs (RemoteFX, progressive, planar) and needs no
# ffmpeg6, whose lame-4.0 dependency would replace the lame ffmpeg8 uses.
.if !empty(PKG_OPTIONS:Mffmpeg)
.  include "../../multimedia/ffmpeg6/buildlink3.mk"
CMAKE_CONFIGURE_ARGS+=	-DAVCODEC_LIBRARY=${PREFIX}/lib/ffmpeg6/libavcodec.so
.else
CMAKE_CONFIGURE_ARGS+=	-DWITH_FFMPEG=OFF -DWITH_DSP_FFMPEG=OFF -DWITH_SWSCALE=OFF
.endif
