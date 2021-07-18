ACLOCAL_AMFLAGS = -I build-aux/m4

AM_CPPFLAGS = $(GMP_CPPFLAGS) $(SODIUM_CPPFLAGS)
AM_LDFLAGS = $(PTHREAD_CFLAGS) $(LIBTOOL_LDFLAGS)

LIBRELIC = librelic.la
LIBCHIABLS = libchiabls.la

noinst_PROGRAMS =
EXTRA_DIST =

RELIC_INCLUDES = -I$(top_srcdir)/contrib/relic/include -I$(top_srcdir)/contrib/relic/include/low -I$(top_srcdir)/contrib/relic/src/tmpl
CHIABLS_INCLUDES = -I$(builddir) -I$(builddir)/obj -I$(top_srcdir)/src $(RELIC_INCLUDES)

lib_LTLIBRARIES = $(LIBCHIABLS)
noinst_LTLIBRARIES = $(LIBRELIC)

RELIC_H = \
  contrib/relic/include/relic.h \
  contrib/relic/include/relic_alloc.h \
  contrib/relic/include/relic_arch.h \
  contrib/relic/include/relic_bc.h \
  contrib/relic/include/relic_bench.h \
  contrib/relic/include/relic_bn.h \
  contrib/relic/include/relic_conf.h \
  contrib/relic/include/relic_core.h \
  contrib/relic/include/relic_cp.h \
  contrib/relic/include/relic_dv.h \
  contrib/relic/include/relic_eb.h \
  contrib/relic/include/relic_ec.h \
  contrib/relic/include/relic_ed.h \
  contrib/relic/include/relic_ep.h \
  contrib/relic/include/relic_epx.h \
  contrib/relic/include/relic_err.h \
  contrib/relic/include/relic_fb.h \
  contrib/relic/include/relic_fbx.h \
  contrib/relic/include/relic_fp.h \
  contrib/relic/include/relic_fpx.h \
  contrib/relic/include/relic_label.h \
  contrib/relic/include/relic_md.h \
  contrib/relic/include/relic_mpc.h \
  contrib/relic/include/relic_multi.h \
  contrib/relic/include/relic_pc.h \
  contrib/relic/include/relic_pp.h \
  contrib/relic/include/relic_rand.h \
  contrib/relic/include/relic_test.h \
  contrib/relic/include/relic_types.h \
  contrib/relic/include/relic_util.h \
  contrib/relic/include/low/relic_bn_low.h \
  contrib/relic/include/low/relic_dv_low.h \
  contrib/relic/include/low/relic_fb_low.h \
  contrib/relic/include/low/relic_fp_low.h \
  contrib/relic/include/low/relic_fpx_low.h \
  contrib/relic/src/arch/lzcnt.inc \
  contrib/relic/src/tmpl/relic_tmpl_map.h

RELIC_SRCS = \
  contrib/relic/src/relic_err.c \
  contrib/relic/src/relic_core.c \
  contrib/relic/src/relic_conf.c \
  contrib/relic/src/relic_util.c

BN_SRCS = \
  contrib/relic/src/bn/relic_bn_add.c \
  contrib/relic/src/bn/relic_bn_cmp.c \
  contrib/relic/src/bn/relic_bn_div.c \
  contrib/relic/src/bn/relic_bn_factor.c \
  contrib/relic/src/bn/relic_bn_gcd.c \
  contrib/relic/src/bn/relic_bn_inv.c \
  contrib/relic/src/bn/relic_bn_lcm.c \
  contrib/relic/src/bn/relic_bn_mem.c \
  contrib/relic/src/bn/relic_bn_mod.c \
  contrib/relic/src/bn/relic_bn_mul.c \
  contrib/relic/src/bn/relic_bn_mxp.c \
  contrib/relic/src/bn/relic_bn_prime.c \
  contrib/relic/src/bn/relic_bn_rec.c \
  contrib/relic/src/bn/relic_bn_shift.c \
  contrib/relic/src/bn/relic_bn_smb.c \
  contrib/relic/src/bn/relic_bn_sqr.c \
  contrib/relic/src/bn/relic_bn_srt.c \
  contrib/relic/src/bn/relic_bn_util.c

FP_SRCS = \
  contrib/relic/src/fp/relic_fp_add.c \
  contrib/relic/src/fp/relic_fp_cmp.c \
  contrib/relic/src/fp/relic_fp_exp.c \
  contrib/relic/src/fp/relic_fp_inv.c \
  contrib/relic/src/fp/relic_fp_mul.c \
  contrib/relic/src/fp/relic_fp_param.c \
  contrib/relic/src/fp/relic_fp_prime.c \
  contrib/relic/src/fp/relic_fp_rdc.c \
  contrib/relic/src/fp/relic_fp_shift.c \
  contrib/relic/src/fp/relic_fp_sqr.c \
  contrib/relic/src/fp/relic_fp_srt.c \
  contrib/relic/src/fp/relic_fp_util.c

FPX_SRCS = \
  contrib/relic/src/fpx/relic_fp2_mul.c \
  contrib/relic/src/fpx/relic_fp2_sqr.c \
  contrib/relic/src/fpx/relic_fp3_mul.c \
  contrib/relic/src/fpx/relic_fp3_sqr.c \
  contrib/relic/src/fpx/relic_fp4_mul.c \
  contrib/relic/src/fpx/relic_fp4_sqr.c \
  contrib/relic/src/fpx/relic_fp6_mul.c \
  contrib/relic/src/fpx/relic_fp6_sqr.c \
  contrib/relic/src/fpx/relic_fp8_mul.c \
  contrib/relic/src/fpx/relic_fp8_sqr.c \
  contrib/relic/src/fpx/relic_fp9_mul.c \
  contrib/relic/src/fpx/relic_fp9_sqr.c \
  contrib/relic/src/fpx/relic_fp12_mul.c \
  contrib/relic/src/fpx/relic_fp12_sqr.c \
  contrib/relic/src/fpx/relic_fp18_mul.c \
  contrib/relic/src/fpx/relic_fp18_sqr.c \
  contrib/relic/src/fpx/relic_fp24_mul.c \
  contrib/relic/src/fpx/relic_fp24_sqr.c \
  contrib/relic/src/fpx/relic_fp48_mul.c \
  contrib/relic/src/fpx/relic_fp48_sqr.c \
  contrib/relic/src/fpx/relic_fp54_mul.c \
  contrib/relic/src/fpx/relic_fp54_sqr.c \
  contrib/relic/src/fpx/relic_fpx_add.c \
  contrib/relic/src/fpx/relic_fpx_cmp.c \
  contrib/relic/src/fpx/relic_fpx_cyc.c \
  contrib/relic/src/fpx/relic_fpx_exp.c \
  contrib/relic/src/fpx/relic_fpx_field.c \
  contrib/relic/src/fpx/relic_fpx_frb.c \
  contrib/relic/src/fpx/relic_fpx_inv.c \
  contrib/relic/src/fpx/relic_fpx_pck.c \
  contrib/relic/src/fpx/relic_fpx_rdc.c \
  contrib/relic/src/fpx/relic_fpx_srt.c \
  contrib/relic/src/fpx/relic_fpx_util.c

FB_SRCS = \
  contrib/relic/src/fb/relic_fb_add.c \
  contrib/relic/src/fb/relic_fb_cmp.c \
  contrib/relic/src/fb/relic_fb_exp.c \
  contrib/relic/src/fb/relic_fb_inv.c \
  contrib/relic/src/fb/relic_fb_itr.c \
  contrib/relic/src/fb/relic_fb_mul.c \
  contrib/relic/src/fb/relic_fb_param.c \
  contrib/relic/src/fb/relic_fb_poly.c \
  contrib/relic/src/fb/relic_fb_rdc.c \
  contrib/relic/src/fb/relic_fb_shift.c \
  contrib/relic/src/fb/relic_fb_slv.c \
  contrib/relic/src/fb/relic_fb_sqr.c \
  contrib/relic/src/fb/relic_fb_srt.c \
  contrib/relic/src/fb/relic_fb_trc.c \
  contrib/relic/src/fb/relic_fb_util.c

EP_SRCS = \
  contrib/relic/src/ep/relic_ep_add.c \
  contrib/relic/src/ep/relic_ep_cmp.c \
  contrib/relic/src/ep/relic_ep_curve.c \
  contrib/relic/src/ep/relic_ep_dbl.c \
  contrib/relic/src/ep/relic_ep_map.c \
  contrib/relic/src/ep/relic_ep_mul.c \
  contrib/relic/src/ep/relic_ep_mul_fix.c \
  contrib/relic/src/ep/relic_ep_mul_sim.c \
  contrib/relic/src/ep/relic_ep_neg.c \
  contrib/relic/src/ep/relic_ep_norm.c \
  contrib/relic/src/ep/relic_ep_param.c \
  contrib/relic/src/ep/relic_ep_pck.c \
  contrib/relic/src/ep/relic_ep_psi.c \
  contrib/relic/src/ep/relic_ep_util.c

EPX_SRCS = \
  contrib/relic/src/epx/relic_ep2_add.c \
  contrib/relic/src/epx/relic_ep2_cmp.c \
  contrib/relic/src/epx/relic_ep2_curve.c \
  contrib/relic/src/epx/relic_ep2_dbl.c \
  contrib/relic/src/epx/relic_ep2_frb.c \
  contrib/relic/src/epx/relic_ep2_map.c \
  contrib/relic/src/epx/relic_ep2_mul.c \
  contrib/relic/src/epx/relic_ep2_mul_cof.c \
  contrib/relic/src/epx/relic_ep2_mul_fix.c \
  contrib/relic/src/epx/relic_ep2_mul_sim.c \
  contrib/relic/src/epx/relic_ep2_neg.c \
  contrib/relic/src/epx/relic_ep2_norm.c \
  contrib/relic/src/epx/relic_ep2_pck.c \
  contrib/relic/src/epx/relic_ep2_util.c

EB_SRCS = \
  contrib/relic/src/eb/relic_eb_add.c \
  contrib/relic/src/eb/relic_eb_cmp.c \
  contrib/relic/src/eb/relic_eb_curve.c \
  contrib/relic/src/eb/relic_eb_dbl.c \
  contrib/relic/src/eb/relic_eb_frb.c \
  contrib/relic/src/eb/relic_eb_hlv.c \
  contrib/relic/src/eb/relic_eb_map.c \
  contrib/relic/src/eb/relic_eb_mul.c \
  contrib/relic/src/eb/relic_eb_mul_fix.c \
  contrib/relic/src/eb/relic_eb_mul_sim.c \
  contrib/relic/src/eb/relic_eb_neg.c \
  contrib/relic/src/eb/relic_eb_norm.c \
  contrib/relic/src/eb/relic_eb_param.c \
  contrib/relic/src/eb/relic_eb_pck.c \
  contrib/relic/src/eb/relic_eb_util.c

ED_SRCS = \
  contrib/relic/src/ed/relic_ed_add.c \
  contrib/relic/src/ed/relic_ed_cmp.c \
  contrib/relic/src/ed/relic_ed_curve.c \
  contrib/relic/src/ed/relic_ed_dbl.c \
  contrib/relic/src/ed/relic_ed_map.c \
  contrib/relic/src/ed/relic_ed_mul.c \
  contrib/relic/src/ed/relic_ed_mul_fix.c \
  contrib/relic/src/ed/relic_ed_mul_sim.c \
  contrib/relic/src/ed/relic_ed_neg.c \
  contrib/relic/src/ed/relic_ed_norm.c \
  contrib/relic/src/ed/relic_ed_param.c \
  contrib/relic/src/ed/relic_ed_pck.c \
  contrib/relic/src/ed/relic_ed_util.c

PP_SRCS = \
  contrib/relic/src/pp/relic_pp_add_k2.c \
  contrib/relic/src/pp/relic_pp_add_k8.c \
  contrib/relic/src/pp/relic_pp_add_k12.c \
  contrib/relic/src/pp/relic_pp_add_k48.c \
  contrib/relic/src/pp/relic_pp_add_k54.c \
  contrib/relic/src/pp/relic_pp_dbl_k2.c \
  contrib/relic/src/pp/relic_pp_dbl_k8.c \
  contrib/relic/src/pp/relic_pp_dbl_k12.c \
  contrib/relic/src/pp/relic_pp_dbl_k48.c \
  contrib/relic/src/pp/relic_pp_dbl_k54.c \
  contrib/relic/src/pp/relic_pp_exp_k2.c \
  contrib/relic/src/pp/relic_pp_exp_k8.c \
  contrib/relic/src/pp/relic_pp_exp_k12.c \
  contrib/relic/src/pp/relic_pp_exp_k48.c \
  contrib/relic/src/pp/relic_pp_exp_k54.c \
  contrib/relic/src/pp/relic_pp_map.c \
  contrib/relic/src/pp/relic_pp_map_k2.c \
  contrib/relic/src/pp/relic_pp_map_k8.c \
  contrib/relic/src/pp/relic_pp_map_k12.c \
  contrib/relic/src/pp/relic_pp_map_k48.c \
  contrib/relic/src/pp/relic_pp_map_k54.c \
  contrib/relic/src/pp/relic_pp_norm.c

PC_SRCS = \
  contrib/relic/src/pc/relic_pc_core.c \
  contrib/relic/src/pc/relic_pc_exp.c \
  contrib/relic/src/pc/relic_pc_util.c

MD_SRCS = \
  contrib/relic/src/md/blake2.h \
  contrib/relic/src/md/blake2-impl.h \
  contrib/relic/src/md/blake2s-ref.c \
  contrib/relic/src/md/relic_md_blake2s.c \
  contrib/relic/src/md/relic_md_hmac.c \
  contrib/relic/src/md/relic_md_kdf.c \
  contrib/relic/src/md/relic_md_mgf.c \
  contrib/relic/src/md/relic_md_sha224.c \
  contrib/relic/src/md/relic_md_sha256.c \
  contrib/relic/src/md/relic_md_sha384.c \
  contrib/relic/src/md/relic_md_sha512.c \
  contrib/relic/src/md/relic_md_xmd.c \
  contrib/relic/src/md/sha.h \
  contrib/relic/src/md/sha224-256.c \
  contrib/relic/src/md/sha384-512.c \
  contrib/relic/src/md/sha_private.h

EXTRA_DIST += \
  contrib/relic/src/arch/relic_arch_arm.c \
  contrib/relic/src/arch/relic_arch_none.c \
  contrib/relic/src/arch/relic_arch_x64.c \
  contrib/relic/src/arch/relic_arch_x86.c

RELIC_SRCS += contrib/relic/src/arch/relic_arch_@CPU_ARCH@.c
RELIC_SRCS += contrib/relic/src/rand/relic_rand_core.c
RELIC_SRCS += contrib/relic/src/rand/relic_rand_hashd.c

if WITH_BN
RELIC_SRCS += $(BN_SRCS)
RELIC_SRCS += contrib/relic/src/low/gmp/relic_bn_add_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_bn_div_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_bn_mod_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_bn_mul_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_bn_shift_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_bn_sqr_low.c
endif

if WITH_DV
RELIC_SRCS += contrib/relic/src/dv/relic_dv_mem.c
RELIC_SRCS += contrib/relic/src/dv/relic_dv_util.c
endif

if WITH_FP
RELIC_SRCS += $(FP_SRCS)
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fp_add_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fp_inv_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fp_mul_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fp_rdc_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fp_shift_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fp_sqr_low.c
endif

if WITH_FPX
RELIC_SRCS += $(FPX_SRCS)
RELIC_SRCS += contrib/relic/src/low/easy/relic_fpx_add_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fpx_mul_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fpx_rdc_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fpx_sqr_low.c
endif

if WITH_FB
RELIC_SRCS += $(FB_SRCS)
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fb_add_low.c
RELIC_SRCS += contrib/relic/src/low/gmp/relic_fb_shift_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_inv_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_itr_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_mul_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_rdc_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_slv_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_sqr_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_srt_low.c
RELIC_SRCS += contrib/relic/src/low/easy/relic_fb_trc_low.c
endif

if WITH_FBX
RELIC_SRCS += contrib/relic/src/fbx/relic_fb2_inv.c
RELIC_SRCS += contrib/relic/src/fbx/relic_fb2_mul.c
RELIC_SRCS += contrib/relic/src/fbx/relic_fb2_slv.c
RELIC_SRCS += contrib/relic/src/fbx/relic_fb2_sqr.c
endif

if WITH_EP
RELIC_SRCS += $(EP_SRCS)
endif

if WITH_EPX
RELIC_SRCS += $(EPX_SRCS)
endif

if WITH_EB
RELIC_SRCS += $(EB_SRCS)
endif

if WITH_ED
RELIC_SRCS += $(ED_SRCS)
endif

if WITH_PP
RELIC_SRCS += $(PP_SRCS)
endif

if WITH_PC
RELIC_SRCS += $(PC_SRCS)
endif

if WITH_CP
RELIC_SRCS += contrib/relic/src/cp/relic_cp_rsa.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_rabin.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_bdpe.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ghpe.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_phpe.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ecdh.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ecmqv.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ecies.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ecdsa.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ecss.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_vbnn.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_sokaka.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_bgn.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_ibe.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_bls.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_cls.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_pss.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_mpss.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_bbs.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_zss.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_cmlhs.c
RELIC_SRCS += contrib/relic/src/cp/relic_cp_mklhs.c
endif

if WITH_BC
RELIC_SRCS += contrib/relic/src/bc/relic_bc_aes.c
RELIC_SRCS += contrib/relic/src/bc/rijndael-alg-fst.c
RELIC_SRCS += contrib/relic/src/bc/rijndael-alg-fst.h
RELIC_SRCS += contrib/relic/src/bc/rijndael-api-fst.c
RELIC_SRCS += contrib/relic/src/bc/rijndael-api-fst.h
endif

if WITH_MD
RELIC_SRCS += $(MD_SRCS)
endif

if WITH_MPC
RELIC_SRCS += contrib/relic/src/mpc/relic_mt_mpc.c
RELIC_SRCS += contrib/relic/src/mpc/relic_pc_mpc.c
endif

librelic_la_SOURCES = \
  $(RELIC_SRCS) \
  $(RELIC_H)

librelic_la_CPPFLAGS = $(AM_CPPFLAGS) $(RELIC_CPPFLAGS) $(RELIC_INCLUDES)
librelic_la_CFLAGS = $(AM_CFLAGS) $(WARN_CFLAGS) $(NOWARN_CFLAGS)


# libchiabls.la
BLS_H = \
  src/bls.hpp \
  src/elements.hpp \
  src/hdkeys.hpp \
  src/hkdf.hpp \
  src/privatekey.hpp \
  src/schemes.hpp \
  src/test-utils.hpp \
  src/threshold.hpp \
  src/util.hpp

libchiabls_la_SOURCES = \
  src/privatekey.cpp \
  src/bls.cpp \
  src/elements.cpp \
  src/schemes.cpp \
  src/threshold.cpp \
  $(BLS_H) \
  $(RELIC_H)

libchiabls_la_CPPFLAGS = $(AM_CPPFLAGS) $(CHIABLS_INCLUDES) -DBLSALLOC_SODIUM=1
libchiabls_la_CXXFLAGS = $(AM_CXXFLAGS)

libchiabls_la_LIBADD = $(LIBRELIC) $(SODIUM_LIBS) $(GMP_LIBS)


if USE_BENCH
noinst_PROGRAMS += runbench

runbench_SOURCES = \
  src/test-bench.cpp \
  src/test-utils.hpp \
  $(RELIC_H) \
  $(BLS_H)

runbench_CPPFLAGS = $(AM_CPPFLAGS) $(CHIABLS_INCLUDES)
runbench_CXXFLAGS = $(AM_CXXFLAGS)
runbench_LDFLAGS = $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

runbench_LDADD = $(LIBCHIABLS) $(SODIUM_LIBS) $(GMP_LIBS)

endif

TESTS =
if USE_TESTS
noinst_PROGRAMS += runtest

runtest_SOURCES = src/test.cpp \
  src/test-utils.hpp \
  contrib/catch/catch.hpp

runtest_CPPFLAGS = $(AM_CPPFLAGS) $(CHIABLS_INCLUDES) -I$(top_srcdir)/contrib/catch
runtest_CXXFLAGS = $(AM_CXXFLAGS)
runtest_LDFLAGS = $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

runtest_LDADD = $(LIBCHIABLS) $(SODIUM_LIBS) $(GMP_LIBS)

TESTS += runtest

endif

CLEANFILES = $(LIBRELIC) $(LIBCHIABLS)
