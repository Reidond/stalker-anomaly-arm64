#pragma once
#ifdef XR_PLATFORM_APPLE
// Apple platforms have no IMMNotificationClient. Device changes come from OpenAL Soft (ALC_SOFT_system_events: macOS
// default output device, device list) and, on iOS, from AVAudioSession (route changes and interruptions). The flags
// it sets are the same ones the WASAPI client sets. Implemented in SoundRender_Apple.cpp.
class CNotificationClient
{
public:
    CNotificationClient();
    ~CNotificationClient();

private:
    bool m_bSystemEvents;
};
#else
#include <mmdeviceapi.h>
#include <endpointvolume.h>

class CNotificationClient : 
    public IMMNotificationClient 
{
public:
    CNotificationClient();
    ~CNotificationClient();

    // IUnknown methods
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    ULONG STDMETHODCALLTYPE Release() {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (0 == ulRef) {
            delete this;
        }
        return ulRef;
    }

    // IMMNotificationClient methods
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId);
    STDMETHOD(OnDeviceRemoved)(LPCWSTR pwstrDeviceId);
    STDMETHOD(OnDeviceAdded)(LPCWSTR pwstrDeviceId);
    STDMETHOD(OnDeviceStateChanged)(LPCWSTR pwstrDeviceId, DWORD dwNewState);
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR pwstrDeviceId, const PROPERTYKEY key);

private:
    bool Start();
    void Close();

private:
    LONG m_cRef;
    IMMDeviceEnumerator* m_pEnumerator;
    bool m_bComInitialized;
};
#endif // XR_PLATFORM_APPLE
