// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO

#include <cstdio>
#include <cstdlib>

#include <OpenImageIO/half.h>

#include <OpenImageIO/color.h>
#include <OpenImageIO/dassert.h>
#include <OpenImageIO/filter.h>
#include <OpenImageIO/fmath.h>
#include <OpenImageIO/hash.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/optparser.h>
#include <OpenImageIO/parallel.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/thread.h>
#include <OpenImageIO/timer.h>
#include <OpenImageIO/typedesc.h>

#include "buildopts.h"
#include "imageio_pvt.h"

OIIO_NAMESPACE_BEGIN

static int
threads_default()
{
    int n = Strutil::from_string<int>(
        Sysutil::getenv("OPENIMAGEIO_THREADS", Sysutil::getenv("CUE_THREADS")));
    if (n < 1)
        n = Sysutil::hardware_concurrency();
    return n;
}

// Global private data
namespace pvt {
std::recursive_mutex imageio_mutex;
atomic_int oiio_threads(threads_default());
atomic_int oiio_exr_threads(threads_default());
atomic_int oiio_read_chunk(256);
atomic_int oiio_try_all_readers(1);
#ifndef OIIO_OPENEXR_CORE_DEFAULT
#    define OIIO_OPENEXR_CORE_DEFAULT 0
#endif
// Should we use "Exr core C library"?
int openexr_core(OIIO_OPENEXR_CORE_DEFAULT);
int jpeg_com_attributes(1);
int png_linear_premult(0);
int tiff_half(0);
int tiff_multithread(1);
int dds_bc5normal(0);
int limit_channels(1024);
int limit_imagesize_MB(std::min(32 * 1024,
                                int(Sysutil::physical_memory() >> 20)));
int imageinput_strict(0);
ustring font_searchpath(Sysutil::getenv("OPENIMAGEIO_FONTS"));
ustring plugin_searchpath(OIIO_DEFAULT_PLUGIN_SEARCHPATH);
std::string format_list;         // comma-separated list of all formats
std::string input_format_list;   // comma-separated list of readable formats
std::string output_format_list;  // comma-separated list of writable formats
std::string extension_list;      // list of all extensions for all formats
std::string library_list;        // list of all libraries for all formats
int oiio_log_times = Strutil::stoi(Sysutil::getenv("OPENIMAGEIO_LOG_TIMES"));
std::vector<float> oiio_missingcolor;
}  // namespace pvt

using namespace pvt;


namespace {
// Hidden global OIIO data.
static std::recursive_mutex attrib_mutex;
static const int maxthreads = 512;  // reasonable maximum for sanity check

class TimingLog {
public:
    spin_mutex mutex;
    std::map<std::string, std::pair<double, size_t>> timing_map;

    TimingLog() noexcept {}

    // Destructor prints the timing report if oiio_log_times >= 2
    ~TimingLog()
    {
        if (oiio_log_times >= 2)
            std::cout << report();
    }

    // Call like a function to record times (but only if oiio_log_times > 0).
    // The `count` parameter is the number of times the operation was invoked,
    // as tallied by the timer (defaulting to 1).
    void operator()(string_view key, const Timer& timer, int count = 1)
    {
        if (oiio_log_times) {
            auto t = timer();
            spin_lock lock(mutex);
            auto entry = timing_map.find(key);
            if (entry == timing_map.end())
                timing_map[key] = std::make_pair(t, size_t(count));
            else {
                entry->second.first += t;
                entry->second.second += count;
            }
        }
    }

    // Retrieve the report as a big string
    std::string report()
    {
        std::stringstream out;
        spin_lock lock(mutex);
        for (const auto& item : timing_map) {
            size_t ncalls       = item.second.second;
            double time         = item.second.first;
            double percall      = time / ncalls;
            bool use_ms_percall = (percall < 0.1);
            print(out, "{:25s}{:6d} {:7.3f}s  (avg {:6.2f}{})\n", item.first,
                  ncalls, time, percall * (use_ms_percall ? 1000.0 : 1.0),
                  use_ms_percall ? "ms" : "s");
        }
        return out.str();
    }
};
static TimingLog timing_log;



// Pipe-fitting class to set global options, for the sake of optparser.
struct GlobalOptSetter {
public:
    template<typename T> bool attribute(string_view name, T val) const
    {
        return OIIO::attribute(name, val);
    }
};


// Trick to force get the "OPENIMAGEIO_OPTIONS" env var on startup.
bool force_global_opts = []() {
    auto options = Sysutil::getenv("OPENIMAGEIO_OPTIONS");
    // std::cout << "OPTIONS: '" << options << "'\n";
    if (!options.empty())
        attribute("options", options);
    return true;
}();

}  // namespace



// Return a comma-separated list of all the important SIMD/capabilities
// supported by the hardware we're running on right now.
static std::string
hw_simd_caps()
{
    // clang-format off
    std::vector<string_view> caps;
    if (cpu_has_sse2())        caps.emplace_back ("sse2");
    if (cpu_has_sse3())        caps.emplace_back ("sse3");
    if (cpu_has_ssse3())       caps.emplace_back ("ssse3");
    if (cpu_has_sse41())       caps.emplace_back ("sse41");
    if (cpu_has_sse42())       caps.emplace_back ("sse42");
    if (cpu_has_avx())         caps.emplace_back ("avx");
    if (cpu_has_avx2())        caps.emplace_back ("avx2");
    if (cpu_has_avx512f())     caps.emplace_back ("avx512f");
    if (cpu_has_avx512dq())    caps.emplace_back ("avx512dq");
    if (cpu_has_avx512ifma())  caps.emplace_back ("avx512ifma");
    if (cpu_has_avx512pf())    caps.emplace_back ("avx512pf");
    if (cpu_has_avx512cd())    caps.emplace_back ("avx512cd");
    if (cpu_has_avx512bw())    caps.emplace_back ("avx512bw");
    if (cpu_has_avx512vl())    caps.emplace_back ("avx512vl");
    if (cpu_has_fma())         caps.emplace_back ("fma");
    if (cpu_has_f16c())        caps.emplace_back ("f16c");
    if (cpu_has_popcnt())      caps.emplace_back ("popcnt");
    if (cpu_has_rdrand())      caps.emplace_back ("rdrand");
    return Strutil::join (caps, ",");
    // clang-format on
}



// Return a comma-separated list of all the important SIMD/capabilities
// that were enabled as a compile-time option when OIIO was built.
static std::string
oiio_simd_caps()
{
    // clang-format off
    std::vector<string_view> caps;
    if (OIIO_SIMD_SSE >= 2)      caps.emplace_back ("sse2");
    if (OIIO_SIMD_SSE >= 3)      caps.emplace_back ("sse3");
    if (OIIO_SIMD_SSE >= 3)      caps.emplace_back ("ssse3");
    if (OIIO_SIMD_SSE >= 4)      caps.emplace_back ("sse41");
    if (OIIO_SIMD_SSE >= 4)      caps.emplace_back ("sse42");
    if (OIIO_SIMD_AVX)           caps.emplace_back ("avx");
    if (OIIO_SIMD_AVX >= 2)      caps.emplace_back ("avx2");
    if (OIIO_SIMD_AVX >= 512)    caps.emplace_back ("avx512f");
    if (OIIO_AVX512DQ_ENABLED)   caps.emplace_back ("avx512dq");
    if (OIIO_AVX512IFMA_ENABLED) caps.emplace_back ("avx512ifma");
    if (OIIO_AVX512PF_ENABLED)   caps.emplace_back ("avx512pf");
    if (OIIO_AVX512CD_ENABLED)   caps.emplace_back ("avx512cd");
    if (OIIO_AVX512BW_ENABLED)   caps.emplace_back ("avx512bw");
    if (OIIO_AVX512VL_ENABLED)   caps.emplace_back ("avx512vl");
    if (OIIO_SIMD_NEON)          caps.emplace_back ("neon");
    if (OIIO_FMA_ENABLED)        caps.emplace_back ("fma");
    if (OIIO_F16C_ENABLED)       caps.emplace_back ("f16c");
    // if (OIIO_POPCOUNT_ENABLED)   caps.emplace_back ("popcnt");
    return Strutil::join (caps, ",");
    // clang-format on
}


static std::string
oiio_build_compiler()
{
    using Strutil::fmt::format;

    std::string comp;
#if OIIO_INTEL_CLASSIC_COMPILER_VERSION
    comp = format("Intel icc {}", OIIO_INTEL_CLASSIC_COMPILER_VERSION);
#elif OIIO_INTEL_LLVM_COMPILER
    comp = format("Intel icx {}.{}", __clang_major__, __clang_minor__);
#elif OIIO_APPLE_CLANG_VERSION
    comp = format("Apple clang {}.{}", __clang_major__, __clang_minor__);
#elif OIIO_CLANG_VERSION
    comp = format("clang {}.{}", __clang_major__, __clang_minor__);
#elif OIIO_GNUC_VERSION
    comp = format("gcc {}.{}", __GNUC__, __GNUC_MINOR__);
#elif OIIO_MSVS_VERSION
    comp = format("MSVS {}", OIIO_MSVS_VERSION);
#else
    comp = "unknown compiler?";
#endif
    return comp;
}


static std::string
oiio_build_platform()
{
    std::string platform;
#if defined(__linux__)
    platform = "Linux";
#elif defined(__APPLE__)
    platform = "MacOS";
#elif defined(_WIN32)
    platform = "Windows";
#elif defined(__MINGW32__)
    platform = "MinGW";
#elif defined(__FreeBSD__)
    platform = "FreeBSD";
#else
    platform = "UnknownOS";
#endif
    platform += "/";
#if defined(__x86_64__)
    platform += "x86_64";
#elif defined(__i386__)
    platform += "i386";
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__aarch64)
    platform += "ARM";
#else
    platform = "unknown arch?";
#endif
    return platform;
}



void
shutdown()
{
    default_thread_pool_shutdown();
}


int
openimageio_version()
{
    return OIIO_VERSION;
}



void
pvt::append_error(string_view message)
{
    Strutil::pvt::append_error(message);
}



bool
has_error()
{
    return Strutil::pvt::has_error();
}



std::string
geterror(bool clear)
{
    return Strutil::pvt::geterror(clear);
}



void
debug(string_view message)
{
    Strutil::pvt::debug(message);
}



void
log_time(string_view key, const Timer& timer, int count)
{
    timing_log(key, timer, count);
}



bool
attribute(string_view name, TypeDesc type, const void* val)
{
    if (name == "options" && type == TypeDesc::STRING) {
        GlobalOptSetter gos;
        return optparser(gos, *(const char**)val);
    }
    if (name == "threads" && type == TypeInt) {
        int ot = OIIO::clamp(*(const int*)val, 0, maxthreads);
        if (ot == 0)
            ot = threads_default();
        oiio_threads = ot;
        default_thread_pool()->resize(ot - 1);
        return true;
    }
    if (Strutil::starts_with(name, "gpu:")
        || Strutil::starts_with(name, "cuda:")) {
        return pvt::gpu_attribute(name, type, val);
    }

    // Things below here need to buarded by the attrib_mutex
    std::lock_guard lock(attrib_mutex);
    if (name == "read_chunk" && type == TypeInt) {
        oiio_read_chunk = *(const int*)val;
        return true;
    }
    if (name == "font_searchpath" && type == TypeString) {
        font_searchpath = ustring(*(const char**)val);
        return true;
    }
    if (name == "plugin_searchpath" && type == TypeString) {
        plugin_searchpath = ustring(*(const char**)val);
        return true;
    }
    if (name == "exr_threads" && type == TypeInt) {
        oiio_exr_threads = OIIO::clamp(*(const int*)val, -1, maxthreads);
        return true;
    }
    if (name == "openexr:core" && type == TypeInt) {
        openexr_core = *(const int*)val;
        return true;
    }
    if (name == "jpeg:com_attributes" && type == TypeInt) {
        jpeg_com_attributes = *(const int*)val;
        return true;
    }
    if (name == "png:linear_premult" && type == TypeInt) {
        png_linear_premult = *(const int*)val;
        return true;
    }
    if (name == "tiff:half" && type == TypeInt) {
        tiff_half = *(const int*)val;
        return true;
    }
    if (name == "tiff:multithread" && type == TypeInt) {
        tiff_multithread = *(const int*)val;
        return true;
    }
    if (name == "dds:bc5normal" && type == TypeInt) {
        dds_bc5normal = *(const int*)val;
        return true;
    }
    if (name == "limits:channels" && type == TypeInt) {
        limit_channels = *(const int*)val;
        return true;
    }
    if (name == "limits:imagesize_MB" && type == TypeInt) {
        limit_imagesize_MB = *(const int*)val;
        return true;
    }
    if (name == "oiio:print_uncaught_errors" && type == TypeInt) {
        oiio_print_uncaught_errors = *(const int*)val;
        return true;
    }
    if (name == "imagebuf:print_uncaught_errors" && type == TypeInt) {
        imagebuf_print_uncaught_errors = *(const int*)val;
        return true;
    }
    if (name == "imagebuf:use_imagecache" && type == TypeInt) {
        imagebuf_use_imagecache = *(const int*)val;
        return true;
    }
    if (name == "imageinput:strict" && type == TypeInt) {
        imageinput_strict = *(const int*)val;
        return true;
    }
    if (name == "use_tbb" && type == TypeInt) {
        oiio_use_tbb = *(const int*)val;
        return true;
    }
    if (name == "debug" && type == TypeInt) {
        oiio_print_debug = *(const int*)val;
        return true;
    }
    if (name == "log_times" && type == TypeInt) {
        oiio_log_times = *(const int*)val;
        return true;
    }
    if (name == "missingcolor" && type.basetype == TypeDesc::FLOAT) {
        // missingcolor as float array
        oiio_missingcolor.assign((const float*)val,
                                 (const float*)val + type.numelements());
        return true;
    }
    if (name == "missingcolor" && type == TypeString) {
        // missingcolor as string
        oiio_missingcolor = Strutil::extract_from_list_string<float>(
            *(const char**)val);
        return true;
    }
    if (name == "try_all_readers" && type == TypeInt) {
        oiio_try_all_readers = *(const int*)val;
        return true;
    }

    return false;
}



bool
getattribute(string_view name, TypeDesc type, void* val)
{
    using Strutil::fmt::format;
    if (name == "threads" && type == TypeInt) {
        *(int*)val = oiio_threads;
        return true;
    }
    if (name == "version" && type == TypeString) {
        *(ustring*)val = ustring(OIIO_VERSION_STRING);
        return true;
    }
    if (Strutil::starts_with(name, "gpu:")
        || Strutil::starts_with(name, "cuda:")) {
        return pvt::gpu_getattribute(name, type, val);
    }

    // Things below here need to buarded by the attrib_mutex
    std::lock_guard lock(attrib_mutex);
    if (name == "read_chunk" && type == TypeInt) {
        *(int*)val = oiio_read_chunk;
        return true;
    }
    if (name == "font_searchpath" && type == TypeString) {
        *(ustring*)val = font_searchpath;
        return true;
    }
    if (name == "plugin_searchpath" && type == TypeString) {
        *(ustring*)val = plugin_searchpath;
        return true;
    }
    if (name == "format_list" && type == TypeString) {
        if (format_list.empty())
            pvt::catalog_all_plugins(plugin_searchpath.string());
        *(ustring*)val = ustring(format_list);
        return true;
    }
    if (name == "input_format_list" && type == TypeString) {
        if (input_format_list.empty())
            pvt::catalog_all_plugins(plugin_searchpath.string());
        *(ustring*)val = ustring(input_format_list);
        return true;
    }
    if (name == "output_format_list" && type == TypeString) {
        if (output_format_list.empty())
            pvt::catalog_all_plugins(plugin_searchpath.string());
        *(ustring*)val = ustring(output_format_list);
        return true;
    }
    if (name == "extension_list" && type == TypeString) {
        if (extension_list.empty())
            pvt::catalog_all_plugins(plugin_searchpath.string());
        *(ustring*)val = ustring(extension_list);
        return true;
    }
    if (name == "library_list" && type == TypeString) {
        if (library_list.empty())
            pvt::catalog_all_plugins(plugin_searchpath.string());
        *(ustring*)val = ustring(library_list);
        return true;
    }
    if (name == "font_dir_list" && type == TypeString) {
        *(ustring*)val = ustring(Strutil::join(font_dirs(), ";"));
        return true;
    }
    if (name == "font_file_list" && type == TypeString) {
        *(ustring*)val = ustring(Strutil::join(font_file_list(), ";"));
        return true;
    }
    if (name == "font_list" && type == TypeString) {
        *(ustring*)val = ustring(Strutil::join(font_list(), ";"));
        return true;
    }
    if (name == "font_family_list" && type == TypeString) {
        *(ustring*)val = ustring(Strutil::join(font_family_list(), ";"));
        return true;
    }
    if (Strutil::starts_with(name, "font_style_list:") && type == TypeString) {
        string_view family = name.substr(strlen("font_style_list:"));
        *(ustring*)val = ustring(Strutil::join(font_style_list(family), ";"));
        return true;
    }
    if (Strutil::starts_with(name, "font_filename:") && type == TypeString) {
        std::vector<string_view> tokens;
        Strutil::split(name, tokens, ":");
        string_view family = tokens.size() >= 1 ? tokens[1] : string_view();
        string_view style  = tokens.size() >= 2 ? tokens[2] : string_view();
        *(ustring*)val     = ustring(font_filename(family, style));
        return true;
    }
    if (name == "filter_list" && type == TypeString) {
        std::vector<string_view> filternames;
        for (int i = 0, e = Filter2D::num_filters(); i < e; ++i)
            filternames.emplace_back(Filter2D::get_filterdesc(i).name);
        *(ustring*)val = ustring(Strutil::join(filternames, ";"));
        return true;
    }
    if (name == "exr_threads" && type == TypeInt) {
        *(int*)val = oiio_exr_threads;
        return true;
    }
    if (name == "openexr:core" && type == TypeInt) {
        *(int*)val = openexr_core;
        return true;
    }
    if (name == "jpeg:com_attributes" && type == TypeInt) {
        *(int*)val = jpeg_com_attributes;
        return true;
    }
    if (name == "png:linear_premult" && type == TypeInt) {
        *(int*)val = png_linear_premult;
        return true;
    }
    if (name == "tiff:half" && type == TypeInt) {
        *(int*)val = tiff_half;
        return true;
    }
    if (name == "limits:channels" && type == TypeInt) {
        *(int*)val = limit_channels;
        return true;
    }
    if (name == "limits:imagesize_MB" && type == TypeInt) {
        *(int*)val = limit_imagesize_MB;
        return true;
    }
    if (name == "tiff:multithread" && type == TypeInt) {
        *(int*)val = tiff_multithread;
        return true;
    }
    if (name == "dds:bc5normal" && type == TypeInt) {
        *(int*)val = dds_bc5normal;
        return true;
    }
    if (name == "oiio:print_uncaught_errors" && type == TypeInt) {
        *(int*)val = oiio_print_uncaught_errors;
        return true;
    }
    if (name == "imagebuf:print_uncaught_errors" && type == TypeInt) {
        *(int*)val = imagebuf_print_uncaught_errors;
        return true;
    }
    if (name == "imagebuf:use_imagecache" && type == TypeInt) {
        *(int*)val = imagebuf_use_imagecache;
        return true;
    }
    if (name == "imageinput:strict" && type == TypeInt) {
        *(int*)val = imageinput_strict;
        return true;
    }
    if (name == "use_tbb" && type == TypeInt) {
        *(int*)val = oiio_use_tbb;
        return true;
    }
    if (name == "debug" && type == TypeInt) {
        *(int*)val = oiio_print_debug;
        return true;
    }
    if (name == "log_times" && type == TypeInt) {
        *(int*)val = oiio_log_times;
        return true;
    }
    if (name == "timing_report" && type == TypeString) {
        *(ustring*)val = ustring(timing_log.report());
        return true;
    }
    if (name == "hw:simd" && type == TypeString) {
        *(ustring*)val = ustring(hw_simd_caps());
        return true;
    }
    if ((name == "build:simd" || name == "oiio:simd") && type == TypeString) {
        *(ustring*)val = ustring(oiio_simd_caps());
        return true;
    }
    if (name == "build:compiler" && type == TypeString) {
        *(ustring*)val = ustring(oiio_build_compiler());
        return true;
    }
    if (name == "build:platform" && type == TypeString) {
        *(ustring*)val = ustring(oiio_build_platform());
        return true;
    }
    if (name == "build:dependencies" && type == TypeString) {
        *(ustring*)val = ustring(OIIO_ALL_BUILD_DEPS_FOUND);
        return true;
    }
    if (name == "resident_memory_used_MB" && type == TypeInt) {
        *(int*)val = int(Sysutil::memory_used(true) >> 20);
        return true;
    }
    if (name == "resident_memory_used_MB" && type == TypeFloat) {
        *(float*)val = float(Sysutil::memory_used(true) >> 20);
        return true;
    }
    if (name == "missingcolor" && type.basetype == TypeDesc::FLOAT
        && oiio_missingcolor.size()) {
        // missingcolor as float array
        int n  = type.basevalues();
        int nm = std::min(int(oiio_missingcolor.size()), n);
        for (int i = 0; i < nm; ++i)
            ((float*)val)[i] = oiio_missingcolor[i];
        for (int i = nm; i < n; ++i)
            ((float*)val)[i] = 0.0f;
        return true;
    }
    if (name == "missingcolor" && type == TypeString) {
        // missingcolor as string
        *(ustring*)val = ustring(Strutil::join(oiio_missingcolor, ","));
        return true;
    }
    if (name == "try_all_readers" && type == TypeInt) {
        *(int*)val = oiio_try_all_readers;
        return true;
    }
    if (name == "opencolorio_version" && type == TypeString) {
        int v          = ColorConfig::OpenColorIO_version_hex();
        *(ustring*)val = ustring::fmtformat("{}.{}.{}", v >> 24,
                                            (v >> 16) & 0xff, (v >> 8) & 0xff);
        return true;
    }
    if (name == "IB_local_mem_current" && type == TypeInt64) {
        *(long long*)val = IB_local_mem_current;
        return true;
    }
    if (name == "IB_local_mem_peak" && type == TypeInt64) {
        *(long long*)val = IB_local_mem_peak;
        return true;
    }
    if (name == "IB_total_open_time" && type == TypeFloat) {
        *(float*)val = IB_total_open_time;
        return true;
    }
    if (name == "IB_total_image_read_time" && type == TypeFloat) {
        *(float*)val = IB_total_image_read_time;
        return true;
    }
    return false;
}



namespace {

/// Type-independent template for turning potentially
/// non-contiguous-stride data (e.g. "RGB RGB ") into contiguous-stride
/// ("RGBRGB").  Caller must pass in a dst pointing to enough memory to
/// hold the contiguous rectangle.  Return a ptr to where the contiguous
/// data ended up, which is either dst or src (if the strides indicated
/// that data were already contiguous).
template<typename T>
const T*
_contiguize(const T* src, int nchannels, stride_t xstride, stride_t ystride,
            stride_t zstride, T* dst, int width, int height, int depth)
{
    int datasize = sizeof(T);
    if (xstride == nchannels * datasize && ystride == xstride * width
        && (zstride == ystride * height || !zstride))
        return src;

    if (depth < 1)  // Safeguard against volume-unaware clients
        depth = 1;

    T* dstsave = dst;
    if (xstride == nchannels * datasize) {
        // Optimize for contiguous scanlines, but not from scanline to scanline
        for (int z = 0; z < depth;
             ++z, src = (const T*)((char*)src + zstride)) {
            const T* scanline = src;
            for (int y        = 0; y < height; ++y, dst += nchannels * width,
                     scanline = (const T*)((char*)scanline + ystride))
                memcpy(dst, scanline, xstride * width);
        }
    } else {
        for (int z = 0; z < depth;
             ++z, src = (const T*)((char*)src + zstride)) {
            const T* scanline = src;
            for (int y = 0; y < height;
                 ++y, scanline = (const T*)((char*)scanline + ystride)) {
                const T* pixel = scanline;
                for (int x = 0; x < width;
                     ++x, pixel = (const T*)((char*)pixel + xstride))
                    for (int c = 0; c < nchannels; ++c)
                        *dst++ = pixel[c];
            }
        }
    }
    return dstsave;
}

}  // namespace



span<const std::byte>
pvt::contiguize(const image_span<const std::byte>& src, span<std::byte> dst)
{
    // Contiguized result must fit in dst
    OIIO_DASSERT(src.size_bytes() <= dst.size_bytes());

    if (src.is_contiguous()) {
        // Already contiguous -- don't copy, just return a span representation
        // of the place it already lives in src.
        return make_cspan(src.data(), src.size_bytes());
    } else {
        // Non-contiguous -- rely on copy_image() to do the heavy lifting.
        image_span dstspan(dst.data(), src.nchannels(), src.width(),
                           src.height(), src.depth(), AutoStride, AutoStride,
                           AutoStride, AutoStride, src.chansize());
        OIIO_DASSERT(dstspan.size_bytes() == src.size_bytes());
        copy_image(dstspan, src);
        return make_cspan(dst.data(), src.size_bytes());
    }
}



const void*
pvt::contiguize(const void* src, int nchannels, stride_t xstride,
                stride_t ystride, stride_t zstride, void* dst, int width,
                int height, int depth, TypeDesc format)
{
    OIIO_DASSERT(nchannels >= 0 && width >= 0 && height >= 0 && depth >= 0);
    switch (format.basetype) {
    case TypeDesc::FLOAT:
        return _contiguize((const float*)src, nchannels, xstride, ystride,
                           zstride, (float*)dst, width, height, depth);
    case TypeDesc::INT8:
    case TypeDesc::UINT8:
        return _contiguize((const char*)src, nchannels, xstride, ystride,
                           zstride, (char*)dst, width, height, depth);
    case TypeDesc::HALF: OIIO_DASSERT(sizeof(half) == sizeof(short));
    case TypeDesc::INT16:
    case TypeDesc::UINT16:
        return _contiguize((const short*)src, nchannels, xstride, ystride,
                           zstride, (short*)dst, width, height, depth);
    case TypeDesc::INT:
    case TypeDesc::UINT:
        return _contiguize((const int*)src, nchannels, xstride, ystride,
                           zstride, (int*)dst, width, height, depth);
    case TypeDesc::INT64:
    case TypeDesc::UINT64:
        return _contiguize((const long long*)src, nchannels, xstride, ystride,
                           zstride, (long long*)dst, width, height, depth);
    case TypeDesc::DOUBLE:
        return _contiguize((const double*)src, nchannels, xstride, ystride,
                           zstride, (double*)dst, width, height, depth);
    default:
        OIIO_ASSERT(0 && "OpenImageIO::contiguize : bad format");
        return NULL;
    }
}



const float*
pvt::convert_to_float(const void* src, float* dst, int nvals, TypeDesc format)
{
    switch (format.basetype) {
    case TypeDesc::FLOAT: return (float*)src;
    case TypeDesc::UINT8:
        convert_type((const unsigned char*)src, dst, nvals);
        break;
    case TypeDesc::HALF: convert_type((const half*)src, dst, nvals); break;
    case TypeDesc::UINT16:
        convert_type((const unsigned short*)src, dst, nvals);
        break;
    case TypeDesc::INT8: convert_type((const char*)src, dst, nvals); break;
    case TypeDesc::INT16: convert_type((const short*)src, dst, nvals); break;
    case TypeDesc::INT: convert_type((const int*)src, dst, nvals); break;
    case TypeDesc::UINT:
        convert_type((const unsigned int*)src, dst, nvals);
        break;
    case TypeDesc::INT64:
        convert_type((const long long*)src, dst, nvals);
        break;
    case TypeDesc::UINT64:
        convert_type((const unsigned long long*)src, dst, nvals);
        break;
    case TypeDesc::DOUBLE: convert_type((const double*)src, dst, nvals); break;
    default: OIIO_ASSERT(0 && "ERROR to_float: bad format"); return NULL;
    }
    return dst;
}



const void*
pvt::convert_from_float(const float* src, void* dst, size_t nvals,
                        TypeDesc format)
{
    // If no source pixels, assume zeroes
    if (!src) {
        memset(dst, 0, nvals * format.size());
        return dst;
    }

    // clang-format off
    switch (format.basetype) {
    case TypeDesc::FLOAT:
        // If it's already float, return the source itself
        return src;
    case TypeDesc::HALF:   convert_type(src, (half*)    dst, nvals); break;
    case TypeDesc::UINT8:  convert_type(src, (uint8_t*) dst, nvals); break;
    case TypeDesc::UINT16: convert_type(src, (uint16_t*)dst, nvals); break;
    case TypeDesc::UINT:   convert_type(src, (uint32_t*)dst, nvals); break;
    case TypeDesc::INT8:   convert_type(src, (int8_t*)  dst, nvals); break;
    case TypeDesc::INT16:  convert_type(src, (int16_t*) dst, nvals); break;
    case TypeDesc::INT:    convert_type(src, (int32_t*) dst, nvals); break;
    case TypeDesc::DOUBLE: convert_type(src, (double*)  dst, nvals); break;
    case TypeDesc::INT64:  convert_type(src, (int64_t*) dst, nvals); break;
    case TypeDesc::UINT64: convert_type(src, (uint64_t*)dst, nvals); break;
    default: OIIO_ASSERT(0 && "ERROR from_float: bad format"); dst = nullptr;
    }
    // clang-format on
    return dst;
}


const void*
pvt::parallel_convert_from_float(const float* src, void* dst, size_t nvals,
                                 TypeDesc format)
{
    if (format.basetype == TypeDesc::FLOAT)
        return src;

    parallel_for_chunked(0, int64_t(nvals), 0, [=](int64_t b, int64_t e) {
        convert_from_float(src + b, (char*)dst + b * format.size(), e - b,
                           format);
    });
    return dst;
}



bool
convert_pixel_values(TypeDesc src_type, const void* src, TypeDesc dst_type,
                     void* dst, int n)
{
    // If no conversion is necessary, just memcpy
    if ((src_type == dst_type || dst_type.basetype == TypeDesc::UNKNOWN)) {
        memcpy(dst, src, n * src_type.size());
        return true;
    }

    if (dst_type == TypeFloat) {
        // Special case -- converting non-float to float
        pvt::convert_to_float(src, (float*)dst, n, src_type);
        return true;
    }

    // Conversion is to a non-float type

    std::unique_ptr<float[]> tmp;  // In case we need a lot of temp space
    float* buf = (float*)src;
    if (src_type != TypeFloat) {
        // If src is also not float, convert through an intermediate buffer
        if (n <= 4096)  // If < 16k, use the stack
            buf = OIIO_ALLOCA(float, n);
        else {
            tmp.reset(new float[n]);  // Freed when tmp exists its scope
            buf = tmp.get();
        }
        pvt::convert_to_float(src, buf, n, src_type);
    }

    // Convert float to 'dst_type'
    switch (dst_type.basetype) {
    case TypeDesc::UINT8: convert_type(buf, (unsigned char*)dst, n); break;
    case TypeDesc::UINT16: convert_type(buf, (unsigned short*)dst, n); break;
    case TypeDesc::HALF: convert_type(buf, (half*)dst, n); break;
    case TypeDesc::INT8: convert_type(buf, (char*)dst, n); break;
    case TypeDesc::INT16: convert_type(buf, (short*)dst, n); break;
    case TypeDesc::INT: convert_type(buf, (int*)dst, n); break;
    case TypeDesc::UINT: convert_type(buf, (unsigned int*)dst, n); break;
    case TypeDesc::INT64: convert_type(buf, (long long*)dst, n); break;
    case TypeDesc::UINT64:
        convert_type(buf, (unsigned long long*)dst, n);
        break;
    case TypeDesc::DOUBLE: convert_type(buf, (double*)dst, n); break;
    default: return false;  // unknown format
    }

    return true;
}



bool
convert_image(int nchannels, int width, int height, int depth, const void* src,
              TypeDesc src_type, stride_t src_xstride, stride_t src_ystride,
              stride_t src_zstride, void* dst, TypeDesc dst_type,
              stride_t dst_xstride, stride_t dst_ystride, stride_t dst_zstride)
{
    // If no format conversion is taking place, use the simplified
    // copy_image.
    if (src_type == dst_type)
        return copy_image(nchannels, width, height, depth, src,
                          src_type.size() * nchannels, src_xstride, src_ystride,
                          src_zstride, dst, dst_xstride, dst_ystride,
                          dst_zstride);

    ImageSpec::auto_stride(src_xstride, src_ystride, src_zstride, src_type,
                           nchannels, width, height);
    ImageSpec::auto_stride(dst_xstride, dst_ystride, dst_zstride, dst_type,
                           nchannels, width, height);
    bool result = true;
    bool contig = (src_xstride == stride_t(nchannels * src_type.size())
                   && dst_xstride == stride_t(nchannels * dst_type.size()));
    for (int z = 0; z < depth; ++z) {
        for (int y = 0; y < height; ++y) {
            const char* f = (const char*)src
                            + (z * src_zstride + y * src_ystride);
            char* t = (char*)dst + (z * dst_zstride + y * dst_ystride);
            if (contig) {
                // Special case: pixels within each row are contiguous
                // in both src and dst and we're copying all channels.
                // Be efficient by converting each scanline as a single
                // unit.  (Note that within convert_pixel_values, a memcpy
                // will be used if the formats are identical.)
                result &= convert_pixel_values(src_type, f, dst_type, t,
                                               nchannels * width);
            } else {
                // General case -- anything goes with strides.
                for (int x = 0; x < width; ++x) {
                    result &= convert_pixel_values(src_type, f, dst_type, t,
                                                   nchannels);
                    f += src_xstride;
                    t += dst_xstride;
                }
            }
        }
    }
    return result;
}



bool
parallel_convert_image(int nchannels, int width, int height, int depth,
                       const void* src, TypeDesc src_type, stride_t src_xstride,
                       stride_t src_ystride, stride_t src_zstride, void* dst,
                       TypeDesc dst_type, stride_t dst_xstride,
                       stride_t dst_ystride, stride_t dst_zstride, int nthreads)
{
    if (nthreads <= 0)
        nthreads = oiio_threads;
    nthreads
        = clamp(int((int64_t(width) * height * depth * nchannels) / 100000), 1,
                nthreads);
    if (nthreads <= 1)
        return convert_image(nchannels, width, height, depth, src, src_type,
                             src_xstride, src_ystride, src_zstride, dst,
                             dst_type, dst_xstride, dst_ystride, dst_zstride);

    ImageSpec::auto_stride(src_xstride, src_ystride, src_zstride, src_type,
                           nchannels, width, height);
    ImageSpec::auto_stride(dst_xstride, dst_ystride, dst_zstride, dst_type,
                           nchannels, width, height);

    int blocksize = std::max(1, height / nthreads);
    parallel_for_chunked(0, height, blocksize, [=](int64_t ybegin, int64_t yend) {
        convert_image(nchannels, width, yend - ybegin, depth,
                      (const char*)src + src_ystride * ybegin, src_type,
                      src_xstride, src_ystride, src_zstride,
                      (char*)dst + dst_ystride * ybegin, dst_type, dst_xstride,
                      dst_ystride, dst_zstride);
    });
    return true;
}



bool
copy_image(int nchannels, int width, int height, int depth, const void* src,
           stride_t pixelsize, stride_t src_xstride, stride_t src_ystride,
           stride_t src_zstride, void* dst, stride_t dst_xstride,
           stride_t dst_ystride, stride_t dst_zstride)
{
    stride_t channelsize = pixelsize / nchannels;
    ImageSpec::auto_stride(src_xstride, src_ystride, src_zstride, channelsize,
                           nchannels, width, height);
    ImageSpec::auto_stride(dst_xstride, dst_ystride, dst_zstride, channelsize,
                           nchannels, width, height);
    bool contig = (src_xstride == dst_xstride
                   && src_xstride == (stride_t)pixelsize);
    for (int z = 0; z < depth; ++z) {
        for (int y = 0; y < height; ++y) {
            const char* f = (const char*)src
                            + (z * src_zstride + y * src_ystride);
            char* t = (char*)dst + (z * dst_zstride + y * dst_ystride);
            if (contig) {
                // Special case: pixels within each row are contiguous
                // in both src and dst and we're copying all channels.
                // Be efficient by converting each scanline as a single
                // unit.
                memcpy(t, f, width * pixelsize);
            } else {
                // General case -- anything goes with strides.
                for (int x = 0; x < width; ++x) {
                    memcpy(t, f, pixelsize);
                    f += src_xstride;
                    t += dst_xstride;
                }
            }
        }
    }
    return true;
}



template<typename T>
void
aligned_copy_image(const image_span<std::byte>& dst,
                   const image_span<const std::byte>& src)
{
    size_t systride  = src.ystride();
    size_t dystride  = dst.ystride();
    size_t chunksize = src.chansize() * src.nchannels();
    size_t nT        = chunksize / sizeof(T);
    size_t sstrideT  = src.xstride() / sizeof(T);
    size_t dstrideT  = dst.xstride() / sizeof(T);
    for (uint32_t z = 0; z < src.depth(); ++z) {
        std::byte* dscanline       = dst.getptr(0, 0, 0, z);
        const std::byte* sscanline = src.getptr(0, 0, 0, z);
        for (uint32_t y = 0; y < src.height(); ++y) {
            auto dscanlineT = reinterpret_cast<T*>(dscanline);
            auto sscanlineT = reinterpret_cast<const T*>(sscanline);
            for (uint32_t x = 0; x < src.width(); ++x) {
                for (size_t c = 0; c < nT; ++c) {
                    dscanlineT[x * dstrideT + c] = sscanlineT[x * sstrideT + c];
                }
            }
            sscanline += systride;
            dscanline += dystride;
        }
    }
}



bool
copy_image(const image_span<std::byte>& dst,
           const image_span<const std::byte>& src)
{
    OIIO_DASSERT(src.width() == dst.width() && src.height() == dst.height()
                 && src.depth() == dst.depth()
                 && src.nchannels() == dst.nchannels()
                 && src.chansize() == dst.chansize());
    if (src.is_contiguous() && dst.is_contiguous()) {
        // Whole image is contiguous -- just do it in one memcpy
        memcpy(dst.data(), src.data(), src.size_bytes());
        return true;
    }
    if (src.is_contiguous_scanline() && dst.is_contiguous_scanline()) {
        // Scanlines are contiguous -- do one memcpy per scanline
        size_t chunksize = src.chansize() * src.nchannels()
                           * size_t(src.width());
        for (uint32_t z = 0; z < src.depth(); ++z) {
            for (uint32_t y = 0; y < src.height(); ++y) {
                std::byte* d       = dst.getptr(0, 0, y, z);
                const std::byte* s = src.getptr(0, 0, y, z);
                memcpy(d, s, chunksize);
            }
        }
        return true;
    }
    if (dst.is_contiguous_pixel()) {
        size_t systride = src.ystride();
        size_t dystride = dst.ystride();
        // Pixels are contiguous -- do one memcpy per pixel
        size_t chunksize = src.chansize() * src.nchannels();
#if 1
        // Speedup trick: if we 'or' all the pointers and strides and the
        // result is a multiple of 4, we can copy 4 bytes at a time.
        size_t mashed = (chunksize | uintptr_t(src.data())
                         | uintptr_t(dst.data()) | src.xstride() | src.ystride()
                         | src.zstride());
        if ((mashed & 3) == 0) {
            aligned_copy_image<uint32_t>(dst, src);
            return true;
        }
        if ((mashed & 1) == 0) {
            aligned_copy_image<uint16_t>(dst, src);
            return true;
        }
#endif
        for (uint32_t z = 0; z < src.depth(); ++z) {
            std::byte* dscanline       = dst.getptr(0, 0, 0, z);
            const std::byte* sscanline = src.getptr(0, 0, 0, z);
            for (uint32_t y = 0; y < src.height(); ++y) {
                for (uint32_t x = 0; x < src.width(); ++x) {
                    memcpy(dscanline + x * dst.xstride(),
                           sscanline + x * src.xstride(), chunksize);
                }
                sscanline += systride;
                dscanline += dystride;
            }
        }
        return true;
    }
    // General case -- have to do item by item copy.
    // FIXME: is there advantage to doing copies individually rather
    // then with memcpy?
    size_t chunksize = src.chansize();
    for (uint32_t z = 0; z < src.depth(); ++z) {
        for (uint32_t y = 0; y < src.height(); ++y) {
            std::byte* dscanline       = dst.getptr(0, 0, y, z);
            const std::byte* sscanline = src.getptr(0, 0, y, z);
            for (uint32_t x = 0; x < src.width(); ++x) {
                std::byte* dpel       = dscanline + x * dst.xstride();
                const std::byte* spel = sscanline + x * src.xstride();
                for (uint32_t c = 0; x < src.nchannels(); ++c) {
                    memcpy(dpel + c * dst.chanstride(),
                           spel + c * src.chanstride(), chunksize);
                }
            }
        }
    }
    return true;
}



void
add_bluenoise(int nchannels, int width, int height, int depth, float* data,
              stride_t xstride, stride_t ystride, stride_t zstride,
              float ditheramplitude, int alpha_channel, int z_channel,
              unsigned int ditherseed, int chorigin, int xorigin, int yorigin,
              int zorigin)
{
    ImageSpec::auto_stride(xstride, ystride, zstride, sizeof(float), nchannels,
                           width, height);
    char* plane = (char*)data;
    for (int z = 0; z < depth; ++z, plane += zstride) {
        char* scanline = plane;
        for (int y = 0; y < height; ++y, scanline += ystride) {
            char* pixel = scanline;
            for (int x = 0; x < width; ++x, pixel += xstride) {
                float* val = (float*)pixel;
                for (int c = 0; c < nchannels; ++c, ++val) {
                    int channel = c + chorigin;
                    if (channel == alpha_channel || channel == z_channel)
                        continue;
                    float dither
                        = pvt::bluenoise_4chan_ptr(x + xorigin, y + yorigin,
                                                   z + zorigin, channel & (~3),
                                                   ditherseed)[channel & 3];
                    *val += ditheramplitude * (dither - 0.5f);
                }
            }
        }
    }
}



void
add_dither(int nchannels, int width, int height, int depth, float* data,
           stride_t xstride, stride_t ystride, stride_t zstride,
           float ditheramplitude, int alpha_channel, int z_channel,
           unsigned int ditherseed, int chorigin, int xorigin, int yorigin,
           int zorigin)
{
    add_bluenoise(nchannels, width, height, depth, data, xstride, ystride,
                  zstride, ditheramplitude, alpha_channel, z_channel,
                  ditherseed, chorigin, xorigin, yorigin, zorigin);
}



template<typename T>
static void
premult_impl(int width, int height, int depth, int chbegin, int chend, T* data,
             stride_t xstride, stride_t ystride, stride_t zstride,
             int alpha_channel, int z_channel)
{
    char* plane = (char*)data;
    for (int z = 0; z < depth; ++z, plane += zstride) {
        char* scanline = plane;
        for (int y = 0; y < height; ++y, scanline += ystride) {
            char* pixel = scanline;
            for (int x = 0; x < width; ++x, pixel += xstride) {
                DataArrayProxy<T, float> val((T*)pixel);
                float alpha = val[alpha_channel];
                for (int c = chbegin; c < chend; ++c) {
                    if (c == alpha_channel || c == z_channel)
                        continue;
                    val[c] = alpha * val[c];
                }
            }
        }
    }
}



void
premult(int nchannels, int width, int height, int depth, int chbegin, int chend,
        TypeDesc datatype, void* data, stride_t xstride, stride_t ystride,
        stride_t zstride, int alpha_channel, int z_channel)
{
    if (alpha_channel < 0 || alpha_channel > nchannels)
        return;  // nothing to do
    ImageSpec::auto_stride(xstride, ystride, zstride, datatype.size(),
                           nchannels, width, height);
    switch (datatype.basetype) {
    case TypeDesc::FLOAT:
        premult_impl(width, height, depth, chbegin, chend, (float*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::UINT8:
        premult_impl(width, height, depth, chbegin, chend, (unsigned char*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::UINT16:
        premult_impl(width, height, depth, chbegin, chend,
                     (unsigned short*)data, xstride, ystride, zstride,
                     alpha_channel, z_channel);
        break;
    case TypeDesc::HALF:
        premult_impl(width, height, depth, chbegin, chend, (half*)data, xstride,
                     ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::INT8:
        premult_impl(width, height, depth, chbegin, chend, (char*)data, xstride,
                     ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::INT16:
        premult_impl(width, height, depth, chbegin, chend, (short*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::INT:
        premult_impl(width, height, depth, chbegin, chend, (int*)data, xstride,
                     ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::UINT:
        premult_impl(width, height, depth, chbegin, chend, (unsigned int*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::INT64:
        premult_impl(width, height, depth, chbegin, chend, (int64_t*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::UINT64:
        premult_impl(width, height, depth, chbegin, chend, (uint64_t*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    case TypeDesc::DOUBLE:
        premult_impl(width, height, depth, chbegin, chend, (double*)data,
                     xstride, ystride, zstride, alpha_channel, z_channel);
        break;
    default: OIIO_ASSERT(0 && "OIIO::premult() of an unsupported type"); break;
    }
}



bool
wrap_black(int& coord, int origin, int width)
{
    return (coord >= origin && coord < (width + origin));
}


bool
wrap_clamp(int& coord, int origin, int width)
{
    if (coord < origin)
        coord = origin;
    else if (coord >= origin + width)
        coord = origin + width - 1;
    return true;
}


bool
wrap_periodic(int& coord, int origin, int width)
{
    coord -= origin;
    coord %= width;
    if (coord < 0)  // Fix negative values
        coord += width;
    coord += origin;
    return true;
}


bool
wrap_periodic_pow2(int& coord, int origin, int width)
{
    OIIO_DASSERT(ispow2(width));
    coord -= origin;
    coord &= (width - 1);  // Shortcut periodic if we're sure it's a pow of 2
    coord += origin;
    return true;
}


bool
wrap_mirror(int& coord, int origin, int width)
{
    coord -= origin;
    if (coord < 0)
        coord = -coord - 1;
    int iter = coord / width;  // Which iteration of the pattern?
    coord -= iter * width;
    if (iter & 1)  // Odd iterations -- flip the sense
        coord = width - 1 - coord;
    OIIO_DASSERT_MSG(coord >= 0 && coord < width,
                     "width=%d, origin=%d, result=%d", width, origin, coord);
    coord += origin;
    return true;
}



/// Verify that the image_span has all its contents lying within the
/// contiguous span.
bool
image_span_within_span(const image_span<const std::byte>& ispan,
                       span<const std::byte> contiguous) noexcept
{
    // Start with first,last being the first byte of pixel 0, channel 0
    const std::byte* first = ispan.data();  // first byte of ispan
    const std::byte* last  = ispan.data();  // last byte of ispan
    // Extend them to the start of the first pixel of the first and last image
    // plane.
    if (ispan.zstride() > 0)
        last += ispan.zstride() * (ispan.depth() - 1);
    else
        first += ispan.zstride() * (ispan.depth() - 1);  // neg stride
    // Extend them to the start of the first pixel of the first and last
    // scanline of the first and last image plane.
    if (ispan.ystride() > 0)
        last += ispan.ystride() * (ispan.height() - 1);
    else
        first += ispan.ystride() * (ispan.height() - 1);  // neg stride
    // Extend them to the start of the first pixel of the first and last
    // column of the first and last scanline of the first and last image
    // plane.
    if (ispan.xstride() > 0)
        last += ispan.xstride() * (ispan.width() - 1);
    else
        first += ispan.xstride() * (ispan.width() - 1);  // neg stride
    // Make sure they cover the whole of those extreme pixels
    if (ispan.chanstride() > 0)
        last += ispan.chanstride() * (ispan.nchannels() - 1);
    else
        first += ispan.chanstride() * (ispan.nchannels() - 1);  // neg stride
    // Make sure last covers the whole data type
    last += ispan.chansize() - 1;
    return (first >= contiguous.data()
            && last < contiguous.data() + contiguous.size());
}


OIIO_NAMESPACE_END
