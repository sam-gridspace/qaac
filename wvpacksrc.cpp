#include <io.h>
#include <sys/stat.h>
#include "wvpacksrc.h"
#include <wavpack.h>
#include "strutil.h"
#include "itunetags.h"
#include "cuesheet.h"
#include "chanmap.h"
#include "cautil.h"
#include "win32util.h"

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("!?"); } \
    while (0)

WavpackModule::WavpackModule(const std::wstring &path)
    : m_dl(path)
{
    if (!m_dl.loaded()) return;
    try {
        CHECK(GetLibraryVersionString =
              m_dl.fetch( "WavpackGetLibraryVersionString"));
        CHECK(OpenFileInputEx = m_dl.fetch( "WavpackOpenFileInputEx"));
        CHECK(CloseFile = m_dl.fetch( "WavpackCloseFile"));
        CHECK(GetBitsPerSample = m_dl.fetch( "WavpackGetBitsPerSample"));
        CHECK(GetChannelMask = m_dl.fetch( "WavpackGetChannelMask"));
        CHECK(GetMode = m_dl.fetch( "WavpackGetMode"));
        CHECK(GetNumChannels = m_dl.fetch( "WavpackGetNumChannels"));
        CHECK(GetNumSamples = m_dl.fetch( "WavpackGetNumSamples"));
        CHECK(GetNumTagItems = m_dl.fetch( "WavpackGetNumTagItems"));
        CHECK(GetSampleIndex = m_dl.fetch( "WavpackGetSampleIndex"));
        CHECK(GetSampleRate = m_dl.fetch( "WavpackGetSampleRate"));
        CHECK(GetTagItem = m_dl.fetch( "WavpackGetTagItem"));
        CHECK(GetTagItemIndexed = m_dl.fetch( "WavpackGetTagItemIndexed"));
        CHECK(SeekSample = m_dl.fetch( "WavpackSeekSample"));
        CHECK(UnpackSamples = m_dl.fetch( "WavpackUnpackSamples"));
    } catch (...) {
        m_dl.reset();
    }
}

struct WavpackStreamReaderImpl: public WavpackStreamReader
{
    WavpackStreamReaderImpl()
    {
        static WavpackStreamReader t = {
            read, tell, seek_abs, seek, pushback, size, seekable, 0/*write*/
        };
        std::memcpy(this, &t, sizeof(WavpackStreamReader));
    }
    static int32_t read(void *cookie, void *data, int32_t count)
    {
        int fd = reinterpret_cast<int>(cookie);
        return util::nread(fd, data, count);
    }
    static uint32_t tell(void *cookie)
    {
        int fd = reinterpret_cast<int>(cookie);
        int64_t off = _lseeki64(fd, 0, SEEK_CUR);
        return std::min(off, 0xffffffffLL); // XXX
    }
    static int seek_abs(void *cookie, uint32_t pos)
    {
        int fd = reinterpret_cast<int>(cookie);
        return _lseeki64(fd, pos, SEEK_SET) >= 0 ? 0 : -1;
    }
    static int seek(void *cookie, int32_t off, int whence)
    {
        int fd = reinterpret_cast<int>(cookie);
        return _lseeki64(fd, off, whence) >= 0 ? 0 : -1;
    }
    static int pushback(void *cookie, int c)
    {
        int fd = reinterpret_cast<int>(cookie);
        _lseeki64(fd, -1, SEEK_CUR); // XXX
        return c;
    }
    static uint32_t size(void *cookie)
    {
        int fd = reinterpret_cast<int>(cookie);
        int64_t size = _filelengthi64(fd);
        return std::min(size, 0xffffffffLL); // XXX
    }
    static int seekable(void *cookie)
    {
        return util::is_seekable(reinterpret_cast<int>(cookie));
    }
};

WavpackSource::WavpackSource(const WavpackModule &module,
                             const std::wstring &path)
    : m_module(module)
{
    char error[0x100];
    /* wavpack doesn't copy WavpackStreamReader into their context, therefore
     * must be kept in the memory */
    static WavpackStreamReaderImpl reader;
    m_fp = win32::fopen(path, L"rb");
    try { m_cfp = win32::fopen(path + L"c", L"rb"); } catch(...) {}

    int flags = OPEN_TAGS | (m_cfp.get() ? OPEN_WVC : 0);
    void *ra = reinterpret_cast<void*>(fileno(m_fp.get()));
    void *rc =
        m_cfp.get() ? reinterpret_cast<void*>(fileno(m_cfp.get())) : 0;

    WavpackContext *wpc =
        m_module.OpenFileInputEx(&reader, ra, rc, error, flags, 0);
    if (!wpc)
        throw std::runtime_error("WavpackOpenFileInputEx() failed");
    m_wpc.reset(wpc, m_module.CloseFile);

    bool is_float = m_module.GetMode(wpc) & MODE_FLOAT;
    m_asbd = cautil::buildASBDForPCM2(m_module.GetSampleRate(wpc),
                                      m_module.GetNumChannels(wpc),
                                      m_module.GetBitsPerSample(wpc),
                                      32,
                                      is_float ? kAudioFormatFlagIsFloat 
                                         : kAudioFormatFlagIsSignedInteger);

    uint64_t duration = m_module.GetNumSamples(wpc);
    if (duration == 0xffffffff) duration = ~0ULL;
    m_length = duration;

    unsigned mask = m_module.GetChannelMask(wpc);
    chanmap::getChannels(mask, &m_chanmap, m_asbd.mChannelsPerFrame);

    fetchTags();
}

void WavpackSource::seekTo(int64_t count)
{
    if (!m_module.SeekSample(m_wpc.get(), static_cast<int32_t>(count)))
        throw std::runtime_error("WavpackSeekSample()");
}

int64_t WavpackSource::getPosition()
{
    return m_module.GetSampleIndex(m_wpc.get());
}

size_t WavpackSource::readSamples(void *buffer, size_t nsamples)
{
    /*
     * Wavpack sample is aligned to low at byte level,
     * but aligned to high at bit level inside of valid bytes.
     * 20bits sample is stored like the following:
     * <-- MSB ------------------ LSB ---->
     * 00000000 xxxxxxxx xxxxxxxx xxxx0000
     */
    int shifts = 32 - ((m_asbd.mBitsPerChannel + 7) & ~7);
    size_t total = 0;
    int *bp = static_cast<int*>(buffer);
    while (total < nsamples) {
        int rc = m_module.UnpackSamples(m_wpc.get(), bp, nsamples - total);
        if (rc <= 0)
            break;
        if (shifts) {
            /* align to MSB side */
            for (size_t i = 0; i < rc * m_asbd.mChannelsPerFrame; ++i)
                bp[i] <<= shifts; 
        }
        total += rc;
        bp += rc;
    }
    return total;
}

void WavpackSource::fetchTags()
{
    WavpackContext *wpc = m_wpc.get();

    int count = m_module.GetNumTagItems(wpc);
    std::map<std::string, std::string> vorbisComments;
    std::wstring cuesheet;
    std::map<uint32_t, std::wstring> tags;
    for (int i = 0; i < count; ++i) {
        int size = m_module.GetTagItemIndexed(wpc, i, 0, 0);
        std::vector<char> name(size + 1);
        m_module.GetTagItemIndexed(wpc, i, &name[0], name.size());
        size = m_module.GetTagItem(wpc, &name[0], 0, 0);
        std::vector<char> value(size + 1);
        m_module.GetTagItem(wpc, &name[0], &value[0], value.size());
        if (!strcasecmp(&name[0], "cuesheet"))
            cuesheet = strutil::us2w(&value[0]);
        else
            vorbisComments[&name[0]] = &value[0];
    }
    Vorbis::ConvertToItunesTags(vorbisComments, &m_tags);
    if (cuesheet.size()) {
        std::map<uint32_t, std::wstring> tags;
        Cue::CueSheetToChapters(cuesheet,
                                m_length / m_asbd.mSampleRate,
                                &m_chapters, &tags);
        std::map<uint32_t, std::wstring>::const_iterator it;
        for (it = tags.begin(); it != tags.end(); ++it)
            m_tags[it->first] = it->second;
    }
}
