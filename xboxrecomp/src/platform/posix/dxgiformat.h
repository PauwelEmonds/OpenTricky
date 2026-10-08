/*
 * dxgiformat.h for POSIX hosts: the DXGI_FORMAT values the push-buffer
 * translator uses to describe vertex attributes to the renderer. Same
 * numbers as the Windows SDK; the GLES renderer maps them to GL formats.
 */
#ifndef OT_POSIX_DXGIFORMAT_H
#define OT_POSIX_DXGIFORMAT_H

typedef enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN              = 0,
    DXGI_FORMAT_R32G32B32A32_FLOAT   = 2,
    DXGI_FORMAT_R32G32B32_FLOAT      = 6,
    DXGI_FORMAT_R16G16B16A16_SNORM   = 13,
    DXGI_FORMAT_R16G16B16A16_SINT    = 14,
    DXGI_FORMAT_R32G32_FLOAT         = 16,
    DXGI_FORMAT_R8G8B8A8_UNORM       = 28,
    DXGI_FORMAT_R16G16_SNORM         = 37,
    DXGI_FORMAT_R16G16_SINT          = 38,
    DXGI_FORMAT_D24_UNORM_S8_UINT    = 45,
    DXGI_FORMAT_R32_FLOAT            = 41,
    DXGI_FORMAT_R32_UINT             = 42,
    DXGI_FORMAT_R8G8_UNORM           = 49,
    DXGI_FORMAT_R16_UINT             = 57,
    DXGI_FORMAT_R16_SNORM            = 58,
    DXGI_FORMAT_R16_SINT             = 59,
    DXGI_FORMAT_R8_UNORM             = 61,
    DXGI_FORMAT_BC1_UNORM            = 71,
    DXGI_FORMAT_BC2_UNORM            = 74,
    DXGI_FORMAT_BC3_UNORM            = 77,
    DXGI_FORMAT_B8G8R8A8_UNORM       = 87,
    DXGI_FORMAT_FORCE_UINT           = 0x7FFFFFFF
} DXGI_FORMAT;

#endif /* OT_POSIX_DXGIFORMAT_H */
