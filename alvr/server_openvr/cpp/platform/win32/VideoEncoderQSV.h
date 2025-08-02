#pragma once

#include "VideoEncoder.h"
#include "shared/d3drender.h"

#include <memory>
#include <vpl/mfxvideo.h>

// Video encoder for Intel Quick Sync Video via oneVPL.
class VideoEncoderQSV : public VideoEncoder {
public:
    VideoEncoderQSV(std::shared_ptr<CD3DRender> pD3DRender, int width, int height);
    ~VideoEncoderQSV();

    void Initialize() override;
    void Shutdown() override;

    void Transmit(
        ID3D11Texture2D* pTexture,
        uint64_t presentationTime,
        uint64_t targetTimestampNs,
        bool insertIDR
    ) override;

private:
    class D3D11FrameAllocator : public mfxFrameAllocator {
    public:
        D3D11FrameAllocator(ID3D11Device* device);

        static mfxStatus MFX_CDECL Alloc(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
        static mfxStatus MFX_CDECL Free(mfxHDL pthis, mfxFrameAllocResponse* response);
        static mfxStatus MFX_CDECL Lock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
        static mfxStatus MFX_CDECL Unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
        static mfxStatus MFX_CDECL GetHDL(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);

    private:
        ID3D11Device* m_device;
    };

    std::shared_ptr<CD3DRender> m_pD3DRender;

    mfxLoader m_loader = nullptr;
    mfxSession m_session = nullptr;
    mfxVideoParam m_params{};
    D3D11FrameAllocator* m_allocator = nullptr;

    int m_codec;
    int m_renderWidth;
    int m_renderHeight;
};

