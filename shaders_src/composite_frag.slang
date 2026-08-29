#include "output_encode.slang"

[[vk::binding(0, 0)]] Sampler2D inputImage;

// The external image arrives already in RGB: the sampler's
// VkSamplerYcbcrConversion applied the Y'CbCr matrix (BT.601/709/2020,
// limited or full range) before this stage ever ran. What is left here is the
// TRANSFER function, which a sampler cannot do.
//
// `transfer` selects it. The two non-SDR paths exist for opposite reasons:
// HLG because the camera delivers HLG-encoded BT.2020 while recording, and PQ
// because an HDR10 video file is PQ-encoded. Both look washed out or dark when
// handed to a display expecting something else, and both failures read as "the
// preview is broken" rather than as a colour bug.
//
// `peakNits` is the display's peak luminance for the PQ path's tone map, and
// is ignored otherwise.
static const int TRANSFER_SDR = 0;
static const int TRANSFER_HLG = 1;
static const int TRANSFER_PQ  = 2;

// ── The push block ─────────────────────────────────────────────────────────
//
// THIS STRUCT IS A CONTRACT WITH C++. Its layout is mirrored by
// `CompositePush` in vk_canvas's core/renderer.cc, and the two are checked
// against each other at build time by cmake/check_composite_pc.cmake.
//
// That check exists because of what happened without it. Two lines of
// development each appended their own fields here — the camera's loupe and
// focus peaking on one side, the video path's transfer, rotation and crop on
// the other — and when the branches were reconciled the C++ side kept writing
// one layout while the shader read the other. Nothing failed. Nothing logged.
// `uvScale.x` landed in `peak`, so focus peaking ran on every frame of every
// video, and the PQ decode simply did not happen. A push block mismatch is
// invisible by construction: it is a memcpy into a struct nobody validates.
//
// So: append new fields at the END, never reorder, and update the C++ mirror
// in the same commit.
[[vk::push_constant]] cbuffer PC {
    // Kept as the FIRST float and kept meaning "HLG when > 0.5", because the
    // camera consumer pushes exactly this one float and must not have to
    // change. A zero-initialized push block is therefore SDR, which is the
    // behaviour every existing consumer already had.
    float hlg;
    int   transfer;
    float peakNits;
    // Quarter-turns CLOCKWISE to apply to the sampled image: 0, 1, 2 or 3.
    //
    // A camera preview needs 1 and always did; that used to be baked into
    // composite_vert. A video file needs whatever its container says, which is
    // usually 0 for something recorded landscape and 1 or 3 for portrait. A
    // quadrant rather than an angle because this is a uv swizzle — free, exact,
    // and resampling nothing. The consumer always pushes it, so there is no
    // zero-init default to preserve here.
    int   rotQuadrant;

    // How much of the sampled image is actually PICTURE.
    //
    // A hardware decoder allocates its buffers aligned up to whatever its
    // hardware wants — a 2040x1530 video lands in a 2048x1536 buffer — and the
    // 8 columns and 6 rows past the edge hold whatever the codec left there.
    // Sampling 0..1 stretches that debris across the last millimetre of the
    // right and bottom edges, which is precisely the artefact this fixes.
    //
    // Applied to the FINAL uv, in texture space, after the rotation above.
    //
    // 0 means "not set" and is read as 1: a zero-initialized push block must
    // keep meaning "sample the whole image" for consumers that never set it.
    float2 uvScale;

    // ── The camera's viewfinder aids ───────────────────────────────────────
    // zoom > 1 draws the focus-check loupe: the same frame magnified about
    // (cx, cy) in UV space, so an inset viewport can show real sensor detail
    // while the main pass keeps the wide view. zoom <= 1 is the normal pass.
    float zoom;
    float cx;
    float cy;
    // peak > 0 enables focus peaking: pixels sitting on a hard luminance edge
    // are painted a flat colour, so what is critically sharp is unmistakable.
    // It is a gradient detector, so it is only as honest as the texture it
    // runs on — the texel steps are the BOUND IMAGE's, not the screen's, which
    // is what makes peaking inside the magnified inset (a full-sensor stream)
    // trustworthy.
    float peak;
    float texelX;
    float texelY;
    float peakColor;
} pc;

// HLG inverse-OETF: encoded signal [0,1] -> scene-linear [0,1].
float3 hlgToLinear(float3 e) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    float3 o;
    [unroll] for (int i = 0; i < 3; ++i) {
        float x = saturate(e[i]);
        o[i] = (x <= 0.5) ? (x * x / 3.0) : ((exp((x - c) / a) + b) / 12.0);
    }
    return o;
}

// ST 2084 EOTF: encoded signal -> absolute luminance, normalized so 1.0 is
// 10000 cd/m^2. This is the INVERSE of the OETF an HDR10 swapchain's output
// encode applies; the two are different functions and swapping them yields a
// picture that is plausibly wrong rather than obviously wrong.
float3 pqToLinear(float3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 ep  = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(ep - c1, 0.0);
    float3 den = max(c2 - c3 * ep, 1e-6);
    return pow(num / den, 1.0 / m1);
}

// Linear -> sRGB (display) transfer.
float3 linearToSrgb(float3 l) {
    float3 o;
    [unroll] for (int i = 0; i < 3; ++i) {
        float x = saturate(l[i]);
        o[i] = (x <= 0.0031308) ? (12.92 * x) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
    }
    return o;
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    // The loupe magnifies about a point in SCREEN uv, before anything that is
    // a property of the texture: it changes which part of the frame this
    // fragment is looking at, not how that part is laid out in memory.
    if (pc.zoom > 1.0) {
        const float2 c = float2(pc.cx, pc.cy);
        uv = c + (uv - c) / pc.zoom;
    }

    // Rotate next, in the unit square, then crop. The order matters: uvScale
    // describes the picture's extent within the DECODER's buffer, which is a
    // property of the texture and not of how it is being shown.
    float2 r = uv;
    if      (pc.rotQuadrant == 1) r = float2(uv.y, 1.0 - uv.x);
    else if (pc.rotQuadrant == 2) r = float2(1.0 - uv.x, 1.0 - uv.y);
    else if (pc.rotQuadrant == 3) r = float2(1.0 - uv.y, uv.x);

    float2 scale = float2(pc.uvScale.x > 0.0 ? pc.uvScale.x : 1.0,
                          pc.uvScale.y > 0.0 ? pc.uvScale.y : 1.0);
    const float2 texUv = r * scale;

    float4 s = inputImage.Sample(texUv);
    float3 c = s.rgb;

    // Focus peaking, on the RAW samples and before any transfer.
    //
    // Deliberately ahead of the transfer, and that is a change from the first
    // version of this: that one took l0 from the TONE-MAPPED colour and the
    // two neighbours from raw samples, so with HLG on it was measuring the
    // difference between two different transfer functions rather than an edge,
    // and lit up across flat areas. A transfer is monotonic, so it moves no
    // edge — running the detector before it is both cheaper and correct.
    //
    // Forward differences on luma, one texel apart. Two taps rather than a
    // full Sobel: at preview rates the cheaper detector is indistinguishable
    // in use, and an in-focus edge is high-contrast in SOME direction, which
    // the sum of both axes captures.
    if (pc.peak > 0.0) {
        float l0 = luma(c);
        float lx = luma(inputImage.Sample(texUv + float2(pc.texelX, 0.0)).rgb);
        float ly = luma(inputImage.Sample(texUv + float2(0.0, pc.texelY)).rgb);
        if (abs(l0 - lx) + abs(l0 - ly) > pc.peak) {
            // Flat, fully saturated, and nothing like a real scene colour, so
            // it reads as an overlay rather than as part of the image. It IS
            // an overlay, so it goes out through the UI encode on every
            // swapchain rather than through whichever transfer the content
            // happens to use.
            float3 pk = (pc.peakColor > 0.5) ? float3(0.0, 1.0, 0.2)
                                             : float3(1.0, 0.1, 0.2);
            return float4(encodeUiColor(pk), s.a);
        }
    }

    // The legacy single-float contract: a consumer that pushes only `hlg` and
    // leaves the rest zeroed still gets its HLG path.
    int transfer = (pc.transfer != TRANSFER_SDR) ? pc.transfer
                                                 : (pc.hlg > 0.5 ? TRANSFER_HLG : TRANSFER_SDR);

    if (transfer == TRANSFER_HLG) {
        c = linearToSrgb(hlgToLinear(c));
        // ...and then out through the encode below, which is the identity on
        // the SDR swapchain this path has always run on.
        return float4(encodeUiColor(c), s.a);
    } else if (transfer == TRANSFER_PQ) {
        // To absolute luminance, then into the display's range. Values above
        // what the panel can show are compressed rather than clipped: clipping
        // specular highlights is what makes HDR content read as flat and grey.
        float peakN = max(pc.peakNits, 1.0);
        float3 nits = pqToLinear(c) * 10000.0;

        // Identity below the knee; a soft shoulder only above it.
        //
        // This was plain Reinhard, n/(1 + n/peak), and that is a fidelity loss
        // applied to EVERY pixel: at a 1000-nit peak a 100-nit midtone comes
        // out at 91. Nothing about a midtone needs compressing — only the
        // highlights a display cannot reach do — so darkening the entire image
        // by ~9% to handle them is exactly the wrong trade for a player whose
        // job is to show the file.
        //
        // Below `knee` the value passes through UNCHANGED, so most of the
        // picture is bit-accurate to what was graded. Above it, an exponential
        // shoulder compresses toward `peak` asymptotically: continuous in value
        // and slope at the knee (so no visible banding at the join), monotonic,
        // and it never clips, which is what turns specular highlights into flat
        // white blobs.
        //
        // Applied to the BRIGHTEST CHANNEL and used to scale all three, not
        // applied to each channel on its own.
        //
        // Per channel it was a hue shift. A saturated highlight — a red brake
        // light, a blue LED, low-sun skin — has one channel far above the knee
        // and the others below it, so the bright one was compressed while the
        // others passed through untouched. That moves the RATIOS between the
        // channels, and the ratios are the hue and the saturation: the colour
        // visibly drifts toward white as it gets brighter, worst on exactly
        // the saturated highlights this curve exists to preserve.
        //
        // Scaling by one common ratio keeps r:g:b exactly as graded and
        // changes only the intensity, which is the one thing a display that
        // cannot reach the luminance is entitled to change.
        const float knee = 0.75;
        float kneeNits = peakN * knee;
        float headroom = peakN - kneeNits;

        float m = max(nits.r, max(nits.g, nits.b));
        if (m > kneeNits) {
            float over = m - kneeNits;
            float rolled = kneeNits + headroom * (1.0 - exp(-over / max(headroom, 1e-6)));
            // m > kneeNits > 0 here, so the divide is safe.
            nits *= rolled / m;
        }

        // Hand LINEAR light to the engine's own encode, with 1.0 meaning
        // `peakN` nits. THIS is what was missing and what made an HDR10 file
        // render as a black screen: output_encode.slang is included by
        // image/shape/overlay/msdf and was never included here, so this stage
        // wrote linear values into a swapchain whose colourspace says they are
        // PQ code values. The panel then reads 0.1 linear as about 0.6 nits,
        // and the whole picture collapses into black with no error anywhere.
        return float4(encodeImageLinear(nits / peakN, peakN), s.a);
    }

    // SDR content. On an SDR swapchain encodeUiColor() is the identity, so the
    // camera preview stays byte-for-byte what it was. On an HDR swapchain it
    // carries the sRGB numbers into the target's space instead of leaving them
    // to be misread, which is the same bug as above in its milder form.
    return float4(encodeUiColor(c), s.a);
}
