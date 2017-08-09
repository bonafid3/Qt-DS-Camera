/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef DSCAMERASESSION_H
#define DSCAMERASESSION_H

#include <QtCore/qobject.h>
#include <QTime>
#include <QUrl>
#include <QMap>
#include <QMutex>

#include <qcamera.h>
#include <QtMultimedia/qvideoframe.h>
#include <QtMultimedia/qabstractvideosurface.h>
#include <QtMultimedia/qvideosurfaceformat.h>

#include <opencv2/core/core.hpp>

#include <tchar.h>
#include <dshow.h>
#include <objbase.h>
#include <initguid.h>
#ifdef Q_CC_MSVC
#  pragma comment(lib, "strmiids.lib")
#  pragma comment(lib, "ole32.lib")
#endif // Q_CC_MSVC
#include <windows.h>

#ifdef Q_CC_MSVC
#  pragma include_alias("dxtrans.h","qedit.h")
#endif // Q_CC_MSVC
#define __IDxtCompositor_INTERFACE_DEFINED__
#define __IDxtAlphaSetter_INTERFACE_DEFINED__
#define __IDxtJpeg_INTERFACE_DEFINED__
#define __IDxtKey_INTERFACE_DEFINED__

#include "directshowglobal.h"

struct ICaptureGraphBuilder2;
struct ISampleGrabber;



QT_BEGIN_NAMESPACE

class SampleGrabberCallbackPrivate;

struct video_buffer {
    unsigned char* buffer;
    int            length;
    qint64         time;
};

class DSCameraSession : public QObject
{
    Q_OBJECT
public:
    DSCameraSession(const QByteArray &device, QObject *parent = 0);
    ~DSCameraSession();

    static QList<QByteArray> availableDevices();
    static QString deviceDescription(const QByteArray &device);

    // camera formats
    QList<QVideoSurfaceFormat> supportedFormats();
    void setFormat(QVideoSurfaceFormat format);
    QVideoSurfaceFormat format();

    AM_MEDIA_TYPE StillMediaType;
    QList<video_buffer*> frames;
    SampleGrabberCallbackPrivate* StillCapCB;

    QMutex mutex;

    bool mCaptureNextFrame;

    bool deviceReady();
    bool pictureInProgress();

    // camera controls
    bool getCameraControlPropertyRange(tagCameraControlProperty property, tRange &range);
    bool getVideoProcAmpPropertyRange(tagVideoProcAmpProperty property, tRange &range);

    bool setFocus(long value, bool aut=false);
    bool setExposure(long value, bool aut=false);
    bool setZoom(long value, bool aut=false);
    bool setIris(long value, bool aut=false);
    bool setPan(long value, bool aut=false);
    bool setTilt(long value, bool aut=false);
    bool setRoll(long value, bool aut=false);

    // video proc amp
    bool setBrightness(long value, bool aut=false);
    bool setContrast(long value, bool aut=false);
    bool setHue(long value, bool aut=false);
    bool setSaturation(long value, bool aut=false);
    bool setSharpness(long value, bool aut=false);
    bool setGamma(long value, bool aut=false);
    bool setWhiteBalance(long value, bool aut=false);
    bool setBacklitComp(long value, bool aut=false);
    bool setGain(long value, bool aut=false);

    // media control
    bool setOutputLocation(const QUrl &sink);
    QUrl outputLocation() const;
    qint64 position() const;
    int state() const;
    void record();
    void pause();
    void stop();

    void setSurface(QAbstractVideoSurface* surface);

    int captureImage(const QString &fileName);

    bool startStream();
    void stopStream();

    void suspendStream();
    void resumeStream();

    void capture();

private:
    QVideoSurfaceFormat actualFormat;
    QList<QVideoSurfaceFormat> m_formats;

    QTime timeStamp;
    bool graph;
    bool active;
    bool opened;
    bool available;
    QCamera::State m_state;
    QByteArray m_device;
    QUrl m_sink;
    QAbstractVideoSurface* m_surface;

    ICaptureGraphBuilder2* pBuild;
    IGraphBuilder* pGraph;
    IBaseFilter* pCap;
    IBaseFilter* pNullRenderer;
    IBaseFilter* pIntermediateFilter;
    IBaseFilter* pSG_Filter;
    ISampleGrabber *pSG;

    QString m_snapshot;
    int m_currentImageId;

    QList<QByteArray> m_devices;
    QStringList m_descriptions;

    static void enumerateDevices(QList<QByteArray> *devices, QStringList *descriptions);
    int yuv2rgb(int y, int u, int v);

    HRESULT getPin(IBaseFilter *pFilter, QString type, PIN_DIRECTION PinDir, IPin **ppPin);
    bool createFilterGraph();
    void updateProperties();
    bool setProperties();
    bool openStream();
    void closeStream();

    bool setCameraPropertyValue(tagCameraControlProperty property, int value, tagCameraControlFlags flags);
    bool setVideoProcAmpPropertyValue(tagVideoProcAmpProperty property, int value, tagVideoProcAmpFlags flag);

    HRESULT getFilterAndPinInfo(IBaseFilter *pFilter);

Q_SIGNALS:
    void cvFrameCaptured(cv::Mat frame);

private Q_SLOTS:
    void captureFrame();
};

QT_END_NAMESPACE

#endif
