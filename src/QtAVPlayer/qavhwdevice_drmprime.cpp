/*********************************************************
 * Copyright (C) 2026, Val Doroshchuk <valbok@gmail.com> *
 *                                                       *
 * This file is part of QtAVPlayer.                      *
 * Free Qt Media Player based on FFmpeg.                 *
 *********************************************************/

#include "qavhwdevice_drmprime_p.h"
#include "qavvideobuffer_gpu_p.h"

#include <QDebug>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
}

static PFNEGLCREATEIMAGEKHRPROC s_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC s_eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC s_glEGLImageTargetTexture2DOES = nullptr;

static QString fourccToString(uint32_t fourcc)
{
    char text[5] = {
        char(fourcc & 0xff),
        char((fourcc >> 8) & 0xff),
        char((fourcc >> 16) & 0xff),
        char((fourcc >> 24) & 0xff),
        '\0'
    };
    return QString::fromLatin1(text);
}

QT_BEGIN_NAMESPACE

class QAVHWDevice_DRMPrimePrivate
{
public:
    GLuint textures[3] = {0, 0, 0};
};

QAVHWDevice_DRMPrime::QAVHWDevice_DRMPrime()
    : d_ptr(new QAVHWDevice_DRMPrimePrivate)
{
}

QAVHWDevice_DRMPrime::~QAVHWDevice_DRMPrime()
{
    if (d_ptr->textures[0]) {
        glDeleteTextures(3, d_ptr->textures);
    }
}

AVPixelFormat QAVHWDevice_DRMPrime::format() const
{
    return AV_PIX_FMT_DRM_PRIME;
}

AVHWDeviceType QAVHWDevice_DRMPrime::type() const
{
    return AV_HWDEVICE_TYPE_DRM;
}

class VideoBuffer_DRMPrime : public QAVVideoBuffer_GPU
{
public:
    VideoBuffer_DRMPrime(QAVHWDevice_DRMPrimePrivate *hw, const QAVVideoFrame &frame)
        : QAVVideoBuffer_GPU(frame)
        , m_hw(hw)
    {
        if (!s_eglCreateImageKHR) {
            s_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
            s_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
            s_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        }
    }

    ~VideoBuffer_DRMPrime() override = default;

    QAVVideoFrame::HandleType handleType() const override
    {
        return QAVVideoFrame::GLTextureHandle;
    }

    QVariant textures() const
    {
        QList<QVariant> result;
        for (int i = 0; i < m_planeCount; ++i) {
            result << m_hw->textures[i];
        }
        return result;
    }

    QVariant handle(QRhi */*rhi*/) const override
    {
        if (!s_eglCreateImageKHR || !s_eglDestroyImageKHR || !s_glEGLImageTargetTexture2DOES) {
            qWarning() << "Could not resolve EGL import functions";
            return {};
        }

        auto *avFrame = frame().frame();
        if (!avFrame || avFrame->format != AV_PIX_FMT_DRM_PRIME || !avFrame->data[0]) {
            qWarning() << "Received a frame without DRM PRIME payload";
            return {};
        }

        auto *drm = reinterpret_cast<const AVDRMFrameDescriptor *>(avFrame->data[0]);
        if (!drm || drm->nb_layers < 1 || drm->nb_objects < 1) {
            qWarning() << "Invalid DRM frame descriptor";
            return {};
        }

        const auto &layer = drm->layers[0];
        if (layer.nb_planes != 2 && layer.nb_planes != 3) {
            qWarning() << "Unsupported DRM PRIME layer plane count:" << layer.nb_planes
                       << "format=" << fourccToString(layer.format) << Qt::hex << layer.format;
            return {};
        }

        const bool isNv12 = layer.format == DRM_FORMAT_NV12 || layer.format == DRM_FORMAT_NV21;
        const bool isYuv420 = layer.format == DRM_FORMAT_YUV420 || layer.format == DRM_FORMAT_YVU420;
        if (!isNv12 && !isYuv420) {
            qWarning() << "Unsupported DRM PRIME layer format:" << fourccToString(layer.format) << Qt::hex << layer.format
                       << "planes=" << layer.nb_planes;
            return {};
        }

        qDebug() << "DRM PRIME layer format:" << fourccToString(layer.format)
                 << Qt::hex << layer.format
                 << "planes=" << layer.nb_planes;

        m_planeCount = layer.nb_planes;

        if (!m_hw->textures[0]) {
            glGenTextures(m_planeCount, m_hw->textures);
        }

        const EGLDisplay eglDisplay = eglGetCurrentDisplay();
        if (eglDisplay == EGL_NO_DISPLAY) {
            qWarning() << "No current EGL display";
            return {};
        }

        static const uint32_t formats[3] = { DRM_FORMAT_C8, DRM_FORMAT_C8, DRM_FORMAT_C8 };
        for (int i = 0; i < m_planeCount; ++i) {
            const auto &plane = layer.planes[i];
            if (plane.object_index < 0 || plane.object_index >= drm->nb_objects) {
                qWarning() << "Invalid DRM PRIME object index" << plane.object_index;
                return {};
            }

            const auto &object = drm->objects[plane.object_index];
            if (object.fd < 0) {
                qWarning() << "Invalid DRM PRIME fd";
                return {};
            }

            if (layer.planes[i].pitch <= 0 || layer.planes[i].offset < 0) {
                qWarning() << "Invalid DRM PRIME plane geometry";
                return {};
            }

            const int planeWidth = i == 0 ? avFrame->width : qMax(1, (avFrame->width + 1) / 2);
            const int planeHeight = i == 0 ? avFrame->height : qMax(1, (avFrame->height + 1) / 2);
            EGLint imgAttr[] = {
                EGL_LINUX_DRM_FOURCC_EXT, EGLint(formats[i]),
                EGL_WIDTH, GLint(planeWidth),
                EGL_HEIGHT, GLint(planeHeight),
                EGL_DMA_BUF_PLANE0_FD_EXT, object.fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(plane.offset),
                EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(plane.pitch),
                EGL_NONE
            };

            EGLImage image = s_eglCreateImageKHR(eglDisplay,
                                                 EGL_NO_CONTEXT,
                                                 EGL_LINUX_DMA_BUF_EXT,
                                                 nullptr,
                                                 imgAttr);
            if (!image) {
                qWarning() << "eglCreateImageKHR failed for DRM PRIME plane" << i;
                return {};
            }

            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, m_hw->textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            s_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
            if (glGetError()) {
                qWarning() << "glEGLImageTargetTexture2DOES failed for DRM PRIME plane" << i;
            }

            glBindTexture(GL_TEXTURE_2D, 0);
            s_eglDestroyImageKHR(eglDisplay, image);
        }

        return textures();
    }

    QAVHWDevice_DRMPrimePrivate *m_hw = nullptr;
    mutable int m_planeCount = 0;
};

QAVVideoBuffer *QAVHWDevice_DRMPrime::videoBuffer(const QAVVideoFrame &frame) const
{
    return new VideoBuffer_DRMPrime(d_ptr.get(), frame);
}

QT_END_NAMESPACE
