/*
 * config.h for building vkr without meson (vkr/build-vkr.sh), for the Linux
 * test host and for iOS. Only what vkr and the few util files it uses need.
 */
#define VERSION "1.3.0"
#define VIRGL_RENDERER_UNSTABLE_APIS 1
#define ENABLE_VENUS 1
/* vkr_library_load: the embedder always passes vkGetInstanceProcAddr, so
 * nothing gets dlopen()ed; DLOAD only keeps vkr from linking libvulkan
 */
#define ENABLE_VULKAN_DLOAD 1
#define HAVE_DLFCN_H 1
#define HAVE_PTHREAD 1
#define HAVE_TIMESPEC_GET 1
#define HAVE___BUILTIN_BSWAP32 1
#define HAVE___BUILTIN_BSWAP64 1
#define HAVE___BUILTIN_CLZ 1
#define HAVE___BUILTIN_CLZLL 1
#define HAVE___BUILTIN_EXPECT 1
#define HAVE___BUILTIN_FFS 1
#define HAVE___BUILTIN_FFSLL 1
#define HAVE___BUILTIN_POPCOUNT 1
#define HAVE___BUILTIN_POPCOUNTLL 1
#define HAVE___BUILTIN_TYPES_COMPATIBLE_P 1
#define HAVE___BUILTIN_UNREACHABLE 1
#define HAVE_FUNC_ATTRIBUTE_CONST 1
#define HAVE_FUNC_ATTRIBUTE_FLATTEN 1
#define HAVE_FUNC_ATTRIBUTE_FORMAT 1
#define HAVE_FUNC_ATTRIBUTE_MALLOC 1
#define HAVE_FUNC_ATTRIBUTE_NORETURN 1
#define HAVE_FUNC_ATTRIBUTE_PACKED 1
#define HAVE_FUNC_ATTRIBUTE_PURE 1
#define HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL 1
#define HAVE_FUNC_ATTRIBUTE_UNUSED 1
#define HAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT 1
#define HAVE_FUNC_ATTRIBUTE_WEAK 1
#define HAVE_STRTOK_R 1
#define HAVE_SYS_UIO_H 1
#ifdef __linux__
#define _GNU_SOURCE 1
#define HAVE_MEMFD_CREATE 1
#endif
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define UTIL_ARCH_BIG_ENDIAN 0
#if defined(__x86_64__)
#define PIPE_ARCH_X86_64
#elif defined(__aarch64__)
#define PIPE_ARCH_AARCH64
#endif
