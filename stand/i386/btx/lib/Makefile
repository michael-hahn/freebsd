# $FreeBSD$

.include <bsd.init.mk>

PROG=	crt0.o
INTERNALPROG=
SRCS=	btxcsu.S btxsys.s btxv86.s
CFLAGS+=-I${BOOTSRC}/i386/common
LDFLAGS+=-Wl,-r

MK_INSTRUMENT_BINARIES=	no

.include <bsd.prog.mk>
