/***************************************************************
 * Copyright (C) 2020, 2026, Val Doroshchuk <valbok@gmail.com> *
 *                                                             *
 * This file is part of QtAVPlayer.                            *
 * Free Qt Media Player based on FFmpeg.                       *
 ***************************************************************/

#include "qavvideocodec_p.h"
#include "qavhwdevice_p.h"
#include "qavcodec_p_p.h"
#include "qavpacket.h"
#include "qavframe.h"
#include "qavvideoframe.h"
#include <QDebug>
#include <QString>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
}

QT_BEGIN_NAMESPACE

class QAVVideoCodecPrivate : public QAVCodecPrivate
{
public:
    QSharedPointer<QAVHWDevice> hw_device;
};

static bool isSoftwarePixelFormat(AVPixelFormat from)
{
    switch (from) {
    case AV_PIX_FMT_VAAPI:
    case AV_PIX_FMT_VDPAU:
    case AV_PIX_FMT_MEDIACODEC:
    case AV_PIX_FMT_VIDEOTOOLBOX:
    case AV_PIX_FMT_D3D11:
    case AV_PIX_FMT_D3D11VA_VLD:
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 0, 0)
    case AV_PIX_FMT_OPENCL:
#endif
    case AV_PIX_FMT_CUDA:
    case AV_PIX_FMT_DXVA2_VLD:
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 58, 101) && LIBAVUTIL_VERSION_INT < AV_VERSION_INT(59, 8, 0)
    case AV_PIX_FMT_XVMC:
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 134, 0)
    case AV_PIX_FMT_VULKAN:
#endif
    case AV_PIX_FMT_DRM_PRIME:
    case AV_PIX_FMT_MMAL:
    case AV_PIX_FMT_QSV:
        return false;
    default:
        return true;
    }
}

QList<AVHWDeviceType> QAVVideoCodec::supportedHWDevices(const AVCodec *c)
{
    QList<AVHWDeviceType> supported;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(c, i);
        if (!config)
            break;
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            supported.append(config->device_type);
    }
#endif
    return supported;
}

static AVPixelFormat negotiate_pixel_format(AVCodecContext *c, const AVPixelFormat *f)
{
    auto d = reinterpret_cast<QAVVideoCodecPrivate *>(c->opaque);
    auto supported = QAVVideoCodec::supportedHWDevices(c->codec);
    qDebug() << "negotiate_pixel_format for codec" << c->codec->name;
    if (!supported.isEmpty()) {
        qDebug() << c->codec->name << ": supported hardware device contexts:";
        for (auto a: supported)
            qDebug() << "   " << av_hwdevice_get_type_name(a);
    } else {
        qWarning() << "None of the hardware accelerations are supported";
    }

    QList<AVPixelFormat> softwareFormats;
    QList<AVPixelFormat> hardwareFormats;
    QList<AVPixelFormat> allFormats;
    for (int i = 0; f[i] != AV_PIX_FMT_NONE; ++i) {
        allFormats.append(f[i]);
        if (!isSoftwarePixelFormat(f[i])) {
            hardwareFormats.append(f[i]);
            continue;
        }
        softwareFormats.append(f[i]);
    }

    qDebug() << "get_format candidates (raw order):";
    for (auto a : allFormats) {
        auto dsc = av_pix_fmt_desc_get(a);
        qDebug() << "  " << (dsc ? dsc->name : "unknown") << ": AVPixelFormat(" << a << ")";
    }

    qDebug() << "Available pixel formats (software-first classification):";
    for (auto a : softwareFormats) {
        auto dsc = av_pix_fmt_desc_get(a);
        qDebug() << "  " << dsc->name << ": AVPixelFormat(" << a << ")";
    }

    for (auto a : hardwareFormats) {
        auto dsc = av_pix_fmt_desc_get(a);
        qDebug() << "  " << dsc->name << ": AVPixelFormat(" << a << ")";
    }

    AVPixelFormat pf = !softwareFormats.isEmpty() ? softwareFormats[0] : AV_PIX_FMT_NONE;
    const char *decStr = "software";

    qDebug() << "Initial pixel format candidate:" << (av_pix_fmt_desc_get(pf) ? av_pix_fmt_desc_get(pf)->name : "none")
             << "(" << pf << ")";

    auto chooseHardwareFormat = [&](AVPixelFormat desired, const char *label) {
        for (auto fmt : hardwareFormats) {
            if (fmt == desired) {
                auto dsc = av_pix_fmt_desc_get(fmt);
                qDebug() << "Selecting" << label << "format" << (dsc ? dsc->name : "unknown")
                         << "(" << fmt << ")";
                pf = fmt;
                decStr = label;
                return true;
            }
        }
        qDebug() << "Desired hardware format" << label << "not offered by get_format";
        return false;
    };

    auto chooseSoftwareFormat = [&](AVPixelFormat desired, const char *label) {
        for (auto fmt : softwareFormats) {
            if (fmt == desired) {
                auto dsc = av_pix_fmt_desc_get(fmt);
                qDebug() << "Selecting" << label << "software format" << (dsc ? dsc->name : "unknown")
                         << "(" << fmt << ")";
                pf = fmt;
                decStr = label;
                return true;
            }
        }
        qDebug() << "Desired software format" << label << "not offered by get_format";
        return false;
    };

    if (d->hw_device) {
        auto desired = d->hw_device->format();
        auto dsc = av_pix_fmt_desc_get(desired);
        qDebug() << "Codec device reports desired format" << (dsc ? dsc->name : "unknown")
                 << "(" << desired << ")";
        for (auto f : hardwareFormats) {
            if (f == d->hw_device->format()) {
                d->hw_device->init(c);
                pf = d->hw_device->format();
                decStr = "hardware";
                qDebug() << "Matched codec device to hardware format"
                         << (av_pix_fmt_desc_get(pf) ? av_pix_fmt_desc_get(pf)->name : "unknown")
                         << "(" << pf << ")";
                break;
            }
        }
    }

    auto dsc = av_pix_fmt_desc_get(pf);
    if (dsc)
        qDebug() << "Using" << decStr << "decoding in" << dsc->name;
    else
        qDebug() << "None of the pixel formats";

    qDebug() << "Final selected pixel format:" << (dsc ? dsc->name : "unknown")
             << "(" << pf << ")" << "mode=" << decStr;

    return pf;
}

QAVVideoCodec::QAVVideoCodec(const AVCodec *codec)
    : QAVFrameCodec(*new QAVVideoCodecPrivate, codec)
{
    d_ptr->avctx->opaque = d_ptr.get();
    d_ptr->avctx->get_format = negotiate_pixel_format;
}

QAVVideoCodec::~QAVVideoCodec()
{
    av_buffer_unref(&avctx()->hw_device_ctx);
}

void QAVVideoCodec::setDevice(const QSharedPointer<QAVHWDevice> &d)
{
    d_func()->hw_device = d;
}

QAVHWDevice *QAVVideoCodec::device() const
{
    return d_func()->hw_device.data();
}

QT_END_NAMESPACE
