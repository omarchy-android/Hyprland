#include "GLRenderbuffer.hpp"
#include "../Renderer.hpp"
#include "../OpenGL.hpp"
#include "../../Compositor.hpp"
#include "../../helpers/env/Env.hpp"
#include "../Framebuffer.hpp"
#include "GLFramebuffer.hpp"
#include "../Renderbuffer.hpp"
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/signal/Listener.hpp>
#include <hyprutils/signal/Signal.hpp>

#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <drm_fourcc.h>
#include <unistd.h>

using namespace Render::GL;

CGLRenderbuffer::~CGLRenderbuffer() {
    if (!g_pCompositor || g_pCompositor->m_isShuttingDown || !g_pHyprRenderer)
        return;

    g_pHyprOpenGL->makeEGLCurrent();

    if (m_framebuffer) {
        unbind();
        m_framebuffer->release();
    }

    if (m_rbo)
        glDeleteRenderbuffers(1, &m_rbo);

    if (m_image != EGL_NO_IMAGE_KHR)
        g_pHyprOpenGL->m_proc.eglDestroyImageKHR(g_pHyprOpenGL->m_eglDisplay, m_image);
}

CGLRenderbuffer::CGLRenderbuffer(SP<Aquamarine::IBuffer> buffer, uint32_t format) : IRenderbuffer(buffer, format) {
    auto dma = buffer->dmabuf();

    if (!dma.success) {
        const auto shm = buffer->shm();
        if (!shm.success || (shm.format != DRM_FORMAT_XRGB8888 && shm.format != DRM_FORMAT_ARGB8888)) {
            Log::logger->log(Log::ERR, "rb: invalid or unsupported SHM output buffer");
            return;
        }

        m_framebuffer = makeShared<CGLFramebuffer>();
        if (!m_framebuffer->alloc(shm.size.x, shm.size.y, shm.format)) {
            Log::logger->log(Log::ERR, "rb: failed to allocate the GPU staging framebuffer for SHM output");
            return;
        }

        if (Env::envEnabled("HYPRLAND_GL_DMABUF_EXPORT") && g_pHyprOpenGL->m_proc.eglExportDMABUFImageQueryMESA &&
            g_pHyprOpenGL->m_proc.eglExportDMABUFImageMESA) {
            const auto texture = m_framebuffer->getTexture();
            const EGLint imageAttrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            m_exportImage = g_pHyprOpenGL->m_proc.eglCreateImageKHR(
                g_pHyprOpenGL->m_eglDisplay, g_pHyprOpenGL->m_eglContext, EGL_GL_TEXTURE_2D_KHR,
                reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture->m_texID)), imageAttrs);

            if (m_exportImage != EGL_NO_IMAGE_KHR) {
                int          fourcc = 0;
                int          planes = 0;
                EGLuint64KHR modifier = DRM_FORMAT_MOD_INVALID;
                if (g_pHyprOpenGL->m_proc.eglExportDMABUFImageQueryMESA(g_pHyprOpenGL->m_eglDisplay, m_exportImage, &fourcc, &planes, &modifier) && planes > 0 &&
                    planes <= 4) {
                    int    fds[4]     = {-1, -1, -1, -1};
                    EGLint strides[4] = {};
                    EGLint offsets[4] = {};
                    if (g_pHyprOpenGL->m_proc.eglExportDMABUFImageMESA(g_pHyprOpenGL->m_eglDisplay, m_exportImage, fds, strides, offsets)) {
                        Aquamarine::SDMABUFAttrs attrs{
                            .success  = true,
                            .size     = shm.size,
                            .format   = static_cast<uint32_t>(fourcc),
                            .modifier = static_cast<uint64_t>(modifier),
                            .planes   = planes,
                        };
                        for (int i = 0; i < planes; ++i) {
                            attrs.fds.at(i)     = fds[i];
                            attrs.strides.at(i) = static_cast<uint32_t>(strides[i]);
                            attrs.offsets.at(i) = static_cast<uint32_t>(offsets[i]);
                        }
                        const bool validDescriptors = std::all_of(fds, fds + planes, [](int fd) { return fd >= 0; });
                        m_gpuExported = validDescriptors && buffer->setPresentationDMABUF(attrs);
                        if (m_gpuExported)
                            LOG(Log::INFO, "rb: GPU presentation export ready: format 0x{:08x}, modifier 0x{:016x}, {} plane(s)",
                                attrs.format, attrs.modifier, attrs.planes);
                        else
                            Log::logger->log(Log::WARN, "rb: GPU presentation export returned invalid DMA-BUF descriptors; retaining SHM output");
                        for (int i = 0; i < planes; ++i) {
                            if (fds[i] >= 0)
                                close(fds[i]);
                        }
                    }
                }
            }

            if (!m_gpuExported && m_exportImage != EGL_NO_IMAGE_KHR) {
                g_pHyprOpenGL->m_proc.eglDestroyImageKHR(g_pHyprOpenGL->m_eglDisplay, m_exportImage);
                m_exportImage = EGL_NO_IMAGE_KHR;
            }
        }

        m_shmBacked = true;
        m_good      = true;
        return;
    }

    m_image = g_pHyprOpenGL->createEGLImage(dma);
    if (m_image == EGL_NO_IMAGE_KHR) {
        Log::logger->log(Log::ERR, "rb: createEGLImage failed");
        return;
    }

    glGenRenderbuffers(1, &m_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
    g_pHyprOpenGL->m_proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, m_image);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    m_framebuffer = makeShared<CGLFramebuffer>();
    glGenFramebuffers(1, &GLFB(m_framebuffer)->m_fb);
    GLFB(m_framebuffer)->m_fbAllocated = true;
    m_framebuffer->m_size              = buffer->size;
    m_framebuffer->m_drmFormat         = dma.format;
    m_framebuffer->bind();
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Log::logger->log(Log::ERR, "rbo: glCheckFramebufferStatus failed");
        return;
    }

    GLFB(m_framebuffer)->unbind();

    m_listeners.destroyBuffer = buffer->events.destroy.listen([this] { g_pHyprRenderer->onRenderbufferDestroy(this); });

    m_good = true;
}

void CGLRenderbuffer::bind() {
    g_pHyprOpenGL->makeEGLCurrent();
    g_pHyprRenderer->bindFB(m_framebuffer);
}

void CGLRenderbuffer::unbind() {
    const auto outputBuffer = m_hlBuffer.lock();
    if (m_gpuExported && outputBuffer && outputBuffer->presentationDMABUFActive()) {
        // The current KGSL stack does not attach implicit DMA-BUF fences.
        // Finish the GPU work before the parent compositor samples the
        // exported texture. This removes the full-frame CPU readback while a
        // native-fence hand-off is implemented as the next optimization.
        glFinish();
        GLFB(m_framebuffer)->unbind();
        return;
    }

    if (m_shmBacked) {
        // Environment is fixed for the lifetime of the compositor. Cache the
        // switches instead of calling getenv on every output frame.
        static const bool damageOnly = Env::envEnabled("HYPRLAND_SHM_DAMAGE");
        static const bool forceFinish = Env::envEnabled("HYPRLAND_SHM_GLFINISH");
        static const bool profile     = Env::envEnabled("HYPRLAND_SHM_PROFILE");

        // With continuous frame pacing Hyprland may submit an idle frame with
        // no changed pixels. Every buffer in the rotating swapchain has already
        // received its buffer-age damage by that point, so neither map nor read
        // the 1920x1200 SHM image again.
        if (damageOnly && g_pHyprRenderer->context().m_data.damage.empty()) {
            GLFB(m_framebuffer)->unbind();
            return;
        }

        const auto buffer = m_hlBuffer.lock();
        if (!buffer) {
            GLFB(m_framebuffer)->unbind();
            return;
        }

        const auto shm                         = buffer->shm();
        auto [pixelData, dataFormat, bufferLen] = buffer->beginDataPtr(0);
        const auto requiredLen                 = static_cast<size_t>(shm.offset) + static_cast<size_t>(shm.stride) * static_cast<size_t>(shm.size.y);

        if (!shm.success || !pixelData || shm.stride <= 0 || requiredLen > bufferLen ||
            (shm.format != DRM_FORMAT_XRGB8888 && shm.format != DRM_FORMAT_ARGB8888)) {
            Log::logger->log(Log::ERR, "rb: cannot copy the GPU staging framebuffer into the SHM output buffer");
            GLFB(m_framebuffer)->unbind();
            return;
        }

        const auto started = std::chrono::steady_clock::now();

        if (forceFinish)
            glFinish();

        glBindFramebuffer(GL_READ_FRAMEBUFFER, GLFB(m_framebuffer)->getFBID());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        int x = 0;
        int y = 0;
        int w = static_cast<int>(shm.size.x);
        int h = static_cast<int>(shm.size.y);

        // The original Android path read 1920x1200 (9.2 MB) after every tiny
        // cursor or keyboard update.  With full damage tracking enabled, the
        // staging framebuffer already preserves undamaged pixels, so copy only
        // the bounding rectangle touched by this frame.  GL_PACK_ROW_LENGTH
        // writes it directly into the full-stride Wayland SHM image.
        if (damageOnly) {
            const auto extents = g_pHyprRenderer->context().m_data.damage.copy()
                                     .intersect(0, 0, shm.size.x, shm.size.y)
                                     .getExtents()
                                     .round();
            x = std::clamp(static_cast<int>(extents.x), 0, static_cast<int>(shm.size.x));
            y = std::clamp(static_cast<int>(extents.y), 0, static_cast<int>(shm.size.y));
            w = std::clamp(static_cast<int>(extents.w), 0, static_cast<int>(shm.size.x) - x);
            h = std::clamp(static_cast<int>(extents.h), 0, static_cast<int>(shm.size.y) - y);
        }

        if (w > 0 && h > 0) {
            glPixelStorei(GL_PACK_ROW_LENGTH, shm.stride / 4);
            auto* const destination = pixelData + shm.offset + static_cast<size_t>(y) * static_cast<size_t>(shm.stride) + static_cast<size_t>(x) * 4;
            glReadPixels(x, y, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, destination);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        }

        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        buffer->endDataPtr();

        if (profile) {
            static uint64_t frameCount = 0;
            static uint64_t totalUs    = 0;
            static uint64_t totalBytes = 0;
            totalUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
            totalBytes += static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4;
            if (++frameCount % 120 == 0) {
                LOG(Log::INFO, "SHM readback: avg {} us, avg {} KiB over {} frames", totalUs / frameCount, totalBytes / frameCount / 1024, frameCount);
            }
        }
    }

    GLFB(m_framebuffer)->unbind();
}
