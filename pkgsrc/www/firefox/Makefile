# $NetBSD: Makefile,v 1.668 2026/07/29 13:58:19 ryoon Exp $

FIREFOX_VER=		${MOZ_BRANCH}${MOZ_BRANCH_MINOR}
MOZ_BRANCH=		153.0
MOZ_BRANCH_MINOR=	.1

DISTNAME=	firefox-${FIREFOX_VER}.source
PKGNAME=	${DISTNAME:S/.source//:S/b/beta/:S/esr//}
CATEGORIES=	www
MASTER_SITES+=	${MASTER_SITE_MOZILLA:=firefox/releases/${FIREFOX_VER}/source/}
MASTER_SITES+=	${MASTER_SITE_MOZILLA_ALL:=firefox/releases/${FIREFOX_VER}/source/}
EXTRACT_SUFX=	.tar.xz

NODEJSKIT=		nodejs-output-153.0.tgz
DISTFILES=		${DEFAULT_DISTFILES} ${NODEJSKIT}
SITES.${NODEJSKIT}=	${MASTER_SITE_LOCAL}

MAINTAINER=	ryoon@NetBSD.org
HOMEPAGE=	https://www.mozilla.org/en-US/firefox/
COMMENT=	Web browser with support for extensions (version ${FIREFOX_VER:tu:C/\\.[[:digit:]\.]*//})
LICENSE=	mpl-1.1

# -------- BEFORE UPDATING THIS PACKAGE PLEASE READ & UNDERSTAND: -------
#
# This package works around a (stupid) build time dependency on nodejs
# (which is not available for all architectures and unnecessary for the
# real build). To do this, it places some additional burden on the
# maintainer.
#
# While working on the package, please make sure you have
#
#	FIREFOX_MAINTAINER=yes
#
# set in your build environment.
# When the package is ready for commit (but before commit), do:
#
#	make maintainer-files
#
# This will do another round (depending on state of your work dir at this
# moment) of one or two builds and generate a cache of all output that
# nodejs generates during a build.
#
# When this is done, just commit the results (they will be in the files/
# directory).
#
# -----------------------------------------------------------------------

WRKSRC=		${WRKDIR}/firefox-${FIREFOX_VER:C/b.*//}

MOZILLA_DIR=	# empty

# Note: In --enable-chrome-format=flat case,
#       when updating remember to conditionalise about-background.png in PLIST
CONFIGURE_ARGS+=	--enable-application=browser

#CFLAGS+=	-I${PREFIX}/include/nspr
# for lang/gcc6
CFLAGS+=	-D_GLIBCXX_INCLUDE_NEXT_C_HEADERS
CFLAGS.SunOS+=	-D_POSIX_PTHREAD_SEMANTICS
# Do not use uselocale() in third_party/pipewire.
CFLAGS.NetBSD+=	-D__LOCALE_C_ONLY

LDFLAGS+=		${COMPILER_RPATH_FLAG}${PREFIX}/lib/${PKGBASE}
LDFLAGS+=		${COMPILER_RPATH_FLAG}${PREFIX}/lib
LDFLAGS.DragonFly+=	-lplc4 -lnspr4
LDFLAGS.FreeBSD+=	-lplc4 -lnspr4
LDFLAGS.Linux+=		-lnspr4
LDFLAGS.SunOS+=		-lm

# Should revisit to complete mprotect support.
NOT_PAX_MPROTECT_SAFE+=	lib/${PKGBASE}/firefox
NOT_PAX_MPROTECT_SAFE+=	lib/${PKGBASE}/firefox-bin

# Avoid ld "invalid section index" errors.
BUILDLINK_TRANSFORM.SunOS+=	rm:-fdata-sections
BUILDLINK_TRANSFORM.SunOS+=	rm:-ffunction-sections
BUILDLINK_TRANSFORM.SunOS+=	rm:-pie

BUILDLINK_TRANSFORM.SunOS+=	rm:-Wl,-rpath-link,${WRKDIR}/build/dist/bin
BUILDLINK_TRANSFORM.SunOS+=	rm:-Wl,-rpath-link,${PREFIX}/lib

# Workaround for https://bugs.llvm.org/show_bug.cgi?id=46366
BUILDLINK_TRANSFORM.NetBSD+=	rm:-fexperimental-new-pass-manager

SUBST_CLASSES+=			dfly_malloc_h
SUBST_STAGE.dfly_malloc_h=	pre-configure
SUBST_MESSAGE.dfly_malloc_h=	Dont include malloc.h on dragonflybsd
SUBST_SED.dfly_malloc_h=	-e 's,HAVE_MALLOC_H,HAVE_MALLOC_H \&\& !defined(__DragonFly__),g'
SUBST_FILES.dfly_malloc_h+=	media/ffvpx/libavutil/mem.c

SUBST_CLASSES+=			paths
SUBST_STAGE.paths=		pre-configure
SUBST_FILES.paths=		../firefox.sh
SUBST_VARS.paths=		PREFIX MOZILLA

# LispBSD: the patched glean-core files' entries in its .cargo-checksum.json
# (cargo refuses a vendored crate whose files changed).
CKSUM_CRATES+=		third_party/rust/glean-core
CKSUMS+=		e62d58b090618e0af5000fdbf927a59a2286d48a396b4e2d5b10f90f936ad768
CKSUMS+=		b49469dd2d47534d488aa6fbcbede610f7305a86a55072bb5372018b1feb0d75
CKSUMS+=		de31eb3d942801d82edbf26e61f5b20d0083246ac4dc965d75faf7bfcc386420
CKSUMS+=		ad9303f40436100e867d41da62f25ee132c8f6e533a99af7ea09fa3e2fa731b1
CKSUMS+=		9541aa19680873661710715e60e723dbdb80ae35df29d59ff0c2594ddb04d0b4
CKSUMS+=		b0c0e3ef30c6d7d558d281c78cfc55cfb3156f5f5a26e683fe8fda5cbfc3c9e1
CKSUMS+=		d1dce0c3502be724d54085ac4fdd980d8fe8c60b98a7efd57f92074fd9adaba7
CKSUMS+=		fc003df658aaa5ee35eaded605eab8c84c3951569f03b1a4d68bf0efefb00de8
CKSUMS+=		0818727d7d109bca7be98b6842e0c50daf03feaadc1a8341fedbdc290c40bac9
CKSUMS+=		bcf357f9ddf40d36241e4789d5035959bbe3e912c4c68a6e0e6c4fb70f294e62
CKSUMS+=		f38db0aae52dd2eba726b76a9a246c70e15ed9dc303c3d1df33229b9fdc3826b
CKSUMS+=		1ae521569ac4df6eeacc73db34dfcdaa6234a93897feaeabadfb74bad009b1f2
CKSUMS+=		f79103475b656b631861431e7b943956d94b1a92621f2e05d6fbca0312673c20
CKSUMS+=		58f148d10798ea97b9750b65e9e920b742ba78c86501b83b79aa36e4bc03660d
CKSUMS+=		b513a2a4cc2ab3208ed175112cd742cd97cf9071c3ea8c40aed4d3de376db95a
CKSUMS+=		864210d585eb9a4285110dc34d9df24fbdbab688d8233a5ac08c3861659971c2
CKSUMS+=		de8853a691980606745895c083c22036347117b3d556de66a9d9978ec0f51168
CKSUMS+=		3e0ea969828af7ae1b2be8bde9bca64ff414a4641735fc541c74b7128e932c41
CKSUMS+=		2f2e15b32dc8f0a831e57c17dc3f2279280b58c701cf717f5a6da7ee87e441c4
CKSUMS+=		36f5e41e371e0d9edb8e552c1b800716e49bf260f8d9b18e6a265dc1f24d7016
CKSUMS+=		065e3203b7c82a88937c5241be5964ee2d12f9c8404c57f7fd2e15a5cf7f0469
CKSUMS+=		95622e74e3d0f18cf42521390a23ce219b5971a9661184d72902a71769e6c4be
CKSUMS+=		316f966a5997bceaccf56066f6a4349b26eef4f8f79b380317ee217a217cbe4e
CKSUMS+=		64a29f189014942ce2872b699ca299431a6869e3080e38c1dc07fe5ddea960ca
CKSUMS+=		955d377487c0968a9c2112897e7a0352c53aaf01d79954a323c2223152973d27
CKSUMS+=		04951e043bd1a6d0281f2e87e7e02b878dc80aa9e68102010d4199dc17f15192
CKSUMS+=		4286be7d12d23ecf50573b18a98f828c7645f1f115f9569cda63506f4dde987c
CKSUMS+=		9220882439b2dfb2d070eb7b4d53cb756ae89b6bd3a8393624f17722a7214277
CKSUMS+=		961cba993cc2c004ee3699dade3170c2eda267e9fd4d6c52677e0951cbdc9656
CKSUMS+=		a9494a77f2bf580886aad0e4fdffaf403f5e534b99ffd57b540d3c067b152759
CKSUMS+=		82478ce7326e453fbe44b58355d0d48c73a58cee9be3c68af47e76841fb0506e
CKSUMS+=		29035fe8b65debe72b7d50f7eebaf2ac978ea12d43074e35bd93ce5243676370
CKSUMS+=		b876e3553c35642d137e89c631bd327689d45395579459e4d446772ec2a79147
CKSUMS+=		b7d4fd14787adf296ce2e3c8d0182cea9badd15297b3718eac6d82752333ad6e
CKSUMS+=		2b66e9db1efeb831e6691855058e1dee154122c382d9d66cc2afe138e8599b7a
CKSUMS+=		a058fc83c3b6daecba58cc260b2d946eaf96893c6597e7ae59e1db12f816fd34
CKSUMS+=		9b4bc78d21ebb7265e91eda7ae110117246569a67b9e289e039f457bc5d47264
CKSUMS+=		46f3a652a40917415e71fe6f6ccf452c12984bc2230df82f8f1b5b14b1f8a0bd
CKSUMS+=		16185d33aba689c20a4cadda397376efda5de9b91a4fe5bbede9db6ab7513c32
CKSUMS+=		8463b293ac2f757726a43beae280b59adb9e5afefd36d2a9b066101005e59691

.include "mozilla-common.mk"
.include "options.mk"

CHECK_INTERPRETER_SKIP+=	lib/firefox-sdk/sdk/bin/header.py
CHECK_INTERPRETER_SKIP+=	lib/firefox-sdk/sdk/bin/typelib.py
CHECK_INTERPRETER_SKIP+=	lib/firefox-sdk/sdk/bin/xpidl.py
CHECK_INTERPRETER_SKIP+=	lib/firefox-sdk/sdk/bin/xpt.py

CHECK_WRKREF_SKIP+=	lib/${MOZILLA}/omni.ja

MOZILLA=	${PKGBASE}

.if !empty(PKG_OPTIONS:Mofficial-mozilla-branding)
MOZILLA_NAME=		Firefox
MOZILLA_BRANDING=	official
.else
MOZILLA_NAME=		Browser
MOZILLA_BRANDING=	unofficial
.endif

pre-configure:
# As of 106.0, .in template files are not patched.
#	cd ${WRKSRC} && autoconf
#	cd ${WRKSRC}/js/src && autoconf
	cd ${WRKSRC} && mkdir ${OBJDIR}
	cd ${WRKSRC}/${OBJDIR} && touch old-configure.vars
#	Do not fetch Rust Cargo file via network during build
.if !defined(FIREFOX_MAINTAINER)
	mv ${WRKDIR}/dist ${WRKSRC}/${OBJDIR}
.endif

.if defined(FIREFOX_MAINTAINER)
# Create files needed only by the firefox maintainer when updating
# the package
# XXX - manually removing the .*_done files is wrong!
.PHONY: build-list
build-list:
	cd ${WRKSRC}/${OBJDIR}/dist/bin/browser/chrome && \
		find . -type f | sort > ${OUT:Q}

NODE_LIST=	"${WRKDIR}/node.list"
NO_NODE_LIST=	"${WRKDIR}/no-node.list"
NODE_FILES=	"${WRKDIR}/node.flist"

.PHONY: maintainer-files
maintainer-files:
	rm -f ${FILESDIR}/node-wrapper.sh
	V=$$( node -v ) && \
	    printf '#! /bin/sh\n\nVERS=%s\n\nif [ "$$1" = "-v" ] || [ "$$1" = "--version" ]; then\n\tprintf "$${VERS}\\n"\nfi\n\nexit 0\n' $$V \
	    > ${FILESDIR}/node-wrapper.sh && \
	    chmod 0755 ${FILESDIR}/node-wrapper.sh
	rm -f ${WRKDIR}/.build_done ${WRKDIR}/.configure_done
	${MAKE} MAINTAINER_INTERNAL=yes build
	${MAKE} MAINTAINER_INTERNAL=yes OUT="${NO_NODE_LIST}" build-list
	${MAKE} OUT="${NODE_LIST}" build-list
	${DIFF} -u "${NO_NODE_LIST}" "${NODE_LIST}" | \
	    ${AWK} \
	    '/^\+\.\//{ printf("dist/bin/browser/chrome/%s\n", gensub(/^\+\.\//, "", "")) }' \
	    > "${NODE_FILES}"
	cd ${WRKSRC}/${OBJDIR} && tar -c -T "${NODE_FILES}" -z \
	    -f ${FILESDIR}/nodejs-output-${PKGVERSION_NOREV}.tgz
.endif

pre-patch:
	for f in $$(find ${WRKSRC}/third_party/libwebrtc -name moz.build -type f) ; \
	do \
		${AWK} -f ${FILESDIR}/replace-moz.build.awk $$f > $$f.new; mv $$f.new $$f ; \
	done
	for f in $$(find ${WRKSRC}/third_party/abseil-cpp -name moz.build -type f) ; \
	do \
		${AWK} -f ${FILESDIR}/replace-moz.build.awk $$f > $$f.new; mv $$f.new $$f ; \
	done

post-build:
	${SED} -e 's|@MOZILLA@|${MOZILLA}|g'				\
	  -e 's|@MOZILLA_NAME@|${MOZILLA_NAME}|g'			\
	  -e 's|@FIREFOX_ICON@|${MOZILLA}|g'				\
	  < ${FILESDIR}/desktop.in					\
	  > ${WRKDIR}/desktop

INSTALLATION_DIRS+=	share/applications

post-extract:
	${CP} ${FILESDIR}/firefox.sh ${WRKDIR}/firefox.sh
	# LispBSD: wlr-screencopy screen capture (see patches/patch-third__party_libwebrtc_*)
	${MKDIR} ${WRKSRC}/third_party/libwebrtc/modules/desktop_capture/linux/wayland_wlr
	${CP} ${FILESDIR}/wlr/* ${WRKSRC}/third_party/libwebrtc/modules/desktop_capture/linux/wayland_wlr/

post-install:
.if ${OPSYS} == "NetBSD" && ${X11_TYPE} == "native"
	${INSTALL_SCRIPT} ${WRKDIR}/firefox.sh ${DESTDIR}${PREFIX}/bin/${MOZILLA}
.else
	${ECHO} '#! /bin/sh' > ${DESTDIR}${PREFIX}/bin/${MOZILLA}
	${ECHO} '${PREFIX}/lib/${MOZILLA}/${MOZILLA} "$$@"' >> \
		${DESTDIR}${PREFIX}/bin/${MOZILLA}
	${CHMOD} 755 ${DESTDIR}${PREFIX}/bin/${MOZILLA}
.endif
	${INSTALL_DATA} ${WRKDIR}/desktop				\
	  ${DESTDIR}${PREFIX}/share/applications/${MOZILLA}.desktop
.for i in 16 22 24 32 48 64 128 256
	${INSTALL_DATA_DIR} ${DESTDIR}${PREFIX}/share/icons/hicolor/${i}x${i}/apps
	${INSTALL_DATA} ${WRKSRC}/browser/branding/${MOZILLA_BRANDING}/default${i}.png \
	  ${DESTDIR}${PREFIX}/share/icons/hicolor/${i}x${i}/apps/${MOZILLA}.png
.endfor

.include "../../graphics/hicolor-icon-theme/buildlink3.mk"
.include "../../sysutils/desktop-file-utils/desktopdb.mk"
.include "../../mk/bsd.pkg.mk"
