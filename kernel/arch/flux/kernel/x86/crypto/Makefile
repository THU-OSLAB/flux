# SPDX-License-Identifier: GPL-2.0
#
# x86 crypto algorithms

obj-$(CONFIG_CRYPTO_BLAKE2S_X86) += libblake2s-x86_64.o
libblake2s-x86_64-y := blake2s-core.o blake2s-glue.o

obj-$(CONFIG_CRYPTO_CRC32C_INTEL) += crc32c-intel.o
crc32c-intel-y := crc32c-intel_glue.o
crc32c-intel-$(CONFIG_64BIT) += crc32c-pcl-intel-asm_64.o

obj-$(CONFIG_CRYPTO_CRC32_PCLMUL) += crc32-pclmul.o
crc32-pclmul-y := crc32-pclmul_asm.o crc32-pclmul_glue.o

obj-$(CONFIG_CRYPTO_CRCT10DIF_PCLMUL) += crct10dif-pclmul.o
crct10dif-pclmul-y := crct10dif-pcl-asm_64.o crct10dif-pclmul_glue.o

obj-$(CONFIG_CRYPTO_CHACHA20_X86_64) += chacha-x86_64.o
chacha-x86_64-y := chacha-avx2-x86_64.o chacha-ssse3-x86_64.o chacha_glue.o
chacha-x86_64-$(CONFIG_AS_AVX512) += chacha-avx512vl-x86_64.o
