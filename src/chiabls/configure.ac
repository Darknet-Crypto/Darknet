AC_PREREQ([2.60])
AC_INIT([libchiabls],[0.1])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([build-aux/m4])

dnl Unless the user specified ARFLAGS, force it to be cr
AC_ARG_VAR(ARFLAGS, [Flags for the archiver, defaults to <cr> if not set])
if test "x${ARFLAGS+set}" != "xset"; then
  ARFLAGS="qc"
fi

AC_CANONICAL_HOST

AH_TOP([#ifndef RLC_CONF_H])
AH_TOP([#define RLC_CONF_H])
AH_BOTTOM([#endif /*RLC_CONF_H*/])
AM_INIT_AUTOMAKE([foreign subdir-objects])

dnl faketime messes with timestamps and causes configure to be re-run.
dnl --disable-maintainer-mode can be used to bypass this.
AM_MAINTAINER_MODE([enable])

dnl make the compilation flags quiet unless V=1 is used
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl Compiler checks (here before libtool).
if test "x${CXXFLAGS+set}" = "xset"; then
  CXXFLAGS_overridden=yes
else
  CXXFLAGS_overridden=no
fi
AC_PROG_CXX

AM_PROG_CC_C_O

AC_PROG_CC_C99
if test x"$ac_cv_prog_cc_c99" = x"no"; then
  AC_MSG_ERROR([c99 compiler support required])
fi

dnl By default, libtool for mingw refuses to link static libs into a dll for
dnl fear of mixing pic/non-pic objects, and import/export complications. Since
dnl we have those under control, re-enable that functionality.
case $host in
  *mingw*)
     lt_cv_deplibs_check_method="pass_all"
  ;;
esac
dnl Require C++14 compiler (no GNU extensions)
AX_CXX_COMPILE_STDCXX([14], [noext], [mandatory], [nodefault])

dnl Libtool init checks.
LT_INIT([pic-only])

AC_PATH_TOOL(AR, ar)
AC_PATH_TOOL(RANLIB, ranlib)
AC_PATH_TOOL(STRIP, strip)

AM_PROG_AS

AC_ARG_ENABLE(tests,
    AS_HELP_STRING([--disable-tests],[do not compile tests (default is to compile)]),
    [use_tests=$enableval],
    [use_tests=yes])

AC_ARG_ENABLE(bench,
    AS_HELP_STRING([--disable-bench],[do not compile benchmarks (default is to compile)]),
    [use_bench=$enableval],
    [use_bench=yes])


dnl Define enum mappings for relic config
AC_DEFINE([RLC_VERSION], ["0.5.0"], [Relic Version])
AC_DEFINE([AVR],     [1], [Atmel AVR ATMega128 8-bit architecture.])
AC_DEFINE([MSP],     [2], [MSP430 16-bit architecture.])
AC_DEFINE([ARM],     [3], [ARM 32-bit architecture.])
AC_DEFINE([X86],     [4], [Intel x86-compatible 32-bit architecture.])
AC_DEFINE([X64],     [5], [AMD64-compatible 64-bit architecture.])
AC_DEFINE([EASY],    [1], [Easy C-only backend.])
AC_DEFINE([GMP],     [2], [GMP backend.])
AC_DEFINE([GMP_SEC], [3], [GMP constant-time backend.])
AC_DEFINE([SINGLE],  [0], [A multiple precision integer can store w words.])
AC_DEFINE([CARRY],   [1], [A multiple precision integer can store the result of an addition.])
AC_DEFINE([DOUBLE],  [2], [A multiple precision integer can store the result of a multiplication.])
AC_DEFINE([BASIC],   [1], [Basic method.])
AC_DEFINE([PRIME],   [1], [Prime curves.])
AC_DEFINE([TATEP],   [1], [Tate pairing.])
AC_DEFINE([AUTO],    [1], [Automatic memory allocation.])
AC_DEFINE([HASHD],   [1], [NIST HASH-DRBG generator.])
AC_DEFINE([LIBC],    [1], [Standard C library generator.])
AC_DEFINE([OPENMP],  [1], [OpenMP multithreading support.])
AC_DEFINE([LINUX],   [1], [GNU/Linux operating system.])
AC_DEFINE([HREAL],   [1], [Per-process high-resolution timer.])
AC_DEFINE([COMBA],   [2], [Comba method.])
AC_DEFINE([LEHME],   [2], [Lehmer's fast GCD Algorithm.])
AC_DEFINE([SAFEP],   [2], [Safe prime generation.])
AC_DEFINE([QUICK],   [2], [Fast reduction modulo special form prime.])
AC_DEFINE([BINAR],   [2], [Binary inversion.])
AC_DEFINE([LAZYR],   [2], [Lazy-reduced extension field arithmetic.])
AC_DEFINE([LODAH],   [2], [Lopez-Dahab multiplication.])
AC_DEFINE([SLIDE],   [2], [Sliding window modular exponentiation.])
AC_DEFINE([PROJC],   [2], [Projective coordinates.])
AC_DEFINE([COMBS],   [2], [Single-table comb method.])
AC_DEFINE([TRICK],   [2], [Shamir's trick.])
AC_DEFINE([CHAR2],   [2], [Binary curves.])
AC_DEFINE([WEILP],   [2], [Weil pairing.])
AC_DEFINE([SH224],   [2], [SHA-224 hash function.])
AC_DEFINE([PKCS1],   [2], [RSA PKCS#1 v1.5 padding.])
AC_DEFINE([DYNAMIC], [2], [Dynamic memory allocation.])
AC_DEFINE([RDRND],   [2], [Intel RdRand instruction.])
AC_DEFINE([FREEBSD], [2], [FreeBSD operating system.])
AC_DEFINE([PTHREAD], [2], [POSIX multithreading support.])
AC_DEFINE([HPROC],   [2], [Per-process high-resolution timer.])
AC_DEFINE([MONTY],   [3], [Montgomery method.])
AC_DEFINE([STEIN],   [3], [Stein's binary GCD Algorithm.])
AC_DEFINE([STRON],   [3], [Strong prime generation.])
AC_DEFINE([INTEG],   [3], [Integrated modular addtion.])
AC_DEFINE([INTER],   [3], [Interleaving of w-(T)NAFs.])
AC_DEFINE([ALMOS],   [3], [Almost inverse algorithm.])
AC_DEFINE([JACOB],   [3], [Jacobian coordinates.])
AC_DEFINE([COMBD],   [3], [Double-table comb method.])
AC_DEFINE([HALVE],   [3], [Halving.])
AC_DEFINE([EDDIE],   [3], [Edwards curves.])
AC_DEFINE([EXTND],   [3], [Extended projective twisted Edwards coordinates.])
AC_DEFINE([OATEP],   [3], [Optimal ate pairing.])
AC_DEFINE([SH256],   [3], [SHA-256 hash function.])
AC_DEFINE([PKCS2],   [3], [RSA PKCS#1 v2.1 padding.])
AC_DEFINE([UDEV],    [3], [Operating system underlying generator.])
AC_DEFINE([MACOSX],  [3], [MacOS operating system.])
AC_DEFINE([HTHRD],   [3], [Per-thread high-resolution timer.])
AC_DEFINE([PMERS],   [4], [Pseudo-Mersenne method.])
AC_DEFINE([MULTP],   [4], [Reuse multiplication for squaring.])
AC_DEFINE([EXGCD],   [4], [Extended Euclidean algorithm.])
AC_DEFINE([LWNAF],   [4], [Left-to-right Width-w NAF.])
AC_DEFINE([JOINT],   [4], [Joint sparse form.])
AC_DEFINE([SH384],   [4], [SHA-384 hash function.])
AC_DEFINE([CALL],    [4], [Override library generator with the callback.])
AC_DEFINE([WCGR],    [4], [Use Windows' CryptGenRandom.])
AC_DEFINE([WINDOWS], [4], [Windows operating system.])
AC_DEFINE([POSIX],   [4], [POSIX-compatible timer.])
AC_DEFINE([DIVST],   [5], [Constant-time inversion by Bernstein-Yang division steps.])
AC_DEFINE([ITOHT],   [5], [Itoh-Tsuji inversion.])
AC_DEFINE([LWREG],   [5], [Left-to-right Width-w NAF.])
AC_DEFINE([SH512],   [5], [SHA-512 hash function.])
AC_DEFINE([DROID],   [5], [Android operating system.])
AC_DEFINE([ANSI],    [5], [ANSI-compatible timer.])
AC_DEFINE([BRUCH],   [6], [Hardware-friendly inversion by Brunner-Curiger-Hofstetter.])
AC_DEFINE([B2S160],  [6], [BLAKE2s-160 hash function.])
AC_DEFINE([DUINO],   [6], [Arduino platform.])
AC_DEFINE([CYCLE],   [6], [Cycle-counting timer.])
AC_DEFINE([CTAIA],   [7], [Constant-time version of almost inverse.])
AC_DEFINE([B2S256],  [7], [BLAKE2s-256 hash function.])
AC_DEFINE([OPENBSD], [7], [OpenBSD operating system.])
AC_DEFINE([PERF],    [7], [Performance monitoring framework.])
AC_DEFINE([LOWER],   [8], [Use implementation provided by the lower layer.])

use_pkgconfig=yes
case $host in
  *darwin*)
     CPU_ARCH="x64"
     AC_DEFINE([ARCH], [X64], [Architecture.])
     AC_DEFINE([WSIZE], [64], [Size of word in this architecture.])
     AC_DEFINE([SEED], [UDEV], [Chosen random generator seeder.])
     AC_DEFINE([FP_QNRES], [], [Use -1 as quadratic non-residue.])
     AC_DEFINE([OPSYS], [MACOSX], [Detected operation system.])
     if test x$cross_compiling != xyes; then
       AC_PATH_PROG([BREW],brew,)
       if test x$BREW != x; then
         dnl These Homebrew packages may be keg-only, meaning that they won't be found
         dnl in expected paths because they may conflict with system files. Ask
         dnl Homebrew where each one is located, then adjust paths accordingly.

         gmp_prefix=`$BREW --prefix gmp 2>/dev/null`
         if test x$gmp_prefix != x; then
           GMP_CPPFLAGS="-I$gmp_prefix/include"
           GMP_LIBS="-L$gmp_prefix/lib"
         fi
       fi
     fi
     ;;
  *linux*)
     AC_DEFINE([SEED], [UDEV], [Chosen random generator seeder.])
     AC_DEFINE([FP_QNRES], [], [Use -1 as quadratic non-residue.])
     AC_DEFINE([OPSYS], [LINUX], [Detected operation system.])
     RELIC_CPPFLAGS="-D_GNU_SOURCE"
     case $host_cpu in
       x86_64)
          CPU_ARCH="x64"
          AC_DEFINE([ARCH], [X64], [Architecture.])
          AC_DEFINE([WSIZE], [64], [Size of word in this architecture.])
          ;;
       i?86)
          CPU_ARCH="x86"
          AC_DEFINE([ARCH], [X86], [Architecture.])
          AC_DEFINE([WSIZE], [32], [Size of word in this architecture.])
          ;;
       arm*)
          CPU_ARCH="arm"
          AC_DEFINE([ARCH], [ARM], [Architecture.])
          AC_DEFINE([WSIZE], [32], [Size of word in this architecture.])
          ;;
       aarch*)
          dnl Relic doesn't support aarch64 yet, set CPU_ARCH to none.
          CPU_ARCH="none"
          AC_DEFINE([ARCH], [ARM], [Architecture.])
          AC_DEFINE([WSIZE], [64], [Size of word in this architecture.])
          ;;
       *)
          CPU_ARCH="none"
          AC_DEFINE([WSIZE], [64], [Size of word in this architecture.])
          ;;
     esac
     ;;
  *mingw*)
     use_pkgconfig=no
     CPU_ARCH="x64"
     AC_DEFINE([ARCH], [X64], [Architecture.])
     AC_DEFINE([WSIZE], [64], [Size of word in this architecture.])
     AC_DEFINE([SEED], [WCGR], [Chosen random generator seeder.])
     AC_DEFINE([OPSYS], [WINDOWS], [Detected operation system.])
     LIBTOOL_APP_LDFLAGS="$LIBTOOL_APP_LDFLAGS -all-static"

     dnl libtool insists upon adding -nostdlib and a list of objects/libs to link against.
     dnl That breaks our ability to build dll's with static libgcc/libstdc++/libssp. Override
     dnl its command here, with the predeps/postdeps removed, and -static inserted. Postdeps are
     dnl also overridden to prevent their insertion later.
     dnl This should only affect dll's.
     archive_cmds_CXX="\$CC -shared \$libobjs \$deplibs \$compiler_flags -static -o \$output_objdir/\$soname \${wl}--enable-auto-image-base -Xlinker --out-implib -Xlinker \$lib"
     postdeps_CXX=
     ;;
esac

if test x$use_pkgconfig = xyes; then
  m4_ifndef([PKG_PROG_PKG_CONFIG], [AC_MSG_ERROR(PKG_PROG_PKG_CONFIG macro not found. Please install pkg-config and re-run autogen.sh.)])
  m4_ifdef([PKG_PROG_PKG_CONFIG], [
  PKG_PROG_PKG_CONFIG
  if test x"$PKG_CONFIG" = "x"; then
    AC_MSG_ERROR(pkg-config not found.)
  fi
  ])
fi

saved_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -pipe"
AC_MSG_CHECKING([if ${CC} supports -pipe])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[char foo;]])],
    [ AC_MSG_RESULT([yes]) ],
    [ AC_MSG_RESULT([no])
      CFLAGS="$saved_CFLAGS"
    ])

saved_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -fomit-frame-pointer"
AC_MSG_CHECKING([if ${CC} supports -fomit-frame-pointer])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[char foo;]])],
    [ AC_MSG_RESULT([yes]) ],
    [ AC_MSG_RESULT([no])
      CFLAGS="$saved_CFLAGS"
    ])

AC_LANG_PUSH([C])
AX_CHECK_COMPILE_FLAG([-Wall],[WARN_CFLAGS="$WARN_CFLAGS -Wall"],,)
AX_CHECK_COMPILE_FLAG([-Wextra],[WARN_CFLAGS="$WARN_CFLAGS -Wextra"],,)
AX_CHECK_COMPILE_FLAG([-Wcast-align],[WARN_CFLAGS="$WARN_CFLAGS -Wcast-align"],,)
AX_CHECK_COMPILE_FLAG([-Wunused-parameter],[NOWARN_CFLAGS="$NOWARN_CFLAGS -Wno-unused-parameter"],,)
AX_CHECK_COMPILE_FLAG([-Wshadow],[NOWARN_CFLAGS="$NOWARN_CFLAGS -Wno-shadow"],,)
AX_CHECK_COMPILE_FLAG([-Wsign-compare],[NOWARN_CFLAGS="$NOWARN_CFLAGS -Wno-sign-compare"],,)
AX_CHECK_COMPILE_FLAG([-Wstrict-prototypes],[NOWARN_CFLAGS="$NOWARN_CFLAGS -Wno-strict-prototypes"],,)
AX_CHECK_COMPILE_FLAG([-Wunused-function],[NOWARN_CFLAGS="$NOWARN_CFLAGS -Wno-unused-function"],,)
AX_CHECK_COMPILE_FLAG([-Wlong-long],[NOWARN_CFLAGS="$NOWARN_CFLAGS -Wno-long-long"],,)
AC_LANG_POP([C])

dnl set default settings for relic
AC_DEFINE([ALIGN], [1], [Byte boundary to align digit vectors.])

AC_DEFINE(WITH_BN,, Build multiple precision integer module.)
AC_DEFINE(WITH_DV,, Temporary double-precision digit vectors.)
AC_DEFINE(WITH_FP,, Build prime field module.)
AC_DEFINE(WITH_FPX,, Build prime field extension module.)
AC_DEFINE(WITH_FB,, Build binary field module.)
AC_DEFINE(WITH_FBX,, Build binary field extension module.)
AC_DEFINE(WITH_EP,, Build prime elliptic curve module.)
AC_DEFINE(WITH_EPX,, Build prime field extension elliptic curve module.)
AC_DEFINE(WITH_EB,, Build binary elliptic curve module.)
AC_DEFINE(WITH_ED,, Build elliptic Edwards curve module.)
AC_DEFINE(WITH_EC,, Build elliptic curve cryptography module.)
AC_DEFINE(WITH_PP,, Build pairings over prime curves module.)
AC_DEFINE(WITH_PC,, Build pairing-based cryptography module.)
AC_DEFINE(WITH_BC,, Build block ciphers.)
AC_DEFINE(WITH_MD,, Build hash functions.)
AC_DEFINE(WITH_CP,, Build cryptographic protocols.)
AC_DEFINE(WITH_MPC,, Build Multi-party computation primitives.)

AC_DEFINE(BN_PRECI, 1024, Required precision in bits.)
AC_DEFINE(BN_KARAT, 0, Number of Karatsuba steps.)
AC_DEFINE(BN_MAGNI, [DOUBLE], Effective size of a multiple precision integer.)

AC_DEFINE(BN_METHD, "COMBA;COMBA;MONTY;SLIDE;BASIC;BASIC", Multiple precision arithmetic method.)
AC_DEFINE(BN_MUL, COMBA, Chosen multiple precision multiplication method.)
AC_DEFINE(BN_SQR, COMBA, Chosen multiple precision multiplication method.)
AC_DEFINE(BN_MOD, MONTY, Chosen multiple precision modular reduction method.)
AC_DEFINE(BN_MXP, SLIDE, Chosen multiple precision modular exponentiation method.)
AC_DEFINE(BN_GCD, BASIC, Chosen multiple precision greatest common divisor method.)
AC_DEFINE(BN_GEN, BASIC, Chosen prime generation algorithm.)

AC_DEFINE(FP_PRIME, 381, Prime field size in bits.)
AC_DEFINE(FP_METHD, "INTEG;INTEG;INTEG;MONTY;LOWER;SLIDE", Prime field arithmetic method.)
AC_DEFINE(FP_KARAT, 0, Number of Karatsuba steps.)
AC_DEFINE(FP_WIDTH, 4, Width of window processing for exponentiation methods.)
AC_DEFINE(FP_ADD, INTEG, Chosen prime field multiplication method.)
AC_DEFINE(FP_MUL, INTEG, Chosen prime field multiplication method.)
AC_DEFINE(FP_SQR, INTEG, Chosen prime field multiplication method.)
AC_DEFINE(FP_RDC, MONTY, Chosen prime field reduction method.)
AC_DEFINE(FP_INV, LOWER, Chosen prime field inversion method.)
AC_DEFINE(FP_EXP, SLIDE, Chosen multiple precision modular exponentiation method.)

AC_DEFINE(FPX_METHD, "INTEG;INTEG;LAZYR", Prime extension field arithmetic method.)
AC_DEFINE(FPX_QDR, INTEG, Chosen extension field arithmetic method.)
AC_DEFINE(FPX_CBC, INTEG, Chosen extension field arithmetic method.)
AC_DEFINE(FPX_RDC, LAZYR, Chosen extension field arithmetic method.)

AC_DEFINE(FB_METHD, "LODAH;QUICK;QUICK;QUICK;QUICK;QUICK;EXGCD;SLIDE;QUICK", Binary field arithmetic method)
AC_DEFINE(FB_POLYN, 283, Irreducible polynomial size in bits.)
AC_DEFINE(FB_KARAT, 0, Number of Karatsuba levels.)
AC_DEFINE(FB_TRINO,, Prefer trinomials over pentanomials.)
AC_DEFINE(FB_PRECO,, Precompute multiplication table for sqrt(z).)
AC_DEFINE(FB_WIDTH, 4, Width of window processing for exponentiation methods.)
AC_DEFINE(FB_MUL, LODAH, Chosen binary field multiplication method.)
AC_DEFINE(FB_SQR, QUICK, Chosen binary field squaring method.)
AC_DEFINE(FB_RDC, QUICK, Chosen binary field modular reduction method.)
AC_DEFINE(FB_SRT, QUICK, Chosen binary field modular reduction method.)
AC_DEFINE(FB_TRC, QUICK, Chosen trace computation method.)
AC_DEFINE(FB_SLV, QUICK, Chosen method to solve a quadratic equation.)
AC_DEFINE(FB_INV, EXGCD, Chosen binary field inversion method.)
AC_DEFINE(FB_EXP, SLIDE, Chosen multiple precision modular exponentiation method.)
AC_DEFINE(FB_ITR, QUICK, Chosen method to solve a quadratic equation.)

AC_DEFINE(EP_METHD, "PROJC;LWNAF;COMBS;INTER", Prime elliptic curve arithmetic method.)
AC_DEFINE(EP_ENDOM,, Support for prime curves with efficient endormorphisms.)
AC_DEFINE(EP_MIXED,, Use mixed coordinates.)
AC_DEFINE(EP_PRECO,, Build precomputation table for generator.)
AC_DEFINE(EP_CTMAP,, Enable isogeny map for SSWU map-to-curve.)
AC_DEFINE(EP_DEPTH, 4, Width of precomputation table for fixed point methods.)
AC_DEFINE(EP_WIDTH, 4, Width of window processing for unknown point methods.)
AC_DEFINE(EP_ADD, PROJC, Chosen prime elliptic curve coordinate method.)
AC_DEFINE(EP_MUL, LWNAF, Chosen prime elliptic curve point multiplication method.)
AC_DEFINE(EP_FIX, COMBS, Chosen prime elliptic curve point multiplication method.)
AC_DEFINE(EP_SIM, INTER, Chosen prime elliptic curve simulteanous point multiplication method.)

AC_DEFINE(EB_METHD, "PROJC;LWNAF;COMBS;INTER", Binary elliptic curve arithmetic method.)
AC_DEFINE(EB_PLAIN,, Support for ordinary curves without endormorphisms.)
AC_DEFINE(EB_KBLTZ,, Support for Koblitz anomalous binary curves.)
AC_DEFINE(EB_MIXED,, Use mixed coordinates.)
AC_DEFINE(EB_PRECO,, Build precomputation table for generator.)
AC_DEFINE(EB_DEPTH, 4, Width of precomputation table for fixed point methods.)
AC_DEFINE(EB_WIDTH, 4, Width of window processing for unknown point methods.)
AC_DEFINE(EB_ADD, PROJC, Chosen binary elliptic curve coordinate method.)
AC_DEFINE(EB_MUL, LWNAF, Chosen binary elliptic curve point multiplication method.)
AC_DEFINE(EB_FIX, COMBS, Chosen binary elliptic curve point multiplication method.)
AC_DEFINE(EB_SIM, INTER, Chosen binary elliptic curve simulteanous point multiplication method.)

AC_DEFINE(ED_METHD, "PROJC;LWNAF;COMBS;INTER", Edwards elliptic curve arithmetic method.)
AC_DEFINE(ED_PRECO,, Build precomputation table for generator.)
AC_DEFINE(ED_DEPTH, 4, Width of precomputation table for fixed point methods.)
AC_DEFINE(ED_WIDTH, 4, Width of window processing for unknown point methods.)
AC_DEFINE(ED_ADD, PROJC, Chosen binary elliptic curve coordinate method.)
AC_DEFINE(ED_MUL, LWNAF, Chosen prime elliptic twisted Edwards curve point multiplication method.)
AC_DEFINE(ED_FIX, COMBS, Chosen prime elliptic twisted Edwards curve point multiplication method.)
AC_DEFINE(ED_SIM, INTER, Chosen prime elliptic curve simulteanous point multiplication method.)

AC_DEFINE(EC_METHD, "PRIME", Chosen elliptic curve cryptography method.)
AC_DEFINE(EC_CUR, PRIME, Chosen elliptic curve type.)

AC_DEFINE(PP_METHD, "LAZYR;OATEP", Bilinear pairing method.)
AC_DEFINE(PP_EXT, LAZYR, Chosen extension field arithmetic method.)
AC_DEFINE(PP_MAP, OATEP, Chosen pairing method over prime elliptic curves.)

AC_DEFINE(MD_METHD, "SH256", Choice of hash function.)
AC_DEFINE(MD_MAP, SH256, Chosen hash function.)

AC_DEFINE(CP_CRT,, Support for faster CRT-based exponentiation in factoring-based cryptosystems.)
AC_DEFINE(CP_RSAPD, PKCS2, Chosen RSA padding method.)

AC_DEFINE(ALLOC, AUTO, Chosen memory allocation policy.)
AC_DEFINE(RAND, HASHD, Chosen random generator.)
AC_DEFINE(MULTI, PTHREAD, Chosen multithreading API.)
AC_DEFINE(TIMER, CYCLE, Chosen timer.)


AC_CHECK_HEADER(gmp.h,[AC_CHECK_LIB(gmp, __gmpz_init,[has_gmp=yes; GMP_LIBS=-lgmp;])], AC_MSG_ERROR(gmp headers missing),)

if test x$has_gmp = xyes; then
  AC_DEFINE(ARITH, GMP, Arithmetic backend.)
fi

if test x$use_pkgconfig = xyes; then
  : dnl
  m4_ifdef(
    [PKG_CHECK_MODULES],
    [
      PKG_CHECK_MODULES([SODIUM], [libsodium],, [AC_MSG_ERROR(libsodium not found.)])
    ]
  )
else
  AC_CHECK_HEADER([sodium/core.h],, AC_MSG_ERROR(libsodium headers missing),)
  AC_CHECK_LIB([sodium],[main],SODIUM_LIBS=-lsodium, AC_MSG_ERROR(libsodium missing))
fi

dnl Check for pthread compile/link requirements
AX_PTHREAD

AC_SEARCH_LIBS([clock_gettime],[rt])

AC_MSG_CHECKING([whether to build runtest])
if test x$use_tests = xyes; then
  AC_MSG_RESULT([yes])
  BUILD_TEST="yes"
else
  AC_MSG_RESULT([no])
  BUILD_TEST=""
fi

AC_MSG_CHECKING([whether to build runbench])
if test x$use_bench = xyes; then
  AC_MSG_RESULT([yes])
  BUILD_BENCH="yes"
else
  AC_MSG_RESULT([no])
  BUILD_BENCH=""
fi

AM_CONDITIONAL(WITH_BN, test 1 -eq 1)
AM_CONDITIONAL(WITH_FP, test 1 -eq 1)
AM_CONDITIONAL(WITH_FPX, test 1 -eq 1)
AM_CONDITIONAL(WITH_FB, test 1 -eq 1)
AM_CONDITIONAL(WITH_EP, test 1 -eq 1)
AM_CONDITIONAL(WITH_EPX, test 1 -eq 1)
AM_CONDITIONAL(WITH_EB, test 1 -eq 1)
AM_CONDITIONAL(WITH_ED, test 1 -eq 1)
AM_CONDITIONAL(WITH_EC, test 1 -eq 1)
AM_CONDITIONAL(WITH_PP, test 1 -eq 1)
AM_CONDITIONAL(WITH_PC, test 1 -eq 1)
AM_CONDITIONAL(WITH_BC, test 1 -eq 1)
AM_CONDITIONAL(WITH_MD, test 1 -eq 1)
AM_CONDITIONAL(WITH_CP, test 1 -eq 1)
AM_CONDITIONAL(WITH_MPC, test 1 -eq 1)
AM_CONDITIONAL(WITH_DV, test 1 -eq 1)
AM_CONDITIONAL(WITH_FBX, test 1 -eq 1)
AM_CONDITIONAL([USE_TESTS], [test x$BUILD_TEST = xyes])
AM_CONDITIONAL([USE_BENCH], [test x$BUILD_BENCH = xyes])
AC_SUBST(CPU_ARCH)
AC_SUBST(RAND_PATH, hashd)
AC_SUBST(SODIUM_CPPFLAGS)
AC_SUBST(SODIUM_LIBS)
AC_SUBST(GMP_CPPFLAGS)
AC_SUBST(GMP_LIBS)
AC_SUBST(RELIC_CPPFLAGS)
AC_SUBST(WARN_CFLAGS)
AC_SUBST(NOWARN_CFLAGS)
AC_SUBST(LIBTOOL_APP_LDFLAGS)


AC_CONFIG_HEADERS([contrib/relic/include/relic_conf.h])
AC_CONFIG_FILES([Makefile])

AC_OUTPUT