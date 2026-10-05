#pragma once

#include "../../helpers/memory/Memory.hpp"
#include "../Renderbuffer.hpp"
#include <aquamarine/buffer/Buffer.hpp>

namespace Render::GL {
    class CGLRenderbuffer : public IRenderbuffer {
      public:
        CGLRenderbuffer(SP<Aquamarine::IBuffer> buffer, uint32_t format);
        ~CGLRenderbuffer();

        void bind() override;
        void unbind() override;

      private:
        void*  m_image     = nullptr;
        void*  m_exportImage = nullptr;
        GLuint m_rbo       = 0;
        bool   m_shmBacked = false;
        bool   m_gpuExported = false;
    };
}
