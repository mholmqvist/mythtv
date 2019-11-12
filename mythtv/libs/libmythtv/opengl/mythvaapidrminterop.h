#ifndef MYTHVAAPIDRMINTEROP_H
#define MYTHVAAPIDRMINTEROP_H

// MythTV
#include "mythvaapiinterop.h"

class MythDRMPRIMEInterop;
struct AVDRMFrameDescriptor;

class MythVAAPIInteropDRM : public MythVAAPIInterop
{
  public:
    MythVAAPIInteropDRM(MythRenderOpenGL *Context);
    ~MythVAAPIInteropDRM() override;
    vector<MythVideoTexture*> Acquire(MythRenderOpenGL *Context,
                                      VideoColourSpace *ColourSpace,
                                      VideoFrame *Frame,FrameScanType Scan) final override;
    static bool    IsSupported(MythRenderOpenGL *Context);
    void           DeleteTextures(void) override;

  protected:
    void           DestroyDeinterlacer(void) override;
    void           PostInitDeinterlacer(void) override;

  private:
    void           CreateDRMBuffers(VideoFrameType Format,
                                    vector<MythVideoTexture*> Textures,
                                    uintptr_t Handle, VAImage &Image);
    VideoFrameType VATypeToMythType(uint32_t Fourcc);
    void           CleanupReferenceFrames(void);
    void           RotateReferenceFrames(AVBufferRef *Buffer);
    vector<MythVideoTexture*> GetReferenceFrames(void);

  private:
    QFile                 m_drmFile         { };
    QVector<AVBufferRef*> m_referenceFrames { };


    vector<MythVideoTexture*> AcquirePrime(VASurfaceID Id, MythRenderOpenGL *Context,
                                           VideoColourSpace *ColourSpace,
                                           VideoFrame *Frame,FrameScanType Scan);
    void                      CleanupDRMPRIME(void);
    MythDRMPRIMEInterop*      m_drmPrimeInterop { nullptr };
    QHash<unsigned long long, AVDRMFrameDescriptor*> m_drmFrames { };
};

#endif // MYTHVAAPIDRMINTEROP_H