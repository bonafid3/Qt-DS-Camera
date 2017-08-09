#include <QDebug>
#include <QWidget>
#include <QFile>
#include <QByteArray>
#include <QtMultimedia/qabstractvideobuffer.h>
#include <QtMultimedia/qvideosurfaceformat.h>
#include <QVideoSurfaceFormat>
#include "dscamerasession.h"

#include <opencv2/imgproc/imgproc.hpp>

QT_BEGIN_NAMESPACE

// If frames come in quicker than we display them, we allow the queue to build
// up to this number before we start dropping them.
const int LIMIT_FRAME = 5;

namespace {
// DirectShow helper implementation
void _FreeMediaType(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat != 0) {
        CoTaskMemFree((PVOID)mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = NULL;
    }
    if (mt.pUnk != NULL) {
        // pUnk should not be used.
        mt.pUnk->Release();
        mt.pUnk = NULL;
    }
}

} // end namespace

class SampleGrabberCallbackPrivate : public ISampleGrabberCB
{
public:
    STDMETHODIMP_(ULONG) AddRef() { return 1; }
    STDMETHODIMP_(ULONG) Release() { return 2; }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppvObject)
    {
        if (NULL == ppvObject) 
            return E_POINTER;
        if (riid == IID_IUnknown /*__uuidof(IUnknown) */ ) {
            *ppvObject = static_cast<IUnknown*>(this);
            return S_OK;
        }
        if (riid == IID_ISampleGrabberCB /*__uuidof(ISampleGrabberCB)*/ ) {
            *ppvObject = static_cast<ISampleGrabberCB*>(this);
            return S_OK;
        }
        return E_NOTIMPL;
    }

    STDMETHODIMP SampleCB(double Time, IMediaSample *pSample)
    {
        Q_UNUSED(Time)
        Q_UNUSED(pSample)
        return E_NOTIMPL;
    }

    /*
    STDMETHODIMP BufferCB(double Time, BYTE *pBuffer, long BufferLen)
    {
        qDebug() << "buffercb" << active << toggle;
        if (!cs || active) {
            return S_OK;
        }

        if ((cs->StillMediaType.majortype != MEDIATYPE_Video) ||
                (cs->StillMediaType.formattype != FORMAT_VideoInfo) ||
                (cs->StillMediaType.cbFormat < sizeof(VIDEOINFOHEADER))) {
            return VFW_E_INVALIDMEDIATYPE;
        }

        active = true;

        toggle = !toggle;

        if(toggle) {
            active = false;
            return S_OK;
        }

        bool check = false;
        cs->mutex.lock();

        qDebug() << "frames" << cs->frames.size();

        if (cs->frames.size() > LIMIT_FRAME) {
            check = true;
        }

        if (check) {
            cs->mutex.unlock();
            // Frames building up. We're going to drop some here
            Sleep(100);
            active = false;
            return S_OK;
        }
        cs->mutex.unlock();

        unsigned char* vidData = new unsigned char[BufferLen];
        memcpy(vidData, pBuffer, BufferLen);

        cs->mutex.lock();

        video_buffer* buf = new video_buffer;
        buf->buffer = vidData;
        buf->length = BufferLen;
        buf->time   = (qint64)Time;

        cs->frames.append(buf);

        cs->mutex.unlock();

        QMetaObject::invokeMethod(cs, "captureFrame", Qt::QueuedConnection);

        active = false;

        return S_OK;
    }
    */

    STDMETHODIMP BufferCB(double Time, BYTE *pBuffer, long BufferLen)
    {
        if (!cs) {
            return S_OK;
        }

        if ((cs->StillMediaType.majortype != MEDIATYPE_Video) ||
                (cs->StillMediaType.formattype != FORMAT_VideoInfo) ||
                (cs->StillMediaType.cbFormat < sizeof(VIDEOINFOHEADER))) {
            return VFW_E_INVALIDMEDIATYPE;
        }

        cs->mutex.lock();

        /*
        VIDEOINFOHEADER *pvi = NULL;
        pvi = (VIDEOINFOHEADER*)cs->StillMediaType.pbFormat;
        qDebug() << "width, height:" << pvi->bmiHeader.biWidth << pvi->bmiHeader.biHeight;
        */

        qDebug() << "frame time" << Time;

        if(cs->mCaptureNextFrame)
        {
            cs->mCaptureNextFrame = false;

            unsigned char* vidData = new unsigned char[BufferLen];
            memcpy(vidData, pBuffer, BufferLen);

            video_buffer* buf = new video_buffer;
            buf->buffer = vidData;
            buf->length = BufferLen;
            buf->time   = (qint64)Time;

            cs->frames.append(buf);

            QMetaObject::invokeMethod(cs, "captureFrame", Qt::QueuedConnection);
        }
        //else
        //{
            //qDebug() << "dropping frame" << Time;
        //}

        cs->mutex.unlock();

        return S_OK;
    }

    DSCameraSession* cs;
    bool active;
    bool toggle;
};

DSCameraSession::DSCameraSession(const QByteArray &device, QObject *parent)
    : QObject(parent)
      ,m_currentImageId(0), mCaptureNextFrame(true)
{
    pBuild = NULL;
    pGraph = NULL;

    pCap = NULL;
    pNullRenderer = NULL;
    pIntermediateFilter = NULL;
    pSG_Filter = NULL;
    pSG = NULL;

    opened = false;
    available = false;
    m_state = QCamera::UnloadedState;
    m_device = "default";

    enumerateDevices(&m_devices, &m_descriptions);

    if(m_devices.contains(device))
        m_device = device;

    StillCapCB = new SampleGrabberCallbackPrivate;
    StillCapCB->cs = this;
    StillCapCB->active = false;
    StillCapCB->toggle = false;

    m_surface = 0;

    graph = createFilterGraph();
    active = false;
}

DSCameraSession::~DSCameraSession()
{
    if (opened) {
        closeStream();
    }

    CoUninitialize();

    SAFE_RELEASE(pCap);
    SAFE_RELEASE(pSG_Filter);
    SAFE_RELEASE(pGraph);
    SAFE_RELEASE(pBuild);

    if (StillCapCB) {
        delete StillCapCB;
    }
}

int DSCameraSession::captureImage(const QString &fileName)
{
    if (!active) {
        startStream();
    }

    return m_currentImageId;
}

void DSCameraSession::setSurface(QAbstractVideoSurface* surface)
{
    m_surface = surface;
}

bool DSCameraSession::deviceReady()
{
    return available;
}

bool DSCameraSession::pictureInProgress()
{
    return m_snapshot.isEmpty();
}

QList<QVideoSurfaceFormat> DSCameraSession::supportedFormats()
{
    return m_formats;
}

void DSCameraSession::setFormat(QVideoSurfaceFormat format)
{
    actualFormat = format;
}

QVideoSurfaceFormat DSCameraSession::format()
{
    return actualFormat;
}

qint64 DSCameraSession::position() const
{
    return timeStamp.elapsed();
}

int DSCameraSession::state() const
{
    return int(m_state);
}

void DSCameraSession::pause()
{
    suspendStream();
}

void DSCameraSession::stop()
{
    if(!opened) {
        return;
    }

    stopStream();
    opened = false;
}

void DSCameraSession::captureFrame()
{
    if(frames.count())
    {
        cv::Mat flipped, dst;
        VIDEOINFOHEADER *pvi = NULL;

        if(StillMediaType.subtype == MEDIASUBTYPE_RGB24) {
            mutex.lock();

            video_buffer* buf = frames.takeFirst();

            pvi = (VIDEOINFOHEADER*)StillMediaType.pbFormat;

            cv::Mat image(cv::Size(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), CV_8UC3, buf->buffer);
            cv::flip(image, flipped, 0);
            cv::cvtColor(flipped, dst, CV_BGR2RGB);

            delete buf->buffer;
            delete buf;

            mutex.unlock();

            emit cvFrameCaptured(dst);
        }
        else if(StillMediaType.subtype == MEDIASUBTYPE_YUY2 || StillMediaType.subtype == MEDIASUBTYPE_YUYV)
        {
            mutex.lock();

            pvi = (VIDEOINFOHEADER*)StillMediaType.pbFormat;

            quint8 *imgbuf = new quint8[pvi->bmiHeader.biWidth * pvi->bmiHeader.biHeight * 3];

            cv::Mat image(cv::Size(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), CV_8UC3, imgbuf);

            quint8 *pp;

            video_buffer* buf = frames.takeFirst();

            int j=0;
            for(int i=0; i<buf->length; i+=4)
            {
                pp = reinterpret_cast<quint8*>(buf->buffer+i);
                *reinterpret_cast<quint32*>(image.data+j) = yuv2rgb(pp[0], pp[1], pp[3]);
                *reinterpret_cast<quint32*>(image.data+j+3) = yuv2rgb(pp[2], pp[1], pp[3]);
                j+=6;
            }

            cv::flip(image, image, 0);

            delete buf->buffer;
            delete buf;

            mutex.unlock();

            emit cvFrameCaptured(dst);
        }

    }
}

int DSCameraSession::yuv2rgb(int y, int u, int v)
{
   quint32 pixel32;
   quint8 *pixel = (unsigned char *)&pixel32;
   qint32 r, g, b;

   r = y + (1.370705 * (v-128));
   g = y - (0.698001 * (v-128)) - (0.337633 * (u-128));
   b = y + (1.732446 * (u-128));

   r = r>255 ? 255 : r<0 ? 0 : r;
   g = g>255 ? 255 : g<0 ? 0 : g;
   b = b>255 ? 255 : b<0 ? 0 : b;

   pixel[0] = r * 220 / 256;
   pixel[1] = g * 220 / 256;
   pixel[2] = b * 220 / 256;
   pixel[3] = 0;

   return pixel32;
}

HRESULT DSCameraSession::getFilterAndPinInfo(IBaseFilter *pFilter)
{
    HRESULT hr;
    PIN_INFO pinInfo;
    FILTER_INFO filterInfo;

    IPin *pPin = 0;
    IEnumPins *pEnum = 0;

    pFilter->QueryFilterInfo(&filterInfo);
    qDebug() << "filter: " << QString::fromWCharArray(filterInfo.achName);

    if(FAILED(hr = pFilter->EnumPins(&pEnum)))
        return hr;

    while(pEnum->Next(1, &pPin, NULL) == S_OK) {
        pPin->QueryPinInfo(&pinInfo);
        qDebug() << QString::fromWCharArray(pinInfo.achName) << "pin dir: " << pinInfo.dir;
    }

    pEnum->Release();
    return S_OK;
}

HRESULT DSCameraSession::getPin(IBaseFilter *pFilter, QString type, PIN_DIRECTION PinDir, IPin **ppPin)
{
    HRESULT hr;
    *ppPin = 0;
    IEnumPins *pEnum = 0;
    IPin *pPin = 0;
    PIN_INFO pInfo;

    if(FAILED(hr = pFilter->EnumPins(&pEnum)))
        return hr;

    while(pEnum->Next(1, &pPin, NULL) == S_OK)
    {
        pPin->QueryPinInfo(&pInfo);

        qDebug() << QString::fromWCharArray(pInfo.achName);

        if(type != "" && type != QString::fromWCharArray(pInfo.achName))
            continue;

        if(pInfo.dir == PinDir) {
            pEnum->Release();
            if(pInfo.pFilter) pInfo.pFilter->Release();
            *ppPin = pPin;
            return S_OK;
        }
    }
    pEnum->Release();
    return E_FAIL;
}

bool DSCameraSession::createFilterGraph()
{
    // Previously containered in <qedit.h>.
    static const IID iID_ISampleGrabber = { 0x6B652FFF, 0x11FE, 0x4fce, { 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F } };
    static const CLSID cLSID_SampleGrabber = { 0xC1F400A0, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

    HRESULT hr;
    IMoniker* pMoniker = NULL;
    ICreateDevEnum* pDevEnum = NULL;
    IEnumMoniker* pEnum = NULL;

    CoInitialize(NULL);

    // Create the filter graph
    if(FAILED(hr = CoCreateInstance(CLSID_FilterGraph,NULL,CLSCTX_INPROC,
            IID_IGraphBuilder, (void**)&pGraph))) {
        qWarning() << "failed to create filter graph";
        return false;
    }

    // Create the capture graph builder
    if(FAILED(hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC,
                          IID_ICaptureGraphBuilder2, (void**)&pBuild))) {
        qWarning() << "failed to create graph builder";
        return false;
    }

    // Attach the filter graph to the capture graph
    if(FAILED(hr = pBuild->SetFiltergraph(pGraph))) {
        qWarning() << "failed to connect capture graph and filter graph";
        return false;
    }

    if(FAILED(hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC, IID_IBaseFilter, (void**)&pNullRenderer))) {
        qDebug() << "Cannot create null renderer" << QString::number(hr, 16);
        return false;
    }

    // Find the Capture device
    if(SUCCEEDED(hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL,
                          CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                          reinterpret_cast<void**>(&pDevEnum))))
    {
        // Create an enumerator for the video capture category
        hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
        pDevEnum->Release();
        if (S_OK == hr) {
            pEnum->Reset();
            IMalloc *mallocInterface = 0;
            CoGetMalloc(1, (LPMALLOC*)&mallocInterface);
            //go through and find all video capture devices
            while (pEnum->Next(1, &pMoniker, NULL) == S_OK) {
                BSTR strName = 0;
                hr = pMoniker->GetDisplayName(NULL, NULL, &strName);
                if (SUCCEEDED(hr)) {
                    QString output = QString::fromWCharArray(strName);
                    mallocInterface->Free(strName);
                    if (m_device.contains(output.toUtf8().constData())) {
                        hr = pMoniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&pCap); // capture filter by name
                        if (SUCCEEDED(hr)) {
                            pMoniker->Release();
                            break;
                        }
                    }
                }
                pMoniker->Release();
            }
            mallocInterface->Release();
            if (NULL == pCap)
            {
                if (m_device.contains("default"))
                {
                    pEnum->Reset();
                    // still have to loop to discard bind to storage failure case
                    while (pEnum->Next(1, &pMoniker, NULL) == S_OK) {
                        IPropertyBag *pPropBag = 0;

                        hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag, (void**)(&pPropBag));
                        if (FAILED(hr)) {
                            pMoniker->Release();
                            continue; // Don't panic yet
                        }

                        // No need to get the description, just grab it

                        hr = pMoniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&pCap); // first available capture filter
                        pPropBag->Release();
                        pMoniker->Release();
                        if (SUCCEEDED(hr)) {
                            break; // done, stop looping through
                        }
                        else
                        {
                            qWarning() << "Object bind failed";
                        }
                    }
                }
            }
            pEnum->Release();
        }
    }

    // Sample grabber filter
    if(FAILED(hr = CoCreateInstance(cLSID_SampleGrabber, NULL, CLSCTX_INPROC,
                          IID_IBaseFilter, (void**)&pSG_Filter)))
    {
        qWarning() << "failed to create sample grabber";
        return false;
    }

    if(FAILED(hr = pSG_Filter->QueryInterface(iID_ISampleGrabber, (void**)&pSG))) {
        qWarning() << "failed to get sample grabber";
        return false;
    }

    pSG->SetOneShot(FALSE);
    pSG->SetBufferSamples(TRUE);
    pSG->SetCallback(StillCapCB, 1); //0=SampleCB, 1=BufferCB

    updateProperties();
    CoUninitialize();
    return true;
}

void DSCameraSession::enumerateDevices(QList<QByteArray> *devices, QStringList *descriptions)
{
    devices->clear();
    descriptions->clear();

    HRESULT hr;
    CoInitialize(NULL);
    ICreateDevEnum* pDevEnum = NULL;
    IEnumMoniker* pEnum = NULL;
    // Create the System device enumerator
    if(SUCCEEDED(hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL,
            CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
            reinterpret_cast<void**>(&pDevEnum))))
    {
        // Create the enumerator for the video capture category
        if(SUCCEEDED(hr = pDevEnum->CreateClassEnumerator(
                CLSID_VideoInputDeviceCategory, &pEnum, 0)))
        {
            pEnum->Reset();
            // go through and find all video capture devices
            IMoniker* pMoniker = NULL;
            IMalloc *mallocInterface = 0;
            CoGetMalloc(1, (LPMALLOC*)&mallocInterface);
            while (pEnum->Next(1, &pMoniker, NULL) == S_OK) {
                BSTR strName = 0;
                if(SUCCEEDED(hr = pMoniker->GetDisplayName(NULL, NULL, &strName))) {
                    QString output(QString::fromWCharArray(strName));
                    mallocInterface->Free(strName);
                    devices->append(output.toUtf8().constData());

                    IPropertyBag *pPropBag;
                    if(SUCCEEDED(hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag, (void**)(&pPropBag)))) {
                        // Find the description
                        VARIANT varName;
                        varName.vt = VT_BSTR;
                        if(SUCCEEDED(hr = pPropBag->Read(L"FriendlyName", &varName, 0)))
                            output = QString::fromWCharArray(varName.bstrVal);
                        pPropBag->Release();
                    }
                    descriptions->append(output);
                }
                pMoniker->Release();
            }
            mallocInterface->Release();
            pEnum->Release();
        }
        pDevEnum->Release();
    }
    CoUninitialize();
}

QList<QByteArray> DSCameraSession::availableDevices()
{
    QList<QByteArray> devices;
    QStringList descriptions;
    enumerateDevices(&devices, &descriptions);
    return devices;
}

QString DSCameraSession::deviceDescription(const QByteArray &device)
{
    QList<QByteArray> devices;
    QStringList descriptions;
    enumerateDevices(&devices, &descriptions);
    for(int i=0; i<devices.size(); ++i)
    {
        if(devices[i].contains(device))
        {
            return descriptions[i];
        }
    }
    return QString();
}

void DSCameraSession::updateProperties()
{
    HRESULT hr;
    AM_MEDIA_TYPE *pmt = NULL;
    VIDEOINFOHEADER *pvi = NULL;
    VIDEO_STREAM_CONFIG_CAPS scc;
    IAMStreamConfig* pConfig = 0;

    if(FAILED(hr = pBuild->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCap,
                               IID_IAMStreamConfig, (void**)&pConfig)))
    {
        qWarning()<<"failed to get config on capture device";
        return;
    }

    int iCount;
    int iSize;
    if(FAILED(hr = pConfig->GetNumberOfCapabilities(&iCount, &iSize))) {
        qWarning() << "failed to get capabilities";
        return;
    }

    m_formats.clear();

    for (int iIndex = 0; iIndex < iCount; iIndex++)
    {
        if(SUCCEEDED(hr = pConfig->GetStreamCaps(iIndex, &pmt, reinterpret_cast<BYTE*>(&scc)))) {
            pvi = (VIDEOINFOHEADER*)pmt->pbFormat;
            if ((pmt->majortype == MEDIATYPE_Video) &&
                    (pmt->formattype == FORMAT_VideoInfo))
            {
                // save each format available from the camera
                if(pmt->subtype == MEDIASUBTYPE_RGB24) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_RGB24);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_RGB32) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_RGB32);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_YUY2) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_YUYV);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_MJPG) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_User);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_I420) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_YUV420P);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_RGB555) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_RGB555);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_UYVY) {
                    QVideoSurfaceFormat sfmt(QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight), QVideoFrame::Format_UYVY);
                    sfmt.setFrameRate(1000 / (pvi->AvgTimePerFrame / 10000));
                    m_formats.append(sfmt);
                } else if(pmt->subtype == MEDIASUBTYPE_H263) {
                    qWarning() << "QVideoFrame does not have format for H263, frameSize: " << QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight);
                } else {
                    qWarning() << "UNKNOWN FORMAT: " << pmt->subtype.Data1 << " frameSize: " << QSize(pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight);
                }
            }
        }
    }
    pConfig->Release();
}

bool DSCameraSession::getCameraControlPropertyRange(tagCameraControlProperty property, tRange &range)
{
    HRESULT hr;
    IAMCameraControl *pCameraControl = 0;
    if(FAILED(hr = pCap->QueryInterface(IID_IAMCameraControl, (void **)&pCameraControl))) {
        qWarning() << "Failed to query camera control interface" << QString::number(hr, 16);
        return false;
    }

    if(FAILED(hr = pCameraControl->GetRange(property,
                                &range.min,
                                &range.max,
                                &range.steppingDelta,
                                &range.defaultValue,
                                &range.capsFlags)))
    {
        qWarning() << "Failed to get CameraControl property range" << property << QString::number(hr, 16);
        return false;
    }

    return true;
}

bool DSCameraSession::setCameraPropertyValue(tagCameraControlProperty property, int value, CameraControlFlags flag)
{
    HRESULT hr;
    IAMCameraControl *pCameraControl = 0;
    if(FAILED(hr = pCap->QueryInterface(IID_IAMCameraControl, (void **)&pCameraControl))) {
        qWarning() << "Failed to query camera control interface" << QString::number(hr, 16);
        return false;
    }
    if(FAILED(hr = pCameraControl->Set(property, // property
                           value, // value
                           flag)))
    {
        qWarning() << "Failed to set CameraControl property value" << QString::number(hr, 16);
        return false;
    }
    qDebug() << "setting val" << value << "success";
    return true;
}

bool DSCameraSession::getVideoProcAmpPropertyRange(tagVideoProcAmpProperty property, tRange &range)
{
    HRESULT hr;
    IAMVideoProcAmp *pVideoProcAmp;
    if(FAILED(hr = pCap->QueryInterface(IID_IAMVideoProcAmp, (void **)&pVideoProcAmp))) {
        qWarning() << "failed to query video proc amp interface";
        return false;
    }

    if(FAILED(hr = pVideoProcAmp->GetRange(property,
                                &range.min,
                                &range.max,
                                &range.steppingDelta,
                                &range.defaultValue,
                                &range.capsFlags)))
    {
        qWarning() << "Failed to get VideoProcAmp property range" << property << QString::number(hr, 16);
        return false;
    }

    return true;
}

bool DSCameraSession::setVideoProcAmpPropertyValue(tagVideoProcAmpProperty property, int value, tagVideoProcAmpFlags flag)
{
    HRESULT hr;
    IAMVideoProcAmp *pVideoProcAmp;
    if(FAILED(hr = pCap->QueryInterface(IID_IAMVideoProcAmp, (void **)&pVideoProcAmp))) {
        qWarning() << "failed to query video proc amp interface";
        return false;
    }
    if(FAILED(hr = pVideoProcAmp->Set(property, // property
                           value, // value
                           flag)))
    {
        qWarning() << "Failed to set VideoProcAmp property value" << QString::number(hr, 16);
        return false;
    }
    return true;
}

bool DSCameraSession::setFocus(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Focus, value, flag);
}

bool DSCameraSession::setExposure(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Exposure, value, flag);
}

bool DSCameraSession::setZoom(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Zoom, value, flag);
}

bool DSCameraSession::setIris(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Iris, value, flag);
}

bool DSCameraSession::setPan(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Pan, value, flag);
}

bool DSCameraSession::setTilt(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Tilt, value, flag);
}

bool DSCameraSession::setRoll(long value, bool aut)
{
    tagCameraControlFlags flag = aut?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    return setCameraPropertyValue(CameraControl_Roll, value, flag);
}

bool DSCameraSession::setBrightness(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Brightness, value, flag);
}

bool DSCameraSession::setContrast(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Contrast, value, flag);
}

bool DSCameraSession::setHue(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Hue, value, flag);
}

bool DSCameraSession::setSaturation(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Saturation, value, flag);
}

bool DSCameraSession::setSharpness(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Sharpness, value, flag);
}

bool DSCameraSession::setGamma(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Gamma, value, flag);
}

bool DSCameraSession::setWhiteBalance(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_WhiteBalance, value, flag);
}

bool DSCameraSession::setBacklitComp(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_BacklightCompensation, value, flag);
}

bool DSCameraSession::setGain(long value, bool aut)
{
    tagVideoProcAmpFlags flag = aut?VideoProcAmp_Flags_Auto:VideoProcAmp_Flags_Manual;
    return setVideoProcAmpPropertyValue(VideoProcAmp_Gain, value, flag);
}


bool DSCameraSession::setProperties()
{
    CoInitialize(NULL);

    HRESULT hr;
    AM_MEDIA_TYPE in_mt, out_mt;
    AM_MEDIA_TYPE *pmt = NULL;
    VIDEOINFOHEADER *pvi = NULL;
    VIDEO_STREAM_CONFIG_CAPS scc;

    IAMStreamConfig* pConfig = 0;
    if(FAILED(hr = pBuild->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCap,
                               IID_IAMStreamConfig, (void**)&pConfig)))
    {
        qWarning()<<"failed to get config on capture device";
        return false;
    }

    int iCount;
    int iSize;
    if(FAILED(hr = pConfig->GetNumberOfCapabilities(&iCount, &iSize))) {
        qWarning() << "failed to get capabilities";
        return false;
    }

    ZeroMemory(&in_mt, sizeof(in_mt));
    ZeroMemory(&out_mt, sizeof(out_mt));
    out_mt.majortype = MEDIATYPE_Video;
    out_mt.formattype = FORMAT_VideoInfo;

    if (actualFormat.pixelFormat() == QVideoFrame::Format_RGB24) {
        in_mt.subtype = out_mt.subtype = MEDIASUBTYPE_RGB24;
    } else if (actualFormat.pixelFormat() == QVideoFrame::Format_RGB32) {
        in_mt.subtype = out_mt.subtype = MEDIASUBTYPE_RGB32;
    } else if (actualFormat.pixelFormat() == QVideoFrame::Format_YUYV) {
        in_mt.subtype = out_mt.subtype = MEDIASUBTYPE_YUY2;
    } else if(actualFormat.pixelFormat() == QVideoFrame::Format_User) {
        if(FAILED(hr = CoCreateInstance(CLSID_MjpegDec, NULL, CLSCTX_INPROC, IID_IBaseFilter, (void**)&pIntermediateFilter))) {
            qDebug() << "Cannot create MJPEG decoder" << QString::number(hr, 16);
            return false;
        }
        in_mt.subtype = MEDIASUBTYPE_MJPG;
        out_mt.subtype = MEDIASUBTYPE_RGB24;
    } else if (actualFormat.pixelFormat() == QVideoFrame::Format_YUV420P) {
        if(FAILED(hr = CoCreateInstance(CLSID_AVIDec, NULL, CLSCTX_INPROC, IID_IBaseFilter, (void**)&pIntermediateFilter))) {
            qDebug() << "Cannot create AVI decoder" << QString::number(hr, 16);
            return false;
        }
        in_mt.subtype = MEDIASUBTYPE_I420;
        out_mt.subtype = MEDIASUBTYPE_RGB24;
    } else if (actualFormat.pixelFormat() == QVideoFrame::Format_RGB555) {
        in_mt.subtype = out_mt.subtype = MEDIASUBTYPE_RGB555;
    } else if (actualFormat.pixelFormat() == QVideoFrame::Format_UYVY) {
        in_mt.subtype = out_mt.subtype = MEDIASUBTYPE_UYVY;
    } else {
        qWarning() << "Unknown format? for sample grabber";
        return false;
    }

    bool setFormatOK = false;
    for (int iIndex = 0; iIndex < iCount; iIndex++) {
        if(SUCCEEDED(hr = pConfig->GetStreamCaps(iIndex, &pmt, reinterpret_cast<BYTE*>(&scc))))
        {
            pvi = (VIDEOINFOHEADER*)pmt->pbFormat;

            if ((pmt->majortype == MEDIATYPE_Video) &&
                (pmt->formattype == FORMAT_VideoInfo)) {
                if ((actualFormat.frameWidth() == pvi->bmiHeader.biWidth) &&
                    (actualFormat.frameHeight() == pvi->bmiHeader.biHeight)) {

                    // capture pin output must be set to a specificy media subtype
                    pmt->subtype = in_mt.subtype;

                    hr = pConfig->SetFormat(pmt);
                    _FreeMediaType(*pmt);
                    if(FAILED(hr)) {
                        qWarning() << "failed to set format: " << QString::number(hr,16);
                        qWarning() << "but going to continue";
                        continue; // We going to continue
                    } else {
                        setFormatOK = true;
                        break;
                    }
                }
            }
        }
    }
    pConfig->Release();

    if (!setFormatOK) {
        qWarning() << "unable to set any format for camera";
        return false;
    }

    // Set Sample Grabber config to match capture
    if(FAILED(hr = pSG->SetMediaType(&out_mt))) {
        qWarning() << "failed to set video format on sample grabber";
        return false;
    }

    CoUninitialize();

    return true;
}

bool DSCameraSession::openStream()
{
    //Opens the stream for reading and allocates any necessary resources needed
    //Return true if success, false otherwise

    if (opened) {
        return true;
    }

    if (!graph) {
        graph = createFilterGraph();
        if(!graph) {
            qWarning()<<"failed to create filter graph in openStream";
            return false;
        }
    }

    CoInitialize(NULL);

    HRESULT hr;

    if(FAILED(hr = pGraph->AddFilter(pCap, L"Capture Filter"))) {
        qWarning() << "failed to create capture filter";
        return false;
    }

    if(FAILED(hr = pGraph->AddFilter(pNullRenderer, L"Capture Filter"))) {
        qWarning() << "failed to add null renderer to graph";
        return false;
    }

    if(pIntermediateFilter) {
        if(FAILED(hr = pGraph->AddFilter(pIntermediateFilter, L"Intermediate filter aka decompressor"))) {
            qWarning() << "failed to add intermediate filter to graph";
            return false;
        }
    }

    if(FAILED(hr = pGraph->AddFilter(pSG_Filter, L"Sample Grabber"))) {
        qWarning() << "failed to add sample grabber";
        return false;
    }

    /*
    if(FAILED(hr = pBuild->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                              pCap, NULL, pNullRenderer)))
    {
        qWarning() << "failed to renderstream " << QString::number(hr, 16);
        return false;
    }

    IPin *capturePin = NULL, *stillPin = NULL, *mjpegInput = NULL, *mjpegOutput, *sgPin = NULL, *nullPin = NULL;
    if(FAILED(hr = pBuild->FindPin(pCap, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, NULL, false, 0, &capturePin)))
    {
        qDebug() << "capture output pin";
    }
    if(FAILED(hr = pBuild->FindPin(pCap, PINDIR_OUTPUT, &PIN_CATEGORY_STILL, NULL, false, 0, &stillPin)))
    {
        qDebug() << "still output pin";
    }

    if(FAILED(hr = pBuild->FindPin(pIntermediateFilter, PINDIR_INPUT, NULL, NULL, false, 0, &mjpegInput)))
    {
        qDebug() << "mjpeg input";
    }
    if(FAILED(hr = pBuild->FindPin(pIntermediateFilter, PINDIR_OUTPUT, NULL, NULL, false, 0, &mjpegOutput)))
    {
        qDebug() << "mjpeg output";
    }

    if(FAILED(hr = pBuild->FindPin(pNullRenderer, PINDIR_INPUT, NULL, NULL, false, 0, &nullPin)))
    {
        qDebug() << "sg input";
    }
    if(FAILED(hr = pBuild->FindPin(pSG_Filter, PINDIR_INPUT, NULL, NULL, false, 0, &sgPin)))
    {
        qDebug() << "sg input";
    }

    if(FAILED(hr = pGraph->ConnectDirect(capturePin, nullPin, NULL)))
    {
        qDebug() << "failed to connect pins" << QString::number(hr, 16);
    }

    if(FAILED(hr = pGraph->ConnectDirect(stillPin, sgPin, NULL)))
    {
        qDebug() << "failed to connect pins" << QString::number(hr, 16);
    }

    if(FAILED(hr = pGraph->ConnectDirect(stillPin, mjpegInput, NULL)))
    {
        qDebug() << "failed to connect pins" << QString::number(hr, 16);
    }
    if(FAILED(hr = pGraph->ConnectDirect(mjpegOutput, sgPin, NULL)))
    {
        qDebug() << "failed to connect pins" << QString::number(hr, 16);
    }
*/

    if(FAILED(hr = pBuild->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                              pCap, pIntermediateFilter, pSG_Filter)))
    {
        qWarning() << "failed to renderstream " << QString::number(hr, 16);
        return false;
    }

    // StillMediaType will be used to identify captured data
    if(FAILED(pSG->GetConnectedMediaType(&StillMediaType)))
    {
        qWarning() << "GetConnectedMediaType failed";
        return false;
    }

    pSG_Filter->Release();

    CoUninitialize();

    return true;
}

void DSCameraSession::closeStream()
{
    // Closes the stream and internally frees any resources used
    HRESULT hr;
    IMediaControl* pControl = 0;

    if(FAILED(hr = pGraph->QueryInterface(IID_IMediaControl,(void**)&pControl))) {
        qWarning() << "failed to get stream control";
        return;
    }

    if(FAILED(hr = pControl->StopWhenReady())) {
        qWarning() << "failed to stop";
        pControl->Release();
        return;
    }

    pControl->Release();

    opened = false;
    IPin *pPin = 0;

    if(pCap) {
        if(FAILED(hr = getPin(pCap, "Capture", PINDIR_OUTPUT, &pPin))) {
            qWarning() << "failed to get pin";
            return;
        }
    }

    if(FAILED(hr = pGraph->Disconnect(pPin))) {
        qWarning() << "failed to disconnect capture filter";
        return;
    }

    if(FAILED(hr = getPin(pSG_Filter, "Input", PINDIR_INPUT, &pPin))) {
        qWarning() << "failed to get pin";
        return;
    }

    if(FAILED(hr = pGraph->Disconnect(pPin))) {
        qWarning() << "failed to disconnect sample grabber filter";
        return;
    }

    if(FAILED(hr = pGraph->RemoveFilter(pSG_Filter))) {
        qWarning() << "failed to remove sample grabber filter";
        return;
    }

    if(FAILED(hr = pGraph->RemoveFilter(pCap))) {
        qWarning() << "failed to remove capture filter";
        return;
    }

    SAFE_RELEASE(pCap);
    if(pIntermediateFilter)
        SAFE_RELEASE(pIntermediateFilter);
    SAFE_RELEASE(pSG_Filter);
    SAFE_RELEASE(pGraph);
    SAFE_RELEASE(pBuild);

    graph = false;
}

bool DSCameraSession::startStream()
{
    // Starts the stream, by emitting either QVideoPackets
    // or QvideoFrames, depending on Format chosen
    if (!graph)
        graph = createFilterGraph();

    if (!setProperties()) {
        qWarning() << "Couldn't set properties (retrying)";
        closeStream();
        if (!openStream()) {
            qWarning() << "Retry to open strean failed";
            return false;
        }
    }

    if (!opened) {
        opened = openStream();
        if (!opened) {
            qWarning() << "failed to openStream()";
            return false;
        }
    }

    HRESULT hr;
    IMediaControl* pControl = 0;

    if(FAILED(hr = pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl))) {
        qWarning() << "failed to get stream control";
        return false;
    }

    hr = pControl->Run();
    pControl->Release();

    if (FAILED(hr)) {
        qWarning() << "failed to run";
        return false;
    }

    active = true;

    return true;
}


void DSCameraSession::stopStream()
{
    // Stops the stream from emitting packets
    HRESULT hr;

    IMediaControl* pControl = 0;
    if(FAILED(hr = pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl))) {
        qWarning() << "failed to get stream control";
        return;
    }

    hr = pControl->Stop();
    pControl->Release();

    if (FAILED(hr)) {
        qWarning() << "failed to stop";
        return;
    }
    active = false;

    if (opened) {
        closeStream();
    }
}

void DSCameraSession::suspendStream()
{
    // Pauses the stream
    HRESULT hr;

    IMediaControl* pControl = 0;
    if(FAILED(hr = pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl))) {
        qWarning() << "failed to get stream control";
        return;
    }

    hr = pControl->Pause();
    pControl->Release();

    if (FAILED(hr)) {
        qWarning() << "failed to pause";
        return;
    }

    active = false;
}

void DSCameraSession::resumeStream()
{
    // Pauses the stream
    HRESULT hr;

    IMediaControl* pControl = 0;
    if(FAILED(hr = pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl))) {
        qWarning() << "failed to get stream control";
        return;
    }

    hr = pControl->Run();
    pControl->Release();

    if (FAILED(hr)) {
        qWarning() << "failed to run";
        return;
    }

    active = false;
}

void DSCameraSession::capture()
{
    mCaptureNextFrame = true;

    HRESULT hr;
    IAMVideoControl *pAMVidControl = NULL;

    if(FAILED(hr = pCap->QueryInterface(IID_IAMVideoControl, (void**)&pAMVidControl)))
    {
            qWarning() << "QueryInterface failed" << QString::number(hr, 16);
            return;
    }

    IPin *stillPin = NULL;

    if(FAILED(hr = pBuild->FindPin(pCap, PINDIR_OUTPUT, &PIN_CATEGORY_STILL, NULL, false, 0, &stillPin)))
    {
        qWarning() << "FindPin failed" << QString::number(hr, 16);
        pAMVidControl->Release();
        return;
    }

    if(FAILED(hr = pAMVidControl->SetMode(stillPin, VideoControlFlag_Trigger)))
    {
        qWarning() << "SetMode failed" << QString::number(hr, 16);
        stillPin->Release();
        pAMVidControl->Release();
        return;
    }

    stillPin->Release();
    pAMVidControl->Release();

}

QT_END_NAMESPACE
