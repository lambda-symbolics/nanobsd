#!/bin/sh
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin:/usr/pkg/sbin
export PKG_CONFIG_PATH=/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig
cd /var/tmp/emacs-pgtk || exit 1
[ -f emacs-30.2.tar.xz ] || ftp -o emacs-30.2.tar.xz https://ftp.gnu.org/gnu/emacs/emacs-30.2.tar.xz || exit 1
want=313432d11e95c74f8cd35c5b1da442e6223f5d40f9173c55883c0339ecbfb97a0bedf79177ef8902afd3e33c078a233777bed01f5caffa1e7524f17d58bfc9a2
got=$(cksum -a SHA512 emacs-30.2.tar.xz | awk '{print $NF}')
[ "$got" = "$want" ] && echo "SHA512 ok" || { echo "SHA512 MISMATCH $got"; exit 1; }
rm -rf emacs-30.2 && xz -dc emacs-30.2.tar.xz | tar xf - && cd emacs-30.2 || exit 1
patch -p0 < /usr/pkgsrc/editors/emacs30/patches/patch-src_treesit.c && echo "pkgsrc patch ok"
CPPFLAGS="-I/usr/pkg/include -I/usr/pkg/gcc14/include" \
LDFLAGS="-L/usr/pkg/lib -Wl,-R/usr/pkg/lib -L/usr/pkg/gcc14/lib -Wl,-R/usr/pkg/gcc14/lib -L/usr/X11R7/lib -Wl,-R/usr/X11R7/lib" \
./configure --prefix=/usr/local/emacs-pgtk --localstatedir=/var --with-pgtk --with-native-compilation \
  --with-tree-sitter --with-gnutls --with-xml2 --with-modules --with-webp --with-file-notification=gfile \
  --without-pop --without-gconf > configure.out 2>&1
echo "CONFIGURE rc=$?"
grep -e 'Does Emacs use' -e 'What window system' -e 'native' configure.out | head -40
gmake -j8 > make.out 2>&1; echo "MAKE rc=$?"
gmake install > install.out 2>&1; echo "INSTALL rc=$?"
ls -l /usr/local/emacs-pgtk/bin/
echo EMACS-BUILD-FINISHED
