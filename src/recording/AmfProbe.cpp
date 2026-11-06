#include "recording/AmfProbe.h"

#include <Windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

#ifndef FASTRECORD_HAS_AMF
#define FASTRECORD_HAS_AMF 0
#endif

#if FASTRECORD_HAS_AMF
#include <core/Factory.h>
#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderVCE.h>
#endif

namespace fastrecord::recording {

namespace {

#if FASTRECORD_HAS_AMF

class Library final {
public:
    Library() {
#if defined(_WIN64)
        handle = LoadLibraryW(L"amfrt64.dll");
        if (handle == nullptr) {
            handle = LoadLibraryW(L"amfrtlt64.dll");
        }
#else
        handle = LoadLibraryW(L"amfrt32.dll");
        if (handle == nullptr) {
            handle = LoadLibraryW(L"amfrtlt32.dll");
        }
#endif
    }

    ~Library() {
        if (handle != nullptr) {
            FreeLibrary(handle);
        }
    }

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    HMODULE handle{nullptr};
};

bool createD3D11Device(Microsoft::WRL::ComPtr<ID3D11Device>& device) {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL createdLevel{};
    const HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        &createdLevel,
        nullptr);

    return SUCCEEDED(result) && device != nullptr;
}

#endif

} // namespace

AmfProbeResult probeAmf() {
    AmfProbeResult result{};

#if FASTRECORD_HAS_AMF
    result.sdkBindingsCompiled = true;

    Library library;
    if (library.handle == nullptr) {
        result.description = L"AMF não disponível: runtime AMD não foi encontrada.";
        return result;
    }
    result.runtimeLibraryFound = true;

    const auto init = reinterpret_cast<AMFInit_Fn>(
        GetProcAddress(library.handle, AMF_INIT_FUNCTION_NAME));
    if (init == nullptr) {
        result.description = L"AMF não disponível: função AMFInit não foi encontrada.";
        return result;
    }

    amf::AMFFactory* factory = nullptr;
    if (init(AMF_FULL_VERSION, &factory) != AMF_OK || factory == nullptr) {
        result.description = L"AMF não disponível: falha ao inicializar o runtime.";
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (!createD3D11Device(device)) {
        result.description = L"AMF não testado: não foi possível criar um dispositivo Direct3D 11.";
        return result;
    }

    amf::AMFContextPtr context;
    if (factory->CreateContext(&context) != AMF_OK || context == nullptr) {
        result.description = L"AMF não disponível: falha ao criar o contexto.";
        return result;
    }

    if (context->InitDX11(device.Get()) != AMF_OK) {
        result.description = L"AMF não disponível: falha ao inicializar o contexto Direct3D 11.";
        context->Terminate();
        return result;
    }
    result.contextInitialized = true;

    amf::AMFComponentPtr encoder;
    if (factory->CreateComponent(context, AMFVideoEncoderVCE_AVC, &encoder) == AMF_OK &&
        encoder != nullptr) {
        if (encoder->Init(amf::AMF_SURFACE_NV12, 1920, 1080) == AMF_OK) {
            result.h264Supported = true;
        }
        encoder->Terminate();
    }

    // AV1 hardware encoding starts with RDNA 3. 1280x720 fits the 64x16
    // block alignment of every generation, so the probe does not depend on
    // the alignment mode; the backend checks the chosen size when recording.
    amf::AMFComponentPtr av1Encoder;
    if (factory->CreateComponent(context, AMFVideoEncoder_AV1, &av1Encoder) == AMF_OK &&
        av1Encoder != nullptr) {
        if (av1Encoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_USAGE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY)) == AMF_OK &&
            av1Encoder->Init(amf::AMF_SURFACE_NV12, 1280, 720) == AMF_OK) {
            result.av1Supported = true;
        }
        av1Encoder->Terminate();
    }

    context->Terminate();

    if (result.h264Supported && result.av1Supported) {
        result.description = L"AMF disponível para H.264 e AV1 via Direct3D 11.";
    } else if (result.h264Supported) {
        result.description = L"AMF disponível para H.264 via Direct3D 11; esta GPU não codifica AV1.";
    } else if (result.av1Supported) {
        result.description = L"AMF disponível para AV1 via Direct3D 11; o encoder H.264 não foi inicializado.";
    } else {
        result.description = L"AMF encontrado, mas nenhum encoder de vídeo foi inicializado.";
    }
#else
    HMODULE library = nullptr;
#if defined(_WIN64)
    library = LoadLibraryW(L"amfrt64.dll");
    if (library == nullptr) {
        library = LoadLibraryW(L"amfrtlt64.dll");
    }
#else
    library = LoadLibraryW(L"amfrt32.dll");
    if (library == nullptr) {
        library = LoadLibraryW(L"amfrtlt32.dll");
    }
#endif
    result.runtimeLibraryFound = library != nullptr;
    if (library != nullptr) {
        FreeLibrary(library);
    }

    result.description = result.runtimeLibraryFound
        ? L"Runtime AMF encontrada; configure o AMD Advanced Media Framework SDK para a sonda completa."
        : L"AMF não detectado; o fallback será usado.";
#endif

    return result;
}

} // namespace fastrecord::recording
