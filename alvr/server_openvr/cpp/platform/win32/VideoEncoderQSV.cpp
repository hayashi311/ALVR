#include "VideoEncoderQSV.h"

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/Utils.h"

#include <dxgi1_2.h>
#include <vector>

VideoEncoderQSV::VideoEncoderQSV(std::shared_ptr<CD3DRender> pD3DRender, int width, int height)
    : m_pD3DRender(pD3DRender)
    , m_codec(Settings::Instance().m_codec)
    , m_renderWidth(width)
    , m_renderHeight(height) {
}

VideoEncoderQSV::~VideoEncoderQSV() { }

VideoEncoderQSV::D3D11FrameAllocator::D3D11FrameAllocator(ID3D11Device* device)
    : m_device(device) {
    Alloc = &D3D11FrameAllocator::Alloc;
    Free = &D3D11FrameAllocator::Free;
    Lock = &D3D11FrameAllocator::Lock;
    Unlock = &D3D11FrameAllocator::Unlock;
    GetHDL = &D3D11FrameAllocator::GetHDL;
}

mfxStatus MFX_CDECL VideoEncoderQSV::D3D11FrameAllocator::Alloc(
    mfxHDL, mfxFrameAllocRequest*, mfxFrameAllocResponse* response
) {
    response->mids = nullptr;
    response->NumFrameActual = 0;
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL VideoEncoderQSV::D3D11FrameAllocator::Free(
    mfxHDL, mfxFrameAllocResponse*
) {
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL VideoEncoderQSV::D3D11FrameAllocator::Lock(
    mfxHDL, mfxMemId, mfxFrameData*
) {
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL VideoEncoderQSV::D3D11FrameAllocator::Unlock(
    mfxHDL, mfxMemId, mfxFrameData*
) {
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL VideoEncoderQSV::D3D11FrameAllocator::GetHDL(
    mfxHDL, mfxMemId mid, mfxHDL* handle
) {
    *handle = mid;
    return MFX_ERR_NONE;
}

void VideoEncoderQSV::Initialize() {
    Debug("Initializing QSV encoder.\n");

    IDXGIDevice* dxgiDevice = nullptr;
    m_pD3DRender->GetDevice()->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    adapter->Release();
    dxgiDevice->Release();
    if (desc.VendorId != 0x8086) {
        throw MakeException("QSV encoder requires Intel GPU.");
    }

    m_loader = MFXLoad();
    if (!m_loader) {
        throw MakeException("MFXLoad failed");
    }

    mfxConfig cfg = MFXCreateConfig(m_loader);
    mfxVariant impl_type{};
    impl_type.Type = MFX_VARIANT_TYPE_U32;
    impl_type.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    MFXSetConfigFilterProperty(cfg, (mfxU8*)"mfxImplDescription.Impl", impl_type);

    mfxStatus sts = MFXCreateSession(m_loader, 0, &m_session);
    if (sts < MFX_ERR_NONE) {
        throw MakeException("MFXCreateSession failed: %d", sts);
    }

    m_allocator = new D3D11FrameAllocator(m_pD3DRender->GetDevice());
    mfxFrameAllocator* alloc = m_allocator;
    sts = MFXVideoCORE_SetFrameAllocator(m_session, alloc);
    if (sts < MFX_ERR_NONE) {
        throw MakeException("SetFrameAllocator failed: %d", sts);
    }

    m_params = {};
    m_params.mfx.CodecId = m_codec == ALVR_CODEC_HEVC
        ? MFX_CODEC_HEVC
        : (m_codec == ALVR_CODEC_AV1 ? MFX_CODEC_AV1 : MFX_CODEC_AVC);
    m_params.mfx.TargetUsage = Settings::Instance().m_qsvTargetUsage;
    m_params.mfx.RateControlMethod = Settings::Instance().m_qsvRateControlMode;
    m_params.mfx.TargetKbps = 30000;
    m_params.mfx.FrameInfo.FrameRateExtN = Settings::Instance().m_refreshRate;
    m_params.mfx.FrameInfo.FrameRateExtD = 1;
    m_params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    m_params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    m_params.mfx.FrameInfo.CropW = m_renderWidth;
    m_params.mfx.FrameInfo.CropH = m_renderHeight;
    m_params.mfx.FrameInfo.Width = m_renderWidth;
    m_params.mfx.FrameInfo.Height = m_renderHeight;
    m_params.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;

    sts = MFXVideoENCODE_Init(m_session, &m_params);
    if (sts < MFX_ERR_NONE) {
        throw MakeException("QSV Init failed: %d", sts);
    }

    Debug("QSV encoder initialized.\n");
}

void VideoEncoderQSV::Shutdown() {
    if (m_session) {
        MFXVideoENCODE_Close(m_session);
        MFXClose(m_session);
        MFXUnload(m_loader);
        m_session = nullptr;
        m_loader = nullptr;
    }
    delete m_allocator;
    m_allocator = nullptr;
    Debug("VideoEncoderQSV::Shutdown\n");
}

void VideoEncoderQSV::Transmit(
    ID3D11Texture2D* pTexture,
    uint64_t presentationTime,
    uint64_t targetTimestampNs,
    bool insertIDR
) {
    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    mfxBitstream bs{};
    bs.MaxLength = (mfxU32)buffer.size();
    bs.Data = buffer.data();

    mfxFrameSurface1 surface{};
    surface.Info = m_params.mfx.FrameInfo;
    surface.Data.MemId = (mfxMemId)pTexture;

    mfxSyncPoint syncp = nullptr;
    mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(m_session, nullptr, &surface, &bs, &syncp);
    if (sts == MFX_ERR_NONE) {
        MFXVideoCORE_SyncOperation(m_session, syncp, 10000);
        if (bs.DataLength > 0) {
            ParseFrameNals(
                m_codec,
                bs.Data + bs.DataOffset,
                bs.DataLength,
                targetTimestampNs,
                insertIDR
            );
        }
    } else if (sts != MFX_ERR_MORE_DATA) {
        Error("QSV EncodeFrameAsync failed: %d\n", sts);
    }
}

