#include "glyphs.hh"
#include "curve_rasterizer.hh"
#include <algorithm>
#include <cctype>

namespace {

struct Seg { float x0, y0, x1, y1; };

struct GlyphDef {
  const Seg* segs;
  int        count;
  float      advance;
};

// ── Numbers ────────────────────────────────────────────────────────────────

static const Seg seg_0[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.9f},
  {0.9f,0.9f,0.1f,0.9f},{0.1f,0.9f,0.1f,0.1f},
};
static const Seg seg_1[] = {
  {0.5f,0.05f,0.5f,0.95f},{0.3f,0.20f,0.5f,0.05f},{0.3f,0.95f,0.7f,0.95f},
};
static const Seg seg_2[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.5f},
  {0.9f,0.5f,0.1f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_3[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.5f},{0.4f,0.5f,0.9f,0.5f},
  {0.9f,0.5f,0.9f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_4[] = {
  {0.1f,0.1f,0.1f,0.5f},{0.1f,0.5f,0.9f,0.5f},{0.7f,0.1f,0.7f,0.95f},
};
static const Seg seg_5[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.1f,0.1f,0.1f,0.5f},{0.1f,0.5f,0.9f,0.5f},
  {0.9f,0.5f,0.9f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_6[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.1f,0.1f,0.1f,0.9f},{0.1f,0.5f,0.9f,0.5f},
  {0.9f,0.5f,0.9f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_7[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.3f,0.9f},
};
static const Seg seg_8[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.9f},
  {0.9f,0.9f,0.1f,0.9f},{0.1f,0.9f,0.1f,0.1f},{0.1f,0.5f,0.9f,0.5f},
};
static const Seg seg_9[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.1f,0.1f,0.1f,0.5f},{0.1f,0.5f,0.9f,0.5f},
  {0.9f,0.1f,0.9f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};

// ── Uppercase letters ──────────────────────────────────────────────────────

static const Seg seg_A[] = {
  {0.1f,0.9f,0.5f,0.1f},{0.5f,0.1f,0.9f,0.9f},{0.25f,0.6f,0.75f,0.6f},
};
static const Seg seg_B[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.1f,0.75f,0.1f},{0.75f,0.1f,0.9f,0.3f},
  {0.9f,0.3f,0.75f,0.5f},{0.1f,0.5f,0.75f,0.5f},{0.75f,0.5f,0.9f,0.7f},
  {0.9f,0.7f,0.75f,0.9f},{0.75f,0.9f,0.1f,0.9f},
};
static const Seg seg_C[] = {
  {0.9f,0.1f,0.1f,0.1f},{0.1f,0.1f,0.1f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_D[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.1f,0.7f,0.1f},{0.7f,0.1f,0.9f,0.3f},
  {0.9f,0.3f,0.9f,0.7f},{0.9f,0.7f,0.7f,0.9f},{0.7f,0.9f,0.1f,0.9f},
};
static const Seg seg_E[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.1f,0.9f,0.1f},
  {0.1f,0.5f,0.75f,0.5f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_F[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.1f,0.9f,0.1f},{0.1f,0.5f,0.75f,0.5f},
};
static const Seg seg_G[] = {
  {0.9f,0.1f,0.1f,0.1f},{0.1f,0.1f,0.1f,0.9f},{0.1f,0.9f,0.9f,0.9f},
  {0.9f,0.9f,0.9f,0.5f},{0.5f,0.5f,0.9f,0.5f},
};
static const Seg seg_H[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.9f,0.1f,0.9f,0.9f},{0.1f,0.5f,0.9f,0.5f},
};
static const Seg seg_I[] = {
  {0.3f,0.1f,0.7f,0.1f},{0.5f,0.1f,0.5f,0.9f},{0.3f,0.9f,0.7f,0.9f},
};
static const Seg seg_J[] = {
  {0.3f,0.1f,0.7f,0.1f},{0.7f,0.1f,0.7f,0.75f},
  {0.7f,0.75f,0.5f,0.9f},{0.5f,0.9f,0.1f,0.9f},
};
static const Seg seg_K[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.9f,0.1f,0.1f,0.5f},{0.1f,0.5f,0.9f,0.9f},
};
static const Seg seg_L[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};
static const Seg seg_M[] = {
  {0.1f,0.9f,0.1f,0.1f},{0.1f,0.1f,0.5f,0.55f},
  {0.5f,0.55f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.9f},
};
static const Seg seg_N[] = {
  {0.1f,0.9f,0.1f,0.1f},{0.1f,0.1f,0.9f,0.9f},{0.9f,0.9f,0.9f,0.1f},
};
static const Seg seg_O[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.9f},
  {0.9f,0.9f,0.1f,0.9f},{0.1f,0.9f,0.1f,0.1f},
};
static const Seg seg_P[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.1f,0.75f,0.1f},{0.75f,0.1f,0.9f,0.3f},
  {0.9f,0.3f,0.75f,0.5f},{0.75f,0.5f,0.1f,0.5f},
};
static const Seg seg_Q[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.9f,0.9f},
  {0.9f,0.9f,0.1f,0.9f},{0.1f,0.9f,0.1f,0.1f},{0.6f,0.6f,0.95f,0.95f},
};
static const Seg seg_R[] = {
  {0.1f,0.1f,0.1f,0.9f},{0.1f,0.1f,0.75f,0.1f},{0.75f,0.1f,0.9f,0.3f},
  {0.9f,0.3f,0.75f,0.5f},{0.75f,0.5f,0.1f,0.5f},{0.5f,0.5f,0.9f,0.9f},
};
static const Seg seg_S[] = {
  {0.9f,0.1f,0.1f,0.1f},{0.1f,0.1f,0.1f,0.5f},{0.1f,0.5f,0.9f,0.5f},
  {0.9f,0.5f,0.9f,0.9f},{0.9f,0.9f,0.1f,0.9f},
};
static const Seg seg_T[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.5f,0.1f,0.5f,0.9f},
};
static const Seg seg_U[] = {
  {0.1f,0.1f,0.1f,0.8f},{0.1f,0.8f,0.5f,0.9f},
  {0.5f,0.9f,0.9f,0.8f},{0.9f,0.8f,0.9f,0.1f},
};
static const Seg seg_V[] = {
  {0.1f,0.1f,0.5f,0.9f},{0.5f,0.9f,0.9f,0.1f},
};
static const Seg seg_W[] = {
  {0.1f,0.1f,0.25f,0.9f},{0.25f,0.9f,0.5f,0.5f},
  {0.5f,0.5f,0.75f,0.9f},{0.75f,0.9f,0.9f,0.1f},
};
static const Seg seg_X[] = {
  {0.1f,0.1f,0.9f,0.9f},{0.9f,0.1f,0.1f,0.9f},
};
static const Seg seg_Y[] = {
  {0.1f,0.1f,0.5f,0.5f},{0.9f,0.1f,0.5f,0.5f},{0.5f,0.5f,0.5f,0.9f},
};
static const Seg seg_Z[] = {
  {0.1f,0.1f,0.9f,0.1f},{0.9f,0.1f,0.1f,0.9f},{0.1f,0.9f,0.9f,0.9f},
};

// ── Punctuation ────────────────────────────────────────────────────────────

static const Seg seg_dot[] = {
  {0.40f,0.85f,0.60f,0.85f},{0.60f,0.85f,0.60f,0.95f},
  {0.60f,0.95f,0.40f,0.95f},{0.40f,0.95f,0.40f,0.85f},
};
static const Seg seg_comma[] = {
  {0.40f,0.82f,0.60f,0.82f},{0.60f,0.82f,0.60f,0.95f},
  {0.60f,0.95f,0.45f,1.05f},
};
static const Seg seg_apostrophe[] = {
  {0.45f,0.10f,0.55f,0.10f},{0.55f,0.10f,0.55f,0.22f},
  {0.55f,0.22f,0.45f,0.28f},
};
static const Seg seg_exclaim[] = {
  {0.5f,0.1f,0.5f,0.7f},{0.5f,0.8f,0.5f,0.9f},
};
static const Seg seg_question[] = {
  {0.1f,0.2f,0.5f,0.1f},{0.5f,0.1f,0.9f,0.2f},{0.9f,0.2f,0.9f,0.45f},
  {0.9f,0.45f,0.5f,0.6f},{0.5f,0.6f,0.5f,0.72f},
  {0.5f,0.82f,0.5f,0.92f},
};
static const Seg seg_lparen[] = {
  {0.7f,0.1f,0.4f,0.3f},{0.4f,0.3f,0.4f,0.7f},{0.4f,0.7f,0.7f,0.9f},
};
static const Seg seg_rparen[] = {
  {0.3f,0.1f,0.6f,0.3f},{0.6f,0.3f,0.6f,0.7f},{0.6f,0.7f,0.3f,0.9f},
};
static const Seg seg_colon[] = {
  {0.4f,0.3f,0.6f,0.3f},{0.6f,0.3f,0.6f,0.4f},
  {0.6f,0.4f,0.4f,0.4f},{0.4f,0.4f,0.4f,0.3f},
  {0.4f,0.65f,0.6f,0.65f},{0.6f,0.65f,0.6f,0.75f},
  {0.6f,0.75f,0.4f,0.75f},{0.4f,0.75f,0.4f,0.65f},
};
static const Seg seg_minus[] = {
  {0.2f,0.5f,0.8f,0.5f},
};
static const Seg seg_plus[] = {
  {0.2f,0.5f,0.8f,0.5f},{0.5f,0.2f,0.5f,0.8f},
};
static const Seg seg_slash[] = {
  {0.2f,0.9f,0.8f,0.1f},
};
static const Seg seg_eq[] = {
  {0.2f,0.4f,0.8f,0.4f},{0.2f,0.6f,0.8f,0.6f},
};
static const Seg seg_percent[] = {
  {0.05f,0.95f,0.95f,0.05f},
  {0.05f,0.05f,0.30f,0.05f},{0.30f,0.05f,0.30f,0.30f},
  {0.30f,0.30f,0.05f,0.30f},{0.05f,0.30f,0.05f,0.05f},
  {0.70f,0.70f,0.95f,0.70f},{0.95f,0.70f,0.95f,0.95f},
  {0.95f,0.95f,0.70f,0.95f},{0.70f,0.95f,0.70f,0.70f},
};

#define G(a, ad) GlyphDef{ a, (int)(sizeof(a)/sizeof(a[0])), ad }

const GlyphDef* lookup(char c) {
  // Normalize: lowercase → uppercase
  if (c >= 'a' && c <= 'z') c = char(c - 32);

  static const GlyphDef d0  = G(seg_0, 1.1f);
  static const GlyphDef d1  = G(seg_1, 1.1f);
  static const GlyphDef d2  = G(seg_2, 1.1f);
  static const GlyphDef d3  = G(seg_3, 1.1f);
  static const GlyphDef d4  = G(seg_4, 1.1f);
  static const GlyphDef d5  = G(seg_5, 1.1f);
  static const GlyphDef d6  = G(seg_6, 1.1f);
  static const GlyphDef d7  = G(seg_7, 1.1f);
  static const GlyphDef d8  = G(seg_8, 1.1f);
  static const GlyphDef d9  = G(seg_9, 1.1f);

  static const GlyphDef dA  = G(seg_A, 1.1f);
  static const GlyphDef dB  = G(seg_B, 1.1f);
  static const GlyphDef dC  = G(seg_C, 1.1f);
  static const GlyphDef dD  = G(seg_D, 1.1f);
  static const GlyphDef dE  = G(seg_E, 1.1f);
  static const GlyphDef dF  = G(seg_F, 1.1f);
  static const GlyphDef dG  = G(seg_G, 1.1f);
  static const GlyphDef dH  = G(seg_H, 1.1f);
  static const GlyphDef dI  = G(seg_I, 0.7f);
  static const GlyphDef dJ  = G(seg_J, 1.0f);
  static const GlyphDef dK  = G(seg_K, 1.1f);
  static const GlyphDef dL  = G(seg_L, 1.1f);
  static const GlyphDef dM  = G(seg_M, 1.2f);
  static const GlyphDef dN  = G(seg_N, 1.1f);
  static const GlyphDef dO  = G(seg_O, 1.1f);
  static const GlyphDef dP  = G(seg_P, 1.1f);
  static const GlyphDef dQ  = G(seg_Q, 1.1f);
  static const GlyphDef dR  = G(seg_R, 1.1f);
  static const GlyphDef dS  = G(seg_S, 1.1f);
  static const GlyphDef dT  = G(seg_T, 1.1f);
  static const GlyphDef dU  = G(seg_U, 1.1f);
  static const GlyphDef dV  = G(seg_V, 1.1f);
  static const GlyphDef dW  = G(seg_W, 1.3f);
  static const GlyphDef dX  = G(seg_X, 1.1f);
  static const GlyphDef dY  = G(seg_Y, 1.1f);
  static const GlyphDef dZ  = G(seg_Z, 1.1f);

  static const GlyphDef dDot         = G(seg_dot,         0.6f);
  static const GlyphDef dComma       = G(seg_comma,        0.6f);
  static const GlyphDef dApostrophe  = G(seg_apostrophe,   0.5f);
  static const GlyphDef dExclaim     = G(seg_exclaim,      0.7f);
  static const GlyphDef dQuestion    = G(seg_question,     1.1f);
  static const GlyphDef dLParen      = G(seg_lparen,       0.7f);
  static const GlyphDef dRParen      = G(seg_rparen,       0.7f);
  static const GlyphDef dColon       = G(seg_colon,        0.7f);
  static const GlyphDef dMinus       = G(seg_minus,        1.0f);
  static const GlyphDef dPlus        = G(seg_plus,         1.1f);
  static const GlyphDef dSlash       = G(seg_slash,        1.0f);
  static const GlyphDef dEq         = G(seg_eq,           1.1f);
  static const GlyphDef dPercent     = G(seg_percent,      1.1f);

  switch (c) {
    case '0': return &d0;  case '1': return &d1;  case '2': return &d2;
    case '3': return &d3;  case '4': return &d4;  case '5': return &d5;
    case '6': return &d6;  case '7': return &d7;  case '8': return &d8;
    case '9': return &d9;
    case 'A': return &dA;  case 'B': return &dB;  case 'C': return &dC;
    case 'D': return &dD;  case 'E': return &dE;  case 'F': return &dF;
    case 'G': return &dG;  case 'H': return &dH;  case 'I': return &dI;
    case 'J': return &dJ;  case 'K': return &dK;  case 'L': return &dL;
    case 'M': return &dM;  case 'N': return &dN;  case 'O': return &dO;
    case 'P': return &dP;  case 'Q': return &dQ;  case 'R': return &dR;
    case 'S': return &dS;  case 'T': return &dT;  case 'U': return &dU;
    case 'V': return &dV;  case 'W': return &dW;  case 'X': return &dX;
    case 'Y': return &dY;  case 'Z': return &dZ;
    case '.': return &dDot;
    case ',': return &dComma;
    case '\'': return &dApostrophe;
    case '!': return &dExclaim;
    case '?': return &dQuestion;
    case '(': return &dLParen;
    case ')': return &dRParen;
    case ':': return &dColon;
    case '-': return &dMinus;
    case '+': return &dPlus;
    case '/': return &dSlash;
    case '=': return &dEq;
    case '%': return &dPercent;
    default:  return nullptr;
  }
}

void pushCurve(std::vector<float>& out,
               float type, float a, float b, float c, float d, float e,
               float r, float g, float bb, float aa, float lineWidth,
               float minX, float minY, float maxX, float maxY) {
  size_t n = out.size();
  out.resize(n + CurveRasterizer::CURVE_FLOATS, 0.0f);
  // [0-4]: type + params (a,b,c,d); [5-8]: reserved for cubic P2/P3 (stay 0)
  out[n+ 0] = type; out[n+ 1] = a;   out[n+ 2] = b;   out[n+ 3] = c;
  out[n+ 4] = d;    out[n+ 5] = e;
  // [9-12]: RGBA color
  out[n+ 9] = r;   out[n+10] = g;   out[n+11] = bb;  out[n+12] = aa;
  // [13]: lineWidth; [14-17]: bounding box
  out[n+13] = lineWidth;
  out[n+14] = minX; out[n+15] = minY; out[n+16] = maxX; out[n+17] = maxY;
}

} // namespace

void emitFilledRect(std::vector<float>& out,
                    float cx, float cy, float halfW, float halfH,
                    float r, float g, float b, float a, float radius) {
  pushCurve(out, 2.0f, cx, cy, halfW, halfH, radius, r, g, b, a, 0.0f,
            cx-halfW-1.0f, cy-halfH-1.0f, cx+halfW+1.0f, cy+halfH+1.0f);
}

void emitLineSegment(std::vector<float>& out,
                     float x0, float y0, float x1, float y1,
                     float lineWidth,
                     float r, float g, float b, float a) {
  float pad = lineWidth + 1.5f;
  pushCurve(out, 3.0f, x0, y0, x1, y1, 0.0f, r, g, b, a, lineWidth,
            std::min(x0,x1)-pad, std::min(y0,y1)-pad,
            std::max(x0,x1)+pad, std::max(y0,y1)+pad);
}

void emitGlyph(std::vector<float>& out,
               char c, float x, float y, float scale, float lineWidth,
               float r, float g, float b, float a) {
  const GlyphDef* def = lookup(c);
  if (!def) return;
  for (int i = 0; i < def->count; i++) {
    const Seg& s = def->segs[i];
    float x0 = x + s.x0*scale, y0 = y + s.y0*scale;
    float x1 = x + s.x1*scale, y1 = y + s.y1*scale;
    emitLineSegment(out, x0, y0, x1, y1, lineWidth, r, g, b, a);
    // Square caps at endpoints fill the corner gap where two capsule SDFs meet.
    emitFilledRect(out, x0, y0, lineWidth, lineWidth, r, g, b, a);
    emitFilledRect(out, x1, y1, lineWidth, lineWidth, r, g, b, a);
  }
}

float glyphAdvance(char c, float scale) {
  const GlyphDef* def = lookup(c);
  if (c == ' ') return 0.5f * scale;
  if (!def) return 0.5f * scale;
  return def->advance * scale;
}

void emitString(std::vector<float>& out,
                std::string_view str, float x, float y, float scale, float lineWidth,
                float r, float g, float b, float a) {
  for (char p : str) {
    if (p != ' ') emitGlyph(out, p, x, y, scale, lineWidth, r, g, b, a);
    x += glyphAdvance(p, scale);
  }
}

float stringWidth(std::string_view str, float scale) {
  float w = 0.0f;
  for (char p : str) w += glyphAdvance(p, scale);
  return w;
}
