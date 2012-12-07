#ifndef _RESAMPLER_H
#define _RESAMPLER_H

#include <libsoxrate.h>
#include "iointer.h"
#include "util.h"
#include "dl.h"

class SoxModule {
    DL m_dl;
public:
    SoxModule() {}
    SoxModule(const std::wstring &path);
    bool loaded() const { return m_dl.loaded(); }

    const char *(*version_string)();
    lsx_rate_t *(*rate_create)(unsigned, unsigned, unsigned);
    void (*rate_close)(lsx_rate_t *);
    int (*rate_config)(lsx_rate_t *, lsx_rate_config_e, ...);
    int (*rate_start)(lsx_rate_t *);
    size_t (*rate_process)(lsx_rate_t *, const float * const *, float **,
                           size_t *, size_t *, size_t, size_t);
    size_t (*rate_process_d)(lsx_rate_t *, const double * const *, double **,
                             size_t *, size_t *, size_t, size_t);
    lsx_fir_t *(*fir_create)(unsigned, double *, unsigned, unsigned, int);
    int (*fir_close)(lsx_fir_t *);
    int (*fir_start)(lsx_fir_t *);
    int (*fir_process)(lsx_fir_t *, const float * const *, float **,
                       size_t *, size_t *, size_t, size_t);
    int (*fir_process_d)(lsx_fir_t *, const double * const *, double **,
                         size_t *, size_t *, size_t, size_t);
    double *(*design_lpf)(double, double, double, double, int *, int, double);
    void (*free)(void*);
};

struct ISoxDSPEngine {
    virtual ~ISoxDSPEngine() {}
    virtual const AudioStreamBasicDescription &getSampleFormat() const = 0;
    virtual ssize_t process(const double * const *ibuf, double **obuf,
                            size_t *ilen, size_t *olen,
                            size_t istride, size_t ostride) = 0;
};

class SoxDSPProcessor: public FilterBase {
    int64_t m_position;
    uint64_t m_length;
    std::shared_ptr<ISoxDSPEngine> m_engine;
    std::vector<uint8_t> m_ibuffer;
    DecodeBuffer<double> m_buffer;
    AudioStreamBasicDescription m_asbd;
public:
    SoxDSPProcessor(const std::shared_ptr<ISoxDSPEngine> &engine,
                    const std::shared_ptr<ISource> &src);
    uint64_t length() const
    {
        return m_length;
    }
    const AudioStreamBasicDescription &getSampleFormat() const
    {
        return m_asbd;
    }
    size_t readSamples(void *buffer, size_t nsamples);
    int64_t getPosition() { return m_position; }
};

class SoxResampler: public ISoxDSPEngine {
    SoxModule m_module;
    std::shared_ptr<lsx_rate_t> m_processor;
    AudioStreamBasicDescription m_asbd;
    double m_factor;
public:
    SoxResampler(const SoxModule &module,
                 const AudioStreamBasicDescription &asbd,
                 uint32_t Fp, bool mt);
    const AudioStreamBasicDescription &getSampleFormat() const
    {
        return m_asbd;
    }
    ssize_t process(const double * const *ibuf, double **obuf, size_t *ilen,
                    size_t *olen, size_t istride, size_t ostride)
    {
        return m_module.rate_process_d(m_processor.get(), ibuf, obuf,
                                       ilen, olen, istride, ostride);
    }
};

class SoxLowpassFilter: public ISoxDSPEngine {
    SoxModule m_module;
    std::shared_ptr<lsx_fir_t> m_processor;
    AudioStreamBasicDescription m_asbd;
public:
    SoxLowpassFilter(const SoxModule &module,
                     const AudioStreamBasicDescription &asbd,
                     uint32_t rate, bool mt);
    const AudioStreamBasicDescription &getSampleFormat() const
    {
        return m_asbd;
    }
    ssize_t process(const double * const *ibuf, double **obuf, size_t *ilen,
                    size_t *olen, size_t istride, size_t ostride)
    {
        return m_module.fir_process_d(m_processor.get(), ibuf, obuf,
                                      ilen, olen, istride, ostride);
    }
};
#endif
