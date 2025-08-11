# SPDX-License-Identifier: GPL-2.0-only
zram-y	:=	zcomp.o zram_drv.o

zram-$(CONFIG_ZRAM_LZ4P)		+=	lz4p/lz4p.o
zram-$(CONFIG_ZRAM_LZ4P)		+=	lz4p/lz4p_compress.o
zram-$(CONFIG_ZRAM_LZ4P)		+=	lz4p/lz4p_decompress.o
zram-$(CONFIG_ZRAM_LZ4P)		+=	lz4p/lz4p_encode.o
zram-$(CONFIG_ZRAM_LZ4P)		+=	lz4p/lz4p_decode.o

obj-$(CONFIG_ZRAM)	+=	zram.o

