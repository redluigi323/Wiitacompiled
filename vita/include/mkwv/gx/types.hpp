#pragma once

#include <cstdint>

namespace mkwv::gx {

// Hardware FIFO opcodes. Values match dolphin/gx/GXCommandList.h.
inline constexpr std::uint8_t kNop = 0x00;
inline constexpr std::uint8_t kLoadCp = 0x08;
inline constexpr std::uint8_t kLoadXf = 0x10;
inline constexpr std::uint8_t kLoadIndxA = 0x20;
inline constexpr std::uint8_t kLoadIndxB = 0x28;
inline constexpr std::uint8_t kLoadIndxC = 0x30;
inline constexpr std::uint8_t kLoadIndxD = 0x38;
inline constexpr std::uint8_t kCallDl = 0x40;
inline constexpr std::uint8_t kInvalidateVtx = 0x48;
inline constexpr std::uint8_t kLoadAurora = 0x50;
inline constexpr std::uint8_t kLoadBp = 0x61;
inline constexpr std::uint8_t kOpcodeMask = 0xF8;
inline constexpr std::uint8_t kVatMask = 0x07;
inline constexpr std::uint16_t kAuroraViewport = 0x0001;
inline constexpr std::uint16_t kAuroraScissor = 0x0002;
inline constexpr std::uint16_t kAuroraArrayBase = 0x0010;
inline constexpr std::uint16_t kAuroraXfArray = 0x0036;
inline constexpr std::uint16_t kAuroraDebugPush = 0x0020;
inline constexpr std::uint16_t kAuroraDebugPop = 0x0021;
inline constexpr std::uint16_t kAuroraDebugMarker = 0x0022;
inline constexpr std::uint16_t kAuroraTexObj = 0x0030;
inline constexpr std::uint16_t kAuroraTlut = 0x0031;
inline constexpr std::uint16_t kAuroraDestroyTexObj = 0x0032;
inline constexpr std::uint16_t kAuroraDestroyTlut = 0x0033;
inline constexpr std::uint16_t kAuroraDestroyCopyTex = 0x0034;
inline constexpr std::uint16_t kAuroraInvalidateTex = 0x0035;
inline constexpr std::uint8_t kCpArrayStride = 0xB0;

enum Primitive : std::uint8_t {
    Quads = 0x80,
    Triangles = 0x90,
    TriangleStrip = 0x98,
    TriangleFan = 0xA0,
    Lines = 0xA8,
    LineStrip = 0xB0,
    Points = 0xB8,
};

enum VtxFmt : std::uint8_t {
    VtxFmt0 = 0,
    VtxFmt1,
    VtxFmt2,
    VtxFmt3,
    VtxFmt4,
    VtxFmt5,
    VtxFmt6,
    VtxFmt7,
    VtxFmtCount,
};

enum Attr : std::uint8_t {
    AttrPnMtxIdx = 0,
    AttrTex0MtxIdx,
    AttrTex1MtxIdx,
    AttrTex2MtxIdx,
    AttrTex3MtxIdx,
    AttrTex4MtxIdx,
    AttrTex5MtxIdx,
    AttrTex6MtxIdx,
    AttrTex7MtxIdx,
    AttrPos,
    AttrNrm,
    AttrClr0,
    AttrClr1,
    AttrTex0,
    AttrTex1,
    AttrTex2,
    AttrTex3,
    AttrTex4,
    AttrTex5,
    AttrTex6,
    AttrTex7,
    AttrCount,
};

enum AttrType : std::uint8_t {
    AttrNone = 0,
    AttrDirect = 1,
    AttrIndex8 = 2,
    AttrIndex16 = 3,
};

enum CompCnt : std::uint8_t {
    PosXY = 0,
    PosXYZ = 1,
    NrmXYZ = 0,
    NrmNBT = 1,
    NrmNBT3 = 2,
    ClrRGB = 0,
    ClrRGBA = 1,
    TexS = 0,
    TexST = 1,
};

enum CompType : std::uint8_t {
    U8 = 0,
    S8 = 1,
    U16 = 2,
    S16 = 3,
    F32 = 4,
    RGB565 = 0,
    RGB8 = 1,
    RGBX8 = 2,
    RGBA4 = 3,
    RGBA6 = 4,
    RGBA8 = 5,
};

enum ProjectionType : std::uint8_t {
    Perspective = 0,
    Orthographic = 1,
};

enum Compare : std::uint8_t {
    Never = 0,
    Less,
    Equal,
    LEqual,
    Greater,
    NEqual,
    GEqual,
    Always,
};

enum CullMode : std::uint8_t {
    CullNone = 0,
    CullFront,
    CullBack,
    CullAll,
};

enum TexFmt : std::uint32_t {
    TexI4 = 0x0,
    TexI8 = 0x1,
    TexIA4 = 0x2,
    TexIA8 = 0x3,
    TexRGB565 = 0x4,
    TexRGB5A3 = 0x5,
    TexRGBA8 = 0x6,
    TexCI4 = 0x8,
    TexCI8 = 0x9,
    TexCI14X2 = 0xA,
    TexCMPR = 0xE,
};

enum TlutFmt : std::uint32_t {
    TlutIa8 = 0,
    TlutRgb565 = 1,
    TlutRgb5a3 = 2,
};

inline constexpr std::uint32_t kTexPc = 0x40;
inline constexpr std::uint32_t TexRGBA8PC = TexRGBA8 | kTexPc;

enum TexWrap : std::uint8_t {
    WrapClamp = 0,
    WrapRepeat = 1,
    WrapMirror = 2,
};

enum TevOp : std::uint8_t {
    TevModulate = 0,
    TevDecal,
    TevBlend,
    TevReplace,
    TevPassClr,
    TevOther,
};

enum TevCombineOp : std::uint8_t {
    TevAdd = 0,
    TevSub = 1,
    TevCompR8Gt = 8,
    TevCompR8Eq = 9,
    TevCompGr16Gt = 10,
    TevCompGr16Eq = 11,
    TevCompBgr24Gt = 12,
    TevCompBgr24Eq = 13,
    TevCompRgb8Gt = 14,
    TevCompRgb8Eq = 15,
};

enum FogType : std::uint8_t {
    FogNone = 0,
    FogPerspLin = 2,
    FogPerspExp = 4,
    FogPerspExp2 = 5,
    FogPerspRevExp = 6,
    FogPerspRevExp2 = 7,
    FogOrthoLin = 10,
    FogOrthoExp = 12,
    FogOrthoExp2 = 13,
    FogOrthoRevExp = 14,
    FogOrthoRevExp2 = 15,
};

inline constexpr std::uint8_t kTexMapNull = 0xFF;
inline constexpr std::uint8_t kTexCoordNull = 0xFF;
inline constexpr std::uint8_t kRasColor0 = 0;
inline constexpr std::uint8_t kRasColor1 = 1;
// Hardware TREF colour-channel codes 5 and 6 (Aurora command_processor r2c table): the
// indirect-texture "bump alpha" and its normalised form. 7 is GX_COLOR_ZERO/NULL.
inline constexpr std::uint8_t kRasAlphaBump = 5;
inline constexpr std::uint8_t kRasAlphaBumpN = 6;
inline constexpr std::uint8_t kRasZero = 7;

enum TevColorArg : std::uint8_t {
    CcCPrev = 0,
    CcAPrev,
    CcC0,
    CcA0,
    CcC1,
    CcA1,
    CcC2,
    CcA2,
    CcTexC,
    CcTexA,
    CcRasC,
    CcRasA,
    CcOne,
    CcHalf,
    CcKonst,
    CcZero,
};

enum TevAlphaArg : std::uint8_t {
    CaAPrev = 0,
    CaA0,
    CaA1,
    CaA2,
    CaTexA,
    CaRasA,
    CaKonst,
    CaZero,
};

enum BlendMode : std::uint8_t {
    BlendNone = 0,
    BlendMix,
    BlendLogic,
    BlendSubtract,
};

enum LogicOp : std::uint8_t {
    LoClear = 0,
    LoAnd,
    LoAndReverse,
    LoCopy,
    LoAndInverted,
    LoNoop,
    LoXor,
    LoOr,
    LoNor,
    LoEquiv,
    LoInvert,
    LoOrReverse,
    LoCopyInverted,
    LoOrInverted,
    LoNand,
    LoSet,
};

enum TexFilter : std::uint8_t {
    TexNear = 0,
    TexLinear,
    TexNearMipNear,
    TexLinMipNear,
    TexNearMipLin,
    TexLinMipLin,
};

enum IndTexFmt : std::uint8_t {
    IndFmt8 = 0,
    IndFmt5,
    IndFmt4,
    IndFmt3,
};

enum IndTexWrap : std::uint8_t {
    IndWrapOff = 0,
    IndWrap256,
    IndWrap128,
    IndWrap64,
    IndWrap32,
    IndWrap16,
    IndWrap0,
};

inline constexpr std::uint8_t kIndMtxOff = 0;
inline constexpr unsigned kMaxIndStages = 4;
inline constexpr unsigned kMaxIndMtx = 3;
inline constexpr unsigned kMaxXfArrays = 4;
inline constexpr unsigned kMaxMemMaps = 16;

enum BlendFactor : std::uint8_t {
    BlZero = 0,
    BlOne,
    BlSrcClr,
    BlInvSrcClr,
    BlSrcAlpha,
    BlInvSrcAlpha,
    BlDstAlpha,
    BlInvDstAlpha,
};

enum TexGenType : std::uint8_t {
    TgMtx2x4 = 0,
    TgMtx3x4,
    TgSrtg,
};

enum TexGenSrc : std::uint8_t {
    TgPos = 0,
    TgNrm,
    TgColor0,
    TgBinrm,
    TgTangent,
    TgTex0,
    TgTex1,
    TgTex2,
    TgTex3,
    TgTex4,
    TgTex5,
    TgTex6,
    TgTex7,
    TgColor1,  // SRTG from colour channel 1 (XF texgen type 3)
};

enum ColorSrc : std::uint8_t {
    SrcReg = 0,
    SrcVtx = 1,
};

enum DiffuseFn : std::uint8_t {
    DfNone = 0,
    DfSign,
    DfClamp,
};

enum AttnFn : std::uint8_t {
    AfNone = 0,
    AfSpec,
    AfSpot,
};

inline constexpr unsigned kMaxPnMtx = 10;
inline constexpr unsigned kVtxFmtCount = 8;
inline constexpr unsigned kTevStages = 16;
inline constexpr unsigned kMaxTexMaps = 8;
inline constexpr unsigned kMaxTexGens = 8;
inline constexpr unsigned kMaxLights = 8;
inline constexpr unsigned kMaxTluts = 20;
inline constexpr unsigned kXfMemSize = 256;
inline constexpr unsigned kXfDttSize = 256;
inline constexpr std::uint8_t kTexMtx0 = 30;
inline constexpr std::uint8_t kTexMtxIdentity = 60;
inline constexpr std::uint8_t kPtTexMtx0 = 64;
inline constexpr std::uint8_t kPtTexMtxIdentity = 125;

inline constexpr std::uint32_t pack_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}

} // namespace mkwv::gx
