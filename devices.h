// Audio device helpers: names/ids, listing outputs, and switching the Windows default output.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static std::wstring DevName(IMMDevice* d) {
    std::wstring s = L"?";
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) s = v.pwszVal;
        PropVariantClear(&v);
    }
    return s;
}
static std::wstring DevId(IMMDevice* d) {
    std::wstring s; LPWSTR p = nullptr;
    if (SUCCEEDED(d->GetId(&p))) { s = p; CoTaskMemFree(p); }
    return s;
}

struct OutDev { std::wstring id, name; };
static std::vector<OutDev> EnumOutputs(std::wstring* defId = nullptr, std::wstring* defName = nullptr) {
    std::vector<OutDev> v;
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDeviceCollection> col; ComPtr<IMMDevice> def;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return v;
    if (defId) defId->clear();
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        if (defId) *defId = DevId(def.Get());
        if (defName) *defName = DevName(def.Get());
    }
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return v;
    UINT cnt = 0; col->GetCount(&cnt);
    for (UINT i = 0; i < cnt; i++) { ComPtr<IMMDevice> d; if (SUCCEEDED(col->Item(i, &d))) v.push_back({DevId(d.Get()), DevName(d.Get())}); }
    return v;
}

// Undocumented but long-stable Windows interface used by every "switch default device" tool.
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

static bool SetDefaultOutput(const std::wstring& id) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)pc.GetAddressOf())))
        return false;
    HRESULT a = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    HRESULT b = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    return SUCCEEDED(a) && SUCCEEDED(b);
}
