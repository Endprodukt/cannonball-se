#pragma once

#include <SDL_opengles2.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../texture_replacement_frame.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
    #include <windows.h>
    #include <objbase.h>
    #include <wincodec.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ole32.lib")
        #pragma comment(lib, "windowscodecs.lib")
    #endif
#endif

namespace texture_replacement_renderer
{
    struct Image
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };

    struct Texture
    {
        GLuint id = 0;
        int width = 0;
        int height = 0;
        bool attempted = false;
    };

    inline GLuint program = 0;
    inline GLuint vbo = 0;
    inline GLuint mask_texture = 0;
    inline GLint loc_pos = -1;
    inline GLint loc_uv = -1;
    inline GLint loc_mask_uv = -1;
    inline std::unordered_map<std::string, Texture> textures;

#ifdef _WIN32
    inline bool load_png(const std::filesystem::path& path, Image& image)
    {
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(init_hr);

        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));

        if (SUCCEEDED(hr))
        {
            hr = factory->CreateDecoderFromFilename(
                path.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
        }
        if (SUCCEEDED(hr))
            hr = decoder->GetFrame(0, &frame);

        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(hr))
            hr = frame->GetSize(&width, &height);

        if (SUCCEEDED(hr) &&
            (width == 0 || height == 0 || width > 16384 || height > 16384))
        {
            hr = E_FAIL;
        }

        if (SUCCEEDED(hr))
            hr = factory->CreateFormatConverter(&converter);

        if (SUCCEEDED(hr))
        {
            hr = converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }

        if (SUCCEEDED(hr))
        {
            image.width = static_cast<int>(width);
            image.height = static_cast<int>(height);
            image.rgba.resize(static_cast<size_t>(width) * height * 4);
            hr = converter->CopyPixels(
                nullptr,
                width * 4,
                static_cast<UINT>(image.rgba.size()),
                image.rgba.data());
        }

        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
        if (uninitialize) CoUninitialize();

        return SUCCEEDED(hr);
    }
#else
    inline bool load_png(const std::filesystem::path&, Image&)
    {
        return false;
    }
#endif

    inline GLuint compile_shader(GLenum type, const char* source)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            GLint length = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<size_t>(std::max(1, length)), '\0');
            glGetShaderInfoLog(shader, length, nullptr, log.data());
            std::cerr << "Texture replacement shader compile failed: "
                      << log << "\n";
            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

    inline bool ensure_program()
    {
        if (program)
            return true;

        static const char* vertex_shader =
            "precision mediump float;\n"
            "attribute vec2 VertexCoord;\n"
            "attribute vec2 TexCoord;\n"
            "attribute vec2 MaskCoord;\n"
            "varying vec2 vUV;\n"
            "varying vec2 vMaskUV;\n"
            "void main(){\n"
            "    vUV = TexCoord;\n"
            "    vMaskUV = MaskCoord;\n"
            "    gl_Position = vec4(VertexCoord, 0.0, 1.0);\n"
            "}\n";

        static const char* fragment_shader =
            "precision mediump float;\n"
            "varying vec2 vUV;\n"
            "varying vec2 vMaskUV;\n"
            "uniform sampler2D Texture;\n"
            "uniform sampler2D Visibility;\n"
            "void main(){\n"
            "    if (texture2D(Visibility, vMaskUV).r < 0.5) discard;\n"
            "    gl_FragColor = texture2D(Texture, vUV);\n"
            "}\n";

        const GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader);
        const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader);
        if (!vs || !fs)
        {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "VertexCoord");
        glBindAttribLocation(program, 1, "TexCoord");
        glBindAttribLocation(program, 2, "MaskCoord");
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<size_t>(std::max(1, length)), '\0');
            glGetProgramInfoLog(program, length, nullptr, log.data());
            std::cerr << "Texture replacement shader link failed: "
                      << log << "\n";
            glDeleteProgram(program);
            program = 0;
            return false;
        }

        loc_pos = glGetAttribLocation(program, "VertexCoord");
        loc_uv = glGetAttribLocation(program, "TexCoord");
        loc_mask_uv = glGetAttribLocation(program, "MaskCoord");

        glUseProgram(program);
        const GLint texture_sampler = glGetUniformLocation(program, "Texture");
        const GLint mask_sampler = glGetUniformLocation(program, "Visibility");
        if (texture_sampler >= 0) glUniform1i(texture_sampler, 0);
        if (mask_sampler >= 0) glUniform1i(mask_sampler, 1);

        glGenBuffers(1, &vbo);
        glGenTextures(1, &mask_texture);
        if (!vbo || !mask_texture)
            return false;

        glBindTexture(GL_TEXTURE_2D, mask_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        return true;
    }

    inline Texture* get_texture(
        const texture_replacement_frame::DrawCommand& command)
    {
        Texture& texture = textures[command.path];
        if (texture.attempted)
            return texture.id ? &texture : nullptr;

        texture.attempted = true;

        Image image;
        const std::filesystem::path path(command.path);
        if (!load_png(path, image))
        {
            std::cerr << "Replacement PNG could not be loaded: "
                      << path.string() << "\n";
            return nullptr;
        }

        if (command.base_texture_width == 0 ||
            command.base_texture_height == 0 ||
            image.width <= 0 || image.height <= 0 ||
            image.width % static_cast<int>(command.base_texture_width) != 0 ||
            image.height % static_cast<int>(command.base_texture_height) != 0)
        {
            std::cerr << "Ignoring replacement with incompatible size: "
                      << path.string() << " (" << image.width << 'x'
                      << image.height << ")\n";
            return nullptr;
        }

        const int scale_x =
            image.width / static_cast<int>(command.base_texture_width);
        const int scale_y =
            image.height / static_cast<int>(command.base_texture_height);

        if (scale_x < 1 || scale_x != scale_y)
        {
            std::cerr << "Ignoring replacement with non-uniform scale: "
                      << path.string() << "\n";
            return nullptr;
        }

        // GLES2 repeating textures must remain power-of-two. 1024x512 source
        // maps at 1x/2x/4x/8x satisfy this naturally.
        if (command.repeat && (scale_x & (scale_x - 1)) != 0)
        {
            std::cerr << "Ignoring repeating replacement with non-power-of-two scale: "
                      << path.string() << "\n";
            return nullptr;
        }

        glGenTextures(1, &texture.id);
        glBindTexture(GL_TEXTURE_2D, texture.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        command.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        command.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            image.width,
            image.height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            image.rgba.data());

        texture.width = image.width;
        texture.height = image.height;

        std::cout << "Loaded texture replacement: " << path.string()
                  << " (" << scale_x << "x)\n";
        return &texture;
    }

    inline void draw_frame(const texture_replacement_frame::Frame& frame)
    {
        if (frame.logical_width <= 0 || frame.logical_height <= 0 ||
            frame.commands.empty())
        {
            return;
        }

        if (!ensure_program())
            return;

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(program);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        for (const auto& command : frame.commands)
        {
            Texture* texture = get_texture(command);
            if (!texture)
                continue;

            const size_t mask_size =
                static_cast<size_t>(command.width) * command.height;
            if (command.width <= 0 || command.height <= 0 ||
                command.visibility_mask.size() != mask_size)
            {
                continue;
            }

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, mask_texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_LUMINANCE,
                command.width,
                command.height,
                0,
                GL_LUMINANCE,
                GL_UNSIGNED_BYTE,
                command.visibility_mask.data());

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture->id);

            const float left =
                (2.0f * static_cast<float>(command.x) /
                 frame.logical_width) - 1.0f;
            const float right =
                (2.0f * static_cast<float>(command.x + command.width) /
                 frame.logical_width) - 1.0f;
            const float top =
                1.0f - (2.0f * static_cast<float>(command.y) /
                         frame.logical_height);
            const float bottom =
                1.0f - (2.0f * static_cast<float>(command.y + command.height) /
                         frame.logical_height);

            const float vertices[24] = {
                left,  top,    command.u0, command.v0, 0.0f, 0.0f,
                left,  bottom, command.u0, command.v1, 0.0f, 1.0f,
                right, top,    command.u1, command.v0, 1.0f, 0.0f,
                right, bottom, command.u1, command.v1, 1.0f, 1.0f
            };

            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
            const GLsizei stride = 6 * sizeof(float);

            if (loc_pos >= 0)
            {
                glEnableVertexAttribArray(loc_pos);
                glVertexAttribPointer(
                    loc_pos, 2, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(0));
            }
            if (loc_uv >= 0)
            {
                glEnableVertexAttribArray(loc_uv);
                glVertexAttribPointer(
                    loc_uv, 2, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(2 * sizeof(float)));
            }
            if (loc_mask_uv >= 0)
            {
                glEnableVertexAttribArray(loc_mask_uv);
                glVertexAttribPointer(
                    loc_mask_uv, 2, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(4 * sizeof(float)));
            }

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glDisable(GL_BLEND);
    }

    inline void draw_next_presented_frame()
    {
        draw_frame(texture_replacement_frame::consume_for_present());
    }

    inline void shutdown()
    {
        for (auto& pair : textures)
        {
            if (pair.second.id)
                glDeleteTextures(1, &pair.second.id);
        }
        textures.clear();

        if (mask_texture)
        {
            glDeleteTextures(1, &mask_texture);
            mask_texture = 0;
        }
        if (vbo)
        {
            glDeleteBuffers(1, &vbo);
            vbo = 0;
        }
        if (program)
        {
            glDeleteProgram(program);
            program = 0;
        }

        loc_pos = -1;
        loc_uv = -1;
        loc_mask_uv = -1;
    }
}
